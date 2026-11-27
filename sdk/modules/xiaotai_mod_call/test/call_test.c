// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_call_room_recovery.py
// Replaces legacy: tests/test_at_diagnostics.py
// Replaces legacy: tests/native/cores3/test_uplink_gate.py
// Replaces legacy: tests/regression/test_device_profile_response.py
// Replaces legacy: tests/regression/test_call_controls.py
// Replaces legacy: tests/regression/test_call_signals.py
// Replaces legacy: tests/regression/test_call_hangup.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_call_signal_room_match
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/xmini/xmini_business/test_business.c::test_device_release_reconciliation
#include "call_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define BINDING 42U

static struct {
    bool online, binding_valid, fresh, contribute, unsupported_video, muted, refuse_background;
    bool require_cancel_block;
    bool dial_contact_reordered, dial_contact_offline, dial_contact_replaced, dial_contact_duplicate;
    bool dial_change_on_lookup, dial_change_on_submit;
    bool platform_held, reconnecting, media_open;
    uint32_t watched_link, pcm_written, tx, rx;
    unsigned hold_calls, resume_calls, hangup_posts;
    xiaotai_platform_done_fn_t hold_callback;
    void *hold_context;
    xiaotai_link_release_state_t retirement;
    xiaotai_err_t watch_error;
    uint32_t attachment_link, link, inbound_listener, contact_flags, binding, other_activity;
    xiaotai_platform_reply_fn_t reply;
    void *reply_ctx;
    xiaotai_platform_reply_fn_t release_reply;
    void *release_ctx;
    xiaotai_platform_signal_fn_t signal;
    void *signal_ctx;
    xiaotai_platform_profile_fn_t profile_writer;
    void *profile_ctx;
    xiaotai_media_requirement_t requirement;
    xiaotai_media_session_t media;
    xiaotai_link_listener_t listener[2];
    xiaotai_link_profile_t connection_profile;
    xiaotai_link_listen_t mode[2];
    xiaotai_link_handlers_t connected;
    unsigned listener_count, listener_attempts, requests, signals, bg_refused;
    unsigned connect_attempts, urgent_attempts;
    uint16_t inbound_blocks;
    unsigned blocked_ingress;
    unsigned mic_applies, mic_setting_changes, call_states;
    unsigned guarded_dials;
    char body[512], path[40], trace[512];
    uint32_t flags, timeout;
    size_t long_token_size;
    xiaotai_platform_method_t method;
    xiaotai_err_t command_error, hangup_error, media_error, submit_error, connect_error;
    uint32_t message_word, messages_in, message_session;
    size_t message_size;
    uint8_t message[XIAOTAI_CALL_MESSAGE_MAX], message_type;
    char message_text[XIAOTAI_CALL_MESSAGE_MAX + 1];
} s;
static bool test_duplicate_video, test_scene, test_scene_fail, test_shared_at, other_call;
static unsigned foreign_actions;
static _Atomic int64_t usec = 1000;
static xiaotai_signal_t fenced;
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&usec); }

static void trace(char ch)
{
    const size_t i = strlen(s.trace);
    assert(i + 1 < sizeof(s.trace));
    s.trace[i] = ch;
    s.trace[i + 1] = '\0';
}

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *r)
{
    assert(r->request_body_bytes == 512 && r->response_bytes == 2048 && r->signal_bytes == 1024 &&
           r->concurrent_requests == 3);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_require_service(const char *service)
{
    assert(!strcmp(service, "call"));
    return XIAOTAI_OK;
}

void xiaotai_platform_status(xiaotai_platform_status_t *out)
{
    const bool ready = s.online && !s.platform_held && !s.reconnecting;
    *out = (xiaotai_platform_status_t){.phase = ready ? XIAOTAI_PLATFORM_ONLINE :
        s.online ? XIAOTAI_PLATFORM_PAUSED : XIAOTAI_PLATFORM_STOPPED,
        .generation = s.binding, .authorized = s.online, .signaling = ready, .held = s.platform_held};
}

xiaotai_err_t xiaotai_platform_signaling_hold_bounded(uint32_t max_ms, xiaotai_platform_done_fn_t callback,
                                                      void *ctx, uint32_t *hold)
{
    assert(max_ms == 60000U && callback && ctx && hold && !s.platform_held && !s.reconnecting && s.watched_link);
    s.platform_held = true;
    ++s.hold_calls;
    s.hold_callback = callback;
    s.hold_context = ctx;
    *hold = s.hold_calls;
    return XIAOTAI_OK;
}

bool xiaotai_platform_signaling_bounded_hold_active(uint32_t hold, const void *ctx)
{
    return hold == s.hold_calls && ctx == s.hold_context && s.platform_held;
}

void xiaotai_platform_signaling_release_bounded(uint32_t hold, const void *ctx)
{
    if (!xiaotai_platform_signaling_bounded_hold_active(hold, ctx)) return;
    s.platform_held = false;
    s.reconnecting = true;
    ++s.resume_calls;
}

xiaotai_err_t xiaotai_platform_signal_subscribe(const char *type, const char *channel,
                                               xiaotai_platform_signal_fn_t fn, void *ctx)
{
    assert(type && !channel && (!s.signal || s.signal == fn));
    assert(s.signals++ < 5);
    s.signal = fn;
    s.signal_ctx = ctx;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_profile_scene(const char *scene, xiaotai_platform_profile_fn_t write, void *ctx)
{
    assert(!strcmp(scene, "call") && !s.profile_writer && write && !s.listener_attempts);
    s.profile_writer = write;
    s.profile_ctx = ctx;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *r,
                                      xiaotai_platform_reply_fn_t reply, void *ctx)
{
    assert(!strcmp(r->service, "call") && r->auth == XIAOTAI_PLATFORM_AUTH_SESSION);
    assert(!s.platform_held && !s.reconnecting);
    if (r->method == XIAOTAI_PLATFORM_GET) assert(!r->body && !strcmp(r->path, "/v1/call/room"));
    else assert(r->method == XIAOTAI_PLATFORM_POST && r->body);
    assert(reply);
    if (s.require_cancel_block && !strcmp(r->path, "/v1/call/cancel")) assert(s.inbound_blocks);
    if (s.refuse_background && (r->flags & XIAOTAI_PLATFORM_BACKGROUND) && xiaotai_activity_current(NULL)) {
        ++s.bg_refused;
        return XIAOTAI_ERR_BUSY;
    }
    if (r->flags & XIAOTAI_PLATFORM_URGENT) ++s.urgent_attempts;
    if (s.submit_error) return s.submit_error;
    if (r->flags & XIAOTAI_PLATFORM_URGENT) {
        s.release_reply = reply;
        s.release_ctx = ctx;
    } else {
        assert(!s.reply);
        s.reply = reply;
        s.reply_ctx = ctx;
    }
    ++s.requests;
    if (!strcmp(r->path, "/v1/call/hangup")) ++s.hangup_posts;
    snprintf(s.path, sizeof(s.path), "%s", r->path);
    snprintf(s.body, sizeof(s.body), "%s", r->body ? r->body : "");
    s.method = r->method;
    s.flags = r->flags;
    s.timeout = r->timeout_ms;
    trace('H');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_request_for_generation(const xiaotai_platform_request_t *r, uint32_t generation,
                                                      xiaotai_platform_reply_fn_t reply, void *ctx)
{
    assert(!strcmp(r->path, "/v1/call/request") && generation);
    assert(xiaotai_intent_from_external() == s.dial_contact_reordered);
    ++s.guarded_dials;
    if (s.dial_change_on_submit) ++s.binding;
    if (generation != s.binding) return XIAOTAI_ERR_PLATFORM_STALE;
    return xiaotai_platform_request(r, reply, ctx);
}

xiaotai_transport_state_t xiaotai_transport_state(void)
{
    return s.online ? XIAOTAI_TRANSPORT_ONLINE : XIAOTAI_TRANSPORT_DOWN;
}

xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *r)
{
    assert(r->links == 1 && r->command_pool_bytes == CALL_COMMAND_SHARE);
    return XIAOTAI_OK;
}

static void assert_profile(const xiaotai_link_profile_t *p)
{
    assert(p->audio.send && p->audio.receive && p->audio.receive_request && p->audio.send_stream == 10 &&
           p->audio.receive_stream == 10 && p->audio.peer_streams == XIAOTAI_LINK_PEER_STREAM_REFUSE);
    assert(p->audio.send_format.codec == XIAOTAI_LINK_CODEC_G711A && p->audio.send_format.rate_hz == 8000 &&
           p->audio.send_format.channels == 1 && p->audio.receive_formats[0].codec == XIAOTAI_LINK_CODEC_G711A);
    assert(p->command_max_bytes == 512 && p->command_pool_bytes == 1024);
    assert(p->track_peer_retirement == call_config()->pause_platform_signaling_while_active);
}

xiaotai_err_t xiaotai_link_listen(const xiaotai_link_listener_t *l, uint32_t *id)
{
    assert(s.profile_writer && xiaotai_on_owner());
    ++s.listener_attempts;
    assert_profile(&l->profile);
    if (l->profile.video.send && s.unsupported_video) return XIAOTAI_ERR_NOT_SUPPORTED;
    assert(s.listener_count < 2);
    s.listener[s.listener_count++] = *l;
    *id = s.listener_count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_listen_set(uint32_t id, xiaotai_link_listen_t mode)
{
    assert(id && id <= s.listener_count);
    // In the real transport OFF would retire any open link of this listener.
    assert(mode != XIAOTAI_LINK_LISTEN_OFF || !s.link || s.inbound_listener != id);
    s.mode[id - 1] = mode;
    trace(mode == XIAOTAI_LINK_LISTEN_ONCE ? 'A' : 'F');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_listener_block_inbound(uint32_t id, bool blocked)
{
    assert(id && id <= s.listener_count);
    const uint16_t mask = (uint16_t)(1U << (id - 1U));
    if (blocked) s.inbound_blocks |= mask;
    else s.inbound_blocks &= (uint16_t)~mask;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_connect(const xiaotai_link_request_t *r, const xiaotai_link_handlers_t *h, uint32_t *id)
{
    assert(r->kind == XIAOTAI_LINK_P2P && !strcmp(r->peer, "peer"));
    if (s.long_token_size) {
        assert(strlen(r->token) == s.long_token_size);
        for (size_t i = 0; i < s.long_token_size; ++i) assert(r->token[i] == 't');
    } else assert(!strcmp(r->token, "fixture-token"));
    assert_profile(&r->profile);
    s.connection_profile = r->profile;
    assert(r->timeout_ms == 30000);
    ++s.connect_attempts;
    if (s.connect_error != XIAOTAI_OK) return s.connect_error;
    s.connected = *h;
    *id = s.link = xiaotai_generation_next();
    trace('L');
    return XIAOTAI_OK;
}

void xiaotai_link_disconnect(uint32_t id) { assert(id == s.link); trace('D'); s.link = 0; }

bool xiaotai_link_stats(uint32_t id, xiaotai_link_stats_t *out)
{
    if (s.link != id) return false;
    *out = (xiaotai_link_stats_t){.state = XIAOTAI_LINK_STATE_OPEN, .audio = {.sent = s.tx, .received = s.rx}};
    return true;
}

xiaotai_err_t xiaotai_link_watch_release(uint32_t id)
{
    assert(id == s.link && !s.watched_link);
    if (s.watch_error != XIAOTAI_OK) return s.watch_error;
    s.watched_link = id;
    s.retirement = XIAOTAI_LINK_RELEASE_PENDING;
    return XIAOTAI_OK;
}

xiaotai_link_release_state_t xiaotai_link_release_state(uint32_t id)
{
    assert(id == s.watched_link);
    return s.retirement;
}

void xiaotai_link_unwatch_release(uint32_t id)
{
    assert(id == s.watched_link);
    s.watched_link = 0;
}

xiaotai_err_t xiaotai_link_command(uint32_t id, uint32_t word, const void *data, size_t size)
{
    assert(id == s.link);
    if (word == CALL_COMMAND_ANSWER) {
        assert(size == strlen("{\"room_id\":\"d_room\"}") && !memcmp(data, "{\"room_id\":\"d_room\"}", size));
        trace('B');
        return s.command_error;
    }
    if (word >= XIAOTAI_CALL_MESSAGE_WORD(0) && word < XIAOTAI_CALL_MESSAGE_WORD(XIAOTAI_CALL_MESSAGE_TYPES)) {
        s.message_word = word;
        s.message_size = size;
        if (size) memcpy(s.message, data, size);
        return s.command_error;
    }
    assert(word == CALL_COMMAND_END && !data && !size);
    trace('E');
    return s.hangup_error;
}

xiaotai_err_t xiaotai_link_receive(uint32_t id, xiaotai_link_media_kind_t kind,
                                   xiaotai_link_receive_fn_t fn, void *ctx)
{
    assert(id == s.link && kind == XIAOTAI_LINK_AUDIO && !fn && !ctx);
    trace('R');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_permit(uint32_t id, xiaotai_link_media_kind_t kind, bool allow)
{
    assert(id == s.link && kind == XIAOTAI_LINK_AUDIO && allow);
    trace('P');
    return XIAOTAI_OK;
}

const xiaotai_media_sink_t *xiaotai_link_audio_sink(void)
{
    static const xiaotai_media_sink_t sink = {0};
    return &sink;
}

xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *r)
{
    assert(r->uplink.encoding == XIAOTAI_CODEC_G711A && r->downlink.encoding == XIAOTAI_CODEC_G711A &&
           r->downlink_burst_ms == 200 && r->packet_ms == 20);
    s.requirement = *r;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *m)
{
    assert(m->generation == s.link && m->duplex && m->uplink.rate_hz == 8000 && m->downlink.channels == 1 &&
           m->sink == xiaotai_link_audio_sink() && m->downlink_burst_ms == s.requirement.downlink_burst_ms &&
           m->latency.base_ms == s.requirement.latency.base_ms);
    s.media = *m;
    s.media_open = s.media_error == XIAOTAI_OK;
    trace('M');
    return s.media_error;
}

void xiaotai_media_close(uint32_t id)
{
    assert(id == s.link && id == s.media.generation);
    s.media_open = false;
    trace('C');
}

void xiaotai_media_status(xiaotai_media_status_t *out)
{
    *out = (xiaotai_media_status_t){.generation = s.media.generation, .active = s.media_open};
}

void xiaotai_media_stats(xiaotai_media_stats_t *out)
{
    *out = (xiaotai_media_stats_t){.pcm_written = s.pcm_written};
}
xiaotai_err_t xiaotai_media_set_mic_muted(bool muted)
{
    ++s.mic_applies;
    s.muted = muted;
    trace('U');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_kind_register(const xiaotai_contact_kind_def_t *d, int *kind)
{
    assert(!strcmp(d->name, "device") && !strcmp(d->alias, "DEV") && !strcmp(d->server_type, "device"));
    assert(d->presence && d->friendships && !d->extra_fields[0] && !d->extra_fields[1] && !d->extra_required);
    *kind = 3;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_find(const char *id, int kind, uint32_t flags, xiaotai_contact_t *out)
{
    assert(kind == 3);
    s.contact_flags = flags;
    if (!s.binding_valid || (!s.fresh && !(flags & XIAOTAI_CONTACTS_STALE_OK))) {
        return XIAOTAI_ERR_NOT_FOUND;
    }
    if (s.dial_contact_reordered && !strcmp(id, "other")) {
        *out = (xiaotai_contact_t){.kind = 3, .id = "other", .name = "After refresh"};
        return XIAOTAI_OK;
    }
    if (s.dial_contact_replaced || strcmp(id, "peer")) return XIAOTAI_ERR_NOT_FOUND;
    *out = (xiaotai_contact_t){.kind = 3, .id = "peer", .name = "Friendly"};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_find_unique_online_prefix(const char *id, int kind, uint32_t flags, xiaotai_contact_t *out)
{
    if (s.dial_change_on_lookup) {
        s.dial_change_on_lookup = false;
        ++s.binding;
    }
    if (s.dial_contact_duplicate || !strcmp(id, "duplicate")) return XIAOTAI_ERR_INVALID_STATE;
    if (s.dial_contact_offline) return XIAOTAI_ERR_NOT_FOUND;
    return xiaotai_contacts_find(id, kind, flags, out);
}

xiaotai_err_t xiaotai_contacts_at(uint8_t index, uint32_t flags, xiaotai_contact_t *out)
{
    if (s.dial_contact_reordered && index == 0) {
        *out = (xiaotai_contact_t){.kind = 3, .id = "other", .name = "After reorder"};
        return XIAOTAI_OK;
    }
    if (index > 1 || xiaotai_contacts_find("peer", 3, flags, out) != XIAOTAI_OK) return XIAOTAI_ERR_NOT_FOUND;
    if (index == 1) out->kind = 4;
    return XIAOTAI_OK;
}

static xiaotai_call_status_t status(void)
{
    xiaotai_call_status_t out;
    xiaotai_call_status(&out);
    return out;
}

static void signal_at(const char *type, const char *channel, const char *json, uint32_t binding)
{
    assert(s.signals == 5);
    const xiaotai_platform_signal_t event = {
        .type = type, .channel = channel, .json = json, .size = strlen(json), .generation = binding,
    };
    s.signal(&event, s.signal_ctx);
}

static void incoming_room(const char *room)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"payload\":{\"room_id\":\"%s\",\"caller_id\":\"peer\"}}", room);
    signal_at("call_incoming", "", body, BINDING);
}

static void incoming(void) { incoming_room("d_room"); }

typedef struct { xiaotai_platform_reply_fn_t callback; void *ctx; } http_callback_t;

static http_callback_t take_reply(bool urgent)
{
    const http_callback_t call = urgent ? (http_callback_t){s.release_reply, s.release_ctx} :
                                        (http_callback_t){s.reply, s.reply_ctx};
    assert(call.callback);
    if (urgent) s.release_reply = NULL;
    else s.reply = NULL;
    return call;
}

static void deliver(http_callback_t call, uint32_t binding, xiaotai_err_t result, int http, int code, const char *json)
{
    const xiaotai_platform_reply_t r = {
        .result = result, .http_status = http, .code = code,
        .body = result == XIAOTAI_OK ? json : NULL,
        .size = result == XIAOTAI_OK && json ? strlen(json) : 0, .generation = binding,
    };
    call.callback(&r, call.ctx);
}

static void reply(int http, int code, const char *json)
{
    deliver(take_reply(false), BINDING, XIAOTAI_OK, http, code, json);
}

static const char success_room[] = "{\"code\":200,\"data\":{\"room_id\":\"d_room\"}}";
static const char success_token[] = "{\"code\":200,\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}}";
static void room_reply(void) { reply(200, 200, success_room); }
static void token_reply(void) { reply(200, 200, success_token); }

static void release_is(const char *verb, const char *value)
{
    char path[40];
    snprintf(path, sizeof(path), "/v1/call/%s", verb);
    assert(!strcmp(path, s.path) && s.flags == XIAOTAI_PLATFORM_URGENT && s.timeout == 4000);
    assert(strstr(s.body, "\"room_id\":"));
    assert(value ? strstr(s.body, value) != NULL : strstr(s.body, "\"reason\"") == NULL);
}

static void start_outgoing(void)
{
    uint32_t session;
    assert(xiaotai_call_dial("peer", 0, &session) == XIAOTAI_OK && session && session == status().session);
    assert(!strcmp(s.path, "/v1/call/request") && !strcmp(s.body, "{\"targets\":[\"peer\"],\"call_type\":\"audio\"}"));
    assert(s.timeout == 28000 && !s.flags && s.mode[0] == XIAOTAI_LINK_LISTEN_ONCE && strstr(s.trace, "AH"));
}

static void open_inbound(void)
{
    if (s.inbound_blocks) {
        ++s.blocked_ingress;
        return;
    }
    assert(s.mode[0] == XIAOTAI_LINK_LISTEN_ONCE);
    s.mode[0] = XIAOTAI_LINK_LISTEN_OFF;
    s.link = xiaotai_generation_next();
    s.inbound_listener = 1;
    s.listener[0].handlers.opened(s.listener[0].handlers.ctx, s.link);
}

static void command(uint32_t link, uint32_t word, const char *text)
{
    const xiaotai_link_command_t input = {
        .word = word, .data = (const uint8_t *)text, .size = text ? strlen(text) : 0,
    };
    s.listener[0].handlers.command(s.listener[0].handlers.ctx, link, &input);
}

static void active_outgoing(void)
{
    start_outgoing();
    room_reply();
    open_inbound();
    assert(status().state == XIAOTAI_CALL_OUTGOING && !s.attachment_link && !strchr(s.trace, 'M'));
    command(s.link, CALL_COMMAND_ANSWER, "{\"room_id\":\"d_room\"}");
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.attachment_link == s.link);
}

static void test_incoming(void *unused)
{
    (void)unused;
    signal_at("call_incoming", "", "{\"payload\":{\"room_id\":\"d_room\"}}", BINDING);
    signal_at("call_incoming", "wx", "{\"payload\":{\"room_id\":\"d_room\",\"caller_id\":\"peer\"}}", BINDING);
    signal_at("call_incoming", "", "{\"payload\":{\"room_id\":\"d_room\",\"caller_id\":\"peer\"}}", BINDING - 1);
    assert(status().state == XIAOTAI_CALL_IDLE && status().stale_calls == 1 && s.requests == 0);
    const char *unsupported = "{\"payload\":{\"room_id\":\"d_unsupported\",\"caller_id\":\"peer\",\"call_type\":\"screen\"}}";
    signal_at("call_incoming", "", unsupported, BINDING);
    assert(status().refused_media == 1);
    release_is("reject", "unsupported_media");
    signal_at("call_incoming", "", unsupported, BINDING);
    assert(status().state == XIAOTAI_CALL_IDLE && status().refused_media == 1 && s.requests == 1);
    signal_at("call_incoming", "", "{\"payload\":{\"room_id\":\"d_video\",\"caller_id\":\"peer\",\"call_type\":\"video\"}}",
              BINDING);
    assert(status().state == XIAOTAI_CALL_INCOMING && status().flags == XIAOTAI_CALL_VIDEO_OFFERED);
    signal_at("call_cancel", "", "{\"payload\":{\"room_id\":\"d_video\"}}", BINDING);
    assert(status().state == XIAOTAI_CALL_IDLE && status().announced == 1 && status().refused_media == 1);
    incoming();
    assert(status().state == XIAOTAI_CALL_INCOMING && !strcmp(status().peer_name, "Friendly") && !s.link && !status().flags);
    const uint32_t session = status().session;
    incoming();
    assert(status().session == session && status().announced == 2 && s.requests == 1);
    signal_at("call_cancel", "", "{\"payload\":{\"room_id\":\"old\"}}", BINDING);
    assert(status().session == session && status().stale_calls == 2);
    signal_at("call_incoming", "", "{\"payload\":{\"room_id\":\"other\",\"caller_id\":\"peer\"}}", BINDING);
    release_is("reject", "busy");
    assert(status().refused_busy == 1 && status().session == session);

    static const char *const invalid_cancel[] = {
        "{\"payload\":{}}", "{\"payload\":{\"room_id\":\"\"}}", "{\"payload\":{\"room_id\":null}}",
    };
    const unsigned requests = s.requests;
    for (size_t i = 0; i < sizeof(invalid_cancel) / sizeof(invalid_cancel[0]); ++i) {
        signal_at("call_cancel", "", invalid_cancel[i], BINDING);
        assert(status().session == session && status().state == XIAOTAI_CALL_INCOMING && s.requests == requests);
    }
    signal_at("call_cancel", "", "{\"payload\":{\"room_id\":\"d_room\"}}", BINDING);
    assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_PEER_CANCEL);
    assert(s.requests == requests);
    signal_at("call_cancel", "", "{\"payload\":{\"room_id\":\"d_room\"}}", BINDING);
    assert(!status().session && status().end == XIAOTAI_CALL_END_PEER_CANCEL && s.requests == requests);
    incoming();
    assert(status().session && status().session != session);
    signal_at("room_cancel", "", "{\"payload\":{\"room_id\":\"d_room\"}}", BINDING);
    assert(!status().session && status().end == XIAOTAI_CALL_END_PEER_CANCEL && s.requests == requests);
}

static void test_accept_connection_failure(void *context)
{
    const bool submitted_error = context != NULL;
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    const unsigned before = s.requests;
    if (submitted_error) {
        s.connect_error = XIAOTAI_ERR_NO_MEM;
    }
    token_reply();
    assert(s.connect_attempts == 1 && !strchr(s.trace, 'B') && !strchr(s.trace, 'M'));
    if (!submitted_error) {
        const uint32_t link = s.link, session = status().session;
        assert(link && session && status().state == XIAOTAI_CALL_CONNECTING);
        s.connected.closed(s.connected.ctx, link + 1, XIAOTAI_LINK_END_NETWORK, -71);
        assert(status().session == session && s.requests == before && status().link == link);
        s.link = 0;
        s.connected.closed(s.connected.ctx, link, XIAOTAI_LINK_END_NETWORK, -71);
        assert(status().end == XIAOTAI_CALL_END_NETWORK && status().error == XIAOTAI_FAIL);
        s.connected.closed(s.connected.ctx, link, XIAOTAI_LINK_END_NETWORK, -71);
    } else {
        assert(status().end == XIAOTAI_CALL_END_NO_MEMORY && status().error == XIAOTAI_ERR_NO_MEM);
    }
    assert(status().state == XIAOTAI_CALL_IDLE && !status().session && !status().link && !s.link);
    assert(s.requests == before + 1 && !xiaotai_activity_current(NULL));
    assert(!strchr(s.trace, 'B') && !strchr(s.trace, 'M'));
    release_is("hangup", "p2p_error");
}

static void test_accept_failure_cleanup(void *context)
{
    const bool refuse_release = context != NULL;
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    if (refuse_release) s.submit_error = XIAOTAI_ERR_NO_MEM;
    const http_callback_t old_token = take_reply(false);
    deliver(old_token, BINDING, XIAOTAI_OK, 503, 200, success_token);
    assert(status().state == XIAOTAI_CALL_IDLE && status().error == CALL_INVALID_RESPONSE);
    assert(s.urgent_attempts == 1 && !s.connect_attempts && status().releases_pending == 1);
    if (refuse_release) {
        assert(!s.release_reply && s.requests == 1);
        return;
    }
    release_is("hangup", "p2p_error");
    const http_callback_t old_release = take_reply(true);
    incoming_room("d_other");
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    const uint32_t session = status().session;
    deliver(old_token, BINDING, XIAOTAI_OK, 200, 200, success_token);
    deliver(old_release, BINDING, XIAOTAI_OK, 200, 40400, "{\"code\":40400}");
    assert(status().session == session && status().state == XIAOTAI_CALL_CONNECTING);
    assert(s.urgent_attempts == 1 && !s.connect_attempts && s.requests == 3);
}

static void test_accept(void *unused)
{
    (void)unused;
    incoming();
    const uint32_t session = status().session;
    assert(xiaotai_call_accept(session + 1U) == XIAOTAI_ERR_INVALID_STATE && status().stale_calls == 1);
    assert(xiaotai_call_accept(session) == XIAOTAI_OK && status().state == XIAOTAI_CALL_CONNECTING);
    assert(!strcmp(s.path, "/v1/call/device/info") && s.flags == XIAOTAI_PLATFORM_RELEASE && s.timeout == 28000);
    assert(!strcmp(s.body, "{\"device_id\":\"peer\",\"room_id\":\"d_room\",\"purpose\":\"call\"}"));
    char body[sizeof(success_token) + 3];
    snprintf(body, sizeof(body), "%s\r\n ", success_token);
    reply(200, 200, body);
    assert(status().link == s.link && !s.attachment_link && !strchr(s.trace, 'M'));
    s.connected.opened(s.connected.ctx, s.link);
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.attachment_link == s.link && strstr(s.trace, "LBMRPY"));
}

static void test_wrong_token(void *unused)
{
    (void)unused;
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    reply(200, 200, "{\"code\":200,\"data\":{\"device_id\":\"wrong\",\"token\":\"fixture-token\"}}");
    assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_FAILED && !s.link);
    release_is("hangup", "p2p_error");
}

static void test_incoming_closed_after_answer(void *unused)
{
    (void)unused;
    incoming();
    const uint32_t session = status().session;
    assert(xiaotai_call_accept(session) == XIAOTAI_OK);
    token_reply();
    const uint32_t link = s.link;
    assert(link);
    s.connected.opened(s.connected.ctx, link);
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.attachment_link == link);
    const unsigned requests = s.requests;
    s.connected.closed(s.connected.ctx, link, XIAOTAI_LINK_END_NETWORK, -9);
    assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_NETWORK);
    assert(status().error == XIAOTAI_FAIL && !status().link && !s.attachment_link && !xiaotai_activity_current(NULL));
    assert(s.requests == requests + 1U && !strchr(s.trace, 'E') && !strchr(s.trace, 'D'));
    release_is("hangup", "hangup");
    assert(!strstr(s.body, "p2p_error"));
    xiaotai_eventlog_entry_t entries[16];
    const size_t n = xiaotai_eventlog_read(0, entries, 16);
    bool raw_error = false;
    for (size_t i = 0; i < n; ++i) {
        if (!strcmp(entries[i].source, "call") && !strcmp(entries[i].label, "link.detail"))
            raw_error |= entries[i].generation == session && entries[i].value == -9;
    }
    assert(raw_error);
}

static void test_answer_send_failure(void *unused)
{
    (void)unused;
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    token_reply();
    s.command_error = XIAOTAI_ERR_BUSY;
    s.connected.opened(s.connected.ctx, s.link);
    assert(status().end == XIAOTAI_CALL_END_NETWORK && status().error == XIAOTAI_ERR_BUSY);
    assert(strchr(s.trace, 'B') && !strchr(s.trace, 'M') && !s.link && !s.attachment_link);
    release_is("hangup", "p2p_error");
}

static void test_accept_denial(void *unused)
{
    (void)unused;
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    const unsigned requests = s.requests;
    reply(200, 40300, "{\"code\":40300}");
    assert(status().state == XIAOTAI_CALL_IDLE && s.requests == requests && !s.link);
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    reply(503, 40300, "{\"code\":40300}");
    assert(status().state == XIAOTAI_CALL_IDLE && s.requests == requests + 2U && !s.link);
    release_is("hangup", "p2p_error");
}

typedef struct {
    const char *json_room;
    bool strict;
    bool post_release;
} failed_accept_room_case_t;

static void test_accept_bad_room_release(void *context)
{
    const failed_accept_room_case_t *test = context;
    incoming_room(test->json_room);
    assert(status().state == XIAOTAI_CALL_INCOMING);
    assert(xiaotai_call_accept(0) == XIAOTAI_OK && status().state == XIAOTAI_CALL_CONNECTING);
    assert(!strcmp(s.path, "/v1/call/device/info") && strstr(s.body, test->json_room));
    const unsigned requests = s.requests;
    reply(503, 0, NULL);
    assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_FAILED && !s.link);
    if (!test->post_release) {
        assert(s.requests == requests && !s.release_reply && !status().releases_pending);
    } else {
        assert(s.requests == requests + 1U && s.release_reply && status().releases_pending == 1);
        release_is("hangup", "p2p_error");
        assert(strstr(s.body, test->json_room));
    }
}

static void test_invalid_failed_accept_keeps_release_owner(void *context)
{
    uint32_t *session = context;
    incoming_room("d_old");
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    reply(503, 0, NULL);
    release_is("hangup", "p2p_error");
    const http_callback_t old_release = take_reply(true);
    assert(status().releases_pending == 1);

    incoming_room("d_invalid\\\"room");
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    const unsigned requests = s.requests;
    reply(503, 0, NULL);
    assert(status().state == XIAOTAI_CALL_IDLE && !s.release_reply && s.requests == requests);
    assert(status().releases_pending == 1);

    incoming_room("d_invalid\\\"room");
    *session = status().session;
    assert(*session && status().state == XIAOTAI_CALL_INCOMING);
    deliver(old_release, BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
    assert(status().releases_pending == 1 && status().session == *session && s.requests == requests);
}

static void test_previous_release_confirmation(void *context)
{
    const uint32_t session = *(const uint32_t *)context;
    assert(!strcmp(s.path, "/v1/call/room") && s.flags == XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, "{\"code\":200,\"data\":null}");
    assert(!status().releases_pending && status().session == session && status().state == XIAOTAI_CALL_INCOMING);
    assert(xiaotai_call_reject(session) == XIAOTAI_OK);
    release_is("reject", "decline");
    assert(strstr(s.body, "d_invalid\\\"room"));
}

static void test_dial_envelopes(void *unused)
{
    (void)unused;
    static const struct { int http, code; xiaotai_err_t result; const char *body; } invalid[] = {
        {201, 200, XIAOTAI_OK, success_room}, {503, 200, XIAOTAI_OK, success_room},
        {0, 200, XIAOTAI_ERR_TIMEOUT, success_room}, {200, 40201, XIAOTAI_FAIL, "{\"code\":40201}"},
        {200, 0, XIAOTAI_OK, "{\"code\":0,\"data\":{\"room_id\":\"d_room\"}}"},
        {200, 200, XIAOTAI_OK, "{\"code\":200.5,\"data\":{\"room_id\":\"d_room\"}}"},
        {200, 200, XIAOTAI_OK, "{\"code\":200,\"data\":{\"room_id\":\"d_room\"}} extra"},
        {200, 200, XIAOTAI_OK, "{\"code\":200,\"code\":0,\"data\":{\"room_id\":\"d_room\"}}"},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        start_outgoing();
        const unsigned requests = s.requests;
        deliver(take_reply(false), BINDING, invalid[i].result, invalid[i].http, invalid[i].code, invalid[i].body);
        const xiaotai_err_t err = invalid[i].result == XIAOTAI_OK ? CALL_INVALID_RESPONSE : invalid[i].result;
        assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_FAILED && status().error == err);
        assert(s.requests == requests && !s.link && status().releases_pending == i + 1U);
    }
    assert(xiaotai_call_dial("peer", 0, NULL) == XIAOTAI_ERR_NO_MEM);
}

static void test_accept_envelopes(void *which)
{
    static const struct { int http, code; xiaotai_err_t result; const char *body; bool denied; } cases[] = {
        {201, 200, XIAOTAI_OK, success_token, false}, {0, 200, XIAOTAI_OK, success_token, false},
        {0, 0, XIAOTAI_ERR_TIMEOUT, NULL, false}, {401, 0, XIAOTAI_OK, NULL, true},
        {200, 0, XIAOTAI_OK, "{\"code\":0,\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}}", false},
        {200, 200, XIAOTAI_OK, "{\"code\":200.5,\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}}", false},
        {200, 200, XIAOTAI_OK, "{\"code\":200,\"data\":{\"device_id\":\"wrong\",\"token\":\"fixture-token\"}}", false},
        {200, 200, XIAOTAI_OK, "{\"code\":200,\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}} trailing", false},
        {200, 200, XIAOTAI_OK, "{\"code\":200,\"data\":{\"device_id\":\"peer\",\"token\":\"\"}}", false},
        {200, 40210, XIAOTAI_FAIL, "{\"code\":40210,\"data\":null}", false},
        {200, 40000, XIAOTAI_OK, "{\"code\":40000}", true}, {200, 40210, XIAOTAI_OK, "{\"code\":40210}", true},
        {200, 40300, XIAOTAI_OK, "{\"code\":40300}", true}, {200, 40400, XIAOTAI_OK, "{\"code\":40400}", true},
        {503, 40300, XIAOTAI_OK, "{\"code\":40300}", false},
        {503, 200, XIAOTAI_OK, success_token, false},
        {200, 200, XIAOTAI_OK, "{\"code\":\"200\",\"data\":{\"device_id\":\"peer\",\"token\":\"fixture-token\"}}", false},
        {200, 200, XIAOTAI_OK, "{\"code\":200,\"data\":{\"token\":\"fixture-token\"}}", false},
        {200, 50000, XIAOTAI_OK, "{\"code\":50000}", false},
        {200, 40210, XIAOTAI_OK, "{\"code\":40210,\"data\":null}", true},
    };
    const size_t start = (size_t)(uintptr_t)which;
    for (size_t i = start; i < sizeof(cases) / sizeof(cases[0]) && i < start + 4U; ++i) {
        const unsigned requests = s.requests;
        char room[32];
        snprintf(room, sizeof(room), "d_fixture_%zu", i);
        incoming_room(room);
        assert(xiaotai_call_accept(0) == XIAOTAI_OK && s.requests == requests + 1U);
        http_callback_t h = take_reply(false);
        if (i == 0) {
            const unsigned stale = status().stale_calls;
            deliver(h, BINDING - 1U, XIAOTAI_OK, 200, 200, success_token);
            assert(status().state == XIAOTAI_CALL_CONNECTING && status().stale_calls == stale + 1U && !s.link);
        }
        deliver(h, BINDING, cases[i].result, cases[i].http, cases[i].code, cases[i].body);
        const xiaotai_err_t err = cases[i].result == XIAOTAI_OK ? CALL_INVALID_RESPONSE : cases[i].result;
        assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_FAILED && status().error == err);
        assert(!s.connect_attempts && !s.link && s.requests == requests + (cases[i].denied ? 1U : 2U));
        if (!cases[i].denied) release_is("hangup", "p2p_error");
    }
}

static void test_long_token(void *opaque)
{
    const bool oversized = opaque != NULL;
    s.long_token_size = XIAOTAI_LINK_TOKEN_MAX - (oversized ? 0U : 1U);
    char token[XIAOTAI_LINK_TOKEN_MAX + 1U];
    memset(token, 't', s.long_token_size);
    token[s.long_token_size] = '\0';
    char json[XIAOTAI_LINK_TOKEN_MAX + 100U];
    int n = snprintf(json, sizeof(json), "{\"code\":200,\"data\":{\"device_id\":\"peer\",\"token\":\"%s\"}}", token);
    assert(n > 0 && (size_t)n < sizeof(json));
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    reply(200, 200, json);
    if (oversized) {
        assert(!s.link && status().end == XIAOTAI_CALL_END_FAILED && status().error == CALL_INVALID_RESPONSE);
        release_is("hangup", "p2p_error");
    } else assert(s.link && status().state == XIAOTAI_CALL_CONNECTING);
}

static const char occupied[] = "{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}";
static const char empty_room[] = "{\"code\":200,\"data\":null}";
static const char old_active[] = "{\"code\":200,\"data\":{\"room_id\":\"d_old\",\"role\":\"caller\",\"status\":\"active\"}}";

static void query_is(void)
{
    assert(s.method == XIAOTAI_PLATFORM_GET && !strcmp(s.path, "/v1/call/room"));
    assert(!s.body[0] && !s.flags && s.timeout == 4000);
}

static void repair_start(void *unused)
{
    (void)unused;
    start_outgoing();
    reply(200, 40202, occupied);
    assert(status().state == XIAOTAI_CALL_OUTGOING);
    query_is();
}

static void test_repair_roles(void *case_index)
{
    const size_t i = (size_t)(uintptr_t)case_index;
    const char *const role[] = {"caller", "callee", "caller", "callee"};
    const char *const phase[] = {"answered", "answered", "active", "active"};
    const char *const verb[] = {"hangup", "hangup", "cancel", "reject"};
    const char *const reason[] = {"hangup", "hangup", NULL, "decline"};
    start_outgoing();
    const uint32_t session = status().session;
    const http_callback_t original = take_reply(false);
    if (i == 0) {
        open_inbound();
        command(s.link, CALL_COMMAND_ANSWER, "{\"room_id\":\"d_old\"}");
        assert(status().link && !strchr(s.trace, 'M'));
    }
    deliver(original, BINDING, XIAOTAI_OK, 200, 40202, occupied);
    assert(status().session == session && !status().link && !s.link && s.mode[0] == XIAOTAI_LINK_LISTEN_ONCE);
    query_is();
    if (i == 0) {
        assert(strstr(s.trace, "DAH"));
        open_inbound();
        assert(!s.link && !status().link && s.mode[0] == XIAOTAI_LINK_LISTEN_ONCE && !strchr(s.trace, 'M'));
    }
    char json[150];
    int n = snprintf(json, sizeof(json), "{\"code\":200,\"data\":{\"room_id\":\"d_old\",\"status\":\"%s\",\"role\":\"%s\"}}",
                     phase[i], role[i]);
    assert(n > 0 && (size_t)n < sizeof(json));
    const http_callback_t query = take_reply(false);
    deliver(query, BINDING, XIAOTAI_OK, 200, 200, json);
    release_is(verb[i], reason[i]);
    assert(strstr(s.body, "\"room_id\":\"d_old\""));
    const http_callback_t cleanup = take_reply(true);
    deliver(cleanup, BINDING, i == 0 ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK, i == 0 ? 0 : 200, 200, "{\"code\":200}");
    query_is();
    const unsigned count = s.requests;
    deliver(query, BINDING, XIAOTAI_OK, 200, 200, empty_room);
    assert(s.requests == count);  /* GET QUERY cannot satisfy GET VERIFY. */
    reply(200, 200, i == 1 ? "{\"code\":200}" : empty_room);
    assert(status().session == session && s.requests == count + 1U && s.timeout == 28000);
    assert(s.method == XIAOTAI_PLATFORM_POST && !strcmp(s.path, "/v1/call/request") && strstr(s.body, "\"audio\""));
    deliver(original, BINDING, XIAOTAI_OK, 200, 200, success_room);
    assert(status().session == session && !status().link);  /* Not the retry callback. */
    room_reply();
    open_inbound();
    assert(status().state == XIAOTAI_CALL_OUTGOING && !strchr(s.trace, 'M'));
    command(s.link, CALL_COMMAND_ANSWER, "{\"room_id\":\"d_room\"}");
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.attachment_link == s.link);
}

static void test_repair_reject_conflict(void *unused)
{
    (void)unused;
    static const struct { int http; const char *body; xiaotai_call_end_t end; } bad[] = {
        {409, occupied, XIAOTAI_CALL_END_FAILED},
        {200, "{\"code\":40202.5,\"data\":{\"room_id\":\"d_old\"}}", XIAOTAI_CALL_END_FAILED},
        {200, "{\"code\":40202}", XIAOTAI_CALL_END_BUSY},
        {200, "{\"code\":40202,\"data\":{\"room_id\":\"d_\"}}", XIAOTAI_CALL_END_BUSY},
        {200, "{\"code\":40202,\"data\":{\"room_id\":\"wx_room\"}}", XIAOTAI_CALL_END_BUSY},
        {200, "{\"code\":40202,\"data\":{\"room_id\":\"d_bad\\\"\"}}", XIAOTAI_CALL_END_BUSY},
        {200, "{\"code\":40202,\"data\":{\"room_id\":\"d_old\",\"room_id\":\"d_other\"}}", XIAOTAI_CALL_END_BUSY},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        start_outgoing();
        if (i == 2U) open_inbound();
        const unsigned requests = s.requests;
        reply(bad[i].http, 40202, bad[i].body);
        assert(status().end == bad[i].end && s.requests == requests && !s.reply && !s.release_reply);
        if (i == 2U) assert(!s.link && !strchr(s.trace, 'E'));
    }
    char room[CALL_ROOM_SIZE + 1U];
    memset(room, 'x', sizeof(room));
    room[0] = 'd'; room[1] = '_'; room[CALL_ROOM_SIZE] = '\0';
    assert(!call_http_room_id_valid(room));
    room[CALL_ROOM_SIZE - 1U] = '\0';
    assert(call_http_room_id_valid(room));
}

static void test_repair_authority(void *unused)
{
    (void)unused;
    static const struct { int http; const char *body; xiaotai_call_end_t end; } bad[] = {
        {503, old_active, XIAOTAI_CALL_END_FAILED},
        {200, "{\"code\":500}", XIAOTAI_CALL_END_FAILED},
        {200, "{\"code\":200,\"data\":{}}", XIAOTAI_CALL_END_BUSY},
        {200, "{\"code\":200,\"data\":null,\"data\":{}}", XIAOTAI_CALL_END_BUSY},
        {200, "{\"code\":200,\"data\":{\"room_id\":\"d_other\",\"status\":\"answered\",\"role\":\"caller\"}}", XIAOTAI_CALL_END_BUSY},
        {200, "{\"code\":200,\"data\":{\"room_id\":\"d_old\",\"status\":\"unknown\",\"role\":\"caller\"}}", XIAOTAI_CALL_END_FAILED},
        {200, "{\"code\":200,\"data\":{\"room_id\":\"d_old\",\"status\":\"answered\",\"role\":\"stranger\"}}", XIAOTAI_CALL_END_FAILED},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        repair_start(NULL);
        const unsigned requests = s.requests;
        reply(bad[i].http, 200, bad[i].body);
        assert(status().end == bad[i].end && s.requests == requests && !s.release_reply && !status().releases_pending);
    }
    const struct { const char *query; const char *release; int release_http, verify_http;
        const char *verify; xiaotai_call_end_t end; } verification[] = {
        {"{\"code\":200,\"data\":{\"room_id\":\"d_old\",\"status\":\"answered\",\"role\":\"caller\"}}",
         "{\"code\":500}", 200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_old\"}}", XIAOTAI_CALL_END_BUSY},
        {old_active, "", 503, 201, empty_room, XIAOTAI_CALL_END_FAILED},
    };
    for (size_t i = 0; i < sizeof(verification) / sizeof(verification[0]); ++i) {
        repair_start(NULL);
        reply(200, 200, verification[i].query);
        release_is(i ? "cancel" : "hangup", i ? NULL : "hangup");
        deliver(take_reply(true), BINDING, XIAOTAI_OK, verification[i].release_http,
                i ? XIAOTAI_PLATFORM_NO_CODE : 500, verification[i].release);
        query_is();
        const unsigned requests = s.requests;
        reply(verification[i].verify_http, 200, verification[i].verify);
        assert(status().end == verification[i].end);
        assert(s.requests == requests && !s.reply && !status().releases_pending);
    }

    repair_start(NULL);
    const unsigned before = s.requests;
    reply(200, 200, empty_room);
    assert(s.requests == before + 1 && !strcmp(s.path, "/v1/call/request"));
    reply(200, 40202, occupied);
    assert(s.requests == before + 1U && status().end == XIAOTAI_CALL_END_BUSY && !status().releases_pending);

    start_outgoing();
    s.submit_error = XIAOTAI_ERR_BUSY;
    reply(200, 40202, occupied);
    assert(status().end == XIAOTAI_CALL_END_FAILED && status().error == XIAOTAI_ERR_BUSY && !status().releases_pending);
}

static void test_http_parse_budget(void *unused)
{
    (void)unused;
    static const struct { const char *body; xiaotai_err_t release, repair; } cases[] = {
        {"{\"code\":200}", XIAOTAI_OK, XIAOTAI_OK},
        {"{\"code\":200,\"data\":null}", XIAOTAI_OK, XIAOTAI_OK},
        {"{\"code\":200,\"data\":null,\"data\":null}", CALL_INVALID_RESPONSE, XIAOTAI_ERR_BUSY},
        {"{\"code\":200,\"co\\u0064e\":200}", CALL_INVALID_RESPONSE, CALL_INVALID_RESPONSE},
        {"{\"code\":200,\"data\":{\"room_id\":\"d_x\",\"room_id\":\"d_x\"}}",
            CALL_INVALID_RESPONSE, XIAOTAI_ERR_BUSY},
        {"{\"code\":200,\"nested\":{\"code\":401,\"data\":{}},\"data\":null}", XIAOTAI_OK, XIAOTAI_OK},
    };
    xiaotai_call_state_t phase;
    xiaotai_platform_reply_t reply = {.result = XIAOTAI_OK, .http_status = 200};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reply.body = cases[i].body;
        reply.size = strlen(cases[i].body);
        phase = XIAOTAI_CALL_ACTIVE;
        assert(call_http_release_action(&reply, "d_x", &phase) == cases[i].release);
        if (cases[i].release == XIAOTAI_OK) assert(phase == XIAOTAI_CALL_IDLE);
        assert(call_http_repair_action(&reply, true, "d_x", &phase) == cases[i].repair);
    }
    char body[256] = "{\"code\":200,\"data\":null,\"f\":[";
    char *end = body + strlen(body);
    for (size_t i = 0; i < CALL_JSON_TOKENS - 7U; ++i) {
        if (i) *end++ = ',';
        *end++ = '0';
    }
    strcpy(end, "]}");
    reply.body = body;
    reply.size = strlen(body);
    assert(call_http_release_action(&reply, "d_x", &phase) == XIAOTAI_OK);
    strcpy(end, ",0]}");
    reply.size = strlen(body);
    assert(call_http_release_action(&reply, "d_x", &phase) == CALL_INVALID_RESPONSE);
}

static void test_repair_staleness(void *unused)
{
    (void)unused;
    repair_start(NULL);
    const http_callback_t previous = take_reply(false);
    const unsigned requests = s.requests;
    const uint32_t matching = status().session;
    deliver(previous, BINDING - 1U, XIAOTAI_OK, 200, 200, empty_room);
    assert(status().session == matching && s.requests == requests);
    assert(xiaotai_call_hangup(matching) == XIAOTAI_OK && !status().releases_pending);
    start_outgoing();
    const uint32_t current = status().session;
    assert(current != matching);
    deliver(previous, BINDING, XIAOTAI_OK, 200, 200, old_active);
    assert(status().session == current && s.requests == requests + 1U && !s.release_reply);
    reply(200, 40202, occupied);
    reply(200, 200, old_active);
    release_is("cancel", NULL);
    const unsigned noverify = s.requests;
    deliver(take_reply(true), BINDING, XIAOTAI_ERR_PLATFORM_STALE, 200, 200, empty_room);
    assert(status().end == XIAOTAI_CALL_END_NETWORK && s.requests == noverify && !status().releases_pending);
}

static void test_timed_out_repair(void *unused)
{
    (void)unused;
    const unsigned requests = s.requests;
    assert(status().end == XIAOTAI_CALL_END_NO_ANSWER && status().error == XIAOTAI_ERR_TIMEOUT);
    reply(200, 200, empty_room);
    assert(s.requests == requests && !s.release_reply && !status().releases_pending);
}

static void test_retired_dial(void *unused)
{
    (void)unused;
    start_outgoing();
    const http_callback_t retired = take_reply(false);
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK && status().releases_pending == 1);
    const unsigned requests = s.requests;
    deliver(retired, BINDING, XIAOTAI_OK, 200, 200, success_room);
    assert(status().state == XIAOTAI_CALL_IDLE && s.requests == requests + 1 && !s.link);
    release_is("cancel", NULL);
    assert(!strcmp(s.body, "{\"room_id\":\"d_room\"}"));
    deliver(retired, BINDING, XIAOTAI_OK, 200, 200, success_room);
    assert(s.requests == requests + 1);

    const unsigned retained = status().releases_pending;
    start_outgoing();
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK);
    const unsigned before_conflict = s.requests;
    reply(200, 40202, occupied);
    assert(s.requests == before_conflict && status().releases_pending == retained);
}

static void test_outgoing_connect(void *unused)
{
    (void)unused;
    start_outgoing();
    open_inbound();
    command(s.link, CALL_COMMAND_ANSWER, "{\"room_id\":\"old\"}");
    assert(!strchr(s.trace, 'M'));
    room_reply();
    assert(status().state == XIAOTAI_CALL_OUTGOING && status().stale_calls == 1);
    command(s.link + 1, CALL_COMMAND_ANSWER, NULL);
    command(s.link, 0x12000, NULL);
    assert(!strchr(s.trace, 'M') && status().stale_calls == 2);
    signal_at("callee_answered", "", "{\"payload\":{\"room_id\":\"d_room\",\"callee_id\":\"answered-peer\"}}", BINDING);
    assert(status().state == XIAOTAI_CALL_ACTIVE && !strcmp(status().peer_id, "answered-peer"));
    assert(s.attachment_link == s.link && status().session != s.link && strstr(s.trace, "MRPY"));
}

/* Application messages ride on the active call's link, both ways. */
static void test_message_send(void *unused)
{
    (void)unused;
    start_outgoing();
    room_reply();
    open_inbound();
    const uint32_t session = status().session;
    assert(xiaotai_call_send_message(session, 1, "{}", 2) == XIAOTAI_ERR_INVALID_STATE);   /* not ACTIVE yet */
    command(s.link, XIAOTAI_CALL_MESSAGE_WORD(1), "early");                                  /* dropped */
    command(s.link, CALL_COMMAND_ANSWER, "{\"room_id\":\"d_room\"}");
    assert(status().state == XIAOTAI_CALL_ACTIVE);
    assert(xiaotai_call_send_message(session + 1, 1, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_call_send_message(session, XIAOTAI_CALL_MESSAGE_TYPES, NULL, 0) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_call_send_message(session, 1, NULL, 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_call_send_message(session, 1, s.body, XIAOTAI_CALL_MESSAGE_MAX + 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_call_send_message(session, 2, "open", 4) == XIAOTAI_OK);
    assert(s.message_word == 0x2602U && s.message_size == 4 && !memcmp(s.message, "open", 4));
    s.command_error = XIAOTAI_ERR_BUSY;
    assert(xiaotai_call_send_message(session, 2, NULL, 0) == XIAOTAI_ERR_BUSY);
    s.command_error = XIAOTAI_OK;
    command(s.link + 1, XIAOTAI_CALL_MESSAGE_WORD(3), "stale");                              /* another link */
    command(s.link, XIAOTAI_CALL_MESSAGE_WORD(3), "opened");
    assert(status().state == XIAOTAI_CALL_ACTIVE);
}

static void test_message_check(void *unused)
{
    (void)unused;
    assert(s.messages_in == 1 && s.message_type == 3 && !strcmp(s.message_text, "opened"));
    assert(s.message_session == status().session);
}

static void test_cloud_before_link(void *unused)
{
    (void)unused;
    start_outgoing();
    room_reply();
    signal_at("callee_answered", "", "{\"payload\":{\"room_id\":\"d_room\"}}", BINDING);
    assert(status().state == XIAOTAI_CALL_OUTGOING && !status().link && !s.attachment_link && !strchr(s.trace, 'M'));
    open_inbound();
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.attachment_link == s.link);
}

static void test_video(void *unused)
{
    (void)unused;
    assert(s.listener_count == 2 && !s.listener[0].profile.video.send &&
           s.listener[1].profile.video.send_stream == 11);
    signal_at("call_incoming", "", "{\"payload\":{\"room_id\":\"d_room\",\"caller_id\":\"peer\",\"call_type\":\"video\"}}", BINDING);
    assert(status().state == XIAOTAI_CALL_INCOMING && status().flags == XIAOTAI_CALL_VIDEO);
    assert(xiaotai_call_reject(0) == XIAOTAI_OK);
    release_is("reject", "decline");
    assert(xiaotai_call_dial("peer", XIAOTAI_CALL_VIDEO, NULL) == XIAOTAI_OK);
    assert(s.mode[1] == XIAOTAI_LINK_LISTEN_ONCE && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF);
    assert(!strcmp(s.body, "{\"targets\":[\"peer\"],\"call_type\":\"video\"}"));
}

static void test_end(void *context)
{
    active_outgoing();
    s.hangup_error = context ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
    const uint32_t session = status().session;
    assert(xiaotai_call_hangup(session + 1) == XIAOTAI_ERR_INVALID_STATE && !strchr(s.trace, 'E'));
    assert(xiaotai_call_hangup(session) == XIAOTAI_OK && status().releasing);
    assert(status().state == XIAOTAI_CALL_IDLE && !status().session && !status().link && !s.attachment_link);
    assert(status().end == XIAOTAI_CALL_END_LOCAL && status().error == s.hangup_error);
    assert(strstr(s.trace, "ZCEH") && !strchr(s.trace, 'D'));
    release_is("hangup", "hangup");
    deliver(take_reply(true), BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
    assert(status().releases_pending == 1);
    assert(xiaotai_call_dial("peer", 0, NULL) == XIAOTAI_ERR_BUSY);
}

static void test_legacy_shutdown(void *context)
{
    const uintptr_t i = (uintptr_t)context;
    if (i == 0) {
        incoming();
        assert(xiaotai_call_reject(0) == XIAOTAI_OK);
        release_is("reject", "decline");
        assert(s.requests == 1 && !strchr(s.trace, 'E') && !s.link);
    } else if (i == 1) {
        start_outgoing();
        room_reply();
        assert(xiaotai_call_hangup(0) == XIAOTAI_OK);
        release_is("cancel", NULL);
        assert(s.requests == 2 && !strchr(s.trace, 'E') && !s.link);
    } else if (i == 2 || i == 3) {
        incoming();
        assert(xiaotai_call_accept(0) == XIAOTAI_OK);
        token_reply();
        s.connected.opened(s.connected.ctx, s.link);
        assert(status().state == XIAOTAI_CALL_ACTIVE && !status().outgoing);
        s.hangup_error = i == 3 ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
        assert(xiaotai_call_hangup(0) == XIAOTAI_OK);
        const char *sent = strchr(s.trace, 'E');
        assert(sent && !strchr(sent + 1, 'E') && s.requests == 2);
        assert(status().end == XIAOTAI_CALL_END_LOCAL && status().error == s.hangup_error);
        release_is("hangup", "hangup");
    } else {
        uint32_t activity;
        const xiaotai_activity_t ai = {.name = "ai", .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_LINK};
        assert(xiaotai_activity_begin(&ai, &activity) == XIAOTAI_OK);
        assert(xiaotai_call_hangup(0) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_activity_current(NULL) == activity && !s.requests && !strchr(s.trace, 'E'));
        xiaotai_activity_end(activity);
    }
    assert(status().state == XIAOTAI_CALL_IDLE && !status().session);
    if (i < 4) assert(strstr(s.body, "\"room_id\":\"d_room\""));
}

static void test_no_early_disconnect(void *unused)
{
    (void)unused;
    assert(status().releasing && !strchr(s.trace, 'D'));
}

static void test_disconnected(void *unused)
{
    (void)unused;
    assert(!status().releasing && strstr(s.trace, "EH") && strchr(s.trace, 'D') && !xiaotai_activity_current(NULL));
}

static void test_reject_stage(void *context)
{
    const uintptr_t i = (uintptr_t)context;
    if (i == 0) { start_outgoing(); room_reply(); }
    else if (i == 1) { incoming(); assert(xiaotai_call_accept(0) == XIAOTAI_OK); }
    else if (i == 2) active_outgoing();
    else incoming();
    /* Who rejects may say why; without a word somebody declined. */
    static const char *const reasons[] = {"", ",\"reason\":\"decline\"", ",\"reason\":\"busy\"", ",\"reason\":\"unsupported_media\""};
    char body[96];
    (void)snprintf(body, sizeof(body), "{\"payload\":{\"room_id\":\"d_room\"%s}}", reasons[i]);
    signal_at("call_reject", "", body, BINDING);
    assert(status().end == (i == 2U ? XIAOTAI_CALL_END_BUSY : XIAOTAI_CALL_END_PEER_REJECT));
    assert(status().error == (i == 3U ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_OK));
    assert(status().state == XIAOTAI_CALL_IDLE && !s.link);
    /* Who rejected does not connect late: the next call's answer is let in at once. */
    assert(!s.inbound_blocks && !xiaotai_call_answer_hold_ms());
    if (s.reply) token_reply();
}

static void test_missed(void *unused)
{
    (void)unused;
    assert(status().end == XIAOTAI_CALL_END_MISSED);
    release_is("reject", "decline");
}

static void test_revoked(void *unused)
{
    (void)unused;
    active_outgoing();
    uint32_t id;
    const xiaotai_activity_t preempt = {.name = "maintenance", .resources = XIAOTAI_RESOURCE_LINK};
    assert(xiaotai_activity_begin(&preempt, &id) == XIAOTAI_OK);
    assert(status().end == XIAOTAI_CALL_END_PREEMPTED && !status().releasing && !s.link && strstr(s.trace, "CEHD"));
    xiaotai_activity_end(id);
}

static void test_busy(void *context)
{
    const bool block = *(const bool *)context;
    uint32_t id;
    const xiaotai_activity_t recognition = {.name = "recognition", .resources = XIAOTAI_RESOURCE_AUDIO_IN};
    assert(xiaotai_activity_begin(&recognition, &id) == XIAOTAI_OK);
    incoming();
    if (block) {
        assert(status().refused_busy == 1 && xiaotai_activity_current(NULL) == id);
        release_is("reject", "busy");
        start_outgoing();
        assert(xiaotai_activity_current(NULL) != id);
    } else {
        assert(status().state == XIAOTAI_CALL_INCOMING && xiaotai_activity_current(NULL) != id && s.requests == 0);
    }
}

static void test_contacts(void *context)
{
    const bool fresh = *(const bool *)context;
    assert(xiaotai_call_dial("unknown", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_call_dial_contact(1, 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    s.binding_valid = false;
    assert(xiaotai_call_dial("peer", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    s.binding_valid = true;
    s.fresh = false;
    if (fresh) assert(xiaotai_call_dial("peer", 0, NULL) == XIAOTAI_ERR_NOT_FOUND && s.contact_flags == 0);
    else {
        start_outgoing();
        assert(s.contact_flags == XIAOTAI_CONTACTS_STALE_OK);
    }
}

static void test_media(void *context)
{
    const bool configured = *(const bool *)context;
    active_outgoing();
    assert(s.media.flags == (configured ? XIAOTAI_MEDIA_GATE_IDLE_NOISE : 0));
    assert(s.media.latency.base_ms == (configured ? 40 : 140));
    s.media.event(s.link + 1, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL, s.media.ctx);
    assert(status().state == XIAOTAI_CALL_ACTIVE);
    s.media.event(s.link, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL, s.media.ctx);
    assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_AUDIO);
    assert(strstr(s.trace, "ZCEHD") && !s.attachment_link && !s.link);
}

static void test_remote_closed(void *context)
{
    const bool peer = *(const bool *)context;
    active_outgoing();
    const uint32_t link = s.link;
    s.listener[0].handlers.closed(s.listener[0].handlers.ctx, link + 1, XIAOTAI_LINK_END_PEER, 0);
    assert(status().state == XIAOTAI_CALL_ACTIVE && status().stale_calls == 1);
    s.listener[0].handlers.closed(s.listener[0].handlers.ctx, link,
                                  peer ? XIAOTAI_LINK_END_PEER : XIAOTAI_LINK_END_NETWORK, 0);
    assert(status().end == (peer ? XIAOTAI_CALL_END_PEER_HANGUP : XIAOTAI_CALL_END_NETWORK));
    assert(status().state == XIAOTAI_CALL_IDLE && !status().link && strstr(s.trace, "ZCH"));
    assert(!strchr(s.trace, 'E') && !strchr(s.trace, 'D'));
}

static void test_media_error(void *unused)
{
    (void)unused;
    incoming();
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    token_reply();
    s.media_error = XIAOTAI_ERR_INVALID_STATE;
    s.connected.opened(s.connected.ctx, s.link);
    assert(status().end == XIAOTAI_CALL_END_AUDIO && !s.link && !s.attachment_link);
    release_is("hangup", "p2p_error");
}

static void test_platform_start(void *unused)
{
    (void)unused;
    active_outgoing();
    const xiaotai_args_t offline = {.value = XIAOTAI_PLATFORM_OFFLINE_NETWORK};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, &offline, NULL, 0) == XIAOTAI_OK);
}

static void test_platform_check(void *ctx)
{
    const bool end_active = *(const bool *)ctx;
    assert(end_active ? status().end == XIAOTAI_CALL_END_NETWORK : status().state == XIAOTAI_CALL_ACTIVE);
    if (!end_active) {
        const xiaotai_args_t offline = {.value = XIAOTAI_PLATFORM_OFFLINE_BINDING};
        assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, &offline, NULL, 0) == XIAOTAI_OK);
    }
}

static void test_platform_binding(void *unused)
{
    (void)unused;
    assert(status().end == XIAOTAI_CALL_END_SYSTEM && !status().releases_pending && !s.link);
}

static void test_mute(void *context)
{
    const bool settings = *(const bool *)context;
    incoming();
    uint32_t id = status().session;
    assert(status().muted == settings);
    assert(xiaotai_call_set_muted(id + 1U, false) == XIAOTAI_ERR_INVALID_STATE && status().stale_calls == 1);
    assert(xiaotai_call_set_muted(id, true) == XIAOTAI_OK && status().muted && s.muted);
    if (settings) assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, 0) == XIAOTAI_OK);
    else {
        assert(xiaotai_call_set_muted(id, false) == XIAOTAI_OK && !status().muted);
        assert(xiaotai_call_set_muted(id, true) == XIAOTAI_OK);
        assert(xiaotai_call_hangup(id) == XIAOTAI_OK && !s.muted);
    }
}

static void test_mute_setting(void *unused)
{
    (void)unused;
    assert(!status().muted && !s.muted);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, 1) == XIAOTAI_OK);
    assert(xiaotai_call_set_muted(0, false) == XIAOTAI_OK && status().muted && s.muted);
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK && s.muted);
}

static struct {
    uint32_t session, stale;
    unsigned mic_applies, mic_setting_changes, call_states;
} mute_observation;
static bool mic_preaccepted;
static xiaotai_err_t mic_preflight_error;

xiaotai_err_t xiaotai_settings_set_mic_mute(bool muted)
{
    if (mic_preflight_error != XIAOTAI_OK) return mic_preflight_error;
    xiaotai_err_t err = xiaotai_media_set_mic_muted(muted);
    if (err != XIAOTAI_OK) return err;
    mic_preaccepted = true;
    err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, muted);
    mic_preaccepted = false;
    return err;
}

static void start_persisted_muted_call(void *unused)
{
    (void)unused;
    int32_t stored = 0;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &stored) == XIAOTAI_OK && stored == 1);
    // The settings owner already applied the boot preference in the product.
    assert(xiaotai_media_set_mic_muted(true) == XIAOTAI_OK);
    active_outgoing();
    assert(status().muted && s.muted);
}

static void post_mic_on(bool stale)
{
    const uint32_t session = status().session + (stale ? 1U : 0U);
    const xiaotai_args_t args = {.value = (int32_t)session};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_CALL_MIC_ON, &args) == XIAOTAI_OK);
}

static void post_stale_mic_on(void *unused)
{
    (void)unused;
    mute_observation.session = status().session;
    mute_observation.stale = status().stale_calls;
    mute_observation.mic_applies = s.mic_applies;
    mute_observation.mic_setting_changes = s.mic_setting_changes;
    mute_observation.call_states = s.call_states;
    post_mic_on(true);
}

static void post_current_mic_on(void *unused)
{
    (void)unused;
    assert(status().session == mute_observation.session && status().stale_calls == mute_observation.stale + 1U);
    assert(s.mic_applies == mute_observation.mic_applies && s.call_states == mute_observation.call_states);
    post_mic_on(false);
}

static void confirm_persisted_policy(void *unused)
{
    (void)unused;
    int32_t stored = 0;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &stored) == XIAOTAI_OK && stored == 1);
    assert(status().state == XIAOTAI_CALL_ACTIVE && status().session == mute_observation.session);
    assert(s.mic_applies == mute_observation.mic_applies + 1U && s.muted && status().muted);
    assert(s.call_states == mute_observation.call_states && s.mic_setting_changes == mute_observation.mic_setting_changes);
    // The explicit saved action is session-scoped; an error does not change the call.
    assert(xiaotai_call_set_saved_mute(0, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_call_set_saved_mute(mute_observation.session + 1U, false) == XIAOTAI_ERR_INVALID_STATE);
    mic_preflight_error = XIAOTAI_FAIL;
    assert(xiaotai_call_set_saved_mute(mute_observation.session, false) == XIAOTAI_FAIL);
    assert(status().muted && s.muted && s.mic_applies == mute_observation.mic_applies + 1U);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &stored) == XIAOTAI_OK && stored == 1);
    mic_preflight_error = XIAOTAI_OK;
    assert(xiaotai_call_set_saved_mute(mute_observation.session, false) == XIAOTAI_OK);
    assert(!status().muted && !s.muted);
}

static void confirm_settings_authority(void *unused)
{
    (void)unused;
    assert(s.call_states == mute_observation.call_states + 1U && !status().muted && !s.muted);
    assert(s.mic_setting_changes == mute_observation.mic_setting_changes + 1U);
    assert(xiaotai_call_set_muted(mute_observation.session, true) == XIAOTAI_OK && s.muted);
    // When the saved value already matches, update the live call without rewriting the setting.
    assert(xiaotai_call_set_saved_mute(mute_observation.session, false) == XIAOTAI_OK && !status().muted);
    assert(!s.muted && s.mic_setting_changes == mute_observation.mic_setting_changes + 1U);
    assert(xiaotai_call_set_muted(mute_observation.session, true) == XIAOTAI_OK && s.muted);
    assert(xiaotai_call_hangup(mute_observation.session) == XIAOTAI_OK && !s.muted);
    assert(xiaotai_call_set_saved_mute(mute_observation.session, true) == XIAOTAI_ERR_INVALID_STATE);
}

static void test_eventlog(void *unused)
{
    (void)unused;
    active_outgoing();
    const uint32_t session = status().session, link = status().link;
    command(s.link, CALL_COMMAND_END, "{}");
    assert(status().end == XIAOTAI_CALL_END_PEER_HANGUP);
    incoming_room("d_new_call");
    xiaotai_eventlog_entry_t entries[16];
    const size_t n = xiaotai_eventlog_read(0, entries, 16);
    size_t found = 0;
    const char *const names[] = {"outgoing", "connected", "ended", "incoming"};
    const int32_t values[] = {0, (int32_t)link, XIAOTAI_CALL_END_PEER_HANGUP, 0};
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(entries[i].source, "call")) continue;
        assert(found < 4 && !strcmp(entries[i].label, names[found]) && entries[i].value == values[found]);
        assert(entries[i].generation == (found == 3 ? status().session : session));
        ++found;
    }
    assert(found == 4);
}

static void attachment_generation(uint32_t id, uint32_t link, void *ctx)
{
    (void)id;
    (void)ctx;
    s.attachment_link = link;
    trace(link ? 'Y' : 'Z');
}

static void attachment_end(uint32_t id, uint32_t link, void *ctx)
{
    (void)id;
    (void)link;
    (void)ctx;
    s.attachment_link = 0;
}

static const xiaotai_link_media_t video_part = {
    .send = true, .send_stream = 11, .send_format = {.codec = XIAOTAI_LINK_CODEC_H264},
    .receive = true, .receive_stream = 13, .receive_request = true,
    .receive_formats = {{.codec = XIAOTAI_LINK_CODEC_H264}},
};
static const xiaotai_link_media_t other_video = {
    .send = true, .send_stream = 27, .send_format = {.codec = XIAOTAI_LINK_CODEC_JPEG},
};

static int part_text(char *out, size_t capacity, void *ctx)
{
    const char *text = ctx;
    const size_t n = strlen(text);
    if (n > capacity) return -1;
    memcpy(out, text, n);
    return (int)n;
}

static int part_optional(char *out, size_t capacity, void *ctx)
{
    if (test_scene_fail) {
        if (capacity) out[0] = '!';
        return -1;
    }
    return part_text(out, capacity, ctx);
}

static xiaotai_err_t foreign_status(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_at_printf(out, "+CALL: kind=wechat state=%s gen=0 muted=0 end=0 peer=- error=0",
                      other_call ? "incoming" : "idle");
    return XIAOTAI_AT_PASS;
}

static xiaotai_err_t foreign_action(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)out;
    if (!other_call) return XIAOTAI_AT_PASS;
    if (argc) assert(!strcmp(argv[0], "1"));
    ++foreign_actions;
    return XIAOTAI_OK;
}

static xiaotai_err_t attach(void)
{
    static char codecs[] = "\"up_video_mt\":[\"h264\"],\"down_video_mt\":[\"h264\"]";
    static char shape[] = "\"camera_rotation\":90,\"aspect_ratio\":\"4:3\",\"hor_mirror\":true,"
                          "\"vert_mirror\":false,\"object_fit\":\"contain\"";
    static const xiaotai_platform_scene_part_t scene_audio = {.write = part_text, .ctx = codecs};
    static const xiaotai_platform_scene_part_t scene_shape = {.write = part_optional, .ctx = shape};
    static const xiaotai_contribution_t items[] = {
        {.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = XIAOTAI_CALL_SCENE, .data = &video_part}, {0},
    };
    static const xiaotai_contribution_t extra[] = {
        {.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = XIAOTAI_CALL_SCENE, .data = &other_video}, {0},
    };
    static const xiaotai_contribution_t scenes[] = {
        {.slot = XIAOTAI_PLATFORM_SLOT_SCENE, .scene = XIAOTAI_CALL_SCENE, .data = &scene_audio}, {0},
    };
    static const xiaotai_contribution_t shapes[] = {
        {.slot = XIAOTAI_PLATFORM_SLOT_SCENE, .scene = XIAOTAI_CALL_SCENE, .data = &scene_shape}, {0},
    };
    const xiaotai_attachment_t entry = {
        .session = attachment_generation, .end = attachment_end, .contributions = s.contribute ? items : NULL,
    };
    xiaotai_err_t err = xiaotai_activity_attach(XIAOTAI_CALL_ACTIVITY, &entry);
    if (err == XIAOTAI_OK && test_duplicate_video) {
        err = xiaotai_activity_attach(XIAOTAI_CALL_ACTIVITY, &(xiaotai_attachment_t){.contributions = extra});
    }
    if (err == XIAOTAI_OK && test_scene) {
        err = xiaotai_activity_attach(XIAOTAI_CALL_ACTIVITY, &(xiaotai_attachment_t){.contributions = scenes});
        if (err == XIAOTAI_OK) {
            err = xiaotai_activity_attach(XIAOTAI_CALL_ACTIVITY, &(xiaotai_attachment_t){.contributions = shapes});
        }
    }
    if (err == XIAOTAI_OK && test_shared_at) {
        static const xiaotai_at_verb_t verbs[] = {
            {.run = foreign_status}, {.word = "ACCEPT", .run = foreign_action},
            {.word = "REJECT", .run = foreign_action}, {.word = "HANGUP", .run = foreign_action},
            {.word = "INDEX", .min_args = 1, .max_args = 1, .run = foreign_action, .usage = "<n>"},
            {.word = "MUTE", .min_args = 1, .max_args = 1, .run = foreign_action, .usage = "0|1"},
        };
        err = xiaotai_at_verbs_register("AT+CALL", verbs, sizeof(verbs) / sizeof(verbs[0]));
    }
    return err;
}

static void scene_string(const xiaotai_json_doc_t *doc, const char *path, const char *expected)
{
    assert(xiaotai_json_equals(doc, xiaotai_json_find(doc, 0, path), expected));
}

static void scene_members(const char *scene, size_t bytes, bool video, bool voice_geometry)
{
    xiaotai_json_token_t tokens[48];
    xiaotai_json_doc_t doc;
    assert(xiaotai_json_parse(scene, bytes, tokens, 48, NULL, &doc) == XIAOTAI_OK);
    assert(tokens[0].type == XIAOTAI_JSON_OBJECT && tokens[0].children == (video || voice_geometry ? 12 : 7));
    static const char *const arrays[] = {"up_audio_mt", "down_audio_mt", "up_video_mt", "down_video_mt"};
    for (size_t i = 0; i < 4; ++i) {
        const int array = xiaotai_json_get(&doc, 0, arrays[i]);
        const char *codec = i < 2 ? "alaw" : video ? "h264" : NULL;
        assert(array > 0 && tokens[array].type == XIAOTAI_JSON_ARRAY);
        assert(tokens[array].children == (codec != NULL));
        if (codec) assert(xiaotai_json_equals(&doc, xiaotai_json_at(&doc, array, 0), codec));
    }
    int64_t value;
    bool flag;
    assert(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "audio_rate"), &value) == XIAOTAI_OK && value == 8000);
    assert(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "audio_channels"), &value) == XIAOTAI_OK && value == 1);
    assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "no_video"), &flag) == XIAOTAI_OK && flag != video);
    if (!video && !voice_geometry) return;
    scene_string(&doc, "aspect_ratio", "4:3");
    scene_string(&doc, "object_fit", "contain");
    assert(xiaotai_json_int(&doc, xiaotai_json_get(&doc, 0, "camera_rotation"), &value) == XIAOTAI_OK && value == (video ? 90 : 0));
    assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "hor_mirror"), &flag) == XIAOTAI_OK && flag == video);
    assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "vert_mirror"), &flag) == XIAOTAI_OK && !flag);
    assert(xiaotai_json_find(&doc, 0, "camera_rotation") > xiaotai_json_find(&doc, 0, "down_video_mt"));
}

static void test_profile(void *ctx)
{
    const bool voice_geometry = ctx != NULL;
    char out[384];
    assert(s.profile_writer);
    const bool video = test_scene && !test_scene_fail;
    int n = s.profile_writer(out, sizeof(out), s.profile_ctx);
    assert(n > 0);
    scene_members(out, (size_t)n, video, voice_geometry);
    memset(out, '@', sizeof(out));
    assert(s.profile_writer(out, (size_t)n, s.profile_ctx) == n && out[n] == '@');
    scene_members(out, (size_t)n, video, voice_geometry);
    const int short_write = s.profile_writer(out, (size_t)n - 1U, s.profile_ctx);
    if (video) {
        assert(short_write > 0 && short_write < n);
        scene_members(out, (size_t)short_write, false, voice_geometry);
    } else assert(short_write < 0);
    call_diagnostic_counters_t diag;
    call_diagnostic_counters(&diag);
    assert(diag.profile_fallbacks == (test_scene_fail ? 3U : video ? 1U : 0U));
}

static void assert_first_video(const xiaotai_link_media_t *v)
{
    assert(v->send && v->send_stream == video_part.send_stream && v->send_format.codec == video_part.send_format.codec);
    assert(v->receive && v->receive_stream == video_part.receive_stream && v->receive_request &&
           v->receive_formats[0].codec == video_part.receive_formats[0].codec);
}

static void test_listen(void *unused)
{
    (void)unused;
    assert(s.listener_attempts == (s.contribute ? 2U : 1U));
    assert(s.listener_count == (s.contribute && !s.unsupported_video ? 2U : 1U));
    assert(!strcmp(s.listener[0].name, "call") && !s.listener[0].profile.video.send);
    const unsigned attempts = s.listener_attempts;
    xiaotai_module_call_start();
    assert(s.listener_attempts == attempts);
    call_diagnostic_counters_t diag;
    call_diagnostic_counters(&diag);
    assert(diag.video_parts_ignored == (unsigned)test_duplicate_video);
    assert(diag.listen_failures == (unsigned)s.unsupported_video);
    if (s.listener_count == 2) {
        assert(!strcmp(s.listener[1].name, "call.video"));
        assert_first_video(&s.listener[1].profile.video);
        test_video(NULL);
    } else {
        assert(xiaotai_call_dial("peer", XIAOTAI_CALL_VIDEO, NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
        assert(xiaotai_call_dial("peer", 0, NULL) == XIAOTAI_OK);
    }
}

static void test_video_connect(void *unused)
{
    (void)unused;
    signal_at("call_incoming", "", "{\"payload\":{\"room_id\":\"d_room\",\"caller_id\":\"peer\","
                                     "\"call_type\":\"video\"}}", BINDING);
    assert(xiaotai_call_accept(0) == XIAOTAI_OK);
    token_reply();
    assert_first_video(&s.connection_profile.video);
}

static struct {
    const char *input;
    size_t remaining, used;
    char output[2048];
} wire;

static size_t at_read(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)ctx;
    if (!wire.remaining) {
        atomic_fetch_add(&usec, (int64_t)(timeout_ms ? timeout_ms : 1) * 1000);
        return 0;
    }
    const size_t n = wire.remaining < capacity ? wire.remaining : capacity;
    memcpy(data, wire.input, n);
    wire.input += n;
    wire.remaining -= n;
    return n;
}

static xiaotai_err_t at_write(void *ctx, const char *data, size_t size)
{
    (void)ctx;
    assert(!xiaotai_on_owner());
    assert(size >= 2 && !memcmp(data + size - 2, "\r\n", 2));
    assert(wire.used + size < sizeof(wire.output));
    memcpy(wire.output + wire.used, data, size);
    wire.used += size;
    wire.output[wire.used] = 0;
    return XIAOTAI_OK;
}

static const char *at_command(const char *line)
{
    wire.input = line;
    wire.remaining = strlen(line);
    wire.used = 0;
    wire.output[0] = '\0';
    assert(xiaotai_at_service(20) == XIAOTAI_OK);
    assert(!wire.remaining);
    return wire.output;
}

static void at_expect(const char *line, const char *expected)
{
    const char *result = at_command(line);
    if (strcmp(result, expected)) fprintf(stderr, "%s -> [%s], expected [%s]\n", line, result, expected);
    assert(!strcmp(result, expected));
}

static void summary_write(void *ctx, const char *text)
{
    unsigned *lines = ctx;
    if ((*lines)++ == 0) assert(!strncmp(text, "+CALL: kind=device ", 19));
    else assert(!strncmp(text, "+CALL-DIAG:", 11) && strstr(text, "profile_fallbacks=0"));
}

static void test_registration(void *unused)
{
    (void)unused;
    unsigned lines = 0;
    const xiaotai_reply_t out = {.write = summary_write, .ctx = &lines};
    xiaotai_diag_report("call", &out);
    assert(lines == 2);
}

static void test_call_help(const char *reply)
{
    FILE *file = fopen(XIAOTAI_TEST_CALL_MANIFEST, "rb");
    assert(file);
    char json[2048];
    const size_t n = fread(json, 1, sizeof(json), file);
    assert(n && n < sizeof(json) && feof(file));
    fclose(file);
    xiaotai_json_token_t tokens[160];
    xiaotai_json_doc_t doc;
    assert(xiaotai_json_parse(json, n, tokens, 160, NULL, &doc) == XIAOTAI_OK);
    const int commands = xiaotai_json_get(&doc, 0, "commands");
    assert(commands > 0 && tokens[commands].type == XIAOTAI_JSON_ARRAY && tokens[commands].children == 7);
    size_t call_lines = 0;
    const char *line = reply;
    while (!strncmp(line, "+HELP: ", 7)) {
        const char *end = strstr(line, "\r\n");
        assert(end);
        if (!strncmp(line, "+HELP: AT+CALL", 14) && (line[14] == ' ' || line[14] == '\r')) ++call_lines;
        line = end + 2;
    }
    assert(call_lines == 7 && !strcmp(line, "OK\r\n"));
    for (size_t i = 0; i < call_lines; ++i) {
        const int entry = xiaotai_json_at(&doc, commands, i);
        const int verb = xiaotai_json_get(&doc, entry, "verb");
        const int parameters = xiaotai_json_get(&doc, entry, "usage");
        assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, entry, "command"), "AT+CALL"));
        char word[32] = "", usage[96] = "", expected[160];
        if (verb >= 0) assert(xiaotai_json_string(&doc, verb, word, sizeof(word), NULL) == XIAOTAI_OK);
        if (parameters >= 0) assert(xiaotai_json_string(&doc, parameters, usage, sizeof(usage), NULL) == XIAOTAI_OK);
        const int length = snprintf(expected, sizeof(expected), "+HELP: AT+CALL%s%s%s%s\r\n",
                                    *word ? " " : "", word, *usage ? " " : "", usage);
        assert(length > 0 && (size_t)length < sizeof(expected));
        const char *found = strstr(reply, expected);
        assert(found && !strstr(found + length, expected));
    }
}

static void toggle_foreign(void *arg) { other_call = arg != NULL; }

static void setting_changed(const char *name, void *ctx)
{
    (void)ctx;
    ++s.mic_setting_changes;
    int32_t value;
    assert(xiaotai_setting_get_int(name, &value) == XIAOTAI_OK);
    if (!mic_preaccepted) assert(xiaotai_media_set_mic_muted(value != 0) == XIAOTAI_OK);
    xiaotai_args_t args = {.value = value};
    snprintf(args.text, sizeof(args.text), "%s", name);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, &args, NULL, 0) == XIAOTAI_OK);
}

static void count_call_state(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->size == sizeof(xiaotai_call_status_t));
    ++s.call_states;
}

static void count_message(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->size == sizeof(xiaotai_call_message_t));
    const xiaotai_call_message_t *m = event->data;
    assert((uint32_t)event->args.value == m->session);
    ++s.messages_in;
    s.message_session = m->session;
    s.message_type = m->type;
    memcpy(s.message_text, m->data, m->size);
    s.message_text[m->size] = '\0';
}

static xiaotai_err_t init_settings(void)
{
    const xiaotai_setting_t setting = {
        .name = XIAOTAI_SETTING_AUDIO_MIC_MUTED, .key = "call_test_mute", .kind = XIAOTAI_SETTING_BOOL,
        .min = 0, .max = 1, .default_value = 1, .changed = setting_changed,
    };
    xiaotai_err_t err = xiaotai_setting_register(&setting);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_CALL_STATE, count_call_state, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_CALL_MESSAGE, count_message, NULL);
    return err;
}

static void fence(void *unused) { (void)unused; xiaotai_signal_give(&fenced); }
static void sync_owner(void (*fn)(void *), void *ctx)
{
    assert(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    for (unsigned i = 0; i < 2; ++i) {
        assert(xiaotai_run(fence, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fenced, 3000));
    }
}

static void set_time(void *context) { atomic_store(&usec, *(const int64_t *)context); }
static void advance(int64_t time) { sync_owner(set_time, &time); }

static void published_online(void *unused)
{
    (void)unused;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
}

static void check_release_request(const char *verb, uint32_t flags)
{
    char path[40];
    snprintf(path, sizeof(path), "/v1/call/%s", verb);
    assert(!strcmp(s.path, path) && s.flags == flags && s.timeout == 4000);
    assert(!strcmp(verb, "room") ? s.method == XIAOTAI_PLATFORM_GET && !s.body[0] :
        s.method == XIAOTAI_PLATFORM_POST && strstr(s.body, "\"room_id\":\"d_room\""));
}

static void retire_uncertain_http(void *case_index)
{
    const bool conflict = case_index != NULL;
    start_outgoing();
    const http_callback_t dial = take_reply(false);
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK && status().releases_pending == 1);
    const unsigned requests = s.requests;
    deliver(dial, BINDING, XIAOTAI_OK, conflict ? 409 : 201, conflict ? 40202 : 200,
            conflict ? occupied : "{\"code\":200,\"data\":{\"room_id\":\"d_new\"}}");
    assert(status().state == XIAOTAI_CALL_IDLE && status().releases_pending == 1);
    assert(s.requests == requests && !s.reply && !s.release_reply && !s.link);
}

static void verify_uncertain_http(void *unused)
{
    (void)unused;
    check_release_request("room", 0);
    const unsigned requests = s.requests;
    reply(200, 200, empty_room);
    assert(status().state == XIAOTAI_CALL_IDLE && !status().releases_pending);
    assert(s.requests == requests && !s.reply && !s.release_reply && !s.link);
}

static void cancel_known(void *unused)
{
    (void)unused;
    start_outgoing();
    room_reply();
    s.require_cancel_block = true;
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK);
    assert(status().state == XIAOTAI_CALL_IDLE && status().releases_pending == 1);
    release_is("cancel", NULL);
    assert(s.mode[0] == XIAOTAI_LINK_LISTEN_OFF && s.inbound_blocks == 1U && !xiaotai_activity_current(NULL));
}

static void no_such_room(void *unused)
{
    (void)unused;
    deliver(take_reply(true), BINDING, XIAOTAI_OK, 200, 40400, "{\"code\":40400}");
    assert(status().releases_pending == 1);
    signal_at("call_cancel", "", "{\"payload\":{\"room_id\":\"d_other\"}}", BINDING);
    assert(status().releases_pending == 1);
}

static void answered_room(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_room\",\"role\":\"caller\",\"status\":\"answered\"}}");
}

static void active_room(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_room\",\"role\":\"caller\",\"status\":\"active\"}}");
}

static void retry_cancel(void *unused)
{
    (void)unused;
    check_release_request("cancel", XIAOTAI_PLATFORM_BACKGROUND | XIAOTAI_PLATFORM_URGENT);
    deliver(take_reply(true), BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
}

static void retry_hangup(void *unused)
{
    (void)unused;
    check_release_request("hangup", XIAOTAI_PLATFORM_BACKGROUND | XIAOTAI_PLATFORM_URGENT);
    deliver(take_reply(true), BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
}

static void unconfirmed_is_not_dial_quota(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    assert(s.requests == 7);  // Dial plus six release/verification attempts.
    reply(201, 200, empty_room);
    assert(status().releases_unconfirmed == 1 && !status().releases_pending);
    start_outgoing();
    reply(200, 40201, "{\"code\":40201}");
    assert(status().releases_unconfirmed == 1 && !status().releases_pending);
    s.online = false;
    published_online(NULL);  // Already stale when delivered.
}

static void resume_online(void *unused)
{
    (void)unused;
    assert(!s.reply);
    s.online = true;
    published_online(NULL);
}

static void different_room(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    const unsigned requests = s.requests;
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_foreign\"}}");
    assert(!status().releases_pending && s.requests == requests && !s.reply && !s.release_reply);
}

static void other_activity(void *unused)
{
    (void)unused;
    const xiaotai_activity_t recognition = {.name = "recognition", .resources = XIAOTAI_RESOURCE_AUDIO_IN};
    assert(xiaotai_activity_begin(&recognition, &s.other_activity) == XIAOTAI_OK);
    s.refuse_background = true;
}

static void first_ack(void *extra)
{
    (void)extra;
    deliver(take_reply(true), BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
    assert(status().releases_pending == 1);
}

static void end_sync_busy(void *queued)
{
    assert(s.bg_refused == 1 && !s.reply && s.requests == (queued ? 3U : 2U));
    xiaotai_activity_end(s.other_activity);
}

static void async_busy(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    other_activity(NULL);
    deliver(take_reply(false), BINDING, XIAOTAI_ERR_BUSY, 0, XIAOTAI_PLATFORM_NO_CODE, NULL);
}

static void end_async_busy(void *unused)
{
    (void)unused;
    assert(!s.reply && s.requests == 3 && !s.bg_refused);
    xiaotai_activity_end(s.other_activity);
}

static void rejected_check_envelope(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"not-a-room\"}}");
    assert(status().releases_pending == 1 && !status().releases_unconfirmed && !s.release_reply);
}

static void budget_after_both_busy_paths(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    assert(s.bg_refused == 1 && s.requests == 8); // Dial, POST, queued BUSY and five real GETs.
    reply(200, 200, empty_room);
    assert(!status().releases_pending && !status().releases_unconfirmed);
}

static void confirmed_but_hold(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, empty_room);
    assert(!status().releases_pending && s.inbound_blocks == 1U && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF);
    assert(xiaotai_call_answer_hold_ms() > 0 && xiaotai_call_answer_hold_ms() <= 10000U);
    open_inbound();
    open_inbound();
    assert(!s.link && s.blocked_ingress == 2 && !status().stale_calls && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF);
    start_outgoing();  // New foreground dial is admitted; it does not own a late link.
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_next\"}}");
    signal_at("callee_answered", "", "{\"payload\":{\"room_id\":\"d_next\"}}", BINDING);
}

static void still_hold(void *unused)
{
    (void)unused;
    open_inbound();
    assert(!s.link && status().state == XIAOTAI_CALL_OUTGOING && !s.attachment_link && s.blocked_ingress == 3 &&
           s.mode[0] == XIAOTAI_LINK_LISTEN_ONCE && s.inbound_blocks == 1U);
}

static void new_dial_link(void *unused)
{
    (void)unused;
    open_inbound();
    assert(s.link && status().state == XIAOTAI_CALL_ACTIVE && s.attachment_link == s.link && !s.inbound_blocks);
}

static void hold_still_blocked(void *unused)
{
    (void)unused;
    assert(s.inbound_blocks == 1U && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF);
}

static void hold_has_ended(void *unused)
{
    (void)unused;
    assert(!s.inbound_blocks && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF && !xiaotai_call_answer_hold_ms());
}

static void failed_cancel_submission(void *unused)
{
    (void)unused;
    start_outgoing();
    room_reply();
    s.require_cancel_block = true;
    s.submit_error = XIAOTAI_ERR_NO_MEM;
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK);
    assert(!s.inbound_blocks && !s.release_reply && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF);
    assert(status().releases_pending == 1);
}

static void replace_cancel_binding(void *unused)
{
    (void)unused;
    assert(s.inbound_blocks == 1U && status().releases_pending == 1);
    ++s.binding;
    published_online(NULL);
}

static void check_replaced_cancel_binding(void *unused)
{
    (void)unused;
    assert(!s.inbound_blocks && !status().releases_pending);
    deliver(take_reply(true), BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
    assert(!s.inbound_blocks && !status().releases_pending);
}

static http_callback_t original_dial, lost_release;

static void cancel_unknown(void *unused)
{
    (void)unused;
    start_outgoing();
    original_dial = take_reply(false);
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK && status().releases_pending == 1);
    assert(s.mode[0] == XIAOTAI_LINK_LISTEN_OFF);
}

static void before_lost_dial(void *unused) { (void)unused; assert(s.requests == 1 && !s.reply); }

static void timer_unused(void *unused) { (void)unused; assert(false); }

static void fill_timer_table(void)
{
    uint32_t timer;
    unsigned added = 0;
    xiaotai_err_t err;
    while ((err = xiaotai_timer_start(60000, timer_unused, NULL, &timer)) == XIAOTAI_OK) ++added;
    assert(added && err == XIAOTAI_ERR_NO_MEM);
}

static void release_without_timer_capacity(void *unused)
{
    (void)unused;
    cancel_unknown(NULL);
    fill_timer_table();
    const unsigned requests = s.requests;
    deliver(original_dial, BINDING, XIAOTAI_OK, 200, 200, success_room);
    /* Late dial has a room to cancel, but no slot can promise its hold. */
    assert(s.requests == requests && !s.release_reply && !s.inbound_blocks);
    assert(status().releases_pending == 1 && status().state == XIAOTAI_CALL_IDLE);
}

static void confirm_room_for_hold_test(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, empty_room);
    assert(!status().releases_pending && s.inbound_blocks == 1U);
}

static void renew_hold_with_full_timer_table(void *unused)
{
    (void)unused;
    fill_timer_table();
    bool prepared = true;
    assert(call_hold_prepare(false, &prepared) == XIAOTAI_OK && !prepared);
    call_hold_after_submit(prepared, false); // A refused repeat does not withdraw the existing promise.
    assert(s.inbound_blocks == 1U);
    call_hold_after_submit(prepared, true); // Accepted repeat extends the one reserved timer.
    assert(s.inbound_blocks == 1U);
}

static void late_dial_during_check(void *unused)
{
    (void)unused;
    check_release_request("room", 0);
    const http_callback_t superseded = take_reply(false);
    deliver(original_dial, BINDING, XIAOTAI_OK, 200, 200, success_room);
    check_release_request("cancel", XIAOTAI_PLATFORM_BACKGROUND | XIAOTAI_PLATFORM_URGENT);
    lost_release = take_reply(true);
    deliver(superseded, BINDING, XIAOTAI_OK, 200, 200, empty_room);
    assert(status().releases_pending == 1 && s.requests == 3);
}

static void after_lost_release(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    deliver(lost_release, BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
    assert(status().releases_pending == 1 && s.requests == 4);
    // Same room but this device is the callee: no ownership to send a follow-up cancel.
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_room\",\"role\":\"callee\",\"status\":\"active\"}}");
}

static void new_binding(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    ++s.binding;
    published_online(NULL);
}

static void stale_binding_reply(void *unused)
{
    (void)unused;
    assert(!status().releases_pending && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF && !s.inbound_blocks);
    const unsigned requests = s.requests;
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_room\",\"role\":\"caller\",\"status\":\"active\"}}");
    assert(s.requests == requests && !s.release_reply);
}

static void active_link_during_old_cleanup(void *unused)
{
    (void)unused;
    active_outgoing();
    const uint32_t active = s.link;
    deliver(original_dial, BINDING, XIAOTAI_OK, 200, 200,
            "{\"code\":200,\"data\":{\"room_id\":\"d_old\"}}");
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.link == active && s.mode[0] == XIAOTAI_LINK_LISTEN_OFF &&
           s.inbound_blocks == 1U);
    release_is("cancel", NULL);
    assert(!strcmp(s.body, "{\"room_id\":\"d_old\"}"));
}

static void active_link_after_old_cleanup(void *unused)
{
    (void)unused;
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.link && status().link == s.link && s.attachment_link == s.link &&
           !s.inbound_blocks);
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, empty_room);
    assert(!status().releases_pending);
}

static void active_room_not_released(void *unused)
{
    (void)unused;
    active_outgoing();
    const unsigned requests = s.requests;
    deliver(original_dial, BINDING, XIAOTAI_OK, 200, 200, success_room);
    assert(status().state == XIAOTAI_CALL_ACTIVE && status().releases_pending == 1);
    assert(s.requests == requests && !s.release_reply);
}

static void verify_while_room_owned(void *unused)
{
    (void)unused;
    check_release_request("room", 0);
    reply(200, 200, "{\"code\":200,\"data\":{\"room_id\":\"d_room\",\"role\":\"caller\",\"status\":\"active\"}}");
}

static void verify_owned_again(void *unused)
{
    (void)unused;
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    assert(status().state == XIAOTAI_CALL_ACTIVE && status().releases_pending == 1 && !s.release_reply);
    reply(200, 200, "{\"code\":200}"); // A missing data field is also authoritative no-room.
    assert(!status().releases_pending);
}

static void old_candidate_already_admitted(void *unused)
{
    (void)unused;
    start_outgoing();
    room_reply();
    const uint32_t new_call = status().session;
    assert(s.mode[0] == XIAOTAI_LINK_LISTEN_ONCE && !s.inbound_blocks);
    // The real transport consumed ONCE, but has not handed the link to us.
    s.mode[0] = XIAOTAI_LINK_LISTEN_OFF;
    s.link = xiaotai_generation_next();
    s.inbound_listener = 1;
    s.require_cancel_block = true;
    deliver(original_dial, BINDING, XIAOTAI_OK, 200, 200,
            "{\"code\":200,\"data\":{\"room_id\":\"d_old\"}}");
    assert(s.inbound_blocks == 1U && !status().link);
    s.listener[0].handlers.opened(s.listener[0].handlers.ctx, s.link);
    assert(!s.link && !status().link && status().session == new_call && status().stale_calls == 2);
    assert(s.mode[0] == XIAOTAI_LINK_LISTEN_ONCE && s.inbound_blocks == 1U);
}

static void boot(const xiaotai_call_config_t *config, bool settings, bool video, bool unsupported_video)
{
    static const xiaotai_module_t call = {.name = "call", .init = xiaotai_module_call_init, .start = xiaotai_module_call_start};
    static const xiaotai_module_t ext = {.name = "ext", .init = attach};
    static const xiaotai_module_t prefs = {.name = "settings", .init = init_settings};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t *modules[] = {&call, &ext, NULL, NULL};
    size_t count = 2;
    if (settings) modules[count++] = &prefs;
    if (test_shared_at) modules[count++] = &at;
    static const xiaotai_priority_t priorities[] = {{"maintenance", 4}, {"call", 3}, {"recognition", 2}, {NULL, 0}};
    const xiaotai_framework_config_t framework = {
        .modules = modules, .module_count = count, .priorities = priorities, .available_needs = UINT32_MAX,
    };
    memset(&s, 0, sizeof(s));
    atomic_store(&usec, 1000);
    s.online = s.binding_valid = s.fresh = true;
    s.binding = BINDING;
    s.contribute = video;
    s.unsupported_video = unsupported_video;
    other_call = false;
    foreign_actions = 0;
    if (test_shared_at) {
        static const xiaotai_at_io_t io = {.name = "fixture", .read = at_read, .write = at_write};
        assert(xiaotai_at_configure(&(xiaotai_at_config_t){
            .task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true,
        }) == XIAOTAI_OK);
        assert(xiaotai_at_attach(&io) == XIAOTAI_OK);
    }
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_call_configure(config) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
}

static void reset(void *unused) { (void)unused; xiaotai_call_reset(); }
static void shutdown(void)
{
    sync_owner(reset, NULL);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
}

static void begin_ring(void *unused) { (void)unused; incoming(); }
static void begin_ring_other(void *unused) { (void)unused; incoming_room("d_other"); }

static void test_at_routing(void)
{
    sync_owner(test_registration, NULL);
    assert(strstr(at_command("AT\r"), "+READY:") && strstr(wire.output, "OK\r\n"));
    test_call_help(at_command("AT+HELP\r"));
    at_expect("AT+CALL\r", "+CALL: kind=device state=idle gen=0 muted=0 end=0 peer=- error=0\r\n"
                           "+CALL: kind=wechat state=idle gen=0 muted=0 end=0 peer=- error=0\r\nOK\r\n");
    at_expect("AT+CALL ACCEPT\r", "ERROR: INVALID_STATE NO_OWNER\r\n");
    at_expect("AT+CALL REJECT\r", "ERROR: INVALID_STATE NO_OWNER\r\n");
    at_expect("AT+CALL HANGUP\r", "ERROR: INVALID_STATE NO_OWNER\r\n");
    at_expect("AT+CALL MUTE 0\r", "ERROR: INVALID_STATE NO_OWNER\r\n");
    at_expect("AT+CALL INDEX 256\r", "ERROR: INVALID_ARG\r\n");
    at_expect("AT+CALL INDEX 2\r", "ERROR: INVALID_ARG\r\n");
    at_expect("AT+CALL INDEX bad\r", "ERROR: INVALID_ARG\r\n");
    at_expect("AT+CALL DEVICE bad/id\r", "ERROR: INVALID_ARG\r\n");
    at_expect("AT+CALL MUTE 00\r", "ERROR: INVALID_ARG\r\n");
    sync_owner(toggle_foreign, &test_shared_at);
    at_expect("AT+CALL INDEX 1\r", "OK\r\n");
    at_expect("AT+CALL INDEX 2\r", "ERROR: INVALID_ARG\r\n");
    at_expect("AT+CALL ACCEPT\r", "OK\r\n");
    at_expect("AT+CALL REJECT\r", "OK\r\n");
    at_expect("AT+CALL HANGUP\r", "OK\r\n");
    at_expect("AT+CALL MUTE 1\r", "OK\r\n");
    assert(foreign_actions == 5 && s.requests == 0);
    sync_owner(toggle_foreign, NULL);
    at_expect("AT+CALL INDEX 0\r", "OK\r\n");
    assert(s.requests == 1 && !strcmp(s.body, "{\"targets\":[\"peer\"],\"call_type\":\"audio\"}"));
    const char *text = at_command("AT+CALL\r");
    assert(strstr(text, "kind=device state=outgoing gen=") && strstr(text, "end=0 peer=peer error=0\r\n"));
    at_expect("AT+CALL HANGUP\r", "OK\r\n");
    assert(strstr(at_command("AT+CALL\r"), "kind=device state=ended gen=0"));
}

static void test_at_incoming(void)
{
    (void)at_command("AT\r");
    sync_owner(begin_ring, NULL);
    assert(strstr(at_command("AT+CALL\r"), "kind=device state=incoming gen="));
    at_expect("AT+CALL MUTE 1\r", "OK\r\n");
    assert(strstr(at_command("AT+CALL\r"), "muted=1 end=0 peer=peer error=0\r\n"));
    at_expect("AT+CALL MUTE 0\r", "OK\r\n");
    at_expect("AT+CALL REJECT\r", "OK\r\n");
    assert(!strcmp(s.path, "/v1/call/reject") && strstr(s.body, "\"reason\":\"decline\""));
    sync_owner(begin_ring_other, NULL);
    at_expect("AT+CALL ACCEPT\r", "OK\r\n");
    assert(strstr(at_command("AT+CALL\r"), "kind=device state=connecting gen="));
}

static void test_at_device(void)
{
    (void)at_command("AT\r");
    at_expect("AT+CALL DEVICE missing\r", "ERROR: NOT_FOUND\r\n");
    at_expect("AT+CALL DEVICE duplicate\r", "ERROR: INVALID_STATE\r\n");
    assert(s.requests == 0 && !status().session && status().state == XIAOTAI_CALL_IDLE);
    at_expect("AT+CALL DEVICE peer\r", "OK\r\n");
    assert(s.requests == 1 && !strcmp(s.path, "/v1/call/request"));
}

static void owner_hangup_incoming(void *unused)
{
    (void)unused;
    const xiaotai_call_status_t ringing = status();
    assert(ringing.state == XIAOTAI_CALL_INCOMING);
    assert(xiaotai_call_hangup(ringing.session) == XIAOTAI_OK);
}

static void begin_active(void *unused) { (void)unused; active_outgoing(); }

enum {HOLD_DIALER, HOLD_CALLEE, HOLD_PEER_END, HOLD_UNSUPPORTED, HOLD_POST_DENIED, HOLD_EXPIRED, HOLD_ACTIVE_EXPIRED,
      HOLD_ACTIVE_FAILURE};

static void signaling_setup(void *context)
{
    const uintptr_t which = (uintptr_t)context;
    assert(s.listener[0].profile.track_peer_retirement);
    if (which == HOLD_CALLEE || which == HOLD_PEER_END || which == HOLD_ACTIVE_FAILURE) {
        incoming();
        assert(status().state == XIAOTAI_CALL_INCOMING && !s.hold_calls);
        assert(xiaotai_call_accept(0) == XIAOTAI_OK);
        token_reply();
        assert(status().state == XIAOTAI_CALL_CONNECTING && !s.hold_calls);
        assert(s.connection_profile.track_peer_retirement);
        s.connected.opened(s.connected.ctx, s.link);
    } else {
        active_outgoing();
    }
    assert(status().state == XIAOTAI_CALL_ACTIVE && !s.hold_calls);
    if (which == HOLD_UNSUPPORTED) s.watch_error = XIAOTAI_ERR_NOT_SUPPORTED;
}

static void signaling_one_direction(void *unused)
{
    (void)unused;
    assert(!s.hold_calls && !s.watched_link);
    s.tx = 1;
}

static void signaling_two_directions(void *unused)
{
    (void)unused;
    assert(!s.hold_calls && !s.watched_link);
    s.rx = 1;
    s.pcm_written = 160;
}

static void signaling_end_call(void *context)
{
    const uintptr_t which = (uintptr_t)context;
    assert(status().state == XIAOTAI_CALL_ACTIVE);
    if (which == HOLD_UNSUPPORTED) {
        assert(!s.platform_held && !s.watched_link && !s.hold_calls);
        assert(xiaotai_call_hangup(0) == XIAOTAI_OK && s.hangup_posts == 1);
        return;
    }
    assert(s.platform_held && s.watched_link == status().link && s.hold_calls == 1U);
    s.hold_callback(XIAOTAI_OK, s.hold_context);
    if (which == HOLD_PEER_END) {
        const xiaotai_link_command_t end = {.word = CALL_COMMAND_END};
        s.connected.command(s.connected.ctx, s.link, &end);
        assert(status().end == XIAOTAI_CALL_END_PEER_HANGUP);
        assert(!status().releases_pending);
    } else if (which == HOLD_ACTIVE_FAILURE) {
        s.media.event(s.link, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL, s.media.ctx);
        assert(status().end == XIAOTAI_CALL_END_AUDIO && status().releases_pending == 1);
    } else {
        assert(xiaotai_call_hangup(0) == XIAOTAI_OK);
        assert(status().end == XIAOTAI_CALL_END_LOCAL && status().releases_pending == 1);
    }
    assert(status().state == XIAOTAI_CALL_IDLE && !s.hangup_posts && s.platform_held && !s.resume_calls);
}

static void signaling_close_witness(void *context)
{
    assert(!s.link && !status().releasing && !s.hangup_posts && !s.resume_calls && s.platform_held);
    if ((uintptr_t)context == HOLD_EXPIRED) {
        s.platform_held = false;
        published_online(NULL);
        assert(!s.hangup_posts);
    }
    s.retirement = XIAOTAI_LINK_RELEASE_COMPLETE;
}

static void signaling_return_online(void *context)
{
    const uintptr_t which = (uintptr_t)context;
    assert(!s.platform_held && !s.hangup_posts && !s.watched_link);
    if (which == HOLD_EXPIRED) {
        assert(!s.resume_calls && status().releases_unconfirmed == 1);
        return;
    }
    assert(s.reconnecting && s.resume_calls == 1);
    s.reconnecting = false;
    if (which == HOLD_POST_DENIED) s.submit_error = XIAOTAI_ERR_BUSY;
    published_online(NULL);
}

static void signaling_post_reply(void *context)
{
    const uintptr_t which = (uintptr_t)context;
    if (which == HOLD_PEER_END || which == HOLD_EXPIRED) {
        assert(s.hangup_posts == 0);
        return;
    }
    assert(s.hangup_posts == 1 && !s.reconnecting && !s.platform_held);
    release_is("hangup", which == HOLD_ACTIVE_FAILURE ? "p2p_error" : "hangup");
    deliver(take_reply(true), BINDING, XIAOTAI_OK, 200, 200, "{\"code\":200}");
}

static void signaling_query(void *unused)
{
    (void)unused;
    assert(s.hangup_posts == 1U);
    answered_room(NULL);
    published_online(NULL);
}

static void signaling_query_no_duplicate(void *unused)
{
    (void)unused;
    assert(s.hangup_posts == 1U);
    check_release_request("room", XIAOTAI_PLATFORM_BACKGROUND);
    reply(200, 200, empty_room);
    assert(!status().releases_pending);
}

static void signaling_retry_denial(void *unused)
{
    (void)unused;
    assert(s.urgent_attempts == 1 && !s.hangup_posts && !s.release_reply);
    s.submit_error = XIAOTAI_OK;
}

static void signaling_expire_active(void *unused)
{
    (void)unused;
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.platform_held && s.hold_calls == 1);
    s.platform_held = false;
    published_online(NULL);
}

static void signaling_hangup_after_expiry(void *unused)
{
    (void)unused;
    assert(status().state == XIAOTAI_CALL_ACTIVE && s.watched_link && !s.platform_held);
    xiaotai_eventlog_entry_t entries[16];
    const size_t count = xiaotai_eventlog_read(0, entries, sizeof(entries) / sizeof(entries[0]));
    bool found = false;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(entries[i].label, "signaling.lease_expired") && entries[i].generation == status().session) found = true;
    assert(found);
    assert(xiaotai_call_hangup(0) == XIAOTAI_OK && status().releases_unconfirmed == 1);
    assert(!s.watched_link && !s.resume_calls && !s.hangup_posts);
}

static void test_signaling_hold(void)
{
    const xiaotai_call_config_t config = {.pause_platform_signaling_while_active = true};
    for (uintptr_t which = HOLD_DIALER; which <= HOLD_ACTIVE_FAILURE; ++which) {
        void *ctx = (void *)which;
        boot(&config, false, false, false);
        sync_owner(signaling_setup, ctx);
        advance(81000);
        sync_owner(signaling_one_direction, NULL);
        advance(161000);
        sync_owner(signaling_two_directions, NULL);
        advance(241000);
        if (which == HOLD_ACTIVE_EXPIRED) {
            sync_owner(signaling_expire_active, NULL);
            advance(491000);
            sync_owner(signaling_hangup_after_expiry, NULL);
            shutdown();
            continue;
        }
        sync_owner(signaling_end_call, ctx);
        if (which != HOLD_UNSUPPORTED) {
            advance(501000);
            sync_owner(signaling_close_witness, ctx);
            advance(600000);
            sync_owner(signaling_return_online, ctx);
            advance(602000);
            int64_t sent = 602000;
            if (which == HOLD_POST_DENIED) {
                sync_owner(signaling_retry_denial, NULL);
                sent += 1000000;
                advance(sent);
            }
            sync_owner(signaling_post_reply, ctx);
            if (which == HOLD_DIALER || which == HOLD_CALLEE || which == HOLD_POST_DENIED) {
                advance(sent + 2000);
                sync_owner(signaling_query, NULL);
                advance(sent + 1100000);
                sync_owner(signaling_query_no_duplicate, NULL);
            }
        }
        shutdown();
    }
}

static void test_at_hangup(bool reject_incoming)
{
    (void)at_command("AT\r");
    sync_owner(begin_ring, NULL);
    const uint32_t ringing = status().session;
    assert(ringing && status().state == XIAOTAI_CALL_INCOMING);
    const unsigned before = s.requests;
    at_expect("AT+CALL HANGUP\r", reject_incoming ? "ERROR: INVALID_STATE\r\n" : "OK\r\n");
    if (reject_incoming) {
        assert(status().session == ringing && status().state == XIAOTAI_CALL_INCOMING);
        assert(s.requests == before && !s.release_reply && !s.link);
        sync_owner(owner_hangup_incoming, NULL);
    }
    assert(status().state == XIAOTAI_CALL_IDLE && s.requests == before + 1U);
    release_is("reject", "decline");
}

static void test_at_hangup_connecting(void)
{
    (void)at_command("AT\r");
    sync_owner(begin_ring_other, NULL);
    at_expect("AT+CALL ACCEPT\r", "OK\r\n");
    assert(status().state == XIAOTAI_CALL_CONNECTING);
    at_expect("AT+CALL HANGUP\r", "OK\r\n");
    assert(status().state == XIAOTAI_CALL_IDLE);
}

typedef enum {
    SAME_BINDING, SHIFT_IN_PREPARE, SHIFT_QUEUED, SHIFT_IN_DISPATCH,
    SHIFT_IN_PLATFORM, CONTACT_OFFLINE, CONTACT_REPLACED, CONTACT_DUPLICATE, INTERVENING_CALL,
} dial_scenario_t;

typedef struct {
    xiaotai_signal_t entered, release;
    dial_scenario_t scenario;
    unsigned requests_before;
} dial_probe_t;

static void wait_queued_dial(void *ctx)
{
    dial_probe_t *p = ctx;
    xiaotai_signal_give(&p->entered);
    assert(xiaotai_signal_take(&p->release, 3000));
    switch (p->scenario) {
    case SAME_BINDING:
        s.dial_contact_reordered = true;
        break;
    case SHIFT_QUEUED:
        ++s.binding;
        break;
    case SHIFT_IN_DISPATCH:
        s.dial_change_on_lookup = true;
        break;
    case SHIFT_IN_PLATFORM:
        s.dial_change_on_submit = true;
        break;
    case CONTACT_OFFLINE:
        s.dial_contact_offline = true;
        break;
    case CONTACT_REPLACED:
        s.dial_contact_reordered = s.dial_contact_replaced = true;
        break;
    case CONTACT_DUPLICATE:
        s.dial_contact_duplicate = true;
        break;
    case INTERVENING_CALL:
        incoming_room("dial-epoch-test");
        assert(xiaotai_call_reject(status().session) == XIAOTAI_OK);
        assert(status().state == XIAOTAI_CALL_IDLE && !status().session);
        break;
    case SHIFT_IN_PREPARE:
        break;
    }
    p->requests_before = s.requests;
}

static void check_queued_dial(void *ctx)
{
    const dial_probe_t *p = ctx;
    if (p->scenario == SAME_BINDING) {
        xiaotai_contact_t first;
        assert(xiaotai_contacts_at(0, XIAOTAI_CONTACTS_STALE_OK, &first) == XIAOTAI_OK && !strcmp(first.id, "other"));
        assert(status().state == XIAOTAI_CALL_OUTGOING && !strcmp(status().peer_id, "peer"));
        assert(s.requests == p->requests_before + 1 && s.guarded_dials == 1);
        assert(!strcmp(s.path, "/v1/call/request") && !strcmp(s.body, "{\"targets\":[\"peer\"],\"call_type\":\"audio\"}"));
    } else {
        if (p->scenario == CONTACT_REPLACED) {
            xiaotai_contact_t replacement;
            assert(xiaotai_contacts_at(0, XIAOTAI_CONTACTS_STALE_OK, &replacement) == XIAOTAI_OK &&
                   !strcmp(replacement.id, "other"));
        }
        assert(s.requests == p->requests_before && status().state == XIAOTAI_CALL_IDLE && !status().session);
        assert(!xiaotai_activity_current(NULL) && !status().dialled);
        assert(s.guarded_dials == (p->scenario == SHIFT_IN_PLATFORM ? 1U : 0U));
    }
    printf("guarded dial: scenario=%u binding=%u wire requests=%u platform checks=%u\n", (unsigned)p->scenario,
           (unsigned)s.binding, s.requests - p->requests_before, s.guarded_dials);
}

static void test_queued_dial(void)
{
    for (dial_scenario_t scenario = SAME_BINDING; scenario <= INTERVENING_CALL; ++scenario) {
        dial_probe_t probe = {.scenario = scenario};
        assert(xiaotai_signal_init(&probe.entered) == XIAOTAI_OK && xiaotai_signal_init(&probe.release) == XIAOTAI_OK);
        boot(NULL, false, false, false);
        assert(xiaotai_run(wait_queued_dial, &probe) == XIAOTAI_OK && xiaotai_signal_take(&probe.entered, 3000));
        if (scenario == SHIFT_IN_PREPARE) s.dial_change_on_lookup = true;
        xiaotai_args_t args = {.text = "peer"};
        if (scenario == SAME_BINDING) {
            assert(xiaotai_intent_post(XIAOTAI_INTENT_CALL_DIAL, NULL) == XIAOTAI_ERR_INVALID_ARG);
            assert(xiaotai_intent_post(XIAOTAI_INTENT_CALL_DIAL, &(xiaotai_args_t){.text = "missing"}) ==
                   XIAOTAI_ERR_NOT_FOUND);
            assert(xiaotai_intent_post(XIAOTAI_INTENT_CALL_DIAL, &(xiaotai_args_t){.text = "pe"}) ==
                   XIAOTAI_ERR_NOT_FOUND);
            assert(xiaotai_intent_post(XIAOTAI_INTENT_CALL_DIAL, &(xiaotai_args_t){.text = "duplicate"}) ==
                   XIAOTAI_ERR_INVALID_STATE);
        }
        const xiaotai_err_t posted = scenario == SAME_BINDING ?
            xiaotai_intent_post_external(XIAOTAI_INTENT_CALL_DIAL, &args) :
            xiaotai_intent_post(XIAOTAI_INTENT_CALL_DIAL, &args);
        assert(posted == (scenario == SHIFT_IN_PREPARE ? XIAOTAI_ERR_PLATFORM_STALE : XIAOTAI_OK));
        strcpy(args.text, "other");
        xiaotai_signal_give(&probe.release);
        sync_owner(check_queued_dial, &probe);
        shutdown();
        xiaotai_signal_deinit(&probe.release);
        xiaotai_signal_deinit(&probe.entered);
    }
}

int main(int argc, char **argv)
{
#ifdef CALL_TEST_HEADLESS
    const char *case_name = argc > 1 ? argv[1] : "mute_no_settings";
#else
    const char *case_name = argc > 1 ? argv[1] : "incoming";
#endif
    assert(xiaotai_signal_init(&fenced) == XIAOTAI_OK);
    if (!strcmp(case_name, "queued_dial")) {
        test_queued_dial();
        xiaotai_signal_deinit(&fenced);
        return 0;
    }
    bool option = false;
    xiaotai_call_config_t config = {0};
    if (!strcmp(case_name, "profile") || !strcmp(case_name, "profile_voice_geometry")) {
        config.voice_profile_geometry = !strcmp(case_name, "profile_voice_geometry");
        xiaotai_call_config_t *scene_ctx = config.voice_profile_geometry ? &config : NULL;
        boot(scene_ctx, false, false, false);
        sync_owner(test_profile, scene_ctx);
        shutdown();
        test_scene = true;
        boot(scene_ctx, false, true, false);
        sync_owner(test_profile, scene_ctx);
        shutdown();
        test_scene_fail = true;
        boot(scene_ctx, false, true, false);
        sync_owner(test_profile, scene_ctx);
    } else if (!strcmp(case_name, "listen")) {
        boot(NULL, false, false, false);
        sync_owner(test_listen, NULL);
        shutdown();
        test_duplicate_video = true;
        boot(NULL, false, true, false);
        sync_owner(test_listen, NULL);
        shutdown();
        boot(NULL, false, true, false);
        sync_owner(test_video_connect, NULL);
        shutdown();
        test_duplicate_video = false;
        boot(NULL, false, true, true);
        sync_owner(test_listen, NULL);
    } else if (!strcmp(case_name, "message")) {
        boot(NULL, true, false, false);   /* the settings fixture also subscribes to call.message */
        sync_owner(test_message_send, NULL);
        sync_owner(test_message_check, NULL);
    } else if (!strcmp(case_name, "signaling_hold")) {
        test_signaling_hold();
        xiaotai_signal_deinit(&fenced);
        return 0;
    } else if (!strcmp(case_name, "at")) {
        test_shared_at = true;
        boot(NULL, false, false, false);
        test_at_routing();
        shutdown();
        boot(NULL, false, false, false);
        test_at_incoming();
        shutdown();
        boot(NULL, false, false, false);
        test_at_device();
    } else if (!strcmp(case_name, "at_hangup")) {
        const bool reject_incoming = argc > 2 && !strcmp(argv[2], "strict");
        test_shared_at = true;
        config.at_reject_incoming_hangup = reject_incoming;
        boot(&config, false, false, false);
        test_at_hangup(reject_incoming);
        shutdown();
        boot(&config, false, false, false);
        test_at_hangup_connecting();
        shutdown();
        boot(&config, false, false, false);
        (void)at_command("AT\r");
        at_expect("AT+CALL DEVICE peer\r", "OK\r\n");
        assert(status().state == XIAOTAI_CALL_OUTGOING);
        at_expect("AT+CALL HANGUP\r", "OK\r\n");
        assert(status().state == XIAOTAI_CALL_IDLE);
        shutdown();
        boot(&config, false, false, false);
        (void)at_command("AT\r");
        sync_owner(begin_active, NULL);
        at_expect("AT+CALL HANGUP\r", "OK\r\n");
        assert(status().state == XIAOTAI_CALL_IDLE && status().end == XIAOTAI_CALL_END_LOCAL);
    } else if (!strcmp(case_name, "end")) {
        for (unsigned i = 0; i < 2; ++i) {
            boot(NULL, false, false, false);
            sync_owner(test_end, i ? &option : NULL);
            advance(150999);
            sync_owner(test_no_early_disconnect, NULL);
            advance(151000);
            sync_owner(test_disconnected, NULL);
            shutdown();
        }
        for (uintptr_t i = 0; i < 4U; ++i) {
            boot(NULL, false, false, false);
            sync_owner(test_reject_stage, (void *)i);
            shutdown();
        }
        boot(NULL, false, false, false);
        sync_owner(test_revoked, NULL);
        shutdown();
        for (unsigned i = 0; i < 2; ++i) {
            option = i != 0;
            boot(NULL, false, false, false);
            sync_owner(test_remote_closed, &option);
            shutdown();
        }
        config.ring_timeout_ms = 1000;
        boot(&config, false, false, false);
        sync_owner(begin_ring, NULL);
        advance(1001000);
        sync_owner(test_missed, NULL);
    } else if (!strcmp(case_name, "legacy_shutdown")) {
        for (uintptr_t i = 0; i < 5U; ++i) {
            boot(NULL, false, false, false);
            sync_owner(test_legacy_shutdown, (void *)i);
            if (i == 2 || i == 3) {
                advance(151000);
                sync_owner(test_disconnected, NULL);
            }
            shutdown();
        }
        return 0;
    } else if (!strcmp(case_name, "accept_connection_failure")) {
        for (uintptr_t i = 0; i < 2U; ++i) {
            boot(NULL, false, false, false);
            sync_owner(test_accept_connection_failure, (void *)i);
            shutdown();
            boot(NULL, false, false, false);
            sync_owner(test_accept_failure_cleanup, (void *)i);
            shutdown();
        }
        return 0;
    } else if (!strcmp(case_name, "mute_no_settings")) {
        boot(NULL, false, false, false);
        sync_owner(test_mute, &option);
    } else if (!strcmp(case_name, "mute_policy")) {
        boot(NULL, true, false, false);
        sync_owner(start_persisted_muted_call, NULL);
        sync_owner(post_stale_mic_on, NULL);
        sync_owner(post_current_mic_on, NULL);
        sync_owner(confirm_persisted_policy, NULL);
        sync_owner(confirm_settings_authority, NULL);
    } else if (!strcmp(case_name, "accept")) {
        boot(NULL, false, false, false);
        sync_owner(test_accept, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_wrong_token, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_incoming_closed_after_answer, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_answer_send_failure, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_accept_denial, NULL);
    } else if (!strcmp(case_name, "accept_bad_room_release")) {
        failed_accept_room_case_t cases[] = {
            {"d_invalid\\\"room", true, false},
            {"d_bad\\\\room", true, false},
            {"room-new", true, false},
            {"d_", true, false},
            {"d_a-b_9", true, true},
            {"d_invalid\\\"room", false, true},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            config.strict_failed_accept_room_id = cases[i].strict;
            boot(&config, false, false, false);
            sync_owner(test_accept_bad_room_release, &cases[i]);
            shutdown();
        }
        config.strict_failed_accept_room_id = true;
        boot(&config, false, false, false);
        uint32_t foreground;
        sync_owner(test_invalid_failed_accept_keeps_release_owner, &foreground);
        advance(3000);
        sync_owner(test_previous_release_confirmation, &foreground);
    } else if (!strcmp(case_name, "dial")) {
        config.pending_releases = 8;
        boot(&config, false, false, false);
        sync_owner(test_dial_envelopes, NULL);
        shutdown();
        for (unsigned i = 0; i < 20; i += 4) {
            boot(NULL, false, false, false);
            sync_owner(test_accept_envelopes, (void *)(uintptr_t)i);
            shutdown();
        }
        boot(NULL, false, false, false);
        sync_owner(test_long_token, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_long_token, &option);
    } else if (!strcmp(case_name, "stale_room")) {
        for (unsigned i = 0; i < 4; ++i) {
            boot(NULL, false, false, false);
            sync_owner(test_repair_roles, (void *)(uintptr_t)i);
            shutdown();
        }
        boot(NULL, false, false, false);
        sync_owner(test_repair_reject_conflict, NULL);
        sync_owner(test_http_parse_budget, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_repair_authority, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_repair_staleness, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(repair_start, NULL);
        advance(15000999);
        assert(status().state == XIAOTAI_CALL_OUTGOING);
        advance(15001000);
        sync_owner(test_timed_out_repair, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(test_retired_dial, NULL);
        for (unsigned i = 0; i < 2; ++i) {
            shutdown();
            boot(NULL, false, false, false);
            sync_owner(retire_uncertain_http, (void *)(uintptr_t)i);
            advance(1001000);
            sync_owner(verify_uncertain_http, NULL);
        }
    } else if (!strcmp(case_name, "release_hold_capacity")) {
        boot(NULL, false, false, false);
        sync_owner(release_without_timer_capacity, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(cancel_known, NULL);
        sync_owner(first_ack, NULL);
        advance(2000);
        sync_owner(confirm_room_for_hold_test, NULL);
        advance(5000000);
        sync_owner(renew_hold_with_full_timer_table, NULL);
        advance(10001000);
        sync_owner(hold_still_blocked, NULL);
        advance(14999999);
        sync_owner(hold_still_blocked, NULL);
        advance(15000000);
        sync_owner(hold_has_ended, NULL);
    } else if (!strcmp(case_name, "release")) {
        config.pending_releases = 1;
        boot(&config, false, false, false);
        sync_owner(cancel_known, NULL);
        sync_owner(no_such_room, NULL);
        advance(2000);
        sync_owner(active_room, NULL);
        advance(1002000);
        sync_owner(retry_cancel, NULL);
        advance(1003000);
        sync_owner(answered_room, NULL);
        advance(2003000);
        sync_owner(retry_hangup, NULL);
        advance(2004000);
        sync_owner(unconfirmed_is_not_dial_quota, NULL);
        advance(5000000);
        sync_owner(resume_online, NULL);
        advance(5001000);
        sync_owner(different_room, NULL);
        advance(10001000);
        sync_owner(hold_still_blocked, NULL); // A second accepted cancel renewed it from 1002000.
        advance(11002000);
        sync_owner(hold_has_ended, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(failed_cancel_submission, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(cancel_known, NULL);
        sync_owner(replace_cancel_binding, NULL);
        sync_owner(check_replaced_cancel_binding, NULL);
    } else if (!strcmp(case_name, "release_deferred")) {
        for (unsigned i = 0; i < 2; ++i) {
            boot(NULL, false, false, false);
            sync_owner(cancel_known, NULL);
            if (!i) sync_owner(other_activity, NULL);
            sync_owner(first_ack, NULL);
            advance(2000);
            if (i) sync_owner(async_busy, NULL);
            advance(8000000);
            sync_owner(i ? end_async_busy : end_sync_busy, NULL);
            advance(8001000);
            sync_owner(confirmed_but_hold, NULL);
            advance(10000999);
            sync_owner(still_hold, NULL);
            advance(10001000);
            sync_owner(new_dial_link, NULL);
            shutdown();
        }
        boot(NULL, false, false, false);
        sync_owner(cancel_known, NULL);
        sync_owner(other_activity, NULL);
        sync_owner(first_ack, NULL);
        advance(2000);
        sync_owner(end_sync_busy, NULL);
        advance(3000);
        sync_owner(async_busy, NULL);
        advance(3500);
        sync_owner(end_sync_busy, &option);
        for (int64_t i = 0; i < 4; ++i) {
            advance(4500 + 1000000 * i);
            sync_owner(rejected_check_envelope, NULL);
        }
        advance(4004500);
        sync_owner(budget_after_both_busy_paths, NULL);
    } else if (!strcmp(case_name, "release_timeouts")) {
        boot(NULL, false, false, false);
        sync_owner(cancel_unknown, NULL);
        advance(10000999);
        sync_owner(before_lost_dial, NULL);
        advance(10001000);
        sync_owner(late_dial_during_check, NULL);
        advance(20001000);
        sync_owner(after_lost_release, NULL);
        advance(21001000);
        sync_owner(new_binding, NULL);
        sync_owner(stale_binding_reply, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(cancel_unknown, NULL);
        sync_owner(active_link_during_old_cleanup, NULL);
        advance(10001000);
        sync_owner(active_link_after_old_cleanup, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(cancel_unknown, NULL);
        sync_owner(active_room_not_released, NULL);
        advance(1001000);
        sync_owner(verify_while_room_owned, NULL);
        advance(2001000);
        sync_owner(verify_owned_again, NULL);
        shutdown();
        boot(NULL, false, false, false);
        sync_owner(cancel_unknown, NULL);
        sync_owner(old_candidate_already_admitted, NULL);
    } else if (!strcmp(case_name, "platform") || !strcmp(case_name, "media") || !strcmp(case_name, "mute") ||
               !strcmp(case_name, "busy_during") || !strcmp(case_name, "contacts")) {
        static const char *const busy[] = {"recognition", NULL};
        for (int i = 0; i < 2; ++i) {
            option = i != 0;
            config.end_on_platform_offline = option;
            config.uplink_noise_gate = option;
            config.require_fresh_contacts = option;
            config.incoming_busy_during = option ? busy : NULL;
            if (option) config.latency = (xiaotai_media_latency_t)XIAOTAI_MEDIA_LATENCY_LOW;
            boot(&config, option && !strcmp(case_name, "mute"), false, false);
            if (!strcmp(case_name, "platform")) {
                sync_owner(test_platform_start, NULL);
                sync_owner(test_platform_check, &option);
                if (!option) sync_owner(test_platform_binding, NULL);
            } else if (!strcmp(case_name, "media")) sync_owner(test_media, &option);
            else if (!strcmp(case_name, "busy_during")) sync_owner(test_busy, &option);
            else if (!strcmp(case_name, "contacts")) sync_owner(test_contacts, &option);
            else {
                sync_owner(test_mute, &option);
                if (option) sync_owner(test_mute_setting, NULL);
            }
            shutdown();
        }
        if (!strcmp(case_name, "media")) {
            boot(NULL, false, false, false);
            sync_owner(test_media_error, NULL);
        } else { xiaotai_signal_deinit(&fenced); return 0; }
    } else {
        boot(NULL, false, !strcmp(case_name, "incoming"), !strcmp(case_name, "incoming"));
        if (!strcmp(case_name, "incoming")) {
            sync_owner(test_incoming, NULL);
            shutdown();
            boot(NULL, false, true, false);
            sync_owner(test_video, NULL);
        }
        else if (!strcmp(case_name, "eventlog")) sync_owner(test_eventlog, NULL);
        else {
            assert(!strcmp(case_name, "outgoing_connect"));
            sync_owner(test_outgoing_connect, NULL);
            shutdown();
            boot(NULL, false, false, false);
            sync_owner(test_cloud_before_link, NULL);
        }
    }
    shutdown();
    xiaotai_signal_deinit(&fenced);
    return 0;
}
