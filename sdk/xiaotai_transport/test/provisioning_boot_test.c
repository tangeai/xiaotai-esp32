// SPDX-License-Identifier: MIT
/* transport_provisioning_boot: in a boot the network engine reserves for
 * provisioning the transport takes nothing (no worker, no memory, no start
 * reserve, no stack) and stays DOWN with WAIT_PROVISIONING for the whole
 * boot, while the calls modules make at init and start (requirements, holds)
 * are accepted as in a normal boot. */
#include "support.h"

static void test_provisioning_boot(void)
{
    static const xiaotai_transport_requirement_t need = {.links = 2, .command_pool_bytes = 16384};
    const xiaotai_transport_config_t config = {.start_reserve_bytes = 20480,
                                               .connect_reserve_bytes = 17408,
                                               .start_before_signaling = true};
    fakes_reset();
    fake_net_provisioning(true);
    /* Everything a normal boot would start with is there. */
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    fake_platform_endpoint(XIAOTAI_OK, TEST_ENDPOINT);
    fake_net_set(XIAOTAI_NET_CONNECTING, true);
    g_requirement = &need;
    g_observed.measure_start = true;
    boot_with(&config, (boot_options_t){.with_requiring = true, .keep_fakes = true});

    /* The requirement of a module's init was accepted. */
    CHECK(g_requirement_result == XIAOTAI_OK);
    /* start() allocated nothing and created no worker. */
    CHECK(g_observed.start_allocations == 0U && g_observed.start_outstanding == 0);
    CHECK(!atomic_load(&xt.worker_running) && !xt.jobs_ready && xt.slots == NULL);
    xiaotai_transport_status_t s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_PROVISIONING);
    CHECK(s.engine_bytes == 0U && s.command_pool_bytes == 0U && !s.start_reserve_held);
    /* No signaling hold either: the platform is not the transport's to stop. */
    CHECK(fake_platform_hold_records(NULL, 0) == 0U);

    /* The network's start does not take the start reserve. */
    xiaotai_mem_host_clear();
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_UP);
    emit(XIAOTAI_EVENT_NET_UP, 1);
    emit(XIAOTAI_EVENT_PLATFORM_DISCOVERED, 0);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0);
    xiaotai_transport_host_clock_advance(60000);
    settle();
    CHECK(xiaotai_mem_host_allocations() == 0U);
    s = status_now();
    CHECK(s.state == XIAOTAI_TRANSPORT_DOWN && s.wait == XIAOTAI_TRANSPORT_WAIT_PROVISIONING);
    CHECK(!s.start_reserve_held && s.engine_bytes == 0U);
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 0U && xiaotai_transport_host_calls(NULL, 0) == 0U);
    CHECK(g_observed.state_count == 0U);

    /* Links are refused; holds are accepted; restart has nothing to do. */
    CHECK(xt_link_admissible() == XIAOTAI_ERR_INVALID_STATE);
    uint32_t hold = 0;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK && hold != 0U);
    CHECK(status_now().holds == 1U);
    xiaotai_transport_release(hold);
    CHECK(status_now().holds == 0U);
    CHECK(xiaotai_transport_restart() == XIAOTAI_ERR_INVALID_STATE);
    /* Requirements are closed once start() ran, as in a normal boot. */
    CHECK(xiaotai_transport_require(&need) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_mem_host_allocations() == 0U);
    shutdown_all();
}

/* The same product in a normal boot takes what the provisioning boot left. */
static void test_normal_boot_contrast(void)
{
    const xiaotai_transport_config_t config = {.start_reserve_bytes = 20480};
    fakes_reset();
    fake_net_set(XIAOTAI_NET_CONNECTING, false);
    g_observed.measure_start = true;
    boot_with(&config, (boot_options_t){.keep_fakes = true});
    /* Link table, ring, command slots, command reservation, job queue and start reserve. */
    CHECK(g_observed.start_allocations == 6U);
    CHECK(atomic_load(&xt.worker_running));
    CHECK(status_now().start_reserve_held && status_now().engine_bytes > 20480U);
    shutdown_all();
}

int main(void)
{
    test_provisioning_boot();
    test_normal_boot_contrast();
    printf("transport_provisioning_boot: all checks passed\n");
    return 0;
}
