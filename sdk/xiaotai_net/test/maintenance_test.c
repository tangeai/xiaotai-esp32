// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_companion_maintenance.py
// Replaces legacy: tests/regression/test_native_wifi_maintenance.py
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/framework.h"
#include "xiaotai_net_host.h"

#define WAIT_MS 3000U

static xiaotai_signal_t s_sync, s_entered, s_release, s_request, s_restarted;
static atomic_uint s_attempts, s_busy, s_history_writes, s_release_events, s_prepare_calls;
static atomic_uint s_notices;
static bool s_has_prepare, s_gate_at_init, s_handover_on_notice;
static uint32_t s_quiesce;

xiaotai_err_t __real_xiaotai_restart_request(const char *reason, uint32_t quiesce_ticket);
xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t quiesce_ticket);
void __real_net_units_maintenance_released(void);
void __wrap_net_units_maintenance_released(void);

xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t quiesce_ticket)
{
    assert(xiaotai_on_owner() && !strcmp(reason, "network") && quiesce_ticket == 0);
    const xiaotai_err_t err = __real_xiaotai_restart_request(reason, quiesce_ticket);
    atomic_fetch_add(&s_attempts, 1);
    if (err == XIAOTAI_ERR_BUSY) atomic_fetch_add(&s_busy, 1);
    xiaotai_signal_give(&s_request);
    return err;
}

void __wrap_net_units_maintenance_released(void)
{
    assert(net_on_worker());
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE && ticket == 0);
    __real_net_units_maintenance_released();
    atomic_fetch_add(&s_history_writes, 1);
}

static xiaotai_err_t prepare(void *self, bool manual, xiaotai_network_progress_fn_t progress, void *ctx)
{
    (void)self;
    assert(net_on_worker() && net_gate_bringup());
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE);
    assert(net_gate_coprocessor_begin() == XIAOTAI_ERR_INVALID_STATE);
    assert(net_gate_config_begin() == XIAOTAI_ERR_INVALID_STATE);
    assert(atomic_fetch_add(&s_prepare_calls, 1) == (manual ? 1U : 0U));
    progress(manual ? XIAOTAI_NETWORK_READY : XIAOTAI_NETWORK_NO_LINK, 100, ctx);
    return XIAOTAI_OK;
}

const xiaotai_network_t *xiaotai_hal_network(size_t index)
{
    static const xiaotai_network_t wifi = {.desc = {.kind = XIAOTAI_NETWORK_WIFI}};
    static const xiaotai_network_t hosted = {
        .desc = {.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR}, .prepare = prepare,
    };
    return index == 0 ? (s_has_prepare ? &hosted : &wifi) : NULL;
}

static void quiesce_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event->args.value) atomic_fetch_add(&s_release_events, 1);
}

static void provision_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner() && event->size == sizeof(xiaotai_net_provision_status_t));
    const xiaotai_net_provision_status_t *status = event->data;
    if (!status->restart_required) return;
    atomic_fetch_add(&s_notices, 1);
    if (s_handover_on_notice) {
        /* Same handoff as exclusive provisioning: the holder only gives its
         * ticket to the restart when it sees the engine's promise. */
        assert(s_quiesce && __real_xiaotai_restart_request("holder", s_quiesce) == XIAOTAI_OK);
    }
}

static xiaotai_err_t observe(void)
{
    uint32_t ticket = 0;
    s_gate_at_init = xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK;
    if (ticket) xiaotai_net_maintenance_end(ticket);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_QUIESCE, quiesce_event, NULL) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_NET_PROVISION, provision_event, NULL);
}

static void restart(void) { xiaotai_signal_give(&s_restarted); }

static void give(void *ctx) { (void)ctx; xiaotai_signal_give(&s_sync); }

static void owner_sync(void)
{
    const int64_t deadline = xiaotai_time_us() + WAIT_MS * 1000U;
    xiaotai_err_t err;
    do {
        err = xiaotai_run(give, NULL);
        if (err == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
    } while (err == XIAOTAI_ERR_NO_MEM && xiaotai_time_us() < deadline);
    assert(err == XIAOTAI_OK && xiaotai_signal_take(&s_sync, WAIT_MS));
}

static void boot(void)
{
    static const char *const after_net[] = {"net", NULL};
    static const xiaotai_module_t net = {
        .name = "net", .needs = XIAOTAI_NEED_NETWORK, .init = xiaotai_engine_net_init, .start = xiaotai_engine_net_start,
    };
    static const xiaotai_module_t observer = {.name = "test", .depends = after_net, .init = observe};
    static const xiaotai_module_t *const modules[] = {&net, &observer};
    const xiaotai_framework_config_t config = {
        .modules = modules, .module_count = 2, .available_needs = XIAOTAI_NEED_NETWORK, .restart = restart,
    };
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    owner_sync();
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
}

typedef enum {MAINTENANCE, CONFIGURE, COPROCESSOR} operation_t;
typedef struct { operation_t operation; pthread_barrier_t *barrier; bool acquired; } racer_t;

static void barrier(pthread_barrier_t *bar)
{
    const int rc = pthread_barrier_wait(bar);
    assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *race(void *ctx)
{
    racer_t *racer = ctx;
    for (unsigned i = 0; i < 128; ++i) {
        barrier(racer->barrier);
        uint32_t ticket = 0;
        const xiaotai_err_t err = racer->operation == MAINTENANCE ? xiaotai_net_maintenance_begin(&ticket) :
            racer->operation == CONFIGURE ? net_gate_config_begin() : net_gate_coprocessor_begin();
        assert(err == XIAOTAI_OK || err == XIAOTAI_ERR_INVALID_STATE);
        racer->acquired = err == XIAOTAI_OK;
        barrier(racer->barrier);
        barrier(racer->barrier);
        if (racer->acquired) {
            if (racer->operation == MAINTENANCE) xiaotai_net_maintenance_end(ticket);
            else if (racer->operation == CONFIGURE) net_gate_config_end();
            else net_gate_coprocessor_end();
        }
        barrier(racer->barrier);
    }
    return NULL;
}

static void race_pair(operation_t first, operation_t second)
{
    pthread_barrier_t bar;
    assert(pthread_barrier_init(&bar, NULL, 3) == 0);
    racer_t racers[] = {{.operation = first, .barrier = &bar}, {.operation = second, .barrier = &bar}};
    pthread_t tasks[2];
    for (size_t i = 0; i < 2; ++i) assert(pthread_create(&tasks[i], NULL, race, &racers[i]) == 0);
    for (unsigned i = 0; i < 128; ++i) {
        barrier(&bar);
        barrier(&bar);
        const unsigned winners = (unsigned)racers[0].acquired + (unsigned)racers[1].acquired;
        assert(winners == (first == CONFIGURE && second == CONFIGURE ? 2U : 1U));
        barrier(&bar);
        barrier(&bar);
        if (first == MAINTENANCE) assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    }
    for (size_t i = 0; i < 2; ++i) assert(pthread_join(tasks[i], NULL) == 0);
    assert(pthread_barrier_destroy(&bar) == 0);
}

static void worker_hold(const net_message_t *message)
{
    (void)message;
    xiaotai_signal_give(&s_entered);
    assert(xiaotai_signal_take(&s_release, WAIT_MS));
}

static void nothing_worker(const net_message_t *message) { (void)message; }

static void gate(void)
{
    race_pair(MAINTENANCE, MAINTENANCE);
    race_pair(MAINTENANCE, CONFIGURE);
    race_pair(MAINTENANCE, COPROCESSOR);
    race_pair(CONFIGURE, COPROCESSOR);
    race_pair(CONFIGURE, CONFIGURE);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    uint32_t ticket, stale;
    assert(xiaotai_net_maintenance_begin(NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_net_maintenance_begin(&stale) == XIAOTAI_OK);
    xiaotai_net_maintenance_end(stale);
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket != stale);
    xiaotai_net_maintenance_end(stale);
    assert(net_gate_maintenance_held());
    assert(xiaotai_net_maintenance_restart(stale) == XIAOTAI_ERR_INVALID_STATE);

    assert(net_post(worker_hold, 0, NULL, 0) == XIAOTAI_OK && xiaotai_signal_take(&s_entered, WAIT_MS));
    unsigned jobs = 0;
    while (net_post(nothing_worker, 0, NULL, 0) == XIAOTAI_OK) assert(++jobs < 256);
    assert(jobs != 0);
    const unsigned writes = atomic_load(&s_history_writes);
    xiaotai_net_maintenance_end(ticket);
    assert(atomic_load(&s_history_writes) == writes);
    xiaotai_signal_give(&s_release);
    while (xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_ERR_TIMEOUT) {
        assert(jobs-- != 0);
        xiaotai_delay_ms(1);
    }
    assert(atomic_load(&s_history_writes) == writes + 1);

    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK);
    assert(xiaotai_net_maintenance_restart(ticket) == XIAOTAI_OK);
    xiaotai_net_maintenance_end(ticket);
    assert(net_gate_maintenance_held() && net_gate_restart_pending());
    assert(net_gate_config_begin() == XIAOTAI_ERR_INVALID_STATE && !xiaotai_restart_pending());
    owner_sync();
    assert(!atomic_load(&s_attempts));
}

static void bringup(void)
{
    assert(!s_gate_at_init && !net_gate_bringup() && net_state_now() == XIAOTAI_NET_BLOCKED);
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK);
    assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_maintenance_end(ticket);
    assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_START, &(xiaotai_net_host_step_t){.hold = true}) == XIAOTAI_OK);
    assert(xiaotai_net_retry() == XIAOTAI_OK && xiaotai_net_host_wait_held(WAIT_MS));
    assert(net_gate_bringup() && xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_host_release();
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK && !net_gate_bringup());
    assert(atomic_load(&s_prepare_calls) == 2 && xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK);
    xiaotai_net_maintenance_end(ticket);
}

static void nothing_owner(void *ctx) { (void)ctx; }

static void fill_owner(void)
{
    unsigned count = 0;
    while (xiaotai_run(nothing_owner, NULL) == XIAOTAI_OK) assert(++count < 256);
    assert(count != 0);
}

static void owner_hold(void *ctx)
{
    (void)ctx;
    fill_owner();
    xiaotai_signal_give(&s_entered);
    assert(xiaotai_signal_take(&s_release, WAIT_MS));
}

static void promise_worker(const net_message_t *message)
{
    (void)message;
    net_restart_required();
    net_restart_required();
    xiaotai_signal_give(&s_sync);
}

static void quiesce_begin(void *ctx)
{
    (void)ctx;
    assert(xiaotai_quiesce_begin("other", &s_quiesce) == XIAOTAI_OK);
}

static void quiesce_end(void *ctx)
{
    if (ctx) fill_owner();
    xiaotai_quiesce_end(s_quiesce);
}

static void takeover(void *ctx)
{
    (void)ctx;
    assert(__real_xiaotai_restart_request("holder", s_quiesce) == XIAOTAI_OK);
}

static void promise(const char *scenario)
{
    const bool handover = !strcmp(scenario, "takeover");
    const bool release_lost = !strcmp(scenario, "release_lost");
    const bool busy = !strcmp(scenario, "busy") || handover || release_lost;
    const bool full = !strcmp(scenario, "queue") || handover || s_handover_on_notice;
    if (busy || s_handover_on_notice) {
        assert(xiaotai_run(quiesce_begin, NULL) == XIAOTAI_OK);
        owner_sync();
    }
    xiaotai_restart_hold();
    if (full) assert(xiaotai_run(owner_hold, NULL) == XIAOTAI_OK && xiaotai_signal_take(&s_entered, WAIT_MS));
    assert(net_post(promise_worker, 0, NULL, 0) == XIAOTAI_OK && xiaotai_signal_take(&s_sync, WAIT_MS));
    xiaotai_net_provision_status_t status;
    xiaotai_net_provision_status(&status);
    assert(status.restart_required && net_gate_restart_pending());
    assert(net_gate_config_begin() == XIAOTAI_ERR_INVALID_STATE);
    if (full) {
        assert(!atomic_load(&s_attempts) && !xiaotai_restart_pending());
        xiaotai_signal_give(&s_release);
    }
    assert(xiaotai_signal_take(&s_request, WAIT_MS));
    if (busy) {
        assert(atomic_load(&s_busy) == 1 && !xiaotai_restart_pending());
        if (handover) {
            assert(xiaotai_run(takeover, NULL) == XIAOTAI_OK);
        } else {
            /* In release_lost, the owner queue is full at release; the
             * framework drops the event but the worker keeps its promise. */
            assert(xiaotai_run(quiesce_end, release_lost ? &s_quiesce : NULL) == XIAOTAI_OK);
        }
        assert(xiaotai_signal_take(&s_request, WAIT_MS));
        owner_sync();
        assert(atomic_load(&s_release_events) == (handover || release_lost ? 0U : 1U));
    }
    assert(xiaotai_restart_pending() && atomic_load(&s_attempts) == (busy ? 2U : 1U));
    assert(atomic_load(&s_notices) == 1U);
    if (s_handover_on_notice) assert(!atomic_load(&s_busy) && !atomic_load(&s_release_events));
    xiaotai_restart_release();
    assert(xiaotai_signal_take(&s_restarted, WAIT_MS));
}

int main(int argc, char **argv)
{
    const char *scenario = argc == 2 ? argv[1] : "gate";
    xiaotai_signal_t *const signals[] = {&s_sync, &s_entered, &s_release, &s_request, &s_restarted};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    uint32_t ticket = 42;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE && ticket == 0);
    s_has_prepare = !strcmp(scenario, "bringup");
    s_handover_on_notice = !strcmp(scenario, "notice");
    boot();
    if (s_has_prepare) bringup();
    else if (!strcmp(scenario, "gate")) gate();
    else promise(scenario);
    xiaotai_net_reset();
    xiaotai_framework_reset();
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) xiaotai_signal_deinit(signals[i]);
    printf("net_gate %s: ok\n", scenario);
    return 0;
}
