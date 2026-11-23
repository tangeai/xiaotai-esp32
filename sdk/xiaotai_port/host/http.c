// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "../common/http_rules.h"
#include "xiaotai_http_host.h"
#include "xiaotai/port/mem.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct xiaotai_http {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    http_origin_t origin;
    xiaotai_http_options_t options;
    bool running, cancelled, kept, idle_invalid;
};

static pthread_mutex_t s_fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    xiaotai_http_host_responder_fn respond;
    void *ctx;
    bool tls;
} s_fixture = {.tls = true};

static struct {
    atomic_uint_least32_t requests, completed, failed, timeouts, oversize, cancelled, connections, reused;
} s_stats;

void xiaotai_http_host_set_responder(xiaotai_http_host_responder_fn respond, void *ctx, bool tls)
{
    pthread_mutex_lock(&s_fixture_lock);
    s_fixture.respond = respond;
    s_fixture.ctx = ctx;
    s_fixture.tls = tls;
    pthread_mutex_unlock(&s_fixture_lock);
}

xiaotai_err_t xiaotai_http_create(const xiaotai_http_options_t *options, xiaotai_http_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    xiaotai_http_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) return XIAOTAI_ERR_NO_MEM;
    if (pthread_mutex_init(&c->lock, NULL) != 0) goto free_client;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) goto free_mutex;
    const int set = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    const int init = set ? set : pthread_cond_init(&c->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (init) goto free_mutex;
    if (options) c->options = *options;
    *out = c;
    return XIAOTAI_OK;
free_mutex:
    pthread_mutex_destroy(&c->lock);
free_client:
    xiaotai_mem_free(c);
    return XIAOTAI_ERR_NO_MEM;
}

void xiaotai_http_destroy(xiaotai_http_t *c)
{
    if (!c) return;
    pthread_cond_destroy(&c->changed);
    pthread_mutex_destroy(&c->lock);
    xiaotai_mem_free(c);
}

void xiaotai_http_close(xiaotai_http_t *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    if (!c->running) c->kept = c->idle_invalid = false;
    pthread_mutex_unlock(&c->lock);
}

bool xiaotai_http_idle_connection(xiaotai_http_t *c)
{
    if (!c) return false;
    pthread_mutex_lock(&c->lock);
    if (!c->running && c->idle_invalid) c->kept = c->idle_invalid = false;
    const bool open = !c->running && c->kept;
    pthread_mutex_unlock(&c->lock);
    return open;
}

void xiaotai_http_host_invalidate_idle(xiaotai_http_t *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    if (!c->running && c->kept) c->idle_invalid = true;
    pthread_mutex_unlock(&c->lock);
}

void xiaotai_http_cancel(xiaotai_http_t *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    if (c->running) {
        c->cancelled = true;
        pthread_cond_broadcast(&c->changed);
    }
    pthread_mutex_unlock(&c->lock);
}

/* The fake peer's owned message copies simulate connection storage. Keeping a
 * connection retains only its origin, never these copies or borrowed args. */
typedef struct {
    xiaotai_http_host_request_t request;
    void *bytes;
    size_t used;
} snapshot_t;

static bool extend(size_t *total, size_t amount)
{
    if (amount > SIZE_MAX - *total) return false;
    *total += amount;
    return true;
}

static char *copy_string(char **cursor, const char *input)
{
    char *out = *cursor;
    const size_t bytes = strlen(input) + 1;
    memcpy(out, input, bytes);
    *cursor += bytes;
    return out;
}

static bool snapshot(snapshot_t *out, const xiaotai_http_request_t *req, const xiaotai_http_t *c, bool reused)
{
    if (req->header_count > SIZE_MAX / sizeof(xiaotai_http_header_t)) return false;
    const size_t headers_size = req->header_count * sizeof(xiaotai_http_header_t);
    size_t bytes = headers_size;
    if (!extend(&bytes, strlen(req->url) + 1) || !extend(&bytes, req->body_size)) return false;
    for (size_t i = 0; i < req->header_count; ++i) {
        if (!extend(&bytes, strlen(req->headers[i].name) + 1) ||
            !extend(&bytes, strlen(req->headers[i].value) + 1)) return false;
    }
    out->bytes = malloc(bytes);
    if (!out->bytes) return false;
    out->used = bytes;
    xiaotai_http_header_t *headers = out->bytes;
    char *cursor = (char *)out->bytes + headers_size;
    const char *url = copy_string(&cursor, req->url);
    for (size_t i = 0; i < req->header_count; ++i) {
        headers[i].name = copy_string(&cursor, req->headers[i].name);
        headers[i].value = copy_string(&cursor, req->headers[i].value);
    }
    if (req->body_size) memcpy(cursor, req->body, req->body_size);
    out->request = (xiaotai_http_host_request_t){
        .method = req->method, .url = url, .headers = req->header_count ? headers : NULL,
        .header_count = req->header_count, .body = req->body ? (const uint8_t *)cursor : NULL,
        .body_size = req->source ? req->source_size : req->body_size, .streamed = req->source != NULL,
        .has_content_length = req->method != XIAOTAI_HTTP_GET &&
            (req->method != XIAOTAI_HTTP_DELETE || req->body || req->source),
        .reused = reused, .options = c->options,
    };
    return true;
}

static void clear_free(void *buffer, size_t size)
{
    if (!buffer) return;
    volatile uint8_t *p = buffer;
    while (size--) *p++ = 0;
    free(buffer);
}

static uint64_t monotonic_ms(void)
{
    struct timespec when = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &when);
    return (uint64_t)when.tv_sec * 1000U + (uint64_t)when.tv_nsec / 1000000U;
}

typedef struct {
    xiaotai_http_t *client;
    const xiaotai_http_request_t *req;
    xiaotai_http_response_t *res;
    xiaotai_http_host_reply_t reply;
    uint64_t deadline;
    bool over;
} run_t;

static xiaotai_err_t state(run_t *run)
{
    pthread_mutex_lock(&run->client->lock);
    const bool cancelled = run->client->cancelled;
    pthread_mutex_unlock(&run->client->lock);
    if (cancelled) return XIAOTAI_ERR_INVALID_STATE;
    return monotonic_ms() >= run->deadline ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

static xiaotai_err_t io(run_t *run, xiaotai_http_stage_t where, uint32_t *field, uint32_t millis, bool block)
{
    run->res->stage = where;
    xiaotai_err_t rc = state(run);
    if (rc != XIAOTAI_OK) return rc;
    const uint32_t budget = http_rules_io_budget(run->req, run->res->timing.total_ms);
    if (block) {
        const uint64_t now = monotonic_ms();
        const uint64_t limit = now + budget < run->deadline ? now + budget : run->deadline;
        const struct timespec until = {.tv_sec = (time_t)(limit / 1000), .tv_nsec = (long)(limit % 1000) * 1000000L};
        xiaotai_http_t *c = run->client;
        pthread_mutex_lock(&c->lock);
        int wait = 0;
        while (!c->cancelled && !wait) wait = pthread_cond_timedwait(&c->changed, &c->lock, &until);
        const bool cancelled = c->cancelled;
        pthread_mutex_unlock(&c->lock);
        uint64_t used = monotonic_ms() - now;
        *field += (uint32_t)(used < budget ? used : budget);
        run->res->timing.total_ms += (uint32_t)(used < budget ? used : budget);
        return cancelled ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_TIMEOUT;
    }
    const uint32_t elapsed = millis < budget ? millis : budget;
    *field += elapsed;
    run->res->timing.total_ms += elapsed;
    return millis > budget ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

static xiaotai_err_t peer_failure(run_t *run)
{
    run->res->platform_error = run->reply.platform_error;
    return run->reply.error != XIAOTAI_OK ? run->reply.error : XIAOTAI_FAIL;
}

static xiaotai_err_t step(run_t *run, xiaotai_http_stage_t stage, uint32_t *field, uint32_t millis)
{
    xiaotai_err_t rc = io(run, stage, field, millis, run->reply.block_stage == stage);
    if (rc == XIAOTAI_OK && run->reply.fail_stage == stage &&
        (stage != XIAOTAI_HTTP_STAGE_RECEIVE || !run->reply.fail_after_bytes)) rc = peer_failure(run);
    return rc;
}

static xiaotai_err_t send_body(run_t *run)
{
    const xiaotai_http_request_t *req = run->req;
    xiaotai_http_host_reply_t *reply = &run->reply;
    if (!req->source) return XIAOTAI_OK;
    uint8_t chunk[HTTP_CHUNK_MAX];
    uint8_t *uploaded = reply->uploaded && req->source_size ? malloc(req->source_size) : NULL;
    if (reply->uploaded && req->source_size && !uploaded) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t rc = XIAOTAI_OK;
    size_t offset = 0;
    while (offset < req->source_size) {
        rc = state(run);
        if (rc != XIAOTAI_OK) break;
        size_t bytes;
        rc = http_rules_source(req, chunk, req->source_size - offset, &bytes);
        if (rc != XIAOTAI_OK) break;
        rc = io(run, XIAOTAI_HTTP_STAGE_SEND, &run->res->timing.send_ms, reply->send_chunk_ms, false);
        if (rc != XIAOTAI_OK) break;
        if (uploaded) memcpy(uploaded + offset, chunk, bytes);
        offset += bytes;
    }
    if (rc == XIAOTAI_OK && reply->uploaded) reply->uploaded(reply->upload_ctx, uploaded, offset);
    clear_free(uploaded, offset);
    return rc;
}

static bool stall_precedes_deadline(const run_t *run)
{
    const uint32_t stall = run->req->io_timeout_ms;
    return stall && (uint64_t)run->res->timing.total_ms + stall < run->req->timeout_ms &&
        monotonic_ms() + stall < run->deadline;
}

static xiaotai_err_t receive_fault(run_t *run, xiaotai_http_receive_fault_t phase, xiaotai_err_t rc,
                                   bool stalled)
{
    if (rc == XIAOTAI_FAIL || (rc == XIAOTAI_ERR_TIMEOUT && stalled)) run->res->receive_fault = phase;
    return rc;
}

static xiaotai_err_t receive_io(run_t *run, xiaotai_http_receive_fault_t phase, uint32_t *field,
                                uint32_t millis, bool block, bool inject)
{
    const uint32_t before = run->res->timing.total_ms;
    const bool stall_first = stall_precedes_deadline(run);
    xiaotai_err_t rc = io(run, XIAOTAI_HTTP_STAGE_RECEIVE, field, millis, block);
    const bool injected = rc == XIAOTAI_OK && inject;
    if (injected) rc = peer_failure(run);
    return receive_fault(run, phase, rc, stall_first && (injected || run->res->timing.total_ms != before));
}

static xiaotai_err_t receive(run_t *run)
{
    xiaotai_http_response_t *r = run->res;
    xiaotai_http_host_reply_t *p = &run->reply;
    xiaotai_err_t rc = receive_io(run, XIAOTAI_HTTP_RECEIVE_FAULT_HEAD, &r->timing.wait_ms, p->timing.wait_ms,
        p->block_stage == XIAOTAI_HTTP_STAGE_RECEIVE, p->fail_stage == XIAOTAI_HTTP_STAGE_RECEIVE && !p->fail_after_bytes);
    if (rc != XIAOTAI_OK) return rc;
    if (p->status < 100 || p->status > 599 || p->content_length < -1 ||
        (p->header_count && !p->headers) || (p->body_size && !p->body)) return XIAOTAI_FAIL;
    r->status = p->status;
    r->content_length = p->content_length;
    for (size_t i = 0; i < p->header_count; ++i)
        http_rules_header(r, p->headers[i].name, p->headers[i].value, &p->close);
    const bool has_body = r->status >= 200 && r->status != 204 && r->status != 304;
    const size_t limit = http_rules_receive_limit(r);
    if (has_body && p->content_length >= 0 && (uint64_t)p->content_length > limit) {
        run->over = true;
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (!has_body && p->body_size) return XIAOTAI_FAIL;
    rc = receive_io(run, XIAOTAI_HTTP_RECEIVE_FAULT_BODY, &r->timing.receive_ms, p->timing.receive_ms, false, false);
    if (rc != XIAOTAI_OK) return rc;
    size_t framed = p->body_size;
    if (has_body && p->content_length >= 0 && (uint64_t)p->content_length < framed)
        framed = (size_t)p->content_length;
    for (size_t offset = 0; offset < framed;) {
        size_t bytes = framed - offset;
        const size_t chunk = p->read_chunk_bytes ? p->read_chunk_bytes : HTTP_CHUNK_MAX;
        if (bytes > chunk) bytes = chunk;
        if (p->fail_stage == XIAOTAI_HTTP_STAGE_RECEIVE && p->fail_after_bytes &&
            bytes > p->fail_after_bytes - offset) bytes = p->fail_after_bytes - offset;
        rc = receive_io(run, XIAOTAI_HTTP_RECEIVE_FAULT_BODY, &r->timing.receive_ms, p->read_chunk_ms, false, false);
        if (rc != XIAOTAI_OK) return rc;
        if (r->size > limit || bytes > limit - r->size) {
            run->over = true;
            return XIAOTAI_ERR_INVALID_SIZE;
        }
        rc = http_rules_consume(r, p->body + offset, bytes);
        if (rc != XIAOTAI_OK) return rc;
        offset += bytes;
        if (p->fail_stage == XIAOTAI_HTTP_STAGE_RECEIVE && offset == p->fail_after_bytes)
            return receive_fault(run, XIAOTAI_HTTP_RECEIVE_FAULT_BODY, peer_failure(run), stall_precedes_deadline(run));
    }
    if (has_body && p->content_length >= 0 && (uint64_t)p->content_length != p->body_size) {
        if ((uint64_t)p->content_length > p->body_size)
            r->receive_fault = r->size ? XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED : XIAOTAI_HTTP_RECEIVE_FAULT_BODY;
        return XIAOTAI_FAIL;
    }
    if (p->fail_stage == XIAOTAI_HTTP_STAGE_RECEIVE)
        return receive_fault(run, XIAOTAI_HTTP_RECEIVE_FAULT_BODY, peer_failure(run), stall_precedes_deadline(run));
    return state(run);
}

xiaotai_err_t xiaotai_http_perform(xiaotai_http_t *c, const xiaotai_http_request_t *req, xiaotai_http_response_t *r)
{
    http_rules_reset(r);
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->lock);
    if (c->running) {
        pthread_mutex_unlock(&c->lock);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    c->running = true;
    c->cancelled = false;
    pthread_mutex_unlock(&c->lock);

    bool attempted = false;
    run_t run = {.client = c, .req = req, .res = r, .reply = {.status = 200, .content_length = -1}};
    snapshot_t peer = {0};
    http_origin_t origin = {0};
    xiaotai_err_t rc = http_rules_validate(req, r, &origin);
    if (rc != XIAOTAI_OK) goto done;
    if (req->cancelled && req->cancelled(req->cancel_ctx)) {
        rc = XIAOTAI_ERR_INVALID_STATE;
        goto done;
    }
    pthread_mutex_lock(&s_fixture_lock);
    xiaotai_http_host_responder_fn respond = s_fixture.respond;
    void *fixture_ctx = s_fixture.ctx;
    const bool tls = s_fixture.tls;
    pthread_mutex_unlock(&s_fixture_lock);
    if (origin.tls && !tls) {
        rc = XIAOTAI_ERR_NOT_SUPPORTED;
        goto done;
    }
    const bool reuse = c->kept && !c->idle_invalid && http_rules_same_origin(&c->origin, &origin);
    run.deadline = monotonic_ms() + req->timeout_ms;
    if (!snapshot(&peer, req, c, reuse)) {
        rc = XIAOTAI_ERR_NO_MEM;
        goto done;
    }
    attempted = true;
    ++s_stats.requests;
    if (respond) respond(fixture_ctx, &peer.request, &run.reply);
    else run.reply.fail_stage = reuse ? XIAOTAI_HTTP_STAGE_SEND : XIAOTAI_HTTP_STAGE_RESOLVE;
    if (!reuse) {
        rc = step(&run, XIAOTAI_HTTP_STAGE_RESOLVE, &r->timing.resolve_ms, run.reply.timing.resolve_ms);
        if (rc != XIAOTAI_OK) goto done;
        rc = step(&run, XIAOTAI_HTTP_STAGE_CONNECT, &r->timing.connect_ms, run.reply.timing.connect_ms);
        if (rc != XIAOTAI_OK) goto done;
        ++s_stats.connections;
    }
    r->reused = reuse;
    if (reuse) ++s_stats.reused;
    rc = step(&run, XIAOTAI_HTTP_STAGE_SEND, &r->timing.send_ms, run.reply.timing.send_ms);
    if (rc == XIAOTAI_OK) rc = send_body(&run);
    if (rc == XIAOTAI_OK) rc = receive(&run);

done:
    clear_free(peer.bytes, peer.used);
    pthread_mutex_lock(&c->lock);
    const bool cancelled = attempted && c->cancelled;
    if (cancelled) rc = XIAOTAI_ERR_INVALID_STATE;
    c->kept = rc == XIAOTAI_OK && req->keep_alive && !r->redirect && !run.reply.close;
    if (c->kept) c->origin = origin;
    if (r) r->kept = c->kept;
    c->idle_invalid = false;
    c->running = false;
    pthread_mutex_unlock(&c->lock);
    if (attempted) {
        if (rc == XIAOTAI_OK) {
            r->stage = XIAOTAI_HTTP_STAGE_DONE;
            ++s_stats.completed;
        } else {
            ++s_stats.failed;
            if (cancelled) ++s_stats.cancelled;
            if (rc == XIAOTAI_ERR_TIMEOUT) ++s_stats.timeouts;
            if (rc == XIAOTAI_ERR_INVALID_SIZE && run.over) ++s_stats.oversize;
        }
    }
    return rc;
}

void xiaotai_http_stats(xiaotai_http_stats_t *out)
{
    if (!out) return;
    *out = (xiaotai_http_stats_t){
        .requests = atomic_load(&s_stats.requests), .completed = atomic_load(&s_stats.completed),
        .failed = atomic_load(&s_stats.failed), .timeouts = atomic_load(&s_stats.timeouts),
        .oversize = atomic_load(&s_stats.oversize), .cancelled = atomic_load(&s_stats.cancelled),
        .connections = atomic_load(&s_stats.connections), .reused = atomic_load(&s_stats.reused),
    };
}
