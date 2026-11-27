// SPDX-License-Identifier: MIT
#include "door_internal.h"

#include "xiaotai/eventlog.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#ifdef XIAOTAI_DOOR_WIDE
#include "templates_wide.h"
#endif
#ifdef XIAOTAI_DOOR_PAGES
#include "templates_station.h"
#endif

#include <stdio.h>
#include <string.h>

#define ROW_TOKEN_BASE DOOR_ROW_TOKEN_BASE

static const char *const home_text[] = {
    [XIAOTAI_APP_DOOR_STREAMING] = "远程查看中",
    [XIAOTAI_APP_DOOR_NO_NETWORK] = "网络未连接",
    [XIAOTAI_APP_DOOR_NOT_READY] = "设备尚未就绪",
    [XIAOTAI_APP_DOOR_NO_STATION] = "尚未添加室外机",
    [XIAOTAI_APP_DOOR_STATION_REMOVED] = "室外机已移除",
    [XIAOTAI_APP_DOOR_STATION_OFFLINE] = "室外机离线",
    [XIAOTAI_APP_DOOR_STATION_ONLINE] = "室外机在线",
};

/* The doorbell's indoor station speaks of its bell. */
static const char *const bell_text[] = {
    [XIAOTAI_APP_DOOR_STREAMING] = "远程查看中",
    [XIAOTAI_APP_DOOR_NO_NETWORK] = "网络未连接",
    [XIAOTAI_APP_DOOR_NOT_READY] = "设备尚未就绪",
    [XIAOTAI_APP_DOOR_NO_STATION] = "尚未添加门铃",
    [XIAOTAI_APP_DOOR_STATION_REMOVED] = "门铃已移除",
    [XIAOTAI_APP_DOOR_STATION_OFFLINE] = "门铃离线",
    [XIAOTAI_APP_DOOR_STATION_ONLINE] = "门铃在线",
};

/* The door station speaks to the visitor. */
static const char *const station_text[] = {
    [XIAOTAI_APP_DOOR_STREAMING] = "远程查看中",
    [XIAOTAI_APP_DOOR_NO_NETWORK] = "网络未连接",
    [XIAOTAI_APP_DOOR_NOT_READY] = "设备尚未就绪",
    [XIAOTAI_APP_DOOR_NO_STATION] = "尚未添加室内机",
    [XIAOTAI_APP_DOOR_STATION_REMOVED] = "室内机已移除",
    [XIAOTAI_APP_DOOR_STATION_OFFLINE] = "室内暂时无人在线",
    [XIAOTAI_APP_DOOR_STATION_ONLINE] = "可以呼叫",
};

int door_contact_kind(const char *name)
{
    int kind;
    return xiaotai_contacts_kind_find(name, &kind) == XIAOTAI_OK ? kind : XIAOTAI_CONTACT_ANY;
}

static int device_kind(void) { return door_contact_kind("device"); }

static xiaotai_app_door_state_t state_with_network(const xiaotai_net_status_t *net)
{
    if (door.stream_generation) return XIAOTAI_APP_DOOR_STREAMING;
    if (net->state != XIAOTAI_NET_UP) return XIAOTAI_APP_DOOR_NO_NETWORK;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.phase != XIAOTAI_PLATFORM_ONLINE) return XIAOTAI_APP_DOOR_NOT_READY;
    if (!door.peer_count) return XIAOTAI_APP_DOOR_NO_STATION;
    xiaotai_contact_t contact;
    if (!door.sender) {
        /* The home screen of an indoor station is about its first door station. */
        if (!door_peer_contact(door.peers[0], &contact)) return XIAOTAI_APP_DOOR_STATION_REMOVED;
        return contact.online_known && contact.online ?
               XIAOTAI_APP_DOOR_STATION_ONLINE : XIAOTAI_APP_DOOR_STATION_OFFLINE;
    }
    bool known = false, online = false;
    for (uint8_t i = 0; i < door.peer_count; ++i) {
        if (!door_peer_contact(door.peers[i], &contact)) continue;
        known = true;
        online = online || (contact.online_known && contact.online);
    }
    return !known ? XIAOTAI_APP_DOOR_STATION_REMOVED :
           online ? XIAOTAI_APP_DOOR_STATION_ONLINE : XIAOTAI_APP_DOOR_STATION_OFFLINE;
}

xiaotai_app_door_state_t door_home_state(void)
{
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    return state_with_network(&net);
}

bool door_busy(void)
{
    return door.session || door_hunt_active() || door_look_running() || door.stream_generation || door_fallback_busy() ||
           xiaotai_activity_current(NULL) != 0;
}

static bool busy(void) { return door_busy(); }
static void invalidate_rows(void) { door_rows_invalidate(); }

/* Displays the door's own wide templates do not cover get the theme's clock
 * screen for the home page (time, date, the state beside the title, the
 * controls along the bottom, the pager's arrows at the sides) instead of the
 * generic notice a custom view falls back to. Decided by the display's size,
 * the same test the templates register with. */
bool door_own_template(void)
{
#ifdef XIAOTAI_DOOR_WIDE
    xiaotai_ui_info_t info;
    return xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && door_templates_cover(info.width, info.height);
#else
    return false;
#endif
}

bool door_home_template(void)
{
#ifdef XIAOTAI_DOOR_WIDE
    xiaotai_ui_info_t info;
    return xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && door_templates_home(info.width, info.height);
#else
    return false;
#endif
}

bool door_station_template(void)
{
#ifdef XIAOTAI_DOOR_PAGES
    xiaotai_ui_info_t info;
    return xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.pixel == XIAOTAI_PIXEL_RGB565 &&
           door_station_pages_cover(info.width, info.height);
#else
    return false;
#endif
}

void door_show_list(const xiaotai_view_t *view)
{
    xiaotai_view_t shown = *view;
    if (!door_own_template() && view->item_count) {
        shown.kind = XIAOTAI_VIEW_LIST;
        shown.custom = NULL;
    }
    door_register_view(&shown);
}

static void home_layout(xiaotai_view_t *view, const char *caption)
{
    if (door_home_template()) return;
    view->kind = XIAOTAI_VIEW_CLOCK;
    view->custom = NULL;
    view->title = door.access ? "门禁机" : door.sender ? "门铃室外机" : door.bell_indoor ? "可视门铃" : "门禁室内机";
    view->text = caption;
}

/* The pager's arrows: drawn where the display shows the generic layout (the
 * door's own wide template pages by swiping and ignores them), and only when
 * paging leads to another application. */
const char *door_pager_icon(int direction)
{
    if (xiaotai_app_count() < 2 && !door_pages_on()) return NULL;
    return direction > 0 ? XIAOTAI_ICON_NEXT : XIAOTAI_ICON_PREVIOUS;
}

/* A page left open on the way: somebody else is at the device by now. */
#define IDLE_HOME_MS 60000U
static uint32_t idle_timer;

static bool idle_page(void)
{
    return door.page == DOOR_DEVICES || door.page == DOOR_DIRECTORY || door.page == DOOR_SETTINGS || door.page == DOOR_LOG;
}

static void idle_passed(void *ctx)
{
    (void)ctx;
    idle_timer = 0;
    if (door.foreground && !door.setup && idle_page() && !door.session) door_page(DOOR_HOME);
}

void door_idle_restart(void)
{
    if (idle_timer) xiaotai_timer_cancel(idle_timer);
    idle_timer = 0;
    if (door.foreground && idle_page() && xiaotai_timer_start(IDLE_HOME_MS, idle_passed, NULL, &idle_timer) != XIAOTAI_OK)
        idle_timer = 0;
}

void door_idle_reset(void)
{
    if (idle_timer) xiaotai_timer_cancel(idle_timer);
    idle_timer = 0;
}

void door_page(door_page_t page)
{
    if (page != DOOR_HOME) door_pages_commit();     /* the page in front is in use */
    if ((page == DOOR_DEVICES || page == DOOR_DIRECTORY) && door.page != page) invalidate_rows();
    if (door.overlay && page != door.page) door_overlay_close();
    const bool changed = door.page != page;
    door.page = page;
    if (changed) door_idle_restart();
    door_render();
}

xiaotai_err_t xiaotai_app_door_page_step(int direction)
{
    if (direction != 1 && direction != -1) return XIAOTAI_ERR_INVALID_ARG;
    if (!door.foreground || door.page != DOOR_HOME || door.overlay || door.setup || busy())
        return XIAOTAI_ERR_INVALID_STATE;
    if (door_pages_step(direction)) return XIAOTAI_OK;
    const xiaotai_err_t result = direction == 1 ? xiaotai_app_next() : xiaotai_app_previous();
    return result == XIAOTAI_ERR_NOT_FOUND ? XIAOTAI_OK : result;
}

void door_home_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (door.overlay || door.setup || busy()) return;
    if (!door.foreground) {
        if (args->value == 0) (void)xiaotai_app_switch("door");
        return;
    }
    if (args->value == 0) {
        door_page(DOOR_HOME);
        return;
    }
    const int64_t now = xiaotai_time_us();
    if (door.last_swipe_valid && now - door.last_swipe_us < (int64_t)XIAOTAI_APP_DOOR_PAGER_GUARD_MS * 1000)
        return;
    if (xiaotai_app_door_page_step(args->value) == XIAOTAI_OK && !door.foreground) {
        door.last_swipe_valid = true;
        door.last_swipe_us = now;
    }
}

void door_devices_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground || (door.page != DOOR_HOME && door.page != DOOR_SETTINGS) || door.overlay || door.setup || busy())
        return;
    const int kind = device_kind();
    if (kind != XIAOTAI_CONTACT_ANY) (void)xiaotai_contacts_show(kind, 0);
    (void)xiaotai_contacts_refresh();
    door_page(DOOR_DEVICES);
}

void door_select_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!door.foreground || door.page != DOOR_DEVICES || door.overlay || door.setup || busy() || args->value < 0) return;
    const uint32_t value = (uint32_t)args->value;
    /* Public callers may still select a current index. A tap from a rendered
     * list identifies its old mapping and must never save a replacement ID. */
    uint32_t row = value;
    if (value >= ROW_TOKEN_BASE) {
        const uint32_t token = value - ROW_TOKEN_BASE;
        if (token / DOOR_ROWS_MAX != door.row_generation) return;
        row = token % DOOR_ROWS_MAX;
    }
    if (row >= door.row_count) return;
    char peer[XIAOTAI_SETTING_TEXT_MAX];
    memcpy(peer, door.rows[row].id, sizeof(peer));
    const bool known = door_peer_slot(peer) >= 0;
    const bool others = door.peer_count > (known ? 1U : 0U);
    const xiaotai_err_t error = door_peer_put_first(peer);
    if (error != XIAOTAI_OK) {
        xiaotai_view_toast(error == XIAOTAI_ERR_BUSY ? "维护中，设置暂不可修改" :
                           error == XIAOTAI_ERR_NO_MEM ? "已经有 4 台了，请先在门禁设置里移除一台" : "选择未保存，请重试", 0);
        return;
    }
    door_page(DOOR_HOME);
    /* A doorbell's indoor station keeps one doorbell: another one takes its place. */
    if (door.bell_indoor) xiaotai_view_toast(others ? "已换成这台门铃" : "已选择这台门铃", 0);
    else if (known) xiaotai_view_toast(door.sender ? "已设为先呼叫的一台" : "已选择这台室外机", 0);
    else if (others) xiaotai_view_toast("已添加，原来的仍保留，可在门禁设置里移除", 0);
    else xiaotai_view_toast(door.access ? "已添加管理处" : door.sender ? "已添加室内机" : "已选择这台室外机", 0);
}

void door_back_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground) return;
    if (door.overlay) door_overlay_close();
    else if (door.page == DOOR_OTHER_RESULT) door_finish_presentation();
    else if (door.page == DOOR_MONITOR) {
        if (!door_look_running()) door_page(DOOR_HOME);
    } else if (door_access_back()) {
        /* the door numbers again */
    } else if (door.page == DOOR_DEVICES || door.page == DOOR_DIRECTORY || door.page == DOOR_SETTINGS ||
               door.page == DOOR_LOG || door.page == DOOR_MODE) {
        door_page(DOOR_HOME);
    }
}

static xiaotai_view_indicator_t network_mark(const xiaotai_net_status_t *net)
{
    return (xiaotai_view_indicator_t){
        .icon = XIAOTAI_ICON_WIFI,
        .level = net->state != XIAOTAI_NET_UP ? -1 : !net->signal_valid ? 0 :
                 net->signal_dbm >= -57 ? 3 : net->signal_dbm >= -70 ? 2 : 1,
        .intent = xiaotai_intent_exists(XIAOTAI_INTENT_SYSTEM_NETWORK) ? XIAOTAI_INTENT_SYSTEM_NETWORK : NULL,
    };
}

static void pager_actions(xiaotai_view_action_t *actions, size_t *count)
{
    actions[(*count)++] = (xiaotai_view_action_t){.intent = XIAOTAI_INTENT_APP_DOOR_HOME,
        .trigger = XIAOTAI_TRIGGER_SWIPE_LEFT, .value = 1, .icon = door_pager_icon(1)};
    actions[(*count)++] = (xiaotai_view_action_t){.intent = XIAOTAI_INTENT_APP_DOOR_HOME,
        .trigger = XIAOTAI_TRIGGER_SWIPE_RIGHT, .value = -1, .icon = door_pager_icon(-1)};
}

/* The door station's home screen is for the visitor: a bell, and the people
 * one can call. Its settings are behind a long press on the title. */
static void render_station_home(void)
{
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    if (door.access) {
        const xiaotai_view_indicator_t mark = network_mark(&net);
        door_access_home_render(&mark);
        return;
    }
    const xiaotai_app_door_home_view_t home = {.state = state_with_network(&net), .station = true};
    const bool callable = home.state == XIAOTAI_APP_DOOR_STATION_ONLINE || home.state == XIAOTAI_APP_DOOR_STATION_OFFLINE;
    const bool nobody = !door.peer_count || home.state == XIAOTAI_APP_DOOR_STATION_REMOVED;
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
    size_t count = 0;
    if (nobody) {
        actions[count++] = (xiaotai_view_action_t){.label = "添加室内机", .intent = XIAOTAI_INTENT_APP_DOOR_DEVICES,
            .icon = XIAOTAI_ICON_CONTACTS, .role = XIAOTAI_ROLE_PRIMARY};
    } else {
        actions[count++] = (xiaotai_view_action_t){.label = "呼叫", .icon = XIAOTAI_ICON_PHONE,
            .intent = XIAOTAI_INTENT_APP_DOOR_RING, .role = XIAOTAI_ROLE_PRIMARY, .style = XIAOTAI_STYLE_POSITIVE,
            .state = callable ? 0 : XIAOTAI_STATE_DISABLED};
    }
    /* A doorbell rings its own household: nothing else to choose. */
    pager_actions(actions, &count);
    actions[count++] = (xiaotai_view_action_t){
        .intent = XIAOTAI_INTENT_APP_DOOR_SETTINGS, .trigger = XIAOTAI_TRIGGER_LONG_PRESS,
    };
    const xiaotai_view_indicator_t network = network_mark(&net);
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_HOME, .custom = XIAOTAI_APP_DOOR_TEMPLATE_HOME,
        .title = door_title(), .subtitle = door_config.door_name, .status = station_text[home.state],
        .text = nobody ? "添加后，访客可以按铃呼叫" : "您好，请按门铃",
        .custom_data = &home, .custom_size = sizeof(home),
        .indicators = &network, .indicator_count = 1, .actions = actions, .action_count = count,
        .flags = XIAOTAI_VIEW_TIME | (!busy() ? XIAOTAI_VIEW_MAY_SLEEP : 0),
    };
    /* Without a home picture of its own the display still shows a doorbell, not a clock. */
    if (!door_home_template() && door_station_template()) view.custom = XIAOTAI_APP_DOOR_TEMPLATE_BELL;
    else home_layout(&view, view.text);
    door_register_view(&view);
}

/* The indoor station's home screen: look at the door first; the rest is one tap away. */
static void render_home(void)
{
    char first[XIAOTAI_SETTING_TEXT_MAX], missed[32];
    if (door.peer_count) door_peer_label(door.peers[0], first, sizeof(first));
    const bool streaming = door.stream_generation && !door.overlay && !door.setup;
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
    size_t count = 0;
    /* A small screen has room for three buttons: the settings lead to the rest. */
    const bool wide = door_home_template(), simple = door.bell_indoor;
    if (door.peer_count) {
        if (!simple && (wide || !door.missed))
            actions[count++] = (xiaotai_view_action_t){.label = first, .intent = XIAOTAI_INTENT_APP_DOOR_DEVICES};
        actions[count++] = (xiaotai_view_action_t){.label = "查看门口", .icon = XIAOTAI_ICON_VIDEO,
            .intent = XIAOTAI_INTENT_APP_DOOR_MONITOR, .role = XIAOTAI_ROLE_PRIMARY};
    } else {
        actions[count++] = (xiaotai_view_action_t){.label = simple ? "添加门铃" : "添加室外机", .icon = XIAOTAI_ICON_CONTACTS,
            .intent = XIAOTAI_INTENT_APP_DOOR_DEVICES, .role = XIAOTAI_ROLE_PRIMARY};
    }
    /* Without a lock of its own the device opens the door only while it is connected to it. */
    if (!simple && door_lock_configured()) {
        actions[count++] = (xiaotai_view_action_t){.label = "开门", .icon = XIAOTAI_ICON_KEY,
            .intent = XIAOTAI_INTENT_APP_DOOR_OPEN};
    }
    pager_actions(actions, &count);
    if (xiaotai_intent_exists(XIAOTAI_INTENT_SYSTEM_MENU)) {
        actions[count++] = (xiaotai_view_action_t){
            .intent = XIAOTAI_INTENT_SYSTEM_MENU, .trigger = XIAOTAI_TRIGGER_LONG_PRESS,
        };
    }
    if (streaming) {
        actions[count++] = (xiaotai_view_action_t){
            .label = "结束查看", .icon = XIAOTAI_ICON_STOP, .style = XIAOTAI_STYLE_DESTRUCTIVE,
            .intent = XIAOTAI_INTENT_APP_DOOR_STREAM_STOP, .value = (int32_t)door.stream_generation,
            .state = door.stream_stop_timer && door.stream_stop_until_us > xiaotai_time_us() ? XIAOTAI_STATE_BUSY : 0,
        };
    } else {
        if (door.missed) (void)snprintf(missed, sizeof(missed), "未接 %u", (unsigned)(door.missed > 99 ? 99 : door.missed));
        else (void)snprintf(missed, sizeof(missed), "来访记录");
        if (!simple && (wide || door.missed))
            actions[count++] = (xiaotai_view_action_t){.label = missed, .icon = XIAOTAI_ICON_PHONE,
                .intent = XIAOTAI_INTENT_APP_DOOR_LOG, .state = door.missed ? XIAOTAI_STATE_CHECKED : 0};
        if (count < XIAOTAI_VIEW_ACTIONS_MAX) {
            actions[count++] = (xiaotai_view_action_t){.label = "设置", .icon = XIAOTAI_ICON_SETTINGS,
                .intent = XIAOTAI_INTENT_APP_DOOR_SETTINGS, .style = XIAOTAI_STYLE_QUIET};
        }
    }
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    const xiaotai_app_door_home_view_t home = {.state = state_with_network(&net)};
    const xiaotai_view_indicator_t network = network_mark(&net);
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_HOME, .custom = XIAOTAI_APP_DOOR_TEMPLATE_HOME,
        .title = door_title(), .subtitle = door_config.door_name, .status = (simple ? bell_text : home_text)[home.state],
        .custom_data = &home, .custom_size = sizeof(home),
        .indicators = &network, .indicator_count = 1, .actions = actions,
        .action_count = count,
        .flags = XIAOTAI_VIEW_TIME | (!busy() ? XIAOTAI_VIEW_MAY_SLEEP : 0) |
                 (door.stream_generation && door.stream_picture ? XIAOTAI_VIEW_VIDEO : 0),
    };
    home_layout(&view, NULL);
    door_register_view(&view);
}

static void render_devices(void)
{
    const int kind = device_kind();
    xiaotai_contacts_status_t status;
    xiaotai_contacts_status(&status);
    xiaotai_view_item_t *const items = door.items;
    const uint8_t previous_count = door.row_count;
    bool mapping_changed = false;
    door.row_count = 0;
    for (uint8_t i = 0; i < status.page_count && door.row_count < DOOR_ROWS_MAX; ++i) {
        xiaotai_contact_t c;
        if (kind == XIAOTAI_CONTACT_ANY ||
            xiaotai_contacts_at(i, XIAOTAI_CONTACTS_STALE_OK, &c) != XIAOTAI_OK || c.kind != kind) continue;
        door_row_t *row = &door.rows[door.row_count];
        if (door.row_count >= previous_count || strcmp(row->id, c.id) != 0) mapping_changed = true;
        memcpy(row->id, c.id, sizeof(row->id));
        door_device_label(c.id, c.name, row->label, sizeof(row->label));
        row->online = c.online_known && c.online;
        const int slot = door_peer_slot(row->id);
        items[door.row_count] = (xiaotai_view_item_t){
            .label = row->label,
            .detail = slot < 0 ? (row->online ? "在线" : "离线") :
                      slot > 0 ? (row->online ? "已添加 · 在线" : "已添加 · 离线") :
                      door.sender ? (row->online ? "先呼叫 · 在线" : "先呼叫 · 离线") :
                      row->online ? "当前 · 在线" : "当前 · 离线",
            .intent = XIAOTAI_INTENT_APP_DOOR_SELECT,
            .state = slot >= 0 ? XIAOTAI_STATE_CHECKED : 0,
        };
        door.row_count++;
    }
    if (mapping_changed || previous_count != door.row_count) invalidate_rows();
    for (uint32_t i = 0; i < door.row_count; ++i) {
        items[i].value = (int32_t)(ROW_TOKEN_BASE + door.row_generation * DOOR_ROWS_MAX + i);
    }
    const xiaotai_view_action_t back = {
        .label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_DOOR_BACK,
    };
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_DEVICES, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DEVICES,
        .title = door.access ? "添加管理处" : door.sender ? "添加室内机" : door.bell_indoor ? "选择门铃" : "选择室外机",
        .actions = &back, .action_count = 1,
        .items = items, .item_count = door.row_count,
    };
    if (!door.row_count) {
        if (status.loading) view.text = "正在同步设备联系人";
        else if (kind == XIAOTAI_CONTACT_ANY || !status.generation || status.error != XIAOTAI_OK)
            view.text = "设备状态暂不可用，请返回重试";
        else {
            view.subtitle = "暂无设备联系人";
            view.text = door.sender ? "请先在小程序里把室内机加为设备联系人" : "请先在小程序里把室外机加为设备联系人";
        }
    }
    door_show_list(&view);
}

void door_render_home_page(void)
{
    if (door.page == DOOR_DEVICES) render_devices();
    else if (door.page == DOOR_MONITOR) door_look_render();
    else if (door.page == DOOR_DIRECTORY) door_render_directory();
    else if (door.page == DOOR_SETTINGS) door_settings_render();
    else if (door.page == DOOR_LOG) door_log_render();
    else if (door.page == DOOR_MODE || !door_mode_known()) door_mode_render();
    else if (door.sender) render_station_home();
    else render_home();
}
