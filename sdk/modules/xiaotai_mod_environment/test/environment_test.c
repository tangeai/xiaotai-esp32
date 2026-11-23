// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/regression/test_environment_startup.py
// Replaces legacy: tests/xmini/test_product.c::product
// Replaces legacy: tests/regression/test_service_environment.py
// Replaces legacy: tests/xmini/test_runtime_wifi.c::runtime_wifi
// Replaces legacy: tests/regression/test_environment_console.py
// Replaces legacy: tests/regression/test_native_preferences.py
// Replaces legacy: tests/regression/test_p4_runtime_maintenance.py
// Replaces legacy: tests/regression/test_service_environment_store.py
// Replaces legacy: tests/regression/test_environment_control.py
// Replaces legacy: tests/regression/test_runtime_maintenance.py
// Replaces legacy: tests/regression/test_recognition_environment.py
/* Host tests of the environment module (work package ME-1): the switch with
 * the real framework (quiesce, guards, timers, settings, orderly restart) and
 * the host port (key-value worker, tasks, memory), the platform core, the
 * network engine and the preferences service faked (fakes.h).
 *
 * One source, one executable per group: ENV_TEST_GROUP names the group
 * (flow, refuse, cancel, timeout, store_timeout, settings_frozen,
 * settings_held, boot_error, restart). The linker wraps
 * xiaotai_quiesce_begin/_end, xiaotai_restart_request, xiaotai_kv_flush, the
 * port allocators and xiaotai_task_create to trace the order of calls, to
 * inject a refused restart and to see where memory and the barrier task's
 * stack are placed. Times are asserted against absolute schedules: never
 * earlier than the configured limit, and within a bounded slack after it. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "environment_internal.h"
#include "fakes.h"
#include "xiaotai/framework.h"
#include "xiaotai/framework_config.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/environment_config.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_kv_host.h"

#ifndef ENV_TEST_GROUP
#error "ENV_TEST_GROUP names the test group"
#endif

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)
#define RUN(step)                    \
    do {                             \
        if ((step) != 0) return 1;   \
    } while (0)

/* A storage error as the key-value port passes it through (an NVS code). */
#define TEST_STORAGE_ERROR 0x1102
#define BASE "http://next.example.test/pre"
/* Slack after a deadline for the owner's 50 ms polling and the host scheduler. */
#define LATE_MS 300

static int64_t now_ms(void) { return xiaotai_time_us() / 1000; }

/* ----------------------------------------------------------------- wrappers */

xiaotai_err_t __real_xiaotai_quiesce_begin(const char *reason, uint32_t *ticket);
void __real_xiaotai_quiesce_end(uint32_t ticket);
xiaotai_err_t __real_xiaotai_restart_request(const char *reason, uint32_t quiesce_ticket);
xiaotai_err_t __real_xiaotai_kv_flush(uint32_t timeout_ms);
void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);

static atomic_uint s_env_quiesce_ticket;
static atomic_int s_restart_inject;
static atomic_uint s_restart_ticket;
static char s_restart_reason[32];
static atomic_uint s_kv_flush_timeout;
static atomic_bool s_watch_alloc;
static atomic_uint s_watched_allocs, s_watched_not_bulk;
static atomic_uint s_barrier_tasks, s_barrier_stack_kind, s_barrier_stack_bytes, s_barrier_priority;

xiaotai_err_t __wrap_xiaotai_quiesce_begin(const char *reason, uint32_t *ticket)
{
    fake_trace("quiesce_begin");
    const xiaotai_err_t err = __real_xiaotai_quiesce_begin(reason, ticket);
    if (err == XIAOTAI_OK && reason && strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0) {
        atomic_store(&s_env_quiesce_ticket, *ticket);
    }
    return err;
}

void __wrap_xiaotai_quiesce_end(uint32_t ticket)
{
    fake_trace("quiesce_end");
    __real_xiaotai_quiesce_end(ticket);
}

xiaotai_err_t __wrap_xiaotai_restart_request(const char *reason, uint32_t quiesce_ticket)
{
    fake_trace("restart_request");
    snprintf(s_restart_reason, sizeof(s_restart_reason), "%s", reason ? reason : "");
    atomic_store(&s_restart_ticket, quiesce_ticket);
    const xiaotai_err_t inject = atomic_load(&s_restart_inject);
    if (inject != XIAOTAI_OK) return inject;
    return __real_xiaotai_restart_request(reason, quiesce_ticket);
}

/* Only the barrier task's flushes are traced: the framework flushes too. */
xiaotai_err_t __wrap_xiaotai_kv_flush(uint32_t timeout_ms)
{
    if (!xiaotai_on_owner()) {
        fake_trace("kv_flush");
        atomic_store(&s_kv_flush_timeout, timeout_ms);
    }
    return __real_xiaotai_kv_flush(timeout_ms);
}

static void watch(xiaotai_mem_kind_t kind)
{
    if (!atomic_load(&s_watch_alloc)) return;
    atomic_fetch_add(&s_watched_allocs, 1U);
    if (kind != XIAOTAI_MEM_BULK) atomic_fetch_add(&s_watched_not_bulk, 1U);
}

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    watch(kind);
    return __real_xiaotai_mem_alloc(kind, size);
}

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    watch(kind);
    return __real_xiaotai_mem_calloc(kind, count, size);
}

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (config->name && strcmp(config->name, ENVIRONMENT_BARRIER_TASK_NAME) == 0) {
        atomic_fetch_add(&s_barrier_tasks, 1U);
        atomic_store(&s_barrier_stack_kind, (unsigned)config->stack);
        atomic_store(&s_barrier_stack_bytes, config->stack_bytes);
        atomic_store(&s_barrier_priority, config->priority);
    }
    return __real_xiaotai_task_create(config, task);
}

/* --------------------------------------------------------------- recorder */

typedef struct {
    xiaotai_environment_phase_t phase;
    uint32_t revision;
    xiaotai_err_t error;
    bool cancel, write_attempted, unconfirmed;
} change_t;

typedef struct {
    xiaotai_environment_result_t result;
    xiaotai_err_t error;
    bool write_attempted, unconfirmed;
} result_t;

#define MAX_CHANGES 64
static xiaotai_mutex_t s_record_lock;
static change_t s_changes[MAX_CHANGES];
static unsigned s_change_count;
static result_t s_results[8];
static unsigned s_result_count;
static atomic_uint s_quiesce_on, s_quiesce_off, s_restarting, s_restarted;
static char s_restarting_reason[XIAOTAI_ARG_TEXT_MAX];
static char s_guard_reason[32];
static atomic_int s_guard_result;
static atomic_uint s_revoked;

static void on_changed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_environment_status_t *status = event->data;
    if (event->size != sizeof(*status)) return;
    xiaotai_mutex_lock(&s_record_lock, XIAOTAI_WAIT_FOREVER);
    if (s_change_count < MAX_CHANGES) {
        s_changes[s_change_count++] = (change_t){
            .phase = status->phase, .revision = status->revision, .error = status->error,
            .cancel = status->cancel_requested, .write_attempted = status->write_attempted,
            .unconfirmed = status->environment.unconfirmed};
    }
    xiaotai_mutex_unlock(&s_record_lock);
    (void)event->args.value;
}

static void on_result(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_environment_status_t *status = event->data;
    if (event->size != sizeof(*status)) return;
    xiaotai_mutex_lock(&s_record_lock, XIAOTAI_WAIT_FOREVER);
    if (s_result_count < 8) {
        s_results[s_result_count++] = (result_t){
            .result = (xiaotai_environment_result_t)event->args.value, .error = status->error,
            .write_attempted = status->write_attempted, .unconfirmed = status->environment.unconfirmed};
    }
    xiaotai_mutex_unlock(&s_record_lock);
}

static void on_quiesce(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    atomic_fetch_add(event->args.value ? &s_quiesce_on : &s_quiesce_off, 1U);
}

static void on_restarting(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    snprintf(s_restarting_reason, sizeof(s_restarting_reason), "%s", event->args.text);
    atomic_fetch_add(&s_restarting, 1U);
}

static unsigned changes(void)
{
    xiaotai_mutex_lock(&s_record_lock, XIAOTAI_WAIT_FOREVER);
    const unsigned count = s_change_count;
    xiaotai_mutex_unlock(&s_record_lock);
    return count;
}

static change_t change_at(unsigned index)
{
    xiaotai_mutex_lock(&s_record_lock, XIAOTAI_WAIT_FOREVER);
    const change_t change = s_changes[index];
    xiaotai_mutex_unlock(&s_record_lock);
    return change;
}

static unsigned results(void)
{
    xiaotai_mutex_lock(&s_record_lock, XIAOTAI_WAIT_FOREVER);
    const unsigned count = s_result_count;
    xiaotai_mutex_unlock(&s_record_lock);
    return count;
}

static result_t result_at(unsigned index)
{
    xiaotai_mutex_lock(&s_record_lock, XIAOTAI_WAIT_FOREVER);
    const result_t result = s_results[index];
    xiaotai_mutex_unlock(&s_record_lock);
    return result;
}

/* ------------------------------------------------------------ test module */

static xiaotai_err_t guard(const char *reason, void *ctx)
{
    (void)ctx;
    snprintf(s_guard_reason, sizeof(s_guard_reason), "%s", reason);
    return strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0 ? atomic_load(&s_guard_result) : XIAOTAI_OK;
}

static const xiaotai_setting_t VOLUME = {
    .name = "test.volume", .key = "tvol", .kind = XIAOTAI_SETTING_INT, .min = 0, .max = 10, .default_value = 5,
};

static xiaotai_err_t probe_init(void)
{
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_ENVIRONMENT_CHANGED, on_changed, NULL);
    if (!err) err = xiaotai_event_subscribe(XIAOTAI_EVENT_ENVIRONMENT_RESULT, on_result, NULL);
    if (!err) err = xiaotai_event_subscribe(XIAOTAI_EVENT_QUIESCE, on_quiesce, NULL);
    if (!err) err = xiaotai_event_subscribe(XIAOTAI_EVENT_RESTARTING, on_restarting, NULL);
    if (!err) err = xiaotai_quiesce_guard_register("probe", guard, NULL);
    if (!err) err = xiaotai_setting_register(&VOLUME);
    return err;
}

static xiaotai_err_t environment_init_watched(void)
{
    atomic_store(&s_watch_alloc, true);
    const xiaotai_err_t err = xiaotai_module_environment_init();
    atomic_store(&s_watch_alloc, false);
    return err;
}

static const xiaotai_priority_t PROBE_ACTIVITIES[] = {{"call", 3}, {NULL, 0}};
static const xiaotai_module_t ENVIRONMENT = {
    .name = "environment", .init = environment_init_watched, .start = xiaotai_module_environment_start,
};
static const xiaotai_module_t PROBE = {.name = "probe", .init = probe_init, .activities = PROBE_ACTIVITIES};
static const xiaotai_module_t *const MODULES[] = {&ENVIRONMENT, &PROBE};

static void restart_now(void) { atomic_fetch_add(&s_restarted, 1U); }

/* -------------------------------------------------------------- harness */

static xiaotai_signal_t s_sync;
static void signal_sync(void *ctx) { (void)ctx; xiaotai_signal_give(&s_sync); }
/* Runs fn on the owner task, then waits until the events fn emitted were
 * dispatched too (they are queued behind a sync posted together with fn). */
static void on_owner(void (*fn)(void *ctx), void *ctx)
{
    (void)xiaotai_run(fn, ctx);
    (void)xiaotai_run(signal_sync, NULL);
    (void)xiaotai_signal_take(&s_sync, 3000);
    (void)xiaotai_run(signal_sync, NULL);
    (void)xiaotai_signal_take(&s_sync, 3000);
}

typedef struct {
    const char *base;
    xiaotai_err_t result;
} call_t;
static void do_set(void *ctx) { call_t *call = ctx; call->result = xiaotai_environment_set(call->base); }
static void do_cancel(void *ctx) { call_t *call = ctx; call->result = xiaotai_environment_cancel(); }
static xiaotai_err_t env_set(const char *base)
{
    call_t call = {.base = base, .result = 12345};
    on_owner(do_set, &call);
    return call.result;
}
static xiaotai_err_t env_cancel(void)
{
    call_t call = {.result = 12345};
    on_owner(do_cancel, &call);
    return call.result;
}

typedef struct {
    int32_t value;
    xiaotai_err_t result;
} setting_call_t;
static void do_setting(void *ctx)
{
    setting_call_t *call = ctx;
    call->result = xiaotai_setting_set_int(VOLUME.name, call->value);
}
static xiaotai_err_t setting_set(int32_t value)
{
    setting_call_t call = {.value = value, .result = 12345};
    on_owner(do_setting, &call);
    return call.result;
}

static void do_complete_store(void *ctx) { fake_platform_complete_store((xiaotai_err_t)(intptr_t)ctx); }
static void complete_store(xiaotai_err_t result) { on_owner(do_complete_store, (void *)(intptr_t)result); }

static xiaotai_environment_status_t status(void)
{
    xiaotai_environment_status_t value;
    xiaotai_environment_status(&value);
    return value;
}

static xiaotai_environment_stats_t stats(void)
{
    xiaotai_environment_stats_t value;
    xiaotai_environment_stats(&value);
    return value;
}

static bool wait_phase(xiaotai_environment_phase_t phase, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 5) {
        if (status().phase == phase) return true;
        xiaotai_delay_ms(5);
    }
    return false;
}

static bool wait_count(atomic_uint *counter, unsigned count, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 5) {
        if (atomic_load(counter) >= count) return true;
        xiaotai_delay_ms(5);
    }
    return false;
}

static bool wait_results(unsigned count, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 5) {
        if (results() >= count) return true;
        xiaotai_delay_ms(5);
    }
    return false;
}

static void dump_trace(void)
{
    char trace[2048];
    fake_trace_get(trace, sizeof(trace));
    fprintf(stderr, "trace: %s\n", trace);
}

static bool in_order(const char *const *words)
{
    if (fake_trace_in_order(words)) return true;
    dump_trace();
    return false;
}

static int setup(const xiaotai_environment_config_t *config)
{
    fake_platform_reset();
    fake_net_reset();
    fake_preferences_reset();
    fake_trace_clear();
    xiaotai_kv_host_clear();
    xiaotai_mutex_lock(&s_record_lock, XIAOTAI_WAIT_FOREVER);
    s_change_count = s_result_count = 0;
    xiaotai_mutex_unlock(&s_record_lock);
    atomic_store(&s_quiesce_on, 0U);
    atomic_store(&s_quiesce_off, 0U);
    atomic_store(&s_restarting, 0U);
    atomic_store(&s_restarted, 0U);
    atomic_store(&s_guard_result, XIAOTAI_OK);
    atomic_store(&s_restart_inject, XIAOTAI_OK);
    atomic_store(&s_env_quiesce_ticket, 0U);
    atomic_store(&s_restart_ticket, 0U);
    atomic_store(&s_watched_allocs, 0U);
    atomic_store(&s_watched_not_bulk, 0U);
    atomic_store(&s_barrier_tasks, 0U);
    atomic_store(&s_revoked, 0U);
    s_restart_reason[0] = s_restarting_reason[0] = s_guard_reason[0] = '\0';
    CHECK(xiaotai_environment_configure(config) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {
        .modules = MODULES, .module_count = 2, .available_needs = UINT32_MAX,
        .restart_delay_ms = 20, .restart = restart_now,
    };
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    on_owner(signal_sync, NULL);
    return 0;
}

static void teardown(void)
{
    fake_preferences_unblock();
    xiaotai_kv_host_gate(true);
    xiaotai_framework_reset();
    xiaotai_environment_reset();
}

/* Gates the module holds right now, as the fakes and the framework see them. */
static bool everything_released(void)
{
    return !atomic_load(&g_platform.gate_closed) && !atomic_load(&g_net.held) && !xiaotai_quiesced();
}
static bool everything_held(void)
{
    return atomic_load(&g_platform.gate_closed) && atomic_load(&g_net.held) && xiaotai_quiesced();
}

static const char *const RELEASE_ORDER[] = {"gate_open", "net_end", "quiesce_end", NULL};

/* ------------------------------------------------------------------- flow */

/* The successful switch, step by step (ports tests/environment/
 * test_environment_control.c success cases, test_environment_owners.py and
 * the environment part of tests/xmini/test_runtime_wifi.c). */
static int test_flow(void)
{
    RUN(setup(NULL));
    /* The module's state is BULK memory, taken once at init. */
    CHECK(atomic_load(&s_watched_allocs) >= 1U && atomic_load(&s_watched_not_bulk) == 0U);
    xiaotai_environment_config_t effective;
    environment_effective_config(&effective);
    CHECK(effective.wait_ms == 10000U && effective.release_ms == 5000U);
    xiaotai_environment_status_t before = status();
    CHECK(before.phase == XIAOTAI_ENVIRONMENT_IDLE && before.revision == 0 && before.error == XIAOTAI_OK);
    CHECK(strcmp(before.environment.active, "http://active.example.test/services") == 0);
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_ENVIRONMENT_RESET));
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_ENVIRONMENT_CANCEL));

    /* The platform has requests in flight: the switch waits, polling every
     * 50 ms, and starts no barrier. */
    atomic_store(&g_platform.drained, false);
    CHECK(env_set("https://next.example.test/pre//") == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_WAITING);
    CHECK(everything_held());
    const unsigned checks_before = atomic_load(&g_platform.drained_checks);
    xiaotai_delay_ms(500);
    const unsigned checks = atomic_load(&g_platform.drained_checks) - checks_before;
    CHECK(checks >= 6U && checks <= 12U);
    CHECK(atomic_load(&g_preferences.flushes) == 0U && atomic_load(&g_platform.stores) == 0U);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_WAITING);

    atomic_store(&g_platform.drained, true);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_RESTARTING, 3000));
    CHECK(wait_count(&s_restarted, 1U, 3000));
    static const char *const order[] = {"quiesce_begin", "net_begin", "gate_close", "pending", "pref_flush",
                                        "kv_flush", "store(" BASE ")", "net_restart", "restart_request", NULL};
    CHECK(in_order(order));
    CHECK(fake_trace_count("gate_open") == 0 && fake_trace_count("net_end") == 0 &&
          fake_trace_count("quiesce_end") == 0);
    CHECK(strcmp(g_platform.stored, BASE) == 0 && !atomic_load(&g_platform.store_was_null));

    /* The barrier ran on its own low-priority task with a BULK stack, within
     * the default 10 s wait. */
    CHECK(atomic_load(&s_barrier_tasks) == 1U);
    CHECK(atomic_load(&s_barrier_stack_kind) == (unsigned)XIAOTAI_MEM_BULK);
    CHECK(atomic_load(&s_barrier_stack_bytes) == ENVIRONMENT_BARRIER_STACK_BYTES);
    CHECK(atomic_load(&s_barrier_priority) < XIAOTAI_FRAMEWORK_OWNER_PRIORITY);
    CHECK(!atomic_load(&g_preferences.flushed_on_owner));
    const unsigned flush_timeout = atomic_load(&g_preferences.last_timeout_ms);
    CHECK(flush_timeout <= 10000U && flush_timeout >= 9000U);
    CHECK(atomic_load(&s_kv_flush_timeout) <= flush_timeout);

    /* Events: WAITING, WRITING, RESTARTING, one revision each; one SAVED. */
    CHECK(changes() == 3);
    CHECK(change_at(0).phase == XIAOTAI_ENVIRONMENT_WAITING && change_at(0).revision == 1 && !change_at(0).write_attempted);
    CHECK(change_at(1).phase == XIAOTAI_ENVIRONMENT_WRITING && change_at(1).revision == 2 && change_at(1).write_attempted);
    CHECK(change_at(2).phase == XIAOTAI_ENVIRONMENT_RESTARTING && change_at(2).revision == 3);
    CHECK(results() == 1 && result_at(0).result == XIAOTAI_ENVIRONMENT_SAVED && result_at(0).error == XIAOTAI_OK);
    const xiaotai_environment_status_t after = status();
    CHECK(after.phase == XIAOTAI_ENVIRONMENT_RESTARTING && after.write_attempted && after.error == XIAOTAI_OK);
    const xiaotai_environment_stats_t counted = stats();
    CHECK(counted.requests == 1 && counted.saved == 1 && counted.failed == 0 && counted.cancelled == 0 &&
          counted.refused_busy == 0 && counted.refused_guard == 0 && counted.release_failures == 0);
    CHECK(atomic_load(&s_restarting) == 1U && strcmp(s_restarting_reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0);
    teardown();

    /* Back to the built-in address through the intent. */
    RUN(setup(NULL));
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_ENVIRONMENT_RESET, NULL) == XIAOTAI_OK);
    CHECK(wait_count(&s_restarted, 1U, 3000));
    CHECK(atomic_load(&g_platform.store_was_null) && atomic_load(&g_platform.stores) == 1U);
    static const char *const reset_order[] = {"gate_close", "pref_flush", "store(NULL)", "restart_request", NULL};
    CHECK(in_order(reset_order));
    teardown();
    return 0;
}

/* ----------------------------------------------------------------- refuse */

/* Refusals (ports tests/environment/test_runtime_maintenance.c and
 * test_p4_runtime_maintenance.c, tests/regression/test_runtime_maintenance.py,
 * test_p4_runtime_maintenance.py, the admission cases of
 * test_environment_control.c and test_mz28_environment_console.py, and the
 * "maintenance refused during a call" case of the retired tests/mz28/test_voip_join_lifetime.c). */

static uint32_t s_activity;
static void revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id; (void)reason; (void)ctx;
    atomic_fetch_add(&s_revoked, 1U);
}
static void do_begin_call(void *ctx)
{
    xiaotai_err_t *err = ctx;
    static const xiaotai_activity_t call = {.name = "call", .resources = XIAOTAI_RESOURCE_AUDIO_OUT,
                                            .revoked = revoked};
    *err = xiaotai_activity_begin(&call, &s_activity);
}
static void do_end_call(void *ctx) { (void)ctx; xiaotai_activity_end(s_activity); }
static uint32_t s_other_quiesce;
static void do_quiesce_other(void *ctx)
{
    xiaotai_err_t *err = ctx;
    *err = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_COPROCESSOR, &s_other_quiesce);
}
static void do_end_other(void *ctx) { (void)ctx; xiaotai_quiesce_end(s_other_quiesce); }
static void do_restart_network(void *ctx)
{
    xiaotai_err_t *err = ctx;
    xiaotai_restart_hold();
    *err = xiaotai_restart_request("network", 0);
}

/* A refusal after the base was accepted: returned, recorded in the status
 * with one revision and one changed event, no result, nothing written. */
static int check_recorded(xiaotai_err_t expected, uint32_t revision_before, unsigned changes_before)
{
    const xiaotai_environment_status_t now = status();
    CHECK(now.phase == XIAOTAI_ENVIRONMENT_IDLE && now.error == expected);
    CHECK(!now.write_attempted && !now.cancel_requested);
    CHECK(now.revision == revision_before + 1U);
    CHECK(changes() == changes_before + 1U);
    CHECK(change_at(changes_before).error == expected && change_at(changes_before).phase == XIAOTAI_ENVIRONMENT_IDLE);
    CHECK(results() == 0 && atomic_load(&g_platform.stores) == 0U);
    CHECK(everything_released());
    return 0;
}

static int test_refuse(void)
{
    /* Before the module exists. */
    CHECK(xiaotai_environment_set(BASE) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_environment_cancel() == XIAOTAI_ERR_INVALID_STATE);

    RUN(setup(NULL));
    /* A malformed base: returned only, the status untouched. */
    CHECK(env_set("ftp://next.example.test") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(env_set("http://bad host.example.test") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(status().revision == 0 && changes() == 0 && fake_trace_count("quiesce_begin") == 0);

    /* The quiesce guard refuses with INVALID_STATE: returned as it is,
     * counted, recorded; nothing beyond the quiesce was asked for. */
    atomic_store(&s_guard_result, XIAOTAI_ERR_INVALID_STATE);
    CHECK(env_set(BASE) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(strcmp(s_guard_reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0);
    RUN(check_recorded(XIAOTAI_ERR_INVALID_STATE, 0, 0));
    CHECK(stats().refused_guard == 1 && stats().refused_busy == 0 && stats().requests == 0);
    CHECK(fake_trace_count("net_begin") == 0 && fake_trace_count("gate_close") == 0);

    /* ... and with a storage error (a guard that could not read its record). */
    atomic_store(&s_guard_result, TEST_STORAGE_ERROR);
    CHECK(env_set(NULL) == TEST_STORAGE_ERROR);
    RUN(check_recorded(TEST_STORAGE_ERROR, 1, 1));
    CHECK(stats().refused_guard == 2);

    /* The guard only vetoes this reason: another quiesce passes it. */
    xiaotai_err_t err = 12345;
    on_owner(do_quiesce_other, &err);
    CHECK(err == XIAOTAI_OK && strcmp(s_guard_reason, XIAOTAI_QUIESCE_COPROCESSOR) == 0);
    /* While quiesced by another holder the switch is refused as BUSY, and
     * that quiesce is not touched. */
    fake_trace_clear();
    CHECK(env_set(BASE) == XIAOTAI_ERR_BUSY);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_IDLE && status().error == XIAOTAI_ERR_BUSY);
    CHECK(status().revision == 3 && changes() == 3 && results() == 0);
    CHECK(stats().refused_busy == 1 && xiaotai_quiesced());
    CHECK(fake_trace_count("quiesce_begin") == 0 && fake_trace_count("quiesce_end") == 0);
    on_owner(do_end_other, NULL);
    CHECK(!xiaotai_quiesced());
    atomic_store(&s_guard_result, XIAOTAI_OK);

    /* A current activity (a call) is never ended by a switch. */
    on_owner(do_begin_call, &err);
    CHECK(err == XIAOTAI_OK);
    uint32_t revision = status().revision;
    unsigned before = changes();
    fake_trace_clear();
    CHECK(env_set(BASE) == XIAOTAI_ERR_BUSY);
    RUN(check_recorded(XIAOTAI_ERR_BUSY, revision, before));
    CHECK(atomic_load(&s_revoked) == 0U && fake_trace_count("quiesce_begin") == 0);
    CHECK(stats().refused_busy == 2);
    on_owner(do_end_call, NULL);

    /* The network engine's maintenance gate cannot be taken (held, a
     * submission, a coprocessor operation): the quiesce is given back. */
    atomic_store(&g_net.begin_result, XIAOTAI_ERR_INVALID_STATE);
    revision = status().revision;
    before = changes();
    fake_trace_clear();
    CHECK(env_set(BASE) == XIAOTAI_ERR_INVALID_STATE);
    RUN(check_recorded(XIAOTAI_ERR_INVALID_STATE, revision, before));
    static const char *const net_order[] = {"quiesce_begin", "net_begin", "quiesce_end", NULL};
    CHECK(in_order(net_order) && fake_trace_count("gate_close") == 0);
    CHECK(stats().refused_busy == 3);
    atomic_store(&g_net.begin_result, XIAOTAI_OK);

    /* The platform gate is already closed by someone else. */
    atomic_store(&g_platform.gate_close_result, XIAOTAI_ERR_PLATFORM_GATED);
    revision = status().revision;
    before = changes();
    fake_trace_clear();
    CHECK(env_set(BASE) == XIAOTAI_ERR_PLATFORM_GATED);
    RUN(check_recorded(XIAOTAI_ERR_PLATFORM_GATED, revision, before));
    static const char *const gate_order[] = {"quiesce_begin", "net_begin", "gate_close", "net_end", "quiesce_end",
                                             NULL};
    CHECK(in_order(gate_order));
    CHECK(stats().refused_busy == 4);
    atomic_store(&g_platform.gate_close_result, XIAOTAI_OK);

    /* A switch in progress: a second request is refused and changes nothing,
     * a malformed one is still refused for its base first. */
    atomic_store(&g_platform.drained, false);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    const xiaotai_environment_status_t busy = status();
    before = changes();
    CHECK(env_set("http://other.example.test") == XIAOTAI_ERR_INVALID_STATE);
    CHECK(env_set(NULL) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(env_set("not a url") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(status().revision == busy.revision && status().error == XIAOTAI_OK && changes() == before);
    CHECK(stats().requests == 1);
    CHECK(env_cancel() == XIAOTAI_OK && wait_results(1, 2000));

    /* A restart is pending (held): refused as BUSY. */
    on_owner(do_restart_network, &err);
    CHECK(err == XIAOTAI_OK && xiaotai_restart_pending());
    CHECK(env_set(BASE) == XIAOTAI_ERR_BUSY);
    CHECK(status().error == XIAOTAI_ERR_BUSY && stats().refused_busy == 5);
    CHECK(atomic_load(&s_restarted) == 0U);
    teardown();

    /* After the module is gone. */
    CHECK(xiaotai_environment_set(BASE) == XIAOTAI_ERR_INVALID_STATE);
    return 0;
}

/* ----------------------------------------------------------------- cancel */

static int check_cancelled(void)
{
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_CANCELLED && result_at(0).error == XIAOTAI_OK);
    CHECK(!result_at(0).write_attempted);
    const xiaotai_environment_status_t now = status();
    CHECK(now.phase == XIAOTAI_ENVIRONMENT_IDLE && now.error == XIAOTAI_OK && !now.cancel_requested);
    CHECK(atomic_load(&g_platform.stores) == 0U && everything_released());
    CHECK(in_order(RELEASE_ORDER));
    CHECK(stats().cancelled == 1 && stats().failed == 0);
    return 0;
}

/* Cancel in the three phases that accept it, refused once the store began
 * (ports the cancel cases of test_environment_control.c and of
 * test_mz28_environment_console.py). */
static int test_cancel(void)
{
    /* Nothing to cancel. */
    RUN(setup(NULL));
    CHECK(env_cancel() == XIAOTAI_ERR_INVALID_STATE);
    teardown();

    /* ACQUIRING: a cancel that arrives while the gates are being taken. */
    RUN(setup(NULL));
    atomic_store(&g_net.cancel_in_begin, true);
    atomic_store(&g_net.cancel_in_begin_result, 12345);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(atomic_load(&g_net.cancel_in_begin_result) == XIAOTAI_OK);
    RUN(check_cancelled());
    CHECK(change_at(0).phase == XIAOTAI_ENVIRONMENT_ACQUIRING && change_at(0).cancel);
    CHECK(atomic_load(&g_preferences.flushes) == 0U);
    teardown();

    /* WAITING, no barrier running: released before cancel returns. */
    RUN(setup(NULL));
    atomic_store(&g_platform.drained, false);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    xiaotai_delay_ms(120);
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_IDLE);
    RUN(check_cancelled());
    teardown();

    /* WAITING while the storage barrier runs: the cancel waits for it. */
    RUN(setup(NULL));
    atomic_store(&g_preferences.block, true);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(fake_preferences_wait_blocked(2000));
    CHECK(env_cancel() == XIAOTAI_OK);
    xiaotai_environment_status_t now = status();
    CHECK(now.phase == XIAOTAI_ENVIRONMENT_WAITING && now.cancel_requested);
    CHECK(everything_held() && results() == 0);
    fake_preferences_unblock();
    RUN(check_cancelled());
    teardown();

    /* Intent. */
    RUN(setup(NULL));
    atomic_store(&g_platform.drained, false);
    CHECK(env_set(NULL) == XIAOTAI_OK);
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_ENVIRONMENT_CANCEL, NULL) == XIAOTAI_OK);
    RUN(check_cancelled());
    teardown();

    /* WRITING and RESTARTING: a store that began cannot be undone. */
    RUN(setup(NULL));
    atomic_store(&g_platform.store_manual, true);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_WRITING, 2000));
    CHECK(env_cancel() == XIAOTAI_ERR_INVALID_STATE);
    CHECK(env_set("http://other.example.test") == XIAOTAI_ERR_INVALID_STATE);
    now = status();
    CHECK(now.phase == XIAOTAI_ENVIRONMENT_WRITING && now.write_attempted && !now.cancel_requested);
    complete_store(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_RESTARTING);
    CHECK(env_cancel() == XIAOTAI_ERR_INVALID_STATE);
    CHECK(wait_count(&s_restarted, 1U, 2000));
    CHECK(stats().cancelled == 0 && stats().saved == 1);
    teardown();
    return 0;
}

/* ---------------------------------------------------------------- timeout */

/* Bounded waits and the release that cannot finish (ports the timeout and
 * ERROR_HELD cases of test_environment_control.c and the 60 s / unlimited
 * waits of the other two families as configuration). */
static int test_timeout(void)
{
    /* The gate never drains. */
    xiaotai_environment_config_t config = {.wait_ms = 300};
    RUN(setup(&config));
    atomic_store(&g_platform.drained, false);
    int64_t started = now_ms();
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 3000));
    int64_t elapsed = now_ms() - started;
    CHECK(elapsed >= 300 && elapsed < 300 + LATE_MS);
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(0).error == XIAOTAI_ERR_TIMEOUT);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_IDLE && everything_released() && in_order(RELEASE_ORDER));
    CHECK(stats().wait_timeouts == 1 && stats().failed == 1);
    CHECK(atomic_load(&g_preferences.flushes) == 0U && atomic_load(&g_platform.stores) == 0U);
    teardown();

    /* The settings never settle: the barrier gets what is left of the wait. */
    config = (xiaotai_environment_config_t){.wait_ms = 400};
    RUN(setup(&config));
    atomic_store(&g_preferences.block, true);
    started = now_ms();
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 3000));
    elapsed = now_ms() - started;
    CHECK(elapsed >= 400 && elapsed < 400 + LATE_MS);
    CHECK(atomic_load(&g_preferences.last_timeout_ms) <= 400U && atomic_load(&g_preferences.last_timeout_ms) >= 250U);
    CHECK(result_at(0).error == XIAOTAI_ERR_TIMEOUT && stats().wait_timeouts == 1);
    CHECK(atomic_load(&g_platform.stores) == 0U && everything_released());
    teardown();

    /* An earlier write is still queued on the storage worker. */
    config = (xiaotai_environment_config_t){.wait_ms = 400};
    RUN(setup(&config));
    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_kv_set_int_async("t_env", "queued", XIAOTAI_KV_I32, 1) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(2000));
    started = now_ms();
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 3000));
    elapsed = now_ms() - started;
    CHECK(elapsed >= 400 && elapsed < 400 + LATE_MS);
    static const char *const kv_order[] = {"pref_flush", "kv_flush", "gate_open", NULL};
    CHECK(in_order(kv_order));
    CHECK(result_at(0).error == XIAOTAI_ERR_TIMEOUT && atomic_load(&g_platform.stores) == 0U);
    teardown();

    /* A release step fails: ERROR_HELD with the rest still held; a cancel
     * releases again. */
    config = (xiaotai_environment_config_t){.wait_ms = 200};
    RUN(setup(&config));
    atomic_store(&g_platform.drained, false);
    atomic_store(&g_platform.gate_open_result_once, XIAOTAI_ERR_INVALID_ARG);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_ERROR_HELD, 3000));
    CHECK(status().error == XIAOTAI_ERR_INVALID_ARG && everything_held());
    CHECK(stats().release_failures == 1 && results() == 0);
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(0).error == XIAOTAI_ERR_INVALID_ARG);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_IDLE && everything_released());
    CHECK(fake_trace_count("gate_open") == 2);
    teardown();

    /* No time limit: a barrier that times out is tried again, in slices. */
    config = (xiaotai_environment_config_t){.wait_ms = XIAOTAI_WAIT_FOREVER};
    RUN(setup(&config));
    g_preferences.script[0] = XIAOTAI_ERR_TIMEOUT;
    g_preferences.script[1] = XIAOTAI_ERR_TIMEOUT;
    atomic_store(&g_preferences.script_length, 2U);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_RESTARTING, 3000));
    CHECK(atomic_load(&g_preferences.flushes) == 3U);
    CHECK(atomic_load(&g_preferences.last_timeout_ms) == ENVIRONMENT_BARRIER_SLICE_MS);
    CHECK(stats().wait_timeouts == 0 && atomic_load(&s_barrier_tasks) == 3U);
    teardown();

    /* No time limit: an undrained gate waits past any default until cancelled. */
    RUN(setup(&config));
    atomic_store(&g_platform.drained, false);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    xiaotai_delay_ms(600);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_WAITING && results() == 0);
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(wait_results(1, 2000) && result_at(0).result == XIAOTAI_ENVIRONMENT_CANCELLED);
    teardown();

    /* Configuration: defaults, overrides, refused after init. */
    config = (xiaotai_environment_config_t){.wait_ms = 60000};
    RUN(setup(&config));
    xiaotai_environment_config_t effective;
    environment_effective_config(&effective);
    CHECK(effective.wait_ms == 60000U && effective.release_ms == 5000U);
    CHECK(xiaotai_environment_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    teardown();
    return 0;
}

/* ---------------------------------------------------------- timer capacity */

static void unused_timer(void *ctx) { (void)ctx; }

static void fill_timers(void *ctx)
{
    uint32_t *last = ctx;
    uint32_t timer;
    *last = 0;
    while (xiaotai_timer_start(60000, unused_timer, NULL, &timer) == XIAOTAI_OK) *last = timer;
}

static void free_timer(void *ctx) { xiaotai_timer_cancel(*(const uint32_t *)ctx); }

static int test_timer_capacity(void)
{
    RUN(setup(NULL));
    uint32_t last;
    on_owner(fill_timers, &last);
    CHECK(last != 0);
    const uint32_t revision_before = status().revision;
    const unsigned changes_before = changes();
    CHECK(env_set(BASE) == XIAOTAI_ERR_NO_MEM);
    RUN(check_recorded(XIAOTAI_ERR_NO_MEM, revision_before, changes_before));
    CHECK(in_order(RELEASE_ORDER) && stats().requests == 0U);
    CHECK(!xiaotai_restart_pending());

    on_owner(free_timer, &last);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_SAVED && atomic_load(&g_platform.stores) == 1U);
    teardown();

    /* A slice of an unlimited wait ends while other work owns its former
     * timer slot. Since this switch already began, report a failed result. */
    const xiaotai_environment_config_t config = {.wait_ms = XIAOTAI_WAIT_FOREVER};
    RUN(setup(&config));
    atomic_store(&g_preferences.block, true);
    atomic_store(&g_preferences.flush_result, XIAOTAI_ERR_TIMEOUT);
    CHECK(env_set(BASE) == XIAOTAI_OK && fake_preferences_wait_blocked(2000));
    on_owner(fill_timers, &last);
    CHECK(last != 0 && everything_held());
    fake_preferences_unblock();
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(0).error == XIAOTAI_ERR_NO_MEM);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_IDLE && everything_released() && in_order(RELEASE_ORDER));
    CHECK(!result_at(0).write_attempted && atomic_load(&g_platform.stores) == 0U);
    CHECK(stats().wait_timeouts == 0U && stats().failed == 1U && !xiaotai_restart_pending());
    teardown();
    return 0;
}

/* ----------------------------------------------------------- store timeout */

/* A store that timed out: the accepted write may still run on the storage
 * worker, so nothing is released before a key-value barrier passed (ports the
 * companion branch of test_environment_control.c). */
static int test_store_timeout(void)
{
    xiaotai_environment_config_t config = {.release_ms = 300};
    RUN(setup(&config));
    atomic_store(&g_platform.store_manual, true);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_WRITING, 2000));
    /* The write is stuck on the storage worker. */
    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_kv_set_int_async("t_env", "write", XIAOTAI_KV_I32, 2) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(2000));
    fake_trace_clear();
    int64_t started = now_ms();
    complete_store(XIAOTAI_ERR_TIMEOUT);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_ERROR_HELD, 3000));
    int64_t elapsed = now_ms() - started;
    CHECK(elapsed >= 300 && elapsed < 300 + LATE_MS);
    xiaotai_environment_status_t now = status();
    CHECK(now.error == XIAOTAI_ERR_TIMEOUT && now.write_attempted && now.environment.unconfirmed);
    CHECK(everything_held() && fake_trace_count("gate_open") == 0 && fake_trace_count("kv_flush") == 1);
    CHECK(stats().store_timeouts == 1 && stats().release_failures == 1 && results() == 0);
    CHECK(atomic_load(&s_kv_flush_timeout) == 300U);

    /* Cancel redoes the barrier first; the storage is still stuck. */
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_RELEASING);
    xiaotai_delay_ms(50);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_ERROR_HELD, 3000));
    CHECK(everything_held() && fake_trace_count("kv_flush") == 2 && stats().release_failures == 2);

    /* The write ends: the barrier passes and everything is released. */
    xiaotai_kv_host_gate(true);
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(0).error == XIAOTAI_ERR_TIMEOUT);
    CHECK(result_at(0).write_attempted && result_at(0).unconfirmed);
    static const char *const order[] = {"kv_flush", "kv_flush", "kv_flush", "gate_open", "net_end", "quiesce_end", NULL};
    CHECK(in_order(order));
    now = status();
    CHECK(now.phase == XIAOTAI_ENVIRONMENT_IDLE && now.environment.unconfirmed && now.write_attempted);
    CHECK(everything_released() && atomic_load(&s_restarted) == 0U);
    teardown();

    /* Timed out, but the storage worker is free: one barrier, then released. */
    RUN(setup(&config));
    atomic_store(&g_platform.store_result, XIAOTAI_ERR_TIMEOUT);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    static const char *const free_order[] = {"store(" BASE ")", "kv_flush", "gate_open", "net_end", "quiesce_end",
                                             NULL};
    CHECK(in_order(free_order));
    CHECK(result_at(0).error == XIAOTAI_ERR_TIMEOUT && status().environment.unconfirmed);
    CHECK(stats().store_timeouts == 1 && stats().release_failures == 0 && everything_released());
    teardown();

    /* Any other store failure: no release barrier, no restart, no retry. */
    RUN(setup(&config));
    atomic_store(&g_platform.store_result, TEST_STORAGE_ERROR);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    static const char *const after_store[] = {"store(" BASE ")", "kv_flush", NULL};
    CHECK(!fake_trace_in_order(after_store));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(0).error == TEST_STORAGE_ERROR);
    CHECK(result_at(0).write_attempted && atomic_load(&g_platform.stores) == 1U);
    CHECK(stats().store_timeouts == 0 && everything_released() && atomic_load(&s_restarted) == 0U);
    teardown();

    /* The store cannot even start. */
    RUN(setup(&config));
    atomic_store(&g_platform.store_call_result, XIAOTAI_ERR_NO_MEM);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).error == XIAOTAI_ERR_NO_MEM && everything_released());
    teardown();
    return 0;
}

/* -------------------------------------------------------- settings frozen */

/* Settings are frozen from the quiesce on, and accepted writes finish before
 * the store (ports the "maintenance not ready while settings are unsaved"
 * part of tests/environment/test_native_preferences.c and the settings part
 * of tests/xmini/test_runtime_wifi.c). */
static int test_settings_frozen(void)
{
    RUN(setup(NULL));
    CHECK(setting_set(7) == XIAOTAI_OK);
    atomic_store(&g_preferences.pending, true);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(setting_set(3) == XIAOTAI_ERR_BUSY);
    int32_t value = 0;
    CHECK(xiaotai_setting_get_int(VOLUME.name, &value) == XIAOTAI_OK && value == 7);
    xiaotai_delay_ms(300);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_WAITING);
    CHECK(atomic_load(&g_preferences.flushes) == 0U && atomic_load(&g_platform.stores) == 0U);
    CHECK(fake_trace_count("pending") >= 4);
    atomic_store(&g_preferences.pending, false);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_RESTARTING, 3000));
    static const char *const order[] = {"pending", "pref_flush", "kv_flush", "store(" BASE ")", NULL};
    CHECK(in_order(order));
    teardown();

    /* A setting could not be written: by default the switch fails and lets go. */
    RUN(setup(NULL));
    atomic_store(&g_preferences.flush_result, TEST_STORAGE_ERROR);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(0).error == TEST_STORAGE_ERROR);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_IDLE && everything_released());
    CHECK(stats().release_failures == 0 && atomic_load(&g_platform.stores) == 0U);
    CHECK(setting_set(4) == XIAOTAI_OK);
    teardown();
    return 0;
}

/* The same with XIAOTAI_ENVIRONMENT_HOLD_ON_SETTINGS_ERROR on: the switch
 * stops with every gate held until a cancel. */
static int test_settings_held(void)
{
    CHECK(XIAOTAI_ENVIRONMENT_HOLD_ON_SETTINGS_ERROR == 1);
    RUN(setup(NULL));
    atomic_store(&g_preferences.flush_result, TEST_STORAGE_ERROR);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_ERROR_HELD, 2000));
    CHECK(status().error == TEST_STORAGE_ERROR && everything_held());
    CHECK(setting_set(2) == XIAOTAI_ERR_BUSY);
    xiaotai_delay_ms(200);
    CHECK(status().phase == XIAOTAI_ENVIRONMENT_ERROR_HELD && results() == 0);
    CHECK(stats().release_failures == 1 && atomic_load(&g_platform.stores) == 0U);
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(wait_results(1, 2000));
    CHECK(result_at(0).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(0).error == TEST_STORAGE_ERROR);
    CHECK(everything_released() && setting_set(2) == XIAOTAI_OK);
    teardown();

    /* A timeout is never held. */
    xiaotai_environment_config_t config = {.wait_ms = 300};
    RUN(setup(&config));
    atomic_store(&g_preferences.block, true);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_results(1, 3000));
    CHECK(result_at(0).error == XIAOTAI_ERR_TIMEOUT && everything_released());
    CHECK(stats().release_failures == 0);
    teardown();
    return 0;
}

/* ------------------------------------------------------------- boot error */

/* The stored environment could not be read at boot: the platform stays
 * stopped, and the switch is exactly how the device is repaired (ports
 * tests/regression/test_environment_startup.py, test_p4_environment_startup.py
 * and the offline repair of test_mz28_environment_console.py). */
static int test_boot_error(void)
{
    RUN(setup(NULL));
    atomic_store(&g_platform.boot_error, TEST_STORAGE_ERROR);
    xiaotai_environment_status_t now = status();
    CHECK(now.environment.boot_error == TEST_STORAGE_ERROR && now.environment.active[0] == '\0');
    CHECK(env_set("http://repair.example.test/") == XIAOTAI_OK);
    CHECK(wait_count(&s_restarted, 1U, 3000));
    CHECK(strcmp(g_platform.stored, "http://repair.example.test") == 0);
    CHECK(results() == 1 && result_at(0).result == XIAOTAI_ENVIRONMENT_SAVED);
    teardown();

    RUN(setup(NULL));
    atomic_store(&g_platform.boot_error, TEST_STORAGE_ERROR);
    CHECK(env_set(NULL) == XIAOTAI_OK);
    CHECK(wait_count(&s_restarted, 1U, 3000));
    CHECK(atomic_load(&g_platform.store_was_null));
    teardown();
    return 0;
}

/* ---------------------------------------------------------------- restart */

static void do_hold(void *ctx) { (void)ctx; xiaotai_restart_hold(); }

/* After the store the quiesce goes to the orderly restart and is never
 * released; restart holds delay it; a refused hand-over is held. */
static int test_restart(void)
{
    RUN(setup(NULL));
    on_owner(do_hold, NULL);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_RESTARTING, 3000));
    /* The phase is published before the owner hands its ticket to restart. */
    CHECK(wait_count(&s_restart_ticket, 1U, 3000));
    static const char *const order[] = {"store(" BASE ")", "net_restart", "restart_request", NULL};
    CHECK(in_order(order));
    CHECK(strcmp(s_restart_reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0);
    CHECK(atomic_load(&s_env_quiesce_ticket) != 0U &&
          atomic_load(&s_restart_ticket) == atomic_load(&s_env_quiesce_ticket));
    CHECK(atomic_load(&g_net.restart_ticket) == 201U && atomic_load(&g_net.restart_locked));
    xiaotai_delay_ms(250);
    CHECK(atomic_load(&s_restarted) == 0U && atomic_load(&s_restarting) == 0U);
    CHECK(xiaotai_quiesced() && atomic_load(&s_quiesce_off) == 0U && fake_trace_count("quiesce_end") == 0);
    CHECK(atomic_load(&g_platform.gate_closed));
    xiaotai_restart_release();
    CHECK(wait_count(&s_restarted, 1U, 2000));
    CHECK(atomic_load(&s_restarting) == 1U && strcmp(s_restarting_reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0);
    CHECK(results() == 1 && result_at(0).result == XIAOTAI_ENVIRONMENT_SAVED);
    CHECK(atomic_load(&s_quiesce_off) == 0U);
    teardown();

    /* The framework refuses the hand-over: ERROR_HELD, everything still held. */
    RUN(setup(NULL));
    atomic_store(&s_restart_inject, XIAOTAI_ERR_BUSY);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_ERROR_HELD, 3000));
    CHECK(status().error == XIAOTAI_ERR_BUSY && status().write_attempted && xiaotai_quiesced());
    CHECK(atomic_load(&g_platform.gate_closed) && stats().release_failures == 1 && stats().saved == 1);
    /* The phase is directly visible before the owner dispatches the queued result. */
    CHECK(wait_results(1, 2000));
    CHECK(results() == 1 && result_at(0).result == XIAOTAI_ENVIRONMENT_SAVED);
    xiaotai_delay_ms(100);
    CHECK(atomic_load(&s_restarted) == 0U);
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(wait_results(2, 2000));
    CHECK(result_at(1).result == XIAOTAI_ENVIRONMENT_FAILED && result_at(1).error == XIAOTAI_ERR_BUSY);
    CHECK(!xiaotai_quiesced() && !atomic_load(&g_platform.gate_closed));
    teardown();

    /* The network engine refuses to lock its gate: no restart request. */
    RUN(setup(NULL));
    atomic_store(&g_net.restart_result, XIAOTAI_ERR_INVALID_STATE);
    CHECK(env_set(BASE) == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_ENVIRONMENT_ERROR_HELD, 3000));
    CHECK(status().error == XIAOTAI_ERR_INVALID_STATE && everything_held());
    CHECK(fake_trace_count("restart_request") == 0 && !xiaotai_restart_pending());
    CHECK(env_cancel() == XIAOTAI_OK);
    CHECK(wait_results(2, 2000) && everything_released());
    CHECK(atomic_load(&s_restarted) == 0U);
    teardown();
    return 0;
}

/* -------------------------------------------------------------------- main */

int main(void)
{
    xiaotai_signal_init(&s_sync);
    xiaotai_mutex_init(&s_record_lock);
    static const struct {
        const char *name;
        int (*run)(void);
    } groups[] = {
        {"flow", test_flow},
        {"refuse", test_refuse},
        {"cancel", test_cancel},
        {"timeout", test_timeout},
        {"timer_capacity", test_timer_capacity},
        {"store_timeout", test_store_timeout},
        {"settings_frozen", test_settings_frozen},
        {"settings_held", test_settings_held},
        {"boot_error", test_boot_error},
        {"restart", test_restart},
    };
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
        if (strcmp(groups[i].name, ENV_TEST_GROUP) != 0) continue;
        const int failed = groups[i].run();
        if (failed) {
            dump_trace();
            return 1;
        }
        printf("PASS: mod_environment_%s\n", groups[i].name);
        return 0;
    }
    fprintf(stderr, "unknown group %s\n", ENV_TEST_GROUP);
    return 1;
}
