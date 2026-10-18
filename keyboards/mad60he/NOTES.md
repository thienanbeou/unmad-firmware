# MAD60 HE hardware and firmware notes

Project Unmad's reference notes. What this port is built on, found by reading the official firmware images and
the board's bootloader in a disassembler and an emulator, and confirmed on one
board (2026-10). Other production runs may differ; facts marked "unverified"
were not checked on hardware.

## Chip

- Geehy **APM32F405**, an STM32F405-compatible clone. libhmk built for the
  STM32F405 (ST HAL, 168 MHz) runs on it unchanged.
- How it was identified: the system ROM (0x1FFF0000) contains "Geehy" and
  "APM32 ISP DFU mode"; DBGMCU_IDCODE reads 0x0009A413 (ST-like device id
  0x413, readable without a debugger); the ROM version byte at 0x1FFF76DE is
  0x00 (ST's F4 ROMs have 0x90 / 0x91).
- Flash: the flash size register (0x1FFF7A22) says 512 KB, but the ROM's DFU
  flash map lists 1 MB in 12 sectors (4 x 16 KB, 64 KB, 7 x 128 KB). Sector 11
  is written and read back fine by this firmware; whether it is real or
  aliased is unverified (it would land in an unused sector either way).
- Protection: RDP level 0 (no readout protection), no sector write
  protection (FLASH_OPTCR 0x0FFFAAED).
- RAM: 128 KB at 0x20000000 (+ 64 KB CCM at 0x10000000).

## Flash map

| Sector(s) | Address | Size | Content |
|---|---|---|---|
| 0 | 0x08000000 | 16 KB | bootloader (vendor, or the open `mad60boot`) |
| 1-3 | 0x08004000 | 48 KB | stock firmware settings (EasyFlash; the stock firmware's first-boot format erases 1-3) |
| 4 | 0x08010000 | 64 KB | empty |
| 5-7 | 0x08020000 | 384 KB | firmware (stock or this port); the bootloader erases at most these |
| 8-10 | 0x08040000 | 384 KB | unused |
| 11 | 0x080E0000 | 128 KB | this port's settings (libhmk eeconfig) |

The two firmwares never touch each other's settings, so flashing one over the
other and back keeps both (tested both ways).

## Hardware map

- Clock: 16 MHz crystal (HSE), PLL M 16, N 336, P 2, Q 7: 168 MHz system,
  48 MHz USB, APB1 /4, APB2 /2, 5 flash wait states.
- USB: the **OTG_HS** core with its embedded full-speed PHY on PB14 (D-) /
  PB15 (D+), AF12, no VBUS sensing. OTG_FS is unused.
- Matrix: no multiplexer chips. 14 columns, one driven at a time, push-pull,
  **active low** (all high when idle): PA8, PA9, PA10, PA11, PA12, PA15, PC10,
  PC11, PC12, PD2, PB3, PB4, PB5, PB6. 5 rows read as analog by ADC1: PA3
  (ch3), PA2 (ch2), PA0 (ch0), PA1 (ch1), PA7 (ch7). About 20 us settle time
  after switching a column. 12-bit readings: rest about 1850-2060, bottomed
  out about 1110-1160; the reading falls as the key goes down.
- Extra analog input PB1 (ch9). The stock firmware only scans while PB1 and
  VREFINT are above thresholds; PB1 probably senses the Hall sensor supply
  (unverified).
- RGB: 61 WS2812-type LEDs on PC9 = TIM3_CH4 (AF2), fed by DMA1 stream 2
  channel 5. The stock firmware uses a 12.5 us bit period (WS2812 decodes the
  high time, so the long period works).
- SWD: PA13 / PA14 stay as debug pins; PB3 (SWO) is reused as a column.
- ST's (and Geehy's) ROM DFU only drives OTG_FS on PA11 / PA12, which are
  matrix columns here, so the ROM bootloader can never reach the USB port.
  That is why the board needs its own bootloader in sector 0.

## USB IDs

| Device | ID |
|---|---|
| stock firmware | 373B:105D, config interface usage page 0xFF60 / usage 0x61, 32-byte reports |
| bootloader (vendor and mad60boot) | 373B:2028, usage page 0x00FF / usage 0x01, 64-byte reports |
| this port | 1209:0001 (pid.codes test ID), raw HID usage page 0xFFAB |

## Stock firmware

- QMK + Vial (Vial protocol 6, VIA protocol 9) on ChibiOS, plus a vendor
  command channel (first byte 0x96).
- Entering the bootloader: a 32-byte report on the config interface,
  `96 96 96 98` (clears the settings first) or `96 96 00 98` (no wipe). The
  firmware then sets the bootloader flag (below), resets the peripherals and
  clocks, clears the NVIC, switches to the main stack and **jumps** into the
  bootloader (no reset). `QK_BOOT` is an empty stub in the stock firmware.
- Every official release changes a settings format marker, so going to a
  different stock version resets the stock settings on its first boot.
  Flashing the same version keeps them.

## Bootloader

### Entering it

The word at **0x2001FFFC** (top of RAM) survives a reset but not a power
cycle. At reset the bootloader reads it:

- not 0x96969696: write 0x96969696 there and start the firmware at
  0x08020000 (if its initial stack pointer points into RAM);
- 0x96969696: clear it and stay in USB mode until told to start the firmware.

So any firmware can enter the bootloader by writing 0x96969696 there and
resetting (tested with a software reset and with a watchdog reset), and a
firmware that resets before clearing the word lands in the bootloader too.
This port clears it at boot and uses it for its rescue rules (crash, two
boots in a row without USB, watchdog). RCC_CSR reset flags are sticky across
these resets (the stock firmware never clears them).

### Flash protocol

64-byte HID reports; byte 63 is the OR of bytes 0-62. The reply echoes the
request with byte 63 = that OR, inverted when the command failed. Multi-byte
values are big-endian.

| Request | Meaning |
|---|---|
| `AA 01 <u32 pages> <u32 base>` | start: number of 2 KB pages, base 0x08020000 |
| `AA 02 <u32 sectors>` | erase that many 128 KB sectors from sector 5 (at most 3) |
| `AA 03 <u32 addr> <idx> <len> <last> <data>` | page data, up to 54 bytes per report; `idx` 0 starts a page at `addr`, `last` programs it |
| `AA 96` | done, start the firmware |

The first 8 bytes of the image (initial stack pointer and reset vector) are
programmed only after the last page, so an interrupted flash never leaves a
firmware that looks valid. The vendor bootloader has no read command, no
address check (it would write anywhere, itself included) and drops the last
bytes of a page whose length is not a multiple of 4; `mad60boot` restricts
writes to sectors 5-7, reads every word back and pads with 0xFF.

### Firmware container

What the vendor updater and `tools/fwtool.py` flash (big-endian):

| Offset | Field |
|---|---|
| 0 | u16 firmware VID, u16 firmware PID |
| 4 | u16 bootloader VID, u16 bootloader PID |
| 8 | u8 major, u8 minor version |
| 10 | u32 image length N |
| 14 | image (N bytes, loaded at 0x08020000) |
| 14 + N | u32 checksum = 0x55555555 - byte sum of the image |

### Replacing it

The bootloader lives in sector 0 and nothing protects it, so firmware can
rewrite it. This port's `COMMAND_STAGE_BOOTLOADER` / `COMMAND_INSTALL_BOOTLOADER`
do that (`tools/hmk_bootloader.py`). Losing power during the write (about a
second) leaves the board without boot code; only SWD recovers it then.
