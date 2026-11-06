// SPDX-License-Identifier: MIT
#pragma once
#include "demo_platform.h"
#include <string.h>

static inline platform_status_t platform_status_network_failure(platform_status_t status,
                                                                 bool ready, esp_err_t error)
{
    if (!ready && error != ESP_OK && status.phase != PLATFORM_PHASE_ERROR) {
        status.phase = PLATFORM_PHASE_OFFLINE;
        status.error = error;
        memset(status.code, 0, sizeof(status.code));
        status.expires_at_ms = 0;
    }
    return status;
}

/* Keep a pending binding's code/deadline in its worker-owned state. Transport
 * loss only hides it; recovery must never manufacture a new validity period. */
static inline platform_status_t platform_status_project(platform_status_t status,
                                                         bool network_ready,
                                                         bool mqtt_ready,
                                                         bool online,
                                                         bool grant_received)
{
    if (status.phase == PLATFORM_PHASE_BINDING && grant_received) {
        status.phase = PLATFORM_PHASE_VERIFYING;
        status.error = ESP_OK;
        memset(status.code, 0, sizeof(status.code));
        status.expires_at_ms = 0;
    }
    if (status.phase == PLATFORM_PHASE_IDLE || status.phase == PLATFORM_PHASE_CONNECTING ||
        status.phase == PLATFORM_PHASE_ERROR)
        return status;
    if (!network_ready ||
        (status.phase == PLATFORM_PHASE_ONLINE && (!mqtt_ready || !online)) ||
        (status.phase == PLATFORM_PHASE_BINDING && !mqtt_ready)) {
        status.phase = PLATFORM_PHASE_OFFLINE;
        if (status.error == ESP_OK) status.error = ESP_ERR_INVALID_STATE;
        memset(status.code, 0, sizeof(status.code));
        status.expires_at_ms = 0;
    }
    return status;
}
