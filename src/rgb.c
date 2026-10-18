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

#include "rgb.h"

#if defined(RGB_NUM_LEDS)

#include "commands.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "hid.h"
#include "lib/bitmap.h"
#include "matrix.h"

#include <stdlib.h>

// The effects below are ports of QMK RGB Matrix (vial-qmk, GPL-2.0-or-later)
// and FastLED lib8tion (MIT), kept as close to the originals as possible so
// they look like the stock firmware. QMK names are kept in the comments.

#if !defined(RGB_FRAME_MS)
// Milliseconds between frames (QMK flushes every 16 ms; the MAD60 HE wire
// takes ~21 ms per frame)
#define RGB_FRAME_MS 20
#endif

// QMK LED_HITS_TO_REMEMBER
#define RGB_HITS 8
// QMK LED_FLAG_MODIFIER
#define RGB_LED_FLAG_MODIFIER 0x01
// QMK RGB_DIGITAL_RAIN_DROPS
#define RGB_DIGITAL_RAIN_DROPS 24
#define RGB_NO_LED 255

typedef struct {
  uint8_t h, s, v;
} hsv_t;

typedef struct {
  uint8_t r, g, b;
} rgb_t;

static const uint8_t led_points[RGB_NUM_LEDS][2] = RGB_LED_POINTS;
static const uint8_t led_flags[RGB_NUM_LEDS] = RGB_LED_FLAGS;
static const uint8_t key_leds[NUM_KEYS] = RGB_KEY_LEDS;
static const uint8_t matrix_leds[RGB_MATRIX_ROWS][RGB_MATRIX_COLS] =
    RGB_MATRIX_LEDS;

_Static_assert(M_ARRAY_SIZE(key_leds) == NUM_KEYS, "RGB_KEY_LEDS size");

// QMK k_rgb_matrix_center
static const uint8_t center_x = 112, center_y = 32;

// QMK g_rgb_timer: time of the current frame in milliseconds
static uint32_t rgb_timer;

// QMK g_last_hit_tracker
static struct {
  uint8_t count;
  uint8_t x[RGB_HITS];
  uint8_t y[RGB_HITS];
  uint8_t index[RGB_HITS];
  uint16_t tick[RGB_HITS];
} hits;

// QMK g_rgb_frame_buffer (Digital Rain)
static uint8_t frame_buffer[RGB_MATRIX_ROWS][RGB_MATRIX_COLS];

//--------------------------------------------------------------------+
// lib8tion
//--------------------------------------------------------------------+

// QMK builds lib8tion with FASTLED_SCALE8_FIXED=1
static uint8_t scale8(uint8_t i, uint8_t scale) {
  return ((uint16_t)i * (1 + (uint16_t)scale)) >> 8;
}

static uint16_t scale16by8(uint16_t i, uint8_t scale) {
  return (uint16_t)(((uint32_t)i * (1u + scale)) >> 8);
}

static uint8_t qadd8(uint8_t i, uint8_t j) {
  const uint16_t t = i + j;
  return t > 255 ? 255 : t;
}

static uint8_t qsub8(uint8_t i, uint8_t j) { return i > j ? i - j : 0; }

static uint8_t abs8(int8_t i) { return i < 0 ? -i : i; }

static uint8_t sin8(uint8_t theta) {
  static const uint8_t b_m16_interleave[] = {0, 49, 49, 41, 90, 27, 117, 10};
  uint8_t offset = theta;
  if (theta & 0x40)
    offset = 255 - offset;
  offset &= 0x3F;
  uint8_t secoffset = offset & 0x0F;
  if (theta & 0x40)
    secoffset++;
  const uint8_t *p = &b_m16_interleave[(offset >> 4) * 2];
  int8_t y = (int8_t)(((p[1] * secoffset) >> 4) + p[0]);
  if (theta & 0x80)
    y = -y;
  return (uint8_t)(y + 128);
}

static uint8_t cos8(uint8_t theta) { return sin8(theta + 64); }

static uint8_t atan2_8(int16_t dy, int16_t dx) {
  if (dy == 0)
    return dx >= 0 ? 0 : 128;

  const int16_t abs_y = dy > 0 ? dy : -dy;
  int8_t a;
  if (dx >= 0)
    a = 32 - (32 * (dx - abs_y) / (dx + abs_y));
  else
    a = 96 - (32 * (dx + abs_y) / (abs_y - dx));
  return dy < 0 ? (uint8_t)-a : (uint8_t)a;
}

static uint8_t sqrt16(uint16_t x) {
  if (x <= 1)
    return x;

  uint8_t low = 1, hi, mid;
  hi = x > 7904 ? 255 : (x >> 5) + 8;
  do {
    mid = (low + hi) >> 1;
    if ((uint16_t)(mid * mid) > x) {
      hi = mid - 1;
    } else {
      if (mid == 255)
        return 255;
      low = mid + 1;
    }
  } while (hi >= low);
  return low - 1;
}

static uint16_t rand16seed = 1337;

static uint8_t random8(void) {
  rand16seed = (rand16seed * 2053) + 13849;
  return (uint8_t)((uint8_t)(rand16seed & 0xFF) + (uint8_t)(rand16seed >> 8));
}

static uint8_t random8_max(uint8_t lim) { return (random8() * lim) >> 8; }

static uint8_t random8_min_max(uint8_t min, uint8_t lim) {
  return random8_max(lim - min) + min;
}

// QMK hsv_to_rgb (no CIE curve)
static rgb_t hsv_to_rgb(hsv_t hsv) {
  if (hsv.s == 0)
    return (rgb_t){hsv.v, hsv.v, hsv.v};

  const uint16_t h = hsv.h, s = hsv.s, v = hsv.v;
  const uint8_t region = h * 6 / 255;
  const uint8_t remainder = (h * 2 - region * 85) * 3;
  const uint8_t p = (v * (255 - s)) >> 8;
  const uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  const uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

  switch (region) {
  case 6:
  case 0:
    return (rgb_t){v, t, p};
  case 1:
    return (rgb_t){q, v, p};
  case 2:
    return (rgb_t){p, v, t};
  case 3:
    return (rgb_t){p, q, v};
  case 4:
    return (rgb_t){t, p, v};
  default:
    return (rgb_t){v, p, q};
  }
}

static void set_hsv(uint8_t i, hsv_t hsv) {
  const rgb_t rgb = hsv_to_rgb(hsv);
  rgb_driver_set(i, rgb.r, rgb.g, rgb.b);
}

static void set_all(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
    rgb_driver_set(i, r, g, b);
}

//--------------------------------------------------------------------+
// Effects
//--------------------------------------------------------------------+

// Base colour, speed and colour range of the current frame (QMK
// rgb_matrix_config plus this fork's `hue_range`)
static hsv_t base;
static uint8_t speed;
static uint8_t hue_range;

// Hue for the effects that sweep the colour wheel. `offset` is the effect's own
// hue (QMK's, 0-255 = the whole wheel). A colour range of 255 keeps QMK's look
// (turned by the base hue); a smaller range folds the sweep into a band that
// wide centred on the base hue (the fold keeps the band free of the seam where
// the wheel wraps around).
static uint8_t rainbow_hue(uint8_t offset) {
  if (hue_range == 255)
    return base.h + offset;
  const uint8_t fold = offset < 128 ? offset * 2 : (255 - offset) * 2;
  return base.h + scale8(fold, hue_range) - hue_range / 2;
}

// QMK effect_runner_i
static uint8_t runner_i_time(void) {
  return scale16by8(rgb_timer, qadd8(speed / 4, 1));
}

// QMK effect_runner_dx_dy / effect_runner_dx_dy_dist
static uint8_t runner_dx_dy_time(void) {
  return scale16by8(rgb_timer, speed / 2);
}

static void effect_custom(void) {
  for (uint8_t k = 0; k < NUM_KEYS; k++) {
    const uint8_t *c = eeconfig->rgb.key_colors[k];
    if (key_leds[k] != RGB_NO_LED)
      rgb_driver_set(key_leds[k], scale8(c[0], base.v), scale8(c[1], base.v),
                     scale8(c[2], base.v));
  }
}

static void effect_alphas_mods(void) {
  hsv_t mods = base;
  mods.h += speed;
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
    set_hsv(i, led_flags[i] & RGB_LED_FLAG_MODIFIER ? mods : base);
}

static void effect_gradient(bool up_down) {
  const uint8_t scale = scale8(64, speed);
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    hsv_t hsv = base;
    // The y range 0..64 maps to 0..4, the x range 0..224 to 0..7
    hsv.h = up_down ? base.h + scale * (led_points[i][1] >> 4)
                    : base.h + (scale * led_points[i][0] >> 5);
    set_hsv(i, hsv);
  }
}

// Effects built on QMK effect_runner_i
static void effect_runner_i(rgb_effect_t effect) {
  const uint8_t time = runner_i_time();
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    hsv_t hsv = base;
    const uint8_t x = led_points[i][0];
    switch (effect) {
    case RGB_EFFECT_BREATHING:
      hsv.v = scale8(abs8(sin8(time / 2) - 128) * 2, hsv.v);
      break;
    case RGB_EFFECT_BAND_SAT: {
      const int16_t s = hsv.s - abs(scale8(x, 228) + 28 - time) * 8;
      hsv.s = scale8(s < 0 ? 0 : s, hsv.s);
      break;
    }
    case RGB_EFFECT_BAND_VAL: {
      const int16_t v = hsv.v - abs(scale8(x, 228) + 28 - time) * 8;
      hsv.v = scale8(v < 0 ? 0 : v, hsv.v);
      break;
    }
    case RGB_EFFECT_CYCLE_ALL:
      hsv.h = rainbow_hue(time);
      break;
    case RGB_EFFECT_HUE_BREATHING:
      hsv.h = hsv.h + scale8(abs8(sin8(time / 2) - 128) * 2, 12);
      break;
    case RGB_EFFECT_HUE_WAVE:
      hsv.h = hsv.h + scale8(abs8(x - time), 24);
      break;
    default:
      break;
    }
    set_hsv(i, hsv);
  }
}

// Effects built on QMK effect_runner_dx_dy and effect_runner_dx_dy_dist
static void effect_runner_dx_dy(rgb_effect_t effect) {
  const uint8_t time = runner_dx_dy_time();
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    hsv_t hsv = base;
    const int16_t dx = led_points[i][0] - center_x;
    const int16_t dy = led_points[i][1] - center_y;
    switch (effect) {
    case RGB_EFFECT_BAND_PINWHEEL_SAT:
      hsv.s = scale8(hsv.s - time - atan2_8(dy, dx) * 3, hsv.s);
      break;
    case RGB_EFFECT_BAND_SPIRAL_SAT: {
      const uint8_t dist = sqrt16(dx * dx + dy * dy);
      hsv.s = scale8(hsv.s + dist - time - atan2_8(dy, dx), hsv.s);
      break;
    }
    case RGB_EFFECT_CYCLE_PINWHEEL:
      hsv.h = rainbow_hue(atan2_8(dy, dx) + time);
      break;
    default:
      break;
    }
    set_hsv(i, hsv);
  }
}

// QMK RAINBOW_PINWHEELS on effect_runner_sin_cos_i
static void effect_rainbow_pinwheels(void) {
  const uint16_t time = scale16by8(rgb_timer, speed / 4);
  const int8_t cos_value = cos8(time) - 128;
  const int8_t sin_value = sin8(time) - 128;
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    hsv_t hsv = base;
    // QMK passes cos_value as the effect's `sin` and sin_value as its `cos`
    hsv.h = rainbow_hue(
        ((led_points[i][1] - center_y) * 3 * sin_value +
         (56 - abs8(led_points[i][0] - center_x)) * 3 * cos_value) /
        128);
    set_hsv(i, hsv);
  }
}

static void effect_jellybean_raindrops(bool init) {
  if (init) {
    for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
      const uint8_t h = rainbow_hue(random8());
      set_hsv(i, (hsv_t){h, random8_min_max(127, 255), base.v});
    }
  } else if (scale16by8(rgb_timer, qadd8(speed, 16)) % 5 == 0) {
    // Change one LED per trigger
    const uint8_t led = random8_max(RGB_NUM_LEDS);
    const uint8_t h = rainbow_hue(random8());
    set_hsv(led, (hsv_t){h, random8_min_max(127, 255), base.v});
  }
}

static void effect_digital_rain(bool init) {
  // Algorithm ported from https://github.com/tremby/Kaleidoscope-LEDEffect-DigitalRain
  const uint8_t drop_ticks = 28;
  const uint8_t max_intensity = base.v;
  const uint8_t pure_green_intensity = ((uint16_t)max_intensity * 3) >> 2;
  const uint8_t max_brightness_boost = ((uint16_t)max_intensity * 3) >> 2;
  static uint8_t drop = 0;
  static uint8_t decay = 0;

  if (init || max_intensity == 0) {
    set_all(0, 0, 0);
    memset(frame_buffer, 0, sizeof(frame_buffer));
    drop = 0;
    if (max_intensity == 0)
      return;
  }
  const uint8_t decay_ticks = 0xff / max_intensity;
  // QMK's drops are pure green; this fork takes the base colour (hue 85 and
  // saturation 255 give exactly QMK's green)
  const rgb_t color = hsv_to_rgb((hsv_t){base.h, base.s, max_intensity});

  decay++;
  for (uint8_t col = 0; col < RGB_MATRIX_COLS; col++) {
    for (uint8_t row = 0; row < RGB_MATRIX_ROWS; row++) {
      uint8_t *cell = &frame_buffer[row][col];
      if (row == 0 && drop == 0 && rand() < RAND_MAX / RGB_DIGITAL_RAIN_DROPS) {
        // Top row, pixels have just fallen: a new drop in this column
        *cell = max_intensity;
      } else if (*cell > 0 && *cell < max_intensity) {
        // Neither fully bright nor dark, decay it
        if (decay == decay_ticks)
          (*cell)--;
      }

      const uint8_t led = matrix_leds[row][col];
      if (led == RGB_NO_LED)
        continue;
      if (*cell > pure_green_intensity) {
        const uint8_t boost =
            (uint8_t)((uint16_t)max_brightness_boost *
                      (*cell - pure_green_intensity) /
                      (max_intensity - pure_green_intensity));
        // The drop's head turns towards white
        rgb_driver_set(led, M_MIN(qadd8(color.r, boost), max_intensity),
                       M_MIN(qadd8(color.g, boost), max_intensity),
                       M_MIN(qadd8(color.b, boost), max_intensity));
      } else {
        rgb_driver_set(
            led, (uint8_t)((uint16_t)color.r * *cell / pure_green_intensity),
            (uint8_t)((uint16_t)color.g * *cell / pure_green_intensity),
            (uint8_t)((uint16_t)color.b * *cell / pure_green_intensity));
      }
    }
  }
  if (decay == decay_ticks)
    decay = 0;

  if (++drop > drop_ticks) {
    drop = 0;
    for (uint8_t row = RGB_MATRIX_ROWS - 1; row > 0; row--) {
      for (uint8_t col = 0; col < RGB_MATRIX_COLS; col++) {
        // On the bottom row a bright pixel may decay
        if (row == RGB_MATRIX_ROWS - 1 &&
            frame_buffer[row][col] == max_intensity)
          frame_buffer[row][col]--;
        // A bright pixel above moves down
        if (frame_buffer[row - 1][col] >= max_intensity) {
          frame_buffer[row - 1][col] = max_intensity - 1;
          frame_buffer[row][col] = max_intensity;
        }
      }
    }
  }
}

// QMK SOLID_REACTIVE_SIMPLE on effect_runner_reactive
static void effect_solid_reactive_simple(void) {
  const uint16_t max_tick = 65535 / qadd8(speed, 1);
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    uint16_t tick = max_tick;
    // Reverse search for the most recent hit on this LED
    for (int8_t j = hits.count - 1; j >= 0; j--) {
      if (hits.index[j] == i && hits.tick[j] < tick) {
        tick = hits.tick[j];
        break;
      }
    }
    // QMK lets this overshoot 255 when no key was hit (max_tick scaled by
    // speed + 1 with FASTLED_SCALE8_FIXED), and 255 - offset then wraps to a
    // bright value: every key lit at rest. Clamped so keys stay dark until
    // pressed, which is what the effect is for.
    const uint16_t offset = M_MIN(scale16by8(tick, qadd8(speed, 1)), 255);
    hsv_t hsv = base;
    hsv.h = rainbow_hue(scale16by8(rgb_timer, qadd8(speed, 8) >> 4));
    hsv.v = scale8(255 - offset, hsv.v);
    set_hsv(i, hsv);
  }
}

// QMK SOLID_REACTIVE_CROSS / MULTICROSS and SPLASH / MULTISPLASH on
// effect_runner_reactive_splash (the single versions use only the latest hit)
static void effect_reactive_splash(rgb_effect_t effect) {
  const bool splash =
      effect == RGB_EFFECT_SPLASH || effect == RGB_EFFECT_MULTISPLASH;
  const uint8_t start =
      effect == RGB_EFFECT_SPLASH || effect == RGB_EFFECT_SOLID_REACTIVE_CROSS
          ? qsub8(hits.count, 1)
          : 0;
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    hsv_t hsv = base;
    hsv.v = 0;
    uint8_t offset = 0;
    for (uint8_t j = start; j < hits.count; j++) {
      int16_t dx = led_points[i][0] - hits.x[j];
      int16_t dy = led_points[i][1] - hits.y[j];
      const uint8_t dist = sqrt16(dx * dx + dy * dy);
      const uint16_t tick = scale16by8(hits.tick[j], qadd8(speed, 1));
      uint16_t e;
      if (splash) {
        e = tick - dist;
        if (e > 255)
          e = 255;
        offset += e;
      } else {
        e = tick + dist;
        dx = dx < 0 ? dx * -1 : dx;
        dy = dy < 0 ? dy * -1 : dy;
        dx = dx * 16 > 255 ? 255 : dx * 16;
        dy = dy * 16 > 255 ? 255 : dy * 16;
        e += dx > dy ? dy : dx;
        if (e > 255)
          e = 255;
        offset = scale16by8(rgb_timer, qadd8(speed, 8) >> 4);
      }
      hsv.v = qadd8(hsv.v, 255 - e);
    }
    hsv.h = rainbow_hue(offset);
    hsv.v = scale8(hsv.v, base.v);
    set_hsv(i, hsv);
  }
}

//--------------------------------------------------------------------+
// The rest of QMK's effects (v1.1), ported from qmk_firmware 180f394b48
// (quantum/rgb_matrix/animations). Effects that sweep the colour wheel go
// through rainbow_hue like the stock ones, so the colour range works on them
// (at 255 it gives QMK's hues, turned by the base hue).
//--------------------------------------------------------------------+

typedef hsv_t (*i_f)(hsv_t hsv, uint8_t i, uint8_t time);
typedef hsv_t (*dx_dy_f)(hsv_t hsv, int16_t dx, int16_t dy, uint8_t time);
typedef hsv_t (*dist_f)(hsv_t hsv, int16_t dx, int16_t dy, uint8_t dist,
                        uint8_t time);
typedef hsv_t (*sin_cos_i_f)(hsv_t hsv, int8_t sin_v, int8_t cos_v, uint8_t i,
                             uint8_t time);
typedef hsv_t (*splash_f)(hsv_t hsv, int16_t dx, int16_t dy, uint8_t dist,
                          uint16_t tick);

// QMK effect_runner_i
static void runner_i(i_f f) {
  const uint8_t time = runner_i_time();
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
    set_hsv(i, f(base, i, time));
}

// QMK effect_runner_dx_dy
static void runner_dx_dy(dx_dy_f f) {
  const uint8_t time = runner_dx_dy_time();
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
    set_hsv(i, f(base, led_points[i][0] - center_x,
                 led_points[i][1] - center_y, time));
}

// QMK effect_runner_dx_dy_dist
static void runner_dist(dist_f f) {
  const uint8_t time = runner_dx_dy_time();
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    const int16_t dx = led_points[i][0] - center_x;
    const int16_t dy = led_points[i][1] - center_y;
    set_hsv(i, f(base, dx, dy, sqrt16(dx * dx + dy * dy), time));
  }
}

// QMK effect_runner_sin_cos_i (it passes cos_value as the effect's `sin` and
// sin_value as its `cos`)
static void runner_sin_cos_i(sin_cos_i_f f) {
  const uint16_t time = scale16by8(rgb_timer, speed / 4);
  const int8_t cos_value = cos8(time) - 128;
  const int8_t sin_value = sin8(time) - 128;
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
    set_hsv(i, f(base, cos_value, sin_value, i, time));
}

// QMK effect_runner_reactive_splash
static void runner_splash(uint8_t start, splash_f f) {
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    hsv_t hsv = base;
    hsv.v = 0;
    for (uint8_t j = start; j < hits.count; j++) {
      const int16_t dx = led_points[i][0] - hits.x[j];
      const int16_t dy = led_points[i][1] - hits.y[j];
      hsv = f(hsv, dx, dy, sqrt16(dx * dx + dy * dy),
              scale16by8(hits.tick[j], qadd8(speed, 1)));
    }
    hsv.v = scale8(hsv.v, base.v);
    set_hsv(i, hsv);
  }
}

static hsv_t band_pinwheel_val(hsv_t hsv, int16_t dx, int16_t dy,
                               uint8_t time) {
  hsv.v = scale8(hsv.v - time - atan2_8(dy, dx) * 3, hsv.v);
  return hsv;
}

static hsv_t band_spiral_val(hsv_t hsv, int16_t dx, int16_t dy, uint8_t dist,
                             uint8_t time) {
  hsv.v = scale8(hsv.v + dist - time - atan2_8(dy, dx), hsv.v);
  return hsv;
}

static hsv_t cycle_left_right(hsv_t hsv, uint8_t i, uint8_t time) {
  hsv.h = rainbow_hue(led_points[i][0] - time);
  return hsv;
}

static hsv_t cycle_up_down(hsv_t hsv, uint8_t i, uint8_t time) {
  hsv.h = rainbow_hue(led_points[i][1] - time);
  return hsv;
}

static hsv_t rainbow_moving_chevron(hsv_t hsv, uint8_t i, uint8_t time) {
  hsv.h = rainbow_hue(abs8(led_points[i][1] - center_y) +
                      (led_points[i][0] - time));
  return hsv;
}

static hsv_t cycle_out_in(hsv_t hsv, int16_t dx, int16_t dy, uint8_t dist,
                          uint8_t time) {
  hsv.h = rainbow_hue(3 * dist / 2 + time);
  return hsv;
}

static hsv_t cycle_out_in_dual(hsv_t hsv, int16_t dx, int16_t dy,
                               uint8_t time) {
  dx = (center_x / 2) - abs8(dx);
  const uint8_t dist = sqrt16(dx * dx + dy * dy);
  hsv.h = rainbow_hue(3 * dist + time);
  return hsv;
}

static hsv_t cycle_spiral(hsv_t hsv, int16_t dx, int16_t dy, uint8_t dist,
                          uint8_t time) {
  hsv.h = rainbow_hue(dist - time - atan2_8(dy, dx));
  return hsv;
}

static hsv_t dual_beacon(hsv_t hsv, int8_t sin_v, int8_t cos_v, uint8_t i,
                         uint8_t time) {
  hsv.h = rainbow_hue(((led_points[i][1] - center_y) * cos_v +
                       (led_points[i][0] - center_x) * sin_v) /
                      128);
  return hsv;
}

static hsv_t rainbow_beacon(hsv_t hsv, int8_t sin_v, int8_t cos_v, uint8_t i,
                            uint8_t time) {
  hsv.h = rainbow_hue(((led_points[i][1] - center_y) * 2 * cos_v +
                       (led_points[i][0] - center_x) * 2 * sin_v) /
                      128);
  return hsv;
}

static hsv_t hue_pendulum(hsv_t hsv, uint8_t i, uint8_t time) {
  hsv.h = hsv.h + scale8(abs8(sin8(time) + led_points[i][0] - 128) * 2, 12);
  return hsv;
}

// QMK STARLIGHT_SMOOTH: one random phase per LED, picked on first use
static uint8_t phase_offsets[RGB_NUM_LEDS];

static hsv_t starlight_smooth(hsv_t hsv, uint8_t i, uint8_t time) {
  if (phase_offsets[i] == 0)
    phase_offsets[i] = random8();
  hsv.v = scale8(abs8(sin8((time + phase_offsets[i]) / 2) - 128) * 2, hsv.v);
  return hsv;
}

static hsv_t riverflow(hsv_t hsv, uint8_t i, uint8_t time) {
  time = scale16by8(rgb_timer + i * 315u, speed / 8);
  hsv.v = scale8(abs8(sin8(time) - 128) * 2, hsv.v);
  return hsv;
}

static hsv_t solid_reactive_wide(hsv_t hsv, int16_t dx, int16_t dy,
                                 uint8_t dist, uint16_t tick) {
  uint16_t effect = tick + dist * 5;
  if (effect > 255)
    effect = 255;
  hsv.h = rainbow_hue(scale16by8(rgb_timer, qadd8(speed, 8) >> 4));
  hsv.v = qadd8(hsv.v, 255 - effect);
  return hsv;
}

static hsv_t solid_reactive_nexus(hsv_t hsv, int16_t dx, int16_t dy,
                                  uint8_t dist, uint16_t tick) {
  uint16_t effect = tick - dist;
  if (effect > 255)
    effect = 255;
  if (dist > 72)
    effect = 255;
  if ((dx > 8 || dx < -8) && (dy > 8 || dy < -8))
    effect = 255;
  // QMK first sets a time based hue, then overwrites it with this
  hsv.h = base.h + dy / 4;
  hsv.v = qadd8(hsv.v, 255 - effect);
  return hsv;
}

static hsv_t solid_splash(hsv_t hsv, int16_t dx, int16_t dy, uint8_t dist,
                          uint16_t tick) {
  uint16_t effect = tick - dist;
  if (effect > 255)
    effect = 255;
  hsv.v = qadd8(hsv.v, 255 - effect);
  return hsv;
}

// Tick of the most recent hit on this LED, or `max_tick` (QMK
// effect_runner_reactive)
static uint16_t reactive_tick(uint8_t led, uint16_t max_tick) {
  for (int8_t j = hits.count - 1; j >= 0; j--)
    if (hits.index[j] == led && hits.tick[j] < max_tick)
      return hits.tick[j];
  return max_tick;
}

// QMK SOLID_REACTIVE on effect_runner_reactive
static void effect_solid_reactive(void) {
  const uint16_t max_tick = 65535 / qadd8(speed, 1);
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    uint16_t offset = scale16by8(reactive_tick(i, max_tick), qadd8(speed, 1));
    if (offset > 255)
      offset = 255;
    hsv_t hsv = base;
    hsv.h = rainbow_hue(scale16by8(rgb_timer, qadd8(speed, 8) >> 4) +
                        scale8(255 - offset, 64));
    set_hsv(i, hsv);
  }
}

static void effect_raindrops(bool init) {
  // QMK's deltaH is (int8_t)((hsv.h + 128) - hsv.h) / 4: the int expression is
  // always 128, so -32 after the cast
  const int8_t delta_h = -32;
  if (init) {
    for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
      set_hsv(i, (hsv_t){(uint8_t)(base.h + delta_h * random8_max(3)), base.s,
                         base.v});
  } else if (scale16by8(rgb_timer, qadd8(speed, 16)) % 10 == 0) {
    const uint8_t led = random8_max(RGB_NUM_LEDS);
    set_hsv(led, (hsv_t){(uint8_t)(base.h + delta_h * random8_max(3)), base.s,
                         base.v});
  }
}

// QMK STARLIGHT, STARLIGHT_DUAL_HUE and STARLIGHT_DUAL_SAT
static void starlight_set(uint8_t i, rgb_effect_t effect) {
  const uint16_t time = scale16by8(rgb_timer, speed / 8);
  hsv_t hsv = base;
  hsv.v = scale8(abs8(sin8(time) - 128) * 2, hsv.v);
  if (effect == RGB_EFFECT_STARLIGHT_DUAL_HUE)
    hsv.h = hsv.h + random8_max(31);
  else if (effect == RGB_EFFECT_STARLIGHT_DUAL_SAT)
    hsv.s = hsv.s + random8_max(31);
  set_hsv(i, hsv);
}

static void effect_starlight(rgb_effect_t effect, bool init) {
  if (init) {
    for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
      starlight_set(i, effect);
  } else if (scale16by8(rgb_timer, qadd8(speed, 5)) % 5 == 0) {
    starlight_set(random8_max(RGB_NUM_LEDS), effect);
  }
}

// A random colour, or dark half of the time (QMK PIXEL_RAIN / PIXEL_FLOW)
static hsv_t pixel_hsv(void) {
  if (random8() & 2)
    return (hsv_t){0, 0, 0};
  const uint8_t h = rainbow_hue(random8());
  return (hsv_t){h, random8_min_max(127, 255), base.v};
}

static void effect_pixel_rain(bool init) {
  static uint8_t index = 0;
  static uint32_t timer = 0;
  if (init)
    index = random8_max(RGB_NUM_LEDS);
  if (timer < rgb_timer) {
    set_hsv(index, pixel_hsv());
    index = random8_max(RGB_NUM_LEDS);
    timer = rgb_timer + (2048u - scale16by8(1792, speed));
  }
}

static void effect_pixel_flow(bool init) {
  static rgb_t led[RGB_NUM_LEDS];
  static uint32_t wait_timer = 0;
  if (wait_timer > rgb_timer)
    return;
  if (init) {
    set_all(0, 0, 0);
    for (uint8_t j = 0; j < RGB_NUM_LEDS; j++)
      led[j] = hsv_to_rgb(pixel_hsv());
  }
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
    rgb_driver_set(i, led[i].r, led[i].g, led[i].b);
  memmove(&led[0], &led[1], (RGB_NUM_LEDS - 1) * sizeof(rgb_t));
  led[RGB_NUM_LEDS - 1] = hsv_to_rgb(pixel_hsv());
  wait_timer = rgb_timer + 3000u / scale16by8(qadd8(speed, 16), 16);
}

static void effect_pixel_fractal(bool init) {
  enum { MID_COL = RGB_MATRIX_COLS / 2 };
  static bool lit[RGB_MATRIX_ROWS][MID_COL];
  static uint32_t wait_timer = 0;
  if (init) {
    set_all(0, 0, 0);
    memset(lit, 0, sizeof(lit));
  }
  if (rgb_timer <= wait_timer)
    return;
  const rgb_t on = hsv_to_rgb(base);
  for (uint8_t row = 0; row < RGB_MATRIX_ROWS; row++) {
    for (uint8_t col = 0; col < MID_COL; col++) {
      const rgb_t c = lit[row][col] ? on : (rgb_t){0, 0, 0};
      const uint8_t l = matrix_leds[row][col];
      const uint8_t r = matrix_leds[row][RGB_MATRIX_COLS - 1 - col];
      if (l != RGB_NO_LED)
        rgb_driver_set(l, c.r, c.g, c.b);
      if (r != RGB_NO_LED)
        rgb_driver_set(r, c.r, c.g, c.b);
    }
    // Shift towards the outside, a new pixel enters in the middle
    memmove(lit[row], lit[row] + 1, (MID_COL - 1) * sizeof(bool));
    lit[row][MID_COL - 1] = !(random8() & 3);
  }
  wait_timer = rgb_timer + 3000u / scale16by8(qadd8(speed, 16), 16);
}

// QMK FLOWER_BLOOMING on its effect_runner_bloom: the lower half turns the
// other way and swaps red and blue
static void effect_flower_blooming(void) {
  const uint8_t time = scale16by8(rgb_timer, qadd8(speed / 10, 1));
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    const bool lower = led_points[i][1] > center_y;
    hsv_t hsv = base;
    hsv.h = rainbow_hue(led_points[i][0] * 3 - led_points[i][1] * 3 +
                        (lower ? time : -(int16_t)time));
    const rgb_t rgb = hsv_to_rgb(hsv);
    if (lower)
      rgb_driver_set(i, rgb.b, rgb.g, rgb.r);
    else
      rgb_driver_set(i, rgb.r, rgb.g, rgb.b);
  }
}

// QMK TYPING_HEATMAP with its default settings, kept per LED instead of per
// matrix position (one LED per key on this board)
#define HEATMAP_INCREASE_STEP 32
#define HEATMAP_DECREASE_DELAY_MS 25
#define HEATMAP_SPREAD 40
#define HEATMAP_AREA_LIMIT 16

static uint8_t heat[RGB_NUM_LEDS];

// QMK process_rgb_matrix_typing_heatmap
static void heatmap_hit(uint8_t led) {
  heat[led] = qadd8(heat[led], HEATMAP_INCREASE_STEP);
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    if (i == led) {
      heat[i] = qadd8(heat[i], HEATMAP_INCREASE_STEP);
      continue;
    }
    const int16_t dx = led_points[i][0] - led_points[led][0];
    const int16_t dy = led_points[i][1] - led_points[led][1];
    const uint8_t distance = sqrt16(dx * dx + dy * dy);
    if (distance <= HEATMAP_SPREAD)
      heat[i] = qadd8(heat[i], M_MIN(qsub8(HEATMAP_SPREAD, distance),
                                     HEATMAP_AREA_LIMIT));
  }
}

static void effect_typing_heatmap(bool init) {
  static uint32_t decrease_timer;
  if (init) {
    set_all(0, 0, 0);
    memset(heat, 0, sizeof(heat));
    decrease_timer = rgb_timer;
  }
  // QMK cools every key by 1 every 25 ms; frames here are 20 ms apart, so
  // count the 25 ms steps that passed
  const uint32_t steps =
      (rgb_timer - decrease_timer) / HEATMAP_DECREASE_DELAY_MS;
  decrease_timer += steps * HEATMAP_DECREASE_DELAY_MS;
  for (uint8_t i = 0; i < RGB_NUM_LEDS; i++) {
    const uint8_t val = heat[i];
    set_hsv(i, (hsv_t){(uint8_t)(170 - qsub8(val, 85)), base.s,
                       scale8((qadd8(170, val) - 170) * 3, base.v)});
    heat[i] = qsub8(val, (uint8_t)M_MIN(steps, 255));
  }
}

static void rgb_render(bool init) {
  const eeconfig_rgb_t *cfg = &eeconfig->rgb;
  base = (hsv_t){cfg->hue, cfg->sat, M_MIN(cfg->val, RGB_MAX_BRIGHTNESS)};
  speed = cfg->speed;
  hue_range = cfg->hue_range;

  switch (cfg->effect) {
  case RGB_EFFECT_CUSTOM:
    effect_custom();
    break;
  case RGB_EFFECT_SOLID_COLOR:
    for (uint8_t i = 0; i < RGB_NUM_LEDS; i++)
      set_hsv(i, base);
    break;
  case RGB_EFFECT_ALPHAS_MODS:
    effect_alphas_mods();
    break;
  case RGB_EFFECT_GRADIENT_UP_DOWN:
  case RGB_EFFECT_GRADIENT_LEFT_RIGHT:
    effect_gradient(cfg->effect == RGB_EFFECT_GRADIENT_UP_DOWN);
    break;
  case RGB_EFFECT_BREATHING:
  case RGB_EFFECT_BAND_SAT:
  case RGB_EFFECT_BAND_VAL:
  case RGB_EFFECT_CYCLE_ALL:
  case RGB_EFFECT_HUE_BREATHING:
  case RGB_EFFECT_HUE_WAVE:
    effect_runner_i(cfg->effect);
    break;
  case RGB_EFFECT_BAND_PINWHEEL_SAT:
  case RGB_EFFECT_BAND_SPIRAL_SAT:
  case RGB_EFFECT_CYCLE_PINWHEEL:
    effect_runner_dx_dy(cfg->effect);
    break;
  case RGB_EFFECT_RAINBOW_PINWHEELS:
    effect_rainbow_pinwheels();
    break;
  case RGB_EFFECT_JELLYBEAN_RAINDROPS:
    effect_jellybean_raindrops(init);
    break;
  case RGB_EFFECT_DIGITAL_RAIN:
    effect_digital_rain(init);
    break;
  case RGB_EFFECT_SOLID_REACTIVE_SIMPLE:
    effect_solid_reactive_simple();
    break;
  case RGB_EFFECT_SOLID_REACTIVE_MULTICROSS:
  case RGB_EFFECT_SPLASH:
  case RGB_EFFECT_SOLID_REACTIVE_CROSS:
  case RGB_EFFECT_MULTISPLASH:
    effect_reactive_splash(cfg->effect);
    break;
  case RGB_EFFECT_BAND_PINWHEEL_VAL:
    runner_dx_dy(band_pinwheel_val);
    break;
  case RGB_EFFECT_BAND_SPIRAL_VAL:
    runner_dist(band_spiral_val);
    break;
  case RGB_EFFECT_CYCLE_LEFT_RIGHT:
    runner_i(cycle_left_right);
    break;
  case RGB_EFFECT_CYCLE_UP_DOWN:
    runner_i(cycle_up_down);
    break;
  case RGB_EFFECT_RAINBOW_MOVING_CHEVRON:
    runner_i(rainbow_moving_chevron);
    break;
  case RGB_EFFECT_CYCLE_OUT_IN:
    runner_dist(cycle_out_in);
    break;
  case RGB_EFFECT_CYCLE_OUT_IN_DUAL:
    runner_dx_dy(cycle_out_in_dual);
    break;
  case RGB_EFFECT_CYCLE_SPIRAL:
    runner_dist(cycle_spiral);
    break;
  case RGB_EFFECT_DUAL_BEACON:
    runner_sin_cos_i(dual_beacon);
    break;
  case RGB_EFFECT_RAINBOW_BEACON:
    runner_sin_cos_i(rainbow_beacon);
    break;
  case RGB_EFFECT_RAINDROPS:
    effect_raindrops(init);
    break;
  case RGB_EFFECT_HUE_PENDULUM:
    runner_i(hue_pendulum);
    break;
  case RGB_EFFECT_PIXEL_FRACTAL:
    effect_pixel_fractal(init);
    break;
  case RGB_EFFECT_PIXEL_FLOW:
    effect_pixel_flow(init);
    break;
  case RGB_EFFECT_PIXEL_RAIN:
    effect_pixel_rain(init);
    break;
  case RGB_EFFECT_TYPING_HEATMAP:
    effect_typing_heatmap(init);
    break;
  case RGB_EFFECT_SOLID_REACTIVE:
    effect_solid_reactive();
    break;
  case RGB_EFFECT_SOLID_REACTIVE_WIDE:
    runner_splash(qsub8(hits.count, 1), solid_reactive_wide);
    break;
  case RGB_EFFECT_SOLID_REACTIVE_MULTIWIDE:
    runner_splash(0, solid_reactive_wide);
    break;
  case RGB_EFFECT_SOLID_REACTIVE_NEXUS:
    runner_splash(qsub8(hits.count, 1), solid_reactive_nexus);
    break;
  case RGB_EFFECT_SOLID_REACTIVE_MULTINEXUS:
    runner_splash(0, solid_reactive_nexus);
    break;
  case RGB_EFFECT_SOLID_SPLASH:
    runner_splash(qsub8(hits.count, 1), solid_splash);
    break;
  case RGB_EFFECT_SOLID_MULTISPLASH:
    runner_splash(0, solid_splash);
    break;
  case RGB_EFFECT_STARLIGHT:
  case RGB_EFFECT_STARLIGHT_DUAL_HUE:
  case RGB_EFFECT_STARLIGHT_DUAL_SAT:
    effect_starlight(cfg->effect, init);
    break;
  case RGB_EFFECT_STARLIGHT_SMOOTH:
    if (init)
      memset(phase_offsets, 0, sizeof(phase_offsets));
    runner_i(starlight_smooth);
    break;
  case RGB_EFFECT_RIVERFLOW:
    runner_i(riverflow);
    break;
  case RGB_EFFECT_FLOWER_BLOOMING:
    effect_flower_blooming();
    break;
  case RGB_EFFECT_OFF:
  default:
    set_all(0, 0, 0);
    break;
  }
}

//--------------------------------------------------------------------+
// Task
//--------------------------------------------------------------------+

// QMK rgb_matrix_handle_key_event (key presses only)
static void rgb_add_hit(uint8_t led) {
  if (hits.count + 1 > RGB_HITS) {
    memmove(&hits.x[0], &hits.x[1], RGB_HITS - 1);
    memmove(&hits.y[0], &hits.y[1], RGB_HITS - 1);
    memmove(&hits.tick[0], &hits.tick[1], (RGB_HITS - 1) * 2);
    memmove(&hits.index[0], &hits.index[1], RGB_HITS - 1);
    hits.count = RGB_HITS - 1;
  }
  hits.x[hits.count] = led_points[led][0];
  hits.y[hits.count] = led_points[led][1];
  hits.index[hits.count] = led;
  hits.tick[hits.count] = 0;
  hits.count++;
}

// QMK rgb_task_timers: age the hits, drop the ones that would overflow
static void rgb_age_hits(uint32_t elapsed) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < hits.count; i++) {
    if (UINT16_MAX - elapsed < hits.tick[i])
      continue;
    hits.x[kept] = hits.x[i];
    hits.y[kept] = hits.y[i];
    hits.index[kept] = hits.index[i];
    hits.tick[kept] = hits.tick[i] + elapsed;
    kept++;
  }
  hits.count = kept;
}

void rgb_task(void) {
  static bool started = false;
  static uint32_t last_frame = 0;
  static uint8_t last_effect = 0xFF;
  static bitmap_t pressed[] = MAKE_BITMAP(NUM_KEYS);

  if (!started) {
    rgb_driver_init();
    started = true;
  }

  // Key presses feed the reactive effects and the typing heatmap
  for (uint8_t k = 0; k < NUM_KEYS; k++) {
    const bool is_pressed = key_matrix[k].is_pressed;
    if (is_pressed && !bitmap_get(pressed, k) && key_leds[k] != RGB_NO_LED) {
      rgb_add_hit(key_leds[k]);
      if (eeconfig->rgb.effect == RGB_EFFECT_TYPING_HEATMAP)
        heatmap_hit(key_leds[k]);
    }
    bitmap_set(pressed, k, is_pressed);
  }

  const uint32_t now = timer_read();
  if (now - last_frame < RGB_FRAME_MS)
    return;
  rgb_age_hits(now - last_frame);
  last_frame = now;
  rgb_timer = now;

  bool init = eeconfig->rgb.effect != last_effect;
  last_effect = eeconfig->rgb.effect;
#if defined(RGB_CAPS_LOCK_KEY)
  // Caps Lock indicator: drawn over the effect. When it goes off, redraw the
  // whole effect, since some effects only touch a few LEDs per frame.
  static bool caps_shown = false;
  const bool caps = !eeconfig->options.caps_indicator_disabled &&
                    (hid_get_led_state() & 0x02);
  init |= caps_shown & !caps;
  caps_shown = caps;
#endif
  rgb_render(init);
#if defined(RGB_CAPS_LOCK_KEY)
  if (caps) {
    // White at the set brightness, but always visible (also with the
    // lighting off)
    const uint8_t v =
        M_MAX(M_MIN(eeconfig->rgb.val, RGB_MAX_BRIGHTNESS), 64);
    rgb_driver_set(key_leds[RGB_CAPS_LOCK_KEY], v, v, v);
  }
#endif
}

//--------------------------------------------------------------------+
// Commands
//--------------------------------------------------------------------+

// Key colours per packet: 63 data bytes / 3
#define RGB_KEY_COLORS_PER_PACKET 20

bool rgb_command(const uint8_t *in, uint8_t *out) {
  switch (in[0]) {
  case COMMAND_GET_RGB: {
    // effect, hue, sat, val, speed, number of effects, max brightness, colour
    // range
    const eeconfig_rgb_t *cfg = &eeconfig->rgb;
    out[1] = cfg->effect;
    out[2] = cfg->hue;
    out[3] = cfg->sat;
    out[4] = cfg->val;
    out[5] = cfg->speed;
    out[6] = RGB_EFFECT_COUNT;
    out[7] = RGB_MAX_BRIGHTNESS;
    out[8] = cfg->hue_range;
    return true;
  }
  case COMMAND_SET_RGB: {
    // effect, hue, sat, val, speed, colour range (the key colours are kept)
    if (in[1] >= RGB_EFFECT_COUNT)
      return false;
    eeconfig_rgb_t cfg = eeconfig->rgb;
    cfg.effect = in[1];
    cfg.hue = in[2];
    cfg.sat = in[3];
    cfg.val = M_MIN(in[4], RGB_MAX_BRIGHTNESS);
    cfg.speed = in[5];
    cfg.hue_range = in[6];
    return EECONFIG_WRITE(rgb, &cfg);
  }
  case COMMAND_GET_RGB_KEY_COLORS: {
    // in: key offset; out: count, then R, G, B per key
    const uint8_t offset = in[1];
    if (offset >= NUM_KEYS)
      return false;
    const uint8_t len = M_MIN(RGB_KEY_COLORS_PER_PACKET, NUM_KEYS - offset);
    out[1] = len;
    memcpy(&out[2], eeconfig->rgb.key_colors[offset], len * 3u);
    return true;
  }
  case COMMAND_SET_RGB_KEY_COLORS: {
    // in: key offset, count, then R, G, B per key
    const uint8_t offset = in[1], len = in[2];
    if (offset >= NUM_KEYS || len > RGB_KEY_COLORS_PER_PACKET ||
        len > NUM_KEYS - offset)
      return false;
    return EECONFIG_WRITE_N(rgb.key_colors[offset], &in[3], len * 3u);
  }
  default:
    return false;
  }
}

#endif
