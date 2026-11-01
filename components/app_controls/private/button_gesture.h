// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define APP_BUTTON_HOLD_MS 250U
#define APP_BUTTON_DOUBLE_MS 300U

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
