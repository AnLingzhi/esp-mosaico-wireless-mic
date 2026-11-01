// SPDX-License-Identifier: Apache-2.0
#include "app_audio.h"
#include "app_controls.h"
#include "app_hfp.h"
#include "app_touch.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "nvs_flash.h"

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    iris_ota_support_start();
    /* Preserve Bluetooth bonds and existing NVS on any error. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        err = app_audio_start();
    }
    if (err == ESP_OK) {
        err = app_hfp_start();
    }
    if (err == ESP_OK) {
        err = app_controls_start();
    }
    if (err == ESP_OK) {
        err = app_touch_start(app_controls_touch);
    }
    if (err != ESP_OK) {
        ESP_LOGE("wireless_mic", "Startup failed: %s; Recovery remains available", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_mark_healthy());
    ESP_LOGI("wireless_mic", "Microphone ready. Pair Mosaico Mic and select it as Mac sound input.");
}
