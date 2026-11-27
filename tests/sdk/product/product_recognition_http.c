// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_runtime.py
// Replaces legacy: tests/regression/test_recognition_task.py
// Replaces legacy: tests/regression/test_recognition_operation.py
#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "product_platform.h"
#include "recognition_result.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

void product_recognition_http_prepare(void);
void product_recognition_http(void);
void product_recognition_http_cleanup(void);

enum { WAIT_MS = 6000, MAX_BODY = 1024 * 1024 };
enum { PAYLOAD, QUEUED, CONNECT_FAIL, RECEIVE_RESET, TOO_LARGE, CHUNKED, CANCEL_QUEUED,
       QUEUE_21S, QUEUE_26S, HEAD_FAIL, EARLY_EOF, HEAD_STALL, BODY_STALL, WIRE_DEADLINE,
       NOTICE_VIEW_FIRST, NOTICE_VIEW_REPLACE, NOTICE_DOC_FIRST, NOTICE_DOC_REPLACE, PARTIAL_EOF };
static const char discovery[] =
    "{\"device-srv\":\"http://fixture.invalid/device\",\"mqtt-srv\":\"mqtts://fixture.invalid\","
    "\"ai-srv\":\"http://fixture.invalid/ai\",\"call-srv\":\"http://fixture.invalid/call\","
    "\"voip-srv\":\"http://fixture.invalid/voip\",\"tirtc-srv\":\"http://fixture.invalid/rtc\","
    "\"recognition-srv\":\"http://recognition.invalid:8081/tenant\"}";
static const char journal[] = "{\"version\":1,\"device_id\":\"" PRODUCT_FIXTURE_DEVICE
    "\",\"request_id\":\"task_1\",\"revision\":1,\"seconds\":1}";
static uint8_t *body;
static size_t body_bytes;
static int http, mode;
static uint64_t revision;
static unsigned items, state;
static atomic_uint requests, cleared;
static atomic_uint result_events, next_result_fault, injected_result_faults;
static _Thread_local unsigned internal_allocations_until_fault;
static char retry_body[512];
static atomic_int_fast64_t clock_delta_us;
static xiaotai_signal_t entered, release;

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
bool product_recognition_view_fail_allocation(xiaotai_mem_kind_t kind);
xiaotai_err_t __real_rc_result_adopt(rc_task_t *, uint32_t, const xiaotai_recognition_journal_record_t *,
                                     const xiaotai_recognition_journal_mirror_t *);
xiaotai_err_t __wrap_rc_result_adopt(rc_task_t *, uint32_t, const xiaotai_recognition_journal_record_t *,
                                     const xiaotai_recognition_journal_mirror_t *);
xiaotai_err_t __real_xiaotai_event_emit(const char *, const xiaotai_args_t *, const void *, size_t);
xiaotai_err_t __wrap_xiaotai_event_emit(const char *, const xiaotai_args_t *, const void *, size_t);

int64_t __wrap_xiaotai_time_us(void)
{
    return __real_xiaotai_time_us() + atomic_load(&clock_delta_us);
}

bool product_recognition_view_fail_allocation(xiaotai_mem_kind_t kind)
{
    if (kind != XIAOTAI_MEM_INTERNAL || !internal_allocations_until_fault || --internal_allocations_until_fault)
        return false;
    atomic_fetch_add(&injected_result_faults, 1U);
    return true;
}

xiaotai_err_t __wrap_rc_result_adopt(rc_task_t *task, uint32_t generation,
    const xiaotai_recognition_journal_record_t *record, const xiaotai_recognition_journal_mirror_t *mirror)
{
    const unsigned fault = atomic_exchange(&next_result_fault, 0);
    if (fault) assert(xiaotai_on_owner() && (fault == 1U || fault == 2U));
    internal_allocations_until_fault = fault;
    const xiaotai_err_t result = __real_rc_result_adopt(task, generation, record, mirror);
    if (fault) assert(!internal_allocations_until_fault && result == XIAOTAI_ERR_NO_MEM);
    return result;
}

xiaotai_err_t __wrap_xiaotai_event_emit(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    if (name && !strcmp(name, XIAOTAI_EVENT_RECOGNITION_RESULT)) atomic_fetch_add(&result_events, 1U);
    return __real_xiaotai_event_emit(name, args, data, size);
}

static bool result_fault_case(void) { return mode >= NOTICE_VIEW_FIRST && mode <= NOTICE_DOC_REPLACE; }
static bool with_nothing_to_publish(void) { return mode == NOTICE_VIEW_FIRST || mode == NOTICE_DOC_FIRST; }

static bool queues_behind_another_request(void)
{
    return mode == QUEUED || mode == CANCEL_QUEUED || mode == QUEUE_21S || mode == QUEUE_26S;
}

static void server(void *ctx, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    if (!strcmp(req->url, "http://fixture.invalid/device/hold-recognition-queue")) {
        assert(queues_behind_another_request() && req->method == XIAOTAI_HTTP_GET);
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&release, WAIT_MS));
        return;
    }
    if (!strcmp(req->url, "http://recognition.invalid:8081/tenant" XIAOTAI_RECOGNITION_CONFIG_PATH)) {
        reply->status = 403;
        reply->close = true;
        return;
    }
    assert(!strcmp(req->url, "http://recognition.invalid:8081/tenant/v1/recognition/requests/task_1"));
    assert(req->method == XIAOTAI_HTTP_GET && !req->streamed && !req->body && !req->body_size &&
           !req->has_content_length);
    unsigned authorizations = 0;
    for (size_t i = 0; i < req->header_count; ++i) {
        if (strcmp(req->headers[i].name, "Authorization")) continue;
        assert(!strcmp(req->headers[i].value, "Bearer a.eyJleHAiOjE3MDAwMDA1MDB9.b"));
        ++authorizations;
    }
    assert(authorizations == 1);
    const unsigned index = atomic_fetch_add(&requests, 1U);
    reply->status = http ? http : 200;
    reply->body = body;
    reply->body_size = body_bytes;
    reply->content_length = (int64_t)body_bytes;
    reply->read_chunk_bytes = 23U;
    if (result_fault_case() && (index || with_nothing_to_publish())) {
        const char *response = index ? retry_body : "{\"code\":40400}";
        assert(*response);
        if (index) {
            assert(index == 1U);
            xiaotai_signal_give(&entered);
            assert(xiaotai_signal_take(&release, WAIT_MS));
        }
        reply->status = index ? 200 : 404;
        reply->body = (const uint8_t *)response;
        reply->body_size = strlen(response);
        reply->content_length = (int64_t)reply->body_size;
        return;
    }
    if (mode == CONNECT_FAIL) {
        assert(!req->reused);
        reply->fail_stage = XIAOTAI_HTTP_STAGE_CONNECT;
        reply->error = XIAOTAI_FAIL;
    } else if (mode == RECEIVE_RESET) {
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
        reply->error = XIAOTAI_FAIL;
        reply->fail_after_bytes = 23U;
    } else if (mode == TOO_LARGE) reply->content_length = MAX_BODY + 1;
    else if (mode == CHUNKED) reply->content_length = -1;
    else if (mode == HEAD_FAIL) {
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
        reply->error = XIAOTAI_FAIL;
    } else if (mode == EARLY_EOF) {
        reply->body = NULL;
        reply->body_size = 0;
    } else if (mode == PARTIAL_EOF) {
        assert(body_bytes > 23U);
        reply->body_size = 23U;
    } else if (mode == HEAD_STALL) reply->timing.wait_ms = 5001;
    else if (mode == BODY_STALL) reply->timing.receive_ms = 5001;
    else if (mode == WIRE_DEADLINE) {
        /* Each read is within the 5 s stall limit, but six reads need 24 s. */
        assert(body_bytes > 6U);
        reply->read_chunk_bytes = (body_bytes + 5U) / 6U;
        reply->read_chunk_ms = 4000;
    }
}

void product_recognition_http_prepare(void)
{
    size_t size;
    assert(scanf("%d %d %zu %" SCNu64 " %u %u", &http, &mode, &size, &revision, &items, &state) == 6 &&
           getchar() == '\n');
    assert(size && size <= MAX_BODY && mode >= PAYLOAD && mode <= PARTIAL_EOF);
    body = malloc(size);
    assert(body && fread(body, 1U, size, stdin) == size && getchar() == EOF);
    body_bytes = size;
    assert(xiaotai_signal_init(&entered) == XIAOTAI_OK && xiaotai_signal_init(&release) == XIAOTAI_OK);
    product_platform_prepare(true);
    product_platform_discovery_response(200, discovery, "mqtts://fixture.invalid");
    product_platform_http_responder(server, NULL);
    assert(xiaotai_kv_set_str("xt_recognition", "pending", journal, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
}

static xiaotai_recognition_status_t wait_result(unsigned count)
{
    xiaotai_recognition_status_t status = {0};
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        if (xiaotai_recognition_status(&status) == XIAOTAI_OK && !status.token &&
            (status.phase == XIAOTAI_RECOGNITION_RESULT || status.phase == XIAOTAI_RECOGNITION_RECOVERY) &&
            atomic_load(&requests) == count && !xiaotai_platform_exchanges_busy()) return status;
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "query did not settle: wire=%u phase=%u token=%u error=%#x http=%d\n",
            atomic_load(&requests), (unsigned)status.phase, (unsigned)status.token, (unsigned)status.error, status.http_status);
    abort();
}

static void hold_done(const xiaotai_platform_reply_t *reply, void *unused)
{
    (void)unused;
    assert(reply->result == XIAOTAI_OK && reply->http_status == 200);
    atomic_fetch_add(&cleared, 1U);
}

static void queue_query(void)
{
    const xiaotai_platform_request_t hold = {.service = "device", .path = "/hold-recognition-queue",
        .method = XIAOTAI_PLATFORM_GET, .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .timeout_ms = 30000U};
    assert(xiaotai_platform_request(&hold, hold_done, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&entered, WAIT_MS));
    assert(xiaotai_recognition_query() == XIAOTAI_OK);
    xiaotai_platform_status_t platform = {0};
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        xiaotai_platform_status(&platform);
        if (platform.exchanges == 2U) break;
        xiaotai_delay_ms(5);
    }
    assert(platform.exchanges == 2U && atomic_load(&requests) == 1U && !atomic_load(&cleared));
    if (mode == QUEUED || mode == CANCEL_QUEUED) {
        xiaotai_recognition_status_t status = {0};
        for (unsigned i = 0; i < WAIT_MS; i += 5U) {
            if (xiaotai_recognition_status(&status) == XIAOTAI_OK && status.token &&
                status.phase == XIAOTAI_RECOGNITION_QUERY && status.decision_action == XIAOTAI_RECOGNITION_ACTION_QUERY)
                break;
            xiaotai_delay_ms(5);
        }
        assert(status.token && status.phase == XIAOTAI_RECOGNITION_QUERY &&
               status.decision_action == XIAOTAI_RECOGNITION_ACTION_QUERY && status.decision_error == XIAOTAI_OK);
        if (mode == QUEUED) {
            const uint32_t serial = status.decision_serial;
            assert(xiaotai_recognition_query() == XIAOTAI_OK);
            for (unsigned i = 0; i < WAIT_MS; i += 5U) {
                if (xiaotai_recognition_status(&status) == XIAOTAI_OK && status.decision_serial != serial) break;
                xiaotai_delay_ms(5);
            }
            assert(status.decision_serial != serial && status.decision_error == XIAOTAI_ERR_INVALID_STATE &&
                   atomic_load(&requests) == 1U);
            xiaotai_signal_give(&release);
            return;
        }
        assert(xiaotai_recognition_cancel(status.token) == XIAOTAI_OK);
        for (unsigned i = 0; i < WAIT_MS; i += 5U) {
            if (xiaotai_recognition_status(&status) == XIAOTAI_OK &&
                status.phase == XIAOTAI_RECOGNITION_CANCELLING) break;
            xiaotai_delay_ms(5);
        }
        assert(status.phase == XIAOTAI_RECOGNITION_CANCELLING && atomic_load(&requests) == 1U);
    }
    if (mode == QUEUE_21S || mode == QUEUE_26S)
        atomic_store(&clock_delta_us, mode == QUEUE_21S ? INT64_C(21000000) : INT64_C(26000000));
    xiaotai_signal_give(&release);
}

static void check_journal_unchanged(void)
{
    char saved[sizeof(journal)];
    assert(xiaotai_kv_get_str_strict("xt_recognition", "pending", saved, sizeof(saved), XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK &&
           !strcmp(saved, journal));
}

static xiaotai_recognition_result_t *check_published_revision(uint64_t expected)
{
    xiaotai_recognition_result_t *view = NULL;
    const xiaotai_err_t result = xiaotai_recognition_result_acquire(&view);
    if (!expected) assert(result == XIAOTAI_ERR_NOT_FOUND && !view);
    else {
        xiaotai_recognition_result_info_t info;
        assert(result == XIAOTAI_OK && xiaotai_recognition_result_info(view, &info) == XIAOTAI_OK &&
               !strcmp(info.request_id, "task_1") && info.revision == expected);
    }
    return view;
}

static void notice_allocation_failure(const xiaotai_recognition_status_t *initial)
{
    const uint64_t before = with_nothing_to_publish() ? 0 : revision;
    const unsigned confirmed = before != 0;
    assert(revision && initial->task_revision == before && atomic_load(&result_events) == confirmed);
    xiaotai_recognition_result_t *old = check_published_revision(before);
    const int payload = snprintf(retry_body, sizeof(retry_body),
        "{\"code\":200,\"data\":{\"request_id\":\"task_1\",\"device_id\":\"" PRODUCT_FIXTURE_DEVICE
        "\",\"status_revision\":%" PRIu64 ",\"status\":\"succeeded\",\"error\":null,\"results\":[{"
        "\"item_id\":\"item_1\",\"order\":1,\"scheme_id\":\"fixture\",\"scheme_version\":1,\"output_mode\":\"text\","
        "\"status\":\"succeeded\",\"result\":{\"summary\":\"result\",\"alarm\":false,\"labels\":[],\"details\":{}},"
        "\"error\":null,\"speech\":{\"status\":\"skipped\",\"error\":null}}]}}", revision + 1);
    assert(payload > 0 && (size_t)payload < sizeof(retry_body));
    char envelope[sizeof(retry_body) + 100U];
    const int size = snprintf(envelope, sizeof(envelope), "{\"type\":\"recognition_result\",\"channel\":\"recognition\","
        "\"event_id\":\"fixture-oom\",\"payload\":%s}", retry_body);
    assert(size > 0 && (size_t)size < sizeof(envelope));
    atomic_store(&next_result_fault, mode == NOTICE_VIEW_FIRST || mode == NOTICE_VIEW_REPLACE ? 2U : 1U);
    product_platform_message("notify", envelope);
    xiaotai_recognition_stats_t stats = {0};
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        xiaotai_recognition_stats(&stats);
        if (stats.notifications_ignored) break;
        xiaotai_delay_ms(5);
    }
    assert(atomic_load(&injected_result_faults) == 1U && stats.notifications_ignored == 1U && stats.query_retries == 1U &&
           stats.results_adopted == confirmed && atomic_load(&result_events) == confirmed && atomic_load(&requests) == 1U);
    xiaotai_recognition_status_t status = wait_result(1U);
    assert(status.task_revision == before && status.task_state == initial->task_state &&
           status.task_items == initial->task_items && status.phase == initial->phase &&
           status.error == initial->error && status.possibly_sent == initial->possibly_sent);
    xiaotai_recognition_result_release(check_published_revision(before));
    check_journal_unchanged();

    atomic_fetch_add(&clock_delta_us, INT64_C(6000000));
    assert(xiaotai_signal_take(&entered, WAIT_MS));
    assert(xiaotai_recognition_status(&status) == XIAOTAI_OK && status.phase == XIAOTAI_RECOGNITION_QUERY &&
           status.task_revision == before && atomic_load(&result_events) == confirmed && atomic_load(&requests) == 2U);
    xiaotai_recognition_result_release(check_published_revision(before));
    xiaotai_signal_give(&release);
    status = wait_result(2U);
    assert(status.phase == XIAOTAI_RECOGNITION_RESULT && status.task_revision == revision + 1 && !status.error);
    xiaotai_recognition_result_release(check_published_revision(revision + 1));
    if (old) {
        xiaotai_recognition_result_info_t info;
        assert(xiaotai_recognition_result_info(old, &info) == XIAOTAI_OK && info.revision == before);
        xiaotai_recognition_result_release(old);
    }
    assert(atomic_load(&result_events) == confirmed + 1U);
    check_journal_unchanged();
    printf("query wire: allocation_failures=1 published_before_retry=%" PRIu64 " GET=2 confirmed_after_retry=%" PRIu64 "\n",
           before, status.task_revision);
}

void product_recognition_http(void)
{
    product_platform_online();
    xiaotai_recognition_status_t status = wait_result(1U);
    if (result_fault_case()) { notice_allocation_failure(&status); return; }
    if (queues_behind_another_request()) {
        assert(status.task_revision && status.error == XIAOTAI_OK);
        queue_query();
        status = wait_result(mode == QUEUED || mode == QUEUE_21S ? 2U : 1U);
        assert(atomic_load(&cleared) == 1U);
    }
    assert(!strcmp(status.request_id, "task_1") && !status.cleanup_pending);
    if (mode == CANCEL_QUEUED || mode == QUEUE_26S) {
        assert(status.phase == XIAOTAI_RECOGNITION_RECOVERY && status.http_status == 0 &&
               status.task_revision == revision && status.task_items == items);
        assert(status.error == (mode == QUEUE_26S ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK));
    } else if (revision) {
        assert(status.http_status == http);
        assert(status.phase == XIAOTAI_RECOGNITION_RESULT && status.error == XIAOTAI_OK &&
               status.code == 200 && status.task_revision == revision && status.task_items == items &&
               (unsigned)status.task_state == state);
        xiaotai_recognition_result_t *view;
        xiaotai_recognition_result_info_t info;
        assert(xiaotai_recognition_result_acquire(&view) == XIAOTAI_OK);
        assert(xiaotai_recognition_result_info(view, &info) == XIAOTAI_OK &&
               !strcmp(info.request_id, "task_1") && info.revision == revision && info.items == items &&
               (unsigned)info.state == state);
        xiaotai_recognition_result_release(view);
    } else {
        assert(status.phase == XIAOTAI_RECOGNITION_RECOVERY && status.http_status == http &&
               status.error != XIAOTAI_OK && !status.task_revision);
        if (mode == CONNECT_FAIL || mode == HEAD_FAIL || mode == HEAD_STALL) assert(status.error == XIAOTAI_FAIL);
        if (mode == RECEIVE_RESET || mode == EARLY_EOF || mode == PARTIAL_EOF || mode == BODY_STALL)
            assert(status.error == XIAOTAI_ERR_INVALID_RESPONSE);
        if (mode == TOO_LARGE) assert(status.error == XIAOTAI_ERR_INVALID_SIZE);
        if (mode == WIRE_DEADLINE) assert(status.error == XIAOTAI_ERR_TIMEOUT);
        if (http == 200) {
            xiaotai_recognition_stats_t stats;
            xiaotai_recognition_stats(&stats);
            assert(!stats.query_retries);
        }
    }
    check_journal_unchanged();
    printf("query wire: GET=%u status=%d code=%d error=%#x revision=%llu items=%u\n",
           atomic_load(&requests), status.http_status, status.code, (unsigned)status.error,
           (unsigned long long)status.task_revision, (unsigned)status.task_items);
}

void product_recognition_http_cleanup(void)
{
    free(body);
    xiaotai_signal_deinit(&release);
    xiaotai_signal_deinit(&entered);
}
