// SPDX-License-Identifier: MIT
#pragma once
/* Shared helpers of the transport host tests: the engine runs inside the real
 * framework (its init subscribes to events), next to a fake platform module
 * it depends on and an observer module that records the transport's events.
 * The engine clock is manual: timers are checked at absolute instants. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakes.h"
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/transport.h"
#include "xiaotai_mem_host.h"
#include "xiaotai_transport_host.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

/* Engine clock at boot; never 0 (0 means "no deadline" inside the engine). */
#define T0 100000

#define TEST_ENDPOINT "http://rtc.example.test"

/* ------------------------------------------------------------ observer */

#define OBSERVED_MAX 64U

typedef struct {
    int states[OBSERVED_MAX];
    size_t state_count;
    int links[OBSERVED_MAX];
    size_t link_count;
    /* Transport view when the fake platform module saw net.up. */
    bool net_up_seen;
    bool start_reserve_at_net_up;
    /* Allocations the engine's start() made (host port counters). */
    uint32_t start_allocations;
    int64_t start_outstanding;
    bool measure_start;
} observed_t;

static observed_t g_observed;
static void (*g_observer_start)(void);

static inline void on_transport_state(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (g_observed.state_count < OBSERVED_MAX) g_observed.states[g_observed.state_count++] = event->args.value;
}

static inline void on_transport_links(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (g_observed.link_count < OBSERVED_MAX) g_observed.links[g_observed.link_count++] = event->args.value;
}

static inline void on_platform_net_up(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    xiaotai_transport_status_t status;
    xiaotai_transport_status(&status);
    g_observed.net_up_seen = true;
    g_observed.start_reserve_at_net_up = status.start_reserve_held;
}

static inline xiaotai_err_t fake_platform_init(void)
{
    /* Subscribed before the transport, as the real platform core is: the
     * transport depends on it. */
    return xiaotai_event_subscribe(XIAOTAI_EVENT_NET_UP, on_platform_net_up, NULL);
}

static inline xiaotai_err_t observer_init(void)
{
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_TRANSPORT_STATE, on_transport_state, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_TRANSPORT_LINKS, on_transport_links, NULL);
    return err;
}

static inline void observer_start(void)
{
    if (g_observer_start) g_observer_start();
}

/* Requirements a test module declares in its init (NULL: none). */
static const xiaotai_transport_requirement_t *g_requirement;
static xiaotai_err_t g_requirement_result;

static inline xiaotai_err_t requiring_init(void)
{
    g_requirement_result = g_requirement ? xiaotai_transport_require(g_requirement) : XIAOTAI_OK;
    return XIAOTAI_OK;
}

static inline void transport_start(void)
{
    if (g_observed.measure_start) xiaotai_mem_host_clear();
    xiaotai_engine_transport_start();
    if (g_observed.measure_start) {
        g_observed.start_allocations = xiaotai_mem_host_allocations();
        g_observed.start_outstanding = xiaotai_mem_host_outstanding();
    }
}

static const char *const g_transport_depends[] = {"platform", NULL};
static const char *const g_module_depends[] = {"transport", NULL};
static const xiaotai_module_t g_platform_module = {.name = "platform", .init = fake_platform_init};
static const xiaotai_module_t g_transport_module = {
    .name = "transport", .depends = g_transport_depends, .init = xiaotai_engine_transport_init,
    .start = transport_start};
static const xiaotai_module_t g_observer_module = {
    .name = "observer", .depends = g_module_depends, .init = observer_init, .start = observer_start};
static const xiaotai_module_t g_requiring_module = {
    .name = "requiring", .depends = g_module_depends, .init = requiring_init};

/* ------------------------------------------------------------- driving */

static inline void owner_done(void *ctx) { xiaotai_signal_give((xiaotai_signal_t *)ctx); }

/* Waits until the owner task has handled everything queued before. */
static inline void owner_sync(void)
{
    xiaotai_signal_t done;
    CHECK(xiaotai_signal_init(&done) == XIAOTAI_OK);
    CHECK(xiaotai_run(owner_done, &done) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&done, 2000));
    xiaotai_signal_deinit(&done);
}

/* Owner and worker have nothing left to do at the present engine time. */
static inline void settle(void)
{
    for (int round = 0; round < 3; ++round) {
        owner_sync();
        CHECK(xiaotai_transport_host_settle(3000));
    }
}

static inline void emit(const char *name, int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    CHECK(xiaotai_event_emit(name, &args, NULL, 0) == XIAOTAI_OK);
    settle();
}

static inline void advance(uint32_t ms)
{
    xiaotai_transport_host_clock_advance(ms);
    settle();
}

static inline int64_t now_ms(void) { return xiaotai_transport_host_clock(); }

static inline uint32_t calls(xiaotai_transport_host_call_kind_t kind) { return xiaotai_transport_host_count(kind); }

static inline xiaotai_transport_status_t status_now(void)
{
    xiaotai_transport_status_t status;
    xiaotai_transport_status(&status);
    return status;
}

/* Runs fn on the worker (as the link package would) and waits for it. */
static void (*g_on_worker_fn)(void);
static inline void on_worker_job(uint32_t a, uint32_t b)
{
    (void)a;
    (void)b;
    g_on_worker_fn();
}

static inline void on_worker(void (*fn)(void))
{
    g_on_worker_fn = fn;
    CHECK(xt_worker_post(on_worker_job, 0, 0));
    settle();
}

/* True when the event log holds this label of the transport. */
static inline bool logged(const char *label)
{
    static xiaotai_eventlog_entry_t entries[256];
    const size_t count = xiaotai_eventlog_read(0, entries, 256);
    for (size_t i = 0; i < count && i < 256; ++i) {
        if (strcmp(entries[i].source, XT_LOG_SOURCE) == 0 && strcmp(entries[i].label, label) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------- boot */

typedef struct {
    bool with_requiring;       /* add the module that declares g_requirement */
    bool keep_fakes;           /* the test prepared the fakes before boot */
} boot_options_t;

static inline void boot_with(const xiaotai_transport_config_t *config, boot_options_t options)
{
    xiaotai_transport_host_reset();
    if (!options.keep_fakes) fakes_reset();
    memset(&g_observed.states, 0, sizeof(g_observed.states));
    g_observed.state_count = 0;
    g_observed.link_count = 0;
    g_observed.net_up_seen = false;
    xiaotai_transport_host_clock_manual(T0);
    xiaotai_eventlog_reset();
    const xiaotai_eventlog_config_t log = {.capacity = 256};
    CHECK(xiaotai_eventlog_init(&log) == XIAOTAI_OK);
    CHECK(xiaotai_transport_configure(config) == XIAOTAI_OK);
    const xiaotai_module_t *modules[4] = {&g_platform_module, &g_transport_module, &g_observer_module,
                                          &g_requiring_module};
    const xiaotai_framework_config_t framework = {
        .modules = modules, .module_count = options.with_requiring ? 4U : 3U, .available_needs = UINT32_MAX};
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    settle();
}

static inline void boot(const xiaotai_transport_config_t *config)
{
    boot_with(config, (boot_options_t){0});
}

static inline void shutdown_all(void)
{
    xiaotai_framework_reset();
    xiaotai_transport_reset();
    xiaotai_transport_host_reset();
    fakes_reset();
    g_requirement = NULL;
    g_observer_start = NULL;
    g_observed.measure_start = false;
}

/* Identity, endpoint and network in place, announced by one event (each
 * event is a changed start condition, so one is what a single start
 * attempt needs): the engine starts the stack. */
static inline void give_account(void)
{
    fake_identity_set(XIAOTAI_IDENTITY_VALID, "device-test-1", "key-test-1", "");
    fake_platform_endpoint(XIAOTAI_OK, TEST_ENDPOINT);
    fake_platform_phase(XIAOTAI_PLATFORM_ONLINE);
    fake_net_set(XIAOTAI_NET_UP, true);
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_UP);
}

/* Boots, starts the stack and reports it online. */
static inline void boot_online(const xiaotai_transport_config_t *config)
{
    boot(config);
    give_account();
    CHECK(calls(XIAOTAI_TRANSPORT_HOST_START) == 1U);
    xiaotai_transport_host_started();
    settle();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
}
