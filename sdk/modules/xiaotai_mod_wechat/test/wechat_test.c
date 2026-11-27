// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/regression/test_call_signals.py
// Replaces legacy: tests/xmini/xmini_business/test_business.c::wechat_outgoing
// Replaces legacy: tests/xmini/xmini_business/test_business.c::wechat_authorization_refresh
// Replaces legacy: tests/mz28/test_voip_join_lifetime.c::contacts_response
// Replaces legacy: tests/mz28/test_voip_join_lifetime.c::voip_authorization_refresh
// Replaces legacy: tests/regression/test_device_profile_response.py
// Replaces legacy: tests/regression/test_call_hangup.py
// Replaces legacy: tests/regression/test_device_call_info.py
// Replaces legacy: tests/regression/test_voip_task_cleanup.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_call_signal_room_match
// Replaces legacy: tests/regression/test_voice_reliability.py::test_voip_profile_delivery_recovery
#include "wechat_internal.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/at.h"
#include "xiaotai/at_command.h"
#include "xiaotai/hal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai_mem_host.h"

// Replaces legacy: tests/mz28/test_protocol.c::protocol
/* The owner, arbitration, event queue, timers and JSON codec are real. */
static atomic_int_fast64_t clock_us = 1;
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&clock_us); }
static xiaotai_signal_t sync;
static xiaotai_platform_status_t platform = {.profile_ready = true, .phase = XIAOTAI_PLATFORM_ONLINE};
static xiaotai_transport_state_t transport = XIAOTAI_TRANSPORT_ONLINE;
static xiaotai_platform_reply_fn_t reply_fn;
static void *reply_ctx;
static bool announce_join;
static xiaotai_platform_signal_fn_t update_fn;
#ifdef XIAOTAI_WECHAT_REAL_ROUTE
static bool legacy_incoming_policy;
static const char *const signal_types[] = {"call_incoming", "call_cancel", "room_cancel", "call_reject"};
static xiaotai_platform_signal_fn_t signal_fns[sizeof(signal_types) / sizeof(*signal_types)];
static bool external;
bool __wrap_xiaotai_mem_has_external(void) { return external; }
#endif
static xiaotai_link_handlers_t link_handlers;
static xiaotai_link_request_t link_request;
static xiaotai_media_session_t media;
static uint32_t generation, link, media_gen, last_room_close, attachment_generation, quiesce_ticket;
static unsigned requests, refusals, refreshes, reprofiles, command_count, ended_events, reads, cleared;
static uint32_t request_timeout, contact_flags;
static bool mic, contact_missing_app, contact_fresh = true, rejected_deferred;
static bool physical_open;
static uint32_t contacts_generation = 7, identity_generation = 7, contacts_revision = 1;
static xiaotai_err_t fail_connect, fail_media, fail_hangup;
static uint16_t command_word;
static uint32_t command_envelope;
static size_t command_size;
static uint8_t refusal_reason;
static char command_body[64], trace[96], request_body[800];
static char refusal_body[XIAOTAI_TRANSPORT_SERVICE_BODY_MAX];
static xiaotai_transport_reply_fn_t refusal_reply;
static void *refusal_ctx;
static bool encoded_capture, background_policy, fail_refusal_submit, has_video, extra_scene, wt99_panel;
static xiaotai_platform_profile_fn_t scene_writer;
static xiaotai_media_requirement_t requirements[2];
static size_t requirement_count;
static xiaotai_display_t panel;
#ifdef XIAOTAI_WECHAT_REAL_REFUSAL
static const char *wipe_text;
static void *wipe_ptr;
static size_t wipe_size;
static unsigned verified_wipes;
void __real_xiaotai_mem_free(void *ptr);
void __wrap_xiaotai_mem_free(void *ptr);
void __wrap_xiaotai_mem_free(void *ptr)
{
    if (ptr && ptr == wipe_ptr) {
        for (size_t i = 0; i < wipe_size; ++i) assert(wipe_text[i] == '\0');
        ++verified_wipes;
        wipe_ptr = NULL;
    }
    __real_xiaotai_mem_free(ptr);
}
#endif
static size_t used;
static const xiaotai_contact_kind_def_t *contact_kind;
static const xiaotai_media_sink_t sink;
static const wx_room_t incoming = {
    .room_id = "fixture-room", .call_id = "call-fixture", .peer = "whip-peer", .token = "whip-test-token",
    .remark = "fixture remark", .app_id = "app", .model_id = "model",
    .server_token = "server-test-token", .payload = "test-payload", .type = WX_TYPE_VOICE,
};

static void record(char c) { assert(used + 1 < sizeof(trace)); trace[used++] = c; trace[used] = 0; }
static xiaotai_wechat_status_t status(void)
{
    xiaotai_wechat_status_t out;
    xiaotai_wechat_status(&out);
    return out;
}

#ifndef XIAOTAI_WECHAT_REAL_ROUTE
xiaotai_err_t wx_route_init(void) { return XIAOTAI_OK; }
xiaotai_err_t wx_route_begin(uint32_t session, const char *id, const char *app)
{
    assert(session && strcmp(id, "fake-openid") == 0 && app);
    return XIAOTAI_OK;
}
void wx_route_reply(uint32_t session, const xiaotai_platform_reply_t *reply)
{
    assert(session && reply);
    if (announce_join) {
        assert(status().session == session && status().state == XIAOTAI_WECHAT_OUTGOING);
        wx_join(&incoming);
        assert(!link && status().state == XIAOTAI_WECHAT_OUTGOING);
    }
}
void wx_route_end(uint32_t session, const wx_room_t *room)
{
    last_room_close = session;
    if (room) assert(strcmp(room->room_id, incoming.room_id) == 0);
}
void wx_route_clear(void) { ++cleared; }
void wx_route_reset(void) { wx_route_clear(); }
#endif
#ifndef XIAOTAI_WECHAT_REAL_REFUSAL
void __wrap_wx_refuse(const wx_room_t *room, uint8_t reason, bool defer);
void __wrap_wx_refuse(const wx_room_t *room, uint8_t reason, bool defer)
{
    assert(room && room->room_id[0]);
    ++refusals;
    refusal_reason = reason;
    rejected_deferred = defer;
}
#endif

xiaotai_err_t xiaotai_transport_service_request(const char *path, const char *json,
                                                xiaotai_transport_reply_fn_t fn, void *ctx)
{
    assert(xiaotai_on_owner() && !strcmp(path, "/v1/wxvoip/reject") && fn && ctx);
    assert(!refusal_reply && strlen(json) < sizeof(refusal_body));
    strcpy(refusal_body, json);
    ++refusals;
#ifdef XIAOTAI_WECHAT_REAL_REFUSAL
    assert(!wipe_ptr || wipe_ptr == ctx);
    wipe_ptr = ctx;
    wipe_text = json;
    wipe_size = strlen(json) + 1U;
#endif
    if (fail_refusal_submit) return XIAOTAI_ERR_BUSY;
    refusal_reply = fn;
    refusal_ctx = ctx;
    return XIAOTAI_OK;
}
bool xiaotai_platform_background_deferred(void) { return background_policy && xiaotai_activity_active(); }
xiaotai_err_t xiaotai_platform_profile_scene(const char *name, xiaotai_platform_profile_fn_t fn, void *ctx)
{
    assert(!strcmp(name, "voip") && fn && !ctx && !scene_writer);
    scene_writer = fn;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *requirement)
{
    assert(requirement_count < 2);
    requirements[requirement_count++] = *requirement;
    return XIAOTAI_OK;
}
void xiaotai_media_info(xiaotai_media_info_t *info)
{
    *info = (xiaotai_media_info_t){
        .capture_pcm = !encoded_capture,
        .capture_encoding = encoded_capture ? XIAOTAI_CODEC_OPUS : XIAOTAI_CODEC_PCM16,
    };
}
const xiaotai_display_t *xiaotai_hal_display(size_t index)
{
    return !index && panel.desc.width ? &panel : NULL;
}

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *r)
{
    assert(r->request_body_bytes == 768 && r->response_bytes == 1024 && r->signal_bytes == 8192 &&
           r->concurrent_requests == 1);
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_platform_require_service(const char *service)
{
    assert(strcmp(service, "voip") == 0);
    return XIAOTAI_OK;
}
void xiaotai_platform_status(xiaotai_platform_status_t *out) { *out = platform; }
void xiaotai_platform_profile_refresh(void) { ++reprofiles; }
static bool trust_http_unbound;
static unsigned unbound_reports;
void xiaotai_platform_report_unbound(bool confirmed)
{
    assert(confirmed == trust_http_unbound);
    ++unbound_reports;
    record('U');
}
xiaotai_err_t xiaotai_platform_signal_subscribe(const char *type, const char *channel,
                                               xiaotai_platform_signal_fn_t fn, void *ctx)
{
    assert(strcmp(channel, "wx") == 0 && ctx == NULL);
#ifdef XIAOTAI_WECHAT_REAL_ROUTE
    for (size_t i = 0; i < sizeof(signal_types) / sizeof(*signal_types); ++i) {
        if (strcmp(type, signal_types[i]) != 0) continue;
        assert(!signal_fns[i]);
        signal_fns[i] = fn;
        return XIAOTAI_OK;
    }
#endif
    assert(strcmp(type, "callers_update") == 0);
    update_fn = fn;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *r, xiaotai_platform_reply_fn_t fn, void *ctx)
{
    assert(status().state == XIAOTAI_WECHAT_OUTGOING && status().session == (uint32_t)(uintptr_t)ctx);
    assert(r->method == XIAOTAI_PLATFORM_POST && strcmp(r->service, "voip") == 0 &&
           strcmp(r->path, "/v1/voip/device/call") == 0);
    assert(r->auth == XIAOTAI_PLATFORM_AUTH_SESSION && (r->flags & XIAOTAI_PLATFORM_RELEASE));
    snprintf(request_body, sizeof(request_body), "%s", r->body);
    request_timeout = r->timeout_ms;
    reply_fn = fn; reply_ctx = ctx; ++requests;
    return XIAOTAI_OK;
}
xiaotai_transport_state_t xiaotai_transport_state(void) { return transport; }
xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *r)
{
    assert(r->links == 1 && r->command_pool_bytes == 2048);
    return XIAOTAI_OK;
}
uint32_t xiaotai_identity_generation(void) { return identity_generation; }
xiaotai_err_t xiaotai_identity_device_id(char *out, size_t capacity)
{
    assert(capacity > 15);
    strcpy(out, "fake-device");
    return XIAOTAI_OK;
}
void xiaotai_identity_wipe(void *data, size_t size) { memset(data, 0, size); }
xiaotai_err_t xiaotai_contacts_kind_register(const xiaotai_contact_kind_def_t *def, int *kind)
{
    contact_kind = def; *kind = 2;
    return XIAOTAI_OK;
}
void xiaotai_contacts_status(xiaotai_contacts_status_t *out)
{
    *out = (xiaotai_contacts_status_t){.revision = contacts_revision, .generation = contacts_generation,
                                       .fresh = contact_fresh};
}
xiaotai_err_t xiaotai_contacts_refresh(void) { ++refreshes; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_contacts_find(const char *id, int kind, uint32_t flags, xiaotai_contact_t *out)
{
    assert(kind == 2 && strcmp(id, "fake-openid") == 0);
    ++reads; contact_flags = flags;
    if (contacts_generation != identity_generation || (!contact_fresh && !(flags & XIAOTAI_CONTACTS_STALE_OK))) {
        return XIAOTAI_ERR_NOT_FOUND;
    }
    *out = (xiaotai_contact_t){.kind = 2, .id = "fake-openid", .name = "fake remark", .extra = {"app", "model"}};
    if (contact_missing_app) out->extra[0][0] = 0;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_contacts_at(uint8_t index, uint32_t flags, xiaotai_contact_t *out)
{
    if (index > 1) return XIAOTAI_ERR_NOT_FOUND;
    const xiaotai_err_t err = xiaotai_contacts_find("fake-openid", 2, flags, out);
    if (err == XIAOTAI_OK && index == 1) out->kind = 1;
    return err;
}
bool xiaotai_media_can_send(const xiaotai_media_format_t *f)
{
    return f->encoding == XIAOTAI_CODEC_G711A || (encoded_capture && f->encoding == XIAOTAI_CODEC_OPUS);
}
bool xiaotai_media_can_receive(const xiaotai_media_format_t *f) { return xiaotai_media_can_send(f); }
const xiaotai_media_sink_t *xiaotai_link_audio_sink(void) { return &sink; }
xiaotai_err_t xiaotai_media_set_mic_muted(bool mute) { mic = mute; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *session)
{
    assert(session->generation == link && session->duplex && session->sink == &sink &&
           session->uplink.encoding == (encoded_capture ? XIAOTAI_CODEC_OPUS : XIAOTAI_CODEC_G711A) &&
           session->downlink.rate_hz == (encoded_capture ? 16000U : 8000U) &&
           session->latency.start_ms == 20 && !session->downlink_burst_ms);
    if (fail_media) return fail_media;
    media = *session; media_gen = session->generation; record('M');
    return XIAOTAI_OK;
}
void xiaotai_media_close(uint32_t gen) { assert(gen == media_gen); media_gen = 0; record('m'); }

xiaotai_err_t xiaotai_link_connect(const xiaotai_link_request_t *r, const xiaotai_link_handlers_t *handlers,
                                   uint32_t *gen)
{
    assert(xiaotai_on_owner() && r->kind == XIAOTAI_LINK_WHIP && strcmp(r->peer, incoming.peer) == 0 &&
           strcmp(r->token, incoming.token) == 0 &&
           r->profile.audio.receive_stream == (wx_config()->custom_downlink_stream ?
                                                wx_config()->downlink_stream : XIAOTAI_LINK_STREAM_ANY) &&
           !r->profile.audio.accept_peer_stream &&
           !r->profile.audio.receive_request && r->profile.command_max_bytes == 1024 &&
           r->profile.command_pool_bytes == 2048 &&
           r->profile.audio.send_stream == (wx_config()->custom_uplink_stream ? wx_config()->uplink_stream : 10U) &&
           r->profile.audio.peer_streams == wx_config()->uplink_policy);
    if (fail_connect) return fail_connect;
    assert(!link);
    link_request = *r;
    link_handlers = *handlers;
    generation = xiaotai_generation_next(); *gen = link = generation; record('L');
    return XIAOTAI_OK;
}
void xiaotai_link_disconnect(uint32_t gen) { assert(gen == link); link = 0; physical_open = false; record('D'); }
xiaotai_err_t xiaotai_link_receive(uint32_t gen, xiaotai_link_media_kind_t kind, xiaotai_link_receive_fn_t f, void *ctx)
{
    assert(gen == link && kind == XIAOTAI_LINK_AUDIO && !f && !ctx && media_gen == gen);
    record('R'); return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_link_permit(uint32_t gen, xiaotai_link_media_kind_t kind, bool allow)
{
    assert(gen == link && kind == XIAOTAI_LINK_AUDIO);
    record(allow ? 'P' : 'p'); return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_link_command(uint32_t gen, uint32_t word, const void *data, size_t size)
{
    assert(gen == link && size < sizeof(command_body));
    command_envelope = word;
    command_word = (uint16_t)word;
    command_size = size;
    ++command_count;
    if (size) memcpy(command_body, data, size);
    command_body[size] = 0; record('H');
    return word == 0x2001 ? fail_hangup : XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_finish_opening(uint32_t gen, bool *opened, bool send,
                                        uint32_t word, const void *data, size_t size)
{
    assert(gen == link && !media_gen);
    *opened = physical_open;
    if (!*opened) xiaotai_link_disconnect(gen);
    else if (send) return xiaotai_link_command(gen, word, data, size);
    return XIAOTAI_OK;
}

static void attachment_start(uint32_t id, uint32_t gen, void *ctx)
{
    (void)id;
    (void)ctx;
    if (gen) { assert(gen == link && status().state == XIAOTAI_WECHAT_ACTIVE); record('A'); }
    else if (attachment_generation) record('a');
    attachment_generation = gen;
}
static void attachment_end(uint32_t id, uint32_t gen, void *ctx)
{
    (void)id;
    (void)ctx;
    if (gen) { assert(attachment_generation == gen); record('a'); }
    attachment_generation = 0;
}
static void observe(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->size == sizeof(xiaotai_wechat_status_t));
    const xiaotai_wechat_status_t *out = event->data;
    assert(out->state == XIAOTAI_WECHAT_IDLE && !out->session && !out->link && (int32_t)out->end == event->args.value);
    ++ended_events;
}

static int scene_part(char *out, size_t capacity, void *ctx)
{
    const char *members = ctx ? "\"extra\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"" :
        wt99_panel ? "\"up_video_mt\":\"h264\",\"down_video_mt\":\"jpeg\",\"camera_rotation\":0,"
                     "\"aspect_ratio\":1.7066666667,\"video_res_mode\":\"fit_screen\"" :
                     "\"up_video_mt\":\"h264\",\"down_video_mt\":\"jpeg\",\"camera_rotation\":270,"
                     "\"aspect_ratio\":\"3:4\",\"video_res_mode\":\"auto\"";
    const size_t length = strlen(members);
    if (length > capacity) return -1;
    memcpy(out, members, length);
    return (int)length;
}

static const xiaotai_link_media_t video_first = {
    .send_stream = 11, .receive_stream = 11, .receive_request = true,
};
static const xiaotai_link_media_t video_second = {.send_stream = 12, .receive_stream = 12};

static xiaotai_err_t observers(void)
{
    static const xiaotai_platform_scene_part_t scene = {.write = scene_part};
    static int any;
    static const xiaotai_platform_scene_part_t large = {.write = scene_part, .ctx = &any};
    static const xiaotai_contribution_t parts[] = {
        {.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = "voip", .data = &video_first},
        {.slot = XIAOTAI_PLATFORM_SLOT_SCENE, .scene = "voip", .data = &scene}, {0},
    };
    static const xiaotai_contribution_t duplicate[] = {
        {.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = "voip", .data = &video_second}, {0},
    };
    static const xiaotai_contribution_t too_large[] = {
        {.slot = XIAOTAI_PLATFORM_SLOT_SCENE, .scene = "voip", .data = &large}, {0},
    };
    static const xiaotai_attachment_t v = {.contributions = parts};
    static const xiaotai_attachment_t d = {.contributions = duplicate};
    static const xiaotai_attachment_t t = {.contributions = too_large};
    static const xiaotai_attachment_t a = {
        .begin = attachment_start, .end = attachment_end, .session = attachment_start,
    };
    assert(xiaotai_activity_attach("call", &a) == XIAOTAI_OK);
    if (has_video) {
        assert(xiaotai_activity_attach("call", &v) == XIAOTAI_OK);
        assert(xiaotai_activity_attach("call", &d) == XIAOTAI_OK);
    }
    if (extra_scene) assert(xiaotai_activity_attach("call", &t) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_WECHAT_ENDED, observe, NULL);
}

static xiaotai_err_t settings_init(void)
{
    static const xiaotai_setting_t mute = {
        .name = XIAOTAI_SETTING_AUDIO_MIC_MUTED, .key = XIAOTAI_SETTING_KEY_AUDIO_MIC_MUTED,
        .kind = XIAOTAI_SETTING_BOOL, .max = 1,
    };
    return xiaotai_setting_register(&mute);
}

static void give(void *ctx) { (void)ctx; xiaotai_signal_give(&sync); }
static void run(void (*fn)(void *))
{
    if (fn) assert(xiaotai_run(fn, NULL) == XIAOTAI_OK);
    for (unsigned i = 0; i < 3; ++i) {
        assert(xiaotai_run(give, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&sync, 3000));
    }
}
static void at(int64_t target)
{
    atomic_store(&clock_us, target);
    run(NULL);
}
static void deliver(uint32_t word, const char *data)
{
    const xiaotai_link_command_t c = {.word = word, .data = (const uint8_t *)data, .size = data ? strlen(data) : 0};
    link_handlers.command(link_handlers.ctx, generation, &c);
}
static void accepting(void)
{
    wx_announce(&incoming);
    assert(status().state == XIAOTAI_WECHAT_INCOMING && status().session);
    assert(xiaotai_wechat_accept(status().session) == XIAOTAI_OK && status().state == XIAOTAI_WECHAT_CONNECTING);
    assert(link && link_request.timeout_ms == wx_config()->connect_timeout_ms);
}
static void x22_finish(void *ctx)
{
    (void)ctx;
    accepting();
    physical_open = true; /* Backend connected, but the owner has not run opened(). */
    const unsigned sent = command_count, rejected = refusals;
    assert(xiaotai_wechat_hangup(status().session) == XIAOTAI_OK);
    assert(command_count == sent + 1 && command_word == 0x2001);
    assert(!strcmp(command_body, "{\"reason\":0}") && refusals == rejected);
    assert(!status().session && status().releasing && link && !media_gen);
}

static void x22_without_open(void *ctx)
{
    (void)ctx;
    accepting();
    const unsigned sent = command_count, rejected = refusals;
    assert(xiaotai_wechat_hangup(status().session) == XIAOTAI_OK);
    assert(!status().session && !status().releasing && !link && !media_gen);
    assert(command_count == sent && refusals == rejected + 1 && refusal_reason == 1);
}

static void x22_send_error(void *ctx)
{
    (void)ctx;
    accepting();
    physical_open = true;
    fail_hangup = XIAOTAI_ERR_BUSY;
    const unsigned sent = command_count, rejected = refusals;
    assert(xiaotai_wechat_hangup(status().session) == XIAOTAI_OK);
    assert(!status().session && !status().releasing && !link && !media_gen);
    assert(command_count == sent + 1 && refusals == rejected && status().error == XIAOTAI_ERR_BUSY);
    fail_hangup = XIAOTAI_OK;
}

static void x22_preempt(void *ctx)
{
    (void)ctx;
    accepting();
    physical_open = true;
    const unsigned sent = command_count, rejected = refusals;
    const xiaotai_activity_t emergency = {.name = "emergency"};
    uint32_t id = 0;
    assert(xiaotai_activity_begin(&emergency, &id) == XIAOTAI_OK);
    assert(status().end == XIAOTAI_WECHAT_END_PREEMPTED && !status().releasing && !link);
    assert(command_count == sent + 1 && refusals == rejected && command_word == 0x2001);
    xiaotai_activity_end(id);
}

static void active(void)
{
    accepting();
    link_handlers.opened(link_handlers.ctx, generation);
    deliver(0x2000, NULL);
    assert(status().state == XIAOTAI_WECHAT_ACTIVE && status().link == media_gen && attachment_generation == media_gen);
}
static void incoming_case(void *ctx)
{
    (void)ctx;
    wx_room_t malformed = incoming; malformed.token[0] = 0;
    wx_announce(&malformed);
    assert(!status().session && !refusals);
    platform.profile_ready = false;
    wx_announce(&incoming);
    assert(!status().session && reprofiles == 1 && !refusals);
    platform.profile_ready = true;
    wx_announce(&incoming);
    const uint32_t session = status().session;
    assert(session && !strcmp(status().peer_name, incoming.remark) && status().announced == 1);
    wx_announce(&incoming);
    assert(status().session == session && status().announced == 1 && !refusals);
    wx_room_t another = incoming;
    strcpy(another.room_id, "second-room");
    wx_announce(&another);
    assert(refusals == 1 && refusal_reason == 8 && !link && !requests);
    assert(xiaotai_wechat_accept(session + 1) == XIAOTAI_ERR_INVALID_STATE);
    assert(status().stale_calls == 1);
    assert(xiaotai_wechat_reject(0) == XIAOTAI_OK && status().end == XIAOTAI_WECHAT_END_LOCAL);
    assert(refusals == 2 && refusal_reason == 7 && last_room_close == session);
}
static void answer_case(void *ctx)
{
    (void)ctx;
    accepting();
    assert(xiaotai_activity_generation(xiaotai_activity_current(NULL)) == 0);
    deliver(0x2000, NULL);
    assert(!media_gen);
    link_handlers.opened(link_handlers.ctx, generation);
    assert(!media_gen && !attachment_generation);
    deliver(0x12000, NULL);
    static const char *const invalid[] = {NULL, "{}", "{\"reason\":-1}", "{\"reason\":21}",
        "{\"reason\":1.5}", "{\"reason\":\"1\"}", "{\"reason\":true}", "{\"reason\":1}trailing"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) deliver(0x2001, invalid[i]);
    assert(!media_gen && status().state == XIAOTAI_WECHAT_CONNECTING);
    const xiaotai_link_command_t answer = {.word = 0x2000, .size = 2048, .dropped = true};
    link_handlers.command(link_handlers.ctx, generation, &answer);
    assert(status().state == XIAOTAI_WECHAT_ACTIVE && status().active_since_us == 1);
    assert(!strcmp(trace, "LMRPA") && status().answered == 1 && attachment_generation == link);
    deliver(0x2000, NULL);
    deliver(0x2001, "{\"reason\":6,\"extras\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]}");
    assert(!strcmp(trace, "LMRPAamD") && status().end == XIAOTAI_WECHAT_END_PEER_HANGUP);
    assert(status().peer_reason == 6 && !status().releasing && !command_count);
}
static void make_active(void *ctx) { (void)ctx; active(); }
static void end_case(void *ctx)
{
    (void)ctx;
    active();
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK && status().releasing && status().end == XIAOTAI_WECHAT_END_LOCAL);
    assert(!strcmp(trace, "LMRPAamH") && command_word == 0x2001 && !strcmp(command_body, "{\"reason\":0}"));
    assert(!attachment_generation && !media_gen && link && xiaotai_activity_current(NULL));
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_BUSY && !requests);
}
static void verify_release(void *ctx)
{
    (void)ctx;
    assert(!status().releasing && !link && !xiaotai_activity_current(NULL));
    assert(!strcmp(trace, "LMRPAamHD"));
    accepting();
    link_handlers.opened(link_handlers.ctx, generation);
    /* Joined but 0x2000 pending: hang up that link, never reject the room. */
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK && command_count == 2 && refusals == 0);
}
static void after_pause(void *ctx) { (void)ctx; assert(status().releasing && link); }

static void failed_local_hangup(void *ctx)
{
    (void)ctx;
    active();
    fail_hangup = XIAOTAI_ERR_BUSY;
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    assert(status().end == XIAOTAI_WECHAT_END_LOCAL && status().error == fail_hangup);
    assert(status().state == XIAOTAI_WECHAT_IDLE && !status().releasing);
    assert(!media_gen && !link && !xiaotai_activity_current(NULL));
    assert(command_count == 1 && command_word == 0x2001 && !strcmp(command_body, "{\"reason\":0}"));
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_ERR_INVALID_STATE && command_count == 1);
}

static void failures(void *ctx)
{
    (void)ctx;
    fail_connect = XIAOTAI_ERR_NO_MEM;
    wx_announce(&incoming);
    assert(xiaotai_wechat_accept(0) == XIAOTAI_ERR_NO_MEM && status().end == XIAOTAI_WECHAT_END_NO_MEMORY);
    assert(!link && !xiaotai_activity_current(NULL));
    fail_connect = XIAOTAI_OK;
    fail_media = XIAOTAI_ERR_NOT_SUPPORTED;
    accepting();
    link_handlers.opened(link_handlers.ctx, generation);
    deliver(0x2000, NULL);
    assert(status().end == XIAOTAI_WECHAT_END_AUDIO && status().error == fail_media);
    assert(!link && !media_gen && !status().releasing && !status().answered);
    fail_media = XIAOTAI_OK;
    active();
    const uint32_t before = status().stale_calls;
    link_handlers.closed(link_handlers.ctx, generation + 1U, XIAOTAI_LINK_END_PEER, 0);
    assert(status().state == XIAOTAI_WECHAT_ACTIVE && status().stale_calls == before + 1U);
    const unsigned old_refusals = refusals;
    link = 0; /* The transport releases a link before giving its closed callback. */
    link_handlers.closed(link_handlers.ctx, generation, XIAOTAI_LINK_END_NETWORK, 0);
    assert(status().end == XIAOTAI_WECHAT_END_NETWORK && !media_gen && refusals == old_refusals);
}

static void emergency(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t a = {.name = "emergency"};
    uint32_t id;
    assert(xiaotai_activity_begin(&a, &id) == XIAOTAI_OK);
    assert(status().end == XIAOTAI_WECHAT_END_PREEMPTED && !status().releasing && !link && command_count == 1);
    assert(!strcmp(trace, "LMRPAamHD"));
    xiaotai_activity_end(id);
}

static void quiesce_case(void *ctx)
{
    (void)ctx;
    active();
    assert(xiaotai_quiesce_begin("maintenance", &quiesce_ticket) == XIAOTAI_OK);
    assert(status().end == XIAOTAI_WECHAT_END_SYSTEM && !link && !status().releasing && command_count == 1);
    wx_announce(&incoming);
    assert(refusals == 1 && refusal_reason == 8 && !status().session);
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_BUSY);
    xiaotai_quiesce_end(quiesce_ticket);
    wx_announce(&incoming);
    assert(status().state == XIAOTAI_WECHAT_INCOMING);
}
static void platform_offline(void *ctx)
{
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, &(xiaotai_args_t){.value = (int32_t)(uintptr_t)ctx},
                             NULL, 0) == XIAOTAI_OK);
}
static void platform_active(void *ctx)
{
    (void)ctx; active(); platform_offline((void *)(uintptr_t)XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
}
static void platform_request(void *ctx)
{
    (void)ctx;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK);
    platform_offline((void *)(uintptr_t)XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
}
static void platform_room(void *ctx)
{
    (void)ctx;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK);
    const xiaotai_platform_reply_t r = {
        .http_status = 200, .generation = identity_generation,
    };
    announce_join = true;
    reply_fn(&r, reply_ctx);
    announce_join = false;
    platform_offline((void *)(uintptr_t)XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
}
static void binding_offline(void *ctx)
{
    (void)ctx; platform_offline((void *)(uintptr_t)XIAOTAI_PLATFORM_OFFLINE_BINDING);
}
static void mute_setting(bool value)
{
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, value) == XIAOTAI_OK);
    mic = value;
    xiaotai_args_t args = {.value = value, .text = XIAOTAI_SETTING_AUDIO_MIC_MUTED};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, &args, NULL, 0) == XIAOTAI_OK);
}
static void mute_case(void *ctx)
{
    (void)ctx;
    mute_setting(true);
    active();
    assert(status().muted && mic);
    assert(xiaotai_wechat_set_muted(status().session + 1, false) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_wechat_set_muted(0, false) == XIAOTAI_OK && status().muted && mic);
    mute_setting(false);
}
static void verify_mute(void *ctx)
{
    (void)ctx;
    assert(!status().muted && !mic && status().stale_calls == 1);
    assert(xiaotai_wechat_set_muted(0, true) == XIAOTAI_OK && status().muted && mic);
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK && !mic);
    int32_t saved = 1;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &saved) == XIAOTAI_OK && saved == 0);
}
static void blocked(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t a = {.name = "recognition", .resources = XIAOTAI_RESOURCE_AUDIO_IN};
    uint32_t id;
    assert(xiaotai_activity_begin(&a, &id) == XIAOTAI_OK);
    wx_announce(&incoming);
    assert(!status().session && xiaotai_activity_current(NULL) == id && refusal_reason == 8 && refusals == 1);
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK && xiaotai_activity_current(NULL) != id);
    wx_announce(&incoming);
    assert(refusals == 2 && refusal_reason == 5);
}
static void emit_contacts(void)
{
    xiaotai_contacts_status_t snapshot;
    xiaotai_contacts_status(&snapshot);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_CONTACTS_CHANGED, NULL, &snapshot, sizeof(snapshot)) == XIAOTAI_OK);
}
static void contacts_case(void *ctx)
{
    (void)ctx;
    assert(contact_kind && strcmp(contact_kind->name, "wechat") == 0 && strcmp(contact_kind->alias, "WX") == 0 &&
           strcmp(contact_kind->server_type, "voip") == 0 && !contact_kind->presence && !contact_kind->friendships &&
           !strcmp(contact_kind->extra_fields[0], "wx_app_id") && !strcmp(contact_kind->extra_fields[1], "wx_model_id"));
    assert(xiaotai_wechat_dial_contact(1, 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    contact_missing_app = true;
    if (wx_config()->require_app_id) {
        assert(contact_kind->extra_required == 1 && update_fn == NULL);
        assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
        contact_missing_app = false;
        contact_fresh = false;
        assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND && !contact_flags);
        return;
    }
    assert(!contact_kind->extra_required && update_fn);
    contact_fresh = false;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK && contact_flags == XIAOTAI_CONTACTS_STALE_OK);
    assert(strstr(request_body, "\"wx_app_id\":\"\"") && strstr(request_body, "\"wx_user_openid\":\"fake-openid\""));
    const xiaotai_platform_reply_t reply = {.http_status = 200, .generation = identity_generation, .code = 40205};
    reply_fn(&reply, reply_ctx);
    assert(status().end == XIAOTAI_WECHAT_END_CONTACT && refreshes == 1);
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    emit_contacts();
}
static void contacts_recover(void *ctx)
{
    (void)ctx;
    if (wx_config()->require_app_id) return;
    const unsigned old_reads = reads;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND && reads == old_reads);
    contact_fresh = true; ++contacts_revision;
    emit_contacts();
}
static void contacts_signal(void *ctx)
{
    (void)ctx;
    if (wx_config()->require_app_id) return;
    xiaotai_platform_signal_t signal = {.type = "callers_update", .channel = "phone", .generation = identity_generation};
    update_fn(&signal, NULL);
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK);
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    signal.channel = "wx"; signal.type = "contacts_update"; update_fn(&signal, NULL);
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK);
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    signal.type = "callers_update"; update_fn(&signal, NULL);
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    ++identity_generation;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
}

static void contacts_mz28_refuses_stale(void *ctx)
{
    (void)ctx;
    const unsigned old_reads = reads;
    assert(xiaotai_wechat_dial_contact(0, 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    assert(!status().session && reads == old_reads);
}

static void contacts_mz28_invalidate(void *ctx)
{
    assert(contact_kind->extra_required == 1 && update_fn && !wx_config()->require_fresh_contacts);
    contact_fresh = false;
    const xiaotai_platform_signal_t signal = {
        .type = "callers_update", .channel = "wx", .generation = identity_generation,
    };
    update_fn(&signal, NULL);
    contacts_mz28_refuses_stale(ctx);
    emit_contacts();
}

static void start_timeout(void *ctx) { (void)ctx; wx_announce(&incoming); }
static void check_ring(void *ctx) { (void)ctx; assert(status().state == XIAOTAI_WECHAT_INCOMING); }
static void check_missed(void *ctx)
{
    (void)ctx;
    assert(status().end == XIAOTAI_WECHAT_END_MISSED && status().error == XIAOTAI_ERR_TIMEOUT && refusal_reason == wx_config()->reasons.missed);
}
static void start_connect(void *ctx) { (void)ctx; accepting(); }
static void mark_open(void *ctx) { (void)ctx; link_handlers.opened(link_handlers.ctx, generation); }
static void check_waiting(void *ctx) { (void)ctx; assert(status().state == XIAOTAI_WECHAT_CONNECTING && !media_gen); }
static void check_answer_timeout(void *ctx)
{
    (void)ctx;
    assert(status().end == XIAOTAI_WECHAT_END_NO_ANSWER && status().releasing && !media_gen);
}
static void start_request(void *ctx)
{
    (void)ctx;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK);
    assert(request_timeout == wx_config()->request_timeout_ms && !link);
}
static void check_request(void *ctx) { (void)ctx; assert(status().state == XIAOTAI_WECHAT_OUTGOING); }
static void check_request_timeout(void *ctx)
{
    (void)ctx;
    assert(status().end == XIAOTAI_WECHAT_END_NO_ANSWER && status().error == XIAOTAI_ERR_TIMEOUT && !link);
}
static void log_case(void *ctx)
{
    (void)ctx;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK && request_timeout == wx_config()->request_timeout_ms);
    uint32_t dial_id = status().session;
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    active();
    uint32_t answer_id = status().session, link_id = status().link;
    deliver(0x2001, "{\"reason\":8}");
    xiaotai_eventlog_entry_t events[12];
    const size_t n = xiaotai_eventlog_read(0, events, 12);
    size_t seen = 0;
    const char *const labels[] = {"outgoing", "ended", "incoming", "connected", "ended"};
    for (size_t i = 0; i < n; ++i) {
        const xiaotai_eventlog_entry_t *event = &events[i];
        if (strcmp(event->source, "wechat") != 0) continue;
        assert(seen < 5 && strcmp(event->label, labels[seen]) == 0);
        assert(event->generation == (seen < 2 ? dial_id : answer_id));
        if (seen == 3) assert(event->value == (int32_t)link_id);
        ++seen;
    }
    assert(seen == 5);
}

static void reset(void *ctx) { (void)ctx; xiaotai_wechat_reset(); }
static void restart(void) {}

#ifdef XIAOTAI_WECHAT_REAL_ROUTE
static void push(unsigned which, const char *channel, uint32_t gen, const char *payload)
{
    assert(which < sizeof(signal_types) / sizeof(*signal_types) && signal_fns[which]);
    char envelope[8192];
    const int size = snprintf(envelope, sizeof(envelope), "{\"type\":\"%s\",\"channel\":\"%s\",\"payload\":%s}",
                              signal_types[which], channel, payload);
    assert(size > 0 && (size_t)size < sizeof(envelope));
    const xiaotai_platform_signal_t signal = {
        .type = signal_types[which], .channel = channel, .json = envelope, .size = (size_t)size, .generation = gen,
    };
    signal_fns[which](&signal, NULL);
    memset(envelope, 'x', (size_t)size); /* No references survive the platform callback. */
}

static void room(const char *room_id, const char *call, const char *from, const char *who, const char *app)
{
    char payload[768];
    const int n = snprintf(payload, sizeof(payload),
        "{\"wx_room_id\":\"%s\",\"wx_call_id\":\"%s\",\"wx_from\":\"%s\",\"wx_user_openid\":\"%s\",\"wx_app_id\":\"%s\","
        "\"wx_room_type\":\"voice\",\"peer_id\":\"whip-peer\",\"token\":\"whip-test-token\","
        "\"wx_model_id\":\"model\",\"wx_server_token\":\"server-test-token\",\"wx_payload\":\"test-payload\"}",
        room_id, call, from, who, app);
    assert(n > 0 && (size_t)n < sizeof(payload));
    push(0, "wx", identity_generation, payload);
}

static void cancel_room(unsigned which, const char *name)
{
    char payload[256];
    const int n = snprintf(payload, sizeof(payload), "{\"wx_room_id\":\"%s\"}", name);
    assert(n > 0 && (size_t)n < sizeof(payload));
    push(which, "wx", identity_generation, payload);
}

static void http(const char *body, int http_status, int32_t code)
{
    assert(reply_fn);
    const xiaotai_platform_reply_t response = {.result = XIAOTAI_OK, .http_status = http_status, .code = code,
        .generation = identity_generation, .body = body, .size = body ? strlen(body) : 0};
    reply_fn(&response, reply_ctx);
}

static void dial_once(void)
{
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_OK);
    assert(status().state == XIAOTAI_WECHAT_OUTGOING);
}

static void routing_incoming(void *unused)
{
    (void)unused;
    const char minimal[] = "{\"wx_room_id\":\"fixture-room\",\"peer_id\":\"whip-peer\",\"token\":\"whip-test-token\"}";
    push(0, "device", identity_generation, minimal);
    push(0, "wx", identity_generation - 1U, minimal);
    push(0, "wx", identity_generation, "{\"wx_room_id\":\"fixture-room\",\"peer_id\":\"whip-peer\",\"token\":\"\"}");
    push(0, "wx", identity_generation, "{\"wx_room_id\":\"fixture-room\",\"peer_id\":42,\"token\":\"token\"}");
    assert(!status().session && !refusals);
    push(0, "wx", identity_generation, minimal);
    assert(status().state == XIAOTAI_WECHAT_INCOMING && status().announced == 1);
    room("fixture-room", "call-different", "fake-device", "", "");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && status().announced == 1 && !refusals);
    cancel_room(3, "unrelated");
    assert(status().session && !command_count);
    const uint32_t session = status().session;
    push(3, "wx", identity_generation, "{}");
    push(3, "wx", identity_generation, "{\"wx_room_id\":\"\"}");
    push(3, "wx", identity_generation, "{\"wx_room_id\":null}");
    assert(status().session == session && status().state == XIAOTAI_WECHAT_INCOMING && !command_count);
    cancel_room(3, "fixture-room");
    assert(status().end == XIAOTAI_WECHAT_END_PEER_REJECT && !status().session && !refusals);
    cancel_room(3, "fixture-room");
    assert(status().end == XIAOTAI_WECHAT_END_PEER_REJECT && !status().session && !refusals && !command_count);
    room("fixture-room", "", "", "", "");
    if (legacy_incoming_policy) {
        assert(status().session && status().session != session && status().announced == 2 && !refusals);
    } else assert(!status().session && !refusals);
}

static void routing_early(void *unused)
{
    (void)unused;
    dial_once();
    const uint32_t session = status().session;
    room("stale-app", "bad-1", "fake-device", "", "other");
    assert(refusals == 1 && refusal_reason == wx_config()->reasons.stale);
    room("stale-openid", "bad-2", "fake-device", "other", "app");
    assert(refusals == 2 && refusal_reason == wx_config()->reasons.stale);
    room("inbound-other", "incoming", "other-device", "", "");
    assert(refusals == 3 && refusal_reason == wx_config()->reasons.busy_dialing);
    room("no-id", "", "", "", "");
    assert(refusals == 4 && refusal_reason == (legacy_incoming_policy ? wx_config()->reasons.busy_dialing :
                                                                     wx_config()->reasons.stale));
    assert(status().session == session && status().state == XIAOTAI_WECHAT_OUTGOING && !link);
    room("early-old", "early-old-id", "", "fake-openid", "app");
    room("early-old", "early-old-id", "", "fake-openid", "app");
    room("fixture-room", "exact-id", "fake-device", "", "");
    room("too-many", "discard-id", "fake-device", "", "");
    assert(refusals == 5 && refusal_reason == wx_config()->reasons.overflow);
    room("collision", "exact-id", "fake-device", "", "");
    assert(refusals == 6 && refusal_reason == wx_config()->reasons.overflow);
    assert(!link && status().state == XIAOTAI_WECHAT_OUTGOING);
    http("{\"code\":0,\"data\":{\"call_id\":\"exact-id\"}}", 200, 0);
    assert(!link && status().state == XIAOTAI_WECHAT_OUTGOING);
    room("fixture-room", "exact-id", "fake-device", "", "");
    assert(refusals == 6);
}

static void routing_open(void *unused)
{
    (void)unused;
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && link && !media_gen);
    link_handlers.opened(link_handlers.ctx, link);
    deliver(0x2000U, NULL);
    assert(status().state == XIAOTAI_WECHAT_ACTIVE && media_gen);
    cancel_room(2, "fixture-room");
    assert(status().end == XIAOTAI_WECHAT_END_PEER_HANGUP && !link && !media_gen && !command_count);
}

static void xmini_early_reply(void *unused)
{
    (void)unused;
    dial_once();
    const xiaotai_platform_reply_fn_t old_reply = reply_fn;
    void *const old_ctx = reply_ctx;
    assert(xiaotai_wechat_hangup(status().session) == XIAOTAI_OK && !status().session);

    dial_once();
    const uint32_t current = status().session;
    room("wx-phone-room", "", "phone", "fake-openid", "app");
    room("wx-other-room", "wx-other-id", "fake-device", "fake-openid", "app");
    room("wx-early-room", "wx-early-id", "", "fake-openid", "app");
    room("wx-early-room", "wx-early-id", "", "fake-openid", "app");
    assert(refusals == 1U && status().state == XIAOTAI_WECHAT_OUTGOING && !link);

    static const char stale[] = "{\"code\":0,\"data\":{\"call_id\":\"wx-other-id\"}}";
    const xiaotai_platform_reply_t old = {.result = XIAOTAI_OK, .http_status = 200, .code = 0,
        .generation = identity_generation, .body = stale, .size = sizeof(stale) - 1U};
    old_reply(&old, old_ctx);
    assert(status().session == current && status().state == XIAOTAI_WECHAT_OUTGOING && !link);

    http("{\"code\":0,\"data\":{\"call_id\":\"wx-early-id\"}}", 200, 0);
    assert(status().session == current && wx_same_room("wx-early-room"));
    /* The next owner turn must connect from the cached room without MQTT redelivery. */
}

static void xmini_early_peer_ends_before_answer(void *unused)
{
    (void)unused;
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && link && wx_same_room("wx-early-room"));
    const char *first_link = strchr(trace, 'L');
    assert(first_link && !strchr(first_link + 1, 'L') && !media_gen);
    link_handlers.opened(link_handlers.ctx, link);
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && !media_gen && !strchr(trace, 'M'));

    deliver(0x2001U, "{\"reason\":1}");
    assert(status().state == XIAOTAI_WECHAT_IDLE && status().end == XIAOTAI_WECHAT_END_PEER_HANGUP);
    assert(status().peer_reason == 1 && !status().session && !link && !media_gen && !command_count);
    assert(refusals == 1U && !strchr(trace, 'M') && !xiaotai_activity_current(NULL));

    room("wx-incoming-no-call-id", "", "", "", "");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && status().session && !link && !media_gen && !strchr(trace, 'M'));
}

static const char xmini_valid_reply[] = "{\"code\":0,\"data\":{\"call_id\":\"wx-invalid-id\"}}";
static const struct { int http; xiaotai_err_t result; const char *body; } xmini_bad_replies[] = {
    {201, XIAOTAI_OK, xmini_valid_reply},
    {503, XIAOTAI_FAIL, xmini_valid_reply},
    {200, XIAOTAI_OK, "{\"code\":200,\"data\":{\"call_id\":\"wx-invalid-id\"}}"},
    {200, XIAOTAI_OK, "{\"code\":0.5,\"data\":{\"call_id\":\"wx-invalid-id\"}}"},
    {200, XIAOTAI_OK, "{\"code\":0,\"data\":{\"call_id\":\"\"}}"},
    {200, XIAOTAI_OK, "{\"code\":0,\"data\":{}}"},
    {200, XIAOTAI_OK, "{\"code\":0,\"data\":{\"call_id\":\"wx-invalid-id\"}}trailing"},
    {0, XIAOTAI_ERR_TIMEOUT, NULL},
    {200, XIAOTAI_ERR_PLATFORM_STALE, xmini_valid_reply},
};

static void xmini_buffered_reply_failure(void *ctx)
{
    const size_t i = *(const size_t *)ctx;
    dial_once();
    const int64_t before_room = xiaotai_mem_host_outstanding();
    char room_id[32];
    const int length = snprintf(room_id, sizeof(room_id), "wx-invalid-room-%zu", i);
    assert(length > 0 && (size_t)length < sizeof(room_id));
    room(room_id, "wx-invalid-id", "fake-device", "fake-openid", "app");
    assert(xiaotai_mem_host_outstanding() > before_room && !link && !media_gen && !refusals);

    /* The platform strips response bytes for transport and epoch errors but keeps the binding. */
    const xiaotai_err_t error = xmini_bad_replies[i].result;
    const char *body = error == XIAOTAI_OK ? xmini_bad_replies[i].body : NULL;
    xiaotai_platform_reply_t response = {.result = error, .http_status = xmini_bad_replies[i].http,
        .generation = identity_generation, .code = XIAOTAI_PLATFORM_NO_CODE,
        .body = body, .size = body ? strlen(body) : 0U};
    if (body) (void)xiaotai_platform_json_code(body, response.size, &response.code);
    reply_fn(&response, reply_ctx);
    assert(status().state == XIAOTAI_WECHAT_IDLE && status().end == XIAOTAI_WECHAT_END_FAILED);
    assert(status().error != XIAOTAI_OK && !status().session && !link && !media_gen);
}

static void routing_xmini_buffered_failures(void)
{
    run(NULL);
    const int64_t idle_allocations = xiaotai_mem_host_outstanding();
    for (size_t i = 0; i < sizeof(xmini_bad_replies) / sizeof(*xmini_bad_replies); ++i) {
        assert(xiaotai_run(xmini_buffered_reply_failure, &i) == XIAOTAI_OK);
        run(NULL);
        assert(xiaotai_mem_host_outstanding() == idle_allocations);
        assert(!strchr(trace, 'L') && !strchr(trace, 'M'));
    }
}

static void json_view_contract(void *unused)
{
    (void)unused;
    const size_t limit = external ? 96U : 64U;
    xiaotai_json_token_t tokens[96];
    xiaotai_json_view_t root;
    char array[256];
    for (size_t count = limit; count <= limit + 1U; ++count) {
        size_t size = 0;
        array[size++] = '[';
        for (size_t i = 1; i < count; ++i) {
            if (i > 1) array[size++] = ',';
            array[size++] = '0';
        }
        array[size++] = ']'; /* Exact-size non-NUL input. */
        const xiaotai_err_t original = xiaotai_json_parse(array, size, tokens, limit, NULL, NULL);
        assert(original == (count == limit ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE));
        assert(wx_parse(array, size, &root) == original);
        assert(count == limit ? root.type == XIAOTAI_JSON_ARRAY : !root.text);
        array[size - 1] = '}'; /* Syntax errors still win over the token limit. */
        assert(wx_parse(array, size, &root) == xiaotai_json_parse(array, size, tokens, limit, NULL, NULL));
        assert(!root.text);
    }
    static const char *const messages[] = {
        "{\"payload\":{\"wx_room\\u005fid\":\"first\",\"wx_room_id\":\"second\","
        "\"peer_id\":\"peer\",\"token\":\"t\",\"wx_room_type\":\"voice\",\"wx_room_type\":\"video\"},"
        "\"payload\":null}",
        "{\"payload\":{\"wx_room_id\":null,\"wx_room_id\":\"second\",\"peer_id\":\"peer\",\"token\":\"t\"}}",
        "{\"payload\":null,\"payload\":{\"wx_room_id\":\"second\",\"peer_id\":\"peer\",\"token\":\"t\"}}",
    };
    wx_room_t decoded;
    assert(wx_signal_room(messages[0], strlen(messages[0]), &decoded));
    assert(!strcmp(decoded.room_id, "first") && decoded.type == WX_TYPE_VOICE);
    assert(!wx_signal_room(messages[1], strlen(messages[1]), &decoded));
    assert(!wx_signal_room(messages[2], strlen(messages[2]), &decoded));
    dial_once();
    room("fixture-room", "first-id", "fake-device", "", "");
    http("{\"code\":0,\"code\":99,\"data\":{\"call_id\":\"first-id\",\"call_id\":\"other\"}}", 200, 0);
    assert(wx_same_room("fixture-room") && status().state == XIAOTAI_WECHAT_OUTGOING && !link);
}

static void routing_http_first(void *unused)
{
    (void)unused;
    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":\"exact-id\"}}", 200, 0);
    assert(!link && status().state == XIAOTAI_WECHAT_OUTGOING);
    room("wrong-call", "different", "fake-device", "", "");
    assert(refusals == 1 && refusal_reason == wx_config()->reasons.stale);
    room("fixture-room", "exact-id", "", "fake-openid", "app");
    assert(!link);
}

static void routing_reply_replay(void *unused)
{
    (void)unused;
    dial_once();
    const xiaotai_platform_reply_fn_t retired_reply = reply_fn;
    void *const retired_ctx = reply_ctx;
    http("{\"code\":0,\"data\":{}}", 200, 0);
    assert(!status().session && status().end == XIAOTAI_WECHAT_END_FAILED && !link);

    dial_once();
    const uint32_t session = status().session;
    const uint32_t activity = xiaotai_activity_current(NULL);
    const unsigned count = requests;
    assert(session && activity);
    static const char confirmed[] = "{\"code\":0,\"data\":{\"call_id\":\"exact-id\"}}";
    static const char different[] = "{\"code\":0,\"data\":{\"call_id\":\"different\"}}";
    http(confirmed, 200, 0);
    assert(status().session == session && status().state == XIAOTAI_WECHAT_OUTGOING && !link);

    http("{\"code\":40205,\"data\":{\"call_id\":\"ignored\"}}", 200, 40205);
    assert(status().session == session && status().state == XIAOTAI_WECHAT_OUTGOING && !link);
    http("{\"code\":6006}", 200, 6006);
    http("{\"code\":0,\"data\":{}}", 200, 0);
    http(confirmed, 200, 0);
    http(different, 200, 0);
    static const char retired[] = "{\"code\":0,\"data\":{\"call_id\":\"retired-id\"}}";
    xiaotai_platform_reply_t replay = {.result = XIAOTAI_OK, .http_status = 200, .code = 0,
        .generation = identity_generation, .body = retired, .size = sizeof(retired) - 1U};
    retired_reply(&replay, retired_ctx);
    --replay.generation;
    reply_fn(&replay, reply_ctx);
    assert(status().session == session && status().state == XIAOTAI_WECHAT_OUTGOING && !link && !media_gen);
    assert(requests == count && !refreshes && !unbound_reports && xiaotai_activity_current(NULL) == activity);

    room("wrong-call", "different", "fake-device", "", "");
    assert(!wx_same_room("wrong-call") && !link && status().session == session);
    room("fixture-room", "exact-id", "", "fake-openid", "app");
    assert(wx_same_room("fixture-room") && !link && status().session == session);
}

static void routing_early_cancel(void *unused)
{
    (void)unused;
    dial_once();
    room("pre-cancelled", "exact", "", "", "");
    cancel_room(1, "pre-cancelled");
    room("pre-cancelled", "exact", "", "", "");
    http("{\"code\":0,\"data\":{\"call_id\":\"exact\"}}", 200, 0);
    assert(status().end == XIAOTAI_WECHAT_END_PEER_CANCEL && !status().session && !link && !command_count);
    dial_once();
    cancel_room(2, "cancel-before-push");
    room("cancel-before-push", "new-call", "fake-device", "", "");
    http("{\"code\":0,\"data\":{\"call_id\":\"new-call\"}}", 200, 0);
    assert(status().end == XIAOTAI_WECHAT_END_PEER_CANCEL && !status().session && !link);
    dial_once();
    room("rejected-before-answer", "call-rejected", "", "", "");
    cancel_room(3, "rejected-before-answer");
    http("{\"code\":0,\"data\":{\"call_id\":\"call-rejected\"}}", 200, 0);
    assert(status().end == XIAOTAI_WECHAT_END_PEER_REJECT && !link);
    dial_once();
    cancel_room(3, "reject-before-push");
    room("reject-before-push", "call-early-rejected", "", "", "");
    http("{\"code\":0,\"data\":{\"call_id\":\"call-early-rejected\"}}", 200, 0);
    assert(status().end == XIAOTAI_WECHAT_END_PEER_REJECT && !link);
}

static void routing_late(void *unused)
{
    (void)unused;
    dial_once();
    const xiaotai_platform_reply_fn_t old_handler = reply_fn;
    void *old_ctx = reply_ctx;
    room("early-redelivery", "old-id", "", "fake-openid", "");
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK && !status().session);
    room("early-redelivery", "old-id", "", "fake-openid", "");
    assert(!status().session && !refusals); /* cancelled while the HTTP reply was pending */
    dial_once();
    const uint32_t fresh = status().session;
    const char body[] = "{\"code\":0,\"data\":{\"call_id\":\"old-id\"}}";
    const xiaotai_platform_reply_t response = {.result = XIAOTAI_OK, .http_status = 200, .code = 0,
        .generation = identity_generation, .body = body, .size = sizeof(body) - 1U};
    old_handler(&response, old_ctx);
    room("late", "old-id", "", "", "");
    assert(!refusals && status().session == fresh && !link); /* retired call: S3 NONE */
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    room("late", "old-id", "", "", "");
    assert(!status().session && !refusals);
}

static void routing_reset_identity(void *unused)
{
    (void)unused;
    binding_offline(NULL);
}

static void routing_fresh(void *unused)
{
    (void)unused;
    ++identity_generation;
    room("late", "old-id", "", "", "");
    assert(status().state == XIAOTAI_WECHAT_INCOMING);
    cancel_room(1, "late");
    assert(status().end == XIAOTAI_WECHAT_END_PEER_CANCEL && !status().session);
}

static void confirmed_retired_start(void *unused)
{
    (void)unused;
    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":\"call-current\"}}", 200, 0);
    assert(status().state == XIAOTAI_WECHAT_OUTGOING && !link);
}

static void confirmed_retired_hangup(void *unused)
{
    (void)unused;
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK && status().end == XIAOTAI_WECHAT_END_LOCAL);
}

static void confirmed_retired_before_expiry(void *unused)
{
    (void)unused;
    const unsigned announced = status().announced;
    push(0, "wx", identity_generation, "{\"wx_room_id\":\"room-late\",\"wx_call_id\":\"call-current\","
         "\"peer_id\":\"whip-peer\",\"token\":\"whip-test-token\",\"wx_room_type\":\"voice\"}");
    assert(!status().session && status().announced == announced && requests == 1U && !refusals && !link);
    room("room-other", status().error == XIAOTAI_ERR_TIMEOUT ? "" : "call-other", "", "", "");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && status().announced == announced + 1U && !link);
    cancel_room(1, "room-other");
    assert(!status().session && requests == 1U && !refusals);
}

static void confirmed_retired_at_expiry(void *unused)
{
    (void)unused;
    push(0, "wx", identity_generation, "{\"wx_room_id\":\"room-fresh\",\"wx_call_id\":\"call-current\","
         "\"peer_id\":\"whip-peer\",\"token\":\"whip-test-token\",\"wx_room_type\":\"voice\"}");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && requests == 1U && !refusals && !link);
    cancel_room(1, "room-fresh");
}

static void check_authorization_replies(void)
{
    static const char denied[] = "{\"code\":40205,\"data\":{\"call_id\":\"ignored\"}}";
    const unsigned before = refreshes;
    dial_once();
    http(denied, 200, 40205);
    assert(status().end == XIAOTAI_WECHAT_END_CONTACT && !status().session && !link);
    http(denied, 200, 40205);
    assert(refreshes == before + 1U && !link);

    static const struct { int http; const char *json; } rejected[] = {
        {503, "{\"code\":40205}"}, {401, "{\"code\":40205}"},
        {200, "{\"code\":40205.5}"}, {200, "{\"code\":\"40205\"}"},
        {200, "{\"code\":40205,\"code\":0}"}, {200, "{\"code\":40205} extra"}, {200, "{\"code\":40900}"},
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        int32_t code = XIAOTAI_PLATFORM_NO_CODE;
        (void)xiaotai_platform_json_code(rejected[i].json, strlen(rejected[i].json), &code);
        dial_once();
        http(rejected[i].json, rejected[i].http, code);
        assert(status().end == XIAOTAI_WECHAT_END_FAILED && !status().session && !link);
        assert(refreshes == before + 1U && !unbound_reports);
    }

    dial_once();
    const xiaotai_platform_reply_fn_t old_reply = reply_fn;
    void *const old_ctx = reply_ctx;
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    dial_once();
    const uint32_t current = status().session;
    xiaotai_platform_reply_t response = {.result = XIAOTAI_OK, .http_status = 200, .code = 40205,
        .generation = identity_generation, .body = denied, .size = sizeof(denied) - 1U};
    old_reply(&response, old_ctx);
    --response.generation;
    reply_fn(&response, reply_ctx);
    assert(status().session == current && refreshes == before + 1U && !link && !unbound_reports);
    assert(xiaotai_wechat_hangup(current) == XIAOTAI_OK);
}

static void routing_errors(void *unused)
{
    (void)unused;
    check_authorization_replies();
    dial_once();
    http("{\"code\":6006}", 200, 6006);
    assert(!status().session && status().end == XIAOTAI_WECHAT_END_FAILED);
    assert(strchr(trace, 'U') && refreshes == 1 && unbound_reports == 1);
    const char *invalid[] = {"{\"code\":6006", "{\"code\":6006}tail", "{\"code\":6006,\"code\":6006}",
        "{\"code\":\"6006\"}", "{\"code\":6006.0}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        int32_t code = XIAOTAI_PLATFORM_NO_CODE;
        assert(xiaotai_platform_json_code(invalid[i], strlen(invalid[i]), &code) != XIAOTAI_OK);
        dial_once();
        http(invalid[i], 200, code);
        assert(status().end == XIAOTAI_WECHAT_END_FAILED && !link && unbound_reports == 1);
    }
    dial_once();
    http("{\"code\":6006}", 500, 6006);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !link && unbound_reports == 1);
    dial_once();
    const unsigned voip_requests = requests;
    http("{\"code\":40202,\"data\":{\"room_id\":\"d_old\"}}", 200, 40202);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !status().session && !link);
    assert(requests == voip_requests);
    const struct { int status; const char *body; } invalid_envelopes[] = {
        {201, "{\"code\":0,\"data\":{\"call_id\":\"exact\"}}"},
        {503, "{\"code\":0,\"data\":{\"call_id\":\"exact\"}}"},
        {200, "{\"code\":200,\"data\":{\"call_id\":\"exact\"}}"},
        {200, "{\"code\":0.5,\"data\":{\"call_id\":\"exact\"}}"},
        {200, "{\"code\":0,\"data\":{}}"},
    };
    for (size_t i = 0; i < sizeof(invalid_envelopes) / sizeof(*invalid_envelopes); ++i) {
        int32_t code = XIAOTAI_PLATFORM_NO_CODE;
        (void)xiaotai_platform_json_code(invalid_envelopes[i].body, strlen(invalid_envelopes[i].body), &code);
        dial_once();
        http(invalid_envelopes[i].body, invalid_envelopes[i].status, code);
        assert(status().end == XIAOTAI_WECHAT_END_FAILED && !status().session && !link);
    }
    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":\"\"}}", 200, 0);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !link);
    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":0}}", 200, 0);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !link);
    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":\"exact\"}}junk", 200, 0);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !link);
}

static void xmini_authorization_before_dismiss(void *unused)
{
    (void)unused;
    static const char fractional[] = "{\"code\":40205.5,\"data\":{\"call_id\":\"ignore\"}}";
    static const char denied[] = "{\"code\":40205,\"data\":{\"call_id\":\"ignore\"}}";
    int32_t code = XIAOTAI_PLATFORM_NO_CODE;
    assert(xiaotai_platform_json_code(fractional, sizeof(fractional) - 1U, &code) != XIAOTAI_OK);
    dial_once();
    const xiaotai_platform_reply_fn_t old_reply = reply_fn;
    void *const old_context = reply_ctx;
    http(fractional, 200, code);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !status().session && !refreshes && !link);

    assert(xiaotai_platform_json_code(denied, sizeof(denied) - 1U, &code) == XIAOTAI_OK && code == 40205);
    dial_once();
    const uint32_t current = status().session;
    const xiaotai_platform_reply_t stale = {.result = XIAOTAI_OK, .http_status = 200, .code = code,
        .generation = identity_generation, .body = denied, .size = sizeof(denied) - 1U};
    old_reply(&stale, old_context);
    assert(status().session == current && !refreshes && !link);

    http(denied, 200, code);
    assert(status().end == XIAOTAI_WECHAT_END_CONTACT && !status().session && !link && !strchr(trace, 'L'));
    assert(refreshes == 0U && "Xmini must wait for explicit dismissal of the authorization failure");
    assert(status().authorization_result == current);
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    http(denied, 200, code);
    xiaotai_wechat_result_dismissed(current + 1);
    assert(refreshes == 0U && !link && status().authorization_result == current);
    ++identity_generation;
    contacts_generation = identity_generation;
    xiaotai_wechat_result_dismissed(current);
    assert(refreshes == 0U && !status().authorization_result);

    dial_once();
    const uint32_t next = status().session;
    assert(next && next != current);
    http(denied, 200, code);
    assert(status().authorization_result == next && refreshes == 0U);
    xiaotai_wechat_result_dismissed(current);
    assert(status().authorization_result == next && refreshes == 0U);
    xiaotai_wechat_result_dismissed(next);
    xiaotai_wechat_result_dismissed(next);
    http(denied, 200, code);
    assert(refreshes == 1U && !status().authorization_result && !link);
}

static void mz28_authorization_reply(void *unused)
{
    (void)unused;
    contact_fresh = false; /* MZ28 allows the existing snapshot until the owner invalidates it. */
    static const char denied[] = "{\"code\":40205,\"data\":{\"call_id\":\"ignore-this-id\"}}";
    static const char duplicate[] = "{\"code\":40205,\"code\":0}";
    int32_t code = XIAOTAI_PLATFORM_NO_CODE;
    assert(xiaotai_platform_json_code(duplicate, sizeof(duplicate) - 1, &code) != XIAOTAI_OK);
    assert(code == XIAOTAI_PLATFORM_NO_CODE);
    dial_once();
    http(duplicate, 200, code);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !status().session && !refreshes);
    assert(xiaotai_platform_json_code(denied, sizeof(denied) - 1, &code) == XIAOTAI_OK && code == 40205);
    dial_once();
    http(denied, 503, code);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !status().session && !refreshes);

    dial_once();
    const xiaotai_platform_reply_fn_t old_reply = reply_fn;
    void *const old_context = reply_ctx;
    const xiaotai_platform_reply_t transport_failure = {.result = XIAOTAI_ERR_TIMEOUT,
        .generation = identity_generation, .code = XIAOTAI_PLATFORM_NO_CODE};
    reply_fn(&transport_failure, reply_ctx);
    assert(status().end == XIAOTAI_WECHAT_END_FAILED && !status().session && !refreshes);

    dial_once();
    xiaotai_platform_reply_t response = {.result = XIAOTAI_OK, .http_status = 200, .code = code,
        .generation = identity_generation, .body = denied, .size = sizeof(denied) - 1};
    const xiaotai_platform_reply_fn_t cancelled_reply = reply_fn;
    void *const cancelled_context = reply_ctx;
    assert(xiaotai_wechat_hangup(status().session) == XIAOTAI_OK && !status().session);
    cancelled_reply(&response, cancelled_context);
    assert(!status().session && !refreshes);

    dial_once();
    const uint32_t current = status().session;
    const int64_t before_room = xiaotai_mem_host_outstanding();
    room("early-auth", "ignore-this-id", "fake-device", "fake-openid", "app");
    assert(xiaotai_mem_host_outstanding() > before_room && !refusals);
    old_reply(&response, old_context); /* Wrong request generation, same binding. */
    --response.generation;
    reply_fn(&response, reply_ctx);  /* Right request, wrong binding. */
    assert(status().session == current && status().state == XIAOTAI_WECHAT_OUTGOING && !refreshes && !link);

    ++response.generation;
    reply_fn(&response, reply_ctx);
    assert(status().end == XIAOTAI_WECHAT_END_CONTACT && !status().session && refreshes == 1);
    assert(!link && !strchr(trace, 'L') && !unbound_reports && !command_count);
    reply_fn(&response, reply_ctx); /* A completed request no longer owns the module. */
    assert(refreshes == 1 && !status().session && !link && !unbound_reports);
}

static void mz28_contacts_remain_blocked(void *unused)
{
    (void)unused;
    const unsigned lookup_count = reads, request_count = requests;
    assert(xiaotai_wechat_dial("fake-openid", 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_wechat_dial_contact(0, 0, NULL) == XIAOTAI_ERR_NOT_FOUND);
    assert(reads == lookup_count && requests == request_count && refreshes == 1);
}

static void mz28_redial(void *unused)
{
    (void)unused;
    const unsigned lookup_count = reads, request_count = requests;
    assert(xiaotai_wechat_dial_contact(0, 0, NULL) == XIAOTAI_OK);
    assert(status().state == XIAOTAI_WECHAT_OUTGOING && reads == lookup_count + 1);
    assert(requests == request_count + 1 && refreshes == 1 && !link);
    assert(xiaotai_wechat_hangup(status().session) == XIAOTAI_OK && !status().session);
}

static void routing_mz28_authorization(void)
{
    run(NULL);
    const int64_t idle_allocations = xiaotai_mem_host_outstanding();
    run(mz28_authorization_reply);
    assert(xiaotai_mem_host_outstanding() == idle_allocations);
    run(mz28_contacts_remain_blocked);
    const xiaotai_contacts_status_t snapshots[] = {
        {.generation = identity_generation, .revision = contacts_revision, .fresh = true},
        {.generation = identity_generation - 1, .revision = contacts_revision + 1, .fresh = true},
        {.generation = identity_generation, .revision = contacts_revision + 1, .fresh = false},
    };
    for (size_t i = 0; i < sizeof(snapshots) / sizeof(*snapshots); ++i) {
        assert(xiaotai_event_emit(XIAOTAI_EVENT_CONTACTS_CHANGED, NULL,
                                  &snapshots[i], sizeof(snapshots[i])) == XIAOTAI_OK);
        run(mz28_contacts_remain_blocked);
    }
    contact_fresh = true;
    ++contacts_revision;
    emit_contacts();
    run(mz28_redial);
}

static void routing_retire_fill(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < (external ? 9U : 5U); ++i) {
        char id[24];
        snprintf(id, sizeof(id), "retire-%u", i);
        room(id, id, "", "", "");
        assert(status().state == XIAOTAI_WECHAT_INCOMING);
        cancel_room(1, id);
        assert(!status().session);
        atomic_fetch_add(&clock_us, 1);
    }
    room("retire-1", "retire-1", "", "", "");
    assert(!status().session);
    room("retire-0", "retire-0", "", "", "");
    assert(status().state == XIAOTAI_WECHAT_INCOMING);
    cancel_room(1, "retire-0");
}

static void routing_retire_expired(void *unused)
{
    (void)unused;
    room("retire-2", "retire-2", "", "", "");
    assert(status().state == XIAOTAI_WECHAT_INCOMING);
    cancel_room(1, "retire-2");
}

static void pad_room(char *payload, size_t length, const char *id)
{
    char prefix[300];
    const int prefix_size = snprintf(prefix, sizeof(prefix), "{\"wx_room_id\":\"fixture-room\","
        "\"wx_call_id\":\"%s\",\"peer_id\":\"whip-peer\",\"token\":\"whip-test-token\",\"extra\":\"", id);
    assert(prefix_size > 0 && (size_t)prefix_size + 4U < length);
    memcpy(payload, prefix, (size_t)prefix_size);
    memset(payload + prefix_size, 'z', length - (size_t)prefix_size - 3U);
    memcpy(payload + length - 3U, "\"}", 3U);
}

static void routing_budget(void *unused)
{
    (void)unused;
    dial_once();
    char payload[4300];
    pad_room(payload, sizeof(payload), "large");
    push(0, "wx", identity_generation, payload);
    assert(refusals == (external ? 1U : 0U));
    pad_room(payload, 4000U, "small");
    push(0, "wx", identity_generation, payload);
    assert(refusals == 1U && refusal_reason == 5 && !link);
    if (external) http("{\"code\":0,\"data\":{\"call_id\":\"small\"}}", 200, 0);
    else http("{\"code\":0,\"data\":{\"call_id\":\"large\"}}", 200, 0);
}

/* tests/regression/test_call_hangup.py: keep local end and the wire result distinct. */
static void legacy_hangup_incoming_connect(void *unused)
{
    (void)unused;
    room("fixture-room", "call-fixture", "remote-initiator", "", "app");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && !status().outgoing);
    assert(xiaotai_wechat_accept(status().session) == XIAOTAI_OK);
}

static void legacy_hangup_outgoing_connect(void *unused)
{
    (void)unused;
    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":\"call-fixture\"}}", 200, 0);
    room("fixture-room", "call-fixture", "", "fake-openid", "app");
}

static void legacy_hangup_preopen(void *unused)
{
    (void)unused;
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && !status().outgoing && link && !physical_open);
    const uint32_t session = status().session;
    assert(session && xiaotai_wechat_hangup(session) == XIAOTAI_OK);
    assert(status().state == XIAOTAI_WECHAT_IDLE && status().end == XIAOTAI_WECHAT_END_LOCAL);
    assert(status().error == XIAOTAI_OK && !status().releasing && !link && !media_gen && !xiaotai_activity_current(NULL));
    assert(!command_count && !requests && !refusals && !refusal_reply);
    assert(xiaotai_wechat_hangup(session) == XIAOTAI_ERR_INVALID_STATE);
}

static void legacy_hangup_join(void *unused)
{
    (void)unused;
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && link && !media_gen);
    physical_open = true;
    link_handlers.opened(link_handlers.ctx, generation);
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && !media_gen);
}

static void legacy_hangup_answer(void *unused)
{
    (void)unused;
    deliver(0x2000U, NULL);
    assert(status().state == XIAOTAI_WECHAT_ACTIVE && media_gen == link);
}

static void legacy_hangup_connected(void *unused)
{
    (void)unused;
    static const char payload[] = "{\"reason\":0}";
    const xiaotai_wechat_status_t before = status();
    const unsigned call_requests = before.outgoing ? 1U : 0U;
    assert(before.session && physical_open && requests == call_requests && !refusals && !command_count);
    assert(xiaotai_wechat_hangup(before.session) == XIAOTAI_OK);
    const xiaotai_wechat_status_t after = status();
    assert(after.state == XIAOTAI_WECHAT_IDLE && after.end == XIAOTAI_WECHAT_END_LOCAL && after.error == XIAOTAI_OK);
    assert(!after.session && !after.link && after.releasing && link && !media_gen);
    assert(command_count == 1U && command_envelope == 0x2001U && command_size == sizeof(payload) - 1U);
    assert(!memcmp(command_body, payload, sizeof(payload) - 1U) && requests == call_requests && !refusals);
    assert(xiaotai_wechat_hangup(before.session) == XIAOTAI_ERR_INVALID_STATE && command_count == 1U);
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_ERR_INVALID_STATE && command_count == 1U);
    assert(requests == call_requests && !refusals && !refusal_reply);
}

static bool routing_tests(const char *scenario)
{
    if (!strcmp(scenario, "routes")) run(routing_incoming);
    else if (!strcmp(scenario, "json-view")) { run(json_view_contract); run(routing_open); }
    else if (!strcmp(scenario, "early")) { run(routing_early); run(routing_open); }
    else if (!strcmp(scenario, "xmini-early")) { run(xmini_early_reply); run(xmini_early_peer_ends_before_answer); }
    else if (!strcmp(scenario, "xmini-buffered-failures")) routing_xmini_buffered_failures();
    else if (!strcmp(scenario, "http-first")) { run(routing_http_first); run(routing_open); }
    else if (!strcmp(scenario, "reply-replay")) { run(routing_reply_replay); run(routing_open); }
    else if (!strcmp(scenario, "early-cancel")) run(routing_early_cancel);
    else if (!strcmp(scenario, "late")) { run(routing_late); run(routing_reset_identity); run(routing_fresh); }
    else if (!strncmp(scenario, "confirmed-retired-", sizeof("confirmed-retired-") - 1U)) {
        run(confirmed_retired_start);
        if (!strcmp(scenario, "confirmed-retired-timeout")) {
            at(atomic_load(&clock_us) + (int64_t)wx_config()->connect_timeout_ms * 1000);
            assert(status().end == XIAOTAI_WECHAT_END_NO_ANSWER && status().error == XIAOTAI_ERR_TIMEOUT);
        } else {
            run(confirmed_retired_hangup);
        }
        const int64_t retired_at = atomic_load(&clock_us);
        assert(!status().session && !link && !refusals && ended_events == 1U);
        at(retired_at + 59999000);
        run(confirmed_retired_before_expiry);
        at(retired_at + 60000000);
        run(confirmed_retired_at_expiry);
    } else if (!strcmp(scenario, "http-errors")) run(routing_errors);
    else if (!strcmp(scenario, "xmini-authorization")) run(xmini_authorization_before_dismiss);
    else if (!strcmp(scenario, "mz28-authorization")) routing_mz28_authorization();
    else if (!strcmp(scenario, "budget")) { run(routing_budget); run(routing_open); }
    else if (!strcmp(scenario, "legacy-hangup-preopen")) {
        run(legacy_hangup_incoming_connect);
        run(legacy_hangup_preopen);
        assert(!command_count && !requests && !refusals && ended_events == 1U);
    } else if (!strncmp(scenario, "legacy-hangup-joined-", sizeof("legacy-hangup-joined-") - 1U) ||
               !strncmp(scenario, "legacy-hangup-active-", sizeof("legacy-hangup-active-") - 1U)) {
        const bool outgoing = strstr(scenario, "outgoing") != NULL;
        run(outgoing ? legacy_hangup_outgoing_connect : legacy_hangup_incoming_connect);
        assert(status().outgoing == outgoing);
        run(legacy_hangup_join);
        if (strstr(scenario, "active")) run(legacy_hangup_answer);
        run(legacy_hangup_connected);
        at(150000);
        assert(status().releasing && link && ended_events == 1U);
        at(150001);
        assert(!status().releasing && !link && !physical_open && !xiaotai_activity_current(NULL));
        assert(command_count == 1U && requests == (outgoing ? 1U : 0U) && !refusals && ended_events == 1U);
    } else if (!strcmp(scenario, "retired")) {
        run(routing_retire_fill);
        at(60000020);
        run(routing_retire_expired);
    } else return false;
    return true;
}
#endif

#ifdef XIAOTAI_WECHAT_REAL_REFUSAL
static void response(xiaotai_err_t result)
{
    assert(refusal_reply);
    const xiaotai_transport_reply_fn_t fn = refusal_reply;
    void *const arg = refusal_ctx;
    refusal_reply = NULL;
    refusal_ctx = NULL;
    fn(arg, result, 0, NULL, 0);
}

static void key_text(const xiaotai_json_doc_t *doc, const char *key, const char *value)
{
    assert(xiaotai_json_equals(doc, xiaotai_json_get(doc, 0, key), value));
}

static void key_int(const xiaotai_json_doc_t *doc, const char *key, int64_t value)
{
    int64_t n;
    assert(xiaotai_json_int(doc, xiaotai_json_get(doc, 0, key), &n) == XIAOTAI_OK && n == value);
}

static xiaotai_err_t parse_emitted(const char *body, size_t size, xiaotai_json_doc_t *doc)
{
    static xiaotai_json_token_t tokens[96];
    return xiaotai_json_parse(body, size, tokens, sizeof(tokens) / sizeof(*tokens), NULL, doc);
}

static void legacy_hangup_reject(void *unused)
{
    (void)unused;
    room("fixture-room", "call-fixture", "remote-initiator", "", "app");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && !link);
    const uint32_t session = status().session;
    assert(session && xiaotai_wechat_reject(session) == XIAOTAI_OK);
    assert(status().state == XIAOTAI_WECHAT_IDLE && status().end == XIAOTAI_WECHAT_END_LOCAL);
    assert(status().error == XIAOTAI_OK && !link && !media_gen && !xiaotai_activity_current(NULL));
    assert(!command_count && !requests && refusals == 1U);
    xiaotai_json_doc_t doc;
    assert(parse_emitted(refusal_body, strlen(refusal_body), &doc) == XIAOTAI_OK);
    key_text(&doc, "wx_app_id", "app");
    key_text(&doc, "wx_model_id", "model");
    key_text(&doc, "wx_session_token", "server-test-token");
    key_text(&doc, "wx_room_id", "fixture-room");
    key_text(&doc, "wx_payload", "test-payload");
    key_int(&doc, "hangup_reason", 7);
    assert(xiaotai_wechat_reject(session) == XIAOTAI_ERR_INVALID_STATE && refusals == 1U);
    response(XIAOTAI_OK);
}

static void check_invalid(uint32_t expected)
{
    uint32_t extra, lost, invalid;
    wx_diagnostics(&extra, &lost, &invalid);
    assert(invalid == expected);
}

static void refusal_admission(void *ctx)
{
    (void)ctx;
    wx_room_t room = incoming;
    memcpy(room.payload, "a\"b", 4);
    wx_refuse(&room, 7, false);
    xiaotai_json_doc_t doc;
    assert(parse_emitted(refusal_body, strlen(refusal_body), &doc) == XIAOTAI_OK);
    key_text(&doc, "wx_app_id", "app");
    key_text(&doc, "wx_model_id", "model");
    key_text(&doc, "wx_session_token", "server-test-token");
    key_text(&doc, "wx_room_id", "fixture-room");
    key_text(&doc, "wx_payload", "a\"b");
    key_int(&doc, "hangup_reason", 7);
    assert(xiaotai_json_get(&doc, 0, "wx_server_token") < 0);
    memset(&room, 0, sizeof(room)); /* The transport has its own copy. */
    response(XIAOTAI_OK);
    wx_refuse(&room, 5, false);
    room = incoming;
    room.server_token[0] = room.payload[0] = '\0';
    wx_refuse(&room, 5, false);
    if (wx_config()->require_reject_server_token) {
        assert(refusals == 1 && !refusal_reply);
    } else {
        assert(refusals == 2 && refusal_reply);
        assert(parse_emitted(refusal_body, strlen(refusal_body), &doc) == XIAOTAI_OK);
        key_text(&doc, "wx_session_token", "");
        key_text(&doc, "wx_payload", "");
        response(XIAOTAI_OK);
    }
    const unsigned submitted = refusals;
    static const char malformed[] =
        "{\"payload\":{\"wx_room_id\":\"room\",\"peer_id\":\"peer\",\"token\":\"token\","
        "\"wx_app_id\":\"app\",\"wx_model_id\":\"model\",\"wx_server_token\":123}}";
    assert(wx_signal_room(malformed, sizeof(malformed) - 1U, &room) && room.invalid_refusal_fields);
    wx_refuse(&room, 8, false);
    char token[sizeof(room.server_token) + 1U], message[1024];
    memset(token, 'a', sizeof(token) - 1U);
    token[sizeof(token) - 1U] = '\0';
    const int len = snprintf(message, sizeof(message),
        "{\"payload\":{\"wx_room_id\":\"room\",\"peer_id\":\"peer\",\"token\":\"token\","
        "\"wx_app_id\":\"app\",\"wx_model_id\":\"model\",\"wx_server_token\":\"%s\"}}", token);
    assert(len > 0 && (size_t)len < sizeof(message));
    assert(wx_signal_room(message, (size_t)len, &room) && room.invalid_refusal_fields);
    wx_refuse(&room, 8, false);
    wx_refuse(&incoming, XIAOTAI_WECHAT_REASON_NONE, false);
    check_invalid(wx_config()->require_reject_server_token ? 4 : 3);
    assert(refusals == submitted && !refusal_reply && verified_wipes == submitted);
}

static void refusal_body_limit(void *ctx)
{
    (void)ctx;
    wx_room_t room = incoming;
    room.payload[0] = '\0';
    wx_refuse(&room, 7, false);
    assert(refusals == 1 && refusal_reply);
    const size_t base = strlen(refusal_body);
    response(XIAOTAI_OK);
    const size_t span = 2046U - base;
    size_t used_payload = span / 6U;
    memset(room.payload, 1, used_payload); /* each control is encoded as six bytes */
    memset(room.payload + used_payload, 'x', span % 6U);
    used_payload += span % 6U;
    room.payload[used_payload] = '\0';
    wx_refuse(&room, 7, false);
    assert(refusals == 2 && strlen(refusal_body) == 2046U);
    response(XIAOTAI_OK);
    room.payload[used_payload++] = 'x';
    room.payload[used_payload] = '\0';
    wx_refuse(&room, 7, false);
    if (wx_config()->reject_body_bytes) {
        assert(refusals == 2 && !refusal_reply);
        check_invalid(1);
    } else {
        assert(refusals == 3 && strlen(refusal_body) == 2047U);
        response(XIAOTAI_OK);
    }
}

static uint32_t busy;
static void retry_initial(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t activity = {.name = "recognition", .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    assert(xiaotai_activity_begin(&activity, &busy) == XIAOTAI_OK);
    background_policy = true;
    wx_refuse(&incoming, 5, false);
    assert(refusals == 1 && refusal_reply); /* immediate refusal is not background */
    response(XIAOTAI_FAIL);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
}

static void retry_offline(void *ctx)
{
    (void)ctx;
    platform.phase = XIAOTAI_PLATFORM_CONNECTING;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, NULL, NULL, 0) == XIAOTAI_OK);
}
static void retry_online(void *ctx)
{
    (void)ctx;
    platform.phase = XIAOTAI_PLATFORM_ONLINE;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
}
static void retry_idle(void *ctx) { (void)ctx; xiaotai_activity_end(busy); }
static void retry_reply(void *ctx) { (void)ctx; response(XIAOTAI_FAIL); }
static void retry_case(void)
{
    run(retry_initial);
    assert(refusals == 1 && status().refusals_failed == 1);
    run(retry_offline);
    run(retry_online);
    assert(refusals == 1);
    run(retry_idle);
    assert(refusals == 2 && status().refused == 1);
    run(retry_reply);
    run(retry_offline);
    run(retry_online);
    assert(refusals == 2 && status().refusals_failed == 2 && verified_wipes == 1);
}

static void initial_submit_failure(void *ctx)
{
    (void)ctx;
    fail_refusal_submit = true;
    wx_refuse(&incoming, 5, false);
    assert(refusals == 1 && !refusal_reply && status().refusals_failed == 1 && !verified_wipes);
    fail_refusal_submit = false;
}
static void invalidated_reply(void *ctx)
{
    (void)ctx;
    ++identity_generation;
    response(XIAOTAI_FAIL);
}
static void retry_binding(void)
{
    run(initial_submit_failure);
    run(retry_offline);
    run(retry_online);
    assert(refusals == 2 && refusal_reply);
    run(invalidated_reply);
    run(retry_offline);
    run(retry_online);
    assert(refusals == 2 && status().refusals_failed == 1 && verified_wipes == 1);
}

static void requirement_case(void *ctx)
{
    (void)ctx;
    assert(requirement_count == 2);
    for (size_t i = 0; i < 2; ++i) {
        const xiaotai_media_requirement_t *r = &requirements[i];
        assert(r->packet_ms == 20 && !r->downlink_burst_ms && !r->preroll_ms);
        assert(r->uplink.encoding == (i ? XIAOTAI_CODEC_OPUS : XIAOTAI_CODEC_G711A));
        assert(r->uplink.rate_hz == (i ? 16000U : 8000U) && r->uplink.channels == 1U &&
               r->uplink.packet_ms == 20 && r->downlink.encoding == r->uplink.encoding &&
               r->downlink.rate_hz == r->uplink.rate_hz && r->downlink.channels == 1U &&
               r->downlink.packet_ms == 20);
        assert(r->max_packet_bytes == (i ? 512U : 0U) && r->max_bitrate_bps == (i ? 64000U : 0U));
        assert(r->latency.start_ms == 20 && r->latency.max_ms == (wx_config()->uplink_noise_gate ? 140 : 100));
        assert(r->latency.slow_permille == 30);
    }
    active();
    assert((media.flags & XIAOTAI_MEDIA_GATE_IDLE_NOISE) == (wx_config()->uplink_noise_gate ?
                                                           XIAOTAI_MEDIA_GATE_IDLE_NOISE : 0U));
    assert(media.latency.max_ms == requirements[0].latency.max_ms);
}

static void read_scene(char *body, size_t capacity, bool video)
{
    const int len = scene_writer(body, capacity, NULL);
    xiaotai_json_doc_t doc;
    assert(len > 0 && (size_t)len <= capacity && parse_emitted(body, (size_t)len, &doc) == XIAOTAI_OK);
    const bool voice_geometry = wx_config()->voice_profile_geometry;
    const unsigned geometry_members = video ? 3U + (unsigned)extra_scene : voice_geometry ? 7U : 0U;
    assert(doc.tokens[0].children == 7U + (panel.desc.width ? 2U : 0U) + geometry_members);
    if (panel.desc.width) {
        key_int(&doc, "screen_width", panel.desc.width);
        key_int(&doc, "screen_height", panel.desc.height);
    } else {
        assert(xiaotai_json_get(&doc, 0, "screen_width") < 0);
        assert(xiaotai_json_get(&doc, 0, "screen_height") < 0);
    }
    key_int(&doc, "audio_rate", encoded_capture ? 16000 : 8000);
    key_int(&doc, "audio_channels", 1);
    key_int(&doc, "calling_timeout_sec", 30);
    key_text(&doc, "down_audio_mt", encoded_capture ? "opus" : "alaw");
    key_text(&doc, "up_video_mt", video ? "h264" : "none");
    key_text(&doc, "down_video_mt", video ? "jpeg" : "none");
    if (video) {
        key_int(&doc, "camera_rotation", wt99_panel ? 0 : 270);
        key_text(&doc, "video_res_mode", wt99_panel ? "fit_screen" : "auto");
        if (wt99_panel) {
            size_t length;
            const char *ratio = xiaotai_json_raw(&doc, xiaotai_json_get(&doc, 0, "aspect_ratio"), &length);
            assert(ratio && length == strlen("1.7066666667") && !memcmp(ratio, "1.7066666667", length));
        } else {
            key_text(&doc, "aspect_ratio", "3:4");
        }
    } else if (voice_geometry) {
        key_int(&doc, "camera_rotation", 0);
        key_int(&doc, "down_video_rotation", 0);
        key_text(&doc, "aspect_ratio", "4:3");
        key_text(&doc, "object_fit", "contain");
        key_text(&doc, "video_res_mode", "auto");
        bool mirror;
        assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "hor_mirror"), &mirror) == XIAOTAI_OK && !mirror);
        assert(xiaotai_json_bool(&doc, xiaotai_json_get(&doc, 0, "vert_mirror"), &mirror) == XIAOTAI_OK && !mirror);
    } else {
        assert(xiaotai_json_get(&doc, 0, "camera_rotation") < 0);
        assert(xiaotai_json_get(&doc, 0, "aspect_ratio") < 0);
        assert(xiaotai_json_get(&doc, 0, "extra") < 0);
    }
    const int no_video = xiaotai_json_get(&doc, 0, "no_video");
    assert(no_video >= 0 && doc.tokens[no_video].type == (video ? XIAOTAI_JSON_FALSE : XIAOTAI_JSON_TRUE));
}

static void scene_case(void *ctx)
{
    (void)ctx;
    char body[512];
    read_scene(body, sizeof(body), has_video);
    if (!has_video && wx_config()->voice_profile_geometry) {
        const int exact = scene_writer(body, sizeof(body), NULL);
        assert(exact > 0);
        read_scene(body, (size_t)exact, false);
        assert(scene_writer(body, (size_t)exact - 1U, NULL) < 0);
    }
    if (extra_scene) {
        read_scene(body, 245, false);
        assert(wx_profile_video_fallbacks() == 1);
    }
    assert(scene_writer(body, 18, NULL) < 0);
}

static void video_case(void *ctx)
{
    (void)ctx;
    wx_room_t video = incoming;
    video.type = WX_TYPE_VIDEO;
    wx_announce(&video);
    assert(status().state == XIAOTAI_WECHAT_INCOMING && !refusals);
    assert(((status().flags & XIAOTAI_WECHAT_VIDEO) != 0) == has_video);
    assert(((status().flags & XIAOTAI_WECHAT_VIDEO_OFFERED) != 0) == (!has_video && !wx_config()->ignore_room_type));
    assert(xiaotai_wechat_accept(0) == XIAOTAI_OK);
    assert(link_request.profile.video.send_stream == (has_video ? video_first.send_stream : 0));
    assert(xiaotai_wechat_request_video(link) == XIAOTAI_ERR_INVALID_STATE);
    mark_open(NULL);
    deliver(0x2000, NULL);
    assert(xiaotai_wechat_request_video(link + 1U) == XIAOTAI_ERR_INVALID_STATE);
    if (has_video) {
        assert(xiaotai_wechat_request_video(link) == XIAOTAI_OK);
        assert(command_envelope == 0x00011105U && command_size == 1 && command_body[0] == 1);
        assert(xiaotai_wechat_request_video(link) == XIAOTAI_OK);
        assert(command_envelope == 0x00021105U && command_size == 1 && command_body[0] == 1);
    } else {
        assert(xiaotai_wechat_request_video(link) == XIAOTAI_ERR_INVALID_STATE);
    }
}

static void video_outgoing_start(void *ctx)
{
    (void)ctx;
    assert(has_video && xiaotai_wechat_dial("fake-openid", XIAOTAI_WECHAT_VIDEO, NULL) == XIAOTAI_OK);
    xiaotai_json_doc_t doc;
    assert(parse_emitted(request_body, strlen(request_body), &doc) == XIAOTAI_OK);
    key_text(&doc, "wx_room_type", "video");
    http("{\"code\":0,\"data\":{\"call_id\":\"exact-id\"}}", 200, 0);
    room("fixture-room", "exact-id", "fake-device", "", "");
    assert(!link);
}
static void video_outgoing_ready(void *ctx)
{
    (void)ctx;
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && status().outgoing && link);
    const xiaotai_link_media_t *video = &link_request.profile.video;
    assert(video->send_stream == video_first.send_stream && video->receive_stream == video_first.receive_stream &&
           video->receive_request == video_first.receive_request);
}

static char at_reply[240];
static void at_reply_write(void *ctx, const char *line)
{
    assert(!ctx && strlen(line) < sizeof(at_reply));
    strcpy(at_reply, line);
}
static xiaotai_err_t at_command(const char *line)
{
    at_reply[0] = 0;
    const xiaotai_reply_t reply = {.write = at_reply_write};
    return xiaotai_command_run(line, &reply, 1000);
}
static char wire_out[4096];
static const char *wire_in;
static size_t wire_used;
static size_t read_wire(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    assert(!ctx);
    size_t size = wire_in ? strlen(wire_in) : 0;
    if (!size) {
        atomic_fetch_add(&clock_us, (int64_t)(timeout_ms ? timeout_ms : 1) * 1000);
        return 0;
    }
    if (size > capacity) size = capacity;
    memcpy(data, wire_in, size);
    wire_in += size;
    return size;
}
static xiaotai_err_t write_wire(void *ctx, const char *data, size_t size)
{
    assert(!ctx && size + wire_used < sizeof(wire_out));
    memcpy(wire_out + wire_used, data, size);
    wire_used += size;
    wire_out[wire_used] = 0;
    return XIAOTAI_OK;
}
static const xiaotai_at_io_t console_source = {.name = "wechat_fixture", .read = read_wire, .write = write_wire};
static void serial_command(const char *line)
{
    wire_in = line;
    wire_used = 0;
    assert(xiaotai_at_service(25) == XIAOTAI_OK);
}

static void at_case(const char *manifest)
{
    FILE *const input = fopen(manifest, "rb");
    assert(input);
    char raw[2048];
    const size_t bytes = fread(raw, 1, sizeof(raw), input);
    assert(feof(input) && !fclose(input));
    xiaotai_json_token_t tokens[160];
    xiaotai_json_doc_t doc;
    assert(xiaotai_json_parse(raw, bytes, tokens, sizeof(tokens) / sizeof(*tokens), NULL, &doc) == XIAOTAI_OK);
    serial_command("AT+HELP\r\n");
    const int entries = xiaotai_json_get(&doc, 0, "commands");
    size_t expected = 0, emitted = 0;
    for (const char *p = wire_out; (p = strstr(p, "+HELP: AT+CALL")); p += 14) ++emitted;
    for (int entry; (entry = xiaotai_json_at(&doc, entries, expected)) >= 0; ++expected) {
        char word[24] = {0}, usage[32] = {0}, expected_line[96];
        assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, entry, "command"), "AT+CALL"));
        const int verb = xiaotai_json_get(&doc, entry, "verb");
        const int synopsis = xiaotai_json_get(&doc, entry, "usage");
        if (verb >= 0) assert(xiaotai_json_string(&doc, verb, word, sizeof(word), NULL) == XIAOTAI_OK);
        if (synopsis >= 0) assert(xiaotai_json_string(&doc, synopsis, usage, sizeof(usage), NULL) == XIAOTAI_OK);
        (void)snprintf(expected_line, sizeof(expected_line), "+HELP: AT+CALL%s%s%s%s\r\n",
                       *word ? " " : "", word, *usage ? " " : "", usage);
        assert(strstr(wire_out, expected_line));
    }
    assert(expected == 7 && emitted == expected && strstr(wire_out, "OK\r\n"));
    serial_command("AT+CALL ACCEPT\r\n");
    assert(strstr(wire_out, "ERROR: INVALID_STATE NO_OWNER\r\n"));

    assert(at_command("AT+CALL") == XIAOTAI_OK && strstr(at_reply, "+CALL: kind=wechat state=idle gen=0"));
    assert(at_command("AT+CALL ACCEPT") == XIAOTAI_ERR_INVALID_STATE);
    assert(at_command("AT+CALL INDEX 256") == XIAOTAI_ERR_INVALID_ARG);
    assert(at_command("AT+CALL INDEX 3") == XIAOTAI_ERR_INVALID_ARG);
    assert(at_command("AT+CALL INDEX 1") == XIAOTAI_ERR_INVALID_STATE);
    assert(at_command("AT+CALL MUTE 00") == XIAOTAI_ERR_INVALID_ARG);
    assert(at_command("AT+CALL WX") == XIAOTAI_ERR_INVALID_ARG);
    run(start_timeout);
    assert(at_command("AT+CALL ACCEPT") == XIAOTAI_OK && status().state == XIAOTAI_WECHAT_CONNECTING);
    assert(at_command("AT+CALL MUTE 1") == XIAOTAI_OK && status().muted);
    assert(at_command("AT+CALL HANGUP") == XIAOTAI_OK);
    if (refusal_reply) run(retry_reply);
}

static void owner_hangup_incoming(void *unused)
{
    (void)unused;
    const xiaotai_wechat_status_t ringing = status();
    assert(ringing.state == XIAOTAI_WECHAT_INCOMING);
    assert(xiaotai_wechat_hangup(ringing.session) == XIAOTAI_OK);
}

static void finish_at_refusal(void *unused) { (void)unused; response(XIAOTAI_OK); }

static void at_hangup_case(bool reject_incoming)
{
    serial_command("AT\r\n");
    run(start_timeout);
    const uint32_t ringing = status().session;
    assert(ringing && status().state == XIAOTAI_WECHAT_INCOMING);
    serial_command("AT+CALL HANGUP\r\n");
    assert(!strcmp(wire_out, reject_incoming ? "ERROR: INVALID_STATE\r\n" : "OK\r\n"));
    if (reject_incoming) {
        assert(status().session == ringing && status().state == XIAOTAI_WECHAT_INCOMING);
        assert(!refusals && !refusal_reply && !requests && !link && !command_count);
        run(owner_hangup_incoming);
    }
    assert(status().state == XIAOTAI_WECHAT_IDLE && refusals == 1U);
    run(finish_at_refusal);
    run(start_timeout);
    assert(at_command("AT+CALL REJECT") == XIAOTAI_OK && refusals == 2U);
    run(finish_at_refusal);
    run(start_connect);
    assert(status().state == XIAOTAI_WECHAT_CONNECTING);
    assert(at_command("AT+CALL HANGUP") == XIAOTAI_OK && status().state == XIAOTAI_WECHAT_IDLE);
    if (refusal_reply) run(finish_at_refusal);
    run(start_request);
    assert(status().state == XIAOTAI_WECHAT_OUTGOING);
    assert(at_command("AT+CALL HANGUP") == XIAOTAI_OK && status().state == XIAOTAI_WECHAT_IDLE);
    run(make_active);
    const unsigned sent = command_count;
    assert(at_command("AT+CALL HANGUP") == XIAOTAI_OK && status().state == XIAOTAI_WECHAT_IDLE);
    assert(command_count == sent + 1U && command_word == 0x2001);
}

#define N XIAOTAI_WECHAT_REASON_NONE
#define B XIAOTAI_WECHAT_REASON_BARE
static const xiaotai_wechat_reasons_t profiles[] = {
    {0, 0, N, 7, 7, N, N, 8, 5, 7, N, N, N, 7, N},
    {B, B, N, 7, 7, N, N, 8, 8, 7, N, N, N, 7, N},
    {0, 0, N, 7, 7, N, N, 8, 8, 7, N, N, N, 7, N},
    {0, 0, 0, 7, 7, 1, 1, 5, 5, 5, 5, 5, 5, 7, 5},
    {1, 6, N, 7, 6, 1, 8, 5, 5, 5, 7, N, 5, 5, 5},
};
#undef N
#undef B
static unsigned reason_profile;
static void reason_policy(void *ctx)
{
    (void)ctx;
    const xiaotai_wechat_reasons_t *r = &wx_config()->reasons;
    assert(!memcmp(r, &profiles[reason_profile], sizeof(*r)));
}

static void reason_reply(unsigned before, uint8_t expected)
{
    if (expected == XIAOTAI_WECHAT_REASON_NONE) {
        assert(refusals == before && !refusal_reply);
        return;
    }
    xiaotai_json_doc_t doc;
    assert(refusals == before + 1 && parse_emitted(refusal_body, strlen(refusal_body), &doc) == XIAOTAI_OK);
    key_int(&doc, "hangup_reason", expected);
    response(XIAOTAI_OK);
}

static const struct { const char *fields; wx_type_t type; } signal_media_cases[] = {
    {",\"wx_room_type\":null", WX_TYPE_UNSET},
    {"", WX_TYPE_UNSET},
    {",\"wx_room_type\":\"\"", WX_TYPE_UNSET},
    {",\"media_type\":\"video\",\"wxa_room_type\":\"video\",\"wx_room_type\":\"voice\"", WX_TYPE_VOICE},
    {",\"wxa_room_type\":\"video\"", WX_TYPE_VIDEO},
    {",\"room_type\":\"video\"", WX_TYPE_VIDEO},
    {",\"media_type\":\"voice\"", WX_TYPE_VOICE},
    {",\"room_type\":\"video\",\"wxa_room_type\":\"audio\"", WX_TYPE_VOICE},
    {",\"media_type\":\"video\",\"room_type\":\"voice\"", WX_TYPE_VOICE},
    {",\"wx_room_type\":\"\",\"wxa_room_type\":null,\"room_type\":\"\",\"media_type\":\"audio\"", WX_TYPE_VOICE},
    {",\"wx_room_type\":null,\"wx_room_type\":\"video\",\"wxa_room_type\":\"voice\"", WX_TYPE_VOICE},
    {",\"wx_room_type\":\"invalid\",\"media_type\":\"voice\"", WX_TYPE_OTHER},
    {",\"wxa_room_type\":5,\"room_type\":\"voice\"", WX_TYPE_OTHER},
};
static size_t signal_media_index;

static void signal_media_case(void *ctx)
{
    (void)ctx;
    char id[32], payload[512];
    (void)snprintf(id, sizeof(id), "media-%zu", signal_media_index);
    const char *fields = signal_media_cases[signal_media_index].fields;
    const wx_type_t type = signal_media_cases[signal_media_index].type;
    const int n = snprintf(payload, sizeof(payload),
        "{\"wx_room_id\":\"%s\",\"peer_id\":\"whip-peer\",\"token\":\"whip-test-token\","
        "\"wx_app_id\":\"app\",\"wx_model_id\":\"model\",\"wx_server_token\":\"server-token\","
        "\"wx_payload\":\"payload\"%s}", id, fields);
    assert(n > 0 && (size_t)n < sizeof(payload));
    const unsigned before = refusals;
    const bool ignore = wx_config()->ignore_room_type;
    const bool unsupported = !ignore && type == WX_TYPE_OTHER;
    const bool video = has_video && (ignore || type == WX_TYPE_VIDEO || type == WX_TYPE_UNSET);
    push(0, "wx", identity_generation, payload);
    assert(!link && !media_gen);
    if (!!status().session == unsupported) fprintf(stderr, "Unexpected admission for legacy media fields: %s\n", fields);
    assert(!!status().session == !unsupported);
    reason_reply(before, unsupported ? 7 : XIAOTAI_WECHAT_REASON_NONE);
    if (!unsupported) {
        assert(status().state == XIAOTAI_WECHAT_INCOMING && wx_same_room(id));
        assert(!!(status().flags & XIAOTAI_WECHAT_VIDEO) == video);
        cancel_room(1, id);
        assert(!status().session && refusals == before);
    }
}

static void run_signal_media_cases(void)
{
    for (signal_media_index = 0; signal_media_index < sizeof(signal_media_cases) / sizeof(signal_media_cases[0]);
         ++signal_media_index) run(signal_media_case);
}

static void mz28_protocol_early(void *ctx)
{
    (void)ctx;
    room("self-idle", "call-1", "fake-device", "fake-openid", "app");
    reason_reply(0, 5);
    assert(!status().session && !link);
    room("remote-idle", "call-1", "phone", "fake-openid", "app");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && !link);
    cancel_room(2, "remote-idle");

    dial_once();
    const uint32_t outgoing = status().session;
    room("wrong-openid", "call-2", "fake-device", "other", "app");
    reason_reply(1, 5);
    room("wrong-app", "call-2", "fake-device", "fake-openid", "other");
    reason_reply(2, 5);
    room("wrong-origin", "call-2", "phone", "fake-openid", "app");
    reason_reply(3, 5);
    room("no-call", "", "", "fake-openid", "app");
    reason_reply(4, 5);
    room("early", "call-2", "fake-device", "fake-openid", "app");
    assert(status().session == outgoing && status().state == XIAOTAI_WECHAT_OUTGOING);
    assert(!link && refusals == 5);
    http("{\"code\":0,\"data\":{\"call_id\":\"call-2\"}}", 200, 0);
}

static void mz28_protocol_late(void *ctx)
{
    (void)ctx;
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && link);
    cancel_room(2, "early");
    assert(!status().session && !link);

    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":\"call-3\"}}", 200, 0);
}

static void mz28_protocol_matching(void *ctx)
{
    (void)ctx;
    room("wrong-call", "call-4", "fake-device", "fake-openid", "app");
    reason_reply(5, 5);
    assert(status().state == XIAOTAI_WECHAT_OUTGOING && !link);
    room("anonymous-outgoing", "call-3", "", "fake-openid", "app");
}

static void mz28_protocol_hangup(void *ctx)
{
    assert(status().state == XIAOTAI_WECHAT_CONNECTING && link);
    cancel_room(2, "anonymous-outgoing");
    room("anonymous-idle", "", "", "fake-openid", "app");
    assert(status().state == XIAOTAI_WECHAT_INCOMING && !link);
    cancel_room(2, "anonymous-idle");

    used = 0;
    trace[0] = '\0';
    answer_case(ctx); /* Covers every malformed command in the old parser fixture. */
    for (int32_t i = 0; i <= 1; ++i) {
        active();
        deliver(0x2001, i == 0 ? "{\"reason\":0}" : "{\"reason\":1}");
        assert(status().end == XIAOTAI_WECHAT_END_PEER_HANGUP && status().peer_reason == i);
        assert(!link && !media_gen);
    }
}

static unsigned before_refusal;
static void reason_ringing(void *ctx)
{
    (void)ctx;
    const xiaotai_wechat_reasons_t *r = &profiles[reason_profile];
    wx_announce(&incoming);
    assert(status().state == XIAOTAI_WECHAT_INCOMING);
    wx_room_t second = incoming;
    strcpy(second.room_id, "another-room");
    const unsigned before = refusals;
    wx_announce(&second);
    reason_reply(before, r->busy);
    const unsigned decline = refusals;
    assert(xiaotai_wechat_reject(0) == XIAOTAI_OK);
    reason_reply(decline, r->declined);
    before_refusal = refusals;
    wx_announce(&incoming);
}
static void reason_missed(void *ctx)
{
    (void)ctx;
    assert(!link && status().end == XIAOTAI_WECHAT_END_MISSED);
    reason_reply(before_refusal, profiles[reason_profile].missed);
}
static void reason_media_room(void *ctx)
{
    (void)ctx;
    const xiaotai_wechat_reasons_t *r = &profiles[reason_profile];
    platform.profile_ready = false;
    const unsigned withheld = refusals;
    wx_announce(&incoming);
    reason_reply(withheld, r->not_ready);
    assert(!status().session);
    platform.profile_ready = true;
    wx_room_t unknown = incoming;
    unknown.type = WX_TYPE_OTHER;
    const unsigned unsupported = refusals;
    wx_announce(&unknown);
    reason_reply(unsupported, wx_config()->ignore_room_type ? XIAOTAI_WECHAT_REASON_NONE : r->unsupported);
    assert((status().session != 0) == wx_config()->ignore_room_type);
    if (status().session) wx_cancel(unknown.room_id, false);
}
static void reason_routed(void *ctx)
{
    (void)ctx;
    const xiaotai_wechat_reasons_t *r = &profiles[reason_profile];
    room("retired-room", "retired-call", "remote-initiator", "", "app");
    assert(status().state == XIAOTAI_WECHAT_INCOMING);
    cancel_room(2, "retired-room");
    unsigned before = refusals;
    room("fresh-room", "retired-call", "remote-initiator", "", "app");
    reason_reply(before, r->retired_call);
    before = refusals;
    room("retired-room", "fresh-call", "remote-initiator", "", "app");
    reason_reply(before, r->retired_room);
    assert(!status().session);

    dial_once();
    before = refusals;
    wx_announce(&incoming);
    reason_reply(before, r->busy_dialing);
    before = refusals;
    room("wrong-room", "wrong-call", "fake-device", "wrong-user", "app");
    reason_reply(before, r->stale);
    before = refusals;
    room("early-one", "early-a", "fake-device", "fake-openid", "app");
    room("early-two", "early-b", "fake-device", "fake-openid", "app");
    assert(refusals == before);
    room("early-overflow", "early-c", "fake-device", "fake-openid", "app");
    reason_reply(before, r->overflow);
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
}
static void reason_abandon(void *ctx)
{
    (void)ctx;
    dial_once();
    http("{\"code\":0,\"data\":{\"call_id\":\"abandon-id\"}}", 200, 0);
    room("abandon-room", "abandon-id", "fake-device", "fake-openid", "app");
    /* Hang up before the queued link admission runs. */
    const unsigned before = refusals;
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    reason_reply(before, profiles[reason_profile].abandoned);
}
static void reason_failed_call(void *ctx)
{
    (void)ctx;
    dial_once();
    fail_connect = XIAOTAI_FAIL;
    http("{\"code\":0,\"data\":{\"call_id\":\"failure-id\"}}", 200, 0);
    before_refusal = refusals;
    room("failure-room", "failure-id", "fake-device", "fake-openid", "app");
}
static void reason_failed_result(void *ctx)
{
    (void)ctx;
    assert(!link && !status().session);
    reason_reply(before_refusal, profiles[reason_profile].failed);
    fail_connect = XIAOTAI_OK;
}

static unsigned commands_before;
static void hangup_wire(uint8_t expected)
{
    if (expected == XIAOTAI_WECHAT_REASON_NONE) {
        assert(command_count == commands_before && !status().releasing && !link);
    } else {
        assert(command_count == commands_before + 1 && command_envelope == 0x2001 && status().releasing && link);
        if (expected == XIAOTAI_WECHAT_REASON_BARE) {
            assert(command_size == 0);
        } else {
            xiaotai_json_doc_t doc;
            assert(command_size && parse_emitted(command_body, command_size, &doc) == XIAOTAI_OK);
            key_int(&doc, "reason", expected);
        }
    }
}
static void reason_local(void *ctx)
{
    (void)ctx;
    active();
    commands_before = command_count;
    assert(xiaotai_wechat_hangup(0) == XIAOTAI_OK);
    hangup_wire(profiles[reason_profile].hangup);
}
static void reason_connect(void *ctx)
{
    (void)ctx;
    accepting();
    mark_open(NULL);
    commands_before = command_count;
}
static void reason_deadline(void *ctx) { (void)ctx; hangup_wire(profiles[reason_profile].hangup_timeout); }
static void reason_failure(void *ctx)
{
    (void)ctx;
    active();
    commands_before = command_count;
    media.event(media_gen, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL, media.ctx);
    hangup_wire(profiles[reason_profile].hangup_failure);
}
static void reasons_case(void)
{
    run(reason_policy);
    run(reason_ringing);
    at(atomic_load(&clock_us) + (int64_t)wx_config()->ring_timeout_ms * 1000 + 1);
    run(reason_missed);
    run(reason_media_room);
    run(reason_routed);
    run(reason_abandon);
    run(reason_failed_call);
    run(reason_failed_result);
    run(reason_local);
    at(atomic_load(&clock_us) + (int64_t)XIAOTAI_WECHAT_RELEASE_DELAY_MS * 1000 + 1);
    run(reason_connect);
    at(atomic_load(&clock_us) + (int64_t)wx_config()->answer_timeout_ms * 1000 + 1);
    run(reason_deadline);
    at(atomic_load(&clock_us) + (int64_t)XIAOTAI_WECHAT_RELEASE_DELAY_MS * 1000 + 1);
    run(reason_failure);
}
#endif

int main(int argc, char **argv)
{
    const char *scenario = argc < 2 ? "incoming" : argv[1];
    bool strict = argc > 2 && strcmp(argv[2], "strict") == 0;
    const bool contacts_mz28 = strcmp(scenario, "contacts_mz28") == 0;
    const bool mz28_authorization = strcmp(scenario, "mz28-authorization") == 0;
    bool all_offline = argc > 2 && strcmp(argv[2], "all-offline") == 0;
    trust_http_unbound = argc > 2 && strcmp(argv[2], "trust-unbound") == 0;
    const char *const busy[] = {"recognition", NULL};
    xiaotai_wechat_config_t cfg = {
        .incoming_busy_during = busy, .end_on_platform_offline = all_offline,
        .require_app_id = strict || contacts_mz28 || mz28_authorization, .require_fresh_contacts = strict,
        .trust_http_unbound = trust_http_unbound,
        .refresh_denied_after_dismiss = strcmp(scenario, "xmini-authorization") == 0,
        .refuse_changed_contacts = contacts_mz28 || mz28_authorization ||
                                   (!strict && strcmp(scenario, "contacts") == 0),
    };
    if (all_offline || !strcmp(scenario, "finish_opening")) {
        cfg.custom_reasons = true;
        cfg.reasons = (xiaotai_wechat_reasons_t)XIAOTAI_WECHAT_REASONS_DEFAULT;
        cfg.reasons.abandoned = 1;
    }
    if (strict) {
        cfg.ring_timeout_ms = 30000; cfg.request_timeout_ms = 40000;
        cfg.custom_uplink_stream = true; cfg.uplink_policy = XIAOTAI_LINK_PEER_STREAM_IGNORE;
        cfg.custom_downlink_stream = true;
        cfg.downlink_stream = 0;
    } else if (mz28_authorization || (argc > 2 && strcmp(argv[2], "short") == 0)) {
        cfg.ring_timeout_ms = 30000; cfg.request_timeout_ms = 10000; cfg.answer_timeout_ms = 30000;
    }
#ifdef XIAOTAI_WECHAT_REAL_ROUTE
    external = argc > 2 && strcmp(argv[2], "external") == 0;
    const bool use_busy_reason_8 = argc > 2 && strcmp(argv[2], "legacy-incoming-busy-8") == 0;
    legacy_incoming_policy = use_busy_reason_8 || (argc > 2 && strcmp(argv[2], "legacy-incoming") == 0);
    cfg.allow_reopen_incoming_room = legacy_incoming_policy;
    cfg.anonymous_waiting_is_busy = legacy_incoming_policy;
    cfg.custom_reasons = true;
    cfg.reasons = (xiaotai_wechat_reasons_t)XIAOTAI_WECHAT_REASONS_DEFAULT;
    cfg.reasons.overflow = 5;
    if (use_busy_reason_8) cfg.reasons.busy_dialing = 8;
#endif
#ifdef XIAOTAI_WECHAT_REAL_REFUSAL
    cfg.require_reject_server_token = strict;
    if (argc > 2 && !strcmp(argv[2], "limit")) {
        cfg.require_reject_server_token = true;
        cfg.reject_body_bytes = 2047;
        xiaotai_wechat_config_t invalid = cfg;
        invalid.reject_body_bytes = XIAOTAI_TRANSPORT_SERVICE_BODY_MAX + 1U;
        assert(xiaotai_wechat_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    }
    cfg.uplink_noise_gate = argc > 2 && strcmp(argv[2], "noise") == 0;
    cfg.at_reject_incoming_hangup = argc > 2 && !strcmp(argv[2], "reject-incoming-hangup");
    if (cfg.uplink_noise_gate) {
        cfg.latency = (xiaotai_media_latency_t)XIAOTAI_MEDIA_LATENCY_VOIP;
        cfg.latency.max_ms = 140;
    }
    cfg.ignore_room_type = argc > 2 && strcmp(argv[2], "ignore") == 0;
    if (!strcmp(scenario, "protocol-mz28")) {
        cfg.require_app_id = true;
        cfg.require_reject_server_token = true;
        cfg.reject_body_bytes = 2047;
        cfg.reasons = profiles[4];
    }
    encoded_capture = argc > 2 && strcmp(argv[2], "opus") == 0;
    if (encoded_capture) cfg.uplink_policy = XIAOTAI_LINK_PEER_STREAM_FOLLOW;
    wt99_panel = argc > 2 && !strcmp(argv[2], "wt99");
    const bool voice_geometry_video = argc > 2 && !strcmp(argv[2], "voice-geometry-video");
    cfg.voice_profile_geometry = voice_geometry_video || (argc > 2 && !strcmp(argv[2], "voice-geometry"));
    has_video = argc > 2 && (!strcmp(argv[2], "with-video") || !strcmp(argv[2], "partial") || wt99_panel || voice_geometry_video);
    extra_scene = argc > 2 && !strcmp(argv[2], "partial");
    if (!strcmp(scenario, "reasons")) {
        assert(argc == 3 && argv[2][0] >= '0' && argv[2][0] <= '4' && !argv[2][1]);
        reason_profile = (unsigned)(argv[2][0] - '0');
        cfg.ignore_room_type = reason_profile == 3;
        cfg.custom_reasons = true;
        cfg.reasons = profiles[reason_profile];
        xiaotai_wechat_config_t invalid = cfg;
        invalid.reasons.overflow = XIAOTAI_WECHAT_REASON_BARE;
        assert(xiaotai_wechat_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
        invalid = cfg;
        invalid.reasons.hangup_failure = 21;
        assert(xiaotai_wechat_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
        if (!reason_profile) cfg.custom_reasons = false; /* Verify the real default table. */
    }
    if (!encoded_capture) {
        panel.desc.width = wt99_panel ? 1024 : has_video ? 480 : strict ? 128 : 320;
        panel.desc.height = wt99_panel ? 600 : has_video ? 320 : strict ? 64 : 240;
    }
#endif
#ifdef XIAOTAI_WECHAT_REAL_ROUTE
    if (!strncmp(scenario, "legacy-hangup-", sizeof("legacy-hangup-") - 1U)) cfg.custom_reasons = false;
#endif
    if (!strcmp(scenario, "scene")) {
        const uint32_t bad[] = {16U, 255U, 256U, UINT32_MAX};
        for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
            xiaotai_wechat_config_t invalid = cfg;
            invalid.downlink_stream = bad[i];
            assert(xiaotai_wechat_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
        }
    }
    assert(xiaotai_wechat_configure(&cfg) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&sync) == XIAOTAI_OK);
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    static const xiaotai_module_t subject = {
        .name = "wechat", .init = xiaotai_module_wechat_init, .start = xiaotai_module_wechat_start,
    };
    static const xiaotai_module_t observer = {.name = "observer", .init = observers};
    static const xiaotai_module_t settings = {.name = "settings", .init = settings_init};
    static const xiaotai_module_t console = {
        .name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start,
    };
    const bool hangup_case = !strcmp(scenario, "at-hangup");
    const bool serial = !strcmp(scenario, "at") || hangup_case;
#ifdef XIAOTAI_WECHAT_REAL_REFUSAL
    if (serial) {
        const xiaotai_at_config_t config = {.no_platform_console = true, .task_stack_bytes = XIAOTAI_AT_NO_TASK};
        assert(xiaotai_at_configure(&config) == XIAOTAI_OK && xiaotai_at_attach(&console_source) == XIAOTAI_OK);
    }
#endif
    const xiaotai_module_t *const modules[] = {&settings, &subject, &observer, &console};
    static const xiaotai_priority_t priorities[] = {{"recognition", 2}, {"call", 3}, {"emergency", 4}, {NULL, 0}};
    const xiaotai_framework_config_t c = {
        .modules = modules, .module_count = serial ? 4U : 3U, .priorities = priorities, .restart = restart,
    };
    assert(xiaotai_framework_start(&c) == XIAOTAI_OK);
    assert(xiaotai_wechat_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
#ifdef XIAOTAI_WECHAT_REAL_ROUTE
    if (routing_tests(scenario)) { /* The shared fixture uses the real router in these tests. */ }
    else
#endif
    if (strcmp(scenario, "incoming") == 0) run(incoming_case);
    else if (!strcmp(scenario, "finish_opening")) {
        run(x22_finish);
        at(150000);
        assert(status().releasing && link);
        at(150001);
        assert(!status().releasing && !link && !refusals);
        run(x22_without_open);
        run(x22_send_error);
        run(x22_preempt);
    }
    else if (!strcmp(scenario, "answer")) run(answer_case);
    else if (!strcmp(scenario, "end")) {
        if (argc > 2 && strcmp(argv[2], "preempt") == 0) {
            run(make_active); run(emergency);
        } else {
            run(end_case); at(150000); run(after_pause); at(150001); run(verify_release);
            at(300001);
        }
    } else if (!strcmp(scenario, "platform")) {
        if (all_offline) {
            run(platform_active);
            assert(status().end == XIAOTAI_WECHAT_END_NETWORK && !link && command_count == 1);
            run(platform_room);
            assert(status().end == XIAOTAI_WECHAT_END_NETWORK && !link && command_count == 1);
            assert(refusals == 1 && refusal_reason == 1 && rejected_deferred);
        } else {
            run(platform_request);
            assert(status().end == XIAOTAI_WECHAT_END_NETWORK && !link);
            run(platform_active);
            assert(status().state == XIAOTAI_WECHAT_ACTIVE && link);
            run(binding_offline);
            assert(status().end == XIAOTAI_WECHAT_END_SYSTEM && !link && cleared);
        }
    } else if (!strcmp(scenario, "hangup_error")) run(failed_local_hangup);
    else if (!strcmp(scenario, "failures")) run(failures);
    else if (!strcmp(scenario, "quiesce")) run(quiesce_case);
    else if (!strcmp(scenario, "busy_during")) run(blocked);
    else if (!strcmp(scenario, "mute")) { run(mute_case); run(verify_mute); }
    else if (!strcmp(scenario, "contacts")) { run(contacts_case); run(contacts_recover); run(contacts_signal); }
    else if (contacts_mz28) { run(contacts_mz28_invalidate); run(contacts_mz28_refuses_stale); }
    else if (!strcmp(scenario, "eventlog")) run(log_case);
    else if (!strcmp(scenario, "timeouts")) {
        run(start_timeout);
        const int64_t ring = (int64_t)wx_config()->ring_timeout_ms * 1000;
        at(ring); run(check_ring); at(ring + 1); run(check_missed);
        run(start_connect); run(mark_open);
        const int64_t answer = ring + (int64_t)wx_config()->answer_timeout_ms * 1000;
        at(answer); run(check_waiting); at(answer + 1); run(check_answer_timeout);
        const int64_t release = answer + 1 + (int64_t)XIAOTAI_WECHAT_RELEASE_DELAY_MS * 1000;
        at(release); run(start_request);
        const int64_t request = release + (int64_t)wx_config()->request_timeout_ms * 1000;
        at(request - 1); run(check_request); at(request); run(check_request_timeout);
    }
    if (!strcmp(scenario, "eventlog")) assert(ended_events == 2);
#ifdef XIAOTAI_WECHAT_REAL_REFUSAL
    if (!strcmp(scenario, "reject-fields")) run(refusal_admission);
    if (!strcmp(scenario, "legacy-hangup-reject")) {
        run(legacy_hangup_reject);
        assert(!command_count && !requests && refusals == 1U && ended_events == 1U);
    }
    if (!strcmp(scenario, "reject-body-limit")) run(refusal_body_limit);
    if (!strcmp(scenario, "reject-retry")) retry_case();
    if (!strcmp(scenario, "reject-binding")) retry_binding();
    if (!strcmp(scenario, "requirements")) run(requirement_case);
    if (!strcmp(scenario, "scene")) run(scene_case);
    if (!strcmp(scenario, "video")) run(video_case);
    if (!strcmp(scenario, "signal-media")) run_signal_media_cases();
    if (!strcmp(scenario, "video-outgoing")) { run(video_outgoing_start); run(video_outgoing_ready); }
    if (!strcmp(scenario, "at")) { assert(argc == 3); at_case(argv[2]); }
    if (hangup_case) at_hangup_case(cfg.at_reject_incoming_hangup);
    if (!strcmp(scenario, "reasons")) reasons_case();
    if (!strcmp(scenario, "protocol-mz28")) {
        run(mz28_protocol_early);
        run(mz28_protocol_late);
        run(mz28_protocol_matching);
        run(mz28_protocol_hangup);
    }
#endif
    run(reset);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    printf("mod_wechat %s: ok\n", scenario);
    return 0;
}
