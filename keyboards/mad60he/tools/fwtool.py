#!/usr/bin/env python3
"""Firmware tool for DuckBread boards (MAD60 HE 373B:105D and siblings).

The vendor bootloader has no read command, so the on-chip image cannot be
dumped over USB. The vendor publishes the exact images it flashes instead:

    python tools/fwtool.py download                 # all images for this board
    python tools/fwtool.py info FILE
    python tools/fwtool.py unpack FILE raw.bin      # raw image, load at 0x08020000
    python tools/fwtool.py pack raw.bin out.bin --like FILE [--version 1.07]
    python tools/fwtool.py flash out.bin --yes      # enter IAP and write
    python tools/fwtool.py flash out.bin --yes --keep-settings
    python tools/fwtool.py selftest

From the stock firmware, a flash without --keep-settings resets the stock
settings (keymap, macros, actuation, RT) to firmware defaults and clears the
per-key calibration flags, even when rewriting the same version. From the
libhmk port, flash enters the bootloader with COMMAND_BOOTLOADER, which
touches no settings: stock keeps sectors 1-3, libhmk keeps sector 11.

The wipe comes from the IAP entry packet, not the bootloader: 96 96 96 98
clears settings and then jumps, 96 96 00 98 just jumps (1.06 handler at
0x08026294). --keep-settings sends the second one, so a same-version flash
keeps everything. A different vendor version still resets itself on boot.

Container (all big-endian), from IapFileInfo in the vendor bundle:
    0  u16 normal VID   2  u16 normal PID   4  u16 boot VID   6  u16 boot PID
    8  u8 major  9 u8 minor (shown as hex)  10 u32 image length N
    14 image[N]   14+N u32 crc = 0x55555555 - sum(image)

Flash protocol (DuckBread.startUpgradeBySector, the path for non-Yateli MCUs):
64-byte reports, byte 63 = OR of bytes 0..62, reply's last byte must echo it.
    AA 01 <u32 pages of 0x800> <u32 base>
    AA 02 <u32 sectors of 0x20000> <u32 base>     erase, then wait 0.3 s/sector
    AA 03 <u32 addr> <idx> <len> <last> <data[<=54]>   per 0x800 page
    AA 96                                          done, jump to app
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
import urllib.request
from pathlib import Path

VID, PID, BOOT_PID = 0x373B, 0x105D, 0x2028
FW_MARK = "Mad60"
BASE = 0x08020000          # app base for sector-erase boards (vector table lives here)
PAGE, SECTOR, CHUNK = 0x800, 0x20000, 54
MAX_IMAGE = 3 * SECTOR     # ponytail: guess at app space; raise if a real image needs more
API = "https://api-hub-cloud.atk.pro/firmware"


# container

def parse(blob: bytes) -> dict:
    nv, np_, bv, bp, ma, mi, n = struct.unpack(">HHHHBBI", blob[:14])
    if len(blob) != 18 + n:
        raise ValueError(f"length mismatch: header says {n}, file holds {len(blob) - 18}")
    image = blob[14:14 + n]
    crc = struct.unpack(">I", blob[14 + n:])[0]
    if crc != (0x55555555 - sum(image)) & 0xFFFFFFFF:
        raise ValueError("crc mismatch")
    return {"normal": (nv, np_), "boot": (bv, bp), "version": f"{ma:x}.{mi:02x}",
            "ver_raw": (ma, mi), "image": image}


def pack(image: bytes, normal, boot, ver_raw) -> bytes:
    hdr = struct.pack(">HHHHBBI", *normal, *boot, *ver_raw, len(image))
    return hdr + image + struct.pack(">I", (0x55555555 - sum(image)) & 0xFFFFFFFF)


def check_image(image: bytes) -> None:
    """Refuse images that would obviously brick: wrong base, empty, too big."""
    if not 512 <= len(image) <= MAX_IMAGE:
        raise ValueError(f"image size {len(image)} outside 512..{MAX_IMAGE}")
    if len(image) % 4:
        # Every vendor image is word-aligned; an unaligned one lost its last
        # two bytes on the board and crashed.
        raise ValueError(f"image size {len(image)} not a multiple of 4; pad with 0xFF")
    sp, reset = struct.unpack("<II", image[:8])
    if not 0x20000000 <= sp <= 0x20040000:
        raise ValueError(f"initial SP {sp:#010x} is not in RAM")
    if not BASE <= (reset & ~1) < BASE + len(image) or not reset & 1:
        raise ValueError(f"reset vector {reset:#010x} not a Thumb address inside the image "
                         f"(image must be linked at {BASE:#010x})")


# flash packets

def frame(body: list[int]) -> bytes:
    pkt = bytearray(64)
    pkt[:len(body)] = bytes(body)
    acc = 0
    for b in pkt[:63]:
        acc |= b
    pkt[63] = acc
    return bytes(pkt)


def u32(v: int) -> list[int]:
    return list(v.to_bytes(4, "big"))


def flash_packets(image: bytes) -> tuple[list[bytes], list[bytes]]:
    """(setup packets, data packets) exactly as the vendor app builds them."""
    pages = -(-len(image) // PAGE)
    sectors = -(-len(image) // SECTOR)
    setup = [frame([0xAA, 0x01, *u32(pages), *u32(BASE)]),
             frame([0xAA, 0x02, *u32(sectors), *u32(BASE)])]
    data, off = [], 0
    for p in range(0, len(image), PAGE):
        page = image[p:p + PAGE]
        chunks = [page[i:i + CHUNK] for i in range(0, len(page), CHUNK)]
        for idx, c in enumerate(chunks):
            data.append(frame([0xAA, 0x03, *u32(BASE + off), idx, len(c),
                               int(idx == len(chunks) - 1), *c]))
            off += len(c)
    return setup, data


# device

def _open(pid: int, usage_page: int, usage: int):
    import hid
    for d in hid.enumerate(VID, pid):
        if d["usage_page"] == usage_page and d["usage"] == usage:
            dev = hid.device()
            dev.open_path(d["path"])
            return dev
    return None


def _open_libhmk():
    """The libhmk raw HID interface (usage page 0xFFAB, usage 0xAB), any VID/PID."""
    import hid
    for d in hid.enumerate():
        if d["usage_page"] == 0xFFAB and d["usage"] == 0xAB:
            dev = hid.device()
            dev.open_path(d["path"])
            return dev
    return None


def _xfer(dev, pkt: bytes, timeout_ms=2000) -> bytes:
    dev.write(b"\x00" + pkt)
    deadline = time.monotonic() + timeout_ms / 1000
    while time.monotonic() < deadline:
        r = bytes(dev.read(64, 100))
        if r and r[0] == pkt[0]:
            return r
    return b""


def flash(blob: bytes, keep_settings: bool = False) -> None:
    info = parse(blob)
    if info["normal"] != (VID, PID) or info["boot"] != (VID, BOOT_PID):
        raise SystemExit(f"file is for {info['normal']}/{info['boot']}, not this board")
    image = info["image"]
    check_image(image)
    setup, data = flash_packets(image)

    boot = _open(BOOT_PID, 0xFF, 0x01)
    if boot is None:
        kb = _open(PID, 0xFF60, 0x61)
        if kb is not None:
            print("entering IAP mode...")
            iap = bytearray(32)
            iap[0:4] = b"\x96\x96\x96\x98"   # IapMode: commandId, customId, customId, iap
            if keep_settings:
                iap[2] = 0x00                  # byte 2 = 0x96 is what wipes settings
            kb.write(b"\x00" + bytes(iap))
            kb.close()
        elif (hmk := _open_libhmk()) is not None:
            # The libhmk port (../NOTES.md, "Bootloader"): COMMAND_BOOTLOADER
            # sets the bootloader flag and resets. It never touches stock settings.
            print("libhmk firmware found; entering IAP mode...")
            hmk.write(b"\x00" + bytes([2]).ljust(64, b"\x00"))
            hmk.close()
        else:
            raise SystemExit("keyboard not found (close the vendor app or the hmkconf tab)")
        for _ in range(100):
            time.sleep(0.2)
            if boot := _open(BOOT_PID, 0xFF, 0x01):
                break
        else:
            raise SystemExit("bootloader never appeared; unplug and replug to return to normal")

    print(f"bootloader up; writing {len(image)} bytes v{info['version']}")
    for pkt in setup:
        if not _xfer(boot, pkt):
            raise SystemExit(f"no reply to {pkt[:2].hex()}; nothing written yet, replug to recover")
    time.sleep(0.3 * -(-len(image) // SECTOR))
    for n, pkt in enumerate(data, 1):
        r = _xfer(boot, pkt)
        if not r or r[-1] != pkt[-1]:
            raise SystemExit(f"write failed at packet {n}/{len(data)}. The app area is now "
                             f"partial: stay in the bootloader and rerun flash with a good image.")
        print(f"\r{n * 100 // len(data)}%", end="", flush=True)
    time.sleep(0.2)
    _xfer(boot, frame([0xAA, 0x96]), 500)
    boot.close()
    print("\ndone; keyboard should re-enumerate as normal")


# commands

def download(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for suffix in ("/list", "-private", "-performance"):
        url = f"{API}/{VID}/{BOOT_PID}/{FW_MARK}{suffix}"
        data = json.load(urllib.request.urlopen(url, timeout=20))["data"]
        for e in data if isinstance(data, list) else [data] if data else []:
            dest = out / f"{FW_MARK}_{suffix.strip('/-')}_v{e['version']}.bin"
            dest.write_bytes(urllib.request.urlopen(e["file"], timeout=60).read())
            parse(dest.read_bytes())
            print(dest, "|", (e.get("content") or "").replace("\n", " "))


def selftest() -> None:
    image = bytearray(5000)
    image[:8] = struct.pack("<II", 0x20000400, BASE + 0x31D)
    blob = pack(bytes(image), (VID, PID), (VID, BOOT_PID), (1, 5))
    info = parse(blob)
    assert info["image"] == image and info["version"] == "1.05"
    check_image(info["image"])
    try:
        parse(blob[:-1] + bytes([blob[-1] ^ 1]))
        raise AssertionError("bad crc accepted")
    except ValueError:
        pass
    bad = bytearray(image)
    bad[4:8] = struct.pack("<I", 0x08010000 + 0x31D)
    try:
        check_image(bytes(bad))
        raise AssertionError("wrong base accepted")
    except ValueError:
        pass
    setup, data = flash_packets(bytes(image))
    assert setup[0][:10] == bytes([0xAA, 1, 0, 0, 0, 3, 0x08, 0x02, 0, 0])
    assert setup[1][:6] == bytes([0xAA, 2, 0, 0, 0, 1])
    assert all(len(p) == 64 for p in setup + data)
    assert sum(p[7] for p in data) == len(image)                 # every byte sent once
    assert sum(p[8] for p in data) == 3                          # one "last" per page
    assert data[38][2:6] == (BASE + PAGE).to_bytes(4, "big") and data[38][6] == 0
    print("selftest ok")


def main() -> None:
    # The vendor changelogs are Chinese; a Windows console (cp1252) can't print them
    sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("download").add_argument("--out", type=Path, default=Path("."))
    sub.add_parser("info").add_argument("file", type=Path)
    p = sub.add_parser("unpack"); p.add_argument("file", type=Path); p.add_argument("out", type=Path)
    p = sub.add_parser("pack")
    p.add_argument("raw", type=Path); p.add_argument("out", type=Path)
    p.add_argument("--like", type=Path, required=True, help="vendor file to copy VID/PID header from")
    p.add_argument("--version", help="e.g. 1.07 (default: same as --like)")
    p = sub.add_parser("flash"); p.add_argument("file", type=Path); p.add_argument("--yes", action="store_true")
    p.add_argument("--keep-settings", action="store_true", help="enter IAP without the settings wipe")
    sub.add_parser("selftest")
    a = ap.parse_args()

    if a.cmd == "download":
        download(a.out)
    elif a.cmd == "info":
        i = parse(a.file.read_bytes())
        sp, reset = struct.unpack("<II", i["image"][:8])
        print(f"normal {i['normal'][0]:04x}:{i['normal'][1]:04x}  boot {i['boot'][0]:04x}:{i['boot'][1]:04x}"
              f"  v{i['version']}  {len(i['image'])} bytes  SP {sp:#010x}  reset {reset:#010x}")
    elif a.cmd == "unpack":
        a.out.write_bytes(parse(a.file.read_bytes())["image"])
        print(f"wrote {a.out}; load at {BASE:#010x}, ARM Cortex-M (Thumb, little-endian)")
    elif a.cmd == "pack":
        like = parse(a.like.read_bytes())
        image = a.raw.read_bytes()
        check_image(image)
        ver = like["ver_raw"]
        if a.version:
            ma, mi = a.version.split(".")
            ver = (int(ma, 16), int(mi, 16))
        a.out.write_bytes(pack(image, like["normal"], like["boot"], ver))
        print(f"wrote {a.out}")
    elif a.cmd == "flash":
        if not a.yes:
            raise SystemExit("flash rewrites the application firmware; rerun with --yes")
        flash(a.file.read_bytes(), a.keep_settings)
    else:
        selftest()


if __name__ == "__main__":
    main()
