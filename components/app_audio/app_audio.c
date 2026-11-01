// SPDX-License-Identifier: Apache-2.0

#include "app_audio.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_audio_fifo.h"
#include "bsp/audio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define APP_AUDIO_LOCAL_RATE_HZ       16000U
#define APP_AUDIO_BITS_PER_SAMPLE     16U
#define APP_AUDIO_BYTES_PER_SAMPLE    2U
#define APP_AUDIO_CHUNKS_PER_SECOND   100U
#define APP_AUDIO_CAPTURE_BUFFER_SIZE \
    (APP_AUDIO_LOCAL_RATE_HZ * APP_AUDIO_BYTES_PER_SAMPLE / APP_AUDIO_CHUNKS_PER_SECOND)
#define APP_AUDIO_FIFO_SIZE           1280U
#define APP_AUDIO_TASK_STACK_SIZE     4096U
#define APP_AUDIO_TASK_PRIORITY       5U
#define APP_AUDIO_MIC_GAIN_DB         18.0f
#define APP_AUDIO_START_TIMEOUT_MS    8000U
#define APP_AUDIO_REOPEN_RETRY_MS     500U
#define APP_AUDIO_READ_RETRY_MS       100U
#define APP_AUDIO_REOPEN_LOG_PERIOD   10U
#define APP_AUDIO_READ_LOG_PERIOD     50U
#define APP_AUDIO_STATS_PERIOD_MS     5000U

static const char *TAG = "app_audio";

typedef enum {
    APP_AUDIO_STOPPED,
    APP_AUDIO_STARTING,
    APP_AUDIO_RUNNING,
    APP_AUDIO_FAILED,
} app_audio_state_t;

typedef struct {
    portMUX_TYPE lock;
    app_audio_state_t state;
    uint8_t *capture_buffer;
    uint8_t *fifo_storage;
    app_audio_fifo_t fifo;
    app_audio_stats_t stats;
    uint32_t requested_rate;
    SemaphoreHandle_t startup_signal;
    esp_err_t startup_result;
} app_audio_context_t;

static app_audio_context_t s_audio = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static bool is_valid_stream_rate(uint32_t rate)
{
    return rate == 0 || rate == 8000 || rate == 16000;
}

static uint32_t effective_capture_rate(uint32_t stream_rate)
{
    return stream_rate == 8000 ? 8000 : APP_AUDIO_LOCAL_RATE_HZ;
}

static void record_capture_stats(const int16_t *samples, size_t bytes, bool read_ok)
{
    uint16_t peak = 0;
    uint32_t clipping = 0;
    if (read_ok) {
        const size_t count = bytes / sizeof(*samples);
        for (size_t i = 0; i < count; ++i) {
            const int32_t sample = samples[i];
            const uint16_t magnitude = sample == INT16_MIN ? 32768U : (uint16_t)abs(sample);
            if (magnitude > peak) {
                peak = magnitude;
            }
            if (sample == INT16_MIN || sample == INT16_MAX) {
                clipping++;
            }
        }
    }

    portENTER_CRITICAL(&s_audio.lock);
    if (read_ok) {
        s_audio.stats.captured_bytes += bytes;
        s_audio.stats.clipping_samples += clipping;
        if (peak > s_audio.stats.peak) {
            s_audio.stats.peak = peak;
        }
    } else {
        s_audio.stats.read_errors++;
    }
    portEXIT_CRITICAL(&s_audio.lock);
}

static bool snapshot_request(uint32_t *rate, uint32_t *generation)
{
    portENTER_CRITICAL(&s_audio.lock);
    *rate = s_audio.requested_rate;
    *generation = s_audio.fifo.generation;
    const bool active = s_audio.state == APP_AUDIO_STARTING ||
                        s_audio.state == APP_AUDIO_RUNNING;
    portEXIT_CRITICAL(&s_audio.lock);
    return active;
}

static bool open_microphone(esp_codec_dev_handle_t microphone, uint32_t sample_rate)
{
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate,
        .bits_per_sample = APP_AUDIO_BITS_PER_SAMPLE,
        .channel = 1,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
    };
    if (esp_codec_dev_open(microphone, &sample_info) != ESP_CODEC_DEV_OK) {
        return false;
    }
    if (esp_codec_dev_set_in_gain(microphone, APP_AUDIO_MIC_GAIN_DB) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone gain setup failed; retrying codec initialization");
        (void)esp_codec_dev_close(microphone);
        return false;
    }
    ESP_LOGI(TAG, "Microphone input gain: %.0f dB", (double)APP_AUDIO_MIC_GAIN_DB);
    return true;
}

static void report_startup(esp_err_t result)
{
    portENTER_CRITICAL(&s_audio.lock);
    s_audio.startup_result = result;
    s_audio.state = result == ESP_OK ? APP_AUDIO_RUNNING : APP_AUDIO_FAILED;
    portEXIT_CRITICAL(&s_audio.lock);
    xSemaphoreGive(s_audio.startup_signal);
}

static void log_stats(void)
{
    app_audio_stats_t stats;
    app_audio_get_stats(&stats);
    ESP_LOGI(TAG,
             "stats captured=%" PRIu64 " read_errors=%" PRIu32
             " peak=%u clipping=%" PRIu64 " rate=%" PRIu32
             " sent=%" PRIu64 " underflow=%" PRIu64 " overflow=%" PRIu64,
             stats.captured_bytes, stats.read_errors, stats.peak,
             stats.clipping_samples, stats.sample_rate, stats.sent_bytes,
             stats.underflow_bytes, stats.overflow_bytes);
}

static void capture_task(void *argument)
{
    (void)argument;
    esp_codec_dev_handle_t microphone = bsp_audio_codec_microphone_init();
    if (!microphone) {
        ESP_LOGE(TAG, "ES8311 microphone initialization failed; Recovery can continue");
        report_startup(ESP_FAIL);
        vTaskDelete(NULL);
        return;
    }

    uint32_t opened_rate = 0;
    bool codec_open = false;
    bool startup_reported = false;
    unsigned reopen_failures = 0;
    unsigned read_failures = 0;
    TickType_t last_stats_tick = xTaskGetTickCount();

    while (true) {
        const TickType_t loop_tick = xTaskGetTickCount();
        if (loop_tick - last_stats_tick >= pdMS_TO_TICKS(APP_AUDIO_STATS_PERIOD_MS)) {
            log_stats();
            last_stats_tick = loop_tick;
        }

        uint32_t requested_rate;
        uint32_t generation;
        if (!snapshot_request(&requested_rate, &generation)) {
            break;
        }
        const uint32_t capture_rate = effective_capture_rate(requested_rate);
        if (!codec_open || capture_rate != opened_rate) {
            if (codec_open) {
                (void)esp_codec_dev_close(microphone);
                codec_open = false;
            }
            if (!open_microphone(microphone, capture_rate)) {
                reopen_failures++;
                portENTER_CRITICAL(&s_audio.lock);
                s_audio.stats.read_errors++;
                portEXIT_CRITICAL(&s_audio.lock);
                if (reopen_failures == 1 || reopen_failures % APP_AUDIO_REOPEN_LOG_PERIOD == 0) {
                    ESP_LOGE(TAG, "ES8311 open at %" PRIu32 " Hz failed (%u attempts)",
                             capture_rate, reopen_failures);
                }
                vTaskDelay(pdMS_TO_TICKS(APP_AUDIO_REOPEN_RETRY_MS));
                continue;
            }
            opened_rate = capture_rate;
            codec_open = true;
            reopen_failures = 0;
            ESP_LOGI(TAG, "ES8311 microphone capturing mono at %" PRIu32 " Hz", opened_rate);
        }

        const size_t capture_bytes = capture_rate * APP_AUDIO_BYTES_PER_SAMPLE /
                                     APP_AUDIO_CHUNKS_PER_SECOND;
        const bool read_ok = esp_codec_dev_read(microphone, s_audio.capture_buffer,
                                                (int)capture_bytes) == ESP_CODEC_DEV_OK;
        record_capture_stats((const int16_t *)s_audio.capture_buffer, capture_bytes, read_ok);
        if (!read_ok) {
            read_failures++;
            if (read_failures == 1 || read_failures % APP_AUDIO_READ_LOG_PERIOD == 0) {
                ESP_LOGE(TAG, "ES8311 read failed (%u failures); reopening codec", read_failures);
            }
            (void)esp_codec_dev_close(microphone);
            codec_open = false;
            vTaskDelay(pdMS_TO_TICKS(APP_AUDIO_READ_RETRY_MS));
            continue;
        }
        if (!startup_reported) {
            startup_reported = true;
            report_startup(ESP_OK);
        }

        portENTER_CRITICAL(&s_audio.lock);
        if (requested_rate != 0 && requested_rate == s_audio.requested_rate) {
            size_t dropped = 0;
            if (app_audio_fifo_write(&s_audio.fifo, generation, s_audio.capture_buffer,
                                     capture_bytes, &dropped)) {
                s_audio.stats.overflow_bytes += dropped;
            }
        }
        portEXIT_CRITICAL(&s_audio.lock);

    }

    if (codec_open) {
        (void)esp_codec_dev_close(microphone);
    }
    if (!startup_reported) {
        report_startup(ESP_FAIL);
    } else {
        portENTER_CRITICAL(&s_audio.lock);
        s_audio.state = APP_AUDIO_FAILED;
        portEXIT_CRITICAL(&s_audio.lock);
    }
    vTaskDelete(NULL);
}

esp_err_t app_audio_start(void)
{
    portENTER_CRITICAL(&s_audio.lock);
    if (s_audio.state == APP_AUDIO_RUNNING) {
        portEXIT_CRITICAL(&s_audio.lock);
        return ESP_OK;
    }
    if (s_audio.state == APP_AUDIO_STARTING) {
        portEXIT_CRITICAL(&s_audio.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_audio.state = APP_AUDIO_STARTING;
    portEXIT_CRITICAL(&s_audio.lock);

    if (!s_audio.capture_buffer) {
        s_audio.capture_buffer = heap_caps_malloc(APP_AUDIO_CAPTURE_BUFFER_SIZE,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_audio.fifo_storage) {
        s_audio.fifo_storage = heap_caps_malloc(APP_AUDIO_FIFO_SIZE,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_audio.startup_signal) {
        s_audio.startup_signal = xSemaphoreCreateBinary();
    }
    if (!s_audio.capture_buffer || !s_audio.fifo_storage || !s_audio.startup_signal) {
        portENTER_CRITICAL(&s_audio.lock);
        s_audio.state = APP_AUDIO_FAILED;
        portEXIT_CRITICAL(&s_audio.lock);
        ESP_LOGE(TAG, "audio buffer allocation failed; Recovery can continue");
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_audio.lock);
    app_audio_fifo_init(&s_audio.fifo, s_audio.fifo_storage, APP_AUDIO_FIFO_SIZE);
    s_audio.stats.sample_rate = s_audio.requested_rate;
    s_audio.startup_result = ESP_ERR_TIMEOUT;
    portEXIT_CRITICAL(&s_audio.lock);

    while (xSemaphoreTake(s_audio.startup_signal, 0) == pdTRUE) {
    }

    if (xTaskCreate(capture_task, "app_audio_capture", APP_AUDIO_TASK_STACK_SIZE, NULL,
                    APP_AUDIO_TASK_PRIORITY, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_audio.lock);
        s_audio.state = APP_AUDIO_FAILED;
        portEXIT_CRITICAL(&s_audio.lock);
        ESP_LOGE(TAG, "capture task creation failed; Recovery can continue");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_audio.startup_signal,
                       pdMS_TO_TICKS(APP_AUDIO_START_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "microphone startup timed out after %u ms; task will keep retrying",
                 APP_AUDIO_START_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_audio.lock);
    const esp_err_t startup_result = s_audio.startup_result;
    portEXIT_CRITICAL(&s_audio.lock);
    return startup_result;
}

void app_audio_set_stream_rate(uint32_t sample_rate)
{
    if (!is_valid_stream_rate(sample_rate)) {
        return;
    }
    portENTER_CRITICAL(&s_audio.lock);
    s_audio.requested_rate = sample_rate;
    s_audio.stats.sample_rate = sample_rate;
    if (s_audio.fifo.storage) {
        (void)app_audio_fifo_reset(&s_audio.fifo);
    }
    portEXIT_CRITICAL(&s_audio.lock);
}

uint32_t app_audio_read_pcm(uint8_t *dst, uint32_t bytes)
{
    if (!dst || bytes == 0) {
        return 0;
    }
    memset(dst, 0, bytes);

    portENTER_CRITICAL(&s_audio.lock);
    size_t copied = 0;
    if (s_audio.requested_rate != 0 && s_audio.fifo.storage) {
        copied = app_audio_fifo_read(&s_audio.fifo, dst, bytes);
    }
    s_audio.stats.sent_bytes += bytes;
    s_audio.stats.underflow_bytes += bytes - copied;
    portEXIT_CRITICAL(&s_audio.lock);
    return bytes;
}

void app_audio_get_stats(app_audio_stats_t *stats)
{
    if (!stats) {
        return;
    }
    portENTER_CRITICAL(&s_audio.lock);
    *stats = s_audio.stats;
    portEXIT_CRITICAL(&s_audio.lock);
}
