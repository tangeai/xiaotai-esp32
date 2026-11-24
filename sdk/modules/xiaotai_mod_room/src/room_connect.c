// SPDX-License-Identifier: MIT
#include "room_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/json.h"
#include "xiaotai/link.h"
#include "xiaotai/media.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/sys.h"
#include "xiaotai/transport.h"

#define ROOM_TOKEN_PATH "/v1/call/group/device/connect-token"
#define ROOM_PRESENCE_PATH "/v1/call/group/device/presence"
#define ROOM_COMMAND 0x2200U
#define TOKEN_MS 20000U
#define OPEN_MS 20000U
#define JOIN_MS 9000U
#define SNAPSHOT_MS 2000U
#define PRESENCE_RETRY_MS 2000U
#define PENDING_RETRY_MS 500U
#define UNKNOWN_LEASE_MS 45000U
#define CONNECTING_NOTICE_MS 6000U
#define ROOM_HTTP_MS 8000U

static int64_t earliest(int64_t a, int64_t b) { return a < b ? a : b; }
static uint32_t next_ticket(uint32_t ticket) { return ++ticket ? ticket : 1U; }

static void wipe(void *data, size_t size)
{
    volatile unsigned char *dst = data;
    while (size--) *dst++ = 0U;
}

static void reset_release(room_t *room)
{
    const uint32_t sequence = room->release.sequence;
    memset(&room->release, 0, sizeof(room->release));
    room->release.sequence = sequence;
    room->release_pending = false;
    if (room->link.stage == ROOM_LINK_NONE && !room->link.superseded_unknown) room->link.lease_until = 0;
}

void room_connection_end(room_t *room, room_end_t reason)
{
    room_connection_t *link = &room->link;
    /* A terminal admission failure can end an owned attempt before TOKEN.
     * Retirement for a deferred retry at the same stage stays silent. */
    const uint32_t activity = (link->stage != ROOM_LINK_NONE || reason == ROOM_END_FAILED) ? link->activity : 0;
    const uint32_t gen = room->view.generation;
    if (link->activity) xiaotai_activity_set_generation(link->activity, 0);
    room_talk_retire(link->media_open ? gen : 0);
    room->view.transmitting = false;
    if (link->opened && link->link_alive) (void)room_connection_send(room, "leave_room", false);
    if (link->media_open) xiaotai_media_close(gen);
    if (link->link_alive) {
        (void)xiaotai_link_permit(gen, XIAOTAI_LINK_AUDIO, false);
        xiaotai_link_disconnect(gen);
    }
    if (link->activity && reason != ROOM_END_REVOKED) xiaotai_activity_end(link->activity);

    if (reason == ROOM_END_BINDING) {
        reset_release(room);
    } else if (link->session_id[0] && !room->release_pending) {
        room_release_t *release = &room->release;
        memcpy(release->room_id, room->view.room_id, sizeof(release->room_id));
        memcpy(release->session_id, link->session_id, sizeof(release->session_id));
        release->version = room->view.assignment_version;
        release->await_token = link->token_ticket;
        release->failed = reason == ROOM_END_FAILED;
        release->left = !room->view.attending || reason == ROOM_END_LEAVE;
        release->at = room_now_ms();
        room->release_pending = true;
    }
    if (room->pending == ROOM_REQUEST_TOKEN || room->pending == ROOM_REQUEST_PRESENCE) {
        room->pending = ROOM_REQUEST_NONE;
    }
    link->stage = ROOM_LINK_NONE;
    link->activity = link->token_ticket = 0;
    link->deadline = link->snapshot_at = link->lease_until = link->heartbeat_at = 0;
    link->snapshot_not_before = link->members_until = 0;
    link->oversized = 0;
    link->opened = link->link_alive = link->media_open = false;
    link->superseded_unknown = false;
    link->session_id[0] = '\0';
    link->signal_room[0] = '\0';
    if (activity && room->online) {
        /* Leave/reassignment before a link was acquired only retires a reservation. */
        if (reason == ROOM_END_MANUAL || (gen && (reason == ROOM_END_LEAVE || reason == ROOM_END_ASSIGNMENT)))
            room_attempt_changed(activity, XIAOTAI_ROOM_ATTEMPT_ENDED);
        else if (reason == ROOM_END_FAILED)
            room_attempt_changed(activity, XIAOTAI_ROOM_ATTEMPT_FAILED);
    }
}

void room_connection_fail(room_t *room, xiaotai_err_t err, int32_t code, bool permanent)
{
    const room_link_t stage = room->link.stage;
    const bool superseded = room->config.wait_superseded_lease && code == XIAOTAI_ROOM_CODE_SUPERSEDED;
    xiaotai_eventlog_error(ROOM_LOG_SOURCE, "connection.failed", room->view.generation, err);
    if (stage == ROOM_LINK_TOKEN) ++room->stats.token_failures;
    if (stage == ROOM_LINK_OPEN) ++room->stats.connect_failures;
    if (stage == ROOM_LINK_JOIN) ++room->stats.join_failures;
    room_end_connection(room, ROOM_END_FAILED);
    room->view.error = err;
    room->view.code = code == 200 ? 0 : code;
    const int64_t now = room_now_ms();
    if (superseded) {
        /* NONE reuses the lease clock for the retiring reservation, not for
         * the normal retry delay (which assignment or Wi-Fi can reset). */
        room->link.superseded_unknown = stage < ROOM_LINK_OPEN;
        room->link.lease_until = now + (room->link.superseded_unknown ? UNKNOWN_LEASE_MS : room->link.lease_ms);
    }
    if (code == XIAOTAI_ROOM_CODE_NOT_FOUND) {
        room->link.retry_at = 0;
        room->sync_at = now;
    } else {
        if (room->view.attempts < UINT8_MAX) ++room->view.attempts;
        if (permanent || code == XIAOTAI_ROOM_CODE_UNBOUND || room->view.attempts >= room->config.retry_max) {
            room->view.phase = XIAOTAI_ROOM_BLOCKED;
            room->link.retry_at = room->sync_at = INT64_MAX;
        } else {
            static const uint16_t backoff[] = {4000U, 8000U, 16000U, 30000U};
            const size_t n = room->view.attempts;
            room->link.retry_at = now + backoff[n <= 4U ? n - 1U : 3U];
            room->sync_at = now;
        }
    }
}

static void fail(room_t *room, xiaotai_err_t err, int32_t code) { room_connection_fail(room, err, code, false); }

static bool critical(int32_t code)
{
    return code == 401 || code == XIAOTAI_ROOM_CODE_UNBOUND || code == XIAOTAI_ROOM_CODE_NOT_FOUND ||
           code == XIAOTAI_ROOM_CODE_SUPERSEDED;
}

bool room_connection_sync_failed(room_t *room, xiaotai_err_t error, int32_t code)
{
    if (room->link.stage != ROOM_LINK_ACTIVE) return false;
    if (critical(code)) fail(room, error, code);
    return true;
}

static xiaotai_err_t http(room_t *room, const char *path, const char *id, uint32_t version,
                         const char *session, const char *state, uint32_t timeout_ms, uint32_t flags, uint32_t ticket,
                         xiaotai_platform_reply_fn_t done)
{
    if (room->flights >= 2U) return XIAOTAI_ERR_NO_MEM;
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, room->body, sizeof(room->body));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "room_id", id);
    xiaotai_json_put_int(&writer, "assignment_version", version);
    xiaotai_json_put_string(&writer, "session_id", session);
    if (state) xiaotai_json_put_string(&writer, "state", state);
    xiaotai_json_object_end(&writer);
    xiaotai_err_t err = xiaotai_json_writer_end(&writer, NULL);
    if (err == XIAOTAI_OK) {
        const xiaotai_platform_request_t request = {.service = XIAOTAI_PLATFORM_SERVICE_CALL,
            .method = XIAOTAI_PLATFORM_POST, .path = path, .body = room->body, .timeout_ms = timeout_ms,
            .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .flags = flags};
        err = xiaotai_platform_request(&request, done, (void *)(uintptr_t)ticket);
        if (err == XIAOTAI_OK) ++room->flights;
    }
    wipe(room->body, sizeof(room->body));
    return err;
}

static void release_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    room_t *room = room_context();
    if (!room) return;
    if (room->flights) --room->flights;
    room_update_context(room);
    room_release_t *release = &room->release;
    if (!room->release_pending || !release->inflight || release->sequence != (uintptr_t)ctx ||
        reply->generation != room->binding) {
        ++room->stats.stale;
    } else {
        const int32_t code = room_reply_code(reply);
        release->inflight = false;
        if (reply->result == XIAOTAI_OK &&
                   (code == 200 || code == XIAOTAI_ROOM_CODE_SUPERSEDED || code == XIAOTAI_ROOM_CODE_NOT_FOUND)) {
            ++room->stats.releases;
            reset_release(room);
        } else {
            ++room->stats.release_failures;
            release->at = room_now_ms() + PRESENCE_RETRY_MS;
        }
    }
    room_drive(room);
}

void room_release_drive(room_t *room, int64_t now)
{
    room_release_t *release = &room->release;
    if (room->link.stage == ROOM_LINK_NONE && room->link.lease_until && now >= room->link.lease_until) {
        room->link.superseded_unknown = false;
        reset_release(room);
    }
    if (!room->release_pending || !room->online || xiaotai_quiesced() || room->flights >= 2U || release->inflight ||
        release->await_token || now < release->at) return;
    release->sequence = next_ticket(release->sequence);
    const char *state = release->failed ? "connect_failed" : release->left ? "left" : "suspended";
    const xiaotai_err_t err = http(room, ROOM_PRESENCE_PATH, release->room_id, release->version,
                                 release->session_id, state, ROOM_HTTP_MS, XIAOTAI_PLATFORM_URGENT,
                                 release->sequence, release_reply);
    if (err == XIAOTAI_OK) release->inflight = true;
    else {
        if (err != XIAOTAI_ERR_NO_MEM) ++room->stats.release_failures;
        release->at = now + PRESENCE_RETRY_MS;
    }
}

static xiaotai_err_t parse(const void *data, size_t size, xiaotai_json_doc_t *doc, xiaotai_json_token_t **tokens)
{
    size_t count = 0;
    if (!data || !size || xiaotai_json_parse(data, size, NULL, 0, &count, NULL) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (count > 2048U) return XIAOTAI_ERR_INVALID_SIZE;
    *tokens = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, count * sizeof(**tokens));
    if (!*tokens) return XIAOTAI_ERR_NO_MEM;
    if (xiaotai_json_parse(data, size, *tokens, count, &count, doc) != XIAOTAI_OK ||
        doc->tokens[0].type != XIAOTAI_JSON_OBJECT) return XIAOTAI_FAIL;
    return XIAOTAI_OK;
}

static bool read_int(const xiaotai_json_doc_t *doc, int obj, const char *key, int64_t *value)
{
    return xiaotai_json_int(doc, xiaotai_json_get(doc, obj, key), value) == XIAOTAI_OK;
}

static bool read_string(const xiaotai_json_doc_t *doc, int index, char *out, size_t size)
{
    return xiaotai_json_string(doc, index, out, size, NULL) == XIAOTAI_OK && out[0];
}

static void token_shape(room_t *room, const xiaotai_json_doc_t *doc, int peer, int token)
{
    size_t peer_bytes = 0, token_bytes = 0;
    (void)xiaotai_json_string(doc, peer, NULL, 0, &peer_bytes);
    (void)xiaotai_json_string(doc, token, NULL, 0, &token_bytes);
    room->token_shape.peer_type = peer < 0 ? 0U : doc->tokens[peer].type;
    room->token_shape.token_type = token < 0 ? 0U : doc->tokens[token].type;
    room->token_shape.peer_bytes = (uint32_t)peer_bytes;
    room->token_shape.token_bytes = (uint32_t)token_bytes;
    room->token_shape.valid = true;
    const uint32_t gen = room->view.generation;
    xiaotai_eventlog_warn(ROOM_LOG_SOURCE, "token.peer_type", gen, room->token_shape.peer_type);
    xiaotai_eventlog_warn(ROOM_LOG_SOURCE, "token.peer_bytes", gen, (int32_t)peer_bytes);
    xiaotai_eventlog_warn(ROOM_LOG_SOURCE, "token.type", gen, room->token_shape.token_type);
    xiaotai_eventlog_warn(ROOM_LOG_SOURCE, "token.bytes", gen, (int32_t)token_bytes);
    XIAOTAI_LOGW(ROOM_LOG_SOURCE, "token metadata rejected: peer_type=%u peer_bytes=%u token_type=%u token_bytes=%u",
                (unsigned)room->token_shape.peer_type, (unsigned)peer_bytes,
                (unsigned)room->token_shape.token_type, (unsigned)token_bytes);
}

static xiaotai_err_t admission(const room_connection_t *link, int64_t expires)
{
    if (room_now_ms() >= link->lease_until) return XIAOTAI_ERR_TIMEOUT;
    if (expires) {
        xiaotai_platform_status_t platform;
        xiaotai_platform_status(&platform);
        if (!platform.clock_synced) return XIAOTAI_ERR_INVALID_STATE;
        if (xiaotai_clock_unix_ms() / 1000 >= expires) return XIAOTAI_ERR_TIMEOUT;
    }
    return XIAOTAI_OK;
}

static void opened(void *ctx, uint32_t gen);
static void closed(void *ctx, uint32_t gen, xiaotai_link_end_t reason, int detail);

static void connecting_reply(const xiaotai_platform_reply_t *reply, void *unused)
{
    (void)reply;
    (void)unused;
    room_t *room = room_context();
    if (!room) return;
    if (room->flights) --room->flights;
    room_drive(room);
}

static xiaotai_err_t connect(room_t *room, const char *peer, const char *token, int64_t expires)
{
    room_connection_t *link = &room->link;
    static const xiaotai_link_format_t format = {.codec = XIAOTAI_LINK_CODEC_G711A, .rate_hz = 8000U, .channels = 1U};
    if (room->config.connecting_presence) {
        (void)http(room, ROOM_PRESENCE_PATH, room->view.room_id, room->view.assignment_version,
                   link->session_id, "connecting", CONNECTING_NOTICE_MS, 0, 0, connecting_reply);
        const xiaotai_err_t valid = admission(link, expires);
        if (valid != XIAOTAI_OK) return valid;
    }
    const int64_t now = room_now_ms();
    link->deadline = earliest(now + OPEN_MS, link->lease_until);
    if (now >= link->deadline) return XIAOTAI_ERR_TIMEOUT;
    const xiaotai_link_request_t req = {.kind = XIAOTAI_LINK_WHIP, .peer = peer, .token = token,
        .timeout_ms = (uint32_t)(link->deadline - now),
        .profile = {.command_max_bytes = room->config.command_max_bytes, .command_pool_bytes = room->config.command_pool_bytes,
            .audio = {.send = true, .send_stream = 1, .peer_streams = XIAOTAI_LINK_PEER_STREAM_REFUSE,
                      .send_format = format, .receive = true, .receive_stream = 1,
                      .receive_request = room->config.request_downlink, .receive_formats = {format}}}};
    const xiaotai_link_handlers_t callbacks = {.opened = opened, .closed = closed, .command = room_signal_command,
                                               .ctx = room};
    link->stage = ROOM_LINK_OPEN;
    ++room->stats.connects;
    const xiaotai_err_t err = xiaotai_link_connect(&req, &callbacks, &room->view.generation);
    link->link_alive = err == XIAOTAI_OK;
    return err;
}

static xiaotai_err_t accept_token(room_t *room, const xiaotai_platform_reply_t *reply)
{
    room_connection_t *link = &room->link;
    xiaotai_json_token_t *tokens = NULL;
    xiaotai_json_doc_t doc;
    xiaotai_err_t err = parse(reply->body, reply->size, &doc, &tokens);
    typedef struct { char peer[XIAOTAI_LINK_PEER_MAX], token[XIAOTAI_LINK_TOKEN_MAX]; } credentials_t;
    credentials_t *credentials = NULL;
    if (err != XIAOTAI_OK) goto done;
    const int data = xiaotai_json_get(&doc, 0, "data");
    const int peer_field = xiaotai_json_get(&doc, data, "peer_id");
    const int token_field = xiaotai_json_get(&doc, data, "token");
    int64_t heartbeat, lease, expires = 0;
    const int expiry = xiaotai_json_get(&doc, data, "expires_at");
    if (!read_int(&doc, data, "heartbeat_seconds", &heartbeat) || heartbeat < 1 || heartbeat > 300 ||
        !read_int(&doc, data, "lease_seconds", &lease) || lease <= 2 * heartbeat || lease > 3600 ||
        (expiry >= 0 && (xiaotai_json_int(&doc, expiry, &expires) != XIAOTAI_OK ||
                         expires < 1 || expires > INT64_C(9007199254740991)))) {
        token_shape(room, &doc, peer_field, token_field);
        err = XIAOTAI_FAIL;
        goto done;
    }
    link->heartbeat_ms = (uint32_t)heartbeat * 1000U;
    link->lease_ms = (uint32_t)lease * 1000U;
    link->lease_until = link->sent_at + link->lease_ms;
    err = admission(link, expires);
    if (err != XIAOTAI_OK) goto done;
    credentials = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*credentials));
    if (!credentials) { err = XIAOTAI_ERR_NO_MEM; goto done; }
    if (!read_string(&doc, peer_field, credentials->peer, sizeof(credentials->peer)) ||
        !read_string(&doc, token_field, credentials->token, sizeof(credentials->token))) {
        token_shape(room, &doc, peer_field, token_field);
        err = XIAOTAI_FAIL;
        goto done;
    }
    err = admission(link, expires);
    if (err == XIAOTAI_OK) err = connect(room, credentials->peer, credentials->token, expires);
done:
    if (credentials) {
        wipe(credentials, sizeof(*credentials));
        xiaotai_mem_free(credentials);
    }
    xiaotai_mem_free(tokens);
    return err;
}

static void request_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    room_t *room = room_context();
    if (!room) return;
    if (room->flights) --room->flights;
    room_update_context(room);
    const uint32_t ticket = (uint32_t)(uintptr_t)ctx;
    if (room->release_pending && room->release.await_token == ticket && reply->generation == room->binding) {
        room->release.await_token = 0;
    }
    if ((room->pending != ROOM_REQUEST_TOKEN && room->pending != ROOM_REQUEST_PRESENCE) ||
        ticket != room->ticket || reply->generation != room->binding) {
        ++room->stats.stale;
        room_drive(room);
        return;
    }
    const bool token = room->pending == ROOM_REQUEST_TOKEN;
    room_connection_t *link = &room->link;
    room->pending = ROOM_REQUEST_NONE;
    if (token) link->token_ticket = 0;
    const int64_t now = room_now_ms();
    const int32_t code = room_reply_code(reply);
    xiaotai_err_t err = reply->result;
    if (err == XIAOTAI_ERR_PLATFORM_STALE) {
        /* A platform session can end without changing the device identity.
         * Only a different binding invalidates a reservation's old tuple. */
        room_end_connection(room, ROOM_END_OFFLINE);
        room_assignment_reset(room);
        room_read_again(room);
        ++room->stats.stale;
    } else if (token) {
        if (err == XIAOTAI_OK) err = code != 200 ? XIAOTAI_FAIL : now >= link->deadline ? XIAOTAI_ERR_TIMEOUT :
                                        accept_token(room, reply);
        if (err != XIAOTAI_OK) fail(room, err, code);
    } else if (now >= link->lease_until) {
        ++room->stats.lease_expired;
        fail(room, XIAOTAI_ERR_TIMEOUT, 0);
    } else if (err == XIAOTAI_OK && code == 200) {
        link->lease_until = link->sent_at + link->lease_ms;
        link->heartbeat_at = link->sent_at + link->heartbeat_ms;
    } else {
        ++room->stats.heartbeat_failures;
        if (critical(code)) fail(room, err ? err : XIAOTAI_FAIL, code);
        else link->heartbeat_at = now + PRESENCE_RETRY_MS;
    }
    room_drive(room);
}

static xiaotai_err_t begin_request(room_t *room, bool token, int64_t now)
{
    room_connection_t *link = &room->link;
    const uint32_t ticket = next_ticket(room->ticket);
    const xiaotai_err_t err = http(room, token ? ROOM_TOKEN_PATH : ROOM_PRESENCE_PATH,
        room->view.room_id, room->view.assignment_version, link->session_id, token ? NULL : "joined",
        ROOM_HTTP_MS, token ? XIAOTAI_PLATFORM_RELEASE : 0, ticket, request_reply);
    if (err != XIAOTAI_OK) return err;
    room->ticket = ticket;
    room->pending = token ? ROOM_REQUEST_TOKEN : ROOM_REQUEST_PRESENCE;
    link->sent_at = now;
    if (token) {
        ++room->stats.tokens;
        link->token_ticket = ticket;
        link->stage = ROOM_LINK_TOKEN;
        link->deadline = now + TOKEN_MS;
        room->view.phase = XIAOTAI_ROOM_CONNECTING;
        room->view.wait = XIAOTAI_ROOM_WAIT_NONE;
    } else ++room->stats.heartbeats;
    return XIAOTAI_OK;
}

static void write_audio(xiaotai_json_writer_t *writer, const char *name)
{
    xiaotai_json_object_begin(writer, name);
    xiaotai_json_put_string(writer, "codec", "g711a");
    xiaotai_json_put_int(writer, "sample_rate", 8000);
    xiaotai_json_put_int(writer, "channels", 1);
    xiaotai_json_object_end(writer);
}

xiaotai_err_t room_connection_send(room_t *room, const char *method, bool speaking)
{
    room_connection_t *link = &room->link;
    xiaotai_json_writer_t writer;
    size_t size = 0;
    xiaotai_json_writer_init(&writer, link->command, sizeof(link->command));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "jsonrpc", "2.0");
    xiaotai_json_put_string(&writer, "method", method);
    if (!strcmp(method, "join_room")) {
        char device[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U];
        const xiaotai_err_t err = xiaotai_identity_device_id(device, sizeof(device));
        if (err != XIAOTAI_OK || !device[0]) return err != XIAOTAI_OK ? err : XIAOTAI_ERR_INVALID_STATE;
        xiaotai_json_put_int(&writer, "id", room->view.generation);
        xiaotai_json_object_begin(&writer, "params");
        xiaotai_json_put_string(&writer, "room_id", room->view.room_id);
        xiaotai_json_put_string(&writer, "device_id", device);
        write_audio(&writer, "input_audio");
        write_audio(&writer, "output_audio");
        xiaotai_json_object_end(&writer);
    } else if (!strcmp(method, "set_mic_state")) {
        xiaotai_json_object_begin(&writer, "params");
        xiaotai_json_put_string(&writer, "mic_state", speaking ? "speaking" : "off");
        xiaotai_json_object_end(&writer);
    }
    xiaotai_json_object_end(&writer);
    xiaotai_err_t err = xiaotai_json_writer_end(&writer, &size);
    if (err == XIAOTAI_OK) err = xiaotai_link_command(room->view.generation, ROOM_COMMAND, link->command, size);
    if (err == XIAOTAI_OK) ++room->stats.signals_sent;
    else ++room->stats.signals_failed;
    return err;
}

static void opened(void *ctx, uint32_t gen)
{
    room_t *room = ctx;
    room_update_context(room);
    room_connection_t *link = &room->link;
    if (gen != room->view.generation || link->stage != ROOM_LINK_OPEN) { ++room->stats.stale; room_drive(room); return; }
    link->opened = true;
    const int64_t now = room_now_ms();
    if (now >= link->deadline) fail(room, XIAOTAI_ERR_TIMEOUT, 0);
    else {
        link->stage = ROOM_LINK_JOIN;
        ++room->stats.joins;
        const xiaotai_err_t err = room_connection_send(room, "join_room", false);
        if (err == XIAOTAI_OK) link->deadline = earliest(now + JOIN_MS, link->lease_until);
        else fail(room, err, 0);
    }
    room_drive(room);
}

static void closed(void *ctx, uint32_t gen, xiaotai_link_end_t reason, int detail)
{
    room_t *room = ctx;
    if (gen != room->view.generation || !room->link.link_alive) return;
    room->link.opened = room->link.link_alive = false;
    const xiaotai_err_t err = reason == XIAOTAI_LINK_END_TIMEOUT ? XIAOTAI_ERR_TIMEOUT :
                              reason == XIAOTAI_LINK_END_NO_MEMORY ? XIAOTAI_ERR_NO_MEM :
                              reason == XIAOTAI_LINK_END_PEER_FAULT ? detail : XIAOTAI_FAIL;
    fail(room, err, 0);
    room_drive(room);
}

static bool correct_format(const xiaotai_json_doc_t *doc, int field, bool accept_alaw)
{
    int64_t rate, channels;
    const int codec = xiaotai_json_get(doc, field, "codec");
    return (xiaotai_json_equals(doc, codec, "g711a") || (accept_alaw && xiaotai_json_equals(doc, codec, "alaw"))) &&
           read_int(doc, field, "sample_rate", &rate) && rate == 8000 &&
           read_int(doc, field, "channels", &channels) && channels == 1;
}

xiaotai_err_t room_connection_accept_join(room_t *room, const xiaotai_json_doc_t *doc)
{
    const int result = xiaotai_json_get(doc, 0, "result");
    const int session = xiaotai_json_get(doc, result, "session_id");
    const bool accept_alaw = room->config.accept_alaw_join_reply;
    if (xiaotai_json_get(doc, 0, "error") >= 0 || session < 0 ||
        doc->tokens[session].type != XIAOTAI_JSON_STRING || doc->tokens[session].start == doc->tokens[session].end ||
        !correct_format(doc, xiaotai_json_get(doc, result, "input_audio"), accept_alaw) ||
        !correct_format(doc, xiaotai_json_get(doc, result, "output_audio"), accept_alaw)) return XIAOTAI_FAIL;
    const int64_t now = room_now_ms();
    room_connection_t *link = &room->link;
    if (now >= link->deadline) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t err = room_connection_send(room, "set_mic_state", false);
    if (err != XIAOTAI_OK) return err;
    const uint32_t gen = room->view.generation;
    const xiaotai_media_session_t media = {.generation = gen,
        .uplink = XIAOTAI_MEDIA_FORMAT_G711A_8K, .downlink = XIAOTAI_MEDIA_FORMAT_G711A_8K,
        .downlink_burst_ms = 200U, .duplex = false, .latency = XIAOTAI_MEDIA_LATENCY_RELAYED,
        .sink = xiaotai_link_audio_sink()};
    err = xiaotai_media_open(&media);
    if (err != XIAOTAI_OK) return err;
    link->media_open = true;
    err = xiaotai_link_receive(gen, XIAOTAI_LINK_AUDIO, NULL, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_link_permit(gen, XIAOTAI_LINK_AUDIO, true);
    if (err != XIAOTAI_OK) return err;
    if (room_now_ms() >= link->deadline) return XIAOTAI_ERR_TIMEOUT;
    link->stage = ROOM_LINK_ACTIVE;
    link->deadline = link->retry_at = 0;
    link->heartbeat_at = now;
    link->snapshot_at = now + SNAPSHOT_MS;
    room->view.phase = XIAOTAI_ROOM_LISTENING;
    room->view.attempts = 0;
    room->view.error = XIAOTAI_OK;
    room->view.code = 0;
    xiaotai_activity_set_generation(link->activity, gen);
    room_admit_talk(room);
    return XIAOTAI_OK;
}

static void revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    room_t *room = ctx;
    if (id != room->link.activity) return;
    if (reason == XIAOTAI_REVOKE_PREEMPTED || reason == XIAOTAI_REVOKE_SYSTEM) {
        ++room->stats.preempted;
        room->link.parked = reason == XIAOTAI_REVOKE_PREEMPTED;
    }
    room_end_connection(room, ROOM_END_REVOKED);
    /* A preemptor is not installed until this callback returns. The queued
     * activity/quiesce event drives again with the new context visible. */
}

static void begin(room_t *room, int64_t now)
{
    room_connection_t *link = &room->link;
    const xiaotai_activity_t activity = {.name = "room", .ctx = room, .revoked = revoked,
        .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT | XIAOTAI_RESOURCE_LINK};
    const xiaotai_err_t claimed = xiaotai_activity_begin(&activity, &link->activity);
    if (claimed != XIAOTAI_OK) {
        if (!link->parked) ++room->stats.activity_busy;
        link->parked = true;
        room->view.wait = XIAOTAI_ROOM_WAIT_ACTIVITY;
        return;
    }
    link->parked = false;
    for (size_t i = 0; i < 4U; ++i) {
        (void)snprintf(link->session_id + 8U * i, 9U, "%08" PRIx32, xiaotai_random());
    }
    const xiaotai_err_t err = begin_request(room, true, now);
    if (err == XIAOTAI_OK) {
        link->no_mem_reported = false;
        room_attempt_changed(link->activity, XIAOTAI_ROOM_ATTEMPT_STARTED);
        return;
    }
    /* A locally refused HTTP request could not create a server reservation. */
    link->session_id[0] = '\0';
    if (err == XIAOTAI_ERR_NO_MEM || err == XIAOTAI_ERR_PLATFORM_GATED || err == XIAOTAI_ERR_PLATFORM_OFFLINE) {
        const bool memory_defer = err == XIAOTAI_ERR_NO_MEM && room->config.notify_deferred_no_mem;
        const uint32_t notify = memory_defer && !link->no_mem_reported && room->online ? link->activity : 0;
        room_end_connection(room, ROOM_END_MANUAL);
        link->no_mem_reported = memory_defer;
        link->retry_at = now + PENDING_RETRY_MS;
        if (notify) room_attempt_changed(notify, XIAOTAI_ROOM_ATTEMPT_FAILED);
    } else {
        ++room->stats.token_failures;
        fail(room, err, 0);
    }
}

void room_connection_drive(room_t *room, int64_t now)
{
    room_connection_t *link = &room->link;
    if (xiaotai_quiesced()) return;
    xiaotai_transport_status_t transport;
    xiaotai_transport_status(&transport);
    link->transport_online = transport.state == XIAOTAI_TRANSPORT_ONLINE && transport.connect_ready;
    if (link->stage != ROOM_LINK_NONE) {
        if (transport.state != XIAOTAI_TRANSPORT_ONLINE) fail(room, XIAOTAI_FAIL, 0);
        else if (link->stage != ROOM_LINK_ACTIVE && now >= link->deadline) {
            if (link->lease_until && now >= link->lease_until) ++room->stats.lease_expired;
            fail(room, XIAOTAI_ERR_TIMEOUT, 0);
        } else if (link->stage == ROOM_LINK_ACTIVE && now >= link->lease_until) {
            ++room->stats.lease_expired;
            fail(room, XIAOTAI_ERR_TIMEOUT, 0);
        } else if (link->stage == ROOM_LINK_ACTIVE) {
            room_talk_drive(room, now);
            if (link->stage != ROOM_LINK_ACTIVE) return;
            room_snapshot_drive(room, now);
            if (room->pending == ROOM_REQUEST_NONE && now >= link->heartbeat_at) {
                const xiaotai_err_t err = begin_request(room, false, now);
                if (err != XIAOTAI_OK) {
                    ++room->stats.heartbeat_failures;
                    link->heartbeat_at = now + PRESENCE_RETRY_MS;
                }
            }
        }
        return;
    }
    if (room->view.phase != XIAOTAI_ROOM_WAITING || !room->view.assigned || !room->view.assignment_known) return;
    if (room->view.wait == XIAOTAI_ROOM_WAIT_NOT_ATTENDING || room->view.wait == XIAOTAI_ROOM_WAIT_MANUAL) return;
    if (!room->online || !link->transport_online) { room->view.wait = XIAOTAI_ROOM_WAIT_OFFLINE; return; }
    /* A release ACK for the new token request cannot clear an unknown old id. */
    if (room->release_pending || link->superseded_unknown) { room->view.wait = XIAOTAI_ROOM_WAIT_RELEASE; return; }
    if (link->parked && xiaotai_activity_current(NULL)) { room->view.wait = XIAOTAI_ROOM_WAIT_ACTIVITY; return; }
    if (now < link->retry_at) { room->view.wait = XIAOTAI_ROOM_WAIT_RETRY; return; }
    if (room->pending == ROOM_REQUEST_NONE && !room->mutation_queued && !room->confirming && room->flights < 2U &&
        now < room->sync_at) begin(room, now);
}

int64_t room_connection_due(const room_t *room)
{
    if (xiaotai_quiesced()) return INT64_MAX;
    const room_connection_t *link = &room->link;
    const room_release_t *release = &room->release;
    int64_t due = INT64_MAX;
    if (room->online && room->flights < 2U && room->release_pending && !release->await_token && !release->inflight) {
        due = release->at;
    }
    if (link->stage == ROOM_LINK_NONE && link->lease_until)
        due = earliest(due, link->lease_until);
    if (link->stage == ROOM_LINK_ACTIVE) {
        due = earliest(due, link->lease_until);
        if (room->pending == ROOM_REQUEST_NONE) due = earliest(due, link->heartbeat_at);
        due = earliest(due, room_snapshot_due(room));
        due = earliest(due, room_talk_due(room, room_now_ms()));
    } else if (link->stage != ROOM_LINK_NONE) due = earliest(due, link->deadline);
    else if (room->view.phase == XIAOTAI_ROOM_WAITING && room->view.wait == XIAOTAI_ROOM_WAIT_RETRY &&
             link->retry_at > room_now_ms()) due = earliest(due, link->retry_at);
    return due;
}

static void wake(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    room_t *room = room_context();
    if (room) room_drive(room);
}

static xiaotai_err_t guard(const char *reason, void *ctx)
{
    (void)ctx;
    if (xiaotai_room_settled()) return XIAOTAI_OK;
    const room_t *room = room_context();
    if (!strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) ||
        (!strcmp(reason, XIAOTAI_QUIESCE_PROVISIONING) && room->config.settle_before_provisioning) ||
        (!strcmp(reason, XIAOTAI_QUIESCE_BINDING) && room->config.settle_before_binding)) return XIAOTAI_ERR_BUSY;
    return XIAOTAI_OK;
}

xiaotai_err_t room_connection_init(void)
{
    xiaotai_err_t err = xiaotai_quiesce_guard_register("room", guard, NULL);
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_ACTIVITY, wake, NULL},
        {XIAOTAI_EVENT_TRANSPORT_STATE, wake, NULL},
        {XIAOTAI_EVENT_TRANSPORT_LINKS, wake, NULL},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(events[0]); ++i) {
        err = xiaotai_event_subscribe_static(&events[i]);
    }
    return err;
}
