// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include "connection_status.h"

int main(void)
{
    assert(app_audio_link_state(false, false) == APP_LINK_OFFLINE);
    assert(app_audio_link_state(false, true) == APP_LINK_OFFLINE);
    assert(app_audio_link_state(true, false) == APP_LINK_READY);
    assert(app_audio_link_state(true, true) == APP_LINK_ACTIVE);
    return 0;
}
