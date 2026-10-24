#pragma once
#include "demo_platform.h"
#include <string.h>

typedef struct {
    bool pending[3];
    esp_err_t error[3];
    unsigned epoch[3];
} platform_lifecycle_pending_t;

static inline void platform_lifecycle_latch(platform_lifecycle_pending_t *state,
                                             platform_event_kind_t kind, esp_err_t error, unsigned epoch)
{
    state->pending[kind] = true;
    state->error[kind] = error;
    state->epoch[kind] = epoch;
    if (kind != PLATFORM_ONLINE) state->pending[PLATFORM_ONLINE] = false;
}

static inline bool platform_lifecycle_take(platform_lifecycle_pending_t *state, platform_event_t *event)
{
    static const platform_event_kind_t order[] = {PLATFORM_UNBOUND, PLATFORM_OFFLINE, PLATFORM_ONLINE};
    for (unsigned i = 0; i < 3; ++i) {
        platform_event_kind_t kind = order[i];
        if (!state->pending[kind]) continue;
        *event = (platform_event_t){.kind = kind, .error = state->error[kind], .epoch = state->epoch[kind]};
        state->pending[kind] = false;
        return true;
    }
    return false;
}

typedef struct {
    uint32_t ticket;
    bool held, in_flight;
    unsigned pending;
} platform_http_gate_t;

static inline esp_err_t platform_http_gate_begin(platform_http_gate_t *gate, uint32_t *ticket)
{
    if (!ticket) return ESP_ERR_INVALID_ARG;
    if (gate->held) return ESP_ERR_INVALID_STATE;
    if (++gate->ticket == 0) ++gate->ticket;
    gate->held = true;
    *ticket = gate->ticket;
    return ESP_OK;
}

static inline bool platform_http_gate_ready(const platform_http_gate_t *gate, uint32_t ticket)
{
    return ticket && gate->held && ticket == gate->ticket && !gate->in_flight && !gate->pending;
}

static inline esp_err_t platform_http_gate_end(platform_http_gate_t *gate, uint32_t ticket)
{
    if (!ticket || !gate->held || ticket != gate->ticket) return ESP_ERR_INVALID_STATE;
    gate->held = false;
    return ESP_OK;
}
