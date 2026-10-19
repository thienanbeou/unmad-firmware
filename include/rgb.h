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

#if defined(RGB_NUM_LEDS)

//--------------------------------------------------------------------+
// Lighting (this fork). Effects are QMK RGB Matrix ports; the board provides
// the LED driver and the LED layout in its config.h:
//   RGB_LED_POINTS  {x, y} per LED in chain order (QMK g_led_config.point)
//   RGB_LED_FLAGS   QMK LED flags per LED (1 = modifier)
//   RGB_KEY_LEDS    LED index for each key index
//   RGB_MATRIX_LEDS [rows][cols] LED index or 255 (QMK g_led_config.matrix_co)
//   RGB_MAX_BRIGHTNESS, RGB_DEFAULT_{EFFECT,HUE,SAT,VAL,SPEED}
//--------------------------------------------------------------------+

/**
 * @brief Run the lighting: key hit tracking and one frame every RGB_FRAME_MS
 *
 * Call it from the main loop as often as possible.
 *
 * @return None
 */
void rgb_task(void);

/**
 * @brief Handle a lighting raw HID command (COMMAND_*_RGB*)
 *
 * @param in Request (byte 0 = command id)
 * @param out Response (byte 0 is set by the caller)
 *
 * @return Whether the command succeeded
 */
bool rgb_command(const uint8_t *in, uint8_t *out);

//--------------------------------------------------------------------+
// LED driver (hardware)
//--------------------------------------------------------------------+

/**
 * @brief Start the LED driver with every LED off
 *
 * @return None
 */
void rgb_driver_init(void);

/**
 * @brief Set one LED. It shows once rgb_driver_flush sends it.
 *
 * @param led LED index in chain order
 * @param r Red
 * @param g Green
 * @param b Blue
 *
 * @return None
 */
void rgb_driver_set(uint8_t led, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Send the LEDs once if any changed and the wire is free. Call often.
 *
 * @return None
 */
void rgb_driver_flush(void);

#endif
