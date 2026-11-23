// SPDX-License-Identifier: MIT
/* The door station's directory and the calls it dials from it: WeChat users
 * (the people a visitor comes to see) and devices (the front desk, other
 * stations). Device calls the station dialled are door calls (door_call.c);
 * WeChat calls it dialled are presented here, on the same call template. */
#include "door_internal.h"

#include "xiaotai/eventlog.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/ui.h"
#ifdef XIAOTAI_DOOR_WITH_WECHAT
#include "xiaotai/modules/wechat.h"
#endif
#include "xiaotai/port/os.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- directory */

void door_render_directory(void)
{
    const int devices = door_contact_kind("device");
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    const int users = door_contact_kind("wechat");
#else
    const int users = XIAOTAI_CONTACT_ANY;
#endif
    xiaotai_contacts_status_t status;
    xiaotai_contacts_status(&status);
    xiaotai_view_item_t items[DOOR_ROWS_MAX];
    const uint8_t previous = door.row_count;
    bool changed = false;
    door.row_count = 0;
    /* People first: a visitor usually comes to see somebody. */
    for (int pass = 0; pass < 2; ++pass) {
        const int kind = pass == 0 ? users : devices;
        if (kind == XIAOTAI_CONTACT_ANY) continue;
        for (uint8_t i = 0; i < status.page_count && door.row_count < DOOR_ROWS_MAX; ++i) {
            xiaotai_contact_t c;
            if (xiaotai_contacts_at(i, XIAOTAI_CONTACTS_STALE_OK, &c) != XIAOTAI_OK || c.kind != kind) continue;
            door_row_t *row = &door.rows[door.row_count];
            if (door.row_count >= previous || strcmp(row->id, c.id) != 0) changed = true;
            memcpy(row->id, c.id, sizeof(row->id));
            row->online = c.online_known && c.online;
            row->wechat = pass == 0;
            /* Contacts without a remark get a readable name, never their raw id. */
            const size_t id_length = strlen(c.id);
            if (c.name[0]) (void)snprintf(row->label, sizeof(row->label), "%s", c.name);
            else if (row->wechat) (void)snprintf(row->label, sizeof(row->label), "微信联系人");
            else (void)snprintf(row->label, sizeof(row->label), "设备 %.4s", id_length > 4 ? c.id + id_length - 4 : c.id);
            const bool front_desk = !row->wechat && door.peer[0] && !strcmp(door.peer, row->id);
            items[door.row_count] = (xiaotai_view_item_t){
                .label = row->label,
                .detail = row->wechat ? "微信视频" : front_desk ? "前台" : row->online ? "在线" : "离线",
                .intent = XIAOTAI_INTENT_APP_DOOR_DIAL,
            };
            door.row_count++;
        }
    }
    if (changed || previous != door.row_count) door_rows_invalidate();
    for (uint32_t i = 0; i < door.row_count; ++i) {
        items[i].value = (int32_t)(DOOR_ROW_TOKEN_BASE + door.row_generation * DOOR_ROWS_MAX + i);
    }
    const xiaotai_view_action_t back = {
        .label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_DOOR_BACK,
    };
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_DIRECTORY, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DEVICES,
        .title = "通讯录", .actions = &back, .action_count = 1, .items = items, .item_count = door.row_count,
    };
    if (!door.row_count) {
        if (status.loading) view.text = "正在同步通讯录";
        else if (!status.generation || status.error != XIAOTAI_OK) view.text = "通讯录暂不可用，请返回重试";
        else {
            view.subtitle = "通讯录是空的";
            view.text = "请先在小程序里添加员工或设备";
        }
    }
    door_register_view(&view);
}

static void directory_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground || !door.sender || door.page != DOOR_HOME || door.overlay || door.setup || door_busy()) return;
    (void)xiaotai_contacts_show(XIAOTAI_CONTACT_ANY, 0);
    (void)xiaotai_contacts_refresh();
    door_page(DOOR_DIRECTORY);
}

static void dial_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!door.foreground || !door.sender || door.page != DOOR_DIRECTORY || door.overlay || door.setup || door_busy())
        return;
    const int index = door_row_from_value(args->value);
    if (index < 0) return;
    const door_row_t *row = &door.rows[index];
    uint32_t session = 0;
    xiaotai_err_t err;
    if (row->wechat) {
#ifdef XIAOTAI_DOOR_WITH_WECHAT
        err = xiaotai_wechat_dial(row->id, XIAOTAI_WECHAT_VIDEO, &session);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED) err = xiaotai_wechat_dial(row->id, 0, &session);
        if (err == XIAOTAI_OK) door.wechat_session = session;
#else
        err = XIAOTAI_ERR_NOT_SUPPORTED;
#endif
    } else {
        const uint32_t flags = xiaotai_module_present("video_call") ? XIAOTAI_CALL_VIDEO : 0;
        err = xiaotai_call_dial(row->id, flags, &session);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED && flags) err = xiaotai_call_dial(row->id, 0, &session);
        if (err == XIAOTAI_OK) door.dialed_session = session;
    }
    if (err == XIAOTAI_OK) {
        xiaotai_eventlog_note("door", row->wechat ? "dial.wechat" : "dial.device", session, (int32_t)index);
        return;
    }
    xiaotai_view_toast(err == XIAOTAI_ERR_BUSY ? "设备正忙，请稍后再试" :
                       err == XIAOTAI_ERR_NOT_FOUND ? "该联系人暂不可呼叫" : "呼叫没有发出，请重试", 0);
}

/* ---------------------------------------------------------- WeChat calls */

#ifdef XIAOTAI_DOOR_WITH_WECHAT
bool door_station_wechat(uint32_t session) { return door.wechat_session && session == door.wechat_session; }

bool door_station_claim(const xiaotai_event_t *event)
{
    if (!door.wechat_session || strncmp(event->name, "wechat.", 7) != 0) return false;
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    if (status.session == door.wechat_session) return true;
    /* The end of the station's call: the module is idle again. */
    return !strcmp(event->name, XIAOTAI_EVENT_WECHAT_ENDED) && !status.session;
}

bool door_station_video_link(uint32_t link)
{
    if (!door.wechat_session || !link) return false;
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    return status.session == door.wechat_session && status.link == link;
}

void door_station_render_wechat(const void *wechat_status)
{
    const xiaotai_wechat_status_t *call = wechat_status;
    const bool active = call->state == XIAOTAI_WECHAT_ACTIVE;
    const bool video = (call->flags & XIAOTAI_WECHAT_VIDEO) != 0;
    const xiaotai_app_door_call_view_t custom = {
        .phase = active ? XIAOTAI_APP_DOOR_CALL_ACTIVE : XIAOTAI_APP_DOOR_CALL_CONNECTING,
        .video = video, .muted = call->muted,
    };
    char subtitle[192];
    (void)snprintf(subtitle, sizeof(subtitle), "%s · %s", call->peer_name[0] ? call->peer_name : "微信用户",
                   video ? "微信视频通话" : "微信语音通话");
    const xiaotai_view_action_t hangup = {
        .label = active ? "挂断" : "取消", .intent = XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP,
        .value = (int32_t)call->session, .icon = XIAOTAI_ICON_HANGUP, .style = XIAOTAI_STYLE_DESTRUCTIVE,
        .role = XIAOTAI_ROLE_BACK,
    };
    const int64_t elapsed = active ? xiaotai_time_us() - call->active_since_us : 0;
    const xiaotai_view_t view = {
        .id = XIAOTAI_APP_DOOR_PAGE_CALL, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_CALL,
        .custom_data = &custom, .custom_size = sizeof(custom),
        .title = active ? "正在通话" : "正在呼叫", .subtitle = subtitle,
        .status = active ? NULL : "等待接听",
        .text = active ? "请对着门禁机说话" : "对方在微信里接听后即可通话",
        .timer_mode = active ? XIAOTAI_TIMER_ELAPSED : XIAOTAI_TIMER_NONE,
        .timer_s = elapsed > 0 ? (int32_t)(elapsed / 1000000LL) : 0,
        .flags = active && door.call_picture && call->link && call->link == door.call_picture_generation ?
                 XIAOTAI_VIEW_VIDEO : 0,
        .actions = &hangup, .action_count = 1,
    };
    door_register_view(&view);
}

static const char *wechat_ending(const xiaotai_wechat_status_t *call)
{
    switch (call->end) {
    case XIAOTAI_WECHAT_END_NO_ANSWER: return "对方暂未接听";
    case XIAOTAI_WECHAT_END_PEER_REJECT: return "对方现在不方便接听";
    case XIAOTAI_WECHAT_END_LOCAL: return call->active_since_us ? "通话已结束" : "已取消呼叫";
    case XIAOTAI_WECHAT_END_CONTACT: return "该联系人暂不可呼叫";
    case XIAOTAI_WECHAT_END_NETWORK: return "网络中断，请重试";
    case XIAOTAI_WECHAT_END_FAILED: return "呼叫没有接通，请重试";
    default: return call->active_since_us ? "通话已结束" : "呼叫没有接通";
    }
}

void door_station_wechat_ended(const void *wechat_status)
{
    const xiaotai_wechat_status_t *call = wechat_status;
    door.wechat_session = 0;
    door.call_picture = false;
    door.call_picture_generation = 0;
    door_finish_presentation();
    if (door.foreground) xiaotai_view_toast(wechat_ending(call), 0);
}

static void wechat_hangup(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    const uint32_t session = (uint32_t)args->value;
    if (!door.foreground || !door_station_wechat(session)) return;
    if (xiaotai_wechat_hangup(session) != XIAOTAI_OK) xiaotai_view_toast("操作未受理，请重试", 0);
}
#else
bool door_station_wechat(uint32_t session) { (void)session; return false; }
bool door_station_claim(const xiaotai_event_t *event) { (void)event; return false; }
bool door_station_video_link(uint32_t link) { (void)link; return false; }
void door_station_render_wechat(const void *wechat_status) { (void)wechat_status; }
void door_station_wechat_ended(const void *wechat_status) { (void)wechat_status; }
#endif

xiaotai_err_t door_init_station(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, directory_intent, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_DIAL, dial_intent, NULL);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP, wechat_hangup, NULL);
#endif
    return err;
}
