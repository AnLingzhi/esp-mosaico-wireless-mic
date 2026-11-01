// SPDX-License-Identifier: Apache-2.0
#include "button_gesture.h"
#include <assert.h>
#include <stdio.h>

static void quiet(button_action_t action)
{
    assert(!action.fn_down && !action.fn_up && !action.enter);
}

int main(void)
{
    button_gesture_t state = {0};
    quiet(button_gesture_step(&state, true, 0));
    quiet(button_gesture_step(&state, true, 249));
    assert(button_gesture_step(&state, true, 250).fn_down);
    quiet(button_gesture_step(&state, true, 3000));
    assert(button_gesture_step(&state, false, 3001).fn_up);
    quiet(button_gesture_step(&state, false, 3100));

    state = (button_gesture_t) {
        0
    };
    quiet(button_gesture_step(&state, true, 10));
    quiet(button_gesture_step(&state, false, 60));
    quiet(button_gesture_step(&state, true, 120));
    button_action_t action = button_gesture_step(&state, false, 180);
    assert(action.enter && !action.fn_down && !action.fn_up);
    quiet(button_gesture_step(&state, false, 500));

    /* Single and widely separated clicks must never send Enter or Fn. */
    state = (button_gesture_t) {
        0
    };
    quiet(button_gesture_step(&state, true, 0));
    quiet(button_gesture_step(&state, false, 40));
    quiet(button_gesture_step(&state, true, 341));
    quiet(button_gesture_step(&state, false, 380));

    /* A held second press becomes voice, not Enter on release. */
    state = (button_gesture_t) {
        0
    };
    quiet(button_gesture_step(&state, true, 0));
    quiet(button_gesture_step(&state, false, 40));
    quiet(button_gesture_step(&state, true, 100));
    assert(button_gesture_step(&state, true, 350).fn_down);
    action = button_gesture_step(&state, false, 400);
    assert(action.fn_up && !action.enter);

    /* Unsigned elapsed time remains correct across the millisecond wrap. */
    state = (button_gesture_t) {
        0
    };
    quiet(button_gesture_step(&state, true, UINT32_MAX - 99));
    assert(button_gesture_step(&state, true, 150).fn_down);
    assert(button_gesture_step(&state, false, 151).fn_up);
    puts("gesture cases passed");
    return 0;
}
