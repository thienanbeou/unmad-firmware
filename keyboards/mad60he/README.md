# MAD60 HE

Part of **project Unmad**: fully open firmware for the Madlions MAD60 HE, in
three repos:

| Repo | What |
|---|---|
| [unmad-firmware](https://github.com/thienanbeou/unmad-firmware) (this libhmk fork) | the firmware, the flashing tools, emulator checks, hardware notes |
| [unmad-bootloader](https://github.com/thienanbeou/unmad-bootloader) | an open replacement for the vendor bootloader (optional) |
| [unmad-configurator](https://github.com/thienanbeou/unmad-configurator) | the configurator (hmkconf fork); runs in the browser at https://thienanbeou.github.io/unmad-configurator/ |

Unofficial; not made or supported by Madlions. You flash it at your own risk,
but going back to the stock firmware is one command (below).

## Quick start (from the stock firmware or an older Unmad release)

You need: the keyboard plugged in by USB, Python 3, and the
[release files](https://github.com/thienanbeou/unmad-firmware/releases)
`unmad-firmware-mad60he-v1.1.bin` and `fwtool.py` in one folder.

1. Install the HID library once: `pip install hidapi`.
   Linux only: allow access without root, then replug:

       echo 'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="373b", MODE="0666"
       SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", MODE="0666"' \
         | sudo tee /etc/udev/rules.d/50-mad60he.rules && sudo udevadm control --reload

2. Close the vendor's web app or driver if it is open.
3. Flash (about 10 seconds; keep the cable in):

       python fwtool.py flash unmad-firmware-mad60he-v1.1.bin --yes --keep-settings

   `--keep-settings` leaves your stock settings untouched in flash, so they
   are still there if you go back.
4. The keyboard restarts as "MAD60 HE" by "Unmad". **Keep your hands off the
   keys while it starts** (and whenever you plug it in): the firmware measures
   every key's rest position in its first half second.
5. Configure it in the browser (Chrome, Edge or Brave):
   https://thienanbeou.github.io/unmad-configurator/. Keymap, per-key
   actuation and Rapid Trigger, tap dance and other advanced keys, macros,
   lighting, WASD swap / Win lock / 6KRO, and from v1.1 Mac mode and the
   Caps Lock indicator (its colour and brightness in the Lighting tab).

Upgrading from v1.0 is the same flash command. Your settings are kept and
upgraded on the first start, nothing is reset. Your saved keymap stays as it
is, so Fn + M only becomes Mac mode if you bind it (Special keycodes: Mac
Mode) or use the switch in Settings. Reload the configurator page afterwards.

If the keyboard ever stops responding (firmware v1.1+): unplug it, hold
**Esc**, plug it in and let go. It starts in the bootloader, ready for
`fwtool.py flash`; a plain replug starts the firmware again.

## Default keymap

The stock layout (read from the official 1.06 firmware), 1.5 mm actuation
on every key. Blank = transparent (the layer below shows through), `--` =
no key: stock had its RGB controls there; set lighting in the
configurator. Win/Mac mode is Fn + M as on stock (firmware v1.1; the v1.0
release has nothing there, and an upgrade keeps your saved keymap). Generated
from `keyboard.json`.

```
Layer 0 (base)
,-----------------------------------------------------------------------------------------.
| Esc |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  8  |  9  |  0  |  -  |  =  |    Bksp   |
|-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------|
|  Tab   |  Q  |  W  |  E  |  R  |  T  |  Y  |  U  |  I  |  O  |  P  |  [  |  ]  |   \    |
|--------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+--------|
|   Caps  |  A  |  S  |  D  |  F  |  G  |  H  |  J  |  K  |  L  |  ;  |  '  |    Enter    |
|---------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-------------|
|    Shift    |  Z  |  X  |  C  |  V  |  B  |  N  |  M  |  ,  |  .  |  /  |     Shift     |
|-------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+---------------|
|  Ctrl | Win  | Alt  |                Space                |  Alt  | Menu | Ctrl |   Fn  |
`-----------------------------------------------------------------------------------------'

Layer 1 (Fn: hold the rightmost bottom key)
,-----------------------------------------------------------------------------------------.
|  `  |  F1 |  F2 |  F3 |  F4 |  F5 |  F6 |  F7 |  F8 |  F9 | F10 | F11 | F12 |           |
|-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------|
|   --   |  -- |  -- |  -- |     |     |     |     |     |     |     |     |     |  WASD  |
|--------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+--------|
|         |  -- |  -- |  -- |     |     |     |     |     |     |     |     |             |
|---------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-------------|
|             |     |     |     |     |     | 6KRO| Mac |     |     |  Up |               |
|-------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+---------------|
|       |WinLk |      |                                     |  Left | Down |Right |       |
`-----------------------------------------------------------------------------------------'

Layer 2 (stock Mac base; not reachable by default)
,-----------------------------------------------------------------------------------------.
| Esc |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  8  |  9  |  0  |  -  |  =  |    Bksp   |
|-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------|
|  Tab   |  Q  |  W  |  E  |  R  |  T  |  Y  |  U  |  I  |  O  |  P  |  [  |  ]  |   \    |
|--------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+--------|
|   Caps  |  A  |  S  |  D  |  F  |  G  |  H  |  J  |  K  |  L  |  ;  |  '  |    Enter    |
|---------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-------------|
|    Shift    |  Z  |  X  |  C  |  V  |  B  |  N  |  M  |  ,  |  .  |  /  |     Shift     |
|-------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+---------------|
|  Ctrl | Alt  | Win  |                Space                |  Win  | Alt  | Ctrl |  MO3  |
`-----------------------------------------------------------------------------------------'

Layer 3 (stock Mac Fn; not reachable by default)
,-----------------------------------------------------------------------------------------.
|  `  |  F1 |  F2 |  F3 |  F4 |  F5 |  F6 |  F7 |  F8 |  F9 | F10 | F11 | F12 |           |
|-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------|
|   --   |  -- |  -- |  -- | LPad| Bri+| Bri-| Prev| Play| Next| Mute| Vol+| Vol-|  WASD  |
|--------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+--------|
|         |  -- |  -- |  -- |     |     |     |     |     |     |     |     |             |
|---------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-------------|
|             |     |     |     |     |     | 6KRO| Mac |     |     |  Up |               |
|-------------+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+---------------|
|       |      |      |                                     |  Left | Down |Right |       |
`-----------------------------------------------------------------------------------------'
```

## Going back to stock

    python fwtool.py download                    # the official images, from the vendor's server
    python fwtool.py flash Mad60_list_v1.06.bin --yes

Use the stock version you had before (the vendor app shows it; 1.06 is the
newest as of 2026-10). The same version keeps your stock settings; another
one resets them on its first boot. Switching back and forth keeps both
firmwares' settings: stock keeps flash sectors 1-3, this firmware sector 11.
To come back: the quick start again.

## Changing the bootloader (optional)

The board's own bootloader (in flash sector 0) is the last closed-source
piece. You can replace it with the open
[unmad-bootloader](https://github.com/thienanbeou/unmad-bootloader), which
speaks the same protocol, so everything above keeps working. From this
firmware, with the release files `unmad-bootloader-mad60he-v1.0.bin` and
`hmk_bootloader.py`:

    python hmk_bootloader.py install unmad-bootloader-mad60he-v1.0.bin --yes

It first saves your current (vendor) bootloader as
`sector0-before-install-<time>.bin` in the current folder: **keep that file**,
installing it the same way is the only way back. Then it writes the new one
and reads all 16 KB back. Do not unplug while it runs: losing power in that
second leaves the board without boot code, recoverable only with an SWD
programmer. `python hmk_bootloader.py verify FILE` compares without writing.

## Hardware

Madlions MAD60 HE ("MAD 60 Limit", stock USB ID 373B:105D), STM32F405-class
chip (a Geehy APM32F405 on the tested board). Hardware map, flash map,
bootloader protocol and how they were found: [`NOTES.md`](NOTES.md).

- Matrix: 14 columns driven one at a time (active low), 5 rows read by ADC1
  (PA3, PA2, PA0, PA1, PA7), no multiplexer chips; each row is sampled 3
  times per column and averaged (`ADC_OVERSAMPLE`), 10 us after switching
  the column. Rest noise is about 0.4 ADC counts (fine enough for 0.05 mm
  actuation); the bottom row reads about 2.6 while a moving lighting effect
  runs, because the LED data line couples into that row's sensor line.
- USB: OTG_HS core, embedded full-speed PHY (PB14/PB15).
- RGB: 61 WS2812 on PC9 (driver `src/hardware/stm32f405xx/rgb.c`, effects
  `src/rgb.c`: all of QMK's RGB Matrix effects, 52 from v1.1 (21 in v1.0,
  the stock list), a colour range for the rainbow effects, per-key colours),
  set at runtime with raw HID commands 0xE0-0xE3. From v1.1 the Caps Lock key
  lights while Caps Lock is on, in its own colour and brightness (white 210 by
  default, commands 0xE7 / 0xE8; option bit 7 turns it off). From v1.2 the
  speed sets how fast Typing Heatmap fades (127 = QMK's fixed rate, lower
  fades slower), and a white balance (R, G, B gain on every LED, 255 =
  unchanged, commands 0xE9 / 0xEA) corrects the LEDs' colour. Defaults in
  `config.h`: Rainbow Pinwheels (the stock "Colorful Swirls 2"), brightness
  210 (the stock maximum; the ceiling is 255).
- Stock switches: WASD swap, Win lock, 6KRO (option bits 3-5, keys
  `SP_WASD_SWAP`, `SP_GUI_LOCK`, `SP_NKRO_TOGGLE`), and from v1.1 Mac mode
  (Alt and Win swapped, bit 6, `SP_MAC_MODE`, the stock Win/Mac switch).
- QMK features (v1.1): Grave Escape (`SP_GRAVE_ESCAPE`: Esc, `` ` `` with
  Shift or Win, Esc again with Ctrl + Shift), Repeat Key (`SP_REPEAT_KEY`:
  the last key with its modifiers), Bootmagic Lite (Esc held while plugging
  in). QMK's `TG` (toggle a layer) is Layer Lock here: hold Fn and press
  Layer Lock to keep layer 1 on, the same again to leave it.
- QMK layer keys (v1.2, the Layers section of the configurator's key list):
  `TO(n)` (only layer n stays on), `DF(n)` (layer n becomes the default
  until a restart), One Shot `OSL(n)` and `OSM` (tap: the layer or modifier
  applies to the next key; held: like MO / the plain modifier), and Tri
  Layer Lower / Upper (MO(1) / MO(2), both held: layer 3). A layer indicator
  lights the keys that do something on the active layer (any layer but 0)
  in its own colour (blue by default; option bit 8 turns it off).
- Tap dance is an advanced key type of this fork (not in upstream libhmk);
  edit it in the Unmad configurator. Upstream hmkconf (hmkconf.com) lists tap
  dances as "?" and corrupts them on save, and it clears the switches above
  (option bits 3-8) when it saves the settings: use the Unmad configurator.
- Flash use: firmware at 0x08020000, settings in sector 11, stock settings in
  sectors 1-3 never written, sector 0 (bootloader) written only by
  COMMAND_INSTALL_BOOTLOADER.
- Rescue: a crash, two boots in a row without a working USB connection, or a
  watchdog reset (a hang) send the board to the bootloader by itself, so a
  bad build can always be reflashed over USB.

## Building

PlatformIO, from the repository root (see the main README for setup):

    python setup.py -k mad60he
    pio run
    arm-none-eabi-objcopy -O binary .pio/build/mad60he/firmware.elf raw.bin
    (pad raw.bin with 0xFF to a multiple of 16)
    python keyboards/mad60he/tools/fwtool.py download --out official
    python keyboards/mad60he/tools/fwtool.py pack raw.bin mad60he.bin --like official/Mad60_list_v1.06.bin
    python keyboards/mad60he/tools/fwtool.py flash mad60he.bin --yes --keep-settings

Use the objcopy output, not PlatformIO's `firmware.bin` (it ends in a 16-byte
DFU suffix). The official image only lends its container header (USB IDs);
none of its code ends up in yours. Run the [emulator checks](tests) on the
ELF before flashing.

Settings survive reflashes. A build with new defaults keeps the old settings
until a factory reset (configurator, or raw HID command 3); a build that
changes the settings layout (`EECONFIG_VERSION`) migrates them
(`src/migration.c`).

USB ID: 1209:0001 is the pid.codes test ID, fine for building and flashing
your own board. If you make and distribute hardware, get your own PID from
[pid.codes](https://pid.codes) and change `usb` in `keyboard.json`. The
configurator and fwtool find the firmware by its raw HID usage page
(0xFFAB), not the ID.
