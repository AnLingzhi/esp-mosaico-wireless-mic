// SPDX-License-Identifier: Apache-2.0
#include "app_hfp.h"

#include "app_audio.h"
#include "app_hid.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#if !CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY || !CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
#error "Mosaico Mic requires a Classic BR/EDR controller and HFP HCI audio"
#endif
#if CONFIG_BT_HFP_USE_EXTERNAL_CODEC
#error "This pinned prototype supplies PCM to the internal HFP codec"
#endif

#define PROFILE_READY BIT0
#define PROFILE_FAILED BIT1
#define LINK_BUSY BIT2
#define LINK_CONNECTED BIT3
#define LINK_STREAMING BIT4
#define RECONNECT_INTERVAL_MS 10000
#define COD_HANDSFREE_MINOR 2

static const char *TAG = "app_hfp";
/* Bluedroid owns callbacks; this service and event group live until reboot. */
static struct {
    EventGroupHandle_t events;
    bool controller_initialized;
    bool controller_enabled;
    bool host_initialized;
    bool host_enabled;
} service;

app_hfp_status_t app_hfp_status(void)
{
    const EventBits_t bits = service.events ? xEventGroupGetBits(service.events) : 0;
    return (app_hfp_status_t) {
        .connected = (bits & LINK_CONNECTED) != 0,
        .streaming = (bits & LINK_STREAMING) != 0,
    };
}

static void reconnect_task(void *argument)
{
    (void)argument;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_INTERVAL_MS));
        if (xEventGroupGetBits(service.events) & LINK_BUSY) {
            continue;
        }
        /* Never guess a target when several hosts are bonded. No bond is erased. */
        if (esp_bt_gap_get_bond_device_num() != 1) {
            continue;
        }
        esp_bd_addr_t peer;
        int count = 1;
        if (esp_bt_gap_get_bond_device_list(&count, &peer) != ESP_OK || count != 1) {
            continue;
        }
        xEventGroupSetBits(service.events, LINK_BUSY);
        const esp_err_t err = esp_hf_client_connect(peer);
        ESP_LOGI(TAG, "Reconnect bonded HFP host: %s", esp_err_to_name(err));
        if (err != ESP_OK) {
            xEventGroupClearBits(service.events, LINK_BUSY);
        }
    }
}

static void check_callback_result(esp_err_t err, const char *operation)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", operation, esp_err_to_name(err));
    }
}

static void incoming_pcm(const uint8_t *data, uint32_t bytes)
{
    (void)data;
    (void)bytes;
    /* SCO receive cadence supplies the transmit clock; never echo its data. */
    esp_hf_client_outgoing_data_ready();
}

static uint32_t outgoing_pcm(uint8_t *data, uint32_t bytes)
{
    return app_audio_read_pcm(data, bytes);
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "ACL connection status=%d", param->acl_conn_cmpl_stat.stat);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "ACL disconnected reason=0x%x", param->acl_disconn_cmpl_stat.reason);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "pairing status=%d peer=%s", param->auth_cmpl.stat,
                 param->auth_cmpl.device_name);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* NoInputNoOutput capability selects SSP Just Works for this prototype. */
        check_callback_result(esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true), "SSP reply");
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        check_callback_result(esp_bt_gap_pin_reply(param->pin_req.bda,
                                                   !param->pin_req.min_16_digit, 4, pin), "PIN reply");
        break;
    }
    default:
        break;
    }
}

static void hfp_callback(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        if (param->prof_stat.state == ESP_HF_INIT_SUCCESS) {
            const esp_err_t err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                           ESP_BT_GENERAL_DISCOVERABLE);
            xEventGroupSetBits(service.events, err == ESP_OK ? PROFILE_READY : PROFILE_FAILED);
            ESP_LOGI(TAG, "HFP registered; discoverable as Mosaico Mic, status=%s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "HFP profile state=%d", param->prof_stat.state);
            xEventGroupSetBits(service.events, PROFILE_FAILED);
        }
        break;
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "HFP connection state=%d peer_features=0x%lx",
                 param->conn_stat.state, (unsigned long)param->conn_stat.peer_feat);
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            xEventGroupClearBits(service.events, LINK_BUSY | LINK_CONNECTED | LINK_STREAMING);
            app_hid_peer_disconnected();
            app_audio_set_stream_rate(0);
            check_callback_result(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                           ESP_BT_GENERAL_DISCOVERABLE), "resume discovery");
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            xEventGroupSetBits(service.events, LINK_BUSY | LINK_CONNECTED);
            app_hid_peer_connected(param->conn_stat.remote_bda);
        } else {
            xEventGroupSetBits(service.events, LINK_BUSY);
        }
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        const esp_hf_client_audio_state_t state = param->audio_stat.state;
        uint32_t rate = 0;
        if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            rate = 16000;
        } else if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            rate = 8000;
        }
        app_audio_set_stream_rate(rate);
        if (rate) {
            xEventGroupSetBits(service.events, LINK_STREAMING);
        } else {
            xEventGroupClearBits(service.events, LINK_STREAMING);
        }
        ESP_LOGI(TAG, "SCO state=%d codec=%s pcm_rate=%lu", state,
                 rate == 16000 ? "mSBC" : rate == 8000 ? "CVSD" : "inactive",
                 (unsigned long)rate);
        if (rate != 0) {
            check_callback_result(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                                           ESP_BT_NON_DISCOVERABLE), "pause discovery for SCO");
            esp_hf_client_outgoing_data_ready();
        } else if (state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            check_callback_result(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                           ESP_BT_GENERAL_DISCOVERABLE), "resume discovery");
        }
        break;
    }
    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        ESP_LOGI(TAG, "remote volume target=%d gain=%d (fixed local input gain)",
                 param->volume_control.type, param->volume_control.volume);
        break;
    default:
        break;
    }
}

static void rollback_stack(void)
{
    if (service.host_enabled) {
        check_callback_result(esp_bluedroid_disable(), "disable host");
    }
    if (service.host_initialized) {
        check_callback_result(esp_bluedroid_deinit(), "deinit host");
    }
    if (service.controller_enabled) {
        check_callback_result(esp_bt_controller_disable(), "disable controller");
    }
    if (service.controller_initialized) {
        check_callback_result(esp_bt_controller_deinit(), "deinit controller");
    }
    /* Keep events allocated: an already queued callback may still refer to it. */
}

esp_err_t app_hfp_start(void)
{
    if (service.events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    service.events = xEventGroupCreate();
    if (service.events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret;
    esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t host = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_bt_io_cap_t io = ESP_BT_IO_CAP_NONE;
    esp_bt_cod_t cod = {
        .major = ESP_BT_COD_MAJOR_DEV_AV,
        .minor = COD_HANDSFREE_MINOR,
        .service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_CAPTURING | ESP_BT_COD_SRVC_TELEPHONY,
    };
    ESP_GOTO_ON_ERROR(esp_bt_controller_init(&controller), fail, TAG, "controller init");
    service.controller_initialized = true;
    ESP_LOGI(TAG, "Enabling Classic Bluetooth controller");
    ESP_GOTO_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), fail, TAG, "controller enable");
    service.controller_enabled = true;
    ESP_LOGI(TAG, "Initializing Bluedroid HFP host");
    ESP_GOTO_ON_ERROR(esp_bluedroid_init_with_cfg(&host), fail, TAG, "host init");
    service.host_initialized = true;
    ESP_GOTO_ON_ERROR(esp_bluedroid_enable(), fail, TAG, "host enable");
    service.host_enabled = true;
    ESP_GOTO_ON_ERROR(esp_bt_gap_register_callback(gap_callback), fail, TAG, "GAP callback");
    ESP_GOTO_ON_ERROR(esp_bt_gap_set_device_name("Mosaico Mic"), fail, TAG, "device name");
    ESP_GOTO_ON_ERROR(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io, sizeof(io)),
                      fail, TAG, "pairing capability");
    ESP_GOTO_ON_ERROR(esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL), fail, TAG, "device class");
    /* HID shares this host/controller; never initialize a second Bluetooth stack. */
    ret = app_hid_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HID startup failed: %s; microphone continues", esp_err_to_name(ret));
    }
    ESP_GOTO_ON_ERROR(esp_hf_client_register_callback(hfp_callback), fail, TAG, "HFP callback");
    ESP_GOTO_ON_ERROR(esp_hf_client_register_data_callback(incoming_pcm, outgoing_pcm),
                      fail, TAG, "HFP PCM callbacks");
    ESP_GOTO_ON_ERROR(esp_hf_client_init(), fail, TAG, "HFP init");
    EventBits_t ready = xEventGroupWaitBits(service.events, PROFILE_READY | PROFILE_FAILED,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (ready & PROFILE_READY) {
        ESP_LOGI(TAG, "Bonded hosts=%d; auto reconnect requires exactly one", esp_bt_gap_get_bond_device_num());
        if (xTaskCreate(reconnect_task, "hfp_reconnect", 3072, NULL, 2, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    /* Init is asynchronous; retain resources for late callbacks on timeout. */
    return (ready & PROFILE_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
fail:
    rollback_stack();
    return ret;
}
