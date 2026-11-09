// SPDX-License-Identifier: Apache-2.0
// Modified to add debouncing shared by the onboard and expansion keys.
#include "button_gesture.h"

bool button_input_step(button_input_t *state, bool raw, uint32_t now)
{
    if (raw != state->raw) {
        state->raw = raw;
        state->changed_at = now;
    }
    if (state->down == raw || now - state->changed_at < APP_BUTTON_DEBOUNCE_MS) {
        return false;
    }
    state->down = raw;
    return raw;
}

button_action_t button_gesture_step(button_gesture_t *state, bool down, uint32_t now)
{
    button_action_t action = {0};
    if (state->pending_click && !state->down && now - state->released_at > APP_BUTTON_DOUBLE_MS) {
        state->pending_click = false;
    }
    if (down && !state->down) {
        state->second_click = state->pending_click;
        state->pending_click = false;
        state->pressed_at = now;
    }
    if (down && !state->fn && now - state->pressed_at >= APP_BUTTON_HOLD_MS) {
        state->fn = true;
        state->second_click = false;
        action.fn_down = true;
    }
    if (!down && state->down) {
        if (state->fn) {
            action.fn_up = true;
            state->fn = false;
            state->pending_click = false;
        } else if (state->second_click) {
            action.enter = true;
            state->second_click = false;
            state->pending_click = false;
        } else {
            state->pending_click = true;
            state->released_at = now;
        }
    }
    state->down = down;
    return action;
}
