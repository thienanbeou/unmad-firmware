"""Replace the MAD60 HE bootloader (flash sector 0) from the running Unmad firmware (v1.0+).

usage: hmk_bootloader.py verify FILE          read sector 0 and compare (read-only)
       hmk_bootloader.py install FILE --yes   write FILE to sector 0, then read it back

FILE: a raw sector 0 image: unmad-bootloader-mad60he-v1.2.1.bin (the open
bootloader), or the sector0-before-install-*.bin a previous install saved (to
go back to the vendor bootloader). install saves the current sector 0 to the current folder
first, stages the image in RAM (COMMAND_STAGE_BOOTLOADER 0xE5), has the firmware check
CRC and vectors, erase and write the sector (COMMAND_INSTALL_BOOTLOADER 0xE6),
then reads all 16 KB back over COMMAND_READ_MEMORY 0xE4.

Power loss during install (about a second) leaves the board without boot code;
recovery then needs SWD. Do not unplug while it runs. Close the hmkconf tab first.
"""
import struct
import sys
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

import hid

SECTOR0 = 0x4000


def open_hmk():
    """The libhmk raw HID interface (usage page 0xFFAB, usage 0xAB), any VID/PID."""
    for d in hid.enumerate():
        if d["usage_page"] == 0xFFAB and d["usage"] == 0xAB:
            dev = hid.device()
            dev.open_path(d["path"])
            return dev
    raise SystemExit("libhmk keyboard not found (close the hmkconf tab)")


def cmd(dev, payload, timeout=2.0):
    while dev.read(64, 1):
        pass
    dev.write(b"\x00" + bytes(payload).ljust(64, b"\x00"))
    t = time.monotonic() + timeout
    while time.monotonic() < t:
        r = bytes(dev.read(64, 100))
        if r and r[0] in (payload[0], 0xFF):
            return r if r[0] == payload[0] else None
    raise SystemExit(f"no reply to command {payload[0]:#x}")


def read_sector0(dev):
    out = b""
    for a in range(0x08000000, 0x08000000 + SECTOR0, 60):
        n = min(60, 0x08000000 + SECTOR0 - a)
        r = cmd(dev, [0xE4, *struct.pack("<I", a), n])
        if r is None:
            raise SystemExit("read refused: is libhmk_mad60he_v17diag / v17boot on the board?")
        out += r[1:1 + n]
    return out


def load(path):
    image = Path(path).read_bytes()
    image += b"\xff" * (-len(image) % 4)
    sp, reset = struct.unpack_from("<II", image)
    if len(image) > SECTOR0 or sp & 0x2FFE0000 != 0x20000000 or not reset & 1 \
            or not 0x08000008 <= reset < 0x08000000 + len(image):
        raise SystemExit(f"{path}: not a sector 0 image (size {len(image)}, SP {sp:#x}, reset {reset:#x})")
    return image


def same(dev, image):
    got = read_sector0(dev)
    want = image + b"\xff" * (SECTOR0 - len(image))
    bad = [i for i in range(SECTOR0) if got[i] != want[i]]
    print(f"sector 0 crc32 {zlib.crc32(got):#010x}; "
          + ("matches the image" if not bad else f"{len(bad)} bytes differ, first at {bad[0]:#x}"))
    return not bad


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("verify", "install"):
        raise SystemExit(__doc__)
    image = load(sys.argv[2])
    dev = open_hmk()
    if sys.argv[1] == "verify":
        sys.exit(0 if same(dev, image) else 1)
    if "--yes" not in sys.argv:
        raise SystemExit("install writes sector 0: add --yes")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    saved = Path(f"sector0-before-install-{stamp}.bin").resolve()
    saved.write_bytes(read_sector0(dev))
    print(f"saved {saved} (keep it: it is how you go back)")

    for off in range(0, len(image), 60):
        chunk = image[off:off + 60]
        if cmd(dev, [0xE5, *struct.pack("<H", off), len(chunk), *chunk]) is None:
            raise SystemExit(f"staging refused at {off:#x} (not the v17boot build?); nothing written")
    print(f"staged {len(image)} bytes, crc {zlib.crc32(image):#010x}; writing sector 0, do not unplug")
    r = cmd(dev, [0xE6, *struct.pack("<II", len(image), zlib.crc32(image)), *b"BOOT"], timeout=15)
    print("firmware reports", "success" if r else "FAILURE (the app keeps running; do not unplug)")
    ok = same(dev, image)
    dev.close()
    sys.exit(0 if r and ok else 1)


if __name__ == "__main__":
    main()
