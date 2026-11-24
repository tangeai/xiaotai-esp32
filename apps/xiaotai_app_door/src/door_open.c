// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/ui.h"

#include <stdio.h>
#include <string.h>

/* How long the indoor station waits for the door station's answer. */
#define REMOTE_OPEN_MS 4000U
/* How long the door station shows that the door is open. */
#define STATION_OPENED_MS 5000U

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
    if (!door.foreground || door.setup || door.sender) return false;
    if (door.page == DOOR_HOME || door.page == DOOR_MONITOR) return true;
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    return door.page == DOOR_CALL && current.session && current.session == door.session &&
           current.state == XIAOTAI_CALL_ACTIVE;
}

static void new_token(void)
{
    do { dialog.token = xiaotai_generation_next(); } while (dialog.token <= 1U);
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
    new_token();
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

static void show_result_for(xiaotai_err_t error, const char *title, const char *reason, uint32_t duration_ms)
{
    const xiaotai_app_door_result_view_t result = {.opened = error == XIAOTAI_OK};
    const xiaotai_view_action_t back = {
        .label = "返回", .intent = XIAOTAI_INTENT_APP_DOOR_BACK, .role = XIAOTAI_ROLE_BACK,
    };
    const xiaotai_view_t view = {
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG,
        .id = XIAOTAI_APP_DOOR_OVERLAY_RESULT, .title = title,
        .text = reason, .actions = &back, .action_count = 1, .custom_data = &result, .custom_size = sizeof(result),
    };
    if (dialog.timer) xiaotai_timer_cancel(dialog.timer);
    dialog.timer = 0;
    door.overlay = DOOR_DIALOG_RESULT;
    xiaotai_view_show(&view);
    if (xiaotai_timer_start(duration_ms, result_expired, (void *)(uintptr_t)dialog.token, &dialog.timer) != XIAOTAI_OK) {
        dialog.timer = 0;
        xiaotai_view_toast("请点返回关闭结果", 0);
    }
}

static void finish(xiaotai_err_t error, const char *reason)
{
    ++door.opens;
    if (error == XIAOTAI_OK) ++door.opens_done;
    const xiaotai_args_t event = {.value = error};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_DOOR_OPENED, &event, NULL, 0);
    xiaotai_ui_activity();
    show_result_for(error, error == XIAOTAI_OK ? "门已打开" : "暂时无法开门", reason, XIAOTAI_TOAST_DEFAULT_MS);
}

/* ------------------------------------------------- asking the door station */

static void pending_expired(void *ctx)
{
    if (door.overlay != DOOR_DIALOG_PENDING || dialog.token != (uint32_t)(uintptr_t)ctx) return;
    dialog.timer = 0;
    finish(XIAOTAI_ERR_TIMEOUT, "门口机没有回应，请重试");
}

static void show_pending(void)
{
    const xiaotai_view_action_t cancel = {
        .label = "取消", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, .role = XIAOTAI_ROLE_BACK,
    };
    char subtitle[192];
    (void)snprintf(subtitle, sizeof(subtitle), "%s · 门锁", door_config.door_name);
    const xiaotai_view_t view = {
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG,
        .id = XIAOTAI_APP_DOOR_OVERLAY_OPEN, .title = "正在开门…", .subtitle = subtitle,
        .text = "已通知门口机开门，请稍候。", .actions = &cancel, .action_count = 1,
    };
    door.overlay = DOOR_DIALOG_PENDING;
    xiaotai_view_show(&view);
    if (xiaotai_timer_start(REMOTE_OPEN_MS, pending_expired, (void *)(uintptr_t)dialog.token, &dialog.timer) !=
        XIAOTAI_OK) {
        dialog.timer = 0;
        finish(XIAOTAI_ERR_NO_MEM, "门口机没有回应，请重试");
    }
}

/* An indoor station without a lock of its own asks the door station of the door call. */
static bool ask_station(xiaotai_err_t *error, const char **reason)
{
    if (door_lock_configured() || door.page != DOOR_CALL) return false;
    *error = xiaotai_call_send_message(door.session, XIAOTAI_APP_DOOR_MESSAGE_OPEN, NULL, 0);
    if (*error == XIAOTAI_OK) {
        xiaotai_ui_activity();
        show_pending();
        return true;
    }
    *reason = "开门指令没有发出，请重试";
    return false;
}

static void confirm(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!door.foreground) return;
    const uint32_t token = (uint32_t)args->value;
    if (door.overlay == DOOR_DIALOG_PENDING) {
        if (!token) door_overlay_close();   /* stop waiting; the request may still reach the door */
        return;
    }
    if (door.overlay != DOOR_DIALOG_OPEN) return;
    if (!token) {
        door_overlay_close();
        return;
    }
    if ((token != 1U && token != dialog.token) || dialog.session != door.session || !can_open()) return;
    xiaotai_err_t error = XIAOTAI_OK;
    const char *reason = NULL;
    if (!dialog.peer_selected) {
        error = XIAOTAI_ERR_NOT_FOUND;
        reason = "尚未选择门外机，未发送开门指令";
    } else if (dialog.peer_revision != door.peer_revision) {
        error = XIAOTAI_ERR_INVALID_STATE;
        reason = "门外机已变更，未发送开门指令";
    } else if (ask_station(&error, &reason)) {
        return;
    } else if (!reason && !door_lock_configured()) {
        error = XIAOTAI_ERR_NOT_SUPPORTED;
        reason = "门锁尚未配置，未发送开门指令";
    } else if (!reason) {
        error = door_lock_request();
        reason = error == XIAOTAI_OK ? NULL : "门锁未受理，未发送开门指令";
    }
    finish(error, reason);
}

/* ------------------------------------------------------ the door station */

static void answer(uint32_t session, xiaotai_err_t error)
{
    const uint32_t value = (uint32_t)error;
    const uint8_t data[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    (void)xiaotai_call_send_message(session, XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT, data, sizeof(data));
}

static void station_open(uint32_t session)
{
    const bool lock = door_lock_configured();
    const xiaotai_err_t error = lock ? door_lock_request() :
                                door_config.open_on_screen ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
    answer(session, error);
    ++door.opens_received;
    const xiaotai_args_t event = {.value = error};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_DOOR_OPENED, &event, NULL, 0);
    if (error != XIAOTAI_OK || !door.foreground) return;
    if (door.overlay) {
        const door_dialog_t closed = door.overlay;
        door.overlay = DOOR_DIALOG_NONE;
        if (closed == DOOR_DIALOG_VOLUME) door_volume_commit();
        xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, NULL);
    }
    new_token();
    xiaotai_ui_activity();
    show_result_for(XIAOTAI_OK, "门已打开", lock ? "欢迎光临，请推门进入" : "欢迎光临，请进（演示，未接门锁）",
                    STATION_OPENED_MS);
}

void door_on_message(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_call_message_t *m = event->size == sizeof(*m) ? event->data : NULL;
    if (!m || !m->session || m->session != door.session || !door.classified_door) return;
    if (m->type == XIAOTAI_APP_DOOR_MESSAGE_OPEN && door.sender) {
        station_open(m->session);
    } else if (m->type == XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT && !door.sender &&
               door.overlay == DOOR_DIALOG_PENDING && m->size == 4) {
        const xiaotai_err_t error = (xiaotai_err_t)((uint32_t)m->data[0] | (uint32_t)m->data[1] << 8 |
                                                    (uint32_t)m->data[2] << 16 | (uint32_t)m->data[3] << 24);
        if (dialog.timer) xiaotai_timer_cancel(dialog.timer);
        dialog.timer = 0;
        finish(error, error == XIAOTAI_OK ? NULL : error == XIAOTAI_ERR_NOT_SUPPORTED ?
               "门口机没有接门锁" : "门口机未能开门，请重试");
    }
}

void door_open_call_ended(void)
{
    if (door.overlay == DOOR_DIALOG_PENDING && dialog.timer) {
        xiaotai_timer_cancel(dialog.timer);
        dialog.timer = 0;
    }
}

xiaotai_err_t door_init_open(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_OPEN, open_dialog, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, confirm, NULL);
    return err;
}
