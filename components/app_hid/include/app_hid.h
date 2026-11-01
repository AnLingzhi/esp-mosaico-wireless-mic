// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The shared Classic controller/host must already be enabled by app_hfp. */
esp_err_t app_hid_start(void);
void app_hid_peer_connected(const uint8_t address[6]);
void app_hid_peer_disconnected(void);
bool app_hid_connected(uint32_t *generation);
void app_hid_reconnect(void);
esp_err_t app_hid_set_keys(bool fn, uint8_t modifiers, const uint8_t keys[6]);
