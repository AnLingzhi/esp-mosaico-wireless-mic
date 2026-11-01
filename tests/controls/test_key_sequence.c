// SPDX-License-Identifier: Apache-2.0
#include "key_sequence.h"
#include <assert.h>
#include <stdio.h>

static void expect(key_sequence_t *s, unsigned modifier, unsigned key)
{
    uint8_t m, keys[6];
    key_sequence_report(s, &m, keys);
    assert(m == modifier && keys[0] == key);
    for (unsigned i = 1; i < 6; i++) {
        assert(keys[i] == 0);
    }
}
int main(void)
{
    key_sequence_t s = {0};
    assert(key_sequence_start(&s, KEY_ACTION_CLEAR));
    assert(!key_sequence_start(&s, KEY_ACTION_ENTER));
    expect(&s, 8, 4);
    key_sequence_tick(&s, 1000); // Failed/unaccepted transport must not advance.
    expect(&s, 8, 4);
    key_sequence_accepted(&s, 1000);
    key_sequence_tick(&s, 1039); expect(&s, 8, 4);
    key_sequence_tick(&s, 1040); expect(&s, 0, 0);
    key_sequence_accepted(&s, 1040);
    key_sequence_tick(&s, 1080); expect(&s, 0, 0x2a);
    key_sequence_accepted(&s, 1080);
    key_sequence_tick(&s, 1120); expect(&s, 0, 0);
    key_sequence_accepted(&s, 1120);
    key_sequence_tick(&s, 1160); assert(s.action == KEY_ACTION_NONE);
    assert(key_sequence_start(&s, KEY_ACTION_ENTER));
    expect(&s, 0, 0x28);
    key_sequence_accepted(&s, UINT32_MAX - 19);
    key_sequence_tick(&s, 20); expect(&s, 0, 0);
    key_sequence_accepted(&s, 20);
    key_sequence_tick(&s, 60); assert(s.action == KEY_ACTION_NONE);
    assert(key_sequence_start(&s, KEY_ACTION_CLEAR));
    s = (key_sequence_t) {
        0
    }; // Link loss cancels instead of replaying deletion.
    expect(&s, 0, 0);
    assert(key_sequence_start(&s, KEY_ACTION_SETUP_Z));
    expect(&s, 0, 0x1d);
    key_sequence_accepted(&s, 0);
    key_sequence_tick(&s, 40); expect(&s, 0, 0);
    key_sequence_accepted(&s, 40);
    key_sequence_tick(&s, 80); assert(s.action == KEY_ACTION_NONE);
    assert(key_sequence_start(&s, KEY_ACTION_SETUP_SLASH));
    expect(&s, 0, 0x38);
    key_sequence_accepted(&s, 100);
    key_sequence_tick(&s, 140); expect(&s, 0, 0);
    key_sequence_accepted(&s, 140);
    key_sequence_tick(&s, 180); assert(s.action == KEY_ACTION_NONE);
    puts("key sequences passed");
}
