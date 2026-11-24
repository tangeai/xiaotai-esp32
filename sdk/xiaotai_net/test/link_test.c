// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_station_identity.py
// Replaces legacy: tests/regression/test_wifi_portal.py
// Replaces legacy: tests/xmini/test_platform_network.c::platform_network
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_net_host.h"

static const xiaotai_net_credentials_t saved = {.ssid = "saved", .password = "fictional-password"};
static const xiaotai_net_credentials_t candidate = {.ssid = "candidate", .password = "another-fixture"};
static const xiaotai_network_t wifi = {.desc = {.kind = XIAOTAI_NETWORK_WIFI}};
static struct { unsigned ups, downs, signals, completed_holds; xiaotai_net_status_t up, down; } events;
static _Atomic int64_t now_us = 1000000;
static xiaotai_signal_t sync_owner, hold_entered, hold_release;

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now_us); }

const xiaotai_network_t *xiaotai_hal_network(size_t index) { return index ? NULL : &wifi; }

static void record_event(const xiaotai_event_t *e, void *unused)
{
    (void)unused;
    if (!strcmp(e->name, XIAOTAI_EVENT_NET_UP)) {
        assert(e->size == sizeof(events.up));
        memcpy(&events.up, e->data, sizeof(events.up));
        ++events.ups;
    } else if (!strcmp(e->name, XIAOTAI_EVENT_NET_DOWN)) {
        assert(e->size == sizeof(events.down));
        memcpy(&events.down, e->data, sizeof(events.down));
        ++events.downs;
    } else if (!strcmp(e->name, XIAOTAI_EVENT_NET_STATE)) {
        xiaotai_net_status_t s;
        assert(e->size == sizeof(s));
        memcpy(&s, e->data, sizeof(s));
        if (s.state == XIAOTAI_NET_HELD && !s.control_pending) ++events.completed_holds;
    } else {
        ++events.signals;
    }
}

static xiaotai_err_t subscribe(void)
{
    const char *names[] = {XIAOTAI_EVENT_NET_UP, XIAOTAI_EVENT_NET_DOWN, XIAOTAI_EVENT_NET_SIGNAL,
                           XIAOTAI_EVENT_NET_STATE};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(xiaotai_event_subscribe(names[i], record_event, NULL) == XIAOTAI_OK);
    }
    return XIAOTAI_OK;
}

static const xiaotai_module_t net = {
    .name = "net", .init = xiaotai_engine_net_init, .start = xiaotai_engine_net_start,
};
static const xiaotai_module_t logger = {.name = "recorder", .init = subscribe};
static const xiaotai_module_t *const modules[] = {&net, &logger};
static const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 2};

static void notify(void *ctx) { xiaotai_signal_give(ctx); }

static void owner_sync(void)
{
    assert(xiaotai_run(notify, &sync_owner) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&sync_owner, 2000));
}

static void settle(void)
{
    /* The call slot returns before the worker services due timers. */
    assert(xiaotai_net_host_sync(2000) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(2000) == XIAOTAI_OK);
    owner_sync();
}

static void advance(uint32_t ms)
{
    atomic_fetch_add(&now_us, (int64_t)ms * 1000);
    settle();
}

static xiaotai_net_status_t status(void)
{
    xiaotai_net_status_t s;
    xiaotai_net_status(&s);
    return s;
}

static xiaotai_net_diag_t diag(void)
{
    xiaotai_net_diag_t d;
    xiaotai_net_diag(&d);
    return d;
}

static xiaotai_err_t install(net_call_t *call)
{
    (void)call;
    net_state->has_credentials = true;
    net_state->credentials = saved;
    net_link_connect();
    return XIAOTAI_OK;
}

static void set_credentials(void)
{
    assert(net_call(install, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    settle();
}

static void boot(const xiaotai_net_config_t *config)
{
    assert(xiaotai_signal_init(&sync_owner) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&hold_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&hold_release) == XIAOTAI_OK);
    assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_configure(config) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    owner_sync();
    settle();
    assert(status().state == XIAOTAI_NET_UNCONFIGURED && status().generation == 0);
}

static void stop(void)
{
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_net_host_reset();
    xiaotai_signal_deinit(&hold_release);
    xiaotai_signal_deinit(&hold_entered);
    xiaotai_signal_deinit(&sync_owner);
}

static unsigned calls(xiaotai_net_host_call_t call, xiaotai_net_host_record_t *last)
{
    xiaotai_net_host_record_t all[XIAOTAI_NET_HOST_RECORDS_MAX];
    const size_t n = xiaotai_net_host_records(all, XIAOTAI_NET_HOST_RECORDS_MAX);
    unsigned count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (all[i].call == call) {
            assert(all[i].on_worker);
            ++count;
            if (last) *last = all[i];
        }
    }
    return count;
}

static uint32_t last_join(unsigned count)
{
    xiaotai_net_host_record_t last;
    assert(calls(XIAOTAI_NET_HOST_JOIN, &last) == count);
    assert(last.token);
    return last.token;
}

static void emit(xiaotai_net_driver_event_t e)
{
    assert(xiaotai_net_host_emit(&e) == XIAOTAI_OK);
    settle();
}

static void up(uint32_t token, uint32_t ipv4)
{
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED, .token = token, .ssid = "saved"});
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = token, .ipv4 = ipv4});
}

static void lost(uint32_t token, xiaotai_net_loss_t reason)
{
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST, .token = token, .loss = reason,
                                    .code = 201, .signal_valid = true, .signal_dbm = -63});
}

static void step(xiaotai_net_host_call_t call, xiaotai_err_t result)
{
    const xiaotai_net_host_step_t scripted = {.result = result};
    assert(xiaotai_net_host_step(call, &scripted) == XIAOTAI_OK);
}

static void invalidation(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    up(old, 123);
    const uint32_t generation = xiaotai_net_generation();
    assert(generation && events.ups == 1);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old, .ipv4 = 123,
                                    .ipv6_valid = true, .ipv6 = {0x20, 1}});
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old,
                                    .ipv6_valid = true, .ipv6 = {0x20, 1}});
    assert(status().ipv4 == 0 && status().ipv6_valid && xiaotai_net_generation() == generation);
    assert(!events.downs && !calls(XIAOTAI_NET_HOST_LEAVE, NULL));
    step(XIAOTAI_NET_HOST_LEAVE, XIAOTAI_FAIL);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old});
    assert(status().state == XIAOTAI_NET_WAITING && status().failures == 1 && !status().generation);
    assert(!status().ipv4 && !status().ipv6_valid && !xiaotai_net_generation());
    assert(events.downs == 1 && events.down.state == XIAOTAI_NET_WAITING && !events.down.generation);
    assert(calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 1 && diag().first.loss == XIAOTAI_NET_LOSS_ADDRESS);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old, .ipv4 = 333});
    advance(499);
    assert(last_join(1) == old && !xiaotai_net_generation() && calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 1);
    advance(1);
    assert(calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 2 && last_join(1) == old);
    lost(old, XIAOTAI_NET_LOSS_ADDRESS);
    assert(diag().losses == 1 && events.downs == 1);
    advance(999);
    assert(last_join(1) == old);
    advance(1);
    const uint32_t fresh = last_join(2);
    assert(fresh != old);
    lost(old, XIAOTAI_NET_LOSS_SIGNAL);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old, .ipv4 = 333});
    assert(!xiaotai_net_generation() && status().state == XIAOTAI_NET_CONNECTING && diag().stale_events == 3);
    up(fresh, 456);
    assert(status().ipv4 == 456 && xiaotai_net_generation() != generation && status().failures == 0);
    stop();
}

static void controls(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    up(old, 123);
    const uint32_t generation = xiaotai_net_generation();
    step(XIAOTAI_NET_HOST_LEAVE, XIAOTAI_FAIL);
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    settle();
    assert(status().state == XIAOTAI_NET_UP && status().control_error == XIAOTAI_FAIL && !status().control_pending);
    assert(xiaotai_net_generation() == generation && !status().user_disconnected && !events.downs);
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    settle();
    assert(status().control_pending && status().user_disconnected && status().state == XIAOTAI_NET_HELD);
    assert(events.downs == 1 && !status().generation);
    assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old, .ipv4 = 222});
    lost(old, XIAOTAI_NET_LOSS_REQUESTED);
    assert(!status().control_pending && status().control_error == XIAOTAI_OK);
    assert(events.completed_holds == 1);
    assert(status().user_disconnected && !status().failures && !status().ipv4 && diag().losses == 0);
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_maintenance_end(ticket);
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    settle();
    const uint32_t next = last_join(2);
    assert(next != old && status().user_disconnected);
    up(next, 133);
    assert(!status().user_disconnected && status().ipv4 == 133);
    stop();
}

static void timeout(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED, .token = old, .ssid = "saved"});
    advance(XIAOTAI_NET_ADDRESS_TIMEOUT_MS - 1);
    assert(calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 0);
    advance(1);
    assert(calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 1 && status().failures == 1);
    /* A successful leave() without its terminal event cannot permit join(). */
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    settle();
    for (unsigned i = 0; i < 4; ++i) {
        advance(500);
        assert(last_join(1) == old);
    }
    assert(calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 4 && status().failing && status().state == XIAOTAI_NET_HELD);
    assert(!status().control_pending && status().control_error == XIAOTAI_ERR_TIMEOUT);
    advance(30000);
    assert(last_join(1) == old);
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    settle();
    assert(calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 5 && last_join(1) == old);
    lost(old, XIAOTAI_NET_LOSS_REQUESTED);
    assert(last_join(2) != old);
    stop();
}

static void hold_worker(const net_message_t *message)
{
    (void)message;
    xiaotai_signal_give(&hold_entered);
    assert(xiaotai_signal_take(&hold_release, 2000));
}

static void hold_owner(void *unused) { (void)unused; hold_worker(NULL); }
static void empty_task(void *unused) { (void)unused; }

static void control_notice(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    up(old, 123);
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    settle();
    assert(status().control_pending && !events.completed_holds);
    assert(xiaotai_run(hold_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&hold_entered, 2000));
    for (unsigned n = 1; n < XIAOTAI_FRAMEWORK_QUEUE_DEPTH; ++n) {
        assert(xiaotai_run(empty_task, NULL) == XIAOTAI_OK);
    }
    assert(xiaotai_run(notify, &sync_owner) == XIAOTAI_OK);
    assert(xiaotai_run(empty_task, NULL) == XIAOTAI_ERR_NO_MEM);
    const xiaotai_net_driver_event_t stopped = {
        .kind = XIAOTAI_NET_DRIVER_LOST, .token = old, .loss = XIAOTAI_NET_LOSS_REQUESTED,
    };
    assert(xiaotai_net_host_emit(&stopped) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(2000) == XIAOTAI_OK && !status().control_pending);
    xiaotai_signal_give(&hold_release);
    assert(xiaotai_signal_take(&sync_owner, 2000));
    owner_sync();
    assert(!events.completed_holds);
    advance(100);
    assert(events.completed_holds == 1);
    stop();
}

static void capacity(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    up(old, 123);
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&hold_entered, 2000));
    const xiaotai_net_driver_event_t late = {.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old, .ipv4 = 456};
    for (unsigned i = 0; i < XIAOTAI_NET_QUEUE_DEPTH; ++i) assert(xiaotai_net_host_emit(&late) == XIAOTAI_OK);
    assert(xiaotai_net_disconnect() == XIAOTAI_ERR_TIMEOUT && !status().control_pending);
    const xiaotai_net_driver_event_t empty = {.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old};
    assert(xiaotai_net_host_emit(&empty) == XIAOTAI_OK);
    assert(diag().dropped_events == 2);
    xiaotai_signal_give(&hold_release);
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_LEAVE, 1, 2000));
    settle();
    assert(!status().generation && !status().ipv4 && events.downs == 1 && events.ups == 1);
    assert(last_join(1) == old && diag().first.loss == XIAOTAI_NET_LOSS_UNKNOWN);
    lost(old, XIAOTAI_NET_LOSS_ADDRESS);
    advance(1000);
    assert(last_join(2) != old);
    stop();
}

static void backoff(void)
{
    static const uint32_t steps[] = {1000, 2000, 4000, 8000, 15000, 30000};
    xiaotai_net_retry_policy_t p = {.steps_ms = steps, .step_count = 6};
    for (unsigned i = 1; i < 40; ++i) {
        assert(net_backoff_wait_ms(&p, i, XIAOTAI_NET_LOSS_UNKNOWN, 0, NULL) == steps[i > 6 ? 5 : i - 1]);
    }
    p = (xiaotai_net_retry_policy_t){.fast_retries = 3, .first_ms = 2000, .max_ms = 60000,
                                   .max_credentials_ms = 300000, .jitter_percent = 25};
    for (unsigned i = 1; i <= 3; ++i)
        assert(!net_backoff_wait_ms(&p, i, XIAOTAI_NET_LOSS_UNKNOWN, UINT32_MAX, NULL));
    for (unsigned i = 4; i < 9; ++i) {
        const uint32_t ms = 2000U << (i - 4);
        assert(net_backoff_wait_ms(&p, i, XIAOTAI_NET_LOSS_UNKNOWN, 0, NULL) == ms);
        assert(net_backoff_wait_ms(&p, i, XIAOTAI_NET_LOSS_UNKNOWN, UINT32_MAX, NULL) == ms + ms / 4);
    }
    assert(net_backoff_wait_ms(&p, 20, XIAOTAI_NET_LOSS_NOT_FOUND, 0, NULL) == 60000);
    assert(net_backoff_wait_ms(&p, 20, XIAOTAI_NET_LOSS_CREDENTIALS, 0, NULL) == 300000);
    assert(net_backoff_wait_ms(&p, 20, XIAOTAI_NET_LOSS_SECURITY, UINT32_MAX, NULL) == 375000);
    assert(net_backoff_wait_ms(&p, 20, XIAOTAI_NET_LOSS_NOT_FOUND, 0, NULL) == 60000);
    for (unsigned i = 1; i <= 1000; ++i) assert(!net_backoff_give_up(&p, i));
    p = (xiaotai_net_retry_policy_t){.fast_retries = 10, .retries_max = 10};
    for (unsigned i = 1; i <= 10; ++i) assert(!net_backoff_give_up(&p, i));
    assert(net_backoff_give_up(&p, 11));
}

static void expect_wait_and_join(unsigned attempt, uint32_t base)
{
    const uint32_t wait = status().retry_in_ms;
    assert(status().state == XIAOTAI_NET_WAITING && diag().backoff_ms == wait);
    assert(wait >= base && wait <= base + base / 4U);
    advance(wait - 1U);
    (void)last_join(attempt);
    advance(1);
    (void)last_join(attempt + 1U);
}

static void mixed_retry(void)
{
    const xiaotai_net_config_t p = {.retry = {
        .fast_retries = 3, .first_ms = 2000, .max_ms = 60000,
        .max_credentials_ms = 300000, .jitter_percent = 25, .grow_from_previous_base = true,
    }};
    boot(&p);
    set_credentials();
    unsigned attempt = 1;
    for (; attempt <= 3; ++attempt) lost(last_join(attempt), XIAOTAI_NET_LOSS_SIGNAL);
    static const struct { xiaotai_net_loss_t reason; uint32_t base; } sequence[] = {
        {XIAOTAI_NET_LOSS_NOT_FOUND, 2000}, {XIAOTAI_NET_LOSS_NOT_FOUND, 4000},
        {XIAOTAI_NET_LOSS_NOT_FOUND, 8000}, {XIAOTAI_NET_LOSS_NOT_FOUND, 16000},
        {XIAOTAI_NET_LOSS_NOT_FOUND, 32000}, {XIAOTAI_NET_LOSS_NOT_FOUND, 60000},
        {XIAOTAI_NET_LOSS_NOT_FOUND, 60000}, {XIAOTAI_NET_LOSS_NOT_FOUND, 60000},
        {XIAOTAI_NET_LOSS_CREDENTIALS, 120000}, {XIAOTAI_NET_LOSS_SECURITY, 240000},
        {XIAOTAI_NET_LOSS_CREDENTIALS, 300000}, {XIAOTAI_NET_LOSS_NOT_FOUND, 60000},
        {XIAOTAI_NET_LOSS_CREDENTIALS, 120000},
    };
    for (size_t i = 0; i < sizeof(sequence) / sizeof(*sequence); ++i, ++attempt) {
        lost(last_join(attempt), sequence[i].reason);
        expect_wait_and_join(attempt, sequence[i].base);
    }
    lost(last_join(attempt), XIAOTAI_NET_LOSS_CREDENTIALS);
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    settle();
    ++attempt;
    for (unsigned i = 0; i < 3; ++i, ++attempt) lost(last_join(attempt), XIAOTAI_NET_LOSS_SIGNAL);
    lost(last_join(attempt), XIAOTAI_NET_LOSS_SIGNAL);
    expect_wait_and_join(attempt, 2000);
    up(last_join(++attempt), 123);
    for (unsigned i = 0; i < 3; ++i, ++attempt) lost(last_join(attempt), XIAOTAI_NET_LOSS_SIGNAL);
    lost(last_join(attempt), XIAOTAI_NET_LOSS_SIGNAL);
    expect_wait_and_join(attempt, 2000);
    stop();
}

static void retry_after_stop(bool immediate)
{
    const xiaotai_net_config_t p = {.retry = {.first_ms = 2000, .grow_from_previous_base = true,
                                            .join_immediately_on_radio_start = immediate}};
    boot(&p);
    set_credentials();
    const uint32_t first = last_join(1);
    lost(first, XIAOTAI_NET_LOSS_NOT_FOUND);
    assert(status().retry_in_ms == 2000 && diag().backoff_ms == 2000);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STOPPED});
    const uint32_t expected = immediate ? 0U : 2000U;
    assert(status().retry_in_ms == expected && diag().backoff_ms == expected);
    lost(first, XIAOTAI_NET_LOSS_UNKNOWN); /* Late disconnect from the stopped association. */
    advance(60000);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = first, .ipv4 = 123});
    assert(last_join(1) == first && !status().generation && !events.ups);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STARTED});
    if (!immediate) {
        (void)last_join(1);
        advance(2000);
    }
    const uint32_t second = last_join(2);
    assert(second != first);
    lost(second, XIAOTAI_NET_LOSS_NOT_FOUND);
    assert(status().retry_in_ms == 4000);
    stop();
}

static void policies(void)
{
    const xiaotai_net_config_t p = {.retry = {.fast_retries = 10, .retries_max = 10},
                                   .provisioning = XIAOTAI_NET_PROVISIONING_EXCLUSIVE,
                                   .accept_weak_security = true};
    boot(&p);
    for (unsigned i = 1; i <= 11; ++i) step(XIAOTAI_NET_HOST_JOIN, XIAOTAI_ERR_NOT_FOUND);
    set_credentials();
    assert(status().state == XIAOTAI_NET_HELD && status().failures == 11 && status().failing);
    assert(!status().user_disconnected && diag().attempts == 11 && diag().recent_count == XIAOTAI_NET_LOSS_HISTORY);
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    settle();
    xiaotai_net_host_record_t last;
    assert(calls(XIAOTAI_NET_HOST_JOIN, &last) == 12 && !last.flags && !status().failures);
    up(last.token, 123);
    lost(last.token, XIAOTAI_NET_LOSS_SIGNAL);
    assert(status().failures == 1 && diag().first.signal_dbm == -63 && diag().signal_losses == 1);
    assert(last_join(13) != last.token);
    stop();
}

static void clock_and_mac(void)
{
    boot(NULL);
    set_credentials();
    xiaotai_net_host_set_mac(XIAOTAI_FAIL, NULL);
    up(last_join(1), 123);
    const uint32_t gen = xiaotai_net_generation();
    assert(gen && events.ups == 1 && !status().mac_valid && events.signals == 1);
    advance(XIAOTAI_NET_MAC_RETRY_MS - 1);
    assert(calls(XIAOTAI_NET_HOST_MAC, NULL) == 1);
    advance(1);
    assert(events.ups == 1 && calls(XIAOTAI_NET_HOST_MAC, NULL) == 2);
    xiaotai_net_host_set_mac(XIAOTAI_OK, (uint8_t[]){2, 1, 0, 0, 0, 7});
    advance(XIAOTAI_NET_MAC_RETRY_MS);
    assert(status().mac_valid && status().mac[5] == 7 && events.ups == 2 && events.up.generation == gen);
    xiaotai_net_host_set_signal(XIAOTAI_OK, -52);
    advance(XIAOTAI_NET_SIGNAL_INTERVAL_MS);
    assert(events.signals == 1 && status().signal_dbm == -52);
    xiaotai_net_host_set_signal(XIAOTAI_OK, -56);
    advance(XIAOTAI_NET_SIGNAL_INTERVAL_MS);
    assert(events.signals == 2 && status().signal_dbm == -56);
    stop();
}

static unsigned verified_joins, verified_addresses, verified_losses;
static void observer_join(const char *ssid, void *unused)
{
    (void)unused;
    assert(!strcmp(ssid, candidate.ssid));
    ++verified_joins;
}

static void observer_address(void *unused) { (void)unused; ++verified_addresses; }
static void observer_loss(xiaotai_net_loss_t loss, int32_t code, void *unused)
{
    (void)unused;
    assert(loss == XIAOTAI_NET_LOSS_REFUSED && code == XIAOTAI_ERR_NOT_FOUND);
    ++verified_losses;
}

static xiaotai_err_t begin_verify(net_call_t *call)
{
    (void)call;
    const net_link_observer_t observer = {.joined = observer_join, .address = observer_address, .lost = observer_loss};
    return net_link_try(&candidate, &observer);
}

static xiaotai_err_t promote(net_call_t *call)
{
    (void)call;
    net_link_try_end(true);
    return XIAOTAI_OK;
}

static void verification(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    up(old, 123);
    assert(net_call(begin_verify, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    settle();
    assert(!xiaotai_net_generation() && last_join(1) == old);
    lost(old, XIAOTAI_NET_LOSS_REQUESTED);
    uint32_t test = last_join(2);
    up(test, 321);
    assert(!verified_addresses && !verified_joins && !verified_losses && !xiaotai_net_generation());
    lost(test, XIAOTAI_NET_LOSS_REQUESTED);
    assert(verified_losses == 1 && !status().failures);
    assert(net_call(begin_verify, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    settle();
    test = last_join(3);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED, .token = test, .ssid = "candidate"});
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = test, .ipv4 = 333});
    assert(verified_addresses == 1 && verified_joins == 1 && verified_losses == 1 && !xiaotai_net_generation());
    assert(net_call(promote, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    settle();
    assert(xiaotai_net_generation() && status().ipv4 == 333 && last_join(3) == test);
    stop();
}

static void stopped(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    up(old, 123);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STOPPED});
    assert(events.downs == 1 && !xiaotai_net_generation() && calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 0);
    lost(old, XIAOTAI_NET_LOSS_UNKNOWN);
    advance(60000);
    assert(last_join(1) == old);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STARTED});
    advance(1000);
    assert(last_join(2) != old && events.downs == 1);
    const uint32_t second = last_join(2);
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    settle();
    lost(second, XIAOTAI_NET_LOSS_REQUESTED);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STOPPED});
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    settle();
    assert(status().state == XIAOTAI_NET_WAITING && last_join(2) == second);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STARTED});
    assert(last_join(3) != second);
    stop();
}

static void overflow_stop(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    up(old, 123);
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&hold_entered, 2000));
    const xiaotai_net_driver_event_t queued = {.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = old, .ipv4 = 456};
    for (unsigned n = 1; n < XIAOTAI_NET_QUEUE_DEPTH; ++n) assert(xiaotai_net_host_emit(&queued) == XIAOTAI_OK);
    /* Last ordinary job signals that there is room again, after the edge. */
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK);
    const xiaotai_net_driver_event_t stop_event = {.kind = XIAOTAI_NET_DRIVER_STOPPED};
    assert(xiaotai_net_host_emit(&stop_event) == XIAOTAI_OK);
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_signal_give(&hold_release);
    assert(xiaotai_signal_take(&hold_entered, 2000));
    xiaotai_signal_give(&hold_release);
    settle();
    assert(events.downs == 1 && events.ups == 1 && !xiaotai_net_generation());
    lost(old, XIAOTAI_NET_LOSS_SIGNAL);
    advance(60000);
    assert(last_join(1) == old && calls(XIAOTAI_NET_HOST_LEAVE, NULL) == 0);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STARTED});
    advance(1000);
    assert(last_join(2) != old);
    stop();
}

static void ordering(void)
{
    boot(NULL);
    set_credentials();
    const uint32_t old = last_join(1);
    lost(old, XIAOTAI_NET_LOSS_NOT_FOUND);
    lost(old, XIAOTAI_NET_LOSS_SIGNAL);
    assert(diag().stale_events == 1 && diag().first.loss == XIAOTAI_NET_LOSS_NOT_FOUND);
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    settle();
    lost(last_join(2), XIAOTAI_NET_LOSS_SIGNAL);
    assert(diag().losses == 2 && diag().first.loss == XIAOTAI_NET_LOSS_NOT_FOUND);
    advance(1000);
    const uint32_t third = last_join(3);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = third, .ipv4 = 123});
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_JOINED, .token = third, .ssid = "saved"});
    advance(XIAOTAI_NET_ADDRESS_TIMEOUT_MS);
    assert(xiaotai_net_generation() && status().ipv4 == 123 && !calls(XIAOTAI_NET_HOST_LEAVE, NULL));
    lost(third, XIAOTAI_NET_LOSS_SIGNAL);
    assert(diag().first.loss == XIAOTAI_NET_LOSS_SIGNAL);
    stop();
}

typedef struct { char text[2048]; size_t used; } diag_output_t;

static void append_diag(void *ctx, const char *line)
{
    diag_output_t *out = ctx;
    const size_t n = strlen(line);
    assert(n < sizeof(out->text) - out->used);
    memcpy(out->text + out->used, line, n + 1U);
    out->used += n;
}

static void outage_history(void)
{
    static const uint32_t waits[] = {2000};
    const xiaotai_net_config_t config = {.retry = {.steps_ms = waits, .step_count = 1}};
    const xiaotai_net_loss_t reasons[] = {XIAOTAI_NET_LOSS_NOT_FOUND, XIAOTAI_NET_LOSS_CREDENTIALS,
                                       XIAOTAI_NET_LOSS_SIGNAL};
    uint32_t observed_s[9];
    boot(&config);
    set_credentials();
    assert(!diag().recent_count);
    for (unsigned i = 0; i < 9; ++i) {
        const uint32_t token = last_join(i + 1U);
        const xiaotai_net_loss_t why = reasons[i % (sizeof(reasons) / sizeof(reasons[0]))];
        advance(1000);
        observed_s[i] = (uint32_t)(xiaotai_time_us() / 1000000);
        emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_LOST, .token = token,
            .loss = why, .code = 201 + (int32_t)i, .signal_valid = true, .signal_dbm = (int8_t)(-90 + (int)i)});
        assert(diag().losses == i + 1U && diag().backoff_ms == waits[0]);
        if (i == 8U) step(XIAOTAI_NET_HOST_JOIN, XIAOTAI_ERR_NO_MEM);
        advance(waits[0]);
    }
    xiaotai_net_diag_t result = diag();
    assert(result.losses == 10U && result.attempts == 10U && result.signal_losses == 3U);
    assert(result.first.loss == XIAOTAI_NET_LOSS_NOT_FOUND && result.first.code == 201 &&
           result.first.signal_valid && result.first.signal_dbm == -90 && result.first.uptime_s == observed_s[0]);
    assert(result.recent_count == XIAOTAI_NET_LOSS_HISTORY);
    for (unsigned i = 0; i < 7; ++i) {
        const unsigned legacy_index = i + 2U;
        const xiaotai_net_loss_record_t *r = &result.recent[i];
        assert(r->loss == reasons[legacy_index % (sizeof(reasons) / sizeof(reasons[0]))] &&
               r->code == 201 + (int32_t)legacy_index && r->signal_valid &&
               r->signal_dbm == -90 + (int)legacy_index && r->uptime_s == observed_s[legacy_index]);
    }
    const xiaotai_net_loss_record_t *refused = &result.recent[7];
    assert(refused->loss == XIAOTAI_NET_LOSS_REFUSED && refused->code == XIAOTAI_ERR_NO_MEM &&
           !refused->signal_valid && refused->uptime_s == observed_s[8] + 2U);

    diag_output_t output = {0};
    const xiaotai_reply_t reply = {.write = append_diag, .ctx = &output};
    xiaotai_diag_report("net", &reply);
    assert(strstr(output.text, "type=first index=0") && strstr(output.text, "loss=not_found code=201 rssi=-90"));
    assert(strstr(output.text, "type=loss index=7") && strstr(output.text, "loss=refused code="));
    assert(!strstr(output.text, saved.ssid) && !strstr(output.text, saved.password));

    /* The worker has a retry pending but no live token. STOP must prevent
     * both that deadline and a late driver event from starting a join. */
    const uint32_t old_token = last_join(10);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STOPPED});
    const uint32_t stopped_wait = status().retry_in_ms;
    const uint32_t stopped_diag = diag().backoff_ms;
    advance(60000);
    lost(old_token, XIAOTAI_NET_LOSS_SIGNAL);
    assert(calls(XIAOTAI_NET_HOST_JOIN, NULL) == 10U && diag().losses == 10U);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_STARTED});
    const bool start_immediate = calls(XIAOTAI_NET_HOST_JOIN, NULL) == 11U;
    advance(status().retry_in_ms);
    assert(calls(XIAOTAI_NET_HOST_JOIN, NULL) == 11U && last_join(11) != old_token);
    up(last_join(11), 123);
    result = diag();
    assert(!result.first.uptime_s && result.recent_count == XIAOTAI_NET_LOSS_HISTORY &&
           result.recent[0].code == 203 && result.recent[7].loss == XIAOTAI_NET_LOSS_REFUSED);
    output = (diag_output_t){0};
    xiaotai_diag_report("net", &reply);
    assert(!strstr(output.text, "type=first") && strstr(output.text, "type=loss index=7"));
    printf("outage: 10 losses, first/recent preserved until address; stopped_retry_ms=%lu stopped_diag_ms=%lu START_immediate=%u\n",
           (unsigned long)stopped_wait, (unsigned long)stopped_diag, (unsigned)start_immediate);
    stop();
}

static xiaotai_err_t begin_scan(net_call_t *call)
{
    (void)call;
    return net_scan_start();
}

static void scan(void)
{
    boot(NULL);
    assert(net_call(begin_scan, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    xiaotai_net_host_record_t request;
    assert(calls(XIAOTAI_NET_HOST_SCAN, &request) == 1 && request.token);
    set_credentials();
    assert(!calls(XIAOTAI_NET_HOST_JOIN, NULL) && status().state == XIAOTAI_NET_WAITING);
    assert(net_call(begin_verify, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_ERR_BUSY);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = request.token});
    assert(last_join(1) && status().state == XIAOTAI_NET_CONNECTING);
    stop();
}

static xiaotai_err_t configure_scan_snapshot(net_call_t *call)
{
    (void)call;
    xiaotai_net_provision_status_t *portal = net_provision_edit();
    memcpy(portal->ap_ssid, "fixture-ap", sizeof("fixture-ap"));
    net_provision_commit(false);
    net_scan_prepare();
    return XIAOTAI_OK;
}

static void cancel_scan_cache(const net_message_t *message) { (void)message; net_scan_cancel(); }

static void scan_cache(void)
{
    boot(NULL);
    const struct { uint8_t version, count; xiaotai_net_credentials_t entries[5]; } saved_profiles = {
        .version = 1, .count = 2, .entries = {
            {.ssid = "same", .password = "fictional-password"}, {.ssid = "stored-only"},
        },
    };
    assert(xiaotai_kv_set("wifi_cfg", "known", &saved_profiles, sizeof(saved_profiles), 1000) == XIAOTAI_OK);
    assert(net_call(configure_scan_snapshot, 0, NULL, 0, NULL, 0, NULL, 2000) == XIAOTAI_OK);
    xiaotai_net_network_t list[8];
    xiaotai_net_scan_status_t s;
    assert(xiaotai_net_networks(list, 8, &s) == 2 && s.state == XIAOTAI_NET_SCAN_IDLE);
    assert(!list[0].available && list[0].saved && list[0].secure && !strcmp(list[0].ssid, "same"));

    xiaotai_net_scan_record_t raw[] = {
        {.ssid = "same", .signal_dbm = -70, .security = XIAOTAI_NET_SECURITY_WPA2},
        {.ssid = "", .signal_dbm = -13, .security = XIAOTAI_NET_SECURITY_OPEN},
        {.ssid = "same", .signal_dbm = -44, .security = XIAOTAI_NET_SECURITY_WPA3},
        {.ssid = "same", .signal_dbm = -24, .security = XIAOTAI_NET_SECURITY_OPEN},
        {.ssid = "fixture-ap", .signal_dbm = -20, .security = XIAOTAI_NET_SECURITY_OPEN},
        {.ssid = "wep", .signal_dbm = -40, .security = XIAOTAI_NET_SECURITY_WEP},
        {.signal_dbm = -9, .security = XIAOTAI_NET_SECURITY_OPEN},
    };
    memset(raw[6].ssid, 'x', sizeof(raw[6].ssid));
    xiaotai_net_host_set_scan(raw, sizeof(raw) / sizeof(raw[0]), true);
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    settle();
    xiaotai_net_host_record_t active;
    assert(calls(XIAOTAI_NET_HOST_SCAN, &active) == 1 && active.token);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = active.token});
    assert(xiaotai_net_networks(list, 8, &s) == 4 && s.state == XIAOTAI_NET_SCAN_READY && s.truncated);
    assert(!strcmp(list[0].ssid, "same") && list[0].available && list[0].supported && !list[0].secure && !list[0].saved);
    assert(!strcmp(list[1].ssid, "wep") && list[1].available && list[1].secure && !list[1].supported);
    assert(!strcmp(list[2].ssid, "same") && list[2].signal_valid && list[2].signal_dbm == -44 && list[2].saved);
    assert(list[2].security == XIAOTAI_NET_SECURITY_WPA3 && list[2].secure && list[2].supported);
    assert(!strcmp(list[3].ssid, "stored-only") && !list[3].available && list[3].saved && !list[3].signal_valid);
    assert(xiaotai_net_networks(NULL, 8, NULL) == 0 && xiaotai_net_networks(list, 1, NULL) == 1);
    const uint32_t revision = s.revision;
    advance(XIAOTAI_NET_SCAN_INTERVAL_MS - 1);
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    settle();
    assert(calls(XIAOTAI_NET_HOST_SCAN, NULL) == 1);
    assert(xiaotai_net_networks(list, 8, &s) == 4 && s.revision == revision);

    set_credentials();
    advance(1);
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    settle();
    assert(calls(XIAOTAI_NET_HOST_SCAN, NULL) == 1);
    assert(xiaotai_net_networks(list, 8, &s) == 4 && s.state == XIAOTAI_NET_SCAN_BUSY && s.error == XIAOTAI_ERR_BUSY);
    assert(net_post(cancel_scan_cache, 0, NULL, 0) == XIAOTAI_OK);
    settle();
    assert(xiaotai_net_networks(list, 8, &s) == 0 && s.state == XIAOTAI_NET_SCAN_IDLE);
    stop();
}

static void scan_timeout(void)
{
    boot(NULL);
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    settle();
    xiaotai_net_host_record_t active;
    assert(calls(XIAOTAI_NET_HOST_SCAN, &active) == 1 && active.token);
    set_credentials();
    step(XIAOTAI_NET_HOST_SCAN_CANCEL, XIAOTAI_FAIL);
    advance(XIAOTAI_NET_SCAN_TIMEOUT_MS - 1);
    assert(!calls(XIAOTAI_NET_HOST_JOIN, NULL) && !calls(XIAOTAI_NET_HOST_SCAN_CANCEL, NULL));
    advance(1);
    xiaotai_net_scan_status_t scan_status;
    assert(xiaotai_net_networks(NULL, 0, &scan_status) == 0);
    assert(scan_status.state == XIAOTAI_NET_SCAN_ERROR && scan_status.error == XIAOTAI_ERR_TIMEOUT);
    assert(diag().scan_timeouts == 1 && !calls(XIAOTAI_NET_HOST_JOIN, NULL));
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = active.token + 100U});
    assert(diag().stale_events == 1 && !calls(XIAOTAI_NET_HOST_JOIN, NULL));
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = active.token});
    assert(!calls(XIAOTAI_NET_HOST_SCAN_READ, NULL) && calls(XIAOTAI_NET_HOST_SCAN_CANCEL, NULL) == 2);
    assert(last_join(1) != active.token);
    xiaotai_net_networks(NULL, 0, &scan_status);
    assert(scan_status.state == XIAOTAI_NET_SCAN_ERROR && scan_status.error == XIAOTAI_ERR_TIMEOUT);
    stop();
}

static void scan_disconnect_timeout(void)
{
    boot(NULL);
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    settle();
    xiaotai_net_host_record_t active;
    assert(calls(XIAOTAI_NET_HOST_SCAN, &active) == 1 && active.token);
    assert(xiaotai_net_disconnect() == XIAOTAI_OK);
    settle();
    xiaotai_net_scan_status_t scan;
    (void)xiaotai_net_networks(NULL, 0, &scan);
    assert(status().state == XIAOTAI_NET_HELD && status().user_disconnected && scan.state == XIAOTAI_NET_SCAN_RUNNING);
    advance(XIAOTAI_NET_SCAN_TIMEOUT_MS - 1U);
    assert(!calls(XIAOTAI_NET_HOST_SCAN_CANCEL, NULL) && !calls(XIAOTAI_NET_HOST_JOIN, NULL));
    advance(1);
    assert(xiaotai_net_networks(NULL, 0, &scan) == 0 && scan.state == XIAOTAI_NET_SCAN_ERROR &&
           scan.error == XIAOTAI_ERR_TIMEOUT && calls(XIAOTAI_NET_HOST_SCAN_CANCEL, NULL) == 1);
    emit((xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = active.token});
    assert(!calls(XIAOTAI_NET_HOST_SCAN_READ, NULL) && !calls(XIAOTAI_NET_HOST_JOIN, NULL));
    printf("manual disconnect preserves scan watchdog: timeout_ms=%u canceled once; no late results/rejoin\n",
           (unsigned)XIAOTAI_NET_SCAN_TIMEOUT_MS);
    stop();
}

int main(int argc, char **argv)
{
    const char *test = argc > 1 ? argv[1] : "invalidation";
    if (!strcmp(test, "invalidation")) invalidation();
    else if (!strcmp(test, "backoff")) backoff();
    else if (!strcmp(test, "mixed_retry")) mixed_retry();
    else if (!strcmp(test, "retry_after_stop")) retry_after_stop(false);
    else if (!strcmp(test, "retry_immediately_after_stop")) retry_after_stop(true);
    else if (!strcmp(test, "controls")) controls();
    else if (!strcmp(test, "control_notice")) control_notice();
    else if (!strcmp(test, "timeout")) timeout();
    else if (!strcmp(test, "capacity")) capacity();
    else if (!strcmp(test, "policies")) policies();
    else if (!strcmp(test, "clock")) clock_and_mac();
    else if (!strcmp(test, "verification")) verification();
    else if (!strcmp(test, "stopped")) stopped();
    else if (!strcmp(test, "overflow_stop")) overflow_stop();
    else if (!strcmp(test, "ordering")) ordering();
    else if (!strcmp(test, "outage_history")) outage_history();
    else if (!strcmp(test, "scan")) scan();
    else if (!strcmp(test, "scan_cache")) scan_cache();
    else if (!strcmp(test, "scan_timeout")) scan_timeout();
    else if (!strcmp(test, "scan_disconnect_timeout")) scan_disconnect_timeout();
    else return 2;
    return 0;
}
