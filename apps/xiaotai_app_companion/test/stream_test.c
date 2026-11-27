// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_h5_local_stop.py
#include "feedback_fakes.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/modules/stream.h"
#include "xiaotai/port/os.h"
#ifdef COMPANION_TEST_STREAM_VIDEO
#include "xiaotai/modules/stream_video.h"
#endif

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_stream;
#ifdef COMPANION_TEST_STREAM_VIDEO
extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_stream_video;
#endif

static xiaotai_signal_t fence;
static atomic_int_fast64_t now_us;
static xiaotai_stream_status_t stream;
static uint32_t activity, last_stop;
static unsigned stop_requests;
static xiaotai_err_t stop_error;
#ifdef COMPANION_TEST_STREAM_VIDEO
static xiaotai_stream_video_status_t video;
#endif

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now_us); }

void xiaotai_stream_status(xiaotai_stream_status_t *status)
{
    assert(xiaotai_on_owner());
    *status = stream;
}

xiaotai_err_t xiaotai_stream_stop(uint32_t generation)
{
    assert(xiaotai_on_owner() && stream.phase == XIAOTAI_STREAM_ACTIVE && stream.generation == generation);
    ++stop_requests;
    last_stop = generation;
    return stop_error;  /* Acceptance is distinct from the later terminal event. */
}

#ifdef COMPANION_TEST_STREAM_VIDEO
void xiaotai_stream_video_status(xiaotai_stream_video_status_t *status)
{
    assert(xiaotai_on_owner());
    *status = video;
}
#endif

static void note_activity(void) { ++fixture.activity_notes; }
static const xiaotai_app_companion_part_t presentation = {.name = "fixture", .user_activity = note_activity};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &xiaotai_app_companion_with_stream,
#ifdef COMPANION_TEST_STREAM_VIDEO
    &xiaotai_app_companion_with_stream_video,
#endif
    &presentation, NULL,
};

static void notify(void *unused) { (void)unused; xiaotai_signal_give(&fence); }

static void flush(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        assert(xiaotai_run(notify, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void run(void (*fn)(void *), void *ctx)
{
    assert(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    flush();
}

static void emit(const char *name, int32_t value, const void *data, size_t size)
{
    assert(xiaotai_event_emit(name, &(const xiaotai_args_t){.value = value}, data, size) == XIAOTAI_OK);
}

static void intent(const char *name, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_intent_post(name, &args) == XIAOTAI_OK);
    flush();
}

static void start_stream(void *ctx)
{
    const xiaotai_stream_status_t *requested = ctx;
    assert(!activity);
    assert(xiaotai_activity_begin(&(const xiaotai_activity_t){
        .name = XIAOTAI_STREAM_ACTIVITY, .generation = requested->generation}, &activity) == XIAOTAI_OK);
    stream = *requested;
    if (stream.phase == XIAOTAI_STREAM_ACTIVE)
        emit(XIAOTAI_EVENT_STREAM_STARTED, (int32_t)stream.generation, NULL, 0);
}

static void end_notice(uint32_t generation, xiaotai_stream_end_t reason, xiaotai_err_t error, bool started)
{
    const xiaotai_stream_ended_t ended = {.generation = generation, .started = started, .end = reason, .error = error};
    emit(XIAOTAI_EVENT_STREAM_ENDED, reason, &ended, sizeof(ended));
}

static void finish(void *ctx)
{
    assert(activity && stream.generation);
    const uint32_t generation = stream.generation;
    const bool started = stream.phase == XIAOTAI_STREAM_ACTIVE;
    xiaotai_activity_end(activity);
    activity = 0;
    stream = (xiaotai_stream_status_t){.phase = XIAOTAI_STREAM_LISTENING};
    end_notice(generation, ctx ? XIAOTAI_STREAM_END_MEDIA : XIAOTAI_STREAM_END_LOCAL,
               ctx ? XIAOTAI_FAIL : XIAOTAI_OK, started);
}

static void start(uint32_t generation, bool video_declared)
{
    run(start_stream, &(xiaotai_stream_status_t){
        .phase = XIAOTAI_STREAM_ACTIVE, .generation = generation, .video_declared = video_declared});
}

static xiaotai_view_action_t button(const char *icon)
{
    for (size_t i = 0; i < fixture.action_count; ++i)
        if (fixture.actions[i].icon && !strcmp(fixture.actions[i].icon, icon)) return fixture.actions[i];
    return (xiaotai_view_action_t){0};
}

static void press(xiaotai_view_action_t action)
{
    assert(action.intent);
    intent(action.intent, NULL, action.value);
}

static void boot(bool keys, bool phases, bool legacy_session)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const char *const handles[] = {
        XIAOTAI_EVENT_STREAM_STARTED, XIAOTAI_EVENT_STREAM_STATE, XIAOTAI_EVENT_STREAM_ENDED,
#ifdef COMPANION_TEST_STREAM_VIDEO
        XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED,
#endif
        NULL,
    };
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground,
        .claim = xiaotai_app_companion_claim, .handles = handles};
    static const xiaotai_application_t other = {.name = "other"};
    static const xiaotai_application_t *const applications[] = {&app, &other};
    static const xiaotai_priority_t priorities[] = {{XIAOTAI_STREAM_ACTIVITY, 1}, {NULL, 0}};
    fixture.needs = XIAOTAI_NEED_DISPLAY | (keys ? XIAOTAI_NEED_KEY : XIAOTAI_NEED_TOUCH);
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    fixture.saved_credentials = true;
    fixture.board_settings = true;
    feedback_fixture = (companion_feedback_fixture_t){.enabled = true, .audio_ready = true, .audio_present = true};
    atomic_store(&now_us, 1000000);
    assert(xiaotai_app_companion_configure(&(const xiaotai_app_companion_config_t){
        .stream_camera_status = phases, .stream_active_text = legacy_session ? "通话中" : NULL,
        .stream_show_elapsed = legacy_session, .hold_refuses_sessions = legacy_session}) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 1,
        .applications = applications, .application_count = 2, .home = "companion", .available_needs = fixture.needs,
        .priorities = priorities}) == XIAOTAI_OK);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL, 0);
    flush();
    assert(!strcmp(fixture.id, "home"));
    assert(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP);
}

#ifndef COMPANION_TEST_STREAM_VIDEO
static void replace(void *ctx)
{
    finish(NULL);
    start_stream(ctx);
}

static void change_net(void *ctx)
{
    fixture.net.state = ctx ? XIAOTAI_NET_UP : XIAOTAI_NET_CONNECTING;
    emit(XIAOTAI_EVENT_NET_STATE, fixture.net.state, NULL, 0);
}

static void audio_home(void)
{
    boot(false, false, false);
    assert(!button(XIAOTAI_ICON_STOP).intent);
    const unsigned before = fixture.activity_notes;
    run(start_stream, &(xiaotai_stream_status_t){.phase = XIAOTAI_STREAM_STARTING, .generation = 7});
    assert(!button(XIAOTAI_ICON_STOP).intent);
    intent(XIAOTAI_INTENT_APP_COMPANION_SESSION, XIAOTAI_INTENT_STREAM_STOP, 7);
    assert(fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE && !stop_requests);
    run(finish, NULL);
    start(UINT32_C(0x80000001), false);
    const xiaotai_view_action_t original = button(XIAOTAI_ICON_STOP);
    assert(original.intent && !strcmp(original.label, "结束对讲") && (uint32_t)original.value == UINT32_C(0x80000001));
    assert(!strcmp(fixture.status, "远程对讲中") && !fixture.text[0]);
    assert(!(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP) && fixture.activity_notes > before);
    assert(!strcmp(fixture.context, XIAOTAI_APP_COMPANION_CONTEXT_SESSION));
    assert(xiaotai_intent_post_external(original.intent, NULL) == XIAOTAI_ERR_NOT_ALLOWED);

    run(change_net, NULL);
    assert(!strcmp(fixture.id, "home") && !(button(XIAOTAI_ICON_STOP).state & XIAOTAI_STATE_DISABLED));
    stop_error = XIAOTAI_ERR_NO_MEM;
    press(original);
    assert(stop_requests == 1 && fixture.last_refusal.value == XIAOTAI_ERR_NO_MEM);
    assert(!(button(XIAOTAI_ICON_STOP).state & XIAOTAI_STATE_BUSY));
    stop_error = XIAOTAI_OK;
    press(original);
    assert(stop_requests == 2 && last_stop == UINT32_C(0x80000001));
    assert(button(XIAOTAI_ICON_STOP).state & XIAOTAI_STATE_BUSY);
    assert(!strcmp(fixture.status, "正在结束"));
    press(original);
    assert(stop_requests == 2);
    atomic_fetch_add(&now_us, (int64_t)XIAOTAI_APP_COMPANION_PENDING_MS * 1000 + 1);
    flush();
    assert(!(button(XIAOTAI_ICON_STOP).state & XIAOTAI_STATE_BUSY));
    press(original);
    assert(stop_requests == 3);

    run(replace, &(xiaotai_stream_status_t){.phase = XIAOTAI_STREAM_ACTIVE, .generation = 42});
    assert((uint32_t)button(XIAOTAI_ICON_STOP).value == 42);
    assert(!(button(XIAOTAI_ICON_STOP).state & XIAOTAI_STATE_BUSY));
    const unsigned refused_before = fixture.refused;
    press(original);
    assert(stop_requests == 3 && fixture.refused == refused_before + 1);
    end_notice(42, XIAOTAI_STREAM_END_BUSY, XIAOTAI_ERR_BUSY, false);
    end_notice((uint32_t)original.value, XIAOTAI_STREAM_END_MEDIA, XIAOTAI_FAIL, true);
    emit(XIAOTAI_EVENT_STREAM_STARTED, original.value, NULL, 0);
    flush();
    assert((uint32_t)button(XIAOTAI_ICON_STOP).value == 42 && fixture.refused == refused_before + 1);
    intent(XIAOTAI_INTENT_APP_COMPANION_SESSION, XIAOTAI_INTENT_STREAM_STOP, 0);
    assert(stop_requests == 3 && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE);
    const bool fail = true;
    const unsigned before_takeover = fixture.refused;
    run(finish, (void *)&fail);
    /* The activity ends before its terminal event, so offline setup takes
     * over first. A delayed refusal must not cover that unrelated page. */
    assert(!button(XIAOTAI_ICON_STOP).intent && fixture.refused == before_takeover);
    run(change_net, (void *)&fail);
    start(43, false);
    run(finish, (void *)&fail);
    assert(!button(XIAOTAI_ICON_STOP).intent && fixture.last_refusal.value == XIAOTAI_FAIL);
    assert(strcmp(fixture.context, XIAOTAI_APP_COMPANION_CONTEXT_SESSION));
}

static void render(void *unused)
{
    (void)unused;
    companion_render();
}

static void expect_hold(void *unused)
{
    (void)unused;
    const char *name = NULL;
    assert(xiaotai_activity_current(&name) && !strcmp(name, XIAOTAI_APP_COMPANION_ACTIVITY));
    uint32_t refused_activity = 0;
    assert(xiaotai_activity_begin(&(const xiaotai_activity_t){.name = XIAOTAI_STREAM_ACTIVITY}, &refused_activity) ==
           XIAOTAI_ERR_BUSY);
}

static void keys_ui(void)
{
    boot(true, false, true);
    assert(!strcmp(fixture.status, "在线"));
    intent(XIAOTAI_INTENT_APP_COMPANION_SHOW, "settings", 0);
    start(20, false);
    assert(!strcmp(fixture.id, "stream") && !strcmp(fixture.title, "实时查看"));
    assert(fixture.kind == XIAOTAI_VIEW_NOTICE && !strcmp(fixture.text, "通话中"));
    assert(fixture.timer_mode == XIAOTAI_TIMER_ELAPSED && fixture.timer_s == 0);
    atomic_fetch_add(&now_us, INT64_C(8000000));
    run(render, NULL);
    assert(fixture.timer_mode == XIAOTAI_TIMER_ELAPSED && fixture.timer_s == 8);
    const xiaotai_view_action_t back = button(XIAOTAI_ICON_BACK);
    assert(back.role == XIAOTAI_ROLE_BACK && back.value == 20);
    press(back);
    assert(stop_requests == 1 && last_stop == 20 && !strcmp(fixture.text, "正在结束"));
    assert(fixture.timer_mode == XIAOTAI_TIMER_NONE);
    assert(button(XIAOTAI_ICON_BACK).state & XIAOTAI_STATE_BUSY);
    press(back); /* A held/repeated Back cannot stop again while ending. */
    assert(stop_requests == 1 && last_stop == 20 && !strcmp(fixture.id, "stream"));
    run(finish, NULL);
    assert(!strcmp(fixture.id, "home") && !strcmp(fixture.status, "在线") && (fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));

    start(21, false);
    press(back);
    assert(stop_requests == 1 && last_stop == 20 && !strcmp(fixture.id, "stream"));
    intent(XIAOTAI_INTENT_APP_COMPANION_SHOW, "settings", 0);
    run(finish, NULL);  /* A covered completed stream must not reappear on Back. */
    assert(!strcmp(fixture.id, "settings"));
    intent(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!strcmp(fixture.id, "home"));

    start(23, false);
    assert(fixture.timer_mode == XIAOTAI_TIMER_ELAPSED && fixture.timer_s == 0);
    const bool fail = true;
    run(finish, (void *)&fail);
    assert(!strcmp(fixture.id, "result") && !strcmp(fixture.text, "呼叫失败"));
    assert(fixture.last_refusal.value == XIAOTAI_FAIL);
    run(expect_hold, NULL);
    intent(XIAOTAI_INTENT_APP_COMPANION_END, NULL, 0);
    assert(!strcmp(fixture.id, "home"));
    start(24, false);
    assert(!strcmp(fixture.id, "stream"));
    run(finish, NULL);
}

static void switch_other(void *unused)
{
    (void)unused;
    assert(xiaotai_app_switch("other") == XIAOTAI_OK);
}

static void presentation_routing(void)
{
    boot(true, false, false);
    run(switch_other, NULL);
    assert(!strcmp(xiaotai_app_foreground(), "other"));
    emit(XIAOTAI_EVENT_STREAM_STARTED, 3, NULL, 0);
    end_notice(3, XIAOTAI_STREAM_END_CLOSED, XIAOTAI_OK, false);
    flush();
    assert(!strcmp(xiaotai_app_foreground(), "other"));
    start(22, false);
    assert(!strcmp(xiaotai_app_foreground(), "companion") && !strcmp(fixture.id, "stream"));
    assert(!strcmp(fixture.text, "远程对讲中") && fixture.timer_mode == XIAOTAI_TIMER_NONE);
    run(finish, NULL);
    assert(!strcmp(xiaotai_app_foreground(), "other"));
}

#else
static void camera_phase(void *ctx)
{
    const xiaotai_video_event_t *update = ctx;
    video.generation = update->generation;
    video.uplink = update->phase;
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UPLINK, update->phase, update, sizeof(*update));
}

static void remote(void *ctx)
{
    const bool ready = ctx != NULL;
    video.remote_ready = ready;
    emit(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, ready, &video.generation, sizeof(video.generation));
}

static void video_ui(bool phases)
{
    boot(false, phases, false);
    start(83, true);
    assert(!strcmp(fixture.status, "远程查看中") && !strcmp(button(XIAOTAI_ICON_STOP).label, "结束查看"));
    run(camera_phase, &(xiaotai_video_event_t){.generation = 83, .phase = XIAOTAI_VIDEO_STARTING});
    assert(!strcmp(fixture.status, phases ? "摄像头启动中" : "远程查看中"));
    run(camera_phase, &(xiaotai_video_event_t){.generation = 83, .phase = XIAOTAI_VIDEO_RUNNING});
    assert(!strcmp(fixture.status, phases ? "视频查看中" : "远程查看中"));
    const xiaotai_video_event_t stale = {.generation = 82, .phase = XIAOTAI_VIDEO_ERROR};
    const unsigned before = fixture.views;
    emit(XIAOTAI_EVENT_STREAM_VIDEO_UPLINK, stale.phase, &stale, sizeof(stale));
    flush();
    assert(fixture.views == before);
    run(camera_phase, &(xiaotai_video_event_t){.generation = 83, .phase = XIAOTAI_VIDEO_ERROR});
    assert(!strcmp(fixture.status, phases ? "摄像头异常" : "远程查看中") && !(fixture.flags & XIAOTAI_VIEW_VIDEO));
    run(remote, (void *)&phases);
    assert(fixture.flags & XIAOTAI_VIEW_VIDEO);
    run(remote, NULL);
    assert(!(fixture.flags & XIAOTAI_VIEW_VIDEO));
    press(button(XIAOTAI_ICON_STOP));
    assert(stop_requests == 1 && last_stop == 83 && !strcmp(fixture.status, "正在结束"));
    run(camera_phase, &(xiaotai_video_event_t){.generation = 83, .phase = XIAOTAI_VIDEO_RUNNING});
    assert(!strcmp(fixture.status, "正在结束"));
    run(finish, NULL);
    assert(!(fixture.flags & XIAOTAI_VIEW_VIDEO) && !button(XIAOTAI_ICON_STOP).intent);
    assert(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP);
}
#endif

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
#ifdef COMPANION_TEST_STREAM_VIDEO
    if (argc == 1) video_ui(true);
    else { assert(argc == 2 && !strcmp(argv[1], "fixed")); video_ui(false); }
#else
    if (argc == 1) audio_home();
    else if (!strcmp(argv[1], "keys")) keys_ui();
    else { assert(argc == 2 && !strcmp(argv[1], "present")); presentation_routing(); }
#endif
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
