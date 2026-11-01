#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the board display and show the microphone touch UI.
 *
 * @param callback Optional callback for Talk (0), Enter (1), Clear (3), and
 *                 setup-page Z (4) or slash (5). Talk reports down/up; click
 *                 keys report only `(index, true)` on a completed click.
 * @return ESP_OK, ESP_ERR_INVALID_STATE if already started, ESP_ERR_TIMEOUT if
 *         the LVGL lock cannot be acquired, or ESP_FAIL if display startup fails.
 */
esp_err_t app_touch_start(void (*callback)(unsigned index, bool pressed));

#ifdef __cplusplus
}
#endif
