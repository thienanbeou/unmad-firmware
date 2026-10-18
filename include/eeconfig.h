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
#include "wear_leveling.h"

//--------------------------------------------------------------------+
// Keyboard Persistent Configuration
//--------------------------------------------------------------------+

// Magic number to identify the start of the configuration
#define EECONFIG_MAGIC_START 0x0A42494C
// Magic number to identify the end of the configuration
#define EECONFIG_MAGIC_END 0x0A4B4D48

// Keyboard calibration configuration
typedef struct __attribute__((packed)) {
  // Initial rest value of the key matrix. If the value is smaller than the
  // actual rest value, the key will have a dead zone at the beginning of the
  // keystroke. If the value is larger than the actual rest value, a longer
  // calibration process may be required.
  uint16_t initial_rest_value;
  // Minimum change in ADC values for the key to be considered bottom-out. If
  // the value is larger than the actual bottom-out threshold, the key will have
  // a dead zone at the end of the keystroke. If the value is smaller than the
  // actual bottom-out threshold, the distance calculation may be inaccurate
  // until the first bottom-out event.
  uint16_t initial_bottom_out_threshold;
} eeconfig_calibration_t;

// Keyboard options configuration
typedef union __attribute__((packed)) {
  struct __attribute__((packed)) {
    // Whether the XInput interface is enabled
    bool xinput_enabled : 1;
    bool _unused0 : 1;
    // Whether 8kHz polling rate is enabled. Only applicable if USB HS is
    // enabled. If disabled, the 1kHz polling rate is used instead.
    bool high_polling_rate_enabled : 1;
    // Stock MAD60 HE switches (this fork): W/A/S/D and the arrow keys swapped,
    // the GUI (Windows) keys blocked, and 6KRO only (no NKRO bitmap)
    bool wasd_swap_enabled : 1;
    bool gui_lock_enabled : 1;
    bool nkro_disabled : 1;
    // Mac mode: Alt and GUI swapped on both sides (the stock WIN/MAC switch)
    bool mac_mode_enabled : 1;
    // Caps Lock indicator on the RGB off (stored inverted: on by default, and
    // older configurations, whose reserved bits are 0, get it without a
    // migration)
    bool caps_indicator_disabled : 1;
    // Reserved bits for future use
    uint16_t reserved : 8;
  };
  uint16_t raw;
} eeconfig_options_t;

_Static_assert(sizeof(eeconfig_options_t) == sizeof(uint16_t),
               "Invalid eeconfig_options_t size");

#if defined(RGB_NUM_LEDS)
// Lighting effects, ported from QMK RGB Matrix. The order follows the stock
// MAD60 HE effect list (vendor number in brackets), then the rest of QMK's
// effects (v1.1); never reorder, the number is stored.
typedef enum {
  RGB_EFFECT_OFF = 0,               // [0] Close
  RGB_EFFECT_CUSTOM,                // [1] Customization (per-key colours)
  RGB_EFFECT_SOLID_COLOR,           // [2] Monochromatic Constant Brightness
  RGB_EFFECT_ALPHAS_MODS,           // [3] Focus
  RGB_EFFECT_GRADIENT_UP_DOWN,      // [4] Rainbow static up and down
  RGB_EFFECT_GRADIENT_LEFT_RIGHT,   // [5] Rainbow static left and right
  RGB_EFFECT_BREATHING,             // [6] Monochromatic Breathing
  RGB_EFFECT_BAND_SAT,              // [7] Ribbon fluttering 1
  RGB_EFFECT_BAND_VAL,              // [8] Ribbon fluttering 2
  RGB_EFFECT_BAND_PINWHEEL_SAT,     // [9] Rotating Belt 1
  RGB_EFFECT_BAND_SPIRAL_SAT,       // [11] Rotating Belt 2
  RGB_EFFECT_CYCLE_ALL,             // [13] Spectrum Cycle
  RGB_EFFECT_CYCLE_PINWHEEL,        // [19] Colorful Swirls 1
  RGB_EFFECT_RAINBOW_PINWHEELS,     // [23] Colorful Swirls 2
  RGB_EFFECT_JELLYBEAN_RAINDROPS,   // [25] Christmas
  RGB_EFFECT_HUE_BREATHING,         // [26] Spectrum Breathing
  RGB_EFFECT_HUE_WAVE,              // [28] Spectrum Floating
  RGB_EFFECT_DIGITAL_RAIN,          // [30] Digital Rain
  RGB_EFFECT_SOLID_REACTIVE_SIMPLE, // [31] Monochromatic Lighting Up
  RGB_EFFECT_SOLID_REACTIVE_MULTICROSS, // [36] Crucifix
  RGB_EFFECT_SPLASH,                // [39] Fingertip Rainbow
  // Not in the stock list (QMK effect names)
  RGB_EFFECT_BAND_PINWHEEL_VAL,
  RGB_EFFECT_BAND_SPIRAL_VAL,
  RGB_EFFECT_CYCLE_LEFT_RIGHT,
  RGB_EFFECT_CYCLE_UP_DOWN,
  RGB_EFFECT_RAINBOW_MOVING_CHEVRON,
  RGB_EFFECT_CYCLE_OUT_IN,
  RGB_EFFECT_CYCLE_OUT_IN_DUAL,
  RGB_EFFECT_CYCLE_SPIRAL,
  RGB_EFFECT_DUAL_BEACON,
  RGB_EFFECT_RAINBOW_BEACON,
  RGB_EFFECT_RAINDROPS,
  RGB_EFFECT_HUE_PENDULUM,
  RGB_EFFECT_PIXEL_FRACTAL,
  RGB_EFFECT_PIXEL_FLOW,
  RGB_EFFECT_PIXEL_RAIN,
  RGB_EFFECT_TYPING_HEATMAP,
  RGB_EFFECT_SOLID_REACTIVE,
  RGB_EFFECT_SOLID_REACTIVE_WIDE,
  RGB_EFFECT_SOLID_REACTIVE_MULTIWIDE,
  RGB_EFFECT_SOLID_REACTIVE_CROSS,
  RGB_EFFECT_SOLID_REACTIVE_NEXUS,
  RGB_EFFECT_SOLID_REACTIVE_MULTINEXUS,
  RGB_EFFECT_MULTISPLASH,
  RGB_EFFECT_SOLID_SPLASH,
  RGB_EFFECT_SOLID_MULTISPLASH,
  RGB_EFFECT_STARLIGHT,
  RGB_EFFECT_STARLIGHT_SMOOTH,
  RGB_EFFECT_STARLIGHT_DUAL_HUE,
  RGB_EFFECT_STARLIGHT_DUAL_SAT,
  RGB_EFFECT_RIVERFLOW,
  RGB_EFFECT_FLOWER_BLOOMING,
  RGB_EFFECT_COUNT,
} rgb_effect_t;

// Lighting configuration (this fork, see src/rgb.c)
typedef struct __attribute__((packed)) {
  // Effect (`rgb_effect_t`)
  uint8_t effect;
  // Base colour in QMK HSV (0-255 each); `val` is the brightness
  uint8_t hue;
  uint8_t sat;
  uint8_t val;
  // Animation speed (0-255)
  uint8_t speed;
  // Per-key colours (R, G, B) for the custom effect, by key index
  uint8_t key_colors[NUM_KEYS][3];
  // Colour range of the rainbow effects (added in v1.7): 255 sweeps the whole
  // colour wheel as in QMK, less keeps them in a band around the base hue
  uint8_t hue_range;
} eeconfig_rgb_t;
#endif

// Keyboard profile configuration
typedef struct __attribute__((packed)) {
  uint8_t keymap[NUM_LAYERS][NUM_KEYS];
  actuation_t actuation_map[NUM_KEYS];
  advanced_key_t advanced_keys[NUM_ADVANCED_KEYS];
  macro_node_t macros[NUM_MACRO_NODES];
  uint8_t gamepad_buttons[NUM_KEYS];
  gamepad_options_t gamepad_options;
  uint8_t tick_rate;
} eeconfig_profile_t;

// Persistent configuration version. The size of the configuration must be
// non-decreasing, so that the migration can assume that the new version is at
// least as large as the previous version.
#define EECONFIG_VERSION 0x0107

// Keyboard configuration
// Whenever there is a change in the configuration, `EECONFIG_VERSION` must be
// bumped. Make sure to update `eeconfig_reset()`, and add a migration function
// in `migration.c`.
typedef struct __attribute__((packed)) {
  // Global configurations
  // Magic number to identify the start of the configuration
  uint32_t magic_start;
  // Version of the configuration
  uint16_t version;

  // Calibration configuration
  eeconfig_calibration_t calibration;
  // Saved bottom-out threshold
  uint16_t bottom_out_threshold[NUM_KEYS];
  // Options configuration
  eeconfig_options_t options;

  // Current profile index
  uint8_t current_profile;
  // Last non-default profile index, used for profile swapping
  uint8_t last_non_default_profile;
#if defined(RGB_NUM_LEDS)
  // Lighting configuration (added in v1.6 of this fork, hue_range in v1.7)
  eeconfig_rgb_t rgb;
#endif
  // End of global configurations

  // Profiles
  eeconfig_profile_t profiles[NUM_PROFILES];

  // Magic number to identify the end of the configuration
  uint32_t magic_end;
} eeconfig_t;

_Static_assert(
    sizeof(eeconfig_t) <= WL_VIRTUAL_SIZE,
    "Keyboard configuration size must be at most the virtual storage size.");

extern const eeconfig_t *eeconfig;

#define CURRENT_PROFILE (eeconfig->profiles[eeconfig->current_profile])

//--------------------------------------------------------------------+
// Default Keyboard Configuration
//--------------------------------------------------------------------+

#if !defined(DEFAULT_CALIBRATION)
#error "DEFAULT_CALIBRATION is not defined"
#endif

#if !defined(DEFAULT_OPTIONS)
// Default global options
#define DEFAULT_OPTIONS                                                        \
  {                                                                            \
      .xinput_enabled = false,                                                 \
      .high_polling_rate_enabled = true,                                       \
  }
#endif

#if !defined(DEFAULT_KEYMAPS)
#error "DEFAULT_KEYMAPS is not defined"
#endif

#if defined(RGB_NUM_LEDS) && !defined(DEFAULT_RGB)
// Default lighting (board config.h sets the RGB_DEFAULT_* values)
#define DEFAULT_RGB                                                            \
  {                                                                            \
      .effect = RGB_DEFAULT_EFFECT,                                            \
      .hue = RGB_DEFAULT_HUE,                                                  \
      .sat = RGB_DEFAULT_SAT,                                                  \
      .val = RGB_DEFAULT_VAL,                                                  \
      .speed = RGB_DEFAULT_SPEED,                                              \
      .hue_range = 255,                                                        \
  }
#endif

#if !defined(DEFAULT_ACTUATION_POINT)
// Default actuation point
#define DEFAULT_ACTUATION_POINT 128
#endif

#if !defined(DEFAULT_GAMEPAD_OPTIONS)
// Default gamepad options
#define DEFAULT_GAMEPAD_OPTIONS                                                \
  {                                                                            \
      .analog_curve = {{4, 20}, {85, 95}, {165, 170}, {255, 255}},             \
      .keyboard_enabled = true,                                                \
      .snappy_joystick = true,                                                 \
  }
#endif

#if !defined(DEFAULT_TICK_RATE)
// Default tick rate
#define DEFAULT_TICK_RATE 30
#endif

//--------------------------------------------------------------------+
// Persistent Configuration API
//--------------------------------------------------------------------+

/**
 * @brief Initialize the persistent configuration module
 *
 * @return None
 */
void eeconfig_init(void);

/**
 * @brief Reset the persistent configuration to default values
 *
 * @return true if successful, false otherwise
 */
bool eeconfig_reset(void);

/**
 * @brief Reset a specific profile to default values
 *
 * @param profile Profile index
 *
 * @return true if successful, false otherwise
 */
bool eeconfig_reset_profile(uint8_t profile);

/**
 * @brief Write a value to a field in the persistent configuration
 *
 * @param field Field to write to
 * @param value Value to write
 *
 * @return true if successful, false otherwise
 */
#define EECONFIG_WRITE(field, value)                                           \
  wear_leveling_write(offsetof(eeconfig_t, field), value,                      \
                      sizeof(((eeconfig_t *)0)->field))

/**
 * @brief Write a value to a field in the persistent configuration
 *
 * @param field Field to write to
 * @param value Value to write
 * @param len Length of the value in bytes
 *
 * @return true if successful, false otherwise
 */
#define EECONFIG_WRITE_N(field, value, len)                                    \
  wear_leveling_write(offsetof(eeconfig_t, field), value, len)
