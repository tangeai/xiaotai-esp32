// SPDX-License-Identifier: MIT
#include "platform_internal.h"

/* Worker-backed and dormant tickets share a sequence even across host reset. */
static atomic_uint_least32_t s_next_ticket;

static uint32_t next_ticket(void)
{
    uint32_t ticket;
    do {
        ticket = (uint32_t)atomic_fetch_add_explicit(&s_next_ticket, 1U, memory_order_relaxed) + 1U;
    } while (!ticket);
    return ticket;
}

static bool dormant(void)
{
    return atomic_load(&platform.initialized) && atomic_load(&platform.started) &&
        !atomic_load(&platform.stopped) && !atomic_load(&platform.running) &&
        ((platform.provisioning && !platform.objects) || xiaotai_platform_start_failed());
}

static xiaotai_err_t close_gate(uint32_t *ticket, bool require_binding_idle)
{
    if (!ticket) return XIAOTAI_ERR_INVALID_ARG;
    /* Neither dormant path has a worker to drain exchanges. */
    if (dormant()) {
        uint_least32_t held = 0;
        const uint32_t next = next_ticket();
        if (!atomic_compare_exchange_strong(&platform.dormant_gate, &held, next))
            return XIAOTAI_ERR_PLATFORM_GATED;
        *ticket = next;
        return XIAOTAI_OK;
    }
    if (!platform.objects || !atomic_load(&platform.running)) return XIAOTAI_ERR_INVALID_STATE;
    platform_lock();
    if (platform.gate) {
        platform_unlock();
        return XIAOTAI_ERR_PLATFORM_GATED;
    }
    if (require_binding_idle && (platform.binding.phase != PLATFORM_BIND_NONE ||
                                 platform.status.phase == XIAOTAI_PLATFORM_UNBOUND)) {
        platform_unlock();
        return XIAOTAI_ERR_BUSY;
    }
    *ticket = platform.gate = next_ticket();
    platform.gate_closed = 0;
    platform.status.gated = true;
    if (platform.session.http) xiaotai_http_cancel(platform.session.http);
    platform_unlock();
    platform_wake();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_gate_close(uint32_t *ticket)
{
    return close_gate(ticket, false);
}

xiaotai_err_t xiaotai_platform_gate_close_binding_idle(uint32_t *ticket)
{
    return close_gate(ticket, true);
}

bool xiaotai_platform_gate_drained(uint32_t ticket)
{
    if (!ticket) return false;
    if (dormant()) return atomic_load(&platform.dormant_gate) == ticket;
    if (!platform.objects) return false;
    platform_lock();
    const bool drained = ticket == platform.gate && ticket == platform.gate_closed && !platform.active;
    platform_unlock();
    return drained;
}

xiaotai_err_t xiaotai_platform_gate_open(uint32_t ticket)
{
    if (!ticket) return XIAOTAI_ERR_INVALID_ARG;
    if (dormant()) {
        uint_least32_t held = ticket;
        return atomic_compare_exchange_strong(&platform.dormant_gate, &held, 0) ?
            XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
    }
    if (!platform.objects) return XIAOTAI_ERR_INVALID_ARG;
    platform_lock();
    const bool matches = ticket == platform.gate;
    if (matches) {
        platform.gate = 0;
        platform.gate_closed = 0;
        platform.status.gated = false;
    }
    platform_unlock();
    if (matches) platform_wake();
    return matches ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}
