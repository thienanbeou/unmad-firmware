# MAD60 HE emulator checks

Run the real firmware images in [Unicorn](https://www.unicorn-engine.org/)
(a CPU emulator) with a small model of the chip's peripherals, and assert on
what the code does. No hardware needed. Run them on the exact ELF you flash.

Needs Python 3, `pip install unicorn==2.1.1`, and the PlatformIO ARM toolchain
(`hmk_emu.py` calls `~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm`).
Every ELF needs its raw image next to it, named `raw.bin`:

    arm-none-eabi-objcopy -O binary firmware.elf raw.bin

| Check | Usage | What it covers |
|---|---|---|
| `check_mad60he_emu.py` | `firmware.elf` | boot, rescue rules (crash, failed boots, watchdog), flash guard, lighting registers, LED data sent once per change, all effects (reactive ones dark at rest, lit around a pressed key), Caps Lock and layer indicators and their colours, white balance, read-memory whitelist, bootloader install, Bootmagic Lite (14 scenarios + 11b, 11c) |
| `check_tapdance_emu.py` | `firmware.elf` | key handling: tap dance, macros, null bind, WASD swap / Win lock / 6KRO incl. the HID bitmap the host reads, Grave Escape, Repeat Key, Mac mode, Layer Lock as TG, the lighting commands reach rgb.c, One Shot Shift / layer, TO / DF, Tri Layer (27 checks). Loads its own test setup through the raw HID commands, so it works with any default keymap that has Fn (MO(1)) on the stock Fn key |
| `check_migration_emu.py` | `OLD.elf NEW.elf` | settings migration (OLD eeconfig v1.5 to v1.9, NEW v1.10): every byte kept, defaults filled, second boot stable |
| `check_boot_emu.py` | `BOOT.elf APP.bin [STOCK.bin]` | the open bootloader (mad60boot): boot rule, USB setup, a full `fwtool.py flash` session against a flash controller model, refused writes and bad checksums, the stock firmware's jump into it, the Esc rescue (v4: Esc held while plugging in, against an ADC model) and the AA 96 reset (10 checks). `APP.bin` / `STOCK.bin` are fwtool containers |

`hmk_emu.py` is the shared chip model. Each script prints one line per check
and `all ok` at the end, or stops at the first failed assertion.
