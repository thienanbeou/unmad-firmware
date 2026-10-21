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

#include "eeconfig.h"

#include "keycodes.h"
#include "migration.h"

const eeconfig_t *eeconfig;

// Default configuration values
static eeconfig_options_t default_options = DEFAULT_OPTIONS;
static eeconfig_calibration_t default_calibration = DEFAULT_CALIBRATION;
#if defined(RGB_NUM_LEDS)
static const eeconfig_rgb_t default_rgb = DEFAULT_RGB;
#endif
#if defined(DEFAULT_KEYMAP)
// The same keymap for every profile
static const uint8_t default_keymap[NUM_LAYERS][NUM_KEYS] = DEFAULT_KEYMAP;
#else
static const uint8_t default_keymaps[NUM_PROFILES][NUM_LAYERS][NUM_KEYS] =
    DEFAULT_KEYMAPS;
#endif
static const macro_node_t default_macro = {
    .keycode = KC_NO,
    .action = MACRO_ACTION_NONE,
    .delay = 0,
    .next = MACRO_NODE_NONE,
};
static eeconfig_profile_t default_profile = {
    .gamepad_options = DEFAULT_GAMEPAD_OPTIONS,
    .tick_rate = DEFAULT_TICK_RATE,
};

// Optional default macros and advanced keys from keyboard.json (scripts/make.py)
#if defined(DEFAULT_MACRO_NODES)
static const macro_node_t default_macro_nodes[] = DEFAULT_MACRO_NODES;
_Static_assert(M_ARRAY_SIZE(default_macro_nodes) <= NUM_MACRO_NODES,
               "Too many default macro nodes");
#endif
#if defined(DEFAULT_ADVANCED_KEYS)
static const advanced_key_t default_advanced_keys[] = DEFAULT_ADVANCED_KEYS;
_Static_assert(M_ARRAY_SIZE(default_advanced_keys) <= NUM_ADVANCED_KEYS,
               "Too many default advanced keys");
#endif

const eeconfig_profile_t *eeconfig_default_profile(uint8_t profile) {
#if defined(DEFAULT_KEYMAP)
  (void)profile;
  memcpy(default_profile.keymap, default_keymap, sizeof(default_profile.keymap));
#else
  memcpy(default_profile.keymap, default_keymaps[profile],
         sizeof(default_profile.keymap));
#endif
  for (uint32_t i = 0; i < NUM_MACRO_NODES; i++)
    default_profile.macros[i] = default_macro;
#if defined(DEFAULT_MACRO_NODES)
  memcpy(default_profile.macros, default_macro_nodes,
         sizeof(default_macro_nodes));
#endif
#if defined(DEFAULT_ADVANCED_KEYS)
  memcpy(default_profile.advanced_keys, default_advanced_keys,
         sizeof(default_advanced_keys));
#endif
  return &default_profile;
}

static bool eeconfig_write_default_profile(uint8_t profile) {
  if (profile >= NUM_PROFILES)
    return false;

  return EECONFIG_WRITE(profiles[profile], eeconfig_default_profile(profile));
}

static bool eeconfig_is_latest_version(void) {
  return eeconfig->magic_start == EECONFIG_MAGIC_START &&
         eeconfig->magic_end == EECONFIG_MAGIC_END &&
         eeconfig->version == EECONFIG_VERSION;
}

void eeconfig_init(void) {
  // Update default profile with its default values
  for (uint32_t i = 0; i < NUM_KEYS; i++)
    default_profile.actuation_map[i].actuation_point = DEFAULT_ACTUATION_POINT;
#if defined(DEFAULT_KEY_ACTUATIONS)
  // Per-key actuation and Rapid Trigger from keyboard.json (scripts/make.py)
  static const struct {
    uint8_t key;
    actuation_t actuation;
  } key_actuations[] = DEFAULT_KEY_ACTUATIONS;
  for (uint32_t i = 0; i < M_ARRAY_SIZE(key_actuations); i++)
    default_profile.actuation_map[key_actuations[i].key] =
        key_actuations[i].actuation;
#endif

  eeconfig = (const eeconfig_t *)wl_cache;
  if (eeconfig->magic_start == EECONFIG_MAGIC_START &&
      (eeconfig->version >> 8) != 0x01) {
    // Every version is 0x01xx: a v1.4 install could lose the high byte (see
    // WL_FIRST_WORD_BYTES), which would skip every migration
    const uint16_t version = 0x0100 | (eeconfig->version & 0xFF);
    EECONFIG_WRITE(version, &version);
  }
  if (!eeconfig_is_latest_version() && !migration_try_migrate())
    eeconfig_reset();
}

// Helper macro for writing rvalue
#define EECONFIG_WRITE_LOCAL(field, value)                                     \
  do {                                                                         \
    typeof(((eeconfig_t *)0)->field) _value = value;                           \
    status &= EECONFIG_WRITE(field, &_value);                                  \
  } while (0)

bool eeconfig_reset(void) {
  uint16_t bottom_out_threshold[NUM_KEYS] = {0};

  // We must not perform any action here that requires reading from
  // the configuration as it may be in an invalid state.
  bool status = true;
  EECONFIG_WRITE_LOCAL(magic_start, EECONFIG_MAGIC_START);
  EECONFIG_WRITE_LOCAL(version, EECONFIG_VERSION);
  status &= EECONFIG_WRITE(calibration, &default_calibration);
  status &= EECONFIG_WRITE(bottom_out_threshold, bottom_out_threshold);
  status &= EECONFIG_WRITE(options, &default_options);
  EECONFIG_WRITE_LOCAL(current_profile, 0);
  EECONFIG_WRITE_LOCAL(last_non_default_profile, M_MIN(1, NUM_PROFILES - 1));
#if defined(RGB_NUM_LEDS)
  status &= EECONFIG_WRITE(rgb, &default_rgb);
#endif
  EECONFIG_WRITE_LOCAL(active_profiles, DEFAULT_ACTIVE_PROFILES);
  EECONFIG_WRITE_LOCAL(active_layers, DEFAULT_ACTIVE_LAYERS);
#if defined(RGB_NUM_LEDS)
  EECONFIG_WRITE_LOCAL(rgb_idle_minutes, 0);
#endif
  for (uint32_t i = 0; i < NUM_PROFILES; i++)
    status &= eeconfig_write_default_profile(i);
  EECONFIG_WRITE_LOCAL(magic_end, EECONFIG_MAGIC_END);

  return status;
}

#undef EECONFIG_WRITE_LOCAL

bool eeconfig_reset_profile(uint8_t profile) {
  if (profile >= NUM_PROFILES)
    return false;

  return eeconfig_write_default_profile(profile);
}
