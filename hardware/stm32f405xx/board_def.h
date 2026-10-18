/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

//--------------------------------------------------------------------+
// ADC Configuration
//--------------------------------------------------------------------+

#if !defined(ADC_NUM_SAMPLE_CYCLES)
// Number of sample cycles for each ADC conversion (stock MAD60 HE firmware
// samples every channel for 28 cycles)
#define ADC_NUM_SAMPLE_CYCLES ADC_SAMPLETIME_28CYCLES
#endif

#if !defined(ADC_OVERSAMPLE)
// Conversions per multiplexer input per select state, averaged (stock takes 3
// per row). The regular sequence holds at most 16 conversions.
#define ADC_OVERSAMPLE 3
#endif

// ADC resolution in bits, set by `scripts/make.py`
#if ADC_RESOLUTION == 12
#define ADC_RESOLUTION_HAL ADC_RESOLUTION_12B
#elif ADC_RESOLUTION == 10
#define ADC_RESOLUTION_HAL ADC_RESOLUTION_10B
#elif ADC_RESOLUTION == 8
#define ADC_RESOLUTION_HAL ADC_RESOLUTION_8B
#elif ADC_RESOLUTION == 6
#define ADC_RESOLUTION_HAL ADC_RESOLUTION_6B
#else
#error "Unsupported ADC resolution"
#endif

//--------------------------------------------------------------------+
// USB Configuration
//--------------------------------------------------------------------+

// USB runs on the OTG_HS core with its embedded full-speed PHY (PB14/PB15).
// keyboard.json says "fs" so the descriptors use full-speed timing.
// ponytail: fixed to that wiring, add a keyboard.json option if an F405 board
// with USB on OTG_FS (PA11/PA12) shows up.
#define BOARD_TUD_RHPORT 1

//--------------------------------------------------------------------+
// Rescue Configuration
//--------------------------------------------------------------------+

// Consecutive boots that never reached a configured USB connection, after
// which the next boot goes straight to the vendor bootloader
#if !defined(BOARD_RESCUE_MAX_FAILED_BOOTS)
#define BOARD_RESCUE_MAX_FAILED_BOOTS 2
#endif

// Watchdog timeout in milliseconds; the main loop refreshes it
#if !defined(BOARD_RESCUE_WATCHDOG_MS)
#define BOARD_RESCUE_WATCHDOG_MS 2000
#endif

// If USB is not configured this many milliseconds after boot, go to the
// bootloader. 0 disables it (a PC that is asleep at power-up would otherwise
// send the board to the bootloader). Bring-up builds set it.
#if !defined(BOARD_RESCUE_NO_USB_MS)
#define BOARD_RESCUE_NO_USB_MS 0
#endif

// Go to the bootloader after any watchdog reset (RCC_CSR IWDGRSTF), without
// waiting for the failed boot count. Works even if the bootloader clears RAM.
#if !defined(BOARD_RESCUE_ON_WATCHDOG)
#define BOARD_RESCUE_ON_WATCHDOG 0
#endif

// COMMAND_READ_MEMORY: read-only access to the bootloader sectors, system
// memory, option bytes and the ID registers (board_read_memory)
#define BOARD_HAS_READ_MEMORY

// COMMAND_STAGE_BOOTLOADER / COMMAND_INSTALL_BOOTLOADER: replace the bootloader
// in sector 0 (board_install_bootloader). Power loss while it runs (about a
// second) leaves the board without boot code.
#define BOARD_HAS_BOOTLOADER_INSTALL

// Append the boot count and reset flags to the USB serial number (bring-up)
#if !defined(BOARD_SERIAL_DIAGNOSTICS)
#define BOARD_SERIAL_DIAGNOSTICS 0
#endif
