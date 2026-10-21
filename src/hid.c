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

#include "hid.h"

#include "commands.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "keycodes.h"
#include "matrix.h"
#include "tusb.h"
#include "usb_descriptors.h"

// Track how many keys are currently in the 6KRO part of the report
static uint8_t num_6kro_keys;
static hid_nkro_kb_report_t kb_report;

static uint16_t system_report;
static uint16_t consumer_report;
static hid_mouse_report_t mouse_report;

// Keyboard LEDs set by the host
static uint8_t led_state;
// Repeat Key: the last key pressed and the modifiers held with it
static uint8_t last_keycode = KC_NO, last_modifiers;
// What Repeat Key and Grave Escape pressed, to release the same
static uint8_t repeat_keycode = KC_NO, repeat_modifiers;
static uint8_t grave_escape_keycode = KC_NO;

// HID modifier bits
#define HID_MOD_CTRL 0x11
#define HID_MOD_SHIFT 0x22
#define HID_MOD_GUI 0x88
#define HID_MOD_LEFT_SHIFT 0x02
#define HID_MOD_RIGHT_SHIFT 0x20

// Caps Word (QMK, this fork v1.5): letters get Shift and - becomes _ until a
// key that ends a word, or this long without a key press
#define CAPS_WORD_IDLE_MS 5000
static bool caps_word;
static uint32_t caps_word_time;
// The key Caps Word added Shift for; the Shift goes with it
static uint8_t caps_word_key = KC_NO;

static void hid_caps_word_unshift(void) {
  if (caps_word_key == KC_NO)
    return;
  kb_report.modifiers &= ~HID_MOD_LEFT_SHIFT;
  caps_word_key = KC_NO;
}

bool hid_caps_word_active(void) {
  return caps_word && timer_elapsed(caps_word_time) < CAPS_WORD_IDLE_MS;
}

void hid_caps_word_toggle(void) {
  caps_word = !hid_caps_word_active();
  caps_word_time = timer_read();
  if (!caps_word)
    hid_caps_word_unshift();
}

/**
 * @brief Caps Word's part of a key press, before the key goes in the report
 *
 * @param keycode Keycode (before the Mac mode swap)
 *
 * @return None
 */
static void hid_caps_word_press(uint8_t keycode) {
  if (!hid_caps_word_active()) {
    caps_word = false;
    // Both Shifts together turn it on (option)
    if (eeconfig->options.caps_word_both_shifts &&
        ((keycode == KC_LEFT_SHIFT &&
          (kb_report.modifiers & HID_MOD_RIGHT_SHIFT)) ||
         (keycode == KC_RIGHT_SHIFT &&
          (kb_report.modifiers & HID_MOD_LEFT_SHIFT)))) {
      caps_word = true;
      caps_word_time = timer_read();
    }
    return;
  }

  caps_word_time = timer_read();
  // A letter's Shift does not carry over to the next key
  hid_caps_word_unshift();
  switch (keycode) {
  case KC_A ... KC_Z:
  case KC_MINUS:
    if (!(kb_report.modifiers & HID_MOD_SHIFT)) {
      kb_report.modifiers |= HID_MOD_LEFT_SHIFT;
      caps_word_key = keycode;
    }
    break;
  case KC_1 ... KC_0:
  case KC_BACKSPACE:
  case KC_DELETE:
  case KC_LEFT_SHIFT:
  case KC_RIGHT_SHIFT:
  case KC_RIGHT_ALT:
    // Part of the word
    break;
  default:
    // Anything else ends it
    caps_word = false;
    break;
  }
}

bool hid_shift_held(void) { return kb_report.modifiers & HID_MOD_SHIFT; }

/**
 * @brief Swap Alt and GUI in Mac mode
 *
 * @param keycode Keycode
 *
 * @return Keycode to use
 */
static uint8_t hid_mac_swap(uint8_t keycode) {
  if (!eeconfig->options.mac_mode_enabled)
    return keycode;
  switch (keycode) {
  case KC_LEFT_ALT:
    return KC_LEFT_GUI;
  case KC_LEFT_GUI:
    return KC_LEFT_ALT;
  case KC_RIGHT_ALT:
    return KC_RIGHT_GUI;
  case KC_RIGHT_GUI:
    return KC_RIGHT_ALT;
  default:
    return keycode;
  }
}

#if !defined(HID_DISABLED)
/**
 * @brief Send the keyboard report
 *
 * This function will send the keyboard report to its exclusive interface.
 *
 * @return None
 */
static void hid_send_keyboard_report(void) {
  static hid_nkro_kb_report_t prev_kb_report = {0};

  if (memcmp(&prev_kb_report, &kb_report, sizeof(prev_kb_report)) == 0)
    // Don't send the report if it hasn't changed
    return;

  prev_kb_report = kb_report;
  tud_hid_n_report(USB_ITF_KEYBOARD, 0, &kb_report, sizeof(kb_report));
}
#endif

/**
 * @brief Find the next available report and send it
 *
 * @param starting_report_id The report ID to start searching from
 *
 * @return None
 */
static void hid_send_hid_report(uint8_t starting_report_id) {
  static uint16_t prev_system_report = 0;
  static uint16_t prev_consumer_report = 0;
  static hid_mouse_report_t prev_mouse_report = {0};

  for (uint8_t report_id = starting_report_id; report_id < REPORT_ID_COUNT;
       report_id++) {
    switch (report_id) {
    case REPORT_ID_SYSTEM_CONTROL:
      if (system_report == prev_system_report)
        // Don't send the report if it hasn't changed
        break;
      prev_system_report = system_report;
      tud_hid_n_report(USB_ITF_HID, report_id, &system_report,
                       sizeof(system_report));
      return;

    case REPORT_ID_CONSUMER_CONTROL:
      if (consumer_report == prev_consumer_report)
        // Don't send the report if it hasn't changed
        break;
      prev_consumer_report = consumer_report;
      tud_hid_n_report(USB_ITF_HID, report_id, &consumer_report,
                       sizeof(consumer_report));
      return;

    case REPORT_ID_MOUSE:
      if (memcmp(&prev_mouse_report, &mouse_report,
                 sizeof(prev_mouse_report)) == 0)
        // Don't send the report if it hasn't changed
        break;
      prev_mouse_report = mouse_report;
      tud_hid_n_report(USB_ITF_HID, report_id, &mouse_report,
                       sizeof(mouse_report));
      return;

    default:
      break;
    }
  }
}

void hid_init(void) {}

void hid_keycode_add(uint8_t keycode) {
  hid_caps_word_press(keycode);
  keycode = hid_mac_swap(keycode);
  const uint16_t hid_code = keycode_to_hid[keycode];

  if (!hid_code)
    // No HID code for this keycode
    return;

  bool found = false;
  switch (keycode) {
  case KEYBOARD_KEYCODE_RANGE:
    for (uint32_t i = 0; i < num_6kro_keys; i++) {
      if (kb_report.keycodes[i] == hid_code) {
        found = true;
        break;
      }
    }

    if (!found) {
      if (num_6kro_keys == 6) {
        if (eeconfig->options.nkro_disabled)
          // 6KRO mode: a 7th key is ignored, like on a 6KRO keyboard
          break;
        // If the 6KRO report is full, remove the oldest key
        for (uint32_t i = 0; i < 5; i++)
          kb_report.keycodes[i] = kb_report.keycodes[i + 1];
        num_6kro_keys--;
      }
      kb_report.keycodes[num_6kro_keys++] = hid_code;
    }
    // The host reads only the bitmap (the descriptor declares the 6-key list
    // as padding; it is for the BIOS boot protocol), so in 6KRO mode the
    // bitmap holds just the keys that made it into the 6-key list
    kb_report.bitmap[hid_code / 8] |= 1 << (hid_code & 7);
    last_keycode = keycode;
    last_modifiers = kb_report.modifiers;
    break;

  case MODIFIER_KEYCODE_RANGE:
    if (eeconfig->options.gui_lock_enabled &
        ((keycode == KC_LEFT_GUI) | (keycode == KC_RIGHT_GUI)))
      // Win lock
      break;
    kb_report.modifiers |= hid_code;
    break;

  case SYSTEM_KEYCODE_RANGE:
    system_report = hid_code;
    break;

  case CONSUMER_KEYCODE_RANGE:
    consumer_report = hid_code;
    break;

  case MOUSE_KEYCODE_RANGE:
    mouse_report.buttons |= hid_code;
    break;

  default:
    break;
  }
}

void hid_keycode_remove(uint8_t keycode) {
  if (keycode == caps_word_key)
    hid_caps_word_unshift();
  keycode = hid_mac_swap(keycode);
  const uint16_t hid_code = keycode_to_hid[keycode];

  if (!hid_code)
    // No HID code for this keycode
    return;

  switch (keycode) {
  case KEYBOARD_KEYCODE_RANGE:
    for (uint32_t i = 0; i < num_6kro_keys; i++) {
      if (kb_report.keycodes[i] == hid_code) {
        memmove(kb_report.keycodes + i, kb_report.keycodes + i + 1,
                sizeof(uint8_t) * (num_6kro_keys - i - 1));
        num_6kro_keys--;
        // Clear the freed slot, or the BIOS (boot protocol) sees a stale key
        kb_report.keycodes[num_6kro_keys] = 0;
        break;
      }
    }
    kb_report.bitmap[hid_code / 8] &= ~(1 << (hid_code & 7));
    break;

  case MODIFIER_KEYCODE_RANGE:
    kb_report.modifiers &= ~hid_code;
    break;

  case SYSTEM_KEYCODE_RANGE:
    if (system_report == hid_code)
      // Only remove the system report if it matches the one we're trying to
      system_report = 0;
    break;

  case CONSUMER_KEYCODE_RANGE:
    if (consumer_report == hid_code)
      // Only remove the consumer report if it matches the one we're trying to
      consumer_report = 0;
    break;

  case MOUSE_KEYCODE_RANGE:
    mouse_report.buttons &= ~hid_code;
    break;

  default:
    break;
  }
}

void hid_grave_escape_press(void) {
  const uint8_t mods = kb_report.modifiers;
  grave_escape_keycode =
      (mods & (HID_MOD_SHIFT | HID_MOD_GUI)) && !(mods & HID_MOD_CTRL)
          ? KC_GRAVE
          : KC_ESCAPE;
  hid_keycode_add(grave_escape_keycode);
}

void hid_grave_escape_release(void) {
  hid_keycode_remove(grave_escape_keycode);
  grave_escape_keycode = KC_NO;
}

void hid_repeat_press(void) {
  if (last_keycode == KC_NO)
    return;
  // Add the modifiers that were held with the key and are not held now. They
  // are report bits already (after Mac mode and Win lock), so set them
  // directly rather than through hid_keycode_add.
  repeat_modifiers = last_modifiers & ~kb_report.modifiers;
  kb_report.modifiers |= repeat_modifiers;
  repeat_keycode = last_keycode;
  hid_keycode_add(repeat_keycode);
}

void hid_repeat_release(void) {
  if (repeat_keycode == KC_NO)
    return;
  hid_keycode_remove(repeat_keycode);
  kb_report.modifiers &= ~repeat_modifiers;
  repeat_keycode = KC_NO;
  repeat_modifiers = 0;
}

uint8_t hid_get_led_state(void) { return led_state; }

#if !defined(HID_DISABLED)
// Long enough for a host to resume after a remote wakeup
#define HID_READY_TIMEOUT_MS 100

/**
 * @brief Wait for an interface to be ready, but not for a host that is asleep
 *
 * A host that is off or asleep with USB power still on never polls. Waiting
 * for it forever stopped the main loop until the watchdog sent the board to
 * the bootloader. The report is dropped instead; the next one carries the
 * current state.
 *
 * @param instance HID interface
 *
 * @return Whether the interface is ready
 */
static bool hid_wait_ready(uint8_t instance) {
  const uint32_t start = timer_read();
  while (!tud_hid_n_ready(instance)) {
    if (timer_elapsed(start) >= HID_READY_TIMEOUT_MS)
      return false;
    tud_task();
  }
  return true;
}
#endif

void hid_send_reports(void) {
#if !defined(HID_DISABLED)
  if (tud_suspended())
    // Wake up the host if it's suspended
    tud_remote_wakeup();

  if (!hid_wait_ready(USB_ITF_KEYBOARD))
    return;

  hid_send_keyboard_report();

  if (!hid_wait_ready(USB_ITF_HID))
    return;

  // Start from the first report ID
  hid_send_hid_report(REPORT_ID_SYSTEM_CONTROL);
#endif
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, const uint8_t *buffer,
                           uint16_t bufsize) {
  if (instance == USB_ITF_RAW_HID && report_id == 0 &&
      report_type == HID_REPORT_TYPE_OUTPUT)
    command_enqueue(buffer, bufsize);
  else if (instance == USB_ITF_KEYBOARD &&
           report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1)
    // The keyboard report has no report ID: one byte of LED bits
    led_state = buffer[0];
}

void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report,
                                uint16_t len) {
  if (instance == USB_ITF_HID)
    // Start from the next report ID
    hid_send_hid_report(report[0] + 1);
}
