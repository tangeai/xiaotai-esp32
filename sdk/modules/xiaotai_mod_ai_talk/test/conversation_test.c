// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_xmini_ai_token_http.py
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/regression/test_voice_reliability.py::test_preroll_handoff
// Replaces legacy: tests/test_at_diagnostics.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_stop_and_diagnostics
// Replaces legacy: tests/regression/test_ai_token_response.py
// Replaces legacy: tests/regression/test_contact_query.py
// Replaces legacy: tests/regression/test_ai_start_contract.py
// Replaces legacy: tests/regression/test_ai_audio_drain.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_command_recovery
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ai_talk_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/link.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai/view.h"
#include "xiaotai_mem_host.h"

static struct {
    bool muted, format, online, profile;
    bool media_active, media_wrong_generation, reject_hold, allow_failed_suspension_release;
    xiaotai_platform_reply_fn_t reply;
    void *reply_context;
    xiaotai_err_t request_error, send_error, media_open_error;
    xiaotai_link_handlers_t handlers;
    xiaotai_link_request_t request;
    xiaotai_media_session_t media;
    xiaotai_media_requirement_t requirement;
    xiaotai_transport_requirement_t transport;
    uint32_t id, releases, last_token, commands;
    uint32_t held_signaling, signaling_attempts, signaling_releases;
    uint32_t watched_link;
    xiaotai_link_release_state_t release_state;
    xiaotai_platform_done_fn_t suspended;
    void *suspended_ctx;
    char sent[XIAOTAI_LINK_COMMAND_LIMIT_BYTES + 1U];
    char calls[256];
} s;

static _Atomic int64_t test_time_us;
int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);

int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t now = atomic_load(&test_time_us);
    return now ? now : __real_xiaotai_time_us();
}

static void record(char call)
{
    const size_t i = strlen(s.calls);
    assert(i < sizeof(s.calls) - 1U);
    s.calls[i] = call;
    s.calls[i + 1U] = 0;
}

xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *request,
                                      xiaotai_platform_reply_fn_t reply, void *context)
{
    assert(strcmp(request->service, "ai") == 0 && strcmp(request->path, "/v1/ai/token") == 0);
    assert(request->method == XIAOTAI_PLATFORM_GET && request->body == NULL);
    assert(request->auth == XIAOTAI_PLATFORM_AUTH_SESSION && request->timeout_ms == 15000);
    assert(request->flags == (XIAOTAI_PLATFORM_URGENT | XIAOTAI_PLATFORM_RELEASE));
    if (s.request_error != XIAOTAI_OK) return s.request_error;
    s.reply = reply;
    s.reply_context = context;
    record('T');
    return XIAOTAI_OK;
}

void xiaotai_platform_status(xiaotai_platform_status_t *out)
{
    *out = (xiaotai_platform_status_t){
        .phase = s.online ? XIAOTAI_PLATFORM_ONLINE : XIAOTAI_PLATFORM_STOPPED, .profile_ready = s.profile,
    };
}

xiaotai_err_t xiaotai_platform_signaling_hold_bounded(uint32_t max_ms, xiaotai_platform_done_fn_t suspended,
                                                     void *ctx, uint32_t *hold)
{
    xiaotai_ai_talk_status_t current;
    xiaotai_ai_talk_status(&current);
    assert(max_ms == 60000U && !s.held_signaling && *hold == 0 && suspended);
    assert(current.phase == XIAOTAI_AI_TALK_LISTENING && current.session == (uint32_t)(uintptr_t)ctx);
    assert(s.media_active && current.link == s.id && !s.media_wrong_generation);
    ++s.signaling_attempts;
    if (s.reject_hold) return XIAOTAI_ERR_BUSY;
    *hold = s.held_signaling = xiaotai_generation_next();
    s.suspended = suspended;
    s.suspended_ctx = ctx;
    return XIAOTAI_OK;
}

void xiaotai_platform_signaling_release(uint32_t hold)
{
    assert(hold && hold == s.held_signaling);
    xiaotai_ai_talk_status_t current;
    xiaotai_ai_talk_status(&current);
    assert(s.allow_failed_suspension_release || (!s.media_active && current.phase == XIAOTAI_AI_TALK_IDLE));
    s.held_signaling = 0;
    ++s.signaling_releases;
}

bool xiaotai_platform_signaling_bounded_hold_active(uint32_t hold, const void *ctx)
{
    return hold && hold == s.held_signaling && ctx == s.suspended_ctx;
}

void xiaotai_platform_signaling_release_bounded(uint32_t hold, const void *ctx)
{
    if (xiaotai_platform_signaling_bounded_hold_active(hold, ctx)) xiaotai_platform_signaling_release(hold);
}

xiaotai_err_t xiaotai_link_watch_release(uint32_t link)
{
    assert(link == s.id);
    if (s.watched_link) return XIAOTAI_ERR_BUSY;
    s.watched_link = link;
    s.release_state = XIAOTAI_LINK_RELEASE_PENDING;
    return XIAOTAI_OK;
}

xiaotai_link_release_state_t xiaotai_link_release_state(uint32_t link)
{
    return s.watched_link == link ? s.release_state : XIAOTAI_LINK_RELEASE_UNWATCHED;
}

void xiaotai_link_unwatch_release(uint32_t link)
{
    assert(link == s.watched_link);
    s.watched_link = 0;
}

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *req)
{
    assert(req->response_bytes == 2560 && req->concurrent_requests == 1);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_require_service(const char *service)
{
    assert(strcmp(service, "ai") == 0);
    return XIAOTAI_OK;
}

xiaotai_transport_state_t xiaotai_transport_state(void)
{
    return s.online ? XIAOTAI_TRANSPORT_ONLINE : XIAOTAI_TRANSPORT_DOWN;
}

xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *req)
{
    assert(req->links == 1);
    s.transport = *req;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_connect(const xiaotai_link_request_t *request, const xiaotai_link_handlers_t *handlers,
                                 uint32_t *generation)
{
    assert(request->kind == XIAOTAI_LINK_WHIP);
    assert(strcmp(request->peer, "test-peer") == 0 && strcmp(request->token, "test-token") == 0);
    assert(request->profile.audio.send && request->profile.audio.send_stream == 1);
    assert(request->profile.audio.send_format.codec == XIAOTAI_LINK_CODEC_G711A);
    assert(request->profile.audio.send_format.rate_hz == 8000 && request->profile.audio.send_format.channels == 1);
    assert(request->profile.audio.receive && request->profile.audio.receive_formats[0].codec == XIAOTAI_LINK_CODEC_G711A);
    s.handlers = *handlers;
    s.request = *request;
    s.request.peer = s.request.token = NULL;
    *generation = s.id = xiaotai_generation_next();
    record('L');
    return XIAOTAI_OK;
}

void xiaotai_link_disconnect(uint32_t generation)
{
    assert(generation == s.id);
    record('D');
}

xiaotai_err_t xiaotai_link_command(uint32_t generation, uint32_t word, const void *text, size_t length)
{
    assert(generation == s.id && word == 0x2100 && length < sizeof(s.sent));
    memcpy(s.sent, text, length);
    s.sent[length] = 0;
    ++s.commands;
    record('S');
    return s.send_error;
}

xiaotai_err_t xiaotai_link_receive(uint32_t generation, xiaotai_link_media_kind_t kind,
                                   xiaotai_link_receive_fn_t receiver, void *context)
{
    assert(generation == s.id && kind == XIAOTAI_LINK_AUDIO && receiver == NULL && context == NULL);
    record('R');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool allowed)
{
    assert(generation == s.id && kind == XIAOTAI_LINK_AUDIO && allowed);
    record('P');
    return XIAOTAI_OK;
}

const xiaotai_media_sink_t *xiaotai_link_audio_sink(void)
{
    static const xiaotai_media_sink_t sink = {0};
    return &sink;
}

bool xiaotai_media_can_send(const xiaotai_media_format_t *format) { return s.format && format->rate_hz == 8000; }
bool xiaotai_media_can_receive(const xiaotai_media_format_t *format) { return xiaotai_media_can_send(format); }

void xiaotai_media_status(xiaotai_media_status_t *out)
{
    *out = (xiaotai_media_status_t){.mic_muted = s.muted, .active = s.media_active,
        .generation = s.media_active ? s.id + (uint32_t)s.media_wrong_generation : 0U};
}

xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *req)
{
    assert(req->uplink.encoding == XIAOTAI_CODEC_G711A && req->downlink.encoding == XIAOTAI_CODEC_G711A);
    assert(req->downlink_burst_ms == 1000);
    s.requirement = *req;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *media)
{
    assert(media->generation == s.id && (media->flags & XIAOTAI_MEDIA_HOLD) != 0 && media->duplex);
    assert(media->uplink.rate_hz == 8000 && media->downlink.rate_hz == 8000 && media->downlink_burst_ms == 1000);
    assert(media->sink == xiaotai_link_audio_sink());
    if (s.media_open_error != XIAOTAI_OK) return s.media_open_error;
    s.media = *media;
    record('O');
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_activate(uint32_t generation)
{ assert(generation == s.id); s.media_active = true; record('A'); return XIAOTAI_OK; }
void xiaotai_media_close(uint32_t generation) { assert(generation == s.id); s.media_active = false; record('C'); }
xiaotai_err_t xiaotai_media_flush(uint32_t generation) { assert(generation == s.id); record('F'); return XIAOTAI_OK; }
static uint32_t tail_wait_ms;
static xiaotai_err_t finish_result;
xiaotai_err_t xiaotai_media_finish(uint32_t generation, const xiaotai_media_tail_t *tail)
{
    assert(generation == s.id && tail && tail->quiet_ms == 800 && tail->limit_ms == 8000);
    tail_wait_ms = tail->wait_ms;
    record('T');
    return finish_result;
}
xiaotai_err_t xiaotai_media_drain(uint32_t generation, uint32_t delay)
{
    assert(generation == s.id && delay == 7000);
    record('G');
    return XIAOTAI_OK;
}

void xiaotai_media_preroll_release(uint32_t token)
{
    assert(token != 0);
    s.last_token = token;
    ++s.releases;
    record('U');
}

xiaotai_err_t xiaotai_identity_device_id(char *out, size_t capacity)
{
    assert(capacity >= sizeof("test-device"));
    memcpy(out, "test-device", sizeof("test-device"));
    return XIAOTAI_OK;
}

void xiaotai_identity_wipe(void *data, size_t length) { memset(data, 0, length); }

static uint32_t generation;
static bool extend;
static struct {
    unsigned calls;
    uint32_t session, request;
    char args[96];
} tool;
static const xiaotai_link_media_t VIDEO = {
    .send = true, .send_stream = 3, .send_format = {.codec = XIAOTAI_LINK_CODEC_JPEG},
};

static void on_generation(uint32_t id, uint32_t link, void *ctx)
{
    (void)ctx;
    assert(id != 0);
    generation = link;
    record(link ? 'Y' : 'Z');
}

static void on_activity_end(uint32_t id, uint32_t link, void *ctx)
{
    (void)id;
    (void)link;
    (void)ctx;
    generation = 0;
}

static void capture_tool(const xiaotai_ai_talk_tool_call_t *call, void *ctx)
{
    assert(strcmp(call->name, ctx) == 0 && xiaotai_on_owner());
    assert(call->size == strlen(call->arguments) && call->size < sizeof(tool.args));
    ++tool.calls;
    tool.session = call->session;
    tool.request = call->request;
    memcpy(tool.args, call->arguments, call->size + 1U);
    if (!call->request) assert(xiaotai_ai_talk_tool_reply(call->session, 0, NULL) == XIAOTAI_OK);
}

static bool register_tools = true;
static bool register_compat;
static bool external_memory;
static bool scope_scenario;
static void scoped_tool(const xiaotai_ai_talk_tool_call_t *call, void *ctx);
bool __wrap_xiaotai_mem_has_external(void) { return external_memory; }

static void compat_fallback(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    assert(call->device_action && call->request && call->id_text_bytes == 4U);
    assert(!strcmp(call->name, "unknown") || !*call->name);
    xiaotai_ai_talk_tool_response_t response = {
        .object = "{\"status\":\"unsupported_action\",\"count\":\"0\"}",
        .code = -32601, .message = "未知 \"动作\"\\", .max_bytes = 8,
    };
    assert(xiaotai_ai_talk_tool_respond(call->session, call->request, &response) == XIAOTAI_ERR_INVALID_SIZE);
    response.max_bytes = 4095;
    assert(xiaotai_ai_talk_tool_respond(call->session, call->request, &response) == XIAOTAI_OK);
    assert(xiaotai_ai_talk_tool_respond(call->session, call->request, &response) == XIAOTAI_ERR_INVALID_STATE);
}

static xiaotai_err_t init_extension(void)
{
    static const xiaotai_link_media_t spare = {.send = true, .send_stream = 8};
    static const xiaotai_contribution_t items[] = {
        {.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = "ai", .data = &VIDEO},
        {.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = "ai", .data = &spare},
        {0},
    };
    const xiaotai_attachment_t attachment = {
        .session = on_generation, .end = on_activity_end, .contributions = extend ? items : NULL,
    };
    static char lookup[] = "lookup", notice[] = "notice";
    if (register_tools) {
        assert(xiaotai_ai_talk_tool_register(lookup, capture_tool, lookup) == XIAOTAI_OK);
        assert(xiaotai_ai_talk_tool_register(notice, capture_tool, notice) == XIAOTAI_OK);
        assert(xiaotai_ai_talk_tool_register(lookup, capture_tool, lookup) == XIAOTAI_ERR_INVALID_STATE);
    }
    if (scope_scenario) assert(xiaotai_ai_talk_tool_register("module_lookup", scoped_tool, NULL) == XIAOTAI_OK);
    if (register_compat) {
        assert(xiaotai_ai_talk_tool_fallback_register(compat_fallback, NULL) == XIAOTAI_OK);
        assert(xiaotai_ai_talk_tool_fallback_register(compat_fallback, NULL) == XIAOTAI_ERR_INVALID_STATE);
    }
    return xiaotai_activity_attach("ai", &attachment);
}

static const xiaotai_module_t talk = {
    .name = "ai_talk", .init = xiaotai_module_ai_talk_init, .start = xiaotai_module_ai_talk_start,
};
static const xiaotai_module_t extension = {.name = "extension", .init = init_extension};
static const xiaotai_module_t *const modules[] = {&talk, &extension};
static unsigned application_views, scope_tool_calls, deferred_tool_calls;

static void count_tool_view(const xiaotai_view_t *view, void *ctx)
{
    (void)view;
    (void)ctx;
    ++application_views;
}

static void deferred_tool_view(void *ctx)
{
    (void)ctx;
    ++deferred_tool_calls;
    const xiaotai_view_t view = {0};
    xiaotai_view_show(&view);
}

static void scoped_tool(const xiaotai_ai_talk_tool_call_t *call, void *ctx)
{
    (void)ctx;
    ++scope_tool_calls;
    const xiaotai_view_t view = {0};
    xiaotai_view_show(&view);
    assert(xiaotai_ai_talk_tool_reply(call->session, call->request, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(deferred_tool_view, NULL) == XIAOTAI_OK);
}

static xiaotai_err_t init_tool_app(void)
{
    xiaotai_view_set_sink(count_tool_view, NULL);
    return xiaotai_ai_talk_tool_register("lookup", scoped_tool, NULL);
}

static const xiaotai_application_t tool_app = {.name = "tool_app", .init = init_tool_app};
static const xiaotai_application_t other_app = {.name = "other_app"};
static const xiaotai_application_t *const scope_apps[] = {&tool_app, &other_app};
static const xiaotai_priority_t priorities[] = {{"ai", 2}, {"call", 3}, {"stream", 1}, {NULL, 0}};
static xiaotai_signal_t reported, finished;
static unsigned caption_events, media_events;
static uint32_t last_caption_revision;

static xiaotai_ai_talk_status_t status(void)
{
    xiaotai_ai_talk_status_t current;
    xiaotai_ai_talk_status(&current);
    return current;
}

static void report(void *ctx) { (void)ctx; xiaotai_signal_give(&reported); }

static void on_observed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->size == sizeof(xiaotai_ai_talk_status_t));
    if (event->args.value == XIAOTAI_AI_TALK_IDLE) xiaotai_signal_give(&finished);
}

static void on_caption(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->args.value > 0 && event->size == 0 && !event->data && !event->args.text[0]);
    ++caption_events;
    last_caption_revision = (uint32_t)event->args.value;
}

static void on_media_open(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    assert(event->size == sizeof(xiaotai_ai_talk_status_t) && event->data);
    const xiaotai_ai_talk_status_t *opened = event->data;
    assert(opened->phase == XIAOTAI_AI_TALK_CONNECTING && event->args.value == (int32_t)opened->phase);
    assert(opened->session && opened->link == s.id);
    ++media_events;
}

static xiaotai_err_t init_observer(void)
{
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_AI_TALK_STATE, on_observed, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_AI_TALK_CAPTION, on_caption, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_AI_TALK_MEDIA, on_media_open, NULL);
    return err;
}

static const xiaotai_module_t observer = {.name = "observer", .init = init_observer};
static const xiaotai_module_t *const with_observer[] = {&talk, &extension, &observer};

static void sync_owner(void (*fn)(void *), void *ctx)
{
    assert(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    assert(xiaotai_run(report, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&reported, 3000));
}

static void boot(const xiaotai_ai_talk_config_t *config, bool contribute, bool observe)
{
    memset(&s, 0, sizeof(s));
    extend = contribute;
    generation = 0;
    s.online = s.format = s.profile = true;
    assert(xiaotai_ai_talk_configure(config) == XIAOTAI_OK);
    const xiaotai_framework_config_t framework = {
        .modules = observe ? with_observer : modules, .module_count = observe ? 3 : 2,
        .applications = scope_scenario ? scope_apps : NULL, .application_count = scope_scenario ? 2U : 0U,
        .priorities = priorities, .available_needs = UINT32_MAX,
    };
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
}

static void reset_owner(void *ctx) { (void)ctx; xiaotai_ai_talk_reset(); }

static void shutdown(void)
{
    sync_owner(reset_owner, NULL);
    xiaotai_framework_reset();
    xiaotai_at_reset();
}

static uint32_t start(uint32_t preroll)
{
    uint32_t id = 0;
    const xiaotai_ai_talk_start_t options = {.origin = XIAOTAI_AI_TALK_ORIGIN_WAKE, .preroll_token = preroll};
    assert(xiaotai_ai_talk_start(preroll ? &options : NULL, &id) == XIAOTAI_OK);
    assert(id != 0 && status().session == id && status().phase == XIAOTAI_AI_TALK_CONNECTING);
    return id;
}

#define TOKEN_BODY(code) "{\"code\":" code ",\"data\":{\"peer_id\":\"test-peer\",\"token\":\"test-token\",\"role_id\":\"role\"}}"

static xiaotai_platform_reply_t token_response(xiaotai_err_t result, int http, const char *text)
{
    return (xiaotai_platform_reply_t){
        .result = result, .http_status = http, .body = text, .size = strlen(text),
    };
}

static void token_reply(int http, const char *text)
{
    const xiaotai_platform_reply_t reply = token_response(XIAOTAI_OK, http, text);
    s.reply(&reply, s.reply_context);
}

static void grant(void)
{
    token_reply(200, TOKEN_BODY("200"));
}

static void open_link(void)
{
    grant();
    assert(s.id != 0 && status().link == s.id && generation == 0);
    s.handlers.opened(s.handlers.ctx, s.id);
    assert(strstr(s.calls, "LORS") && generation == 0 && status().phase == XIAOTAI_AI_TALK_CONNECTING);
}

static void input(uint32_t link, uint32_t word, const char *text, bool dropped)
{
    const xiaotai_link_command_t command = {
        .word = word, .data = (const uint8_t *)text, .size = text ? strlen(text) : 0, .dropped = dropped,
    };
    s.handlers.command(s.handlers.ctx, link, &command);
}

static void accept(int rate, bool wrong_id)
{
    const char *id = strstr(s.sent, "\"id\":\"");
    assert(id && strstr(s.sent, "\"method\":\"start_session\"") && strstr(s.sent, "\"device_id\":\"test-device\""));
    assert(strstr(s.sent, "\"role_id\":\"role\"") && strstr(s.sent, "\"sample_rate\":8000"));
    char text[300];
    snprintf(text, sizeof(text),
             "{\"id\":\"%.*s\",\"result\":{\"session_id\":\"session\","
             "\"input_audio\":{\"codec\":\"alaw\",\"sample_rate\":%d,\"channels\":1},"
             "\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}",
             16, wrong_id ? "not-this-session" : id + 6, rate);
    input(s.id, (4U << 16U) | (0x2100U << 1U) | 1U, text, false);
}

static void ready(uint32_t preroll)
{
    start(preroll);
    open_link();
    accept(8000, false);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && generation == s.id);
}

static void test_start(void *ctx)
{
    (void)ctx;
    const xiaotai_ai_talk_start_t pre = {.preroll_token = 17};
    s.muted = true;
    assert(xiaotai_ai_talk_start(&pre, NULL) == XIAOTAI_ERR_INVALID_STATE && s.last_token == 17);
    s.muted = false;
    s.online = false;
    assert(xiaotai_ai_talk_start(NULL, NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    s.online = true;
    s.format = false;
    assert(xiaotai_ai_talk_start(NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    s.format = true;
    uint32_t call;
    const xiaotai_activity_t activity = {.name = "call"};
    assert(xiaotai_activity_begin(&activity, &call) == XIAOTAI_OK);
    assert(xiaotai_ai_talk_start(NULL, NULL) == XIAOTAI_ERR_BUSY);
    xiaotai_activity_end(call);
    s.request_error = XIAOTAI_ERR_NO_MEM;
    assert(xiaotai_ai_talk_start(NULL, NULL) == XIAOTAI_ERR_NO_MEM && xiaotai_activity_current(NULL) == 0);
    s.request_error = XIAOTAI_OK;
    const uint32_t old = start(0);
    const xiaotai_platform_reply_fn_t saved_reply = s.reply;
    void *const saved_context = s.reply_context;
    token_reply(200, "{\"code\":200,\"data\":{\"peer_id\":\"p\"}}");
    assert(status().end == XIAOTAI_AI_TALK_END_REFUSED && status().link == 0);
    const uint32_t next = start(0);
    const xiaotai_platform_reply_t reply = {.result = XIAOTAI_FAIL};
    saved_reply(&reply, saved_context);
    assert(next != old && status().stale_calls == 1 && status().session == next);
    assert(xiaotai_ai_talk_stop(old) == XIAOTAI_ERR_INVALID_STATE);
    token_reply(404, "{\"code\":200}");
    assert(status().end == XIAOTAI_AI_TALK_END_REFUSED && status().phase == XIAOTAI_AI_TALK_IDLE);
}

static void test_token_matrix(void *ctx)
{
    (void)ctx;
    static const struct {
        int http;
        const char *body;
        xiaotai_err_t result;
        xiaotai_ai_talk_end_t end;
    } refused[] = {
        {401, TOKEN_BODY("200"), XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {503, TOKEN_BODY("200"), XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {201, TOKEN_BODY("200"), XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {200, TOKEN_BODY("0"), XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {200, TOKEN_BODY("200.5"), XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {200, "{\"code\":40303}", XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {200, TOKEN_BODY("200") "x", XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {200, "{\"code\":200,\"data\":{}}", XIAOTAI_OK, XIAOTAI_AI_TALK_END_REFUSED},
        {200, TOKEN_BODY("200"), XIAOTAI_FAIL, XIAOTAI_AI_TALK_END_NETWORK},
    };

    ready(0);
    const xiaotai_ai_talk_status_t active = status();
    const xiaotai_platform_reply_fn_t old_reply = s.reply;
    void *const old_context = s.reply_context;
    const xiaotai_platform_reply_t valid = token_response(XIAOTAI_OK, 200, TOKEN_BODY("200"));
    old_reply(&valid, old_context);
    xiaotai_ai_talk_status_t after = status();
    assert(after.stale_calls == active.stale_calls + 1 && after.phase == active.phase &&
           after.session == active.session && after.link == active.link && after.failed == active.failed);
    assert(xiaotai_ai_talk_stop(active.session) == XIAOTAI_OK);

    for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); ++i) {
        const uint32_t session = start(0);
        const xiaotai_ai_talk_status_t before = status();
        if (i == 0) {
            old_reply(&valid, old_context);
            after = status();
            assert(after.stale_calls == before.stale_calls + 1 && after.phase == before.phase &&
                   after.session == session && !after.link && after.failed == before.failed);
        }
        const xiaotai_platform_reply_t reply = token_response(refused[i].result, refused[i].http, refused[i].body);
        s.reply(&reply, s.reply_context);
        after = status();
        assert(after.phase == XIAOTAI_AI_TALK_IDLE && after.end == refused[i].end && after.failed == before.failed + 1 &&
               !after.link && s.id == active.link && !xiaotai_activity_current(NULL));
        if (reply.result != XIAOTAI_OK) assert(after.error == reply.result);
    }

    start(0);
    char reused[] = TOKEN_BODY("200") "\t\r\n";
    token_reply(200, reused);
    memset(reused, 0, sizeof(reused));
    assert(s.id != active.link && status().link == s.id && s.request.timeout_ms == 12000U);
    s.handlers.opened(s.handlers.ctx, s.id);
    accept(8000, false);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && generation == s.id);
}

static void test_start_context(void *ctx)
{
    (void)ctx;
    start(0);
    open_link();
    const char *request_id = strstr(s.sent, "\"id\":\"");
    assert(request_id);
    request_id += 6;
    assert(strspn(request_id, "0123456789abcdef") == 16U && request_id[16] == '"');
    const uint32_t link = s.id;
    const xiaotai_ai_talk_status_t pending = status();
    const uint32_t sent = s.commands;
    char frame[384];
    const char *const ignored[] = {
        "\"method\":\"device_action\",\"params\":{}",
        "\"method\":null,\"error\":{\"code\":-32603}",
        "\"method\":\"device_action\",\"result\":{\"session_id\":\"not-a-reply\","
        "\"input_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1},"
        "\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}",
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(*ignored); ++i) {
        const int n = snprintf(frame, sizeof(frame), "{\"id\":\"%.16s\",%s}", request_id, ignored[i]);
        assert(n > 0 && (size_t)n < sizeof(frame));
        input(link, AI_TALK_COMMAND, frame, false);
        const xiaotai_ai_talk_status_t unchanged = status();
        assert(unchanged.phase == XIAOTAI_AI_TALK_CONNECTING && unchanged.session == pending.session &&
               unchanged.link == link && unchanged.failed == pending.failed && s.commands == sent && !generation);
    }
    const int n = snprintf(frame, sizeof(frame), "{\"id\":\"%.16s\",\"error\":{\"code\":-32603}}", request_id);
    assert(n > 0 && (size_t)n < sizeof(frame));
    input(link + 1U, AI_TALK_COMMAND, frame, false);
    assert(status().phase == XIAOTAI_AI_TALK_CONNECTING && !generation);
    accept(8000, false);
    const xiaotai_ai_talk_status_t active = status();
    const size_t calls = strlen(s.calls);
    accept(8000, false);
    input(link, AI_TALK_COMMAND, frame, false);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && status().session == active.session &&
           status().link == link && status().failed == active.failed && generation == link &&
           strlen(s.calls) == calls && s.commands == sent);
    assert(xiaotai_ai_talk_stop(active.session) == XIAOTAI_OK);

    start(0);
    open_link();
    input(s.id, AI_TALK_COMMAND, "{\"id\":\"\",\"error\":{\"code\":-32603}}", false);
    input(s.id, AI_TALK_COMMAND, frame, false); /* Ended session's request id. */
    assert(status().phase == XIAOTAI_AI_TALK_CONNECTING && !generation);
    request_id = strstr(s.sent, "\"id\":\"");
    assert(request_id);
    const int reject = snprintf(frame, sizeof(frame), "{\"id\":\"%.16s\",\"error\":{\"code\":-32603}}",
                                request_id + 6);
    assert(reject > 0 && (size_t)reject < sizeof(frame));
    input(s.id, AI_TALK_COMMAND, frame, false);
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_REFUSED && !generation &&
           !xiaotai_activity_current(NULL));
}

static void startup_resources_and_profiles(void *unused)
{
    (void)unused;
    const int64_t live = xiaotai_mem_host_outstanding();
    const uint32_t sent = s.commands;
    start(0);
    xiaotai_mem_host_fail_nth(1);
    grant();
    assert(!xiaotai_mem_host_fail_pending() && xiaotai_mem_host_outstanding() == live);
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_NO_MEMORY &&
           status().error == XIAOTAI_ERR_NO_MEM && !status().link && !xiaotai_activity_current(NULL));
    assert(s.commands == sent && !s.media_active);

    start(0);
    grant();
    const size_t before = strlen(s.calls);
    s.media_open_error = XIAOTAI_ERR_NO_MEM;
    s.handlers.opened(s.handlers.ctx, s.id);
    s.media_open_error = XIAOTAI_OK;
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_NO_MEMORY &&
           status().error == XIAOTAI_ERR_NO_MEM && !status().link && !xiaotai_activity_current(NULL));
    assert(!strcmp(s.calls + before, "D") && s.commands == sent && !s.media_active &&
           xiaotai_mem_host_outstanding() == live);

    start(0);
    grant();
    s.handlers.closed(s.handlers.ctx, s.id, XIAOTAI_LINK_END_NETWORK, -1);
    s.handlers.opened(s.handlers.ctx, s.id);
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_NETWORK &&
           !status().link && s.commands == sent && !xiaotai_activity_current(NULL));

    static const char mono[] = "{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":1}";
    static const char stereo[] = "{\"codec\":\"alaw\",\"sample_rate\":8000,\"channels\":2}";
    const char *const profiles[][2] = {{NULL, NULL}, {stereo, mono}, {mono, stereo}};
    for (size_t i = 0; i < sizeof(profiles) / sizeof(*profiles); ++i) {
        start(0);
        grant();
        const uint32_t attempted = xiaotai_mem_host_allocations();
        xiaotai_mem_host_fail_nth(1);
        s.handlers.opened(s.handlers.ctx, s.id);
        assert(xiaotai_mem_host_fail_pending() && xiaotai_mem_host_allocations() == attempted);
        xiaotai_mem_host_fail_nth(0);
        assert(s.commands == sent + i + 1U && status().phase == XIAOTAI_AI_TALK_CONNECTING);
        const char *id = strstr(s.sent, "\"id\":\"");
        assert(id);
        char response[320];
        const int n = profiles[i][0] ?
            snprintf(response, sizeof(response), "{\"id\":\"%.16s\",\"result\":{\"session_id\":\"fixture\","
                     "\"input_audio\":%s,\"output_audio\":%s}}", id + 6, profiles[i][0], profiles[i][1]) :
            snprintf(response, sizeof(response), "{\"id\":\"%.16s\",\"result\":{\"session_id\":\"fixture\"}}", id + 6);
        assert(n > 0 && (size_t)n < sizeof(response));
        input(s.id, AI_TALK_COMMAND, response, false);
        assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_UNSUPPORTED &&
               status().error == XIAOTAI_ERR_NOT_SUPPORTED && !s.media_active && !xiaotai_activity_current(NULL));
        assert(xiaotai_mem_host_outstanding() == live);
    }
}

static void test_handshake(void *ctx)
{
    (void)ctx;
    start(0);
    open_link();
    assert(!(s.media.flags & XIAOTAI_MEDIA_SESSION_TRACE_ARRIVALS));
    accept(8000, true);
    assert(status().phase == XIAOTAI_AI_TALK_CONNECTING && generation == 0);
    accept(16000, false);
    assert(status().end == XIAOTAI_AI_TALK_END_UNSUPPORTED && strstr(s.calls, "CD"));
    ready(0);
    assert(strstr(s.calls, "APY"));
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"round_start\"}", false);
    assert(status().phase == XIAOTAI_AI_TALK_SPEAKING);
    assert(xiaotai_ai_talk_interrupt(0) == XIAOTAI_OK && strstr(s.sent, "\"interrupt\""));
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && strstr(s.calls, "FS"));
    assert(xiaotai_ai_talk_submit(0) == XIAOTAI_OK && strstr(s.sent, "\"submit_speech\""));
    s.send_error = XIAOTAI_FAIL;
    assert(xiaotai_ai_talk_submit(0) == XIAOTAI_FAIL && status().end == XIAOTAI_AI_TALK_END_NETWORK);
}

static void handshake_begin(void *unused)
{
    (void)unused;
    start(0);
    open_link();
}

static void handshake_pending(void *unused)
{
    (void)unused;
    assert(status().phase == XIAOTAI_AI_TALK_CONNECTING && status().session && !s.media_active);
    const char *id = strstr(s.sent, "\"id\":\"");
    assert(id);
    char frame[100];
    const int n = snprintf(frame, sizeof(frame), "{\"id\":\"%.16s\",\"method\":null,\"error\":{}}", id + 6);
    assert(n > 0 && (size_t)n < sizeof(frame));
    input(s.id, AI_TALK_COMMAND, frame, false);
    assert(status().phase == XIAOTAI_AI_TALK_CONNECTING && !s.media_active);
}

static void handshake_accept(void *unused)
{
    (void)unused;
    accept(8000, false);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && s.media_active);
}

static void handshake_expired(void *unused)
{
    (void)unused;
    const xiaotai_ai_talk_status_t after = status();
    assert(after.phase == XIAOTAI_AI_TALK_IDLE && after.end == XIAOTAI_AI_TALK_END_TIMEOUT &&
           after.error == XIAOTAI_ERR_TIMEOUT && after.failed == 1 && !s.media_active);
    assert(!after.link && !xiaotai_activity_current(NULL) && strstr(s.calls, "CD") && s.commands == 1);
    const size_t calls = strlen(s.calls);
    accept(8000, false);
    assert(strlen(s.calls) == calls && status().failed == after.failed && !s.media_active);
}

static void handshake_not_expired(void *unused)
{
    (void)unused;
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && status().failed == 1 && s.media_active);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
}

static void startup_send_failure(void *unused)
{
    (void)unused;
    start(0);
    grant();
    const uint32_t link = s.id;
    const uint32_t sent = s.commands;
    const size_t calls = strlen(s.calls);
    s.handlers.opened(s.handlers.ctx, 0);
    assert(strlen(s.calls) == calls && s.commands == sent && status().link == link);

    s.send_error = XIAOTAI_FAIL;
    s.handlers.opened(s.handlers.ctx, link);
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_NETWORK &&
           status().error == XIAOTAI_FAIL && !status().link && !s.media_active && !xiaotai_activity_current(NULL));
    assert(s.commands == sent + 1 && !strcmp(s.calls + calls, "ORSCD"));
}

static void startup_writer_boundary(void)
{
    char wire[AI_TALK_REQUEST_TEXT_MAX];
    size_t bytes = 0;
    assert(ai_talk_session_request(wire, sizeof(wire), "fixture", "device", "role", &ai_talk_audio_g711, &ai_talk_audio_g711, &bytes) == XIAOTAI_OK);
    assert(bytes && strlen(wire) == bytes);
    size_t size = 0;
    assert(ai_talk_session_request(wire, bytes + 1U, "fixture", "device", "role", &ai_talk_audio_g711, &ai_talk_audio_g711, &size) == XIAOTAI_OK && size == bytes);
    assert(ai_talk_session_request(wire, bytes, "fixture", "device", "role", &ai_talk_audio_g711, &ai_talk_audio_g711, &size) == XIAOTAI_ERR_INVALID_SIZE && !size);

    /* What is sent and what is heard are asked for apart, and answered apart. */
    assert(ai_talk_session_request(wire, sizeof(wire), "fixture", "device", "role", &ai_talk_audio_g711, &ai_talk_audio_opus,
                                   &size) == XIAOTAI_OK);
    assert(strstr(strstr(wire, "\"input_audio\""), "\"g711a\"") && strstr(strstr(wire, "\"output_audio\""), "\"opus\""));
    assert(!strstr(strstr(wire, "\"output_audio\""), "g711a"));
    /* Opus asks for Opus and takes nothing else for an answer. */
    assert(ai_talk_session_request(wire, sizeof(wire), "fixture", "device", "role", &ai_talk_audio_opus, &ai_talk_audio_opus, &size) == XIAOTAI_OK);
    assert(strstr(wire, "\"codec\":\"opus\"") && strstr(wire, "\"sample_rate\":16000") && !strstr(wire, "g711a"));
    static const struct { const char *input, *output; ai_talk_handshake_t want; } replies[] = {
        {"opus\",\"sample_rate\":16000", "opus\",\"sample_rate\":16000", AI_TALK_HANDSHAKE_ACCEPTED},
        {"opus\",\"sample_rate\":16000", "alaw\",\"sample_rate\":8000", AI_TALK_HANDSHAKE_UNSUPPORTED},
        {"opus\",\"sample_rate\":8000", "opus\",\"sample_rate\":8000", AI_TALK_HANDSHAKE_UNSUPPORTED},
    };
    for (size_t i = 0; i < sizeof(replies) / sizeof(*replies); ++i) {
        char text[300];
        const int n = snprintf(text, sizeof(text), "{\"id\":\"fixture\",\"result\":{\"session_id\":\"s\","
                               "\"input_audio\":{\"codec\":\"%s,\"channels\":1},\"output_audio\":{\"codec\":\"%s,\"channels\":1}}}",
                               replies[i].input, replies[i].output);
        assert(n > 0 && (size_t)n < sizeof(text));
        ai_talk_message_t message;
        ai_talk_message_parse(text, (size_t)n, 64, &message);
        assert(ai_talk_handshake_check(&message, "fixture", &ai_talk_audio_opus, &ai_talk_audio_opus) == replies[i].want);
        assert(ai_talk_handshake_check(&message, "fixture", &ai_talk_audio_g711, &ai_talk_audio_g711) == AI_TALK_HANDSHAKE_UNSUPPORTED);
        assert(ai_talk_handshake_check(&message, "fixture", &ai_talk_audio_g711, &ai_talk_audio_opus) ==
               AI_TALK_HANDSHAKE_UNSUPPORTED);
    }
}

static void caption_notice(uint32_t link, const char *params)
{
    char message[360];
    int n = snprintf(message, sizeof(message), "{\"method\":\"caption\",\"params\":%s}", params);
    assert(n > 0 && (size_t)n < sizeof(message));
    input(link, AI_TALK_COMMAND, message, false);
}

static void test_caption(void *ctx)
{
    (void)ctx;
    ready(0);
    char full[70];
    xiaotai_ai_talk_caption_t caption = {.text = full, .capacity = sizeof(full)};
    assert(xiaotai_ai_talk_caption_read(&caption) && caption.session == status().session &&
           caption.revision == 0 && !full[0]);
    const char *tts1 = "{\"caption_type\":1,\"text\":\"ha\",\"utterance_id\":2,\"seq_num\":1,\"mode\":1,"
                       "\"emotion\":\"Joy\\u0021\"}";
    const char *tts2 = "{\"caption_type\":1.0,\"text\":\"ha\",\"utterance_id\":2,\"seq_num\":2e0,\"mode\":1}";
    caption_notice(s.id, tts1);
    caption_notice(s.id, tts2);
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(full, "haha") && !strcmp(caption.emotion, "Joy!"));
    assert(caption.paragraph == 1 && caption.revision == 2 && status().phase == XIAOTAI_AI_TALK_SPEAKING);
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"round_end\"}", false);
    caption_notice(s.id, tts2);
    caption_notice(s.id, tts1);
    caption_notice(s.id + 1U, "{\"caption_type\":1,\"text\":\"stale\",\"utterance_id\":2,\"seq_num\":3}");
    assert(!xiaotai_ai_talk_caption_read(&caption) && status().phase == XIAOTAI_AI_TALK_LISTENING);

    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"question\",\"utterance_id\":2,\"seq_num\":1,\"mode\":1}");
    caption_notice(s.id, tts2);
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(full, "question") && !caption.emotion[0]);
    assert(caption.paragraph == 2 && caption.revision == 3);
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"bad\",\"utterance_id\":2,\"seq_num\":1.5}");
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"bad\\u0000\",\"utterance_id\":2,\"seq_num\":2}");
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"bad\xed\xa0\x80\",\"utterance_id\":2,\"seq_num\":2}");
    assert(!xiaotai_ai_talk_caption_read(&caption));
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"answer\",\"utterance_id\":2,\"seq_num\":2,\"mode\":1,"
                         "\"is_final\":true,\"emotion\":\"ignored\"}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(full, "answer") && !caption.emotion[0]);
    assert(caption.final && caption.paragraph == 2 && caption.revision == 4 &&
           status().phase == XIAOTAI_AI_TALK_THINKING);
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"answer\",\"utterance_id\":2,\"seq_num\":3,"
                         "\"is_final\":true}");
    assert(!xiaotai_ai_talk_caption_read(&caption));

    char prefix[62], params[230];
    memset(prefix, 'q', sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
    snprintf(params, sizeof(params), "{\"caption_type\":1,\"text\":\"%s\\u4e2dX\",\"utterance_id\":4}", prefix);
    caption_notice(s.id, params);
    assert(xiaotai_ai_talk_caption_read(&caption) && caption.truncated && !strcmp(full, prefix));
    assert(caption.paragraph == 3 && caption.revision == 5);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"Z\",\"mode\":1,\"utterance_id\":4}");
    assert(!xiaotai_ai_talk_caption_read(&caption)); /* never paste a suffix after a lost unit */
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"\xe4\xb8\xad\\ud83d\\ude00!\",\"utterance_id\":4}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !caption.truncated && caption.size == 8);
    assert(!strcmp(full, "\xe4\xb8\xad\xf0\x9f\x98\x80!") && caption.revision == 6 && caption.paragraph == 3);
    char short_text[6];
    xiaotai_ai_talk_caption_t preview = {.text = short_text, .capacity = sizeof(short_text)};
    assert(xiaotai_ai_talk_caption_read(&preview) && preview.truncated && preview.size == 3);
    assert(!strcmp(short_text, "\xe4\xb8\xad") && status().caption_revision == 6);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
}

static void check_caption_events(void *ctx)
{
    (void)ctx;
    assert(caption_events == 6 && last_caption_revision == 6 && media_events == 1);
}

static void test_caption_emotion(void *ctx)
{
    (void)ctx;
    ready(0);
    uint32_t words = 0, before = 0;
    (void)ai_talk_caption_word(&before);
    xiaotai_ai_talk_caption_t caption = {0};
    /* The first sentence: the word comes with its last caption. */
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"one\",\"utterance_id\":1,\"seq_num\":1,\"mode\":1}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !caption.emotion[0]);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\".\",\"utterance_id\":1,\"seq_num\":2,\"mode\":1,\"is_final\":true,"
                         "\"emotion\":\"开心\",\"begin_time_ms\":0,\"end_time_ms\":640}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(caption.emotion, "开心"));
    /* The next sentence begins without one: the answer keeps the word it had, until its own comes. */
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"two\",\"utterance_id\":2,\"seq_num\":1,\"mode\":1}");
    assert(xiaotai_ai_talk_caption_read(&caption) && caption.paragraph == 2 && !strcmp(caption.emotion, "开心"));
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"?\",\"utterance_id\":2,\"seq_num\":2,\"mode\":1,\"emotion\":\"\"}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(caption.emotion, "开心"));
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\".\",\"utterance_id\":2,\"seq_num\":3,\"mode\":1,\"is_final\":true,"
                         "\"emotion\":\"a b\\n\"}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(caption.emotion, "a b\n"));
    assert(!strcmp(ai_talk_caption_word(&words), "a b\n") && words == before + 2U);
    /* The end of the round keeps it; the user speaking ends it, and the answer that follows begins without. */
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"round_end\"}", false);
    assert(!xiaotai_ai_talk_caption_read(&caption) && !strcmp(caption.emotion, "a b\n"));
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"why\",\"utterance_id\":3,\"seq_num\":1}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !caption.emotion[0]);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"so\",\"utterance_id\":4,\"seq_num\":1,\"mode\":1}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !caption.emotion[0]);
    assert(!strcmp(ai_talk_caption_word(&words), "a b\n") && words == before + 2U);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
}

/* Captions as the service sends them (docs.tange.ai, AI talk events, "caption"): an answer in three
 * sentences, each in pieces, and the word of each with its last caption; ten words. */
static void test_caption_words(void *ctx)
{
    (void)ctx;
    static const char *const words[] = {"中性", "开心", "兴奋", "温和", "安慰", "思考", "惊讶", "严肃", "困倦", "困惑"};
    ready(0);
    uint32_t before = 0, count = 0;
    (void)ai_talk_caption_word(&before);
    xiaotai_ai_talk_caption_t caption = {0};
    char params[320];
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        for (unsigned piece = 1; piece <= 3; ++piece) {
            const bool last = piece == 3;
            const int n = snprintf(params, sizeof(params), "{\"text\":\"%s\",\"caption_type\":1,\"is_final\":%s,\"mode\":1,\"utterance_id\":%u,\"seq_num\":%u,"
                                   "\"begin_time_ms\":%u,\"end_time_ms\":%u%s%s%s}", last ? "。" : "说", last ? "true" : "false", i + 1U, piece,
                                   i * 1800U, i * 1800U + piece * 600U, last ? ",\"emotion\":\"" : "", last ? words[i] : "", last ? "\"" : "");
            assert(n > 0 && (size_t)n < sizeof(params));
            caption_notice(s.id, params);
            assert(xiaotai_ai_talk_caption_read(&caption) && caption.speaker == XIAOTAI_AI_TALK_SPEAKER_AI);
            /* Until its own word comes a sentence has the one before it. */
            assert(!strcmp(caption.emotion, last ? words[i] : i ? words[i - 1U] : ""));
            assert(caption.final == last && caption.paragraph == i + 1U && status().phase == XIAOTAI_AI_TALK_SPEAKING);
            /* A finished sentence counts, with where it begins in its answer's voice. */
            assert(caption.sentences == i + (last ? 1U : 0U) && (!caption.sentences || caption.begin_ms == (int32_t)((caption.sentences - 1U) * 1800U)));
        }
        assert(!strcmp(ai_talk_caption_word(&count), words[i]) && count == before + i + 1U);
    }
    /* An answer of a role without emotion recognition: the same captions, no word. */
    caption_notice(s.id, "{\"text\":\"问\",\"caption_type\":0,\"is_final\":true,\"mode\":0,\"utterance_id\":90,\"seq_num\":1}");
    caption_notice(s.id, "{\"text\":\"答。\",\"caption_type\":1,\"is_final\":true,\"mode\":1,\"utterance_id\":91,\"seq_num\":1,\"begin_time_ms\":0,\"end_time_ms\":900}");
    assert(xiaotai_ai_talk_caption_read(&caption) && caption.final && !caption.emotion[0]);
    assert(caption.sentences == 11U && caption.begin_ms == 0);
    /* One that does not say when it begins. */
    caption_notice(s.id, "{\"text\":\"再答。\",\"caption_type\":1,\"is_final\":true,\"mode\":0,\"utterance_id\":91,\"seq_num\":0}");
    assert(xiaotai_ai_talk_caption_read(&caption) && caption.sentences == 12U && caption.begin_ms == -1);
    (void)ai_talk_caption_word(&count);
    assert(count == before + 10U);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
}

/* Captions as the service sent them on 2026-11-26: nothing numbered (seq_num 0), what the user says
 * as the whole text so far, an answer sentence by sentence under one utterance. */
static void test_caption_sentences(void *ctx)
{
    (void)ctx;
    ready(0);
    char text[64];
    xiaotai_ai_talk_caption_t caption = {.text = text, .capacity = sizeof(text)};
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"你\",\"utterance_id\":2,\"seq_num\":0,\"mode\":1}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "你"));
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"你好呀\",\"utterance_id\":2,\"seq_num\":0,\"mode\":1}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "你好呀") && !caption.final);
    caption_notice(s.id, "{\"caption_type\":0,\"text\":\"你好呀。\",\"utterance_id\":2,\"seq_num\":0,\"mode\":0,"
                         "\"is_final\":true}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "你好呀。") && caption.final);
    const uint32_t asked = caption.paragraph;
    /* The answer: the second sentence goes on after the first, the same one again is not said twice. */
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"好的。\",\"utterance_id\":2,\"seq_num\":0,\"mode\":0,"
                         "\"is_final\":true}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "好的。") && caption.paragraph == asked + 1U);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"再说说？\",\"utterance_id\":2,\"seq_num\":0,\"mode\":0,"
                         "\"is_final\":true}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "好的。再说说？") && caption.paragraph == asked + 1U);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"再说说？\",\"utterance_id\":2,\"seq_num\":0,\"mode\":0,"
                         "\"is_final\":true}");
    assert(!xiaotai_ai_talk_caption_read(&caption));
    /* Whole text over a sentence not finished corrects it; a finished one is followed. */
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"嗯\",\"utterance_id\":3,\"seq_num\":0,\"mode\":0}");
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"嗯嗯\",\"utterance_id\":3,\"seq_num\":0,\"mode\":0}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "嗯嗯") && caption.paragraph == asked + 2U);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"嗯嗯，对。\",\"utterance_id\":3,\"seq_num\":0,\"mode\":0,"
                         "\"is_final\":true}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "嗯嗯，对。") && caption.final);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"好。\",\"utterance_id\":3,\"seq_num\":0,\"mode\":0}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "嗯嗯，对。好。") && !caption.final);
    /* Numbered pieces keep their order. */
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"a\",\"utterance_id\":4,\"seq_num\":2,\"mode\":1}");
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"b\",\"utterance_id\":4,\"seq_num\":1,\"mode\":1}");
    assert(xiaotai_ai_talk_caption_read(&caption) && !strcmp(text, "a"));
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
}

static void test_caption_capacity(void *ctx)
{
    (void)ctx;
    ai_talk_caption_reset();
    const int64_t before = xiaotai_mem_host_outstanding();
    for (unsigned fail = 1; fail <= 2; ++fail) {
        xiaotai_mem_host_fail_nth(fail);
        ai_talk_caption_start(64);
        assert(!xiaotai_mem_host_fail_pending() && xiaotai_mem_host_outstanding() == before);
    }
    ai_talk_caption_start(64);
    assert(xiaotai_mem_host_outstanding() == before + 2);
    ready(0);
    char params[120];
    for (unsigned group = 0; group < 8; ++group) {
        snprintf(params, sizeof(params), "{\"caption_type\":1,\"text\":\"x\",\"utterance_id\":%u,\"seq_num\":1}", group);
        caption_notice(s.id, params);
    }
    assert(status().caption_revision == 8);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"old\",\"utterance_id\":0,\"seq_num\":1}");
    assert(status().caption_revision == 8);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"ninth\",\"utterance_id\":8,\"seq_num\":1}");
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"evicted\",\"utterance_id\":0,\"seq_num\":1}");
    assert(status().caption_revision == 10);
    char text[20];
    xiaotai_ai_talk_caption_t snapshot = {.text = text, .capacity = sizeof(text)};
    assert(xiaotai_ai_talk_caption_read(&snapshot) && snapshot.paragraph == 10 && !strcmp(text, "evicted"));
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
    ready(0);
    assert(xiaotai_ai_talk_caption_read(&snapshot) && snapshot.revision == 0 && !text[0]);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"new\",\"utterance_id\":0,\"seq_num\":1}");
    assert(xiaotai_ai_talk_caption_read(&snapshot) && snapshot.revision == 1 &&
           snapshot.paragraph == 1 && !strcmp(text, "new"));
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"overflow\",\"utterance_id\":0,\"seq_num\":4294967296}");
    assert(!xiaotai_ai_talk_caption_read(&snapshot) && status().caption_revision == 1);
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"A\\u4e2dB\"}"); /* old peers omit sequence metadata */
    const struct { size_t capacity; const char *text; bool truncated; } edges[] = {
        {1, "", true}, {3, "A", true}, {5, "A\xe4\xb8\xad", true}, {6, "A\xe4\xb8\xad" "B", false},
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(*edges); ++i) {
        xiaotai_ai_talk_caption_t edge = {.text = text, .capacity = edges[i].capacity};
        assert(xiaotai_ai_talk_caption_read(&edge) && edge.revision == 2 && edge.speaker == XIAOTAI_AI_TALK_SPEAKER_AI);
        assert(edge.truncated == edges[i].truncated && !strcmp(text, edges[i].text));
    }
}

static void remote(void)
{
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"end_session\"}", false);
}

static void platform_event(xiaotai_platform_offline_t reason)
{
    const xiaotai_args_t args = {.value = reason};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, &args, NULL, 0) == XIAOTAI_OK);
}

/* 8 s of receiving, 7 s of playing out and 1 s: the engine's report has had its time. */
#define ENDING_US INT64_C(16000000)

static struct {
    uint32_t completed, failed, session;
} ending;

static void ending_begin(void *ctx)
{
    const bool left = *(const bool *)ctx; /* the service leaves without a word */
    ready(0);
    ending.completed = status().completed;
    ending.failed = status().failed;
    ending.session = status().session;
    if (left) s.handlers.closed(NULL, s.id, XIAOTAI_LINK_END_PEER, 0);
    else remote();
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && s.media_active);
}

static void ending_pending(void *unused)
{
    (void)unused;
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && status().session == ending.session && s.media_active);
}

static void ending_expired(void *unused)
{
    (void)unused;
    const xiaotai_ai_talk_status_t after = status();
    assert(after.phase == XIAOTAI_AI_TALK_IDLE && !after.session && !after.link && !s.media_active);
    assert(after.end == XIAOTAI_AI_TALK_END_REMOTE && after.error == XIAOTAI_OK);
    assert(after.completed == ending.completed + 1U && after.failed == ending.failed);
    assert(!xiaotai_activity_current(NULL));
    /* The report that comes after all is nobody's. */
    s.media.event(s.id, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    assert(status().completed == after.completed && status().phase == XIAOTAI_AI_TALK_IDLE);
}

static void ending_drained_then_talk(void *unused)
{
    (void)unused;
    s.media.event(s.id, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_REMOTE);
    ready(0);
    ending.session = status().session;
}

static void ending_still_talking(void *unused)
{
    (void)unused;
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && status().session == ending.session && s.media_active);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
}

static void intent(const char *name, uint32_t session)
{
    const xiaotai_args_t args = {.value = (int32_t)session};
    assert(xiaotai_intent_post(name, &args) == XIAOTAI_OK);
}

static void intent_in_farewell(void *ctx)
{
    s.calls[0] = '\0';
    ready(0);
    remote();
    ending.completed = status().completed;
    ending.session = status().session;
    intent(ctx, 0);
}

static void intent_started_next(void *unused)
{
    (void)unused;
    assert(status().phase == XIAOTAI_AI_TALK_CONNECTING && status().session != ending.session);
    assert(status().completed == ending.completed + 1U && !s.media_active);
    assert(status().origin == XIAOTAI_AI_TALK_ORIGIN_USER);
    ending.session = status().session;
    intent(XIAOTAI_INTENT_TALK_START, 0); /* Ignored while it runs. */
}

static void intent_ignored(void *unused)
{
    (void)unused;
    assert(status().phase == XIAOTAI_AI_TALK_CONNECTING && status().session == ending.session);
    intent(XIAOTAI_INTENT_TALK_TOGGLE, 0); /* Stops what runs. */
}

static void intent_stopped(void *unused)
{
    (void)unused;
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_LOCAL);
}

/* A toggle that names the conversation never starts one. */
static void intent_named_in_farewell(void *unused)
{
    (void)unused;
    s.calls[0] = '\0';
    ready(0);
    remote();
    intent(XIAOTAI_INTENT_TALK_TOGGLE, status().session);
}

/* The next one cannot start: the farewell goes on. */
static void intent_offline_in_farewell(void *ctx)
{
    s.calls[0] = '\0';
    ready(0);
    remote();
    ending.session = status().session;
    s.online = false;
    intent(ctx, 0);
}

static void intent_refused(void *unused)
{
    ending_pending(unused);
    s.online = true;
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
}

static void test_stop_scope(void *ctx)
{
    (void)ctx;
    const size_t initial = strlen(s.calls);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_activity_current(NULL) == 0 && strlen(s.calls) == initial);

    const char *const other[] = {"stream", "call"};
    for (size_t i = 0; i < sizeof(other) / sizeof(*other); ++i) {
        const xiaotai_activity_t request = {.name = other[i]};
        uint32_t owner = 0;
        assert(xiaotai_activity_begin(&request, &owner) == XIAOTAI_OK && owner);
        xiaotai_activity_set_generation(owner, xiaotai_generation_next());
        assert(xiaotai_ai_talk_stop(0) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_activity_current(NULL) == owner && strlen(s.calls) == initial);
        xiaotai_activity_end(owner);
    }

    for (unsigned active = 0; active <= 1; ++active) {
        const uint32_t id = start(0);
        open_link();
        if (active) accept(8000, false);
        assert(status().phase == (active ? XIAOTAI_AI_TALK_LISTENING : XIAOTAI_AI_TALK_CONNECTING));
        const size_t before = strlen(s.calls);
        assert(xiaotai_ai_talk_stop(id) == XIAOTAI_OK && strstr(s.sent, "\"end_session\""));
        assert(!strcmp(s.calls + before, active ? "ZSCD" : "SCD"));
        assert(status().phase == XIAOTAI_AI_TALK_IDLE && status().end == XIAOTAI_AI_TALK_END_LOCAL);
        assert(status().completed == active + 1U && xiaotai_activity_current(NULL) == 0);
        const size_t ended = strlen(s.calls);
        assert(xiaotai_ai_talk_stop(id) == XIAOTAI_ERR_INVALID_STATE && strlen(s.calls) == ended);
    }
}

static void test_end(void *ctx)
{
    (void)ctx;
    ready(0);
    s.handlers.closed(NULL, s.id + 1U, XIAOTAI_LINK_END_NETWORK, -1);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK && strstr(s.sent, "\"end_session\""));
    assert(status().end == XIAOTAI_AI_TALK_END_LOCAL && strstr(s.calls, "ZSCD"));
    ready(0);
    const uint32_t previous = s.id;
    const uint32_t sent = s.commands;
    const size_t before = strlen(s.calls);
    input(previous + 1U, AI_TALK_COMMAND, "{\"method\":\"end_session\"}", false);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && strlen(s.calls) == before);
    /* The link stays for the last sentence, until everything was played. */
    remote();
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && generation == 0 && status().link == previous);
    assert(!strcmp(s.calls + before, "ZT") && s.media_active && s.commands == sent);
    input(previous, AI_TALK_COMMAND, "{\"method\":\"end_session\"}", false);
    s.media.event(previous + 1U, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    s.handlers.closed(NULL, previous + 1U, XIAOTAI_LINK_END_NETWORK, -1);
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && !strcmp(s.calls + before, "ZT"));
    s.media.event(previous, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    uint32_t completed = status().completed;
    s.media.event(previous, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    assert(status().end == XIAOTAI_AI_TALK_END_REMOTE && status().phase == XIAOTAI_AI_TALK_IDLE &&
           status().completed == completed && !strcmp(s.calls + before, "ZTCD") && s.commands == sent);
    /* The service closes the link meanwhile: nothing more comes, what is held is played. */
    ready(0);
    const size_t closing = strlen(s.calls);
    const uint32_t closed = s.id;
    remote();
    s.handlers.closed(NULL, closed, XIAOTAI_LINK_END_NETWORK, -1);
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && !status().link && !strcmp(s.calls + closing, "ZTG"));
    s.media.event(closed, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    assert(status().end == XIAOTAI_AI_TALK_END_REMOTE && status().error == XIAOTAI_OK &&
           status().completed == completed + 1U && !strcmp(s.calls + closing, "ZTGC"));
    /* The service leaves without a word: what came is played, and nothing failed. */
    for (unsigned fault = 0; fault < 2; ++fault) {
        ready(0);
        const size_t left = strlen(s.calls);
        const uint32_t done = status().completed, failures = status().failed;
        s.handlers.closed(NULL, s.id, fault ? XIAOTAI_LINK_END_PEER_FAULT : XIAOTAI_LINK_END_PEER, -40008);
        assert(status().phase == XIAOTAI_AI_TALK_ENDING && !status().link && s.media_active && generation == 0);
        assert(!strcmp(s.calls + left, "ZG"));
        s.media.event(s.id, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
        assert(status().end == XIAOTAI_AI_TALK_END_REMOTE && status().completed == done + 1U &&
               status().failed == failures && !s.media_active);
    }
    /* In the user's turn the packets tell when it is over; in the service's turn its answer may not have begun. */
    ready(0);
    remote();
    assert(tail_wait_ms == 0);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
    ready(0);
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"round_start\"}", false);
    remote();
    assert(tail_wait_ms == 1000 && status().phase == XIAOTAI_AI_TALK_ENDING);
    /* The words of the last sentence still come; nothing else is heard of the service. */
    const uint32_t words = ai_talk_caption_revision(), unheard = status().stale_commands, answered = s.commands;
    caption_notice(s.id, "{\"caption_type\":1,\"text\":\"bye\"}");
    assert(ai_talk_caption_revision() != words && status().stale_commands == unheard);
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"interrupt\"}", false);
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"round_end\"}", false);
    input(s.id, AI_TALK_COMMAND, "{\"id\":17,\"method\":\"device_action\",\"params\":{\"action\":\"unknown\"}}", false);
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && status().stale_commands == unheard + 3U && s.commands == answered);
    /* A new conversation takes its place. */
    const uint32_t leaving = status().session;
    ready(0);
    assert(status().session != leaving && status().phase == XIAOTAI_AI_TALK_LISTENING);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
    /* A call, maintenance or a platform that goes away end it at once, and it has not failed. */
    ready(0);
    remote();
    uint32_t caller = 0;
    const xiaotai_activity_t ringing = {.name = "call"};
    assert(xiaotai_activity_begin(&ringing, &caller) == XIAOTAI_OK && !s.media_active);
    assert(status().end == XIAOTAI_AI_TALK_END_PREEMPTED && xiaotai_activity_current(NULL) == caller);
    xiaotai_activity_end(caller);
    ready(0);
    remote();
    uint32_t maintenance = 0;
    assert(xiaotai_quiesce_begin("test", &maintenance) == XIAOTAI_OK && status().end == XIAOTAI_AI_TALK_END_SYSTEM);
    xiaotai_quiesce_end(maintenance);
    ready(0);
    remote();
    s.media.event(s.id, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL, NULL);
    assert(status().end == XIAOTAI_AI_TALK_END_AUDIO && status().phase == XIAOTAI_AI_TALK_IDLE);
    ready(0);
    finish_result = XIAOTAI_ERR_INVALID_STATE;
    remote();
    finish_result = XIAOTAI_OK;
    assert(status().end == XIAOTAI_AI_TALK_END_AUDIO && status().phase == XIAOTAI_AI_TALK_IDLE && !s.media_active);
    /* Whoever stops it meanwhile stops it at once, and tells the service nothing it said itself. */
    ready(0);
    const size_t stopping = strlen(s.calls);
    const uint32_t told = s.commands;
    remote();
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK && s.commands == told && !strcmp(s.calls + stopping, "ZTCD"));
    assert(status().end == XIAOTAI_AI_TALK_END_LOCAL && status().phase == XIAOTAI_AI_TALK_IDLE);
    ready(0);
    remote();
    s.media.event(s.id, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_ERR_TIMEOUT, NULL);
    assert(status().end == XIAOTAI_AI_TALK_END_TIMEOUT && status().error == XIAOTAI_ERR_TIMEOUT);
    ready(0);
    s.handlers.closed(NULL, s.id, XIAOTAI_LINK_END_NETWORK, -1);
    assert(status().end == XIAOTAI_AI_TALK_END_NETWORK && status().phase == XIAOTAI_AI_TALK_IDLE);
    ready(0);
    uint32_t call = 0;
    const xiaotai_activity_t activity = {.name = "call"};
    assert(xiaotai_activity_begin(&activity, &call) == XIAOTAI_OK);
    assert(status().end == XIAOTAI_AI_TALK_END_PREEMPTED && xiaotai_activity_current(NULL) == call);
    xiaotai_activity_end(call);
    ready(0);
    uint32_t ticket = 0;
    assert(xiaotai_quiesce_begin("test", &ticket) == XIAOTAI_OK && status().end == XIAOTAI_AI_TALK_END_SYSTEM);
    xiaotai_quiesce_end(ticket);
    ready(0);
    platform_event(XIAOTAI_PLATFORM_OFFLINE_REFRESH);
    platform_event(XIAOTAI_PLATFORM_OFFLINE_SIGNALING);
}

static void end_binding(void *ctx)
{
    (void)ctx;
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING);
    platform_event(XIAOTAI_PLATFORM_OFFLINE_BINDING);
}

static void test_signaling_hold(void *ctx)
{
    const bool enabled = *(const bool *)ctx;
    const xiaotai_activity_t call = {.name = "call"};
    uint32_t owner = 0;
    assert(xiaotai_activity_begin(&call, &owner) == XIAOTAI_OK);
    xiaotai_activity_end(owner);
    assert(s.signaling_attempts == 0);

    uint32_t session = start(0);
    open_link();
    assert(s.signaling_attempts == 0);
    assert(xiaotai_ai_talk_stop(session) == XIAOTAI_OK);
    assert(s.signaling_releases == 0);

    s.media_wrong_generation = true;
    ready(0);
    assert(s.signaling_attempts == 0);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
    s.media_wrong_generation = false;
    ready(0);
    assert(s.signaling_attempts == (unsigned)enabled);
    if (!enabled) {
        assert(!s.held_signaling && xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
        assert(!s.signaling_releases);
        return;
    }

    assert(s.held_signaling && s.signaling_releases == 0);
    const xiaotai_platform_done_fn_t first = s.suspended;
    void *const first_context = s.suspended_ctx;
    first(XIAOTAI_OK, first_context);
    remote();
    assert(s.held_signaling && status().phase == XIAOTAI_AI_TALK_ENDING);
    s.media.event(s.id, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    assert(!s.held_signaling && s.signaling_releases == 1 && !s.media_active);

    ready(0);
    uint32_t hold = s.held_signaling;
    first(XIAOTAI_FAIL, first_context); /* A stale callback must not release the new session's hold. */
    assert(hold && s.held_signaling == hold);
    s.allow_failed_suspension_release = true;
    s.suspended(XIAOTAI_FAIL, s.suspended_ctx);
    s.allow_failed_suspension_release = false;
    assert(!s.held_signaling && s.signaling_releases == 2 && status().phase == XIAOTAI_AI_TALK_LISTENING);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK && s.signaling_releases == 2);

    s.reject_hold = true;
    ready(0);
    assert(!s.held_signaling && status().phase == XIAOTAI_AI_TALK_LISTENING);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
    s.reject_hold = false;
    ready(0);
    s.handlers.closed(NULL, s.id, XIAOTAI_LINK_END_NETWORK, -1);
    assert(!s.held_signaling && s.signaling_releases == 3 && status().end == XIAOTAI_AI_TALK_END_NETWORK);

    ready(0);
    assert(s.held_signaling && xiaotai_ai_talk_stop(0) == XIAOTAI_OK);
    s.suspended(XIAOTAI_OK, s.suspended_ctx); /* Local cancellation preceded the acknowledgement. */
    assert(!s.held_signaling && s.signaling_releases == 4);

    ready(0);
    assert(s.held_signaling && xiaotai_activity_begin(&call, &owner) == XIAOTAI_OK);
    assert(!s.held_signaling && s.signaling_releases == 5 && xiaotai_activity_current(NULL) == owner);
    xiaotai_activity_end(owner);

    ready(0); /* Close the owner before the async acknowledgement is delivered. */
    xiaotai_ai_talk_reset();
    assert(!s.held_signaling && s.signaling_releases == 6 && !s.media_active);
}

static void check_system_end(void *ctx) { (void)ctx; assert(status().end == XIAOTAI_AI_TALK_END_SYSTEM); }

static void stay_armed(void *ctx) { (void)ctx; }

static void deferred_begin(void *ctx)
{
    const char *scenario = ctx;
    ready(0);
    assert(s.held_signaling && s.watched_link == s.id);
    if (!strcmp(scenario, "signaling_hold_ready")) s.release_state = XIAOTAI_LINK_RELEASE_COMPLETE;
    if (!strcmp(scenario, "signaling_hold_timer_error")) {
        uint32_t timer;
        while (xiaotai_timer_start(30000U, stay_armed, NULL, &timer) == XIAOTAI_OK) {}
    }
    remote();
    assert(s.held_signaling && status().phase == XIAOTAI_AI_TALK_ENDING && status().link == s.id);
    s.media.event(s.id, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    if (!strcmp(scenario, "signaling_hold_ready")) {
        assert(!s.held_signaling && !s.watched_link && s.signaling_releases == 1);
    } else if (!strcmp(scenario, "signaling_hold_timer_error")) {
        assert(s.held_signaling && !s.watched_link && !s.signaling_releases);
    } else {
        assert(s.held_signaling && s.watched_link == s.id && !s.signaling_releases);
        if (!strcmp(scenario, "signaling_hold_expired")) s.held_signaling = 0; /* Platform expires on its own. */
        if (!strcmp(scenario, "signaling_hold_reused")) s.suspended_ctx = &s; /* Same slot ID, new owning session. */
        if (!strcmp(scenario, "signaling_hold_record")) s.release_state = XIAOTAI_LINK_RELEASE_COMPLETE;
        if (!strcmp(scenario, "signaling_hold_offline")) { s.online = false; s.held_signaling = 0; }
        if (!strcmp(scenario, "signaling_hold_reset")) xiaotai_ai_talk_reset();
        if (!strcmp(scenario, "signaling_hold_transport_stopped")) s.release_state = XIAOTAI_LINK_RELEASE_ABORTED;
    }
}

static void deferred_overdue(void *ctx)
{
    (void)ctx;
    assert(s.held_signaling && s.watched_link == s.id && !s.signaling_releases);
    s.release_state = XIAOTAI_LINK_RELEASE_COMPLETE;
}

static void deferred_check(void *ctx)
{
    const char *scenario = ctx;
    const bool preserved = !strcmp(scenario, "signaling_hold_timer_error") || !strcmp(scenario, "signaling_hold_reset") ||
                           !strcmp(scenario, "signaling_hold_reused");
    assert((s.held_signaling != 0) == preserved && !s.watched_link);
    const bool self_dropped = !strcmp(scenario, "signaling_hold_expired") || !strcmp(scenario, "signaling_hold_offline");
    assert(s.signaling_releases == (!preserved && !self_dropped ? 1U : 0U));
    if (preserved) s.held_signaling = 0; /* Independent bounded expiry/platform shutdown belongs to the platform. */
}

static void check_network_end(void *ctx) { (void)ctx; assert(status().end == XIAOTAI_AI_TALK_END_NETWORK); }

static void test_preroll(void *ctx)
{
    (void)ctx;
    start(21);
    assert(xiaotai_ai_talk_preroll_lost(22) == XIAOTAI_ERR_INVALID_STATE && status().stale_calls == 1);
    assert(xiaotai_ai_talk_preroll_lost(21) == XIAOTAI_OK);
    assert(s.last_token == 21 && status().end == XIAOTAI_AI_TALK_END_AUDIO);
    start(30);
    open_link();
    assert(s.media.preroll_token == 30);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK && s.last_token == 30 && strstr(s.calls, "CDU"));
    ready(40);
    assert(xiaotai_ai_talk_preroll_lost(40) == XIAOTAI_ERR_INVALID_STATE && s.releases == 2);
    s.media.event(s.id, XIAOTAI_MEDIA_EVENT_PREROLL_LOST, XIAOTAI_ERR_INVALID_STATE, NULL);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING);
    assert(xiaotai_ai_talk_stop(0) == XIAOTAI_OK && s.releases == 2);
}

static void test_dropped(void *ctx)
{
    (void)ctx;
    start(0);
    open_link();
    input(s.id, AI_TALK_COMMAND, NULL, true);
    ai_talk_diag_t diag;
    ai_talk_diag(&diag);
    assert(diag.commands_dropped == 1 && status().phase == XIAOTAI_AI_TALK_CONNECTING);
}

static void check_timeout(void *ctx) { (void)ctx; assert(status().end == XIAOTAI_AI_TALK_END_TIMEOUT); }

static void dropped_while_running_then_peer_close(void *ctx)
{
    (void)ctx;
    ready(0);
    ai_talk_diag_t previous, after;
    ai_talk_diag(&previous);
    const uint32_t link = s.id;
    const uint32_t completed = status().completed;
    const uint32_t failed = status().failed;
    const size_t effects = strlen(s.calls);
    input(link, AI_TALK_COMMAND, NULL, true);
    ai_talk_diag(&after);
    assert(after.commands_dropped == previous.commands_dropped + 1);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && status().link == link && s.media_active);
    assert(strlen(s.calls) == effects && status().completed == completed && status().failed == failed);
    s.handlers.closed(s.handlers.ctx, link, XIAOTAI_LINK_END_PEER, 0);
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && !status().link && s.media_active);
    s.media.event(link, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    assert(status().phase == XIAOTAI_AI_TALK_IDLE && !s.media_active);
    assert(status().end == XIAOTAI_AI_TALK_END_REMOTE && status().completed == completed + 1 &&
           status().failed == failed && !xiaotai_activity_current(NULL));
}

static void test_contrib(void *ctx)
{
    const bool enabled = *(const bool *)ctx;
    start(0);
    grant();
    assert(s.request.profile.video.send == enabled);
    if (enabled) {
        assert(s.request.profile.video.send_stream == VIDEO.send_stream);
        assert(s.request.profile.video.send_format.codec == VIDEO.send_format.codec);
    }
    ai_talk_diag_t diag;
    ai_talk_diag(&diag);
    assert(diag.video_extra == (enabled ? 1 : 0));
}

static void test_config(void *ctx)
{
    (void)ctx;
    ready(0);
    assert(s.media.flags & XIAOTAI_MEDIA_SESSION_TRACE_ARRIVALS);
    assert(s.requirement.latency.start_ms == 152 && s.media.latency.start_ms == 152);
    assert(s.transport.command_pool_bytes == 12288 && s.request.profile.command_pool_bytes == 12288);
    assert(s.request.profile.command_max_bytes == 8192 && s.request.timeout_ms == 5432);
    assert(s.request.profile.audio.receive_request && s.request.profile.audio.receive_stream == 1);
    const uint32_t link = s.id, sent = s.commands, completed = status().completed;
    const size_t before = strlen(s.calls);
    input(link + 1U, AI_TALK_COMMAND, "{\"method\":\"end_session\"}", false);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING && strlen(s.calls) == before);
    remote();
    input(link, AI_TALK_COMMAND, "{\"method\":\"end_session\"}", false);
    assert(status().phase == XIAOTAI_AI_TALK_ENDING && s.media_active);
    s.media.event(link, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_OK, NULL);
    assert(status().end == XIAOTAI_AI_TALK_END_REMOTE && status().phase == XIAOTAI_AI_TALK_IDLE);
    assert(!strcmp(s.calls + before, "ZTCD") && !s.media_active && s.commands == sent &&
           status().completed == completed + 1 && !xiaotai_activity_current(NULL));
    ready(0);
    platform_event(XIAOTAI_PLATFORM_OFFLINE_REFRESH);
}

static void test_policy(void *ctx)
{
    (void)ctx;
    s.profile = false;
    assert(xiaotai_ai_talk_start(NULL, NULL) == XIAOTAI_ERR_PLATFORM_OFFLINE);
    s.profile = true;
    start(0);
    token_reply(200, "{\"code\":200,\"data\":{\"peer_id\":\"test-peer\",\"token\":\"test-token\"}}");
    assert(status().end == XIAOTAI_AI_TALK_END_REFUSED && s.id == 0);
    ready(0);
    input(s.id, AI_TALK_COMMAND, NULL, true);
    assert(status().end == XIAOTAI_AI_TALK_END_PROTOCOL && status().error == XIAOTAI_ERR_INVALID_SIZE);
    assert(strstr(s.sent, "\"end_session\"") && strstr(s.calls, "ZSCD"));
}

static void test_tools(void *ctx)
{
    (void)ctx;
    ready(0);
    assert(xiaotai_ai_talk_tool_register("too_late", capture_tool, NULL) == XIAOTAI_ERR_INVALID_STATE);
    const uint32_t stale_commands = status().stale_commands;
    input(s.id + 1U, AI_TALK_COMMAND, "{\"method\":\"notice\"}", false);
    input(s.id, AI_TALK_COMMAND, "{\"method\":", false);
    assert(tool.calls == 0 && status().stale_commands == stale_commands + 2);
    input(s.id, AI_TALK_COMMAND, "{\"id\":17,\"method\":\"device_action\",\"params\":{\"action\":\"unknown\"}}", false);
    assert(strstr(s.sent, "\"id\":17,") && strstr(s.sent, "\"code\":-32601"));
    input(s.id, AI_TALK_COMMAND, "{\"id\":\"x\",\"method\":\"device_action\",\"params\":{}}", false);
    assert(strstr(s.sent, "\"id\":\"x\"") && strstr(s.sent, "\"code\":-32602"));

    const uint32_t commands = s.commands;
    input(s.id, AI_TALK_COMMAND, "{\"id\":18,\"method\":\"notice\",\"params\":{\"payload\":{\"a\":1}}}", false);
    assert(tool.calls == 1 && !tool.request && !strcmp(tool.args, "{\"a\":1}") && s.commands == commands);
    static const char lookup[] = "{\"id\":\"x\\u0031\",\"method\":\"device_action\","
                                 "\"params\":{\"arguments\":{\"tool\":\"lookup\",\"query\":\"name\"}}}";
    input(s.id, AI_TALK_COMMAND, lookup, false);
    assert(tool.calls == 2 && tool.request && !strcmp(tool.args, "{\"tool\":\"lookup\",\"query\":\"name\"}"));
    const uint32_t session = tool.session, first = tool.request;
    input(s.id, AI_TALK_COMMAND, lookup, false);
    assert(tool.calls == 2 && s.commands == commands);
    /* Names of tools that ran, newest first; unknown ones leave no trace. */
    assert(!strcmp(ai_talk_tools_recent(0), "lookup") && !strcmp(ai_talk_tools_recent(1), "notice"));
    assert(!ai_talk_tools_recent(2) && !ai_talk_tools_recent(4));
    assert(xiaotai_ai_talk_tool_reply(session, first, "[]") == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_ai_talk_tool_reply(session, first, "{}{}") == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_ai_talk_tool_fail(session, first, -6, "bad message", "{}") == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_ai_talk_tool_reply(session, first, "{\"ready\":true}") == XIAOTAI_OK);
    assert(strstr(s.sent, "\"id\":\"x\\u0031\"") && strstr(s.sent, "\"result\":{\"ready\":true}"));
    assert(xiaotai_ai_talk_tool_reply(session, first, NULL) == XIAOTAI_ERR_INVALID_STATE);

    uint32_t pending[4];
    for (unsigned i = 0; i < 5; ++i) {
        char request[100];
        snprintf(request, sizeof(request), "{\"id\":%u,\"method\":\"device_action\",\"params\":{\"name\":\"lookup\"}}", i);
        input(s.id, AI_TALK_COMMAND, request, false);
        if (i < 4) pending[i] = tool.request;
    }
    assert(tool.calls == 6 && strstr(s.sent, "\"id\":4") && strstr(s.sent, "\"message\":\"busy\""));
    assert(xiaotai_ai_talk_tool_fail(session, pending[0], -6, "not_found", "{\"count\":0}") == XIAOTAI_OK);
    assert(strstr(s.sent, "\"code\":-6") && strstr(s.sent, "\"data\":{\"count\":0}"));

    char result[XIAOTAI_AI_TALK_TOOL_REPLY_MAX + 1U];
    memset(result, ' ', sizeof(result));
    result[0] = '{';
    result[1] = '}';
    result[sizeof(result) - 1U] = '\0';
    assert(xiaotai_ai_talk_tool_reply(session, pending[1], result) == XIAOTAI_ERR_INVALID_SIZE);
    result[sizeof(result) - 2U] = '\0';
    assert(xiaotai_ai_talk_tool_reply(session, pending[1], result) == XIAOTAI_OK);
    assert(xiaotai_ai_talk_stop(session) == XIAOTAI_OK);
    s.calls[0] = '\0';
    ready(0);
    input(s.id, AI_TALK_COMMAND, lookup, false);
    assert(tool.session != session && tool.request != pending[2]);
    const uint32_t sent = s.commands, stale = status().stale_calls;
    assert(xiaotai_ai_talk_tool_reply(session, pending[2], "{}") == XIAOTAI_ERR_INVALID_STATE);
    assert(s.commands == sent && status().stale_calls == stale + 1 && status().tool_unhandled == 3);
    s.send_error = XIAOTAI_FAIL;
    assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, NULL) == XIAOTAI_FAIL);
    assert(status().phase == XIAOTAI_AI_TALK_LISTENING);
    assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, NULL) == XIAOTAI_ERR_INVALID_STATE);
    s.send_error = XIAOTAI_OK;
    assert(xiaotai_ai_talk_stop(tool.session) == XIAOTAI_OK);
}

static void assert_action_reply_resources(void);

static void test_tools_compat(void *unused)
{
    (void)unused;
    ready(0);
    input(s.id, AI_TALK_COMMAND,
          "{\"id\":\"旧\\u0022\",\"method\":\"device_action\",\"params\":{\"action\":\"unknown\"}}", false);
    assert(!strcmp(s.sent, "{\"jsonrpc\":\"2.0\",\"id\":\"旧\\u0022\",\"error\":{\"code\":-32601,"
                           "\"message\":\"未知 \\\"动作\\\"\\\\\",\"data\":{\"status\":\"unsupported_action\",\"count\":\"0\"}}}"));
    const uint32_t sent = s.commands;
    input(s.id, AI_TALK_COMMAND, "{\"id\":99,\"method\":\"unknown\"}", false);
    assert(s.commands == sent);
    const char *const wraps[] = {"data", "arguments", "args", "input", "payload", "parameters"};
    for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); ++i) {
        char source[180];
        snprintf(source, sizeof(source),
                 "{\"id\":42,\"method\":\"device_action\",\"params\":{\"action\":\"lookup\",\"%s\":{\"target\":\"one\"}}}",
                 wraps[i]);
        input(s.id, AI_TALK_COMMAND, source, false);
        assert(tool.calls == i + 1U && !strcmp(tool.args, "{\"target\":\"one\"}"));
        assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, "{}") == XIAOTAI_OK);
    }
    assert_action_reply_resources();
}

static char test_command[XIAOTAI_LINK_COMMAND_LIMIT_BYTES + 2U];
static const size_t request_overhead =
    sizeof("{\"id\":,\"method\":\"device_action\",\"params\":{\"action\":\"lookup\"}}") - 1U;

static void tool_command(const char *id, const char *action)
{
    const int bytes = snprintf(test_command, sizeof(test_command), "{\"id\":%s,\"method\":\"device_action\","
                               "\"params\":{\"action\":\"%s\"}}", id, action);
    assert(bytes > 0 && (size_t)bytes < sizeof(test_command));
    input(s.id, AI_TALK_COMMAND, test_command, false);
    memset(test_command, '?', (size_t)bytes); /* transport's borrowed data is gone when the handler returns */
}

static void make_string_id(char *id, size_t wire_size, char last)
{
    assert(wire_size >= 3U);
    memset(id, 'x', wire_size);
    id[0] = id[wire_size - 1U] = '"';
    id[wire_size - 2U] = last;
    id[wire_size] = '\0';
}

static void expect_id(const char *wire_id)
{
    xiaotai_json_token_t tokens[32];
    xiaotai_json_doc_t doc;
    size_t count, size;
    assert(xiaotai_json_parse(s.sent, strlen(s.sent), tokens, 32, &count, &doc) == XIAOTAI_OK);
    const char *raw = xiaotai_json_raw(&doc, xiaotai_json_get(&doc, 0, "id"), &size);
    assert(raw);
    if (wire_id[0] == '"') {
        --raw;
        size += 2U;
    }
    assert(size == strlen(wire_id) && !memcmp(raw, wire_id, size));
}

static const char wire_args[] = "{\"t\\u006fol\":\"look\\u0075p\", \"query\" : [1, 2]}";
static const char wire_id_escaped[] = "\"w\\u0069re\\u0022\"";

static void padded_tool(size_t tokens, const char *method_fields)
{
    char members[256];
    const int bytes = snprintf(members, sizeof(members),
                               "%s,\"id\":%s,\"params\":{\"data\":null,\"arguments\":%s}",
                               method_fields, wire_id_escaped, wire_args);
    assert(bytes > 0 && (size_t)bytes < sizeof(members));
    size_t count = 0;
    int size = snprintf(test_command, sizeof(test_command), "{\"pad\":[],%s}", members);
    assert(size > 0 && (size_t)size < sizeof(test_command));
    assert(xiaotai_json_parse(test_command, (size_t)size, NULL, 0, &count, NULL) == XIAOTAI_OK);
    assert(tokens >= count);
    const size_t padding = tokens - count;
    size_t written = sizeof("{\"pad\":[") - 1U;
    for (size_t i = 0; i < padding; ++i) {
        if (i) test_command[written++] = ',';
        test_command[written++] = '0';
    }
    size = snprintf(test_command + written, sizeof(test_command) - written, "],%s}", members);
    assert(size > 0 && (size_t)size < sizeof(test_command) - written);
    assert(xiaotai_json_parse(test_command, written + (size_t)size, NULL, 0, &count, NULL) == XIAOTAI_OK);
    assert(count == tokens);
    input(s.id, AI_TALK_COMMAND, test_command, false);
    memset(test_command, '?', written + (size_t)size); /* Inbound bytes are borrowed. */
}

static void test_json_wire(void *ctx)
{
    (void)ctx;
    ready(0);
    const size_t limit = external_memory ? 128U : 64U;
    const uint32_t stale = status().stale_commands, sent = s.commands;
    static const char action_first[] =
        "\"m\\u0065thod\":\"device_\\u0061ction\",\"method\":null";
    static const char null_first[] =
        "\"m\\u0065thod\":null,\"method\":\"device_action\"";

    padded_tool(limit, null_first);
    assert(status().stale_commands == stale + 1U && tool.calls == 0 && s.commands == sent);
    padded_tool(limit + 1U, action_first);
    assert(status().stale_commands == stale + 2U && tool.calls == 0 && s.commands == sent);
    padded_tool(limit, action_first);
    assert(status().stale_commands == stale + 2U && tool.calls == 1 && s.commands == sent);
    assert(tool.request && strcmp(tool.args, wire_args) == 0);
    assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, "{\"accepted\":true}") == XIAOTAI_OK);
    expect_id(wire_id_escaped);
    assert(s.commands == sent + 1U && strstr(s.sent, "\"result\":{\"accepted\":true}"));
}

static void make_result(char *out, size_t length)
{
    assert(length >= 2U && length < XIAOTAI_AI_TALK_TOOL_REPLY_MAX);
    out[0] = '{';
    out[1] = '}';
    memset(out + 2U, ' ', length - 2U);
    out[length] = '\0';
}

static uint32_t reply_failures(void)
{
    ai_talk_diag_t diag;
    ai_talk_diag(&diag);
    return diag.tool_reply_failures;
}

static void assert_action_reply_resources(void)
{
    const uint32_t sent = s.commands, session = status().session, failures = reply_failures();
    const unsigned calls = tool.calls;
    const int64_t live = xiaotai_mem_host_outstanding();
    const char result[] = "{\"ok\":false,\"status\":\"busy\",\"message\":\"not available\"}";
    input(s.id, AI_TALK_COMMAND, "{\"method\":\"device_action\",\"params\":{\"action\":\"lookup\"}}", false);
    assert(tool.calls == calls + 1U && !tool.request && s.commands == sent && xiaotai_mem_host_outstanding() == live);

    tool_command("\"request-7\"", "lookup");
    assert(tool.calls == calls + 2U && tool.request);
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_ai_talk_tool_reply(session, tool.request, result) == XIAOTAI_ERR_NO_MEM);
    assert(!xiaotai_mem_host_fail_pending() && s.commands == sent && reply_failures() == failures + 1U &&
           xiaotai_mem_host_outstanding() == live && status().session == session);
    static const char accepted[] = "{\"ok\":true,\"status\":\"ok\",\"message\":\"completed\"}";
    assert(xiaotai_ai_talk_tool_reply(session, tool.request, accepted) == XIAOTAI_OK);
    expect_id("\"request-7\"");
    assert(strstr(s.sent, accepted));

    tool_command("42", "lookup");
    s.send_error = XIAOTAI_FAIL;
    assert(xiaotai_ai_talk_tool_reply(session, tool.request, result) == XIAOTAI_FAIL);
    expect_id("42");
    assert(strstr(s.sent, result) && s.commands == sent + 2U && reply_failures() == failures + 2U &&
           xiaotai_mem_host_outstanding() == live && status().session == session);
    assert(xiaotai_ai_talk_tool_reply(session, tool.request, result) == XIAOTAI_ERR_INVALID_STATE);
}

static void test_long_tools(void *ctx)
{
    (void)ctx;
    ready(0);
    assert(s.request.profile.command_max_bytes == 4096U);
    const uint32_t session = status().session;
    const int64_t before = xiaotai_mem_host_outstanding();
    static char id[4097], result[XIAOTAI_AI_TALK_TOOL_REPLY_MAX];
    for (size_t n = 96U; n <= 97U; ++n) {
        make_string_id(id, n, 'x');
        const unsigned calls = tool.calls;
        tool_command(id, "lookup");
        assert(tool.calls == calls + 1U && tool.request);
        assert(xiaotai_mem_host_outstanding() == before + (n > 96U));
        assert(xiaotai_ai_talk_tool_reply(session, tool.request, "{}") == XIAOTAI_OK);
        expect_id(id);
        assert(xiaotai_mem_host_outstanding() == before);
    }

    id[0] = '"';
    for (size_t n = 0; n < 128U; ++n) memcpy(id + 1U + n * 6U, "\\u0041", 6U);
    memcpy(id + 769U, "\"", 2U); /* 128 decoded bytes, 770 wire bytes */
    assert(strlen(id) == 770U);
    tool_command(id, "lookup");
    assert(xiaotai_ai_talk_tool_reply(session, tool.request, NULL) == XIAOTAI_OK);
    expect_id(id);
    memset(id, '9', 150U);
    id[150U] = '\0';
    tool_command(id, "lookup");
    assert(xiaotai_ai_talk_tool_reply(session, tool.request, NULL) == XIAOTAI_OK);
    expect_id(id);

    make_string_id(id, s.request.profile.command_max_bytes - request_overhead, 'z');
    const unsigned original_calls = tool.calls, original_tx = s.commands;
    xiaotai_mem_host_fail_nth(2); /* arguments exist, but the owned ID could not be copied */
    tool_command(id, "lookup");
    assert(!xiaotai_mem_host_fail_pending() && tool.calls == original_calls);
    assert(s.commands == original_tx + 1U && strstr(s.sent, "\"message\":\"busy\""));
    expect_id(id);
    assert(xiaotai_mem_host_outstanding() == before);
    tool_command(id, "lookup");
    const uint32_t pending = tool.request;
    make_result(result, sizeof(result) - 1U);
    xiaotai_mem_host_fail_nth(1); /* the long output scratch could not be allocated */
    const unsigned before_reply = s.commands;
    assert(xiaotai_ai_talk_tool_reply(session, pending, result) == XIAOTAI_ERR_NO_MEM);
    assert(s.commands == before_reply && reply_failures() == 1U && xiaotai_mem_host_outstanding() == before + 1);
    assert(xiaotai_ai_talk_tool_reply(session, pending, result) == XIAOTAI_OK);
    expect_id(id);
    assert(strlen(s.sent) == strlen(id) + strlen(result) + 33U);
    assert(xiaotai_ai_talk_tool_reply(session, pending, result) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mem_host_outstanding() == before);

    uint32_t slots[4];
    for (unsigned n = 0; n < 4U; ++n) {
        make_string_id(id, 700U, (char)('0' + n));
        tool_command(id, "lookup");
        slots[n] = tool.request;
    }
    const unsigned calls = tool.calls;
    make_string_id(id, 700U, '0');
    tool_command(id, "lookup");
    assert(tool.calls == calls && s.commands == before_reply + 1U);
    make_string_id(id, 700U, '4');
    tool_command(id, "lookup");
    expect_id(id);
    assert(tool.calls == calls && strstr(s.sent, "\"message\":\"busy\""));
    assert(xiaotai_mem_host_outstanding() == before + 4);
    char word[XIAOTAI_AI_TALK_TOOL_NAME_MAX];
    memset(word, 'x', sizeof(word) - 1U);
    word[sizeof(word) - 1U] = '\0';
    assert(xiaotai_ai_talk_tool_fail(session, slots[3], INT32_MIN, word, "{}") == XIAOTAI_OK);
    make_string_id(id, 700U, '3');
    expect_id(id);
    assert(strstr(s.sent, "\"code\":-2147483648") && strstr(s.sent, word));
    s.send_error = XIAOTAI_FAIL;
    assert(xiaotai_ai_talk_tool_reply(session, slots[1], NULL) == XIAOTAI_FAIL);
    assert(xiaotai_ai_talk_tool_reply(session, slots[1], NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(reply_failures() == 2U);
    s.send_error = XIAOTAI_OK;
    assert(xiaotai_mem_host_outstanding() == before + 2);
    assert(xiaotai_ai_talk_stop(session) == XIAOTAI_OK && xiaotai_mem_host_outstanding() == before);
    s.calls[0] = '\0';
    ready(0);
    assert(xiaotai_ai_talk_tool_reply(session, slots[0], NULL) == XIAOTAI_ERR_INVALID_STATE);
}

static void test_long_unknown(void *ctx)
{
    (void)ctx;
    ready(0);
    static char id[12289];
    assert(s.request.profile.command_max_bytes == 12288U);
    make_string_id(id, s.request.profile.command_max_bytes - request_overhead, 'u');
    const uint32_t before = s.commands;
    const int64_t outstanding = xiaotai_mem_host_outstanding();
    const uint32_t attempts = xiaotai_mem_host_allocations();
    xiaotai_mem_host_fail_nth(1);
    tool_command(id, "lookup");
    assert(tool.calls == 0 && s.commands == before && reply_failures() == 1U);
    assert(!xiaotai_mem_host_fail_pending() && xiaotai_mem_host_outstanding() == outstanding);
    tool_command(id, "lookup");
    assert(tool.calls == 0 && s.commands == before + 1U);
    expect_id(id);
    assert(strstr(s.sent, "\"code\":-32601"));
    assert(xiaotai_mem_host_allocations() == attempts + 2U && xiaotai_mem_host_outstanding() == outstanding);
    make_string_id(id, s.request.profile.command_max_bytes - request_overhead + 1U, 'u');
    tool_command(id, "lookup");
    ai_talk_diag_t diag;
    ai_talk_diag(&diag);
    assert(s.commands == before + 1U && diag.commands_dropped == 1U);
}

static void test_external_reply(void *ctx)
{
    (void)ctx;
    ready(0);
    char id[281], result[XIAOTAI_AI_TALK_TOOL_REPLY_MAX];
    make_string_id(id, 280U, 'e');
    const size_t fits = 4352U - 1U - strlen(id) - 33U;
    tool_command(id, "lookup");
    const uint32_t before = xiaotai_mem_host_allocations();
    make_result(result, fits + 1U);
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, result) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_mem_host_allocations() == before + 1U && !xiaotai_mem_host_fail_pending());
    make_result(result, fits);
    assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, result) == XIAOTAI_OK);
    assert(xiaotai_mem_host_allocations() == before + 1U && strlen(s.sent) == 4351U);
    expect_id(id);
}

static void test_outbound_limit(void *ctx)
{
    (void)ctx;
    ready(0);
    static char id[XIAOTAI_LINK_COMMAND_LIMIT_BYTES + 1U];
    char result[XIAOTAI_AI_TALK_TOOL_REPLY_MAX];
    const size_t limit = XIAOTAI_LINK_COMMAND_LIMIT_BYTES;
    assert(s.request.profile.command_max_bytes == limit);
    make_string_id(id, limit - request_overhead, 't');
    tool_command(id, "lookup");
    const size_t fits = limit - strlen(id) - 33U;
    make_result(result, fits + 1U);
    const uint32_t before = xiaotai_mem_host_allocations(), sent = s.commands;
    assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, result) == XIAOTAI_ERR_INVALID_SIZE);
    assert(reply_failures() == 1U && s.commands == sent && xiaotai_mem_host_allocations() == before);
    make_result(result, fits);
    assert(xiaotai_ai_talk_tool_reply(tool.session, tool.request, result) == XIAOTAI_OK);
    assert(strlen(s.sent) == limit);
    expect_id(id);
}

static void test_tool_scope(void *ctx)
{
    switch ((uintptr_t)ctx) {
    case 0:
        ready(0);
        assert(xiaotai_app_switch("other_app") == XIAOTAI_OK);
        tool_command("\"background\"", "lookup");
        assert(scope_tool_calls == 1 && application_views == 0);
        break;
    case 1:
        assert(deferred_tool_calls == 1 && application_views == 0);
        assert(xiaotai_app_switch("tool_app") == XIAOTAI_OK);
        tool_command("\"foreground\"", "lookup");
        assert(scope_tool_calls == 2 && application_views == 1);
        break;
    case 2:
        assert(deferred_tool_calls == 2 && application_views == 2);
        /* A tool genuinely registered by a module remains module-owned. */
        assert(xiaotai_app_switch("other_app") == XIAOTAI_OK);
        tool_command("\"module\"", "module_lookup");
        assert(scope_tool_calls == 3 && application_views == 3);
        break;
    default:
        assert(deferred_tool_calls == 3 && application_views == 4);
        break;
    }
}

static void capture_line(void *ctx, const char *text)
{
    char *output = ctx;
    const size_t at = strlen(output), size = strlen(text);
    assert(at + size + 2U < 1024);
    memcpy(output + at, text, size);
    output[at + size] = '\n';
    output[at + size + 1U] = '\0';
}

static void at_accept(void *ctx)
{
    (void)ctx;
    open_link();
    accept(8000, false);
}

static void at_caption(void *ctx)
{
    caption_notice(s.id, ctx);
}

static void at_diag(void *ctx)
{
    xiaotai_diag_report("ai_talk", ctx);
}

static void test_at(void)
{
    char output[1024] = {0};
    const xiaotai_reply_t reply = {.write = capture_line, .ctx = output};
    assert(xiaotai_command_run("AT+AI", &reply, 1000) == XIAOTAI_OK);
    assert(!strcmp(output, "+AI: phase=idle gen=0 link=0 origin=user end=none caption=none"
                           " revision=0 paragraph=0 final=0 truncated=0 emotion=- emotions=0\n"));
    assert(xiaotai_command_run("AT+AI STOP", &reply, 1000) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_command_run("AT+AI START extra", &reply, 1000) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_command_run("AT+AI START", &reply, 1000) == XIAOTAI_OK);
    sync_owner(at_accept, NULL);
    output[0] = '\0';
    assert(xiaotai_command_run("AT+AI", &reply, 1000) == XIAOTAI_OK);
    assert(strstr(output, "phase=listening") && strstr(output, "caption=none revision=0 paragraph=0 final=0 truncated=0"));

    static char user_caption[] = "{\"caption_type\":0,\"text\":\"private-question\",\"utterance_id\":1,"
                                 "\"seq_num\":1,\"is_final\":true}";
    sync_owner(at_caption, user_caption);
    output[0] = '\0';
    assert(xiaotai_command_run("AT+AI", &reply, 1000) == XIAOTAI_OK);
    assert(strstr(output, "phase=thinking") && strstr(output, "caption=user revision=1 paragraph=1 final=1 truncated=0"));
    assert(!strstr(output, "private-question"));

    static char ai_caption[] = "{\"caption_type\":1,\"text\":\"private-example "
                               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\",\"utterance_id\":2,\"seq_num\":1,\"emotion\":\"开 心\\r\"}";
    sync_owner(at_caption, ai_caption);
    output[0] = '\0';
    assert(xiaotai_command_run("AT+AI", &reply, 1000) == XIAOTAI_OK);
    assert(strstr(output, "phase=speaking") && strstr(output, "caption=ai revision=2 paragraph=2 final=0 truncated=1 emotion=开_心_ emotions=1"));
    assert(!strstr(output, "private-example"));
    assert(xiaotai_command_run("AT+AI INTERRUPT", &reply, 1000) == XIAOTAI_OK && strstr(s.sent, "interrupt"));
    assert(xiaotai_command_run("AT+AI SUBMIT", &reply, 1000) == XIAOTAI_OK && strstr(s.sent, "submit_speech"));
    assert(xiaotai_command_run("AT+AI STOP", &reply, 1000) == XIAOTAI_OK && strstr(s.sent, "end_session"));
    output[0] = '\0';
    sync_owner(at_diag, (void *)&reply);
    assert(strstr(output, "phase=idle") && strstr(output, "end=local") && strstr(output, "completed=1"));
    assert(strstr(output, "+AI-TOOLS: recent=-,-,-,-"));
    assert(!strstr(output, "private-example") && !strstr(output, "test-token"));
}

int main(int argc, char **argv)
{
#ifdef AI_TALK_POLICY_TEST
    const char *scenario = argc > 1 ? argv[1] : "policy";
#else
    const char *scenario = argc > 1 ? argv[1] : "start";
#endif
    assert(xiaotai_signal_init(&reported) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&finished) == XIAOTAI_OK);
    if (!strcmp(scenario, "contrib") || !strcmp(scenario, "contrib_empty")) {
        /* The POSIX port intentionally retains exited tasks' notification handles. */
        bool contribute = !strcmp(scenario, "contrib");
        boot(NULL, contribute, false);
        sync_owner(test_contrib, &contribute);
        shutdown();
    } else if (!strcmp(scenario, "signaling_hold_off") || !strcmp(scenario, "signaling_hold_on")) {
        const bool enabled = !strcmp(scenario, "signaling_hold_on");
        const xiaotai_ai_talk_config_t config = {.pause_platform_signaling_while_active = enabled};
        boot(enabled ? &config : NULL, false, false);
        sync_owner(test_signaling_hold, (void *)&enabled);
        shutdown();
    } else if (!strncmp(scenario, "signaling_hold_", sizeof("signaling_hold_") - 1U)) {
        const xiaotai_ai_talk_config_t bad = {.resume_signaling_after_link_release = true};
        assert(xiaotai_ai_talk_configure(&bad) == XIAOTAI_ERR_INVALID_ARG);
        const xiaotai_ai_talk_config_t config = {.pause_platform_signaling_while_active = true,
                                                .resume_signaling_after_link_release = true};
        atomic_store(&test_time_us, INT64_C(1000000));
        boot(&config, false, false);
        sync_owner(deferred_begin, (void *)scenario);
        const bool overtime = !strcmp(scenario, "signaling_hold_timeout");
        atomic_fetch_add(&test_time_us, overtime ? INT64_C(2000000) : INT64_C(80000));
        sync_owner(report, NULL);
        if (overtime) {
            sync_owner(deferred_overdue, NULL);
            atomic_fetch_add(&test_time_us, INT64_C(250000));
            sync_owner(report, NULL);
        }
        sync_owner(deferred_check, (void *)scenario);
        shutdown();
    } else if (!strcmp(scenario, "config")) {
        const xiaotai_ai_talk_config_t config = {
            .request_downlink = true, .latency = {.start_ms = 152},
            .command_max_bytes = 8192, .command_pool_bytes = 12288, .connect_timeout_ms = 5432,
            .end_on_platform_offline = true, .trace_arrivals = true,
        };
        boot(&config, false, false);
        sync_owner(test_config, NULL);
        sync_owner(check_network_end, NULL);
        shutdown();
    } else if (!strcmp(scenario, "dropped")) {
        const xiaotai_ai_talk_config_t config = {.handshake_timeout_ms = 200};
        boot(&config, false, true);
        sync_owner(test_dropped, NULL);
        assert(xiaotai_signal_take(&finished, 3000));
        sync_owner(check_timeout, NULL);
        sync_owner(dropped_while_running_then_peer_close, NULL);
        shutdown();
    } else if (!strcmp(scenario, "start_context")) {
        boot(NULL, false, false);
        sync_owner(startup_resources_and_profiles, NULL);
        sync_owner(test_start_context, NULL);
        shutdown();
    } else if (!strcmp(scenario, "handshake_deadline")) {
        atomic_store(&test_time_us, INT64_C(1000000));
        startup_writer_boundary();
        boot(NULL, false, true);
        sync_owner(handshake_begin, NULL);
        atomic_fetch_add(&test_time_us, INT64_C(9999999));
        sync_owner(handshake_pending, NULL);
        atomic_fetch_add(&test_time_us, 1);
        assert(xiaotai_signal_take(&finished, 3000));
        sync_owner(handshake_expired, NULL);
        sync_owner(handshake_begin, NULL);
        atomic_fetch_add(&test_time_us, INT64_C(9999999));
        sync_owner(handshake_accept, NULL);
        atomic_fetch_add(&test_time_us, 1);
        sync_owner(handshake_not_expired, NULL);
        sync_owner(startup_send_failure, NULL);
        shutdown();
    } else if (!strcmp(scenario, "ending_deadline")) {
        atomic_store(&test_time_us, INT64_C(1000000));
        boot(NULL, false, true);
        for (unsigned left = 0; left < 2U; ++left) {
            bool silent = left != 0;
            sync_owner(ending_begin, &silent);
            atomic_fetch_add(&test_time_us, ENDING_US - 1);
            sync_owner(ending_pending, NULL);
            atomic_fetch_add(&test_time_us, 1);
            assert(xiaotai_signal_take(&finished, 3000));
            sync_owner(ending_expired, NULL);
        }
        /* The report came in time: nothing is left to end the next one. */
        bool silent = false;
        sync_owner(ending_begin, &silent);
        sync_owner(ending_drained_then_talk, NULL);
        atomic_fetch_add(&test_time_us, 2 * ENDING_US);
        sync_owner(report, NULL);
        sync_owner(ending_still_talking, NULL);
        shutdown();
    } else if (!strcmp(scenario, "ending_intents")) {
        static char start_intent[] = XIAOTAI_INTENT_TALK_START, toggle_intent[] = XIAOTAI_INTENT_TALK_TOGGLE;
        char *const both[] = {start_intent, toggle_intent};
        boot(NULL, false, false);
        for (size_t i = 0; i < sizeof(both) / sizeof(*both); ++i) {
            sync_owner(intent_in_farewell, both[i]);
            sync_owner(intent_started_next, NULL);
            sync_owner(intent_ignored, NULL);
            sync_owner(intent_stopped, NULL);
            sync_owner(intent_offline_in_farewell, both[i]);
            sync_owner(intent_refused, NULL);
        }
        sync_owner(intent_named_in_farewell, NULL);
        sync_owner(intent_stopped, NULL);
        shutdown();
    } else if (!strcmp(scenario, "caption")) {
        const xiaotai_ai_talk_config_t config = {.caption_bytes = 64};
        boot(&config, false, true);
        sync_owner(test_caption, NULL);
        sync_owner(check_caption_events, NULL);
        shutdown();
    } else if (!strcmp(scenario, "caption_capacity")) {
        const xiaotai_ai_talk_config_t config = {.caption_bytes = 64};
        boot(&config, false, false);
        sync_owner(test_caption_emotion, NULL);
        sync_owner(test_caption_words, NULL);
        sync_owner(test_caption_sentences, NULL);
        sync_owner(test_caption_capacity, NULL);
        shutdown();
    } else if (!strcmp(scenario, "tools_scope")) {
        register_tools = false;
        scope_scenario = true;
        boot(NULL, false, false);
        for (uintptr_t step = 0; step < 4U; ++step) sync_owner(test_tool_scope, (void *)step);
        shutdown();
    } else if (!strcmp(scenario, "tools_external")) {
        external_memory = true;
        boot(NULL, false, false);
        sync_owner(test_external_reply, NULL);
        shutdown();
    } else if (!strcmp(scenario, "tools_json") || !strcmp(scenario, "tools_json_external")) {
        external_memory = !strcmp(scenario, "tools_json_external");
        boot(NULL, false, false);
        sync_owner(test_json_wire, NULL);
        shutdown();
    } else if (!strcmp(scenario, "tools_outbound")) {
        const xiaotai_ai_talk_config_t config = {.command_max_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES,
                                               .command_pool_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES};
        boot(&config, false, false);
        sync_owner(test_outbound_limit, NULL);
        shutdown();
    } else if (!strcmp(scenario, "tools_xmini")) {
        const xiaotai_ai_talk_config_t config = {.command_max_bytes = 12288, .command_pool_bytes = 12288};
        register_tools = false;
        boot(&config, false, false);
        sync_owner(test_long_unknown, NULL);
        shutdown();
    } else if (!strcmp(scenario, "at")) {
        const xiaotai_ai_talk_config_t config = {.caption_bytes = 64};
        boot(&config, false, false);
        test_at();
        shutdown();
    } else {
        register_compat = !strcmp(scenario, "tools_compat");
        boot(NULL, false, false);
        if (!strcmp(scenario, "start")) sync_owner(test_start, NULL);
        else if (!strcmp(scenario, "token_matrix")) sync_owner(test_token_matrix, NULL);
        else if (!strcmp(scenario, "handshake")) sync_owner(test_handshake, NULL);
        else if (!strcmp(scenario, "stop_scope")) sync_owner(test_stop_scope, NULL);
        else if (!strcmp(scenario, "preroll")) sync_owner(test_preroll, NULL);
        else if (!strcmp(scenario, "policy")) sync_owner(test_policy, NULL);
        else if (!strcmp(scenario, "tools")) sync_owner(test_tools, NULL);
        else if (register_compat) sync_owner(test_tools_compat, NULL);
        else if (!strcmp(scenario, "tools_long")) sync_owner(test_long_tools, NULL);
        else {
            assert(!strcmp(scenario, "end"));
            sync_owner(test_end, NULL);
            sync_owner(end_binding, NULL);
            sync_owner(check_system_end, NULL);
        }
        shutdown();
    }
    xiaotai_signal_deinit(&reported);
    xiaotai_signal_deinit(&finished);
    return 0;
}
