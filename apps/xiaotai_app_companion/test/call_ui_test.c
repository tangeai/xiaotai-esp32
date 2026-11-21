// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
// Replaces legacy: tests/regression/test_p4_h264_feedback.py
#include "feedback_fakes.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/modules/stream_video.h"
#include "xiaotai/modules/video_call.h"
#include "xiaotai/modules/wechat.h"
#include "xiaotai/port/os.h"

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_call, xiaotai_app_companion_with_wechat;
extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_video_call, xiaotai_app_companion_with_stream_video;

static xiaotai_signal_t fence;
static atomic_int_fast64_t usec;
static xiaotai_call_status_t call;
static xiaotai_wechat_status_t wechat;
static xiaotai_video_call_status_t video;
static xiaotai_stream_status_t stream;
static xiaotai_stream_video_status_t stream_video;
static uint32_t activity, sequence, last_identity;
static unsigned accepted[2], rejected[2], hung_up[2], synced[2], saved[2], camera_calls, main_fallbacks;
static uint32_t authorization_dismissed;
static unsigned authorization_dismissals;
static xiaotai_err_t action_error;

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&usec); }

static xiaotai_err_t main_fallback(int32_t value)
{
    (void)value;
    ++main_fallbacks;
    return XIAOTAI_OK;
}

static const xiaotai_app_companion_part_t fallback = {.name = "main_fallback", .main = main_fallback};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &fallback, &xiaotai_app_companion_with_call, &xiaotai_app_companion_with_wechat,
    &xiaotai_app_companion_with_video_call, &xiaotai_app_companion_with_stream_video, NULL,
};

static void notified(void *unused) { (void)unused; xiaotai_signal_give(&fence); }
static void flush(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        assert(xiaotai_run(notified, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void run(void (*fn)(void *), void *ctx)
{
    assert(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    flush();
}

static void publish(const char *name, int32_t value, const void *status, size_t size)
{
    assert(xiaotai_event_emit(name, &(xiaotai_args_t){.value = value}, status, size) == XIAOTAI_OK);
}

static void intent(const char *name, const char *text, uint32_t identity)
{
    xiaotai_args_t args = {.value = (int32_t)identity};
    if (text) snprintf(args.text, sizeof(args.text), "%s", text);
    assert(xiaotai_intent_post(name, &args) == XIAOTAI_OK);
    flush();
}

static void device_event(const char *event) { publish(event, call.state, &call, sizeof(call)); }
static void wechat_event(const char *event) { publish(event, wechat.state, &wechat, sizeof(wechat)); }

void xiaotai_call_status(xiaotai_call_status_t *out) { assert(xiaotai_on_owner()); *out = call; }
void xiaotai_wechat_status(xiaotai_wechat_status_t *out) { assert(xiaotai_on_owner()); *out = wechat; }
void xiaotai_wechat_result_dismissed(uint32_t session)
{
    assert(xiaotai_on_owner() && session);
    authorization_dismissed = session;
    ++authorization_dismissals;
}
void xiaotai_video_call_status(xiaotai_video_call_status_t *out) { assert(xiaotai_on_owner()); *out = video; }
void xiaotai_stream_status(xiaotai_stream_status_t *out) { assert(xiaotai_on_owner()); *out = stream; }
void xiaotai_stream_video_status(xiaotai_stream_video_status_t *out) { assert(xiaotai_on_owner()); *out = stream_video; }

static xiaotai_err_t observe(unsigned *counter, uint32_t expected, uint32_t identity)
{
    assert(xiaotai_on_owner() && identity && identity == expected);
    ++*counter;
    last_identity = identity;
    return action_error;
}

xiaotai_err_t xiaotai_call_accept(uint32_t identity) { return observe(&accepted[0], call.session, identity); }
xiaotai_err_t xiaotai_call_reject(uint32_t identity) { return observe(&rejected[0], call.session, identity); }
xiaotai_err_t xiaotai_call_hangup(uint32_t identity) { return observe(&hung_up[0], call.session, identity); }
xiaotai_err_t xiaotai_wechat_accept(uint32_t identity) { return observe(&accepted[1], wechat.session, identity); }
xiaotai_err_t xiaotai_wechat_reject(uint32_t identity) { return observe(&rejected[1], wechat.session, identity); }
xiaotai_err_t xiaotai_wechat_hangup(uint32_t identity) { return observe(&hung_up[1], wechat.session, identity); }

xiaotai_err_t xiaotai_call_set_muted(uint32_t identity, bool muted)
{
    const xiaotai_err_t err = observe(&synced[0], call.session, identity);
    if (err == XIAOTAI_OK) { call.muted = muted; device_event(XIAOTAI_EVENT_CALL_STATE); }
    return err;
}

xiaotai_err_t xiaotai_wechat_set_muted(uint32_t identity, bool muted)
{
    const xiaotai_err_t err = observe(&synced[1], wechat.session, identity);
    if (err == XIAOTAI_OK) { wechat.muted = muted; wechat_event(XIAOTAI_EVENT_WECHAT_STATE); }
    return err;
}

xiaotai_err_t xiaotai_call_set_saved_mute(uint32_t identity, bool muted)
{
    const xiaotai_err_t err = observe(&saved[0], call.session, identity);
    if (err != XIAOTAI_OK) return err;
    call.muted = muted;
    device_event(XIAOTAI_EVENT_CALL_STATE);
    return xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, muted);
}

xiaotai_err_t xiaotai_settings_set_mic_mute(bool muted)
{
    assert(xiaotai_on_owner() && wechat.session);
    ++saved[1];
    wechat.muted = muted;
    wechat_event(XIAOTAI_EVENT_WECHAT_STATE);
    return xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, muted);
}

static xiaotai_err_t begin(const char *kind, uint32_t *id)
{
    return xiaotai_activity_begin(&(const xiaotai_activity_t){.name = kind}, id);
}

static xiaotai_err_t start_call(bool is_wechat, bool outgoing, bool use_video)
{
    uint32_t id;
    const xiaotai_err_t err = begin("call", &id);
    if (err != XIAOTAI_OK) return err;
    assert(!activity);
    activity = id;
    if (is_wechat) {
        wechat = (xiaotai_wechat_status_t){.state = outgoing ? XIAOTAI_WECHAT_OUTGOING : XIAOTAI_WECHAT_INCOMING,
            .session = ++sequence, .flags = use_video ? XIAOTAI_WECHAT_VIDEO : 0, .outgoing = outgoing,
            .peer_name = "WeChat peer"};
        wechat_event(outgoing ? XIAOTAI_EVENT_WECHAT_STATE : XIAOTAI_EVENT_WECHAT_INCOMING);
    } else {
        call = (xiaotai_call_status_t){.state = outgoing ? XIAOTAI_CALL_OUTGOING : XIAOTAI_CALL_INCOMING,
            .session = ++sequence, .flags = use_video ? XIAOTAI_CALL_VIDEO : 0, .outgoing = outgoing,
            .peer_name = "Device peer"};
        device_event(outgoing ? XIAOTAI_EVENT_CALL_STATE : XIAOTAI_EVENT_CALL_INCOMING);
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_call_dial(const char *id, uint32_t flags, uint32_t *session)
{
    assert(id && !session);
    return start_call(false, true, flags != 0);
}

xiaotai_err_t xiaotai_wechat_dial(const char *id, uint32_t flags, uint32_t *session)
{
    assert(id && !session);
    return start_call(true, true, flags != 0);
}

xiaotai_err_t xiaotai_video_call_set_camera(uint32_t link, bool enabled)
{
    assert(xiaotai_on_owner() && link && link == video.link && video.active);
    ++camera_calls;
    video.camera_enabled = enabled;
    publish(XIAOTAI_EVENT_VIDEO_CALL_STATE, enabled, &video, sizeof(video));
    return XIAOTAI_OK;
}

static void incoming(void *ctx) { assert(start_call(ctx != NULL, false, false) == XIAOTAI_OK); }
static void outgoing_video(void *unused)
{
    (void)unused;
    assert(start_call(false, true, true) == XIAOTAI_OK);
}

static void change_call(void *ctx)
{
    call.state = *(const xiaotai_call_state_t *)ctx;
    if (call.state == XIAOTAI_CALL_ACTIVE) {
        call.active_since_us = 1000000;
        call.link = 900;
    }
    device_event(XIAOTAI_EVENT_CALL_STATE);
}

static void change_wechat_active(void *unused)
{
    (void)unused;
    wechat.state = XIAOTAI_WECHAT_ACTIVE;
    wechat.active_since_us = 1000000;
    wechat_event(XIAOTAI_EVENT_WECHAT_CONNECTED);
}

static void finish_call(void *ctx)
{
    const bool failed = ctx != NULL;
    if (call.session) {
        call = (xiaotai_call_status_t){.end = failed ? XIAOTAI_CALL_END_NETWORK : XIAOTAI_CALL_END_LOCAL,
            .error = failed ? XIAOTAI_FAIL : XIAOTAI_OK, .releasing = true};
        device_event(XIAOTAI_EVENT_CALL_ENDED);
    } else {
        assert(wechat.session);
        wechat = (xiaotai_wechat_status_t){.end = failed ? XIAOTAI_WECHAT_END_NO_ANSWER : XIAOTAI_WECHAT_END_PEER_HANGUP,
            .peer_reason = failed ? -1 : 6, .releasing = true};
        wechat_event(XIAOTAI_EVENT_WECHAT_ENDED);
    }
}

static void release_call(void *unused)
{
    (void)unused;
    assert(activity);
    xiaotai_activity_end(activity);
    activity = 0;
    if (call.releasing) { call.releasing = false; device_event(XIAOTAI_EVENT_CALL_STATE); }
    if (wechat.releasing) { wechat.releasing = false; wechat_event(XIAOTAI_EVENT_WECHAT_STATE); }
}

static void end_call(bool failure)
{
    static const bool yes = true;
    run(finish_call, failure ? (void *)&yes : NULL);
    run(release_call, NULL);
}

static struct { const char *input; size_t left, used; char output[4096]; } wire;

static size_t at_read(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout)
{
    (void)ctx;
    if (!wire.left) {
        atomic_fetch_add(&usec, (int64_t)(timeout ? timeout : 1) * 1000);
        return 0;
    }
    const size_t n = wire.left < capacity ? wire.left : capacity;
    memcpy(data, wire.input, n);
    wire.input += n;
    wire.left -= n;
    return n;
}

static xiaotai_err_t at_write(void *ctx, const char *data, size_t length)
{
    (void)ctx;
    assert(wire.used + length < sizeof(wire.output));
    memcpy(wire.output + wire.used, data, length);
    wire.used += length;
    wire.output[wire.used] = 0;
    return XIAOTAI_OK;
}

static const char *at_command(const char *line)
{
    wire.input = line;
    wire.left = strlen(wire.input);
    wire.used = 0;
    assert(xiaotai_at_service(20) == XIAOTAI_OK && !wire.left);
    return wire.output;
}

static void at_dismiss(void)
{
    at_command("AT+CALL DISMISS\r");
    if (strcmp(wire.output, "OK\r\n")) fprintf(stderr, "AT+CALL DISMISS -> [%s]\n", wire.output);
    assert(!strcmp(wire.output, "OK\r\n"));
    flush();
}

static void boot(uint32_t needs, bool hold, bool preempt)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t *const modules[] = {&probe, &at};
    static const char *const handles[] = {XIAOTAI_EVENT_CALL_INCOMING, XIAOTAI_EVENT_CALL_STATE,
        XIAOTAI_EVENT_CALL_ENDED, XIAOTAI_EVENT_WECHAT_INCOMING, XIAOTAI_EVENT_WECHAT_STATE,
        XIAOTAI_EVENT_WECHAT_CONNECTED, XIAOTAI_EVENT_WECHAT_ENDED, NULL};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground,
        .claim = xiaotai_app_companion_claim, .handles = handles};
    static const xiaotai_application_t other = {.name = "other"};
    static const xiaotai_application_t *const apps[] = {&app, &other};
    static xiaotai_priority_t priorities[] = {{"call", 1}, {"ai", 1}, {"room", 1}, {"stream", 1},
        {XIAOTAI_APP_COMPANION_ACTIVITY, 1}, {NULL, 0}};
    static const xiaotai_at_io_t io = {.name = "fixture", .read = at_read, .write = at_write};
    priorities[0].priority = preempt ? 2 : 1;
    fixture.needs = needs;
    fixture.board_settings = true;
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    feedback_fixture = (companion_feedback_fixture_t){.enabled = true, .audio_ready = true, .audio_present = true};
    atomic_store(&usec, 1000000);
    assert(xiaotai_at_configure(&(const xiaotai_at_config_t){.task_stack_bytes = XIAOTAI_AT_NO_TASK,
        .no_platform_console = true}) == XIAOTAI_OK);
    assert(xiaotai_at_attach(&io) == XIAOTAI_OK);
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_app_companion_configure(&(const xiaotai_app_companion_config_t){.hold_refuses_sessions = hold}) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 2,
        .applications = apps, .application_count = 2, .home = "companion", .available_needs = needs,
        .priorities = priorities}) == XIAOTAI_OK);
    assert(xiaotai_at_service(20) == XIAOTAI_OK);
    assert(!strcmp(wire.output, "+READY: at=1 source=fixture locked=0\r\n"));
    publish(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL, 0);
    flush();
    if (needs & XIAOTAI_NEED_DISPLAY) assert(!strcmp(fixture.id, "home"));
}

static const xiaotai_view_action_t *action(const char *icon)
{
    for (size_t i = 0; i < fixture.action_count; ++i)
        if (fixture.actions[i].icon && !strcmp(fixture.actions[i].icon, icon)) return &fixture.actions[i];
    return NULL;
}

static void press(const char *icon)
{
    const xiaotai_view_action_t *selected = action(icon);
    assert(selected && selected->intent && !(selected->state & XIAOTAI_STATE_BUSY));
    intent(selected->intent, NULL, (uint32_t)selected->value);
}

static void initial_mute(void *unused)
{
    (void)unused;
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, 1) == XIAOTAI_OK);
}

static void replace_incoming(void *peer)
{
    assert(xiaotai_on_owner() && call.state == XIAOTAI_CALL_INCOMING);
    call.session = ++sequence;
    snprintf(call.peer_name, sizeof(call.peer_name), "%s", (const char *)peer);
    device_event(XIAOTAI_EVENT_CALL_INCOMING);
}

static void repeat_incoming(void *unused)
{
    (void)unused;
    device_event(XIAOTAI_EVENT_CALL_STATE);
}

static void start_peer_normalization(void *unused)
{
    (void)unused;
    assert(start_call(false, true, false) == XIAOTAI_OK);
    strcpy(call.peer_id, "peer-original");
}

static void normalize_call_peer(void *peer)
{
    assert(xiaotai_on_owner() && call.state == XIAOTAI_CALL_OUTGOING);
    snprintf(call.peer_id, sizeof(call.peer_id), "%s", (const char *)peer);
    device_event(XIAOTAI_EVENT_CALL_STATE);
}

static void same_phase_identity(void)
{
    run(incoming, NULL);
    assert(call.state == XIAOTAI_CALL_INCOMING && !strcmp(fixture.context, "ringing"));
    uint32_t current = call.session;
    const unsigned invalidations = fixture.invalidations;
    const unsigned old_rejections = rejected[0], old_acceptances = accepted[0];
    run(repeat_incoming, NULL);
    assert(fixture.invalidations == invalidations && call.session == current);
    run(replace_incoming, "Device peer");
    assert(call.state == XIAOTAI_CALL_INCOMING && call.session != current && fixture.invalidations == invalidations + 1U);
    assert(!strcmp(fixture.context, "ringing") && !strcmp(fixture.subtitle, "Device peer"));
    intent(XIAOTAI_INTENT_APP_COMPANION_SESSION, "accept", current);
    intent(XIAOTAI_INTENT_APP_COMPANION_END, NULL, current);
    assert(accepted[0] == old_acceptances && rejected[0] == old_rejections);
    assert(action(XIAOTAI_ICON_ACCEPT) && (uint32_t)action(XIAOTAI_ICON_ACCEPT)->value == call.session);

    current = call.session;
    run(replace_incoming, "Another peer");
    assert(call.state == XIAOTAI_CALL_INCOMING && fixture.invalidations == invalidations + 2U);
    assert(!strcmp(fixture.subtitle, "Another peer") && !strcmp(fixture.context, "ringing"));
    intent(XIAOTAI_INTENT_APP_COMPANION_SESSION, "accept", current);
    assert(accepted[0] == old_acceptances);
    press(XIAOTAI_ICON_ACCEPT);
    assert(accepted[0] == old_acceptances + 1U && last_identity == call.session);
    end_call(false);

    /* call_cloud_answer can normalize a target while the link is not yet open. */
    run(start_peer_normalization, NULL);
    current = call.session;
    const unsigned before_peer = fixture.invalidations;
    assert(!strcmp(fixture.subtitle, "Device peer") && !strcmp(fixture.context, "call"));
    run(normalize_call_peer, "peer-original");
    assert(fixture.invalidations == before_peer);
    run(normalize_call_peer, "peer-normalized");
    assert(call.session == current && call.state == XIAOTAI_CALL_OUTGOING);
    assert(!strcmp(fixture.subtitle, "Device peer") && fixture.invalidations == before_peer + 1U);
    run(normalize_call_peer, "peer-normalized");
    assert(fixture.invalidations == before_peer + 1U);
    end_call(false);
}

typedef enum { BACK_RINGING, BACK_DIALING, BACK_CONNECTING, BACK_ACTIVE } back_phase_t;
typedef struct { bool wx; back_phase_t phase; } back_case_t;

static void begin_back_case(void *ctx)
{
    const back_case_t *test = ctx;
    assert(start_call(test->wx, test->phase != BACK_RINGING, false) == XIAOTAI_OK);
    if (test->phase != BACK_CONNECTING && test->phase != BACK_ACTIVE) return;
    if (test->wx) {
        if (test->phase == BACK_ACTIVE) change_wechat_active(NULL);
        else { wechat.state = XIAOTAI_WECHAT_CONNECTING; wechat_event(XIAOTAI_EVENT_WECHAT_STATE); }
    } else {
        const xiaotai_call_state_t phase = test->phase == BACK_ACTIVE ? XIAOTAI_CALL_ACTIVE : XIAOTAI_CALL_CONNECTING;
        change_call((void *)&phase);
    }
}

static void call_back_matrix(void)
{
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (back_phase_t phase = BACK_RINGING; phase <= BACK_ACTIVE; ++phase) {
            back_case_t test = {.wx = kind != 0, .phase = phase};
            run(begin_back_case, &test);
            assert(!strcmp(fixture.id, "call"));
            const bool ringing = phase == BACK_RINGING;
            const uint32_t session = test.wx ? wechat.session : call.session;
            assert(session && !strcmp(fixture.context, ringing ? "ringing" : "call"));
            const xiaotai_view_action_t *back = action(XIAOTAI_ICON_HANGUP);
            assert(back && back->role == XIAOTAI_ROLE_BACK && (uint32_t)back->value == session);
            const xiaotai_view_action_t *answer = action(XIAOTAI_ICON_ACCEPT);
            assert(ringing ? answer && answer->role == XIAOTAI_ROLE_PRIMARY && (uint32_t)answer->value == session : !answer);
            if (ringing) {
                assert(answer && answer->label && !strcmp(answer->label, "双击接听"));
                assert(back->label && !strcmp(back->label, "长按拒绝"));
            } else {
                assert(!back->label || !back->label[0]);
            }
            const unsigned before_accept = accepted[kind], before_reject = rejected[kind], before_end = hung_up[kind];
            const char *captured = back->intent;
            press(XIAOTAI_ICON_HANGUP);
            /* The real handler accepted the action. It must keep the owning view until the terminal event. */
            assert(!strcmp(fixture.id, "call") && last_identity == session &&
                   (action(XIAOTAI_ICON_HANGUP)->state & XIAOTAI_STATE_BUSY));
            intent(captured, NULL, session);
            assert(accepted[kind] == before_accept && rejected[kind] == before_reject + (unsigned)ringing &&
                   hung_up[kind] == before_end + (unsigned)!ringing);
            end_call(false);
        }
    }
    puts("call Back: device and WeChat ring/dial/connect/active; role, generation, and duplicate held action checked");
}

static void call_ui(bool keys)
{
    boot(XIAOTAI_NEED_DISPLAY | (keys ? XIAOTAI_NEED_KEY : XIAOTAI_NEED_TOUCH), false, false);
    run(initial_mute, NULL);
    intent(XIAOTAI_INTENT_APP_COMPANION_SHOW, "settings", 0);
    assert(!strcmp(fixture.id, "settings"));
    run(incoming, NULL);
    assert(!strcmp(fixture.id, "call") && fixture.kind == XIAOTAI_VIEW_CALL && !strcmp(fixture.context, "ringing"));
    assert(!strcmp(fixture.subtitle, "Device peer") && synced[0] == 1 && call.muted);
    if (keys) assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_INCOMING) == 1);
    else assert(feedback_fixture.rings[0]);
    const uint32_t identity = sequence;
    assert(action(XIAOTAI_ICON_ACCEPT) && action(XIAOTAI_ICON_HANGUP));
    if (!keys) {
        assert(!strcmp(action(XIAOTAI_ICON_ACCEPT)->label, "接听"));
        assert(!strcmp(action(XIAOTAI_ICON_HANGUP)->label, "拒接"));
    }
    intent(XIAOTAI_INTENT_APP_COMPANION_SESSION, "accept", identity + 1);
    assert(!accepted[0] && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE);
    action_error = XIAOTAI_FAIL;
    press(XIAOTAI_ICON_HANGUP);
    assert(rejected[0] == 1 && fixture.last_refusal.value == XIAOTAI_FAIL);
    action_error = XIAOTAI_OK;
    press(XIAOTAI_ICON_ACCEPT);
    assert(accepted[0] == 1 && last_identity == identity && (action(XIAOTAI_ICON_ACCEPT)->state & XIAOTAI_STATE_BUSY));
    intent(XIAOTAI_INTENT_APP_COMPANION_SESSION, "accept", identity);
    assert(accepted[0] == 1);
    atomic_fetch_add(&usec, 1600000);
    flush();
    press(XIAOTAI_ICON_ACCEPT);
    assert(accepted[0] == 2);
    xiaotai_call_state_t state = XIAOTAI_CALL_CONNECTING;
    run(change_call, &state);
    assert(!action(XIAOTAI_ICON_ACCEPT) && action(XIAOTAI_ICON_HANGUP));
    assert(!feedback_fixture.rings[0]);
    state = XIAOTAI_CALL_ACTIVE;
    run(change_call, &state);
    assert(!strcmp(fixture.context, "call") && action(XIAOTAI_ICON_MIC_OFF) && fixture.timer_mode == XIAOTAI_TIMER_ELAPSED);
    if (!keys) assert(!strcmp(action(XIAOTAI_ICON_HANGUP)->label, "挂断"));
    press(XIAOTAI_ICON_MIC_OFF);
    assert(saved[0] == 1 && action(XIAOTAI_ICON_MIC));
    int32_t persisted;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &persisted) == XIAOTAI_OK && persisted == 0);
    press(XIAOTAI_ICON_HANGUP);
    assert(hung_up[0] == 1);
    end_call(false);
    assert(!strcmp(fixture.id, keys ? "settings" : "result") && !xiaotai_activity_active());
    if (!keys) {
        assert(!strcmp(fixture.text, "通话已结束"));
        at_dismiss();
        assert(!strcmp(fixture.id, "home"));
    }
    static const bool yes = true;
    run(incoming, (void *)&yes);
    assert(!strcmp(fixture.id, "call") && !strcmp(fixture.subtitle, "WeChat peer"));
    intent(XIAOTAI_INTENT_APP_COMPANION_SESSION, "reject", identity);
    assert(!rejected[1]);
    press(XIAOTAI_ICON_ACCEPT);
    assert(accepted[1] == 1 && last_identity == sequence);
    run(change_wechat_active, NULL);
    press(XIAOTAI_ICON_MIC);
    assert(saved[1] == 1 && action(XIAOTAI_ICON_MIC_OFF));
    end_call(true);
    assert(!strcmp(fixture.id, "result") && !strcmp(fixture.context, "error") && !strcmp(fixture.text, "无人接听"));
    assert(!xiaotai_activity_active());
    run(outgoing_video, NULL);
    assert(!strcmp(fixture.id, "call") && action(XIAOTAI_ICON_HANGUP) && !action(XIAOTAI_ICON_ACCEPT));
    end_call(false);
    if (keys) {
        same_phase_identity();
        call_back_matrix();
    }
}

static void headless(void)
{
    boot(XIAOTAI_NEED_KEY, false, false);
    run(incoming, NULL);
    intent(XIAOTAI_INTENT_APP_COMPANION_MAIN, NULL, 0);
    assert(accepted[0] == 1 && !main_fallbacks && !fixture.views);
    intent(XIAOTAI_INTENT_APP_COMPANION_END, NULL, sequence);
    assert(rejected[0] == 1);
    end_call(false);
    at_dismiss();
    assert(!fixture.views && !xiaotai_activity_active());
}

static void switch_other(void *unused)
{
    (void)unused;
    assert(xiaotai_app_switch("other") == XIAOTAI_OK);
}

static void assert_foreground(void *name) { assert(!strcmp(xiaotai_app_foreground(), name)); }

static uint32_t assert_diagnostics(const char *page, const char *context, bool foreground, bool setup, bool error_held)
{
    char expected[128];
    snprintf(expected, sizeof(expected), "+APP-COMPANION: foreground=%u setup=%u error_held=%u style=pointer"
             " page=%s context=%s pages=", (unsigned)foreground, (unsigned)setup, (unsigned)error_held, page, context);
    const char *line = at_command("AT+STATUS app.companion\r");
    if (strncmp(line, expected, strlen(expected))) fprintf(stderr, "companion diagnostics: [%s]\n", line);
    assert(!strncmp(line, expected, strlen(expected)));
    uint32_t pages, refused, stale;
    int end = 0;
    assert(sscanf(line + strlen(expected), "%" SCNu32 " refused=%" SCNu32 " stale=%" SCNu32 "%n",
                  &pages, &refused, &stale, &end) == 3 && end > 0);
    assert(!strcmp(line + strlen(expected) + (size_t)end, "\r\nOK\r\n"));
    assert(pages && refused == fixture.refused);
    return stale;
}

static void setup_hotspot(void *active)
{
    if (active) {
        fixture.provision.phase = XIAOTAI_PROVISIONING_WAITING;
        fixture.provision.provision.reason = XIAOTAI_NET_PROVISION_REASON_UNCONFIGURED;
        strcpy(fixture.provision.provision.ap_ssid, "Workshop-AP");
        strcpy(fixture.provision.provision.ap_password, "fixture-private");
    } else {
        memset(&fixture.provision, 0, sizeof(fixture.provision));
    }
    publish(XIAOTAI_EVENT_PROVISIONING_CHANGED, 0, NULL, 0);
}

static void diagnostics_at(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, false, false);
    const uint32_t stale = assert_diagnostics("home", "idle", true, false, false);
    char first[256];
    const size_t first_size = (size_t)(strstr(wire.output, "\r\n") - wire.output);
    assert(first_size < sizeof(first));
    memcpy(first, wire.output, first_size);
    const char *aggregate = at_command("AT+STATUS\r");
    const char *record = strstr(aggregate, "+APP-COMPANION:");
    assert(record && !strstr(record + 1, "+APP-COMPANION:"));
    assert(!memcmp(record, first, first_size) && !strncmp(record + first_size, "\r\n", 2));
    assert(strlen(aggregate) >= 4 && !strcmp(aggregate + strlen(aggregate) - 4, "OK\r\n"));
    assert(!strstr(aggregate, "Device peer") && !strstr(aggregate, "WeChat peer"));

    const unsigned before = fixture.views;
    intent(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 1);
    assert(assert_diagnostics("home", "idle", true, false, false) == stale + 1);
    at_dismiss();
    assert(fixture.views == before);
    run(incoming, NULL);
    at_dismiss();
    assert(call.session && !rejected[0] && !hung_up[0]);
    assert_diagnostics("call", "ringing", true, false, false);
    end_call(false);
    assert_diagnostics("result", "error", true, false, false); /* normal RESULT_ALWAYS receipt */
    at_dismiss();
    assert_diagnostics("home", "idle", true, false, false);
    run(incoming, NULL);
    end_call(true);
    assert_diagnostics("result", "error", true, false, true);
    at_dismiss();
    assert_diagnostics("home", "idle", true, false, false);
    bool active = true;
    run(setup_hotspot, &active);
    assert_diagnostics("setup", "setup", true, true, false);
    assert(!strstr(wire.output, "Workshop-AP") && !strstr(wire.output, "fixture-private"));
    run(setup_hotspot, NULL);
    run(switch_other, NULL);
    assert_diagnostics("-", "-", false, false, false);
    assert(!strcmp(at_command("AT+STATUS unknown\r"), "ERROR: NOT_FOUND SECTION\r\n"));
}

static void present(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, false, false);
    run(switch_other, NULL);
    run(incoming, NULL);
    run(assert_foreground, "companion");
    assert(!strcmp(fixture.id, "call"));
    end_call(false);
    assert(!strcmp(fixture.id, "result"));
    intent(XIAOTAI_INTENT_APP_COMPANION_END, NULL, 0);
    run(assert_foreground, "other");
}

static void held(void *unused)
{
    (void)unused;
    const char *name = NULL;
    assert(xiaotai_activity_current(&name) && !strcmp(name, XIAOTAI_APP_COMPANION_ACTIVITY));
    assert(start_call(false, false, false) == XIAOTAI_ERR_BUSY);
    assert(start_call(true, false, false) == XIAOTAI_ERR_BUSY);
    assert(xiaotai_call_dial("out-device", 0, NULL) == XIAOTAI_ERR_BUSY);
    assert(xiaotai_wechat_dial("out-wechat", 0, NULL) == XIAOTAI_ERR_BUSY);
    uint32_t id;
    assert(begin("ai", &id) == XIAOTAI_ERR_BUSY);
    assert(begin("room", &id) == XIAOTAI_ERR_BUSY);
    assert(begin("stream", &id) == XIAOTAI_ERR_BUSY);
}

static void outgoing_wechat(void *unused)
{
    (void)unused;
    assert(start_call(true, true, false) == XIAOTAI_OK);
}

static void deny_wechat_authorization(void *unused)
{
    assert(wechat.session && wechat.outgoing && activity);
    wechat = (xiaotai_wechat_status_t){.end = XIAOTAI_WECHAT_END_CONTACT, .error = XIAOTAI_ERR_NOT_FOUND,
        .outgoing = true, .authorization_result = wechat.session};
    wechat_event(XIAOTAI_EVENT_WECHAT_ENDED);
    release_call(unused);
}

static void wechat_authorization_dismiss_at(void)
{
    assert(!authorization_dismissals && !authorization_dismissed);
    run(outgoing_wechat, NULL);
    const uint32_t denied = sequence;
    assert(!strcmp(fixture.id, "call"));
    run(deny_wechat_authorization, NULL);
    assert(!strcmp(fixture.id, "result") && !strcmp(fixture.context, "error") && xiaotai_activity_active());
    assert(!authorization_dismissals);
    at_dismiss();
    assert(!strcmp(fixture.id, "home") && !xiaotai_activity_active());
    assert(authorization_dismissals == 1 && authorization_dismissed == denied);
    at_dismiss();
    assert(authorization_dismissals == 1 && authorization_dismissed == denied);

    run(outgoing_wechat, NULL);
    assert(sequence != denied);
    run(deny_wechat_authorization, NULL);
    assert(!strcmp(fixture.id, "result") && xiaotai_activity_active());
    intent(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!strcmp(fixture.id, "home") && !xiaotai_activity_active());
    at_dismiss();
    assert(authorization_dismissals == 1 && authorization_dismissed == denied);
}

static void hold(bool preempt)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY, true, preempt);
    const unsigned before = fixture.views;
    at_dismiss();
    assert(fixture.views == before && !xiaotai_activity_active());
    run(incoming, NULL);
    end_call(true);
    assert(!strcmp(fixture.id, "result") && !strcmp(fixture.text, "网络中断") && xiaotai_activity_active());
    if (preempt) {
        run(incoming, NULL);
        assert(!strcmp(fixture.id, "call") && !strcmp(fixture.context, "ringing"));
        end_call(false);
        assert(!strcmp(fixture.id, "home") && !xiaotai_activity_active());
        return;
    }
    run(held, NULL);
    assert(!strcmp(fixture.id, "result"));
    intent(XIAOTAI_INTENT_APP_COMPANION_END, NULL, 0);
    assert(!strcmp(fixture.id, "home") && !xiaotai_activity_active());
    run(incoming, NULL);
    assert(!strcmp(fixture.id, "call"));
    end_call(true);
    at_dismiss();
    assert(!strcmp(fixture.id, "home") && !xiaotai_activity_active());
    run(incoming, NULL);
    end_call(true);
    intent(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!strcmp(fixture.id, "home") && !xiaotai_activity_active());
    wechat_authorization_dismiss_at();
}

static void remote_call(void *ctx)
{
    video = (xiaotai_video_call_status_t){.active = true, .available = true, .link = call.link,
        .camera_enabled = true, .remote = ctx != NULL, .unsupported = true};
    publish(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, video.remote, &video, sizeof(video));
    publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
    publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
    xiaotai_video_call_status_t stale = video;
    stale.link = video.link - 1;
    publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &stale, sizeof(stale));
}

static void remote_stream(void *ctx)
{
    const uint32_t generation = 901;
    stream.generation = ctx ? generation : 0;
    stream_video = (xiaotai_stream_video_status_t){.generation = stream.generation, .remote_ready = ctx != NULL,
        .remote_unsupported = true};
    publish(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, ctx != NULL, &generation, sizeof(generation));
    publish(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)generation, NULL, 0);
    publish(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)generation, NULL, 0);
}

static void call_format_recovered(void *ctx)
{
    xiaotai_video_call_status_t recovery = video;
    recovery.unsupported = false;
    if (ctx) --recovery.link;
    else video.unsupported = false;
    publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0, &recovery, sizeof(recovery));
}

static void stale_call_recovery(void *unused)
{
    (void)unused;
    xiaotai_video_call_status_t obsolete = video;
    obsolete.unsupported = false;
    publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 0, &obsolete, sizeof(obsolete));
    publish(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, 1, &video, sizeof(video));
}

static void new_call_link(void *unused)
{
    (void)unused;
    ++call.link;
    device_event(XIAOTAI_EVENT_CALL_STATE);
}

static void stream_format_recovered(void *ctx)
{
    const uint32_t generation = stream.generation - (ctx != NULL);
    if (!ctx) stream_video.remote_unsupported = false;
    publish(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, (int32_t)generation, NULL, 0);
}

static void stale_stream_recovery(void *unused)
{
    (void)unused;
    publish(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, (int32_t)stream.generation, NULL, 0);
    publish(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)stream.generation, NULL, 0);
}

static void new_stream_link(void *unused)
{
    (void)unused;
    ++stream.generation;
    stream_video.generation = stream.generation;
    stream_video.remote_unsupported = true;
    publish(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, (int32_t)stream.generation, NULL, 0);
}

static void unrelated_toast(void *unused)
{
    (void)unused;
    xiaotai_view_toast("unrelated notice", 0);
}

static void video_ui(void)
{
    boot(XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH, false, false);
    run(outgoing_video, NULL);
    xiaotai_call_state_t state = XIAOTAI_CALL_ACTIVE;
    run(change_call, &state);
    assert(!(fixture.flags & XIAOTAI_VIEW_VIDEO) && !action(XIAOTAI_ICON_CAMERA));
    unsigned warnings = fixture.toasts;
    run(remote_call, &state);
    assert(fixture.flags & XIAOTAI_VIEW_VIDEO);
    assert(fixture.toasts == warnings + 1);
    press(XIAOTAI_ICON_CAMERA);
    assert(camera_calls == 1 && action(XIAOTAI_ICON_CAMERA_OFF));
    run(remote_call, NULL);
    assert(!(fixture.flags & XIAOTAI_VIEW_VIDEO) && fixture.toasts == warnings + 1);
    run(call_format_recovered, &state);
    assert(fixture.toast[0]);
    run(stale_call_recovery, NULL);
    assert(fixture.toast[0] && fixture.toasts == warnings + 1);
    run(call_format_recovered, NULL);
    assert(!fixture.toast[0] && !(fixture.flags & XIAOTAI_VIEW_VIDEO));
    run(remote_call, NULL);
    assert(fixture.toast[0] && fixture.toasts == warnings + 2);
    run(unrelated_toast, NULL);
    run(call_format_recovered, NULL);
    assert(!strcmp(fixture.toast, "unrelated notice"));
    run(new_call_link, NULL);
    run(remote_call, NULL);
    run(call_format_recovered, &state); /* The previous link cannot clear this notice. */
    assert(!strcmp(fixture.toast, "视频格式不支持") && fixture.toasts == warnings + 4);
    end_call(false);
    at_dismiss();
    assert(!strcmp(fixture.id, "home"));
    warnings = fixture.toasts;
    run(remote_stream, &state);
    assert((fixture.flags & XIAOTAI_VIEW_VIDEO) && fixture.toasts == warnings + 1);
    run(stream_format_recovered, &state);
    assert(fixture.toast[0]);
    run(stale_stream_recovery, NULL);
    assert(fixture.toast[0] && fixture.toasts == warnings + 1);
    run(stream_format_recovered, NULL);
    assert(!fixture.toast[0]);
    run(remote_stream, &state);
    assert(fixture.toast[0] && fixture.toasts == warnings + 2);
    run(unrelated_toast, NULL);
    run(stream_format_recovered, NULL);
    assert(!strcmp(fixture.toast, "unrelated notice"));
    run(new_stream_link, NULL);
    run(stream_format_recovered, &state);
    assert(!strcmp(fixture.toast, "视频格式不支持") && fixture.toasts == warnings + 4);
    run(stream_format_recovered, NULL);
    assert(!fixture.toast[0]);
    run(remote_stream, NULL);
    assert(!(fixture.flags & XIAOTAI_VIEW_VIDEO) && fixture.toasts == warnings + 4);
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    const char *scenario = argc > 1 ? argv[1] : "call";
    if (!strcmp(scenario, "call")) call_ui(false);
    else if (!strcmp(scenario, "call_keys")) call_ui(true);
    else if (!strcmp(scenario, "call_headless")) headless();
    else if (!strcmp(scenario, "call_present")) present();
    else if (!strcmp(scenario, "hold")) hold(false);
    else if (!strcmp(scenario, "hold_preempt")) hold(true);
    else if (!strcmp(scenario, "at")) diagnostics_at();
    else { assert(!strcmp(scenario, "video")); video_ui(); }
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
