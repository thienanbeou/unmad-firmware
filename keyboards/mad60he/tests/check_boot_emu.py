"""Run the open MAD60 HE bootloader (mad60boot) in Unicorn.

usage: python check_boot_emu.py BOOT.elf APP_CONTAINER.bin [STOCK_CONTAINER.bin]

BOOT.elf needs raw.bin next to it (objcopy -O binary). The app container is a
fwtool image (e.g. libhmk_mad60he_v17diag.bin); its packets are built with
fwtool.flash_packets, exactly what fwtool.py flash sends. Flash is modelled
the way the controller behaves: CR locked until the KEYR sequence, sector
erase sets 0xFF, programming needs PG and can only clear bits.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from hmk_emu import RET, Board  # noqa: E402
import fwtool  # noqa: E402
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE  # noqa: E402
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP  # noqa: E402

ELF = Path(sys.argv[1])
APP = fwtool.parse(Path(sys.argv[2]).read_bytes())["image"]
STOCK = fwtool.parse(Path(sys.argv[3]).read_bytes())["image"] if len(sys.argv) > 3 else None
BOOT = (ELF.parent / "raw.bin").read_bytes()
APP_BASE, FLAG, MAGIC = 0x08020000, 0x2001FFFC, 0x96969696
KEYR, SR, CR = 0x40023C04, 0x40023C0C, 0x40023C10
SECTORS = [0x4000] * 4 + [0x10000] + [0x20000] * 7
COUNTERS = {0x2001FFF0: 0x52534355, 0x2001FFF4: 1, 0x2001FFF8: 7}


def sector_base(n):
    return 0x08000000 + sum(SECTORS[:n])


class Chip(Board):
    def __init__(self, app=APP, flag=0x12345678):
        super().__init__(ELF, {CR: 0x80000000})
        uc = self.uc
        uc.mem_write(0x08000000, b"\xff" * 0x100000)
        uc.mem_write(0x08000000, BOOT)
        if app:
            uc.mem_write(APP_BASE, app)
        self.erased, self.programmed, self.bad = [], [], []
        self.keys, self.pending = [], []
        for a, v in {FLAG: flag, **COUNTERS}.items():
            self.w32(a, v)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._flash_wr, begin=0x08000000, end=0x080FFFFF)
        uc.hook_add(UC_HOOK_CODE, self._settle)

    def _wr(self, off, sz, v, base):
        a = base + off
        cr = self.shadow.get(CR, 0)
        if a == KEYR:
            self.keys = (self.keys + [v])[-2:]
            if self.keys == [0x45670123, 0xCDEF89AB]:
                self.shadow[CR] = cr & ~0x80000000
            return
        if a == SR:
            return                                   # write 1 to clear: nothing is ever set
        if a == CR:
            if cr & 0x80000000:
                self.bad.append(("CR write while locked", v))
                return
            if v & 0x10000 and v & 2:                # STRT + SER
                assert v & 0x300 == 0x200, f"erase PSIZE {v:#x}"
                n = (v >> 3) & 0xF
                self.erased.append(n)
                self.uc.mem_write(sector_base(n), b"\xff" * SECTORS[n])
                v &= ~0x10000
        super()._wr(off, sz, v, base)

    def _flash_wr(self, u, acc, a, size, v, d):
        cr = self.shadow.get(CR, 0)
        if cr & 0x80000000 or not cr & 1:
            self.bad.append(("flash write without PG", a))
        self.programmed.append(a)
        self.pending.append((a, size, bytes(u.mem_read(a, size))))

    def _settle(self, u, addr, size, d):
        # applies the bit-clearing rule after the store has landed
        for a, size, old in self.pending:
            new = bytes(u.mem_read(a, size))
            u.mem_write(a, bytes(x & y for x, y in zip(old, new)))
        self.pending.clear()

    def flash(self, a, n):
        return bytes(self.uc.mem_read(a, n))


def frame(body):
    return fwtool.frame(list(body))


# 1. power-on: no flag, valid app -> starts the app from reset state
app_sp, app_reset = struct.unpack_from("<II", APP)
for name, img in (("libhmk", APP), ("stock", STOCK)):
    if img is None:
        continue
    sp, reset = struct.unpack_from("<II", img)
    c = Chip(img)
    c.stop.add(reset & ~1)
    c.run(None, 200_000)
    assert c.reg(UC_ARM_REG_PC) == reset & ~1, f"{name}: app not started"
    assert c.reg(UC_ARM_REG_SP) == sp, f"{name}: MSP {c.reg(UC_ARM_REG_SP):#x}"
    assert c.shadow.get(0xE000ED08) == APP_BASE, f"{name}: VTOR"
    assert c.r32(FLAG) == MAGIC, f"{name}: flag not set before the jump"
    assert all(c.r32(a) == v for a, v in COUNTERS.items()), f"{name}: rescue counters touched"
    assert not c.programmed and not c.erased and not any(a == CR for a, _ in c.log)
    assert not any(0x40023800 <= a < 0x40023C00 for a, _ in c.log), f"{name}: clocks touched"
print("1 power-on: starts libhmk" + (" and stock 1.06" if STOCK else "")
      + " (VTOR, MSP, flag set, counters kept, no clock or flash access)")

# 2. flag set -> stays, clears it, sets up clocks + USB, never enters the app
c = Chip(flag=MAGIC)
c.stop.add(app_reset & ~1)
c.run(None, 2_000_000)
assert c.reg(UC_ARM_REG_PC) != app_reset & ~1, "jumped to the app"
assert c.r32(FLAG) == 0 and all(c.r32(a) == v for a, v in COUNTERS.items())
sh = c.shadow
assert sh[0x40023804] == 0x07405410, f"PLLCFGR {sh[0x40023804]:#x}"
assert sh[0x40023C00] == 0x305 and sh[0x40023808] & 0xFFF3 == 0x9402, "ACR / CFGR"
assert sh[0x40020424] >> 24 == 0xCC and (sh[0x40020400] >> 28) == 0xA, "PB14/PB15 AF12"
assert sh.get(0x40040038, 0) & (1 << 21), "NOVBUSSENS"
assert not c.programmed and not c.erased
print("2 flag set: flag cleared, counters kept, 168 MHz HSE PLL, PB14/PB15 AF12, "
      "USB core running, no flash access")

# 3. no flag, erased app -> stays (flag left set, like the vendor code)
c = Chip(app=None)
c.run(None, 300_000)
assert c.r32(FLAG) == MAGIC and c.shadow.get(0x40023804) == 0x07405410
print("3 erased app: stays in the bootloader")

# 4. full fwtool session over boot_packet
IN, OUT = 0x2000F000, 0x2000F100
bp = Chip(flag=MAGIC)
bp.run(None, 2_000_000)                          # C runtime + USB init done
bp.uc.mem_write(APP_BASE, bytes(range(256)) * 512)  # old firmware in sector 5


def send(pkt, chip=bp):
    chip.uc.mem_write(IN, pkt)
    chip.run(chip.syms["boot_packet"], 3_000_000, (IN, OUT))
    return chip.flash(OUT, 64)


def session(image, chip):
    setup, data = fwtool.flash_packets(image)
    chip.programmed.clear()
    chip.erased.clear()
    for pkt in setup + data:
        r = send(pkt, chip)
        assert r[:63] == pkt[:63] and r[63] == pkt[63], f"reply to {pkt[:9].hex()}"
    return chip


before = bp.flash(0x08000000, 0x100000)
session(APP, bp)
assert bp.erased == [5], f"erased {bp.erased}"
assert bp.flash(APP_BASE, len(APP)) == APP, "app content"
after = bp.flash(0x08000000, 0x100000)
assert after[:0x20000] == before[:0x20000] and after[0x40000:] == before[0x40000:], "outside sector 5"
assert after[0x20000 + len(APP):0x40000] == b"\xff" * (0x20000 - len(APP)), "tail not erased"
assert bp.programmed[-2:] == [APP_BASE, APP_BASE + 4], "vectors not last"
assert not bp.bad, bp.bad
r = send(frame([0xAA, 0x96]))
assert r[63] == frame([0xAA, 0x96])[63] and bp.uc.mem_read(bp.syms["reset_pending"], 1)[0] == 1
print(f"4 fwtool session: {len(APP)} bytes written exactly, only sector 5 erased, vectors last, "
      "every reply echoes, AA 96 arms the reset")

# 5. refused requests write nothing
def fails(pkt, chip=bp):
    chip.programmed.clear()
    chip.erased.clear()
    r = send(pkt, chip)
    return r[63] == (~pkt[63] & 0xFF) and not chip.programmed


def page(addr, data, last=1, idx=0):
    return frame([0xAA, 0x03, *fwtool.u32(addr), idx, len(data), last, *data])


assert fails(frame([0xAA, 0x01, *fwtool.u32(1), *fwtool.u32(0x08000000)])), "AA 01 base"
assert fails(frame([0xAA, 0x01, *fwtool.u32(193), *fwtool.u32(APP_BASE)])), "AA 01 too big"
send(frame([0xAA, 0x01, *fwtool.u32(1), *fwtool.u32(APP_BASE)]))
for addr in (0x08000000, 0x08004000, 0x0801F800, 0x080E0000, 0x0807FFF0, APP_BASE + 2):
    assert fails(page(addr, b"\x00" * 32)), f"page at {addr:#x}"
assert fails(page(APP_BASE, b"\x00" * 55)), "55-byte chunk"
assert fails(page(APP_BASE, b"\x00" * 8, idx=1)), "chunk without idx 0"
bad = bytearray(frame([0xAA, 0x02, *fwtool.u32(1)]))
bad[63] ^= 0x80
bp.erased.clear()
r = send(bytes(bad))
assert r[63] != bad[63] and not bp.erased, "bad checksum acted on"
bp.erased.clear()
send(frame([0xAA, 0x02, *fwtool.u32(10)]))
assert bp.erased == [5, 6, 7], f"erase clamp {bp.erased}"
assert bp.flash(0x08000000, len(BOOT)) == BOOT, "bootloader changed"
print("5 refused: wrong base, oversize, pages outside sectors 5-7 (bootloader, stock settings, "
      "libhmk config), misaligned, 55-byte chunk, chunk without start, bad checksum; "
      "AA 02 erases at most 5-7")

# 6. odd length is padded, an unfinished session leaves no valid vectors
c = Chip(flag=MAGIC)
c.run(None, 2_000_000)
odd = APP[:4093]
session(odd, c)
assert c.flash(APP_BASE, 4096) == odd + b"\xff" * 3, "odd tail"
c2 = Chip(flag=MAGIC)
c2.run(None, 2_000_000)
setup, data = fwtool.flash_packets(APP)
for pkt in setup + data[:40]:
    send(pkt, c2)
assert c2.flash(APP_BASE, 8) == b"\xff" * 8 and c2.flash(APP_BASE + 8, 100) == APP[8:108]
print("6 odd length padded with 0xFF (vendor drops it); interrupted session leaves the "
      "vectors erased, so the bootloader keeps control")

# 8. stock -> bootloader: the stock firmware's IAP entry (1.06 FUN_08026a80,
# reached by fwtool's 96 96 00 98) jumps here from a ChibiOS thread instead of
# resetting. Start it on the process stack with stock's MSP, as a thread would.
if STOCK:
    from unicorn.arm_const import UC_ARM_REG_CONTROL, UC_ARM_REG_MSP, UC_ARM_REG_PSP
    c = Chip(STOCK, flag=0)
    c.shadow[0x40023804] = 0x07405410                # stock's PLL running
    c.uc.reg_write(UC_ARM_REG_MSP, 0x20000400)
    c.uc.reg_write(UC_ARM_REG_PSP, 0x20006000)
    c.uc.reg_write(UC_ARM_REG_CONTROL, 2)
    c.log.clear()
    c.run(0x08026A80, 2_000_000, reset_sp=False)
    pc = c.reg(UC_ARM_REG_PC)
    assert 0x08000000 <= pc < 0x08000000 + len(BOOT), f"not in the bootloader: pc {pc:#x}"
    assert c.r32(FLAG) == 0, "flag not consumed: the bootloader would start the app"
    assert c.reg(UC_ARM_REG_CONTROL) & 2 == 0 and c.reg(UC_ARM_REG_SP) <= 0x20010000, "stack"
    # stock reset the PLL config to 0x24003010; ours set it again
    plls = [v for a, v in c.log if a == 0x40023804]
    assert plls[0] == 0x24003010 and plls[-1] == 0x07405410, f"PLLCFGR writes {plls}"
    assert c.shadow.get(0x40040038, 0) & (1 << 21) and not c.programmed and not c.erased
    print("8 stock 1.06 IAP entry -> bootloader: handoff from a ChibiOS thread (PSP) works, "
          "flag consumed, clocks and USB set up again, no flash access")

# 7. image
sp, reset = struct.unpack_from("<II", BOOT)
assert len(BOOT) <= 0x4000 and sp == 0x20010000 and 0x08000000 < reset < 0x08000000 + len(BOOT)
print(f"7 image: {len(BOOT)} of 16384 bytes, SP {sp:#x} (below the rescue words), "
      f"reset {reset:#x}")
print("all ok")
