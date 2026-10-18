"""Boot the libhmk MAD60 HE build in Unicorn and check the rescue and flash rules.

usage: python check_mad60he_emu.py firmware.elf
(raw image from `arm-none-eabi-objcopy -O binary`, symbols from `arm-none-eabi-nm`;
both are looked up next to the ELF: raw.bin and the nm output is read here.)

Peripherals are a flat shadow with the ready bits boot code polls on. No
interrupts run, so a boot stops where analog_init waits for the first ADC scan,
which is after clocks, USB, wear leveling and all pin setup.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmk_emu import BASE, RET, Board  # noqa: E402

REPO = Path(__file__).resolve().parents[3]       # libhmk checkout
BOARD = Path(__file__).resolve().parents[1]      # keyboards/mad60he
ELF = Path(sys.argv[1])
RESCUE, FAILS, BOOTS, FLAG = 0x2001FFF0, 0x2001FFF4, 0x2001FFF8, 0x2001FFFC
RESCUE_MAGIC, BOOT_MAGIC = 0x52534355, 0x96969696
WL_START = 0x080E0000          # sector 11
syms = Board(ELF).syms


def boot(ram=None, regs=None, start=None, count=3_000_000, args=()):
    b = Board(ELF, regs)
    for a, v in (ram or {}).items():
        b.w32(a, v)
    b.run(start, count, args)
    from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_R0
    return dict(log=b.log, shadow=b.shadow, flash_writes=b.flash_writes, r32=b.r32,
                r0=b.reg(UC_ARM_REG_R0), pc=b.reg(UC_ARM_REG_PC),
                hot=sorted(b.hot, key=b.hot.get)[-1])


def booted(start, ram=None, count=50_000, args=()):
    """Cold boot (stops in analog_init, after eeconfig_init), then call `start`
    the way the main loop would: with the configuration loaded."""
    b = Board(ELF)
    b.run(None, 3_000_000)
    for a, v in (ram or {}).items():
        b.w32(a, v)
    b.log.clear()
    b.flash_writes.clear()
    b.run(start, count, args)
    from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_R0
    return dict(log=b.log, shadow=b.shadow, flash_writes=b.flash_writes, r32=b.r32,
                r0=b.reg(UC_ARM_REG_R0), pc=b.reg(UC_ARM_REG_PC), board=b)


def first(log, pred):
    return next((i for i, (a, v) in enumerate(log) if pred(a, v)), None)


AIRCR_RESET = lambda a, v: a == 0xE000ED0C and v & 0xFFFF0004 == 0x05FA0004
PLLCFGR = lambda a, v: a == 0x40023804

# 1. cold boot
r = boot()
log, sh = r["log"], r["shadow"]
vtor = first(log, lambda a, v: a == 0xE000ED08)
assert vtor is not None and log[vtor][1] == BASE, "VTOR not set to the app"
assert vtor < first(log, lambda a, v: a == 0xE000E010), "SysTick touched before VTOR"
assert (0x40003000, 0xCCCC) in log and sh[0x40003004] == 3 and sh[0x40003008] == 2000, "IWDG"
assert r["r32"](RESCUE) == RESCUE_MAGIC and r["r32"](FAILS) == 1 and r["r32"](BOOTS) == 1, "boot not counted"
assert any(a == 0x40023874 and v & (1 << 24) for a, v in log), "reset flags not cleared (RMVF)"
assert sh[0x40023804] == 0x07405410, f"PLLCFGR {sh[0x40023804]:#x} differs from stock"
assert sh[0x40023C00] & 0xF == 5, "flash latency"
assert not any(AIRCR_RESET(a, v) for a, v in log), "cold boot reset itself"
erases = [(v >> 3) & 0xF for a, v in log if a == 0x40023C10 and v & 0x10002 == 0x10002]  # SER + STRT
assert set(erases) <= {11}, f"erased sectors {set(erases)}"
bad = [hex(a) for a in r["flash_writes"] if a < WL_START]
assert not bad, f"flash writes below sector 11: {bad[:5]}"
moder = {p: sh.get(0x40020000 + 0x400 * i, 0) for i, p in enumerate("ABCD")}
odr = {p: sh.get(0x40020014 + 0x400 * i, 0) for i, p in enumerate("ABCD")}
mode = lambda p, n: (moder[p] >> 2 * n) & 3
cols = [("A", 8), ("A", 9), ("A", 10), ("A", 11), ("A", 12), ("A", 15), ("C", 10),
        ("C", 11), ("C", 12), ("D", 2), ("B", 3), ("B", 4), ("B", 5), ("B", 6)]
assert all(mode(p, n) == 1 for p, n in cols), "columns not outputs"
levels = [(odr[p] >> n) & 1 for p, n in cols]
assert levels == [0] + [1] * 13, f"column levels {levels}"
assert all(mode("A", n) == 3 for n in (3, 2, 0, 1, 7)), "rows not analog"
assert mode("B", 14) == 2 and mode("B", 15) == 2, "USB pins"
assert mode("A", 13) == 2 and mode("A", 14) == 2, "SWD pins changed"
print(f"1 cold boot: VTOR first, IWDG 2000, boot counted, PLL = stock, only sector 11 "
      f"erased ({len(erases)} erases), {len(r['flash_writes'])} flash writes all in sector 11, "
      f"pins = hardware map; stopped in {r['hot']:#x}")

# 2. two failed boots already counted
r = boot(ram={RESCUE: RESCUE_MAGIC, FAILS: 2})
assert r["r32"](FLAG) == BOOT_MAGIC and any(AIRCR_RESET(a, v) for a, v in r["log"])
assert first(r["log"], PLLCFGR) is None and not r["flash_writes"], "did work before rescue"
print("2 after 2 failed boots: marker + reset before clocks or flash")

# 3. bootloader left the PLL running as the system clock
r = boot(regs={0x40023800: 0x01010001, 0x40023808: 0x0000000A})
log = r["log"]
pll = first(log, PLLCFGR)
to_hsi = first(log, lambda a, v: a == 0x40023808 and v & 3 == 0)
pll_off = first(log, lambda a, v: a == 0x40023800 and not v & (1 << 24))
assert to_hsi is not None and pll_off is not None and to_hsi < pll_off < pll
assert r["shadow"][0x40023804] == 0x07405410
print("3 dirty entry: switched to HSI and stopped the PLL before configuring it")

# 4. bootloader flag left over from IAP
r = boot(ram={FLAG: BOOT_MAGIC})
assert r["r32"](FLAG) == 0 and not any(AIRCR_RESET(a, v) for a, v in r["log"])
print("4 leftover flag: cleared, boot continues")

# 5. HardFault
r = boot(start=syms["HardFault_Handler"], count=2000)
assert r["r32"](FLAG) == BOOT_MAGIC and any(AIRCR_RESET(a, v) for a, v in r["log"])
print("5 HardFault: marker + reset")

# 6. flash guard, called directly
for sector in range(12):
    r = boot(start=syms["flash_erase"], args=(sector,), count=200_000)
    erased = [(v >> 3) & 0xF for a, v in r["log"] if a == 0x40023C10 and v & 0x10002 == 0x10002]
    if sector < 11:
        assert r["r0"] == 0 and not erased, f"sector {sector} erase allowed"
    else:
        assert r["r0"] == 1 and erased == [11], f"sector 11 erase failed {erased}"
for addr, ok in ((0x0, 0), (0x4000, 0), (0x20000, 0), (0xDFFFC, 0), (0xE0000, 1)):
    r = boot(start=syms["flash_write"], args=(addr, 0x20000000, 1), count=200_000)
    assert r["r0"] == ok and bool(r["flash_writes"]) == bool(ok), f"write at {addr:#x}"
print("6 flash guard: erase of sectors 0-10 and writes below 0xE0000 refused, sector 11 works")

# 7. board_task: no configured USB after BOARD_RESCUE_NO_USB_MS (10 s in config.h)
tick = syms["uwTick"]
r = booted(syms["board_task"], {RESCUE: RESCUE_MAGIC, FAILS: 1, tick: 9999}, 2_000_000)
assert not any(AIRCR_RESET(a, v) for a, v in r["log"]) and r["r32"](FAILS) == 1, "reset too early"
assert (0x40003000, 0xAAAA) in r["log"], "watchdog not refreshed"
r = booted(syms["board_task"], {RESCUE: RESCUE_MAGIC, FAILS: 1, tick: 10000}, 2_000_000)
NO_USB_RULE = any(AIRCR_RESET(a, v) for a, v in r["log"])     # bring-up config only
assert not NO_USB_RULE or r["r32"](FLAG) == BOOT_MAGIC, "no-USB rule reset without the flag"
r = booted(syms["board_task"], {RESCUE: RESCUE_MAGIC, FAILS: 0, tick: 99999}, 2_000_000)
assert not any(AIRCR_RESET(a, v) for a, v in r["log"]), "reset after a good boot"
print("7 board_task: refreshes IWDG, never resets after a good boot; no-USB rule "
      + ("ON (bootloader at 10 s, bring-up config)" if NO_USB_RULE else "OFF (daily config)"))

# 8. previous reset was the watchdog (RCC_CSR IWDGRSTF) after our own boot
r = boot(regs={0x40023874: 1 << 29}, ram={RESCUE: RESCUE_MAGIC, FAILS: 0})
assert r["r32"](FLAG) == BOOT_MAGIC and any(AIRCR_RESET(a, v) for a, v in r["log"])
assert first(r["log"], PLLCFGR) is None, "did work before rescue"
# stale IWDGRSTF left by other firmware (no counters): normal boot, flag cleared
r = boot(regs={0x40023874: 1 << 29})
assert r["r32"](FLAG) != BOOT_MAGIC and first(r["log"], PLLCFGR) is not None, "stale flag rescued"
r = boot(regs={0x40023874: 1 << 28}, ram={RESCUE: RESCUE_MAGIC, FAILS: 0})   # software reset
assert r["r32"](FLAG) != BOOT_MAGIC and first(r["log"], PLLCFGR) is not None
print("8 watchdog reset after our boot: bootloader; stale flag from other firmware or a "
      "software reset: normal boot")

# 9. serial number diagnostics: UID hex + "-BB-FF" (boot count, reset flags)
uc_buf = 0x20010000
r = boot(start=syms["board_serial"], args=(uc_buf,), ram={RESCUE: RESCUE_MAGIC, BOOTS: 3}, count=20_000)
assert r["r0"] in (24, 30), r["r0"]
print(f"9 serial number: {r['r0']} characters ("
      + ("with bring-up diagnostics)" if r["r0"] == 30 else "UID only, daily config)"))
# 10. RGB: board_task starts TIM3 CH4 PWM + circular DMA1 stream 2 on PC9 with
# the stock timing; the LED buffer decodes to the default effect; the raw HID
# lighting commands (src/rgb.c) change effect, colour and per-key colours.
CONFIG_H = (BOARD / "config.h").read_text()
if "rgb_slots" in syms:
    import re
    cfg = dict(re.findall(r"#define (RGB_\w+) (\S+)", CONFIG_H))
    pts = [tuple(map(int, m)) for m in
           re.findall(r"\{(\d+), (\d+)\}", CONFIG_H[CONFIG_H.index("RGB_LED_POINTS"):CONFIG_H.index("RGB_LED_FLAGS")])]
    key_leds = list(map(int, re.findall(r"\d+", CONFIG_H[CONFIG_H.index("#define RGB_KEY_LEDS"):CONFIG_H.index("#define RGB_MATRIX_LEDS")])))
    assert len(pts) == 61 and len(key_leds) == 61
    TICK = 12345
    r = booted(syms["board_task"], {RESCUE: RESCUE_MAGIC, FAILS: 0, syms["uwTick"]: TICK}, 3_000_000)
    b, sh = r["board"], r["shadow"]
    assert sh.get(0x40000428) == 0 and sh.get(0x4000042C) == 1049, "TIM3 PSC / ARR"
    assert (sh.get(0x4000041C, 0) >> 8) & 0xF8 == 0x68, "TIM3 CH4 not PWM1 with preload"
    assert sh.get(0x40000420, 0) & (1 << 12) and sh.get(0x4000040C, 0) & (1 << 12), "CC4E / CC4DE"
    assert sh.get(0x40000400, 0) & 1, "TIM3 not running"
    cr = sh.get(0x40026040, 0)
    assert (cr >> 25) & 7 == 5 and cr & (1 << 8) and (cr >> 6) & 3 == 1 and cr & 1, f"DMA1 S2 CR {cr:#x}"
    assert sh.get(0x40026044) == 61 * 24 + 224 and sh.get(0x40026048) == 0x40000440, "DMA NDTR / PAR"
    assert sh.get(0x4002604C) == syms["rgb_slots"], "DMA M0AR"
    assert (sh.get(0x40020800, 0) >> 18) & 3 == 2 and (sh.get(0x40020824, 0) >> 4) & 0xF == 2, "PC9 AF2"

    def leds():
        slots = struct.unpack("<1688H", bytes(b.uc.mem_read(syms["rgb_slots"], 1688 * 2)))
        assert set(slots[:1464]) <= {29, 67} and set(slots[1464:]) == {0}, "slot values"
        out = []
        for i in range(61):
            grb = int("".join("1" if x == 67 else "0" for x in slots[i * 24:i * 24 + 24]), 2)
            out.append(((grb >> 8) & 255, grb >> 16, grb & 255))
        return out

    def sin8(t):
        o = 255 - t if t & 0x40 else t
        o &= 0x3F
        so = (o & 0xF) + (1 if t & 0x40 else 0)
        bm = (0, 49, 49, 41, 90, 27, 117, 10)[(o >> 4) * 2:(o >> 4) * 2 + 2]
        y = ((bm[1] * so) >> 4) + bm[0]
        return ((-y if t & 0x80 else y) + 128) & 255

    def hsv(h, s, v):
        if s == 0:
            return (v, v, v)
        reg = h * 6 // 255
        rem = ((h * 2 - reg * 85) * 3) & 255
        p_, q = (v * (255 - s)) >> 8, (v * (255 - ((s * rem) >> 8))) >> 8
        t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8
        return [(v, t, p_), (q, v, p_), (p_, v, t), (p_, q, v), (t, p_, v), (v, p_, q), (v, t, p_)][reg]

    def rainbow_hue(h0, off, rng):
        # src/rgb.c rainbow_hue (scale8 as QMK builds it: i * (1 + scale) >> 8)
        if rng == 255:
            return (h0 + off) & 255
        fold = (off * 2 if off < 128 else (255 - off) * 2) & 255
        return (h0 + (fold * (1 + rng) >> 8) - rng // 2) & 255

    def pinwheels(tick, h0, s, v, speed, rng=255):
        time = ((tick & 0xFFFF) * (1 + speed // 4) >> 8) & 0xFFFF
        cos_v, sin_v = sin8((time + 64) & 255) - 128, sin8(time & 255) - 128
        return [hsv(rainbow_hue(h0, int(((y - 32) * 3 * sin_v + (56 - abs(x - 112)) * 3 * cos_v) / 128) & 255, rng), s, v)
                for x, y in pts]

    def bad(got, want):
        return [(i, got[i], want[i]) for i in range(61) if got[i] != want[i]][:4]

    want = pinwheels(TICK, int(cfg["RGB_DEFAULT_HUE"]), int(cfg["RGB_DEFAULT_SAT"]),
                     int(cfg["RGB_DEFAULT_VAL"]), int(cfg["RGB_DEFAULT_SPEED"]))
    got = leds()
    assert got == want, f"default effect (led, got, want): {bad(got, want)}"

    BUF_IN, BUF_OUT = 0x20010000, 0x20010100

    def command(*payload):
        b.uc.mem_write(BUF_IN, bytes(payload).ljust(64, b"\0"))
        b.uc.mem_write(BUF_OUT, bytes(64))
        b.run(syms["rgb_command"], 2_000_000, (BUF_IN, BUF_OUT))
        from unicorn.arm_const import UC_ARM_REG_R0
        return b.reg(UC_ARM_REG_R0) & 0xFF, bytes(b.uc.mem_read(BUF_OUT, 64))

    def frame(tick):
        b.w32(syms["uwTick"], tick)
        b.run(syms["board_task"], 3_000_000)

    ok, out = command(0xE0)
    assert ok and list(out[1:9]) == [13, 0, 255, 210, 70, 21, int(cfg["RGB_MAX_BRIGHTNESS"]), 255], f"GET_RGB {list(out[1:9])}"
    # Colour range: Rainbow Pinwheels folded into a band around hue 100
    # (frames 20 ms apart, the frame rate; later frames restart at TICK + 40,
    # which rgb_task's unsigned elapsed time also treats as due)
    for k, rng in enumerate((0, 60, 254)):
        ok, _ = command(0xE1, 13, 100, 255, 210, 70, rng)
        assert ok, "SET_RGB range"
        t = TICK + 2000 + 20 * k
        frame(t)
        got, want = leds(), pinwheels(t, 100, 255, 210, 70, rng)
        assert got == want, f"pinwheels range {rng}: {bad(got, want)}"
    ok, out = command(0xE0)
    assert out[8] == 254, "readback of the colour range"
    ok, _ = command(0xE1, 21, 0, 0, 0, 0)
    assert not ok, "SET_RGB accepted effect 21 (out of range)"
    ok, _ = command(0xE1, 2, 152, 195, 210, 70, 255)       # solid stock colour
    assert ok, "SET_RGB solid"
    frame(TICK + 40)
    got, want = leds(), [hsv(152, 195, 210)] * 61
    assert got == want, f"solid colour: {bad(got, want)}"
    ok, out = command(0xE0)
    assert list(out[1:6]) == [2, 152, 195, 210, 70] and out[8] == 255, "readback after SET_RGB"
    colors = [(k, 255 - k, k * 3 & 255) for k in range(61)]
    for off in range(0, 61, 20):
        part = colors[off:off + 20]
        ok, _ = command(0xE3, off, len(part), *[c for rgb in part for c in rgb])
        assert ok, f"SET_RGB_KEY_COLORS {off}"
    ok, out = command(0xE2, 40)
    assert ok and out[1] == 20 and list(out[2:5]) == list(colors[40]), "GET_RGB_KEY_COLORS"
    ok, _ = command(0xE1, 1, 152, 195, 255, 70)             # custom, full brightness
    frame(TICK + 80)
    got = leds()
    want = [None] * 61
    for k, led in enumerate(key_leds):
        want[led] = tuple((c * 256) >> 8 for c in colors[k])  # scale8(c, 255)
    assert got == want, f"per-key colours: {bad(got, want)}"
    print("10 RGB: TIM3/DMA1/PC9 as stock; default Rainbow Pinwheels matches the "
          "reference; colour range 0 / 60 / 254 match the reference; GET/SET_RGB, range check, solid colour, per-key colours and "
          "the custom effect all verified")

    # 11. Every effect renders frames without faulting (each frame returns to
    # the caller, watchdog refreshed), and the reactive effects answer a key
    # press (A = key 29) with light around that key's LED.
    from unicorn.arm_const import UC_ARM_REG_PC
    KEY_A, IS_PRESSED = 29, 9
    names = re.findall(r"^\s*RGB_EFFECT_(\w+)[ ,=]", (
        REPO / "include" / "eeconfig.h").read_text(), re.M)
    names = [n for n in names if n != "COUNT"]
    assert len(names) == 21, names
    tick = TICK + 1000
    summary = []
    for effect, name in enumerate(names):
        ok, _ = command(0xE1, effect, 85, 255, 210, 128, 255)
        assert ok, f"SET_RGB {name}"
        lit = []
        for _ in range(4):
            tick += 40
            b.log.clear()
            frame(tick)
            assert b.reg(UC_ARM_REG_PC) == RET, f"{name}: frame did not return"
            assert (0x40003000, 0xAAAA) in b.log, f"{name}: watchdog not refreshed"
            lit.append(sum(1 for c in leds() if c != (0, 0, 0)))
        got = leds()
        if name == "OFF":
            assert lit[-1] == 0, "OFF lit LEDs"
        if name == "DIGITAL_RAIN":
            # Drops start at random (1 in 24 per column per drop cycle)
            most = 0
            for _ in range(120):
                tick += 40
                frame(tick)
                got = leds()
                assert all(r == bb and r <= g for r, g, bb in got), "digital rain not green"
                most = max(most, sum(1 for c in got if c != (0, 0, 0)))
            assert most > 0, "digital rain never started a drop"
            lit.append(most)
            # The drops take the base colour: hue 170 (blue) has no red in
            # the dim tail and blue as the strongest channel
            ok, _ = command(0xE1, effect, 170, 255, 210, 128, 255)
            seen = 0
            for _ in range(120):
                tick += 40
                frame(tick)
                for r_, g, bb in leds():
                    if (r_, g, bb) != (0, 0, 0):
                        assert bb >= g and bb >= r_, f"digital rain not blue {(r_, g, bb)}"
                        seen += 1
            assert seen, "blue digital rain stayed dark"
        if name in ("SOLID_REACTIVE_SIMPLE", "SOLID_REACTIVE_MULTICROSS", "SPLASH"):
            # Let earlier hits expire (the tracker drops them after ~65 s)
            tick += 70_000
            frame(tick)
            tick += 40
            frame(tick)
            assert all(c == (0, 0, 0) for c in leds()), f"{name} lit without a key press"
            b.uc.mem_write(syms["key_matrix"] + KEY_A * 10 + IS_PRESSED, b"\x01")
            tick += 40
            frame(tick)
            b.uc.mem_write(syms["key_matrix"] + KEY_A * 10 + IS_PRESSED, b"\x00")
            tick += 40
            frame(tick)
            got = leds()
            led = key_leds[KEY_A]
            assert got[led] != (0, 0, 0), f"{name}: pressed key's LED dark"
            lit.append(sum(1 for c in got if c != (0, 0, 0)))
        summary.append(f"{name.lower()} {lit[-1]}")
    print("11 effects: all 21 render and return; LEDs lit per effect: " + ", ".join(summary))
else:
    print("10 RGB: not in this build")

# 12. COMMAND_READ_MEMORY (board_read_memory): whitelisted regions only, registers
# as whole words, nothing written
if "board_read_memory" in syms:
    OPTCR, IDCODE, OUT = 0x40023C14, 0xE0042000, 0x20010000
    b = Board(ELF, {OPTCR: 0x0FFFAAED, IDCODE: 0x10076413})
    b.uc.mem_write(0x08000000, bytes(range(64)))
    b.uc.mem_write(0x1FFF76DE, b"\x91")

    def rd(addr, n):
        b.uc.mem_write(OUT, bytes(64))
        b.flash_writes.clear()
        b.run(syms["board_read_memory"], 20_000, (addr, n, OUT))
        from unicorn.arm_const import UC_ARM_REG_R0
        assert not b.flash_writes, f"read {addr:#x} wrote flash"
        return b.reg(UC_ARM_REG_R0) & 0xFF, bytes(b.uc.mem_read(OUT, n))

    assert rd(0x08000000, 63) == (1, bytes(range(63))), "sector 0"
    assert rd(0x1FFF76DE, 1) == (1, b"\x91"), "ROM bootloader version"
    assert rd(OPTCR, 4) == (1, struct.pack("<I", 0x0FFFAAED)), "FLASH_OPTCR"
    assert rd(IDCODE, 4) == (1, struct.pack("<I", 0x10076413)), "DBGMCU_IDCODE"
    for addr, n in ((0x0801FFF0, 32), (0x08020000, 4), (0x080E0000, 4), (0x20000000, 4),
                    (0x1FFF7A20, 8), (OPTCR + 1, 2), (OPTCR, 8), (0x07FFFFFF, 2)):
        assert rd(addr, n)[0] == 0, f"read {addr:#x}+{n} allowed"
    print("12 read memory: sector 0, ROM, FLASH_OPTCR, IDCODE readable; app, settings, RAM, "
          "edges and misaligned registers refused; no flash writes")
else:
    print("12 read memory: not in this build")

# 13. bootloader install (board_install_bootloader): only sector 0 erased, only
# the image written, anything wrong refused before the erase
if "board_install_bootloader" in syms:
    import zlib
    from unicorn.arm_const import UC_ARM_REG_R0
    STAGE = syms["board_boot_image"]

    def sector0(n):
        """A sector 0 image: SP in RAM, Thumb reset vector inside it, filler."""
        return struct.pack("<II", 0x20010000, 0x08000101) + bytes(i * 7 & 0xFF for i in range(n - 8))

    boot_raw, full = sector0(11944), sector0(0x4000)

    def install(image, length=None, crc=None, stage=True):
        b = Board(ELF)
        b.uc.mem_write(0x08000000, bytes(range(256)) * 64)      # "old bootloader"
        if stage:
            b.uc.mem_write(STAGE, image)
        b.run(syms["board_install_bootloader"], 5_000_000,
              (len(image) if length is None else length, zlib.crc32(image) if crc is None else crc))
        erases = [(v >> 3) & 0xF for a, v in b.log if a == 0x40023C10 and v & 0x10002 == 0x10002]
        return b.reg(UC_ARM_REG_R0) & 0xFF, erases, b.flash_writes, bytes(b.uc.mem_read(0x08000000, 0x4000))

    for name, image in (("small image", boot_raw), ("full sector", full)):
        ok, erases, writes, sector = install(image)
        assert ok == 1 and erases == [0], f"{name}: ok {ok} erases {erases}"
        assert writes and min(writes) == 0x08000000 and max(writes) < 0x08000000 + len(image), name
        # the emulator does not model erase, so check only the written range
        assert sector[:len(image)] == image, f"{name}: content"
    bad_vt = bytearray(boot_raw)
    bad_vt[4:8] = struct.pack("<I", 0x08020001)                 # reset vector outside the image
    for why, args in (("wrong CRC", dict(image=boot_raw, crc=1)),
                      ("length not a multiple of 4", dict(image=boot_raw, length=len(boot_raw) - 2)),
                      ("longer than the sector", dict(image=boot_raw, length=0x4004)),
                      ("SP outside RAM", dict(image=b"\xff" * 4 + boot_raw[4:])),
                      ("reset vector outside the image", dict(image=bytes(bad_vt))),
                      ("nothing staged", dict(image=boot_raw, stage=False))):
        ok, erases, writes, _ = install(**args)
        assert ok == 0 and not erases and not writes, f"{why} accepted"

    def stage(offset, n):
        b = Board(ELF)
        b.run(syms["board_stage_bootloader"], 50_000, (offset, n, 0x20010000))
        return b.reg(UC_ARM_REG_R0) & 0xFF
    assert stage(0x4000 - 60, 60) == 1 and stage(0x4000 - 59, 60) == 0 and stage(0x4000, 0) == 0
    assert stage(0xFFFFFFF0, 32) == 0, "stage offset wrap"
    print(f"13 bootloader install: {len(boot_raw)}-byte and full 16 KB images "
          "written to sector 0 only; wrong CRC, odd length, oversize, bad SP / reset vector and "
          "unstaged data refused before any erase; staging bounds hold")
else:
    print("13 bootloader install: not in this build")
print("all ok")
