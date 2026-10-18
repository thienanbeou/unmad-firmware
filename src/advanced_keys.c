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

#include "advanced_keys.h"

#include "deferred_actions.h"
#include "eeconfig.h"
#include "hardware/hardware.h"
#include "keycodes.h"
#include "layout.h"
#include "matrix.h"

static advanced_key_state_t ak_states[NUM_ADVANCED_KEYS];

static void advanced_key_null_bind_reset_state(ak_state_null_bind_t *state) {
  state->is_pressed[0] = state->is_pressed[1] = false;
  state->keycodes[0] = state->keycodes[1] = KC_NO;
}

static void advanced_key_null_bind(const advanced_key_event_t *event) {
  const null_bind_t *null_bind =
      &CURRENT_PROFILE.advanced_keys[event->ak_index].null_bind;
  ak_state_null_bind_t *state = &ak_states[event->ak_index].null_bind;

  const uint8_t keys[] = {
      CURRENT_PROFILE.advanced_keys[event->ak_index].key,
      null_bind->secondary_key,
  };
  const uint8_t index = event->key == keys[0] ? 0 : 1;

  // Update the active keycodes
  switch (event->type) {
  case AK_EVENT_TYPE_PRESS:
    state->keycodes[index] = event->keycode;
    break;

  case AK_EVENT_TYPE_RELEASE:
    if (state->is_pressed[index]) {
      // Also release the key if it is registered
      layout_unregister(keys[index], state->keycodes[index]);
      state->is_pressed[index] = false;
    }
    state->keycodes[index] = KC_NO;
    break;

  default:
    break;
  }

  bool is_pressed[] = {
      state->keycodes[0] != KC_NO,
      state->keycodes[1] != KC_NO,
  };
  if (is_pressed[0] & is_pressed[1]) {
    // Both keys are pressed so we perform the Null Bind resolution.
    if ((null_bind->bottom_out_point > 0) &&
        ((key_matrix[keys[0]].distance >= null_bind->bottom_out_point) &
         (key_matrix[keys[1]].distance >= null_bind->bottom_out_point)))
      // Input on both bottom out is enabled and both keys are bottomed out so
      // we register both keys.
      is_pressed[0] = is_pressed[1] = true;
    else if (null_bind->behavior == NB_BEHAVIOR_DISTANCE) {
      // Always compare the distance, regardless of the event type. If there is
      // a tie between the travel distances, the last pressed key is
      // prioritized.
      is_pressed[index] = key_matrix[keys[index]].distance >=
                          key_matrix[keys[index ^ 1]].distance;
      is_pressed[index ^ 1] = !is_pressed[index];
    } else if (event->type == AK_EVENT_TYPE_PRESS) {
      // Other behaviors only require comparison on press events.
      is_pressed[index] =
          (null_bind->behavior != NB_BEHAVIOR_NEUTRAL) &
          ((null_bind->behavior == NB_BEHAVIOR_LAST) |
           ((null_bind->behavior == NB_BEHAVIOR_PRIMARY) & (index == 0)) |
           ((null_bind->behavior == NB_BEHAVIOR_SECONDARY) & (index == 1)));
      // Only one key can be registered at a time except for the
      // `NB_BEHAVIOR_NEUTRAL`.
      is_pressed[index ^ 1] =
          (null_bind->behavior != NB_BEHAVIOR_NEUTRAL) & !is_pressed[index];
    } else {
      // No action is required for other event types.
      is_pressed[0] = state->is_pressed[0];
      is_pressed[1] = state->is_pressed[1];
    }
  }

  // Update the key states. The only changes here are the results of the Null
  // Bind resolution.
  for (uint32_t i = 0; i < 2; i++) {
    if (is_pressed[i] & !state->is_pressed[i]) {
      layout_register(keys[i], state->keycodes[i]);
      state->is_pressed[i] = true;
    } else if (!is_pressed[i] & state->is_pressed[i]) {
      layout_unregister(keys[i], state->keycodes[i]);
      state->is_pressed[i] = false;
    }
  }
}

static void advanced_key_dynamic_keystroke_reset_state(
    ak_state_dynamic_keystroke_t *state) {
  for (uint32_t i = 0; i < NUM_DYNAMIC_KEYSTROKE_MAX_BINDINGS; i++)
    state->is_pressed[i] = false;
  state->is_bottomed_out = false;
}

static void advanced_key_dynamic_keystroke(const advanced_key_event_t *event) {
  static deferred_action_t deferred_action = {0};

  const dynamic_keystroke_t *dks =
      &CURRENT_PROFILE.advanced_keys[event->ak_index].dynamic_keystroke;
  ak_state_dynamic_keystroke_t *state =
      &ak_states[event->ak_index].dynamic_keystroke;

  const bool is_bottomed_out =
      (key_matrix[event->key].distance >= dks->bottom_out_point);
  uint8_t event_type = event->type;

  if (is_bottomed_out & !state->is_bottomed_out)
    event_type = AK_EVENT_TYPE_BOTTOM_OUT;
  else if ((event_type != AK_EVENT_TYPE_RELEASE) & !is_bottomed_out &
           state->is_bottomed_out)
    // Key release is prioritized over release from bottom out.
    event_type = AK_EVENT_TYPE_RELEASE_FROM_BOTTOM_OUT;
  state->is_bottomed_out = is_bottomed_out;

  if (event_type == AK_EVENT_TYPE_HOLD)
    // Nothing to do for hold events
    return;

  // Disable Rapid Trigger when the key is bound with Dynamic Keystroke
  matrix_disable_rapid_trigger(event->key, event_type != AK_EVENT_TYPE_RELEASE);
  for (uint32_t i = 0; i < NUM_DYNAMIC_KEYSTROKE_MAX_BINDINGS; i++) {
    const uint8_t keycode = dks->keycodes[i];
    // We arrange the event types so that we can use the event type as an index
    // to the bitmap.
    const uint8_t action =
        (dks->bitmap[i] >> ((event_type - AK_EVENT_TYPE_PRESS) * 2)) & 3;

    if (keycode == KC_NO || action == DKS_ACTION_HOLD)
      continue;

    if (state->is_pressed[i]) {
      // All actions except for `DKS_ACTION_HOLD` require the key to be
      // unregistered first if it was registered.
      layout_unregister(event->key, keycode);
      state->is_pressed[i] = false;
    }

    if ((action == DKS_ACTION_PRESS) | (action == DKS_ACTION_TAP)) {
      // The report may have been modified in the previous step so we defer
      // the actual DKS action to the next matrix scan.
      deferred_action = (deferred_action_t){
          .type = action == DKS_ACTION_PRESS ? DEFERRED_ACTION_TYPE_PRESS
                                             : DEFERRED_ACTION_TYPE_TAP,
          .key = event->key,
          .keycode = keycode,
      };
      state->is_pressed[i] = (deferred_action_push(&deferred_action) &
                              (action == DKS_ACTION_PRESS));
    }
  }
}

static void advanced_key_tap_hold_reset_state(ak_state_tap_hold_t *state) {
  state->since = 0;
  state->stage = TAP_HOLD_STAGE_NONE;
}

static void advanced_key_tap_hold(const advanced_key_event_t *event) {
  static deferred_action_t deferred_action = {0};

  const tap_hold_t *tap_hold =
      &CURRENT_PROFILE.advanced_keys[event->ak_index].tap_hold;
  ak_state_tap_hold_t *state = &ak_states[event->ak_index].tap_hold;

  switch (event->type) {
  case AK_EVENT_TYPE_PRESS:
    state->since = timer_read();
    state->stage = TAP_HOLD_STAGE_TAP;
    break;

  case AK_EVENT_TYPE_RELEASE:
    if (state->stage == TAP_HOLD_STAGE_TAP) {
      deferred_action = (deferred_action_t){
          .type = DEFERRED_ACTION_TYPE_RELEASE,
          .key = event->key,
          .keycode = tap_hold->tap_keycode,
      };
      if (deferred_action_push(&deferred_action))
        // We only perform the tap action if the release action was
        // successfully.
        layout_register(event->key, tap_hold->tap_keycode);
    } else if (state->stage == TAP_HOLD_STAGE_HOLD)
      layout_unregister(event->key, tap_hold->hold_keycode);
    state->stage = TAP_HOLD_STAGE_NONE;
    break;

  default:
    break;
  }
}

static void advanced_key_toggle_reset_state(ak_state_toggle_t *state) {
  state->since = 0;
  state->stage = TOGGLE_STAGE_NONE;
  state->is_toggled = false;
}

static void advanced_key_toggle(const advanced_key_event_t *event) {
  const toggle_t *toggle =
      &CURRENT_PROFILE.advanced_keys[event->ak_index].toggle;
  ak_state_toggle_t *state = &ak_states[event->ak_index].toggle;

  switch (event->type) {
  case AK_EVENT_TYPE_PRESS:
    layout_register(event->key, toggle->keycode);
    state->is_toggled = !state->is_toggled;
    if (state->is_toggled) {
      state->since = timer_read();
      state->stage = TOGGLE_STAGE_TOGGLE;
    } else
      // If the key is toggled off, we use the normal key behavior.
      state->stage = TOGGLE_STAGE_NORMAL;
    break;

  case AK_EVENT_TYPE_RELEASE:
    if (!state->is_toggled)
      layout_unregister(event->key, toggle->keycode);
    state->stage = TOGGLE_STAGE_NONE;
    break;

  default:
    break;
  }
}

static void advanced_key_macro_reset_state(ak_state_macro_t *state) {
  state->since = 0;
  state->deferred_tap_ticks = 0;
  state->current_node = MACRO_NODE_NONE;
  state->num_active_keycodes = 0;
}

static void advanced_key_macro_add_active_keycode(ak_state_macro_t *state,
                                                  uint8_t keycode) {
  if (keycode == KC_NO ||
      state->num_active_keycodes >= MAX_MACRO_ACTIVE_KEYCODES)
    return;

  for (uint32_t i = 0; i < state->num_active_keycodes; i++) {
    if (state->active_keycodes[i] == keycode)
      // Keycode is already active
      return;
  }

  state->active_keycodes[state->num_active_keycodes] = keycode;
  state->num_active_keycodes++;
}

static void advanced_key_macro_remove_active_keycode(ak_state_macro_t *state,
                                                     uint8_t keycode) {
  for (uint32_t i = 0; i < state->num_active_keycodes; i++) {
    if (state->active_keycodes[i] == keycode) {
      memmove(state->active_keycodes + i, state->active_keycodes + i + 1,
              sizeof(uint8_t) * (state->num_active_keycodes - i - 1));
      state->num_active_keycodes--;
      break;
    }
  }
}

static void advanced_key_macro_stop(ak_state_macro_t *state, uint8_t key) {
  for (uint32_t i = 0; i < state->num_active_keycodes; i++)
    layout_unregister(key, state->active_keycodes[i]);

  advanced_key_macro_reset_state(state);
}

static void advanced_key_macro_run_step(ak_state_macro_t *state, uint8_t key,
                                        macro_node_id_t node_id) {
  if (node_id == MACRO_NODE_NONE || node_id >= NUM_MACRO_NODES) {
    state->current_node = MACRO_NODE_NONE;
    return;
  }

  const macro_node_t *node = &CURRENT_PROFILE.macros[node_id];
  state->current_node = node_id;
  state->since = timer_read();

  switch (node->action) {
  case MACRO_ACTION_PRESS:
    layout_register(key, node->keycode);
    advanced_key_macro_add_active_keycode(state, node->keycode);
    break;

  case MACRO_ACTION_TAP:
    layout_register(key, node->keycode);
    advanced_key_macro_add_active_keycode(state, node->keycode);
    state->deferred_tap_ticks = CURRENT_PROFILE.tick_rate;
    break;

  case MACRO_ACTION_RELEASE:
    layout_unregister(key, node->keycode);
    advanced_key_macro_remove_active_keycode(state, node->keycode);
    break;

  default:
    break;
  }
}

/**
 * @brief Advance a running macro by one tick
 *
 * @param state Macro state
 * @param key Key the macro keycodes are registered on
 *
 * @return None
 */
static void advanced_key_macro_advance(ak_state_macro_t *state, uint8_t key) {
  if (state->current_node == MACRO_NODE_NONE ||
      state->current_node >= NUM_MACRO_NODES)
    return;

  const macro_node_t *current_node = &CURRENT_PROFILE.macros[state->current_node];

  bool delay_elapsed = timer_elapsed(state->since) >= current_node->delay;
  switch (current_node->action) {
  case MACRO_ACTION_TAP:
    // Run the next macro step in a different matrix scan than the one that
    // unregistered the key in the case of consecutive taps of the same keycode.
    if (state->deferred_tap_ticks > 0) {
      state->deferred_tap_ticks--;
      if (state->deferred_tap_ticks == 0) {
        layout_unregister(key, current_node->keycode);
        advanced_key_macro_remove_active_keycode(state, current_node->keycode);
      }
    } else if (state->deferred_tap_ticks == 0 && delay_elapsed) {
      // Unlike other actions, we wait for both the delay and the deferred tap
      // ticks to elapse before running the next step.
      advanced_key_macro_run_step(state, key, current_node->next);
    }
    break;

  case MACRO_ACTION_PRESS:
  case MACRO_ACTION_RELEASE:
    if (delay_elapsed)
      advanced_key_macro_run_step(state, key, current_node->next);
    break;

  default:
    break;
  }
}

static void advanced_key_macro(const advanced_key_event_t *event) {
  const macro_t *macro = &CURRENT_PROFILE.advanced_keys[event->ak_index].macro;
  ak_state_macro_t *state = &ak_states[event->ak_index].macro;

  switch (event->type) {
  case AK_EVENT_TYPE_PRESS:
    advanced_key_macro_stop(state, event->key);
    advanced_key_macro_run_step(state, event->key, macro->head);
    break;

  case AK_EVENT_TYPE_RELEASE:
    advanced_key_macro_stop(state, event->key);
    break;

  default:
    break;
  }
}

static void advanced_key_tap_dance_reset_state(ak_state_tap_dance_t *state) {
  advanced_key_macro_reset_state(&state->macro);
  state->since = state->restart_since = 0;
  state->stage = TAP_DANCE_STAGE_NONE;
  state->count = 0;
  state->is_pressed = state->restart = state->restart_pressed = false;
  state->first = state->last = MACRO_NODE_NONE;
  state->first_left = 0;
  state->on_last = false;
  state->release_ticks = 0;
}

/**
 * @brief Finish a Tap Dance run, or start the dance pressed meanwhile
 *
 * @param state Tap Dance state
 *
 * @return None
 */
static void advanced_key_tap_dance_done(ak_state_tap_dance_t *state) {
  const bool restart = state->restart;
  const uint32_t since = state->restart_since;
  const bool is_pressed = state->restart_pressed;

  advanced_key_tap_dance_reset_state(state);
  if (restart) {
    state->stage = TAP_DANCE_STAGE_PENDING;
    state->count = 1;
    state->since = since;
    state->is_pressed = is_pressed;
  }
}

/**
 * @brief Start the next queued action of a Tap Dance, or finish it
 *
 * @param key Key index of the Tap Dance
 * @param state Tap Dance state
 *
 * @return None
 */
static void advanced_key_tap_dance_next(uint8_t key,
                                        ak_state_tap_dance_t *state) {
  if (state->first_left > 0) {
    state->first_left--;
    advanced_key_macro_run_step(&state->macro, key, state->first);
  } else if (!state->on_last && state->last != MACRO_NODE_NONE) {
    state->on_last = true;
    advanced_key_macro_run_step(&state->macro, key, state->last);
  } else
    advanced_key_tap_dance_done(state);
}

/**
 * @brief Decide what a Tap Dance does and start doing it
 *
 * Same decisions as Vial's tap dance (`vial_tap_dance_finished`), which is what
 * the stock MAD60 HE firmware runs:
 * - 1 tap: released or interrupted -> tap; held -> hold, else tap
 * - 2 taps: interrupted -> tap twice (the second held); released -> double tap,
 *   else tap twice; held -> tap-hold, else tap then hold (or the one that is set)
 * - 3 or more taps: tap that many times
 *
 * @param ak Advanced key configuration
 * @param state Tap Dance state
 * @param interrupted Whether another key was pressed during the dance
 *
 * @return None
 */
static void advanced_key_tap_dance_finish(const advanced_key_t *ak,
                                          ak_state_tap_dance_t *state,
                                          bool interrupted) {
  const macro_node_id_t *actions = ak->tap_dance.actions;
  const macro_node_id_t tap = actions[TD_ACTION_TAP];
  const macro_node_id_t hold = actions[TD_ACTION_HOLD];
  const macro_node_id_t double_tap = actions[TD_ACTION_DOUBLE_TAP];
  const macro_node_id_t tap_hold = actions[TD_ACTION_TAP_HOLD];

  state->first = state->last = MACRO_NODE_NONE;
  state->first_left = 0;

  if (state->count == 1) {
    if (interrupted || !state->is_pressed || hold == MACRO_NODE_NONE)
      state->last = tap;
    else
      state->last = hold;
  } else if (state->count == 2) {
    if (interrupted) {
      state->first = state->last = tap;
      state->first_left = 1;
    } else if (!state->is_pressed) {
      if (double_tap != MACRO_NODE_NONE)
        state->last = double_tap;
      else {
        state->first = state->last = tap;
        state->first_left = 1;
      }
    } else if (tap_hold != MACRO_NODE_NONE)
      state->last = tap_hold;
    else if (tap == MACRO_NODE_NONE)
      state->last = hold;
    else {
      state->first = tap;
      state->first_left = 1;
      state->last = hold != MACRO_NODE_NONE ? hold : tap;
    }
  } else {
    state->first = tap;
    state->first_left = state->count;
  }

  state->stage = TAP_DANCE_STAGE_RUNNING;
  state->on_last = false;
  state->release_ticks = 0;
  advanced_key_tap_dance_next(ak->key, state);
}

static void advanced_key_tap_dance(const advanced_key_event_t *event) {
  ak_state_tap_dance_t *state = &ak_states[event->ak_index].tap_dance;

  switch (event->type) {
  case AK_EVENT_TYPE_PRESS:
    // Pressing a Tap Dance key interrupts every other undecided dance
    for (uint32_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
      if (i != event->ak_index &&
          CURRENT_PROFILE.advanced_keys[i].type == AK_TYPE_TAP_DANCE &&
          ak_states[i].tap_dance.stage == TAP_DANCE_STAGE_PENDING)
        advanced_key_tap_dance_finish(&CURRENT_PROFILE.advanced_keys[i],
                                      &ak_states[i].tap_dance, true);
    }

    switch (state->stage) {
    case TAP_DANCE_STAGE_NONE:
      state->stage = TAP_DANCE_STAGE_PENDING;
      state->count = 1;
      state->since = timer_read();
      state->is_pressed = true;
      break;

    case TAP_DANCE_STAGE_PENDING:
      if (state->count < UINT8_MAX)
        state->count++;
      state->since = timer_read();
      state->is_pressed = true;
      break;

    default:
      // The previous dance is still sending its action; start a new one after
      state->restart = true;
      state->restart_since = timer_read();
      state->restart_pressed = true;
      break;
    }
    break;

  case AK_EVENT_TYPE_RELEASE:
    if (state->restart) {
      state->restart_pressed = false;
    } else if (state->stage == TAP_DANCE_STAGE_HELD) {
      advanced_key_macro_stop(&state->macro, event->key);
      advanced_key_tap_dance_done(state);
    } else
      // A running action releases its keys when it finishes
      state->is_pressed = false;
    break;

  default:
    break;
  }
}

/**
 * @brief Advance a Tap Dance by one tick
 *
 * @param ak Advanced key configuration
 * @param state Tap Dance state
 *
 * @return None
 */
static void advanced_key_tap_dance_tick(const advanced_key_t *ak,
                                        ak_state_tap_dance_t *state) {
  switch (state->stage) {
  case TAP_DANCE_STAGE_PENDING:
    if (timer_elapsed(state->since) >= ak->tap_dance.tapping_term)
      advanced_key_tap_dance_finish(ak, state, false);
    break;

  case TAP_DANCE_STAGE_RUNNING:
    if (state->release_ticks > 0) {
      // Keep the keys of a finished run pressed for a while so that the host
      // sees them, then release them. The next run starts on the next tick, so
      // a repeated key is never released and pressed in the same report.
      if (--state->release_ticks == 0)
        advanced_key_macro_stop(&state->macro, ak->key);
      break;
    }

    advanced_key_macro_advance(&state->macro, ak->key);
    if (state->macro.current_node != MACRO_NODE_NONE)
      // The current run is still going
      break;

    if (state->on_last && state->is_pressed) {
      // The last action keeps its keys pressed until the key is released
      state->stage = TAP_DANCE_STAGE_HELD;
      break;
    }
    if (state->macro.num_active_keycodes > 0) {
      state->release_ticks = M_MAX(CURRENT_PROFILE.tick_rate, 1);
      break;
    }
    advanced_key_tap_dance_next(ak->key, state);
    break;

  default:
    break;
  }
}

bool advanced_key_tap_dance_busy(void) {
  for (uint32_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
    if (CURRENT_PROFILE.advanced_keys[i].type == AK_TYPE_TAP_DANCE &&
        (ak_states[i].tap_dance.stage == TAP_DANCE_STAGE_PENDING ||
         ak_states[i].tap_dance.stage == TAP_DANCE_STAGE_RUNNING))
      return true;
  }
  return false;
}

void advanced_key_tap_dance_interrupt(void) {
  for (uint32_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
    if (CURRENT_PROFILE.advanced_keys[i].type == AK_TYPE_TAP_DANCE &&
        ak_states[i].tap_dance.stage == TAP_DANCE_STAGE_PENDING)
      advanced_key_tap_dance_finish(&CURRENT_PROFILE.advanced_keys[i],
                                    &ak_states[i].tap_dance, true);
  }
}

static void advanced_key_reset_states(void) {
  for (uint32_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
    const advanced_key_t *ak = &CURRENT_PROFILE.advanced_keys[i];
    advanced_key_state_t *state = &ak_states[i];

    switch (ak->type) {
    case AK_TYPE_NULL_BIND:
      advanced_key_null_bind_reset_state(&state->null_bind);
      break;

    case AK_TYPE_DYNAMIC_KEYSTROKE:
      advanced_key_dynamic_keystroke_reset_state(&state->dynamic_keystroke);
      break;

    case AK_TYPE_TAP_HOLD:
      advanced_key_tap_hold_reset_state(&state->tap_hold);
      break;

    case AK_TYPE_TOGGLE:
      advanced_key_toggle_reset_state(&state->toggle);
      break;

    case AK_TYPE_MACRO:
      advanced_key_macro_reset_state(&state->macro);
      break;

    case AK_TYPE_TAP_DANCE:
      advanced_key_tap_dance_reset_state(&state->tap_dance);
      break;

    default:
      break;
    }
  }
}

void advanced_key_init(void) { advanced_key_reset_states(); }

void advanced_key_clear(void) {
  // Release any keys that are currently pressed
  for (uint32_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
    const advanced_key_t *ak = &CURRENT_PROFILE.advanced_keys[i];
    advanced_key_state_t *state = &ak_states[i];

    switch (ak->type) {
    case AK_TYPE_TAP_HOLD:
      if (state->tap_hold.stage == TAP_HOLD_STAGE_HOLD)
        layout_unregister(ak->key, ak->tap_hold.hold_keycode);
      break;

    case AK_TYPE_TOGGLE:
      if (state->toggle.stage != TOGGLE_STAGE_NONE || state->toggle.is_toggled)
        layout_unregister(ak->key, ak->toggle.keycode);
      break;

    case AK_TYPE_MACRO:
      advanced_key_macro_stop(&state->macro, ak->key);
      break;

    case AK_TYPE_TAP_DANCE:
      advanced_key_macro_stop(&state->tap_dance.macro, ak->key);
      break;

    default:
      break;
    }
  }

  // Clear the advanced key states
  advanced_key_reset_states();
}

void advanced_key_process(const advanced_key_event_t *event) {
  if (event->ak_index >= NUM_ADVANCED_KEYS)
    return;

  switch (CURRENT_PROFILE.advanced_keys[event->ak_index].type) {
  case AK_TYPE_NULL_BIND:
    advanced_key_null_bind(event);
    break;

  case AK_TYPE_DYNAMIC_KEYSTROKE:
    advanced_key_dynamic_keystroke(event);
    break;

  case AK_TYPE_TAP_HOLD:
    advanced_key_tap_hold(event);
    break;

  case AK_TYPE_TOGGLE:
    advanced_key_toggle(event);
    break;

  case AK_TYPE_MACRO:
    advanced_key_macro(event);
    break;

  case AK_TYPE_TAP_DANCE:
    advanced_key_tap_dance(event);
    break;

  default:
    break;
  }
}

void advanced_key_tick(bool has_non_tap_hold_press) {
  for (uint32_t i = 0; i < NUM_ADVANCED_KEYS; i++) {
    const advanced_key_t *ak = &CURRENT_PROFILE.advanced_keys[i];
    advanced_key_state_t *state = &ak_states[i];

    switch (ak->type) {
    case AK_TYPE_TAP_HOLD:
      if (state->tap_hold.stage == TAP_HOLD_STAGE_TAP &&
          // If hold on other key press is enabled, immediately register the
          // hold key when another non-Tap-Hold key is pressed.
          ((has_non_tap_hold_press & ak->tap_hold.hold_on_other_key_press) ||
           // Otherwise, the key must be held for the tapping term.
           timer_elapsed(state->tap_hold.since) >= ak->tap_hold.tapping_term)) {
        layout_register(ak->key, ak->tap_hold.hold_keycode);
        state->tap_hold.stage = TAP_HOLD_STAGE_HOLD;
      }
      break;

    case AK_TYPE_TOGGLE:
      if (state->toggle.stage == TOGGLE_STAGE_TOGGLE &&
          timer_elapsed(state->toggle.since) >= ak->toggle.tapping_term) {
        // If the key is held for more than the tapping term, switch to the
        // normal key behavior.
        state->toggle.stage = TOGGLE_STAGE_NORMAL;
        // Always toggle the key off when in normal behavior
        state->toggle.is_toggled = false;
      }
      break;

    case AK_TYPE_MACRO:
      advanced_key_macro_advance(&state->macro, ak->key);
      break;

    case AK_TYPE_TAP_DANCE:
      advanced_key_tap_dance_tick(ak, &state->tap_dance);
      break;

    default:
      break;
    }
  }
}
