// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t captured_bytes;
    uint64_t sent_bytes;
    uint64_t overflow_bytes;
    uint64_t underflow_bytes;
    uint64_t clipping_samples;
    uint32_t read_errors;
    uint16_t peak;
    uint32_t sample_rate;
} app_audio_stats_t;

/** Start the permanent microphone capture task; safe to call again after success. */
esp_err_t app_audio_start(void);

/** Select 0, 8000, or 16000 Hz and discard audio queued for the old stream. */
void app_audio_set_stream_rate(uint32_t sample_rate);

/** Fill dst without blocking; unavailable bytes are zero-filled. */
uint32_t app_audio_read_pcm(uint8_t *dst, uint32_t bytes);

/** Copy a cross-core-safe statistics snapshot; NULL is ignored. */
void app_audio_get_stats(app_audio_stats_t *stats);

#ifdef __cplusplus
}
#endif
