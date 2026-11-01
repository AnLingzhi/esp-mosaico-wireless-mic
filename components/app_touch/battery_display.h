// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Start once after the process-lifetime label exists. No stop or deletion allowed. */
esp_err_t app_battery_display_start(lv_obj_t *label);
