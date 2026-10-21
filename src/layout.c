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

#include "layout.h"

#include "advanced_keys.h"
#include "deferred_actions.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "hid.h"
#include "keycodes.h"
#include "lib/bitmap.h"
#include "matrix.h"
#include "rgb.h"
#include "xinput.h"

// Layer mask. Each bit represents whether a layer is active or not.
static uint16_t layer_mask;
static uint8_t default_layer;

/**
 * @brief Get the current layer
 *
 * The current layer is the highest layer that is currently active in the layer
 * mask. If no layers are active, the default layer is returned.
 *
 * @return Current layer
 */
__attribute__((always_inline)) static inline uint8_t
layout_get_current_layer(void) {
  return layer_mask ? 31 - __builtin_clz(layer_mask) : default_layer;
}

__attribute__((always_inline)) static inline void
layout_layer_on(uint8_t layer) {
  layer_mask |= (1 << layer);
}

__attribute__((always_inline)) static inline void
layout_layer_off(uint8_t layer) {
  layer_mask &= ~(1 << layer);
}

/**
 * @brief Lock the current layer
 *
 * This function sets the default layer to the current layer if it is not
 * already set. Otherwise, it sets the default layer to 0.
 *
 * @return None
 */
__attribute__((always_inline)) static inline void layout_layer_lock(void) {
  const uint8_t current_layer = layout_get_current_layer();
  default_layer = current_layer == default_layer ? 0 : current_layer;
}

uint8_t layout_current_layer(void) { return layout_get_current_layer(); }

// QMK Tri Layer (this fork): Lower and Upper turn on these layers, both
// together also the adjust layer
#define TRI_LAYER_LOWER 1
#define TRI_LAYER_UPPER 2
#define TRI_LAYER_ADJUST 3

static void layout_update_tri_layer(void) {
  const uint16_t both = (1 << TRI_LAYER_LOWER) | (1 << TRI_LAYER_UPPER);
  if ((layer_mask & both) == both)
    layout_layer_on(TRI_LAYER_ADJUST);
  else
    layout_layer_off(TRI_LAYER_ADJUST);
}

// One Shot keys (this fork, QMK OSM / OSL). A tapped OSM / OSL key arms its
// modifier or layer for the next key: the layer turns off once that key is
// pressed, the modifier once it is released. A key pressed while an OSM / OSL
// key is still held makes them plain held keys instead.
static uint8_t oneshot_mods;   // bit m = OSM(m) armed
static uint8_t oneshot_layers; // bit n = OSL(n) armed
static uint8_t oneshot_held;   // OSM / OSL keys held down
static bool oneshot_interrupted;
// Key whose release ends the armed modifiers (NUM_KEYS: none)
static uint8_t oneshot_trigger_key = NUM_KEYS;

// Only send reports if they changed
static bool should_send_reports;

/**
 * @brief Let a key press use the armed One Shot modifiers and layers
 *
 * @param key Key index
 *
 * @return None
 */
static void layout_oneshot_consume(uint8_t key) {
  if (oneshot_held) {
    oneshot_interrupted = true;
    return;
  }
  layer_mask &= ~oneshot_layers;
  oneshot_layers = 0;
  if (oneshot_mods && oneshot_trigger_key == NUM_KEYS)
    oneshot_trigger_key = key;
}

static void layout_oneshot_release_mods(void) {
  for (uint32_t m = 0; m < 8; m++)
    if ((oneshot_mods >> m) & 1)
      hid_keycode_remove(KC_LEFT_CTRL + m);
  oneshot_mods = 0;
  oneshot_trigger_key = NUM_KEYS;
  should_send_reports = true;
}

/**
 * @brief Get the keycode of a key
 *
 * This function returns the keycode of a key in the current layer. If the key
 * is transparent, the function will search for the highest active layer with a
 * non-transparent keycode.
 *
 * @param current_layer Current layer
 * @param key Key index
 *
 * @return Keycode
 */
static uint8_t layout_get_keycode(uint8_t current_layer, uint8_t key) {
  // Find the first active layer with a non-transparent keycode
  for (uint32_t i = (uint32_t)current_layer + 1; i-- > 0;) {
    if (((layer_mask >> i) & 1) == 0)
      // Layer is not active
      continue;

    const uint8_t keycode = CURRENT_PROFILE.keymap[i][key];
    if (keycode != KC_TRANSPARENT)
      return keycode;
  }

  // No keycode found, use the default keycode
  return CURRENT_PROFILE.keymap[default_layer][key];
}

uint8_t layout_current_keycode(uint8_t key) {
  return layout_get_keycode(layout_get_current_layer(), key);
}

/**
 * @brief Swap W/A/S/D with the arrow keys when the stock WASD switch is on
 *
 * @param keycode Keycode
 *
 * @return Keycode to use
 */
static uint8_t layout_wasd_swap(uint8_t keycode) {
  static const uint8_t pairs[][2] = {
      {KC_W, KC_UP}, {KC_A, KC_LEFT}, {KC_S, KC_DOWN}, {KC_D, KC_RIGHT}};

  if (!eeconfig->options.wasd_swap_enabled)
    return keycode;
  for (uint32_t i = 0; i < M_ARRAY_SIZE(pairs); i++) {
    if (keycode == pairs[i][0])
      return pairs[i][1];
    if (keycode == pairs[i][1])
      return pairs[i][0];
  }
  return keycode;
}

/**
 * @brief Toggle one of the stock switches stored in the options
 *
 * @param keycode `SP_WASD_SWAP`, `SP_GUI_LOCK`, `SP_NKRO_TOGGLE` or
 * `SP_MAC_MODE`
 *
 * @return None
 */
static void layout_toggle_option(uint8_t keycode) {
  eeconfig_options_t options = eeconfig->options;

  if (keycode == SP_WASD_SWAP)
    options.wasd_swap_enabled ^= 1;
  else if (keycode == SP_GUI_LOCK)
    options.gui_lock_enabled ^= 1;
  else if (keycode == SP_MAC_MODE)
    options.mac_mode_enabled ^= 1;
  else
    options.nkro_disabled ^= 1;
  EECONFIG_WRITE(options, &options);
}

// Whether the key is disabled by `SP_KEY_LOCK`
static bitmap_t key_disabled[] = MAKE_BITMAP(NUM_KEYS);

// Track whether the key is currently pressed. Used to detect key events.
static bitmap_t key_press_states[] = MAKE_BITMAP(NUM_KEYS);
// Store the keycodes of the currently pressed keys. Layer/profile may change so
// we need to remember the keycodes we pressed to release them correctly.
static uint8_t active_keycodes[NUM_KEYS];

// Store the indices of the advanced keys bind to each key. If no advanced key
// is bind to a key, the index is 0. Otherwise, the index is added by 1.
static uint8_t advanced_key_indices[NUM_LAYERS][NUM_KEYS];
// Same as `active_keycodes` but for advanced keys
static uint8_t active_advanced_keys[NUM_KEYS];
// Key presses held back while a Tap Dance decides or sends its output, so that
// they reach the host after it. Replayed once the Tap Dance is done.
// ponytail: keys held back together replay in key index order, not press order
static bitmap_t deferred_presses[] = MAKE_BITMAP(NUM_KEYS);

void layout_init(void) { layout_load_advanced_keys(); }

void layout_load_advanced_keys(void) {
  advanced_key_init();

  memset(advanced_key_indices, 0, sizeof(advanced_key_indices));
  for (uint32_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
    const advanced_key_t *ak = &CURRENT_PROFILE.advanced_keys[i];

    if (ak->type == AK_TYPE_NONE || ak->layer >= NUM_LAYERS ||
        ak->key >= NUM_KEYS)
      continue;

    advanced_key_indices[ak->layer][ak->key] = i + 1;
    if (ak->type == AK_TYPE_NULL_BIND && ak->null_bind.secondary_key < NUM_KEYS)
      // Null Bind advanced keys also have a secondary key
      advanced_key_indices[ak->layer][ak->null_bind.secondary_key] = i + 1;
  }
}

void layout_task(void) {
  static advanced_key_event_t ak_event = {0};
  static uint32_t last_ak_tick = 0;

  const uint8_t current_layer = layout_get_current_layer();
  bool has_non_tap_hold_press = false;
  bool tap_dance_busy = advanced_key_tap_dance_busy();

  for (uint32_t i = 0; i < NUM_KEYS; i++) {
    const key_state_t *k = &key_matrix[i];
    const bool last_key_press = bitmap_get(key_press_states, i);
    bool is_pressed = k->is_pressed;

    if (bitmap_get(deferred_presses, i)) {
      if (tap_dance_busy)
        // Still waiting for the Tap Dance
        continue;
      bitmap_set(deferred_presses, i, false);
      // Replay the press now. If the key is already up, its release follows
      // in the next scan.
      is_pressed = true;
    }

    if ((current_layer == 0) & eeconfig->options.xinput_enabled) {
      // XInput key only applies to layer 0. We process it first since the
      // subsequent key processing may be skipped due to the gamepad options.
      if (CURRENT_PROFILE.gamepad_buttons[i] != GP_BUTTON_NONE) {
        xinput_process(i);

        if (CURRENT_PROFILE.gamepad_options.gamepad_override)
          // If the key is mapped to a gamepad button, and the gamepad override
          // is enabled, we skip the key processing.
          continue;
      }

      if (!CURRENT_PROFILE.gamepad_options.keyboard_enabled)
        // If the keyboard is disabled for this profile, we skip the key
        // processing.
        continue;
    }

    if ((current_layer == 0) & bitmap_get(key_disabled, i))
      // Only keys in layer 0 can be disabled.
      continue;

    if (is_pressed & !last_key_press) {
      // Key press event
      const uint8_t keycode =
          layout_wasd_swap(layout_get_keycode(current_layer, i));
      const uint8_t ak_index = advanced_key_indices[current_layer][i];
      const bool is_tap_dance =
          ak_index && CURRENT_PROFILE.advanced_keys[ak_index - 1].type ==
                          AK_TYPE_TAP_DANCE;

      if (tap_dance_busy & !is_tap_dance) {
        // Another key interrupts the Tap Dance, and waits for its output
        advanced_key_tap_dance_interrupt();
        bitmap_set(deferred_presses, i, true);
        continue;
      }
      tap_dance_busy |= is_tap_dance;

      if (ak_index) {
        active_advanced_keys[i] = ak_index;
        ak_event = (advanced_key_event_t){
            .type = AK_EVENT_TYPE_PRESS,
            .key = i,
            .keycode = keycode,
            .ak_index = ak_index - 1,
        };
        advanced_key_process(&ak_event);
        has_non_tap_hold_press |=
            (CURRENT_PROFILE.advanced_keys[ak_index - 1].type !=
             AK_TYPE_TAP_HOLD);
      } else {
        active_keycodes[i] = keycode;
        layout_register(i, keycode);
        has_non_tap_hold_press |= (keycode != KC_NO);
      }
      if (ak_index || !(SP_OSL_MIN <= keycode && keycode <= SP_OSM_MAX))
        layout_oneshot_consume(i);
    } else if (!is_pressed & last_key_press) {
      // Key release event
      const uint8_t keycode = active_keycodes[i];
      const uint8_t ak_index = active_advanced_keys[i];

      if (ak_index) {
        active_advanced_keys[i] = 0;
        ak_event = (advanced_key_event_t){
            .type = AK_EVENT_TYPE_RELEASE,
            .key = i,
            .keycode = keycode,
            .ak_index = ak_index - 1,
        };
        advanced_key_process(&ak_event);
      } else {
        active_keycodes[i] = KC_NO;
        layout_unregister(i, keycode);
      }
      if (i == oneshot_trigger_key)
        layout_oneshot_release_mods();
    } else if (is_pressed) {
      // Key hold event
      const uint8_t keycode = active_keycodes[i];
      const uint8_t ak_index = active_advanced_keys[i];

      if (ak_index) {
        ak_event = (advanced_key_event_t){
            .type = AK_EVENT_TYPE_HOLD,
            .key = i,
            .keycode = keycode,
            .ak_index = ak_index - 1,
        };
        advanced_key_process(&ak_event);
      }
    }

    // Finally, update the key state
    bitmap_set(key_press_states, i, is_pressed);
  }

  if (has_non_tap_hold_press || timer_elapsed(last_ak_tick) > 0) {
    // We only need to tick the advanced keys every 1ms, or when there is a
    // non-Tap-Hold key press event since these are the only cases that
    // the advanced keys might perform an action.
    advanced_key_tick(has_non_tap_hold_press);
    last_ak_tick = timer_read();
  }

  if (should_send_reports) {
    hid_send_reports();
    should_send_reports = false;
  }

  // Process deferred actions for the next matrix scan
  deferred_action_process();
}

/**
 * @brief Set the current profile
 *
 * This function also refreshes the advanced keys, and saves the last
 * non-default profile for profile swapping.
 *
 * @param profile Profile index
 *
 * @return true if successful, false otherwise
 */
static bool layout_set_profile(uint8_t profile) {
  if (profile >= eeconfig->active_profiles)
    return false;

  advanced_key_clear();
  bool status = EECONFIG_WRITE(current_profile, &profile);
  if (status && profile != 0)
    status = EECONFIG_WRITE(last_non_default_profile, &profile);
  layout_load_advanced_keys();

  return status;
}

bool layout_set_in_use(uint8_t profiles, uint8_t layers) {
  if (profiles < 1 || profiles > NUM_PROFILES || layers < 1 ||
      layers > NUM_LAYERS)
    return false;

  // Profiles and layers taken out of use are cleared, so adding them back
  // starts fresh: profiles to their defaults, layers to transparent (in every
  // profile) without their advanced keys
  bool status = true;
  for (uint8_t p = profiles; p < eeconfig->active_profiles; p++)
    status &= eeconfig_reset_profile(p);
  if (layers < eeconfig->active_layers) {
    static const advanced_key_t no_key = {0};
    uint8_t transparent[NUM_KEYS];
    memset(transparent, KC_TRANSPARENT, sizeof(transparent));
    advanced_key_clear();
    for (uint8_t p = 0; p < profiles; p++) {
      for (uint8_t l = layers; l < eeconfig->active_layers; l++)
        status &= EECONFIG_WRITE(profiles[p].keymap[l], &transparent);
      for (uint8_t i = 0; i < NUM_ADVANCED_KEYS; i++)
        if (eeconfig->profiles[p].advanced_keys[i].type != AK_TYPE_NONE &&
            eeconfig->profiles[p].advanced_keys[i].layer >= layers)
          status &= EECONFIG_WRITE(profiles[p].advanced_keys[i], &no_key);
    }
    layout_load_advanced_keys();
    // Nothing stays on a removed layer
    layer_mask &= (1 << layers) - 1;
    if (default_layer >= layers)
      default_layer = 0;
  }

  const uint8_t in_use[2] = {profiles, layers};
  status &= EECONFIG_WRITE_N(active_profiles, in_use, sizeof(in_use));
  if (eeconfig->current_profile >= profiles)
    status &= layout_set_profile(0);
  return status;
}

void layout_register(uint8_t key, uint8_t keycode) {
  if (keycode == KC_NO)
    return;

  switch (keycode) {
  case HID_KEYCODE_RANGE:
    hid_keycode_add(keycode);
    should_send_reports = true;
    break;

  case MOMENTARY_LAYER_RANGE:
    layout_layer_on(MO_GET_LAYER(keycode));
    break;

  case PROFILE_RANGE:
    layout_set_profile(PF_GET_PROFILE(keycode));
    break;

  case TO_LAYER_RANGE:
    if (LAYER_KEY_GET_LAYER(keycode) < NUM_LAYERS) {
      // Only that layer stays on (over the default layer)
      const uint8_t layer = LAYER_KEY_GET_LAYER(keycode);
      layer_mask = layer == default_layer ? 0 : 1 << layer;
      oneshot_layers = 0;
    }
    break;

  case DEFAULT_LAYER_RANGE:
    if (LAYER_KEY_GET_LAYER(keycode) < NUM_LAYERS)
      default_layer = LAYER_KEY_GET_LAYER(keycode);
    break;

  case ONE_SHOT_LAYER_RANGE:
    oneshot_held++;
    oneshot_interrupted = false;
    if (LAYER_KEY_GET_LAYER(keycode) < NUM_LAYERS) {
      layout_layer_on(LAYER_KEY_GET_LAYER(keycode));
      oneshot_layers |= 1 << LAYER_KEY_GET_LAYER(keycode);
    }
    break;

  case ONE_SHOT_MOD_RANGE:
    oneshot_held++;
    oneshot_interrupted = false;
    oneshot_mods |= 1 << (keycode & 0x07);
    hid_keycode_add(OSM_GET_MODIFIER(keycode));
    should_send_reports = true;
    break;

  case SP_TRI_LAYER_LOWER:
  case SP_TRI_LAYER_UPPER:
    if (TRI_LAYER_ADJUST < NUM_LAYERS) {
      layout_layer_on(keycode == SP_TRI_LAYER_LOWER ? TRI_LAYER_LOWER
                                                    : TRI_LAYER_UPPER);
      layout_update_tri_layer();
    }
    break;

  case SP_KEY_LOCK:
    bitmap_toggle(key_disabled, key);
    break;

  case SP_LAYER_LOCK:
    layout_layer_lock();
    break;

  case SP_PROFILE_SWAP:
    layout_set_profile(
        eeconfig->current_profile ? 0 : eeconfig->last_non_default_profile);
    break;

  case SP_PROFILE_NEXT:
    layout_set_profile((eeconfig->current_profile + 1) %
                       eeconfig->active_profiles);
    break;

  case SP_BOOT:
    board_enter_bootloader();
    break;

  case SP_WASD_SWAP:
  case SP_GUI_LOCK:
  case SP_NKRO_TOGGLE:
  case SP_MAC_MODE:
    layout_toggle_option(keycode);
    break;

  case SP_GRAVE_ESCAPE:
    hid_grave_escape_press();
    should_send_reports = true;
    break;

  case SP_REPEAT_KEY:
    hid_repeat_press();
    should_send_reports = true;
    break;

  case SP_CAPS_WORD:
    hid_caps_word_toggle();
    should_send_reports = true;
    break;

#if defined(RGB_NUM_LEDS)
  case SP_RGB_TOGGLE ... SP_RGB_SPEED_DOWN:
    rgb_keycode(keycode, hid_shift_held());
    break;
#endif

  default:
    break;
  }
}

void layout_unregister(uint8_t key, uint8_t keycode) {
  if (keycode == KC_NO)
    return;

  switch (keycode) {
  case HID_KEYCODE_RANGE:
    hid_keycode_remove(keycode);
    should_send_reports = true;
    break;

  case SP_GRAVE_ESCAPE:
    hid_grave_escape_release();
    should_send_reports = true;
    break;

  case SP_REPEAT_KEY:
    hid_repeat_release();
    should_send_reports = true;
    break;

  case MOMENTARY_LAYER_RANGE:
    layout_layer_off(MO_GET_LAYER(keycode));
    break;

  case ONE_SHOT_LAYER_RANGE:
    if (oneshot_held)
      oneshot_held--;
    if (oneshot_interrupted) {
      // Used like MO: off with the key
      layout_layer_off(LAYER_KEY_GET_LAYER(keycode));
      oneshot_layers &= ~(1 << LAYER_KEY_GET_LAYER(keycode));
    }
    break;

  case ONE_SHOT_MOD_RANGE:
    if (oneshot_held)
      oneshot_held--;
    if (oneshot_interrupted) {
      // Used like the plain modifier: released with the key
      hid_keycode_remove(OSM_GET_MODIFIER(keycode));
      oneshot_mods &= ~(1 << (keycode & 0x07));
      should_send_reports = true;
    }
    break;

  case SP_TRI_LAYER_LOWER:
  case SP_TRI_LAYER_UPPER:
    if (TRI_LAYER_ADJUST < NUM_LAYERS) {
      layout_layer_off(keycode == SP_TRI_LAYER_LOWER ? TRI_LAYER_LOWER
                                                     : TRI_LAYER_UPPER);
      layout_update_tri_layer();
    }
    break;

  default:
    break;
  }
}
