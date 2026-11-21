// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/ui.h"

#include <stdio.h>

typedef struct {
    uint32_t token, timer, session, peer_revision;
    bool peer_selected;
} dialog_t;
static dialog_t dialog;

void door_overlay_reset(void)
{
    if (dialog.timer) xiaotai_timer_cancel(dialog.timer);
    dialog = (dialog_t){0};
}

void door_overlay_close(void)
{
    if (!door.overlay) return;
    const door_dialog_t closed = door.overlay;
    door.overlay = DOOR_DIALOG_NONE;
    if (closed == DOOR_DIALOG_VOLUME) door_volume_commit();
    door_overlay_reset();
    xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, NULL);
    door_render();
}

static bool can_open(void)
{
    if (!door.foreground || door.setup) return false;
    if (door.page == DOOR_HOME || door.page == DOOR_MONITOR) return true;
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    return door.page == DOOR_CALL && current.session && current.session == door.session &&
           current.state == XIAOTAI_CALL_ACTIVE;
}

static void open_dialog(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (door.overlay || !can_open()) return;
    if (args->value && (door.page != DOOR_CALL || (uint32_t)args->value != door.session)) return;
    door.overlay = DOOR_DIALOG_OPEN;
    dialog.session = door.session;
    dialog.peer_revision = door.peer_revision;
    dialog.peer_selected = door.peer[0] != '\0';
    do { dialog.token = xiaotai_generation_next(); } while (dialog.token <= 1U);
    char subtitle[192];
    (void)snprintf(subtitle, sizeof(subtitle), "%s · 门锁", door_config.door_name);
    const xiaotai_view_action_t actions[] = {
        {.label = "取消", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, .role = XIAOTAI_ROLE_BACK},
        {.label = "确认开门", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, .style = XIAOTAI_STYLE_POSITIVE,
         .role = XIAOTAI_ROLE_PRIMARY, .value = (int32_t)dialog.token},
    };
    const xiaotai_view_t view = {
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG,
        .id = XIAOTAI_APP_DOOR_OVERLAY_OPEN, .title = "确认开门？", .subtitle = subtitle,
        .text = "开门后，门口访客将可进入。", .actions = actions, .action_count = 2,
    };
    door_render(); // The stream stop action disappears under the overlay.
    xiaotai_ui_activity();
    xiaotai_view_show(&view);
}

static void result_expired(void *ctx)
{
    if (door.overlay == DOOR_DIALOG_RESULT && dialog.token == (uint32_t)(uintptr_t)ctx) {
        dialog.timer = 0;
        door_overlay_close();
    }
}

static void show_result(xiaotai_err_t error, const char *reason)
{
    const xiaotai_app_door_result_view_t result = {.opened = error == XIAOTAI_OK};
    const xiaotai_view_action_t back = {
        .label = "返回", .intent = XIAOTAI_INTENT_APP_DOOR_BACK, .role = XIAOTAI_ROLE_BACK,
    };
    const xiaotai_view_t view = {
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG,
        .id = XIAOTAI_APP_DOOR_OVERLAY_RESULT, .title = result.opened ? "门已打开" : "暂时无法开门",
        .text = reason, .actions = &back, .action_count = 1, .custom_data = &result, .custom_size = sizeof(result),
    };
    door.overlay = DOOR_DIALOG_RESULT;
    xiaotai_view_show(&view);
    if (xiaotai_timer_start(XIAOTAI_TOAST_DEFAULT_MS, result_expired, (void *)(uintptr_t)dialog.token,
                           &dialog.timer) != XIAOTAI_OK) {
        xiaotai_view_toast("请点返回关闭结果", 0);
    }
}

static void confirm(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!door.foreground || door.overlay != DOOR_DIALOG_OPEN) return;
    const uint32_t token = (uint32_t)args->value;
    if (!token) {
        door_overlay_close();
        return;
    }
    if ((token != 1U && token != dialog.token) || dialog.session != door.session || !can_open()) return;
    xiaotai_err_t error;
    const char *reason;
    if (!dialog.peer_selected) {
        error = XIAOTAI_ERR_NOT_FOUND;
        reason = "尚未选择门外机，未发送开门指令";
    } else if (dialog.peer_revision != door.peer_revision) {
        error = XIAOTAI_ERR_INVALID_STATE;
        reason = "门外机已变更，未发送开门指令";
    } else if (!door_lock_configured()) {
        error = XIAOTAI_ERR_NOT_SUPPORTED;
        reason = "门锁尚未配置，未发送开门指令";
    } else {
        error = door_lock_request();
        reason = error == XIAOTAI_OK ? NULL : "门锁未受理，未发送开门指令";
    }
    ++door.opens;
    if (error == XIAOTAI_OK) ++door.opens_done;
    const xiaotai_args_t event = {.value = error};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_DOOR_OPENED, &event, NULL, 0);
    xiaotai_ui_activity();
    show_result(error, reason);
}

xiaotai_err_t door_init_open(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_OPEN, open_dialog, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, confirm, NULL);
    return err;
}
