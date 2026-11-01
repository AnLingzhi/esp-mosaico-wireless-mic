// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>

typedef enum {
    APP_LINK_OFFLINE,
    APP_LINK_READY,
    APP_LINK_ACTIVE,
} app_link_state_t;

static inline app_link_state_t app_audio_link_state(bool connected, bool streaming)
{
    return !connected ? APP_LINK_OFFLINE : streaming ? APP_LINK_ACTIVE : APP_LINK_READY;
}
