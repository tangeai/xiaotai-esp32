// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/ui.h"

#include <stdio.h>
#include <string.h>

/* How long the indoor station waits for the door station's answer. */
#define REMOTE_OPEN_MS 4000U
/* How long the door station shows what became of it. */
#define STATION_OPENED_MS 5000U

typedef struct {
    uint32_t token, timer, session, peer_revision;
    bool peer_selected;
} dialog_t;
static dialog_t dialog;

/* The call in which the door station was asked and has not answered yet. */
static struct {
    uint32_t session;
    bool counted;       /* its failure was shown already */
} asked;

void door_overlay_reset(void)
{
    if (dialog.timer) xiaotai_timer_cancel(dialog.timer);
    dialog = (dialog_t){0};
}

void door_overlay_drop(void)
{
    if (!door.overlay) return;
    const door_dialog_t closed = door.overlay;
    door.overlay = DOOR_DIALOG_NONE;
    if (closed == DOOR_DIALOG_VOLUME) door_volume_commit();
    door_overlay_reset();
    xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, NULL);
}

void door_overlay_close(void)
{
    if (!door.overlay) return;
    door_overlay_drop();
    door_render();
}

static bool can_open(void)
{
    if (!door.foreground || door.setup || door.sender) return false;
    if (door.page == DOOR_HOME && !door_mode_known()) return false;
    if (door.page == DOOR_HOME || (door.page == DOOR_MONITOR && !door_look_running())) return true;
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    return door_on_call_page() && current.session && current.session == door.session &&
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
    if (args->value && (!door_on_call_page() || (uint32_t)args->value != door.session)) return;
    door.overlay = DOOR_DIALOG_OPEN;
    dialog.session = door.session;
    dialog.peer_revision = door.peer_revision;
    dialog.peer_selected = door.peer_count > 0;
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
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_OPENED,
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

static void note(xiaotai_err_t error)
{
    if (error == XIAOTAI_OK) {
        ++door.opens_done;
        const xiaotai_call_status_t *shown = door_shown_call();
        door_log_add(DOOR_LOG_OPENED, door.session && shown->session == door.session ? shown->peer_id : door.peers[0]);
    }
    const xiaotai_args_t event = {.value = error};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_DOOR_OPENED, &event, NULL, 0);
}

static void show_result(xiaotai_err_t error, const char *reason)
{
    xiaotai_ui_activity();
    show_result_for(error, error == XIAOTAI_OK ? "门已打开" : "暂时无法开门", reason, XIAOTAI_TOAST_DEFAULT_MS);
}

static void finish(xiaotai_err_t error, const char *reason)
{
    ++door.opens;
    note(error);
    show_result(error, reason);
}

/* ------------------------------------------------- asking the door station */

static void pending_expired(void *ctx)
{
    if (door.overlay != DOOR_DIALOG_PENDING || dialog.token != (uint32_t)(uintptr_t)ctx) return;
    dialog.timer = 0;
    asked.counted = true;
    finish(XIAOTAI_ERR_TIMEOUT, "室外机没有回应，请重试");
}

static void show_pending(void)
{
    const xiaotai_view_action_t cancel = {
        .label = "不等了", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, .role = XIAOTAI_ROLE_BACK,
    };
    char subtitle[192];
    (void)snprintf(subtitle, sizeof(subtitle), "%s · 门锁", door_config.door_name);
    const xiaotai_view_t view = {
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG,
        .id = XIAOTAI_APP_DOOR_OVERLAY_OPEN, .title = "正在开门…", .subtitle = subtitle,
        .text = "已通知室外机开门，请稍候。", .actions = &cancel, .action_count = 1,
    };
    door.overlay = DOOR_DIALOG_PENDING;
    xiaotai_view_show(&view);
    if (xiaotai_timer_start(REMOTE_OPEN_MS, pending_expired, (void *)(uintptr_t)dialog.token, &dialog.timer) !=
        XIAOTAI_OK) {
        dialog.timer = 0;
        finish(XIAOTAI_ERR_NO_MEM, "室外机没有回应，请重试");
    }
}

/* An indoor station without a lock of its own asks the door station of the door call. */
static bool ask_station(xiaotai_err_t *error, const char **reason)
{
    if (door_lock_configured() || !door_on_call_page()) return false;
    *error = xiaotai_call_send_message(door.session, XIAOTAI_APP_DOOR_MESSAGE_OPEN, NULL, 0);
    if (*error == XIAOTAI_OK) {
        asked.session = door.session;
        asked.counted = false;
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
        reason = "尚未添加室外机，未发送开门指令";
    } else if (dialog.peer_revision != door.peer_revision) {
        error = XIAOTAI_ERR_INVALID_STATE;
        reason = "室外机已变更，未发送开门指令";
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
    /* A device the station called from its directory, but did not choose, opens
     * nothing. Whom the access machine calls by the room lives there. */
    const bool allowed = door_peer_slot(door_shown_call()->peer_id) >= 0 || (door.access && session == door.dialed_session);
    const xiaotai_err_t error = !allowed ? XIAOTAI_ERR_NOT_ALLOWED : lock ? door_lock_request() :
                                door_config.open_on_screen ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
    answer(session, error);
    ++door.opens_received;
    const xiaotai_args_t event = {.value = error};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_APP_DOOR_OPENED, &event, NULL, 0);
    if (error == XIAOTAI_OK) door_sound_opened();
    if (!door.foreground) return;
    door_overlay_drop();
    new_token();
    xiaotai_ui_activity();
    show_result_for(error, error == XIAOTAI_OK ? "门已打开，请进" : "门没有打开",
                    error == XIAOTAI_OK ? (lock ? "请推门进入" : "演示：本机没有接门锁") :
                    error == XIAOTAI_ERR_NOT_ALLOWED ? "对方没有开门的权限" :
                    error == XIAOTAI_ERR_NOT_SUPPORTED ? "本机没有接门锁" : "门锁没有受理，请重试", STATION_OPENED_MS);
}

static const char *refusal(xiaotai_err_t error)
{
    return error == XIAOTAI_OK ? NULL : error == XIAOTAI_ERR_NOT_SUPPORTED ? "室外机没有接门锁" :
           error == XIAOTAI_ERR_NOT_ALLOWED ? "室外机没有把本机选为室内机" : "室外机未能开门，请重试";
}

/* The door station's answer, also when nobody waits for it any more. */
static void answered(uint32_t session, xiaotai_err_t error)
{
    if (door.overlay == DOOR_DIALOG_PENDING) {
        if (dialog.timer) xiaotai_timer_cancel(dialog.timer);
        dialog.timer = 0;
        asked.session = 0;
        finish(error, refusal(error));
        return;
    }
    if (!asked.session || session != asked.session) return;
    asked.session = 0;
    if (!asked.counted) ++door.opens;
    note(error);
    if (door.overlay == DOOR_DIALOG_RESULT) show_result(error, refusal(error));
    else xiaotai_view_toast(error == XIAOTAI_OK ? "门已打开" : refusal(error), 0);
}

void door_on_message(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_call_message_t *m = event->size == sizeof(*m) ? event->data : NULL;
    if (!m || !m->session) return;
    if (m->type == XIAOTAI_APP_DOOR_MESSAGE_HELLO) {
        /* Also in a call another application presents: that is where it is news. */
        xiaotai_call_status_t call;
        xiaotai_call_status(&call);
        const bool own = m->session == door.session && door.classified_door;
        if (m->size == 1 && call.session == m->session && door_peer_learn_station(&call, m->data[0]) && !own)
            door_call_pass(m->session);
        return;
    }
    if (m->session != door.session || !door.classified_door) return;
    if (m->type == XIAOTAI_APP_DOOR_MESSAGE_LOOK || m->type == XIAOTAI_APP_DOOR_MESSAGE_RING) {
        door_look_message(m);
    } else if (m->type == XIAOTAI_APP_DOOR_MESSAGE_OPEN && door.sender) {
        station_open(m->session);
    } else if (m->type == XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT && !door.sender && m->size == 4) {
        answered(m->session, (xiaotai_err_t)((uint32_t)m->data[0] | (uint32_t)m->data[1] << 8 |
                                             (uint32_t)m->data[2] << 16 | (uint32_t)m->data[3] << 24));
    }
}

/* The boxes of the call go with it; whether the door opened is not known. */
void door_open_call_ended(uint32_t session)
{
    asked.session = 0;
    const bool waiting = door.overlay == DOOR_DIALOG_PENDING;
    if (waiting || door.overlay == DOOR_DIALOG_VOLUME || (door.overlay == DOOR_DIALOG_OPEN && dialog.session == session))
        door_overlay_drop();
    if (!waiting) return;
    ++door.opens;
    note(XIAOTAI_ERR_TIMEOUT);
    xiaotai_view_toast("通话断了，没有收到开门结果", 0);
}

xiaotai_err_t door_init_open(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_OPEN, open_dialog, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM, confirm, NULL);
    return err;
}
