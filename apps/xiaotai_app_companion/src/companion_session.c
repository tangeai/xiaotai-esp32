// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>

#include "xiaotai/modules/binding.h"
#include "xiaotai/port/os.h"

static struct {
    const char *context;
    uint32_t identity;
    const char *action;
    uint32_t revision, timer;
    int64_t pending_until;
    const char *result;
    void (*result_ack)(uint32_t);
    uint32_t result_token;
    uint32_t hold;
} s_session;

void companion_session_refresh_context(void)
{
    if (!companion.status.foreground) return;
    const char *context = s_session.context ? s_session.context : XIAOTAI_APP_COMPANION_CONTEXT_IDLE;
    if (companion.status.setup) context = companion_feedback_audio_error() && !xiaotai_quiesced() ?
        XIAOTAI_APP_COMPANION_CONTEXT_ERROR : XIAOTAI_APP_COMPANION_CONTEXT_SETUP;
    if (strcmp(companion.status.context, context)) {
        strcpy(companion.status.context, context);
        xiaotai_interaction_set_context(context);
    }
}

void companion_session_context(const char *context, uint32_t identity)
{
    if (s_session.identity != identity && companion.status.foreground && !companion.status.setup &&
        context && !strcmp(companion.status.context, context)) xiaotai_interaction_invalidate();
    s_session.context = context;
    s_session.identity = identity;
    companion_session_refresh_context();
}

bool companion_session_pending(const char *action, uint32_t revision)
{
    return s_session.action && s_session.revision == revision && !strcmp(s_session.action, action) &&
        xiaotai_time_us() < s_session.pending_until;
}

static void pending_expired(void *unused)
{
    (void)unused;
    s_session.timer = 0;
    s_session.action = NULL;
    companion_render();
}

void companion_session_accepted(const char *action, uint32_t revision)
{
    if (s_session.timer) {
        xiaotai_timer_cancel(s_session.timer);
        s_session.timer = 0;
    }
    s_session.action = action;
    s_session.revision = revision;
    s_session.pending_until = xiaotai_time_us() + XIAOTAI_APP_COMPANION_PENDING_MS * INT64_C(1000);
    (void)xiaotai_timer_start(XIAOTAI_APP_COMPANION_PENDING_MS, pending_expired, NULL, &s_session.timer);
}

void companion_session_dispatch(const xiaotai_args_t *args)
{
    if (!companion.status.foreground) { ++companion.status.stale; return; }
    xiaotai_err_t err = XIAOTAI_ERR_NOT_SUPPORTED;
    if (!strcmp(args->text, "retry") || (!args->text[0] && companion_setup_can_retry())) {
        err = args->value == 0 && companion_setup_can_retry() ? xiaotai_binding_retry() : XIAOTAI_ERR_INVALID_STATE;
    } else if (!companion.status.setup) {
        for (size_t i = 0; xiaotai_app_companion_parts[i] && err == XIAOTAI_ERR_NOT_SUPPORTED; ++i) {
            if (xiaotai_app_companion_parts[i]->session) err = xiaotai_app_companion_parts[i]->session(args);
        }
    } else err = XIAOTAI_ERR_BUSY;
    if (err == XIAOTAI_ERR_PLATFORM_OFFLINE && !strcmp(args->text, "talk.start") &&
        (companion.style->policy & XIAOTAI_APP_COMPANION_OFFLINE_GATE)) {
        companion_refuse_text(XIAOTAI_INTENT_APP_COMPANION_SESSION, err, "设备尚未就绪，请稍候");
    } else if (err != XIAOTAI_OK) {
        companion_refuse(XIAOTAI_INTENT_APP_COMPANION_SESSION, err);
    }
}

static void on_session(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    companion_session_dispatch(args);
}

void companion_session_result_left(void)
{
    const uint32_t hold = s_session.hold;
    s_session.hold = 0;
    s_session.result = NULL;
    s_session.result_ack = NULL;
    s_session.result_token = 0;
    companion.status.error_held = false;
    if (hold) xiaotai_activity_end(hold);
    if (s_session.context && !strcmp(s_session.context, XIAOTAI_APP_COMPANION_CONTEXT_ERROR))
        companion_session_context(NULL, 0);
}

static bool result_arg(int32_t arg)
{
    return arg == 0 && s_session.result;
}

static void hold_revoked(uint32_t id, xiaotai_revoke_t reason, void *unused)
{
    (void)unused;
    (void)reason;
    if (id != s_session.hold) return;
    s_session.hold = 0;
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_RESULT)) companion_back_page();
    companion_session_result_left();
}

static void build_result(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    const int32_t token = (int32_t)s_session.result_token;
    companion.buffer->actions[0] = (xiaotai_view_action_t){
        .role = XIAOTAI_ROLE_PRIMARY, .intent = XIAOTAI_INTENT_APP_COMPANION_END, .label = "确认", .value = token};
    companion.buffer->actions[1] = (xiaotai_view_action_t){
        .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_COMPANION_END, .icon = XIAOTAI_ICON_BACK, .value = token};
    view->kind = XIAOTAI_VIEW_NOTICE;
    view->text = s_session.result;
    view->actions = companion.buffer->actions;
    view->action_count = 2U;
}

xiaotai_err_t companion_session_result_with_ack(const char *text, bool failed, void (*ack)(uint32_t), uint32_t token)
{
    if (!companion.status.foreground || companion.status.setup) return XIAOTAI_ERR_INVALID_STATE;
    if (companion.config.hold_refuses_sessions && !s_session.hold) {
        const xiaotai_activity_t hold = {.name = XIAOTAI_APP_COMPANION_ACTIVITY, .revoked = hold_revoked};
        const xiaotai_err_t err = xiaotai_activity_begin(&hold, &s_session.hold);
        if (err != XIAOTAI_OK) return err;
    }
    s_session.result = text;  /* static presentation text */
    s_session.result_ack = ack;
    s_session.result_token = token;
    const xiaotai_err_t err = companion_show_page(XIAOTAI_APP_COMPANION_PAGE_RESULT, 0);
    if (err != XIAOTAI_OK) companion_session_result_left();
    else {
        companion.status.error_held = failed;
        companion_session_context(XIAOTAI_APP_COMPANION_CONTEXT_ERROR, token);
    }
    return err;
}

xiaotai_err_t companion_session_result(const char *text, bool failed)
{
    return companion_session_result_with_ack(text, failed, NULL, 0);
}

static void on_end(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!companion.status.foreground) { ++companion.status.stale; return; }
    if (companion.status.setup) { companion_refuse(XIAOTAI_INTENT_APP_COMPANION_END, XIAOTAI_ERR_BUSY); return; }
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_RESULT)) {
        if ((uint32_t)args->value != s_session.result_token) {
            companion_refuse(XIAOTAI_INTENT_APP_COMPANION_END, XIAOTAI_ERR_INVALID_STATE);
            return;
        }
        (void)companion_session_dismiss_result();
        return;
    }
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (!part->end) continue;
        const xiaotai_err_t err = part->end((uint32_t)args->value);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED) continue;
        if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_END, err);
        return;
    }
    if (args->value) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_END, XIAOTAI_ERR_INVALID_STATE);
    else companion_back_page();
}

xiaotai_err_t companion_session_dismiss_result(void)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_RESULT)) return XIAOTAI_OK;
    void (*const ack)(uint32_t) = s_session.result_ack;
    const uint32_t token = s_session.result_token;
    companion_session_result_left();
    if (xiaotai_app_entry() != XIAOTAI_APP_ENTRY_PRESENT || xiaotai_app_return() != XIAOTAI_OK)
        (void)companion_reset_home(0);
    if (ack) ack(token);
    return XIAOTAI_OK;
}

xiaotai_err_t companion_session_init(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_COMPANION_SESSION, on_session, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_COMPANION_END, on_end, NULL);
    static const companion_page_t result = {.id = XIAOTAI_APP_COMPANION_PAGE_RESULT, .title = "结果",
        .build = build_result, .arg_valid = result_arg};
    for (size_t i = 0; err == XIAOTAI_OK && companion.has_display && xiaotai_app_companion_parts[i]; ++i) {
        if (!xiaotai_app_companion_parts[i]->end) continue;
        err = companion_page_register(&result);
        break;
    }
    return err;
}

void companion_session_reset(void) { memset(&s_session, 0, sizeof(s_session)); }
