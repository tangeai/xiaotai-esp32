// SPDX-License-Identifier: MIT
#include "settings_harness.h"
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_kv_host.h"

#define SPACE "xt_settings"

/* ------------------------------------------------------------------ probe */

static probe_event_t s_events[PROBE_EVENTS_MAX];
static size_t s_event_count;

static void on_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_event_count < PROBE_EVENTS_MAX) {
        probe_event_t *slot = &s_events[s_event_count];
        strncpy(slot->name, event->name, sizeof(slot->name) - 1);
        memcpy(slot->text, event->args.text, sizeof(slot->text));
        slot->value = event->args.value;
    }
    ++s_event_count;
}

void probe_clear(void)
{
    memset(s_events, 0, sizeof(s_events));
    s_event_count = 0;
}

size_t probe_total(void) { return s_event_count; }

const probe_event_t *probe_at(size_t index) { return index < s_event_count ? &s_events[index] : NULL; }

size_t probe_count(const char *name, const char *text)
{
    size_t n = 0;
    for (size_t i = 0; i < s_event_count && i < PROBE_EVENTS_MAX; ++i) {
        n += strcmp(s_events[i].name, name) == 0 && (!text || strcmp(s_events[i].text, text) == 0);
    }
    return n;
}

int32_t probe_last_value(const char *name, const char *text)
{
    int32_t value = -999;
    for (size_t i = 0; i < s_event_count && i < PROBE_EVENTS_MAX; ++i) {
        if (strcmp(s_events[i].name, name) == 0 && (!text || strcmp(s_events[i].text, text) == 0)) {
            value = s_events[i].value;
        }
    }
    return value;
}

static xiaotai_err_t probe_init(void)
{
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_STARTED, on_event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_SETTINGS_CHANGED, on_event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_SETTINGS_REFUSED, on_event, NULL);
    return err;
}

/* ---------------------------------------------------------------- storage */

int harness_preset(const char *key, int32_t value)
{
    if (xiaotai_kv_init() != XIAOTAI_OK) return 0;
    return xiaotai_kv_set(SPACE, key, &value, sizeof(value), 2000) == XIAOTAI_OK;
}

int32_t harness_stored(const char *key)
{
    (void)xiaotai_kv_flush(2000);
    int32_t value = 0;
    size_t size = sizeof(value);
    if (xiaotai_kv_get(SPACE, key, &value, &size, 2000) != XIAOTAI_OK || size != sizeof(value)) return -999;
    return value;
}

static uint32_t s_write_base;

static uint32_t backend_writes(void)
{
    (void)xiaotai_kv_flush(2000);
    xiaotai_kv_host_counts_t counts;
    xiaotai_kv_host_counts(&counts);
    return counts.writes;
}

uint32_t harness_writes(void) { return backend_writes() - s_write_base; }

void harness_clear_writes(void) { s_write_base = backend_writes(); }

/* ---------------------------------------------------------------- harness */

static xiaotai_signal_t s_sync;
static bool s_sync_ready;
static uint32_t s_log_start;

static void give(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&s_sync);
}

int harness_settle(void)
{
    /* Each round lets the events queued by the previous one be dispatched. */
    for (int round = 0; round < 4; ++round) {
        if (xiaotai_run(give, NULL) != XIAOTAI_OK) return 0;
        if (!xiaotai_signal_take(&s_sync, 2000)) return 0;
    }
    return 1;
}

void harness_on_owner(void (*fn)(void *ctx), void *ctx)
{
    (void)xiaotai_run(fn, ctx);
    (void)harness_settle();
}

static const char *const PROBE_DEPENDS[] = {"settings", NULL};
static const xiaotai_module_t SETTINGS_MODULE = {
    .name = "settings", .init = xiaotai_module_settings_init,
};
static const xiaotai_module_t PROBE_MODULE = {.name = "probe", .depends = PROBE_DEPENDS, .init = probe_init};
static const xiaotai_module_t *const MODULES[] = {&SETTINGS_MODULE, &PROBE_MODULE};

int harness_start(const xiaotai_settings_config_t *config)
{
    if (!s_sync_ready) {
        if (xiaotai_signal_init(&s_sync) != XIAOTAI_OK) return 1;
        s_sync_ready = true;
    }
    if (xiaotai_eventlog_init(NULL) != XIAOTAI_OK) return 1;
    s_log_start = xiaotai_eventlog_latest();
    if (xiaotai_settings_configure(config) != XIAOTAI_OK) return 1;
    const xiaotai_framework_config_t framework = {.modules = MODULES, .module_count = 2};
    if (xiaotai_framework_start(&framework) != XIAOTAI_OK) return 1;
    return harness_settle() ? 0 : 1;
}

void harness_stop(void)
{
    xiaotai_framework_reset();
    xiaotai_settings_reset();
    (void)xiaotai_kv_flush(2000);
    xiaotai_kv_host_clear();
    s_write_base = 0;
#ifndef SETTINGS_TEST_NO_MEDIA
    fake_media_reset();
#endif
    fake_preferences_set_pending(false);
    probe_clear();
}

void harness_emit(const char *name, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) strncpy(args.text, text, sizeof(args.text) - 1);
    (void)xiaotai_event_emit(name, &args, NULL, 0);
    (void)harness_settle();
}

int32_t harness_eventlog(const char *label)
{
    xiaotai_eventlog_entry_t entries[64];
    const size_t count = xiaotai_eventlog_read(s_log_start, entries, 64);
    int32_t value = -999;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].source, "settings") == 0 && strcmp(entries[i].label, label) == 0) {
            value = entries[i].value;
        }
    }
    return value;
}

/* ------------------------------------------------------ owner-task calls */

typedef struct {
    const char *name;
    int32_t value;
    xiaotai_err_t result;
} call_t;

static void do_set(void *ctx)
{
    call_t *call = ctx;
    call->result = xiaotai_setting_set_int(call->name, call->value);
}

xiaotai_err_t owner_set(const char *name, int32_t value)
{
    call_t call = {.name = name, .value = value, .result = XIAOTAI_FAIL};
    harness_on_owner(do_set, &call);
    return call.result;
}

int32_t setting(const char *name)
{
    int32_t value = -999;
    return xiaotai_setting_get_int(name, &value) == XIAOTAI_OK ? value : -999;
}

static void do_step(void *ctx)
{
    call_t *call = ctx;
    call->result = xiaotai_settings_volume_step((int)call->value);
}

xiaotai_err_t owner_step(int steps)
{
    call_t call = {.value = steps, .result = XIAOTAI_FAIL};
    harness_on_owner(do_step, &call);
    return call.result;
}

static void do_preview(void *ctx)
{
    call_t *call = ctx;
    call->result = xiaotai_settings_volume_preview((uint8_t)call->value);
}

xiaotai_err_t owner_preview(uint8_t percent)
{
    call_t call = {.value = percent, .result = XIAOTAI_FAIL};
    harness_on_owner(do_preview, &call);
    return call.result;
}

static void do_restore(void *ctx)
{
    (void)ctx;
    xiaotai_settings_volume_restore();
}

void owner_restore(void) { harness_on_owner(do_restore, NULL); }

static uint32_t s_ticket;

static void do_quiesce(void *ctx)
{
    call_t *call = ctx;
    call->result = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &s_ticket);
}

xiaotai_err_t owner_quiesce(void)
{
    call_t call = {.result = XIAOTAI_FAIL};
    harness_on_owner(do_quiesce, &call);
    return call.result;
}

static void do_release(void *ctx)
{
    (void)ctx;
    xiaotai_quiesce_end(s_ticket);
}

void owner_release(void) { harness_on_owner(do_release, NULL); }

xiaotai_err_t owner_intent(const char *name, int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    const xiaotai_err_t err = xiaotai_intent_post(name, &args);
    (void)harness_settle();
    return err;
}

typedef struct {
    char *out;
    size_t capacity;
} text_t;

static void append(void *ctx, const char *line)
{
    text_t *text = ctx;
    const size_t used = strlen(text->out);
    snprintf(text->out + used, text->capacity - used, "%s\n", line);
}

static void do_diag(void *ctx)
{
    const xiaotai_reply_t reply = {.write = append, .ctx = ctx};
    xiaotai_diag_report("settings", &reply);
}

void owner_diag(char *out, size_t capacity)
{
    out[0] = '\0';
    text_t text = {.out = out, .capacity = capacity};
    harness_on_owner(do_diag, &text);
}
