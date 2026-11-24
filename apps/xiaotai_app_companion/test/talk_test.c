// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_ack
// Replaces legacy: tests/regression/test_s3_user_button.py
// Replaces legacy: tests/regression/test_ai_start_contract.py
#include "audio_fakes.h"
#include "feedback_fakes.h"
#include "xiaotai/hal.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/wake.h"
#include "xiaotai/port/os.h"

#include "ai_device_tools.h"
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
#include "ai_contact_tools.h"
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef COMPANION_TEST_REAL_INTERACTION
#include "interaction_internal.h"

static bool main_press;
static uint32_t key_edges;
static xiaotai_err_t key_open(void *ctx) { (void)ctx; return XIAOTAI_OK; }
static uint32_t key_poll(void *ctx) { (void)ctx; return key_edges; }

const xiaotai_input_t *xiaotai_hal_input(size_t index)
{
    static const xiaotai_input_t key = {
        .desc = {.role = "main", .events = XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED | XIAOTAI_INPUT_CLICK},
        .open = key_open, .poll = key_poll,
    };
    return index ? NULL : &key;
}

xiaotai_err_t __wrap_interaction_poll_start(void);
xiaotai_err_t __wrap_interaction_poll_start(void) { return XIAOTAI_OK; }
#endif

static xiaotai_signal_t fence;
static atomic_int_fast64_t now;
static xiaotai_ai_talk_status_t status;
static xiaotai_ai_talk_caption_t caption;
static bool prefix_captions, wake_ack_on_admission;
static uint32_t serial, last_session, wake_sequence;
static unsigned starts, stops, interrupts, submits;
static xiaotai_err_t start_error, submit_error;

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now); }

static void note_activity(void)
{
    ++fixture.activity_notes;
    fixture.screen_asleep = false;
}
static bool screen_awake(void) { return !fixture.screen_asleep; }

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_ai_talk;
extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_wake;
static const xiaotai_app_companion_part_t ui_fixture = {.name = "ui_fixture",
    .user_activity = note_activity, .screen_awake = screen_awake};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &xiaotai_app_companion_with_ai_talk, &xiaotai_app_companion_with_wake, &ui_fixture, NULL,
};

static void fence_signal(void *unused) { (void)unused; xiaotai_signal_give(&fence); }
static void flush(void)
{
    for (unsigned i = 0; i < 4U; ++i) {
        assert(xiaotai_run(fence_signal, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void at(int64_t ms) { atomic_store(&now, ms * INT64_C(1000)); flush(); }

static void send(const char *name, const char *text, uint32_t value)
{
    xiaotai_args_t args = {.value = (int32_t)value};
    if (text) snprintf(args.text, sizeof(args.text), "%s", text);
    assert(xiaotai_intent_post(name, &args) == XIAOTAI_OK);
    flush();
}

static void emit(const char *name, int32_t value, const void *data, size_t size)
{
    assert(xiaotai_event_emit(name, &(xiaotai_args_t){.value = value}, data, size) == XIAOTAI_OK);
}

static void state(xiaotai_ai_talk_status_t next)
{
    status = next;
    emit(XIAOTAI_EVENT_AI_TALK_STATE, next.phase, &next, sizeof(next));
    flush();
}

static xiaotai_ai_talk_status_t active(xiaotai_ai_talk_phase_t phase, uint32_t session,
                                      uint32_t link, xiaotai_ai_talk_origin_t origin)
{
    return (xiaotai_ai_talk_status_t){.phase = phase, .session = session, .link = link, .origin = origin};
}

void xiaotai_ai_talk_status(xiaotai_ai_talk_status_t *out)
{
    assert(xiaotai_on_owner());
    *out = status;
}

xiaotai_err_t xiaotai_ai_talk_start(const xiaotai_ai_talk_start_t *options, uint32_t *session)
{
    assert(xiaotai_on_owner());
    assert(!options || (options->origin == XIAOTAI_AI_TALK_ORIGIN_USER && !options->preroll_token));
    ++starts;
    if (start_error) return start_error;
    if (status.session) return XIAOTAI_ERR_INVALID_STATE;
    status = active(XIAOTAI_AI_TALK_CONNECTING, ++serial, 0, XIAOTAI_AI_TALK_ORIGIN_USER);
    if (session) *session = serial;
    emit(XIAOTAI_EVENT_AI_TALK_STATE, status.phase, &status, sizeof(status));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_stop(uint32_t session)
{
    assert(xiaotai_on_owner());
    ++stops;
    last_session = session;
    if (!session || session != status.session) return XIAOTAI_ERR_INVALID_STATE;
    status = (xiaotai_ai_talk_status_t){.end = XIAOTAI_AI_TALK_END_LOCAL};
    emit(XIAOTAI_EVENT_AI_TALK_STATE, status.phase, &status, sizeof(status));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_interrupt(uint32_t session)
{
    assert(xiaotai_on_owner());
    ++interrupts;
    last_session = session;
    return session && session == status.session ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t xiaotai_ai_talk_submit(uint32_t session)
{
    assert(xiaotai_on_owner());
    ++submits;
    last_session = session;
    if (!session || session != status.session) return XIAOTAI_ERR_INVALID_STATE;
    return submit_error;
}

bool xiaotai_ai_talk_caption_read(xiaotai_ai_talk_caption_t *out)
{
    assert(xiaotai_on_owner());
    if (out->revision == caption.revision) return false;
    char *buffer = out->text;
    const size_t capacity = out->capacity;
    *out = caption;
    out->text = buffer;
    out->capacity = capacity;
    out->size = 0;
    if (buffer && capacity) {
        const int size = snprintf(buffer, capacity, "%s", caption.text);
        assert(size >= 0 && (size_t)size < capacity);
        out->size = (size_t)size;
    }
    return true;
}

static void caption_update(uint32_t owner, uint32_t revision, uint32_t paragraph,
                           xiaotai_ai_talk_speaker_t speaker, const char *word, const char *text)
{
    caption = (xiaotai_ai_talk_caption_t){.revision = revision, .session = owner, .paragraph = paragraph,
        .speaker = speaker, .text = (char *)text, .final = true};
    snprintf(caption.emotion, sizeof(caption.emotion), "%s", word);
    status.caption_revision = revision;
    emit(XIAOTAI_EVENT_AI_TALK_CAPTION, (int32_t)revision, NULL, 0);
    flush();
}

xiaotai_err_t companion_ai_device_tools_init(void) { return XIAOTAI_OK; }

#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
xiaotai_err_t companion_ai_contact_tools_init(void) { return XIAOTAI_OK; }
void companion_ai_contact_tools_observe(uint32_t session, uint32_t link) { (void)session; (void)link; }
void companion_ai_contact_tools_reset(void) {}

static void local_hint(void *ctx)
{
    assert(xiaotai_on_owner());
    companion_ai_talk_hint(*(const uint32_t *)ctx, "没有找到联系人");
}
#endif

static void wake(xiaotai_local_wake_outcome_t outcome, uint32_t session, xiaotai_err_t error)
{
    const xiaotai_local_wake_detection_t data = {
        .sequence = ++wake_sequence, .outcome = outcome, .error = error, .session = session,
    };
    emit(XIAOTAI_EVENT_WAKE_DETECTED, outcome, &data, sizeof(data));
    flush();
}

static void media(xiaotai_ai_talk_status_t latest, uint32_t generation)
{
    audio_fixture.status.active = generation != 0;
    audio_fixture.status.generation = generation;
    emit(XIAOTAI_EVENT_AI_TALK_MEDIA, latest.phase, &latest, sizeof(latest));
    flush();
}

static const xiaotai_view_action_t *action(xiaotai_view_role_t role)
{
    for (size_t i = 0; i < fixture.action_count; ++i) {
        if (fixture.actions[i].role == role) return &fixture.actions[i];
    }
    return NULL;
}

static void press(xiaotai_view_role_t role)
{
    const xiaotai_view_action_t *a = action(role);
    assert(a && a->intent && !(a->state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY | XIAOTAI_STATE_HIDDEN)));
    send(a->intent, NULL, (uint32_t)a->value);
}

static void session_action(const char *name, uint32_t generation)
{
    send(XIAOTAI_INTENT_APP_COMPANION_SESSION, name, generation);
}

static void boot(bool keys, bool hold)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
#ifdef COMPANION_TEST_REAL_INTERACTION
    static const xiaotai_module_t input = {.name = "interaction", .init = xiaotai_engine_interaction_init,
        .start = xiaotai_engine_interaction_start};
    static const xiaotai_module_t *const modules[] = {&probe, &input};
#else
    static const xiaotai_module_t *const modules[] = {&probe};
#endif
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    fixture.needs = XIAOTAI_NEED_DISPLAY | (keys ? XIAOTAI_NEED_KEY : XIAOTAI_NEED_TOUCH);
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    feedback_fixture = (companion_feedback_fixture_t){.enabled = true, .audio_ready = true};
    atomic_store(&now, 1000000);
    xiaotai_app_companion_config_t companion_config = {
        .hold_refuses_sessions = hold, .ai_caption_speaker_prefix = prefix_captions,
        .wake_ack_on_admission = wake_ack_on_admission,
    };
#ifdef COMPANION_TEST_REAL_INTERACTION
    companion_config.main_button_on_press = main_press;
#endif
    assert(xiaotai_app_companion_configure(&companion_config) == XIAOTAI_OK);
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = sizeof(modules) / sizeof(*modules),
        .applications = apps, .application_count = 1, .home = "companion", .available_needs = fixture.needs};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    flush();
    assert(!strcmp(fixture.id, "home"));
}

static unsigned ack_count(void)
{
    return companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ACK_FEMALE) +
           companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ACK_MALE);
}

static uint32_t mock_ai_activity;

static void begin_wake_activity(void *unused)
{
    (void)unused;
    static const xiaotai_activity_t ai = {.name = XIAOTAI_AI_TALK_ACTIVITY};
    assert(xiaotai_activity_begin(&ai, &mock_ai_activity) == XIAOTAI_OK && mock_ai_activity);
}

static void end_wake_activity(void *unused)
{
    (void)unused;
    xiaotai_activity_end(mock_ai_activity);
    assert(!xiaotai_activity_current(NULL));
}

static void activate_wake_activity(void *unused)
{
    (void)unused;
    xiaotai_activity_set_generation(mock_ai_activity, status.link);
}

static void wake_admission_case(void)
{
    wake_ack_on_admission = true;
    boot(false, false);
    const unsigned before = ack_count();
    state(active(XIAOTAI_AI_TALK_CONNECTING, 31, 0, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 31, XIAOTAI_OK);
    assert(ack_count() == before); /* An event alone does not own an activity. */

    assert(xiaotai_run(begin_wake_activity, NULL) == XIAOTAI_OK);
    flush();
    state(active(XIAOTAI_AI_TALK_CONNECTING, 32, 0, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 31, XIAOTAI_OK);
    assert(ack_count() == before);
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 32, XIAOTAI_OK);
    assert(ack_count() == before + 1 && feedback_fixture.last_generation == 0 &&
           feedback_fixture.last_valid_ms == 1000 && !feedback_fixture.cancel_playing);
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 32, XIAOTAI_OK);
    assert(ack_count() == before + 1);

    at(2100);
    state(active(XIAOTAI_AI_TALK_CONNECTING, 32, 470, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    media(status, 470);
    assert(ack_count() == before + 1 && !feedback_fixture.cancel_playing);
    /* Synchronous activity callback, before any terminal/active state event. */
    assert(xiaotai_run(activate_wake_activity, NULL) == XIAOTAI_OK);
    flush();
    assert(feedback_fixture.cancel_playing == 1 &&
           !strcmp(feedback_fixture.last_cancelled, XIAOTAI_APP_COMPANION_EFFECT_ACK_FEMALE));
    assert(xiaotai_run(end_wake_activity, NULL) == XIAOTAI_OK);
    flush();
    assert(feedback_fixture.cancel_playing == 1);

    assert(xiaotai_run(begin_wake_activity, NULL) == XIAOTAI_OK);
    flush();
    state(active(XIAOTAI_AI_TALK_CONNECTING, 33, 0, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 33, XIAOTAI_OK);
    assert(ack_count() == before + 2 && feedback_fixture.cancel_playing == 1);
    assert(xiaotai_run(end_wake_activity, NULL) == XIAOTAI_OK);
    flush();
    assert(feedback_fixture.cancel_playing == 2); /* Cancel even without a media link. */
    state((xiaotai_ai_talk_status_t){.end = XIAOTAI_AI_TALK_END_TIMEOUT, .error = XIAOTAI_ERR_TIMEOUT});
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 33, XIAOTAI_OK);
    assert(ack_count() == before + 2 && feedback_fixture.cancel_playing == 2);
}

static void emotions(void)
{
    static const struct { const char *word, *face; } pairs[] = {
        {"中性", "neutral"}, {"开心", "happy"}, {"兴奋", "excited"}, {"温和", "calm"},
        {"安慰", "loving"}, {"思考", "thinking"}, {"惊讶", "surprised"}, {"严肃", "confident"},
        {"困倦", "sleepy"}, {"困惑", "confused"}, {"", "neutral"}, {"unlisted", "neutral"},
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
        assert(!strcmp(xiaotai_app_companion_emotion_face(pairs[i].word), pairs[i].face));
        assert(!strcmp(xiaotai_app_companion_emotion_face(pairs[i].face), pairs[i].face));
    }
}

static void quiesce_roundtrip(void *unused)
{
    (void)unused;
    uint32_t ticket;
    assert(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &ticket) == XIAOTAI_OK);
    xiaotai_quiesce_end(ticket);
    assert(!xiaotai_quiesced());
}

static void wake_cases(void)
{
    boot(false, false);
    emotions();
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "menu", 0);
    assert(!strcmp(fixture.id, "menu"));
    fixture.screen_asleep = true;
    const unsigned idle_activity = fixture.activity_notes;
    const unsigned effects = ack_count();
    wake(XIAOTAI_LOCAL_WAKE_ANNOUNCED, 0, XIAOTAI_OK);
    wake(XIAOTAI_LOCAL_WAKE_NO_PREROLL, 0, XIAOTAI_ERR_NOT_SUPPORTED);
    wake(XIAOTAI_LOCAL_WAKE_BUSY, 0, XIAOTAI_ERR_BUSY);
    assert(fixture.screen_asleep && fixture.activity_notes == idle_activity);
    assert(!strcmp(fixture.id, "menu") && ack_count() == effects);

    fixture.net.state = XIAOTAI_NET_WAITING;
    wake(XIAOTAI_LOCAL_WAKE_REFUSED, 0, XIAOTAI_ERR_PLATFORM_OFFLINE);
    assert(fixture.screen_asleep && fixture.activity_notes == idle_activity && !strcmp(fixture.id, "menu"));
    fixture.net.state = XIAOTAI_NET_UP;
    wake(XIAOTAI_LOCAL_WAKE_REFUSED, 0, XIAOTAI_ERR_PLATFORM_OFFLINE);
    assert(!fixture.screen_asleep && fixture.activity_notes > idle_activity);
    assert(!strcmp(fixture.id, "home") && !strcmp(fixture.status, "现在暂时不能开始对话"));
    assert(ack_count() == effects);

    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "menu", 0);
    state(active(XIAOTAI_AI_TALK_CONNECTING, 31, 0, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    fixture.screen_asleep = true;
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 31, XIAOTAI_OK);
    assert(!fixture.screen_asleep && !strcmp(fixture.id, "home") && !strcmp(fixture.status, "在呢。"));
    assert(ack_count() == effects && starts == 0);

    at(1210);
    state(active(XIAOTAI_AI_TALK_CONNECTING, 31, 471, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    media(active(XIAOTAI_AI_TALK_CONNECTING, 99, 471, XIAOTAI_AI_TALK_ORIGIN_WAKE), 471);
    assert(ack_count() == effects);
    media(status, 472);
    assert(ack_count() == effects);
    media(status, 471);
    assert(ack_count() == effects + 1U);
    assert(feedback_fixture.last_generation == 471 && feedback_fixture.last_valid_ms == 790);
    media(status, 471);
    assert(ack_count() == effects + 1U);

    at(2699);
    assert(!strcmp(fixture.status, "在呢。"));
    at(2700);
    assert(strcmp(fixture.status, "在呢。"));
    state(active(XIAOTAI_AI_TALK_LISTENING, 31, 471, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "menu", 0);
    wake(XIAOTAI_LOCAL_WAKE_BUSY, 0, XIAOTAI_ERR_BUSY);
    assert(!strcmp(fixture.id, "home") && !strcmp(fixture.status, "我在听，请直接说。"));
    assert(ack_count() == effects + 1U);

    state((xiaotai_ai_talk_status_t){.end = XIAOTAI_AI_TALK_END_LOCAL});
    at(3000);
    state(active(XIAOTAI_AI_TALK_CONNECTING, 32, 0, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 32, XIAOTAI_OK);
    at(4000);
    state(active(XIAOTAI_AI_TALK_CONNECTING, 32, 472, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    media(status, 472);
    assert(ack_count() == effects + 1U);
    state((xiaotai_ai_talk_status_t){.end = XIAOTAI_AI_TALK_END_TIMEOUT, .error = XIAOTAI_ERR_TIMEOUT});
    assert(!strcmp(fixture.status, "连接失败，请重新开始"));

    at(5000);
    state(active(XIAOTAI_AI_TALK_CONNECTING, 33, 473, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    media(status, 473);
    assert(ack_count() == effects + 1U);
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 33, XIAOTAI_OK);
    assert(ack_count() == effects + 2U && feedback_fixture.last_generation == 473);
    assert(feedback_fixture.last_valid_ms == 1000);

    at(6000);
    state((xiaotai_ai_talk_status_t){.end = XIAOTAI_AI_TALK_END_LOCAL});
    send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 1);
    assert(fixture.kind == XIAOTAI_VIEW_CLOCK);
    state(active(XIAOTAI_AI_TALK_CONNECTING, 34, 0, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    assert(fixture.kind == XIAOTAI_VIEW_CLOCK);
    wake(XIAOTAI_LOCAL_WAKE_STARTED, 34, XIAOTAI_OK);
    assert(fixture.kind == XIAOTAI_VIEW_CLOCK && !strcmp(fixture.status, "在呢。"));
    assert(xiaotai_run(quiesce_roundtrip, NULL) == XIAOTAI_OK);
    flush();
    state(active(XIAOTAI_AI_TALK_CONNECTING, 34, 474, XIAOTAI_AI_TALK_ORIGIN_WAKE));
    media(status, 474);
    assert(ack_count() == effects + 2U);
}

static void session_cases(void)
{
    boot(false, false);
    start_error = XIAOTAI_ERR_PLATFORM_OFFLINE;
    session_action("talk.start", 0);
    assert(starts == 1 && fixture.last_refusal.value == XIAOTAI_ERR_PLATFORM_OFFLINE);
    assert(!strcmp(fixture.toast, "设备尚未就绪，请稍候") && fixture.toast_ms == 2400);
    start_error = XIAOTAI_OK;
    press(XIAOTAI_ROLE_PRIMARY);
    assert(starts == 2 && status.session && stops == 0 && status.phase == XIAOTAI_AI_TALK_CONNECTING);
    const uint32_t old = status.session;
    state(active(XIAOTAI_AI_TALK_LISTENING, old, 113, XIAOTAI_AI_TALK_ORIGIN_USER));
    assert(!(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));

    session_action("talk.submit", old);
    assert(submits == 1 && last_session == old);
    session_action("talk.submit", old);
    assert(submits == 1);
    at(2500);
    session_action("talk.submit", old);
    assert(submits == 2);
    submit_error = XIAOTAI_FAIL;
    state(active(XIAOTAI_AI_TALK_THINKING, old, 113, XIAOTAI_AI_TALK_ORIGIN_USER));
    session_action("talk.submit", old);
    assert(submits == 3 && fixture.last_refusal.value == XIAOTAI_FAIL);

    state(active(XIAOTAI_AI_TALK_SPEAKING, old, 113, XIAOTAI_AI_TALK_ORIGIN_USER));
    session_action("talk.interrupt", old);
    assert(interrupts == 1 && last_session == old);
    caption_update(old, 1, 1, XIAOTAI_AI_TALK_SPEAKER_AI, "开心", "first answer");
    assert(!strcmp(fixture.text, "first answer") && !strcmp(fixture.emotion, "happy"));
    assert(fixture.speaker == XIAOTAI_SPEAKER_ASSISTANT);
    caption_update(old, 2, 2, XIAOTAI_AI_TALK_SPEAKER_USER, "", "second question");
    assert(!strcmp(fixture.text, "second question") && fixture.speaker == XIAOTAI_SPEAKER_USER);
    caption_update(old, 3, 3, XIAOTAI_AI_TALK_SPEAKER_AI, "", "second answer");
    assert(!strcmp(fixture.text, "second answer") && !strcmp(fixture.emotion, "neutral"));

    const uint32_t newer = old + 1U;
    const unsigned updates = fixture.invalidations;
    state(active(XIAOTAI_AI_TALK_SPEAKING, newer, 114, XIAOTAI_AI_TALK_ORIGIN_USER));
    assert(fixture.invalidations > updates);
    assert(strcmp(fixture.text, "second answer"));
    caption_update(old, 4, 4, XIAOTAI_AI_TALK_SPEAKER_AI, "开心", "stale answer");
    assert(strcmp(fixture.text, "stale answer"));
    const unsigned rejected = fixture.refused;
    session_action("talk.stop", old);
    assert(fixture.refused == rejected + 1U && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE);
    assert(status.session == newer && stops == 0);
    session_action("talk.stop", newer);
    assert(stops == 1 && last_session == newer && status.phase == XIAOTAI_AI_TALK_IDLE);
    assert(strcmp(fixture.status, "连接失败，请重新开始"));
}

static void caption_prefix_case(bool keys)
{
    prefix_captions = true;
    boot(keys, false);
    if (keys) send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "talk", 0);
    uint32_t session = keys ? status.session : 31;
    state(active(XIAOTAI_AI_TALK_SPEAKING, session, 113, XIAOTAI_AI_TALK_ORIGIN_USER));
    assert(fixture.kind == (keys ? XIAOTAI_VIEW_TALK : XIAOTAI_VIEW_HOME) && !fixture.text[0]);

    caption_update(session, 1, 1, XIAOTAI_AI_TALK_SPEAKER_AI, "开心", "你好");
    assert(!strcmp(fixture.text, "AI：你好") && !strcmp(fixture.emotion, "happy"));
    assert(fixture.speaker == XIAOTAI_SPEAKER_ASSISTANT && !strcmp(caption.text, "你好"));
    caption_update(session, 2, 2, XIAOTAI_AI_TALK_SPEAKER_USER, "", "再说一次");
    assert(!strcmp(fixture.text, "你：再说一次") && fixture.speaker == XIAOTAI_SPEAKER_USER);
    assert(!strcmp(caption.text, "再说一次"));
    caption_update(session, 3, 3, XIAOTAI_AI_TALK_SPEAKER_AI, "开心", "");
    assert(!strcmp(fixture.text, "AI：") && fixture.speaker == XIAOTAI_SPEAKER_ASSISTANT);
    caption_update(session, 4, 4, XIAOTAI_AI_TALK_SPEAKER_NONE, "", "未标记");
    assert(!strcmp(fixture.text, "未标记") && fixture.speaker == XIAOTAI_SPEAKER_NONE);
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
    assert(xiaotai_run(local_hint, &session) == XIAOTAI_OK);
    flush();
    assert(!strcmp(fixture.text, "AI：没有找到联系人") && fixture.speaker == XIAOTAI_SPEAKER_ASSISTANT);
    assert(!strcmp(fixture.emotion, "confused") && !strcmp(caption.text, "未标记"));
    caption_update(session, 5, 5, XIAOTAI_AI_TALK_SPEAKER_AI, "", "新的字幕");
    assert(!strcmp(fixture.text, "AI：新的字幕"));
#endif

    state(active(XIAOTAI_AI_TALK_SPEAKING, session + 1U, 114, XIAOTAI_AI_TALK_ORIGIN_USER));
    assert(!fixture.text[0]);
    caption_update(session, 6, 6, XIAOTAI_AI_TALK_SPEAKER_AI, "", "旧会话");
    assert(!fixture.text[0]);
}

static void check_hold(void *unused)
{
    (void)unused;
    const char *name = NULL;
    assert(xiaotai_activity_current(&name) && name && !strcmp(name, XIAOTAI_APP_COMPANION_ACTIVITY));
}

static void keys_cases(bool hold)
{
    boot(true, hold);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "talk", 0);
    assert(!strcmp(fixture.id, "talk") && fixture.kind == XIAOTAI_VIEW_TALK);
    assert(starts == 1 && status.phase == XIAOTAI_AI_TALK_CONNECTING);
    const uint32_t id = status.session;
    assert(!action(XIAOTAI_ROLE_AUX));
    state(active(XIAOTAI_AI_TALK_SPEAKING, id, 114, XIAOTAI_AI_TALK_ORIGIN_USER));
    assert(fixture.timer_mode == XIAOTAI_TIMER_NONE);
    const xiaotai_view_action_t *aux = action(XIAOTAI_ROLE_AUX);
    assert(aux && (uint32_t)aux->value == id);
    press(XIAOTAI_ROLE_AUX);
    assert(interrupts == 1 && last_session == id);
    aux = action(XIAOTAI_ROLE_AUX);
    assert(aux && (aux->state & XIAOTAI_STATE_BUSY));
    const xiaotai_view_action_t *back = action(XIAOTAI_ROLE_BACK);
    assert(back && (uint32_t)back->value == id && !strcmp(back->intent, XIAOTAI_INTENT_APP_COMPANION_END));
    press(XIAOTAI_ROLE_BACK);
    assert(stops == 1 && last_session == id && !strcmp(fixture.id, "home"));
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "talk", 0);
    assert(starts == 2 && status.phase == XIAOTAI_AI_TALK_CONNECTING);
    const uint32_t ended = status.session;
    state((xiaotai_ai_talk_status_t){.end = XIAOTAI_AI_TALK_END_TIMEOUT, .error = XIAOTAI_ERR_TIMEOUT});
    assert(!strcmp(fixture.id, "result") && xiaotai_activity_active() == hold);
    if (hold) {
        assert(xiaotai_run(check_hold, NULL) == XIAOTAI_OK);
        flush();
    }
    const unsigned rejected = fixture.refused;
    send(XIAOTAI_INTENT_APP_COMPANION_END, NULL, ended);
    assert(fixture.refused == rejected + 1U && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE);
    assert(!strcmp(fixture.id, "result") && xiaotai_activity_active() == hold);
    at(6000);
    assert(!strcmp(fixture.id, "result"));
    press(XIAOTAI_ROLE_PRIMARY);
    assert(!strcmp(fixture.id, "home") && !xiaotai_activity_active());

    static const xiaotai_ai_talk_phase_t owned[] = {
        XIAOTAI_AI_TALK_CONNECTING, XIAOTAI_AI_TALK_LISTENING, XIAOTAI_AI_TALK_THINKING, XIAOTAI_AI_TALK_SPEAKING,
    };
    for (size_t i = 0; i < sizeof(owned) / sizeof(*owned); ++i) {
        send(XIAOTAI_INTENT_APP_COMPANION_SHOW, "talk", 0);
        const uint32_t current = status.session;
        assert(current && !strcmp(fixture.id, "talk"));
        if (owned[i] != XIAOTAI_AI_TALK_CONNECTING) state(active(owned[i], current, 114, XIAOTAI_AI_TALK_ORIGIN_USER));
        back = action(XIAOTAI_ROLE_BACK);
        assert(back && (uint32_t)back->value == current && !strcmp(back->icon, XIAOTAI_ICON_BACK));
        const unsigned before = stops;
        send(XIAOTAI_INTENT_APP_COMPANION_END, NULL, current + 1U);
        assert(stops == before && status.session == current);
        press(XIAOTAI_ROLE_BACK);
        assert(stops == before + 1U && last_session == current && !strcmp(fixture.id, "home"));
    }
    puts("AI Back: connecting/listening/thinking/speaking stop only the owning generation");
}

#ifdef COMPANION_TEST_REAL_INTERACTION
static void poll_key(void *ctx) { interaction_poll(*(const uint32_t *)ctx); }

static void key_tick(uint32_t tick, uint32_t edge)
{
    key_edges = edge;
    uint32_t time = 1000U + tick * XIAOTAI_INPUT_POLL_MS;
    atomic_store(&now, (int64_t)time * INT64_C(1000));
    assert(xiaotai_run(poll_key, &time) == XIAOTAI_OK);
    flush();
}

static void main_key_case(void)
{
    boot(false, false);
    xiaotai_interaction_input_t key;
    assert(xiaotai_interaction_input(0, &key) == XIAOTAI_OK);

    // Consume the stable edges from board_szpi_button's legacy waveform.
    for (uint32_t tick = 1; tick <= (main_press ? 259U : 65U); ++tick) {
        const bool down = tick == 32U || tick == 66U || tick == 90U;
        const bool up = tick == 55U || tick == 78U || tick == 246U;
        key_tick(tick, down ? XIAOTAI_INPUT_PRESSED : up ? XIAOTAI_INPUT_RELEASED : 0);
        if (!main_press) {
            assert(starts == (tick >= 55U) && stops == 0U);
            continue;
        }
        assert(starts == (tick >= 32U));
        assert(stops == (unsigned)(tick >= 66U) + (unsigned)(tick >= 90U));
        if (tick == 32U) {
            assert(status.phase == XIAOTAI_AI_TALK_CONNECTING && status.session);
            state(active(XIAOTAI_AI_TALK_LISTENING, status.session, 101, XIAOTAI_AI_TALK_ORIGIN_USER));
        } else if (tick == 66U) {
            assert(status.phase == XIAOTAI_AI_TALK_IDLE && last_session == serial);
            state(active(XIAOTAI_AI_TALK_CONNECTING, ++serial, 0, XIAOTAI_AI_TALK_ORIGIN_USER));
        } else if (tick == 90U) {
            assert(status.phase == XIAOTAI_AI_TALK_IDLE && last_session == serial);
        }
    }
    if (!main_press) {
        // CoreS3's PMIC provides a ready CLICK, not GPIO edges.
        key_tick(66U, XIAOTAI_INPUT_CLICK);
        key_tick(67U, 0);
        assert(starts == 1U && stops == 1U && status.phase == XIAOTAI_AI_TALK_IDLE);
    }
}
#endif

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
#ifdef COMPANION_TEST_REAL_INTERACTION
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "click"))) {
        main_press = argc == 1;
        main_key_case();
        xiaotai_interaction_reset();
    } else
#endif
    if (argc == 1) wake_cases();
    else if (!strcmp(argv[1], "session")) session_cases();
    else if (!strcmp(argv[1], "wake_admission")) wake_admission_case();
    else if (!strcmp(argv[1], "caption_prefix")) caption_prefix_case(false);
    else if (!strcmp(argv[1], "caption_prefix_keys")) caption_prefix_case(true);
    else if (!strcmp(argv[1], "talk_keys")) keys_cases(false);
    else { assert(!strcmp(argv[1], "talk_keys_hold")); keys_cases(true); }
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
