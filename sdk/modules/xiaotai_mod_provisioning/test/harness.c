// SPDX-License-Identifier: MIT
/* Harness of the provisioning module's host tests (see harness.h). */
#include "harness.h"
#include <stdlib.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"

char trace[2048];
char reply_text[1024];
atomic_int restarts;
atomic_int_fast64_t probe_restart_us;
atomic_int_fast64_t probe_restarting_us;
atomic_bool probe_restart_off_owner;
atomic_int revocations;
atomic_bool probe_hold_on_done;
xiaotai_provisioning_store_fn_t probe_store_observer;
static atomic_int pressure_state = -1;
static atomic_uint pressure_jobs, pressure_max;
atomic_int probe_guard_calls;
atomic_bool probe_guard_blocked;
atomic_int probe_settings_flushes;

void trace_clear(void) { trace[0] = '\0'; }

int trace_count(const char *word)
{
    int count = 0;
    const size_t length = strlen(word);
    for (const char *at = strstr(trace, word); at; at = strstr(at + length, word)) ++count;
    return count;
}

static void note(const char *text)
{
    strncat(trace, text, sizeof(trace) - strlen(trace) - 1);
    strncat(trace, " ", sizeof(trace) - strlen(trace) - 1);
}

int64_t now_ms(void) { return xiaotai_time_us() / 1000; }

/* ------------------------------------------------------------ probe module */

static void on_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    char text[128];
    if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_CHANGED) == 0) {
        /* The payload is the status the module published at that moment. */
        const xiaotai_provisioning_status_t *status = event->data;
        const bool same = event->size == sizeof(*status) && (int32_t)status->phase == event->args.value;
        snprintf(text, sizeof(text), "changed:%d%s", (int)event->args.value, same ? "" : "!payload");
    } else if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_OPENED) == 0) {
        snprintf(text, sizeof(text), "opened:%d:%s", (int)event->args.value, event->args.text);
    } else if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_DONE) == 0) {
        xiaotai_provisioning_status_t status;
        xiaotai_provisioning_status(&status);
        /* The module still holds the restart when DONE is handled. */
        snprintf(text, sizeof(text), "done:%d:held=%d", (int)event->args.value, status.holding_restart ? 1 : 0);
        if (event->args.value && atomic_load(&probe_hold_on_done)) xiaotai_provisioning_restart_hold();
    } else if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_FAILED) == 0) {
        snprintf(text, sizeof(text), "failed:%s:%d", event->args.text, (int)event->args.value);
    } else if (strcmp(event->name, XIAOTAI_EVENT_QUIESCE) == 0) {
        snprintf(text, sizeof(text), "quiesce:%d", (int)event->args.value);
    } else if (strcmp(event->name, XIAOTAI_EVENT_RESTARTING) == 0) {
        atomic_store(&probe_restarting_us, xiaotai_time_us());
        snprintf(text, sizeof(text), "restarting:%s", event->args.text);
    } else {
        return;
    }
    note(text);
}

static void on_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id;
    (void)ctx;
    note(reason == XIAOTAI_REVOKE_SYSTEM ? "revoked:system" : "revoked:other");
    atomic_fetch_add(&revocations, 1);
}

static xiaotai_err_t probe_guard(const char *reason, void *ctx)
{
    (void)reason;
    (void)ctx;
    atomic_fetch_add(&probe_guard_calls, 1);
    return atomic_load(&probe_guard_blocked) ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
}

static xiaotai_err_t load_int(void *ctx, const char *key, int32_t fallback, int32_t *value)
{
    (void)ctx;
    (void)key;
    (void)fallback;
    (void)value;
    return XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t load_text(void *ctx, const char *key, char *out, size_t capacity)
{
    (void)ctx;
    (void)key;
    (void)out;
    (void)capacity;
    return XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t store_int(void *ctx, const char *key, int32_t value)
{
    (void)ctx;
    (void)key;
    (void)value;
    abort(); /* No test settings are registered. */
}

static xiaotai_err_t store_text(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    (void)key;
    (void)value;
    abort();
}

static xiaotai_err_t store_flush(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    atomic_fetch_add(&probe_settings_flushes, 1);
    return XIAOTAI_OK;
}

static xiaotai_err_t probe_init(void)
{
    const xiaotai_setting_store_t store = {
        .load_int = load_int, .load_text = load_text, .store_int = store_int,
        .store_text = store_text, .flush = store_flush,
    };
    xiaotai_err_t err = xiaotai_setting_store_set(&store);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_quiesce_guard_register("probe", probe_guard, NULL);
    if (err != XIAOTAI_OK) return err;
    if (probe_store_observer) {
        err = xiaotai_provisioning_observe_store(probe_store_observer, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    static const char *const names[] = {
        XIAOTAI_EVENT_PROVISIONING_CHANGED, XIAOTAI_EVENT_PROVISIONING_OPENED, XIAOTAI_EVENT_PROVISIONING_DONE,
        XIAOTAI_EVENT_PROVISIONING_FAILED,  XIAOTAI_EVENT_QUIESCE,             XIAOTAI_EVENT_RESTARTING,
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        err = xiaotai_event_subscribe(names[i], on_event, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

static const char *const s_probe_depends[] = {"provisioning", NULL};
static const xiaotai_module_t s_probe = {.name = "probe", .depends = s_probe_depends, .init = probe_init};
static const xiaotai_module_t s_provisioning = {
    .name = "provisioning", .init = xiaotai_module_provisioning_init, .start = xiaotai_module_provisioning_start,
};

static void pressure_job(void *ctx) { (void)ctx; atomic_fetch_sub(&pressure_jobs, 1U); }

static void fill_before_provisioning(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->args.value != atomic_load(&pressure_state)) return;
    unsigned count = 0;
    while (xiaotai_run(pressure_job, NULL) == XIAOTAI_OK) ++count;
    atomic_store(&pressure_jobs, count);
    atomic_store(&pressure_max, count);
    atomic_store(&pressure_state, -1);
}

static xiaotai_err_t pressure_init(void)
{
    return xiaotai_event_subscribe(XIAOTAI_EVENT_NET_PROVISION, fill_before_provisioning, NULL);
}

static const xiaotai_module_t s_pressure = {.name = "test-pressure", .init = pressure_init};
static const xiaotai_module_t *const s_modules[] = {&s_pressure, &s_provisioning, &s_probe};

static void restart_fn(void)
{
    atomic_store(&probe_restart_us, xiaotai_time_us());
    atomic_store(&probe_restart_off_owner, !xiaotai_on_owner());
    atomic_fetch_add(&restarts, 1);
}

int harness_start(uint32_t restart_delay_ms)
{
    trace_clear();
    atomic_store(&restarts, 0);
    atomic_store(&probe_restart_us, 0);
    atomic_store(&probe_restarting_us, 0);
    atomic_store(&probe_restart_off_owner, false);
    atomic_store(&revocations, 0);
    atomic_store(&probe_guard_calls, 0);
    atomic_store(&probe_guard_blocked, false);
    atomic_store(&probe_settings_flushes, 0);
    fake_at_table_count = 0;
    (void)xiaotai_eventlog_init(NULL);
    const xiaotai_framework_config_t config = {
        .modules = s_modules, .module_count = sizeof(s_modules) / sizeof(*s_modules), .available_needs = 0xFFFFFFFFU,
        .restart_delay_ms = restart_delay_ms, .restart = restart_fn,
    };
    const xiaotai_err_t err = xiaotai_framework_start(&config);
    if (err != XIAOTAI_OK) {
        fprintf(stderr, "framework start failed: %d\n", err);
        return 1;
    }
    drain();
    return 0;
}

void harness_stop(void)
{
    xiaotai_framework_reset();
    xiaotai_provisioning_reset();
}

/* ------------------------------------------------------------- owner calls */

static xiaotai_signal_t s_sync;
static bool s_sync_ready;

static void give(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&s_sync);
}

void drain(void)
{
    if (!s_sync_ready) {
        (void)xiaotai_signal_init(&s_sync);
        s_sync_ready = true;
    }
    /* Several rounds: work queued by a round (an event the module emits, the
     * hold check it queues behind it) runs before the next round ends. */
    for (int i = 0; i < 5; ++i) {
        if (xiaotai_run(give, NULL) != XIAOTAI_OK || !xiaotai_signal_take(&s_sync, 3000)) {
            fprintf(stderr, "owner task did not answer\n");
            abort();
        }
    }
}

void on_owner(void (*fn)(void *ctx), void *ctx)
{
    (void)xiaotai_run(fn, ctx);
    drain();
}

bool on_owner_with_store_pressure(xiaotai_net_provision_state_t state, void (*fn)(void *ctx), void *ctx)
{
    atomic_store(&pressure_max, 0);
    atomic_store(&pressure_state, (int)state);
    if (xiaotai_run(fn, ctx) != XIAOTAI_OK) return false;
    for (unsigned i = 0; i < 1000; ++i) {
        if (atomic_load(&pressure_state) < 0 && atomic_load(&pressure_jobs) == 0) {
            drain();
            return atomic_load(&pressure_max) > 0;
        }
        xiaotai_delay_ms(1);
    }
    return false;
}

typedef struct {
    uint32_t flags;
    xiaotai_err_t result;
    uint32_t id;
    bool flag;
} owner_call_t;

static void do_begin(void *ctx)
{
    owner_call_t *c = ctx;
    c->result = xiaotai_provisioning_begin(c->flags);
}
static void do_cancel(void *ctx) { ((owner_call_t *)ctx)->result = xiaotai_provisioning_cancel(); }
static void do_retry(void *ctx) { ((owner_call_t *)ctx)->result = xiaotai_provisioning_retry(); }
static void do_activity(void *ctx)
{
    static const xiaotai_activity_t call = {
        .name = "call", .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT, .revoked = on_revoked,
    };
    owner_call_t *c = ctx;
    if (xiaotai_activity_begin(&call, &c->id) != XIAOTAI_OK) c->id = 0;
}
static void do_quiesced(void *ctx) { ((owner_call_t *)ctx)->flag = xiaotai_quiesced(); }

xiaotai_err_t owner_begin(uint32_t flags)
{
    owner_call_t c = {.flags = flags};
    on_owner(do_begin, &c);
    return c.result;
}

xiaotai_err_t owner_cancel(void)
{
    owner_call_t c = {0};
    on_owner(do_cancel, &c);
    return c.result;
}

xiaotai_err_t owner_retry(void)
{
    owner_call_t c = {0};
    on_owner(do_retry, &c);
    return c.result;
}

uint32_t owner_activity_begin(void)
{
    owner_call_t c = {0};
    on_owner(do_activity, &c);
    return c.id;
}

bool owner_quiesced(void)
{
    owner_call_t c = {0};
    on_owner(do_quiesced, &c);
    return c.flag;
}

xiaotai_provisioning_phase_t phase(void)
{
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    return status.phase;
}

bool wait_phase(xiaotai_provisioning_phase_t expected, uint32_t timeout_ms)
{
    const int64_t end = now_ms() + timeout_ms;
    while (phase() != expected) {
        if (now_ms() > end) return false;
        xiaotai_delay_ms(5);
    }
    return true;
}

static void read_reprovisions(void *ctx) { *(unsigned *)ctx = fake_net.reprovisions; }

bool wait_reprovisions(unsigned count, uint32_t timeout_ms)
{
    const int64_t end = now_ms() + timeout_ms;
    for (;;) {
        unsigned seen = 0;
        on_owner(read_reprovisions, &seen);
        if (seen >= count) return true;
        if (now_ms() > end) return false;
        xiaotai_delay_ms(5);
    }
}

/* ----------------------------------------------------------------- console */

static void collect(void *ctx, const char *text)
{
    (void)ctx;
    strncat(reply_text, text, sizeof(reply_text) - strlen(reply_text) - 2);
    strcat(reply_text, "\n");
}

xiaotai_err_t command(const char *line)
{
    reply_text[0] = '\0';
    const xiaotai_reply_t reply = {.write = collect};
    const xiaotai_err_t err = xiaotai_command_run(line, &reply, 3000);
    drain();
    return err;
}

static void read_diag(void *ctx)
{
    reply_text[0] = '\0';
    const xiaotai_reply_t reply = {.write = collect};
    xiaotai_diag_report((const char *)ctx, &reply);
}

void diag_section(const char *name) { on_owner(read_diag, (void *)name); }

int framework_holds(void)
{
    diag_section("framework");
    const char *at = strstr(reply_text, "holds=");
    return at ? atoi(at + 6) : -1;
}
