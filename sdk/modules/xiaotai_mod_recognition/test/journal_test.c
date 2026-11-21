// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_journal.py
#include "kv_backend.h"
#include "recognition_journal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } \
} while (0)

#define RJ(name) XIAOTAI_RECOGNITION_JOURNAL_##name
#define SPACE "xt_recognition"
#define WAIT 2000u

typedef struct {
    xiaotai_err_t result;
    xiaotai_recognition_journal_item_state_t item;
    xiaotai_recognition_journal_mirror_t mirror;
    bool still_busy;
} result_t;

typedef struct {
    xiaotai_recognition_journal_job_t job;
    xiaotai_signal_t submitted;
    xiaotai_signal_t completed;
    xiaotai_err_t submit_result;
    xiaotai_recognition_journal_state_t state_after_submit;
    bool busy_after_submit;
    result_t result;
} request_t;

enum { NO_TIMEOUT, ACTIVE_TIMEOUT, QUEUED_TIMEOUT };
enum { NO_FAULT, SET_FAILURE, STAGED_COMMIT_FAILURE, COMMITTED_TIMEOUT, READ_TIMEOUT };
static atomic_int write_timeout, storage_fault;
static bool staged;

static void ignore_scope(const xiaotai_kv_value_t *values, size_t count, void *ctx)
{
    (void)values;
    (void)ctx;
    CHECK(count == 1U);
}

xiaotai_err_t __real_xiaotai_kv_set_str(const char *space, const char *key, const char *value, uint32_t timeout_ms);
xiaotai_err_t __wrap_xiaotai_kv_set_str(const char *space, const char *key, const char *value, uint32_t timeout_ms)
{
    const int mode = atomic_exchange(&write_timeout, NO_TIMEOUT);
    if (mode != NO_TIMEOUT) {
        CHECK(strcmp(space, SPACE) == 0 && strcmp(key, "pending") == 0 && timeout_ms == 5000);
        xiaotai_kv_host_gate(false);
        if (mode == QUEUED_TIMEOUT) {
            const xiaotai_kv_read_t read = {.kind = XIAOTAI_KV_READ_INT, .key = "scope", .int_type = XIAOTAI_KV_U32};
            CHECK(xiaotai_kv_read_async(SPACE, &read, 1, ignore_scope, NULL) == XIAOTAI_OK);
            CHECK(xiaotai_kv_host_wait_held(WAIT)); /* The write cannot reach the backend yet. */
        }
        return __real_xiaotai_kv_set_str(space, key, value, 100);
    }
    return __real_xiaotai_kv_set_str(space, key, value, timeout_ms);
}

xiaotai_err_t __real_xiaotai_kv_backend_get(xiaotai_kv_handle_t, const char *, bool, void *, size_t *);
xiaotai_err_t __wrap_xiaotai_kv_backend_get(xiaotai_kv_handle_t h, const char *key,
                                           bool text, void *data, size_t *size)
{
    const xiaotai_err_t result = __real_xiaotai_kv_backend_get(h, key, text, data, size);
    if (atomic_load(&storage_fault) != READ_TIMEOUT) return result;
    CHECK(result == XIAOTAI_OK && text && !strcmp(key, "pending"));
    atomic_store(&storage_fault, NO_FAULT);
    return XIAOTAI_ERR_TIMEOUT;
}

xiaotai_err_t __real_xiaotai_kv_backend_set(xiaotai_kv_handle_t, const char *, bool, const void *, size_t);
xiaotai_err_t __wrap_xiaotai_kv_backend_set(xiaotai_kv_handle_t h, const char *key,
                                           bool text, const void *data, size_t size)
{
    const int fault = atomic_load(&storage_fault);
    if (fault == SET_FAILURE || fault == STAGED_COMMIT_FAILURE) {
        CHECK(text && !strcmp(key, "pending") && size == strlen(data) + 1U && !staged);
        if (fault == SET_FAILURE) {
            atomic_store(&storage_fault, NO_FAULT);
            return XIAOTAI_FAIL;
        }
        /* Match the legacy harness: staged bytes are discarded on rejected commit. */
        staged = true;
        return XIAOTAI_OK;
    }
    return __real_xiaotai_kv_backend_set(h, key, text, data, size);
}

xiaotai_err_t __real_xiaotai_kv_backend_commit(xiaotai_kv_handle_t);
xiaotai_err_t __wrap_xiaotai_kv_backend_commit(xiaotai_kv_handle_t h)
{
    const int fault = atomic_exchange(&storage_fault, NO_FAULT);
    if (fault == STAGED_COMMIT_FAILURE) {
        CHECK(staged);
        staged = false;
        return XIAOTAI_FAIL;
    }
    const xiaotai_err_t result = __real_xiaotai_kv_backend_commit(h);
    if (fault == COMMITTED_TIMEOUT) {
        CHECK(result == XIAOTAI_OK);
        return XIAOTAI_ERR_TIMEOUT;
    }
    CHECK(fault == NO_FAULT);
    return result;
}

static void completed(xiaotai_err_t result, xiaotai_recognition_journal_item_state_t item,
                      const xiaotai_recognition_journal_mirror_t *snapshot, void *ctx)
{
    CHECK(xiaotai_on_owner());
    request_t *r = ctx;
    r->result = (result_t){result, item, *snapshot, xiaotai_recognition_journal_busy()};
    xiaotai_signal_give(&r->completed);
}

static void submit(void *ctx)
{
    request_t *r = ctx;
    r->job.done = completed;
    r->job.ctx = r;
    r->submit_result = xiaotai_recognition_journal_submit(&r->job);
    r->state_after_submit = xiaotai_recognition_journal_mirror()->state;
    r->busy_after_submit = xiaotai_recognition_journal_busy();
    xiaotai_signal_give(&r->submitted);
}

static void enqueue(request_t *r, xiaotai_err_t expected)
{
    CHECK(xiaotai_signal_init(&r->submitted) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&r->completed) == XIAOTAI_OK);
    CHECK(xiaotai_run(submit, r) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&r->submitted, WAIT));
    CHECK(r->submit_result == expected);
}

static result_t finish(request_t *r)
{
    if (r->submit_result == XIAOTAI_OK) CHECK(xiaotai_signal_take(&r->completed, WAIT));
    else CHECK(!xiaotai_signal_take(&r->completed, 0));
    xiaotai_signal_deinit(&r->completed);
    xiaotai_signal_deinit(&r->submitted);
    return r->result;
}

static result_t call(xiaotai_recognition_journal_operation_t operation,
    const xiaotai_recognition_journal_record_t *record, xiaotai_err_t expected)
{
    request_t r = {.job = {.operation = operation}};
    if (record) r.job.record = *record;
    enqueue(&r, XIAOTAI_OK);
    result_t result = finish(&r);
    CHECK(result.result == expected);
    return result;
}

static result_t item(xiaotai_recognition_journal_operation_t operation,
    const xiaotai_recognition_journal_record_t *record, const char *id,
    xiaotai_recognition_journal_item_state_t from, xiaotai_recognition_journal_item_state_t to,
    xiaotai_err_t submit_result, xiaotai_err_t expected)
{
    request_t r = {.job = {.operation = operation, .record = *record, .expected_item = from, .next_item = to}};
    CHECK(strlen(id) < sizeof(r.job.item_id));
    strcpy(r.job.item_id, id);
    enqueue(&r, submit_result);
    result_t result = finish(&r);
    CHECK(submit_result != XIAOTAI_OK || result.result == expected);
    return result;
}

static void seed_scope(uint32_t scope)
{
    CHECK(xiaotai_kv_set_int(SPACE, "scope", XIAOTAI_KV_U32, scope, WAIT) == XIAOTAI_OK);
}

static void seed_pending(const char *text)
{
    CHECK(xiaotai_kv_set_str(SPACE, "pending", text, WAIT) == XIAOTAI_OK);
}

static void stored_is(const char *text)
{
    char buf[2048];
    CHECK(xiaotai_kv_get_str(SPACE, "pending", buf, sizeof(buf), WAIT) == XIAOTAI_OK);
    CHECK(strcmp(buf, text) == 0);
}

static const xiaotai_recognition_journal_record_t record = {
    .device_id = "device", .request_id = "request", .config_revision = INT64_C(9007199254740991), .seconds = 60,
};

static const char v1[] =
    "{\"version\":1,\"device_id\":\"device\",\"request_id\":\"request\","
    "\"revision\":9007199254740991,\"seconds\":60}";

static void test_deployed_v1(void)
{
    xiaotai_kv_host_clear();
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_ERR_NOT_FOUND).mirror.state == RJ(ABSENT));
    seed_pending(v1);
    result_t r = call(RJ(LOAD), NULL, XIAOTAI_OK);
    CHECK(r.mirror.state == RJ(PRESENT) && r.mirror.version == 1 && r.mirror.record.scope == 0);
    CHECK(r.mirror.record.config_revision == record.config_revision);
    CHECK(item(RJ(ITEM_READ), &record, "first", 0, 0, XIAOTAI_OK, XIAOTAI_OK).item == RJ(UNTOUCHED));
    call(RJ(MARK_CONFLICT), &record, XIAOTAI_ERR_INVALID_STATE);
    item(RJ(ITEM_TRANSITION), &record, "first", RJ(UNTOUCHED), RJ(PLAYED), XIAOTAI_ERR_INVALID_ARG, 0);
    item(RJ(ITEM_TRANSITION), &record, "first", RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_OK, XIAOTAI_OK);
    item(RJ(ITEM_TRANSITION), &record, "first", RJ(UNTOUCHED), RJ(DISMISSED), XIAOTAI_OK, XIAOTAI_ERR_INVALID_STATE);
    item(RJ(ITEM_TRANSITION), &record, "first", RJ(PLAY_STARTED), RJ(PLAYED), XIAOTAI_OK, XIAOTAI_OK);
    item(RJ(ITEM_TRANSITION), &record, "second", RJ(UNTOUCHED), RJ(DISMISSED), XIAOTAI_OK, XIAOTAI_OK);
    stored_is("{\"version\":3,\"device_id\":\"device\",\"request_id\":\"request\","
              "\"revision\":9007199254740991,\"seconds\":60,\"scope\":0,\"items\":["
              "{\"item_id\":\"first\",\"state\":2},{\"item_id\":\"second\",\"state\":3}]}");

    xiaotai_recognition_journal_host_stop();
    CHECK(xiaotai_recognition_journal_start() == XIAOTAI_OK);
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_OK).mirror.version == 3);
    CHECK(item(RJ(ITEM_READ), &record, "first", 0, 0, XIAOTAI_OK, XIAOTAI_OK).item == RJ(PLAYED));
    CHECK(item(RJ(ITEM_READ), &record, "second", 0, 0, XIAOTAI_OK, XIAOTAI_OK).item == RJ(DISMISSED));
    for (int i = 0; i < 6; ++i) {
        char id[] = "item-0";
        id[5] = (char)('0' + i);
        item(RJ(ITEM_TRANSITION), &record, id, RJ(UNTOUCHED), RJ(DISMISSED), XIAOTAI_OK, XIAOTAI_OK);
    }
    item(RJ(ITEM_TRANSITION), &record, "ninth", RJ(UNTOUCHED), RJ(DISMISSED), XIAOTAI_OK, XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_recognition_journal_record_t other = record;
    --other.config_revision;
    CHECK(call(RJ(CLEAR), &other, XIAOTAI_ERR_INVALID_STATE).mirror.state == RJ(PRESENT));
    CHECK(call(RJ(CLEAR), &record, XIAOTAI_OK).mirror.state == RJ(ABSENT));
}

static void test_deployed_v2_v4_and_q7(void)
{
    xiaotai_kv_host_clear();
    const uint32_t scope = UINT32_C(4294967201);
    seed_scope(scope);
    char text[2048];
    int n = snprintf(text, sizeof(text), "{\"version\":2,\"device_id\":\"device\","
        "\"request_id\":\"request\",\"revision\":9007199254740991,\"seconds\":60,\"scope\":%u}", scope);
    CHECK(n > 0 && (size_t)n < sizeof(text) - 1);
    memset(text + n, ' ', sizeof(text) - (size_t)n - 1);
    text[sizeof(text) - 1] = '\0';
    seed_pending(text);
    result_t r = call(RJ(LOAD), NULL, XIAOTAI_OK);
    CHECK(r.mirror.version == 2 && r.mirror.record.scope == scope);
    CHECK(r.mirror.record.config_revision == record.config_revision);
    xiaotai_recognition_journal_record_t old = r.mirror.record;
    stored_is(text);
    CHECK(call(RJ(MARK_CONFLICT), &old, XIAOTAI_OK).mirror.version == 4);
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    CHECK(call(RJ(MARK_CONFLICT), &old, XIAOTAI_OK).mirror.version == 4);
    xiaotai_kv_host_counts(&after);
    CHECK(before.writes == after.writes);

    xiaotai_recognition_journal_host_stop();
    CHECK(xiaotai_recognition_journal_start() == XIAOTAI_OK);
    const result_t before_transition = call(RJ(LOAD), NULL, XIAOTAI_OK);
    CHECK(before_transition.mirror.version == 4);
    r = item(RJ(ITEM_TRANSITION), &old, "known-task", RJ(UNTOUCHED), RJ(DISMISSED), XIAOTAI_OK, XIAOTAI_OK);
    CHECK(r.mirror.version == 3);
    CHECK(call(RJ(CLEAR_CONFLICT), &before_transition.mirror.record, XIAOTAI_ERR_INVALID_STATE).mirror.version == 3);
    CHECK(call(RJ(CLEAR), &before_transition.mirror.record, XIAOTAI_OK).mirror.state == RJ(ABSENT));

    CHECK(call(RJ(PREPARE), &record, XIAOTAI_OK).mirror.record.scope == scope);
    CHECK(call(RJ(MARK_CONFLICT), &old, XIAOTAI_OK).mirror.version == 4);
    CHECK(call(RJ(CLEAR_CONFLICT), &old, XIAOTAI_OK).mirror.state == RJ(ABSENT));
    int64_t remaining_scope = -1;
    CHECK(xiaotai_kv_get_int(SPACE, "scope", XIAOTAI_KV_U32, &remaining_scope, WAIT) == XIAOTAI_OK);
    CHECK(remaining_scope == scope);
}

static void test_scope(void)
{
    xiaotai_kv_host_clear();
    seed_pending(v1);
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    CHECK(call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_OK).mirror.state == RJ(UNREAD));
    xiaotai_kv_host_counts(&after);
    CHECK(after.opens - before.opens == 2 && after.writes - before.writes == 1);
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_ERR_NOT_FOUND).mirror.state == RJ(ABSENT));
    CHECK(call(RJ(CLEAR), &record, XIAOTAI_OK).mirror.state == RJ(ABSENT));
    stored_is(v1);
    CHECK(call(RJ(PREPARE), &record, XIAOTAI_OK).mirror.record.scope == 1);
    CHECK(call(RJ(CLEAR), &record, XIAOTAI_ERR_INVALID_STATE).mirror.state == RJ(PRESENT));
    seed_scope(UINT32_MAX);
    CHECK(call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_ERR_INVALID_STATE).mirror.state == RJ(UNREAD));
    int64_t scope = 0;
    CHECK(xiaotai_kv_get_int(SPACE, "scope", XIAOTAI_KV_U32, &scope, WAIT) == XIAOTAI_OK && scope == UINT32_MAX);
}

static void test_scope_retirement_fences(void)
{
    xiaotai_kv_host_clear();
    seed_pending(v1);
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_OK).mirror.state == RJ(PRESENT));
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    CHECK(call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_OK).mirror.state == RJ(ABSENT));
    xiaotai_kv_host_counts(&after);
    CHECK(after.opens == before.opens + 2U && after.writes == before.writes + 1U &&
          after.commits == before.commits + 1U);
    stored_is(v1);

    xiaotai_kv_host_clear();
    const char future[] = "{\"version\":2,\"device_id\":\"device\",\"request_id\":\"future\","
                          "\"revision\":1,\"seconds\":1,\"scope\":1}";
    seed_pending(future);
    CHECK(call(RJ(LOAD), NULL, RJ(INVALID_RESPONSE)).mirror.state == RJ(ERROR));
    CHECK(call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_OK).mirror.state == RJ(UNREAD));
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_OK).mirror.record.scope == 1U);
    stored_is(future);

    xiaotai_kv_host_clear();
    seed_pending(v1);
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_OK).mirror.record.scope == 0U);
    seed_scope(1U); /* The durable previous scope no longer matches the owner mirror. */
    CHECK(call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_OK).mirror.state == RJ(UNREAD));
    stored_is(v1);

    xiaotai_kv_host_clear();
    seed_pending(v1);
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_OK).mirror.state == RJ(PRESENT));
    xiaotai_kv_host_fail_key("scope", XIAOTAI_FAIL);
    CHECK(call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_FAIL).mirror.state == RJ(ERROR));
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    CHECK(call(RJ(LOAD), NULL, XIAOTAI_OK).mirror.state == RJ(PRESENT));
    stored_is(v1);

    xiaotai_kv_host_gate(false);
    request_t advance = {.job = {.operation = RJ(ADVANCE_SCOPE)}};
    request_t prepare = {.job = {.operation = RJ(PREPARE), .record = record}};
    strcpy(prepare.job.record.request_id, "after-advance");
    enqueue(&advance, XIAOTAI_OK);
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    enqueue(&prepare, XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    const result_t earlier = finish(&advance);
    CHECK(earlier.result == XIAOTAI_OK && earlier.still_busy && earlier.mirror.state == RJ(UNREAD));
    const result_t later = finish(&prepare);
    CHECK(later.result == XIAOTAI_OK && !later.still_busy && later.mirror.state == RJ(PRESENT));
    CHECK(later.mirror.record.scope == 1U && !strcmp(later.mirror.record.request_id, "after-advance"));
}

static void test_maximum_deployed_record(void)
{
    xiaotai_kv_host_clear();
    xiaotai_recognition_journal_record_t maximum = record;
    memset(maximum.device_id, 'd', sizeof(maximum.device_id) - 1U);
    memset(maximum.request_id, 'r', sizeof(maximum.request_id) - 1U);
    CHECK(strlen(maximum.device_id) == 64 && strlen(maximum.request_id) == 64);
    call(RJ(PREPARE), &maximum, XIAOTAI_OK);
    char id[65] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        memset(id, 'a' + (int)i, sizeof(id) - 1U);
        item(RJ(ITEM_TRANSITION), &maximum, id, RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_OK, XIAOTAI_OK);
    }
    char pending[2048];
    CHECK(xiaotai_kv_get_str(SPACE, "pending", pending, sizeof(pending), WAIT) == XIAOTAI_OK);
    CHECK(strlen(pending) > 512 && strlen(pending) < sizeof(pending));
    item(RJ(ITEM_TRANSITION), &maximum, "ninth", RJ(UNTOUCHED), RJ(PLAY_STARTED),
         XIAOTAI_OK, XIAOTAI_ERR_INVALID_SIZE);
    stored_is(pending);
    xiaotai_recognition_journal_host_stop();
    CHECK(xiaotai_recognition_journal_start() == XIAOTAI_OK);
    for (unsigned i = 0; i < 8; ++i) {
        memset(id, 'a' + (int)i, sizeof(id) - 1U);
        const bool dismiss = i == 7;
        CHECK(item(RJ(ITEM_READ), &maximum, id, 0, 0, XIAOTAI_OK, XIAOTAI_OK).item == RJ(PLAY_STARTED));
        item(RJ(ITEM_TRANSITION), &maximum, id, RJ(PLAY_STARTED), dismiss ? RJ(DISMISSED) : RJ(PLAYED),
             XIAOTAI_OK, XIAOTAI_OK);
        CHECK(item(RJ(ITEM_READ), &maximum, id, 0, 0, XIAOTAI_OK, XIAOTAI_OK).item ==
              (dismiss ? RJ(DISMISSED) : RJ(PLAYED)));
    }
    item(RJ(ITEM_READ), &maximum, "invalid/id", 0, 0, XIAOTAI_ERR_INVALID_ARG, 0);
    call(RJ(CLEAR), &maximum, XIAOTAI_OK);
    item(RJ(ITEM_READ), &maximum, id, 0, 0, XIAOTAI_OK, XIAOTAI_ERR_NOT_FOUND);
}

static void test_corruption(void)
{
    xiaotai_kv_host_clear();
    const char *bad[] = {
        "{\"", "{}", "[]",
        "{\"version\":2,\"device_id\":\"device\",\"request_id\":\"request\",\"revision\":1,\"seconds\":60,\"scope\":1}",
        "{\"version\":1,\"device_id\":\"device\",\"request_id\":\"request\",\"revision\":9007199254740992,\"seconds\":60}",
        "{\"version\":1,\"device_id\":\"device\\u0000\",\"request_id\":\"request\",\"revision\":0,\"seconds\":60}",
        "{\"version\":1,\"device_id\":\"device\",\"device_id\":\"duplicate\",\"revision\":0,\"seconds\":60}",
        "{\"version\":1,\"device_id\":\"device\",\"request_id\":\"request\",\"revision\":0.5,\"seconds\":60}",
        "{\"version\":3,\"device_id\":\"device\",\"request_id\":\"request\",\"revision\":0,\"seconds\":1,\"scope\":0,"
         "\"items\":[{\"item_id\":\"a\",\"state\":1},{\"item_id\":\"\\u0061\",\"state\":2}]}",
        "{\"version\":3,\"device_id\":\"device\",\"request_id\":\"request\",\"revision\":0,\"seconds\":1,\"scope\":0,"
         "\"items\":[{\"item_id\":\"a\",\"state\":{\"too_deep\":1}}]}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        seed_pending(bad[i]);
        result_t r = call(RJ(LOAD), NULL, RJ(INVALID_RESPONSE));
        CHECK(r.mirror.state == RJ(ERROR) && r.mirror.error == RJ(INVALID_RESPONSE));
        call(RJ(PREPARE), &record, RJ(INVALID_RESPONSE));
        call(RJ(CLEAR), &record, RJ(INVALID_RESPONSE));
        stored_is(bad[i]);
    }
    seed_pending(v1);
    const xiaotai_err_t read_failures[] = {XIAOTAI_FAIL, XIAOTAI_ERR_INVALID_SIZE};
    for (size_t i = 0; i < sizeof(read_failures)/sizeof(read_failures[0]); ++i) {
        xiaotai_kv_host_fail_key("pending", read_failures[i]);
        CHECK(call(RJ(PREPARE), &record, read_failures[i]).mirror.error == read_failures[i]);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        stored_is(v1);
    }
    xiaotai_kv_host_fail_key("scope", XIAOTAI_FAIL);
    CHECK(call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_FAIL).mirror.error == XIAOTAI_FAIL);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
}

static void test_serialization_and_pending(void)
{
    xiaotai_kv_host_clear();
    xiaotai_kv_host_gate(false);
    request_t jobs[] = {
        {.job = {.operation = RJ(LOAD)}},
        {.job = {.operation = RJ(PREPARE), .record = record}},
        {.job = {.operation = RJ(PREPARE), .record = record}},
        {.job = {.operation = RJ(LOAD)}},
        {.job = {.operation = RJ(LOAD)}},
        {.job = {.operation = RJ(LOAD)}},
    };
    strcpy(jobs[2].job.record.request_id, "second-request");
    enqueue(&jobs[0], XIAOTAI_OK);
    CHECK(jobs[0].state_after_submit == RJ(READING) && jobs[0].busy_after_submit);
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    for (size_t i = 1; i < 5; ++i) enqueue(&jobs[i], XIAOTAI_OK);
    enqueue(&jobs[5], XIAOTAI_ERR_NO_MEM);
    CHECK(jobs[5].state_after_submit == RJ(READING) && jobs[5].busy_after_submit);
    finish(&jobs[5]);
    xiaotai_kv_host_gate(true);

    const result_t absent = finish(&jobs[0]);
    CHECK(absent.result == XIAOTAI_ERR_NOT_FOUND && absent.mirror.state == RJ(ABSENT) && absent.still_busy);
    const result_t prepared = finish(&jobs[1]);
    CHECK(prepared.result == XIAOTAI_OK && prepared.mirror.state == RJ(PRESENT) && prepared.still_busy);
    const result_t rejected = finish(&jobs[2]);
    CHECK(rejected.result == XIAOTAI_ERR_INVALID_STATE && rejected.mirror.state == RJ(PRESENT));
    CHECK(strcmp(rejected.mirror.record.request_id, "request") == 0);
    CHECK(finish(&jobs[3]).result == XIAOTAI_OK);
    CHECK(!finish(&jobs[4]).still_busy);
}

static void test_active_write_timeout(void)
{
    xiaotai_kv_host_clear();
    atomic_store(&write_timeout, ACTIVE_TIMEOUT);
    const result_t timed_out = call(RJ(PREPARE), &record, XIAOTAI_ERR_TIMEOUT);
    CHECK(timed_out.mirror.state == RJ(ERROR) && timed_out.mirror.error == XIAOTAI_ERR_TIMEOUT);
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    request_t reload = {.job = {.operation = RJ(LOAD)}};
    enqueue(&reload, XIAOTAI_OK);
    CHECK(reload.state_after_submit == RJ(READING) && reload.busy_after_submit);
    xiaotai_kv_host_gate(true);
    const result_t late = finish(&reload);
    CHECK(late.result == XIAOTAI_OK && late.mirror.version == 2 && late.mirror.state == RJ(PRESENT));
    CHECK(strcmp(late.mirror.record.request_id, "request") == 0);
}

static void check_unread(void *ctx)
{
    CHECK(xiaotai_recognition_journal_mirror()->state == RJ(UNREAD));
    CHECK(!xiaotai_recognition_journal_busy());
    const xiaotai_recognition_journal_job_t no_callback = {
        .operation = RJ(ITEM_READ), .record = record, .item_id = "item",
    };
    CHECK(xiaotai_recognition_journal_submit(NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_recognition_journal_submit(&no_callback) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(!xiaotai_recognition_journal_busy());
    xiaotai_signal_give(ctx);
}

static const char v2[] =
    "{\"version\":2,\"device_id\":\"device\",\"request_id\":\"request\","
    "\"revision\":9007199254740991,\"seconds\":60,\"scope\":0}";

static result_t move(const xiaotai_recognition_journal_record_t *r, const char *id,
                      xiaotai_recognition_journal_item_state_t from,
                      xiaotai_recognition_journal_item_state_t to, xiaotai_err_t expected)
{
    return item(RJ(ITEM_TRANSITION), r, id, from, to, XIAOTAI_OK, expected);
}

static xiaotai_recognition_journal_item_state_t read_state(const char *id)
{
    return item(RJ(ITEM_READ), &record, id, 0, 0, XIAOTAI_OK, XIAOTAI_OK).item;
}

static void test_legacy_cas(void)
{
    for (unsigned version = 1; version <= 2; ++version) {
        xiaotai_kv_host_clear();
        const char *old = version == 1U ? v1 : v2;
        seed_pending(old);
        CHECK(read_state("item") == RJ(UNTOUCHED));
        stored_is(old);
        CHECK(move(&record, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_OK).mirror.version == 3U);
        char started[2048];
        CHECK(xiaotai_kv_get_str(SPACE, "pending", started, sizeof(started), WAIT) == XIAOTAI_OK);
        xiaotai_kv_host_counts_t before, after;
        xiaotai_kv_host_counts(&before);
        move(&record, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_ERR_INVALID_STATE);
        for (unsigned field = 0; field < 5; ++field) {
            xiaotai_recognition_journal_record_t wrong = record;
            if (field == 0U) strcpy(wrong.device_id, "other");
            if (field == 1U) strcpy(wrong.request_id, "another");
            if (field == 2U) --wrong.config_revision;
            if (field == 3U) --wrong.seconds;
            if (field == 4U) ++wrong.scope;
            move(&wrong, "item", RJ(PLAY_STARTED), RJ(PLAYED), XIAOTAI_ERR_INVALID_STATE);
        }
        xiaotai_kv_host_counts(&after);
        CHECK(before.writes == after.writes && before.commits == after.commits);
        stored_is(started);
        move(&record, "item", RJ(PLAY_STARTED), RJ(DISMISSED), XIAOTAI_OK);
        CHECK(read_state("item") == RJ(DISMISSED));
    }
    call(RJ(ADVANCE_SCOPE), NULL, XIAOTAI_OK);
    CHECK(item(RJ(ITEM_READ), &record, "item", 0, 0, XIAOTAI_OK, XIAOTAI_ERR_NOT_FOUND).item ==
          RJ(UNTOUCHED));
    CHECK(call(RJ(PREPARE), &record, XIAOTAI_OK).mirror.record.scope == 1U);
    move(&record, "item", RJ(PLAY_STARTED), RJ(PLAYED), XIAOTAI_ERR_INVALID_STATE);
    xiaotai_recognition_journal_record_t replacement = record;
    replacement.scope = 1U;
    CHECK(item(RJ(ITEM_READ), &replacement, "item", 0, 0, XIAOTAI_OK, XIAOTAI_OK).item == RJ(UNTOUCHED));
    item(RJ(ITEM_TRANSITION), &replacement, "item", RJ(PLAYED), RJ(PLAY_STARTED), XIAOTAI_ERR_INVALID_ARG, 0);
    call(RJ(CLEAR), &replacement, XIAOTAI_OK);
    move(&replacement, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_ERR_NOT_FOUND);

    request_t invalid = {.job = {.operation = RJ(ITEM_READ)}};
    strcpy(invalid.job.item_id, "item"); /* No record identity. */
    enqueue(&invalid, XIAOTAI_ERR_INVALID_ARG);
    finish(&invalid);
    invalid.job.record = record;
    memset(invalid.job.item_id, 'a', sizeof(invalid.job.item_id));
    enqueue(&invalid, XIAOTAI_ERR_INVALID_ARG);
    finish(&invalid);
    item(RJ(ITEM_READ), &record, "", 0, 0, XIAOTAI_ERR_INVALID_ARG, 0);
}

static void reject_legacy_item_record(const char *saved, xiaotai_err_t expected)
{
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    const result_t r = item(RJ(ITEM_READ), &record, "item", 0, 0, XIAOTAI_OK, expected);
    CHECK(r.item == RJ(UNTOUCHED) && r.mirror.state == RJ(ERROR) && !r.mirror.record.device_id[0]);
    move(&record, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), expected);
    CHECK(call(RJ(LOAD), NULL, expected).mirror.state == RJ(ERROR));
    xiaotai_kv_host_counts(&after);
    CHECK(before.writes == after.writes && before.commits == after.commits);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    stored_is(saved);
}

static void test_legacy_bad_items(void)
{
    static const char *const bad[] = {
        "null", "{}", "[null]", "[{}]", "[{\"item_id\":\"item\",\"state\":0}]",
        "[{\"item_id\":\"item\",\"state\":4}]", "[{\"item_id\":\"item\",\"state\":-1}]",
        "[{\"item_id\":\"item\",\"state\":1.5}]", "[{\"item_id\":\"item\",\"state\":\"1\"}]",
        "[{\"item_id\":\"item\",\"state\":true}]", "[{\"item_id\":\"bad/id\",\"state\":1}]",
        "[{\"item_id\":\"\",\"state\":1}]", "[{\"item_id\":\"item\\u0000x\",\"state\":1}]",
        "[{\"item_id\":\"item\",\"state\":1,\"extra\":0}]",
        "[{\"item_id\":\"item\",\"item_id\":\"item\"}]",
        "[{\"item_id\":\"item\",\"state\":1},{\"item_id\":\"item\",\"state\":2}]",
        "[{\"item_id\":\"item\",\"state\":{\"nested\":1}}]",
    };
    static const char prefix[] = "{\"version\":3,\"device_id\":\"device\",\"request_id\":\"request\","
                                 "\"revision\":9007199254740991,\"seconds\":60,\"scope\":0,\"items\":";
    xiaotai_kv_host_clear();
    char raw[2048];
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        const int len = snprintf(raw, sizeof(raw), "%s%s}", prefix, bad[i]);
        CHECK(len > 0 && (size_t)len < sizeof(raw));
        seed_pending(raw);
        reject_legacy_item_record(raw, RJ(INVALID_RESPONSE));
    }
    for (unsigned i = 0; i < 2U; ++i) {
        size_t at = strlen(prefix);
        memcpy(raw, prefix, at);
        if (i == 0U) {
            raw[at++] = '[';
            for (unsigned item_index = 0; item_index < 9U; ++item_index) {
                const int len = snprintf(raw + at, sizeof(raw) - at, "%s{\"item_id\":\"item%u\",\"state\":1}",
                                         item_index ? "," : "", item_index);
                CHECK(len > 0 && (size_t)len < sizeof(raw) - at);
                at += (size_t)len;
            }
            raw[at++] = ']';
        } else {
            memset(raw + at, '[', 100U);
            at += 100U;
            raw[at++] = '0';
            memset(raw + at, ']', 100U);
            at += 100U;
        }
        raw[at++] = '}';
        raw[at] = '\0';
        seed_pending(raw);
        reject_legacy_item_record(raw, RJ(INVALID_RESPONSE));
    }
    seed_pending("{broken");
    reject_legacy_item_record("{broken", RJ(INVALID_RESPONSE));
    seed_pending(v2);
    xiaotai_kv_host_fail_key("pending", XIAOTAI_ERR_INVALID_SIZE);
    reject_legacy_item_record(v2, XIAOTAI_ERR_INVALID_SIZE); /* Stored payload exceeds the 2048-byte buffer. */
}

static void test_legacy_storage_faults(void)
{
    const int faults[] = {NO_FAULT, NO_FAULT, SET_FAILURE, STAGED_COMMIT_FAILURE, COMMITTED_TIMEOUT};
    for (size_t i = 0; i < sizeof(faults) / sizeof(*faults); ++i) {
        xiaotai_kv_host_clear();
        call(RJ(PREPARE), &record, XIAOTAI_OK);
        if (i == 0U) xiaotai_kv_host_fail(XIAOTAI_FAIL);
        if (i == 1U) xiaotai_kv_host_fail_key("pending", XIAOTAI_FAIL);
        atomic_store(&storage_fault, faults[i]);
        const bool committed = faults[i] == COMMITTED_TIMEOUT;
        const xiaotai_err_t expected = committed ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL;
        const result_t failure = move(&record, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), expected);
        CHECK(failure.mirror.state == RJ(ERROR) && failure.mirror.error == expected);
        CHECK(atomic_load(&storage_fault) == NO_FAULT && !staged);
        xiaotai_kv_host_fail(XIAOTAI_OK);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        CHECK(read_state("item") == (committed ? RJ(PLAY_STARTED) : RJ(UNTOUCHED)));
        if (!committed) stored_is(v2);
    }

    xiaotai_kv_host_clear();
    call(RJ(PREPARE), &record, XIAOTAI_OK);
    atomic_store(&write_timeout, QUEUED_TIMEOUT);
    CHECK(move(&record, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_ERR_TIMEOUT).mirror.state == RJ(ERROR));
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT) == XIAOTAI_OK);
    CHECK(read_state("item") == RJ(UNTOUCHED));
    stored_is(v2);

    atomic_store(&write_timeout, ACTIVE_TIMEOUT);
    CHECK(move(&record, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_ERR_TIMEOUT).mirror.state == RJ(ERROR));
    CHECK(xiaotai_kv_host_wait_held(WAIT));
    xiaotai_kv_host_gate(true);
    CHECK(xiaotai_kv_flush(WAIT) == XIAOTAI_OK);
    CHECK(read_state("item") == RJ(PLAY_STARTED));

    atomic_store(&storage_fault, COMMITTED_TIMEOUT);
    CHECK(move(&record, "item", RJ(PLAY_STARTED), RJ(PLAYED), XIAOTAI_ERR_TIMEOUT).mirror.state == RJ(ERROR));
    CHECK(read_state("item") == RJ(PLAYED));
    move(&record, "item", RJ(PLAY_STARTED), RJ(PLAYED), XIAOTAI_ERR_INVALID_STATE);
    atomic_store(&storage_fault, READ_TIMEOUT);
    const result_t missing = item(RJ(ITEM_READ), &record, "item", 0, 0, XIAOTAI_OK, XIAOTAI_ERR_TIMEOUT);
    CHECK(missing.item == RJ(UNTOUCHED) && missing.mirror.state == RJ(ERROR));
    CHECK(atomic_load(&storage_fault) == NO_FAULT && read_state("item") == RJ(PLAYED));
}

static void test_legacy_allocation_faults(void)
{
    for (unsigned edge = 0; edge < 3U; ++edge) {
        unsigned failed = 0;
        bool exhausted = false;
        for (unsigned nth = 1; nth <= 16U; ++nth) {
            xiaotai_kv_host_clear();
            call(RJ(PREPARE), &record, XIAOTAI_OK);
            const char *const id = edge == 2U ? "second" : "item";
            if (edge != 0U) move(&record, "item", RJ(UNTOUCHED), RJ(PLAY_STARTED), XIAOTAI_OK);
            char saved[2048];
            CHECK(xiaotai_kv_get_str(SPACE, "pending", saved, sizeof(saved), WAIT) == XIAOTAI_OK);
            const int64_t initial = xiaotai_mem_host_outstanding();
            const xiaotai_recognition_journal_item_state_t from = edge == 1U ? RJ(PLAY_STARTED) : RJ(UNTOUCHED);
            const xiaotai_recognition_journal_item_state_t to = edge == 0U ? RJ(PLAY_STARTED) :
                                                               edge == 1U ? RJ(PLAYED) : RJ(DISMISSED);
            request_t request = {.job = {.operation = RJ(ITEM_TRANSITION), .record = record,
                                           .expected_item = from, .next_item = to}};
            strcpy(request.job.item_id, id);
            xiaotai_mem_host_fail_nth(nth);
            enqueue(&request, XIAOTAI_OK);
            const result_t r = finish(&request);
            const bool triggered = !xiaotai_mem_host_fail_pending();
            xiaotai_mem_host_fail_nth(0);
            CHECK(xiaotai_mem_host_outstanding() == initial);
            if (!triggered) {
                CHECK(r.result == XIAOTAI_OK && read_state(id) == to);
                exhausted = true;
                break;
            }
            CHECK(r.result == XIAOTAI_ERR_NO_MEM && r.mirror.state == RJ(ERROR));
            stored_is(saved);
            CHECK(read_state(id) == from);
            ++failed;
        }
        CHECK(exhausted && failed >= 2U);
        printf("Legacy journal transition %u: %u allocation failures checked\n", edge, failed);
    }
}

int main(void)
{
    const xiaotai_framework_config_t framework = {0};
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    CHECK(xiaotai_recognition_journal_start() == XIAOTAI_OK);
    CHECK(xiaotai_recognition_journal_start() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_t initial;
    CHECK(xiaotai_signal_init(&initial) == XIAOTAI_OK);
    CHECK(xiaotai_run(check_unread, &initial) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&initial, WAIT));
    xiaotai_signal_deinit(&initial);
    test_deployed_v1();
    test_deployed_v2_v4_and_q7();
    test_scope();
    test_scope_retirement_fences();
    test_maximum_deployed_record();
    test_corruption();
    test_serialization_and_pending();
    test_active_write_timeout();
    test_legacy_cas();
    test_legacy_bad_items();
    test_legacy_storage_faults();
    test_legacy_allocation_faults();
    xiaotai_recognition_journal_host_stop();
    xiaotai_framework_reset();
    return 0;
}
