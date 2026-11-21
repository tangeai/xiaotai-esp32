// SPDX-License-Identifier: MIT
#include "wechat_internal.h"

#include <string.h>
#include "xiaotai/identity.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/transport.h"

typedef struct {
    uint32_t epoch, binding;
    size_t capacity;
    bool retry;
    char body[];
} refusal_t;

static refusal_t *s_pending;
static uint32_t s_epoch;
static bool s_recovery, s_online;

static void forget(refusal_t *request)
{
    if (!request) return;
    volatile char *body = request->body;
    for (size_t i = 0; i < request->capacity; ++i) body[i] = 0;
    xiaotai_mem_free(request);
}

static void defer(refusal_t *request)
{
    if (!s_pending) {
        s_pending = request;
        s_recovery = false;
    } else {
        forget(request);
    }
}

static void replied(void *ctx, xiaotai_err_t result, int status, const char *body, size_t size)
{
    (void)status;
    (void)body;
    (void)size;
    refusal_t *request = ctx;
    if (request->epoch == s_epoch && request->binding == xiaotai_identity_generation() && result != XIAOTAI_OK) {
        wx_refusal_noted(true);
        if (!request->retry) {
            defer(request);
            return;
        }
    }
    forget(request);
}

static void send(refusal_t *request, bool retry)
{
    request->retry = retry;
    if (!retry) wx_refusal_noted(false);
    const xiaotai_err_t err = xiaotai_transport_service_request("/v1/wxvoip/reject", request->body,
                                                                 replied, request);
    if (err != XIAOTAI_OK) replied(request, err, 0, NULL, 0);
}

static void retry_pending(void)
{
    if (!s_pending) return;
    if (s_pending->binding != xiaotai_identity_generation()) {
        forget(s_pending);
        s_pending = NULL;
        s_recovery = false;
        return;
    }
    if (!s_recovery || !s_online || xiaotai_platform_background_deferred() ||
        xiaotai_transport_state() != XIAOTAI_TRANSPORT_ONLINE) return;
    xiaotai_platform_status_t status;
    xiaotai_platform_status(&status);
    if (status.phase != XIAOTAI_PLATFORM_ONLINE) return;
    refusal_t *request = s_pending;
    s_pending = NULL;
    s_recovery = false;
    send(request, true);
}

static void retry_event(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    retry_pending();
}

xiaotai_err_t wx_refusals_init(void)
{
    xiaotai_platform_status_t status;
    xiaotai_platform_status(&status);
    s_online = status.phase == XIAOTAI_PLATFORM_ONLINE;
    xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ACTIVITY, retry_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_TRANSPORT_STATE, retry_event, NULL);
    return err;
}

void wx_refusals_platform(const xiaotai_event_t *event)
{
    if (strcmp(event->name, XIAOTAI_EVENT_PLATFORM_OFFLINE) == 0) {
        s_online = false;
    } else if (strcmp(event->name, XIAOTAI_EVENT_PLATFORM_ONLINE) == 0 && !s_online) {
        s_online = true;
        if (s_pending) s_recovery = true;
        retry_pending();
    }
}

void wx_refusals_reset(void)
{
    ++s_epoch;
    forget(s_pending);
    s_pending = NULL;
    s_recovery = s_online = false;
}

void wx_refuse(const wx_room_t *room, uint8_t reason, bool wait_online)
{
    if (reason == XIAOTAI_WECHAT_REASON_NONE) return;
    if (!room || reason > 20 || room->invalid_refusal_fields) {
        wx_refusal_invalid();
        return;
    }
    static const char *const keys[] = {
        "wx_app_id", "wx_model_id", "wx_session_token", "wx_room_id", "wx_payload",
    };
    const struct {const char *text; size_t bound; bool required;} fields[] = {
        {room->app_id, sizeof(room->app_id), true},
        {room->model_id, sizeof(room->model_id), true},
        {room->server_token, sizeof(room->server_token), wx_config()->require_reject_server_token},
        {room->room_id, sizeof(room->room_id), true},
        {room->payload, sizeof(room->payload), false},
    };
    size_t capacity = sizeof("{\"hangup_reason\":20}");
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        const size_t length = strnlen(fields[i].text, fields[i].bound);
        if (length == fields[i].bound || (!length && fields[i].required)) {
            wx_refusal_invalid();
            return;
        }
        capacity += strlen(keys[i]) + 6U;
        for (size_t j = 0; j < length; ++j) {
            const unsigned char ch = (unsigned char)fields[i].text[j];
            capacity += ch < 0x20U ? 6U : (ch == '"' || ch == '\\' ? 2U : 1U);
        }
    }
    const size_t limit = wx_config()->reject_body_bytes ? wx_config()->reject_body_bytes :
                         XIAOTAI_TRANSPORT_SERVICE_BODY_MAX;
    if (capacity > limit) capacity = limit;
    refusal_t *request = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*request) + capacity);
    if (!request) {
        wx_refusal_noted(true);
        return;
    }
    request->capacity = capacity;
    request->epoch = s_epoch;
    request->binding = xiaotai_identity_generation();
    xiaotai_json_writer_t json;
    xiaotai_json_writer_init(&json, request->body, capacity);
    xiaotai_json_object_begin(&json, NULL);
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i)
        xiaotai_json_put_string(&json, keys[i], fields[i].text);
    xiaotai_json_put_int(&json, "hangup_reason", reason);
    xiaotai_json_object_end(&json);
    if (xiaotai_json_writer_end(&json, NULL) != XIAOTAI_OK) {
        wx_refusal_invalid();
        forget(request);
    } else if (wait_online) {
        wx_refusal_noted(false);
        defer(request);
    } else {
        send(request, false);
    }
}
