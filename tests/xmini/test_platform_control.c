// SPDX-License-Identifier: MIT
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "platform_control_state.h"

int main(void)
{
    platform_lifecycle_pending_t lifecycle = {0};
    platform_event_t event;
    assert(!platform_lifecycle_take(&lifecycle, &event));
    /* A full JSON queue cannot consume the independent lifecycle mailbox. */
    platform_lifecycle_latch(&lifecycle, PLATFORM_ONLINE, ESP_OK, 1);
    platform_lifecycle_latch(&lifecycle, PLATFORM_OFFLINE, ESP_ERR_TIMEOUT, 2);
    platform_lifecycle_latch(&lifecycle, PLATFORM_UNBOUND, ESP_OK, 3);
    platform_lifecycle_latch(&lifecycle, PLATFORM_ONLINE, ESP_OK, 3);
    assert(platform_lifecycle_take(&lifecycle, &event));
    assert(event.kind == PLATFORM_UNBOUND && event.epoch == 3 && !event.json);
    assert(platform_lifecycle_take(&lifecycle, &event));
    assert(event.kind == PLATFORM_OFFLINE && event.epoch == 2 && event.error == ESP_ERR_TIMEOUT);
    assert(platform_lifecycle_take(&lifecycle, &event));
    assert(event.kind == PLATFORM_ONLINE && event.epoch == 3);
    assert(!platform_lifecycle_take(&lifecycle, &event));
    for (unsigned epoch = 4; epoch < 1004; ++epoch) {
        platform_lifecycle_latch(&lifecycle, PLATFORM_ONLINE, ESP_OK, epoch);
        platform_lifecycle_latch(&lifecycle, PLATFORM_OFFLINE, ESP_FAIL, epoch);
    }
    assert(platform_lifecycle_take(&lifecycle, &event));
    assert(event.kind == PLATFORM_OFFLINE && event.epoch == 1003);
    assert(!platform_lifecycle_take(&lifecycle, &event));
    platform_lifecycle_latch(&lifecycle, PLATFORM_UNBOUND, ESP_OK, 1004);
    platform_lifecycle_latch(&lifecycle, PLATFORM_OFFLINE, ESP_FAIL, 1005);
    assert(platform_lifecycle_take(&lifecycle, &event) && event.kind == PLATFORM_UNBOUND);
    assert(platform_lifecycle_take(&lifecycle, &event) && event.kind == PLATFORM_OFFLINE);

    platform_http_gate_t gate = {.pending = 2, .in_flight = true};
    uint32_t ticket, ignored;
    assert(platform_http_gate_begin(&gate, NULL) == ESP_ERR_INVALID_ARG);
    assert(platform_http_gate_begin(&gate, &ticket) == ESP_OK && ticket);
    assert(platform_http_gate_begin(&gate, &ignored) == ESP_ERR_INVALID_STATE);
    assert(!platform_http_gate_ready(&gate, ticket));
    gate.in_flight = false;
    assert(!platform_http_gate_ready(&gate, ticket));
    gate.pending--;
    gate.in_flight = true;
    assert(!platform_http_gate_ready(&gate, ticket));
    gate.pending--;
    assert(!platform_http_gate_ready(&gate, ticket));
    gate.in_flight = false;
    assert(platform_http_gate_ready(&gate, ticket));
    assert(!platform_http_gate_ready(&gate, 0));
    assert(platform_http_gate_end(&gate, ticket + 1) == ESP_ERR_INVALID_STATE && gate.held);
    assert(platform_http_gate_end(&gate, ticket) == ESP_OK);
    assert(!platform_http_gate_ready(&gate, ticket));
    assert(platform_http_gate_begin(&gate, &ignored) == ESP_OK && ignored != ticket);
    assert(platform_http_gate_end(&gate, ticket) == ESP_ERR_INVALID_STATE && gate.held);
    assert(platform_http_gate_ready(&gate, ignored));
    assert(platform_http_gate_end(&gate, ignored) == ESP_OK);
    gate.ticket = UINT32_MAX;
    assert(platform_http_gate_begin(&gate, &ticket) == ESP_OK && ticket == 1);
    puts("platform control: sticky lifecycle, unbind ordering, drain, stale tickets PASS");
    return 0;
}
