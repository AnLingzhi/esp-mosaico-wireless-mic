// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdbool.h>
typedef struct {
    bool connected;
    bool streaming;
} app_hfp_status_t;
/* Thread-safe service state, not a claim that a Mac application is recording. */
app_hfp_status_t app_hfp_status(void);
/* Start once after app_audio_start(); wait up to ten seconds for HF registration. */
esp_err_t app_hfp_start(void);
