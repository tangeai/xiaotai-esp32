// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>

#include "xiaotai/modules/stream.h"
#include "xiaotai/port/os.h"

#define UI_STOP "companion.stream.stop"

static uint32_t shown_generation;
static int64_t shown_since_us;

static uint32_t active_stream(xiaotai_stream_status_t *stream)
{
    xiaotai_stream_status(stream);
    return stream->phase == XIAOTAI_STREAM_ACTIVE ? stream->generation : 0;
}

static bool follows_stream(void)
{
    return companion.has_display && (companion.style->policy & XIAOTAI_APP_COMPANION_FOLLOW_SESSIONS);
}

static bool on_stream_view(void)
{
    return companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME) ||
           companion_on_page(XIAOTAI_APP_COMPANION_PAGE_STREAM);
}

static bool pending(uint32_t generation)
{
    return companion_session_pending(XIAOTAI_INTENT_STREAM_STOP, generation);
}

static const char *stream_text(const xiaotai_stream_status_t *stream)
{
    if (pending(stream->generation)) return "正在结束";
    return stream->video_declared ? "远程查看中" : "远程对讲中";
}

static xiaotai_view_action_t stop_action(const xiaotai_stream_status_t *stream, bool back)
{
    return (xiaotai_view_action_t){
        .role = back ? XIAOTAI_ROLE_BACK : XIAOTAI_ROLE_AUX,
        .icon = back ? XIAOTAI_ICON_BACK : XIAOTAI_ICON_STOP,
        .label = back ? NULL : stream->video_declared ? "结束查看" : "结束对讲",
        .intent = UI_STOP, .value = (int32_t)stream->generation,
        .state = pending(stream->generation) ? XIAOTAI_STATE_BUSY :
                 back ? 0 : XIAOTAI_STATE_TAP_LOCAL_REJECTION,
    };
}

static bool page_arg(int32_t arg)
{
    xiaotai_stream_status_t stream;
    return arg == 0 && shown_generation && shown_generation == active_stream(&stream);
}

static void build_stream(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    view->kind = XIAOTAI_VIEW_NOTICE;
    xiaotai_stream_status_t stream;
    if (!shown_generation || active_stream(&stream) != shown_generation) return;
    const bool ending = pending(stream.generation);
    view->text = !ending && companion.config.stream_active_text ?
        companion.config.stream_active_text : stream_text(&stream);
    if (!ending && companion.config.stream_show_elapsed) {
        const int64_t now = xiaotai_time_us();
        const int64_t seconds = now > shown_since_us ? (now - shown_since_us) / INT64_C(1000000) : 0;
        view->timer_s = seconds > INT32_MAX ? INT32_MAX : (int32_t)seconds;
        view->timer_mode = XIAOTAI_TIMER_ELAPSED;
    }
    companion.buffer->actions[0] = stop_action(&stream, true);
    view->actions = companion.buffer->actions;
    view->action_count = 1;
}

static bool home_session(companion_home_session_t *session)
{
    xiaotai_stream_status_t stream;
    if (!shown_generation || active_stream(&stream) != shown_generation) return false;
    *session = (companion_home_session_t){.mode = stream.video_declared ?
        COMPANION_HOME_STREAM_VIEW : COMPANION_HOME_STREAM_AUDIO};
    return true;
}

static void decorate(xiaotai_view_t *view)
{
    if (view->kind != XIAOTAI_VIEW_HOME || strcmp(view->id, XIAOTAI_APP_COMPANION_PAGE_HOME)) return;
    xiaotai_stream_status_t stream;
    if (!shown_generation || active_stream(&stream) != shown_generation) return;
    view->flags &= ~(uint32_t)XIAOTAI_VIEW_MAY_SLEEP;
    view->text = NULL;
    if (pending(stream.generation) && !(companion.style->policy & XIAOTAI_APP_COMPANION_HOME_NETWORK_STATUS) &&
        !xiaotai_quiesced() && !companion_save_failure_home()) view->status = stream_text(&stream);
    if (view->actions == companion.buffer->actions && view->action_count < XIAOTAI_VIEW_ACTIONS_MAX)
        companion.buffer->actions[view->action_count++] = stop_action(&stream, false);
}

static xiaotai_err_t perform(uint32_t generation)
{
    xiaotai_stream_status_t stream;
    if (!generation || active_stream(&stream) != generation) return XIAOTAI_ERR_INVALID_STATE;
    if (pending(generation)) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_stream_stop(generation);
    if (err == XIAOTAI_OK) {
        companion_session_accepted(XIAOTAI_INTENT_STREAM_STOP, generation);
        if (on_stream_view()) companion_render();
    }
    return err;
}

static xiaotai_err_t session(const xiaotai_args_t *args)
{
    if (strcmp(args->text, XIAOTAI_INTENT_STREAM_STOP)) return XIAOTAI_ERR_NOT_SUPPORTED;
    return perform((uint32_t)args->value);
}

static xiaotai_err_t end(uint32_t generation)
{
    xiaotai_stream_status_t stream;
    const uint32_t active = active_stream(&stream);
    if (!active || (generation && active != generation)) return XIAOTAI_ERR_NOT_SUPPORTED;
    return perform(active);
}

static void ui_stop(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!on_stream_view()) {
        ++companion.status.stale;
        return;
    }
    xiaotai_args_t request = {.value = args->value, .text = XIAOTAI_INTENT_STREAM_STOP};
    companion_session_dispatch(&request);
}

static void present(uint32_t generation)
{
    const bool changed = shown_generation != generation;
    shown_generation = generation;
    if (changed) shown_since_us = xiaotai_time_us();
    companion_session_context(XIAOTAI_APP_COMPANION_CONTEXT_SESSION, generation);
    if (!companion.status.foreground || companion.status.setup) return;
    if (changed) {
        companion_feedback_session(COMPANION_FEEDBACK_ACTIVE, generation);
        companion_home_interaction();
        companion_user_activity();
    }
    if (follows_stream() && !companion_on_page(XIAOTAI_APP_COMPANION_PAGE_STREAM)) {
        const xiaotai_err_t err = companion_show_page(XIAOTAI_APP_COMPANION_PAGE_STREAM, 0);
        if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_APP_COMPANION_PAGE_STREAM, err);
    } else if (on_stream_view()) companion_render();
}

static void leave(bool failed)
{
    const uint32_t previous = shown_generation;
    shown_generation = 0;
    /* A preempting session's owner may already have set its context. */
    if (!xiaotai_activity_current(NULL)) companion_session_context(NULL, 0);
    companion_feedback_session(failed ? COMPANION_FEEDBACK_FAILED : COMPANION_FEEDBACK_IDLE, previous);
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_STREAM)) (void)companion_reset_home(0);
    else (void)companion_remove_page(XIAOTAI_APP_COMPANION_PAGE_STREAM);
    companion_home_interaction();
    companion_user_activity();
}

static const xiaotai_stream_ended_t *ended_event(const xiaotai_event_t *event)
{
    const xiaotai_stream_ended_t *ended = event->data;
    return ended && event->size == sizeof(*ended) && ended->started &&
        shown_generation && ended->generation == shown_generation ? ended : NULL;
}

static bool claim(const xiaotai_event_t *event)
{
    xiaotai_stream_status_t stream;
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_STARTED)) {
        const uint32_t generation = (uint32_t)event->args.value;
        return !companion.status.setup && generation && active_stream(&stream) == generation;
    }
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_ENDED))
        return companion.status.foreground && ended_event(event) && !active_stream(&stream);
    return false;  /* Changes in admission alone do not present a session. */
}

static void stream_event(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    xiaotai_stream_status_t stream;
    const uint32_t active = active_stream(&stream);
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_STARTED)) {
        if (active && active == (uint32_t)event->args.value) present(active);
        return;
    }
    const xiaotai_stream_ended_t *ended = ended_event(event);
    if (!ended || active == ended->generation) return;
    if (active) return;  /* The new stream's own start event will present it. */
    const bool visible = on_stream_view();
    const bool failed = ended->error != XIAOTAI_OK || ended->end == XIAOTAI_STREAM_END_MEDIA ||
        ended->end == XIAOTAI_STREAM_END_LINK;
    leave(failed);
    if (visible && failed) {
        const xiaotai_err_t error = ended->error != XIAOTAI_OK ? ended->error : XIAOTAI_FAIL;
        /* Keep the deployed failed-session hold only on products that require it. */
        if (companion.config.hold_refuses_sessions && follows_stream() && !xiaotai_activity_current(NULL) &&
            companion_session_result("呼叫失败", true) == XIAOTAI_OK) {
            companion_report_refusal(XIAOTAI_INTENT_STREAM_STOP, error);
            return;
        }
        companion_refuse(XIAOTAI_INTENT_STREAM_STOP, error);
    }
    if (companion.status.foreground && !companion.status.setup && !xiaotai_activity_current(NULL) &&
        xiaotai_app_entry() == XIAOTAI_APP_ENTRY_PRESENT) (void)xiaotai_app_return();
}

static void foreground(bool visible)
{
    if (!visible) return;
    xiaotai_stream_status_t stream;
    const uint32_t active = active_stream(&stream);
    if (active) present(active);
    else if (shown_generation) leave(false);
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_STREAM_STARTED, stream_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_STREAM_ENDED, stream_event, NULL);
    if (err == XIAOTAI_OK && companion.has_display)
        err = xiaotai_intent_handle_ex(UI_STOP, ui_stop, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK && follows_stream()) {
        static const companion_page_t stream_page = {.id = XIAOTAI_APP_COMPANION_PAGE_STREAM, .title = "实时查看",
            .build = build_stream, .arg_valid = page_arg};
        err = companion_page_register(&stream_page);
    }
    return err;
}

static void reset(void)
{
    shown_generation = 0;
    shown_since_us = 0;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_stream = {
    .name = "stream", .init = init, .claim = claim, .foreground = foreground, .reset = reset,
    .decorate_view = decorate, .home_session = home_session, .session = session, .end = end,
};
