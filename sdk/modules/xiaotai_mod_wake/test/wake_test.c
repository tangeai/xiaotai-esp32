// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_preroll_handoff
#include "wake_internal.h"

#include <assert.h>
#include <string.h>
#include "xiaotai/at.h"
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"

static const char *const good[] = {"hello", "hi there", "wake", NULL};
static const char *const bad[] = {"hello", "hi there", "\xe4\xbb\x8a", NULL};
static xiaotai_wake_model_t model = {.phrases = good};
static struct {
    xiaotai_wake_fn_t detection;
    void *ctx;
    xiaotai_media_preroll_t held;
    xiaotai_local_wake_detection_t last;
    xiaotai_err_t listen_error, hold_error, start_error;
    uint32_t declared, activity, quiet, ai, token, handed, lost;
    unsigned listens, holds, starts, releases, events;
    bool wanted, no_model;
    char console[1024];
} s;
static xiaotai_signal_t s_done;

xiaotai_err_t xiaotai_wake_subscribe(xiaotai_wake_fn_t handler, void *ctx)
{
    assert(!s.detection && handler && xiaotai_on_owner());
    s.detection = handler;
    s.ctx = ctx;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_wake_listen(bool on)
{
    assert(xiaotai_on_owner());
    ++s.listens;
    if (s.listen_error != XIAOTAI_OK) return s.listen_error;
    s.wanted = on;
    return XIAOTAI_OK;
}

const xiaotai_wake_model_t *xiaotai_wake_model(void) { return s.no_model ? NULL : &model; }

void xiaotai_wake_status(xiaotai_wake_status_t *state)
{
    *state = (xiaotai_wake_status_t){.threshold_milli = 901};
}

xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *request)
{
    assert(!request->uplink.rate_hz && !request->downlink.rate_hz);
    s.declared = request->preroll_ms;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_preroll_hold(const xiaotai_media_preroll_t *request, uint32_t *token)
{
    assert(xiaotai_on_owner());
    ++s.holds;
    if (s.hold_error != XIAOTAI_OK) return s.hold_error;
    s.held = *request;
    s.token = 700U + s.holds;
    *token = s.token;
    return XIAOTAI_OK;
}

void xiaotai_media_preroll_release(uint32_t token)
{
    assert(token == s.token && token != 0);
    ++s.releases;
    s.token = 0;
}

xiaotai_err_t xiaotai_ai_talk_start(const xiaotai_ai_talk_start_t *request, uint32_t *session)
{
    assert(xiaotai_on_owner() && request->origin == XIAOTAI_AI_TALK_ORIGIN_WAKE);
    assert(request->preroll_token == s.token);
    s.handed = request->preroll_token;
    ++s.starts;
    if (s.start_error != XIAOTAI_OK) {
        if (s.handed) xiaotai_media_preroll_release(s.handed);
        return s.start_error;
    }
    const xiaotai_activity_t ai = {.name = "ai"};
    assert(xiaotai_activity_begin(&ai, &s.ai) == XIAOTAI_OK);
    *session = s.ai;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_preroll_lost(uint32_t token)
{
    assert(s.ai && xiaotai_on_owner() && token == s.handed);
    s.lost = token;
    xiaotai_activity_end(s.ai);
    s.ai = s.token = 0;
    return XIAOTAI_OK;
}

static void seen(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->size == sizeof(s.last) && event->data);
    s.last = *(const xiaotai_local_wake_detection_t *)event->data;
    assert((int32_t)s.last.outcome == event->args.value);
    if (s.last.outcome == XIAOTAI_LOCAL_WAKE_STARTED)
        assert(s.last.session == s.ai && s.ai == xiaotai_activity_current(NULL));
    ++s.events;
}

static xiaotai_err_t engine_query(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_at_printf(reply, "+WAKE: ready=1");
    return XIAOTAI_OK;
}

static xiaotai_err_t init_fixture(void)
{
    static const xiaotai_at_verb_t verbs[] = {{.run = engine_query}};
    assert(xiaotai_at_verbs_register("AT+WAKE", verbs, 1) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_WAKE_DETECTED, seen, NULL);
}

static const xiaotai_module_t wake = {.name = "wake", .init = xiaotai_module_wake_init, .start = xiaotai_module_wake_start};
static const xiaotai_module_t fake = {.name = "fixture", .init = init_fixture};
static const xiaotai_module_t *const modules[] = {&fake, &wake};

static void notify(void *ctx) { (void)ctx; xiaotai_signal_give(&s_done); }
static void barrier(void)
{
    assert(xiaotai_run(notify, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_done, 3000));
}

static void run_owner(void (*fn)(void *))
{
    assert(xiaotai_run(fn, NULL) == XIAOTAI_OK);
    barrier();
    barrier();
}

static void read_status(void *ctx) { xiaotai_local_wake_status(ctx); }
static xiaotai_local_wake_status_t status(void)
{
    xiaotai_local_wake_status_t value;
    assert(xiaotai_run(read_status, &value) == XIAOTAI_OK);
    barrier();
    return value;
}

static void detect(void *ctx)
{
    (void)ctx;
    const xiaotai_wake_detection_t detection = {.sequence = 19, .captured_us = 65432109,
        .score_milli = 963, .threshold_milli = 901};
    s.detection(&detection, s.ctx);
}

static void occupied(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t call = {.name = "call"};
    assert(xiaotai_activity_begin(&call, &s.activity) == XIAOTAI_OK);
    detect(NULL); /* Owner hasn't dispatched activity.changed yet, recheck real state. */
}

static void free_activity(void *ctx)
{
    (void)ctx;
    xiaotai_activity_end(s.activity);
    s.activity = 0;
}

static void quiet(void *ctx)
{
    (void)ctx;
    assert(xiaotai_quiesce_begin("test-maintenance", &s.quiet) == XIAOTAI_OK);
    detect(NULL);
}

static void unquiet(void *ctx)
{
    (void)ctx;
    xiaotai_quiesce_end(s.quiet);
}

static void disable(void *ctx)
{
    (void)ctx;
    const xiaotai_err_t expected = s.listen_error == XIAOTAI_ERR_NOT_SUPPORTED ? s.listen_error : XIAOTAI_OK;
    assert(xiaotai_local_wake_set_enabled(false) == expected);
}

static void do_gate(void)
{
    assert(s.wanted && s.listens == 1 && status().gate == XIAOTAI_LOCAL_WAKE_LISTENING);
    run_owner(occupied);
    assert(!s.wanted && s.listens == 2 && status().gate == XIAOTAI_LOCAL_WAKE_OFF_ACTIVITY);
    assert(s.events == 1 && s.last.outcome == XIAOTAI_LOCAL_WAKE_BUSY && !s.holds && !s.starts);
    run_owner(free_activity);
    assert(s.wanted && s.listens == 3);
    run_owner(quiet);
    assert(!s.wanted && s.listens == 4 && status().gate == XIAOTAI_LOCAL_WAKE_OFF_QUIESCED);
    assert(s.events == 2 && s.last.error == XIAOTAI_OK && !s.holds);
    run_owner(disable);
    assert(s.listens == 4 && status().gate == XIAOTAI_LOCAL_WAKE_OFF_DISABLED);
    run_owner(unquiet);
    assert(s.listens == 4 && !s.wanted);
}

static void do_switch(bool persistent, bool stored_off)
{
    int32_t value = -1;
    if (persistent) {
        assert(xiaotai_setting_get_int("wake.enabled", &value) == XIAOTAI_OK);
        assert(value == !stored_off && s.wanted == !stored_off);
    } else {
        assert(xiaotai_setting_get_int("wake.enabled", &value) == XIAOTAI_ERR_NOT_FOUND);
    }
    run_owner(disable);
    assert(!status().enabled && !s.wanted);
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    size_t size = sizeof(value);
    const xiaotai_err_t error = xiaotai_kv_get("xt_settings", "wake_on", &value, &size, 3000);
    if (persistent) assert(error == XIAOTAI_OK && size == sizeof(value) && value == 0);
    else assert(error == XIAOTAI_ERR_NOT_FOUND);
    const xiaotai_args_t toggle = {.value = -1};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_WAKE_ENABLE, &toggle) == XIAOTAI_OK);
    barrier();
    assert(status().enabled && s.wanted);
}

static void do_detect(void)
{
    s.hold_error = XIAOTAI_ERR_TIMEOUT;
    run_owner(detect);
    assert(s.last.outcome == XIAOTAI_LOCAL_WAKE_NO_PREROLL && s.last.error == XIAOTAI_ERR_TIMEOUT && s.starts == 0);
    assert(s.last.sequence == 19 && s.last.score_milli == 963 && s.last.threshold_milli == 901);
    s.hold_error = XIAOTAI_OK;
    s.start_error = XIAOTAI_ERR_BUSY;
    run_owner(detect);
    assert(s.last.outcome == XIAOTAI_LOCAL_WAKE_REFUSED && s.last.error == XIAOTAI_ERR_BUSY);
    assert(s.starts == 1 && s.releases == 1 && s.token == 0);
    s.start_error = XIAOTAI_OK;
    run_owner(detect);
    assert(s.last.outcome == XIAOTAI_LOCAL_WAKE_STARTED && s.last.session == s.ai && s.ai);
    assert(s.held.wake_us == 65432109 && s.held.keep_before_ms == 800 && s.held.ttl_ms == 19000 && s.held.lost);
    assert(!s.wanted && s.token == s.handed && s.token && s.releases == 1);
    const xiaotai_local_wake_status_t snapshot = status();
    assert(snapshot.detections == 3 && snapshot.no_preroll == 1 && snapshot.refused == 1 && snapshot.started == 1);
}

static void captured_line(void *ctx, const char *text)
{
    (void)ctx;
    const size_t at = strlen(s.console), n = strlen(text);
    assert(at + n + 2U < sizeof(s.console));
    memcpy(s.console + at, text, n);
    memcpy(s.console + at + n, "\n", 2);
}

static const xiaotai_reply_t reply = {.write = captured_line};

static xiaotai_err_t at(const char *command)
{
    s.console[0] = '\0';
    const xiaotai_err_t result = xiaotai_command_run(command, &reply, 1000);
    barrier();
    return result;
}

static void diag(void *ctx)
{
    (void)ctx;
    xiaotai_diag_report("wake", &reply);
}

static void do_at(void)
{
    assert(at("AT+WAKE") == XIAOTAI_OK && strcmp(s.console, "+WAKE: ready=1\n") == 0);
    s.console[0] = '\0';
    run_owner(diag);
    assert(strncmp(s.console, "+WAKE-MODULE: ", 14) == 0 && !strstr(s.console, "+WAKE:"));
    assert(strstr(s.console, "enabled=1 gate=listening") && strstr(s.console, "last=none"));
    assert(at("AT+WAKE TEST") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+WAKE SELFTEST excess") == XIAOTAI_ERR_INVALID_ARG);
}

static void do_inject(void)
{
    assert(at("AT+WAKE TEST 6F74686572") == XIAOTAI_ERR_NOT_FOUND); /* other */
    assert(at("AT+WAKE TEST 6869xx") == XIAOTAI_ERR_INVALID_ARG);
    assert(at("AT+WAKE TEST 686900") == XIAOTAI_ERR_INVALID_ARG);
    assert(s.starts == 0 && s.events == 0 && status().injected == 0);
    run_owner(occupied);
    assert(at("AT+WAKE TEST 2068656C6C6F") == XIAOTAI_OK); /* leading space + hello */
    assert(strstr(s.console, "outcome=busy") && !s.starts && !s.holds);
    run_owner(free_activity);
    assert(at("AT+WAKE TEST 200968656C6C6F2C20") == XIAOTAI_OK);
    assert(strstr(s.console, "outcome=started") && s.starts == 1 && !s.holds && !s.handed);
    assert(s.last.sequence == 0 && s.last.score_milli == 0 && s.last.threshold_milli == 901);
    assert(status().injected == 2);
}

static void lose(void *ctx)
{
    (void)ctx;
    assert(s.token && s.held.lost);
    s.held.lost(s.token, s.held.ctx);
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "gate";
    const bool persisted = !strcmp(scenario, "persisted");
    xiaotai_local_wake_config_t config = {.user_switch = persisted || !strcmp(scenario, "switch"),
        .announce_only = !strcmp(scenario, "announce")};
    if (!strcmp(scenario, "unavailable")) s.listen_error = XIAOTAI_ERR_NOT_SUPPORTED;
    if (!strcmp(scenario, "selftest-bad")) model.phrases = bad;
    if (!strcmp(scenario, "selftest-empty")) model.phrases = NULL;
    if (!strcmp(scenario, "preroll_lost")) {
        config.keep_before_ms = 600;
        config.preroll_ttl_ms = 5000;
        config.preroll_ms = 8000;
    }
    if (persisted) {
        assert(xiaotai_kv_init() == XIAOTAI_OK);
        const int32_t off = 0;
        assert(xiaotai_kv_set("xt_settings", "wake_on", &off, sizeof(off), 1000) == XIAOTAI_OK);
    }
    assert(xiaotai_local_wake_configure(&config) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_done) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 2};
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    assert(xiaotai_local_wake_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(s.declared == (config.preroll_ms ? config.preroll_ms : 20000));
    if (!strcmp(scenario, "gate")) do_gate();
    else if (!strcmp(scenario, "disabled")) {
        run_owner(disable);
        run_owner(detect);
        assert(!s.wanted && s.last.outcome == XIAOTAI_LOCAL_WAKE_BUSY && !s.holds);
    } else if (!strcmp(scenario, "unavailable")) {
        run_owner(disable);
        run_owner(quiet);
        run_owner(unquiet);
        assert(s.listens == 1 && status().gate == XIAOTAI_LOCAL_WAKE_OFF_UNAVAILABLE && !status().enabled);
    } else if (config.user_switch) do_switch(true, persisted);
    else if (!strcmp(scenario, "detect")) do_detect();
    else if (config.announce_only) {
        run_owner(detect);
        assert(s.last.outcome == XIAOTAI_LOCAL_WAKE_ANNOUNCED && !s.holds && !s.starts && s.wanted);
    } else if (!strcmp(scenario, "inject")) do_inject();
    else if (!strcmp(scenario, "at")) {
        do_at();
        do_switch(false, false);
    } else if (!strcmp(scenario, "preroll_lost")) {
        run_owner(detect);
        const uint32_t handed = s.handed;
        assert(s.ai && !s.wanted && s.held.keep_before_ms == 600 && s.held.ttl_ms == 5000 && handed);
        run_owner(lose);
        assert(s.lost == handed && !s.ai && s.wanted && s.releases == 0);
    } else if (model.phrases == bad) {
        assert(at("AT+WAKE SELFTEST") == XIAOTAI_FAIL && strstr(s.console, "passed=6 total=7"));
        assert(!s.holds && !s.events && !s.starts && !status().injected);
    } else if (!model.phrases) {
        assert(at("AT+WAKE SELFTEST") == XIAOTAI_ERR_NOT_SUPPORTED);
        s.no_model = true;
        assert(at("AT+WAKE TEST 68656C6C6F") == XIAOTAI_ERR_NOT_SUPPORTED && !s.events);
    } else {
        assert(!strcmp(scenario, "selftest") && at("AT+WAKE SELFTEST") == XIAOTAI_OK);
        assert(strstr(s.console, "passed=7 total=7") && !s.holds && !s.events && !s.starts && !status().injected);
    }
    xiaotai_framework_reset();
    xiaotai_local_wake_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&s_done);
    return 0;
}
