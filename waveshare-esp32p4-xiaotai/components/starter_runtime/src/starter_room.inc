/* Included by the single runtime owner, not a second session task.
 * Contract: thing-connect/device-room.md and Room JSON-RPC 0x2200.
 * Assignment, transport connection and successful join_room are distinct.
 * Only the last one admits media; every reconnection starts with PTT closed. */
#define JSMN_STATIC
#define JSMN_STRICT
#include "vendor/jsmn.h"
#undef JSMN_STATIC
#undef JSMN_STRICT

typedef enum { ROOM_GET = 1, ROOM_CREATE, ROOM_JOIN, ROOM_LEAVE,
               ROOM_TOKEN, ROOM_PRESENCE, ROOM_REFRESH, ROOM_RELEASE } room_op_t;
static EXT_RAM_BSS_ATTR struct {
    starter_room_snapshot_t view;
    starter_room_member_t members[STARTER_ROOM_MEMBERS_MAX];
    char id[129], scoped_id[257], session[33];
    uint32_t version, epoch, ticket, pending_ticket, change_revision, pending_revision;
    uint32_t app_request;
    room_op_t pending;
    int64_t pending_at, poll_at, retry_at, heartbeat_at, lease_until, snapshot_at, snapshot_allowed_at, members_valid_until;
    uint32_t heartbeat_ms, lease_ms;
    unsigned failures;
    bool dirty, blocked, leave_requested, mic_announced;
    /* Keep the old ownership tuple until the server acknowledges its release.
     * Wi-Fi loss must not discard it when the next connection gets a new ID. */
    struct { char id[129], session[33]; bool failed; uint32_t version; } release;
} s_room;
static EXT_RAM_BSS_ATTR struct {
    starter_room_snapshot_t view;
    starter_room_member_t members[STARTER_ROOM_MEMBERS_MAX];
} s_room_public;
static atomic_uint s_room_ptt_touch_ms;
static atomic_uint s_room_ptt_admitted;
static atomic_uint s_room_ptt_app_request;
static atomic_uint s_room_http_lost;
static atomic_uint s_room_lost_session;
/* Bit 0 is foreground ownership, the other bits record navigation changes.
 * A close/open pair between two runtime ticks must still release the old call. */
static atomic_uint s_room_app_request;

static bool room_app_open(void) { return (atomic_load(&s_room_app_request) & 1U) != 0; }
static bool room_app_current(void)
{
    unsigned request = atomic_load(&s_room_app_request);
    return (request & 1U) && request == s_room.app_request;
}
void starter_runtime_room_set_open(bool open)
{
    unsigned previous = atomic_load(&s_room_app_request);
    do {
        if (((previous & 1U) != 0) == open) return;
    } while (!atomic_compare_exchange_weak(&s_room_app_request, &previous,
                  ((previous + 2U) & ~1U) | (open ? 1U : 0U)));
    if (!open) {
        unsigned generation = atomic_exchange(&s_room_ptt_admitted, 0);
        if (generation) starter_media_room_ptt(generation, false);
    }
}

static bool room_owns_media(void)
{
    int state = atomic_load(&s_public_state);
    return state == STARTER_RUNTIME_ROOM_CONNECTING || state == STARTER_RUNTIME_ROOM_ACTIVE;
}

static void room_transport_lost(uint32_t generation, uint32_t request_tag)
{
    /* A late ROOM callback must never tear down a replacement AI/CALL session. */
    unsigned session = atomic_load(&s_public_session_generation);
    if (room_owns_media() && session &&
        ((request_tag && request_tag == session) ||
         (generation && generation == atomic_load(&s_public_connection_generation))))
        atomic_store(&s_room_lost_session, session);
}

static void room_publish(void)
{
    s_room.view.revision++;
    s_room.view.app_open = room_app_current();
    s_room.view.connection_generation = s_room.view.state == STARTER_ROOM_LISTENING ? s_connection_generation : 0;
    s_room.view.busy = s_room.pending != 0 || (s_room.leave_requested && !s_room.blocked);
    s_room.view.transmitting = room_owns_media() &&
        starter_tirtc_room_transmitting(s_connection_generation);
    xSemaphoreTake(s_product_mutex, portMAX_DELAY);
    s_room_public.view = s_room.view;
    memcpy(s_room_public.members, s_room.members,
           s_room.view.member_count * sizeof(s_room.members[0]));
    xSemaphoreGive(s_product_mutex);
}

static void room_status(starter_room_state_t state, const char *message)
{
    s_room.view.state = state;
    snprintf(s_room.view.message, sizeof(s_room.view.message), "%s", message);
    room_publish();
}

bool starter_runtime_room_read(unsigned offset, starter_room_snapshot_t *out)
{
    if (!out || !s_product_mutex || xSemaphoreTake(s_product_mutex, 0) != pdTRUE) return false;
    *out = s_room_public.view;
    memset(out->members, 0, sizeof(out->members));
    out->page_count = 0;
    for (unsigned i = offset; i < out->member_count && out->page_count < STARTER_ROOM_PAGE_SIZE; ++i)
        out->members[out->page_count++] = s_room_public.members[i];
    xSemaphoreGive(s_product_mutex);
    return true;
}

bool starter_runtime_room_ptt(uint32_t generation, bool pressed)
{
    bool allowed = pressed && room_app_open() && generation != 0 &&
        atomic_load(&s_room_app_request) == atomic_load(&s_room_ptt_app_request) &&
        atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_ACTIVE &&
        atomic_load(&s_public_connection_generation) == generation &&
        atomic_load(&s_room_ptt_admitted) == generation &&
        !starter_media_status().microphone_muted;
    if (allowed) atomic_store(&s_room_ptt_touch_ms, (uint32_t)now_ms());
    if (starter_tirtc_room_transmitting(generation) != allowed)
        starter_media_room_ptt(generation, allowed);
    return allowed;
}

static bool room_digits(const char *text, size_t length, bool optional)
{
    if (!text || !text[0]) return optional;
    if (strlen(text) != length) return false;
    for (size_t i = 0; i < length; ++i) if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

static esp_err_t room_intent(room_op_t op, const char *code, const char *password)
{
    if (!room_app_open()) return ESP_ERR_INVALID_STATE;
    if ((op == ROOM_JOIN && !room_digits(code, 6, false)) ||
        !room_digits(password, 4, true)) return ESP_ERR_INVALID_ARG;
    runtime_event_t event = {.type = EVENT_ROOM_INTENT, .command = op,
                             .request_tag = atomic_load(&s_room_app_request),
                             .platform_epoch = platform_client_epoch()};
    char body[64];
    int n = snprintf(body, sizeof(body), "{\"room_code\":\"%s\",\"password\":\"%s\"}",
                     code ? code : "", password ? password : "");
    if (n < 0 || (size_t)n >= sizeof(body) || !copy_event_text(&event, body, n))
        return ESP_ERR_NO_MEM;
    if (queue_event(&event)) return ESP_OK;
    release_event(&event);
    return ESP_ERR_TIMEOUT;
}
esp_err_t starter_runtime_room_create(const char *password) { return room_intent(ROOM_CREATE, NULL, password); }
esp_err_t starter_runtime_room_join(const char *code, const char *password) { return room_intent(ROOM_JOIN, code, password); }
esp_err_t starter_runtime_room_leave(void)
{
    (void)starter_runtime_room_ptt(atomic_load(&s_public_connection_generation), false);
    return room_intent(ROOM_LEAVE, NULL, NULL);
}
esp_err_t starter_runtime_room_refresh(void) { return room_intent(ROOM_REFRESH, NULL, NULL); }

static void room_http_response(const char *body, void *context)
{
    if (!queue_http_result(EVENT_ROOM_HTTP, (uint32_t)(uintptr_t)context, 0, body))
        atomic_store(&s_room_http_lost, (uint32_t)(uintptr_t)context);
}

static esp_err_t room_http(room_op_t op, const cJSON *body)
{
    static const char *const paths[] = {NULL, "assignment", "create", "join", "leave", "connect-token", "presence", NULL, "presence"};
    if (s_room.pending || op < ROOM_GET || op > ROOM_RELEASE || op == ROOM_REFRESH)
        return ESP_ERR_INVALID_STATE;
    char path[64], json[512];
    snprintf(path, sizeof(path), "/v1/call/group/device/%s", paths[op]);
    if (body && !cJSON_PrintPreallocated((cJSON *)body, json, sizeof(json), false)) return ESP_ERR_NO_MEM;
    uint32_t ticket = ++s_room.ticket;
    if (!ticket) ticket = ++s_room.ticket;
    esp_err_t err = platform_client_request_timeout(PLATFORM_SERVICE_CALL, path,
        body ? json : NULL, 6000, room_http_response, (void *)(uintptr_t)ticket);
    if (err == ESP_OK) {
        s_room.pending = op;
        s_room.pending_ticket = ticket;
        s_room.pending_at = now_ms();
        s_room.pending_revision = s_room.change_revision;
        room_publish();
    }
    return err;
}

static cJSON *room_relation_body(bool session)
{
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "room_id", s_room.id) ||
        !cJSON_AddNumberToObject(body, "assignment_version", s_room.version) ||
        (session && !cJSON_AddStringToObject(body, "session_id", s_room.session))) {
        cJSON_Delete(body);
        return NULL;
    }
    return body;
}

static esp_err_t room_presence(const char *state, bool tracked)
{
    if (!s_room.id[0] || !s_room.session[0] || !s_room.version) return ESP_ERR_INVALID_STATE;
    cJSON *body = room_relation_body(true);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (body && cJSON_AddStringToObject(body, "state", state)) {
        if (tracked) err = room_http(ROOM_PRESENCE, body);
        else {
            char json[512];
            if (cJSON_PrintPreallocated(body, json, sizeof(json), false))
                err = platform_client_request_timeout(PLATFORM_SERVICE_CALL,
                    "/v1/call/group/device/presence", json, 6000, NULL, NULL);
        }
    }
    cJSON_Delete(body);
    return err;
}

static bool room_send(const char *json)
{
    return starter_tirtc_send_command(0x2200U, json, (uint32_t)strlen(json)) >= 0;
}

static void room_remember_release(bool failed)
{
    if (!s_room.id[0] || !s_room.session[0] || !s_room.version ||
        s_room.epoch != platform_client_epoch()) return;
    snprintf(s_room.release.id, sizeof(s_room.release.id), "%s", s_room.id);
    snprintf(s_room.release.session, sizeof(s_room.release.session), "%s", s_room.session);
    s_room.release.version = s_room.version;
    s_room.release.failed |= failed;
}

static esp_err_t room_release_request(void)
{
    cJSON *body = cJSON_CreateObject();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (body && cJSON_AddStringToObject(body, "room_id", s_room.release.id) &&
        cJSON_AddNumberToObject(body, "assignment_version", s_room.release.version) &&
        cJSON_AddStringToObject(body, "session_id", s_room.release.session) &&
        cJSON_AddStringToObject(body, "state", s_room.release.failed ? "connect_failed" : "suspended"))
        err = room_http(ROOM_RELEASE, body);
    cJSON_Delete(body);
    return err;
}

static bool room_complete_release(double code)
{
    if (code != 200 && code != 40921 && code != 40400) return false;
    /* A stale tuple / closed room no longer owns this connection. It is not
     * a successful release and must never be retried with a newer tuple. */
    ESP_LOGI(TAG, "ROOM release %s code=%d", code == 200 ? "acknowledged" : "obsolete", (int)code);
    memset(&s_room.release, 0, sizeof(s_room.release));
    s_room.dirty = true;
    s_room.poll_at = now_ms();
    return true;
}

static void room_before_finish(int error)
{
    if (s_room.view.assigned) { s_room.dirty = true; s_room.poll_at = now_ms(); }
    if (!room_owns_media()) return;
    atomic_store(&s_room_ptt_admitted, 0);
    starter_media_room_ptt(s_connection_generation, false);
    if (s_connection_generation && starter_tirtc_connected())
        (void)room_send("{\"jsonrpc\":\"2.0\",\"method\":\"leave_room\"}");
    room_remember_release(error != 0);
    s_room.view.member_count = 0;
    s_room.view.members_ready = false;
    s_room.scoped_id[0] = '\0';
    s_room.mic_announced = false;
    s_room.dirty = true;
    s_room.poll_at = now_ms();
    room_status(STARTER_ROOM_SUSPENDED, "对讲已暂停");
}

static void room_fail(int code, const char *message, bool retry)
{
    if (room_owns_media()) finish_session(code);
    ++s_room.failures;
    s_room.blocked = !retry || s_room.failures >= 5;
    s_room.retry_at = now_ms() + (s_room.failures < 4 ? (2000U << s_room.failures) : 30000U);
    s_room.dirty = true;
    ESP_LOGW(TAG, "ROOM failure code=%d attempt=%u retry=%d", code, s_room.failures, !s_room.blocked);
    room_status(s_room.blocked ? STARTER_ROOM_ERROR : STARTER_ROOM_RETRY, message);
}

static void room_reset(void)
{
    s_room.view.assigned = false;
    if (room_owns_media()) finish_session(ESP_ERR_INVALID_STATE);
    uint32_t ticket = s_room.ticket + 1, revision = s_room.view.revision;
    memset(&s_room, 0, sizeof(s_room));
    s_room.app_request = atomic_load(&s_room_app_request);
    s_room.ticket = ticket;
    s_room.view.revision = revision;
    s_room.epoch = platform_client_epoch();
    s_room.dirty = true;
    room_status(STARTER_ROOM_EMPTY, "尚未加入房间");
}

static void room_apply_navigation(void)
{
    unsigned request = atomic_load(&s_room_app_request);
    if (request == s_room.app_request) return;
    /* Only this app's session is ours to close. AI, CALL, H5 and permanent
     * MQTT remain owned by their existing runtime paths. Keep in-flight HTTP
     * tickets so a late token can release its reservation instead of leaking it. */
    if (room_owns_media()) finish_session(0);
    s_room.app_request = request;
    ++s_room.change_revision;
    s_room.view.member_count = s_room.view.online_count = 0;
    s_room.view.members_ready = false;
    s_room.view.assignment_known = false;
    s_room.dirty = true;
    if (request & 1U) {
        s_room.blocked = false;
        s_room.failures = 0;
        s_room.retry_at = s_room.poll_at = 0;
    } else if (s_room.release.id[0]) {
        /* Explicit close also owns cleanup after a failed join. A previous
         * admission failure must not suppress the bounded release transaction. */
        s_room.blocked = false;
        s_room.failures = 0;
        s_room.retry_at = 0;
    }
    room_status((request & 1U) ? STARTER_ROOM_SYNCING :
        s_room.view.assigned ? STARTER_ROOM_SUSPENDED : STARTER_ROOM_EMPTY,
        (request & 1U) ? "正在同步房间" : s_room.view.assigned ? "对讲已离线" : "尚未加入房间");
    ESP_LOGI(TAG, "ROOM app=%s revision=%lu", request & 1U ? "open" : "closed", (unsigned long)request);
}

static bool room_audio_format(const cJSON *profile)
{
    const cJSON *codec = cJSON_GetObjectItemCaseSensitive(profile, "codec");
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(profile, "sample_rate");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(profile, "channels");
    return cJSON_IsString(codec) && strcmp(codec->valuestring, "g711a") == 0 &&
        cJSON_IsNumber(rate) && rate->valuedouble == 8000 &&
        cJSON_IsNumber(channels) && channels->valuedouble == 1;
}

static void room_connection(const runtime_event_t *event)
{
    if (!room_owns_media()) return;
    if (event->flag && (!event->generation ||
        atomic_load(&s_public_state) != STARTER_RUNTIME_ROOM_CONNECTING)) return;
    bool current = event->generation ?
        (event->flag ? event->request_tag == s_session_generation && !s_connection_generation :
                       event->generation == s_connection_generation) :
        event->request_tag == s_session_generation && !s_connection_generation;
    if (!current) return;
    if (!room_app_current()) { finish_session(0); return; }
    restore_external_connect_reserve();
    if (!event->flag || !platform_client_ready()) {
        room_fail(event->error ? event->error : ESP_FAIL, "连接中断，等待重连", true);
        return;
    }
    s_connection_generation = event->generation;
    publish_state(STARTER_RUNTIME_ROOM_CONNECTING);
    /* IDs come from authenticated responses and must be JSON-escaped. */
    cJSON *request = cJSON_CreateObject();
    cJSON *params = request ? cJSON_AddObjectToObject(request, "params") : NULL;
    bool ok = request && params && cJSON_AddStringToObject(request, "jsonrpc", "2.0") &&
        cJSON_AddNumberToObject(request, "id", 1) && cJSON_AddStringToObject(request, "method", "join_room") &&
        cJSON_AddStringToObject(params, "room_id", s_room.id) &&
        cJSON_AddStringToObject(params, "device_id", s_device_id);
    const char *names[] = {"input_audio", "output_audio"};
    for (unsigned i = 0; ok && i < 2; ++i) {
        cJSON *audio = cJSON_AddObjectToObject(params, names[i]);
        ok = audio && cJSON_AddStringToObject(audio, "codec", "g711a") &&
            cJSON_AddNumberToObject(audio, "sample_rate", 8000) && cJSON_AddNumberToObject(audio, "channels", 1);
    }
    char json[768];
    ok = ok && cJSON_PrintPreallocated(request, json, sizeof(json), false) && room_send(json);
    cJSON_Delete(request);
    if (!ok) { room_fail(ESP_FAIL, "入房请求发送失败", true); return; }
    s_deadline_ms = now_ms() + 9000;
    room_status(STARTER_ROOM_CONNECTING, "正在加入音频房间");
}

static bool room_scope(const char *value)
{
    if (!value || !s_room.id[0]) return false;
    if (s_room.scoped_id[0]) return strcmp(value, s_room.scoped_id) == 0;
    size_t n = strlen(value), raw = strlen(s_room.id);
    bool match = strcmp(value, s_room.id) == 0 ||
        (n > raw && value[n - raw - 1] == ':' && strcmp(value + n - raw, s_room.id) == 0);
    if (match && n < sizeof(s_room.scoped_id)) {
        memcpy(s_room.scoped_id, value, n + 1);
        return true;
    }
    return false;
}

static uint8_t room_mic(const cJSON *object)
{
    const cJSON *mic = cJSON_GetObjectItemCaseSensitive(object, "mic_state");
    if (!cJSON_IsString(mic)) return UINT8_MAX;
    if (!strcmp(mic->valuestring, "speaking")) return 2;
    return !strcmp(mic->valuestring, "on") ? 1 : !strcmp(mic->valuestring, "off") ? 0 : UINT8_MAX;
}

static void room_self_name(const cJSON *object, char name[65])
{
    /* Optional server extension. Empty clears a previous name; missing fields
     * keep a name from assignment/self. Never invent a product-name fallback. */
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "device_name");
    if (cJSON_IsString(value) && value->valuestring && strlen(value->valuestring) < 65)
        snprintf(name, 65, "%s", value->valuestring);
}

static bool room_member(const cJSON *object, starter_room_member_t *member)
{
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(object, "state");
    memset(member, 0, sizeof(*member));
    if (!cJSON_IsString(state) || strcmp(state->valuestring, "active") ||
        !copy_json_string(object, "participant_id", member->participant_id, sizeof(member->participant_id), true) ||
        !copy_json_string(object, "device_id", member->device_id, sizeof(member->device_id), true)) return false;
    member->self = !strcmp(member->device_id, s_device_id);
    member->mic = room_mic(object);
    if (member->mic > 2) return false;
    if (member->self) member->mic = starter_tirtc_room_transmitting(s_connection_generation) ? 2 : 0;
    snprintf(member->name, sizeof(member->name), "%s", member->device_id);
    for (unsigned i = 0; !member->self && i < s_product_snapshot.contact_count; ++i) {
        const starter_product_contact_t *c = &s_product_snapshot.contacts[i];
        if (c->source == STARTER_CONTACT_DEVICE && !strcmp(c->id, member->device_id) && c->name[0]) {
            snprintf(member->name, sizeof(member->name), "%s", c->name);
            break;
        }
    }
    return true;
}

/* The 100-member JSON must not allocate hundreds of cJSON nodes in internal
 * RAM. jsmn indexes the original PSRAM payload; decode only one small member
 * at a time with the project's existing JSON parser (including escaped IDs). */
static int room_json_next(const jsmntok_t *tokens, int count, int index)
{
    int end = tokens[index].end;
    do { ++index; } while (index < count && tokens[index].start < end);
    return index;
}
static int room_json_key(const char *text, const jsmntok_t *tokens, int count, int object, const char *key)
{
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT) return -1;
    for (int i = object + 1; i < count && tokens[i].start < tokens[object].end;) {
        if (tokens[i].type != JSMN_STRING || i + 1 >= count) return -1;
        if ((size_t)(tokens[i].end - tokens[i].start) == strlen(key) &&
            !memcmp(text + tokens[i].start, key, strlen(key))) return i + 1;
        i = room_json_next(tokens, count, i + 1);
    }
    return -1;
}
static cJSON *room_json_value(const char *text, const jsmntok_t *tokens, int count, int index)
{
    if (index < 0) return NULL;
    if (room_json_next(tokens, count, index) - index > 32) return NULL;
    int start = tokens[index].start, end = tokens[index].end;
    if (tokens[index].type == JSMN_STRING) { --start; ++end; }
    if (end - start > 2048) return NULL;
    return cJSON_ParseWithLength(text + start, (size_t)(end - start));
}

static bool room_snapshot(const runtime_event_t *event)
{
    jsmntok_t *tokens = heap_caps_malloc(2048U * sizeof(*tokens), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    starter_room_member_t *members = heap_caps_calloc(STARTER_ROOM_MEMBERS_MAX, sizeof(*members), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    if (!tokens || !members) goto done;
    jsmn_parser parser;
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, event->text, event->length, tokens, 2048);
    if (count < 1 || tokens[0].type != JSMN_OBJECT || room_json_next(tokens, count, 0) != count) goto done;
    int method = room_json_key(event->text, tokens, count, 0, "method");
    cJSON *value = room_json_value(event->text, tokens, count, method);
    bool snapshot = cJSON_IsString(value) && !strcmp(value->valuestring, "room_snapshot");
    cJSON_Delete(value);
    if (!snapshot) goto done;
    int params = room_json_key(event->text, tokens, count, 0, "params");
    int room = room_json_key(event->text, tokens, count, params, "room_id");
    value = room_json_value(event->text, tokens, count, room);
    bool scope = cJSON_IsString(value) && room_scope(value->valuestring);
    cJSON_Delete(value);
    int self = room_json_key(event->text, tokens, count, params, "self");
    int list = room_json_key(event->text, tokens, count, params, "participants");
    value = room_json_value(event->text, tokens, count, self);
    char self_id[129], self_device[65], self_name[65];
    memcpy(self_name, s_room.view.self_name, sizeof(self_name));
    bool valid_self = copy_json_string(value, "participant_id", self_id, sizeof(self_id), true) &&
        copy_json_string(value, "device_id", self_device, sizeof(self_device), true) && !strcmp(self_device, s_device_id);
    if (valid_self) room_self_name(value, self_name);
    cJSON_Delete(value);
    if (!scope || !valid_self || list < 0 || tokens[list].type != JSMN_ARRAY) goto done;
    unsigned used = 0, online = 0;
    bool has_self = false;
    for (int i = list + 1; i < count && tokens[i].start < tokens[list].end; i = room_json_next(tokens, count, i)) {
        if (tokens[i].end - tokens[i].start > 1024) goto done;
        value = room_json_value(event->text, tokens, count, i);
        starter_room_member_t member;
        bool valid = room_member(value, &member);
        if (valid && member.self) room_self_name(value, self_name);
        cJSON_Delete(value);
        if (!valid) goto done;
        for (unsigned j = 0; j < used; ++j)
            if (!strcmp(members[j].participant_id, member.participant_id)) goto done;
        has_self |= !strcmp(member.participant_id, self_id);
        ++online;
        if (used < STARTER_ROOM_MEMBERS_MAX) members[used++] = member;
    }
    if (!has_self) {
        ++online;
        if (used < STARTER_ROOM_MEMBERS_MAX) {
            starter_room_member_t *me = &members[used++];
            snprintf(me->participant_id, sizeof(me->participant_id), "%s", self_id);
            snprintf(me->device_id, sizeof(me->device_id), "%s", s_device_id);
            snprintf(me->name, sizeof(me->name), "%s", s_device_id);
            me->self = true;
            me->mic = starter_tirtc_room_transmitting(s_connection_generation) ? 2 : 0;
        }
    }
    memcpy(s_room.members, members, sizeof(s_room.members));
    memcpy(s_room.view.self_name, self_name, sizeof(s_room.view.self_name));
    s_room.view.member_count = used;
    s_room.view.online_count = online;
    s_room.view.members_truncated = online > used;
    s_room.view.members_ready = true;
    s_room.members_valid_until = now_ms() + 45000;
    s_room.snapshot_at = now_ms() + 30000;
    room_publish();
    ok = true;
done:
    free(tokens);
    free(members);
    return ok;
}

static void room_command(const runtime_event_t *event)
{
    if (!room_owns_media() || !event->generation || event->generation != s_connection_generation ||
        !starter_tirtc_is_room_command(event->command) || !event->text) return;
    if (!room_app_current()) { finish_session(0); return; }
    /* Find the method without materializing a cJSON tree. JSON key order is
     * unspecified, so the bounded PSRAM token index covers the whole message. */
    jsmn_parser parser;
    jsmntok_t *head = heap_caps_malloc(2048U * sizeof(*head), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!head) { room_fail(ESP_ERR_NO_MEM, "成员解析内存不足", false); return; }
    jsmn_init(&parser);
    int parsed = jsmn_parse(&parser, event->text, event->length, head, 2048);
    bool envelope = parsed > 0 && head[0].type == JSMN_OBJECT &&
        room_json_next(head, parsed, 0) == parsed;
    int rpc = envelope ? room_json_key(event->text, head, parsed, 0, "jsonrpc") : -1;
    cJSON *protocol = room_json_value(event->text, head, parsed, rpc);
    envelope = envelope && cJSON_IsString(protocol) && !strcmp(protocol->valuestring, "2.0");
    cJSON_Delete(protocol);
    int method = parsed > 0 ? room_json_key(event->text, head, parsed, 0, "method") : -1;
    cJSON *name = room_json_value(event->text, head, parsed, method);
    bool snapshot = cJSON_IsString(name) && !strcmp(name->valuestring, "room_snapshot");
    cJSON_Delete(name);
    free(head);
    if (!envelope) { room_fail(ESP_ERR_INVALID_RESPONSE, "房间信令解析失败", false); return; }
    if (snapshot) {
        /* A snapshot arriving ahead of the join ACK may update presentation,
         * but only the validated ACK below is allowed to admit audio. */
        if (!room_snapshot(event))
            room_fail(ESP_ERR_INVALID_RESPONSE, "成员数据异常，请重试", false);
        return;
    }
    if (event->length > 2048 || parsed > 64) { room_fail(ESP_ERR_INVALID_SIZE, "房间信令过长", false); return; }
    cJSON *root = cJSON_ParseWithLength(event->text, event->length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        room_fail(ESP_ERR_INVALID_RESPONSE, "房间信令格式错误", false);
        return;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsNumber(id) && id->valuedouble == 1 &&
        atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_CONNECTING) {
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(result, "session_id");
        bool valid = !cJSON_GetObjectItemCaseSensitive(root, "error") &&
            cJSON_IsString(session) && session->valuestring[0] &&
            room_audio_format(cJSON_GetObjectItemCaseSensitive(result, "input_audio")) &&
            room_audio_format(cJSON_GetObjectItemCaseSensitive(result, "output_audio"));
        cJSON_Delete(root);
        if (!valid) { room_fail(ESP_ERR_INVALID_RESPONSE, "入房失败或音频格式不兼容", false); return; }
        if (!room_send("{\"jsonrpc\":\"2.0\",\"method\":\"set_mic_state\",\"params\":{\"mic_state\":\"off\"}}") ||
            starter_media_start(STARTER_TIRTC_ROOM, s_connection_generation) != ESP_OK) {
            room_fail(ESP_FAIL, "音频启动失败", false); return;
        }
        s_deadline_ms = 0;
        s_room.failures = 0;
        s_room.heartbeat_at = now_ms();
        s_room.snapshot_at = now_ms() + 2000;
        publish_state(STARTER_RUNTIME_ROOM_ACTIVE);
        atomic_store(&s_room_ptt_app_request, s_room.app_request);
        atomic_store(&s_room_ptt_admitted, s_connection_generation);
        room_status(STARTER_ROOM_LISTENING, "等待成员信息");
        return;
    }
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    char room[257];
    bool valid = cJSON_IsString(m) && copy_json_string(params, "room_id", room, sizeof(room), true) && room_scope(room);
    if (valid && !strcmp(m->valuestring, "room_closed")) {
        cJSON_Delete(root);
        finish_session(0);
        s_room.blocked = true;
        s_room.dirty = true;
        room_status(STARTER_ROOM_ERROR, "房间已关闭，请刷新");
        return;
    }
    if (valid && (!strcmp(m->valuestring, "participant_joined") ||
                  !strcmp(m->valuestring, "participant_left"))) {
        /* Coalesce membership changes into the authoritative snapshot. */
        s_room.snapshot_at = now_ms();
        if (!strcmp(m->valuestring, "participant_left")) {
            char id_text[129];
            if (copy_json_string(params, "participant_id", id_text, sizeof(id_text), true)) {
                for (unsigned i = 0; i < s_room.view.member_count; ++i) {
                    if (strcmp(s_room.members[i].participant_id, id_text)) continue;
                    memmove(&s_room.members[i], &s_room.members[i + 1],
                        (s_room.view.member_count - i - 1) * sizeof(s_room.members[0]));
                    --s_room.view.member_count;
                    if (s_room.view.online_count) --s_room.view.online_count;
                    room_publish();
                    break;
                }
            }
        }
    } else if (valid && !strcmp(m->valuestring, "participant_mic_state_changed")) {
        char id_text[129];
        if (copy_json_string(params, "participant_id", id_text, sizeof(id_text), true)) {
            bool found = false;
            for (unsigned i = 0; i < s_room.view.member_count; ++i) {
                if (strcmp(s_room.members[i].participant_id, id_text)) continue;
                uint8_t mic = room_mic(params);
                if (mic > 2) { s_room.snapshot_at = now_ms(); break; }
                if (!s_room.members[i].self) s_room.members[i].mic = mic;
                found = true;
            }
            if (!found) s_room.snapshot_at = now_ms();
            room_publish();
        }
    }
    cJSON_Delete(root);
}

static void room_assignment(const cJSON *data)
{
    char desired[16], state[32], id[129], code[7];
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(data, "assignment_version");
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(data, "device_id");
    bool valid = cJSON_IsObject(data) && cJSON_IsString(device) && !strcmp(device->valuestring, s_device_id) &&
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "desired_state")) &&
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "state")) &&
        cJSON_IsNumber(version) && version->valuedouble >= 0 && version->valuedouble <= UINT32_MAX &&
        version->valuedouble == (double)(uint32_t)version->valuedouble &&
        copy_json_string(data, "desired_state", desired, sizeof(desired), false) &&
        copy_json_string(data, "state", state, sizeof(state), false) &&
        (!desired[0] || !strcmp(desired, "joined") || !strcmp(desired, "left"));
    if (!valid) { room_fail(ESP_ERR_INVALID_RESPONSE, "房间配置返回异常", false); return; }
    if (strcmp(desired, "joined") || !strcmp(state, "room_closed")) {
        s_room.view.assignment_known = true;
        room_self_name(data, s_room.view.self_name);
        s_room.view.assigned = false;
        if (room_owns_media()) finish_session(0);
        s_room.id[0] = s_room.session[0] = s_room.view.room_code[0] = '\0';
        s_room.version = 0;
        s_room.leave_requested = false;
        s_room.view.member_count = 0;
        s_room.view.members_ready = false;
        s_room.dirty = false;
        room_status(STARTER_ROOM_EMPTY, !strcmp(state, "room_closed") ? "房间已关闭" : "尚未加入房间");
        return;
    }
    if (!cJSON_IsNumber(version) || version->valuedouble < 1 || version->valuedouble > UINT32_MAX ||
        version->valuedouble != (double)(uint32_t)version->valuedouble ||
        !copy_json_string(data, "room_id", id, sizeof(id), true) ||
        strlen(id) > 64 ||
        !copy_json_string(data, "room_code", code, sizeof(code), true) || !room_digits(code, 6, false)) {
        room_fail(ESP_ERR_INVALID_RESPONSE, "房间标识返回异常", false); return;
    }
    bool changed = s_room.version != (uint32_t)version->valuedouble || strcmp(s_room.id, id);
    if (changed && room_owns_media()) finish_session(0);
    if (changed) { s_room.session[0] = 0; s_room.scoped_id[0] = 0; }
    s_room.view.assignment_known = true;
    room_self_name(data, s_room.view.self_name);
    snprintf(s_room.id, sizeof(s_room.id), "%s", id);
    snprintf(s_room.view.room_code, sizeof(s_room.view.room_code), "%s", code);
    s_room.version = (uint32_t)version->valuedouble;
    s_room.view.assigned = true;
    s_room.dirty = false;
    s_room.poll_at = now_ms() + 30000;
    if (!room_owns_media()) room_status(STARTER_ROOM_SUSPENDED, "等待进入对讲");
    else room_publish();
}

static void room_http_result(const runtime_event_t *event)
{
    if (!s_room.pending || event->request_tag != s_room.pending_ticket) return;
    room_op_t op = s_room.pending;
    int64_t sent_at = s_room.pending_at;
    s_room.pending = 0;
    cJSON *root = event->text ? cJSON_ParseWithLength(event->text, event->length) : NULL;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    int error = cJSON_IsNumber(code) ? code->valueint : ESP_ERR_INVALID_RESPONSE;
    if (op != ROOM_GET && op != ROOM_PRESENCE)
        ESP_LOGI(TAG, "ROOM HTTP op=%u code=%d elapsed=%lldms", (unsigned)op, error,
                 (long long)(now_ms() - sent_at));
    if (op == ROOM_RELEASE && cJSON_IsNumber(code) && room_complete_release(code->valuedouble)) {
        cJSON_Delete(root);
        room_publish();
        return;
    }
    if (!cJSON_IsNumber(code) || code->valuedouble != 200) {
        cJSON_Delete(root);
        if (op == ROOM_CREATE || op == ROOM_JOIN || op == ROOM_LEAVE) s_room.dirty = true;
        const char *message = error == 40320 ? "房间密码错误" : error == 40400 ? "房间不存在或已关闭" :
            error == 40920 ? "房间人数已满" : error == 40921 ? "房间关系或会话已更新，正在同步" :
            error == 40923 ? "已在其他房间，请先退出" : error == 42920 ? "密码错误过多，请稍后再试" :
            error == 40300 ? "请先完成设备绑定" : "房间服务请求失败";
        bool retry = op != ROOM_CREATE && op != ROOM_JOIN && op != ROOM_LEAVE &&
            (error == 40921 || error == 42900 || error >= 50000 || error == ESP_ERR_INVALID_RESPONSE);
        room_fail(error, message, retry);
        return;
    }
    if (op == ROOM_GET) {
        if (s_room.pending_revision != s_room.change_revision) {
            s_room.dirty = true;
            s_room.poll_at = now_ms();
        } else room_assignment(data);
    }
    else if (op == ROOM_CREATE || op == ROOM_JOIN || op == ROOM_LEAVE) {
        /* Mutation receipts are not online/member snapshots. Read authority next. */
        s_room.view.assignment_known = false;
        s_room.dirty = true;
        s_room.poll_at = now_ms();
        if (op == ROOM_LEAVE) s_room.leave_requested = false;
        room_status(STARTER_ROOM_SYNCING, "正在同步房间");
    } else if (op == ROOM_PRESENCE) {
        if (atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_ACTIVE) {
            s_room.lease_until = sent_at + s_room.lease_ms;
            s_room.heartbeat_at = sent_at + s_room.heartbeat_ms;
            if (s_room.view.members_ready) room_status(STARTER_ROOM_LISTENING, "房间已连接");
        }
    } else if (op == ROOM_TOKEN) {
        if (!room_app_current() || !room_owns_media() || s_room.leave_requested ||
            s_room.pending_revision != s_room.change_revision) {
            /* Preemption or a newer assignment invalidates this reservation. */
            if (room_owns_media()) finish_session(0);
            else room_remember_release(false);
            s_room.dirty = true;
        } else {
            ai_credentials_t *credentials = heap_caps_calloc(1, sizeof(*credentials), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            const cJSON *hb = cJSON_GetObjectItemCaseSensitive(data, "heartbeat_seconds");
            const cJSON *lease = cJSON_GetObjectItemCaseSensitive(data, "lease_seconds");
            bool valid = credentials && copy_json_string(data, "peer_id", credentials->peer_id, sizeof(credentials->peer_id), true) &&
                copy_json_string(data, "token", credentials->token, sizeof(credentials->token), true) &&
                cJSON_IsNumber(hb) && cJSON_IsNumber(lease) && hb->valueint >= 1 &&
                hb->valuedouble == hb->valueint && lease->valuedouble == lease->valueint &&
                hb->valueint < lease->valueint && lease->valueint <= 3600;
            if (!valid) room_fail(ESP_ERR_INVALID_RESPONSE, "入房凭据返回异常", false);
            else {
                s_room.heartbeat_ms = hb->valueint * 1000U;
                s_room.lease_ms = lease->valueint * 1000U;
                s_room.lease_until = sent_at + s_room.lease_ms;
                (void)room_presence("connecting", false);
                int rc = prepare_external_connect() ?
                    starter_tirtc_room_connect(credentials->peer_id, credentials->token, s_session_generation) : ESP_ERR_NO_MEM;
                if (rc) room_fail(rc, "房间连接提交失败", true);
                else s_deadline_ms = now_ms() + 12000;
            }
            if (credentials) { memset(credentials, 0, sizeof(*credentials)); free(credentials); }
        }
    }
    cJSON_Delete(root);
    room_publish();
}

static void room_handle_intent(const runtime_event_t *event)
{
    /* Discard an old page's queued tap, including close/open between ticks. */
    if (!room_app_current() || event->request_tag != s_room.app_request) return;
    room_op_t op = (room_op_t)event->command;
    if (op == ROOM_LEAVE) {
        s_room.leave_requested = true;
        s_room.blocked = false;
        if (room_owns_media()) finish_session(0);
        room_status(STARTER_ROOM_LEAVING, wifi_manager_connected() ? "正在退出房间" : "联网后退出房间");
        return;
    }
    if (op == ROOM_REFRESH) {
        s_room.view.assignment_known = false;
        s_room.blocked = false;
        s_room.failures = 0;
        s_room.retry_at = 0;
        s_room.dirty = true;
        s_room.poll_at = now_ms();
        s_room.snapshot_at = now_ms();
        room_publish();
        return;
    }
    if (!s_room.view.assignment_known || s_room.pending || s_room.leave_requested || s_room.view.assigned) {
        room_status(s_room.view.state, s_room.view.assigned ? "请先退出当前房间" : "正在处理，请稍候");
        return;
    }
    if (!platform_client_ready() || !wifi_manager_connected()) {
        room_status(STARTER_ROOM_ERROR, "请先联网并完成绑定"); return;
    }
    cJSON *body = event->text ? cJSON_Parse(event->text) : NULL;
    if (op == ROOM_CREATE) cJSON_DeleteItemFromObjectCaseSensitive(body, "room_code");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (cJSON_IsString(password) && !password->valuestring[0]) cJSON_DeleteItemFromObjectCaseSensitive(body, "password");
    s_room.blocked = false;
    s_room.failures = 0;
    esp_err_t err = body ? room_http(op, body) : ESP_ERR_NO_MEM;
    cJSON_Delete(body);
    if (err != ESP_OK) room_fail(err, "房间请求未提交", false);
    else room_status(STARTER_ROOM_SYNCING, op == ROOM_CREATE ? "正在创建房间" : "正在加入房间");
}

static void room_tick(void)
{
    int64_t now = now_ms();
    if (s_room.epoch != platform_client_epoch()) room_reset();
    room_apply_navigation();
    unsigned lost_session = atomic_exchange(&s_room_lost_session, 0);
    if (lost_session && lost_session == s_session_generation && room_owns_media())
        room_fail(ESP_ERR_NO_MEM, "房间事件未送达，请重试", true);
    unsigned lost = atomic_exchange(&s_room_http_lost, 0);
    if ((s_room.pending && lost != 0 && lost == s_room.pending_ticket) ||
        (s_room.pending && now - s_room.pending_at >= 12000)) {
        room_op_t op = s_room.pending;
        s_room.pending = 0;
        ++s_room.ticket;
        room_fail(ESP_ERR_TIMEOUT, "房间请求超时，请刷新", op == ROOM_GET || op == ROOM_PRESENCE || op == ROOM_TOKEN || op == ROOM_RELEASE);
    }
    if (!wifi_manager_connected() || !platform_client_ready()) {
        if (room_owns_media()) room_fail(ESP_ERR_INVALID_STATE, "网络已断开", true);
        return;
    }
    if (s_room.blocked || now < s_room.retry_at) return;
    if (s_room.release.id[0]) {
        /* The existing HTTP/runtime workers own this transaction. No UI wait,
         * new task, or new token until the old reservation is resolved. */
        if (!s_room.pending) {
            esp_err_t err = room_release_request();
            if (err != ESP_OK) room_fail(err, "旧会话释放请求未提交", true);
        }
        return;
    }
    /* Closing an app stops work, not its server assignment. Only a release
     * receipt or an explicitly requested durable leave may finish off-page. */
    if (!room_app_current() && !s_room.leave_requested) return;
    if (atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_ACTIVE) {
        if (s_room.view.members_ready && now >= s_room.members_valid_until) {
            s_room.view.members_ready = false;
            s_room.view.member_count = 0;
            ESP_LOGW(TAG, "ROOM member snapshot expired; requesting current members");
            s_room.snapshot_at = now;
            room_publish();
        }
        bool speaking = starter_tirtc_room_transmitting(s_connection_generation);
        if (speaking && (uint32_t)((uint32_t)now - atomic_load(&s_room_ptt_touch_ms)) > 750U) {
            starter_media_room_ptt(s_connection_generation, false);
            speaking = false;
            ESP_LOGW(TAG, "ROOM PTT released: touch heartbeat expired");
        }
        if (speaking != s_room.mic_announced) {
            if (!room_send(speaking ? "{\"jsonrpc\":\"2.0\",\"method\":\"set_mic_state\",\"params\":{\"mic_state\":\"speaking\"}}" :
                                     "{\"jsonrpc\":\"2.0\",\"method\":\"set_mic_state\",\"params\":{\"mic_state\":\"off\"}}")) {
                room_fail(ESP_FAIL, "麦克风状态同步失败", true); return;
            }
            s_room.mic_announced = speaking;
            ESP_LOGI(TAG, "ROOM mic=%s generation=%lu", speaking ? "speaking" : "off",
                     (unsigned long)s_connection_generation);
            for (unsigned i = 0; i < s_room.view.member_count; ++i)
                if (s_room.members[i].self) s_room.members[i].mic = speaking ? 2 : 0;
            room_publish();
        }
        if (now >= s_room.lease_until) { room_fail(ESP_ERR_TIMEOUT, "房间在线租约已过期", true); return; }
        if (!s_room.pending && now >= s_room.heartbeat_at) {
            esp_err_t err = room_presence("joined", true);
            if (err != ESP_OK) { room_fail(err, "在线心跳未提交", true); return; }
        }
        if (now >= s_room.snapshot_at && now >= s_room.snapshot_allowed_at) {
            if (!room_send("{\"jsonrpc\":\"2.0\",\"method\":\"get_room_snapshot\"}")) {
                room_fail(ESP_FAIL, "成员列表刷新失败", true); return;
            }
            s_room.snapshot_at = now + 5000;
            s_room.snapshot_allowed_at = now + 500;
        }
    }
    if (s_room.pending) return;
    if (s_room.leave_requested) {
        cJSON *body = s_room.view.assigned ? room_relation_body(false) : cJSON_CreateObject();
        esp_err_t err = body ? room_http(ROOM_LEAVE, body) : ESP_ERR_NO_MEM;
        cJSON_Delete(body);
        if (err != ESP_OK) room_fail(err, "退出请求未提交，请重试", false);
        return;
    }
    if (!room_app_current()) return;
    if (s_room.dirty || now >= s_room.poll_at) {
        /* Leave headroom for presence in the shared HTTP worker. */
        if (atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_ACTIVE && s_room.heartbeat_at - now < 8000) return;
        esp_err_t err = room_http(ROOM_GET, NULL);
        if (err != ESP_OK) room_fail(err, "房间配置查询未提交", true);
        else s_room.poll_at = now + 30000;
        return;
    }
    if (!s_room.view.assigned || !starter_tirtc_started() || room_owns_media()) return;
    int state = atomic_load(&s_public_state);
    if (state != STARTER_RUNTIME_WAITING && state != STARTER_RUNTIME_H5_ACTIVE) {
        if (s_room.view.state != STARTER_ROOM_SUSPENDED) room_status(STARTER_ROOM_SUSPENDED, "通话结束后恢复对讲");
        return;
    }
    if (state == STARTER_RUNTIME_H5_ACTIVE) finish_session(0);
    starter_tirtc_accept_h5(false);
    starter_media_stop();
    (void)starter_tirtc_disconnect();
    s_connection_generation = 0;
    if (!++s_session_generation) ++s_session_generation;
    snprintf(s_room.session, sizeof(s_room.session), "%08lx%08lx%08lx%08lx",
        (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    publish_state(STARTER_RUNTIME_ROOM_CONNECTING);
    s_deadline_ms = now + 12000;
    cJSON *body = room_relation_body(true);
    esp_err_t err = body ? room_http(ROOM_TOKEN, body) : ESP_ERR_NO_MEM;
    cJSON_Delete(body);
    if (err != ESP_OK) room_fail(err, "入房凭据请求未提交", true);
    else room_status(STARTER_ROOM_CONNECTING, "正在连接房间");
}
