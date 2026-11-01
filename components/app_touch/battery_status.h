// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;
    bool charging;
    unsigned percent;
} app_battery_status_t;

/* Same BATTPRES/DSG validity rules as the retained system demo. */
static inline app_battery_status_t app_battery_decode(bool read_ok, int16_t voltage_mv,
                                                      uint16_t flags, int16_t current_ma,
                                                      uint16_t percent)
{
    const bool valid = read_ok && voltage_mv > 0 && percent <= 100 && (flags & (1U << 3));
    return (app_battery_status_t) {
        .valid = valid,
        .charging = valid && !(flags & 1U) && current_ma > 0,
        .percent = valid ? percent : 0,
    };
}
