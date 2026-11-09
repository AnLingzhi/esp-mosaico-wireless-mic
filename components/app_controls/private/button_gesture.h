// SPDX-License-Identifier: Apache-2.0
// Modified to expose the private debounced input state.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define APP_BUTTON_HOLD_MS 250U
#define APP_BUTTON_DOUBLE_MS 300U
#define APP_BUTTON_DEBOUNCE_MS 30U

typedef struct {
    bool raw;
    bool down;
    uint32_t changed_at;
} button_input_t;

/* Update debounced state and return true once per stable press. */
bool button_input_step(button_input_t *state, bool raw, uint32_t now);

typedef struct {
    bool down;
    bool fn;
    bool pending_click;
    bool second_click;
    uint32_t pressed_at;
    uint32_t released_at;
} button_gesture_t;

typedef struct {
    bool fn_down;
    bool fn_up;
    bool enter;
} button_action_t;

button_action_t button_gesture_step(button_gesture_t *state, bool down, uint32_t now);
