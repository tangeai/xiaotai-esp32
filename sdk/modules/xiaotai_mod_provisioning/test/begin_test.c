// SPDX-License-Identifier: MIT
/* mod_provisioning_begin: the begin sequence (specification 2.3) with the
 * platform core in the build and the default failure rule (release). Built
 * with a settle bound of SETTLE_MS (-DCONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS)
 * so the bound is measured on an absolute schedule without a 5 s wait; the
 * default of 5000 is checked by mod_provisioning_config.
 *   - refused while an activity is current without PREEMPT; PREEMPT revokes it
 *     through the quiesce; refused while somebody else quiesced (maintenance);
 *     argument, state and network-kind refusals; nothing taken on a refusal
 *   - concurrent: no gate; the wait for settings ends at the bound and the
 *     request is handed anyway (counted); a setting not saved is counted and
 *     the wait goes on; the quiesce ends when the engine finished; an engine
 *     failure (synchronous or later) releases and reports
 *   - exclusive: the gate is closed and the request handed only once settings
 *     are written and the gate drained; the bound or a setting not saved stops
 *     the begin with the gate opened and the quiesce ended; a held gate is
 *     BUSY; cancel during the wait opens the gate */
#include <string.h>
#include "harness.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/platform.h"

#define SETTLE_MS CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS

static void not_saved(void *ctx)
{
    const xiaotai_args_t args = {.value = *(xiaotai_err_t *)ctx, .text = "audio.volume"};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, &args, NULL, 0);
}

static void finish(void *ctx) { fake_net_finish(*(xiaotai_err_t *)ctx); }

static uint32_t s_other;
static void other_quiesce(void *ctx)
{
    (void)ctx;
    (void)xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &s_other);
}
static void other_release(void *ctx)
{
    (void)ctx;
    xiaotai_quiesce_end(s_other);
}

static void stats(xiaotai_provisioning_stats_t *out) { xiaotai_provisioning_stats(out); }

/* The last event-log entry of this module carries label. */
static bool logged(const char *label)
{
    xiaotai_eventlog_entry_t entries[64];
    const uint32_t latest = xiaotai_eventlog_latest();
    const size_t count = xiaotai_eventlog_read(latest > 64 ? latest - 64 : 0, entries, 64);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].source, "provisioning") == 0 && strcmp(entries[i].label, label) == 0) return true;
    }
    return false;
}

static int refusals(void)
{
    xiaotai_provisioning_stats_t s;

    /* Busy: an activity is current and PREEMPT was not given. */
    fake_net_reset();
    CHECK(harness_start(20) == 0);
    CHECK(owner_activity_begin() != 0);
    CHECK(owner_begin(0) == XIAOTAI_ERR_BUSY);
    CHECK(!owner_quiesced() && fake_net.reprovisions == 0 && atomic_load(&revocations) == 0);
    CHECK(owner_begin(1u << 7) == XIAOTAI_ERR_INVALID_ARG);
    stats(&s);
    CHECK(s.begins_refused == 2 && s.begins == 0);
    CHECK(logged("begin.refused"));

    /* PREEMPT: the quiesce revokes the call first, then the request goes out. */
    trace_clear();
    CHECK(owner_begin(XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_OK);
    CHECK(wait_reprovisions(1, 1000));
    CHECK(atomic_load(&revocations) == 1 && fake_net.quiesced_at_reprovision && !fake_net.last_forget);
    const char *revoked = strstr(trace, "revoked:system");
    const char *quiesced = strstr(trace, "quiesce:1");
    CHECK(revoked && quiesced);
    /* Concurrent provisioning closes no gate. */
    CHECK(fake_gate.closes == 0);
    /* A second begin while the first is with the engine. */
    CHECK(owner_begin(XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_err_t ok = XIAOTAI_OK;
    on_owner(finish, &ok);
    CHECK(!owner_quiesced() && strstr(trace, "quiesce:0") != NULL);
    CHECK(phase() == XIAOTAI_PROVISIONING_WAITING);
    stats(&s);
    CHECK(s.begins == 1 && s.begins_failed == 0);

    /* Maintenance (somebody else's quiesce) refuses with BUSY. */
    fake_net.link.state = XIAOTAI_NET_UP;
    on_owner(other_quiesce, NULL);
    CHECK(owner_begin(XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_ERR_BUSY);
    on_owner(other_release, NULL);

    /* A network that takes no credentials. */
    fake_net.link.kind = XIAOTAI_NETWORK_ETHERNET;
    CHECK(owner_begin(0) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(fake_net.reprovisions == 1);
    harness_stop();
    return 0;
}

static int concurrent(void)
{
    xiaotai_provisioning_stats_t s;
    fake_net_reset();
    CHECK(harness_start(20) == 0);

    /* The bound runs out: counted, and the request goes out anyway. */
    atomic_store(&fake_settings_pending, true);
    const int64_t begun = now_ms();
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(phase() == XIAOTAI_PROVISIONING_STARTING && owner_quiesced());
    CHECK(wait_reprovisions(1, SETTLE_MS + 2000));
    const int64_t handed = now_ms() - begun;
    CHECK(handed >= SETTLE_MS && handed <= SETTLE_MS + 2 * 100 + 400);
    CHECK(fake_net.settings_pending_at_reprovision && fake_net.quiesced_at_reprovision);
    stats(&s);
    CHECK(s.flush_timeouts == 1 && s.begins == 1);
    CHECK(logged("settle.timeout"));
    xiaotai_err_t ok = XIAOTAI_OK;
    on_owner(finish, &ok);
    CHECK(!owner_quiesced() && phase() == XIAOTAI_PROVISIONING_WAITING);
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_OK && !status.restart_pending);

    /* A setting not saved is counted; the request waits for the rest. */
    harness_stop();
    fake_net_reset();
    CHECK(harness_start(20) == 0);
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    xiaotai_err_t storage = XIAOTAI_ERR_TIMEOUT;
    on_owner(not_saved, &storage);
    CHECK(fake_net.reprovisions == 0 && trace_count("failed:") == 0);
    atomic_store(&fake_settings_pending, false);
    CHECK(wait_reprovisions(1, 1000));
    CHECK(!fake_net.settings_pending_at_reprovision);
    stats(&s);
    CHECK(s.flush_failures == 1 && s.flush_timeouts == 0);

    /* The engine fails the request later: everything released, reported. */
    xiaotai_err_t failure = XIAOTAI_ERR_INVALID_STATE;
    trace_clear();
    on_owner(finish, &failure);
    CHECK(!owner_quiesced() && strstr(trace, "failed:begin:259 ") != NULL);
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_ERR_INVALID_STATE && status.phase == XIAOTAI_PROVISIONING_NONE);
    stats(&s);
    CHECK(s.begins_failed == 1);

    /* The engine refuses at once: released, reported, the user may begin again. */
    trace_clear();
    fake_net.link.control_pending = false;
    fake_net.reprovision_result = XIAOTAI_ERR_INVALID_STATE;
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(wait_reprovisions(2, 1000));
    drain();
    CHECK(!owner_quiesced() && strstr(trace, "failed:begin:259 ") != NULL);
    CHECK(strstr(trace, "quiesce:0") != NULL && phase() == XIAOTAI_PROVISIONING_NONE);
    stats(&s);
    CHECK(s.begins_failed == 2);
    fake_net.reprovision_result = XIAOTAI_OK;
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(wait_reprovisions(3, 1000));
    harness_stop();
    return 0;
}

static int exclusive(void)
{
    xiaotai_provisioning_stats_t s;
    xiaotai_provisioning_status_t status;

    /* Handed only once settings are written and the gate drained. */
    fake_net_reset();
    memset(&fake_gate, 0, sizeof(fake_gate));
    fake_net.exclusive_mode = true;
    CHECK(harness_start(20) == 0);
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(fake_gate.closed && fake_gate.closes == 1 && owner_quiesced());
    xiaotai_delay_ms(250);
    atomic_store(&fake_settings_pending, false);
    xiaotai_delay_ms(250);
    drain();
    CHECK(fake_net.reprovisions == 0);       /* the gate is not drained yet */
    fake_gate.drained = true;
    CHECK(wait_reprovisions(1, 1000));
    CHECK(fake_net.gate_closed_at_reprovision && fake_net.quiesced_at_reprovision);
    CHECK(!fake_net.settings_pending_at_reprovision);
    xiaotai_provisioning_status(&status);
    CHECK(status.restart_pending && status.phase == XIAOTAI_PROVISIONING_STARTING);
    harness_stop();

    /* The bound runs out (gate never drained): stop, gate open, quiesce ended. */
    fake_net_reset();
    memset(&fake_gate, 0, sizeof(fake_gate));
    fake_net.exclusive_mode = true;
    CHECK(harness_start(20) == 0);
    const int64_t begun = now_ms();
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_PROVISIONING_NONE, SETTLE_MS + 2000));
    const int64_t stopped = now_ms() - begun;
    CHECK(stopped >= SETTLE_MS && stopped <= SETTLE_MS + 2 * 100 + 400);
    drain();
    CHECK(fake_net.reprovisions == 0 && !fake_gate.closed && fake_gate.opens == 1 && !owner_quiesced());
    CHECK(strstr(trace, "failed:begin:263 ") != NULL);   /* XIAOTAI_ERR_TIMEOUT */
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_ERR_TIMEOUT && !status.restart_pending);
    stats(&s);
    CHECK(s.flush_timeouts == 1 && s.begins_failed == 0);

    /* A setting not saved stops at once. */
    trace_clear();
    fake_gate.drained = false;
    CHECK(owner_begin(0) == XIAOTAI_OK);
    xiaotai_err_t storage = XIAOTAI_FAIL;
    on_owner(not_saved, &storage);
    CHECK(fake_net.reprovisions == 0 && !fake_gate.closed && fake_gate.opens == 2 && !owner_quiesced());
    CHECK(strstr(trace, "failed:begin:-1 ") != NULL && phase() == XIAOTAI_PROVISIONING_NONE);
    stats(&s);
    CHECK(s.flush_failures == 1);

    /* The user begins again and this time it goes through. */
    fake_gate.drained = true;
    CHECK(owner_begin(XIAOTAI_PROVISIONING_FORGET) == XIAOTAI_OK);
    CHECK(wait_reprovisions(1, 1000));
    CHECK(fake_net.last_forget);
    harness_stop();

    /* The gate is held by somebody else: BUSY, the quiesce given back. */
    fake_net_reset();
    memset(&fake_gate, 0, sizeof(fake_gate));
    fake_net.exclusive_mode = true;
    fake_gate.close_result = XIAOTAI_ERR_PLATFORM_GATED;
    CHECK(harness_start(20) == 0);
    CHECK(owner_begin(0) == XIAOTAI_ERR_BUSY);
    CHECK(!owner_quiesced() && strstr(trace, "quiesce:1") && strstr(trace, "quiesce:0"));
    CHECK(fake_net.reprovisions == 0 && phase() == XIAOTAI_PROVISIONING_NONE);

    /* Cancel during the wait opens the gate and ends the quiesce. */
    fake_gate.close_result = XIAOTAI_OK;
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(fake_gate.closed && owner_quiesced());
    CHECK(owner_retry() == XIAOTAI_ERR_INVALID_STATE);   /* nothing to retry while a begin settles */
    CHECK(owner_cancel() == XIAOTAI_OK);
    CHECK(!fake_gate.closed && fake_gate.opens == 1 && !owner_quiesced() && phase() == XIAOTAI_PROVISIONING_NONE);
    CHECK(fake_net.closes == 0 && fake_net.reprovisions == 0);   /* nothing reached the engine */
    xiaotai_delay_ms(SETTLE_MS + 200);
    drain();
    CHECK(fake_net.reprovisions == 0 && trace_count("failed:") == 0);
    stats(&s);
    CHECK(s.cancels == 1);

    /* The engine refuses at once: the gate opens again. */
    fake_gate.drained = true;
    fake_net.reprovision_result = XIAOTAI_ERR_INVALID_STATE;
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(wait_reprovisions(1, 1000));
    drain();
    CHECK(!fake_gate.closed && fake_gate.opens == 2 && !owner_quiesced());
    harness_stop();
    return 0;
}

int main(void)
{
    if (refusals() || concurrent() || exclusive()) return 1;
    puts("mod_provisioning_begin: passed");
    return 0;
}
