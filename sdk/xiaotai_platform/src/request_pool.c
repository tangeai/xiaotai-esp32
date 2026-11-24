// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "transfer_internal.h"
#include "xiaotai/identity.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/sys.h"

static bool string_valid(const char *value, size_t capacity)
{
    if (!value || !*value) return false;
    for (size_t i = 0; i < capacity; ++i) {
        const unsigned char ch = (unsigned char)value[i];
        if (!ch) return true;
        if (ch < 0x20 || ch > 0x7e) return false;
    }
    return false;
}

static xiaotai_err_t admit(const xiaotai_platform_request_t *req, const xiaotai_platform_transfer_t *transfer,
                          const xiaotai_platform_stream_t *stream, xiaotai_platform_reply_fn_t reply, void *ctx,
                          uint32_t *id, uint32_t profile_id, uint32_t expected_generation)
{
    if (id) *id = 0;
    const uint32_t timeout_max = transfer ? XIAOTAI_PLATFORM_TRANSFER_TIMEOUT_MAX_MS : XIAOTAI_PLATFORM_TIMEOUT_MAX_MS;
    const uint32_t allowed_flags = XIAOTAI_PLATFORM_URGENT | XIAOTAI_PLATFORM_RELEASE | XIAOTAI_PLATFORM_BACKGROUND |
        (transfer ? XIAOTAI_PLATFORM_TRANSFER_FULL_TIMEOUT : 0U);
    if (!req || (!transfer && !reply) || (transfer && (!stream || !stream->done)) ||
        !string_valid(req->service, XIAOTAI_PLATFORM_SERVICE_NAME_MAX) || !platform_url_path_valid(req->path) ||
        (unsigned)req->method > XIAOTAI_PLATFORM_DELETE || (unsigned)req->auth > XIAOTAI_PLATFORM_AUTH_NONE ||
        req->timeout_ms > timeout_max || (req->flags & ~allowed_flags) ||
        (req->method == XIAOTAI_PLATFORM_GET && (req->body || (transfer && transfer->body_size))) ||
        (req->auth == XIAOTAI_PLATFORM_AUTH_BEARER && !string_valid(req->bearer, XIAOTAI_PLATFORM_BEARER_MAX)) ||
        (req->auth != XIAOTAI_PLATFORM_AUTH_BEARER && req->bearer) ||
        (transfer && transfer->body_size && (req->body || !stream->read ||
          !string_valid(transfer->content_type, XIAOTAI_PLATFORM_CONTENT_TYPE_MAX)))) return XIAOTAI_ERR_INVALID_ARG;
    if (!platform.objects || !atomic_load(&platform.running) || atomic_load(&platform.stopped)) {
        return XIAOTAI_ERR_PLATFORM_OFFLINE;
    }

    platform_lock();
    const char *base = NULL;
    const size_t body_size = req->body ? strnlen(req->body, platform.request_limit) : 0;
    xiaotai_err_t err = platform.gate ? XIAOTAI_ERR_PLATFORM_GATED : !platform.network_up ? XIAOTAI_ERR_PLATFORM_OFFLINE :
                        platform_service_find(req->service, &base);
    if (expected_generation && expected_generation != xiaotai_identity_generation()) err = XIAOTAI_ERR_PLATFORM_STALE;
    if (profile_id && !platform_profile_current(profile_id)) err = XIAOTAI_ERR_PLATFORM_STALE;
    if (profile_id && err == XIAOTAI_OK && platform.status.held) err = XIAOTAI_ERR_PLATFORM_GATED;
    if (err == XIAOTAI_OK && !strcmp(req->service, XIAOTAI_PLATFORM_SERVICE_MQTT)) err = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
    if (err == XIAOTAI_OK && body_size >= platform.request_limit) err = XIAOTAI_ERR_INVALID_ARG;
    if (err == XIAOTAI_OK && req->auth == XIAOTAI_PLATFORM_AUTH_SESSION && (!platform.status.authorized ||
        (platform.config.requests_need_signaling && (!platform.status.signaling || platform.status.held))))
        err = XIAOTAI_ERR_PLATFORM_OFFLINE;
    if (err == XIAOTAI_OK && req->auth == XIAOTAI_PLATFORM_AUTH_SIGNED &&
        (!xiaotai_clock_synced() || xiaotai_identity_state() != XIAOTAI_IDENTITY_VALID)) err = XIAOTAI_ERR_PLATFORM_OFFLINE;
    if (err == XIAOTAI_OK && platform.config.defer_background_while_active &&
        (req->flags & XIAOTAI_PLATFORM_BACKGROUND) && platform_active_now()) err = XIAOTAI_ERR_BUSY;
    if (err == XIAOTAI_OK && req->auth == XIAOTAI_PLATFORM_AUTH_BEARER && platform.bearer) err = XIAOTAI_ERR_NO_MEM;
    size_t index = 0;
    if (err == XIAOTAI_OK) {
        if (profile_id && platform.profile_limit > platform.request_limit) index = platform.slot_count - 1U;
        while (index < platform.slot_count && platform.slots[index].state != PLATFORM_FREE) ++index;
        if (index == platform.slot_count) {
            const bool compat = platform.config.request_full_timeout && !profile_id &&
                (!transfer || (req->flags & XIAOTAI_PLATFORM_TRANSFER_FULL_TIMEOUT));
            err = compat ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_ERR_NO_MEM;
        }
    }
    platform_slot_t *slot = err == XIAOTAI_OK ? &platform.slots[index] : NULL;
    char *body = slot ? slot->body : NULL;
    const size_t body_capacity = profile_id ? platform.profile_limit : body_size ? body_size + 1U : 0;
    if (err == XIAOTAI_OK && !platform.external && body_capacity) {
        body = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, body_capacity);
        if (!body) err = XIAOTAI_ERR_NO_MEM;
    }
    if (err == XIAOTAI_OK && req->auth == XIAOTAI_PLATFORM_AUTH_BEARER) {
        platform.bearer = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, XIAOTAI_PLATFORM_BEARER_MAX);
        if (!platform.bearer) err = XIAOTAI_ERR_NO_MEM;
        else strcpy(platform.bearer, req->bearer);
    }
    if (err != XIAOTAI_OK) {
        if (!platform.external && body) xiaotai_mem_free(body);
        ++platform.stats.requests_refused;
        if (err == XIAOTAI_ERR_BUSY) ++platform.stats.requests_busy;
        platform_unlock();
        return err;
    }

    uint32_t sequence = ++platform.id_next;
    if (!sequence) sequence = ++platform.id_next;
    const uint32_t ms = req->timeout_ms ? req->timeout_ms : XIAOTAI_PLATFORM_TIMEOUT_DEFAULT_MS;
    *slot = (platform_slot_t){
        .state = PLATFORM_QUEUED, .is_transfer = transfer != NULL, .id = sequence,
        .generation = expected_generation ? expected_generation : xiaotai_identity_generation(),
        .session_revision = platform.session_revision,
        .network_revision = platform.network_revision, .deadline_us = xiaotai_time_us() + (int64_t)ms * 1000,
        .exchange_timeout_ms = transfer ? transfer->exchange_timeout_ms : 0U,
        .method = req->method, .auth = req->auth, .flags = req->flags, .body = body, .body_size = body_size,
        .profile_id = profile_id,
    };
    strcpy(slot->service, req->service);
    strcpy(slot->path, req->path);
    if (body) {
        if (body_size) memcpy(body, req->body, body_size);
        body[body_size] = '\0';
    }
    if (profile_id) {
        /* Reserve a real request slot without lending it to the worker yet.
         * A scene writer may call framework and platform APIs on the owner. */
        slot->state = PLATFORM_DRAFT;
        ++platform.active;
        platform.profile.in_slot = true;
        platform_count_publish();
        platform_unlock();
        const int written = platform_profile_document(body, platform.profile_limit);
        platform_lock();
        if (!platform_profile_current(profile_id)) err = XIAOTAI_ERR_PLATFORM_STALE;
        else if (platform.status.held) err = XIAOTAI_ERR_PLATFORM_GATED;
        else if (written <= 0 || (size_t)written >= platform.profile_limit) err = XIAOTAI_ERR_INVALID_SIZE;
        if (err != XIAOTAI_OK) {
            slot->body_size = platform.profile_limit;
            platform_slot_release(slot);
            platform_unlock();
            platform_wake();
            return err;
        }
        slot->state = PLATFORM_QUEUED;
        slot->body_size = (size_t)written;
        ++platform.stats.profile_reports;
    }
    if (transfer) {
        slot->handler.transfer.stream = *stream;
        if (transfer->body_size) strcpy(slot->handler.transfer.content_type, transfer->content_type);
        slot->handler.transfer.source_size = transfer->body_size;
        slot->handler.transfer.response_max = transfer->response_max;
        slot->handler.transfer.stall_ms = transfer->stall_timeout_ms ? transfer->stall_timeout_ms : PLATFORM_DEFAULT_STALL_MS;
        ++platform.stats.transfers;
    } else {
        slot->handler.reply.fn = reply;
        slot->handler.reply.ctx = ctx;
        ++platform.stats.requests;
    }
    /* Publish before the worker sees the ready slot and before the caller
     * returns success. A transport callback can refuse on this single read. */
    if (!profile_id) ++platform.active;
    platform_count_publish();
    if (slot->flags & XIAOTAI_PLATFORM_URGENT) {
        memmove(platform.pending + 1U, platform.pending, platform.queue_count);
        platform.pending[0] = (uint8_t)index;
    } else {
        platform.pending[platform.queue_count] = (uint8_t)index;
    }
    ++platform.queue_count;
    if (id) *id = slot->id;
    platform_unlock();
    platform_wake();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *req, xiaotai_platform_reply_fn_t reply, void *ctx)
{
    return admit(req, NULL, NULL, reply, ctx, NULL, 0, 0);
}

xiaotai_err_t xiaotai_platform_request_for_generation(const xiaotai_platform_request_t *req,
                                                      uint32_t generation,
                                                      xiaotai_platform_reply_fn_t reply, void *ctx)
{
    if (!generation) return XIAOTAI_ERR_INVALID_ARG;
    return admit(req, NULL, NULL, reply, ctx, NULL, 0, generation);
}

xiaotai_err_t platform_profile_request(uint32_t id, xiaotai_platform_reply_fn_t reply)
{
    const xiaotai_platform_request_t request = {.method = XIAOTAI_PLATFORM_POST, .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
        .service = XIAOTAI_PLATFORM_SERVICE_DEVICE, .path = "/v1/device/profile", .timeout_ms = 10000,
        .flags = XIAOTAI_PLATFORM_BACKGROUND};
    return admit(&request, NULL, NULL, reply, (void *)(uintptr_t)id, NULL, id, 0);
}

xiaotai_err_t xiaotai_platform_transfer(const xiaotai_platform_transfer_t *transfer,
                                        const xiaotai_platform_stream_t *stream, uint32_t *id)
{
    return admit(transfer ? &transfer->request : NULL, transfer, stream, NULL, NULL, id, 0, 0);
}

void platform_slot_body_free(platform_slot_t *slot)
{
    if (platform.external || !slot->body) return;
    const size_t capacity = slot->profile_id ? platform.profile_limit : slot->body_size + 1U;
    xiaotai_identity_wipe(slot->body, capacity);
    xiaotai_mem_free(slot->body);
    slot->body = NULL;
    slot->body_size = 0;
}

void platform_slot_release(platform_slot_t *slot)
{
    if (slot->auth == XIAOTAI_PLATFORM_AUTH_BEARER && platform.bearer) {
        xiaotai_identity_wipe(platform.bearer, XIAOTAI_PLATFORM_BEARER_MAX);
        xiaotai_mem_free(platform.bearer);
        platform.bearer = NULL;
    }
    platform_slot_body_free(slot);
    char *body = slot->body;
    if (body) xiaotai_identity_wipe(body, slot->body_size);
    if (slot->profile_id && slot->profile_id == platform.profile.id) platform.profile.in_slot = false;
    *slot = (platform_slot_t){.body = body};
    --platform.active;
    platform_count_publish();
}

typedef struct {
    xiaotai_http_header_t header[6];
    size_t count;
    union {
        char bearer[7U + XIAOTAI_PLATFORM_BEARER_MAX];
        platform_signature_t signed_headers;
    } value;
} request_headers_t;

static void header_add(request_headers_t *headers, const char *name, const char *value)
{
    headers->header[headers->count++] = (xiaotai_http_header_t){.name = name, .value = value};
}

static xiaotai_err_t device_signature(request_headers_t *headers)
{
    const xiaotai_err_t err = platform_signature_make(&headers->value.signed_headers);
    if (err == XIAOTAI_OK) headers->count += platform_signature_headers(&headers->value.signed_headers,
        headers->header + headers->count);
    return err;
}

static xiaotai_err_t make_headers(const platform_slot_t *slot, request_headers_t *headers)
{
    if (slot->method != XIAOTAI_PLATFORM_GET) {
        header_add(headers, "Content-Type", slot->is_transfer && slot->handler.transfer.source_size ?
                     slot->handler.transfer.content_type : "application/json");
    }
    if (slot->auth == XIAOTAI_PLATFORM_AUTH_SIGNED) return device_signature(headers);
    if (slot->auth == XIAOTAI_PLATFORM_AUTH_NONE) return XIAOTAI_OK;
    platform_lock();
    const char *bearer = slot->auth == XIAOTAI_PLATFORM_AUTH_BEARER ? platform.bearer : platform.token;
    if (bearer) snprintf(headers->value.bearer, sizeof(headers->value.bearer), "Bearer %s", bearer);
    platform_unlock();
    if (!bearer) return XIAOTAI_ERR_PLATFORM_STALE;
    header_add(headers, "Authorization", headers->value.bearer);
    return XIAOTAI_OK;
}

typedef struct {
    platform_result_t *result;
    xiaotai_http_response_t *http;
} sink_t;

static xiaotai_err_t sink_write(void *ctx, const uint8_t *data, size_t size)
{
    sink_t *sink = ctx;
    platform_result_t *result = sink->result;
    if (memchr(data, 0, size)) return XIAOTAI_ERR_INVALID_ARG;
    const size_t offset = result->reply.size;
    size_t capacity = platform.response_limit;
    char *buffer = platform.response;
    if (!buffer) {
        if (!result->temporary_body) {
            const int64_t length = sink->http->content_length;
            if (length >= 0 && (uint64_t)length < capacity) capacity = (size_t)length;
            result->temporary_capacity = capacity + 1U;
            result->temporary_body = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, result->temporary_capacity);
            if (!result->temporary_body) return XIAOTAI_ERR_NO_MEM;
        }
        capacity = result->temporary_capacity - 1U;
        buffer = result->temporary_body;
    }
    if (offset > capacity || size > capacity - offset) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(buffer + offset, data, size);
    result->reply.size += size;
    buffer[result->reply.size] = '\0';
    result->reply.body = buffer;
    return XIAOTAI_OK;
}

static void reset_reply_body(platform_result_t *result)
{
    if (result->temporary_body) {
        xiaotai_identity_wipe(result->temporary_body, result->temporary_capacity);
        xiaotai_mem_free(result->temporary_body);
        result->temporary_body = NULL;
        result->temporary_capacity = 0;
    }
    result->reply.body = NULL;
    result->reply.size = 0;
}

void platform_http_run(platform_slot_t *slot, platform_result_t *result)
{
    char url[XIAOTAI_PLATFORM_URL_MAX + XIAOTAI_PLATFORM_PATH_MAX];
    request_headers_t headers = {0};
    platform_lock();
    const char *base = NULL;
    xiaotai_err_t err = platform_service_find(slot->service, &base);
    if (err == XIAOTAI_OK) err = platform_url_join(base, slot->path, url, sizeof(url));
    platform_unlock();
    if (err == XIAOTAI_OK) err = make_headers(slot, &headers);
    if (err != XIAOTAI_OK) goto finish;

    if (!platform.client) {
        const xiaotai_http_options_t opts = {.no_delay = platform.config.http_no_delay};
        xiaotai_http_t *client = NULL;
        err = xiaotai_http_create(&opts, &client);
        if (err != XIAOTAI_OK) goto finish;
        platform_lock();
        platform.client = client;
        platform_unlock();
    }
    platform_lock();
    if (slot->cancelled) err = XIAOTAI_ERR_PLATFORM_CANCELLED;
    else if (platform_slot_stale(slot)) err = XIAOTAI_ERR_PLATFORM_STALE;
    else if (platform_profile_hold_blocks(slot->profile_id)) err = XIAOTAI_ERR_PLATFORM_GATED;
    else if ((slot->flags & XIAOTAI_PLATFORM_BACKGROUND) && platform.config.defer_background_while_active &&
             platform_active_now()) err = XIAOTAI_ERR_BUSY;
    const bool release = (slot->flags & XIAOTAI_PLATFORM_RELEASE) != 0;
    /* A second caller may arrive while the first request is on the wire.
     * Decide whether to retain its usable connection at reply handoff, not
     * at the start of I/O when the queue may still look empty. */
    const bool keep = !platform.gate && (platform.config.idle_keep_ms || !release);
    if (err == XIAOTAI_OK) platform.io = slot;
    platform_unlock();
    if (err != XIAOTAI_OK) goto finish;

    xiaotai_http_response_t response = {.content_length = -1};
    sink_t sink = {.http = &response, .result = result};
    platform_transfer_io_t transfer = {0};
    xiaotai_http_request_t http = {
        .method = (xiaotai_http_method_t)slot->method, .url = url, .headers = headers.header, .header_count = headers.count,
        .body = slot->body_size ? slot->body : NULL, .body_size = slot->body_size, .keep_alive = keep,
    };
    bool retry = false;
    do {
        http.timeout_ms = platform_remaining_ms(slot->deadline_us);
        if (!http.timeout_ms) { err = XIAOTAI_ERR_TIMEOUT; break; }
        if (slot->exchange_timeout_ms && http.timeout_ms > slot->exchange_timeout_ms)
            http.timeout_ms = slot->exchange_timeout_ms;
        response = (xiaotai_http_response_t){.sink = sink_write, .sink_ctx = &sink, .content_length = -1,
            .sink_limit = platform.response_limit ? platform.response_limit : SIZE_MAX};
        if (slot->is_transfer) platform_transfer_io_init(&transfer, slot, &result->transfer, &http, &response);
        err = xiaotai_http_perform(platform.client, &http, &response);
        if (!retry && !slot->is_transfer && slot->method == XIAOTAI_PLATFORM_GET && platform.config.get_retry_on_reuse &&
            err != XIAOTAI_OK && response.reused && !response.status) {
            xiaotai_http_close(platform.client);
            reset_reply_body(result);
            retry = true;
            continue;
        }
        break;
    } while (true);
    if (slot->is_transfer && transfer.response) err = platform_transfer_io_finish(&transfer, err);
    if (err != XIAOTAI_OK || response.status < 200 || response.status >= 300 || response.redirect) {
        xiaotai_http_close(platform.client);
    }
    platform_lock();
    platform.io = NULL;
    platform.keep_until = response.kept && platform.config.idle_keep_ms ?
        xiaotai_time_us() + (int64_t)platform.config.idle_keep_ms * 1000 : 0;
    platform_unlock();
    platform_http_trace(slot, &response, err);
    result->reply.http_status = result->transfer.http_status = response.status;
    result->transfer.sent = response.stage >= XIAOTAI_HTTP_STAGE_SEND;
    if (response.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_HEAD)
        result->transfer.receive_fault = XIAOTAI_PLATFORM_RECEIVE_FAULT_HEAD;
    else if (response.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY)
        result->transfer.receive_fault = XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY;
    else if (response.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED)
        result->transfer.receive_fault = XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY_TRUNCATED;
finish:
    /* Only the response is borrowed by the owner. HTTP no longer uses the
     * request body, even if delivery has to wait for the owner queue. */
    platform_slot_body_free(slot);
    xiaotai_identity_wipe(&headers, sizeof(headers));
    result->reply.result = result->transfer.result = err;
    if (!slot->is_transfer && err == XIAOTAI_OK) {
        if (!result->reply.body) result->reply.body = "";
        (void)xiaotai_platform_json_code(result->reply.body, result->reply.size, &result->reply.code);
    } else if (err != XIAOTAI_OK) {
        result->reply.body = NULL;
        result->reply.size = 0;
    }
}
