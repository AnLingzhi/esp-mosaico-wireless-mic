// SPDX-License-Identifier: Apache-2.0
#include "app_controls.h"
#include "app_hid.h"
#include "button_gesture.h"
#include "key_sequence.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define INPUT_DEBOUNCE_MS 30U
#define RECONNECT_MS 500U
#define HAPTIC_MS 35U
#define VOICE_MASK 1U

static const char *TAG = "app_controls";
static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint8_t held;
    key_action_t action;
    uint32_t generation;
    bool feedback;
} input;
static TaskHandle_t control_task;
static bool motor_ready;

void app_controls_feedback(void)
{
    portENTER_CRITICAL(&input_lock);
    input.feedback = true;
    portEXIT_CRITICAL(&input_lock);
}

void app_controls_touch(unsigned index, bool pressed)
{
    uint32_t generation;
    const bool connected = app_hid_connected(&generation);
    portENTER_CRITICAL(&input_lock);
    if (index == 0) {
        if (pressed) {
            input.held |= (uint8_t)(1U << index);
        } else {
            input.held &= (uint8_t)~(1U << index);
        }
    } else if (pressed && connected && input.action == KEY_ACTION_NONE) {
        input.action = index == 1 ? KEY_ACTION_ENTER : index == 3 ? KEY_ACTION_CLEAR :
                       index == 4 ? KEY_ACTION_SETUP_Z : index == 5 ? KEY_ACTION_SETUP_SLASH : KEY_ACTION_NONE;
        input.generation = generation;
    }
    portEXIT_CRITICAL(&input_lock);
    ESP_LOGI(TAG, "touch index=%u pressed=%d", index, pressed);
}

static void poll_controls(void *arg)
{
    (void)arg;
    button_gesture_t gesture = {0};
    key_sequence_t sequence = {0};
    bool raw_previous = false, stable = false, inhibited = true;
    bool old_fn = false, dirty = true, vibrating = false;
    uint8_t old_modifiers = 0, old_keys[6] = {0};
    uint32_t raw_changed = 0, previous_generation = 0, last_reconnect = 0, vibration_at = 0;
    for (;;) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        const bool raw = gpio_get_level(BSP_BUTTON_AI_GPIO) == BSP_BUTTON_ACTIVE_LEVEL;
        if (raw != raw_previous) {
            raw_previous = raw;
            raw_changed = now;
        }
        bool physical_press = false;
        if (now - raw_changed >= INPUT_DEBOUNCE_MS && stable != raw) {
            stable = raw;
            physical_press = stable;
        }
        portENTER_CRITICAL(&input_lock);
        const uint8_t touches = input.held;
        const key_action_t requested = input.action;
        const uint32_t request_generation = input.generation;
        const bool feedback = input.feedback || physical_press;
        input.action = KEY_ACTION_NONE;
        input.feedback = false;
        portEXIT_CRITICAL(&input_lock);
        if (motor_ready && feedback) {
            if (bsp_motor_set_strength(60) == ESP_OK) {
                vibrating = true;
                vibration_at = now;
            }
        }
        if (vibrating && now - vibration_at >= HAPTIC_MS) {
            if (bsp_motor_set(false) == ESP_OK) {
                vibrating = false;
            }
        }
        uint32_t generation;
        const bool connected = app_hid_connected(&generation);
        if (!connected || generation != previous_generation) {
            gesture = (button_gesture_t) {
                0
            };
            sequence = (key_sequence_t) {
                0
            };
            inhibited = true;
            dirty = true;
            previous_generation = generation;
        }
        if (now - last_reconnect >= RECONNECT_MS) {
            app_hid_reconnect();
            last_reconnect = now;
        }
        /* Reconnection never replays held controls or queued destructive actions. */
        if (connected && inhibited && !raw && !stable && touches == 0) {
            inhibited = false;
        }
        uint8_t keys[6] = {0}, modifiers = 0;
        bool fn = false;
        if (connected && !inhibited) {
            const button_action_t action = button_gesture_step(&gesture, stable, now);
            const bool held_fn = gesture.fn || (touches & VOICE_MASK);
            key_sequence_tick(&sequence, now);
            /* Never combine Command+A/Delete with Fn or replay a rejected click. */
            if (!held_fn) {
                if (requested != KEY_ACTION_NONE && request_generation == generation) {
                    key_sequence_start(&sequence, requested);
                } else if (action.enter) {
                    key_sequence_start(&sequence, KEY_ACTION_ENTER);
                }
            }
            if (sequence.action != KEY_ACTION_NONE) {
                key_sequence_report(&sequence, &modifiers, keys);
            } else {
                fn = held_fn;
            }
            if (action.fn_down || action.fn_up) {
                ESP_LOGI(TAG, "physical Fn=%s", action.fn_down ? "down" : "up");
            }
        }
        if (connected) {
            if (dirty || fn != old_fn || modifiers != old_modifiers || memcmp(keys, old_keys, sizeof(keys))) {
                const esp_err_t err = app_hid_set_keys(fn, modifiers, keys);
                dirty = err != ESP_OK;
                if (!dirty) {
                    old_fn = fn;
                    old_modifiers = modifiers;
                    memcpy(old_keys, keys, sizeof(keys));
                }
            }
            if (!dirty) {
                key_sequence_accepted(&sequence, now);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_controls_start(void)
{
    if (control_task) {
        return ESP_ERR_INVALID_STATE;
    }
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BSP_BUTTON_AI_GPIO,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    err = bsp_motor_init();
    motor_ready = err == ESP_OK;
    if (!motor_ready) {
        ESP_LOGW(TAG, "Haptics unavailable: %s", esp_err_to_name(err));
    }
    if (xTaskCreate(poll_controls, "mic_keys", 4096, NULL, 4, &control_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AI hold=Fn, double-click=Enter; touch voice/Enter/clear/ANSI setup enabled");
    return ESP_OK;
}
