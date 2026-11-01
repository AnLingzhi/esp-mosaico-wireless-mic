// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include "battery_status.h"

int main(void)
{
    for (unsigned percent = 0; percent <= 100; ++percent) {
        app_battery_status_t s = app_battery_decode(true, 3800, 8, 10, percent);
        assert(s.valid && s.percent == percent && s.charging);
        s = app_battery_decode(true, 3800, 9, 10, percent);
        assert(s.valid && !s.charging);
    }
    assert(!app_battery_decode(false, 3800, 8, 10, 50).valid);
    assert(!app_battery_decode(true, 3800, 0, 10, 50).valid);
    assert(!app_battery_decode(true, 0, 8, 10, 50).valid);
    assert(!app_battery_decode(true, -1, 8, 10, 50).valid);
    assert(!app_battery_decode(true, 3800, 8, 10, 101).valid);
    assert(!app_battery_decode(true, 3800, 8, 10, 65535).valid);
    assert(!app_battery_decode(true, 3800, 8, -1, 50).charging);
    assert(!app_battery_decode(true, 3800, 8, 0, 100).charging);
    return 0;
}
