// SPDX-License-Identifier: MIT
#include "room_internal.h"

#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/json.h"
#include "xiaotai/port/mem.h"

#define ROOM_SYNC_MS 30000
#define ROOM_SYNC_RETRY_MS 5000
#define ROOM_HTTP_MS 8000U
#define ROOM_PATH "/v1/call/group/device/"

typedef enum { ASSIGNMENT_VALID, ASSIGNMENT_OLD, ASSIGNMENT_RECEIPT, ASSIGNMENT_BAD } assignment_result_t;

bool room_request_can_yield(room_request_t request, xiaotai_room_action_t action)
{
    return request == ROOM_REQUEST_NONE || request == ROOM_REQUEST_ASSIGNMENT ||
           (action == XIAOTAI_ROOM_ACTION_LEAVE && (request == ROOM_REQUEST_TOKEN || request == ROOM_REQUEST_PRESENCE));
}

void room_read_again(room_t *room)
{
    ++room->notice;
    room->sync_at = room_now_ms();
    /* Going offline changes the phase but leaves a blocked connection latched.
     * Wait for an authoritative assignment before reopening that connection. */
    const bool retry_latched = room->link.retry_at == INT64_MAX;
    if (retry_latched) room->view.assignment_known = false;
    if (room->view.phase == XIAOTAI_ROOM_BLOCKED || retry_latched) {
        room->view.phase = XIAOTAI_ROOM_OFFLINE;
        room->view.attempts = 0;
        room->link.retry_at = 0;
    }
}

void room_retire_read(room_t *room)
{
    if (room->pending == ROOM_REQUEST_ASSIGNMENT && !room->confirming) room->pending = ROOM_REQUEST_NONE;
}

static void clear_mutation(room_t *room)
{
    memset(&room->mutation, 0, sizeof(room->mutation));
    room->mutation_queued = false;
    room->confirming = false;
    room->mutation_at = INT64_MAX;
}

void room_assignment_reset(room_t *room)
{
    clear_mutation(room);
    room->pending = ROOM_REQUEST_NONE;
    room->sync_at = INT64_MAX;
    room->manual_requested = room->manual_disconnected = false;
    room->view.assignment_known = room->view.assigned = room->view.busy = false;
    room->view.attempts = 0;
    room->view.error = XIAOTAI_OK;
    room->view.code = 0;
    room->view.assignment_version = 0;
    memset(room->view.room_id, 0, sizeof(room->view.room_id));
    memset(room->view.room_code, 0, sizeof(room->view.room_code));
    memset(room->view.self_name, 0, sizeof(room->view.self_name));
}

static bool object(const xiaotai_json_doc_t *doc, int index)
{
    return index >= 0 && doc->tokens[index].type == XIAOTAI_JSON_OBJECT;
}

static assignment_result_t apply(room_t *room, const xiaotai_json_doc_t *doc, bool receipt_allowed, bool *left)
{
    char device_id[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U], id[XIAOTAI_ROOM_ID_MAX] = {0};
    char code[XIAOTAI_ROOM_CODE_DIGITS + 1U] = {0}, name[XIAOTAI_ROOM_NAME_MAX] = {0};
    const int data = xiaotai_json_get(doc, 0, "data");
    if (!object(doc, data)) {
        return receipt_allowed && (data < 0 || doc->tokens[data].type == XIAOTAI_JSON_NULL)
                   ? ASSIGNMENT_RECEIPT : ASSIGNMENT_BAD;
    }
    const int device = xiaotai_json_get(doc, data, "device_id");
    const int desired = xiaotai_json_get(doc, data, "desired_state");
    const int version = xiaotai_json_get(doc, data, "assignment_version");
    const bool device_matches = device >= 0 &&
        xiaotai_identity_device_id(device_id, sizeof(device_id)) == XIAOTAI_OK && device_id[0] &&
        xiaotai_json_equals(doc, device, device_id);
    if (receipt_allowed && desired < 0 && version < 0) {
        return device < 0 || device_matches ? ASSIGNMENT_RECEIPT : ASSIGNMENT_BAD;
    }
    int64_t next;
    const bool joined = xiaotai_json_equals(doc, desired, "joined");
    if (!device_matches || xiaotai_json_int(doc, version, &next) != XIAOTAI_OK ||
        next < 0 || next > UINT32_MAX ||
        (!joined && !xiaotai_json_equals(doc, desired, "left") && !xiaotai_json_equals(doc, desired, ""))) {
        return ASSIGNMENT_BAD;
    }
    if (joined && (next == 0 ||
        xiaotai_json_string(doc, xiaotai_json_get(doc, data, "room_id"), id, sizeof(id), NULL) != XIAOTAI_OK || !id[0] ||
        xiaotai_json_string(doc, xiaotai_json_get(doc, data, "room_code"), code, sizeof(code), NULL) != XIAOTAI_OK ||
        !room_digits(code, XIAOTAI_ROOM_CODE_DIGITS, false))) return ASSIGNMENT_BAD;
    if ((uint32_t)next < room->view.assignment_version) return ASSIGNMENT_OLD;

    const bool named = xiaotai_json_string(doc, xiaotai_json_get(doc, data, "device_name"), name, sizeof(name), NULL) == XIAOTAI_OK;
    const bool different_room = strcmp(id, room->view.room_id) != 0;
    const bool changed = joined != room->view.assigned || (uint32_t)next != room->view.assignment_version || different_room;
    *left = !joined || different_room;
    if (changed) {
        room_end_connection(room, ROOM_END_ASSIGNMENT);
        room->view.attempts = 0;
        room->link.retry_at = 0;
    }
    room->view.assignment_known = true;
    room->view.assigned = joined;
    room->view.assignment_version = (uint32_t)next;
    if (changed || !room->link.retry_at) {
        room->view.error = XIAOTAI_OK;
        room->view.code = 0;
    }
    memcpy(room->view.room_id, id, sizeof(id));
    memcpy(room->view.room_code, code, sizeof(code));
    if (named) memcpy(room->view.self_name, name, sizeof(name));
    return ASSIGNMENT_VALID;
}

static assignment_result_t response_assignment(room_t *room, const xiaotai_platform_reply_t *reply,
                                                bool allow_receipt, bool *left, xiaotai_err_t *error)
{
    size_t count = 0;
    *error = xiaotai_json_parse(reply->body, reply->size, NULL, 0, &count, NULL);
    if (*error != XIAOTAI_OK) return ASSIGNMENT_BAD;
    xiaotai_json_token_t *tokens = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, count * sizeof(*tokens));
    if (!tokens) { *error = XIAOTAI_ERR_NO_MEM; return ASSIGNMENT_BAD; }
    xiaotai_json_doc_t doc;
    *error = xiaotai_json_parse(reply->body, reply->size, tokens, count, &count, &doc);
    const assignment_result_t result = *error == XIAOTAI_OK && object(&doc, 0)
                                          ? apply(room, &doc, allow_receipt, left) : ASSIGNMENT_BAD;
    xiaotai_mem_free(tokens);
    return result;
}

static void mutation_result(room_t *room, xiaotai_err_t error, int32_t code)
{
    room_result(room->mutation.action, error, code);
    clear_mutation(room);
    if (room->view.phase == XIAOTAI_ROOM_LEAVING) room->view.phase = XIAOTAI_ROOM_OFFLINE;
    if (error != XIAOTAI_OK) {
        room->view.error = error;
        room->view.code = code;
    }
}

int32_t room_reply_code(const xiaotai_platform_reply_t *reply)
{
    if (reply->http_status == 401) return 401;
    return reply->http_status == 200 && reply->code > 0 ? reply->code : 0;
}

static void on_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    room_t *room = room_context();
    if (!room) return;
    if (room->flights) --room->flights;
    room_update_context(room);
    if (room->pending == ROOM_REQUEST_NONE || (uintptr_t)ctx != room->ticket || reply->generation != room->binding) {
        ++room->stats.stale;
        if (room->mutation_queued) room->mutation_at = room_now_ms();
        room_drive(room);
        return;
    }
    const room_request_t request = room->pending;
    const bool read = request == ROOM_REQUEST_ASSIGNMENT;
    const bool leave = room->mutation.action == XIAOTAI_ROOM_ACTION_LEAVE && (!read || room->confirming);
    room->pending = ROOM_REQUEST_NONE;
    const int64_t now = room_now_ms();
    if (reply->result == XIAOTAI_ERR_PLATFORM_STALE) {
        ++room->stats.stale;
        room_end_connection(room, ROOM_END_OFFLINE);
        room_assignment_reset(room);
        room->sync_at = now + ROOM_SYNC_RETRY_MS;
        room_drive(room);
        return;
    }
    if (read && room->read_notice != room->notice) {
        /* An invalidation after this GET began outranks its snapshot. Applying
         * it could end an active link before the current assignment is known. */
        ++room->stats.stale;
        room->sync_at = now;
        if (room->confirming) room->mutation_at = now;
        room_drive(room);
        return;
    }
    const int32_t code = room_reply_code(reply);
    xiaotai_err_t error = reply->result;
    bool left = false;
    assignment_result_t result = ASSIGNMENT_BAD;
    if (error == XIAOTAI_OK) {
        if (code == 200) result = response_assignment(room, reply, !read, &left, &error);
        if (error == XIAOTAI_OK && result == ASSIGNMENT_BAD) error = XIAOTAI_FAIL;
    }
    if (result == ASSIGNMENT_OLD) {
        ++room->stats.assignments_stale;
        ++room->stats.stale;
    }
    if (!read && error != XIAOTAI_OK) {
        mutation_result(room, error, code == 200 ? 0 : code);
        if (room->view.attending && code != XIAOTAI_ROOM_CODE_WRONG_PASSWORD && code != XIAOTAI_ROOM_CODE_FULL &&
            code != XIAOTAI_ROOM_CODE_PASSWORD_LOCKED && code != XIAOTAI_ROOM_CODE_ALREADY_MEMBER) room_read_again(room);
    } else if (!read && (result != ASSIGNMENT_VALID || (leave && !left))) {
        /* A mutation receipt is not authoritative membership. Follow it with
         * GET, even off-page, and never replay the accepted POST. */
        room->confirming = true;
        room->mutation_at = result == ASSIGNMENT_OLD ? now + ROOM_SYNC_RETRY_MS : now;
        if (!leave) room->view.assignment_known = false;
    } else if (!read) {
        mutation_result(room, XIAOTAI_OK, code);
        if (room->view.attending) room_read_again(room);
    } else {
        if (error != XIAOTAI_OK) {
            ++room->stats.sync_failures;
            if (!room_connection_sync_failed(room, error, code)) {
                room->view.error = error;
                room->view.code = code == 200 ? 0 : code;
            }
            if (code == 401 || code == XIAOTAI_ROOM_CODE_UNBOUND) room->view.assignment_known = false;
        }
        const bool reread = error != XIAOTAI_OK &&
            (code == XIAOTAI_ROOM_CODE_NOT_FOUND || code == XIAOTAI_ROOM_CODE_SUPERSEDED) &&
            room->view.phase == XIAOTAI_ROOM_OFFLINE;
        room->sync_at = reread ? now : now + (result == ASSIGNMENT_VALID ? ROOM_SYNC_MS : ROOM_SYNC_RETRY_MS);
        if (code == XIAOTAI_ROOM_CODE_UNBOUND) {
            room->sync_at = INT64_MAX;
            room->view.phase = XIAOTAI_ROOM_BLOCKED;
        }
        if (room->confirming) {
            if (result == ASSIGNMENT_VALID && (!leave || left)) mutation_result(room, XIAOTAI_OK, code);
            else if (code == 401 || code == XIAOTAI_ROOM_CODE_UNBOUND) mutation_result(room, error, code);
            else room->mutation_at = now + ROOM_SYNC_RETRY_MS;
        }
    }
    room_drive(room);
}

static xiaotai_err_t start_request(room_t *room, room_request_t request)
{
    static const char *const paths[] = {[ROOM_REQUEST_ASSIGNMENT] = ROOM_PATH "assignment",
        [ROOM_REQUEST_CREATE] = ROOM_PATH "create", [ROOM_REQUEST_JOIN] = ROOM_PATH "join", [ROOM_REQUEST_LEAVE] = ROOM_PATH "leave"};
    const bool read = request == ROOM_REQUEST_ASSIGNMENT;
    if (room->flights >= 2U) return XIAOTAI_ERR_NO_MEM;
    if (!read) {
        xiaotai_json_writer_t writer;
        xiaotai_json_writer_init(&writer, room->body, sizeof(room->body));
        xiaotai_json_object_begin(&writer, NULL);
        if (request == ROOM_REQUEST_LEAVE) {
            xiaotai_json_put_string(&writer, "room_id", room->view.room_id);
            xiaotai_json_put_int(&writer, "assignment_version", room->view.assignment_version);
        } else {
            if (request == ROOM_REQUEST_JOIN) xiaotai_json_put_string(&writer, "room_code", room->mutation.code);
            xiaotai_json_put_string(&writer, "password", room->mutation.password);
        }
        xiaotai_json_object_end(&writer);
        const xiaotai_err_t err = xiaotai_json_writer_end(&writer, NULL);
        if (err != XIAOTAI_OK) { memset(room->body, 0, sizeof(room->body)); return err; }
    }
    const xiaotai_platform_request_t http = {.service = XIAOTAI_PLATFORM_SERVICE_CALL,
        .method = read ? XIAOTAI_PLATFORM_GET : XIAOTAI_PLATFORM_POST,
        .path = paths[request], .body = read ? NULL : room->body, .timeout_ms = ROOM_HTTP_MS,
        .auth = XIAOTAI_PLATFORM_AUTH_SESSION};
    uint32_t next = room->ticket + 1U;
    if (!next) ++next;
    const xiaotai_err_t err = xiaotai_platform_request(&http, on_reply, (void *)(uintptr_t)next);
    if (!read) memset(room->body, 0, sizeof(room->body));
    if (err != XIAOTAI_OK) return err;
    room->ticket = next;
    ++room->flights;
    room->pending = request;
    if (read) {
        room->read_notice = room->notice;
        ++room->stats.syncs;
    } else {
        room->mutation_queued = false;
        memset(room->mutation.password, 0, sizeof(room->mutation.password));
    }
    return XIAOTAI_OK;
}

static bool is_mutation(room_request_t request)
{
    return request >= ROOM_REQUEST_CREATE && request <= ROOM_REQUEST_LEAVE;
}

static void update_phase(room_t *room)
{
    xiaotai_room_status_t *view = &room->view;
    view->busy = room->mutation_queued || room->confirming || is_mutation(room->pending);
    view->wait = XIAOTAI_ROOM_WAIT_NONE;
    if (view->busy && room->mutation.action == XIAOTAI_ROOM_ACTION_LEAVE) view->phase = XIAOTAI_ROOM_LEAVING;
    else if (view->phase == XIAOTAI_ROOM_BLOCKED || view->phase == XIAOTAI_ROOM_CONNECTING ||
             view->phase == XIAOTAI_ROOM_LISTENING || view->phase == XIAOTAI_ROOM_SPEAKING) return;
    else if (!view->assignment_known && !(view->assigned && !view->attending)) {
        view->phase = view->attending && room->online ? XIAOTAI_ROOM_SYNCING : XIAOTAI_ROOM_OFFLINE;
    } else if (!view->assigned) view->phase = XIAOTAI_ROOM_EMPTY;
    else {
        view->phase = XIAOTAI_ROOM_WAITING;
        if (!view->attending) view->wait = XIAOTAI_ROOM_WAIT_NOT_ATTENDING;
        else if (room->manual_disconnected || (room->config.manual_connect && !room->manual_requested)) view->wait = XIAOTAI_ROOM_WAIT_MANUAL;
        else if (!room->online) view->wait = XIAOTAI_ROOM_WAIT_OFFLINE;
        else if (room->release_pending) view->wait = XIAOTAI_ROOM_WAIT_RELEASE;
    }
}

void room_assignment_drive(room_t *room, int64_t now)
{
    if (room->mutation_queued && room->mutation.action != XIAOTAI_ROOM_ACTION_LEAVE &&
        room->mutation.attendance != room->attendance) {
        ++room->stats.stale;
        clear_mutation(room);
    }
    /* The urgent release can consume the second platform slot even though
     * the abandoned heartbeat is no longer the tracked request. Its reply
     * wakes the owner; do not overwrite the actual failure with NO_MEM. */
    if (room->online && !xiaotai_quiesced() && room->pending == ROOM_REQUEST_NONE && room->flights < 2U) {
        if (room->mutation_queued && now >= room->mutation_at) {
            const xiaotai_room_action_t action = room->mutation.action;
            const room_request_t request = action == XIAOTAI_ROOM_ACTION_CREATE ? ROOM_REQUEST_CREATE :
                action == XIAOTAI_ROOM_ACTION_JOIN ? ROOM_REQUEST_JOIN : ROOM_REQUEST_LEAVE;
            const xiaotai_err_t err = start_request(room, request);
            if (err == XIAOTAI_ERR_NO_MEM || (action == XIAOTAI_ROOM_ACTION_LEAVE &&
                (err == XIAOTAI_ERR_PLATFORM_GATED || err == XIAOTAI_ERR_PLATFORM_OFFLINE))) {
                room->mutation_at = now + ROOM_SYNC_RETRY_MS;
            } else if (err != XIAOTAI_OK) mutation_result(room, err, 0);
        } else if ((room->confirming && now >= room->mutation_at) ||
                   (!room->confirming && !room->mutation_queued && room->view.attending && now >= room->sync_at)) {
            const xiaotai_err_t err = start_request(room, ROOM_REQUEST_ASSIGNMENT);
            if (err != XIAOTAI_OK) {
                ++room->stats.sync_failures;
                room->view.error = err;
                room->view.code = 0;
                if (room->confirming) room->mutation_at = now + ROOM_SYNC_RETRY_MS;
                else room->sync_at = now + ROOM_SYNC_RETRY_MS;
            }
        }
    }
    update_phase(room);
}

void room_assignment_action(room_t *room, const room_action_t *action)
{
    const xiaotai_room_action_t kind = action->action;
    const bool leave = kind == XIAOTAI_ROOM_ACTION_LEAVE;
    if (xiaotai_quiesced()) { room_result(kind, XIAOTAI_ERR_INVALID_STATE, 0); return; }
    if (kind == XIAOTAI_ROOM_ACTION_REFRESH || kind == XIAOTAI_ROOM_ACTION_CONNECT || kind == XIAOTAI_ROOM_ACTION_DISCONNECT) {
        if (!room->view.attending || (kind == XIAOTAI_ROOM_ACTION_CONNECT && !room->view.assigned)) {
            room_result(kind, XIAOTAI_ERR_INVALID_STATE, 0);
            return;
        }
        if (kind != XIAOTAI_ROOM_ACTION_DISCONNECT &&
            (!room_request_can_yield(room->pending, kind) || room->mutation_queued || room->confirming ||
             (kind == XIAOTAI_ROOM_ACTION_CONNECT && room->link.stage != ROOM_LINK_NONE))) {
            room_result(kind, XIAOTAI_ERR_BUSY, 0);
            return;
        }
        room->view.attempts = 0;
        room->link.retry_at = 0;
        room->manual_disconnected = kind == XIAOTAI_ROOM_ACTION_DISCONNECT;
        if (room->manual_disconnected) {
            room->manual_requested = false;
            room_end_connection(room, ROOM_END_MANUAL);
        } else {
            room_retire_read(room);
            if (kind == XIAOTAI_ROOM_ACTION_CONNECT) room->manual_requested = true;
            if (kind == XIAOTAI_ROOM_ACTION_REFRESH) room_snapshot_request(room, false);
            room_read_again(room);
        }
        room_result(kind, XIAOTAI_OK, 0);
        return;
    }
    if (is_mutation(room->pending) || room->mutation_queued || room->confirming || !room_request_can_yield(room->pending, kind)) {
        room_result(kind, XIAOTAI_ERR_BUSY, 0);
    } else if ((!room->view.assignment_known && !(leave && room->view.assigned)) ||
               (!leave && (!room->view.attending || !room->online))) {
        room_result(kind, XIAOTAI_ERR_INVALID_STATE, 0);
    } else if (!leave && room->view.assigned) {
        room_result(kind, XIAOTAI_ERR_INVALID_STATE, XIAOTAI_ROOM_CODE_ALREADY_MEMBER);
    } else if (leave && !room->view.assigned) {
        room_result(kind, XIAOTAI_OK, 0);
    } else {
        if (leave) room_end_connection(room, ROOM_END_LEAVE);
        room->pending = ROOM_REQUEST_NONE;
        room->mutation = *action;
        room->mutation_queued = true;
        room->mutation_at = room_now_ms();
    }
}
