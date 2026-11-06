// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform_state.h"

int main(void)
{
    platform_status_t state = {.phase = PLATFORM_PHASE_ONLINE, .revision = 7};
    platform_status_t shown = platform_status_project(state, true, true, true, false);
    assert(shown.phase == PLATFORM_PHASE_ONLINE);
    shown = platform_status_project(state, false, true, true, false);
    assert(shown.phase == PLATFORM_PHASE_OFFLINE);
    shown = platform_status_project(state, true, false, true, false);
    assert(shown.phase == PLATFORM_PHASE_OFFLINE);
    shown = platform_status_project(state, true, true, false, false);
    assert(shown.phase == PLATFORM_PHASE_OFFLINE);

    state = (platform_status_t){.phase = PLATFORM_PHASE_BINDING, .code = "001234", .expires_at_ms = 190000};
    shown = platform_status_project(state, true, true, false, false);
    assert(shown.phase == PLATFORM_PHASE_BINDING && !strcmp(shown.code, "001234"));
    shown = platform_status_project(state, false, true, false, false);
    assert(shown.phase == PLATFORM_PHASE_OFFLINE && !shown.code[0] && !shown.expires_at_ms);
    shown = platform_status_project(state, true, false, false, false);
    assert(shown.phase == PLATFORM_PHASE_OFFLINE && !shown.code[0]);
    shown = platform_status_project(state, true, true, false, false);
    assert(shown.phase == PLATFORM_PHASE_BINDING && shown.expires_at_ms == 190000);
    assert(!strcmp(state.code, "001234") && state.expires_at_ms == 190000);
    shown = platform_status_project(state, true, true, false, true);
    assert(shown.phase == PLATFORM_PHASE_VERIFYING && !shown.code[0] && !shown.expires_at_ms);
    shown = platform_status_project(state, false, false, false, true);
    assert(shown.phase == PLATFORM_PHASE_OFFLINE && !shown.code[0]);

    state = (platform_status_t){.phase = PLATFORM_PHASE_ERROR, .error = 0x110d};
    shown = platform_status_project(state, false, false, false, false);
    assert(shown.phase == PLATFORM_PHASE_ERROR && shown.error == 0x110d);
    state.phase = PLATFORM_PHASE_CONNECTING;
    shown = platform_status_project(state, false, false, false, false);
    assert(shown.phase == PLATFORM_PHASE_CONNECTING);
    shown = platform_status_network_failure(state, false, ESP_ERR_TIMEOUT);
    shown = platform_status_project(shown, false, false, false, false);
    assert(shown.phase == PLATFORM_PHASE_OFFLINE && shown.error == ESP_ERR_TIMEOUT);
    shown = platform_status_network_failure(state, false, ESP_OK);
    assert(shown.phase == PLATFORM_PHASE_CONNECTING);
    shown = platform_status_network_failure(state, true, ESP_ERR_TIMEOUT);
    assert(shown.phase == PLATFORM_PHASE_CONNECTING);
    state = (platform_status_t){.phase = PLATFORM_PHASE_ERROR, .error = ESP_ERR_INVALID_RESPONSE};
    shown = platform_status_network_failure(state, false, ESP_ERR_TIMEOUT);
    assert(shown.phase == PLATFORM_PHASE_ERROR && shown.error == ESP_ERR_INVALID_RESPONSE);
    state.phase = PLATFORM_PHASE_VERIFYING;
    shown = platform_status_project(state, true, false, false, false);
    assert(shown.phase == PLATFORM_PHASE_VERIFYING);
    puts("platform state: transport, binding recovery, grant cancellation, errors passed");
    return 0;
}
