// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_product.c::product
// Replaces legacy: tests/xmini/test_feedback.c
#include "feedback_fakes.h"

#include <assert.h>
#include <string.h>

#include "xiaotai/media.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"
#include "xiaotai_os_host.h"

static xiaotai_signal_t fence;

static void signal_done(void *unused) { (void)unused; xiaotai_signal_give(&fence); }

static void flush(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        assert(xiaotai_run(signal_done, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void emit(const char *name, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_event_emit(name, &args, NULL, 0) == XIAOTAI_OK);
    flush();
}

static void advance(uint32_t ms)
{
    xiaotai_os_host_advance_us((int64_t)ms * 1000);
    flush();
}

static void render_call(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    view->kind = XIAOTAI_VIEW_CALL;
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = companion_fixture_init();
    if (err != XIAOTAI_OK) return err;
    static const xiaotai_setting_t voice = {.name = XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE,
        .key = XIAOTAI_SETTING_KEY_APP_COMPANION_ACK_VOICE, .max = 1};
    err = xiaotai_setting_register(&voice);
    if (err != XIAOTAI_OK) return err;
    static const companion_page_t call = {.id = "call", .title = "Call", .build = render_call};
    return companion_page_register(&call);
}

static void notify_early_startup(void)
{
    if (feedback_fixture.audio_ready || feedback_fixture.audio_start_error)
        assert(xiaotai_event_emit(XIAOTAI_EVENT_MEDIA_STARTUP, NULL, NULL, 0) == XIAOTAI_OK);
}

static void boot(bool pointer, const xiaotai_app_companion_config_t *config)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = init, .start = notify_early_startup};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground,
        .claim = xiaotai_app_companion_claim};
    static const xiaotai_application_t *const apps[] = {&app};
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | (pointer ? XIAOTAI_NEED_TOUCH : XIAOTAI_NEED_KEY);
    fixture = (companion_test_fixture_t){.needs = needs};
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    feedback_fixture.enabled = true;
    assert(xiaotai_app_companion_configure(config) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 1, .available_needs = needs}) == XIAOTAI_OK);
    flush();
}

static void stop(void)
{
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    feedback_fixture = (companion_feedback_fixture_t){0};
}

typedef struct { companion_feedback_phase_t phase; uint32_t generation; } session_t;

static void owner_phase(void *ctx)
{
    session_t *event = ctx;
    companion_feedback_session(event->phase, event->generation);
}

static void session(companion_feedback_phase_t phase, uint32_t generation)
{
    session_t event = {phase, generation};
    assert(xiaotai_run(owner_phase, &event) == XIAOTAI_OK);
    flush();
}

static void owner_audio_idle(void *unused) { (void)unused; companion_fixture_audio_idle(); }
static void idle_audio(void)
{
    assert(xiaotai_run(owner_audio_idle, NULL) == XIAOTAI_OK);
    flush();
}

static void connected_done(void *ctx)
{
    const xiaotai_err_t result = ctx ? *(const xiaotai_err_t *)ctx : XIAOTAI_ERR_INVALID_STATE;
    companion_fixture_prompt_end(result);
    assert(companion_fixture_restart_holds() == 0);
}

static void check_released(void *unused) { (void)unused; assert(companion_fixture_restart_holds() == 0); }
static void check_held(void *unused) { (void)unused; assert(companion_fixture_restart_holds() == 1); }

static void keys_feedback(void)
{
    boot(false, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_STARTUP) == 1);
    assert(feedback_fixture.guidance);
    for (size_t i = 0; i < fixture.feedback->effect_count; ++i)
        assert(fixture.feedback->effects[i].sound.flags & XIAOTAI_FEEDBACK_WAIT);

    emit(XIAOTAI_EVENT_PROVISIONING_OPENED, 0, NULL);
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, XIAOTAI_PROVISIONING_VERIFYING, NULL);
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED, XIAOTAI_PROVISIONING_VERIFYING, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_SETUP) == 1);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_CONNECTING) == 1);
    emit(XIAOTAI_EVENT_PROVISIONING_DONE, 1, NULL);
    assert(feedback_fixture.holds_at_connected == 1 && feedback_fixture.done);
    assert(xiaotai_run(connected_done, NULL) == XIAOTAI_OK);
    flush();
    assert(feedback_fixture.released == 1);
    feedback_fixture.connected_error = XIAOTAI_ERR_INVALID_STATE;
    emit(XIAOTAI_EVENT_PROVISIONING_DONE, 0, NULL);
    assert(feedback_fixture.holds_at_connected == 1 && !feedback_fixture.done);
    assert(xiaotai_run(check_released, NULL) == XIAOTAI_OK);
    flush();

    emit(XIAOTAI_EVENT_BINDING_BOUND, 1, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_BOUND) == 1);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ONLINE) == 1);

    feedback_fixture.busy = true;
    session(COMPANION_FEEDBACK_DIALING, 10);
    assert(!strcmp(feedback_fixture.waiting, XIAOTAI_APP_COMPANION_EFFECT_DIALING));
    idle_audio();
    assert(feedback_fixture.heard == 1 && !strcmp(feedback_fixture.heard_effect, XIAOTAI_APP_COMPANION_EFFECT_DIALING));
    feedback_fixture.busy = true;
    session(COMPANION_FEEDBACK_INCOMING, 11);
    session(COMPANION_FEEDBACK_CALL, 11);
    assert(!feedback_fixture.waiting && !strcmp(feedback_fixture.last_cancelled, XIAOTAI_APP_COMPANION_EFFECT_INCOMING));
    idle_audio();
    assert(feedback_fixture.heard == 1);
    session(COMPANION_FEEDBACK_IDLE, 10);
    assert(!companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED));
    session(COMPANION_FEEDBACK_IDLE, 11);
    session(COMPANION_FEEDBACK_IDLE, 11);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED) == 1);
    session(COMPANION_FEEDBACK_TALK, 12);
    session(COMPANION_FEEDBACK_FAILED, 12);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) == 1);

    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, 0, XIAOTAI_SETTING_GUIDANCE_ENABLED);
    assert(!feedback_fixture.guidance);
    emit(XIAOTAI_EVENT_PROVISIONING_OPENED, 0, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_SETUP) == 1);
    emit(XIAOTAI_EVENT_PROVISIONING_FAILED, XIAOTAI_FAIL, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, 0, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_SETUP_FAILED) == 1);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_OFFLINE) == 1);
    session(COMPANION_FEEDBACK_INCOMING, 13);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_INCOMING) == 1);
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_BLOCKED, NULL);
    emit(XIAOTAI_EVENT_NET_STATE, XIAOTAI_NET_BLOCKED, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_PAUSED, 0, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) == 3);
    stop();
}

static void open_call(void *unused)
{
    (void)unused;
    assert(companion_show_page("call", 0) == XIAOTAI_OK);
}

static void replace_pending_incoming(void)
{
    boot(false, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL);
    feedback_fixture.busy = true;
    const char *const cue = XIAOTAI_APP_COMPANION_EFFECT_INCOMING;
    session(COMPANION_FEEDBACK_INCOMING, 7);
    assert(feedback_fixture.waiting && !strcmp(feedback_fixture.waiting, cue));
    const unsigned cancelled = feedback_fixture.cancelled;

    session(COMPANION_FEEDBACK_INCOMING, 8);
    assert(feedback_fixture.cancelled == cancelled + 1U && !strcmp(feedback_fixture.last_cancelled, cue));
    assert(companion_fixture_fire_count(cue) == 2U && !feedback_fixture.heard);
    session(COMPANION_FEEDBACK_INCOMING, 8);
    session(COMPANION_FEEDBACK_IDLE, 7);
    assert(feedback_fixture.cancelled == cancelled + 1U && companion_fixture_fire_count(cue) == 2U);
    assert(!companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED));
    idle_audio();
    idle_audio();
    assert(feedback_fixture.heard == 1U && !strcmp(feedback_fixture.heard_effect, cue));
    session(COMPANION_FEEDBACK_IDLE, 8);
    session(COMPANION_FEEDBACK_IDLE, 8);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED) == 1U);
    stop();
}

static void xmini_pending_cues(void)
{
    boot(false, NULL);
    emit(XIAOTAI_EVENT_BINDING_BOUND, 1, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_OFFLINE, 0, NULL);
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE, 0, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_BOUND) == 1);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ONLINE) == 1);
    feedback_fixture.busy = true;
    session(COMPANION_FEEDBACK_INCOMING, 10);
    session(COMPANION_FEEDBACK_DIALING, 11);
    for (unsigned i = 0; i < 20; ++i) session(COMPANION_FEEDBACK_DIALING, 11);
    assert(feedback_fixture.busy && !feedback_fixture.heard && !feedback_fixture.cancel_playing);
    assert(!strcmp(feedback_fixture.last_cancelled, XIAOTAI_APP_COMPANION_EFFECT_INCOMING));
    assert(!strcmp(feedback_fixture.waiting, XIAOTAI_APP_COMPANION_EFFECT_DIALING));
    idle_audio();
    idle_audio();
    assert(feedback_fixture.heard == 1 && !strcmp(feedback_fixture.heard_effect, XIAOTAI_APP_COMPANION_EFFECT_DIALING));
    stop();
}

static void xmini_connected_hold(void)
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        boot(false, NULL);
        feedback_fixture.busy = mode == 1;
        if (mode == 4) feedback_fixture.connected_error = XIAOTAI_FAIL;
        if (mode == 5) emit(XIAOTAI_EVENT_SETTINGS_CHANGED, 0, XIAOTAI_SETTING_GUIDANCE_ENABLED);
        emit(XIAOTAI_EVENT_PROVISIONING_DONE, 1, NULL);
        assert(xiaotai_run(mode < 4 ? check_held : check_released, NULL) == XIAOTAI_OK);
        flush();
        if (mode < 4) {
            const xiaotai_err_t results[] = {XIAOTAI_OK, XIAOTAI_OK, XIAOTAI_FAIL, XIAOTAI_ERR_INVALID_STATE};
            if (mode == 1) {
                assert(!feedback_fixture.cancel_playing && !feedback_fixture.heard);
                idle_audio();
                assert(feedback_fixture.heard == 1);
                assert(xiaotai_run(check_held, NULL) == XIAOTAI_OK);
                flush();
            }
            assert(xiaotai_run(connected_done, (void *)&results[mode]) == XIAOTAI_OK);
            flush();
            assert(feedback_fixture.released == 1);
        }
        stop();
    }
}

static void ack(void *unused)
{
    (void)unused;
    companion_feedback_wake_ack(0, 1000, false);
    companion_feedback_wake_ack(28, 1000, false);
}

static void male_ack(void *unused)
{
    (void)unused;
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, 1) == XIAOTAI_OK);
    companion_feedback_wake_ack(29, 1000, true);
}

static void pointer_feedback(void)
{
    boot(true, NULL);
    assert(fixture.feedback->states[0].priority == 3 && fixture.feedback->states[1].priority == 2);
    session(COMPANION_FEEDBACK_INCOMING, 20);
    assert(!feedback_fixture.rings[0] && !feedback_fixture.rings[1]);
    assert(xiaotai_run(open_call, NULL) == XIAOTAI_OK);
    flush();
    assert(feedback_fixture.rings[0] && !feedback_fixture.rings[1]);
    session(COMPANION_FEEDBACK_CONNECTING, 20);
    assert(!feedback_fixture.rings[0] && !feedback_fixture.rings[1]);
    session(COMPANION_FEEDBACK_DIALING, 21);
    assert(!feedback_fixture.rings[0] && feedback_fixture.rings[1]);
    session(COMPANION_FEEDBACK_CALL, 21);
    assert(!feedback_fixture.rings[1]);
    assert(xiaotai_run(ack, NULL) == XIAOTAI_OK);
    flush();
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ACK_FEMALE) == 1);
    assert(feedback_fixture.last_generation == 28 && feedback_fixture.last_valid_ms == 1000);
    assert(xiaotai_run(male_ack, NULL) == XIAOTAI_OK);
    flush();
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ACK_MALE) == 1);
    assert(feedback_fixture.last_generation == 29 && feedback_fixture.last_valid_ms == 1000);
    stop();
}

static void binding_wait(void *unused)
{
    (void)unused;
    fixture.binding.phase = XIAOTAI_BINDING_WAITING;
    fixture.binding.seconds_left = 200;
    strcpy(fixture.binding.verification, "012345");
}

static void binding_clear(void *unused)
{
    (void)unused;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.binding.verification[0] = '\0';
    fixture.binding.announcing = false;
}

static void announce_end(void *unused) { (void)unused; companion_fixture_announcement_end(); }

static void stop_guidance_before_previous_completion(void *unused)
{
    (void)unused;
    xiaotai_args_t args = {.value = 0};
    strcpy(args.text, XIAOTAI_SETTING_GUIDANCE_ENABLED);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, &args, NULL, 0) == XIAOTAI_OK);
    companion_fixture_announcement_end();
    /* Explicit replay in the owner queue can already be next while the
     * automatic announcement's completion is still waiting to dispatch. */
    assert(xiaotai_binding_announce() == XIAOTAI_OK);
}

static void begin_binding(void)
{
    assert(xiaotai_run(binding_wait, NULL) == XIAOTAI_OK);
    flush();
    emit(XIAOTAI_EVENT_BINDING_CHANGED, XIAOTAI_BINDING_WAITING, NULL);
    emit(XIAOTAI_EVENT_BINDING_CODE, 200, "012345");
}

static void binding_feedback(void)
{
    xiaotai_app_companion_config_t config = {
        .announce_on_code = true, .announce_repeat_s = 30, .announce_follows_guidance = true
    };
    boot(false, &config);
    feedback_fixture.busy = true;
    begin_binding();
    assert(!feedback_fixture.announced);
    idle_audio();
    advance(100);
    assert(!feedback_fixture.announced);
    advance(120);
    assert(feedback_fixture.announced == 1);
    emit(XIAOTAI_EVENT_BINDING_CODE, 199, "012345");
    assert(feedback_fixture.announced == 1);
    advance(5000);
    assert(xiaotai_run(announce_end, NULL) == XIAOTAI_OK);
    flush();
    advance(24000);
    assert(feedback_fixture.announced == 1);
    advance(1100);
    assert(feedback_fixture.announced == 2);

    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, 0, XIAOTAI_SETTING_GUIDANCE_ENABLED);
    assert(feedback_fixture.stopped == 1 && !fixture.binding.announcing);
    advance(31000);
    assert(feedback_fixture.announced == 2);
    assert(xiaotai_run(binding_clear, NULL) == XIAOTAI_OK);
    flush();
    emit(XIAOTAI_EVENT_BINDING_CHANGED, XIAOTAI_BINDING_BOUND, NULL);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, 1, XIAOTAI_SETTING_GUIDANCE_ENABLED);
    advance(31000);
    assert(feedback_fixture.announced == 2);
    stop();

    config.announce_repeat_from_end = true;
    boot(false, &config);
    feedback_fixture.announce_result = XIAOTAI_ERR_BUSY;
    begin_binding();
    assert(feedback_fixture.announced == 1);
    feedback_fixture.announce_result = XIAOTAI_OK;
    advance(1000);
    assert(feedback_fixture.announced == 1);
    advance(30000);
    assert(feedback_fixture.announced == 2 && fixture.binding.announcing);
    advance(31000);
    assert(feedback_fixture.announced == 2);
    assert(xiaotai_run(announce_end, NULL) == XIAOTAI_OK);
    flush();
    advance(29000);
    assert(feedback_fixture.announced == 2);
    advance(1100);
    assert(feedback_fixture.announced == 3);
    stop();

    config.announce_repeat_s = 0;
    boot(true, &config);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, 0, XIAOTAI_SETTING_GUIDANCE_ENABLED);
    begin_binding();
    assert(!feedback_fixture.announced);
    emit(XIAOTAI_EVENT_SETTINGS_CHANGED, 1, XIAOTAI_SETTING_GUIDANCE_ENABLED);
    assert(feedback_fixture.announced == 1);
    assert(xiaotai_run(announce_end, NULL) == XIAOTAI_OK);
    flush();
    advance(60000);
    assert(feedback_fixture.announced == 1);
    stop();

    boot(true, &config);
    begin_binding();
    assert(feedback_fixture.announced == 1);
    assert(xiaotai_run(stop_guidance_before_previous_completion, NULL) == XIAOTAI_OK);
    flush();
    assert(feedback_fixture.announced == 2 && !feedback_fixture.stopped && fixture.binding.announcing);
    stop();
}

static void startup_failure(void)
{
    feedback_fixture.audio_present = true;
    feedback_fixture.audio_start_error = XIAOTAI_ERR_INVALID_STATE;
    boot(false, NULL);
    assert(!companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_STARTUP));
    assert(companion_feedback_audio_error() && !strcmp(fixture.text, "音频设备未就绪"));
    stop();
    feedback_fixture.audio_present = true;
    feedback_fixture.audio_ready = true;
    feedback_fixture.startup_error = XIAOTAI_ERR_INVALID_STATE;
    boot(false, NULL);
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_STARTUP) == 1);
    assert(companion_feedback_audio_error());
    stop();
}

static void resolve_startup(void *ctx)
{
    const xiaotai_err_t error = *(const xiaotai_err_t *)ctx;
    feedback_fixture.audio_ready = error == XIAOTAI_OK;
    feedback_fixture.audio_start_error = error;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_MEDIA_STARTUP, NULL, NULL, 0) == XIAOTAI_OK);
}

static void startup_order(void)
{
    for (unsigned fail = 0; fail < 2; ++fail) {
        xiaotai_err_t result = fail ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_OK;
        feedback_fixture.audio_present = true;
        boot(false, NULL);
        assert(!companion_feedback_audio_error());
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_STARTUP) == 0);
        assert(xiaotai_run(resolve_startup, &result) == XIAOTAI_OK);
        flush();
        assert(companion_feedback_audio_error() == (fail != 0));
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_STARTUP) == 1 - fail);
        if (fail) assert(!strcmp(fixture.text, "音频设备未就绪"));
        emit(XIAOTAI_EVENT_MEDIA_STARTUP, 0, NULL);
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_STARTUP) == 1 - fail);
        stop();
    }
    feedback_fixture.audio_present = feedback_fixture.audio_ready = true;
    boot(false, NULL); /* A notice already queued before STARTED is not missed. */
    assert(!companion_feedback_audio_error());
    assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_STARTUP) == 1);
    stop();
}

int main(void)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    keys_feedback();
    replace_pending_incoming();
    xmini_pending_cues();
    xmini_connected_hold();
    pointer_feedback();
    binding_feedback();
    startup_failure();
    startup_order();
    xiaotai_signal_deinit(&fence);
    return 0;
}
