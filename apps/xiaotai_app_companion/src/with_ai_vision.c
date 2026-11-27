// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>
#include "xiaotai/modules/ai_vision.h"

#define TOGGLE_INTENT "companion.vision.toggle"

static bool offered(void)
{
    xiaotai_ai_vision_status_t status;
    xiaotai_ai_vision_status(&status);
    return status.state != XIAOTAI_AI_VISION_UNAVAILABLE;
}

/* Whoever talks to the device sees that it looks, and can stop it at once. */
static void decorate(xiaotai_view_t *view)
{
    if (!view->id || strcmp(view->id, XIAOTAI_APP_COMPANION_PAGE_HOME) || !companion_home_talking || !offered()) return;
    xiaotai_ai_vision_status_t status;
    xiaotai_ai_vision_status(&status);
    const bool looking = status.state == XIAOTAI_AI_VISION_RUNNING;
    if (view->action_count < XIAOTAI_VIEW_ACTIONS_MAX)
        companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
            .label = !status.enabled ? "看图已关" : looking ? "正在看" :
                     status.state == XIAOTAI_AI_VISION_FAILED ? "看不到" : "看图",
            .icon = status.enabled ? XIAOTAI_ICON_CAMERA : XIAOTAI_ICON_CAMERA_OFF,
            .intent = TOGGLE_INTENT, .state = looking ? XIAOTAI_STATE_CHECKED : 0};
    if (looking && view->indicator_count < XIAOTAI_VIEW_INDICATORS_MAX)
        companion.buffer->indicators[view->indicator_count++] =
            (xiaotai_view_indicator_t){.icon = XIAOTAI_ICON_CAMERA, .level = 1};
}

static void on_toggle(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) { ++companion.status.stale; return; }
    xiaotai_ai_vision_status_t status;
    xiaotai_ai_vision_status(&status);
    const xiaotai_err_t err = xiaotai_ai_vision_set_enabled(!status.enabled);
    if (err != XIAOTAI_OK) { companion_refuse(TOGGLE_INTENT, err); return; }
    companion_home_session_notice(status.enabled ? "AI 不再看画面" : "AI 能看到画面了", 2200);
    companion_render();
}

static void changed(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) companion_render();
}

static xiaotai_err_t init(void)
{
    if (!companion.has_display) return XIAOTAI_OK;
    xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_AI_VISION_STATE, changed, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(TOGGLE_INTENT, on_toggle, NULL, XIAOTAI_INTENT_LOCAL);
    return err;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_ai_vision = {
    .name = "ai_vision", .init = init, .decorate_view = decorate,
};
