// SPDX-License-Identifier: MIT
/* Fake network engine (maintenance gate) and preferences service for the
 * environment module's host tests. */
#include <string.h>
#include "fakes.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/net.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"

fake_net_t g_net;
fake_preferences_t g_preferences;

void fake_net_reset(void) { memset(&g_net, 0, sizeof(g_net)); }

bool xiaotai_net_provisioning_held(void)
{
    return atomic_load(&g_net.held) || atomic_load(&g_net.restart_locked);
}

xiaotai_err_t xiaotai_net_maintenance_begin(uint32_t *ticket)
{
    fake_trace("net_begin");
    if (atomic_load(&g_net.cancel_in_begin)) {
        atomic_store(&g_net.cancel_in_begin_result, xiaotai_environment_cancel());
    }
    const xiaotai_err_t result = atomic_load(&g_net.begin_result);
    if (result != XIAOTAI_OK) return result;
    if (atomic_load(&g_net.held)) return XIAOTAI_ERR_INVALID_STATE;
    atomic_store(&g_net.held, true);
    *ticket = atomic_fetch_add(&g_net.ticket, 1U) + 201U;
    return XIAOTAI_OK;
}

void xiaotai_net_maintenance_end(uint32_t ticket)
{
    (void)ticket;
    fake_trace("net_end");
    /* As the engine: a gate that ends in a restart can no longer be released. */
    if (!atomic_load(&g_net.restart_locked)) atomic_store(&g_net.held, false);
}

xiaotai_err_t xiaotai_net_maintenance_restart(uint32_t ticket)
{
    fake_trace("net_restart");
    atomic_store(&g_net.restart_ticket, ticket);
    const xiaotai_err_t result = atomic_load(&g_net.restart_result);
    if (result != XIAOTAI_OK) return result;
    atomic_store(&g_net.restart_locked, true);
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------- preferences */

static xiaotai_signal_t s_unblock;
static atomic_bool s_blocked;
static atomic_bool s_signal_ready;

void fake_preferences_reset(void)
{
    if (!atomic_load(&s_signal_ready)) {
        xiaotai_signal_init(&s_unblock);
        atomic_store(&s_signal_ready, true);
    }
    (void)xiaotai_signal_take(&s_unblock, 0);
    atomic_store(&s_blocked, false);
    atomic_store(&g_preferences.pending, false);
    memset(g_preferences.script, 0, sizeof(g_preferences.script));
    atomic_store(&g_preferences.script_length, 0U);
    atomic_store(&g_preferences.flushes, 0U);
    atomic_store(&g_preferences.flush_result, XIAOTAI_OK);
    atomic_store(&g_preferences.block, false);
    atomic_store(&g_preferences.last_timeout_ms, 0U);
    atomic_store(&g_preferences.flushed_on_owner, false);
}

void fake_preferences_unblock(void) { xiaotai_signal_give(&s_unblock); }

bool fake_preferences_wait_blocked(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 5) {
        if (atomic_load(&s_blocked)) return true;
        xiaotai_delay_ms(5);
    }
    return false;
}

bool xiaotai_preferences_pending(void)
{
    fake_trace("pending");
    return atomic_load(&g_preferences.pending);
}

xiaotai_err_t xiaotai_preferences_maintenance_flush(uint32_t timeout_ms)
{
    fake_trace("pref_flush");
    const unsigned index = atomic_fetch_add(&g_preferences.flushes, 1U);
    atomic_store(&g_preferences.last_timeout_ms, timeout_ms);
    if (xiaotai_on_owner()) atomic_store(&g_preferences.flushed_on_owner, true);
    if (atomic_load(&g_preferences.block)) {
        atomic_store(&s_blocked, true);
        const bool released = xiaotai_signal_take(&s_unblock, timeout_ms);
        atomic_store(&s_blocked, false);
        if (!released) return XIAOTAI_ERR_TIMEOUT;
    }
    if (index < atomic_load(&g_preferences.script_length)) return g_preferences.script[index];
    return atomic_load(&g_preferences.flush_result);
}
