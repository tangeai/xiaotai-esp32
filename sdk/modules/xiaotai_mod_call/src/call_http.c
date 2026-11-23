// SPDX-License-Identifier: MIT
#include "call_internal.h"

#include <limits.h>
#include <string.h>

#include "xiaotai/port/mem.h"

#define CALL_HTTP_MS 28000U
#define CALL_ROOM_HTTP_MS 4000U

static xiaotai_err_t request(const char *path, const char *body, uint32_t timeout, uint32_t flags,
                            xiaotai_platform_reply_fn_t reply, uint32_t session, uint32_t binding)
{
    const xiaotai_platform_request_t req = {
        .service = XIAOTAI_PLATFORM_SERVICE_CALL, .method = body ? XIAOTAI_PLATFORM_POST : XIAOTAI_PLATFORM_GET,
        .path = path, .body = body, .timeout_ms = timeout, .flags = flags,
    };
    void *const ctx = (void *)(uintptr_t)session;
    return binding ? xiaotai_platform_request_for_generation(&req, binding, reply, ctx) :
        xiaotai_platform_request(&req, reply, ctx);
}

static void dial_reply(const xiaotai_platform_reply_t *reply, void *context)
{
    call_dial_reply((uint32_t)(uintptr_t)context, false, reply);
}

static void redial_reply(const xiaotai_platform_reply_t *reply, void *context)
{
    call_dial_reply((uint32_t)(uintptr_t)context, true, reply);
}

static void accept_reply(const xiaotai_platform_reply_t *reply, void *context)
{
    call_accept_reply((uint32_t)(uintptr_t)context, reply);
}

static void repair_query(const xiaotai_platform_reply_t *reply, void *context)
{
    call_repair_reply((uint32_t)(uintptr_t)context, CALL_REPAIR_QUERY, reply);
}

static void repair_release(const xiaotai_platform_reply_t *reply, void *context)
{
    call_repair_reply((uint32_t)(uintptr_t)context, CALL_REPAIR_RELEASE, reply);
}

static void repair_verify(const xiaotai_platform_reply_t *reply, void *context)
{
    call_repair_reply((uint32_t)(uintptr_t)context, CALL_REPAIR_VERIFY, reply);
}

xiaotai_err_t call_submit_dial(uint32_t session, const char *target, bool video, bool redial, uint32_t binding)
{
    char body[CALL_COMMAND_BYTES];
    xiaotai_json_writer_t w;
    xiaotai_json_writer_init(&w, body, sizeof(body));
    xiaotai_json_object_begin(&w, NULL);
    xiaotai_json_array_begin(&w, "targets");
    xiaotai_json_put_string(&w, NULL, target);
    xiaotai_json_array_end(&w);
    xiaotai_json_put_string(&w, "call_type", video ? "video" : "audio");
    xiaotai_json_object_end(&w);
    const xiaotai_err_t err = xiaotai_json_writer_end(&w, NULL);
    return err == XIAOTAI_OK ? request("/v1/call/request", body, CALL_HTTP_MS, 0,
                                       redial ? redial_reply : dial_reply, session, binding) : err;
}

xiaotai_err_t call_submit_accept(uint32_t session, const char *room, const char *peer)
{
    char body[CALL_COMMAND_BYTES];
    xiaotai_json_writer_t w;
    xiaotai_json_writer_init(&w, body, sizeof(body));
    xiaotai_json_object_begin(&w, NULL);
    xiaotai_json_put_string(&w, "device_id", peer);
    xiaotai_json_put_string(&w, "room_id", room);
    xiaotai_json_put_string(&w, "purpose", "call");
    xiaotai_json_object_end(&w);
    const xiaotai_err_t err = xiaotai_json_writer_end(&w, NULL);
    return err == XIAOTAI_OK ? request("/v1/call/device/info", body, CALL_HTTP_MS, XIAOTAI_PLATFORM_RELEASE,
                                       accept_reply, session, 0) : err;
}

static xiaotai_err_t release(xiaotai_call_state_t phase, const char *room, const char *reason, uint32_t session,
                             uint32_t flags, xiaotai_platform_reply_fn_t reply)
{
    if (!room[0]) return XIAOTAI_ERR_INVALID_ARG;
    const char *path = "/v1/call/hangup";
    if (phase == XIAOTAI_CALL_INCOMING) {
        path = "/v1/call/reject";
        if (!reason) reason = "decline";
    } else if (phase == XIAOTAI_CALL_OUTGOING) {
        path = "/v1/call/cancel";
        reason = NULL;
    } else if (!reason) reason = "hangup";
    char body[CALL_COMMAND_BYTES];
    xiaotai_json_writer_t w;
    xiaotai_json_writer_init(&w, body, sizeof(body));
    xiaotai_json_object_begin(&w, NULL);
    xiaotai_json_put_string(&w, "room_id", room);
    if (reason) xiaotai_json_put_string(&w, "reason", reason);
    xiaotai_json_object_end(&w);
    const xiaotai_err_t err = xiaotai_json_writer_end(&w, NULL);
    return err == XIAOTAI_OK ? request(path, body, CALL_ROOM_HTTP_MS, flags | XIAOTAI_PLATFORM_URGENT,
                                       reply, session, 0) : err;
}

xiaotai_err_t call_submit_release(xiaotai_call_state_t phase, const char *room, const char *reason, uint32_t ticket,
                                 uint32_t flags, xiaotai_platform_reply_fn_t reply)
{
    return release(phase, room, reason, ticket, flags, reply);
}

xiaotai_err_t call_submit_room_check(uint32_t ticket, uint32_t flags, xiaotai_platform_reply_fn_t reply)
{
    return request("/v1/call/room", NULL, CALL_ROOM_HTTP_MS, flags, reply, ticket, 0);
}

xiaotai_err_t call_submit_repair(uint32_t session, call_repair_step_t step, const char *room, xiaotai_call_state_t phase)
{
    if (step == CALL_REPAIR_RELEASE) return release(phase, room, NULL, session, 0, repair_release);
    if (step != CALL_REPAIR_QUERY && step != CALL_REPAIR_VERIFY) return XIAOTAI_ERR_INVALID_ARG;
    return request("/v1/call/room", NULL, CALL_ROOM_HTTP_MS, 0,
                   step == CALL_REPAIR_QUERY ? repair_query : repair_verify, session, 0);
}

typedef enum { FIELD_ABSENT, FIELD_PRESENT, FIELD_DUPLICATE } field_result_t;
typedef struct {
    const char *name;
    xiaotai_json_view_t value;
    field_result_t state;
} field_t;

/* HTTP distinguishes duplicate members from absent fields, unlike signaling. */
static void fields(const xiaotai_json_view_t *object, field_t *out, size_t count)
{
    if (object->type != XIAOTAI_JSON_OBJECT) return;
    xiaotai_json_view_iter_t it;
    xiaotai_json_view_iter(object, &it);
    xiaotai_json_view_t key, value;
    while (xiaotai_json_view_next(&it, &key, &value)) {
        for (size_t n = 0; n < count; ++n) {
            field_t *field = &out[n];
            if (!xiaotai_json_view_equals(&key, field->name)) continue;
            if (field->state == FIELD_ABSENT) {
                field->state = FIELD_PRESENT;
                field->value = value;
            } else {
                field->state = FIELD_DUPLICATE;
                field->value = (xiaotai_json_view_t){0};
            }
            break;
        }
    }
}

int32_t call_http_code(const xiaotai_platform_reply_t *reply, call_http_payload_t *payload)
{
    /* Only the call service's HTTP 200 envelope proves a business outcome;
     * status 401 is handled independently as its JWT middleware's refusal. */
    if (payload) *payload = (call_http_payload_t){0};
    xiaotai_json_view_t root;
    if (reply->result != XIAOTAI_OK || reply->http_status != 200 || !call_parse(reply->body, reply->size, &root))
        return XIAOTAI_PLATFORM_NO_CODE;
    field_t root_fields[] = {{.name = "code"}, {.name = "data"}};
    fields(&root, root_fields, payload ? 2U : 1U);
    int64_t code;
    if (xiaotai_json_view_int(&root_fields[0].value, &code) != XIAOTAI_OK || code <= 0 || code > INT32_MAX)
        return XIAOTAI_PLATFORM_NO_CODE;
    if (payload) {
        payload->data = root_fields[1].value;
        payload->duplicate = root_fields[1].state == FIELD_DUPLICATE;
    }
    return (int32_t)code;
}

bool call_http_room(const call_http_payload_t *payload, char room[CALL_ROOM_SIZE])
{
    field_t id = {.name = "room_id"};
    fields(&payload->data, &id, 1U);
    return call_string(&id.value, room, CALL_ROOM_SIZE);
}

bool call_http_room_id_valid(const char *room)
{
    if (room[0] != 'd' || room[1] != '_' || room[2] == '\0') return false;
    for (size_t i = 2; room[i]; ++i) {
        const unsigned char c = (unsigned char)room[i];
        if (i >= CALL_ROOM_SIZE - 1U || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                         (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

xiaotai_err_t call_http_release_action(const xiaotai_platform_reply_t *reply, const char *expected_room,
                                      xiaotai_call_state_t *phase)
{
    call_http_payload_t payload;
    if (call_http_code(reply, &payload) != XIAOTAI_PLATFORM_CODE_OK) {
        return reply->result == XIAOTAI_OK ? CALL_INVALID_RESPONSE : reply->result;
    }
    if ((!payload.data.type && !payload.duplicate) || payload.data.type == XIAOTAI_JSON_NULL) {
        *phase = XIAOTAI_CALL_IDLE;
        return XIAOTAI_OK;
    }
    char room[CALL_ROOM_SIZE];
    field_t data[] = {{.name = "room_id"}, {.name = "role"}, {.name = "status"}};
    fields(&payload.data, data, 3U);
    if (!call_string(&data[0].value, room, sizeof(room)) || !call_http_room_id_valid(room))
        return CALL_INVALID_RESPONSE;
    if (!expected_room[0]) return XIAOTAI_ERR_BUSY;
    if (strcmp(room, expected_room)) {
        *phase = XIAOTAI_CALL_IDLE;
        return XIAOTAI_OK;
    }
    if (!xiaotai_json_view_equals(&data[1].value, "caller")) return XIAOTAI_ERR_BUSY;
    if (xiaotai_json_view_equals(&data[2].value, "active")) *phase = XIAOTAI_CALL_OUTGOING;
    else if (xiaotai_json_view_equals(&data[2].value, "answered")) *phase = XIAOTAI_CALL_ACTIVE;
    else return CALL_INVALID_RESPONSE;
    return XIAOTAI_OK;
}

xiaotai_err_t call_http_repair_action(const xiaotai_platform_reply_t *reply, bool verify,
                                    const char *old_room, xiaotai_call_state_t *phase)
{
    call_http_payload_t payload;
    if (call_http_code(reply, &payload) != XIAOTAI_PLATFORM_CODE_OK) {
        return reply->result == XIAOTAI_OK ? CALL_INVALID_RESPONSE : reply->result;
    }
    if ((!payload.data.type && !payload.duplicate) || payload.data.type == XIAOTAI_JSON_NULL) {
        *phase = XIAOTAI_CALL_IDLE;
        return XIAOTAI_OK;
    }
    char room[CALL_ROOM_SIZE];
    if (verify) return XIAOTAI_ERR_BUSY;
    field_t data[] = {{.name = "room_id"}, {.name = "role"}, {.name = "status"}};
    fields(&payload.data, data, 3U);
    if (!call_string(&data[0].value, room, sizeof(room)) || strcmp(room, old_room) != 0) return XIAOTAI_ERR_BUSY;
    const bool caller = xiaotai_json_view_equals(&data[1].value, "caller");
    const bool callee = xiaotai_json_view_equals(&data[1].value, "callee");
    if (!caller && !callee) return CALL_INVALID_RESPONSE;
    if (xiaotai_json_view_equals(&data[2].value, "answered")) *phase = XIAOTAI_CALL_ACTIVE;
    else if (xiaotai_json_view_equals(&data[2].value, "active"))
        *phase = caller ? XIAOTAI_CALL_OUTGOING : XIAOTAI_CALL_INCOMING;
    else return CALL_INVALID_RESPONSE;
    return XIAOTAI_OK;
}

xiaotai_err_t call_http_accept_token(const xiaotai_platform_reply_t *reply, const char *expected_peer,
                                   bool *denied, char **token)
{
    *token = NULL;
    call_http_payload_t payload;
    const int32_t code = call_http_code(reply, &payload);
    *denied = reply->http_status == 401 || code == 40000 || code == 40210 || code == 40300 || code == 40400;
    if (code != XIAOTAI_PLATFORM_CODE_OK) return reply->result == XIAOTAI_OK ? CALL_INVALID_RESPONSE : reply->result;
    field_t data[] = {{.name = "device_id"}, {.name = "token"}};
    fields(&payload.data, data, 2U);
    if (!xiaotai_json_view_equals(&data[0].value, expected_peer)) return CALL_INVALID_RESPONSE;
    const xiaotai_json_view_t *value = &data[1].value;
    if (value->type != XIAOTAI_JSON_STRING || value->length <= 2U) {
        return CALL_INVALID_RESPONSE;
    }
    char *decoded = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, XIAOTAI_LINK_TOKEN_MAX);
    if (!decoded) return XIAOTAI_ERR_NO_MEM;
    if (!call_string(value, decoded, XIAOTAI_LINK_TOKEN_MAX)) {
        call_http_free_token(decoded);
        return CALL_INVALID_RESPONSE;
    }
    *token = decoded;
    return XIAOTAI_OK;
}

void call_http_free_token(char *token)
{
    if (!token) return;
    for (volatile char *p = token; p < token + XIAOTAI_LINK_TOKEN_MAX; ++p) *p = '\0';
    xiaotai_mem_free(token);
}
