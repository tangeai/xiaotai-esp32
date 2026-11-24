// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_platform_http_trace.py
// Single-request subset of tests/regression/test_queued_http.py; queues are not tested here.
#include "xiaotai/port/http.h"
#include "xiaotai/port/os.h"
#include "xiaotai_http_host.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

typedef struct {
    xiaotai_http_host_reply_t reply;
    xiaotai_http_request_t *expect;
    unsigned count;
    bool reused, has_content_length, streamed;
    xiaotai_signal_t *entered;
} fixture_t;

static void responder(void *ctx, const xiaotai_http_host_request_t *incoming, xiaotai_http_host_reply_t *reply)
{
    fixture_t *f = ctx;
    ++f->count;
    f->reused = incoming->reused;
    f->has_content_length = incoming->has_content_length;
    f->streamed = incoming->streamed;
    if (f->expect) {
        const xiaotai_http_request_t *req = f->expect;
        assert(incoming->url != req->url && !strcmp(incoming->url, req->url));
        assert(incoming->method == req->method && incoming->header_count == req->header_count);
        assert(incoming->body_size == (req->source ? req->source_size : req->body_size));
        if (req->body) {
            assert(incoming->body && incoming->body != req->body);
            assert(!memcmp(incoming->body, req->body, req->body_size));
        } else assert(!incoming->body);
        for (size_t i = 0; i < req->header_count; ++i) {
            assert(incoming->headers != req->headers);
            assert(incoming->headers[i].name != req->headers[i].name);
            assert(incoming->headers[i].value != req->headers[i].value);
            assert(!strcmp(incoming->headers[i].name, req->headers[i].name));
            assert(!strcmp(incoming->headers[i].value, req->headers[i].value));
        }
        assert(incoming->options.no_delay && incoming->options.rx_buffer_bytes == 450 &&
               incoming->options.tx_buffer_bytes == 181);
    }
    *reply = f->reply;
    if (f->entered) xiaotai_signal_give(f->entered);
}

static const xiaotai_http_host_reply_t normal_reply = {.status = 200, .content_length = -1};
static xiaotai_http_t *create(fixture_t *fixture)
{
    xiaotai_http_t *out = NULL;
    xiaotai_http_options_t opts = {.no_delay = true, .rx_buffer_bytes = 450, .tx_buffer_bytes = 181};
    assert(xiaotai_http_create(&opts, &out) == XIAOTAI_OK && out);
    fixture->reply = normal_reply;
    xiaotai_http_host_set_responder(responder, fixture, true);
    return out;
}

static xiaotai_http_request_t request(void)
{
    return (xiaotai_http_request_t){.url = "http://fixture.invalid/v1?arg=2", .timeout_ms = 2000};
}

static void assert_invalid(xiaotai_http_t *c, xiaotai_http_request_t req, fixture_t *f)
{
    unsigned before = f->count;
    xiaotai_http_response_t r = {.status = -1, .size = 50, .redirect = true, .kept = true};
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_INVALID_ARG);
    assert(r.stage == XIAOTAI_HTTP_STAGE_NONE && !r.status && !r.size && !r.redirect && !r.kept);
    assert(r.content_length == -1 && !r.timing.total_ms && f->count == before);
}

typedef struct {
    uint8_t bytes[2100];
    size_t count, given;
    unsigned uploads, calls;
    xiaotai_err_t source_result;
    bool malformed;
} source_t;

static xiaotai_err_t source(void *data, uint8_t *out, size_t cap, size_t *n)
{
    source_t *s = data;
    ++s->calls;
    assert(cap <= 1024);
    if (s->source_result) return s->source_result;
    size_t left = s->count - s->given;
    *n = cap < left ? cap : left;
    memcpy(out, s->bytes + s->given, *n);
    s->given += *n;
    if (s->malformed) *n = cap + 1;
    return XIAOTAI_OK;
}

static void upload(void *data, const uint8_t *bytes, size_t n)
{
    source_t *s = data;
    ++s->uploads;
    assert(s->count == n && s->given == n && !memcmp(bytes, s->bytes, n));
}

static void validation(void)
{
    fixture_t f = {0};
    xiaotai_http_t *c = create(&f);
    xiaotai_http_request_t req = request();
    xiaotai_http_stats_t before, after;
    xiaotai_http_stats(&before);
    static const char *const urls[] = {
        NULL, "", "/", "ftp://fixture.invalid/", "http:", "http://", "http:///path",
        "http://u@fixture.invalid/", "https://fixture.invalid:65536/", "http://x:0/", "http://x:3a",
        "http://x/path\r\n", "http://x\\unsafe/", "http://[::::]/", "http://[::ffff:192.0.2.256]/",
        "http://[a:1:2:3:4:5:6:7:8]/", "http://[1:2:3:4:5:6]/"
    };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); ++i) {
        req.url = urls[i];
        assert_invalid(c, req, &f);
    }
    req = request();
    req.body = "x";
    assert_invalid(c, req, &f); /* Even a non-NULL zero-byte GET body is invalid. */
    req.source = source;
    req.method = XIAOTAI_HTTP_POST;
    assert_invalid(c, req, &f); /* fixed and streamed at once */
    req = request();
    req.body_size = 1;
    assert_invalid(c, req, &f);
    req = request();
    req.source_size = 1;
    assert_invalid(c, req, &f);
    req = request();
    req.source = source;
    assert_invalid(c, req, &f);
    req = request();
    req.method = (xiaotai_http_method_t)99;
    assert_invalid(c, req, &f);
    req = request();
    req.timeout_ms = 0;
    assert_invalid(c, req, &f);
    req.timeout_ms = 600001;
    assert_invalid(c, req, &f);
    req = request();
    req.header_count = 1;
    assert_invalid(c, req, &f);
    xiaotai_http_header_t h = {.name = "Invalid Header", .value = "1"};
    req.headers = &h;
    assert_invalid(c, req, &f);
    h.name = "Correct";
    h.value = "1\r\nX-Entry: 2";
    assert_invalid(c, req, &f);
    xiaotai_http_stats(&after);
    assert(before.requests == after.requests && before.connections == after.connections &&
           before.failed == after.failed);

    req = request();
    req.url = "https://[::1]:443/";
    xiaotai_http_host_set_responder(responder, &f, false);
    xiaotai_http_response_t r = {0};
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_NOT_SUPPORTED && r.stage == XIAOTAI_HTTP_STAGE_NONE);
    xiaotai_http_host_set_responder(responder, &f, true);
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && r.stage == XIAOTAI_HTTP_STAGE_DONE);
    req = request();
    f.expect = &req;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && !f.has_content_length);
    req.method = XIAOTAI_HTTP_POST;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && f.has_content_length);
    req.method = XIAOTAI_HTTP_PUT;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && f.has_content_length);
    req.method = XIAOTAI_HTTP_DELETE;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && !f.has_content_length);
    const uint8_t data[] = {0, 3, 5};
    req.body = data;
    req.body_size = sizeof(data);
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && f.has_content_length);
    xiaotai_http_destroy(c);
}

typedef struct {
    xiaotai_http_response_t *r;
    size_t bytes, calls;
    xiaotai_err_t err;
    const uint8_t *expect;
} sink_t;

static xiaotai_err_t sink(void *ctx, const uint8_t *data, size_t size)
{
    sink_t *s = ctx;
    ++s->calls;
    /* Status and requested headers precede the first body callback. */
    assert(s->r->status == 418 && s->r->content_length == 6 && s->r->captures[0].count == 2);
    assert(!strcmp(s->r->captures[0].value, "application") && s->r->captures[0].truncated);
    assert(!memcmp(s->expect + s->bytes, data, size));
    if (s->err) return s->err;
    s->bytes += size;
    return XIAOTAI_OK;
}

static void receive_and_limits(void)
{
    fixture_t f = {0};
    xiaotai_http_t *c = create(&f);
    xiaotai_http_request_t req = request();
    const uint8_t bytes[] = {0, 1, 2, 7, 8, 9};
    static const xiaotai_http_header_t headers[] = {
        {"Content-Type", "application/octet-stream"}, {"content-type", "text/plain"},
        {"x-debug", ""}, {"X-TRACE", "ABC"}
    };
    char kind[12], debug[3], missing[3] = "st";
    xiaotai_http_capture_t caps[] = {
        {.name = "CONTENT-TYPE", .value = kind, .capacity = sizeof(kind)},
        {.name = "location", .value = missing, .capacity = sizeof(missing), .count = 2},
        {.name = "X-Debug", .value = debug, .capacity = sizeof(debug), .truncated = true},
        {.name = "x-trace"}, /* Count-only still reports inability to copy a value. */
    };
    xiaotai_http_response_t r = {
        .capture_count = sizeof(caps) / sizeof(caps[0]), .captures = caps, .sink = sink, .sink_limit = 6
    };
    sink_t s = {.r = &r, .expect = bytes};
    r.sink_ctx = &s;
    f.reply = (xiaotai_http_host_reply_t){
        .status = 418, .content_length = 6, .body = bytes, .body_size = sizeof(bytes), .read_chunk_bytes = 3,
        .headers = headers, .header_count = sizeof(headers) / sizeof(headers[0])
    };
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK);
    assert(r.status == 418 && r.size == sizeof(bytes) && s.calls == 2 && s.bytes == sizeof(bytes));
    assert(!caps[1].count && !caps[1].truncated && !missing[0]);
    assert(caps[2].count == 1 && !caps[2].truncated && !debug[0]);
    assert(caps[3].count == 1 && caps[3].truncated);

    xiaotai_http_stats_t before, after;
    xiaotai_http_stats(&before);
    s.calls = s.bytes = 0;
    r.sink_limit = 5;
    req.keep_alive = true;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_INVALID_SIZE && !s.calls);
    assert(r.status == 418 && r.content_length == 6 && !r.size && !r.kept);
    r.sink_limit = 6;
    s.err = XIAOTAI_ERR_BUSY;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_BUSY && s.calls == 1 && !r.size && !r.kept);
    s.err = XIAOTAI_ERR_INVALID_SIZE;
    s.calls = 0;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_INVALID_SIZE && s.calls == 1);
    xiaotai_http_stats(&after);
    assert(after.oversize - before.oversize == 1 && after.failed - before.failed == 3);

    uint8_t body[7];
    memset(body, 0xa5, sizeof(body));
    r = (xiaotai_http_response_t){.body = body, .capacity = 4};
    f.reply.content_length = -1;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_INVALID_SIZE &&
           r.status == 418 && r.size == 3 && body[0] == 0 && body[3] == 0xa5 && !r.kept);
    r.capacity = 6;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && r.size == 6 && body[6] == 0xa5);
    assert(!memcmp(body, bytes, 6));
    r = (xiaotai_http_response_t){0};
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && r.size == 0);
    r = (xiaotai_http_response_t){.body = body, .capacity = 6};
    f.reply.content_length = 6;
    f.reply.body_size = 3;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL && r.size == 3 && !r.kept);
    assert(r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED);
    f.reply.body_size = 0;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL && !r.size && !r.kept);
    assert(r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    f.reply.content_length = 4;
    f.reply.body_size = 6;
    body[4] = 0x5a;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL && r.size == 4 && body[4] == 0x5a && !r.kept);
    assert(r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_NONE);
    f.reply.status = 304;
    f.reply.content_length = 6;
    f.reply.body_size = 0;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && r.content_length == 6 && !r.size);
    assert(r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_NONE);
    xiaotai_http_destroy(c);
}

static void upload_limits(void)
{
    fixture_t f = {0};
    xiaotai_http_t *c = create(&f);
    xiaotai_http_request_t req = request();
    req.method = XIAOTAI_HTTP_PUT;
    source_t in = {.count = sizeof(in.bytes)};
    for (size_t i = 0; i < sizeof(in.bytes); ++i) in.bytes[i] = (uint8_t)(i % 199);
    req.source = source;
    req.source_ctx = &in;
    req.source_size = in.count;
    xiaotai_http_response_t r = {0};
    f.reply.uploaded = upload;
    f.reply.upload_ctx = &in;
    f.reply.send_chunk_ms = 10;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK);
    assert(in.given == in.count && in.calls == 3 && in.uploads == 1 && f.streamed &&
           r.timing.send_ms == 30 && r.timing.total_ms == 30);
    in.given = in.calls = 0;
    in.count = 1024;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL);
    assert(in.calls == 2 && in.uploads == 1 && r.stage == XIAOTAI_HTTP_STAGE_SEND && !r.status && !r.kept);
    in.given = in.calls = 0;
    in.source_result = XIAOTAI_ERR_BUSY;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_BUSY && in.calls == 1);
    in.source_result = XIAOTAI_OK;
    in.malformed = true;
    in.calls = 0;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_INVALID_SIZE && in.calls == 1);
    f.reply.fail_stage = XIAOTAI_HTTP_STAGE_CONNECT;
    in.calls = 0;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL && !in.calls);
    xiaotai_http_destroy(c);
}

static void reuse_and_timings(void)
{
    fixture_t f = {0};
    xiaotai_http_t *c = create(&f);
    xiaotai_http_request_t req = request();
    xiaotai_http_response_t r = {0};
    f.expect = &req;
    req.keep_alive = true;
    req.method = XIAOTAI_HTTP_POST;
    req.body = "payload";
    req.body_size = 7;
    static const xiaotai_http_header_t extra[] = {{"Authorization", "fixture-auth"}, {"content-TYPE", "synthetic"}};
    req.headers = extra;
    req.header_count = 2;
    f.reply.timing = (xiaotai_http_timing_t){.resolve_ms = 11, .connect_ms = 13, .send_ms = 17,
                                             .wait_ms = 19, .receive_ms = 23};
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK);
    assert(r.kept && !r.reused && xiaotai_http_idle_connection(c));
    assert(r.timing.resolve_ms == 11 && r.timing.connect_ms == 13 && r.timing.send_ms == 17 &&
           r.timing.wait_ms == 19 && r.timing.receive_ms == 23 && r.timing.total_ms == 83);
    req = request();
    req.keep_alive = true;
    req.url = "HTTP://FIXTURE.INVALID:80/next";
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK);
    assert(r.kept && r.reused && f.reused && !f.has_content_length && !r.timing.resolve_ms &&
           !r.timing.connect_ms && r.timing.total_ms == 59 && f.count == 2);
    f.reply.fail_stage = XIAOTAI_HTTP_STAGE_SEND;
    f.reply.platform_error = -78;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL && f.count == 3);
    assert(r.reused && !r.kept && !r.status && r.platform_error == -78 && r.stage == XIAOTAI_HTTP_STAGE_SEND);

    f.reply.fail_stage = XIAOTAI_HTTP_STAGE_NONE;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && !r.reused && r.kept);
    f.reply.fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
    req.method = XIAOTAI_HTTP_POST;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL && r.reused && !r.kept && f.count == 5);
    f.reply.fail_stage = XIAOTAI_HTTP_STAGE_NONE;
    static const xiaotai_http_header_t location[] = {{"lOcAtIoN", "http://unused.invalid/"}};
    f.reply.headers = location;
    f.reply.header_count = 1;
    f.reply.status = 307;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && r.status == 307 &&
           r.redirect && !r.kept && f.count == 6);
    static const xiaotai_http_header_t close[] = {{"CONNECTION", "keep-alive, Close\t"}};
    f.reply.headers = close;
    f.reply.status = 200;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && !r.kept && !r.redirect);
    f.reply.header_count = 0;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && r.kept && !r.reused);
    req.url = "http://fixture.invalid:81/next";
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && !r.reused);
    xiaotai_http_host_invalidate_idle(c);
    assert(!xiaotai_http_idle_connection(c));
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_OK && !r.reused);
    xiaotai_http_close(c);
    assert(!xiaotai_http_idle_connection(c));

    req.timeout_ms = 1000;
    req.io_timeout_ms = 150;
    f.reply.timing = (xiaotai_http_timing_t){.resolve_ms = 81, .connect_ms = 160};
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_TIMEOUT);
    assert(r.stage == XIAOTAI_HTTP_STAGE_CONNECT && r.timing.connect_ms == 150 && r.timing.total_ms == 231);
    req.timeout_ms = 200;
    req.io_timeout_ms = 0;
    f.reply.timing = (xiaotai_http_timing_t){.resolve_ms = 71, .connect_ms = 98, .send_ms = 80};
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_TIMEOUT);
    assert(r.stage == XIAOTAI_HTTP_STAGE_SEND && r.timing.send_ms == 31 && r.timing.total_ms == 200);

    req = request();
    req.keep_alive = true;
    static const uint8_t bytes[] = {1, 0, 3, 4};
    uint8_t out[sizeof(bytes)] = {0};
    r = (xiaotai_http_response_t){.body = out, .capacity = sizeof(out)};
    f.reply = normal_reply;
    f.reply.fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
    f.reply.body = bytes;
    f.reply.body_size = sizeof(bytes);
    f.reply.fail_after_bytes = 2;
    f.reply.platform_error = 963;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL);
    assert(r.status == 200 && r.size == 2 && out[0] == 1 && out[1] == 0 && !r.kept && r.platform_error == 963);
    assert(r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    f.reply.fail_after_bytes = 0;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_FAIL && !r.status && !r.size &&
           r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_HEAD);
    f.reply.fail_stage = XIAOTAI_HTTP_STAGE_NONE;
    f.reply.timing = (xiaotai_http_timing_t){.receive_ms = 21};
    req.timeout_ms = 100; req.io_timeout_ms = 20;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_TIMEOUT && r.status == 200 && !r.size &&
           r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    req.timeout_ms = 20;
    assert(xiaotai_http_perform(c, &req, &r) == XIAOTAI_ERR_TIMEOUT && r.status == 200 && !r.size &&
           r.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_NONE);
    xiaotai_http_destroy(c);
}

typedef struct {
    xiaotai_http_t *client;
    xiaotai_http_request_t req;
    xiaotai_http_response_t res;
    xiaotai_err_t rc;
} worker_t;

static void *worker(void *ctx)
{
    worker_t *job = ctx;
    job->rc = xiaotai_http_perform(job->client, &job->req, &job->res);
    return NULL;
}

static void cancellation(void)
{
    fixture_t f = {0};
    xiaotai_http_t *c = create(&f);
    xiaotai_signal_t entered;
    assert(xiaotai_signal_init(&entered) == XIAOTAI_OK);
    f.entered = &entered;
    f.reply.block_stage = XIAOTAI_HTTP_STAGE_RESOLVE;
    worker_t job = {.client = c, .req = request()};
    pthread_t task;
    xiaotai_http_stats_t before, after;
    xiaotai_http_stats(&before);
    assert(!pthread_create(&task, NULL, worker, &job));
    assert(xiaotai_signal_take(&entered, 1000));
    xiaotai_http_response_t r = {0};
    assert(xiaotai_http_perform(c, &job.req, &r) == XIAOTAI_ERR_INVALID_STATE &&
           r.stage == XIAOTAI_HTTP_STAGE_NONE);
    xiaotai_http_cancel(c);
    assert(!pthread_join(task, NULL));
    assert(job.rc == XIAOTAI_ERR_INVALID_STATE && job.res.stage == XIAOTAI_HTTP_STAGE_RESOLVE &&
           !job.res.kept && f.count == 1);
    xiaotai_http_stats(&after);
    assert(after.cancelled - before.cancelled == 1 && after.failed - before.failed == 1 &&
           after.requests - before.requests == 1 && after.connections == before.connections);
    f.entered = NULL;
    job.req.io_timeout_ms = 10;
    assert(xiaotai_http_perform(c, &job.req, &r) == XIAOTAI_ERR_TIMEOUT && r.stage == XIAOTAI_HTTP_STAGE_RESOLVE);
    f.reply = normal_reply;
    xiaotai_http_cancel(c); /* No running request; must not poison this one. */
    assert(xiaotai_http_perform(c, &job.req, &r) == XIAOTAI_OK);
    xiaotai_signal_deinit(&entered);
    xiaotai_http_destroy(c);
}

int main(void)
{
    validation();
    receive_and_limits();
    upload_limits();
    reuse_and_timings();
    cancellation();
    xiaotai_http_host_set_responder(NULL, NULL, true);
    return 0;
}
