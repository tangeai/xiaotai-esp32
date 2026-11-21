// SPDX-License-Identifier: MIT
#include "harness.h"
#include "xiaotai/modules/provisioning_config.h"

static atomic_int_fast64_t now_us = 1000000;

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now_us); }

static void advance(unsigned ms)
{
    atomic_fetch_add(&now_us, (int64_t)ms * 1000);
    drain();
}

static void finish_http(void *ctx)
{
    (void)ctx;
    fake_gate.drained = true;
}

int main(void)
{
    fake_net_reset();
    fake_net.exclusive_mode = true;
    CHECK(harness_start(20) == 0);
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(owner_begin(0) == XIAOTAI_ERR_INVALID_STATE && fake_gate.closes == 1);

    advance(5100); /* Accepted settings and HTTP are healthy but not settled. */
    xiaotai_provisioning_stats_t stats;
    xiaotai_provisioning_stats(&stats);
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    const bool bounded = XIAOTAI_PROVISIONING_SETTLE_WAIT_MS != 0;
    CHECK(stats.flush_timeouts == (bounded ? 1U : 0U));
    CHECK(status.error == (bounded ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK));
    CHECK(fake_gate.closed && owner_quiesced() && fake_net.reprovisions == 0);

    atomic_store(&fake_settings_pending, false);
    advance(100);
    CHECK(fake_net.reprovisions == 0);
    on_owner(finish_http, NULL);
    advance(100);
    if (bounded) {
        CHECK(fake_net.reprovisions == 0 && atomic_load(&restarts) == 0);
        CHECK(owner_cancel() == XIAOTAI_OK && !fake_gate.closed && !owner_quiesced());
    } else {
        CHECK(fake_net.reprovisions == 1 && !fake_net.last_forget);
        CHECK(fake_net.quiesced_at_reprovision && fake_net.gate_closed_at_reprovision);
        CHECK(!fake_net.settings_pending_at_reprovision);
    }
    harness_stop();
    return 0;
}
