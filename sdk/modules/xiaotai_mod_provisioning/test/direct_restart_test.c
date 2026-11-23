// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>
#include "harness.h"

static xiaotai_err_t s_fallback;

static void ordinary_fallback(void *ctx)
{
    (void)ctx;
    s_fallback = xiaotai_restart_request("network", 0);
}

static void finish(void *ctx)
{
    const xiaotai_err_t result = *(xiaotai_err_t *)ctx;
    fake_net_finish(result);
    if (result == XIAOTAI_OK) (void)xiaotai_run(ordinary_fallback, NULL);
}

static void store(void *ctx)
{
    fake_net_provision(*(xiaotai_net_provision_state_t *)ctx, XIAOTAI_OK);
}

static void done(void *ctx)
{
    (void)ctx;
    fake_net.provision.restart_required = true;
    fake_net_provision(XIAOTAI_NET_PROVISION_DONE, XIAOTAI_OK);
    (void)xiaotai_run(ordinary_fallback, NULL);
}

static int boot(bool provisioning_boot)
{
    fake_net_reset();
    memset(&fake_gate, 0, sizeof(fake_gate));
    fake_net.exclusive_mode = true;
    fake_net.provisioning_boot = provisioning_boot;
    if (provisioning_boot) fake_net.provision.state = XIAOTAI_NET_PROVISION_OPEN;
    s_fallback = XIAOTAI_FAIL;
    if (harness_start(100) != 0) return 1;
    atomic_store(&probe_guard_blocked, true);
    atomic_store(&fake_settings_pending, true);
    return 0;
}

static int await_restart(uint32_t call)
{
    CHECK(xiaotai_restart_pending() && s_fallback == XIAOTAI_OK);
    CHECK(framework_holds() == 1 && trace_count("restarting:") == 0);
    CHECK(atomic_load(&restarts) == 0 && !xiaotai_quiesced());
    const int64_t released_us = xiaotai_time_us();
    xiaotai_provisioning_restart_release();
    CHECK(wait_phase(XIAOTAI_PROVISIONING_RESTARTING, 1500));
    for (int i = 0; i < 200 && atomic_load(&restarts) == 0; ++i) xiaotai_delay_ms(5);
    CHECK(atomic_load(&restarts) == 1 && trace_count("restarting:provisioning ") == 1);
    CHECK(atomic_load(&probe_restart_off_owner));
    CHECK(atomic_load(&probe_restarting_us) >= released_us);
    CHECK(atomic_load(&probe_restart_us) >= released_us + 100000);
    /* RESTARTING is dispatched after the framework arms its delay. */
    CHECK(atomic_load(&probe_restart_us) >= atomic_load(&probe_restarting_us) + 95000);
    CHECK(!xiaotai_quiesced() && trace_count("quiesce:") == 0 && atomic_load(&probe_guard_calls) == 0);
    CHECK(xiaotai_activity_current(NULL) == call && atomic_load(&revocations) == 0);
    CHECK(atomic_load(&probe_settings_flushes) == 0);
    CHECK(fake_gate.closes == 0 && fake_gate.opens == 0);
    return 0;
}

static int local_reset(void)
{
    CHECK(boot(false) == 0);
    CHECK(command("AT+NET PROVISION FORGET FORCE") == XIAOTAI_ERR_BUSY);
    const xiaotai_args_t args = {.value = XIAOTAI_PROVISIONING_PREEMPT};
    CHECK(xiaotai_intent_post_external(XIAOTAI_INTENT_PROVISIONING_FORGET, &args) == XIAOTAI_ERR_NOT_ALLOWED);
    CHECK(fake_net.reprovisions == 0 && atomic_exchange(&probe_guard_calls, 0) == 1);
    const uint32_t call = owner_activity_begin();
    CHECK(call != 0);

    /* The local app holds for confirmation only after a successful commit. */
    xiaotai_provisioning_restart_hold();
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_PROVISIONING_FORGET, &args) == XIAOTAI_OK);
    drain();
    CHECK(fake_net.reprovisions == 1 && fake_net.last_forget && phase() == XIAOTAI_PROVISIONING_STARTING);
    CHECK(fake_net.settings_pending_at_reprovision && !fake_net.quiesced_at_reprovision);
    CHECK(!xiaotai_restart_pending() && atomic_load(&revocations) == 0);
    xiaotai_err_t result = XIAOTAI_FAIL;
    on_owner(finish, &result);
    CHECK(phase() == XIAOTAI_PROVISIONING_NONE && trace_count("failed:begin:-1 ") == 1);
    xiaotai_provisioning_restart_release();
    CHECK(!xiaotai_restart_pending() && framework_holds() == 0);

    xiaotai_provisioning_restart_hold();
    CHECK(owner_begin(XIAOTAI_PROVISIONING_FORGET | XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_OK);
    CHECK(fake_net.reprovisions == 2 && fake_net.last_forget && !xiaotai_restart_pending());
    result = XIAOTAI_OK;
    on_owner(finish, &result);
    CHECK(owner_cancel() == XIAOTAI_ERR_INVALID_STATE && fake_net.closes == 0);
    CHECK(await_restart(call) == 0);
    harness_stop();
    return 0;
}

static int credentials_stored(void)
{
    CHECK(boot(true) == 0);
    atomic_store(&probe_hold_on_done, true);
    xiaotai_net_provision_state_t state = XIAOTAI_NET_PROVISION_STORING;
    on_owner(store, &state);
    CHECK(framework_holds() == 1 && !xiaotai_restart_pending());
    state = XIAOTAI_NET_PROVISION_STORE_FAILED;
    on_owner(store, &state);
    CHECK(framework_holds() == 0 && !xiaotai_restart_pending() && fake_net.store_retries == 0);
    CHECK(owner_retry() == XIAOTAI_OK && fake_net.store_retries == 1);
    state = XIAOTAI_NET_PROVISION_STORING;
    on_owner(store, &state);
    on_owner(done, NULL);
    CHECK(trace_count("done:1:held=1 ") == 1);
    /* A repeated notice must not replay confirmation or acquire a second hold. */
    on_owner(done, NULL);
    CHECK(trace_count("done:1:held=1 ") == 1);
    CHECK(await_restart(0) == 0);
    CHECK(strstr(trace, "done:1") < strstr(trace, "restarting:provisioning"));
    atomic_store(&probe_hold_on_done, false);
    harness_stop();
    return 0;
}

static void take_maintenance(void *ctx)
{
    *(xiaotai_err_t *)ctx = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &(uint32_t){0});
}

static int rejected_handoff(bool portal)
{
    CHECK(boot(portal) == 0);
    if (!portal) {
        CHECK(owner_begin(XIAOTAI_PROVISIONING_FORGET | XIAOTAI_PROVISIONING_PREEMPT) == XIAOTAI_OK);
    }
    /* Competing maintenance takes the framework ticket after request
     * admission but before the engine publishes the durable completion. */
    atomic_store(&probe_guard_blocked, false);
    xiaotai_err_t result = XIAOTAI_FAIL;
    on_owner(take_maintenance, &result);
    CHECK(result == XIAOTAI_OK);
    if (portal) on_owner(done, NULL);
    else on_owner(finish, &result);
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.provision.restart_required && !status.restart_pending && status.error == XIAOTAI_ERR_BUSY);
    CHECK(trace_count("failed:restart:") == 1 && trace_count("failed:store:") == 0 && s_fallback == XIAOTAI_ERR_BUSY);
    CHECK(!xiaotai_restart_pending() && owner_quiesced() && atomic_load(&restarts) == 0);
    if (portal) {
        CHECK(status.phase == XIAOTAI_PROVISIONING_DONE && trace_count("done:0:held=1 ") == 1);
        on_owner(done, NULL);
        CHECK(trace_count("done:0:held=1 ") == 1 && trace_count("failed:restart:") == 1);
    }
    CHECK(owner_cancel() == XIAOTAI_ERR_INVALID_STATE && owner_retry() == XIAOTAI_ERR_INVALID_STATE);
    harness_stop();
    return 0;
}

static unsigned store_successes, store_failures;

static void observe_store(xiaotai_err_t error, bool restart, void *ctx)
{
    (void)ctx;
    if (!xiaotai_on_owner()) abort();
    if (error == XIAOTAI_OK) {
        if (!restart || !xiaotai_restart_pending()) abort();
        ++store_successes;
        xiaotai_provisioning_restart_hold();
    } else {
        if (restart || error != XIAOTAI_ERR_TIMEOUT) abort();
        ++store_failures;
    }
}

static void failed_store(void *ctx)
{
    (void)ctx;
    fake_net_provision(XIAOTAI_NET_PROVISION_STORE_FAILED, XIAOTAI_ERR_TIMEOUT);
}

static int store_under_pressure(void)
{
    probe_store_observer = observe_store;
    CHECK(boot(true) == 0);
    CHECK(xiaotai_provisioning_observe_store(observe_store, NULL) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_provision_state_t state = XIAOTAI_NET_PROVISION_STORING;
    on_owner(store, &state);
    CHECK(on_owner_with_store_pressure(XIAOTAI_NET_PROVISION_STORE_FAILED, failed_store, NULL));
    CHECK(store_failures == 1 && store_successes == 0 && trace_count("failed:store:") == 0);
    CHECK(framework_holds() == 0 && !xiaotai_restart_pending());
    CHECK(owner_retry() == XIAOTAI_OK && fake_net.store_retries == 1);
    on_owner(store, &state);
    CHECK(on_owner_with_store_pressure(XIAOTAI_NET_PROVISION_DONE, done, NULL));
    CHECK(store_failures == 1 && store_successes == 1 && trace_count("done:") == 0);
    CHECK(xiaotai_restart_pending() && atomic_load(&restarts) == 0 && !xiaotai_quiesced());
    for (unsigned i = 0; i < 100 && framework_holds() != 1; ++i) xiaotai_delay_ms(5);
    CHECK(await_restart(0) == 0);
    harness_stop();
    probe_store_observer = NULL;
    return 0;
}

int main(void)
{
    if (local_reset() || credentials_stored() || rejected_handoff(false) || rejected_handoff(true) ||
        store_under_pressure()) return 1;
    puts("mod_provisioning_mz28_direct: passed");
    return 0;
}
