// SPDX-License-Identifier: MIT
/* mod_provisioning_options_continue: the options a product sets to start
 * over without looking at settings failures and to store failed credentials
 * again on a fixed period (-DCONFIG_XIAOTAI_PROVISIONING_BEGIN_FAILURE=2,
 * -DCONFIG_XIAOTAI_PROVISIONING_STORE_RETRY_MS=STORE_RETRY_MS), exclusive
 * provisioning with the platform core.
 *   - the factory gesture (FORGET | PREEMPT): a setting not saved or the bound
 *     running out is counted and the request goes out anyway, erasing
 *   - STORE_FAILED: the module stores again every STORE_RETRY_MS (absolute
 *     schedule), again after each failure, and stops once storing goes on */
#include <string.h>
#include "harness.h"

#define SETTLE_MS CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS
#define STORE_RETRY_MS CONFIG_XIAOTAI_PROVISIONING_STORE_RETRY_MS

static void not_saved(void *ctx)
{
    const xiaotai_args_t args = {.value = *(xiaotai_err_t *)ctx, .text = "audio.volume"};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, &args, NULL, 0);
}

static void provision(void *ctx)
{
    fake_net_provision(*(xiaotai_net_provision_state_t *)ctx, XIAOTAI_FAIL);
}

static unsigned store_retries(void)
{
    drain();
    return fake_net.store_retries;
}

static bool wait_store_retries(unsigned count, uint32_t timeout_ms)
{
    const int64_t end = now_ms() + timeout_ms;
    while (store_retries() < count) {
        if (now_ms() > end) return false;
        xiaotai_delay_ms(5);
    }
    return true;
}

int main(void)
{
    xiaotai_provisioning_stats_t stats;

    /* A setting not saved: counted, the request goes out (erasing). */
    fake_net_reset();
    memset(&fake_gate, 0, sizeof(fake_gate));
    fake_gate.drained = true;
    fake_net.exclusive_mode = true;
    CHECK(harness_start(20) == 0);
    CHECK(owner_activity_begin() != 0);
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(XIAOTAI_PROVISIONING_FORGET | XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_OK);
    CHECK(atomic_load(&revocations) == 1);
    xiaotai_err_t storage = XIAOTAI_FAIL;
    on_owner(not_saved, &storage);
    CHECK(trace_count("failed:") == 0 && fake_gate.closed && owner_quiesced());
    /* The bound still applies: the request goes out when it runs out. */
    CHECK(wait_reprovisions(1, SETTLE_MS + 2000));
    CHECK(fake_net.last_forget && fake_net.settings_pending_at_reprovision && fake_net.gate_closed_at_reprovision);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.flush_failures == 1 && stats.flush_timeouts == 1 && stats.begins_failed == 0);
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_OK && status.restart_pending);
    harness_stop();

    /* Store failures are stored again on the period. */
    fake_net_reset();
    fake_net.provisioning_boot = true;
    fake_net.exclusive_mode = true;
    fake_net.provision.state = XIAOTAI_NET_PROVISION_OPEN;
    CHECK(harness_start(20) == 0);
    xiaotai_net_provision_state_t state = XIAOTAI_NET_PROVISION_STORING;
    on_owner(provision, &state);
    state = XIAOTAI_NET_PROVISION_STORE_FAILED;
    const int64_t failed = now_ms();
    on_owner(provision, &state);
    CHECK(store_retries() == 0);
    CHECK(wait_store_retries(1, STORE_RETRY_MS + 2000));
    const int64_t first = now_ms() - failed;
    CHECK(first >= STORE_RETRY_MS && first <= STORE_RETRY_MS + 400);
    /* The engine stores again and fails again: another period. */
    state = XIAOTAI_NET_PROVISION_STORING;
    on_owner(provision, &state);
    state = XIAOTAI_NET_PROVISION_STORE_FAILED;
    const int64_t again = now_ms();
    on_owner(provision, &state);
    CHECK(wait_store_retries(2, STORE_RETRY_MS + 2000));
    const int64_t second = now_ms() - again;
    CHECK(second >= STORE_RETRY_MS && second <= STORE_RETRY_MS + 400);
    CHECK(trace_count("failed:store:-1 ") == 2);
    /* Storing goes on: no further retry. */
    state = XIAOTAI_NET_PROVISION_STORING;
    on_owner(provision, &state);
    xiaotai_delay_ms(STORE_RETRY_MS + 200);
    CHECK(store_retries() == 2);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.store_failures == 2 && stats.retries == 2);
    harness_stop();
    puts("mod_provisioning_options_continue: passed");
    return 0;
}
