// SPDX-License-Identifier: MIT
#include "xmini_group.h"
#include "xmini_rtc_limits.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"

#define GROUP_COMMAND 0x2200U
#define GROUP_INVALID 0x108
#define GROUP_TIMEOUT 0x107
#define GROUP_STATE 0x103

static struct {
    xmini_group_callbacks_t callbacks;
    xmini_group_status_t view;
    bool initialized, pending, want_connect, connecting, closing, retired, needs_sync, deferred;
    platform_operation_t operation;
    xmini_group_action_t deferred_action;
    uint32_t ticket, pending_ticket, notice_revision, read_revision;
    int64_t pending_since, http_deadline, connection_deadline, sync_due, heartbeat_due, lease_deadline, snapshot_due;
    uint32_t heartbeat_ms, lease_ms;
    char device[65], session[33], signaling_room[161], deferred_code[7], deferred_password[5];
} s;

static bool digits(const char *value, unsigned length, bool optional)
{
    if (!value || !*value) return optional;
    if (strlen(value) != length) return false;
    for (unsigned i = 0; i < length; ++i) if (value[i] < '0' || value[i] > '9') return false;
    return true;
}

static const char *string(const cJSON *object, const char *name, size_t capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring && strlen(item->valuestring) < capacity ? item->valuestring : NULL;
}

static bool integer(const cJSON *object, const char *name, uint32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) return false;
    uint32_t result = (uint32_t)item->valuedouble;
    if (item->valuedouble != result) return false;
    *value = result;
    return true;
}

static void changed(void) { ++s.view.revision; }

static cJSON *parse(const char *json, size_t length)
{
    if (!json || !length) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return NULL;
    while (end < json + length && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (end != json + length || !cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

static int request(platform_operation_t operation, cJSON *body, int64_t now)
{
    if (s.pending) { cJSON_Delete(body); return GROUP_STATE; }
    char *json = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (operation != PLATFORM_GROUP_ASSIGNMENT && !json) return ESP_ERR_NO_MEM;
    if (++s.ticket == 0) ++s.ticket;
    s.pending = true;
    s.pending_ticket = s.ticket;
    s.operation = operation;
    s.pending_since = now;
    s.http_deadline = now + 20000;
    if (operation == PLATFORM_GROUP_ASSIGNMENT) s.read_revision = s.notice_revision;
    int result = s.callbacks.request(operation, s.ticket, json, s.callbacks.context);
    cJSON_free(json);
    if (result) s.pending = false;
    return result;
}

static cJSON *identity(bool session)
{
    cJSON *body = cJSON_CreateObject();
    if (body && cJSON_AddStringToObject(body, "room_id", s.view.room_id) &&
        cJSON_AddNumberToObject(body, "assignment_version", s.view.assignment_version) &&
        (!session || cJSON_AddStringToObject(body, "session_id", s.session))) return body;
    cJSON_Delete(body);
    return NULL;
}

static int signal(const char *method, cJSON *params, bool response)
{
    cJSON *root = cJSON_CreateObject();
    bool valid = root && cJSON_AddStringToObject(root, "jsonrpc", "2.0") &&
        cJSON_AddStringToObject(root, "method", method) &&
        (!response || cJSON_AddNumberToObject(root, "id", s.view.generation));
    if (valid && params) { valid = cJSON_AddItemToObject(root, "params", params); if (valid) params = NULL; }
    char *json = valid ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(params); cJSON_Delete(root);
    int result = json ? s.callbacks.signal(s.view.generation, GROUP_COMMAND, json, s.callbacks.context) : ESP_ERR_NO_MEM;
    cJSON_free(json);
    return result;
}

static int presence(const char *state, int64_t now)
{
    cJSON *body = identity(true);
    if (!body || !cJSON_AddStringToObject(body, "state", state)) { cJSON_Delete(body); return ESP_ERR_NO_MEM; }
    return request(PLATFORM_GROUP_PRESENCE, body, now);
}

static void detach(int error, bool foreground, int64_t now)
{
    uint32_t generation = s.view.generation;
    bool connection = s.connecting || s.view.active;
    s.view.active = s.view.speaking = s.view.snapshot_ready = false;
    s.view.foreground = foreground;
    s.want_connect = false;
    s.deferred = false;
    if (s.pending && (s.operation == PLATFORM_GROUP_ASSIGNMENT || s.operation == PLATFORM_GROUP_CONNECT_TOKEN ||
                      s.operation == PLATFORM_GROUP_PRESENCE)) s.pending = false;
    if (generation && !s.closing && !s.retired) {
        if (connection) (void)signal("leave_room", NULL, false);
        /* Best-effort lease release; persistent assignment remains untouched.
         * A failed release leaves the server's unchanged lease authoritative. */
        if (connection && !s.pending) (void)presence(error ? "connect_failed" : foreground ? "suspended" : "left", now);
        s.closing = true;
        s.view.phase = XMINI_GROUP_STOPPING;
        s.callbacks.finish(generation, error, s.callbacks.context);
    } else if (!connection) {
        s.view.phase = error ? XMINI_GROUP_ERROR : s.view.assigned ? XMINI_GROUP_ASSIGNED : XMINI_GROUP_EMPTY;
    }
    s.view.error = error;
    changed();
}

static void fail(int error, int64_t now) { detach(error ? error : GROUP_INVALID, s.view.foreground, now); }

int xmini_group_init(const xmini_group_callbacks_t *callbacks)
{
    if (s.initialized || !callbacks || !callbacks->request || !callbacks->connect || !callbacks->signal ||
        !callbacks->set_profile || !callbacks->finish || !callbacks->random) return GROUP_STATE;
    s.callbacks = *callbacks;
    s.initialized = true;
    return 0;
}

int xmini_group_open(uint32_t generation, const char *device_id, int64_t now)
{
    if (!s.initialized || !generation || !device_id || !*device_id || strlen(device_id) >= sizeof(s.device) ||
        s.connecting || s.view.active || s.closing) return GROUP_STATE;
    if (strcmp(s.device, device_id)) {
        memset(&s.view, 0, sizeof(s.view));
        memcpy(s.device, device_id, strlen(device_id) + 1);
    }
    s.view.generation = generation;
    s.retired = false;
    s.view.foreground = true;
    s.view.assignment_known = false;
    s.view.phase = XMINI_GROUP_SYNCING;
    s.view.error = 0;
    s.needs_sync = true;
    s.sync_due = now;
    s.want_connect = false;
    changed();
    if (s.pending) return 0; // an already-issued mutation must finish once
    return request(PLATFORM_GROUP_ASSIGNMENT, NULL, now);
}

int xmini_group_action(xmini_group_action_t action, uint32_t generation,
                       const char *room_code, const char *password, int64_t now)
{
    if (!s.initialized || !generation || generation != s.view.generation || !s.view.foreground ||
        action < XMINI_GROUP_REFRESH || action > XMINI_GROUP_PTT_UP) return GROUP_STATE;
    if (action == XMINI_GROUP_PTT_UP || action == XMINI_GROUP_PTT_DOWN) {
        if (!s.view.active) return GROUP_STATE;
        bool speaking = action == XMINI_GROUP_PTT_DOWN;
        /* Release gates local TX before any potentially failing command. */
        if (!speaking) s.view.speaking = false;
        cJSON *params = cJSON_CreateObject();
        int result = params && cJSON_AddStringToObject(params, "mic_state", speaking ? "speaking" : "off") ?
            signal("set_mic_state", params, false) : (cJSON_Delete(params), ESP_ERR_NO_MEM);
        if (!result) {
            s.view.speaking = speaking;
            s.view.phase = speaking ? XMINI_GROUP_SPEAKING : XMINI_GROUP_LISTENING;
        } else if (speaking) s.view.speaking = false;
        s.view.error = result;
        changed();
        return result;
    }
    if (action == XMINI_GROUP_DISCONNECT) { detach(0, true, now); return 0; }
    if (action == XMINI_GROUP_REFRESH) { xmini_group_assignment_changed(now); return 0; }
    if ((action == XMINI_GROUP_CREATE || action == XMINI_GROUP_JOIN) &&
        (!digits(password, 4, true) || (action == XMINI_GROUP_JOIN && !digits(room_code, 6, false)))) return ESP_ERR_INVALID_ARG;
    if (!s.view.assignment_known) {
        if (s.deferred || (s.pending && s.operation != PLATFORM_GROUP_ASSIGNMENT)) return GROUP_STATE;
        s.deferred = true;
        s.deferred_action = action;
        snprintf(s.deferred_code, sizeof(s.deferred_code), "%s", room_code ? room_code : "");
        snprintf(s.deferred_password, sizeof(s.deferred_password), "%s", password ? password : "");
        return 0;
    }
    if (action == XMINI_GROUP_CONNECT) {
        if (!s.view.assigned || s.closing || s.connecting || s.view.active) return GROUP_STATE;
        s.want_connect = true;
        return 0;
    }
    if (action == XMINI_GROUP_LEAVE) {
        if (!s.view.assigned) return 0;
        if (s.connecting || s.view.active) detach(0, true, now);
        if (s.pending && s.operation == PLATFORM_GROUP_PRESENCE) s.pending = false;
        if (s.pending) return GROUP_STATE;
        return request(PLATFORM_GROUP_LEAVE, identity(false), now);
    }
    if (s.view.assigned) return 40923;
    if (s.pending || s.closing) return GROUP_STATE;
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "password", password ? password : "") ||
        (action == XMINI_GROUP_JOIN && !cJSON_AddStringToObject(body, "room_code", room_code))) {
        cJSON_Delete(body); return ESP_ERR_NO_MEM;
    }
    s.want_connect = true;
    return request(action == XMINI_GROUP_CREATE ? PLATFORM_GROUP_CREATE : PLATFORM_GROUP_JOIN, body, now);
}

static bool assignment(const cJSON *data, int64_t now)
{
    uint32_t version;
    const char *device = string(data, "device_id", 65);
    const char *desired = string(data, "desired_state", 16);
    const char *room = string(data, "room_id", 65);
    const char *code = string(data, "room_code", 7);
    if (!device || strcmp(device, s.device) || !desired || !integer(data, "assignment_version", &version)) return false;
    bool assigned = strcmp(desired, "joined") == 0;
    if ((!assigned && *desired && strcmp(desired, "left")) ||
        (assigned && (!room || !*room || !digits(code, 6, false) || !version))) return false;
    if (version < s.view.assignment_version) { s.needs_sync = true; s.sync_due = now + 5000; ++s.view.stale_events; return true; }
    bool different = assigned != s.view.assigned || version != s.view.assignment_version ||
        (assigned && strcmp(room, s.view.room_id));
    if (different && (s.connecting || s.view.active)) detach(0, s.view.foreground, now);
    s.view.assigned = assigned;
    s.view.assignment_known = true;
    s.view.assignment_version = version;
    snprintf(s.view.room_id, sizeof(s.view.room_id), "%s", assigned ? room : "");
    snprintf(s.view.room_code, sizeof(s.view.room_code), "%s", assigned ? code : "");
    if (!s.connecting && !s.view.active && !s.closing) s.view.phase = assigned ? XMINI_GROUP_ASSIGNED : XMINI_GROUP_EMPTY;
    s.view.error = 0;
    s.needs_sync = false;
    s.sync_due = now + 30000;
    changed();
    return true;
}

void xmini_group_on_http(platform_operation_t operation, uint32_t ticket,
                          int transport_error, const char *json, int64_t now)
{
    if (!s.pending || ticket != s.pending_ticket || operation != s.operation) { ++s.view.stale_events; return; }
    s.pending = false;
    cJSON *root = json ? parse(json, strlen(json)) : NULL;
    uint32_t code = 0;
    bool code_valid = integer(root, "code", &code);
    /* The shared HTTP layer also returns ESP_FAIL for a non-200 business
     * envelope. Preserve its precise numeric meaning, e.g. password 40320. */
    int error = code_valid && code != 200 ? code > 0 && code <= INT32_MAX ? (int)code : GROUP_INVALID :
        transport_error ? transport_error : code_valid ? 0 : GROUP_INVALID;
    if (error) {
        cJSON_Delete(root);
        s.view.error = error;
        if (s.view.active && (operation == PLATFORM_GROUP_ASSIGNMENT || operation == PLATFORM_GROUP_PRESENCE) &&
            error != 401 && error != 40300 && error != 40400 && error != 40921) {
            s.heartbeat_due = now + 2000; s.sync_due = now + 5000; changed(); return;
        }
        fail(error, now); return;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (operation == PLATFORM_GROUP_PRESENCE) {
        if (s.view.active) {
            s.lease_deadline = s.pending_since + s.lease_ms;
            s.heartbeat_due = now + s.heartbeat_ms;
        }
    } else if (operation != PLATFORM_GROUP_CONNECT_TOKEN) {
        if (!assignment(data, now)) { cJSON_Delete(root); fail(GROUP_INVALID, now); return; }
        if (operation == PLATFORM_GROUP_ASSIGNMENT && s.read_revision != s.notice_revision) {
            s.view.assignment_known = false; s.needs_sync = true; s.sync_due = now;
        }
    } else {
        uint32_t heartbeat, lease;
        const char *peer = string(data, "peer_id", XMINI_RTC_PEER_CAPACITY);
        const char *token = string(data, "token", XMINI_RTC_TOKEN_CAPACITY);
        if (!peer || !*peer || !token || !*token || !integer(data, "heartbeat_seconds", &heartbeat) ||
            !integer(data, "lease_seconds", &lease) || heartbeat < 1 || heartbeat > 300 || lease <= heartbeat * 2 || lease > 3600 ||
            !s.view.foreground || !s.want_connect || s.closing) {
            const cJSON *peer_item = cJSON_GetObjectItemCaseSensitive(data, "peer_id");
            const cJSON *token_item = cJSON_GetObjectItemCaseSensitive(data, "token");
            uint32_t hb_value = 0, lease_value = 0;
            bool hb_valid = integer(data, "heartbeat_seconds", &hb_value) && hb_value >= 1 && hb_value <= 300;
            bool lease_valid = integer(data, "lease_seconds", &lease_value) && hb_valid &&
                lease_value > hb_value * 2 && lease_value <= 3600;
            ESP_LOGW("xmini_group", "connect-token rejected peer_type=%d peer_bytes=%zu token_type=%d token_bytes=%zu hb=%lu hb_valid=%d lease=%lu lease_valid=%d foreground=%d want_connect=%d closing=%d",
                peer_item ? peer_item->type & 0xff : 0,
                cJSON_IsString(peer_item) && peer_item->valuestring ? strlen(peer_item->valuestring) : 0,
                token_item ? token_item->type & 0xff : 0,
                cJSON_IsString(token_item) && token_item->valuestring ? strlen(token_item->valuestring) : 0,
                (unsigned long)hb_value, hb_valid, (unsigned long)lease_value, lease_valid,
                s.view.foreground, s.want_connect, s.closing);
            cJSON_Delete(root); fail(GROUP_INVALID, now); return;
        }
        s.heartbeat_ms = heartbeat * 1000;
        s.lease_ms = lease * 1000;
        s.lease_deadline = s.pending_since + s.lease_ms;
        if (now >= s.lease_deadline) { cJSON_Delete(root); fail(GROUP_TIMEOUT, now); return; }
        s.connecting = true;
        s.connection_deadline = now + 20000;
        s.view.phase = XMINI_GROUP_CONNECTING;
        int result = s.callbacks.connect(s.view.generation, peer, token, s.callbacks.context);
        cJSON_Delete(root);
        if (result) fail(result, now);
        changed(); return;
    }
    cJSON_Delete(root);
    if (s.deferred && s.view.assignment_known && s.view.foreground) {
        s.deferred = false;
        int result = xmini_group_action(s.deferred_action, s.view.generation, s.deferred_code, s.deferred_password, now);
        memset(s.deferred_password, 0, sizeof(s.deferred_password));
        if (result) { s.view.error = result; changed(); }
    }
}

static bool add_audio(cJSON *params, const char *name)
{
    cJSON *format = cJSON_AddObjectToObject(params, name);
    return format && cJSON_AddStringToObject(format, "codec", "g711a") &&
        cJSON_AddNumberToObject(format, "sample_rate", 8000) && cJSON_AddNumberToObject(format, "channels", 1);
}

void xmini_group_on_connected(uint32_t generation, int error, int64_t now)
{
    if (!generation || generation != s.view.generation || !s.connecting || s.closing) { ++s.view.stale_events; return; }
    if (error) { fail(error, now); return; }
    cJSON *params = cJSON_CreateObject();
    if (!params || !cJSON_AddStringToObject(params, "room_id", s.view.room_id) ||
        !cJSON_AddStringToObject(params, "device_id", s.device) || !add_audio(params, "input_audio") || !add_audio(params, "output_audio")) {
        cJSON_Delete(params); fail(ESP_ERR_NO_MEM, now); return;
    }
    int result = signal("join_room", params, true);
    if (result) { fail(result, now); return; }
    s.view.phase = XMINI_GROUP_JOINING;
    s.connection_deadline = now + 9000;
    changed();
}

static bool audio(const cJSON *object)
{
    uint32_t rate, channels;
    const char *codec = string(object, "codec", 16);
    return codec && !strcmp(codec, "g711a") && integer(object, "sample_rate", &rate) && rate == 8000 &&
        integer(object, "channels", &channels) && channels == 1;
}

static bool pin_room(const char *room)
{
    if (!room || !*room) return false;
    if (s.signaling_room[0]) return !strcmp(s.signaling_room, room);
    size_t length = strlen(room), business = strlen(s.view.room_id);
    if (strcmp(room, s.view.room_id) && !(length > business + 1 && room[length - business - 1] == ':' &&
        !strcmp(room + length - business, s.view.room_id))) return false;
    strcpy(s.signaling_room, room);
    return true;
}

void xmini_group_on_signal(uint32_t generation, const char *json, size_t length, int64_t now)
{
    if (!generation || generation != s.view.generation || !s.connecting || s.closing) { ++s.view.stale_events; return; }
    cJSON *root = parse(json, length);
    const char *rpc = string(root, "jsonrpc", 8);
    if (!root || !rpc || strcmp(rpc, "2.0")) { cJSON_Delete(root); fail(GROUP_INVALID, now); return; }
    uint32_t id;
    if (integer(root, "id", &id)) {
        if (id != generation || s.view.phase != XMINI_GROUP_JOINING) { ++s.view.stale_events; cJSON_Delete(root); return; }
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        const char *session = string(result, "session_id", 65);
        if (!session || !*session || cJSON_HasObjectItem(root, "error") ||
            !audio(cJSON_GetObjectItemCaseSensitive(result, "input_audio")) ||
            !audio(cJSON_GetObjectItemCaseSensitive(result, "output_audio"))) {
            cJSON_Delete(root); fail(GROUP_INVALID, now); return;
        }
        if (now >= s.lease_deadline) { cJSON_Delete(root); fail(GROUP_TIMEOUT, now); return; }
        int result_code = s.callbacks.set_profile(generation, 1, 1, s.callbacks.context);
        if (result_code) { cJSON_Delete(root); fail(result_code, now); return; }
        s.view.active = true;
        s.view.speaking = false;
        s.view.phase = XMINI_GROUP_LISTENING;
        s.heartbeat_due = now;
        changed();
        cJSON_Delete(root); return;
    }
    const char *method = string(root, "method", 65);
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const char *room = string(params, "room_id", sizeof(s.signaling_room));
    if (!method || !room) { cJSON_Delete(root); return; }
    if (!strcmp(method, "room_snapshot")) {
        const cJSON *self = cJSON_GetObjectItemCaseSensitive(params, "self");
        const char *device = string(self, "device_id", 65), *self_id = string(self, "participant_id", 97);
        const cJSON *members = cJSON_GetObjectItemCaseSensitive(params, "participants");
        if (!device || strcmp(device, s.device) || !self_id || !*self_id || !cJSON_IsArray(members) || cJSON_GetArraySize(members) > 100 || !pin_room(room)) {
            cJSON_Delete(root); return;
        }
        unsigned count = 0;
        bool found_self = false, valid = true;
        const cJSON *item;
        cJSON_ArrayForEach(item, members) {
            const char *participant = string(item, "participant_id", 97), *member_device = string(item, "device_id", 65);
            if (!participant || !*participant || !member_device || !*member_device) { valid = false; break; }
            for (const cJSON *previous = members->child; previous != item; previous = previous->next) {
                const char *previous_id = string(previous, "participant_id", 97);
                if (previous_id && !strcmp(previous_id, participant)) { valid = false; break; }
            }
            if (!valid) break;
            if (!strcmp(participant, self_id)) { if (strcmp(member_device, s.device)) { valid = false; break; } found_self = true; }
            ++count;
        }
        if (!found_self) ++count;
        if (valid && count <= 100) {
            s.view.participant_count = count; s.view.snapshot_ready = true; s.snapshot_due = 0; changed();
        } else { cJSON_Delete(root); fail(GROUP_INVALID, now); return; }
    } else if (s.signaling_room[0] && pin_room(room)) {
        if (!strcmp(method, "room_closed")) { cJSON_Delete(root); fail(40400, now); xmini_group_assignment_changed(now); return; }
        if (!strcmp(method, "participant_joined") || !strcmp(method, "participant_left") || !strcmp(method, "participant_mic_state_changed")) {
            /* Refresh the authoritative count instead of retaining a large
             * 100-member table in C3 RAM or guessing counts from duplicates. */
            s.view.snapshot_ready = false;
            if (!s.snapshot_due) s.snapshot_due = now + 200;
            changed();
        }
    } else if (!s.signaling_room[0]) {
        if (!s.snapshot_due) s.snapshot_due = now + 200;
    }
    cJSON_Delete(root);
}

void xmini_group_on_closed(uint32_t generation, int error, int64_t now)
{
    if (!generation || generation != s.view.generation) { ++s.view.stale_events; return; }
    s.connecting = s.closing = s.view.active = s.view.speaking = false;
    s.retired = true;
    s.want_connect = false;
    s.view.snapshot_ready = false;
    s.signaling_room[0] = s.session[0] = 0;
    s.view.error = error ? error : s.view.error;
    s.view.phase = s.view.error ? XMINI_GROUP_ERROR : s.view.assigned ? XMINI_GROUP_ASSIGNED : XMINI_GROUP_EMPTY;
    s.needs_sync = true; s.sync_due = now; changed();
}

void xmini_group_suspend(uint32_t generation, int error, int64_t now)
{
    if (generation && generation == s.view.generation) detach(error, false, now);
}

void xmini_group_assignment_changed(int64_t now)
{
    ++s.notice_revision;
    s.needs_sync = true;
    s.sync_due = now;
}

void xmini_group_tick(int64_t now)
{
    if (!s.initialized) return;
    if (s.pending && now >= s.http_deadline) { s.pending = false; fail(GROUP_TIMEOUT, now); }
    if (s.connecting && !s.closing && ((!s.view.active && now >= s.connection_deadline) || now >= s.lease_deadline)) {
        fail(GROUP_TIMEOUT, now); return;
    }
    if (!s.view.foreground || s.closing) return;
    if (s.snapshot_due && now >= s.snapshot_due && s.view.active) {
        int result = signal("get_room_snapshot", NULL, false);
        s.snapshot_due = result ? now + 2000 : 0;
        if (result) { s.view.error = result; changed(); }
    }
    if (s.pending) return;
    if (s.view.active && now >= s.heartbeat_due) {
        int result = presence("joined", now);
        if (result) { s.view.error = result; s.heartbeat_due = now + 2000; changed(); }
    } else if (now >= s.sync_due) {
        int result = request(PLATFORM_GROUP_ASSIGNMENT, NULL, now);
        s.sync_due = now + 30000;
        if (result) { s.view.error = result; changed(); }
    } else if (s.want_connect && s.view.assigned && s.view.assignment_known && !s.needs_sync && !s.connecting) {
        snprintf(s.session, sizeof(s.session), "%08lx%08lx%08lx%08lx", (unsigned long)s.callbacks.random(s.callbacks.context),
            (unsigned long)s.callbacks.random(s.callbacks.context), (unsigned long)s.view.generation, (unsigned long)s.ticket);
        s.signaling_room[0] = 0; s.view.snapshot_ready = false;
        s.view.phase = XMINI_GROUP_TOKEN;
        int result = request(PLATFORM_GROUP_CONNECT_TOKEN, identity(true), now);
        if (result) fail(result, now);
        changed();
    }
}

xmini_group_status_t xmini_group_status(void) { return s.view; }
