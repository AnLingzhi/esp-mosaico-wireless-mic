// SPDX-License-Identifier: Apache-2.0
#include "key_sequence.h"
#include <string.h>

bool key_sequence_start(key_sequence_t *state, key_action_t action)
{
    if (state->action != KEY_ACTION_NONE || action == KEY_ACTION_NONE) {
        return false;
    }
    *state = (key_sequence_t) {
        .action = action
    };
    return true;
}

void key_sequence_tick(key_sequence_t *state, uint32_t now)
{
    if (state->action == KEY_ACTION_NONE || !state->accepted || now - state->accepted_at < 40) {
        return;
    }
    state->accepted = false;
    state->phase++;
    const unsigned phases = state->action == KEY_ACTION_CLEAR ? 4 : 2;
    if (state->phase >= phases) {
        *state = (key_sequence_t) {
            0
        };
    }
}

void key_sequence_report(const key_sequence_t *state, uint8_t *modifiers, uint8_t keys[6])
{
    *modifiers = 0;
    memset(keys, 0, 6);
    if (state->action == KEY_ACTION_ENTER && state->phase == 0) {
        keys[0] = 0x28;
    } else if (state->action == KEY_ACTION_SETUP_Z && state->phase == 0) {
        keys[0] = 0x1D;
    } else if (state->action == KEY_ACTION_SETUP_SLASH && state->phase == 0) {
        keys[0] = 0x38;
    } else if (state->action == KEY_ACTION_CLEAR) {
        if (state->phase == 0) {
            *modifiers = 0x08; /* Left GUI is Command on macOS. */
            keys[0] = 0x04;
        } else if (state->phase == 2) {
            keys[0] = 0x2A;
        }
    }
}

void key_sequence_accepted(key_sequence_t *state, uint32_t now)
{
    if (!state->accepted && state->action != KEY_ACTION_NONE) {
        state->accepted_at = now;
        state->accepted = true;
    }
}
