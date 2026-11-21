// SPDX-License-Identifier: MIT
// Replay address loss across the real ESP adapter, worker and public snapshot.
#include "internal.h"
#include "wifi_worker_fake.h"
#include "xiaotai/port/os.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

static const xiaotai_network_t part = {.desc.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR};
static xiaotai_signal_t owner_barrier;
static _Atomic int64_t now_us = 1000000;

int64_t __wrap_xiaotai_time_us(void) {return atomic_load(&now_us);}

const xiaotai_network_t *xiaotai_hal_network(size_t index)
{
    return index == 0 ? &part : NULL;
}

static const xiaotai_module_t module = {
    .name = "net", .needs = XIAOTAI_NEED_NETWORK, .init = xiaotai_engine_net_init, .start = xiaotai_engine_net_start
};
static const xiaotai_module_t *const modules[] = {&module};
static const xiaotai_framework_config_t config = {
    .modules = modules, .module_count = 1, .available_needs = XIAOTAI_NEED_NETWORK
};

static void owner_drained(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&owner_barrier);
}

static xiaotai_err_t seed_credentials(net_call_t *call)
{
    (void)call;
    assert(net_state->driver_running);
    net_state->credentials = (xiaotai_net_credentials_t){.ssid = "network", .password = "private"};
    net_state->has_credentials = true;
    net_link_connect();
    return XIAOTAI_OK;
}

typedef struct {
    uint32_t token, generation, ipv4;
    unsigned joins, leaves;
    xiaotai_net_state_t state;
    int64_t leave_due;
} observation_t;

static xiaotai_err_t read_worker(net_call_t *call)
{
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    const observation_t observation = {
        .token = net_state->link.token, .generation = status.generation, .ipv4 = status.ipv4,
        .joins = wifi_fixture_joins(), .leaves = wifi_fixture_leaves(), .state = status.state,
        .leave_due = net_deadline_due(NET_DEADLINE_LEAVE),
    };
    memcpy(call->data, &observation, sizeof(observation));
    call->size = sizeof(observation);
    return XIAOTAI_OK;
}

static observation_t observe(void)
{
    observation_t result;
    size_t size = 0;
    assert(net_call(read_worker, 0, NULL, 0, &result, sizeof(result), &size, 3000) == XIAOTAI_OK);
    assert(size == sizeof(result));
    return result;
}

static observation_t advance_to_leave_deadline(observation_t observed)
{
    assert(observed.leave_due > atomic_load(&now_us));
    atomic_store(&now_us, observed.leave_due);
    /* The worker runs due callbacks after dispatching a queued call; queue
     * another read to observe the result independent of which wakes it. */
    (void)observe();
    return observe();
}

static void boot(void)
{
    assert(xiaotai_signal_init(&owner_barrier) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    assert(xiaotai_run(owner_drained, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_barrier, 3000));
    assert(net_call(seed_credentials, 0, NULL, 0, NULL, 0, NULL, 3000) == XIAOTAI_OK);
}

static void advance(uint32_t ms)
{
    atomic_fetch_add(&now_us, (int64_t)ms * 1000);
    (void)observe();
    (void)observe();
}

static void expect_refused(int32_t raw, uint32_t wait, unsigned joins)
{
    const observation_t sampled = observe();
    xiaotai_net_diag_t diagnostic;
    xiaotai_net_status_t status;
    xiaotai_net_diag(&diagnostic);
    xiaotai_net_status(&status);
    assert(sampled.joins == joins && !sampled.token && !sampled.generation);
    assert(sampled.state == XIAOTAI_NET_WAITING && status.retry_in_ms == wait && diagnostic.backoff_ms == wait);
    assert(diagnostic.recent_count && diagnostic.recent[diagnostic.recent_count - 1U].loss == XIAOTAI_NET_LOSS_REFUSED);
    assert(diagnostic.recent[diagnostic.recent_count - 1U].code == raw);
}

static void expect_manual_refusal(esp_err_t raw, unsigned joins)
{
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    expect_refused(raw, 0, joins);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    assert(!status.control_pending && status.control_error == raw);
}

static void join_errors(void)
{
    const xiaotai_net_config_t policy = {.retry = {
        .fast_retries = 3, .first_ms = 2000, .max_ms = 60000, .max_credentials_ms = 300000,
        .grow_from_previous_base = true, .join_immediately_on_radio_start = true,
    }};
    assert(xiaotai_net_configure(&policy) == XIAOTAI_OK);
    wifi_fixture_connect_result(ESP_ERR_NO_MEM);
    boot();
    expect_refused(ESP_ERR_NO_MEM, 2000, 1);

    wifi_fixture_connect_result(ESP_OK);
    advance(2000);
    assert(observe().joins == 2 && observe().token);
    /* The synchronous failure did not consume a fast disconnect attempt. */
    for (unsigned n = 3; n <= 5; ++n) {
        wifi_fixture_physically_disconnected();
        advance(0);
        assert(observe().joins == n && observe().token);
    }
    wifi_fixture_physically_disconnected();
    advance(0);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    assert(observe().joins == 5 && !observe().token && status.retry_in_ms == 4000);

    /* A generic refusal is not a radio event and still uses the timer. */
    wifi_fixture_connect_result(ESP_ERR_INVALID_STATE);
    advance(4000);
    expect_refused(ESP_ERR_INVALID_STATE, 8000, 6);
    wifi_fixture_connect_result(ESP_ERR_WIFI_NOT_STARTED);
    advance(8000);
    expect_refused(ESP_ERR_WIFI_NOT_STARTED, 0, 7);
    advance(60000);
    expect_refused(ESP_ERR_WIFI_NOT_STARTED, 0, 7);

    /* Explicit retry can recover without a STOP or START event. An error
     * before esp_wifi_connect also needs the same stopped-radio handling. */
    wifi_fixture_station_config_result(ESP_ERR_WIFI_STOP_STATE);
    expect_manual_refusal(ESP_ERR_WIFI_STOP_STATE, 7);
    advance(60000);
    expect_refused(ESP_ERR_WIFI_STOP_STATE, 0, 7);
    wifi_fixture_station_config_result(ESP_OK);
    wifi_fixture_connect_result(ESP_ERR_WIFI_NOT_INIT);
    expect_manual_refusal(ESP_ERR_WIFI_NOT_INIT, 8);
    wifi_fixture_connect_result(ESP_OK);

    wifi_fixture_radio(false);
    wifi_fixture_radio(true);
    advance(0);
    const observation_t connecting = observe();
    assert(connecting.joins == 9 && connecting.token && connecting.state == XIAOTAI_NET_CONNECTING);
    wifi_fixture_associated();
    wifi_fixture_ipv4(123);
    assert(observe().generation && observe().ipv4 == 123);
    xiaotai_net_diag_t before, after;
    xiaotai_net_diag(&before);

    wifi_fixture_radio(false);
    advance(0);
    const observation_t stopped = observe();
    assert(stopped.state == XIAOTAI_NET_WAITING && !stopped.generation && !stopped.token && stopped.joins == 9);
    xiaotai_net_diag(&after);
    assert(!after.backoff_ms && after.losses == before.losses);
    wifi_fixture_radio(true);
    advance(0);
    const observation_t restarted = observe();
    assert(restarted.joins == 10 && restarted.token && restarted.token != connecting.token && !restarted.generation);
}

static void leave_fence(void)
{
    wifi_fixture_disconnect_result(ESP_FAIL);
    boot();
    assert(observe().joins == 1);

    wifi_fixture_associated();
    wifi_fixture_ipv4(123);
    const observation_t up = observe();
    assert(up.state == XIAOTAI_NET_UP && up.ipv4 == 123 && up.generation && up.token && !up.leaves);

    wifi_fixture_ipv4(0);
    observation_t observed = observe();
    assert(observed.state != XIAOTAI_NET_UP && !observed.ipv4 && !observed.generation &&
           observed.token == up.token && observed.joins == 1 && observed.leaves == 1);
    wifi_fixture_ipv4(123);
    assert(!observe().generation);

    for (unsigned calls = 2; calls <= 4; ++calls) {
        observed = advance_to_leave_deadline(observed);
        assert(observed.leaves == calls && observed.token == up.token && observed.joins == 1);
    }
    observed = advance_to_leave_deadline(observed);
    assert(observed.state == XIAOTAI_NET_HELD && !observed.ipv4 && !observed.generation &&
           observed.token == up.token && observed.joins == 1 && observed.leaves == 4 && !observed.leave_due);

    wifi_fixture_physically_disconnected();
    observed = observe();
    assert(observed.state == XIAOTAI_NET_HELD && !observed.token && observed.joins == 1);
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    observed = observe();
    assert(observed.joins == 2 && observed.token && observed.token != up.token && observed.leaves == 4);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "join_errors")));
    if (argc == 2) join_errors();
    else leave_fence();
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&owner_barrier);
}
