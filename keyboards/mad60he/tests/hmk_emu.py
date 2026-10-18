"""Unicorn model of the MAD60 HE (STM32F405) for running libhmk builds.

Used by check_mad60he_emu.py (boot, rescue and flash rules) and
check_tapdance_emu.py (key handling). Peripherals are a flat shadow memory with
the few behaviours boot code polls on (RCC ready bits, SWS, VOSRDY, OTG reset,
DWT_CYCCNT, GPIO BSRR -> ODR, bit-band). No interrupts run.
"""
import struct
import subprocess
from pathlib import Path

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_MCLASS,
                     UC_MODE_THUMB, Uc)
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_SP)

NM = Path.home() / ".platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm.exe"
BASE = 0x08020000
RET = 0x08000000               # erased flash, used as a return address
RAM, RAM_SIZE = 0x20000000, 0x20000


def symbols(elf: Path) -> dict[str, int]:
    syms = {}
    for line in subprocess.run([str(NM), str(elf)], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


class Board:
    def __init__(self, elf: Path, regs: dict[int, int] | None = None):
        self.syms = symbols(elf)
        self.img = (elf.parent / "raw.bin").read_bytes()
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x08000000, 0x100000)
        uc.mem_write(0x08000000, b"\xff" * 0x100000)
        uc.mem_write(BASE, self.img)
        uc.mem_map(RAM, RAM_SIZE)
        uc.mem_map(0x1FFF0000, 0x10000)
        # GPIO reset values (RM0090): PA13/PA14/PA15 and PB3/PB4 start as debug AF
        self.shadow = {0x40020000: 0xA8000000, 0x4002000C: 0x64000000,
                       0x40020400: 0x00000280, 0x4002040C: 0x00000100, **(regs or {})}
        self.log: list[tuple[int, int]] = []          # peripheral writes (addr, value)
        self.flash_writes: list[int] = []
        self.hot: dict[int, int] = {}
        self.skip: set[int] = set()                   # functions that return at once
        self.stop: set[int] = {RET}                   # addresses that end a run
        self.calls: dict[int, callable] = {}          # addr -> fn(board) on entry

        for b, n in ((0x40000000, 0x80000), (0x50000000, 0x60000), (0xE0000000, 0x100000)):
            uc.mmio_map(b, n, lambda u, o, s, d, b=b: self._rd(o, s, b), None,
                        lambda u, o, s, v, d, b=b: self._wr(o, s, v, b), None)
        uc.mmio_map(0x42000000, 0x2000000, self._bb_rd, None, self._bb_wr, None)
        uc.hook_add(UC_HOOK_MEM_WRITE, lambda u, acc, a, s, v, d: self.flash_writes.append(a),
                    begin=0x08000000, end=0x080FFFFF)
        uc.hook_add(UC_HOOK_CODE, self._on_code)

    # peripherals

    def _rd(self, off, sz, base):
        a = base + off
        shadow = self.shadow
        v = shadow.get(a & ~3, 0)
        if a == 0x40023800:
            v = (v & ~0x0A020002) | ((v & 0x05010001) << 1)  # RCC_CR ready = enable
        elif a == 0x40023808:
            v = (v & ~0xC) | ((v & 3) << 2)                  # RCC_CFGR SWS = SW
        elif a == 0x40007004:
            v |= 1 << 14                                     # PWR_CSR VOSRDY
        elif a == 0x40040010:
            v = (v & ~0x31) | 0x80000000                     # OTG GRSTCTL idle
        elif a == 0xE0001004:
            shadow[a] = (v + 0x1000) & 0xFFFFFFFF            # DWT_CYCCNT
        return (v >> (8 * (a & 3))) & ((1 << (8 * sz)) - 1)

    def _wr(self, off, sz, v, base):
        a = base + off
        shadow = self.shadow
        sh, mask = 8 * (a & 3), ((1 << (8 * sz)) - 1) << (8 * (a & 3))
        shadow[a & ~3] = (shadow.get(a & ~3, 0) & ~mask) | ((v << sh) & mask)
        self.log.append((a, v))
        if 0x40020000 <= a < 0x40022400 and a & 0x3FC == 0x18:   # GPIO BSRR -> ODR
            bsrr = (v << sh) & mask
            odr = (a & ~0x3FF) + 0x14
            shadow[odr] = (shadow.get(odr, 0) | (bsrr & 0xFFFF)) & ~(bsrr >> 16)
            shadow[a & ~3] = 0

    @staticmethod
    def _bb(off):
        byte, bit = 0x40000000 + off // 32, (off % 32) // 4
        return byte & ~3, (byte & 3) * 8 + bit

    def _bb_rd(self, u, off, sz, d):
        word, bit = self._bb(off)
        return (self._rd(word - 0x40000000, 4, 0x40000000) >> bit) & 1

    def _bb_wr(self, u, off, sz, v, d):
        word, bit = self._bb(off)
        old = self.shadow.get(word, 0)
        self._wr(word - 0x40000000, 4, (old & ~(1 << bit)) | ((v & 1) << bit), 0x40000000)

    # execution

    def _on_code(self, u, addr, size, d):
        self.hot[addr] = self.hot.get(addr, 0) + 1
        if addr in self.calls:
            self.calls[addr](self)
        if addr in self.stop:
            u.emu_stop()
        elif addr in self.skip:
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))

    def run(self, start: int | None = None, count: int = 3_000_000, args=(), reset_sp=True):
        """Run from the reset vector, or call `start(args)` and stop when it returns."""
        uc = self.uc
        sp, reset = struct.unpack_from("<II", self.img, 0)
        if reset_sp:
            uc.reg_write(UC_ARM_REG_SP, sp)
        if start:
            for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
                uc.reg_write(reg, v)
            uc.reg_write(UC_ARM_REG_LR, RET | 1)
        try:
            uc.emu_start((start or reset) | 1, 0xFFFFFFFF, count=count)
        except Exception as e:                               # noqa: BLE001
            print("  stopped:", e)

    def reg(self, r):
        return self.uc.reg_read(r)

    def r32(self, a):
        return struct.unpack("<I", bytes(self.uc.mem_read(a, 4)))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v))

    def snapshot(self):
        return bytes(self.uc.mem_read(RAM, RAM_SIZE)), dict(self.shadow), self.uc.context_save()

    def restore(self, snap):
        ram, shadow, ctx = snap
        self.uc.mem_write(RAM, ram)
        self.shadow.clear()
        self.shadow.update(shadow)
        self.uc.context_restore(ctx)
