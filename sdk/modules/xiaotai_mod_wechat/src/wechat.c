// SPDX-License-Identifier: MIT
#include "wechat_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/log.h"
#include "xiaotai/transport.h"
#ifdef XIAOTAI_WECHAT_SETTINGS
#include "xiaotai/modules/settings.h"
#endif

#define ANSWERED 0x2000U
#define HANGUP 0x2001U
#define ASK_CAMERA 0x1105U
#define REQUEST_BYTES 768U

typedef enum {NONE, REQUEST, ROOM, OPEN, ANSWER, RING, RELEASE} stage_t;

static const xiaotai_media_latency_t latency = XIAOTAI_MEDIA_LATENCY_VOIP;
static const xiaotai_wechat_reasons_t reasons = XIAOTAI_WECHAT_REASONS_DEFAULT;
static xiaotai_contact_kind_def_t kind = {
    .name = XIAOTAI_WECHAT_CONTACT_KIND, .alias = "WX", .server_type = "voip",
    .extra_fields = {"wx_app_id", "wx_model_id"},
};

static struct {
    xiaotai_wechat_config_t cfg;
    xiaotai_wechat_status_t status;
    const xiaotai_media_format_t *format;
    const xiaotai_link_media_t *video;
    wx_room_t *room;
    size_t token_capacity;
    uint32_t activity, timer, media, releasing_link, contacts_generation, contacts_revision;
    uint32_t extra_video, lost_events, invalid_refusals, camera_request_sequence;
    uint32_t authorization_binding;
    int contact_kind;
    stage_t stage;
    bool initialized, persistent_room, has_room, open, contacts_changed;
} s;

const xiaotai_media_format_t *wx_audio_format(void) { return s.format; }
const xiaotai_wechat_config_t *wx_config(void) { return &s.cfg; }
int wx_contact_kind(void) { return s.contact_kind; }

void wx_refusal_noted(bool failed)
{
    if (failed) ++s.status.refusals_failed;
    else ++s.status.refused;
}

void wx_refusal_invalid(void) { ++s.invalid_refusals; }

void wx_diagnostics(uint32_t *extra_video, uint32_t *lost_events, uint32_t *invalid_refusals)
{
    *extra_video = s.extra_video;
    *lost_events = s.lost_events;
    *invalid_refusals = s.invalid_refusals;
}

xiaotai_err_t wx_parse(const char *data, size_t size, xiaotai_json_view_t *root)
{
    if (!s.token_capacity) return XIAOTAI_ERR_INVALID_STATE;
    size_t count;
    const xiaotai_err_t error = xiaotai_json_view_parse(data, size, &count, root);
    if (error != XIAOTAI_OK || count <= s.token_capacity) return error;
    if (root) *root = (xiaotai_json_view_t){0};
    return XIAOTAI_ERR_INVALID_SIZE;
}

void xiaotai_wechat_status(xiaotai_wechat_status_t *out)
{
    if (out) *out = s.status;
}

static void publish(const char *name, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (xiaotai_event_emit(name, &args, &s.status, sizeof(s.status)) != XIAOTAI_OK) ++s.lost_events;
}

static void state_changed(void) { publish(XIAOTAI_EVENT_WECHAT_STATE, s.status.state); }

static void ready_changed(void)
{
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (s.status.ready == platform.profile_ready) return;
    s.status.ready = platform.profile_ready;
    state_changed();
}

static void cancel_timer(void)
{
    if (s.timer) xiaotai_timer_cancel(s.timer);
    s.timer = 0;
    s.stage = NONE;
}

static bool saved_mute(void)
{
#ifdef XIAOTAI_WECHAT_SETTINGS
    int32_t value = 0;
    return xiaotai_module_present("settings") &&
           xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &value) == XIAOTAI_OK && value != 0;
#else
    return false;
#endif
}

static void clear_room(void)
{
    if (s.room) {
        xiaotai_identity_wipe(s.room, sizeof(*s.room));
        if (!s.persistent_room) {
            xiaotai_mem_free(s.room);
            s.room = NULL;
        }
    }
    s.has_room = false;
}

static bool keep_room(const wx_room_t *room, wx_room_t **owned)
{
    if (!s.room && owned) {
        s.room = *owned;
        *owned = NULL;
        s.has_room = true;
        return true;
    }
    if (!s.room) s.room = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*room));
    if (!s.room) return false;
    *s.room = *room;
    s.has_room = true;
    return true;
}

static void drop_activity(void)
{
    const uint32_t id = s.activity;
    s.activity = 0;
    if (id) xiaotai_activity_end(id);
}

static void released(bool closed)
{
    cancel_timer();
    const uint32_t link = s.releasing_link;
    s.releasing_link = 0;
    if (link && !closed) xiaotai_link_disconnect(link);
    drop_activity();
    if (s.status.releasing) {
        s.status.releasing = false;
        state_changed();
    }
}

static void release_due(void *ctx)
{
    (void)ctx;
    if (s.stage == RELEASE) released(false);
}

static xiaotai_err_t tell_peer(uint32_t link, uint8_t reason, bool *opening)
{
    char body[24];
    size_t size = 0;
    const bool send = reason != XIAOTAI_WECHAT_REASON_NONE;
    if (send && reason != XIAOTAI_WECHAT_REASON_BARE) {
        const int n = snprintf(body, sizeof(body), "{\"reason\":%u}", (unsigned)reason);
        if (n <= 0 || (size_t)n >= sizeof(body)) return XIAOTAI_ERR_INVALID_SIZE;
        size = (size_t)n;
    }
    const void *data = size ? body : NULL;
    if (opening) return xiaotai_link_finish_opening(link, opening, send, HANGUP, data, size);
    return send ? xiaotai_link_command(link, HANGUP, data, size) : XIAOTAI_OK;
}

typedef enum {REMOTE, LOCAL, DEADLINE, FAILURE, REVOKED, OFFLINE} ending_t;

/* How a call ended, in words; ends past PEER_CANCEL that are not the user's
 * doing are warnings with the error behind them. */
static void log_ended(uint32_t session, unsigned end, xiaotai_err_t error, int64_t talked_us)
{
    static const char *const why[] = {
        [XIAOTAI_WECHAT_END_NONE] = "ended",
        [XIAOTAI_WECHAT_END_LOCAL] = "hung up here",
        [XIAOTAI_WECHAT_END_PEER_HANGUP] = "the other side hung up",
        [XIAOTAI_WECHAT_END_PEER_REJECT] = "the other side rejected the call",
        [XIAOTAI_WECHAT_END_PEER_CANCEL] = "the other side gave up",
        [XIAOTAI_WECHAT_END_NO_ANSWER] = "not answered in time",
        [XIAOTAI_WECHAT_END_MISSED] = "missed: neither accepted nor rejected in time",
        [XIAOTAI_WECHAT_END_CONTACT] = "the service no longer accepts this contact",
        [XIAOTAI_WECHAT_END_FAILED] = "the service refused or did not answer the request",
        [XIAOTAI_WECHAT_END_NETWORK] = "the link could not be made or broke",
        [XIAOTAI_WECHAT_END_NO_MEMORY] = "out of memory",
        [XIAOTAI_WECHAT_END_AUDIO] = "the audio session could not start or failed",
        [XIAOTAI_WECHAT_END_PREEMPTED] = "a higher-priority activity began",
        [XIAOTAI_WECHAT_END_SYSTEM] = "the system was quiesced or the binding ended",
    };
    const char *text = end < sizeof(why) / sizeof(why[0]) ? why[end] : "unknown end";
    const unsigned seconds = (unsigned)(talked_us / 1000000);
    if (end >= XIAOTAI_WECHAT_END_CONTACT && end != XIAOTAI_WECHAT_END_PREEMPTED && end != XIAOTAI_WECHAT_END_SYSTEM) {
        XIAOTAI_LOGW("wechat", "ended gen=%" PRIu32 ": %s (error 0x%x)", session, text, (unsigned)error);
    } else if (talked_us > 0) {
        XIAOTAI_LOGI("wechat", "ended gen=%" PRIu32 ": %s after %u s", session, text, seconds);
    } else {
        XIAOTAI_LOGI("wechat", "ended gen=%" PRIu32 ": %s", session, text);
    }
}

static void finish(xiaotai_wechat_end_t end, xiaotai_err_t error, ending_t action)
{
    const uint32_t session = s.status.session;
    if (!session) return;
    const bool incoming = s.status.state == XIAOTAI_WECHAT_INCOMING;
    uint32_t link = s.status.link;
    bool open = s.open;
    const xiaotai_wechat_reasons_t *r = &s.cfg.reasons;
    const uint8_t hangup = action == DEADLINE ? r->hangup_timeout :
                           action == FAILURE ? r->hangup_failure : r->hangup;
    cancel_timer();
    if (s.activity) xiaotai_activity_set_generation(s.activity, 0);
    if (s.media) xiaotai_media_close(s.media);
    (void)xiaotai_media_set_mic_muted(saved_mute());
    s.media = 0;
    s.open = false;
    s.status.state = XIAOTAI_WECHAT_IDLE;
    s.status.session = s.status.link = 0;
    s.status.muted = false;
    const int64_t talked_us = s.status.active_since_us ? xiaotai_time_us() - s.status.active_since_us : 0;
    s.status.active_since_us = 0;
    s.status.end = end;
    wx_route_end(session, s.has_room ? s.room : NULL);

    bool notified = false;
    if (action != REMOTE) {
        const xiaotai_err_t sent = link ? tell_peer(link, hangup, open ? NULL : &open) : XIAOTAI_OK;
        if (sent != XIAOTAI_OK) {
            xiaotai_eventlog_warn("wechat", open ? "hangup.send" : "hangup.resolve", session, sent);
            if (action == LOCAL && error == XIAOTAI_OK) error = sent;
        } else if (open) {
            notified = hangup != XIAOTAI_WECHAT_REASON_NONE;
        } else {
            /* The transport withdrew any pending request before allowing a rejection. */
            link = 0;
            if (s.has_room) {
                const uint8_t reason = incoming ? (action == DEADLINE ? r->missed : r->declined) :
                                       (action == FAILURE ? r->failed : r->abandoned);
                if (reason != XIAOTAI_WECHAT_REASON_NONE) wx_refuse(s.room, reason, action == OFFLINE);
            }
        }
    }
    s.status.error = error;
    s.status.releasing = notified && (action == LOCAL || action == DEADLINE || action == FAILURE);
    xiaotai_eventlog_record("wechat", "ended", session, end);
    log_ended(session, (unsigned)end, error, talked_us);
    publish(XIAOTAI_EVENT_WECHAT_ENDED, end);
    state_changed();
    clear_room();
    s.releasing_link = link;
    if (s.status.releasing &&
        xiaotai_timer_start(XIAOTAI_WECHAT_RELEASE_DELAY_MS, release_due, NULL, &s.timer) == XIAOTAI_OK) {
        s.stage = RELEASE;
    } else {
        released(false);
    }
}

static void fail(xiaotai_wechat_end_t end, xiaotai_err_t error)
{
    finish(error == XIAOTAI_ERR_NO_MEM ? XIAOTAI_WECHAT_END_NO_MEMORY : end, error, FAILURE);
}

static void timeout(void *ctx)
{
    if (s.status.session != (uint32_t)(uintptr_t)ctx) return;
    s.timer = 0;
    finish(s.stage == RING ? XIAOTAI_WECHAT_END_MISSED : XIAOTAI_WECHAT_END_NO_ANSWER,
           XIAOTAI_ERR_TIMEOUT, DEADLINE);
}

static xiaotai_err_t deadline(stage_t stage, uint32_t ms)
{
    cancel_timer();
    const xiaotai_err_t err = xiaotai_timer_start(ms, timeout, (void *)(uintptr_t)s.status.session, &s.timer);
    if (err == XIAOTAI_OK) s.stage = stage;
    return err;
}

static void revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)ctx;
    if (s.activity != id) return;
    s.activity = 0; /* The framework has already ended the attachments. */
    if (s.status.releasing) released(false);
    else finish(reason == XIAOTAI_REVOKE_SYSTEM ? XIAOTAI_WECHAT_END_SYSTEM : XIAOTAI_WECHAT_END_PREEMPTED,
                XIAOTAI_OK, REVOKED);
}

static xiaotai_err_t begin(bool outgoing, uint32_t flags, const char *remark)
{
    const xiaotai_activity_t activity = {
        .name = XIAOTAI_WECHAT_ACTIVITY, .revoked = revoked,
        .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT |
                     XIAOTAI_RESOURCE_SCREEN | XIAOTAI_RESOURCE_LINK,
    };
    xiaotai_err_t err = xiaotai_activity_begin(&activity, &s.activity);
    if (err != XIAOTAI_OK) return err;
    s.status.authorization_result = s.authorization_binding = 0;
    s.status.session = xiaotai_generation_next();
    s.status.state = outgoing ? XIAOTAI_WECHAT_OUTGOING : XIAOTAI_WECHAT_INCOMING;
    s.status.outgoing = outgoing;
    s.status.flags = flags;
    s.status.end = XIAOTAI_WECHAT_END_NONE;
    s.status.error = XIAOTAI_OK;
    s.status.peer_reason = -1;
    s.status.muted = saved_mute();
    size_t name_size = strlen(remark);
    if (name_size >= sizeof(s.status.peer_name)) {
        name_size = sizeof(s.status.peer_name) - 1U;
        while (name_size && ((unsigned char)remark[name_size] & 0xc0U) == 0x80U) --name_size;
    }
    memcpy(s.status.peer_name, remark, name_size);
    s.status.peer_name[name_size] = '\0';
    return XIAOTAI_OK;
}

static void media_event(uint32_t generation, xiaotai_media_event_t event, xiaotai_err_t error, void *ctx)
{
    (void)ctx;
    if (generation && generation == s.media && event == XIAOTAI_MEDIA_EVENT_FAILED)
        fail(XIAOTAI_WECHAT_END_AUDIO, error == XIAOTAI_OK ? XIAOTAI_FAIL : error);
}

static void answered(uint32_t generation)
{
    if (!s.open || s.stage != ANSWER) return;
    const xiaotai_media_session_t audio = {
        .generation = generation, .uplink = *s.format, .downlink = *s.format, .duplex = true,
        .sink = xiaotai_link_audio_sink(), .latency = s.cfg.latency, .event = media_event,
        .flags = s.cfg.uplink_noise_gate ? XIAOTAI_MEDIA_GATE_IDLE_NOISE : 0,
    };
    xiaotai_err_t err = xiaotai_media_set_mic_muted(s.status.muted);
    if (err == XIAOTAI_OK) err = xiaotai_media_open(&audio);
    if (err != XIAOTAI_OK) { fail(XIAOTAI_WECHAT_END_AUDIO, err); return; }
    s.media = generation;
    err = xiaotai_link_receive(generation, XIAOTAI_LINK_AUDIO, NULL, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_link_permit(generation, XIAOTAI_LINK_AUDIO, true);
    if (err != XIAOTAI_OK) { fail(XIAOTAI_WECHAT_END_NETWORK, err); return; }
    cancel_timer();
    s.status.state = XIAOTAI_WECHAT_ACTIVE;
    s.status.active_since_us = xiaotai_time_us();
    ++s.status.answered;
    xiaotai_activity_set_generation(s.activity, generation);
    xiaotai_eventlog_note("wechat", "connected", s.status.session, (int32_t)generation);
    publish(XIAOTAI_EVENT_WECHAT_CONNECTED, XIAOTAI_WECHAT_ACTIVE);
    state_changed();
}

static void opened(void *ctx, uint32_t generation)
{
    (void)ctx;
    if (!generation || generation != s.status.link || s.stage != OPEN) return;
    s.open = true;
    const xiaotai_err_t err = deadline(ANSWER, s.cfg.answer_timeout_ms);
    if (err != XIAOTAI_OK) fail(XIAOTAI_WECHAT_END_NETWORK, err);
}

static void closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)ctx;
    if (!generation) return;
    if (generation == s.releasing_link) { released(true); return; }
    if (generation != s.status.link) { ++s.status.stale_calls; return; }
    const bool was_open = s.open;
    s.status.link = 0;
    s.open = false;
    if (reason == XIAOTAI_LINK_END_PEER) {
        finish(XIAOTAI_WECHAT_END_PEER_HANGUP, XIAOTAI_OK, REMOTE);
    } else if (reason == XIAOTAI_LINK_END_NO_MEMORY) {
        finish(XIAOTAI_WECHAT_END_NO_MEMORY, XIAOTAI_ERR_NO_MEM, was_open ? REMOTE : FAILURE);
    } else if (reason == XIAOTAI_LINK_END_TIMEOUT && s.stage == OPEN) {
        finish(XIAOTAI_WECHAT_END_NO_ANSWER, XIAOTAI_ERR_TIMEOUT, DEADLINE);
    } else {
        finish(XIAOTAI_WECHAT_END_NETWORK, reason == XIAOTAI_LINK_END_PEER_FAULT ? detail : XIAOTAI_FAIL,
               was_open ? REMOTE : FAILURE);
    }
}

static void command(void *ctx, uint32_t generation, const xiaotai_link_command_t *cmd)
{
    (void)ctx;
    if (!generation || generation != s.status.link) { ++s.status.stale_calls; return; }
    if (!cmd || !s.open) return;
    if (cmd->word == ANSWERED) {
        if (s.cfg.answer_payload_requires_object) {
            xiaotai_json_view_t doc;
            if (cmd->dropped || (cmd->size && (!cmd->data || cmd->size > WX_COMMAND_BYTES ||
                xiaotai_json_view_parse((const char *)cmd->data, cmd->size, NULL, &doc) != XIAOTAI_OK ||
                doc.type != XIAOTAI_JSON_OBJECT))) return;
        }
        answered(generation);
    } else if (cmd->word == HANGUP && !cmd->dropped && cmd->data && cmd->size && cmd->size <= WX_COMMAND_BYTES) {
        xiaotai_json_view_t doc, field;
        int64_t reason;
        if (wx_parse((const char *)cmd->data, cmd->size, &doc) == XIAOTAI_OK &&
            xiaotai_json_view_get(&doc, "reason", &field) &&
            xiaotai_json_view_int(&field, &reason) == XIAOTAI_OK &&
            reason >= 0 && reason <= 20) {
            s.status.peer_reason = (int32_t)reason;
            finish(XIAOTAI_WECHAT_END_PEER_HANGUP, XIAOTAI_OK, REMOTE);
        }
    }
}

static void peer_subscribed(void *ctx, uint32_t generation, xiaotai_link_media_kind_t kind,
                            uint8_t stream, bool subscribed)
{
    (void)ctx;
    (void)stream;
    if (generation && generation == s.status.link && kind == XIAOTAI_LINK_AUDIO &&
        s.cfg.uplink_policy == XIAOTAI_LINK_PEER_STREAM_FOLLOW && s.status.state == XIAOTAI_WECHAT_ACTIVE) {
        const xiaotai_err_t err = xiaotai_link_permit(generation, XIAOTAI_LINK_AUDIO, subscribed);
        if (err != XIAOTAI_OK) fail(XIAOTAI_WECHAT_END_NETWORK, err);
    }
}

static xiaotai_err_t connect(void)
{
    static const xiaotai_link_handlers_t handlers = {
        .opened = opened, .closed = closed, .command = command, .peer_subscribed = peer_subscribed,
    };
    xiaotai_err_t err = deadline(OPEN, s.cfg.connect_timeout_ms);
    if (err != XIAOTAI_OK) { fail(XIAOTAI_WECHAT_END_NETWORK, err); return err; }
    s.status.state = XIAOTAI_WECHAT_CONNECTING;
    const xiaotai_link_format_t format = {s.format->encoding, s.format->rate_hz, s.format->channels};
    xiaotai_link_request_t request = {
        .kind = XIAOTAI_LINK_WHIP, .peer = s.room->peer, .token = s.room->token,
        .timeout_ms = s.cfg.connect_timeout_ms,
        .profile = {
            .audio = {.send = true, .send_stream = s.cfg.custom_uplink_stream ? s.cfg.uplink_stream : 10,
                      .peer_streams = s.cfg.uplink_policy, .send_format = format, .receive = true,
                      .receive_stream = s.cfg.custom_downlink_stream ? (uint8_t)s.cfg.downlink_stream :
                                                                       XIAOTAI_LINK_STREAM_ANY,
                      .receive_formats = {format}},
            .command_max_bytes = WX_COMMAND_BYTES, .command_pool_bytes = WX_COMMAND_POOL_BYTES,
        },
    };
    if ((s.status.flags & XIAOTAI_WECHAT_VIDEO) && s.video) request.profile.video = *s.video;
    err = xiaotai_link_connect(&request, &handlers, &s.status.link);
    xiaotai_identity_wipe(s.room->peer, sizeof(s.room->peer));
    xiaotai_identity_wipe(s.room->token, sizeof(s.room->token));
    if (err != XIAOTAI_OK) fail(XIAOTAI_WECHAT_END_NETWORK, err);
    else state_changed();
    return err;
}

static bool valid_room(const wx_room_t *room)
{
    return room && room->room_id[0] && room->peer[0] && room->token[0];
}

static void announce(const wx_room_t *room, wx_room_t **owned)
{
    if (!valid_room(room) || (s.has_room && strcmp(s.room->room_id, room->room_id) == 0)) return;
    if (xiaotai_quiesced()) {
        if (s.cfg.reasons.busy != XIAOTAI_WECHAT_REASON_NONE) wx_refuse(room, s.cfg.reasons.busy, false);
        return;
    }
    ready_changed();
    const char *activity = NULL;
    (void)xiaotai_activity_current(&activity);
    bool blocked = false;
    for (const char *const *n = s.cfg.incoming_busy_during; n && *n; ++n) {
        if (activity && strcmp(activity, *n) == 0) { blocked = true; break; }
    }
    uint8_t reason = s.cfg.reasons.busy;
    bool video = room->type == WX_TYPE_VIDEO || (room->type == WX_TYPE_UNSET && s.video);
    if (s.cfg.ignore_room_type) video = s.video != NULL;
    /* Nothing here to show or send a picture with: it rings as a voice call. */
    const bool offered = video && !s.video;
    if (offered) video = false;
    if (!s.status.ready) {
        reason = s.cfg.reasons.not_ready;
        if (reason == XIAOTAI_WECHAT_REASON_NONE) xiaotai_platform_profile_refresh();
    } else if (!s.format || (!s.cfg.ignore_room_type && room->type == WX_TYPE_OTHER)) {
        reason = s.cfg.reasons.unsupported;
    } else if (s.status.session || s.status.releasing || blocked) {
        if (s.status.session && s.status.outgoing && s.status.state != XIAOTAI_WECHAT_ACTIVE)
            reason = s.cfg.reasons.busy_dialing;
    } else if (begin(false, video ? XIAOTAI_WECHAT_VIDEO : offered ? XIAOTAI_WECHAT_VIDEO_OFFERED : 0, room->remark) == XIAOTAI_OK) {
        if (!keep_room(room, owned)) {
            reason = s.cfg.reasons.failed;
            fail(XIAOTAI_WECHAT_END_NO_MEMORY, XIAOTAI_ERR_NO_MEM);
        } else {
            const xiaotai_err_t err = deadline(RING, s.cfg.ring_timeout_ms);
            if (err != XIAOTAI_OK) { fail(XIAOTAI_WECHAT_END_FAILED, err); return; }
            ++s.status.announced;
            xiaotai_eventlog_note("wechat", "incoming", s.status.session, (int32_t)s.status.flags);
            publish(XIAOTAI_EVENT_WECHAT_INCOMING, 0);
            state_changed();
            return;
        }
    }
    if (reason != XIAOTAI_WECHAT_REASON_NONE) wx_refuse(room, reason, false);
}

static void connect_later(void *ctx)
{
    if (s.status.session == (uint32_t)(uintptr_t)ctx && s.status.session && s.stage == ROOM) (void)connect();
}

static void join(const wx_room_t *room, wx_room_t **owned)
{
    if (!valid_room(room) || s.stage != ROOM) { ++s.status.stale_calls; return; }
    if (s.has_room) return; /* One queued link submission per outgoing call. */
    if (!keep_room(room, owned)) {
        if (s.cfg.reasons.failed != XIAOTAI_WECHAT_REASON_NONE) wx_refuse(room, s.cfg.reasons.failed, false);
        fail(XIAOTAI_WECHAT_END_NO_MEMORY, XIAOTAI_ERR_NO_MEM);
        return;
    }
    const xiaotai_err_t err = xiaotai_run(connect_later, (void *)(uintptr_t)s.status.session);
    if (err != XIAOTAI_OK) fail(XIAOTAI_WECHAT_END_NETWORK, err);
}

void wx_announce(const wx_room_t *room) { announce(room, NULL); }
void wx_join(const wx_room_t *room) { join(room, NULL); }
void wx_room_handoff(wx_room_t **room, bool incoming)
{
    if (incoming) announce(*room, room);
    else join(*room, room);
}

void wx_cancel(const char *room_id, bool rejected)
{
    if (!room_id || !s.has_room || strcmp(s.room->room_id, room_id) != 0) return;
    finish(rejected ? XIAOTAI_WECHAT_END_PEER_REJECT :
           (s.status.state == XIAOTAI_WECHAT_ACTIVE ? XIAOTAI_WECHAT_END_PEER_HANGUP :
                                                    XIAOTAI_WECHAT_END_PEER_CANCEL), XIAOTAI_OK, REMOTE);
}

void wx_early_cancel(bool rejected)
{
    if (s.stage == ROOM) finish(rejected ? XIAOTAI_WECHAT_END_PEER_REJECT : XIAOTAI_WECHAT_END_PEER_CANCEL,
                                XIAOTAI_OK, REMOTE);
}

bool wx_same_room(const char *id) { return s.has_room && strcmp(s.room->room_id, id) == 0; }
bool wx_waiting_room(void) { return s.status.outgoing && !s.has_room && (s.stage == REQUEST || s.stage == ROOM); }
void wx_route_stale(void) { ++s.status.stale_calls; }

static void contacts_invalidated(void)
{
    xiaotai_contacts_status_t current;
    xiaotai_contacts_status(&current);
    s.contacts_changed = true;
    s.contacts_generation = xiaotai_identity_generation();
    s.contacts_revision = current.revision;
}

static void callers_update(const xiaotai_platform_signal_t *signal, void *ctx)
{
    (void)ctx;
    if (signal && signal->channel && signal->type && signal->generation == xiaotai_identity_generation() &&
        strcmp(signal->channel, "wx") == 0 && strcmp(signal->type, "callers_update") == 0) {
        contacts_invalidated();
    }
}

static void contacts_updated(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!s.contacts_changed || !event->data || event->size != sizeof(xiaotai_contacts_status_t)) return;
    const xiaotai_contacts_status_t *status = event->data;
    if (status->fresh && status->generation == xiaotai_identity_generation() &&
        (status->generation != s.contacts_generation ||
         (int32_t)(status->revision - s.contacts_revision) > 0)) s.contacts_changed = false;
}

static void dial_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    const uint32_t session = (uint32_t)(uintptr_t)ctx;
    if (reply->generation != xiaotai_identity_generation()) return;
    if (session != s.status.session || s.stage != REQUEST) {
        wx_route_reply(session, reply); /* Retire an ID returned for a just-cancelled dial. */
        ++s.status.stale_calls;
        return;
    }
    if (reply->result == XIAOTAI_OK && reply->http_status == 200 && reply->code == XIAOTAI_PLATFORM_CODE_UNBOUND) {
        /* The platform supplies a code only for a complete, valid body. */
        xiaotai_platform_report_unbound(s.cfg.trust_http_unbound);
        finish(XIAOTAI_WECHAT_END_FAILED, XIAOTAI_FAIL, FAILURE);
    } else if (reply->result == XIAOTAI_OK && reply->http_status == 200 && reply->code == 40205) {
        const bool defer = s.cfg.refresh_denied_after_dismiss;
        if (defer || s.cfg.refuse_changed_contacts) contacts_invalidated();
        if (defer) {
            s.status.authorization_result = session;
            s.authorization_binding = reply->generation;
        }
        finish(XIAOTAI_WECHAT_END_CONTACT, XIAOTAI_ERR_NOT_FOUND, FAILURE);
        if (!defer) (void)xiaotai_contacts_refresh();
    } else if (reply->result == XIAOTAI_OK && reply->http_status == 200 && reply->code == 0) {
        const xiaotai_err_t err = deadline(ROOM, s.cfg.connect_timeout_ms);
        if (err != XIAOTAI_OK) fail(XIAOTAI_WECHAT_END_FAILED, err);
        wx_route_reply(session, reply);
    } else {
        fail(XIAOTAI_WECHAT_END_FAILED, reply->result == XIAOTAI_OK ? XIAOTAI_FAIL : reply->result);
    }
}

void xiaotai_wechat_result_dismissed(uint32_t session)
{
    if (!xiaotai_on_owner() || !session || session != s.status.authorization_result) return;
    const bool same_binding = s.authorization_binding == xiaotai_identity_generation();
    s.status.authorization_result = s.authorization_binding = 0;
    if (same_binding) (void)xiaotai_contacts_refresh();
}

void wx_bad_dial_reply(uint32_t session)
{
    if (s.status.session == session && s.stage == ROOM) fail(XIAOTAI_WECHAT_END_FAILED, XIAOTAI_FAIL);
}

static xiaotai_err_t dial(const xiaotai_contact_t *contact, uint32_t flags, uint32_t *session)
{
    if (contact->kind != s.contact_kind || (s.cfg.require_app_id && !contact->extra[0][0]))
        return XIAOTAI_ERR_NOT_FOUND;
    if (!s.format || ((flags & XIAOTAI_WECHAT_VIDEO) && !s.video)) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.phase != XIAOTAI_PLATFORM_ONLINE || xiaotai_transport_state() != XIAOTAI_TRANSPORT_ONLINE)
        return XIAOTAI_ERR_PLATFORM_OFFLINE;
    ready_changed();
    if (!s.status.ready) {
        xiaotai_platform_profile_refresh();
        return XIAOTAI_ERR_INVALID_STATE;
    }

    char device[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U];
    xiaotai_err_t err = xiaotai_identity_device_id(device, sizeof(device));
    if (err != XIAOTAI_OK) return err;
    char body[REQUEST_BYTES];
    xiaotai_json_writer_t wire;
    xiaotai_json_writer_init(&wire, body, sizeof(body));
    xiaotai_json_object_begin(&wire, NULL);
    xiaotai_json_put_string(&wire, "device_id", device);
    xiaotai_json_put_string(&wire, "wx_app_id", contact->extra[0]);
    xiaotai_json_put_string(&wire, "wx_user_openid", contact->id);
    xiaotai_json_put_string(&wire, "wx_model_id", contact->extra[1]);
    xiaotai_json_put_string(&wire, "wx_room_type", flags & XIAOTAI_WECHAT_VIDEO ? "video" : "voice");
    xiaotai_json_put_int(&wire, "wx_version_type", s.cfg.version);
    xiaotai_json_object_end(&wire);
    err = xiaotai_json_writer_end(&wire, NULL);
    if (err != XIAOTAI_OK) return err;

    err = begin(true, flags, contact->name);
    if (err == XIAOTAI_OK) {
        err = wx_route_begin(s.status.session, contact->id, contact->extra[0]);
        if (err == XIAOTAI_OK) err = deadline(REQUEST, s.cfg.request_timeout_ms);
        const xiaotai_platform_request_t request = {
            .service = XIAOTAI_PLATFORM_SERVICE_VOIP, .method = XIAOTAI_PLATFORM_POST,
            .path = "/v1/voip/device/call", .body = body, .timeout_ms = s.cfg.request_timeout_ms,
            .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .flags = XIAOTAI_PLATFORM_URGENT | XIAOTAI_PLATFORM_RELEASE,
        };
        if (err == XIAOTAI_OK)
            err = xiaotai_platform_request(&request, dial_reply, (void *)(uintptr_t)s.status.session);
        if (err == XIAOTAI_OK) {
            if (session) *session = s.status.session;
            ++s.status.dialled;
            xiaotai_eventlog_note("wechat", "outgoing", s.status.session, (int32_t)flags);
            state_changed();
        } else {
            fail(XIAOTAI_WECHAT_END_FAILED, err);
        }
    }
    xiaotai_identity_wipe(body, sizeof(body));
    return err;
}

static xiaotai_err_t dial_gate(uint32_t flags)
{
    if (flags & ~(uint32_t)XIAOTAI_WECHAT_VIDEO) return XIAOTAI_ERR_INVALID_ARG;
    if (s.status.session || s.status.releasing || xiaotai_quiesced()) return XIAOTAI_ERR_BUSY;
    if (s.contacts_changed && s.contacts_generation == xiaotai_identity_generation()) return XIAOTAI_ERR_NOT_FOUND;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_wechat_dial(const char *id, uint32_t flags, uint32_t *session)
{
    if (!id || !*id || strlen(id) > XIAOTAI_CONTACT_ID_MAX) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_err_t err = dial_gate(flags);
    if (err != XIAOTAI_OK) return err;
    xiaotai_contact_t contact;
    err = xiaotai_contacts_find(id, s.contact_kind, s.cfg.require_fresh_contacts ? 0 : XIAOTAI_CONTACTS_STALE_OK,
                                &contact);
    if (err == XIAOTAI_OK) err = dial(&contact, flags, session);
    xiaotai_identity_wipe(&contact, sizeof(contact));
    return err;
}

xiaotai_err_t xiaotai_wechat_dial_contact(size_t index, uint32_t flags, uint32_t *session)
{
    if (index > UINT8_MAX) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_err_t err = dial_gate(flags);
    if (err != XIAOTAI_OK) return err;
    xiaotai_contact_t contact;
    err = xiaotai_contacts_at((uint8_t)index, s.cfg.require_fresh_contacts ? 0 : XIAOTAI_CONTACTS_STALE_OK, &contact);
    if (err == XIAOTAI_OK) err = dial(&contact, flags, session);
    xiaotai_identity_wipe(&contact, sizeof(contact));
    return err;
}

static bool current(uint32_t session)
{
    if (session && session != s.status.session) { ++s.status.stale_calls; return false; }
    return s.status.session != 0;
}

xiaotai_err_t xiaotai_wechat_accept(uint32_t session)
{
    if (!current(session) || s.status.state != XIAOTAI_WECHAT_INCOMING) return XIAOTAI_ERR_INVALID_STATE;
    return connect();
}

xiaotai_err_t xiaotai_wechat_reject(uint32_t session)
{
    if (!current(session) || s.status.state != XIAOTAI_WECHAT_INCOMING) return XIAOTAI_ERR_INVALID_STATE;
    finish(XIAOTAI_WECHAT_END_LOCAL, XIAOTAI_OK, LOCAL);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_wechat_hangup(uint32_t session)
{
    if (!current(session)) return XIAOTAI_ERR_INVALID_STATE;
    finish(XIAOTAI_WECHAT_END_LOCAL, XIAOTAI_OK, LOCAL);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_wechat_set_muted(uint32_t session, bool muted)
{
    if (!current(session)) return XIAOTAI_ERR_INVALID_STATE;
    muted = muted || saved_mute();
    const xiaotai_err_t err = s.media ? xiaotai_media_set_mic_muted(muted) : XIAOTAI_OK;
    if (err == XIAOTAI_OK && s.status.muted != muted) {
        s.status.muted = muted;
        state_changed();
    }
    return err;
}

xiaotai_err_t xiaotai_wechat_request_video(uint32_t link)
{
    if (!link || link != s.status.link || s.status.state != XIAOTAI_WECHAT_ACTIVE ||
        !(s.status.flags & XIAOTAI_WECHAT_VIDEO)) return XIAOTAI_ERR_INVALID_STATE;
    const uint8_t enabled = 1;
    const uint32_t sequence = ++s.camera_request_sequence & UINT16_MAX;
    return xiaotai_link_command(link, (sequence << 16) | ASK_CAMERA, &enabled, sizeof(enabled));
}

#ifdef XIAOTAI_WECHAT_SETTINGS
static void settings_updated(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (s.status.session && strcmp(event->args.text, XIAOTAI_SETTING_AUDIO_MIC_MUTED) == 0) {
        const bool muted = saved_mute(); /* The settings module already applied it. */
        if (s.status.muted != muted) { s.status.muted = muted; state_changed(); }
    }
}
#endif

static void platform_changed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    ready_changed();
    const bool offline = strcmp(event->name, XIAOTAI_EVENT_PLATFORM_OFFLINE) == 0;
    if (strcmp(event->name, XIAOTAI_EVENT_BINDING_NEEDED) == 0 ||
        strcmp(event->name, XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED) == 0 ||
        (offline && event->args.value == XIAOTAI_PLATFORM_OFFLINE_BINDING)) {
        if (s.status.releasing) released(false);
        finish(XIAOTAI_WECHAT_END_SYSTEM, XIAOTAI_OK, REVOKED);
        wx_route_clear();
        wx_refusals_reset();
        s.contacts_changed = false;
    } else if (offline && (s.cfg.end_on_platform_offline || s.stage == REQUEST)) {
        if (s.status.releasing) released(false);
        finish(XIAOTAI_WECHAT_END_NETWORK, XIAOTAI_ERR_PLATFORM_OFFLINE, OFFLINE);
    }
    wx_refusals_platform(event);
}

static void on_intent(const xiaotai_args_t *args, void *ctx)
{
    const uint32_t value = args ? (uint32_t)args->value : 0;
    switch ((uintptr_t)ctx) {
    case 0: if (args) (void)xiaotai_wechat_dial(args->text, value, NULL); break;
    case 1: (void)xiaotai_wechat_accept(value); break;
    case 2: (void)xiaotai_wechat_reject(value); break;
    case 3: (void)xiaotai_wechat_hangup(value); break;
    case 4: (void)xiaotai_wechat_set_muted(value, true); break;
    case 5: (void)xiaotai_wechat_set_muted(value, false); break;
    case 6: (void)xiaotai_wechat_set_muted(value, !s.status.muted); break;
    }
}

static bool valid_reasons(const xiaotai_wechat_reasons_t *r)
{
    const uint8_t values[] = {
        r->hangup, r->hangup_timeout, r->hangup_failure, r->declined, r->missed, r->abandoned, r->failed,
        r->busy, r->busy_dialing, r->stale, r->retired_call, r->retired_room, r->not_ready, r->unsupported, r->overflow,
    };
    for (size_t i = 0; i < sizeof(values); ++i) {
        if (values[i] > 20 && values[i] != XIAOTAI_WECHAT_REASON_NONE &&
            !(i < 3 && values[i] == XIAOTAI_WECHAT_REASON_BARE)) return false;
    }
    return true;
}

xiaotai_err_t xiaotai_wechat_configure(const xiaotai_wechat_config_t *config)
{
    if (s.initialized) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_wechat_config_t zero = {0};
    const xiaotai_wechat_config_t *c = config ? config : &zero;
    if (c->downlink_stream > XIAOTAI_LINK_STREAM_MAX) {
        XIAOTAI_LOGE("wechat", "downlink_stream=%lu outside 0..15", (unsigned long)c->downlink_stream);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if ((unsigned)c->version > XIAOTAI_WECHAT_TRIAL || c->uplink_stream > XIAOTAI_LINK_STREAM_MAX ||
        (unsigned)c->uplink_policy > XIAOTAI_LINK_PEER_STREAM_FOLLOW ||
        c->request_timeout_ms > XIAOTAI_PLATFORM_TIMEOUT_MAX_MS || c->ring_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS ||
        c->connect_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS || c->answer_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS ||
        c->reject_body_bytes > XIAOTAI_TRANSPORT_SERVICE_BODY_MAX ||
        (c->custom_reasons && !valid_reasons(&c->reasons))) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; c->incoming_busy_during && c->incoming_busy_during[i]; ++i) {
        if (i >= XIAOTAI_WECHAT_BUSY_DURING_MAX || !c->incoming_busy_during[i][0])
            return XIAOTAI_ERR_INVALID_ARG;
    }
    s.cfg = *c;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_module_wechat_init(void)
{
    s.initialized = true;
    s.status.peer_reason = -1;
    if (!s.cfg.request_timeout_ms) s.cfg.request_timeout_ms = 28000;
    if (!s.cfg.ring_timeout_ms) s.cfg.ring_timeout_ms = 45000;
    if (!s.cfg.connect_timeout_ms) s.cfg.connect_timeout_ms = 30000;
    if (!s.cfg.answer_timeout_ms) s.cfg.answer_timeout_ms = 35000;
    if (!s.cfg.calling_timeout_s) s.cfg.calling_timeout_s = 30;
    if (!s.cfg.custom_reasons) s.cfg.reasons = reasons;
    const xiaotai_media_latency_t *p = &s.cfg.latency;
    if (!(p->start_ms || p->base_ms || p->min_ms || p->max_ms || p->surplus_ms || p->settle_s ||
          p->slow_permille || p->steady_jitter_ms || p->steady_peak_ms || p->step_down_ms)) s.cfg.latency = latency;
    kind.extra_required = s.cfg.require_app_id ? 1U : 0U;
    static const xiaotai_platform_requirement_t platform = {
        .request_body_bytes = REQUEST_BYTES, .response_bytes = 1024, .signal_bytes = 8192, .concurrent_requests = 1,
    };
    static const xiaotai_transport_requirement_t transport = {.links = 1, .command_pool_bytes = WX_COMMAND_POOL_BYTES};
    xiaotai_err_t err = xiaotai_contacts_kind_register(&kind, &s.contact_kind);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require(&platform);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require_service(XIAOTAI_PLATFORM_SERVICE_VOIP);
    if (err == XIAOTAI_OK) err = xiaotai_transport_require(&transport);
    if (err == XIAOTAI_OK) err = wx_route_init();
    if (err == XIAOTAI_OK) err = wx_refusals_init();
    if (err == XIAOTAI_OK) err = wx_profile_init();
    if (err == XIAOTAI_OK) err = wx_at_init();
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_PLATFORM_PROFILE, platform_changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_ONLINE, platform_changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, platform_changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, platform_changed, NULL},
        {XIAOTAI_EVENT_BINDING_NEEDED, platform_changed, NULL},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(*events); ++i)
        err = xiaotai_event_subscribe_static(&events[i]);
    if (err == XIAOTAI_OK && s.cfg.refuse_changed_contacts)
        err = xiaotai_platform_signal_subscribe("callers_update", "wx", callers_update, NULL);
    if (err == XIAOTAI_OK && (s.cfg.refuse_changed_contacts || s.cfg.refresh_denied_after_dismiss))
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CONTACTS_CHANGED, contacts_updated, NULL);
#ifdef XIAOTAI_WECHAT_SETTINGS
    if (err == XIAOTAI_OK && xiaotai_module_present("settings"))
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_SETTINGS_CHANGED, settings_updated, NULL);
#endif
    static const char *const intents[] = {
        XIAOTAI_INTENT_WECHAT_DIAL, XIAOTAI_INTENT_WECHAT_ACCEPT, XIAOTAI_INTENT_WECHAT_REJECT,
        XIAOTAI_INTENT_WECHAT_HANGUP, XIAOTAI_INTENT_WECHAT_MIC_OFF, XIAOTAI_INTENT_WECHAT_MIC_ON,
        XIAOTAI_INTENT_WECHAT_MIC_TOGGLE,
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(intents) / sizeof(*intents); ++i)
        err = xiaotai_intent_handle(intents[i], on_intent, (void *)(uintptr_t)i);
    if (err == XIAOTAI_OK) s.token_capacity = xiaotai_mem_has_external() ? 96U : 64U;
    return err;
}

void xiaotai_module_wechat_start(void)
{
    s.format = wx_profile_audio();
    const void *video = NULL;
    const size_t count = xiaotai_activity_contributions(XIAOTAI_WECHAT_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                                                         XIAOTAI_WECHAT_SCENE, &video, 1);
    if (count) s.video = video;
    if (count > 1) s.extra_video += (uint32_t)(count - 1);
    if (xiaotai_mem_has_external()) {
        s.room = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*s.room));
        s.persistent_room = s.room != NULL;
    }
    ready_changed();
}

void xiaotai_wechat_reset(void)
{
    if (xiaotai_on_owner()) {
        if (s.status.releasing) released(false);
        finish(XIAOTAI_WECHAT_END_SYSTEM, XIAOTAI_OK, REMOTE);
    }
    s.persistent_room = false;
    clear_room();
    wx_route_reset();
    wx_refusals_reset();
    wx_profile_reset();
    memset(&s, 0, sizeof(s));
}
