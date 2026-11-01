// SPDX-License-Identifier: Apache-2.0
#include "battery_display.h"
#include "battery_status.h"
#include "bsp/esp_mosaico.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BATTERY_POLL_MS 5000
#define BATTERY_READ_TIMEOUT_MS 100
#define BATTERY_DISPLAY_LOCK_MS 100

static const char *TAG = "app_battery";
static struct {
    lv_obj_t *label;
    i2c_master_dev_handle_t device;
    TaskHandle_t task;
} battery;

static esp_err_t attach_gauge(void)
{
    if (battery.device) {
        return ESP_OK;
    }
    esp_err_t err = bsp_battery_init();
    if (err != ESP_OK) {
        return err;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_BATTERY_I2C_ADDR,
        .scl_speed_hz = BSP_BATTERY_I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(bus, &config, &battery.device);
}

static app_battery_status_t read_status(void)
{
    /* Adapted from system demo vibe_battery.c: scalar BSP getters hide I2C errors.
     * Read checked standard words, outside the LVGL lock; never write gauge commands. */
    static const uint8_t commands[] = {0x08, 0x0a, 0x0c, 0x2c};
    uint16_t values[4] = {0};
    bool ok = attach_gauge() == ESP_OK;
    for (unsigned i = 0; ok && i < 4; ++i) {
        uint8_t bytes[2];
        ok = i2c_master_transmit_receive(battery.device, &commands[i], 1, bytes, 2,
                                         BATTERY_READ_TIMEOUT_MS) == ESP_OK;
        if (ok) {
            values[i] = (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
        }
    }
    return app_battery_decode(ok, (int16_t)values[0], values[1], (int16_t)values[2], values[3]);
}

static void battery_task(void *argument)
{
    (void)argument;
    app_battery_status_t previous = {0};
    bool first = true;
    for (;;) {
        const app_battery_status_t status = read_status();
        if (first || previous.valid != status.valid || previous.percent != status.percent ||
                previous.charging != status.charging) {
            ESP_LOGI(TAG, "valid=%u percent=%u charging=%u", status.valid, status.percent, status.charging);
            previous = status;
            first = false;
        }
        if (bsp_display_lock(BATTERY_DISPLAY_LOCK_MS)) {
            const char *icon = !status.valid ? LV_SYMBOL_BATTERY_EMPTY :
                               status.percent >= 95 ? LV_SYMBOL_BATTERY_FULL :
                               status.percent >= 65 ? LV_SYMBOL_BATTERY_3 :
                               status.percent >= 35 ? LV_SYMBOL_BATTERY_2 :
                               status.percent > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
            if (status.valid) {
                lv_label_set_text_fmt(battery.label, "%s %u%%%s", icon, status.percent,
                                      status.charging ? " " LV_SYMBOL_CHARGE : "");
            } else {
                lv_label_set_text(battery.label, LV_SYMBOL_BATTERY_EMPTY " --%");
            }
            lv_obj_set_style_text_color(battery.label,
                                        lv_color_hex(status.valid && status.percent <= 20 && !status.charging ?
                                                     0xBF685B : 0x34495A), 0);
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
}

esp_err_t app_battery_display_start(lv_obj_t *label)
{
    if (!label) {
        return ESP_ERR_INVALID_ARG;
    }
    if (battery.task) {
        return ESP_ERR_INVALID_STATE;
    }
    battery.label = label;
    if (xTaskCreate(battery_task, "battery_ui", 4096, NULL, 2, &battery.task) != pdPASS) {
        battery.label = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
