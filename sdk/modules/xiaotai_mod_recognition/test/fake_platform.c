// SPDX-License-Identifier: MIT
#include "operation_fixture.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"

#include <string.h>

unsigned fake_gets, fake_posts, fake_queries, fake_downloads, fake_unbound;
bool fake_online = true, fake_unbind_in_hook;
bool fake_http_busy, fake_transfer_busy, fake_transfer_cancelled;
const char *fake_address = "http://rec.invalid";
xiaotai_err_t fake_endpoint_error;
uint32_t fake_generation = 1;
uint32_t fake_notice_revision;
xiaotai_platform_stream_t fake_stream;
size_t fake_body_size;
char fake_query_path[XIAOTAI_PLATFORM_PATH_MAX];
char fake_audio_path[XIAOTAI_PLATFORM_PATH_MAX];
static xiaotai_platform_reply_fn_t reply;
static void *reply_ctx;
static uint32_t reply_gen, transfer_gen;
static bool returning_reply;
static struct {
    const char *type;
    xiaotai_platform_signal_fn_t callback;
    void *ctx;
} subscriptions[2][2];

uint32_t xiaotai_identity_generation(void) { return fake_generation; }

xiaotai_err_t xiaotai_identity_device_id(char *out, size_t capacity)
{
    const char device[] = "dev";
    if (capacity < sizeof(device)) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(out, device, sizeof(device));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *requirement)
{
    CHECK(requirement->response_bytes == 2048 && requirement->concurrent_requests >= 1);
    return XIAOTAI_OK;
}

void xiaotai_platform_status(xiaotai_platform_status_t *status)
{
    *status = (xiaotai_platform_status_t){.generation = fake_generation,
        .notify_revision = fake_notice_revision,
        .phase = fake_online ? XIAOTAI_PLATFORM_ONLINE : XIAOTAI_PLATFORM_STOPPED};
}

xiaotai_err_t xiaotai_platform_endpoint(const char *service, char *out, size_t capacity)
{
    CHECK(strcmp(service, "recognition") == 0 && capacity > strlen(fake_address));
    strcpy(out, fake_address);
    return fake_endpoint_error;
}

xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *request,
                                        xiaotai_platform_reply_fn_t fn, void *ctx)
{
    CHECK(xiaotai_on_owner() && !reply && !fake_stream.done && !returning_reply);
    CHECK(request->method == XIAOTAI_PLATFORM_GET && request->auth == XIAOTAI_PLATFORM_AUTH_SESSION);
    CHECK(request->timeout_ms == 15000 && !request->body &&
          (!request->flags || request->flags == XIAOTAI_PLATFORM_BACKGROUND));
    CHECK(strcmp(request->service, "recognition") == 0 && strcmp(request->path, "/v1/recognition/config") == 0);
    if (fake_http_busy) return XIAOTAI_ERR_NO_MEM;
    ++fake_gets;
    reply = fn;
    reply_ctx = ctx;
    reply_gen = fake_generation;
    return XIAOTAI_OK;
}

void fake_policy_reply(const char *body, int http, int code, xiaotai_err_t error)
{
    CHECK(xiaotai_on_owner() && reply && !fake_stream.done);
    const xiaotai_platform_reply_t result = {.result = error, .http_status = http, .code = code,
        .body = body, .size = body ? strlen(body) : 0, .generation = reply_gen};
    xiaotai_platform_reply_fn_t fn = reply;
    reply = NULL;
    returning_reply = true;
    if (http == 410 && code == XIAOTAI_PLATFORM_CODE_UNBOUND && fake_unbind_in_hook) {
        ++fake_generation; /* P2's hook runs before the module's callback. */
        fake_online = false;
    }
    fn(&result, reply_ctx);
    returning_reply = false;
}

xiaotai_err_t xiaotai_platform_transfer(const xiaotai_platform_transfer_t *transfer,
                                         const xiaotai_platform_stream_t *stream, uint32_t *id)
{
    CHECK(xiaotai_on_owner() && !fake_stream.done && !reply && !returning_reply);
    CHECK(transfer->request.auth == XIAOTAI_PLATFORM_AUTH_SESSION);
    CHECK(strcmp(transfer->request.service, "recognition") == 0);
    const bool get = transfer->request.method == XIAOTAI_PLATFORM_GET;
    const bool download = get && strstr(transfer->request.path, "/audio/") != NULL;
    if (get) {
        const char path_prefix[] = "/v1/recognition/requests/";
        CHECK(strncmp(transfer->request.path, path_prefix, sizeof(path_prefix) - 1U) == 0);
        CHECK(strlen(transfer->request.path) < sizeof(fake_query_path));
        CHECK(transfer->request.timeout_ms == 20000 && !transfer->body_size &&
              !transfer->request.body && !stream->read && stream->begin && stream->write);
        if (download) {
            CHECK(transfer->request.flags == XIAOTAI_PLATFORM_TRANSFER_FULL_TIMEOUT && transfer->stall_timeout_ms == 5000U);
            CHECK(transfer->response_max >= 44U && transfer->response_max <= 10U * 1024U * 1024U);
            CHECK(!strcmp(transfer->request.path, "/v1/recognition/requests/request/audio/item_1") ||
                  !strcmp(transfer->request.path, "/v1/recognition/requests/request/audio/item_2"));
        } else {
            CHECK(transfer->request.flags == XIAOTAI_PLATFORM_BACKGROUND && transfer->response_max == 1024U * 1024U);
        }
    } else {
        CHECK(transfer->request.method == XIAOTAI_PLATFORM_POST && !transfer->request.flags);
        CHECK(strcmp(transfer->request.path, "/v1/recognition/requests") == 0);
        CHECK(transfer->request.timeout_ms == 60000 && transfer->stall_timeout_ms == 5000 &&
              transfer->response_max == 8192);
        const char prefix[] = "multipart/form-data; boundary=xiaotai-";
        CHECK(transfer->body_size && strncmp(transfer->content_type, prefix, sizeof(prefix) - 1U) == 0);
    }
    if (fake_transfer_busy) return XIAOTAI_ERR_NO_MEM;
    if (download) {
        ++fake_downloads;
        strcpy(fake_audio_path, transfer->request.path);
    } else if (get) {
        ++fake_queries;
        strcpy(fake_query_path, transfer->request.path);
    } else ++fake_posts;
    fake_transfer_cancelled = false;
    transfer_gen = fake_generation;
    fake_body_size = transfer->body_size;
    fake_stream = *stream;
    *id = 57;
    return XIAOTAI_OK;
}

void xiaotai_platform_transfer_cancel(uint32_t id)
{
    CHECK(xiaotai_on_owner() && fake_stream.done && id == 57);
    fake_transfer_cancelled = true; /* does not return the borrow */
}

void fake_upload_done(const char *body, int http, bool sent, xiaotai_err_t error)
{
    CHECK(xiaotai_on_owner() && fake_stream.done && !returning_reply);
    const size_t size = body ? strlen(body) : 0;
    if (size) {
        CHECK(fake_stream.write(fake_stream.ctx, (const uint8_t *)body, 3) == XIAOTAI_OK);
        CHECK(fake_stream.write(fake_stream.ctx, (const uint8_t *)body + 3, size - 3) == XIAOTAI_OK);
    }
    const xiaotai_platform_transfer_result_t result = {.result = error, .http_status = http,
        .sent = sent, .received = size, .generation = transfer_gen};
    const xiaotai_platform_stream_t stream = fake_stream;
    fake_stream = (xiaotai_platform_stream_t){0};
    stream.done(stream.ctx, &result);
}

void fake_query_done(const void *body, size_t size, int64_t declared, size_t chunk, int http, xiaotai_err_t error)
{
    CHECK(xiaotai_on_owner() && fake_stream.done && fake_stream.begin && fake_stream.write && !returning_reply);
    const xiaotai_platform_response_head_t head = {.http_status = http, .content_length = declared};
    const xiaotai_platform_stream_t stream = fake_stream;
    size_t received = 0;
    if (error == XIAOTAI_OK) error = stream.begin(stream.ctx, &head);
    while (error == XIAOTAI_OK && received < size) {
        const size_t n = !chunk || size - received < chunk ? size - received : chunk;
        error = stream.write(stream.ctx, (const uint8_t *)body + received, n);
        if (error == XIAOTAI_OK) received += n;
    }
    const xiaotai_platform_transfer_result_t result = {.result = error, .http_status = http,
        .sent = true, .received = received, .generation = transfer_gen};
    fake_stream = (xiaotai_platform_stream_t){0};
    returning_reply = true;
    stream.done(stream.ctx, &result);
    returning_reply = false;
}

void fake_download_done(int http, size_t received, xiaotai_err_t error)
{
    CHECK(xiaotai_on_owner() && fake_stream.done && !returning_reply);
    const xiaotai_platform_transfer_result_t result = {.result = error, .http_status = http,
        .sent = true, .received = received, .generation = transfer_gen};
    const xiaotai_platform_stream_t stream = fake_stream;
    fake_stream = (xiaotai_platform_stream_t){0};
    returning_reply = true;
    stream.done(stream.ctx, &result);
    returning_reply = false;
}

xiaotai_err_t xiaotai_platform_signal_subscribe(const char *type, const char *channel,
                                                xiaotai_platform_signal_fn_t fn, void *ctx)
{
    CHECK(channel && (!channel[0] || strcmp(channel, "recognition") == 0));
    const size_t index = strcmp(type, "recognition_result") == 0 ? 0U : 1U;
    CHECK(index == 0U || strcmp(type, "recognition_config_changed") == 0);
    const size_t slot = channel[0] ? 0U : 1U;
    CHECK(!subscriptions[index][slot].callback);
    subscriptions[index][slot].type = type;
    subscriptions[index][slot].callback = fn;
    subscriptions[index][slot].ctx = ctx;
    return XIAOTAI_OK;
}

bool fake_notice_channel(const char *type, const char *channel, const char *body, uint32_t generation)
{
    CHECK(xiaotai_on_owner());
    const size_t index = strcmp(type, "recognition_result") == 0 ? 0U : 1U;
    if (channel[0] && strcmp(channel, "recognition")) return false;
    const size_t slot = channel[0] ? 0U : 1U;
    if (!subscriptions[index][slot].callback || strcmp(type, subscriptions[index][slot].type)) return false;
    const xiaotai_platform_signal_t signal = {.type = type, .channel = channel, .json = body,
        .size = strlen(body), .generation = generation};
    subscriptions[index][slot].callback(&signal, subscriptions[index][slot].ctx);
    return true;
}

void fake_notice(const char *type, const char *body, uint32_t generation)
{
    CHECK(fake_notice_channel(type, "recognition", body, generation));
}

void xiaotai_platform_report_unbound(bool confirmed)
{
    CHECK(xiaotai_on_owner() && confirmed);
    ++fake_unbound;
}
