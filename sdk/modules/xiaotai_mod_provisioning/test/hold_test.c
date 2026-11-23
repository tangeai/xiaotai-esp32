// SPDX-License-Identifier: MIT
/* mod_provisioning_hold: the restart hold after stored credentials
 * (specification 2.4), in a provisioning boot.
 *   - STORING takes the module's hold (a framework hold); DONE is emitted
 *     while it is still held, so the application's handler can take its own
 *     hold (here through xiaotai_provisioning_restart_hold, which is the
 *     framework's hold)
 *   - the module releases once settings are written; the restart waits for
 *     the application's hold too, then the framework announces it and the
 *     phase becomes RESTARTING
 *   - settings never written: released at the bound (absolute schedule),
 *     counted
 *   - STORE_FAILED gives the hold back; DONE without a restart (the link
 *     kept) releases too and nothing restarts
 *   - the module allocates nothing on the way (host memory counters). */
#include <string.h>
#include "harness.h"
#include "xiaotai_mem_host.h"

#define SETTLE_MS CONFIG_XIAOTAI_PROVISIONING_SETTLE_WAIT_MS

static void request_restart(void *ctx)
{
    (void)ctx;
    (void)xiaotai_restart_request("network", 0);
}

/* The engine stores and promises the restart, as its worker does. */
static void store(void *ctx)
{
    (void)ctx;
    fake_net.provision.restart_required = false;
    fake_net_provision(XIAOTAI_NET_PROVISION_STORING, XIAOTAI_OK);
}

static void done(void *ctx)
{
    const bool restart = *(bool *)ctx;
    fake_net.provision.restart_required = restart;
    fake_net_provision(XIAOTAI_NET_PROVISION_DONE, XIAOTAI_OK);
    if (restart) (void)xiaotai_run(request_restart, NULL);
}

static void store_failed(void *ctx)
{
    (void)ctx;
    fake_net_provision(XIAOTAI_NET_PROVISION_STORE_FAILED, XIAOTAI_ERR_TIMEOUT);
}

static void closed(void *ctx)
{
    (void)ctx;
    fake_net_provision(XIAOTAI_NET_PROVISION_CLOSED, XIAOTAI_OK);
}

static bool held(void)
{
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    return status.holding_restart;
}

static int boot(void)
{
    fake_net_reset();
    fake_net.provisioning_boot = true;
    fake_net.exclusive_mode = true;
    fake_net.provision.state = XIAOTAI_NET_PROVISION_OPEN;
    fake_net.provision.exclusive = true;
    fake_net.provision.reason = XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED;
    return harness_start(20);
}

int main(void)
{
    xiaotai_provisioning_stats_t stats;
    bool restart = true;

    /* Settings written after DONE; the application holds as well. */
    CHECK(boot() == 0);
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    CHECK(status.exclusive && status.phase == XIAOTAI_PROVISIONING_WAITING);
    atomic_store(&probe_hold_on_done, true);
    atomic_store(&fake_settings_pending, true);
    xiaotai_mem_host_clear();
    on_owner(store, &restart);
    CHECK(held() && framework_holds() == 1 && phase() == XIAOTAI_PROVISIONING_STORING);
    on_owner(done, &restart);
    CHECK(strstr(trace, "done:1:held=1 ") != NULL);
    CHECK(framework_holds() == 2 && held());
    xiaotai_delay_ms(300);
    drain();
    CHECK(atomic_load(&restarts) == 0 && trace_count("restarting") == 0);
    atomic_store(&fake_settings_pending, false);
    xiaotai_delay_ms(2 * 100 + 50);
    drain();
    CHECK(!held() && framework_holds() == 1);
    CHECK(atomic_load(&restarts) == 0 && trace_count("restarting") == 0);   /* the application still holds */
    CHECK(xiaotai_mem_host_outstanding() == 0);   /* nothing kept by the module */
    xiaotai_provisioning_restart_release();
    CHECK(wait_phase(XIAOTAI_PROVISIONING_RESTARTING, 1000));
    for (int i = 0; i < 100 && atomic_load(&restarts) == 0; ++i) xiaotai_delay_ms(5);
    CHECK(atomic_load(&restarts) == 1 && strstr(trace, "restarting:network ") != NULL);
    /* DONE came before the restart announcement. */
    CHECK(strstr(trace, "done:1") < strstr(trace, "restarting:network"));
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.stored == 1 && stats.flush_timeouts == 0);
    atomic_store(&probe_hold_on_done, false);
    harness_stop();

    /* Settings never written: the module lets go at the bound. */
    CHECK(boot() == 0);
    atomic_store(&fake_settings_pending, true);
    on_owner(store, &restart);
    const int64_t stored = now_ms();
    on_owner(done, &restart);
    CHECK(wait_phase(XIAOTAI_PROVISIONING_RESTARTING, SETTLE_MS + 2000));
    const int64_t released = now_ms() - stored;
    CHECK(released >= SETTLE_MS && released <= SETTLE_MS + 2 * 100 + 400);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.flush_timeouts == 1);
    harness_stop();

    /* STORE_FAILED gives the hold back (a close may ask for a restart). */
    CHECK(boot() == 0);
    on_owner(store, &restart);
    CHECK(framework_holds() == 1);
    on_owner(store_failed, NULL);
    CHECK(!held() && framework_holds() == 0 && strstr(trace, "failed:store:263 ") != NULL);
    xiaotai_provisioning_stats(&stats);
    CHECK(stats.store_failures == 1);
    harness_stop();

    /* DONE without a restart: released, nothing restarts. */
    CHECK(boot() == 0);
    restart = false;
    on_owner(store, &restart);
    on_owner(done, &restart);
    CHECK(strstr(trace, "done:0:held=1 ") != NULL);
    drain();
    CHECK(!held() && framework_holds() == 0);
    on_owner(closed, NULL);
    xiaotai_delay_ms(100);
    drain();
    CHECK(atomic_load(&restarts) == 0 && phase() == XIAOTAI_PROVISIONING_NONE);
    harness_stop();
    puts("mod_provisioning_hold: passed");
    return 0;
}
