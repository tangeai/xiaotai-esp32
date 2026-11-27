// SPDX-License-Identifier: MIT
/* mod_provisioning_options_hold: the options a product sets to keep a begin
 * held after a setting failure and to wait without a bound
 * (-DCONFIG_XIAOTAI_PROVISIONING_BEGIN_FAILURE=1,
 * -DCONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS=0,
 * -DCONFIG_XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD=1), exclusive
 * provisioning with the platform core. Ported from
 * tests/xmini/test_runtime_wifi.c (setup with pending settings, waiting for
 * the HTTP drain, cancel before the restart, settings failure held until an
 * explicit cancel, a new setup after it, quiesce and gate refusals, the setup
 * request failing) and tests/xmini/test_product.c
 * (test_portal_restart_pending_preferences,
 * test_portal_preferences_failure_explicit_retry: the restart after stored
 * credentials waits for the settings, a failure keeps it back until retry). */
#include <string.h>
#include "harness.h"

static void not_saved(void *ctx)
{
    const xiaotai_args_t args = {.value = *(xiaotai_err_t *)ctx, .text = "audio.volume"};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, &args, NULL, 0);
}

static void request_restart(void *ctx)
{
    (void)ctx;
    (void)xiaotai_restart_request("network", 0);
}

static void stored(void *ctx)
{
    (void)ctx;
    fake_net_provision(XIAOTAI_NET_PROVISION_STORING, XIAOTAI_OK);
    fake_net.provision.restart_required = true;
    fake_net_provision(XIAOTAI_NET_PROVISION_DONE, XIAOTAI_OK);
    (void)xiaotai_run(request_restart, NULL);
}

static int start(void)
{
    fake_net_reset();
    memset(&fake_gate, 0, sizeof(fake_gate));
    fake_gate.drained = true;
    fake_net.exclusive_mode = true;
    return harness_start(20);
}

static void read_cancellable(void *ctx) { *(bool *)ctx = xiaotai_provisioning_begin_cancellable(); }

static bool cancellable(void)
{
    bool value = false;
    on_owner(read_cancellable, &value);
    return value;
}

static int setup(void)
{
    /* test_setup_pending_settings: the request waits for the settings, as
     * long as they take (no bound), then goes out once. */
    CHECK(start() == 0);
    CHECK(!cancellable());
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(owner_begin(0) == XIAOTAI_ERR_INVALID_STATE);   /* one setup at a time */
    xiaotai_delay_ms(1200);   /* beyond the default bound of the other products' tests */
    drain();
    CHECK(fake_net.reprovisions == 0 && owner_quiesced() && fake_gate.closed && trace_count("failed:") == 0);
    CHECK(cancellable());
    atomic_store(&fake_settings_pending, false);
    CHECK(wait_reprovisions(1, 1000));
    CHECK(!cancellable());
    CHECK(!fake_net.last_forget);   /* the setup keeps the stored credentials */
    harness_stop();

    /* Waiting for the HTTP drain; a second setup refused; cancel releases
     * everything and nothing restarts; a new setup goes through. */
    CHECK(start() == 0);
    fake_gate.drained = false;
    CHECK(owner_begin(0) == XIAOTAI_OK);
    xiaotai_delay_ms(300);
    drain();
    CHECK(fake_net.reprovisions == 0 && phase() == XIAOTAI_PROVISIONING_STARTING);
    CHECK(owner_begin(0) == XIAOTAI_ERR_INVALID_STATE && fake_gate.closes == 1);
    CHECK(owner_cancel() == XIAOTAI_OK);
    CHECK(!fake_gate.closed && !owner_quiesced() && phase() == XIAOTAI_PROVISIONING_NONE);
    CHECK(fake_net.reprovisions == 0 && atomic_load(&restarts) == 0);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    fake_gate.drained = true;
    CHECK(wait_reprovisions(1, 1000));
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_OK && status.restart_pending);
    harness_stop();
    return 0;
}

static int held_failure(void)
{
    /* A setting cannot be written: held, quiesced and gated, until cancel. */
    CHECK(start() == 0);
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    xiaotai_err_t storage = XIAOTAI_FAIL;
    on_owner(not_saved, &storage);
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.phase == XIAOTAI_PROVISIONING_STARTING && status.error == XIAOTAI_FAIL);
    CHECK(strstr(trace, "failed:begin:-1 ") != NULL);
    CHECK(fake_gate.closed && owner_quiesced());
    CHECK(cancellable());
    atomic_store(&fake_settings_pending, false);   /* written later: still held */
    xiaotai_delay_ms(400);
    drain();
    CHECK(fake_net.reprovisions == 0 && fake_gate.closed && owner_quiesced() && atomic_load(&restarts) == 0);
    CHECK(owner_begin(0) == XIAOTAI_ERR_INVALID_STATE && fake_gate.closes == 1);
    CHECK(owner_retry() == XIAOTAI_ERR_INVALID_STATE);
    CHECK(owner_cancel() == XIAOTAI_OK);
    CHECK(!fake_gate.closed && fake_gate.opens == 1 && !owner_quiesced() && phase() == XIAOTAI_PROVISIONING_NONE);
    CHECK(!cancellable());
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_OK);
    /* A new setup waits again and goes through once written. */
    atomic_store(&fake_settings_pending, true);
    CHECK(owner_begin(0) == XIAOTAI_OK);
    xiaotai_delay_ms(200);
    drain();
    CHECK(fake_net.reprovisions == 0);
    atomic_store(&fake_settings_pending, false);
    CHECK(wait_reprovisions(1, 1000));
    harness_stop();

    /* Refusals release what was taken: busy (no FORCE), the gate held. */
    CHECK(start() == 0);
    CHECK(owner_activity_begin() != 0);
    CHECK(owner_begin(0) == XIAOTAI_ERR_BUSY && fake_gate.closes == 0 && !owner_quiesced());
    harness_stop();
    CHECK(start() == 0);
    fake_gate.close_result = XIAOTAI_FAIL;
    CHECK(owner_begin(0) == XIAOTAI_ERR_BUSY && !owner_quiesced());
    harness_stop();

    /* The setup request fails: released, the error kept, no restart. */
    CHECK(start() == 0);
    fake_net.reprovision_result = XIAOTAI_FAIL;
    CHECK(owner_begin(0) == XIAOTAI_OK);
    CHECK(wait_reprovisions(1, 1000));
    drain();
    CHECK(!fake_gate.closed && !owner_quiesced() && atomic_load(&restarts) == 0);
    xiaotai_provisioning_status(&status);
    CHECK(status.error == XIAOTAI_FAIL && !status.restart_pending);
    harness_stop();
    return 0;
}

static int stored_credentials(void)
{
    /* test_portal_restart_pending_preferences: stored, the restart waits for
     * the settings (no bound), then goes ahead. */
    fake_net_reset();
    fake_net.provisioning_boot = true;
    fake_net.exclusive_mode = true;
    fake_net.provision.state = XIAOTAI_NET_PROVISION_OPEN;
    CHECK(harness_start(20) == 0);
    atomic_store(&fake_settings_pending, true);
    on_owner(stored, NULL);
    xiaotai_delay_ms(1200);
    drain();
    CHECK(atomic_load(&restarts) == 0 && framework_holds() == 1);
    atomic_store(&fake_settings_pending, false);
    CHECK(wait_phase(XIAOTAI_PROVISIONING_RESTARTING, 2000));
    for (int i = 0; i < 100 && atomic_load(&restarts) == 0; ++i) xiaotai_delay_ms(5);
    CHECK(atomic_load(&restarts) == 1);
    harness_stop();

    /* test_portal_preferences_failure_explicit_retry: a setting fails after
     * storing; no restart however long; retry waits again and the restart
     * follows once the settings are written. */
    fake_net_reset();
    fake_net.provisioning_boot = true;
    fake_net.exclusive_mode = true;
    fake_net.provision.state = XIAOTAI_NET_PROVISION_OPEN;
    CHECK(harness_start(20) == 0);
    atomic_store(&fake_settings_pending, true);
    on_owner(stored, NULL);
    xiaotai_err_t storage = XIAOTAI_FAIL;
    on_owner(not_saved, &storage);
    CHECK(strstr(trace, "failed:store:-1 ") != NULL);
    atomic_store(&fake_settings_pending, false);
    xiaotai_delay_ms(500);
    drain();
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(atomic_load(&restarts) == 0 && status.holding_restart && status.error == XIAOTAI_FAIL);
    CHECK(status.phase == XIAOTAI_PROVISIONING_DONE);
    CHECK(owner_retry() == XIAOTAI_OK);
    CHECK(fake_net.store_retries == 0 && fake_net.retries == 0);   /* nothing for the engine */
    CHECK(wait_phase(XIAOTAI_PROVISIONING_RESTARTING, 2000));
    for (int i = 0; i < 100 && atomic_load(&restarts) == 0; ++i) xiaotai_delay_ms(5);
    CHECK(atomic_load(&restarts) == 1);
    xiaotai_provisioning_stats_t stats;
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.flush_failures == 1 && stats.retries == 1);
    harness_stop();
    return 0;
}

int main(void)
{
    if (setup() || held_failure() || stored_credentials()) return 1;
    puts("mod_provisioning_options_hold: passed");
    return 0;
}
