// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_upload.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "product_platform.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"

enum { SUCCESS, DNS, CONNECT, SEND, EARLY_EOF, BODY_RESET, BODY_STALL, EMPTY_EOF, HEAD_RESET };
enum { SAMPLE_COUNT = 16000, RECORDING_ID = 777, WAIT_MS = 6000 };
static const char *const modes[] = {"success", "dns", "connect", "send", "eof", "body-reset", "body-stall",
    "empty-eof", "head-reset"};
static const char config[] =
    "{\"code\":200,\"data\":{\"policy\":{\"enabled\":true,\"allowed\":true,\"monthly_limit\":100,\"revision\":4,"
    "\"schemes\":[{\"scheme_id\":\"fixture\",\"scheme_version\":1}]},\"effective_enabled\":true,"
    "\"upload_limits\":{\"max_file_bytes\":320044,\"max_audio_seconds\":10},"
    "\"usage\":{\"used\":3,\"input_tokens\":0,\"output_tokens\":0,\"tts_characters\":0,\"unknown_calls\":0}}}";
static const char origin[] = "http://fixture.invalid/recognition";
static const char submit[] = "http://fixture.invalid/recognition/v1/recognition/requests";
static bool enabled;
static unsigned mode;
static atomic_uint policies, captures, posts, full_bodies;
static atomic_size_t response_bytes;
static atomic_uint response_stage, response_fault;
static xiaotai_media_record_t recording;
static char receipt[384], content_type[128], request_id[XIAOTAI_RECOGNITION_ID_MAX];

xiaotai_err_t __real_xiaotai_media_record(const xiaotai_media_record_t *request, uint32_t *id);
xiaotai_err_t __wrap_xiaotai_media_record(const xiaotai_media_record_t *request, uint32_t *id);
xiaotai_err_t __real_xiaotai_http_perform(xiaotai_http_t *, const xiaotai_http_request_t *, xiaotai_http_response_t *);
xiaotai_err_t __wrap_xiaotai_http_perform(xiaotai_http_t *, const xiaotai_http_request_t *, xiaotai_http_response_t *);
void product_recognition_upload_probe_prepare(const char *name);
void product_recognition_upload_probe(void);
bool product_recognition_upload_has_fixture_headroom(void);

bool product_recognition_upload_has_fixture_headroom(void)
{
    /* Host mem_stats has no PSRAM geometry; actual alloc/free still run. */
    return enabled && xiaotai_on_owner() && !atomic_load(&captures);
}

static void captured(void *context)
{
    (void)context;
    assert(xiaotai_on_owner());
    recording.done(RECORDING_ID, XIAOTAI_OK, recording.samples, recording.ctx);
}

xiaotai_err_t __wrap_xiaotai_http_perform(xiaotai_http_t *client, const xiaotai_http_request_t *request,
                                       xiaotai_http_response_t *response)
{
    const xiaotai_err_t result = __real_xiaotai_http_perform(client, request, response);
    if (enabled && request && request->url && !strcmp(request->url, submit)) {
        atomic_store(&response_bytes, response->size);
        atomic_store(&response_fault, response->receive_fault);
        atomic_store(&response_stage, response->stage);
    }
    return result;
}

xiaotai_err_t __wrap_xiaotai_media_record(const xiaotai_media_record_t *request, uint32_t *id)
{
    if (!enabled) return __real_xiaotai_media_record(request, id);
    assert(xiaotai_on_owner() && request->samples == SAMPLE_COUNT && request->pcm && request->done && id);
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    assert(info.capture_rate_hz == SAMPLE_COUNT);
    assert(atomic_fetch_add(&captures, 1U) == 0U && atomic_load(&policies) >= 1U);
    unsigned char *pcm = (unsigned char *)request->pcm;
    for (size_t i = 0; i < request->samples * sizeof(int16_t); ++i) pcm[i] = (unsigned char)(i % 251U);
    recording = *request;
    *id = RECORDING_ID;
    return xiaotai_run(captured, NULL);
}

static void full_body(void *context, const uint8_t *body, size_t size)
{
    (void)context;
    assert(atomic_fetch_add(&full_bodies, 1U) == 0U && atomic_load(&posts) == 1U);
    const char *path = getenv("PRODUCT_UPLOAD_CAPTURE_PATH");
    assert(path);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fprintf(file, "MIME-Version: 1.0\r\nContent-Type: %s\r\n\r\n", content_type) > 0);
    assert(fwrite(body, 1U, size, file) == size && fclose(file) == 0);
}

static xiaotai_recognition_status_t status(void)
{
    xiaotai_recognition_status_t current = {0};
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        if (xiaotai_recognition_status(&current) == XIAOTAI_OK) return current;
        xiaotai_delay_ms(1U);
    }
    assert(!"recognition status unavailable");
    return current;
}

static void server(void *context, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)context;
    if (strncmp(req->url, origin, sizeof(origin) - 1U)) {
        reply->status = 404;
        return;
    }
    const char *path = req->url + sizeof(origin) - 1U;
    unsigned auth = 0, mime = 0;
    for (size_t i = 0; i < req->header_count; ++i) {
        if (!strcmp(req->headers[i].name, "Authorization")) {
            assert(!strcmp(req->headers[i].value, "Bearer a.eyJleHAiOjE3MDAwMDA1MDB9.b"));
            ++auth;
        } else if (!strcmp(req->headers[i].name, "Content-Type")) {
            const int size = snprintf(content_type, sizeof(content_type), "%s", req->headers[i].value);
            assert(size > 0 && (size_t)size < sizeof(content_type));
            ++mime;
        }
    }
    assert(auth == 1U);
    reply->close = true;
    if (!strcmp(path, XIAOTAI_RECOGNITION_CONFIG_PATH)) {
        assert(req->method == XIAOTAI_HTTP_GET);
        atomic_fetch_add(&policies, 1U);
        reply->body = (const uint8_t *)config;
        reply->body_size = strlen(config);
        reply->content_length = (int64_t)reply->body_size;
        return;
    }
    assert(!strcmp(path, "/v1/recognition/requests") && req->method == XIAOTAI_HTTP_POST);
    assert(req->streamed && req->has_content_length && !req->body && req->body_size > 32000 && !req->reused);
    assert(atomic_fetch_add(&posts, 1U) == 0U && atomic_load(&captures) == 1U && atomic_load(&policies) == 3U);
    assert(mime == 1U);
    xiaotai_recognition_status_t current = status();
    assert(current.request_id[0] && strlen(current.request_id) < sizeof(request_id));
    strcpy(request_id, current.request_id);
    const int size = snprintf(receipt, sizeof(receipt), "{\"code\":200,\"data\":{\"request_id\":\"%s\","
        "\"device_id\":\"" PRODUCT_FIXTURE_DEVICE "\",\"status_revision\":1,\"status\":\"queued\",\"results\":[]}}",
        request_id);
    assert(size > 23 && (size_t)size < sizeof(receipt));
    reply->status = 202;
    reply->body = (const uint8_t *)receipt;
    reply->body_size = mode == EARLY_EOF ? 23U : mode == EMPTY_EOF ? 0U : (size_t)size;
    reply->content_length = size;
    reply->read_chunk_bytes = 23U;
    reply->uploaded = full_body;
    if (mode >= DNS && mode <= SEND) {
        const xiaotai_http_stage_t stages[] = {XIAOTAI_HTTP_STAGE_RESOLVE, XIAOTAI_HTTP_STAGE_CONNECT, XIAOTAI_HTTP_STAGE_SEND};
        reply->fail_stage = stages[mode - DNS];
        reply->error = XIAOTAI_FAIL;
    } else if (mode == BODY_RESET || mode == BODY_STALL || mode == HEAD_RESET) {
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
        reply->fail_after_bytes = mode == HEAD_RESET ? 0U : 23U;
        reply->error = mode == BODY_STALL ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_FAIL;
    }
}

void product_recognition_upload_probe_prepare(const char *name)
{
    while (mode < sizeof(modes) / sizeof(modes[0]) && strcmp(modes[mode], name)) ++mode;
    assert(mode < sizeof(modes) / sizeof(modes[0]));
    enabled = true;
    product_platform_prepare(true);
    product_platform_http_responder(server, NULL);
}

void product_recognition_upload_probe(void)
{
    product_platform_online();
    xiaotai_recognition_status_t current = {0};
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        current = status();
        if (current.config_revision == 4U && current.phase == XIAOTAI_RECOGNITION_IDLE && !current.cleanup_pending) break;
        xiaotai_delay_ms(5U);
    }
    assert(current.config_revision == 4U && current.phase == XIAOTAI_RECOGNITION_IDLE && !current.cleanup_pending);
    assert(atomic_load(&policies) == 1U);
    assert(xiaotai_recognition_record(1U) == XIAOTAI_OK);
    const bool sent = mode != DNS && mode != CONNECT;
    const bool full_upload = mode == SUCCESS || mode >= EARLY_EOF;
    const unsigned target = mode == SUCCESS ? XIAOTAI_RECOGNITION_ACCEPTED : sent ?
        XIAOTAI_RECOGNITION_RECOVERY : XIAOTAI_RECOGNITION_FAILED;
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        current = status();
        if (current.phase == target && !current.cleanup_pending) break;
        xiaotai_delay_ms(5U);
    }
    if (current.phase != target || current.cleanup_pending) {
        fprintf(stderr, "case=%s want=%u actual=%u cleanup=%u err=%d decision=%u/%d config=%llu/%d "
                "http=%d config_http=%d recovery=%u policy=%u capture=%u POST=%u port_stage=%u\n",
                modes[mode], target, current.phase, current.cleanup_pending, current.error,
                current.decision_action, current.decision_error, (unsigned long long)current.config_revision,
                current.config_error, current.http_status, current.config_http_status, current.possibly_sent,
                atomic_load(&policies), atomic_load(&captures), atomic_load(&posts), atomic_load(&response_stage));
    }
    assert(current.phase == target && !current.cleanup_pending);
    const xiaotai_err_t expected[] = {XIAOTAI_OK, XIAOTAI_FAIL, XIAOTAI_FAIL, XIAOTAI_FAIL,
        0x108, XIAOTAI_FAIL, XIAOTAI_ERR_TIMEOUT, XIAOTAI_FAIL, XIAOTAI_FAIL};
    assert(current.http_status == (full_upload && mode != HEAD_RESET ? 202 : 0));
    assert(current.error == expected[mode] && current.possibly_sent == sent);
    const xiaotai_http_stage_t stages[] = {XIAOTAI_HTTP_STAGE_DONE, XIAOTAI_HTTP_STAGE_RESOLVE,
        XIAOTAI_HTTP_STAGE_CONNECT, XIAOTAI_HTTP_STAGE_SEND};
    assert(atomic_load(&response_stage) == (unsigned)(mode < EARLY_EOF ? stages[mode] : XIAOTAI_HTTP_STAGE_RECEIVE));
    const xiaotai_http_receive_fault_t faults[] = {0, 0, 0, 0, XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED,
        XIAOTAI_HTTP_RECEIVE_FAULT_BODY, XIAOTAI_HTTP_RECEIVE_FAULT_BODY, XIAOTAI_HTTP_RECEIVE_FAULT_BODY,
        XIAOTAI_HTTP_RECEIVE_FAULT_HEAD};
    assert(atomic_load(&response_fault) == faults[mode]);
    assert(atomic_load(&response_bytes) == (mode == SUCCESS ? strlen(receipt) :
           mode >= EARLY_EOF && mode <= BODY_STALL ? 23U : 0U));
    assert(atomic_load(&posts) == 1U && atomic_load(&captures) == 1U &&
           atomic_load(&full_bodies) == (unsigned)full_upload);
    char pending[384];
    const xiaotai_err_t read = xiaotai_kv_get_str_strict("xt_recognition", "pending", pending,
                                                        sizeof(pending), XIAOTAI_KV_WAIT_MAX_MS);
    assert(sent ? read == XIAOTAI_OK && strstr(pending, request_id) && !strcmp(current.request_id, request_id)
                : read == XIAOTAI_ERR_NOT_FOUND && !current.request_id[0]);
    printf("UPLOAD_PROBE %s phase=%u err=%d http=%d possibly_sent=%d pending=%d posts=%u full_bodies=%u "
           "port_stage=%u body_fault=%u response_bytes=%zu request_id=%s\n",
           modes[mode], current.phase, current.error, current.http_status, sent, read == XIAOTAI_OK,
           atomic_load(&posts), atomic_load(&full_bodies), atomic_load(&response_stage),
           atomic_load(&response_fault), atomic_load(&response_bytes), request_id);
}
