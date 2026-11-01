// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Start once after app_hid_start; all HID reports are serialized in one task. */
esp_err_t app_controls_start(void);
/* 0: held voice, 1: Enter click, 2: local settings, 3: clear click, 4: setup Z click, 5: setup slash click. */
void app_controls_touch(unsigned index, bool pressed);
/* Nonblocking request for one short haptic pulse, including local UI controls. */
void app_controls_feedback(void);
