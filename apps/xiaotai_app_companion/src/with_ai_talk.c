// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "ai_device_tools.h"
#include "ai_expression_tool.h"
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
#include "ai_contact_tools.h"
#endif

#define START XIAOTAI_INTENT_TALK_START
#define STOP XIAOTAI_INTENT_TALK_STOP
#define INTERRUPT XIAOTAI_INTENT_TALK_INTERRUPT
#define SUBMIT XIAOTAI_INTENT_TALK_SUBMIT
#define UI_START "companion.talk.start"
#define UI_INTERRUPT "companion.talk.interrupt"
#define FAILURE_MS 3200U

static struct {
    xiaotai_ai_talk_phase_t phase;
    uint32_t session, revision;
    int64_t active_since_us;
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
    const char *hint;
    xiaotai_ai_talk_phase_t hint_phase;
    uint32_t hint_session, hint_caption_revision;
#endif
} s_talk_view;

/* An expression the AI asked for: it stands instead of the one of the words,
 * in that conversation only, until its timer runs out. */
static struct {
    const char *face;
    uint32_t session, timer;
} s_expression;

/* The service gives 中性 to a sentence that no other word fits: within an answer it does not take away a stronger face
 * that an earlier sentence brought. A new word changes it; the user's turn, or another answer, ends it. */
static struct {
    const char *face;
    uint32_t session, paragraph;
} s_answer;

static const char *answer_face(const xiaotai_ai_talk_caption_t *caption)
{
    const char *face = xiaotai_app_companion_emotion_face(caption->emotion);
    if (caption->speaker != XIAOTAI_AI_TALK_SPEAKER_AI) return face;
    const bool same = s_answer.face && s_answer.session == caption->session && s_answer.paragraph == caption->paragraph;
    if (strcmp(face, "neutral")) {
        s_answer.face = face;
        s_answer.session = caption->session;
        s_answer.paragraph = caption->paragraph;
    } else if (same) {
        face = s_answer.face;
    }
    return face;
}

/* Each sentence of an answer, told to a theme that times her face by what is heard (the character): the face it
 * shows and where it begins in the answer's voice. */
static struct {
    uint32_t session, sentences;
} s_said;

static void tell_sentence(int32_t begin_ms, const char *face)
{
    xiaotai_args_t args = {.value = begin_ms};
    const size_t length = strnlen(face, sizeof(args.text) - 1U);
    memcpy(args.text, face, length);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_COMPANION_SENTENCE, &args, NULL, 0);
}

static bool at_home(void) { return (companion.style->policy & XIAOTAI_APP_COMPANION_TALK_ON_HOME) != 0; }

static bool shows_face(void)
{
    return companion_on_page(XIAOTAI_APP_COMPANION_PAGE_TALK) || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME);
}

static bool active(xiaotai_ai_talk_phase_t phase)
{
    return phase >= XIAOTAI_AI_TALK_LISTENING && phase <= XIAOTAI_AI_TALK_SPEAKING;
}

static const char caption_ai_prefix[] = "AI：";
static const char caption_user_prefix[] = "你：";
enum { CAPTION_PREFIX_SPACE = sizeof(caption_user_prefix) - 1U };
_Static_assert(sizeof(caption_ai_prefix) <= sizeof(caption_user_prefix), "reserve the longest caption prefix");

static const char *caption_label(char *text, xiaotai_speaker_t speaker)
{
    const char *prefix = speaker == XIAOTAI_SPEAKER_ASSISTANT ? caption_ai_prefix :
                         speaker == XIAOTAI_SPEAKER_USER ? caption_user_prefix : NULL;
    if (!prefix) return text;
    const size_t size = strlen(prefix);
    /* The caller reserved space before the UTF-8 text in the render scratch. */
    return memcpy(text - size, prefix, size);
}

#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
void companion_ai_talk_hint(uint32_t session, const char *text)
{
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (status.session != session || status.phase < XIAOTAI_AI_TALK_LISTENING ||
        status.phase > XIAOTAI_AI_TALK_SPEAKING) return;
    s_talk_view.hint = text;
    s_talk_view.hint_session = session;
    s_talk_view.hint_phase = status.phase;
    s_talk_view.hint_caption_revision = status.caption_revision;
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_TALK) || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME))
        companion_render();
}
#endif

static companion_home_session_t presentation_of_words(const xiaotai_ai_talk_status_t *status)
{
    companion_home_session_t view = {0};
    switch (status->phase) {
        case XIAOTAI_AI_TALK_CONNECTING: view.mode = COMPANION_HOME_AI_CONNECT; break;
        case XIAOTAI_AI_TALK_ENDING:   /* the last sentence is still heard */
            view.mode = COMPANION_HOME_AI_STOP;
            view.phase = XIAOTAI_TALK_SPEAKING;
            break;
        case XIAOTAI_AI_TALK_LISTENING: view.phase = XIAOTAI_TALK_LISTENING; break;
        case XIAOTAI_AI_TALK_THINKING: view.phase = XIAOTAI_TALK_THINKING; break;
        case XIAOTAI_AI_TALK_SPEAKING: view.phase = XIAOTAI_TALK_SPEAKING; break;
        default: return view;
    }
    if (!view.mode) view.mode = COMPANION_HOME_AI_ACTIVE;
    const size_t label_space = companion.config.ai_caption_speaker_prefix ? CAPTION_PREFIX_SPACE : 0U;
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
    if (s_talk_view.hint && status->session == s_talk_view.hint_session && status->phase == s_talk_view.hint_phase &&
        status->caption_revision == s_talk_view.hint_caption_revision) {
        view.phase = XIAOTAI_TALK_LISTENING;
        view.speaker = XIAOTAI_SPEAKER_ASSISTANT;
        view.caption = s_talk_view.hint;
        view.emotion = "confused";
        if (label_space && companion.buffer) {
            const size_t hint_size = strlen(s_talk_view.hint) + 1U;
            if (hint_size <= sizeof(companion.buffer->details) - label_space) {
                char *text = companion.buffer->details + label_space;
                memcpy(text, s_talk_view.hint, hint_size);
                view.caption = caption_label(text, view.speaker);
            }
        }
        return view;
    }
#endif
    if (!status->session || !companion.buffer) return view;
    xiaotai_ai_talk_caption_t caption = {.text = companion.buffer->details + label_space,
        .capacity = sizeof(companion.buffer->details) - label_space};
    if (!xiaotai_ai_talk_caption_read(&caption) || caption.session != status->session) return view;
    view.emotion = answer_face(&caption);
    if (caption.speaker == XIAOTAI_AI_TALK_SPEAKER_USER) view.speaker = XIAOTAI_SPEAKER_USER;
    else if (caption.speaker == XIAOTAI_AI_TALK_SPEAKER_AI) view.speaker = XIAOTAI_SPEAKER_ASSISTANT;
    view.caption = label_space ? caption_label(caption.text, view.speaker) : caption.text;
    return view;
}

static companion_home_session_t presentation(const xiaotai_ai_talk_status_t *status)
{
    companion_home_session_t view = presentation_of_words(status);
    if (s_expression.face && status->session && status->session == s_expression.session && view.phase)
        view.emotion = s_expression.face;
    return view;
}

static void expression_over(void *unused)
{
    (void)unused;
    const uint32_t session = s_expression.session;
    memset(&s_expression, 0, sizeof(s_expression));
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (session && session == status.session && status.phase != XIAOTAI_AI_TALK_IDLE)
        tell_sentence(XIAOTAI_APP_COMPANION_SENTENCE_OWN, xiaotai_app_companion_emotion_face(presentation_of_words(&status).emotion));
    if (companion.status.foreground && shows_face()) companion_render();
}

xiaotai_err_t companion_expression_show(uint32_t session, const char *face, uint32_t ms)
{
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (!face || !session || session != status.session || status.phase == XIAOTAI_AI_TALK_IDLE ||
        !companion.has_display || !companion.status.foreground || !shows_face()) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_timer_cancel(s_expression.timer);
    memset(&s_expression, 0, sizeof(s_expression));
    const xiaotai_err_t err = xiaotai_timer_start(ms, expression_over, NULL, &s_expression.timer);
    if (err != XIAOTAI_OK) return err;
    s_expression.face = face;
    s_expression.session = session;
    tell_sentence(XIAOTAI_APP_COMPANION_SENTENCE_NOW, face);
    companion_render();
    return XIAOTAI_OK;
}

static bool home_session(companion_home_session_t *out)
{
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (status.phase == XIAOTAI_AI_TALK_IDLE) return false;
    *out = presentation(&status);
    return true;
}

static void show_talk(void)
{
    if (!companion.status.foreground || companion.status.setup || !companion.has_display) return;
    if (at_home()) {
        /* The conversation is shown on the home page: nothing stays over it. */
        companion_hide_wechat_code();
        if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) (void)companion_reset_home(0);
    } else if ((companion.style->policy & XIAOTAI_APP_COMPANION_FOLLOW_SESSIONS) &&
               !companion_on_page(XIAOTAI_APP_COMPANION_PAGE_TALK)) {
        const xiaotai_err_t err = companion_show_page(XIAOTAI_APP_COMPANION_PAGE_TALK, 0);
        if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_APP_COMPANION_PAGE_TALK, err);
    }
}

static void update(const xiaotai_ai_talk_status_t *status, bool terminal)
{
    const uint32_t previous = s_talk_view.session;
    const xiaotai_ai_talk_phase_t before = s_talk_view.phase;
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
    companion_ai_contact_tools_observe(status->session, status->link);
    if (s_talk_view.hint_session != status->session || s_talk_view.hint_phase != status->phase)
        s_talk_view.hint = NULL;
#endif
    if (status->session == previous && status->phase == before) return;
    if (companion.config.ai_talk_show_elapsed) {
        if (!status->session || !active(status->phase)) s_talk_view.active_since_us = 0;
        else if (status->session != previous || !active(before)) s_talk_view.active_since_us = xiaotai_time_us();
    }
    s_talk_view.session = status->session;
    s_talk_view.phase = status->phase;
    ++s_talk_view.revision;

    if (status->session) {
        companion_session_context(XIAOTAI_APP_COMPANION_CONTEXT_TALK, status->session);
        companion_feedback_session(COMPANION_FEEDBACK_TALK, status->session);
        if (!previous || previous != status->session) {
            companion_home_interaction();
            companion_user_activity();
            show_talk();
        }
    } else if (previous) {
        const bool visible = companion_on_page(at_home() ? XIAOTAI_APP_COMPANION_PAGE_HOME :
                                                           XIAOTAI_APP_COMPANION_PAGE_TALK);
        companion_session_context(NULL, 0);
        const bool failed = status->end >= XIAOTAI_AI_TALK_END_TIMEOUT;
        companion_feedback_session(failed ? COMPANION_FEEDBACK_FAILED : COMPANION_FEEDBACK_IDLE, previous);
        companion_home_interaction();
        companion_home_clear_session_notice();
        companion_user_activity();
        if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_TALK)) companion_back_page();
        if (terminal && failed && visible) {
            companion_report_refusal(XIAOTAI_APP_COMPANION_PAGE_TALK, status->error != XIAOTAI_OK ?
                                     status->error : XIAOTAI_FAIL);
            if (before == XIAOTAI_AI_TALK_CONNECTING && at_home()) {
                companion_home_failure("连接失败，请重新开始", FAILURE_MS);
            } else if (!at_home() && companion.has_display) {
                const xiaotai_err_t err = companion_session_result("连接失败", true);
                if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_APP_COMPANION_PAGE_TALK, err);
            }
        }
        if (terminal && companion_finish_presentation()) return;
    }
    companion_render();
}

static void state_event(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!event->data || event->size != sizeof(xiaotai_ai_talk_status_t)) return;
    const xiaotai_ai_talk_status_t *reported = event->data;
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (reported->started != status.started) return;
    if (reported->session != status.session) {
        /* A connect can already have ended before its queued notice is
         * read. Keep the predecessor for the subsequent terminal notice. */
        if (!status.session && reported->session &&
            (!s_talk_view.session || s_talk_view.session == reported->session)) {
            s_talk_view.session = reported->session;
            s_talk_view.phase = reported->phase;
        }
        return;
    }
    update(&status, true);
}

static void caption_event(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
    if (s_talk_view.hint_caption_revision != status.caption_revision) s_talk_view.hint = NULL;
#endif
    xiaotai_ai_talk_caption_t caption = {0};
    if (status.session && xiaotai_ai_talk_caption_read(&caption) && caption.session == status.session &&
        caption.speaker == XIAOTAI_AI_TALK_SPEAKER_AI && caption.sentences &&
        (caption.session != s_said.session || caption.sentences != s_said.sentences)) {
        s_said.session = caption.session;
        s_said.sentences = caption.sentences;
        /* Its words' face: an expression the AI asked for stands over it in the theme for as long as it lasts, from
         * where it is heard, not from where its caption came. */
        tell_sentence(caption.begin_ms, answer_face(&caption));
    }
    if (status.session && status.session == s_talk_view.session &&
        (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_TALK) || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)))
        companion_render();
}

static bool claim(const xiaotai_event_t *event)
{
    if (strcmp(event->name, XIAOTAI_EVENT_AI_TALK_STATE) || !event->data ||
        event->size != sizeof(xiaotai_ai_talk_status_t) || companion.status.setup) return false;
    const xiaotai_ai_talk_status_t *reported = event->data;
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    return status.session && status.session == reported->session;
}

static xiaotai_err_t perform(const char *action, uint32_t identity)
{
    xiaotai_ai_talk_status_t before;
    xiaotai_ai_talk_status(&before);
    const bool starting = !strcmp(action, START);
    if (starting) {
        if (identity) return XIAOTAI_ERR_INVALID_STATE;
        /* Tapping the face never stops a conversation; one that is ending makes room. */
        if (before.session && before.phase != XIAOTAI_AI_TALK_ENDING) return XIAOTAI_OK;
        if ((companion.style->policy & XIAOTAI_APP_COMPANION_OFFLINE_GATE) && !companion_home_network_up())
            return XIAOTAI_ERR_PLATFORM_OFFLINE;
    } else if (!identity || before.session != identity) {
        return XIAOTAI_ERR_INVALID_STATE;
    }
    update(&before, false);
    const uint32_t revision = s_talk_view.revision;
    if (companion_session_pending(action, revision)) return XIAOTAI_OK;
    xiaotai_err_t err = XIAOTAI_ERR_NOT_SUPPORTED;
    if (starting) err = xiaotai_ai_talk_start(NULL, NULL);
    else if (!strcmp(action, STOP)) err = xiaotai_ai_talk_stop(identity);
    else if (!strcmp(action, INTERRUPT)) err = xiaotai_ai_talk_interrupt(identity);
    else if (!strcmp(action, SUBMIT)) err = xiaotai_ai_talk_submit(identity);
    if (err == XIAOTAI_OK) {
        companion_session_accepted(action, revision);
        xiaotai_ai_talk_status_t after;
        xiaotai_ai_talk_status(&after);
        update(&after, true);
        if (before.session == after.session && before.phase == after.phase) companion_render();
    }
    return err;
}

static xiaotai_err_t session(const xiaotai_args_t *args)
{
    if (!strcmp(args->text, START)) return perform(START, (uint32_t)args->value);
    if (!strcmp(args->text, STOP)) return perform(STOP, (uint32_t)args->value);
    if (!strcmp(args->text, INTERRUPT)) return perform(INTERRUPT, (uint32_t)args->value);
    if (!strcmp(args->text, SUBMIT)) return perform(SUBMIT, (uint32_t)args->value);
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t main_action(int32_t action)
{
    if (!companion.status.foreground) return XIAOTAI_ERR_INVALID_STATE;
    if (action != 0 && action != 1) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (action == 0 && status.session) return perform(STOP, status.session);
    return perform(START, 0);
}

static xiaotai_err_t end_action(uint32_t identity)
{
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (!status.session || (identity && identity != status.session)) return XIAOTAI_ERR_NOT_SUPPORTED;
    return perform(STOP, status.session);
}

static void talk_intent(const xiaotai_args_t *args, void *action)
{
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_TALK)) { ++companion.status.stale; return; }
    if (!strcmp(action, INTERRUPT)) {
        xiaotai_ai_talk_status_t status;
        xiaotai_ai_talk_status(&status);
        if (status.phase != XIAOTAI_AI_TALK_SPEAKING || status.session != (uint32_t)args->value) {
            companion_refuse(XIAOTAI_INTENT_APP_COMPANION_SESSION, XIAOTAI_ERR_INVALID_STATE);
            return;
        }
    }
    xiaotai_args_t request = {.value = args->value};
    strcpy(request.text, action);
    companion_session_dispatch(&request);
}

static void home_start(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) { ++companion.status.stale; return; }
    companion_session_dispatch(&(const xiaotai_args_t){.text = START});
}

static void build_talk(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    const companion_home_session_t shown = presentation(&status);
    view->kind = XIAOTAI_VIEW_TALK;
    view->emotion = xiaotai_app_companion_emotion_face(shown.emotion);
    view->phase = shown.phase;
    view->level = companion_home_voice_level();
    view->text = shown.caption;
    view->speaker = shown.speaker;
    if (companion.config.ai_talk_show_elapsed && status.session && status.session == s_talk_view.session &&
        active(status.phase) && active(s_talk_view.phase)) {
        const int64_t now = xiaotai_time_us();
        const int64_t since = s_talk_view.active_since_us;
        const int64_t seconds = now > since ? (now - since) / INT64_C(1000000) : 0;
        view->timer_s = seconds > INT32_MAX ? INT32_MAX : (int32_t)seconds;
        view->timer_mode = XIAOTAI_TIMER_ELAPSED;
    }
    view->status = status.phase == XIAOTAI_AI_TALK_CONNECTING ? "正在连接" :
        status.phase == XIAOTAI_AI_TALK_ENDING ? "正在结束" :
        shown.phase == XIAOTAI_TALK_SPEAKING ? "正在回答" :
        shown.phase == XIAOTAI_TALK_THINKING || shown.phase == XIAOTAI_TALK_LISTENING ? "请说话" : "开始对话";
    xiaotai_view_action_t *actions = companion.buffer->actions;
    size_t count = 0;
    if (!status.session) {
        actions[count++] = (xiaotai_view_action_t){.role = XIAOTAI_ROLE_PRIMARY,
            .label = "开始对话", .intent = UI_START};
    } else if (status.phase == XIAOTAI_AI_TALK_SPEAKING) {
        actions[count++] = (xiaotai_view_action_t){.role = XIAOTAI_ROLE_AUX, .label = "打断",
            .intent = UI_INTERRUPT, .value = (int32_t)status.session,
            .state = companion_session_pending(INTERRUPT, s_talk_view.revision) ? XIAOTAI_STATE_BUSY : 0};
    }
    actions[count++] = (xiaotai_view_action_t){.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK,
        .intent = status.session ? XIAOTAI_INTENT_APP_COMPANION_END : XIAOTAI_INTENT_APP_COMPANION_BACK,
        .value = (int32_t)status.session};
    view->actions = actions;
    view->action_count = count;
}

static void enter_talk(void)
{
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    if (!status.session) companion_session_dispatch(&(const xiaotai_args_t){.text = START});
}

static void foreground(bool visible)
{
    if (!visible) return;
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    update(&status, false);
    if (status.session) show_talk();
}

static void reset(void)
{
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
    companion_ai_contact_tools_reset();
#endif
    memset(&s_talk_view, 0, sizeof(s_talk_view));
    xiaotai_timer_cancel(s_expression.timer);
    memset(&s_expression, 0, sizeof(s_expression));
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_AI_TALK_STATE, state_event, NULL);
#ifdef COMPANION_WITH_AI_CONTACT_TOOLS
    if (err == XIAOTAI_OK) err = companion_ai_contact_tools_init();
#else
    if (companion.config.ai_contact_tools != XIAOTAI_COMPANION_AI_CONTACT_TOOLS_DISABLED)
        return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
    if (err == XIAOTAI_OK) err = companion_ai_device_tools_init();
    if (err == XIAOTAI_OK && companion.has_display) err = companion_ai_expression_tool_init();
    if (err == XIAOTAI_OK && companion.has_display) {
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_AI_TALK_CAPTION, caption_event, NULL);
    }
    if (err == XIAOTAI_OK && companion.has_display && at_home()) {
        err = xiaotai_intent_handle_ex(COMPANION_HOME_START_INTENT, home_start, NULL, XIAOTAI_INTENT_LOCAL);
    } else if (err == XIAOTAI_OK && companion.has_display) {
        static const companion_page_t talk = {.id = XIAOTAI_APP_COMPANION_PAGE_TALK, .title = "AI 对话",
            .build = build_talk, .entered = enter_talk};
        err = companion_page_register(&talk);
        if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(UI_START, talk_intent, (void *)START, XIAOTAI_INTENT_LOCAL);
        if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(UI_INTERRUPT, talk_intent, (void *)INTERRUPT, XIAOTAI_INTENT_LOCAL);
    }
    if (err != XIAOTAI_OK) reset();
    return err;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_ai_talk = {
    .name = "ai_talk", .init = init, .claim = claim, .main = main_action, .end = end_action,
    .foreground = foreground, .reset = reset, .home_session = home_session, .session = session,
};
