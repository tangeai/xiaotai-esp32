/* Room owns a persistent server assignment, not a second RTC connection.
 * Included in the runtime owner: callbacks only enqueue, no extra task/stack.
 * HTTP assignment version, platform epoch, local ticket, SDK generation and
 * pinned signaling room ID are separate identities and must not be conflated. */
#include "room_json_declarations.h"
#include "room_json_prefix.h"
#define ROOM_COMMAND 0x2200U
#define ROOM_SYNC_MS 30000
#define ROOM_HTTP_MS 8000U

enum { ROOM_SYNC = 1, ROOM_CREATE, ROOM_JOIN, ROOM_LEAVE, ROOM_TOKEN, ROOM_PRESENCE, ROOM_RELEASED };
static EXT_RAM_BSS_ATTR struct {
    starter_room_phase_t phase;
    bool assigned;
    bool assignment_known;
    bool snapshot_ready;
    bool needs_sync;
    bool connected_attempt;
    bool mic_announced;
    bool dirty;
    bool online_before;
    unsigned pending;
    unsigned retries;
    uint32_t ticket;
    uint32_t version;
    uint32_t epoch;
    uint32_t generation;
    uint32_t revision;
    uint32_t notice_revision;
    uint32_t sync_revision;
    uint32_t app_revision;
    uint16_t count;
    int error;
    int64_t due;
    int64_t pending_deadline;
    int64_t pending_since;
    int64_t retry_due;
    int64_t heartbeat_due;
    int64_t lease_deadline;
    uint32_t heartbeat_ms;
    uint32_t lease_ms;
    char room_id[65];
    char room_code[7];
    char self_name[65];
    char session_id[33];
    char signaling_room[161];
    starter_room_member_t members[STARTER_ROOM_MEMBERS_MAX];
} s_room;
static EXT_RAM_BSS_ATTR struct {
    starter_room_view_t view;
    starter_room_member_t members[STARTER_ROOM_MEMBERS_MAX];
} s_room_public;
static atomic_uint s_room_ptt_intent;
static atomic_uint s_room_response_lost;
/* Low bit is desired foreground ownership; upper bits detect close/reopen
 * between two owner ticks, so an old connection can never be reused. */
static atomic_uint s_room_app_request;
static atomic_uint s_room_app_applied;

static bool room_app_open(void) { return (atomic_load(&s_room_app_request) & 1U) != 0; }
static bool room_app_current(void)
{
    unsigned request = atomic_load(&s_room_app_request);
    return (request & 1U) && atomic_load(&s_room_app_applied) == request;
}

void starter_runtime_room_set_open(bool open)
{
    unsigned old = atomic_load(&s_room_app_request);
    while ((old & 1U) != (unsigned)open) {
        unsigned next = ((old + 2U) & ~1U) | (unsigned)open;
        if (atomic_compare_exchange_weak(&s_room_app_request, &old, next)) break;
    }
    if (!open) {
        /* Cancel only our held generation. A redundant page-close must not
         * advance the shared audio epoch of an AI/call already in progress. */
        unsigned generation = atomic_exchange(&s_room_ptt_intent, 0);
        if (generation) (void)starter_media_room_ptt(generation, false);
    }
}

static bool room_owns_media(void)
{
    int state = atomic_load(&s_public_state);
    return state == STARTER_RUNTIME_ROOM_CONNECTING || state == STARTER_RUNTIME_ROOM_ACTIVE;
}

static void room_invalidate_assignment(void)
{
    ++s_room.notice_revision;
    s_room.needs_sync = true;
    s_room.due = now_ms();
}

static bool room_digits(const char *value, unsigned count, bool optional)
{
    if (optional && (value == NULL || !value[0])) return true;
    if (value == NULL || strlen(value) != count) return false;
    for (unsigned i = 0; i < count; ++i) if (value[i] < '0' || value[i] > '9') return false;
    return true;
}

static void room_publish(void)
{
    if (!s_room.dirty || s_product_mutex == NULL || xSemaphoreTake(s_product_mutex, 0) != pdTRUE) return;
    starter_room_view_t *view = &s_room_public.view;
    memset(view, 0, sizeof(*view));
    view->revision = ++s_room.revision;
    view->generation = s_room.generation;
    view->phase = s_room.phase;
    view->assigned = s_room.assigned;
    view->assignment_known = s_room.assignment_known;
    view->app_open = room_app_open();
    view->busy = s_room.pending == ROOM_CREATE || s_room.pending == ROOM_JOIN || s_room.pending == ROOM_LEAVE;
    view->snapshot_ready = s_room.snapshot_ready;
    view->member_count = s_room.count;
    view->error = s_room.error;
    memcpy(view->room_code, s_room.room_code, sizeof(view->room_code));
    memcpy(view->self_name, s_room.self_name, sizeof(view->self_name));
    memcpy(s_room_public.members, s_room.members, sizeof(s_room.members));
    s_room.dirty = false;
    xSemaphoreGive(s_product_mutex);
}

bool starter_runtime_room_read(uint16_t offset, starter_room_view_t *out)
{
    if (out == NULL || s_product_mutex == NULL || xSemaphoreTake(s_product_mutex, 0) != pdTRUE) return false;
    *out = s_room_public.view;
    if (offset >= out->member_count) offset = out->member_count ? ((out->member_count - 1) / 3) * 3 : 0;
    out->offset = offset;
    out->count = out->member_count - offset;
    if (out->count > STARTER_ROOM_PAGE_SIZE) out->count = STARTER_ROOM_PAGE_SIZE;
    memcpy(out->members, s_room_public.members + offset, out->count * sizeof(out->members[0]));
    xSemaphoreGive(s_product_mutex);
    return true;
}

static void room_response(const char *body, void *user_data)
{
    if (!queue_http_result(EVENT_ROOM_HTTP, (uint32_t)(uintptr_t)user_data, 0, body))
        atomic_store(&s_room_response_lost, (uint32_t)(uintptr_t)user_data);
}

static cJSON *room_identity(void)
{
    cJSON *body = cJSON_CreateObject();
    if (body && cJSON_AddStringToObject(body, "room_id", s_room.room_id) &&
        cJSON_AddNumberToObject(body, "assignment_version", s_room.version) &&
        cJSON_AddStringToObject(body, "session_id", s_room.session_id)) return body;
    cJSON_Delete(body);
    return NULL;
}

static esp_err_t room_request(unsigned operation, cJSON *body)
{
    static const char *const paths[] = {"", "/v1/call/group/device/assignment",
        "/v1/call/group/device/create", "/v1/call/group/device/join",
        "/v1/call/group/device/leave", "/v1/call/group/device/connect-token",
        "/v1/call/group/device/presence"};
    if (s_room.pending || operation < ROOM_SYNC || operation > ROOM_PRESENCE) {
        cJSON_Delete(body);
        return ESP_ERR_INVALID_STATE;
    }
    char *json = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (operation != ROOM_SYNC && json == NULL) return ESP_ERR_NO_MEM;
    if (++s_room.ticket == 0) ++s_room.ticket;
    int64_t requested_ms = now_ms();
    esp_err_t err = platform_client_request_timeout(PLATFORM_SERVICE_CALL, paths[operation],
        json, ROOM_HTTP_MS, room_response, (void *)(uintptr_t)s_room.ticket);
    cJSON_free(json);
    if (err == ESP_OK) {
        s_room.pending = operation;
        s_room.pending_since = requested_ms;
        s_room.pending_deadline = now_ms() + ROOM_HTTP_MS + 12000;
        if (operation == ROOM_SYNC) s_room.sync_revision = s_room.notice_revision;
        s_room.dirty = true;
    }
    ESP_LOGI(TAG, "ROOM http op=%u ticket=%lu queued=%d", operation, (unsigned long)s_room.ticket, err);
    return err;
}

static int room_send(const char *method, cJSON *params, bool request)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root && cJSON_AddStringToObject(root, "jsonrpc", "2.0") &&
              cJSON_AddStringToObject(root, "method", method);
    if (request) ok = ok && cJSON_AddNumberToObject(root, "id", s_session_generation);
    if (ok && params) {
        ok = cJSON_AddItemToObject(root, "params", params);
        if (ok) params = NULL;
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(params);
    cJSON_Delete(root);
    int ret = json ? starter_tirtc_send_command(ROOM_COMMAND, json, strlen(json)) : -ESP_ERR_NO_MEM;
    cJSON_free(json);
    if (ret < 0) ESP_LOGW(TAG, "ROOM signal method=%s rc=%d", method, ret);
    return ret;
}

static bool room_mic(bool speaking)
{
    cJSON *params = cJSON_CreateObject();
    if (!params || !cJSON_AddStringToObject(params, "mic_state", speaking ? "speaking" : "off")) {
        cJSON_Delete(params);
        return false;
    }
    int ret = room_send("set_mic_state", params, false);
    return ret >= 0;
}

static void room_release_response(const char *body, void *unused)
{
    (void)unused;
    /* Parse on the runtime owner, not concurrently on the HTTP task. */
    (void)queue_http_result(EVENT_ROOM_HTTP, 0, ROOM_RELEASED, body);
}

static void room_released(const char *body)
{
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 200)
        ESP_LOGW(TAG, "ROOM presence release unconfirmed code=%d; server lease remains authoritative",
                 cJSON_IsNumber(code) ? code->valueint : -1);
    cJSON_Delete(root);
}

/* Called BEFORE generic teardown, including preemption, unbind and failures.
 * Releasing this lease never deletes the persistent assignment. */
static void room_detach(int error)
{
    if (!room_owns_media()) return;
    atomic_store(&s_room_ptt_intent, 0);
    (void)starter_media_room_ptt(0, false);
    if (s_room.generation && starter_tirtc_mode() == STARTER_TIRTC_ROOM)
        (void)room_send("leave_room", NULL, false);
    if (s_room.connected_attempt && s_room.epoch == platform_client_epoch() && platform_client_ready()) {
        cJSON *body = room_identity();
        const char *state = error ? "connect_failed" : room_app_open() ? "suspended" : "left";
        char *json = body && cJSON_AddStringToObject(body, "state", state) ?
                     cJSON_PrintUnformatted(body) : NULL;
        cJSON_Delete(body);
        esp_err_t err = json ? platform_client_request_timeout(PLATFORM_SERVICE_CALL,
            "/v1/call/group/device/presence", json, ROOM_HTTP_MS, room_release_response, NULL) : ESP_ERR_NO_MEM;
        cJSON_free(json);
        if (err != ESP_OK) ESP_LOGW(TAG, "ROOM lease release not queued rc=%d", err);
    }
    s_room.connected_attempt = false;
    s_room.generation = 0;
    s_room.signaling_room[0] = 0;
    s_room.mic_announced = false;
    s_room.snapshot_ready = false;
    s_room.count = 0;
    /* Invalidate token/heartbeat callbacks, but never a user leave transaction. */
    if (s_room.pending == ROOM_TOKEN || s_room.pending == ROOM_PRESENCE) {
        s_room.pending = 0;
        ++s_room.ticket;
    }
    s_room.phase = error ? STARTER_ROOM_ERROR : room_app_open() ? STARTER_ROOM_SUSPENDED : STARTER_ROOM_OFFLINE;
    s_room.error = error;
    s_room.needs_sync = true;
    s_room.due = now_ms();
    s_room.dirty = true;
}

/* Runtime-owner only. Do not delete shared tasks or cancel the shared HTTP
 * worker. Already submitted CRUD may finish once; old reads/token replies are
 * invalidated, and closing never launches another Room poll or connection. */
static void room_apply_app_request(void)
{
    unsigned request = atomic_load(&s_room_app_request);
    if (request == s_room.app_revision) return;
    s_room.app_revision = request;
    if (room_owns_media()) finish_session(0);
    if (s_room.pending == ROOM_SYNC || s_room.pending == ROOM_TOKEN || s_room.pending == ROOM_PRESENCE) {
        s_room.pending = 0;
        ++s_room.ticket;
    }
    s_room.snapshot_ready = false;
    s_room.count = 0;
    s_room.retries = 0;
    s_room.retry_due = 0;
    s_room.assignment_known = false;
    room_invalidate_assignment();
    s_room.phase = room_app_open() ? STARTER_ROOM_SYNCING :
                   s_room.assigned ? STARTER_ROOM_OFFLINE : STARTER_ROOM_EMPTY;
    atomic_store(&s_room_app_applied, request);
    s_room.dirty = true;
    ESP_LOGI(TAG, "ROOM app=%s revision=%lu", room_app_open() ? "open" : "closed", (unsigned long)request);
}

static void room_fail(int error)
{
    if (room_owns_media()) finish_session(error);
    s_room.phase = STARTER_ROOM_ERROR;
    s_room.error = error;
    s_room.retry_due = now_ms() + (error == 40921 ? s_room.lease_ms : 5000);
    if (s_room.retry_due <= now_ms()) s_room.retry_due = now_ms() + 45000;
    ++s_room.retries;
    s_room.needs_sync = true;
    s_room.due = s_room.retry_due;
    s_room.dirty = true;
    ESP_LOGW(TAG, "ROOM failure code=%d attempt=%u", error, s_room.retries);
}

static bool room_audio_profile(const cJSON *object)
{
    const cJSON *codec = cJSON_GetObjectItemCaseSensitive(object, "codec");
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(object, "sample_rate");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(object, "channels");
    return cJSON_IsString(codec) && strcmp(codec->valuestring, "g711a") == 0 &&
        cJSON_IsNumber(rate) && rate->valuedouble == 8000 && cJSON_IsNumber(channels) && channels->valuedouble == 1;
}

static bool room_add_audio(cJSON *params, const char *name)
{
    cJSON *format = cJSON_AddObjectToObject(params, name);
    return format && cJSON_AddStringToObject(format, "codec", "g711a") &&
        cJSON_AddNumberToObject(format, "sample_rate", 8000) && cJSON_AddNumberToObject(format, "channels", 1);
}

static void room_connection(const runtime_event_t *event)
{
    room_apply_app_request();
    if (!room_app_current() || !room_owns_media()) return;
    bool current = event->generation && s_room.generation ? event->generation == s_room.generation :
        event->request_tag != 0 && event->request_tag == s_session_generation;
    if (!current) return;
    restore_external_connect_memory();
    if (!event->flag || !platform_client_ready()) { room_fail(event->error ? event->error : ESP_FAIL); return; }
    if (s_room.generation || event->generation == 0) return;
    s_room.generation = event->generation;
    s_connection_generation = event->generation;
    s_room.dirty = true;
    cJSON *params = cJSON_CreateObject();
    bool valid = params && cJSON_AddStringToObject(params, "room_id", s_room.room_id) &&
        cJSON_AddStringToObject(params, "device_id", s_device_id) &&
        room_add_audio(params, "input_audio") && room_add_audio(params, "output_audio");
    if (!valid) { cJSON_Delete(params); room_fail(ESP_ERR_NO_MEM); return; }
    int ret = room_send("join_room", params, true);
    if (ret < 0) { room_fail(ret); return; }
    s_deadline_ms = now_ms() + 9000;
    ESP_LOGI(TAG, "ROOM join sent gen=%lu", (unsigned long)s_room.generation);
}

/* The first snapshot can carry app_namespace:business_id. Pin the full value
 * once after checking the suffix and self identity; subsequent events must
 * match exactly. HTTP always retains the original business ID. */
static bool room_pin(const char *id)
{
    if (!id || !id[0] || strlen(id) >= sizeof(s_room.signaling_room)) return false;
    if (s_room.signaling_room[0]) return strcmp(id, s_room.signaling_room) == 0;
    size_t n = strlen(id), business = strlen(s_room.room_id);
    if (strcmp(id, s_room.room_id) != 0 &&
        !(n > business + 1 && id[n - business - 1] == ':' && strcmp(id + n - business, s_room.room_id) == 0)) return false;
    strcpy(s_room.signaling_room, id);
    return true;
}

static int room_member_index(const char *id)
{
    if (!id) return -1;
    for (unsigned i = 0; i < s_room.count; ++i)
        if (strcmp(s_room.members[i].participant_id, id) == 0) return i;
    return -1;
}

/* Optional metadata from newer deployments. The public Room contract only
 * guarantees IDs; absent/invalid names must never reject otherwise valid audio.
 * A supplied empty name clears a previous rename. Keep only our own name. */
static void room_self_name(const cJSON *object)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(object, "device_name");
    if (cJSON_IsString(name) && name->valuestring && strlen(name->valuestring) < sizeof(s_room.self_name)) {
        strcpy(s_room.self_name, name->valuestring);
        s_room.dirty = true;
    }
}

static bool room_member_update(const cJSON *object)
{
    starter_room_member_t member = {0};
    if (!copy_json_string(object, "participant_id", member.participant_id, sizeof(member.participant_id), true) ||
        !copy_json_string(object, "device_id", member.device_id, sizeof(member.device_id), true)) return false;
    const cJSON *mic = cJSON_GetObjectItemCaseSensitive(object, "mic_state");
    member.speaking = cJSON_IsString(mic) && strcmp(mic->valuestring, "speaking") == 0;
    member.self = strcmp(member.device_id, s_device_id) == 0;
    if (member.self) room_self_name(object);
    int index = room_member_index(member.participant_id);
    if (index < 0) {
        if (s_room.count == STARTER_ROOM_MEMBERS_MAX) return false;
        index = s_room.count++;
    }
    s_room.members[index] = member;
    return true;
}

static void room_command(const runtime_event_t *event)
{
    room_apply_app_request();
    if (!room_app_current() || !room_owns_media() || event->generation == 0 || event->generation != s_room.generation ||
        !starter_tirtc_is_room_command(event->command)) return;
    cJSON *root = cJSON_ParseWithLength(event->text, event->length);
    if (!root) { room_fail(ESP_ERR_INVALID_RESPONSE); return; }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id)) {
        if (id->valuedouble == s_session_generation && atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_CONNECTING) {
            const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
            const cJSON *session = cJSON_GetObjectItemCaseSensitive(result, "session_id");
            if (!cJSON_IsObject(result) || cJSON_HasObjectItem(root, "error") ||
                !cJSON_IsString(session) || !session->valuestring[0] ||
                !room_audio_profile(cJSON_GetObjectItemCaseSensitive(result, "input_audio")) ||
                !room_audio_profile(cJSON_GetObjectItemCaseSensitive(result, "output_audio"))) {
                cJSON_Delete(root); room_fail(ESP_ERR_INVALID_RESPONSE); return;
            }
            if (now_ms() >= s_room.lease_deadline) {
                cJSON_Delete(root); room_fail(ESP_ERR_TIMEOUT); return;
            }
            (void)starter_media_room_ptt(0, false);
            esp_err_t err = starter_media_start(STARTER_TIRTC_ROOM, s_room.generation);
            if (err != ESP_OK) { cJSON_Delete(root); room_fail(err); return; }
            s_deadline_ms = 0;
            s_room.phase = STARTER_ROOM_LISTENING;
            s_room.heartbeat_due = now_ms();
            publish_state(STARTER_RUNTIME_ROOM_ACTIVE);
            s_room.retries = 0;
            if (!room_mic(false)) { cJSON_Delete(root); room_fail(ESP_FAIL); return; }
            ESP_LOGI(TAG, "ROOM joined gen=%lu format=g711a/8000/1", (unsigned long)s_room.generation);
        }
        s_room.dirty = true;
        cJSON_Delete(root);
        return;
    }
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(params, "room_id");
    if (!cJSON_IsString(method) || !cJSON_IsObject(params) || !cJSON_IsString(room)) { cJSON_Delete(root); return; }
    bool snapshot = strcmp(method->valuestring, "room_snapshot") == 0;
    if (snapshot) {
        const cJSON *self = cJSON_GetObjectItemCaseSensitive(params, "self");
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(self, "device_id");
        if (!cJSON_IsString(device) || strcmp(device->valuestring, s_device_id) != 0) { cJSON_Delete(root); return; }
    } else if (!s_room.signaling_room[0]) {
        (void)room_send("get_room_snapshot", NULL, false);
        cJSON_Delete(root);
        return;
    }
    if (!room_pin(room->valuestring)) { cJSON_Delete(root); return; }
    bool valid = true;
    if (snapshot) {
        const cJSON *members = cJSON_GetObjectItemCaseSensitive(params, "participants");
        valid = cJSON_IsArray(members) && cJSON_GetArraySize(members) <= (int)STARTER_ROOM_MEMBERS_MAX;
        s_room.count = 0;
        const cJSON *item;
        if (valid) cJSON_ArrayForEach(item, members) { if (!room_member_update(item)) { valid = false; break; } }
        /* Some servers list self only in the dedicated self field. Upsert by
         * participant ID so both documented snapshot forms count self once. */
        const cJSON *self = cJSON_GetObjectItemCaseSensitive(params, "self");
        const cJSON *self_id = cJSON_GetObjectItemCaseSensitive(self, "participant_id");
        if (valid && cJSON_IsString(self_id) && room_member_index(self_id->valuestring) < 0)
            valid = room_member_update(self);
        if (valid) room_self_name(self);
        s_room.snapshot_ready = valid;
    } else if (strcmp(method->valuestring, "participant_joined") == 0) {
        valid = room_member_update(cJSON_GetObjectItemCaseSensitive(params, "participant"));
    } else if (strcmp(method->valuestring, "room_closed") == 0) {
        cJSON_Delete(root);
        room_fail(40400);
        return;
    } else {
        const cJSON *participant = cJSON_GetObjectItemCaseSensitive(params, "participant_id");
        int index = cJSON_IsString(participant) ? room_member_index(participant->valuestring) : -1;
        if (index >= 0 && strcmp(method->valuestring, "participant_left") == 0) {
            memmove(s_room.members + index, s_room.members + index + 1,
                    (--s_room.count - index) * sizeof(s_room.members[0]));
        } else if (index >= 0 && strcmp(method->valuestring, "participant_mic_state_changed") == 0) {
            const cJSON *mic = cJSON_GetObjectItemCaseSensitive(params, "mic_state");
            s_room.members[index].speaking = cJSON_IsString(mic) && strcmp(mic->valuestring, "speaking") == 0;
        }
    }
    cJSON_Delete(root);
    if (!valid) { room_fail(ESP_ERR_INVALID_SIZE); return; }
    s_room.dirty = true;
}

static void room_apply_assignment(const cJSON *data)
{
    char id[65] = {0}, code[7] = {0}, desired[16] = {0};
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(data, "assignment_version");
    if (!cJSON_IsNumber(version) || version->valuedouble < 0 || version->valuedouble > UINT32_MAX ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "desired_state")) ||
        !copy_json_string(data, "desired_state", desired, sizeof(desired), false)) { room_fail(ESP_ERR_INVALID_RESPONSE); return; }
    bool assigned = strcmp(desired, "joined") == 0;
    if (assigned && (!copy_json_string(data, "room_id", id, sizeof(id), true) ||
        !copy_json_string(data, "room_code", code, sizeof(code), true) || !room_digits(code, 6, false) ||
        version->valuedouble < 1)) { room_fail(ESP_ERR_INVALID_RESPONSE); return; }
    uint32_t next_version = (uint32_t)version->valuedouble;
    if (next_version != version->valuedouble ||
        (!assigned && desired[0] && strcmp(desired, "left") != 0)) {
        room_fail(ESP_ERR_INVALID_RESPONSE); return;
    }
    if (next_version < s_room.version) {
        s_room.needs_sync = true;
        s_room.due = now_ms() + 5000;
        ESP_LOGW(TAG, "ROOM stale assignment version=%lu current=%lu",
                 (unsigned long)next_version, (unsigned long)s_room.version);
        return;
    }
    bool changed = assigned != s_room.assigned || next_version != s_room.version || strcmp(id, s_room.room_id) != 0;
    if (changed && room_owns_media()) finish_session(0);
    s_room.assigned = assigned;
    s_room.assignment_known = true;
    room_self_name(data);
    s_room.version = next_version;
    strcpy(s_room.room_id, id);
    strcpy(s_room.room_code, code);
    s_room.needs_sync = false;
    s_room.due = now_ms() + ROOM_SYNC_MS;
    if (changed) { s_room.retries = 0; s_room.retry_due = 0; s_room.error = 0; }
    if (!room_owns_media()) s_room.phase = !assigned ? STARTER_ROOM_EMPTY : !room_app_open() ? STARTER_ROOM_OFFLINE :
        atomic_load(&s_public_state) != STARTER_RUNTIME_WAITING ? STARTER_ROOM_SUSPENDED :
        s_room.error ? STARTER_ROOM_ERROR : STARTER_ROOM_SYNCING;
    s_room.dirty = true;
}

static void room_http_failure(unsigned operation, int error)
{
    /* HTTP metadata and lease renewal share the worker. A transient failure
     * must not end still-valid audio; the unchanged lease deadline is the bound. */
    if (atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_ACTIVE &&
        (operation == ROOM_SYNC || operation == ROOM_PRESENCE) &&
        error != 40300 && error != 40400 && error != 40921 && error != 401) {
        s_room.error = error;
        s_room.dirty = true;
        if (operation == ROOM_SYNC) { s_room.needs_sync = true; s_room.due = now_ms() + 5000; }
        else s_room.heartbeat_due = now_ms() + 2000;
        ESP_LOGW(TAG, "ROOM metadata failed op=%u code=%d; lease unchanged", operation, error);
        return;
    }
    room_fail(error);
}

static void room_http(const runtime_event_t *event)
{
    room_apply_app_request();
    if (event->command == ROOM_RELEASED) { room_released(event->text); return; }
    if (!s_room.pending || event->request_tag != s_room.ticket) return;
    unsigned operation = s_room.pending;
    s_room.pending = 0;
    s_room.dirty = true;
    cJSON *root = event->text ? cJSON_ParseWithLength(event->text, event->length) : NULL;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    int error = cJSON_IsNumber(code) ? code->valueint : ESP_ERR_INVALID_RESPONSE;
    ESP_LOGI(TAG, "ROOM http done op=%u ticket=%lu code=%d", operation, (unsigned long)event->request_tag, error);
    if (error != 200) {
        cJSON_Delete(root);
        room_http_failure(operation, error);
        return;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (operation == ROOM_PRESENCE) {
        s_room.error = 0;
        /* HTTP queue/RTT must not artificially extend the server's lease.
         * Request time is a conservative lower bound on server processing. */
        s_room.lease_deadline = s_room.pending_since + s_room.lease_ms;
        s_room.heartbeat_due = now_ms() + s_room.heartbeat_ms;
    } else if (operation != ROOM_TOKEN) {
        room_apply_assignment(data);
        /* Coalesce MQTT/refresh notices arriving during a read. Its old
         * response must not erase a newer invalidation or reconnect stale data. */
        if (operation == ROOM_SYNC && s_room.sync_revision != s_room.notice_revision)
            room_invalidate_assignment();
    } else {
        const cJSON *heartbeat = cJSON_GetObjectItemCaseSensitive(data, "heartbeat_seconds");
        const cJSON *lease = cJSON_GetObjectItemCaseSensitive(data, "lease_seconds");
        ai_credentials_t *credentials = heap_caps_calloc(1, sizeof(*credentials), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bool valid = credentials && cJSON_IsNumber(heartbeat) && heartbeat->valueint >= 1 && heartbeat->valueint <= 300 &&
            heartbeat->valuedouble == heartbeat->valueint &&
            cJSON_IsNumber(lease) && lease->valueint > heartbeat->valueint * 2 && lease->valueint <= 3600 &&
            lease->valuedouble == lease->valueint &&
            copy_json_string(data, "peer_id", credentials->peer_id, sizeof(credentials->peer_id), true) &&
            copy_json_string(data, "token", credentials->token, sizeof(credentials->token), true);
        if (!valid || !room_owns_media()) {
            free(credentials); cJSON_Delete(root); room_fail(valid ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_RESPONSE); return;
        }
        s_room.heartbeat_ms = heartbeat->valueint * 1000U;
        s_room.lease_ms = lease->valueint * 1000U;
        s_room.lease_deadline = s_room.pending_since + s_room.lease_ms;
        cJSON_Delete(root);
        if (!prepare_external_connect_memory()) { free(credentials); room_fail(ESP_ERR_NO_MEM); return; }
        s_deadline_ms = now_ms() + 20000;
        int ret = starter_tirtc_room_connect(credentials->peer_id, credentials->token, s_session_generation);
        memset(credentials, 0, sizeof(*credentials));
        free(credentials);
        if (ret != 0) room_fail(ret);
        return;
    }
    cJSON_Delete(root);
}

static void room_intent(const runtime_event_t *event)
{
    room_apply_app_request();
    if (!room_app_current() || event->generation != s_room.app_revision) return;
    if (event->command == ROOM_SYNC) {
        room_invalidate_assignment(); s_room.retries = 0; s_room.retry_due = 0;
        if (s_room.generation) (void)room_send("get_room_snapshot", NULL, false);
        return;
    }
    /* A cached lobby is not authority to mutate a possibly reassigned room. */
    if (!s_room.assignment_known) {
        s_room.error = ESP_ERR_INVALID_STATE; s_room.dirty = true;
        ESP_LOGW(TAG, "ROOM intent waits for assignment op=%lu", (unsigned long)event->command);
        return;
    }
    /* Metadata reads can be superseded by a user operation. A lease/token
     * transaction is only cancelled by leaving, never by a second mutation. */
    if (s_room.pending == ROOM_SYNC || (event->command == ROOM_LEAVE &&
        (s_room.pending == ROOM_PRESENCE || s_room.pending == ROOM_TOKEN))) {
        s_room.pending = 0;
        ++s_room.ticket;
    }
    if (s_room.pending || !platform_client_ready()) {
        s_room.error = ESP_ERR_INVALID_STATE; s_room.dirty = true;
        ESP_LOGW(TAG, "ROOM intent not accepted op=%lu pending=%u", (unsigned long)event->command, s_room.pending);
        return;
    }
    cJSON *body = NULL;
    if (event->command == ROOM_LEAVE) {
        if (!s_room.assigned) return;
        if (room_owns_media()) finish_session(0);
        body = cJSON_CreateObject();
        if (!body || !cJSON_AddStringToObject(body, "room_id", s_room.room_id) ||
            !cJSON_AddNumberToObject(body, "assignment_version", s_room.version)) {
            cJSON_Delete(body); room_fail(ESP_ERR_NO_MEM); return;
        }
    } else {
        if (s_room.assigned) {
            s_room.error = 40923; s_room.dirty = true;
            ESP_LOGW(TAG, "ROOM mutation requires leaving the existing assignment");
            return;
        }
        body = event->text ? cJSON_ParseWithLength(event->text, event->length) : NULL;
    }
    esp_err_t err = room_request(event->command, body);
    if (err != ESP_OK) room_fail(err);
}

static void room_tick(int64_t current_ms)
{
    room_apply_app_request();
    bool online = platform_client_ready() && wifi_manager_connected();
    if (s_room.epoch != platform_client_epoch()) {
        if (room_owns_media()) finish_session(ESP_ERR_INVALID_STATE);
        uint32_t ticket = s_room.ticket + 1;
        memset(&s_room, 0, sizeof(s_room));
        s_room.ticket = ticket;
        s_room.epoch = platform_client_epoch();
        s_room.app_revision = atomic_load(&s_room_app_request);
        atomic_store(&s_room_app_applied, s_room.app_revision);
        s_room.dirty = true;
    }
    if (online && !s_room.online_before) {
        room_invalidate_assignment(); s_room.retries = 0; s_room.retry_due = 0;
    }
    s_room.online_before = online;
    if (!online) {
        if (room_owns_media()) room_fail(ESP_ERR_INVALID_STATE);
        room_publish(); return;
    }
    uint32_t lost = atomic_exchange(&s_room_response_lost, 0);
    if (s_room.pending && ((lost && lost == s_room.ticket) || current_ms >= s_room.pending_deadline)) {
        unsigned operation = s_room.pending;
        s_room.pending = 0; ++s_room.ticket;
        room_http_failure(operation, ESP_ERR_TIMEOUT);
    }
    if (!room_app_current()) { room_publish(); return; }
    if (atomic_load(&s_public_state) == STARTER_RUNTIME_ROOM_ACTIVE) {
#if CONFIG_IDF_TARGET_ESP32S3
        starter_media_log_room_audio();
#endif
        if (current_ms >= s_room.lease_deadline) { room_fail(ESP_ERR_TIMEOUT); room_publish(); return; }
        bool pressed = atomic_load(&s_room_ptt_intent) == s_room.generation && s_room.generation != 0 &&
                       !starter_media_status().microphone_muted;
        if (pressed != s_room.mic_announced) {
            if (!pressed) (void)starter_media_room_ptt(s_room.generation, false);
            if (room_mic(pressed)) {
                s_room.mic_announced = pressed;
                for (unsigned i = 0; i < s_room.count; ++i)
                    if (s_room.members[i].self) s_room.members[i].speaking = pressed;
                s_room.phase = pressed ? STARTER_ROOM_SPEAKING : STARTER_ROOM_LISTENING;
                s_room.dirty = true;
            } else { room_fail(ESP_FAIL); room_publish(); return; }
        }
        if (!s_room.pending && current_ms >= s_room.heartbeat_due) {
            cJSON *body = room_identity();
            if (!body || !cJSON_AddStringToObject(body, "state", "joined")) {
                cJSON_Delete(body); room_http_failure(ROOM_PRESENCE, ESP_ERR_NO_MEM);
            } else {
                esp_err_t err = room_request(ROOM_PRESENCE, body);
                if (err != ESP_OK) room_http_failure(ROOM_PRESENCE, err);
            }
        }
    }
    if (!s_room.pending && current_ms >= s_room.due) {
        esp_err_t err = room_request(ROOM_SYNC, NULL);
        if (err != ESP_OK) { s_room.due = current_ms + 5000; s_room.error = err; s_room.dirty = true; }
    } else if (!s_room.pending && !s_room.needs_sync && s_room.assigned &&
        atomic_load(&s_public_state) == STARTER_RUNTIME_WAITING && starter_tirtc_started() &&
        !s_external_connect_reserve_loaned && current_ms >= s_room.retry_due && s_room.retries < 3) {
        starter_tirtc_accept_h5(false);
        if (++s_session_generation == 0) ++s_session_generation;
        snprintf(s_room.session_id, sizeof(s_room.session_id), "%08lx%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)s_session_generation);
        s_connection_generation = 0;
        s_room.generation = 0;
        s_room.signaling_room[0] = 0;
        s_room.snapshot_ready = false;
        s_room.count = 0;
        s_room.connected_attempt = true;
        s_room.phase = STARTER_ROOM_CONNECTING;
        s_room.error = 0;
        s_room.lease_ms = 45000;
        atomic_store(&s_room_ptt_intent, 0);
        publish_state(STARTER_RUNTIME_ROOM_CONNECTING);
        s_deadline_ms = current_ms + 20000;
        esp_err_t err = room_request(ROOM_TOKEN, room_identity());
        if (err != ESP_OK) room_fail(err);
    }
    room_publish();
}

static esp_err_t room_enqueue(unsigned operation, cJSON *body)
{
    if (!room_app_open()) { cJSON_Delete(body); return ESP_ERR_INVALID_STATE; }
    char *json = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if ((operation == ROOM_CREATE || operation == ROOM_JOIN) && !json) return ESP_ERR_NO_MEM;
    runtime_event_t event = {.type = EVENT_ROOM_INTENT, .command = operation,
                            .generation = atomic_load(&s_room_app_request),
                            .platform_epoch = platform_client_epoch()};
    bool copied = !json || copy_event_text(&event, json, strlen(json));
    cJSON_free(json);
    if (!copied || !queue_event(&event)) { release_event(&event); return copied ? ESP_ERR_TIMEOUT : ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t starter_runtime_room_create(const char *password)
{
    if (!room_digits(password, 4, true)) return ESP_ERR_INVALID_ARG;
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "password", password ? password : "")) {
        cJSON_Delete(body); return ESP_ERR_NO_MEM;
    }
    return room_enqueue(ROOM_CREATE, body);
}

esp_err_t starter_runtime_room_join(const char *room_code, const char *password)
{
    if (!room_digits(room_code, 6, false) || !room_digits(password, 4, true)) return ESP_ERR_INVALID_ARG;
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "room_code", room_code) ||
        !cJSON_AddStringToObject(body, "password", password ? password : "")) {
        cJSON_Delete(body); return ESP_ERR_NO_MEM;
    }
    return room_enqueue(ROOM_JOIN, body);
}

esp_err_t starter_runtime_room_leave(void) { return room_enqueue(ROOM_LEAVE, NULL); }
esp_err_t starter_runtime_room_refresh(void) { return room_enqueue(ROOM_SYNC, NULL); }

esp_err_t starter_runtime_room_ptt(uint32_t generation, bool pressed)
{
    if (!pressed) {
        unsigned expected = generation;
        if (!generation) atomic_store(&s_room_ptt_intent, 0);
        else (void)atomic_compare_exchange_strong(&s_room_ptt_intent, &expected, 0);
        (void)starter_media_room_ptt(generation, false);
        return ESP_OK;
    }
    unsigned app_request = atomic_load(&s_room_app_request);
    if (!room_app_current() || !generation || atomic_load(&s_public_state) != STARTER_RUNTIME_ROOM_ACTIVE ||
        generation != atomic_load(&s_public_connection_generation) || starter_media_status().microphone_muted)
        return ESP_ERR_INVALID_STATE;
    if (!starter_media_room_ptt(generation, true)) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_room_ptt_intent, generation);
    if (!room_app_current() || atomic_load(&s_room_app_request) != app_request) {
        (void)starter_runtime_room_ptt(generation, false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

#include "room_json_unprefix.h"
