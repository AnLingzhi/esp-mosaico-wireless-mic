// SPDX-License-Identifier: Apache-2.0
// Modified for the expansion board's two keys and the BOOT button mapping.
#include "app_controls.h"
#include "app_hid.h"
#include "button_gesture.h"
#include "key_sequence.h"
#include "bsp/esp_mosaico.h"
#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaico_module_mgr.h"
#include <string.h>

#define RECONNECT_MS 500U
#define EXPANSION_CHECK_MS 250U
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
static bool expansion_available;

typedef struct {
    mosaico_module_lease_t lease;
    gpio_num_t left_gpio;
    gpio_num_t right_gpio;
    button_input_t left;
    button_input_t right;
    bool armed;
} expansion_keys_t;

/* The control task owns the lease and GPIOs. The manager checks EEPROM in
 * the background, so polling the keys never waits for I2C. */
static void refresh_expansion_keys(expansion_keys_t *keys)
{
    if (keys->lease.id) {
        static mosaico_module_mgr_info_t info;
        if (mosaico_module_mgr_get_info(keys->lease.slot, &info) == ESP_OK &&
                info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
                info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
                info.eeprom.board_type == MOSAICO_BOARD_TYPE_INTERACT) {
            return;
        }
        gpio_reset_pin(keys->left_gpio);
        gpio_reset_pin(keys->right_gpio);
        ESP_ERROR_CHECK_WITHOUT_ABORT(mosaico_module_mgr_release(&keys->lease));
        *keys = (expansion_keys_t){0};
        return;
    }

    const mosaico_module_mgr_claim_config_t claim = {
        .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,
        .expected_type = MOSAICO_BOARD_TYPE_INTERACT,
    };
    if (mosaico_module_mgr_claim(&claim, &keys->lease) != ESP_OK) {
        return;
    }
    const bsp_subboard_slot_t slot = (bsp_subboard_slot_t)keys->lease.slot;
    keys->left_gpio = bsp_subboard_map_gpio(slot, GPIO_NUM_13);
    keys->right_gpio = bsp_subboard_map_gpio(slot, GPIO_NUM_12);
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << keys->left_gpio) | (1ULL << keys->right_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Expansion key GPIO setup failed: %s", esp_err_to_name(err));
        gpio_reset_pin(keys->left_gpio);
        gpio_reset_pin(keys->right_gpio);
        ESP_ERROR_CHECK_WITHOUT_ABORT(mosaico_module_mgr_release(&keys->lease));
        *keys = (expansion_keys_t){0};
        return;
    }
    ESP_LOGI(TAG, "Expansion keys ready: slot=%s TALK=GPIO%d ENTER=GPIO%d",
             mosaico_module_mgr_slot_to_name(keys->lease.slot), keys->left_gpio, keys->right_gpio);
}

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
    button_input_t onboard = {0};
    expansion_keys_t expansion = {0};
    key_sequence_t sequence = {0};
    bool inhibited = true;
    bool old_fn = false, dirty = true, vibrating = false;
    uint8_t old_modifiers = 0, old_keys[6] = {0};
    uint32_t previous_generation = 0, last_reconnect = 0, vibration_at = 0;
    uint32_t last_expansion_check = 0;
    for (;;) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        const bool physical_press = button_input_step(&onboard,
                                    gpio_get_level(BSP_BUTTON_BOOT_GPIO) == BSP_BUTTON_ACTIVE_LEVEL, now);
        if (expansion_available && now - last_expansion_check >= EXPANSION_CHECK_MS) {
            refresh_expansion_keys(&expansion);
            last_expansion_check = now;
        }
        bool expansion_talk_press = false;
        bool expansion_enter = false;
        if (expansion.lease.id) {
            expansion_talk_press = button_input_step(&expansion.left,
                                   gpio_get_level(expansion.left_gpio) == 0, now);
            expansion_enter = button_input_step(&expansion.right,
                              gpio_get_level(expansion.right_gpio) == 0, now);
        }
        const bool expansion_idle = !expansion.left.raw && !expansion.left.down &&
                                    !expansion.right.raw && !expansion.right.down;
        /* A key already held at attachment must be released before use. */
        expansion.armed |= expansion.lease.id && expansion_idle;
        expansion_enter &= expansion.armed && !expansion.left.raw;
        portENTER_CRITICAL(&input_lock);
        const uint8_t touches = input.held;
        const key_action_t requested = input.action;
        const uint32_t request_generation = input.generation;
        const bool feedback = input.feedback || physical_press ||
                              (expansion.armed && expansion_talk_press) || expansion_enter;
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
        if (connected && inhibited && !onboard.raw && !onboard.down && expansion_idle && touches == 0) {
            inhibited = false;
        }
        uint8_t keys[6] = {0}, modifiers = 0;
        bool fn = false;
        if (connected && !inhibited) {
            const button_action_t action = button_gesture_step(&gesture, onboard.down, now);
            const bool held_fn = gesture.fn || (touches & VOICE_MASK) ||
                                 (expansion.armed && expansion.left.down);
            key_sequence_tick(&sequence, now);
            /* Never combine Command+A/Delete with Fn or replay a rejected click. */
            if (!held_fn) {
                if (requested != KEY_ACTION_NONE && request_generation == generation) {
                    key_sequence_start(&sequence, requested);
                } else if (action.enter || (expansion_enter && !old_fn)) {
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
        .pin_bit_mask = 1ULL << BSP_BUTTON_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
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
    err = mosaico_module_mgr_init(NULL);
    expansion_available = err == ESP_OK;
    if (!expansion_available) {
        ESP_LOGW(TAG, "Expansion discovery unavailable: %s", esp_err_to_name(err));
    }
    if (xTaskCreate(poll_controls, "mic_keys", 4096, NULL, 4, &control_task) != pdPASS) {
        if (expansion_available) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(mosaico_module_mgr_deinit());
        }
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BOOT hold=F18, double-click=Enter; touch and expansion controls enabled");
    return ESP_OK;
}
