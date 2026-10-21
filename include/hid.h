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

#include "common.h"

//--------------------------------------------------------------------+
// HID API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the HID module
 *
 * @return None
 */
void hid_init(void);

/**
 * @brief Add a keycode to the appropriate HID report
 *
 * @param keycode Keycode to add
 *
 * @return None
 */
void hid_keycode_add(uint8_t keycode);

/**
 * @brief Remove a keycode from the appropriate HID report
 *
 * @param keycode Keycode to remove
 *
 * @return None
 */
void hid_keycode_remove(uint8_t keycode);

/**
 * @brief Press Grave Escape: Escape, or Grave when Shift or GUI is held and
 * Ctrl is not (Ctrl + Shift + Esc stays the Task Manager shortcut)
 *
 * @return None
 */
void hid_grave_escape_press(void);

/**
 * @brief Release what `hid_grave_escape_press` pressed
 *
 * @return None
 */
void hid_grave_escape_release(void);

/**
 * @brief Press Repeat Key: the last key again, with the modifiers held then
 *
 * @return None
 */
void hid_repeat_press(void);

/**
 * @brief Release what `hid_repeat_press` pressed
 *
 * @return None
 */
void hid_repeat_release(void);

/**
 * @brief Keyboard LED state from the host (bit 1 = Caps Lock)
 *
 * @return LED bits
 */
uint8_t hid_get_led_state(void);

/**
 * @brief Turn Caps Word on, or off when it is on
 *
 * @return None
 */
void hid_caps_word_toggle(void);

/**
 * @brief Whether Caps Word is on (it goes off by itself after 5 s idle)
 *
 * @return true if on
 */
bool hid_caps_word_active(void);

/**
 * @brief Whether a Shift is in the report now
 *
 * @return true if a Shift is held
 */
bool hid_shift_held(void);

/**
 * @brief Send all HID reports
 *
 * This function will block until the device is ready to send the reports.
 *
 * @return None
 */
void hid_send_reports(void);
