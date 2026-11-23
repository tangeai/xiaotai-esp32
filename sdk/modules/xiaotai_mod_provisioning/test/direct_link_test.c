// SPDX-License-Identifier: MIT
#include <string.h>
#include "harness.h"

static uint32_t s_other;
static void take_maintenance(void *ctx)
{
    *(xiaotai_err_t *)ctx = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &s_other);
}

static void release_maintenance(void *ctx)
{
    (void)ctx;
    xiaotai_quiesce_end(s_other);
}

static void finish(void *ctx) { fake_net_finish(*(xiaotai_err_t *)ctx); }

int main(void)
{
    fake_net_reset();
    fake_net.exclusive_mode = true;
    CHECK(harness_start(100) == 0);
    CHECK(owner_begin(0) == XIAOTAI_ERR_INVALID_STATE && fake_net.reprovisions == 0);
    harness_stop();

    /* S3 and P4 share the same flow, with native and co-processor Wi-Fi. */
    for (unsigned p4 = 0; p4 < 2; ++p4) {
        fake_net_reset();
        fake_net.link.kind = p4 ? XIAOTAI_NETWORK_WIFI_COPROCESSOR : XIAOTAI_NETWORK_WIFI;
        CHECK(harness_start(100) == 0);

        xiaotai_err_t result = XIAOTAI_FAIL;
        on_owner(take_maintenance, &result);
        CHECK(result == XIAOTAI_OK);
        CHECK(owner_begin(XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_ERR_BUSY);
        CHECK(xiaotai_quiesced() && fake_net.reprovisions == 0);
        on_owner(release_maintenance, NULL);
        trace_clear();

        const uint32_t call = owner_activity_begin();
        CHECK(call != 0);
        CHECK(owner_begin(0) == XIAOTAI_ERR_BUSY);
        atomic_store(&fake_settings_pending, true);
        atomic_store(&probe_guard_blocked, true);
        /* External force keeps the orderly guard and cannot use direct link. */
        CHECK(command("AT+NET PROVISION FORCE") == XIAOTAI_ERR_BUSY);
        const xiaotai_args_t force = {.value = XIAOTAI_PROVISIONING_PREEMPT};
        CHECK(xiaotai_intent_post_external(XIAOTAI_INTENT_PROVISIONING_BEGIN, &force) == XIAOTAI_OK);
        drain();
        CHECK(atomic_load(&probe_guard_calls) == 3 && fake_net.reprovisions == 0);

        CHECK(owner_begin(XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_OK);
        CHECK(fake_net.reprovisions == 1 && !fake_net.last_forget);
        CHECK(fake_net.settings_pending_at_reprovision && !fake_net.quiesced_at_reprovision);
        CHECK(xiaotai_activity_current(NULL) == call && atomic_load(&revocations) == 0);
        CHECK(fake_gate.closes == 0 && atomic_load(&probe_guard_calls) == 3);
        CHECK(phase() == XIAOTAI_PROVISIONING_STARTING);
        result = XIAOTAI_FAIL;
        on_owner(finish, &result);
        CHECK(phase() == XIAOTAI_PROVISIONING_NONE && strstr(trace, "failed:begin:-1 "));
        CHECK(xiaotai_activity_current(NULL) == call);

        CHECK(owner_begin(XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_OK);
        result = XIAOTAI_OK;
        on_owner(finish, &result);
        CHECK(phase() == XIAOTAI_PROVISIONING_WAITING && fake_net.link.state == XIAOTAI_NET_HELD);
        CHECK(fake_net.link.user_disconnected && fake_net.provision.methods == XIAOTAI_PROVISION_PORTAL);
        CHECK(!xiaotai_restart_pending() && atomic_load(&restarts) == 0 && !xiaotai_quiesced());
        CHECK(trace_count("quiesce:") == 0 && atomic_load(&revocations) == 0);
        xiaotai_provisioning_stats_t stats;
        xiaotai_provisioning_stats(&stats);
        CHECK(stats.begins == 2 && stats.begins_failed == 1 && stats.flush_timeouts == 0);
        harness_stop();
    }

    puts("mod_provisioning_s3_p4_direct: passed");
    return 0;
}
