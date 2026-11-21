// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_binding_prompt_cancel.py
// Replaces legacy: tests/regression/test_binding_grant_durability.py
// Replaces legacy: tests/regression/test_c3_binding_grant.py
// Replaces legacy: tests/regression/test_c3_mqtt_token_admission.py
// Replaces legacy: tests/regression/test_binding_lifecycle.py
/* Host tests of the binding flow (work package MB-1): the module's sources
 * with the real framework, event log, JSON library and host port; the
 * platform core, the identity component, the network engine and the wall
 * clock are fakes (fake_platform.c). The window group drives a manual
 * monotonic clock through the real framework timer queue. One executable per group:
 * BINDING_GROUP names the group (CMakeLists.txt).
 *
 * Groups and what they hold the module to (specification section 5):
 *   first        a first binding from the report to BOUND, its order, the
 *                field limits of the report reply (a 16-character code),
 *                40901 and other refusals, no automatic retry (BD1-BD4,
 *                BD6, BD9, BD22)
 *   prepare      preparation steps: none, two in order, synchronous done,
 *                failures, the deadline, a late done after cancel, the
 *                automatic first binding waiting for its step, retry's
 *                BOUND branch running none (BD23)
 *   resign       signed re-binding: empty and absent payload, the same
 *                identity, another identity (BD7, BD14)
 *   grant        every grant shape under the strict and the tolerant rules
 *                (BD5), storing before acknowledging (BD6)
 *   window       token expiry against wait_s, a token without expiry, the
 *                old-code re-report, EXPIRED versus FAILED, the connection
 *                deadline (BD11-BD13)
 *   cancel       network down, the user leaving the network, quiesce,
 *                explicit cancel, STORING running to its end (BD10, BD19)
 *   ack_lost     a lost acknowledgement and the signed retry after reload,
 *                the window ending during the store, store and reload
 *                failures, an unreadable identity (BD8, BD9, BD15)
 *   quiesce      the framework quiesce guard "binding"
 *   auto_start   the automatic start, guards waited out, reports replacing
 *                a wait, manual_start */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "binding_internal.h"
#include "fake_platform.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/net.h"
#include "xiaotai/port/os.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)
#define RUN(test)                                         \
    do {                                                  \
        if (test() != 0) {                                \
            fprintf(stderr, "FAIL: %s\n", #test);         \
            xiaotai_framework_reset();                    \
            xiaotai_binding_reset();                      \
            return 1;                                     \
        }                                                 \
    } while (0)

#define PAIR(id, key) "{\"device_id\":\"" id "\",\"device_key\":\"" key "\"}"
#define GRANT(payload) "{\"type\":\"auth_grant\",\"payload\":" payload "}"

/* ------------------------------------------------------------ event record */

typedef struct {
    char name[24];
    int32_t value;
    char text[XIAOTAI_ARG_TEXT_MAX];
    int64_t at_us;
    xiaotai_binding_phase_t phase;    /* binding.changed: the snapshot's phase */
} record_t;

static record_t s_log[512];
static int s_log_count;

static void on_binding_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_log_count >= (int)(sizeof(s_log) / sizeof(s_log[0]))) return;
    record_t *record = &s_log[s_log_count++];
    snprintf(record->name, sizeof(record->name), "%s", event->name);
    record->value = event->args.value;
    memcpy(record->text, event->args.text, sizeof(record->text));
    record->at_us = xiaotai_time_us();
    if (event->data != NULL && event->size == sizeof(xiaotai_binding_status_t)) {
        record->phase = ((const xiaotai_binding_status_t *)event->data)->phase;
    }
}

static int events(const char *name)
{
    int count = 0;
    for (int i = 0; i < s_log_count; ++i) count += strcmp(s_log[i].name, name) == 0;
    return count;
}

static const record_t *last(const char *name)
{
    for (int i = s_log_count - 1; i >= 0; --i) {
        if (strcmp(s_log[i].name, name) == 0) return &s_log[i];
    }
    static const record_t none = {.value = -12345};
    return &none;
}

static int first_index(const char *name)
{
    for (int i = 0; i < s_log_count; ++i) {
        if (strcmp(s_log[i].name, name) == 0) return i;
    }
    return -1;
}

/* ------------------------------------------------- guards and steps (probe) */

typedef enum { STEP_ASYNC = 0, STEP_SYNC_DONE, STEP_REFUSE } step_mode_t;
typedef struct {
    const char *name;
    step_mode_t mode;
    xiaotai_err_t result;               /* STEP_SYNC_DONE: done's result; STEP_REFUSE: returned */
    unsigned calls;
    xiaotai_binding_prepared_fn_t done;
    void *done_ctx;
} step_t;

static step_t s_steps[2];
static size_t s_step_count;
static char s_step_order[64];
static bool s_guard_on;
static xiaotai_err_t s_guard_result;
static unsigned s_guard_calls;

static xiaotai_err_t step_prepare(xiaotai_binding_prepared_fn_t done, void *done_ctx, void *ctx)
{
    step_t *step = ctx;
    ++step->calls;
    strncat(s_step_order, step->name, sizeof(s_step_order) - strlen(s_step_order) - 1U);
    step->done = done;
    step->done_ctx = done_ctx;
    if (step->mode == STEP_REFUSE) return step->result;
    if (step->mode == STEP_SYNC_DONE) done(step->result, done_ctx);
    return XIAOTAI_OK;
}

static xiaotai_err_t test_guard(void *ctx)
{
    (void)ctx;
    ++s_guard_calls;
    return s_guard_result;
}

static xiaotai_err_t probe_init(void)
{
    static const char *const NAMES[] = {
        XIAOTAI_EVENT_BINDING_CHANGED, XIAOTAI_EVENT_BINDING_NEEDED, XIAOTAI_EVENT_BINDING_CODE,
        XIAOTAI_EVENT_BINDING_BOUND,   XIAOTAI_EVENT_BINDING_FAILED,
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); ++i) {
        xiaotai_err_t result = xiaotai_event_subscribe(NAMES[i], on_binding_event, NULL);
        if (result != XIAOTAI_OK) return result;
    }
    for (size_t i = 0; i < s_step_count; ++i) {
        xiaotai_err_t result = xiaotai_binding_prepare_register(s_steps[i].name, step_prepare, &s_steps[i]);
        if (result != XIAOTAI_OK) return result;
    }
    if (s_guard_on) return xiaotai_binding_guard_register("test", test_guard, NULL);
    return XIAOTAI_OK;
}

static const xiaotai_module_t BINDING_MODULE = {
    .name = "binding", .init = xiaotai_module_binding_init, .start = xiaotai_module_binding_start};
static const char *const PROBE_DEPENDS[] = {"binding", NULL};
static const xiaotai_module_t PROBE_MODULE = {.name = "probe", .depends = PROBE_DEPENDS, .init = probe_init};

/* ------------------------------------------------------------------ system */

/* Start the framework with the module and the probe. id NULL: no identity. */
static int start_system(const xiaotai_binding_config_t *config, const char *id, const char *key)
{
    xiaotai_binding_reset();
    fake_reset();
    fake_identity_set(id, key);
    s_log_count = 0;
    s_step_order[0] = '\0';
    CHECK(xiaotai_binding_configure(config) == XIAOTAI_OK);
    static const xiaotai_module_t *const MODULES[] = {&BINDING_MODULE, &PROBE_MODULE};
    const xiaotai_framework_config_t framework = {.modules = MODULES, .module_count = 2,
                                                  .available_needs = UINT32_MAX};
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    CHECK(fake_drain());
    return 0;
}

static void stop_system(void)
{
    xiaotai_framework_reset();
    xiaotai_binding_reset();
    memset(s_steps, 0, sizeof(s_steps));
    s_step_count = 0;
    s_guard_on = false;
    s_guard_result = XIAOTAI_OK;
    s_guard_calls = 0;
}

static xiaotai_binding_status_t status(void)
{
    xiaotai_binding_status_t snapshot;
    xiaotai_binding_status(&snapshot);
    return snapshot;
}

static xiaotai_binding_stats_t stats(void)
{
    xiaotai_binding_stats_t counters;
    xiaotai_binding_stats(&counters);
    return counters;
}

static xiaotai_err_t s_result;
static const char *s_reason;
static uint32_t s_ticket;
static size_t s_step_index;

static void do_start(void *ctx) { (void)ctx; s_result = xiaotai_binding_start(); }
static void do_retry(void *ctx) { (void)ctx; s_result = xiaotai_binding_retry(); }
static void do_cancel(void *ctx) { (void)ctx; s_result = xiaotai_binding_cancel(); }
static void do_guards_changed(void *ctx) { (void)ctx; xiaotai_binding_guards_changed(); }
static void do_quiesce(void *ctx) { (void)ctx; s_result = xiaotai_quiesce_begin(s_reason, &s_ticket); }
static void do_quiesce_end(void *ctx) { (void)ctx; xiaotai_quiesce_end(s_ticket); }
static void do_step_done(void *ctx)
{
    step_t *step = &s_steps[s_step_index];
    step->done((xiaotai_err_t)(intptr_t)ctx, step->done_ctx);
}

static xiaotai_err_t owner_start(void) { fake_on_owner(do_start, NULL); return s_result; }
static xiaotai_err_t owner_retry(void) { fake_on_owner(do_retry, NULL); return s_result; }
static xiaotai_err_t owner_cancel(void) { fake_on_owner(do_cancel, NULL); return s_result; }
static void owner_guards_changed(void) { fake_on_owner(do_guards_changed, NULL); }
static xiaotai_err_t owner_quiesce(const char *reason)
{
    s_reason = reason;
    s_ticket = 0;
    fake_on_owner(do_quiesce, NULL);
    return s_result;
}
static void owner_quiesce_end(void) { fake_on_owner(do_quiesce_end, NULL); }
static void step_done(size_t index, xiaotai_err_t result)
{
    s_step_index = index;
    fake_on_owner(do_step_done, (void *)(intptr_t)result);
}

static void emit(const char *name, int32_t value, const void *data, size_t size)
{
    xiaotai_args_t args = {.value = value};
    if (xiaotai_event_emit(name, &args, data, size) != XIAOTAI_OK) abort();
    fake_drain_events();
}

static void needs(int32_t rebinding) { emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, rebinding, NULL, 0); }

/* Poll until the phase is reached (owner drained between polls). */
static bool wait_phase(xiaotai_binding_phase_t phase, uint32_t timeout_ms)
{
    int64_t until = xiaotai_time_us() + (int64_t)timeout_ms * 1000;
    while (xiaotai_time_us() < until) {
        (void)fake_drain();
        if (status().phase == phase) return true;
        xiaotai_delay_ms(5);
    }
    return status().phase == phase;
}

static bool wait_requests(unsigned count, uint32_t timeout_ms)
{
    int64_t until = xiaotai_time_us() + (int64_t)timeout_ms * 1000;
    while (xiaotai_time_us() < until) {
        (void)fake_drain();
        if (g_request.count >= count) return true;
        xiaotai_delay_ms(5);
    }
    return g_request.count >= count;
}

static const char *token_expiring_in(int64_t seconds)
{
    static char token[40];
    snprintf(token, sizeof(token), "exp.%lld", (long long)(g_unix_ms / 1000 + seconds));
    return token;
}

/* From REPORTING: a usable reply, then the guest subscription. */
static int reach_waiting(const char *code, int64_t token_seconds)
{
    CHECK(status().phase == XIAOTAI_BINDING_REPORTING);
    fake_reply(200, 200, fake_report_body(code, token_expiring_in(token_seconds), "tmp_fixture"));
    CHECK(status().phase == XIAOTAI_BINDING_CONNECTING);
    fake_guest_ready();
    CHECK(status().phase == XIAOTAI_BINDING_WAITING);
    return 0;
}

static char *repeat(char c, size_t count)
{
    char *text = malloc(count + 1U);
    memset(text, c, count);
    text[count] = '\0';
    return text;
}

/* =================================================================== first */

static int test_first_full(void)
{
    CHECK(start_system(NULL, NULL, NULL) == 0);
    xiaotai_binding_status_t now = status();
    CHECK(now.phase == XIAOTAI_BINDING_UNKNOWN && now.device_id[0] == '\0' && now.client_id[0] == '\0');
    CHECK(g_requirement.request_body_bytes == 64U && g_requirement.response_bytes == 2048U &&
          g_requirement.signal_bytes == 1024U && g_requirement.concurrent_requests == 1U);
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_BINDING_START) && xiaotai_intent_exists(XIAOTAI_INTENT_BINDING_RETRY) &&
          xiaotai_intent_exists(XIAOTAI_INTENT_BINDING_CANCEL));

    needs(0);
    CHECK(events(XIAOTAI_EVENT_BINDING_NEEDED) == 1 && last(XIAOTAI_EVENT_BINDING_NEEDED)->value == 0);
    CHECK(g_request.count == 1U);
    CHECK(strcmp(g_request.service, "device") == 0 && g_request.method == XIAOTAI_PLATFORM_POST);
    CHECK(strcmp(g_request.path, "/v1/device/report") == 0);
    CHECK(strcmp(g_request.body, "{\"mac\":\"AA:BB:CC:DD:EE:01\"}") == 0);
    CHECK(g_request.timeout_ms == 15000U && g_request.auth == XIAOTAI_PLATFORM_AUTH_NONE);
    now = status();
    CHECK(now.phase == XIAOTAI_BINDING_REPORTING && !now.signed_rebinding && now.verification[0] == '\0');
    /* The attempt context lives in BULK memory while the attempt runs. */
    CHECK(g_mem.allocations == 1U && g_mem.kind == XIAOTAI_MEM_BULK && g_mem.live != NULL);
    xiaotai_binding_stats_t counters = stats();
    CHECK(counters.needed_first == 1U && counters.attempts == 1U && counters.attempts_signed == 0U);

    const char *token = token_expiring_in(100);
    char token_copy[40];
    snprintf(token_copy, sizeof(token_copy), "%s", token);
    fake_reply(200, 200, fake_report_body("123456", token_copy, "tmp_fixture"));
    CHECK(g_guest.opens == 1U && strcmp(g_guest.client_id, "tmp_fixture") == 0 &&
          strcmp(g_guest.token, token_copy) == 0);
    /* The code stays hidden until the guest session is subscribed. */
    now = status();
    CHECK(now.phase == XIAOTAI_BINDING_CONNECTING && now.verification[0] == '\0' && now.seconds_left == 0U);
    CHECK(events(XIAOTAI_EVENT_BINDING_CODE) == 0);

    fake_guest_ready();
    now = status();
    CHECK(now.phase == XIAOTAI_BINDING_WAITING && strcmp(now.verification, "123456") == 0);
    CHECK(xiaotai_binding_announce() == XIAOTAI_ERR_NOT_SUPPORTED); /* link without media */
    CHECK(now.seconds_left == 100U);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_CODE)->text, "123456") == 0 &&
          last(XIAOTAI_EVENT_BINDING_CODE)->value == 100);

    fake_guest_message("auth_grant", GRANT(PAIR("dev-1", "key-1")));
    CHECK(status().phase == XIAOTAI_BINDING_STORING);
    CHECK(g_identity.saves == 1U && strcmp(g_identity.saving.device_id, "dev-1") == 0 &&
          strcmp(g_identity.saving.device_key, "key-1") == 0);
    CHECK(g_guest.acks == 0U);   /* nothing acknowledged before it is stored */

    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_CONFIRMING);
    CHECK(g_guest.acks == 1U && g_guest.ack_with_identity);
    /* Default confirm_ms 0: until the window ends. */
    CHECK(g_guest.ack_timeout_ms <= 100000U && g_guest.ack_timeout_ms > 95000U);

    fake_guest_ack_result(XIAOTAI_OK);
    now = status();
    CHECK(now.phase == XIAOTAI_BINDING_BOUND && xiaotai_binding_bound());
    CHECK(strcmp(now.device_id, "dev-1") == 0 && strcmp(now.client_id, "test-aabbccddee01") == 0);
    CHECK(now.verification[0] == '\0' && now.error == XIAOTAI_OK);
    CHECK(last(XIAOTAI_EVENT_BINDING_BOUND)->value == 1);
    /* Stored, acknowledged, the guest session closed, then the platform continues. */
    CHECK(strcmp(g_trace, "save ack close retry ") == 0);
    CHECK(g_platform_retries == 1U && !g_guest.open);
    /* The context with the temporary token was wiped before it was freed. */
    CHECK(g_mem.frees == 1U && g_mem.freed_wiped && g_mem.live == NULL);
    counters = stats();
    CHECK(counters.grants == 1U && counters.stores == 1U && counters.confirmations == 1U &&
          counters.bound == 1U && counters.failed == 0U);
    CHECK(first_index(XIAOTAI_EVENT_BINDING_NEEDED) < first_index(XIAOTAI_EVENT_BINDING_CODE) &&
          first_index(XIAOTAI_EVENT_BINDING_CODE) < first_index(XIAOTAI_EVENT_BINDING_BOUND));
    /* The phases in order, each announced. */
    const xiaotai_binding_phase_t expected[] = {
        XIAOTAI_BINDING_NEEDED,  XIAOTAI_BINDING_PREPARING,
        XIAOTAI_BINDING_REPORTING,  XIAOTAI_BINDING_CONNECTING, XIAOTAI_BINDING_WAITING,
        XIAOTAI_BINDING_STORING,    XIAOTAI_BINDING_CONFIRMING, XIAOTAI_BINDING_BOUND,
    };
    size_t seen = 0;
    for (int i = 0; i < s_log_count; ++i) {
        if (strcmp(s_log[i].name, XIAOTAI_EVENT_BINDING_CHANGED) != 0) continue;
        if (seen > 0U && s_log[i].phase == expected[seen - 1U]) continue;
        CHECK(seen < sizeof(expected) / sizeof(expected[0]) && s_log[i].phase == expected[seen]);
        ++seen;
    }
    CHECK(seen == sizeof(expected) / sizeof(expected[0]));
    stop_system();
    return 0;
}

/* One report reply in a fresh attempt; the phase it leads to. */
static xiaotai_binding_phase_t report_reply(int http, int32_t code, const char *body)
{
    if (status().phase != XIAOTAI_BINDING_REPORTING && owner_start() != XIAOTAI_OK) return XIAOTAI_BINDING_UNKNOWN;
    fake_reply(http, code, body);
    xiaotai_binding_phase_t phase = status().phase;
    if (phase == XIAOTAI_BINDING_CONNECTING) (void)owner_cancel();
    return phase;
}

static int test_first_limits(void)
{
    CHECK(start_system(NULL, NULL, NULL) == 0);
    needs(0);
    char *code16 = repeat('7', 16), *code17 = repeat('7', 17);
    char *token1023 = repeat('t', 1023), *token1024 = repeat('t', 1024);
    char *client64 = repeat('c', 64), *client65 = repeat('c', 65);

    /* The largest values every deployed firmware accepts. */
    CHECK(report_reply(200, 200, fake_report_body(code16, token1023, client64)) == XIAOTAI_BINDING_CONNECTING);
    CHECK(strcmp(g_guest.client_id, client64) == 0 && strcmp(g_guest.token, token1023) == 0);
    CHECK(status().phase == XIAOTAI_BINDING_NEEDED);
    unsigned refused = 0;
    const struct {
        const char *code, *token, *client, *raw;
    } bad[] = {
        {code17, "tok", "cli", NULL},  {"123456", token1024, "cli", NULL}, {"123456", "tok", client65, NULL},
        {"", "tok", "cli", NULL},      {"123456", NULL, "cli", NULL},      {NULL, NULL, NULL, "{\"code\":200,\"data\":[]}"},
        {NULL, NULL, NULL, "{\"code\":200,\"data\":{\"code\":123456,\"temp_token\":\"t\",\"temp_client_id\":\"c\"}}"},
        {NULL, NULL, NULL, "not json"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        const char *body = bad[i].raw != NULL ? bad[i].raw : fake_report_body(bad[i].code, bad[i].token, bad[i].client);
        CHECK(report_reply(200, 200, body) == XIAOTAI_BINDING_FAILED);
        ++refused;
        CHECK(status().error == XIAOTAI_FAIL && strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "report") == 0);
        CHECK(g_guest.opens == 1U);
    }
    CHECK(stats().reports_refused == refused);
    free(code16); free(code17); free(token1023); free(token1024); free(client64); free(client65);
    stop_system();
    return 0;
}

static int test_first_refusals(void)
{
    CHECK(start_system(NULL, NULL, NULL) == 0);
    needs(0);
    /* 40901: the previous code is still valid; kept in the status. */
    fake_reply(200, 40901, "{\"code\":40901,\"msg\":\"code still valid\"}");
    xiaotai_binding_status_t now = status();
    CHECK(now.phase == XIAOTAI_BINDING_FAILED && now.error == XIAOTAI_FAIL && now.code == 40901);
    CHECK(last(XIAOTAI_EVENT_BINDING_FAILED)->value == XIAOTAI_FAIL &&
          strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "report") == 0);
    CHECK(stats().reports_refused == 1U && g_guest.opens == 0U);
    CHECK(g_mem.frees == 1U && g_mem.freed_wiped);
    /* No automatic retry: the module waits for the user. */
    xiaotai_delay_ms(300);
    (void)fake_drain();
    CHECK(g_request.count == 1U && status().phase == XIAOTAI_BINDING_FAILED);

    CHECK(report_reply(500, XIAOTAI_PLATFORM_NO_CODE, "oops") == XIAOTAI_BINDING_FAILED);
    CHECK(status().error == XIAOTAI_FAIL && status().code == XIAOTAI_PLATFORM_NO_CODE);
    CHECK(owner_start() == XIAOTAI_OK);
    fake_reply_error(XIAOTAI_ERR_TIMEOUT);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_TIMEOUT);
    CHECK(stats().reports_refused == 2U);
    /* A request the platform core does not accept ends the attempt at once. */
    g_request.refuse = XIAOTAI_ERR_PLATFORM_GATED;
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_PLATFORM_GATED);
    g_request.refuse = XIAOTAI_OK;
    /* Starting needs the network, discovery and the clock. */
    g_platform.clock_synced = false;
    CHECK(owner_start() == XIAOTAI_ERR_PLATFORM_OFFLINE);
    g_platform.clock_synced = true;
    g_net.mac_valid = false;
    CHECK(owner_start() == XIAOTAI_ERR_PLATFORM_OFFLINE);
    g_net.mac_valid = true;
    g_provisioning_boot = true;
    CHECK(owner_start() == XIAOTAI_ERR_INVALID_STATE);
    g_provisioning_boot = false;
    CHECK(stats().attempts == 4U);
    stop_system();
    return 0;
}

/* ================================================================= prepare */

static int test_prepare_none(void)
{
    CHECK(start_system(NULL, NULL, NULL) == 0);
    needs(0);
    CHECK(g_request.count == 1U && status().phase == XIAOTAI_BINDING_REPORTING);
    stop_system();
    return 0;
}

static int test_prepare_two_in_order(void)
{
    s_steps[0] = (step_t){.name = "A", .mode = STEP_ASYNC};
    s_steps[1] = (step_t){.name = "B", .mode = STEP_ASYNC};
    s_step_count = 2;
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(status().phase == XIAOTAI_BINDING_NEEDED && s_steps[0].calls == 0U);
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_PREPARING && s_steps[0].calls == 1U && s_steps[1].calls == 0U);
    CHECK(g_request.count == 0U);
    step_done(0, XIAOTAI_OK);
    CHECK(s_steps[1].calls == 1U && g_request.count == 0U && status().phase == XIAOTAI_BINDING_PREPARING);
    step_done(1, XIAOTAI_OK);
    CHECK(g_request.count == 1U && status().phase == XIAOTAI_BINDING_REPORTING);
    CHECK(strcmp(s_step_order, "AB") == 0);
    /* A second done of the same step is void. */
    step_done(1, XIAOTAI_OK);
    CHECK(stats().prepare_late == 1U && status().phase == XIAOTAI_BINDING_REPORTING);
    stop_system();
    return 0;
}

static int test_prepare_sync_and_failures(void)
{
    s_steps[0] = (step_t){.name = "A", .mode = STEP_SYNC_DONE, .result = XIAOTAI_OK};
    s_steps[1] = (step_t){.name = "B", .mode = STEP_ASYNC};
    s_step_count = 2;
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    /* A step that completes before returning lets the next one run at once. */
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(s_steps[0].calls == 1U && s_steps[1].calls == 1U && g_request.count == 0U);
    /* An asynchronous failure ends the attempt with the step's error. */
    step_done(1, XIAOTAI_ERR_NO_MEM);
    xiaotai_binding_status_t now = status();
    CHECK(now.phase == XIAOTAI_BINDING_FAILED && now.error == XIAOTAI_ERR_NO_MEM);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "prepare") == 0 && g_request.count == 0U);
    CHECK(stats().prepare_failures == 1U);
    /* A synchronous failure in done. */
    s_steps[0].result = XIAOTAI_ERR_TIMEOUT;
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_TIMEOUT);
    CHECK(s_steps[1].calls == 1U);
    /* A refusal: done is never awaited. */
    s_steps[0].mode = STEP_REFUSE;
    s_steps[0].result = XIAOTAI_ERR_INVALID_STATE;
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(stats().prepare_failures == 3U && g_request.count == 0U);
    stop_system();
    return 0;
}

static int test_prepare_timeout_and_late(void)
{
    s_steps[0] = (step_t){.name = "A", .mode = STEP_ASYNC};
    s_step_count = 1;
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    binding_test_set_prepare_ms(300);
    needs(0);
    int64_t started = xiaotai_time_us();
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(wait_phase(XIAOTAI_BINDING_FAILED, 2000));
    /* The phase is published before the separate failure event is delivered. */
    fake_drain_events();
    int64_t ended = last(XIAOTAI_EVENT_BINDING_FAILED)->at_us;
    CHECK(ended - started >= 300000 && ended - started < 900000);
    CHECK(status().error == XIAOTAI_ERR_TIMEOUT && stats().prepare_timeouts == 1U);
    /* The step finishes after all: counted and ignored. */
    step_done(0, XIAOTAI_OK);
    CHECK(stats().prepare_late == 1U && status().phase == XIAOTAI_BINDING_FAILED && g_request.count == 0U);

    /* Cancelled while preparing: back to NEEDED, the late done is void, and a
     * new attempt runs the step again. */
    CHECK(owner_start() == XIAOTAI_OK && s_steps[0].calls == 2U);
    void *first_ctx = s_steps[0].done_ctx;
    CHECK(owner_cancel() == XIAOTAI_OK && status().phase == XIAOTAI_BINDING_NEEDED);
    CHECK(owner_start() == XIAOTAI_OK && s_steps[0].calls == 3U);
    void *second_ctx = s_steps[0].done_ctx;
    s_steps[0].done_ctx = first_ctx;
    step_done(0, XIAOTAI_OK);
    CHECK(stats().prepare_late == 2U && status().phase == XIAOTAI_BINDING_PREPARING && g_request.count == 0U);
    s_steps[0].done_ctx = second_ctx;
    step_done(0, XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_REPORTING && g_request.count == 1U);
    stop_system();
    return 0;
}

static int test_prepare_auto_and_bound_retry(void)
{
    s_steps[0] = (step_t){.name = "A", .mode = STEP_ASYNC};
    s_step_count = 1;
    CHECK(start_system(NULL, NULL, NULL) == 0);
    /* First boot: the automatic start runs the step and reports once it is done. */
    needs(0);
    CHECK(s_steps[0].calls == 1U && g_request.count == 0U);
    step_done(0, XIAOTAI_OK);
    CHECK(g_request.count == 1U && status().phase == XIAOTAI_BINDING_REPORTING);
    stop_system();

    /* An ordinary login (retry in BOUND) runs no step. */
    s_steps[0] = (step_t){.name = "A", .mode = STEP_ASYNC};
    s_step_count = 1;
    CHECK(start_system(NULL, "dev-1", "key-1") == 0);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND);
    CHECK(owner_retry() == XIAOTAI_OK);
    CHECK(g_platform_retries == 1U && s_steps[0].calls == 0U && g_request.count == 0U);
    /* A signed re-binding runs it. */
    needs(1);
    CHECK(s_steps[0].calls == 1U && status().signed_rebinding);
    stop_system();
    return 0;
}

/* ================================================================== resign */

static int signed_attempt(const char *grant)
{
    if (status().phase != XIAOTAI_BINDING_REPORTING) CHECK(owner_start() == XIAOTAI_OK);
    CHECK(g_request.auth == XIAOTAI_PLATFORM_AUTH_SIGNED && status().signed_rebinding);
    CHECK(reach_waiting("123456", 100) == 0);
    fake_guest_message("auth_grant", grant);
    return 0;
}

static int test_resign(void)
{
    CHECK(start_system(NULL, "old-id", "old-key") == 0);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND && strcmp(status().device_id, "old-id") == 0);
    needs(1);
    CHECK(last(XIAOTAI_EVENT_BINDING_NEEDED)->value == 1 && stats().needed_rebinding == 1U);
    /* Empty payload: the stored identity is confirmed, nothing is written. */
    CHECK(signed_attempt(GRANT("\"\"")) == 0);
    CHECK(status().phase == XIAOTAI_BINDING_CONFIRMING && g_identity.saves == 0U);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND && last(XIAOTAI_EVENT_BINDING_BOUND)->value == 0);

    /* null and an absent payload mean the same. */
    needs(1);
    CHECK(signed_attempt(GRANT("null")) == 0);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND);
    needs(1);
    CHECK(signed_attempt("{\"type\":\"auth_grant\"}") == 0);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND);

    /* The same identity: compared, not rewritten. */
    needs(1);
    CHECK(signed_attempt(GRANT(PAIR("old-id", "old-key"))) == 0);
    CHECK(status().phase == XIAOTAI_BINDING_CONFIRMING && g_identity.saves == 0U);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(last(XIAOTAI_EVENT_BINDING_BOUND)->value == 0);

    /* Another identity: failed, never acknowledged, the stored one stays. */
    needs(1);
    unsigned acks = g_guest.acks;
    CHECK(signed_attempt(GRANT(PAIR("other-id", "other-key"))) == 0);
    xiaotai_binding_status_t now = status();
    CHECK(now.phase == XIAOTAI_BINDING_FAILED && now.error == XIAOTAI_ERR_INVALID_ARG);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "grant") == 0);
    CHECK(g_guest.acks == acks && g_identity.saves == 0U && strcmp(g_identity.identity.device_id, "old-id") == 0);
    CHECK(stats().grants_mismatched == 1U && stats().attempts_signed == 5U);
    stop_system();
    return 0;
}

/* =================================================================== grant */

/* A fresh attempt in WAITING, then one message. */
static int grant_case(const char *grant)
{
    if (status().phase != XIAOTAI_BINDING_WAITING) {
        if (status().phase != XIAOTAI_BINDING_REPORTING) CHECK(owner_start() == XIAOTAI_OK);
        CHECK(reach_waiting("123456", 100) == 0);
    }
    fake_guest_message("auth_grant", grant);
    return 0;
}

static int test_grant_strict(void)
{
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(reach_waiting("123456", 100) == 0);
    /* Another type is not a grant. */
    fake_guest_message("call_incoming", "{\"type\":\"call_incoming\"}");
    CHECK(status().phase == XIAOTAI_BINDING_WAITING && stats().grants_ignored == 1U);

    char *id65 = repeat('i', 65), *key257 = repeat('k', 257);
    char *long_id = malloc(400), *long_key = malloc(400);
    snprintf(long_id, 400, GRANT("{\"device_id\":\"%s\",\"device_key\":\"k\"}"), id65);
    snprintf(long_key, 400, GRANT("{\"device_id\":\"i\",\"device_key\":\"%s\"}"), key257);
    const char *malformed[] = {
        GRANT("{\"device_id\":\"a\"}"), GRANT("{\"device_key\":\"b\"}"), GRANT(PAIR("", "b")),
        GRANT("{\"device_id\":1,\"device_key\":\"b\"}"), GRANT("5"), GRANT("[]"), GRANT("\"x\""),
        long_id, long_key,
        /* Keeping needs a stored identity. */
        GRANT("null"), GRANT("\"\""), GRANT("{}"), GRANT("{\"x\":1}"), "{\"type\":\"auth_grant\"}",
    };
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        CHECK(grant_case(malformed[i]) == 0);
        CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_FAIL);
        CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "grant") == 0);
        ++failures;
    }
    CHECK(stats().grants_malformed == failures && g_identity.saves == 0U && g_guest.acks == 0U);

    /* The longest credentials fit; a later grant cannot replace the staged one. */
    char *id64 = repeat('i', 64), *key256 = repeat('k', 256);
    char *longest = malloc(600);
    snprintf(longest, 600, GRANT("{\"device_id\":\"%s\",\"device_key\":\"%s\"}"), id64, key256);
    CHECK(grant_case(longest) == 0);
    CHECK(status().phase == XIAOTAI_BINDING_STORING && g_identity.saves == 1U);
    fake_guest_message("auth_grant", GRANT(PAIR("other-id", "other-key")));
    CHECK(stats().grants_ignored == 2U && strcmp(g_identity.saving.device_id, id64) == 0 &&
          strcmp(g_identity.saving.device_key, key256) == 0);
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(g_guest.acks == 1U && g_guest.ack_with_identity);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND);
    free(id65); free(key257); free(long_id); free(long_key); free(id64); free(key256); free(longest);
    stop_system();

    /* With an identity stored, the strict rules keep it for "", {} and an
     * object without either credential member. */
    CHECK(start_system(&config, "old-id", "old-key") == 0);
    needs(1);
    const char *keep[] = {GRANT("\"\""), GRANT("{}"), GRANT("{\"x\":1}")};
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); ++i) {
        CHECK(grant_case(keep[i]) == 0);
        CHECK(status().phase == XIAOTAI_BINDING_CONFIRMING);
        fake_guest_ack_result(XIAOTAI_OK);
        CHECK(status().phase == XIAOTAI_BINDING_BOUND);
        needs(1);
    }
    CHECK(g_identity.saves == 0U);
    stop_system();
    return 0;
}

static int test_grant_tolerant(void)
{
    /* The tolerant rules: an unusable grant is ignored and waiting goes on. */
    xiaotai_binding_config_t config = {.manual_start = true, .ignore_unusable_grants = true, .confirm_ms = 5000};
    CHECK(start_system(&config, "old-id", "old-key") == 0);
    needs(1);
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(reach_waiting("123456", 100) == 0);
    const char *ignored[] = {
        GRANT(PAIR("other-id", "other-key")), GRANT(PAIR("old-id", "changed-key")),
        GRANT("{\"device_id\":\"old-id\"}"), GRANT("[]"), GRANT("\"\""), GRANT("{\"x\":1}"),
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        fake_guest_message("auth_grant", ignored[i]);
        CHECK(status().phase == XIAOTAI_BINDING_WAITING);
    }
    xiaotai_binding_stats_t counters = stats();
    CHECK(counters.grants_mismatched == 2U && counters.grants_malformed == 4U && counters.grants == 0U);
    CHECK(g_guest.acks == 0U && g_identity.saves == 0U);
    /* An empty object keeps the stored identity; a later grant changes nothing. */
    fake_guest_message("auth_grant", GRANT("{}"));
    CHECK(status().phase == XIAOTAI_BINDING_CONFIRMING && g_guest.ack_timeout_ms == 5000U);
    fake_guest_message("auth_grant", GRANT(PAIR("other-id", "other-key")));
    CHECK(stats().grants_ignored == 1U);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND && last(XIAOTAI_EVENT_BINDING_BOUND)->value == 0);
    CHECK(g_identity.saves == 0U && strcmp(g_identity.identity.device_id, "old-id") == 0);
    stop_system();

    /* No identity stored: null is ignored, a full grant is stored, then acknowledged. */
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(reach_waiting("123456", 100) == 0);
    fake_guest_message("auth_grant", GRANT("null"));
    CHECK(status().phase == XIAOTAI_BINDING_WAITING && stats().grants_malformed == 1U);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    CHECK(status().phase == XIAOTAI_BINDING_STORING && g_guest.acks == 0U);
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(g_guest.acks == 1U && g_guest.ack_with_identity);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(last(XIAOTAI_EVENT_BINDING_BOUND)->value == 1 && strcmp(g_identity.identity.device_id, "new-id") == 0);

    /* A failed save: nothing acknowledged, nothing stored. */
    fake_identity_set(NULL, NULL);
    needs(0);
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(reach_waiting("123456", 100) == 0);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    unsigned acks = g_guest.acks;
    fake_identity_finish_save(XIAOTAI_FAIL);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_FAIL);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "store") == 0 && g_guest.acks == acks);
    CHECK(g_identity.state == XIAOTAI_IDENTITY_ABSENT && stats().store_failures == 1U && !g_guest.open);
    stop_system();
    return 0;
}

/* ================================================================== window */

static int expect_window_transition(int64_t at_us, xiaotai_binding_phase_t from, xiaotai_binding_phase_t to)
{
    fake_clock_advance_to(at_us - 1);
    CHECK(status().phase == from && events(XIAOTAI_EVENT_BINDING_FAILED) == 0);
    fake_clock_advance_to(at_us);
    CHECK(status().phase == to && events(XIAOTAI_EVENT_BINDING_FAILED) == 1);
    CHECK(last(XIAOTAI_EVENT_BINDING_FAILED)->at_us == at_us);
    return 0;
}

static int test_window_token(void)
{
    CHECK(start_system(NULL, NULL, NULL) == 0);
    needs(0);
    /* The token expires in 2 s, well before wait_s (190). */
    int64_t replied = xiaotai_time_us();
    CHECK(reach_waiting("123456", 2) == 0);
    CHECK(last(XIAOTAI_EVENT_BINDING_CODE)->value == 2 && status().seconds_left == 2U);
    /* Neither a clock correction nor a reconnection moves the end. */
    fake_clock_advance_to(replied + 1000000);
    g_unix_ms += 100000;
    fake_guest_ready();
    CHECK(events(XIAOTAI_EVENT_BINDING_CODE) == 1);
    CHECK(expect_window_transition(replied + 2000000, XIAOTAI_BINDING_WAITING, XIAOTAI_BINDING_EXPIRED) == 0);
    xiaotai_binding_status_t now = status();
    CHECK(now.error == XIAOTAI_ERR_TIMEOUT && now.verification[0] == '\0' && now.seconds_left == 0U);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "expired") == 0 &&
          last(XIAOTAI_EVENT_BINDING_FAILED)->value == XIAOTAI_ERR_TIMEOUT);
    CHECK(stats().expired == 1U && stats().failed == 0U && !g_guest.open && g_guest.closes == 1U);
    /* EXPIRED waits for the user like FAILED. */
    CHECK(owner_start() == XIAOTAI_OK && g_request.count == 2U);
    stop_system();
    return 0;
}

static int test_window_wait_s(void)
{
    xiaotai_binding_config_t config = {.wait_s = 1};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    int64_t replied = xiaotai_time_us();
    /* An opaque token (no expiry) leaves the configured wait alone. */
    fake_reply(200, 200, fake_report_body("123456", "opaque-token", "tmp_fixture"));
    fake_guest_ready();
    CHECK(status().phase == XIAOTAI_BINDING_WAITING && last(XIAOTAI_EVENT_BINDING_CODE)->value == 1);
    CHECK(expect_window_transition(replied + 1000000, XIAOTAI_BINDING_WAITING, XIAOTAI_BINDING_EXPIRED) == 0);
    stop_system();
    return 0;
}

static int expect_stale_rereport(int64_t replied, unsigned previous_count)
{
    fake_clock_advance_to(replied + 999999);
    CHECK(status().phase == XIAOTAI_BINDING_REPORTING && g_request.count == previous_count);
    fake_clock_advance_to(replied + 1000000);
    CHECK(g_request.count == previous_count + 1U && g_request.at_us == replied + 1000000);
    return 0;
}

static int test_window_stale(void)
{
    CHECK(start_system(NULL, NULL, NULL) == 0);
    needs(0);
    /* A token with less than a second left: the server replayed an old code. */
    int64_t replied = xiaotai_time_us();
    fake_reply(200, 200, fake_report_body("111111", token_expiring_in(0), "tmp_fixture"));
    CHECK(status().phase == XIAOTAI_BINDING_REPORTING && g_guest.opens == 0U && stats().stale_code_retries == 1U);
    CHECK(status().verification[0] == '\0');
    CHECK(expect_stale_rereport(replied, 1U) == 0);
    replied = xiaotai_time_us();
    fake_reply(200, 200, fake_report_body("111111", token_expiring_in(0), "tmp_fixture"));
    CHECK(expect_stale_rereport(replied, 2U) == 0);
    /* Three reports in all, then the attempt fails. */
    fake_reply(200, 200, fake_report_body("111111", token_expiring_in(0), "tmp_fixture"));
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_TIMEOUT);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "report") == 0 && stats().stale_code_retries == 3U);
    fake_clock_advance_to(xiaotai_time_us() + 1200000);
    CHECK(g_request.count == 3U);
    /* Once stale, then a fresh code. */
    CHECK(owner_start() == XIAOTAI_OK);
    replied = xiaotai_time_us();
    fake_reply(200, 200, fake_report_body("111111", token_expiring_in(0), "tmp_fixture"));
    CHECK(expect_stale_rereport(replied, 4U) == 0);
    CHECK(reach_waiting("222222", 100) == 0);
    CHECK(strcmp(status().verification, "222222") == 0);
    stop_system();
    return 0;
}

static int test_window_connect(void)
{
    xiaotai_binding_config_t config = {.connect_s = 1};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    int64_t replied = xiaotai_time_us();
    fake_reply(200, 200, fake_report_body("123456", token_expiring_in(100), "tmp_fixture"));
    CHECK(expect_window_transition(replied + 1000000, XIAOTAI_BINDING_CONNECTING, XIAOTAI_BINDING_FAILED) == 0);
    CHECK(status().error == XIAOTAI_ERR_TIMEOUT && strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "connect") == 0);
    CHECK(stats().connect_timeouts == 1U && stats().expired == 0U && !g_guest.open);
    CHECK(events(XIAOTAI_EVENT_BINDING_CODE) == 0);
    stop_system();

    /* The window ends before connect_s: still a connection failure, since no
     * code was ever shown. */
    CHECK(start_system(NULL, NULL, NULL) == 0);
    g_unix_ms += 500;
    needs(0);
    replied = xiaotai_time_us();
    fake_reply(200, 200, fake_report_body("123456", token_expiring_in(2), "tmp_fixture"));
    CHECK(expect_window_transition(replied + 1500000, XIAOTAI_BINDING_CONNECTING, XIAOTAI_BINDING_FAILED) == 0);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "connect") == 0 && stats().expired == 0U);
    /* A refused guest session fails at once. */
    g_guest.open_refuse = XIAOTAI_ERR_PLATFORM_OFFLINE;
    CHECK(owner_start() == XIAOTAI_OK);
    fake_reply(200, 200, fake_report_body("123456", token_expiring_in(100), "tmp_fixture"));
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_PLATFORM_OFFLINE);
    stop_system();
    return 0;
}

#ifdef BINDING_REAL_JWT
static const char jwt_missing[] = "e30.eyJzdWIiOiJmaXh0dXJlIn0.Zml4dHVyZQ";
static const char jwt_expired[] = "e30.eyJleHAiOjE2OTl9.Zml4dHVyZQ";
static const char jwt_two_seconds[] = "e30.eyJleHAiOjE3MDJ9.Zml4dHVyZQ";
static const char jwt_valid[] = "e30.eyJleHAiOjE4MTB9.Zml4dHVyZQ";

static int start_jwt_report_with_timing(const char *token, int64_t millis, uint16_t connect_s, uint16_t guest_s,
                                        bool expiry_required)
{
    const xiaotai_binding_config_t config = {
        .wait_s = 190, .connect_s = connect_s, .confirm_ms = 5000, .ignore_unusable_grants = true,
        .guest_subscribe_timeout_s = guest_s, .guest_token_needs_expiry = expiry_required,
    };
    CHECK(start_system(&config, NULL, NULL) == 0);
    g_unix_ms = millis;
    needs(0);
    fake_reply(200, 200, fake_report_body("012345", token, "tmp_fixture"));
    return 0;
}

static int start_jwt_report(const char *token, int64_t millis)
{
    return start_jwt_report_with_timing(token, millis, 20, 0, false);
}

static int test_window_jwt_split_timeout(void)
{
    const struct {const char *token; int64_t wall_ms, cap_us; uint16_t connect_s, guest_s;} cases[] = {
        {jwt_valid, 1700000, 20000000, 20, 20},
        {jwt_valid, 1700000, 7000000, 7, 20},
        {jwt_two_seconds, 1700750, 1250000, 20, 20},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const int64_t start = xiaotai_time_us();
        CHECK(start_jwt_report_with_timing(cases[i].token, cases[i].wall_ms, cases[i].connect_s,
                                          cases[i].guest_s, false) == 0);
        CHECK(g_guest.opens == 1U && g_guest.guest.subscribe_timeout_s == cases[i].guest_s &&
              g_guest.guest.initial_subscribe_deadline_us == start + cases[i].cap_us);
        CHECK(expect_window_transition(start + cases[i].cap_us, XIAOTAI_BINDING_CONNECTING,
                                       XIAOTAI_BINDING_FAILED) == 0);
        CHECK(g_guest.closes == 1U && stats().connect_timeouts == 1U && !events(XIAOTAI_EVENT_BINDING_CODE));
        printf("Report owner -> guest timeout=%us, absolute cap offset=%lldus\n",
               (unsigned)cases[i].guest_s, (long long)cases[i].cap_us);
        stop_system();
    }
    return 0;
}

static int test_window_jwt_ready_at(int64_t offset_us, bool expiry_required)
{
    const int64_t deadline = xiaotai_time_us() + 2000000;
    CHECK(start_jwt_report_with_timing(jwt_two_seconds, 1700000, 20, expiry_required ? 20 : 0,
                                       expiry_required) == 0);
    CHECK(status().phase == XIAOTAI_BINDING_CONNECTING && g_guest.opens == 1U);
    /* Dispatch the ready callback in the same owner turn as the clock
     * advance, before the framework gets another turn to run its timers. */
    fake_guest_ready_at(deadline + offset_us);
    const xiaotai_binding_status_t now = status();
    const unsigned codes = events(XIAOTAI_EVENT_BINDING_CODE);
    printf("Report JWT ready offset_us=%lld: phase=%d error=%d codes=%u closes=%u\n",
           (long long)offset_us, now.phase, now.error, codes, g_guest.closes);
    if (offset_us < 0) {
        CHECK(now.phase == XIAOTAI_BINDING_WAITING && codes == 1U && !strcmp(now.verification, "012345"));
        CHECK(last(XIAOTAI_EVENT_BINDING_CODE)->value == 1 && g_guest.closes == 0U);
    } else {
        CHECK(now.phase == XIAOTAI_BINDING_FAILED && now.error == XIAOTAI_ERR_TIMEOUT &&
              now.verification[0] == '\0' && now.seconds_left == 0U && codes == 0U);
        CHECK(events(XIAOTAI_EVENT_BINDING_FAILED) == 1U &&
              !strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "connect"));
        CHECK(g_guest.closes == 1U && !g_guest.open && g_mem.freed_wiped);
        CHECK(stats().connect_timeouts == 1U && stats().expired == 0U);
    }
    CHECK(g_identity.saves == 0U && g_guest.acks == 0U);
    stop_system();
    return 0;
}

static int test_window_jwt(void)
{
    static const struct { int64_t wall_ms, deadline_us; } cases[] = {{1700000, 2000000}, {1700750, 1250000}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const int64_t start = xiaotai_time_us();
        CHECK(start_jwt_report(jwt_two_seconds, cases[i].wall_ms) == 0);
        CHECK(g_guest.opens == 1 && status().phase == XIAOTAI_BINDING_CONNECTING);
        CHECK(!g_guest.guest.subscribe_timeout_s && !g_guest.guest.initial_subscribe_deadline_us);
        CHECK(expect_window_transition(start + cases[i].deadline_us, XIAOTAI_BINDING_CONNECTING,
                                       XIAOTAI_BINDING_FAILED) == 0);
        fake_guest_ready();
        CHECK(events(XIAOTAI_EVENT_BINDING_CODE) == 0 && g_guest.closes == 1U &&
              status().error == XIAOTAI_ERR_TIMEOUT);
        printf("Report JWT wall=%lld: deadline=%lldus, no code on timeout or late readiness\n",
               (long long)cases[i].wall_ms, (long long)cases[i].deadline_us);
        stop_system();
    }
    CHECK(test_window_jwt_ready_at(-1, false) == 0);
    CHECK(test_window_jwt_ready_at(0, false) == 0);
    CHECK(test_window_jwt_ready_at(1, false) == 0);
    CHECK(test_window_jwt_split_timeout() == 0);
    CHECK(start_jwt_report(jwt_valid, 1700000) == 0);
    fake_clock_advance_to(xiaotai_time_us() + 300000);
    fake_guest_ready();
    CHECK(status().phase == XIAOTAI_BINDING_WAITING && events(XIAOTAI_EVENT_BINDING_CODE) == 1);
    CHECK(!strcmp(status().verification, "012345") && g_identity.saves == 0U && g_guest.acks == 0U);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    CHECK(g_identity.saves == 1U && g_guest.acks == 0U);
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(g_guest.acks == 1U && g_guest.ack_with_identity && g_guest.ack_timeout_ms == 5000U);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND && g_guest.closes == 1U);
    stop_system();
    return 0;
}

static int test_window_jwt_admission(void)
{
    const struct { const char *name, *token; int64_t wall_ms; xiaotai_err_t error; } bad[] = {
        {"missing-exp", jwt_missing, 1700000, XIAOTAI_ERR_INVALID_RESPONSE},
        {"malformed", "bad.jwt", 1700000, XIAOTAI_ERR_INVALID_RESPONSE},
        {"expired", jwt_expired, 1700000, XIAOTAI_ERR_TIMEOUT},
        {"at-expiry", jwt_two_seconds, 1702000, XIAOTAI_ERR_TIMEOUT},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        CHECK(start_jwt_report_with_timing(bad[i].token, bad[i].wall_ms, 20, 20, true) == 0);
        const xiaotai_binding_status_t now = status();
        CHECK(now.phase == XIAOTAI_BINDING_FAILED && now.error == bad[i].error && !now.verification[0] &&
              !now.seconds_left && !g_guest.opens && !g_guest.closes && !events(XIAOTAI_EVENT_BINDING_CODE));
        CHECK(events(XIAOTAI_EVENT_BINDING_FAILED) == 1U &&
              !strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "report"));
        fake_clock_advance_to(xiaotai_time_us() + 2000000);
        CHECK(g_request.count == 1U && !stats().stale_code_retries && g_mem.freed_wiped);
        printf("Report JWT %s: error=0x%x, no guest, code or re-report\n", bad[i].name, (unsigned)now.error);
        stop_system();
    }

    const int64_t replied = xiaotai_time_us();
    CHECK(start_jwt_report_with_timing(jwt_two_seconds, 1701250, 20, 20, true) == 0);
    CHECK(status().phase == XIAOTAI_BINDING_REPORTING && !g_guest.opens && !events(XIAOTAI_EVENT_BINDING_CODE));
    CHECK(stats().stale_code_retries == 1U && expect_stale_rereport(replied, 1U) == 0);
    fake_reply(200, 200, fake_report_body("012345", jwt_valid, "tmp_fixture"));
    CHECK(status().phase == XIAOTAI_BINDING_CONNECTING && g_guest.opens == 1U);
    fake_guest_ready();
    CHECK(events(XIAOTAI_EVENT_BINDING_CODE) == 1U);
    stop_system();

    const int64_t opened = xiaotai_time_us();
    CHECK(start_jwt_report_with_timing(jwt_two_seconds, 1701000, 20, 20, true) == 0);
    CHECK(g_guest.opens == 1U && g_guest.guest.initial_subscribe_deadline_us == opened + 1000000);
    stop_system();

    /* The opt-out keeps opaque-token support on existing products. */
    CHECK(start_jwt_report(jwt_missing, 1700000) == 0);
    fake_guest_ready();
    CHECK(g_guest.opens == 1U && status().phase == XIAOTAI_BINDING_WAITING && events(XIAOTAI_EVENT_BINDING_CODE) == 1U);
    stop_system();
    CHECK(test_window_jwt_ready_at(0, true) == 0);
    return 0;
}
#endif

/* ================================================================== cancel */

static int waiting_attempt(void)
{
    if (status().phase != XIAOTAI_BINDING_REPORTING) CHECK(owner_start() == XIAOTAI_OK);
    CHECK(reach_waiting("123456", 100) == 0);
    return 0;
}

static int expect_cancelled(const char *step)
{
    xiaotai_binding_status_t now = status();
    CHECK(now.phase == XIAOTAI_BINDING_FAILED && now.error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, step) == 0);
    CHECK(!g_guest.open && now.verification[0] == '\0');
    return 0;
}

static int test_cancel(void)
{
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    /* Network down. */
    CHECK(waiting_attempt() == 0);
    g_net.state = XIAOTAI_NET_WAITING;
    emit(XIAOTAI_EVENT_NET_DOWN, 0, &g_net, sizeof(g_net));
    CHECK(expect_cancelled("grant") == 0);
    g_net.state = XIAOTAI_NET_UP;
    /* The user left the network (a state report, the address still up). */
    CHECK(waiting_attempt() == 0);
    xiaotai_net_status_t left = g_net;
    left.user_disconnected = true;
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_UP, &left, sizeof(left));
    CHECK(expect_cancelled("grant") == 0);
    /* Quiesce for another reason: agreed to, the attempt ends. */
    CHECK(waiting_attempt() == 0);
    CHECK(owner_quiesce(XIAOTAI_QUIESCE_PROVISIONING) == XIAOTAI_OK);
    CHECK(expect_cancelled("grant") == 0);
    CHECK(owner_start() == XIAOTAI_ERR_BUSY);
    owner_quiesce_end();
    /* The platform core left the phases a guest session lives in. */
    CHECK(owner_start() == XIAOTAI_OK);
    g_platform.phase = XIAOTAI_PLATFORM_LOGIN;
    emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, XIAOTAI_PLATFORM_OFFLINE_BINDING, NULL, 0);
    CHECK(expect_cancelled("report") == 0);
    g_platform.phase = XIAOTAI_PLATFORM_NO_IDENTITY;
    /* The guest session ended by itself. */
    CHECK(waiting_attempt() == 0);
    fake_guest_closed(XIAOTAI_ERR_PLATFORM_OFFLINE);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_PLATFORM_OFFLINE);
    /* Explicit cancel: back to NEEDED. */
    CHECK(waiting_attempt() == 0);
    CHECK(owner_cancel() == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_NEEDED && !g_guest.open && status().verification[0] == '\0');
    CHECK(owner_cancel() == XIAOTAI_ERR_INVALID_STATE);
    CHECK(stats().cancelled == 5U && stats().failed == 5U);
    /* platform.online during an attempt: cancelled, then BOUND. */
    CHECK(waiting_attempt() == 0);
    g_platform.phase = XIAOTAI_PLATFORM_ONLINE;
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL, 0);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND && !g_guest.open);
    stop_system();
    return 0;
}

static int test_cancel_storing(void)
{
    xiaotai_binding_config_t config = {.manual_start = true, .require_active_presave_window = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(waiting_attempt() == 0);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    CHECK(status().phase == XIAOTAI_BINDING_STORING);
    /* What was granted is finished: no cancel, no quiesce stops it. */
    CHECK(owner_cancel() == XIAOTAI_ERR_INVALID_STATE);
    CHECK(owner_quiesce(XIAOTAI_QUIESCE_PROVISIONING) == XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_STORING);
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_CONFIRMING);
    CHECK(owner_cancel() == XIAOTAI_ERR_INVALID_STATE);
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND);
    owner_quiesce_end();
    stop_system();

    /* The network goes down while storing: the store finishes, nothing is
     * acknowledged. */
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(waiting_attempt() == 0);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    g_net.state = XIAOTAI_NET_WAITING;
    emit(XIAOTAI_EVENT_NET_DOWN, 0, &g_net, sizeof(g_net));
    CHECK(status().phase == XIAOTAI_BINDING_STORING);
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(expect_cancelled("store") == 0);
    CHECK(g_guest.acks == 0U && g_identity.state == XIAOTAI_IDENTITY_VALID);
    stop_system();
    return 0;
}

/* ================================================================ ack_lost */

static int test_ack_lost(void)
{
    xiaotai_binding_config_t config = {.manual_start = true, .confirm_ms = 5000};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(waiting_attempt() == 0);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(g_guest.acks == 1U && g_guest.ack_timeout_ms == 5000U);
    fake_guest_ack_result(XIAOTAI_ERR_TIMEOUT);
    xiaotai_binding_status_t now = status();
    CHECK(now.phase == XIAOTAI_BINDING_FAILED && now.error == XIAOTAI_ERR_TIMEOUT);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "confirm") == 0 && stats().confirm_timeouts == 1U);
    CHECK(g_identity.state == XIAOTAI_IDENTITY_VALID && !g_guest.open);
    xiaotai_delay_ms(200);
    (void)fake_drain();
    CHECK(g_request.count == 1U);

    /* Retry reads the identity back first and then reports signed. */
    CHECK(owner_retry() == XIAOTAI_OK);
    CHECK(g_identity.reloads == 1U && g_request.count == 1U);
    CHECK(owner_retry() == XIAOTAI_ERR_INVALID_STATE);
    fake_identity_finish_reload(XIAOTAI_OK, XIAOTAI_IDENTITY_VALID);
    CHECK(g_request.count == 2U && g_request.auth == XIAOTAI_PLATFORM_AUTH_SIGNED);
    CHECK(status().signed_rebinding && stats().attempts_signed == 1U);
    CHECK(reach_waiting("123456", 100) == 0);
    fake_guest_message("auth_grant", GRANT("\"\""));
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND && last(XIAOTAI_EVENT_BINDING_BOUND)->value == 0);
    CHECK(g_identity.saves == 1U);
    stop_system();
    return 0;
}

static int test_ack_window_and_storage(void)
{
    xiaotai_binding_config_t config = {.manual_start = true, .require_active_presave_window = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    /* The window (1 s) ends while the identity is being stored: stored, but
     * not acknowledged. */
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(reach_waiting("123456", 1) == 0);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    xiaotai_delay_ms(1200);
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_TIMEOUT);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "confirm") == 0 && g_guest.acks == 0U);
    CHECK(g_identity.state == XIAOTAI_IDENTITY_VALID);

    /* A save refused at once. */
    fake_identity_set(NULL, NULL);
    CHECK(waiting_attempt() == 0);
    g_identity.save_refuse = XIAOTAI_ERR_NO_MEM;
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_NO_MEM);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "store") == 0);
    g_identity.save_refuse = XIAOTAI_OK;

    /* A reload that fails ends in FAILED with its error. */
    CHECK(owner_retry() == XIAOTAI_OK);
    fake_identity_finish_reload(XIAOTAI_ERR_TIMEOUT, XIAOTAI_IDENTITY_ABSENT);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_TIMEOUT);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "store") == 0);
    /* An identity that cannot be read is never overwritten. */
    unsigned requests = g_request.count;
    CHECK(owner_retry() == XIAOTAI_OK);
    fake_identity_finish_reload(XIAOTAI_OK, XIAOTAI_IDENTITY_UNREADABLE);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(g_request.count == requests);
    CHECK(owner_start() == XIAOTAI_ERR_INVALID_STATE);
    /* A reload refused at once. */
    g_identity.reload_refuse = XIAOTAI_ERR_NO_MEM;
    CHECK(owner_retry() == XIAOTAI_ERR_NO_MEM);
    g_identity.reload_refuse = XIAOTAI_OK;
    /* The reload's result reaches the owner task also when its hand-off finds
     * the owner queue full: the module polls for it. */
    CHECK(owner_retry() == XIAOTAI_OK);
    g_identity.state = XIAOTAI_IDENTITY_ABSENT;
    g_run_fail_once = true;
    fake_identity_finish_reload(XIAOTAI_OK, XIAOTAI_IDENTITY_ABSENT);
    CHECK(!g_run_fail_once && g_run_failures == 1U);
    CHECK(wait_requests(requests + 1U, 1000));
    CHECK(status().phase == XIAOTAI_BINDING_REPORTING && !status().signed_rebinding);
    stop_system();
    return 0;
}

static void dispatch_staged_grant(void *ctx)
{
    const fake_guest_t *guest = ctx;
    static const char json[] = GRANT(PAIR("new-id", "new-key"));
    const xiaotai_platform_signal_t message = {.type = "auth_grant", .channel = "", .json = json,
        .size = sizeof(json) - 1U, .generation = 1, .command = true};
    guest->guest.message(guest->id, &message, guest->guest.ctx);
}

static void deliver_grant_at_presave_deadline(void *unused)
{
    (void)unused;
    binding_attempt_t *attempt = binding_attempt();
    if (!attempt || binding_phase() != XIAOTAI_BINDING_WAITING || !g_guest.open) abort();
    /* The time can advance after the owner checked due timers and before it
     * dispatches the already dequeued grant. */
    attempt->deadline_us = xiaotai_time_us();
    dispatch_staged_grant(&g_guest);
}

static int test_presave_window(void)
{
    for (unsigned required = 0; required < 2; ++required) {
        const xiaotai_binding_config_t config = {.manual_start = true,
            .confirm_ms = required ? 0U : 5000U, .require_active_presave_window = required != 0U};
        CHECK(start_system(&config, NULL, NULL) == 0);
        needs(0);
        CHECK(waiting_attempt() == 0);
        fake_on_owner(deliver_grant_at_presave_deadline, NULL);
        CHECK(g_guest.acks == 0U);
        if (required) {
            CHECK(status().phase == XIAOTAI_BINDING_EXPIRED && status().error == XIAOTAI_ERR_TIMEOUT);
            CHECK(g_identity.saves == 0U && !g_guest.open && stats().expired == 1U);
        } else {
            CHECK(status().phase == XIAOTAI_BINDING_STORING && g_identity.saves == 1U);
            fake_identity_finish_save(XIAOTAI_OK);
            CHECK(status().phase == XIAOTAI_BINDING_CONFIRMING && g_guest.acks == 1U && g_guest.ack_with_identity);
            CHECK(g_guest.ack_timeout_ms == 5000U);
            fake_guest_ack_result(XIAOTAI_OK);
            CHECK(status().phase == XIAOTAI_BINDING_BOUND);
        }
        stop_system();
    }
    return 0;
}

static int test_ack_submit_refused(void)
{
    const xiaotai_binding_config_t config = {.manual_start = true, .require_active_presave_window = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(waiting_attempt() == 0);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    CHECK(status().phase == XIAOTAI_BINDING_STORING && g_guest.acks == 0U);
    g_guest.ack_refuse = XIAOTAI_FAIL;
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_FAIL);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "confirm") == 0 && !g_guest.ack_pending);
    CHECK(g_identity.state == XIAOTAI_IDENTITY_VALID && g_identity.saves == 1U && !g_guest.open);
    stop_system();
    return 0;
}

static int test_cancelled_grant_before_save(void)
{
    const xiaotai_binding_config_t config = {.manual_start = true, .require_active_presave_window = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(waiting_attempt() == 0);
    fake_guest_t previous = g_guest;
    CHECK(owner_cancel() == XIAOTAI_OK && status().phase == XIAOTAI_BINDING_NEEDED);
    CHECK(waiting_attempt() == 0);
    const unsigned entered = stats().grants;
    fake_on_owner(dispatch_staged_grant, &previous);
    CHECK(status().phase == XIAOTAI_BINDING_WAITING && stats().grants == entered);
    CHECK(g_identity.saves == 0U && g_guest.acks == 0U);
    stop_system();
    return 0;
}

typedef struct { xiaotai_signal_t blocked, resume; } delayed_net_t;

static void hold_owner_for_net_event(void *ctx)
{
    delayed_net_t *hold = ctx;
    xiaotai_signal_give(&hold->blocked);
    if (!xiaotai_signal_take(&hold->resume, 3000)) abort();
}

static int test_manual_disconnect_backlog(void)
{
    for (unsigned strict = 0; strict < 2U; ++strict) {
        const xiaotai_binding_config_t config = {.manual_start = true,
            .reject_manual_disconnect_during_commit = strict != 0U};
        CHECK(start_system(&config, NULL, NULL) == 0);
        needs(0);
        CHECK(waiting_attempt() == 0);
        delayed_net_t hold;
        CHECK(xiaotai_signal_init(&hold.blocked) == XIAOTAI_OK && xiaotai_signal_init(&hold.resume) == XIAOTAI_OK);
        CHECK(xiaotai_run(hold_owner_for_net_event, &hold) == XIAOTAI_OK);
        CHECK(xiaotai_signal_take(&hold.blocked, 3000));
        fake_guest_t staged = g_guest;
        CHECK(xiaotai_run(dispatch_staged_grant, &staged) == XIAOTAI_OK);
        g_net.user_disconnected = true;
        const xiaotai_args_t note = {.value = XIAOTAI_NET_UP};
        CHECK(xiaotai_event_emit(XIAOTAI_EVENT_NET_STATE, &note, &g_net, sizeof(g_net)) == XIAOTAI_OK);
        xiaotai_signal_give(&hold.resume);
        fake_drain_events();
        CHECK(g_guest.acks == 0U);
        if (strict) {
            CHECK(g_identity.saves == 0U && status().phase == XIAOTAI_BINDING_FAILED);
            CHECK(status().error == XIAOTAI_ERR_INVALID_STATE && g_identity.state == XIAOTAI_IDENTITY_ABSENT);
        } else {
            CHECK(g_identity.saves == 1U && status().phase == XIAOTAI_BINDING_STORING);
            fake_identity_finish_save(XIAOTAI_OK);
            CHECK(g_guest.acks == 0U && g_identity.state == XIAOTAI_IDENTITY_VALID);
        }
        xiaotai_signal_deinit(&hold.resume);
        xiaotai_signal_deinit(&hold.blocked);
        stop_system();
    }
    return 0;
}

static int test_manual_disconnect_after_save(void)
{
    const xiaotai_binding_config_t config = {.manual_start = true, .reject_manual_disconnect_during_commit = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(waiting_attempt() == 0);
    /* A transient link state alone cannot trigger the manual-disconnect policy. */
    g_net.state = XIAOTAI_NET_WAITING;
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    CHECK(status().phase == XIAOTAI_BINDING_STORING && g_identity.saves == 1U);
    /* The worker has a new snapshot, but the owner has not handled NET_STATE. */
    g_net.user_disconnected = true;
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(g_identity.state == XIAOTAI_IDENTITY_VALID && g_identity.saves == 1U && g_guest.acks == 0U);
    CHECK(status().phase == XIAOTAI_BINDING_FAILED && status().error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(strcmp(last(XIAOTAI_EVENT_BINDING_FAILED)->text, "confirm") == 0);
    stop_system();
    return 0;
}

static void unused_timer(void *ctx) { (void)ctx; }

static void fill_timers(void *ctx)
{
    uint32_t *last = ctx;
    uint32_t timer;
    *last = 0;
    while (xiaotai_timer_start(60000, unused_timer, NULL, &timer) == XIAOTAI_OK) *last = timer;
}

static void free_timer(void *ctx) { xiaotai_timer_cancel(*(const uint32_t *)ctx); }

static int test_reload_needs_completion_poll(void)
{
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    uint32_t last;
    fake_on_owner(fill_timers, &last);
    CHECK(last != 0);
    CHECK(owner_retry() == XIAOTAI_ERR_NO_MEM);
    CHECK(!g_identity.reload_pending && g_identity.reloads == 0 && g_request.count == 0);
    CHECK(status().phase == XIAOTAI_BINDING_NEEDED);

    /* Free the only available slot, then fail the direct hand-off. The
     * poll that admitted this job must deliver it, without cancel/reboot. */
    fake_on_owner(free_timer, &last);
    CHECK(owner_retry() == XIAOTAI_OK);
    CHECK(g_identity.reload_pending && g_identity.reloads == 1);
    g_run_fail_once = true;
    fake_identity_finish_reload(XIAOTAI_OK, XIAOTAI_IDENTITY_ABSENT);
    CHECK(g_run_failures == 1);
    CHECK(wait_requests(1, 1000));
    CHECK(status().phase == XIAOTAI_BINDING_REPORTING);
    stop_system();
    return 0;
}

/* ================================================================= quiesce */

static int refuses_environment(void)
{
    CHECK(owner_quiesce(XIAOTAI_QUIESCE_ENVIRONMENT) == XIAOTAI_ERR_BUSY);
    CHECK(!xiaotai_quiesced());
    return 0;
}

static int agrees_environment(void)
{
    CHECK(owner_quiesce(XIAOTAI_QUIESCE_ENVIRONMENT) == XIAOTAI_OK);
    owner_quiesce_end();
    return 0;
}

static int read_file(const char *path, char *out, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    size_t length = fread(out, 1, capacity - 1U, file);
    fclose(file);
    out[length] = '\0';
    return 0;
}

static int test_quiesce_guard(void)
{
    s_steps[0] = (step_t){.name = "A", .mode = STEP_ASYNC};
    s_step_count = 1;
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    CHECK(agrees_environment() == 0);                  /* UNKNOWN */
    needs(0);
    CHECK(agrees_environment() == 0);                  /* NEEDED */
    CHECK(owner_start() == XIAOTAI_OK);
    CHECK(refuses_environment() == 0);                 /* PREPARING */
    step_done(0, XIAOTAI_OK);
    CHECK(refuses_environment() == 0);                 /* REPORTING */
    fake_reply(200, 200, fake_report_body("123456", token_expiring_in(100), "tmp_fixture"));
    CHECK(refuses_environment() == 0);                 /* CONNECTING */
    fake_guest_ready();
    CHECK(refuses_environment() == 0);                 /* WAITING: the code stays valid */
    CHECK(status().phase == XIAOTAI_BINDING_WAITING);
    fake_guest_message("auth_grant", GRANT(PAIR("new-id", "new-key")));
    CHECK(refuses_environment() == 0);                 /* STORING */
    fake_identity_finish_save(XIAOTAI_OK);
    CHECK(refuses_environment() == 0);                 /* CONFIRMING */
    fake_guest_ack_result(XIAOTAI_OK);
    CHECK(status().phase == XIAOTAI_BINDING_BOUND);
    CHECK(agrees_environment() == 0);                  /* BOUND */
    /* Another reason during WAITING is agreed to and cancels the attempt. */
    needs(0);
    CHECK(owner_start() == XIAOTAI_OK);
    step_done(0, XIAOTAI_OK);
    CHECK(reach_waiting("123456", 100) == 0);
    CHECK(owner_quiesce(XIAOTAI_QUIESCE_COPROCESSOR) == XIAOTAI_OK);
    CHECK(expect_cancelled("grant") == 0);
    owner_quiesce_end();
    CHECK(agrees_environment() == 0);                  /* FAILED */
    stop_system();

    /* The module depends on no environment module: neither its manifest nor
     * its build names one. */
    static char text[16384];
    CHECK(read_file(BINDING_COMPONENT_DIR "/module.json", text, sizeof(text)) == 0);
    CHECK(strstr(text, "environment") == NULL && strstr(text, "\"binding\"") != NULL);
    CHECK(read_file(BINDING_COMPONENT_DIR "/CMakeLists.txt", text, sizeof(text)) == 0);
    CHECK(strstr(text, "environment") == NULL && strstr(text, "xiaotai_platform") != NULL);
    return 0;
}

/* ============================================================== auto_start */

static int test_auto_start(void)
{
    /* Zero configuration: every report starts an attempt by itself. */
    CHECK(start_system(NULL, NULL, NULL) == 0);
    needs(0);
    CHECK(g_request.count == 1U && stats().attempts == 1U);
    /* Once per report: a failed attempt is not started again ... */
    fake_reply(200, 40901, "{\"code\":40901}");
    CHECK(status().phase == XIAOTAI_BINDING_FAILED);
    xiaotai_delay_ms(200);
    (void)fake_drain();
    CHECK(g_request.count == 1U);
    /* ... until the platform core reports again. */
    needs(0);
    CHECK(g_request.count == 2U && stats().attempts == 2U);
    stop_system();

    /* A guard's BUSY is waited out. */
    s_guard_on = true;
    s_guard_result = XIAOTAI_ERR_BUSY;
    CHECK(start_system(NULL, NULL, NULL) == 0);
    owner_guards_changed();                            /* nothing waits: harmless */
    CHECK(s_guard_calls == 0U);
    needs(0);
    CHECK(status().phase == XIAOTAI_BINDING_NEEDED && g_request.count == 0U && stats().guard_refusals == 1U);
    owner_guards_changed();                            /* still busy */
    CHECK(g_request.count == 0U && s_guard_calls == 2U);
    s_guard_result = XIAOTAI_OK;
    owner_guards_changed();
    CHECK(g_request.count == 1U && stats().auto_resumed == 1U && status().phase == XIAOTAI_BINDING_REPORTING);
    owner_guards_changed();                            /* no second start */
    CHECK(stats().attempts == 1U);
    CHECK(owner_cancel() == XIAOTAI_OK);

    /* A new report replaces the wait: refused otherwise than BUSY, nothing waits. */
    s_guard_result = XIAOTAI_ERR_BUSY;
    needs(0);
    s_guard_result = XIAOTAI_ERR_NOT_SUPPORTED;
    needs(0);
    s_guard_result = XIAOTAI_OK;
    owner_guards_changed();
    CHECK(stats().attempts == 1U && stats().auto_resumed == 1U && status().phase == XIAOTAI_BINDING_NEEDED);
    /* An application's start refused by a guard is not remembered. */
    s_guard_result = XIAOTAI_ERR_BUSY;
    CHECK(owner_start() == XIAOTAI_ERR_BUSY);
    s_guard_result = XIAOTAI_OK;
    owner_guards_changed();
    CHECK(stats().attempts == 1U && status().phase == XIAOTAI_BINDING_NEEDED);
    /* A wait ends with the phase it waited in. */
    s_guard_result = XIAOTAI_ERR_BUSY;
    needs(0);
    s_guard_result = XIAOTAI_OK;
    g_platform.phase = XIAOTAI_PLATFORM_ONLINE;
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL, 0);
    owner_guards_changed();
    CHECK(stats().attempts == 1U && status().phase == XIAOTAI_BINDING_BOUND);
    stop_system();

    /* manual_start: nothing begins until the application asks. */
    xiaotai_binding_config_t config = {.manual_start = true};
    CHECK(start_system(&config, NULL, NULL) == 0);
    needs(0);
    CHECK(status().phase == XIAOTAI_BINDING_NEEDED && g_request.count == 0U);
    xiaotai_delay_ms(100);
    (void)fake_drain();
    CHECK(g_request.count == 0U);
    CHECK(owner_start() == XIAOTAI_OK && g_request.count == 1U);
    stop_system();

    /* The configuration is checked and frozen at init. */
    xiaotai_binding_config_t bad = {.wait_s = 3601};
    CHECK(xiaotai_binding_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = (xiaotai_binding_config_t){.connect_s = 121};
    CHECK(xiaotai_binding_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = (xiaotai_binding_config_t){.guest_subscribe_timeout_s = 121};
    CHECK(xiaotai_binding_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = (xiaotai_binding_config_t){.announce_rounds = 11};
    CHECK(xiaotai_binding_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = (xiaotai_binding_config_t){.announce_gap_ms = 5001};
    CHECK(xiaotai_binding_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    bad = (xiaotai_binding_config_t){.voice = (xiaotai_binding_voice_t)9};
    CHECK(xiaotai_binding_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(start_system(NULL, NULL, NULL) == 0);
    CHECK(xiaotai_binding_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_binding_config_t *effective = binding_config();
    CHECK(effective->wait_s == 190U && effective->connect_s == 20U && effective->confirm_ms == 0U &&
          effective->stale_code_retries == 3U && effective->announce_rounds == 1U &&
          effective->announce_gap_ms == 350U && !effective->manual_start && !effective->ignore_unusable_grants);
    /* Registrations beyond the build option's capacities are refused. */
    static int dummy;
    for (int i = 0; i < XIAOTAI_BINDING_PREPARES_MAX; ++i) {
        CHECK(xiaotai_binding_prepare_register("extra", step_prepare, &dummy) == XIAOTAI_OK);
    }
    CHECK(xiaotai_binding_prepare_register("extra", step_prepare, &dummy) == XIAOTAI_ERR_NO_MEM);
    for (int i = 0; i < XIAOTAI_BINDING_GUARDS_MAX; ++i) {
        CHECK(xiaotai_binding_guard_register("extra", test_guard, NULL) == XIAOTAI_OK);
    }
    CHECK(xiaotai_binding_guard_register("extra", test_guard, NULL) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_binding_prepare_register(NULL, step_prepare, NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_binding_guard_register("x", NULL, NULL) == XIAOTAI_ERR_INVALID_ARG);
    stop_system();
    CHECK(xiaotai_binding_prepare_register("late", step_prepare, &dummy) == XIAOTAI_ERR_INVALID_STATE);
    return 0;
}

/* -------------------------------------------------------------------- main */

#define STR2(x) #x
#define STR(x) STR2(x)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g_mem.watch_size = sizeof(binding_attempt_t);
    const char *group = STR(BINDING_GROUP);
    if (strcmp(group, "first") == 0) {
        RUN(test_first_full);
        RUN(test_first_limits);
        RUN(test_first_refusals);
    } else if (strcmp(group, "prepare") == 0) {
        RUN(test_prepare_none);
        RUN(test_prepare_two_in_order);
        RUN(test_prepare_sync_and_failures);
        RUN(test_prepare_timeout_and_late);
        RUN(test_prepare_auto_and_bound_retry);
    } else if (strcmp(group, "resign") == 0) {
        RUN(test_resign);
    } else if (strcmp(group, "grant") == 0) {
        RUN(test_grant_strict);
        RUN(test_grant_tolerant);
    } else if (strcmp(group, "window") == 0) {
        fake_clock_start();
        RUN(test_window_token);
        RUN(test_window_wait_s);
        RUN(test_window_stale);
        RUN(test_window_connect);
#ifdef BINDING_REAL_JWT
    } else if (strcmp(group, "window_jwt") == 0) {
        fake_clock_start();
        RUN(test_window_jwt);
        RUN(test_window_jwt_admission);
#endif
    } else if (strcmp(group, "cancel") == 0) {
        RUN(test_cancel);
        RUN(test_cancel_storing);
    } else if (strcmp(group, "ack_lost") == 0) {
        RUN(test_ack_lost);
        RUN(test_ack_window_and_storage);
        RUN(test_presave_window);
        RUN(test_ack_submit_refused);
        RUN(test_cancelled_grant_before_save);
        RUN(test_manual_disconnect_backlog);
        RUN(test_manual_disconnect_after_save);
        RUN(test_reload_needs_completion_poll);
    } else if (strcmp(group, "quiesce_guard") == 0) {
        RUN(test_quiesce_guard);
    } else if (strcmp(group, "auto_start") == 0) {
        RUN(test_auto_start);
    } else {
        fprintf(stderr, "unknown group %s\n", group);
        return 1;
    }
    printf("PASS: binding %s\n", group);
    return 0;
}
