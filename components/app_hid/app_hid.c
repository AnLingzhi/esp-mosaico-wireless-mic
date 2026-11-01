// SPDX-License-Identifier: Apache-2.0
#include "app_hid.h"
#include "app_hid_report_map.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

#define HID_READY BIT0
#define HID_FAILED BIT1
#define REPORT_ID 1
#define REPORT_BYTES 8
#define HID_FN_TRANSPORT_F18 0x6D

static const char *TAG = "app_hid";
static esp_hidd_qos_param_t qos;
static portMUX_TYPE hid_lock = portMUX_INITIALIZER_UNLOCKED;
/* Protected by hid_lock; no Bluetooth calls are made while holding it. */
static struct {
    EventGroupHandle_t ready;
    bool registered;
    bool connected;
    bool connecting;
    bool closing;
    bool peer_present;
    bool report_mode;
    uint32_t generation;
    uint8_t peer[6];
    uint8_t report[REPORT_BYTES];
} hid;

static esp_hidd_app_param_t application = {
    .name = "Mosaico Mic Keys",
    .description = "Microphone Fn and four touch keys",
    .provider = "Mosaico prototype",
    .subclass = ESP_HID_CLASS_KBD,
    .desc_list = app_hid_report_map,
    .desc_list_len = sizeof(app_hid_report_map),
};

static void report_error(esp_hidd_handshake_error_t reason)
{
    const esp_err_t err = esp_bt_hid_device_report_error(reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HID response: %s", esp_err_to_name(err));
    }
}

static void invalidate_link(void)
{
    portENTER_CRITICAL(&hid_lock);
    hid.connected = false;
    hid.connecting = false;
    hid.closing = false;
    hid.generation++;
    memset(hid.report, 0, sizeof(hid.report));
    portEXIT_CRITICAL(&hid_lock);
}

static void request_close(void)
{
    portENTER_CRITICAL(&hid_lock);
    hid.closing = true;
    hid.generation++;
    memset(hid.report, 0, sizeof(hid.report));
    portEXIT_CRITICAL(&hid_lock);
}

static void callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_INIT_EVT:
        if (param->init.status != ESP_HIDD_SUCCESS ||
                esp_bt_hid_device_register_app(&application, &qos, &qos) != ESP_OK) {
            xEventGroupSetBits(hid.ready, HID_FAILED);
        }
        break;
    case ESP_HIDD_REGISTER_APP_EVT:
        portENTER_CRITICAL(&hid_lock);
        hid.registered = param->register_app.status == ESP_HIDD_SUCCESS;
        portEXIT_CRITICAL(&hid_lock);
        xEventGroupSetBits(hid.ready, hid.registered ? HID_READY : HID_FAILED);
        ESP_LOGI(TAG, "HID registered status=%d", param->register_app.status);
        break;
    case ESP_HIDD_OPEN_EVT:
        portENTER_CRITICAL(&hid_lock);
        hid.connected = param->open.status == ESP_HIDD_SUCCESS &&
                        param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED;
        hid.connecting = param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTING;
        if (hid.connected && (!hid.peer_present || memcmp(hid.peer, param->open.bd_addr, 6))) {
            hid.closing = true;
        }
        hid.report_mode = true;
        hid.generation++;
        memset(hid.report, 0, sizeof(hid.report));
        portEXIT_CRITICAL(&hid_lock);
        ESP_LOGI(TAG, "HID open status=%d state=%d", param->open.status, param->open.conn_status);
        break;
    case ESP_HIDD_CLOSE_EVT:
        if ((param->close.status != ESP_HIDD_SUCCESS && param->close.status != ESP_HIDD_NO_CONNECTION) ||
                param->close.conn_status != ESP_HIDD_CONN_STATE_DISCONNECTED) {
            request_close();
            ESP_LOGW(TAG, "HID close pending status=%d", param->close.status);
            break;
        }
        invalidate_link();
        ESP_LOGI(TAG, "HID disconnected");
        break;
    case ESP_HIDD_VC_UNPLUG_EVT:
        invalidate_link();
        ESP_LOGI(TAG, "HID disconnected event=%d", event);
        break;
    case ESP_HIDD_SET_PROTOCOL_EVT:
        portENTER_CRITICAL(&hid_lock);
        hid.report_mode = param->set_protocol.protocol_mode == ESP_HIDD_REPORT_MODE;
        hid.generation++;
        portEXIT_CRITICAL(&hid_lock);
        ESP_LOGI(TAG, "HID protocol=%d (Fn requires report mode)", param->set_protocol.protocol_mode);
        break;
    case ESP_HIDD_GET_REPORT_EVT: {
        uint8_t report[REPORT_BYTES];
        if (param->get_report.report_type != ESP_HIDD_REPORT_TYPE_INPUT ||
                param->get_report.report_id != REPORT_ID) {
            report_error(ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_REP_ID);
            break;
        }
        portENTER_CRITICAL(&hid_lock);
        memcpy(report, hid.report, sizeof(report));
        if (hid.closing) {
            memset(report, 0, sizeof(report));
        }
        const bool report_mode = hid.report_mode;
        portEXIT_CRITICAL(&hid_lock);
        uint16_t length = report_mode ? REPORT_BYTES : 8;
        if (param->get_report.buffer_size && param->get_report.buffer_size < length + 1) {
            length = param->get_report.buffer_size - 1;
        }
        if (esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INPUT, REPORT_ID,
                                          length, report) != ESP_OK) {
            report_error(ESP_HID_PAR_HANDSHAKE_RSP_NOT_READY);
        }
        break;
    }
    case ESP_HIDD_SET_REPORT_EVT:
        report_error(ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ);
        break;
    case ESP_HIDD_SEND_REPORT_EVT:
        if (param->send_report.status != ESP_HIDD_SUCCESS) {
            ESP_LOGE(TAG, "HID send failed status=%d reason=%u; disconnect to release keys",
                     param->send_report.status, param->send_report.reason);
            request_close();
        }
        break;
    default:
        break;
    }
}

esp_err_t app_hid_start(void)
{
    if (hid.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    hid.ready = xEventGroupCreate();
    if (!hid.ready) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_bt_hid_device_register_callback(callback);
    if (err == ESP_OK) {
        err = esp_bt_hid_device_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    const EventBits_t result = xEventGroupWaitBits(hid.ready, HID_READY | HID_FAILED,
                                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    return result & HID_READY ? ESP_OK : result & HID_FAILED ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void app_hid_peer_connected(const uint8_t address[6])
{
    portENTER_CRITICAL(&hid_lock);
    if ((hid.connected || hid.connecting) && memcmp(hid.peer, address, sizeof(hid.peer))) {
        hid.closing = true;
        hid.generation++;
        memset(hid.report, 0, sizeof(hid.report));
    }
    memcpy(hid.peer, address, sizeof(hid.peer));
    hid.peer_present = true;
    portEXIT_CRITICAL(&hid_lock);
    app_hid_reconnect();
}

void app_hid_peer_disconnected(void)
{
    portENTER_CRITICAL(&hid_lock);
    hid.peer_present = false;
    const bool active = hid.connected || hid.connecting;
    portEXIT_CRITICAL(&hid_lock);
    if (active) {
        request_close();
    }
}

void app_hid_reconnect(void)
{
    uint8_t peer[6];
    portENTER_CRITICAL(&hid_lock);
    const bool closing = hid.closing;
    const bool connected = hid.connected;
    const bool connect = !closing && hid.registered && hid.peer_present && !hid.connected && !hid.connecting;
    if (connect) {
        hid.connecting = true;
        memcpy(peer, hid.peer, sizeof(peer));
    }
    portEXIT_CRITICAL(&hid_lock);
    if (closing) {
        if (connected) {
            uint8_t release[REPORT_BYTES] = {0};
            const esp_err_t released = esp_bt_hid_device_send_report(
                                           ESP_HIDD_REPORT_TYPE_INTRDATA, REPORT_ID, sizeof(release), release);
            if (released != ESP_OK) {
                ESP_LOGW(TAG, "HID release retry: %s", esp_err_to_name(released));
            }
        }
        const esp_err_t closed = esp_bt_hid_device_disconnect();
        if (closed != ESP_OK) {
            ESP_LOGW(TAG, "HID disconnect retry: %s", esp_err_to_name(closed));
        }
        return;
    }
    if (connect) {
        const esp_err_t err = esp_bt_hid_device_connect(peer);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&hid_lock);
            hid.connecting = false;
            portEXIT_CRITICAL(&hid_lock);
            ESP_LOGW(TAG, "HID connect: %s", esp_err_to_name(err));
        }
    }
}

bool app_hid_connected(uint32_t *generation)
{
    portENTER_CRITICAL(&hid_lock);
    const bool connected = hid.connected && hid.report_mode && !hid.closing;
    *generation = hid.generation;
    portEXIT_CRITICAL(&hid_lock);
    return connected;
}

esp_err_t app_hid_set_keys(bool fn, uint8_t modifiers, const uint8_t keys[6])
{
    uint8_t report[REPORT_BYTES] = {0};
    report[0] = modifiers;
    memcpy(report + 2, keys, 6);
    /* macOS maps only this device's F18 to Fn through UserKeyMapping. */
    if (fn) {
        report[7] = HID_FN_TRANSPORT_F18;
    }
    portENTER_CRITICAL(&hid_lock);
    const bool connected = hid.connected && hid.report_mode && !hid.closing;
    if (connected) {
        memcpy(hid.report, report, sizeof(report));
    }
    portEXIT_CRITICAL(&hid_lock);
    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA,
                                                        REPORT_ID, sizeof(report), report);
    if (err != ESP_OK) {
        request_close();
    }
    return err;
}
