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
// Layout API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the layout module
 *
 * @return None
 */
void layout_init(void);

/**
 * @brief Load advanced keys
 *
 * This function loads the advanced keys from the current profile. It should be
 * called whenever the profile changes or the advanced keys are updated.
 *
 * @return None
 */
void layout_load_advanced_keys(void);

/**
 * @brief Layout task
 *
 * @return None
 */
void layout_task(void);

/**
 * @brief Current layer (the highest active layer, else the default layer)
 *
 * @return Layer index
 */
uint8_t layout_current_layer(void);

/**
 * @brief Keycode a key has now, through transparent keys to the layers below
 *
 * @param key Key index
 *
 * @return Keycode
 */
uint8_t layout_current_keycode(uint8_t key);

/**
 * @brief Set the profiles and layers in use
 *
 * @param profiles 1 to NUM_PROFILES
 * @param layers 1 to NUM_LAYERS
 *
 * @return true if successful, false otherwise
 */
bool layout_set_in_use(uint8_t profiles, uint8_t layers);

/**
 * @brief Manually register a key press
 *
 * @param key Key code
 * @param keycode Key code
 *
 * @return None
 */
void layout_register(uint8_t key, uint8_t keycode);

/**
 * @brief Manually register a key release
 *
 * @param key Key code
 * @param keycode Key code
 *
 * @return None
 */
void layout_unregister(uint8_t key, uint8_t keycode);
