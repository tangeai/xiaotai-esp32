// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_audio.py
// Replaces legacy: tests/regression/test_recognition_console.py
// Replaces legacy: tests/regression/test_recognition_conflict.py
// Replaces legacy: tests/regression/test_recognition_decision.py
// Replaces legacy: tests/regression/test_recognition_environment.py
// Replaces legacy: tests/regression/test_recognition_operation.py
// Replaces legacy: tests/regression/test_recognition_runtime.py
#include "operation_fixture.h"
#include "recognition_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_kv_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RC(name) XIAOTAI_RECOGNITION_##name
#define RJ(name) XIAOTAI_RECOGNITION_JOURNAL_##name
#define WAIT 2000U
#define POLICY(rev) "{\"code\":200,\"data\":{\"policy\":{\"enabled\":true,\"allowed\":true,\"revision\":" rev "," \
    "\"monthly_limit\":100,\"schemes\":[{\"scheme_id\":\"detect\",\"scheme_version\":1}]}," \
    "\"effective_enabled\":true,\"upload_limits\":{\"max_file_bytes\":320044,\"max_audio_seconds\":10}," \
    "\"usage\":{\"used\":3,\"input_tokens\":8,\"output_tokens\":9,\"tts_characters\":7,\"unknown_calls\":1}}}"

static size_t free_bytes = 16U * 1024U * 1024U, contiguous = 2U * 1024U * 1024U;
static atomic_bool inject_prepare_timeout;
static atomic_bool inject_disposition_error;
static atomic_bool inject_disposition_timeout;
static atomic_bool inject_at_admission;
static _Thread_local bool fail_at_admission;
static _Thread_local bool watch_rejected_action;
static _Thread_local void *rejected_action;
static _Thread_local unsigned rejected_action_frees;
static atomic_bool inject_archive_ack_timeout;
static atomic_bool inject_archive_before_erase_timeout;
static atomic_bool hold_archive_ack;
static atomic_bool inject_growth_failure;
static _Atomic size_t watch_audio_size;
static _Atomic(void *) audio_buffer;
static xiaotai_signal_t arrived;
static xiaotai_signal_t blocked, release_block;
static xiaotai_signal_t archive_erased, archive_ack;
static void (*owner_fn)(void);
static unsigned decisions, phase_events, progress_events;
static int32_t captured_seconds;
static unsigned adopted_events;
static uint32_t busy_activity;
static const char *selected;
static bool journal_busy;
static unsigned stops_at_activity_end;
static bool transfer_cancelled_at_activity_end;
static atomic_bool scope_write_block, scope_write_fail, scope_write_timeout;
static char at_text[4096];
static size_t at_used;
static const char *at_input = "";

void check(bool value, const char *expression, const char *file, int line)
{
    if (!value) {
        fprintf(stderr, "%s:%d: %s: %s\n", file, line, selected ? selected : "startup", expression);
        exit(1);
    }
}

static _Atomic int64_t offset;
int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return __real_xiaotai_time_us() + atomic_load(&offset); }

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void __real_xiaotai_mem_free(void *memory);
static void *observe_action_allocation(void *memory)
{
    if (watch_rejected_action && memory) {
        CHECK(!rejected_action);
        rejected_action = memory;
    }
    return memory;
}

xiaotai_err_t __real_xiaotai_recognition_archive(const char *request_id, uint64_t revision);
xiaotai_err_t __wrap_xiaotai_recognition_archive(const char *request_id, uint64_t revision)
{
    const bool inject = atomic_exchange(&inject_at_admission, false);
    if (inject) {
        CHECK(!strcmp(request_id, "request") && revision == XIAOTAI_RECOGNITION_REVISION_MAX);
        fail_at_admission = true;
    }
    const xiaotai_err_t error = __real_xiaotai_recognition_archive(request_id, revision);
    CHECK(!fail_at_admission);
    return error;
}

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    if (kind == XIAOTAI_MEM_INTERNAL && fail_at_admission) {
        fail_at_admission = false;
        return NULL;
    }
    return observe_action_allocation(__real_xiaotai_mem_calloc(kind, count, size));
}

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    if (kind == XIAOTAI_MEM_BULK && size == 8192U && atomic_exchange(&inject_growth_failure, false))
        return NULL;
    void *memory = __real_xiaotai_mem_alloc(kind, size);
    if (kind == XIAOTAI_MEM_BULK && size == atomic_load(&watch_audio_size)) {
        atomic_store(&watch_audio_size, 0);
        atomic_store(&audio_buffer, memory);
    }
    return observe_action_allocation(memory);
}

void __wrap_xiaotai_mem_free(void *memory)
{
    if (memory && memory == atomic_load(&audio_buffer)) atomic_store(&audio_buffer, NULL);
    if (memory && memory == rejected_action) {
        rejected_action = NULL;
        ++rejected_action_frees;
    }
    __real_xiaotai_mem_free(memory);
}

void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    CHECK(kind == XIAOTAI_MEM_BULK);
    *stats = (xiaotai_mem_stats_t){.free = free_bytes, .largest_block = contiguous};
}

xiaotai_err_t __real_xiaotai_kv_set_str(const char *space, const char *key, const char *value, uint32_t timeout_ms);
xiaotai_err_t __wrap_xiaotai_kv_set_str(const char *space, const char *key, const char *value, uint32_t timeout_ms)
{
    if (!strcmp(key, "pending") && strstr(value, "\"items\"") && atomic_exchange(&inject_disposition_error, false))
        return XIAOTAI_FAIL;
    if (atomic_exchange(&inject_prepare_timeout, false) ||
        (!strcmp(key, "pending") && strstr(value, "\"items\"") && atomic_exchange(&inject_disposition_timeout, false))) {
        CHECK(strcmp(space, "xt_recognition") == 0 && strcmp(key, "pending") == 0);
        xiaotai_kv_host_gate(false);
        return __real_xiaotai_kv_set_str(space, key, value, 100);
    }
    return __real_xiaotai_kv_set_str(space, key, value, timeout_ms);
}

xiaotai_err_t __real_xiaotai_kv_erase(const char *space, const char *key, uint32_t timeout_ms);
xiaotai_err_t __wrap_xiaotai_kv_erase(const char *space, const char *key, uint32_t timeout_ms)
{
    if (!strcmp(space, "xt_recognition") && !strcmp(key, "pending") &&
        atomic_exchange(&inject_archive_before_erase_timeout, false)) return XIAOTAI_ERR_TIMEOUT;
    const xiaotai_err_t result = __real_xiaotai_kv_erase(space, key, timeout_ms);
    if (!strcmp(space, "xt_recognition") && !strcmp(key, "pending") && result == XIAOTAI_OK) {
        if (atomic_exchange(&hold_archive_ack, false)) {
            xiaotai_signal_give(&archive_erased);
            CHECK(xiaotai_signal_take(&archive_ack, WAIT));
        }
        if (atomic_exchange(&inject_archive_ack_timeout, false)) return XIAOTAI_ERR_TIMEOUT;
    }
    return result;
}

xiaotai_err_t __real_xiaotai_kv_set_int(const char *space, const char *key, xiaotai_kv_int_t kind,
                                      int64_t value, uint32_t timeout_ms);
xiaotai_err_t __wrap_xiaotai_kv_set_int(const char *space, const char *key, xiaotai_kv_int_t kind,
                                      int64_t value, uint32_t timeout_ms);
xiaotai_err_t __wrap_xiaotai_kv_set_int(const char *space, const char *key, xiaotai_kv_int_t kind,
                                      int64_t value, uint32_t timeout_ms)
{
    if (!strcmp(space, "xt_recognition") && !strcmp(key, "scope")) {
        if (atomic_exchange(&scope_write_fail, false)) return XIAOTAI_FAIL;
        if (atomic_exchange(&scope_write_block, false)) xiaotai_kv_host_gate(false);
        if (atomic_exchange(&scope_write_timeout, false)) timeout_ms = 70U;
    }
    return __real_xiaotai_kv_set_int(space, key, kind, value, timeout_ms);
}

static size_t serial_read(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    size_t count = strlen(at_input);
    if (count > capacity) count = capacity;
    memcpy(data, at_input, count);
    at_input += count;
    return count;
}

static xiaotai_err_t serial_write(void *ctx, const char *text, size_t count)
{
    (void)ctx;
    CHECK(at_used + count < sizeof(at_text));
    memcpy(at_text + at_used, text, count);
    at_text[at_used += count] = '\0';
    return XIAOTAI_OK;
}

static const xiaotai_at_io_t serial = {.name = "fixture", .read = serial_read, .write = serial_write};

static void owner_task(void *ignored)
{
    (void)ignored;
    if (owner_fn) owner_fn();
    journal_busy = xiaotai_recognition_journal_busy();
    xiaotai_signal_give(&arrived);
}

static void on_owner(void (*fn)(void))
{
    owner_fn = fn;
    CHECK(xiaotai_run(owner_task, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&arrived, WAIT));
}

static xiaotai_recognition_status_t status(void)
{
    xiaotai_recognition_status_t s;
    for (unsigned i = 0; i < 100; ++i) {
        const xiaotai_err_t error = xiaotai_recognition_status(&s);
        if (error == XIAOTAI_OK) return s;
        CHECK(error == XIAOTAI_ERR_TIMEOUT);
        xiaotai_delay_ms(1);
    }
    CHECK(false);
    return (xiaotai_recognition_status_t){0};
}

static xiaotai_recognition_status_t until(xiaotai_recognition_phase_t phase, bool pending)
{
    for (unsigned i = 0; i < 1000; ++i) {
        on_owner(NULL);
        const xiaotai_recognition_status_t s = status();
        if (s.phase == phase && s.cleanup_pending == pending) return s;
        xiaotai_delay_ms(1);
    }
    const xiaotai_recognition_status_t s = status();
    fprintf(stderr, "phase=%d pending=%d token=%u error=%d http=%d code=%d\n",
            s.phase, s.cleanup_pending, s.token, s.error, s.http_status, (int)s.code);
    CHECK(false);
    return s;
}

static void event(const xiaotai_event_t *received, void *ctx)
{
    (void)ctx;
    if (!strcmp(received->name, XIAOTAI_EVENT_RECOGNITION_DECISION)) {
        CHECK(received->size == sizeof(xiaotai_recognition_receipt_t));
        const xiaotai_recognition_receipt_t *receipt = received->data;
        CHECK(receipt->serial && received->args.value == receipt->error);
        ++decisions;
    } else if (!strcmp(received->name, XIAOTAI_EVENT_RECOGNITION_RESULT)) {
        const bool two_items = !strcmp(selected, "playback-order") || !strncmp(selected, "playback-dismiss-", 17U);
        CHECK(received->args.value == (two_items ? 2 : 1));
        ++adopted_events;
    } else if (!strcmp(received->name, XIAOTAI_EVENT_RECOGNITION_PROGRESS)) {
        captured_seconds = received->args.value;
        ++progress_events;
    } else {
        ++phase_events;
    }
}

static void activity_ended(uint32_t id, uint32_t generation, void *ctx)
{
    (void)id;
    (void)generation;
    (void)ctx;
    stops_at_activity_end = fake_stops;
    transfer_cancelled_at_activity_end = fake_transfer_cancelled;
}

static xiaotai_err_t init(void)
{
    const bool download = !strncmp(selected, "audio-", 6U);
    const bool playback = !strncmp(selected, "playback-", 9U) ||
                          (!strncmp(selected, "decision-", 9U) && strcmp(selected, "decision-reused-unknown")) ||
                          !strcmp(selected, "result-views");
    if (download) {
        CHECK(xiaotai_recognition_configure(&(xiaotai_recognition_config_t){.autoplay = true}) == XIAOTAI_OK);
        fake_speaker_muted = true; /* run the isolated transfer contract explicitly */
    }
    if (!strcmp(selected, "playback-auto"))
        CHECK(xiaotai_recognition_configure(&(xiaotai_recognition_config_t){.autoplay = true}) == XIAOTAI_OK);
    if (!strncmp(selected, "playback-dismiss-", 17U))
        CHECK(xiaotai_recognition_configure(&(xiaotai_recognition_config_t){.resume_after_dismiss = true}) == XIAOTAI_OK);
    if (!strcmp(selected, "corrupt"))
        CHECK(xiaotai_kv_set_str("xt_recognition", "pending", "{bad}", WAIT) == XIAOTAI_OK);
    else if (download || playback || !strcmp(selected, "stored") || !strcmp(selected, "foreign") ||
             !strncmp(selected, "guards-retired-", 15U) ||
             (!strncmp(selected, "query-", 6) && strcmp(selected, "query-empty") &&
              strcmp(selected, "query-upload-notice") && strcmp(selected, "query-capture-notice")) ||
             !strcmp(selected, "legacy-conflict-3")) {
        const char *device = !strcmp(selected, "foreign") || !strcmp(selected, "query-foreign") ? "other" : "dev";
        const bool conflicted = !strcmp(selected, "query-conflict") || !strcmp(selected, "decision-conflict") ||
                                !strcmp(selected, "legacy-conflict-3");
        char pending[192];
        const int n = snprintf(pending, sizeof(pending), "{\"version\":%d,\"device_id\":\"%s\","
              "\"request_id\":\"request\",\"revision\":1,\"seconds\":1%s}",
              conflicted ? 4 : 1, device, conflicted ? ",\"scope\":0" : "");
        CHECK(n > 0 && (size_t)n < sizeof(pending));
        CHECK(xiaotai_kv_set_str("xt_recognition", "pending", pending, WAIT) == XIAOTAI_OK);
    }
    if (!strcmp(selected, "at")) {
        CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
        CHECK(xiaotai_at_configure(&(xiaotai_at_config_t){.task_stack_bytes = XIAOTAI_AT_NO_TASK,
                                                          .no_platform_console = true}) == XIAOTAI_OK);
        CHECK(xiaotai_at_attach(&serial) == XIAOTAI_OK);
        CHECK(xiaotai_engine_at_init() == XIAOTAI_OK);
    }
    xiaotai_err_t error = xiaotai_module_recognition_init();
    const xiaotai_attachment_t attachment = {.end = activity_ended};
    if (error == XIAOTAI_OK) error = xiaotai_activity_attach(XIAOTAI_RECOGNITION_ACTIVITY, &attachment);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_RECOGNITION_DECISION, event, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_RECOGNITION_STATE, event, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_RECOGNITION_PROGRESS, event, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_event_subscribe(XIAOTAI_EVENT_RECOGNITION_RESULT, event, NULL);
    return error;
}

static const xiaotai_priority_t priorities[] = {{"call", 20}, {XIAOTAI_RECOGNITION_ACTIVITY, 10}, {0}};
static const xiaotai_module_t module = {.name = "recognition", .init = init, .start = xiaotai_module_recognition_start,
                                       .activities = priorities};
static const xiaotai_module_t *const modules[] = {&module};

static void absent(void) { CHECK(xiaotai_recognition_journal_mirror()->state == RJ(ABSENT)); }
static void present(void) { CHECK(xiaotai_recognition_journal_mirror()->state == RJ(PRESENT)); }
static void conflict(void)
{
    present();
    CHECK(xiaotai_recognition_journal_mirror()->version == 4);
}

static void await_journal(void)
{
    for (unsigned i = 0; i < 1000; ++i) {
        on_owner(NULL);
        if (!journal_busy) return;
        xiaotai_delay_ms(1);
    }
    CHECK(false);
}

static void policy_one(void) { fake_policy_reply(POLICY("1"), 200, 200, XIAOTAI_OK); }
static void policy_two(void) { fake_policy_reply(POLICY("2"), 200, 200, XIAOTAI_OK); }
static void record_full(void) { fake_record_done(XIAOTAI_OK, 16000); }
static void record_short(void) { fake_record_done(XIAOTAI_OK, 15999); }
static void record_gap(void) { fake_record_done(XIAOTAI_FAIL, 512); }

static void start_config(void)
{
    CHECK(xiaotai_recognition_record(1) == XIAOTAI_OK);
    const xiaotai_recognition_status_t s = until(RC(CONFIG), true);
    CHECK(s.token && s.target_samples == 16000 && fake_gets == 1);
}

static void start_recording(void)
{
    start_config();
    on_owner(policy_one);
    const xiaotai_recognition_status_t s = until(RC(RECORDING), true);
    CHECK(s.config_revision == 1 && s.config_enabled && fake_capture_request.done);
}

static void start_recheck(void)
{
    start_recording();
    on_owner(record_full);
    const xiaotai_recognition_status_t s = until(RC(UPLOAD), true);
    CHECK(s.samples == 16000 && fake_gets == 2 && fake_posts == 0 && !fake_capture_request.done);
    CHECK(strlen(s.request_id) == 32 && strspn(s.request_id, "0123456789abcdef") == 32);
    on_owner(present);
}

static void start_upload(void)
{
    start_recheck();
    on_owner(policy_one);
    on_owner(NULL);
    CHECK(fake_posts == 1 && fake_stream.done && status().cleanup_pending);
}

static void busy_begin(void)
{
    const xiaotai_activity_t activity = {.name = "call"};
    CHECK(xiaotai_activity_begin(&activity, &busy_activity) == XIAOTAI_OK);
}

static void busy_end(void) { xiaotai_activity_end(busy_activity); }

static void denied(xiaotai_err_t error)
{
    const xiaotai_recognition_status_t old = status();
    CHECK(xiaotai_recognition_record(1) == XIAOTAI_OK);
    on_owner(NULL);
    const xiaotai_recognition_status_t s = status();
    CHECK(s.decision_serial == old.decision_serial + 1 && s.decision_error == error && s.phase == old.phase);
}

static void admission(void)
{
    CHECK(xiaotai_recognition_record(0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_recognition_record(61) == XIAOTAI_ERR_INVALID_ARG);
    fake_online = false;
    denied(XIAOTAI_ERR_INVALID_STATE);
    fake_online = true;
    fake_muted = true;
    denied(XIAOTAI_ERR_INVALID_STATE);
    fake_muted = false;
    fake_capture = false;
    denied(XIAOTAI_ERR_NOT_SUPPORTED);
    fake_capture = true;
    on_owner(busy_begin);
    denied(XIAOTAI_ERR_INVALID_STATE);
    on_owner(busy_end);
    fake_endpoint_error = XIAOTAI_ERR_NOT_SUPPORTED;
    denied(XIAOTAI_ERR_NOT_SUPPORTED);
    fake_endpoint_error = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
    denied(RC_INVALID_RESPONSE);
    fake_endpoint_error = XIAOTAI_OK;
    fake_address = "https://rec.invalid";
    denied(RC_INVALID_RESPONSE);
    on_owner(NULL);
    CHECK(!fake_gets && !fake_posts && !phase_events && decisions == 7);
}

static void memory(void)
{
    if (!strcmp(selected, "fragmented")) contiguous = 31999;
    else free_bytes = 262144U + 31999U;
    start_config();
    on_owner(policy_one);
    CHECK(until(RC(FAILED), false).error == XIAOTAI_ERR_NO_MEM && !fake_capture_request.done);
}

static void recording_failure(void)
{
    start_recording();
    fake_recorded = 500;
    CHECK(status().samples == 500);
    on_owner(!strcmp(selected, "gap") ? record_gap : record_short);
    CHECK(until(RC(FAILED), false).error == XIAOTAI_FAIL && fake_gets == 1 && !fake_posts);
    on_owner(absent);
}

static void received_progress(void)
{
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    CHECK(media.recording && fake_capture_request.progress);
    fake_capture_request.progress(media.recording + 1U, 16000U, fake_capture_request.ctx);
    fake_recorded = 16000U;
    fake_capture_request.progress(media.recording, fake_recorded, fake_capture_request.ctx);
    fake_capture_request.progress(media.recording, fake_recorded, fake_capture_request.ctx);
}

static void recording_progress_case(void)
{
    CHECK(xiaotai_recognition_record(3U) == XIAOTAI_OK);
    CHECK(until(RC(CONFIG), true).target_samples == 48000U);
    on_owner(policy_one);
    CHECK(until(RC(RECORDING), true).capture_rate_hz == 16000U && progress_events == 0U);
    on_owner(received_progress);
    on_owner(NULL);
    CHECK(progress_events == 1U && captured_seconds == 1 && status().samples == 16000U);
    CHECK(xiaotai_recognition_cancel(status().token) == XIAOTAI_OK);
    CHECK(until(RC(CANCELLED), false).samples == 16000U && progress_events == 1U);
}

static void advance(uint32_t ms)
{
    atomic_fetch_add(&offset, (int64_t)ms * 1000);
    on_owner(NULL);
}

static void silent_get(void) { fake_policy_reply(NULL, 0, XIAOTAI_PLATFORM_NO_CODE, XIAOTAI_ERR_TIMEOUT); }

static void config_timeout(void)
{
    start_config();
    advance(20000);
    CHECK(until(RC(CANCELLING), true).error == XIAOTAI_ERR_TIMEOUT);
    denied(XIAOTAI_ERR_INVALID_STATE);
    on_owner(silent_get);
    CHECK(until(RC(CANCELLED), false).error == XIAOTAI_ERR_TIMEOUT);
    CHECK(!fake_capture_request.done && !fake_posts);
}

static void expect_environment_guard(xiaotai_err_t expected);

static void capture_cancelled(void)
{
    start_recording();
    expect_environment_guard(XIAOTAI_ERR_INVALID_STATE);
    fake_stop_holds = true;
    const uint32_t token = status().token;
    CHECK(xiaotai_recognition_cancel(token + 1U) == XIAOTAI_OK);
    on_owner(NULL);
    CHECK(status().decision_error == XIAOTAI_ERR_INVALID_STATE && !fake_stops);
    const bool timeout = !strcmp(selected, "capture-timeout");
    const bool revoke = !strcmp(selected, "revoke");
    if (timeout) advance(3000);
    else if (revoke) on_owner(busy_begin);
    else CHECK(xiaotai_recognition_cancel(0) == XIAOTAI_OK);
    const xiaotai_err_t expected = timeout ? XIAOTAI_ERR_TIMEOUT : revoke ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
    CHECK(until(RC(CANCELLING), true).error == expected && fake_stops == 1);
    expect_environment_guard(XIAOTAI_ERR_INVALID_STATE);
    /* Preemption delivers framework end before the module's revoked callback. */
    CHECK(stops_at_activity_end == (revoke ? 0U : 1U));
    for (unsigned i = 0; i < 3; ++i) {
        advance(150);
        on_owner(NULL);
    }
    CHECK(until(RC(CANCELLING), true).error == expected);
    CHECK(fake_stops == 3);
    denied(XIAOTAI_ERR_INVALID_STATE);
    if (timeout || revoke) on_owner(record_short);
    else {
        fake_stop_holds = false;
        CHECK(xiaotai_recognition_cancel(token) == XIAOTAI_OK);
    }
    CHECK(until(RC(CANCELLED), false).error == expected && !fake_posts);
    CHECK(fake_stops == (timeout || revoke ? 3U : 4U));
    on_owner(absent);
    if (revoke) on_owner(busy_end);
}

static void prepare_failure(void)
{
    start_recording();
    const bool timeout = !strcmp(selected, "prepare-timeout");
    if (timeout) atomic_store(&inject_prepare_timeout, true);
    else xiaotai_kv_host_fail_key("pending", XIAOTAI_FAIL);
    on_owner(record_full);
    if (timeout) CHECK(xiaotai_kv_host_wait_held(WAIT));
    const xiaotai_recognition_status_t s = until(RC(RECOVERY), false);
    CHECK(s.error == (timeout ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL) && strlen(s.request_id) == 32);
    CHECK(fake_gets == 1 && !fake_posts);
    if (timeout) {
        xiaotai_kv_host_gate(true);
        char raw[2048];
        CHECK(xiaotai_kv_get_str("xt_recognition", "pending", raw, sizeof(raw), WAIT) == XIAOTAI_OK);
        CHECK(strstr(raw, s.request_id) != NULL);
    } else {
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    }
}

static void prepare_offline(void)
{
    start_recording();
    xiaotai_kv_host_gate(false);
    on_owner(record_full);
    CHECK(xiaotai_kv_host_wait_held(WAIT) && status().cleanup_pending);
    fake_online = false;
    xiaotai_kv_host_gate(true);
    CHECK(until(RC(FAILED), false).error == XIAOTAI_ERR_INVALID_STATE);
    on_owner(absent);
    CHECK(fake_gets == 1 && !fake_posts);
}

static void recheck_failure(void)
{
    start_recheck();
    if (!strcmp(selected, "recheck")) {
        on_owner(policy_two);
        CHECK(until(RC(FAILED), false).error == RC_INVALID_RESPONSE && status().config_revision == 2);
    } else if (!strcmp(selected, "enqueue")) {
        fake_transfer_busy = true;
        on_owner(policy_one);
        CHECK(until(RC(FAILED), false).error == XIAOTAI_ERR_NO_MEM);
    } else {
        CHECK(xiaotai_recognition_cancel(status().token) == XIAOTAI_OK);
        (void)until(RC(CANCELLING), true);
        on_owner(policy_one);
        CHECK(until(RC(CANCELLED), false).error == XIAOTAI_OK);
    }
    CHECK(!fake_posts && !status().request_id[0]);
    on_owner(absent);
}

static void not_sent(void) { fake_upload_done(NULL, 0, false, XIAOTAI_FAIL); }
static void sent_but_uncertain(void) { fake_upload_done(NULL, 0, true, XIAOTAI_ERR_TIMEOUT); }
static void sent_conflict(void) { fake_upload_done("{\"code\":40901}", 409, true, XIAOTAI_OK); }
static void sent_unbound(void) { fake_upload_done("{\"code\":6006}", 410, true, XIAOTAI_OK); }
static void sent_other(void) { fake_upload_done("{\"code\":6010}", 410, true, XIAOTAI_OK); }
static void send_cancelled_before_wire(void)
{
    fake_upload_done(NULL, 0, false, XIAOTAI_ERR_PLATFORM_CANCELLED);
}
static void policy_unbound(void) { fake_policy_reply("{\"code\":6006}", 410, 6006, XIAOTAI_OK); }

static void accepted(void)
{
    const xiaotai_recognition_status_t s = status();
    char response[256];
    int n = snprintf(response, sizeof(response), "{\"code\":200,\"data\":{\"request_id\":\"%s\","
        "\"device_id\":\"dev\",\"status_revision\":1,\"status\":\"queued\",\"results\":[]}}", s.request_id);
    CHECK(n > 0 && (size_t)n < sizeof(response));
    fake_upload_done(response, 202, true, XIAOTAI_OK);
}

static void check_upload_stream(void)
{
    uint8_t *wire = malloc(fake_body_size + 1U);
    CHECK(wire);
    size_t total = 0;
    while (total < fake_body_size) {
        uint8_t piece[1200];
        size_t bytes;
        CHECK(fake_stream.read(fake_stream.ctx, piece, sizeof(piece), &bytes) == XIAOTAI_OK);
        CHECK(bytes && bytes <= 1024 && bytes <= fake_body_size - total);
        memcpy(wire + total, piece, bytes);
        total += bytes;
    }
    wire[total] = 0;
    CHECK(strstr((const char *)wire, "name=\"request_id\"") != NULL);
    CHECK(strstr((const char *)wire, "name=\"files\"; filename=\"recording.pcm\"") != NULL);
    free(wire);
}

static void transfer_result(void)
{
    start_upload();
    check_upload_stream();
    if (!strcmp(selected, "accepted")) {
        on_owner(accepted);
        CHECK(until(RC(ACCEPTED), false).possibly_sent && status().http_status == 202);
        on_owner(present);
        denied(XIAOTAI_ERR_INVALID_STATE);
    } else if (!strcmp(selected, "not-sent") || !strcmp(selected, "clear-failure")) {
        const bool clear_fail = !strcmp(selected, "clear-failure");
        if (clear_fail) xiaotai_kv_host_fail(XIAOTAI_ERR_TIMEOUT);
        on_owner(not_sent);
        CHECK(until(clear_fail ? RC(RECOVERY) : RC(FAILED), false).error ==
              (clear_fail ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL));
        if (!clear_fail) on_owner(absent);
        else xiaotai_kv_host_fail(XIAOTAI_OK);
    } else if (!strcmp(selected, "conflict")) {
        on_owner(sent_conflict);
        const xiaotai_recognition_status_t s = until(RC(RECOVERY), false);
        CHECK(s.possibly_sent && s.http_status == 409 && s.code == 40901 && !s.rejected_without_task);
        on_owner(conflict);
    } else if (!strcmp(selected, "transfer-unbound")) {
        on_owner(sent_unbound);
        const xiaotai_recognition_status_t s = until(RC(RECOVERY), false);
        CHECK(s.http_status == 410 && s.code == 6006 && fake_unbound == 1);
        on_owner(present);
    } else if (!strcmp(selected, "transfer-other-code")) {
        on_owner(sent_other);
        const xiaotai_recognition_status_t s = until(RC(RECOVERY), false);
        CHECK(s.http_status == 410 && s.code == 6010 && fake_unbound == 0);
        on_owner(present);
    } else if (!strcmp(selected, "uncertain")) {
        on_owner(sent_but_uncertain);
        CHECK(until(RC(RECOVERY), false).possibly_sent && status().error == XIAOTAI_ERR_TIMEOUT);
        on_owner(present);
    } else {
        const bool timeout = !strcmp(selected, "upload-timeout");
        if (timeout) advance(90000);
        else CHECK(xiaotai_recognition_cancel(status().token) == XIAOTAI_OK);
        CHECK(until(RC(CANCELLING), true).error == (timeout ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK));
        CHECK(fake_transfer_cancelled && transfer_cancelled_at_activity_end);
        uint8_t buffer[4];
        size_t bytes = 99;
        CHECK(fake_stream.read(fake_stream.ctx, buffer, sizeof(buffer), &bytes) == XIAOTAI_ERR_PLATFORM_CANCELLED);
        CHECK(bytes == 0);
        denied(XIAOTAI_ERR_INVALID_STATE);
        if (!strcmp(selected, "upload-unsent-cancel")) {
            on_owner(send_cancelled_before_wire);
            CHECK(!until(RC(CANCELLED), false).possibly_sent && !status().request_id[0]);
            on_owner(absent);
        } else {
            on_owner(sent_but_uncertain);
            CHECK(until(RC(RECOVERY), false).possibly_sent);
            on_owner(present);
        }
    }
    CHECK(fake_posts == 1 && fake_gets == 2);
}

static void binding_changed(void) { ++fake_generation; fake_online = false; }

static void stale_generation(void)
{
    start_upload();
    on_owner(binding_changed);
    CHECK(status().request_id[0] == '\0');
    on_owner(accepted);
    const xiaotai_recognition_status_t s = until(RC(RECOVERY), false);
    CHECK(s.request_id[0] == '\0' && s.http_status == 0 && s.error == XIAOTAI_ERR_INVALID_STATE);
    on_owner(present);
}

static void config_unbound(void)
{
    start_config();
    fake_unbind_in_hook = true;
    on_owner(policy_unbound);
    const xiaotai_recognition_status_t s = until(RC(RECOVERY), false);
    CHECK(s.http_status == 410 && s.code == 6006 && fake_unbound == 0 && fake_generation == 2);
    CHECK(fake_posts == 0 && s.request_id[0] == '\0');
}

static void fail_boot_read(void)
{
    const xiaotai_recognition_status_t s = until(RC(RECOVERY), false);
    CHECK(s.error != XIAOTAI_OK);
    CHECK(strcmp(s.request_id, !strcmp(selected, "stored") ? "request" : "") == 0);
    expect_environment_guard(s.error);
    denied(s.error);
    CHECK(fake_gets == 0 && fake_posts == 0);
    await_journal();
}

static void hold_owner(void *unused)
{
    (void)unused;
    xiaotai_signal_give(&blocked);
    CHECK(xiaotai_signal_take(&release_block, WAIT));
}

static void stale_action(void)
{
    CHECK(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&release_block) == XIAOTAI_OK);
    CHECK(xiaotai_run(hold_owner, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&blocked, WAIT));
    CHECK(xiaotai_recognition_record(1) == XIAOTAI_OK);
    /* Keep the replacement session offline: its legitimate automatic
     * refresh would otherwise race the stale action assertion. */
    fake_online = false;
    ++fake_generation;
    xiaotai_signal_give(&release_block);
    on_owner(NULL);
    CHECK(status().phase == RC(IDLE) && status().decision_error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!fake_gets && !fake_posts);
    xiaotai_signal_deinit(&blocked);
    xiaotai_signal_deinit(&release_block);
}

static void count_queued_job(void *ctx)
{
    atomic_fetch_add((atomic_uint *)ctx, 1);
}

static void full_owner_rejects_actions(void)
{
    CHECK(xiaotai_signal_init(&blocked) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&release_block) == XIAOTAI_OK);
    CHECK(xiaotai_run(hold_owner, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&blocked, WAIT));
    const uint32_t serial = status().decision_serial;
    const unsigned gets = fake_gets, posts = fake_posts, queries = fake_queries;
    atomic_uint drained = 0;
    unsigned queued = 0;
    xiaotai_err_t fill;
    while ((fill = xiaotai_run(count_queued_job, &drained)) == XIAOTAI_OK) CHECK(++queued < 256U);
    CHECK(fill == XIAOTAI_ERR_NO_MEM && queued > 0);

    watch_rejected_action = true;
    CHECK(xiaotai_recognition_record(1) == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_recognition_query() == XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_recognition_dismiss("request", "item_1") == XIAOTAI_ERR_NO_MEM);
    CHECK(!rejected_action && rejected_action_frees == 3);
    watch_rejected_action = false;
    xiaotai_signal_give(&release_block);
    for (unsigned i = 0; i < WAIT && atomic_load(&drained) != queued; ++i) xiaotai_delay_ms(1);
    CHECK(atomic_load(&drained) == queued);
    on_owner(NULL);
    CHECK(status().decision_serial == serial && fake_gets == gets && fake_posts == posts && fake_queries == queries);
    CHECK(!xiaotai_activity_active() && !fake_capture_request.done);

    /* Explicitly retry the disposition after the queue drains. It now reaches the owner. */
    CHECK(xiaotai_recognition_dismiss("request", "item_1") == XIAOTAI_OK);
    on_owner(NULL);
    CHECK(status().decision_serial == serial + 1 && status().decision_action == RC(ACTION_DISMISS) &&
          status().decision_error == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_deinit(&blocked);
    xiaotai_signal_deinit(&release_block);
    printf("owner queue full at %u jobs: three actions rejected, three allocations returned, no work admitted\n", queued);
}

static void observe_query(void)
{
    CHECK(!xiaotai_activity_active() && !fake_capture_request.done);
    CHECK(!strcmp(fake_query_path, "/v1/recognition/requests/request"));
}

static void wait_query(unsigned count)
{
    for (unsigned i = 0; i < 1500; ++i) {
        on_owner(NULL);
        if (fake_queries == count && fake_stream.done) return;
        xiaotai_delay_ms(1);
    }
    CHECK(false);
}

static void start_query(unsigned count)
{
    CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
    wait_query(count);
    on_owner(observe_query);
    CHECK(status().decision_action == RC(ACTION_QUERY) && status().decision_error == XIAOTAI_OK);
}

static void wait_refresh(unsigned count)
{
    for (unsigned i = 0; i < 1500; ++i) {
        on_owner(NULL);
        if (fake_gets == count && status().phase == RC(REFRESH)) return;
        xiaotai_delay_ms(1);
    }
    CHECK(false);
}

static const char *request_identity = "request";

static size_t task_json(char *body, size_t capacity, const char *device, const char *request,
                        unsigned revision, const char *state)
{
    const int len = snprintf(body, capacity,
        "{\"code\":200,\"data\":{\"request_id\":\"%s\",\"device_id\":\"%s\",\"status_revision\":%u,"
        "\"status\":\"%s\",\"error\":null,\"results\":[{\"item_id\":\"item_1\",\"order\":1,"
        "\"scheme_id\":\"fixture\",\"scheme_version\":1,\"output_mode\":\"text\",\"status\":\"queued\","
        "\"result\":null,\"error\":null,\"speech\":{\"status\":\"skipped\",\"error\":null}}]}}",
        request, device, revision, state);
    CHECK(len > 0 && (size_t)len < capacity);
    return (size_t)len;
}

static void deliver_task(unsigned revision, const char *state, const char *device, const char *request, uint32_t generation)
{
    char body[512], envelope[640];
    task_json(body, sizeof(body), device, request, revision, state);
    const int n = snprintf(envelope, sizeof(envelope),
        "{\"type\":\"recognition_result\",\"channel\":\"recognition\",\"event_id\":\"f1\",\"payload\":%s}", body);
    CHECK(n > 0 && (size_t)n < sizeof(envelope));
    fake_notice("recognition_result", envelope, generation);
}

static void new_notice(void) { deliver_task(3U, "succeeded", "dev", request_identity, fake_generation); }
static void pending_notice(void) { deliver_task(3U, "processing", "dev", request_identity, fake_generation); }
static void other_device_notice(void) { deliver_task(99U, "succeeded", "other", request_identity, fake_generation); }
static void other_request_notice(void) { deliver_task(99U, "succeeded", "dev", "other_request", fake_generation); }
static void old_binding_notice(void) { deliver_task(99U, "succeeded", "dev", request_identity, fake_generation - 1U); }
static void legacy_notices(void)
{
    xiaotai_recognition_stats_t before, after;
    xiaotai_recognition_stats(&before);
    const xiaotai_recognition_status_t saved = status();
    CHECK(fake_notice_channel("recognition_result", "", "{\"type\":\"recognition_result\"}", fake_generation));
    CHECK(fake_notice_channel("recognition_config_changed", "", "{\"type\":\"recognition_config_changed\"}", fake_generation));
    CHECK(!fake_notice_channel("recognition_result", "wx", "{\"type\":\"recognition_result\",\"channel\":\"wx\"}",
                               fake_generation));
    CHECK(fake_notice_channel("recognition_result", "", "{\"type\":\"recognition_result\",\"channel\":null}", fake_generation));
    CHECK(fake_notice_channel("recognition_result", "", "{\"type\":\"recognition_result\",\"channel\":\"\"}", fake_generation));
    CHECK(fake_notice_channel("recognition_result", "", "{\"type\":\"recognition_result\"} junk", fake_generation));
    xiaotai_recognition_stats(&after);
    CHECK(after.notifications == before.notifications + 2U && after.notifications_ignored == before.notifications_ignored + 2U);
    CHECK(saved.config_revision == status().config_revision && saved.task_revision == status().task_revision);
}

static void policy_notice(void)
{
    fake_notice("recognition_config_changed",
        "{\"type\":\"recognition_config_changed\",\"channel\":\"recognition\",\"event_id\":\"f2\","
        "\"payload\":{\"code\":200,\"data\":{\"device_id\":\"dev\",\"revision\":2}}}", fake_generation);
}

static void processing_query(void)
{
    char body[512];
    const size_t len = task_json(body, sizeof(body), "dev", "request", 2U, "processing");
    fake_query_done(body, len, (int64_t)len, 11U, 200, XIAOTAI_OK);
}

static void completed_query(void)
{
    char body[512];
    const size_t len = task_json(body, sizeof(body), "dev", "request", 3U, "succeeded");
    fake_query_done(body, len, (int64_t)len, 11U, 200, XIAOTAI_OK);
}

static void audio_task_query(void)
{
    const char payload[] = "{\"code\":200,\"data\":{\"request_id\":\"request\",\"device_id\":\"dev\","
        "\"status_revision\":1,\"status\":\"succeeded\",\"error\":null,\"results\":[{\"item_id\":\"item_1\","
        "\"order\":1,\"scheme_id\":\"fixture\",\"scheme_version\":1,\"output_mode\":\"audio\",\"status\":\"succeeded\","
        "\"result\":null,\"error\":null,\"speech\":{\"status\":\"ready\",\"error\":null,"
        "\"url\":\"/v1/recognition/requests/request/audio/item_1\",\"format\":\"wav\"}}]}}";
    fake_query_done(payload, sizeof(payload) - 1U, -1, 5U, 200, XIAOTAI_OK);
}

static void chunked_query(void)
{
    const size_t size = RC_TASK_BYTES;
    char *body = malloc(size);
    CHECK(body);
    const size_t len = task_json(body, size, "dev", "request", 2U, "succeeded");
    memset(body + len, ' ', size - len);
    fake_query_done(body, size, -1, 2048U, 200, XIAOTAI_OK);
    free(body);
}

static void oversized_query(void) { fake_query_done(NULL, 0, RC_TASK_BYTES + 1U, 0, 200, XIAOTAI_OK); }
static void binary_query(void)
{
    static const char body[] = "{}\0{}";
    fake_query_done(body, sizeof(body) - 1U, -1, 2U, 200, XIAOTAI_OK);
}

static uint32_t quiet;
static void quiet_begin(void) { CHECK(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_BINDING, &quiet) == XIAOTAI_OK); }
static void quiet_end(void) { xiaotai_quiesce_end(quiet); }

static void query_code(int http, const char *body) { fake_query_done(body, strlen(body), -1, 3U, http, XIAOTAI_OK); }
static void query_6006(void) { query_code(410, "{\"code\":6006}"); }
static void query_6010(void) { query_code(410, "{\"code\":6010}"); }
static void query_40400(void) { query_code(404, "{\"code\":40400}"); }
static void query_40401(void) { query_code(404, "{\"code\":40401}"); }
static void query_500(void) { query_code(503, "{\"code\":50001}"); }
static void policy_404(void) { fake_policy_reply("{\"code\":40401}", 404, 40401, XIAOTAI_OK); }
static void query_cancelled(void) { fake_query_done(NULL, 0, -1, 0, 0, XIAOTAI_ERR_PLATFORM_CANCELLED); }
static void notify_dropped(void) { ++fake_notice_revision; }
static void transfer_unavailable(void) { fake_transfer_busy = true; }
static void transfer_available(void) { fake_transfer_busy = false; }

static uint32_t filler_timers[XIAOTAI_FRAMEWORK_MAX_TIMERS];
static size_t filler_count;

static void filler_timeout(void *unused) { (void)unused; CHECK(false); }

static void fill_timers(void)
{
    xiaotai_err_t error = XIAOTAI_OK;
    while (filler_count < XIAOTAI_FRAMEWORK_MAX_TIMERS && error == XIAOTAI_OK) {
        error = xiaotai_timer_start(600000U, filler_timeout, NULL, &filler_timers[filler_count]);
        if (error == XIAOTAI_OK) ++filler_count;
    }
    CHECK(error == XIAOTAI_ERR_NO_MEM && filler_count == XIAOTAI_FRAMEWORK_MAX_TIMERS - 1U);
}

static void free_timers(void)
{
    while (filler_count) xiaotai_timer_cancel(filler_timers[--filler_count]);
}

static void check_no_more_query(unsigned count)
{
    advance(30000);
    xiaotai_delay_ms(300U);
    on_owner(NULL);
    CHECK(fake_queries == count && !fake_stream.done);
}

static void query_case(void)
{
    if (!strcmp(selected, "query-adopt")) {
        start_query(1U);
        on_owner(processing_query);
        CHECK(until(RC(RESULT), false).task_revision == 2);
        advance(5100);
        wait_query(2U);
        on_owner(other_device_notice);
        on_owner(other_request_notice);
        on_owner(old_binding_notice);
        CHECK(status().task_revision == 2 && status().phase == RC(QUERY));
        on_owner(new_notice);
        CHECK(status().task_revision == 3 && status().phase == RC(QUERY));
        on_owner(processing_query); /* Older HTTP must not replace the newer notification. */
        CHECK(until(RC(RESULT), false).task_revision == 3 && status().task_state == RC(TASK_SUCCEEDED));
        check_no_more_query(2U);
        xiaotai_recognition_stats_t stats;
        xiaotai_recognition_stats(&stats);
        CHECK(adopted_events == 2U && stats.notifications_ignored == 3U && stats.results_stale == 1U);
        on_owner(binding_changed);
        CHECK(!status().task_revision && !status().request_id[0]);
    } else if (!strcmp(selected, "query-stream")) {
        start_query(1U);
        on_owner(oversized_query);
        CHECK(until(RC(RECOVERY), false).error == XIAOTAI_ERR_INVALID_SIZE);
        start_query(2U);
        on_owner(binary_query);
        CHECK(until(RC(RECOVERY), false).error == RC_INVALID_RESPONSE);
        start_query(3U);
        atomic_store(&inject_growth_failure, true);
        on_owner(chunked_query);
        CHECK(until(RC(RECOVERY), false).error == XIAOTAI_ERR_NO_MEM);
        advance(5100);
        wait_query(4U);
        on_owner(chunked_query);
        CHECK(until(RC(RESULT), false).task_revision == 2);
        on_owner(present);
    } else if (!strcmp(selected, "query-http")) {
        start_query(1U);
        on_owner(query_6010);
        CHECK(until(RC(RECOVERY), false).code == 6010 && fake_unbound == 0U);
        start_query(2U);
        on_owner(query_6006);
        CHECK(until(RC(RECOVERY), false).code == 6006 && fake_unbound == 1U);
        CHECK(status().http_status == 410 && status().error == RC_INVALID_RESPONSE);
        on_owner(present);
    } else if (!strcmp(selected, "query-conflict")) {
        start_query(1U);
        on_owner(query_40401);
        CHECK(!until(RC(RECOVERY), false).rejected_without_task);
        start_query(2U);
        on_owner(query_40400);
        CHECK(until(RC(RECOVERY), false).rejected_without_task);
        on_owner(conflict);
        check_no_more_query(2U);
    } else if (!strcmp(selected, "query-retry")) {
        start_query(1U);
        on_owner(query_500);
        CHECK(until(RC(RECOVERY), false).http_status == 503);
        advance(5100);
        wait_query(2U);
        on_owner(query_500);
        advance(10100);
        wait_query(3U);
        on_owner(query_500);
        check_no_more_query(3U);
        xiaotai_recognition_stats_t stats;
        xiaotai_recognition_stats(&stats);
        CHECK(stats.query_failures == 3U && stats.query_retries == 2U);
        on_owner(present);
    } else if (!strcmp(selected, "query-no-retry")) {
        start_query(1U);
        on_owner(pending_notice);
        CHECK(status().phase == RC(QUERY) && status().task_revision == 3U);
        on_owner(query_40400);
        CHECK(until(RC(RECOVERY), false).task_revision == 3U && status().code == 40400);
        CHECK(!status().rejected_without_task);
        check_no_more_query(1U);
        CHECK(xiaotai_recognition_policy_refresh() == XIAOTAI_OK);
        wait_refresh(1U);
        on_owner(policy_notice);
        on_owner(policy_404);
        const xiaotai_recognition_status_t s = until(RC(RESULT), false);
        CHECK(s.config_http_status == 404 && s.config_code == 40401);
        CHECK(s.http_status == 404 && s.code == 40400);
        check_no_more_query(1U);
        CHECK(fake_gets == 1U);
        on_owner(present);
    } else if (!strcmp(selected, "query-cancel")) {
        start_query(1U);
        on_owner(busy_begin);
        CHECK(until(RC(CANCELLING), true).error == XIAOTAI_ERR_INVALID_STATE && fake_transfer_cancelled);
        on_owner(query_cancelled);
        CHECK(until(RC(RECOVERY), false).error == XIAOTAI_ERR_INVALID_STATE);
        check_no_more_query(1U);
        on_owner(busy_end);
        wait_query(2U);
        CHECK(xiaotai_recognition_cancel(status().token) == XIAOTAI_OK);
        CHECK(until(RC(CANCELLING), true).error == XIAOTAI_OK && fake_transfer_cancelled);
        on_owner(query_cancelled);
        check_no_more_query(2U);
        on_owner(present);
    } else if (!strcmp(selected, "query-timeout")) {
        start_query(1U);
        for (unsigned attempt = 1; attempt <= 3U; ++attempt) {
            advance(25100);
            CHECK(until(RC(CANCELLING), true).error == XIAOTAI_ERR_TIMEOUT && fake_transfer_cancelled);
            on_owner(query_cancelled);
            CHECK(until(RC(RECOVERY), false).error == XIAOTAI_ERR_TIMEOUT);
            if (attempt < 3U) {
                advance(5100);
                wait_query(attempt + 1U);
            }
        }
        check_no_more_query(3U);
        on_owner(present);
    } else if (!strcmp(selected, "query-empty")) {
        on_owner(notify_dropped);
        advance(300);
        wait_refresh(1U);
        CHECK(!fake_queries && !status().request_id[0]);
        on_owner(policy_one);
        CHECK(until(RC(IDLE), false).config_revision == 1U);
    } else if (!strcmp(selected, "query-sync")) {
        on_owner(quiet_begin);
        on_owner(notify_dropped);
        check_no_more_query(0U);
        CHECK(fake_gets == 0U);
        on_owner(quiet_end);
        wait_query(1U);
        CHECK(fake_gets == 0U);
        on_owner(query_40400);
        wait_refresh(1U);
        CHECK(!status().rejected_without_task); /* v1, not a definitive submit conflict. */
        on_owner(policy_one);
        CHECK(until(RC(RECOVERY), false).config_revision == 1U);
        CHECK(status().http_status == 404 && status().code == 40400);
    } else if (!strcmp(selected, "query-sync-admission")) {
        start_query(1U);
        on_owner(completed_query);
        CHECK(until(RC(RESULT), false).task_state == RC(TASK_SUCCEEDED));
        on_owner(transfer_unavailable);
        on_owner(notify_dropped);
        CHECK(until(RC(RECOVERY), false).error == XIAOTAI_ERR_NO_MEM);
        CHECK(fake_queries == 1U);
        wait_refresh(1U);
        on_owner(policy_one);
        on_owner(transfer_available);
        advance(5100);
        wait_query(2U);
        on_owner(completed_query);
        CHECK(until(RC(RESULT), false).task_revision == 3U);
        check_no_more_query(2U);
    } else if (!strcmp(selected, "query-sync-capacity")) {
        start_query(1U);
        on_owner(completed_query);
        CHECK(until(RC(RESULT), false).task_state == RC(TASK_SUCCEEDED));
        on_owner(fill_timers);
        on_owner(notify_dropped);
        advance(300);
        xiaotai_delay_ms(300U);
        on_owner(NULL);
        if (fake_stream.done) on_owner(completed_query);
        on_owner(free_timers);
        advance(5300);
        wait_query(2U);
        on_owner(completed_query);
        wait_refresh(1U);
        on_owner(policy_one);
        CHECK(until(RC(RESULT), false).config_revision == 1U);
    } else if (!strcmp(selected, "query-refresh")) {
        start_query(1U);
        on_owner(query_40400);
        CHECK(until(RC(RECOVERY), false).code == 40400);
        on_owner(policy_notice);
        wait_refresh(1U);
        on_owner(policy_one); /* The reply predates the notification. */
        CHECK(until(RC(RECOVERY), false).config_error == RC_INVALID_RESPONSE);
        CHECK(status().code == 40400 && !status().config_revision);
        CHECK(xiaotai_recognition_policy_refresh() == XIAOTAI_OK);
        wait_refresh(2U);
        on_owner(policy_two);
        CHECK(until(RC(RECOVERY), false).config_revision == 2U && status().code == 40400);
        CHECK(xiaotai_recognition_policy_refresh() == XIAOTAI_OK);
        wait_refresh(3U);
        on_owner(policy_unbound);
        CHECK(until(RC(RECOVERY), false).config_code == 6006 && status().code == 6006 && fake_unbound == 0U);
    } else if (!strcmp(selected, "query-upload-notice")) {
        start_upload();
        char id[XIAOTAI_RECOGNITION_ID_MAX];
        strcpy(id, status().request_id);
        request_identity = id;
        on_owner(new_notice);
        CHECK(status().phase == RC(UPLOAD) && status().task_revision == 3U);
        on_owner(sent_but_uncertain);
        CHECK(until(RC(RESULT), false).task_revision == 3U);
        CHECK(fake_posts == 1U && status().possibly_sent);
        request_identity = "request";
    } else if (!strcmp(selected, "query-capture-notice")) {
        start_config();
        on_owner(policy_notice);
        CHECK(until(RC(CANCELLING), true).error == XIAOTAI_ERR_INVALID_STATE);
        on_owner(policy_one);
        wait_refresh(2U);
        on_owner(policy_two);
        CHECK(until(RC(CANCELLED), false).config_revision == 2U);
        CHECK(!fake_capture_request.done && fake_posts == 0U);
    } else {
        CHECK(!strcmp(selected, "query-foreign") || !strcmp(selected, "query-bad-service"));
        if (!strcmp(selected, "query-bad-service")) fake_endpoint_error = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
        CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
        for (unsigned i = 0; i < 1000 && status().decision_serial == 0; ++i) xiaotai_delay_ms(1);
        const xiaotai_err_t expected = fake_endpoint_error ? RC_INVALID_RESPONSE : XIAOTAI_ERR_INVALID_STATE;
        CHECK(status().decision_action == RC(ACTION_QUERY) && status().decision_error == expected);
        CHECK(!fake_queries && !fake_gets && !fake_posts);
        if (fake_endpoint_error) {
            CHECK(xiaotai_recognition_policy_refresh() == XIAOTAI_OK);
            on_owner(NULL);
            CHECK(status().config_error == expected);
        }
    }
}

static const uint8_t wav[] = {
    'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
    1,0,1,0,0x40,0x1f,0,0,0x80,0x3e,0,0,2,0,16,0,'d','a','t','a',2,0,0,0,0x12,0x34,
};
static uint32_t download_token;
static unsigned audio_handoffs;
static struct { int http; size_t received; xiaotai_err_t error; } audio_result;

static void take_audio(uint32_t token, uint8_t *data, size_t size, void *ctx)
{
    CHECK(xiaotai_on_owner() && ctx == &audio_handoffs && token == download_token);
    CHECK(status().phase == RC(DOWNLOAD) && !status().cleanup_pending && xiaotai_activity_active());
    CHECK(size == sizeof(wav) && memcmp(data, wav, size) == 0 && data == atomic_load(&audio_buffer));
    ++audio_handoffs;
    xiaotai_mem_free(data);
    fake_speaker_muted = true;
    CHECK(rc_recognition_audio_finish(token, XIAOTAI_OK) == XIAOTAI_OK);
}

static void download_start(void)
{
    fake_speaker_muted = false;
    CHECK(rc_recognition_audio_start("other", take_audio, &audio_handoffs) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(rc_recognition_audio_start("item_1", take_audio, &audio_handoffs) == XIAOTAI_OK);
    const xiaotai_recognition_status_t s = status();
    CHECK(s.phase == RC(DOWNLOAD) && s.token && s.cleanup_pending && xiaotai_activity_active());
    download_token = s.token;
    CHECK(rc_recognition_audio_finish(download_token, XIAOTAI_OK) == XIAOTAI_ERR_INVALID_STATE);
}

static void audio_finish(void)
{
    fake_download_done(audio_result.http, audio_result.received, audio_result.error);
}

static void mute_speaker(void) { fake_speaker_muted = true; }

static void download_case(void)
{
    start_query(1U);
    on_owner(audio_task_query);
    CHECK(until(RC(RESULT), false).task_revision == 1U);
    const bool budget = !strcmp(selected, "audio-budget"), growth = !strcmp(selected, "audio-growth-no-mem");
    if (budget) contiguous = 5000U;
    on_owner(download_start);
    CHECK(fake_downloads == 1U && !xiaotai_on_owner());
    const bool success = !strcmp(selected, "audio-success"), late = !strcmp(selected, "audio-timeout"),
        stale = !strcmp(selected, "audio-stale"), clipped = !strcmp(selected, "audio-clipped"),
        other = !strcmp(selected, "audio-other"), bad_type = !strcmp(selected, "audio-bad-headers"),
        muted = !strcmp(selected, "audio-muted"), bad_encoding = !strcmp(selected, "audio-repeated-encoding");
    const bool bad = bad_type || bad_encoding;
    const char *body = other ? "{\"code\":6010}" : "{\"code\":6006}";
    const size_t length = success || bad || muted ? sizeof(wav) : strlen(body);
    const int http = success || bad || late || muted || budget || growth ? 200 : 410;
    const xiaotai_platform_response_head_t head = {.http_status = http,
        .content_type = http == 200 ? "  AUDIO/WAV ; profile=pcm" : "application/json",
        .content_encoding = " Identity\t", .content_type_repeated = bad_type, .content_encoding_repeated = bad_encoding,
        .content_length = clipped || late || budget || growth ? -1 : (int64_t)length};
    if (!bad) atomic_store(&watch_audio_size, success || muted ? sizeof(wav) : 4096U);
    xiaotai_err_t error = fake_stream.begin(fake_stream.ctx, &head);
    size_t received = 0;
    if (bad) {
        CHECK(error == RC_INVALID_RESPONSE && !atomic_load(&audio_buffer));
    } else {
        CHECK(error == XIAOTAI_OK && atomic_load(&audio_buffer));
        const uint8_t *data = http == 200 ? wav : (const uint8_t *)body;
        CHECK(fake_stream.write(fake_stream.ctx, data, 3U) == XIAOTAI_OK);
        received = 3U;
        if (late) {
            advance(25100);
            CHECK(until(RC(CANCELLING), true).error == XIAOTAI_ERR_TIMEOUT);
            CHECK(fake_transfer_cancelled && atomic_load(&audio_buffer));
            error = fake_stream.write(fake_stream.ctx, data + 3U, 1U);
            CHECK(error == XIAOTAI_ERR_PLATFORM_CANCELLED);
        } else if (budget || growth) {
            static const uint8_t padding[5000];
            if (growth) atomic_store(&inject_growth_failure, true);
            error = fake_stream.write(fake_stream.ctx, padding, sizeof(padding));
            CHECK(error == (budget ? XIAOTAI_ERR_INVALID_SIZE : XIAOTAI_ERR_NO_MEM));
            CHECK(atomic_load(&audio_buffer));
        } else {
            CHECK(fake_stream.write(fake_stream.ctx, data + 3U, length - 3U) == XIAOTAI_OK);
            received = length;
            if (clipped) {
                static const uint8_t padding[4096];
                error = fake_stream.write(fake_stream.ctx, padding, sizeof(padding));
                CHECK(error == RC_INVALID_RESPONSE);
            }
        }
    }
    if (stale) on_owner(binding_changed);
    if (muted) on_owner(mute_speaker);
    audio_result.http = http;
    audio_result.received = received;
    audio_result.error = error;
    on_owner(audio_finish);
    CHECK(!atomic_load(&audio_buffer));
    const bool unbound = !strcmp(selected, "audio-unbound");
    const xiaotai_recognition_phase_t expected = success ? RC(RESULT) : unbound ? RC(RECOVERY) :
        late || stale || muted ? RC(CANCELLED) : RC(FAILED);
    const xiaotai_recognition_status_t s = until(expected, false);
    CHECK(!s.token && !xiaotai_activity_active() && audio_handoffs == (success ? 1U : 0U));
    CHECK(fake_unbound == (unbound ? 1U : 0U));
    if (!stale) CHECK(s.http_status == http && s.code == (unbound ? 6006 : other ? 6010 : 0));
    CHECK(s.error == (success ? XIAOTAI_OK : late ? XIAOTAI_ERR_TIMEOUT : stale || muted ?
                     XIAOTAI_ERR_INVALID_STATE : budget ? XIAOTAI_ERR_INVALID_SIZE : growth ? XIAOTAI_ERR_NO_MEM : RC_INVALID_RESPONSE));
    if (!success && !stale) CHECK(s.playback_paused);
    on_owner(present);
}

static void wait_download(unsigned count, const char *item)
{
    for (unsigned i = 0; i < 1500U; ++i) {
        on_owner(NULL);
        if (fake_downloads == count && fake_stream.done) {
            const char *tail = strrchr(fake_audio_path, '/');
            CHECK(tail && !strcmp(tail + 1, item));
            return;
        }
        xiaotai_delay_ms(1);
    }
    CHECK(false);
}

static void return_wav(void)
{
    const xiaotai_platform_response_head_t head = {.http_status = 200, .content_type = "audio/wav",
        .content_length = sizeof(wav)};
    CHECK(!xiaotai_on_owner());
    CHECK(fake_stream.begin(fake_stream.ctx, &head) == XIAOTAI_OK);
    CHECK(fake_stream.write(fake_stream.ctx, wav, sizeof(wav)) == XIAOTAI_OK);
    audio_result.http = 200;
    audio_result.received = sizeof(wav);
    audio_result.error = XIAOTAI_OK;
    on_owner(audio_finish);
}

static void return_chunked_wav(void)
{
    uint8_t file[5000] = {0};
    memcpy(file, wav, sizeof(wav));
    file[4] = 0x80; file[5] = 0x13;   /* 4992 RIFF payload bytes */
    file[40] = 0x5c; file[41] = 0x13; /* 4956 PCM payload bytes */
    const xiaotai_platform_response_head_t head = {.http_status = 200, .content_type = "audio/wav", .content_length = -1};
    CHECK(!xiaotai_on_owner());
    atomic_store(&watch_audio_size, 4096U);
    CHECK(fake_stream.begin(fake_stream.ctx, &head) == XIAOTAI_OK && atomic_load(&audio_buffer));
    CHECK(fake_stream.write(fake_stream.ctx, file, 1024U) == XIAOTAI_OK);
    CHECK(fake_stream.write(fake_stream.ctx, file + 1024U, 3072U) == XIAOTAI_OK);
    void *old = atomic_load(&audio_buffer);
    atomic_store(&watch_audio_size, 8192U);
    CHECK(fake_stream.write(fake_stream.ctx, file + 4096U, sizeof(file) - 4096U) == XIAOTAI_OK);
    CHECK(atomic_load(&audio_buffer) && atomic_load(&audio_buffer) != old);
    audio_result.http = 200;
    audio_result.received = sizeof(file);
    audio_result.error = XIAOTAI_OK;
    on_owner(audio_finish);
}

static xiaotai_recognition_status_t wait_decision(uint32_t previous, xiaotai_recognition_action_t action,
                                                  xiaotai_err_t expected)
{
    for (unsigned i = 0; i < 1500U; ++i) {
        on_owner(NULL);
        const xiaotai_recognition_status_t s = status();
        if (s.decision_serial != previous) {
            CHECK(s.decision_action == action && s.decision_error == expected);
            return s;
        }
        xiaotai_delay_ms(1);
    }
    CHECK(false);
    return status();
}

static void resume_result(xiaotai_err_t expected)
{
    const uint32_t before = status().decision_serial;
    CHECK(xiaotai_recognition_resume("request") == XIAOTAI_OK);
    (void)wait_decision(before, RC(ACTION_RESUME), expected);
}

static void dismiss_result(const char *item, xiaotai_err_t expected)
{
    const uint32_t before = status().decision_serial;
    CHECK(xiaotai_recognition_dismiss("request", item) == XIAOTAI_OK);
    (void)wait_decision(before, RC(ACTION_DISMISS), expected);
}

static void archive_request(const char *request, uint64_t revision, xiaotai_err_t expected)
{
    const uint32_t before = status().decision_serial;
    CHECK(xiaotai_recognition_archive(request, revision) == XIAOTAI_OK);
    (void)wait_decision(before, RC(ACTION_ARCHIVE), expected);
}

static void archive_result(uint64_t revision, xiaotai_err_t expected)
{
    archive_request("request", revision, expected);
}

static void reject_conflict_archive(const char *request)
{
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    archive_request(request, 0, XIAOTAI_ERR_INVALID_STATE);
    xiaotai_kv_host_counts(&after);
    CHECK(before.writes == after.writes && before.commits == after.commits);
}

static void upload_missing_code(void) { fake_upload_done("{\"message\":\"conflict\"}", 409, true, XIAOTAI_OK); }
static void query_missing_code(void) { query_code(404, "{\"message\":\"not found\"}"); }
static void conflict_new_binding(const char *old_request, bool late_ack);

static void legacy_conflict_case(void)
{
    const unsigned scenario = (unsigned)atoi(selected + sizeof("legacy-conflict-") - 1U);
    CHECK(scenario <= 10U);
    const bool reboot = scenario == 3U, confirmed = scenario != 1U && scenario != 2U;
    char original[2048], saved[2048], after[2048];
    if (!reboot) {
        start_upload();
        CHECK(xiaotai_kv_get_str("xt_recognition", "pending", original, sizeof(original), WAIT) == XIAOTAI_OK);
        on_owner(scenario == 1U ? sent_but_uncertain : scenario == 2U ? upload_missing_code : sent_conflict);
    }
    const xiaotai_recognition_status_t parked = until(RC(RECOVERY), false);
    CHECK(!parked.rejected_without_task && parked.request_id[0]);
    CHECK(xiaotai_kv_get_str("xt_recognition", "pending", saved, sizeof(saved), WAIT) == XIAOTAI_OK);
    CHECK(strstr(saved, confirmed ? "\"version\":4" : "\"version\":2"));
    if (!confirmed) CHECK(!strcmp(saved, original));
    CHECK(strstr(saved, parked.request_id));
    reject_conflict_archive(parked.request_id);

    CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
    wait_query(1U);
    char expected_path[XIAOTAI_PLATFORM_PATH_MAX];
    const int len = snprintf(expected_path, sizeof(expected_path), "/v1/recognition/requests/%s", parked.request_id);
    CHECK(len > 0 && (size_t)len < sizeof(expected_path) && !strcmp(expected_path, fake_query_path));
    on_owner(scenario == 4U ? query_missing_code : query_40400);
    const bool eligible = confirmed && scenario != 4U;
    CHECK(until(RC(RECOVERY), false).rejected_without_task == eligible);
    if (scenario >= 9U) {
        conflict_new_binding(parked.request_id, scenario == 10U);
    } else if (!eligible) {
        reject_conflict_archive(parked.request_id);
        denied(XIAOTAI_ERR_INVALID_STATE);
    } else if (scenario == 5U) {
        request_identity = parked.request_id;
        on_owner(new_notice);
        CHECK(until(RC(RESULT), false).task_revision == 3U && !status().rejected_without_task);
        reject_conflict_archive(parked.request_id);
        request_identity = "request";
    } else if (scenario == 6U) {
        atomic_store(&inject_archive_ack_timeout, true);
        archive_request(parked.request_id, 0, XIAOTAI_ERR_TIMEOUT);
        CHECK(xiaotai_kv_get_str("xt_recognition", "pending", after, sizeof(after), WAIT) == XIAOTAI_ERR_NOT_FOUND);
        const uint32_t previous = status().decision_serial;
        CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
        (void)wait_decision(previous, RC(ACTION_QUERY), XIAOTAI_ERR_INVALID_STATE);
        xiaotai_err_t record_errors[2];
        const unsigned gets = fake_gets;
        for (unsigned attempt = 0; attempt < 2U; ++attempt) {
            const uint32_t before = status().decision_serial;
            CHECK(xiaotai_recognition_record(1) == XIAOTAI_OK);
            on_owner(NULL);
            const xiaotai_recognition_status_t observed = status();
            CHECK(observed.decision_serial != before && observed.decision_action == RC(ACTION_RECORD));
            record_errors[attempt] = observed.decision_error;
            await_journal();
        }
        fprintf(stderr, "archive pending: first record=%d retry=%d config_requests=+%u\n",
                record_errors[0], record_errors[1], fake_gets - gets);
        CHECK(record_errors[0] == XIAOTAI_ERR_INVALID_STATE &&
              record_errors[1] == XIAOTAI_ERR_INVALID_STATE && fake_gets == gets);
        archive_request(parked.request_id, 0, XIAOTAI_OK);
        CHECK(status().phase == RC(IDLE) && !status().request_id[0] && !status().rejected_without_task);
    } else if (scenario == 7U) {
        on_owner(binding_changed);
        reject_conflict_archive(parked.request_id);
    } else if (scenario == 8U) {
        CHECK(xiaotai_kv_set_str("xt_recognition", "pending", original, WAIT) == XIAOTAI_OK);
        reject_conflict_archive(parked.request_id);
    } else {
        archive_request(parked.request_id, 0, XIAOTAI_OK);
        CHECK(status().phase == RC(IDLE) && !status().request_id[0]);
        CHECK(xiaotai_recognition_record(1) == XIAOTAI_OK);
        (void)until(RC(CONFIG), true);
        CHECK(xiaotai_recognition_cancel(0) == XIAOTAI_OK);
        (void)until(RC(CANCELLING), true);
        on_owner(policy_one);
        (void)until(RC(CANCELLED), false);
    }
    const bool removed = eligible && (scenario <= 3U || scenario == 6U || scenario >= 9U);
    CHECK(xiaotai_kv_get_str("xt_recognition", "pending", after, sizeof(after), WAIT) ==
          (removed ? XIAOTAI_ERR_NOT_FOUND : XIAOTAI_OK));
    if (!removed) CHECK(!strcmp(after, scenario == 8U ? original : saved));
    CHECK(fake_queries == 1U && fake_posts == (unsigned)!reboot);
}

static void playback_ok(void) { fake_play_done(XIAOTAI_OK); }
static void playback_cut(void) { fake_play_done(XIAOTAI_ERR_INVALID_STATE); }
static void playback_recording(void) { fake_foreign_recording = true; }

static void probe_ring(bool accepted)
{
    const xiaotai_media_play_t ring = {.clips = {{.name = "tone"}}, .clip_count = 1U,
        .priority = XIAOTAI_PLAY_NORMAL};
    CHECK(xiaotai_media_play_clips(&ring, NULL) == (accepted ? XIAOTAI_OK : XIAOTAI_ERR_BUSY));
}

static void probe_ring_busy(void) { probe_ring(false); }
static void probe_ring_free(void) { probe_ring(true); }

static size_t two_speeches(char *text, size_t capacity, bool ready)
{
    const char *first = ready ? "ready" : "pending";
    const int n = snprintf(text, capacity, "{\"code\":200,\"data\":{\"device_id\":\"dev\","
        "\"request_id\":\"request\",\"status_revision\":%u,\"status\":\"succeeded\",\"error\":null,\"results\":["
        "{\"item_id\":\"item_1\",\"order\":1,\"scheme_id\":\"fixture\",\"scheme_version\":1,\"output_mode\":\"audio\","
        "\"status\":\"succeeded\",\"result\":null,\"error\":null,\"speech\":{\"status\":\"%s\",\"error\":null,"
        "\"url\":\"/v1/recognition/requests/request/audio/item_1\",\"format\":\"wav\"}},"
        "{\"item_id\":\"item_2\",\"order\":2,\"scheme_id\":\"fixture\",\"scheme_version\":1,\"output_mode\":\"audio\","
        "\"status\":\"succeeded\",\"result\":null,\"error\":null,\"speech\":{\"status\":\"ready\",\"error\":null,"
        "\"url\":\"/v1/recognition/requests/request/audio/item_2\",\"format\":\"wav\"}}]}}", ready ? 2U : 1U, first);
    CHECK(n > 0 && (size_t)n < capacity);
    return (size_t)n;
}

static void pending_speech(void)
{
    char payload[1000];
    const size_t length = two_speeches(payload, sizeof(payload), false);
    fake_query_done(payload, length, -1, 32U, 200, XIAOTAI_OK);
}

static void ready_speech_notice(void)
{
    char payload[1000], envelope[1100];
    two_speeches(payload, sizeof(payload), true);
    const int n = snprintf(envelope, sizeof(envelope), "{\"type\":\"recognition_result\",\"channel\":\"recognition\","
        "\"event_id\":\"e100\",\"payload\":%s}", payload);
    CHECK(n > 0 && (size_t)n < sizeof(envelope));
    fake_notice("recognition_result", envelope, fake_generation);
}

static void read_view_item(xiaotai_recognition_result_t *view, size_t index, xiaotai_recognition_consumption_t expected)
{
    xiaotai_recognition_item_t item;
    CHECK(xiaotai_recognition_result_item(view, index, &item) == XIAOTAI_OK && item.consumption == expected);
}

static void dismiss_resume_case(void)
{
    const bool fails = !strcmp(selected, "playback-dismiss-failed");
    start_query(1U);
    on_owner(pending_speech);
    CHECK(until(RC(RESULT), false).task_revision == 1U);
    on_owner(ready_speech_notice);
    CHECK(status().task_revision == 2U && !fake_downloads);
    resume_result(XIAOTAI_OK);
    wait_download(1U, "item_1");
    return_wav();
    CHECK(until(RC(PLAYBACK), true).consumption == RC(PLAY_STARTED));
    on_owner(busy_begin);
    CHECK(until(RC(CANCELLING), true).playback_paused);
    on_owner(playback_cut);
    CHECK(until(RC(CANCELLED), false).consumption == RC(PLAY_STARTED));
    on_owner(busy_end);

    if (fails) {
        atomic_store(&inject_disposition_error, true);
        dismiss_result("item_1", XIAOTAI_FAIL);
        CHECK(status().playback_paused && fake_downloads == 1U);
        return;
    }
    xiaotai_kv_host_gate(false);
    const uint32_t before = status().decision_serial;
    CHECK(xiaotai_recognition_dismiss("request", "item_1") == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    CHECK(status().playback_paused && status().decision_serial == before && fake_downloads == 1U);
    xiaotai_kv_host_gate(true);
    (void)wait_decision(before, RC(ACTION_DISMISS), XIAOTAI_OK);
    wait_download(2U, "item_2");
    CHECK(!status().playback_paused);
    return_wav();
    CHECK(until(RC(PLAYBACK), true).consumption == RC(PLAY_STARTED));
    on_owner(playback_ok);
    CHECK(until(RC(RESULT), false).consumption == RC(PLAYED));
    xiaotai_recognition_result_t *result = NULL;
    CHECK(xiaotai_recognition_result_acquire(&result) == XIAOTAI_OK);
    read_view_item(result, 0, RC(DISMISSED));
    read_view_item(result, 1, RC(PLAYED));
    xiaotai_recognition_result_release(result);
    archive_result(2U, XIAOTAI_OK);
}

static void playback_case(void)
{
    const bool autoplay = !strcmp(selected, "playback-auto");
    const bool ordered = !strcmp(selected, "playback-order");
    const bool chunked = !strcmp(selected, "playback-chunked");
    const bool revoked = !strcmp(selected, "playback-revoke");
    const bool muted = !strcmp(selected, "playback-mute");
    const bool recording = !strcmp(selected, "playback-recording");
    const bool write_fails = !strcmp(selected, "playback-journal-fail");
    const bool write_times_out = !strcmp(selected, "playback-journal-timeout");
    start_query(1U);
    on_owner(ordered ? pending_speech : audio_task_query);
    xiaotai_recognition_result_t *before = NULL;
    if (!autoplay) {
        CHECK(until(RC(RESULT), false).task_revision == 1U);
        CHECK(xiaotai_recognition_result_acquire(&before) == XIAOTAI_OK);
        read_view_item(before, 0, RC(UNTOUCHED));
        advance(300);
        xiaotai_delay_ms(300U);
        on_owner(NULL);
        CHECK(!fake_downloads && !fake_plays); /* autoplay defaults to false. */
        archive_result(1U, XIAOTAI_ERR_INVALID_STATE);
        resume_result(XIAOTAI_OK);
    }
    if (ordered) {
        advance(300);
        xiaotai_delay_ms(300U);
        on_owner(NULL);
        CHECK(!fake_downloads); /* item_2 cannot jump ahead of pending item_1. */
        on_owner(ready_speech_notice);
    }
    wait_download(1U, "item_1");
    if (write_fails) atomic_store(&inject_disposition_error, true);
    if (write_times_out) atomic_store(&inject_disposition_timeout, true);
    if (chunked) return_chunked_wav();
    else return_wav();
    if (write_fails || write_times_out) {
        if (write_times_out) CHECK(xiaotai_kv_host_wait_held(WAIT));
        CHECK(until(RC(FAILED), false).error == (write_fails ? XIAOTAI_FAIL : XIAOTAI_ERR_TIMEOUT));
        CHECK(!fake_plays && status().playback_paused);
        if (write_times_out) {
            xiaotai_kv_host_gate(true);
            char raw[2048];
            CHECK(xiaotai_kv_get_str("xt_recognition", "pending", raw, sizeof(raw), WAIT) == XIAOTAI_OK);
            CHECK(strstr(raw, "\"state\":1") && !fake_plays);
        }
        xiaotai_recognition_result_release(before);
        return;
    }
    CHECK(until(RC(PLAYBACK), true).consumption == RC(PLAY_STARTED));
    expect_environment_guard(XIAOTAI_ERR_INVALID_STATE);
    CHECK(!fake_play_stopped);
    if (chunked) CHECK(status().playback_total_ms >= 300U && atomic_load(&audio_buffer));
    on_owner(probe_ring_busy);
    if (before) read_view_item(before, 0, RC(UNTOUCHED));
    if (revoked || muted || recording) {
        on_owner(revoked ? busy_begin : muted ? mute_speaker : playback_recording);
        CHECK(until(RC(CANCELLING), true).playback_paused && fake_play_stopped);
        on_owner(probe_ring_free);
        on_owner(playback_cut);
        CHECK(until(RC(CANCELLED), false).consumption == RC(PLAY_STARTED));
        if (revoked) on_owner(busy_end);
        resume_result(XIAOTAI_ERR_INVALID_STATE);
        dismiss_result("item_1", XIAOTAI_OK);
        dismiss_result("item_1", XIAOTAI_OK);
        CHECK(status().playback_paused && fake_downloads == 1U);
    } else {
        on_owner(playback_ok);
        if (ordered) {
            wait_download(2U, "item_2");
            return_wav();
            CHECK(until(RC(PLAYBACK), true).consumption == RC(PLAY_STARTED));
            on_owner(playback_ok);
        }
        CHECK(until(RC(RESULT), false).consumption == RC(PLAYED));
        if (chunked) CHECK(!atomic_load(&audio_buffer));
        xiaotai_recognition_result_t *after = NULL;
        CHECK(xiaotai_recognition_result_acquire(&after) == XIAOTAI_OK);
        read_view_item(after, 0, RC(PLAYED));
        if (ordered) read_view_item(after, 1, RC(PLAYED));
        xiaotai_recognition_result_release(after);
    }
    if (before) read_view_item(before, 0, RC(UNTOUCHED));
    archive_result(3U, XIAOTAI_ERR_INVALID_STATE);
    archive_result(ordered ? 2U : 1U, XIAOTAI_OK);
    CHECK(status().phase == RC(IDLE) && !status().request_id[0]);
    xiaotai_recognition_result_t *none = NULL;
    CHECK(xiaotai_recognition_result_acquire(&none) == XIAOTAI_ERR_NOT_FOUND);
    if (before) read_view_item(before, 0, RC(UNTOUCHED));
    xiaotai_recognition_result_release(before);
}

static void no_speaker(void) { fake_playback = false; }

static void decision_case(void)
{
    CHECK(xiaotai_recognition_resume("../request") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_recognition_dismiss("request", "") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_recognition_archive("request", XIAOTAI_RECOGNITION_REVISION_MAX + 1U) == XIAOTAI_ERR_INVALID_ARG);
    start_query(1U);
    if (!strcmp(selected, "decision-conflict")) {
        on_owner(query_40400);
        CHECK(until(RC(RECOVERY), false).rejected_without_task);
        archive_result(1U, XIAOTAI_ERR_INVALID_STATE);
        archive_result(0U, XIAOTAI_OK);
        CHECK(status().phase == RC(IDLE));
    } else {
        on_owner(audio_task_query);
        CHECK(until(RC(RESULT), false).task_items == 1U);
        expect_environment_guard(XIAOTAI_ERR_INVALID_STATE);
        on_owner(no_speaker);
        resume_result(XIAOTAI_ERR_NOT_SUPPORTED);
        dismiss_result("other", XIAOTAI_ERR_NOT_FOUND);
        dismiss_result("item_1", XIAOTAI_OK);
        CHECK(status().consumption == RC(DISMISSED) && !fake_downloads && !fake_plays);
        if (!strcmp(selected, "decision-archive-retry")) {
            atomic_store(&inject_archive_ack_timeout, true);
            archive_result(1U, XIAOTAI_ERR_TIMEOUT);
            CHECK(status().phase == RC(RECOVERY));
            expect_environment_guard(XIAOTAI_ERR_INVALID_STATE);
            dismiss_result("item_1", XIAOTAI_ERR_INVALID_STATE);
            resume_result(XIAOTAI_ERR_INVALID_STATE);
        }
        archive_result(1U, XIAOTAI_OK);
        CHECK(status().phase == RC(IDLE));
    }
    expect_environment_guard(XIAOTAI_OK);
}

#define VIEW_RUN "A\xc2\xb7\xe4\xb8\xad/\xf0\x9f\xa7\xaa\\n"
#define VIEW_RUN_4 VIEW_RUN VIEW_RUN VIEW_RUN VIEW_RUN
#define VIEW_RUN_8 VIEW_RUN_4 VIEW_RUN_4
#define VIEW_JSON "{\"summary\":\"fixture\\u4e2d\",\"alarm\":true,\"labels\":[\"one\"],\n" \
    "\"details\":{\"n\":1,\"utf8\":\"0" VIEW_RUN_8 "1" VIEW_RUN_8 "2" VIEW_RUN_8 "3" VIEW_RUN_8 \
    "4" VIEW_RUN_8 "5" VIEW_RUN_8 "6" VIEW_RUN_8 "7" VIEW_RUN_8 "\"}}"
static const char view_json[] = VIEW_JSON;
static const char text_reply[] = "{\"code\":200,\"data\":{\"device_id\":\"dev\",\"request_id\":\"request\","
    "\"status_revision\":1,\"status\":\"succeeded\",\"error\":null,\"results\":[{\"item_id\":\"item_1\",\"order\":1,"
    "\"scheme_version\":1,\"scheme_id\":\"fixture\",\"output_mode\":\"text\",\"status\":\"succeeded\",\"error\":null,"
    "\"result\":" VIEW_JSON ",\"speech\":{\"status\":\"skipped\",\"error\":null}}]}}";
#undef VIEW_JSON
#undef VIEW_RUN_8
#undef VIEW_RUN_4
#undef VIEW_RUN

static void queried_text(void)
{
    fake_query_done(text_reply, sizeof(text_reply) - 1U, -1, 13U, 200, XIAOTAI_OK);
}

static void view_case(void)
{
    xiaotai_recognition_result_t *result = NULL;
    CHECK(xiaotai_recognition_result_acquire(&result) == XIAOTAI_ERR_NOT_FOUND);
    start_query(1U);
    on_owner(queried_text);
    CHECK(until(RC(RESULT), false).task_revision == 1U);
    CHECK(xiaotai_recognition_result_acquire(&result) == XIAOTAI_OK);
    xiaotai_recognition_item_t item;
    CHECK(xiaotai_recognition_result_item(result, 0U, &item) == XIAOTAI_OK && !strcmp(item.item_id, "item_1"));
    CHECK(item.has_text && item.alarm && item.output == RC(OUTPUT_TEXT) && item.speech == RC(SPEECH_SKIPPED));
    CHECK(xiaotai_recognition_result_item(result, 1U, &item) == XIAOTAI_ERR_INVALID_ARG);
    char text[sizeof(view_json)];
    size_t size = 0;
    CHECK(xiaotai_recognition_result_summary(result, 0U, text, 6U, &size) == XIAOTAI_ERR_INVALID_SIZE && size == 10U);
    CHECK(xiaotai_recognition_result_summary(result, 0U, text, sizeof(text), &size) == XIAOTAI_OK);
    CHECK(!strcmp(text, "fixture\xe4\xb8\xad"));
    CHECK(xiaotai_recognition_result_detail(result, 0U, NULL, 0U, &size) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(size == sizeof(view_json) - 1U && size > 512U);
    CHECK(xiaotai_recognition_result_detail(result, 0U, text, sizeof(text), &size) == XIAOTAI_OK);
    CHECK(size == sizeof(view_json) - 1U && !memcmp(text, view_json, sizeof(text)));
    char page[512];
    for (size_t capacity = 5; capacity <= sizeof(page); ++capacity) {
        size_t start = 0, preceding = 0, pages = 0;
        do {
            size_t next = 0, previous = 0, total = 0;
            CHECK(xiaotai_recognition_result_detail_page(result, 0U, start, page, capacity,
                  &next, &previous, &total) == XIAOTAI_OK);
            CHECK(total == size && previous == preceding && next > start && next <= size);
            CHECK(next - start < capacity && page[next - start] == '\0');
            CHECK(!memcmp(page, view_json + start, next - start));
            CHECK(next == size || ((unsigned char)view_json[next] & 0xc0U) != 0x80U);
            preceding = start;
            start = next;
            ++pages;
        } while (start < size);
        CHECK(pages > 1);
        for (size_t i = pages; i; --i) {
            size_t next = 0, previous = 0, total = 0;
            CHECK(xiaotai_recognition_result_detail_page(result, 0U, preceding, page, capacity,
                  &next, &previous, &total) == XIAOTAI_OK);
            CHECK(total == size && next == start);
            CHECK(i == 1 ? !preceding && !previous : previous < preceding);
            start = preceding;
            preceding = previous;
        }
    }
    size_t next, total;
    CHECK(xiaotai_recognition_result_detail_page(result, 0U, size, page, sizeof(page),
          &next, NULL, &total) == XIAOTAI_ERR_INVALID_ARG);
    on_owner(new_notice);
    CHECK(status().task_revision == 3U);
    xiaotai_recognition_result_t *newer = NULL;
    CHECK(xiaotai_recognition_result_acquire(&newer) == XIAOTAI_OK);
    xiaotai_recognition_result_info_t info;
    CHECK(xiaotai_recognition_result_info(result, &info) == XIAOTAI_OK && info.revision == 1U);
    CHECK(xiaotai_recognition_result_info(newer, &info) == XIAOTAI_OK && info.revision == 3U);
    CHECK(xiaotai_recognition_result_detail(newer, 0U, NULL, 0U, &size) == XIAOTAI_ERR_NOT_FOUND && !size);
    CHECK(xiaotai_recognition_result_detail_page(newer, 0U, 0U, page, sizeof(page),
          &next, NULL, &total) == XIAOTAI_ERR_NOT_FOUND);
    const uint32_t before = status().decision_serial;
    const xiaotai_args_t first_item = {0};
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_RECOGNITION_DISMISS, &first_item) == XIAOTAI_OK);
    const xiaotai_recognition_status_t dismissed = wait_decision(before, RC(ACTION_DISMISS), XIAOTAI_OK);
    CHECK(!strcmp(dismissed.item_id, "item_1"));
    on_owner(binding_changed);
    CHECK(!status().item_id[0]);
    CHECK(xiaotai_recognition_result_info(result, &info) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_recognition_result_detail_page(result, 0U, 0U, page, sizeof(page),
          &next, NULL, &total) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_recognition_result_summary(newer, 0U, text, sizeof(text), &size) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_recognition_result_release(newer);
    xiaotai_recognition_result_release(result);
}

static xiaotai_err_t expected_guard;

static void check_environment(void)
{
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    const unsigned gets = fake_gets, posts = fake_posts, queries = fake_queries, downloads = fake_downloads;
    const xiaotai_err_t actual = xiaotai_recognition_maintenance_check();
    if (actual != expected_guard)
        fprintf(stderr, "maintenance=%d expected=%d mirror=%d busy=%d\n", actual, expected_guard,
                xiaotai_recognition_journal_mirror()->state, xiaotai_recognition_journal_busy());
    CHECK(actual == expected_guard);
    uint32_t ticket = 0;
    CHECK(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &ticket) == expected_guard);
    CHECK(xiaotai_quiesced() == (expected_guard == XIAOTAI_OK));
    if (ticket) xiaotai_quiesce_end(ticket);
    CHECK(!xiaotai_quiesced());
    xiaotai_kv_host_counts(&after);
    CHECK(before.opens == after.opens && before.writes == after.writes && before.commits == after.commits);
    CHECK(gets == fake_gets && posts == fake_posts && queries == fake_queries && downloads == fake_downloads);
}

static void expect_environment_guard(xiaotai_err_t expected)
{
    expected_guard = expected;
    on_owner(check_environment);
}

static void non_environment_quiesce(void)
{
    static const char *const reasons[] = {XIAOTAI_QUIESCE_COPROCESSOR, XIAOTAI_QUIESCE_PROVISIONING,
                                          XIAOTAI_QUIESCE_BINDING};
    for (size_t i = 0; i < sizeof(reasons) / sizeof(*reasons); ++i) {
        uint32_t ticket = 0;
        CHECK(xiaotai_quiesce_begin(reasons[i], &ticket) == XIAOTAI_OK && ticket && xiaotai_quiesced());
        xiaotai_quiesce_end(ticket);
    }
}

static void order_restart(void) { CHECK(xiaotai_restart_request("fixture-restart", 0) == XIAOTAI_OK); }

static void guards_case(void)
{
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    expected_guard = XIAOTAI_ERR_INVALID_STATE;
    on_owner(check_environment);
    xiaotai_kv_host_fail_key("scope", XIAOTAI_FAIL);
    xiaotai_kv_host_gate(true);
    await_journal();
    expected_guard = XIAOTAI_FAIL;
    on_owner(check_environment);
    CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
    await_journal();
    CHECK(status().decision_action == RC(ACTION_QUERY) && status().decision_error == XIAOTAI_FAIL);
    on_owner(check_environment);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    xiaotai_kv_host_gate(false);
    CHECK(xiaotai_recognition_record(1) == XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    expected_guard = XIAOTAI_ERR_INVALID_STATE;
    on_owner(check_environment);
    CHECK(status().decision_action == RC(ACTION_RECORD) && status().decision_error == XIAOTAI_FAIL);
    xiaotai_kv_host_gate(true);
    await_journal();
    expected_guard = XIAOTAI_OK;
    on_owner(check_environment);
    start_config();
    expected_guard = XIAOTAI_ERR_INVALID_STATE;
    on_owner(check_environment);
    CHECK(xiaotai_recognition_cancel(0) == XIAOTAI_OK);
    (void)until(RC(CANCELLING), true);
    on_owner(check_environment);
    on_owner(policy_one);
    (void)until(RC(CANCELLED), false);

    CHECK(xiaotai_kv_set_str("xt_recognition", "pending", "{\"version\":1,\"device_id\":\"foreign\","
                             "\"request_id\":\"foreign_job\",\"revision\":1,\"seconds\":1}", WAIT) == XIAOTAI_OK);
    CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
    await_journal();
    on_owner(present);
    on_owner(check_environment);
    CHECK(!status().request_id[0] && fake_queries == 0);
    on_owner(non_environment_quiesce);
    on_owner(order_restart);
    on_owner(NULL);
    CHECK(xiaotai_quiesced() && xiaotai_restart_pending());
    char stored[128];
    CHECK(xiaotai_kv_get_str("xt_recognition", "pending", stored, sizeof(stored), WAIT) == XIAOTAI_OK);
    CHECK(strstr(stored, "foreign_job"));
}

static struct { unsigned index; xiaotai_err_t outcome; } prepare_results[8];
static unsigned prepare_count, prepare_next;

static void prepared_scope(xiaotai_err_t result, void *ctx)
{
    CHECK(xiaotai_on_owner() && prepare_count < sizeof(prepare_results) / sizeof(*prepare_results));
    prepare_results[prepare_count].index = (unsigned)(uintptr_t)ctx;
    prepare_results[prepare_count++].outcome = result;
}

static void begin_scope(void)
{
    CHECK(fake_binding_prepare(prepared_scope, (void *)(uintptr_t)++prepare_next) == XIAOTAI_OK);
}

static void expect_scope(int64_t expected)
{
    int64_t value = 0;
    CHECK(xiaotai_kv_get_int("xt_recognition", "scope", XIAOTAI_KV_U32, &value, WAIT) == XIAOTAI_OK);
    CHECK(value == expected);
}

static void report_binding_online(void)
{
    fake_online = true;
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
}

static void binding_changed_offline(void)
{
    binding_changed();
    CHECK(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, NULL, NULL, 0) == XIAOTAI_OK);
}

static unsigned reused_prepares;
static uint32_t expected_reused_scope = 1U;
static void reused_prepared(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                            const xiaotai_recognition_journal_mirror_t *mirror, void *ctx)
{
    (void)item;
    (void)ctx;
    CHECK(xiaotai_on_owner() && error == XIAOTAI_OK);
    CHECK(mirror->state == RJ(PRESENT) && mirror->record.scope == expected_reused_scope);
    CHECK(!strcmp(mirror->record.device_id, "dev") && !strcmp(mirror->record.request_id, request_identity));
    ++reused_prepares;
}

static void prepare_reused_request(void)
{
    xiaotai_recognition_journal_job_t job = {.operation = RJ(PREPARE), .done = reused_prepared,
        .record = {.device_id = "dev", .config_revision = 7U, .seconds = 1U}};
    CHECK(strlen(request_identity) < sizeof(job.record.request_id));
    strcpy(job.record.request_id, request_identity);
    CHECK(xiaotai_recognition_journal_submit(&job) == XIAOTAI_OK);
}

static void confirm_old_scope(xiaotai_err_t error, xiaotai_recognition_journal_item_state_t item,
                              const xiaotai_recognition_journal_mirror_t *mirror, void *ctx)
{
    (void)item;
    (void)ctx;
    CHECK(error == XIAOTAI_OK && mirror->state == RJ(PRESENT) && mirror->record.scope == 0U);
    CHECK(!strcmp(mirror->record.device_id, "dev") && !strcmp(mirror->record.request_id, request_identity));
}

static void reload_old_scope(void)
{
    const xiaotai_recognition_journal_job_t job = {.operation = RJ(LOAD), .done = confirm_old_scope};
    CHECK(xiaotai_recognition_journal_submit(&job) == XIAOTAI_OK);
}

static void query_reused_request(void)
{
    fake_online = true;
    new_notice(); /* No durable scope on the signal; it must stay quarantined. */
    CHECK(!status().task_revision);
    CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
}

static void finish_replacement_query(void)
{
    CHECK(!strcmp(fake_query_path, "/v1/recognition/requests/replacement"));
    char body[512];
    const size_t len = task_json(body, sizeof(body), "dev", "replacement", 4U, "succeeded");
    fake_query_done(body, len, (int64_t)len, 11U, 200, XIAOTAI_OK);
}

static void decision_reused_scope(void)
{
    const bool unadvanced = !strcmp(selected, "decision-reused-unadvanced");
    const bool unknown = !strcmp(selected, "decision-reused-unknown");
    const bool replacement = !strcmp(selected, "decision-reused-replacement");
    const bool late = replacement || !strcmp(selected, "decision-reused-late");
    if (unknown) {
        start_recording();
        xiaotai_kv_host_fail_key("pending", XIAOTAI_FAIL);
        on_owner(record_full);
        CHECK(until(RC(RECOVERY), false).error == XIAOTAI_FAIL);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    } else {
        start_query(1U);
        on_owner(audio_task_query);
        CHECK(until(RC(RESULT), false).task_revision == 1U);
        dismiss_result("item_1", XIAOTAI_OK);
        atomic_store(&inject_archive_before_erase_timeout, true);
        archive_result(1U, XIAOTAI_ERR_TIMEOUT);
    }
    const xiaotai_recognition_status_t old = status();
    CHECK(old.request_id[0]);
    request_identity = old.request_id;
    on_owner(binding_changed_offline);
    if (unadvanced) {
        on_owner(reload_old_scope);
        await_journal();
        on_owner(present);
    } else {
        on_owner(begin_scope);
        await_journal();
        CHECK(prepare_count == 1U && prepare_results[0].outcome == XIAOTAI_OK);
        expect_scope(1);
        on_owner(prepare_reused_request);
        await_journal();
        CHECK(reused_prepares == 1U);
    }
    const unsigned queries = fake_queries;
    const uint32_t serial = status().decision_serial;
    on_owner(query_reused_request);
    const bool allowed = !unadvanced && !unknown;
    (void)wait_decision(serial, RC(ACTION_QUERY), allowed ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE);
    if (!allowed) {
        CHECK(!fake_stream.done && fake_queries == queries && !status().task_revision);
    } else {
        on_owner(observe_query);
        CHECK(fake_queries == queries + 1U && fake_stream.done);
        if (late) {
            on_owner(begin_scope);
            await_journal();
            CHECK(prepare_count == 2U && prepare_results[1].outcome == XIAOTAI_OK);
            expect_scope(2);
            if (replacement) {
                request_identity = "replacement";
                expected_reused_scope = 2U;
                on_owner(prepare_reused_request);
                await_journal();
                CHECK(reused_prepares == 2U);
                on_owner(pending_notice);
                CHECK(status().task_revision == 3U && !strcmp(status().request_id, "replacement"));
            }
            on_owner(query_6006);
            const xiaotai_recognition_status_t retired = until(RC(RECOVERY), false);
            CHECK(retired.error == XIAOTAI_ERR_INVALID_STATE && retired.task_revision == (replacement ? 3U : 0U) &&
                  !retired.http_status && !retired.code && !fake_unbound);
            if (replacement) {
                CHECK(!strcmp(retired.request_id, "replacement"));
                advance(5100U);
                wait_query(queries + 2U);
                on_owner(finish_replacement_query);
                CHECK(until(RC(RESULT), false).task_revision == 4U);
            }
        } else {
            on_owner(processing_query);
            CHECK(until(RC(RESULT), false).task_revision == 2U);
            advance(5100U);
            wait_query(queries + 2U);
            on_owner(completed_query);
            CHECK(until(RC(RESULT), false).task_revision == 3U && !strcmp(status().request_id, "request"));
        }
    }
    request_identity = "request";
}

static void conflict_new_binding(const char *old_request, bool late_ack)
{
    const unsigned before_gets = fake_gets;
    if (late_ack) {
        CHECK(xiaotai_signal_init(&archive_erased) == XIAOTAI_OK);
        CHECK(xiaotai_signal_init(&archive_ack) == XIAOTAI_OK);
        atomic_store(&hold_archive_ack, true);
        const uint32_t before = status().decision_serial;
        CHECK(xiaotai_recognition_archive(old_request, 0) == XIAOTAI_OK);
        CHECK(xiaotai_signal_take(&archive_erased, WAIT));
        on_owner(binding_changed);
        on_owner(begin_scope); /* Ordered behind the retiring identity's held completion. */
        xiaotai_signal_give(&archive_ack);
        const xiaotai_recognition_status_t stale = wait_decision(before, RC(ACTION_ARCHIVE), XIAOTAI_ERR_INVALID_STATE);
        CHECK(!stale.request_id[0] && !stale.token && !stale.rejected_without_task);
        xiaotai_signal_deinit(&archive_ack);
        xiaotai_signal_deinit(&archive_erased);
    } else {
        atomic_store(&inject_archive_ack_timeout, true);
        archive_request(old_request, 0, XIAOTAI_ERR_TIMEOUT);
        on_owner(begin_scope);
        await_journal();
        on_owner(binding_changed);
    }
    await_journal();
    CHECK(prepare_count == 1 && prepare_results[0].outcome == XIAOTAI_OK);
    expect_scope(1);
    CHECK(fake_gets == before_gets);
    on_owner(report_binding_online);
    /* The automatic refresh starts after the new generation's catch-up query
     * has reloaded the journal. Checking earlier can race that reload. */
    wait_refresh(before_gets + 1U);
    on_owner(absent);
    on_owner(policy_one);
    (void)until(RC(IDLE), false);
    CHECK(!status().request_id[0] && !status().rejected_without_task);
    CHECK(xiaotai_recognition_record(1) == XIAOTAI_OK);
    const xiaotai_recognition_status_t fresh = until(RC(CONFIG), true);
    CHECK(fresh.token && fake_gets == before_gets + 2U);
    reject_conflict_archive(old_request);
    CHECK(status().token == fresh.token && status().phase == RC(CONFIG));
    CHECK(xiaotai_recognition_cancel(0) == XIAOTAI_OK);
    (void)until(RC(CANCELLING), true);
    on_owner(policy_one);
    (void)until(RC(CANCELLED), false);
}

static void guards_retired_scope(void)
{
    const bool late = !strcmp(selected, "guards-retired-late");
    on_owner(present);
    expected_guard = XIAOTAI_ERR_INVALID_STATE;
    on_owner(check_environment);
    if (late) {
        atomic_store(&scope_write_block, true);
        atomic_store(&scope_write_timeout, true);
    }
    on_owner(begin_scope);
    if (late) CHECK(xiaotai_kv_host_wait_held(WAIT));
    await_journal();
    CHECK(prepare_count == 1 &&
          prepare_results[0].outcome == (late ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK));
    if (late) {
        expected_guard = XIAOTAI_ERR_TIMEOUT;
        on_owner(check_environment);
        xiaotai_kv_host_gate(true);
        CHECK(xiaotai_kv_flush(WAIT) == XIAOTAI_OK);
    }
    expect_scope(1);

    char record[192];
    CHECK(xiaotai_kv_get_str("xt_recognition", "pending", record, sizeof(record), WAIT) == XIAOTAI_OK);
    CHECK(!strcmp(record, "{\"version\":1,\"device_id\":\"dev\",\"request_id\":\"request\","
                         "\"revision\":1,\"seconds\":1}"));
    CHECK(!fake_gets && !fake_posts && !fake_queries);
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    expected_guard = late ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
    on_owner(check_environment);
    xiaotai_kv_host_counts(&after);
    CHECK(before.opens == after.opens && before.writes == after.writes &&
          before.commits == after.commits);
    CHECK(!fake_gets && !fake_posts && !fake_queries);
}

static void scope_case(void)
{
    int64_t absent_scope;
    CHECK(xiaotai_kv_get_int("xt_recognition", "scope", XIAOTAI_KV_U32, &absent_scope, WAIT) == XIAOTAI_ERR_NOT_FOUND);
    atomic_store(&scope_write_block, true);
    on_owner(begin_scope);
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    on_owner(begin_scope);
    CHECK(prepare_count == 0);
    xiaotai_kv_host_gate(true);
    await_journal();
    CHECK(prepare_count == 2 && prepare_results[0].index == 1 && prepare_results[1].index == 2);
    CHECK(prepare_results[0].outcome == XIAOTAI_OK && prepare_results[1].outcome == XIAOTAI_OK);
    expect_scope(2);

    atomic_store(&scope_write_fail, true);
    on_owner(begin_scope);
    await_journal();
    CHECK(prepare_count == 3 && prepare_results[2].outcome == XIAOTAI_FAIL);
    expect_scope(2);

    atomic_store(&scope_write_block, true);
    atomic_store(&scope_write_timeout, true);
    on_owner(begin_scope);
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    await_journal();
    CHECK(prepare_count == 4 && prepare_results[3].outcome == XIAOTAI_ERR_TIMEOUT);
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT) == XIAOTAI_OK);
    expect_scope(3); // Timed-out durable work can finish but cannot report a second result.
    on_owner(begin_scope);
    await_journal();
    CHECK(prepare_count == 5 && prepare_results[4].outcome == XIAOTAI_OK);
    expect_scope(4);

    CHECK(xiaotai_kv_set_int("xt_recognition", "scope", XIAOTAI_KV_U32, UINT32_MAX, WAIT) == XIAOTAI_OK);
    on_owner(begin_scope);
    await_journal();
    CHECK(prepare_count == 6 && prepare_results[5].outcome == XIAOTAI_ERR_INVALID_STATE);
    expect_scope(UINT32_MAX);
    xiaotai_recognition_stats_t stats;
    xiaotai_recognition_stats(&stats);
    CHECK(stats.scope_advances == 3 && stats.journal_failures == 3 && stats.journal_timeouts == 1);
    CHECK(!fake_gets && !fake_posts && !fake_queries);
}

static const char *command(const char *input)
{
    at_used = 0;
    at_text[0] = '\0';
    at_input = input;
    do { CHECK(xiaotai_at_service(1U) == XIAOTAI_OK); } while (*at_input);
    return at_text;
}

static void at_case(void)
{
    xiaotai_engine_at_start();
    CHECK(strstr(command("AT+RECOGNITION\r"),
        "+RECOGNITION-STATE: phase=idle token=0 samples=0 target=0 error=0 http=0 code=0 cleanup=0 possibly_sent=0 request=\r\n"
        "+RECOGNITION-TASK: revision=0 state=0 items=0 rejected_without_task=0\r\n"
        "+RECOGNITION-POLICY: revision=0 enabled=0 error=0 http=0 code=0\r\n"
        "+RECOGNITION-PLAYBACK: item= consumed=0 playback_ms=0 playback_total_ms=0 paused=0\r\n"
        "+RECOGNITION-DECISION: serial=0 action=0 error=0\r\nOK\r\n"));
    CHECK(strstr(command("AT+STATUS recognition\r"), "scope_advances=0 stale=0 release_retries=0\r\nOK\r\n"));
    CHECK(strstr(command("AT+HELP\r"), "+HELP: AT+RECOGNITION RECORD <seconds:1..60>\r\n"));
    static const char *const bad[] = {"AT+RECOGNITION RECORD 0\r", "AT+RECOGNITION RECORD 61\r",
        "AT+RECOGNITION RECORD +1\r", "AT+RECOGNITION CANCEL ignored\r", "AT+RECOGNITION RESUME bad.id\r",
        "AT+RECOGNITION RESUME request extra\r", "AT+RECOGNITION DISMISS request\r",
        "AT+RECOGNITION DISMISS request invalid.id\r", "AT+RECOGNITION ARCHIVE request 9007199254740992\r",
        "AT+RECOGNITION ARCHIVE request -1\r", "AT+RECOGNITION ARCHIVE request +1\r",
        "AT+RECOGNITION ARCHIVE request 1e2\r", "AT+RECOGNITION ARCHIVE request \"\"\r",
        "AT+RECOGNITION ARCHIVE request 999999999999999999999999999\r",
        "AT+RECOGNITION ARCHIVE request 9007199254740991 extra\r"};
    atomic_store(&inject_at_admission, true);
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) CHECK(strstr(command(bad[i]), "ERROR: INVALID_ARG"));
    CHECK(atomic_load(&inject_at_admission));
    CHECK(!strcmp(command("AT+RECOGNITION ARCHIVE request 9007199254740991\r"), "ERROR: NO_MEM\r\n"));
    CHECK(!atomic_load(&inject_at_admission));
    on_owner(NULL);
    CHECK(status().decision_serial == 0 && !fake_gets);
    char id[66], line[100];
    memset(id, 'x', 65);
    id[65] = '\0';
    CHECK(snprintf(line, sizeof(line), "AT+RECOGNITION RESUME %s\r", id) > 0);
    CHECK(strstr(command(line), "ERROR: INVALID_ARG"));
    id[64] = '\0';
    CHECK(snprintf(line, sizeof(line), "AT+RECOGNITION RESUME %s\r", id) > 0);
    CHECK(!strcmp(command(line), "OK\r\n"));
    on_owner(NULL);
    CHECK(status().decision_action == RC(ACTION_RESUME) && status().decision_error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!strcmp(command("AT+RECOGNITION DISMISS request item_1\r"), "OK\r\n"));
    on_owner(NULL);
    CHECK(status().decision_action == RC(ACTION_DISMISS) && status().decision_error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!strcmp(command("AT+RECOGNITION ARCHIVE request 0\r"), "OK\r\n"));
    on_owner(NULL);
    CHECK(status().decision_action == RC(ACTION_ARCHIVE) && status().decision_error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!strcmp(command("AT+RECOGNITION ARCHIVE request 9007199254740991\r"), "OK\r\n"));
    on_owner(NULL);
    CHECK(status().decision_action == RC(ACTION_ARCHIVE) && status().decision_error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!strcmp(command("AT+RECOGNITION RECORD 1\r"), "OK\r\n"));
    (void)until(RC(CONFIG), true);
    CHECK(!strcmp(command("AT+RECOGNITION CANCEL\r"), "OK\r\n"));
    on_owner(policy_one);
    (void)until(RC(CANCELLED), false);
    const xiaotai_args_t invalid = {.value = -1};
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_RECOGNITION_DISMISS, &invalid) == XIAOTAI_OK);
    on_owner(NULL);
    CHECK(status().decision_action == RC(ACTION_DISMISS) && status().decision_error == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_RECOGNITION_RESUME, NULL) == XIAOTAI_OK);
    on_owner(NULL);
    CHECK(status().decision_action == RC(ACTION_RESUME) && status().decision_error == XIAOTAI_ERR_INVALID_STATE);
    CHECK(!fake_posts && !fake_queries && !fake_plays);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    selected = argv[1];
    const bool existing_record = !strcmp(selected, "corrupt") || !strcmp(selected, "stored") ||
                                 !strcmp(selected, "foreign");
    CHECK(xiaotai_recognition_cancel(1) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_signal_init(&arrived) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 1};
    const bool guards = !strcmp(selected, "guards");
    if (guards) {
        CHECK(xiaotai_kv_init() == XIAOTAI_OK);
        CHECK(xiaotai_kv_set_int("xt_recognition", "scope", XIAOTAI_KV_U32, 0, WAIT) == XIAOTAI_OK);
        xiaotai_kv_host_gate(false);
    }
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    if (!guards) await_journal();
    if (guards) guards_case();
    else if (!strncmp(selected, "guards-retired-", 15U)) guards_retired_scope();
    else if (!strcmp(selected, "scope")) scope_case();
    else if (!strcmp(selected, "at")) at_case();
    else if (!strcmp(selected, "legacy-notices")) on_owner(legacy_notices);
    else if (!strncmp(selected, "legacy-conflict-", sizeof("legacy-conflict-") - 1U)) legacy_conflict_case();
    else if (!strncmp(selected, "query-", 6)) query_case();
    else if (!strncmp(selected, "audio-", 6)) download_case();
    else if (!strncmp(selected, "playback-dismiss-", 17)) dismiss_resume_case();
    else if (!strncmp(selected, "playback-", 9)) playback_case();
    else if (!strncmp(selected, "decision-reused-", 16)) decision_reused_scope();
    else if (!strncmp(selected, "decision-", 9)) decision_case();
    else if (!strcmp(selected, "result-views")) view_case();
    else if (existing_record) fail_boot_read();
    else if (!strcmp(selected, "admission")) admission();
    else if (!strcmp(selected, "owner-full")) full_owner_rejects_actions();
    else if (!strcmp(selected, "memory") || !strcmp(selected, "fragmented")) memory();
    else if (!strcmp(selected, "gap") || !strcmp(selected, "short")) recording_failure();
    else if (!strcmp(selected, "capture-progress")) recording_progress_case();
    else if (!strcmp(selected, "config-timeout")) config_timeout();
    else if (!strcmp(selected, "capture-cancel") || !strcmp(selected, "capture-timeout") ||
             !strcmp(selected, "revoke")) capture_cancelled();
    else if (!strcmp(selected, "prepare-failed") || !strcmp(selected, "prepare-timeout")) prepare_failure();
    else if (!strcmp(selected, "prepare-offline")) prepare_offline();
    else if (!strcmp(selected, "recheck") || !strcmp(selected, "enqueue") ||
             !strcmp(selected, "recheck-cancel")) recheck_failure();
    else if (!strcmp(selected, "stale")) stale_generation();
    else if (!strcmp(selected, "stale-action")) stale_action();
    else if (!strcmp(selected, "config-unbound")) config_unbound();
    else transfer_result();
    on_owner(NULL);
    CHECK(!journal_busy && !fake_stream.done && !fake_capture_request.done);
    xiaotai_framework_reset();
    xiaotai_recognition_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&arrived);
    return 0;
}
