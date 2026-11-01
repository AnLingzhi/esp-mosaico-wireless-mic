// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { KEY_ACTION_NONE, KEY_ACTION_ENTER, KEY_ACTION_CLEAR, KEY_ACTION_SETUP_Z, KEY_ACTION_SETUP_SLASH } key_action_t;
typedef struct {
    key_action_t action;
    unsigned phase;
    uint32_t accepted_at;
    bool accepted;
} key_sequence_t;

bool key_sequence_start(key_sequence_t *state, key_action_t action);
/* Time begins only after the current report was accepted by the transport. */
void key_sequence_tick(key_sequence_t *state, uint32_t now);
void key_sequence_report(const key_sequence_t *state, uint8_t *modifiers, uint8_t keys[6]);
void key_sequence_accepted(key_sequence_t *state, uint32_t now);
