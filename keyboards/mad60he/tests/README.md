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
| `check_mad60he_emu.py` | `firmware.elf` | boot, rescue rules (crash, failed boots, watchdog), flash guard, lighting registers and all effects (reactive ones dark at rest, lit around a pressed key), Caps Lock indicator, read-memory whitelist, bootloader install, Bootmagic Lite (14 scenarios + 11b) |
| `check_tapdance_emu.py` | `firmware.elf` | key handling: tap dance, macros, null bind, WASD swap / Win lock / 6KRO incl. the HID bitmap the host reads, Grave Escape, Repeat Key, Mac mode, Layer Lock as TG (22 checks). Loads its own test setup through the raw HID commands, so it works with any default keymap that has Fn (MO(1)) on the stock Fn key |
| `check_migration_emu.py` | `OLD.elf NEW.elf` | settings migration (OLD eeconfig v1.5 or v1.6, NEW v1.7): every byte kept, defaults filled, second boot stable |
| `check_boot_emu.py` | `BOOT.elf APP.bin [STOCK.bin]` | the open bootloader (mad60boot): boot rule, USB setup, a full `fwtool.py flash` session against a flash controller model, refused writes, the stock firmware's jump into it (8 checks). `APP.bin` / `STOCK.bin` are fwtool containers |

`hmk_emu.py` is the shared chip model. Each script prints one line per check
and `all ok` at the end, or stops at the first failed assertion.
