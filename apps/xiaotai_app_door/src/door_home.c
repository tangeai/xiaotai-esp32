// SPDX-License-Identifier: MIT
#include "door_internal.h"

#include "xiaotai/modules/contacts.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"

#include <string.h>

#define ROW_TOKEN_BASE (UINT32_C(1) << 30)

static const char *const home_text[] = {
    [XIAOTAI_APP_DOOR_STREAMING] = "远程查看中",
    [XIAOTAI_APP_DOOR_NO_NETWORK] = "网络未连接",
    [XIAOTAI_APP_DOOR_NOT_READY] = "设备尚未就绪",
    [XIAOTAI_APP_DOOR_NO_STATION] = "尚未选择门外机",
    [XIAOTAI_APP_DOOR_STATION_REMOVED] = "门外机已移除",
    [XIAOTAI_APP_DOOR_STATION_OFFLINE] = "门外机离线",
    [XIAOTAI_APP_DOOR_STATION_ONLINE] = "门外机在线",
};

static int device_kind(void)
{
    int kind;
    return xiaotai_contacts_kind_find("device", &kind) == XIAOTAI_OK ? kind : XIAOTAI_CONTACT_ANY;
}

static xiaotai_app_door_state_t state_with_network(const xiaotai_net_status_t *net)
{
    if (door.stream_generation) return XIAOTAI_APP_DOOR_STREAMING;
    if (net->state != XIAOTAI_NET_UP) return XIAOTAI_APP_DOOR_NO_NETWORK;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.phase != XIAOTAI_PLATFORM_ONLINE) return XIAOTAI_APP_DOOR_NOT_READY;
    if (!door.peer[0]) return XIAOTAI_APP_DOOR_NO_STATION;
    xiaotai_contact_t station;
    const int kind = device_kind();
    if (kind == XIAOTAI_CONTACT_ANY ||
        xiaotai_contacts_find(door.peer, kind, XIAOTAI_CONTACTS_STALE_OK, &station) != XIAOTAI_OK) {
        return XIAOTAI_APP_DOOR_STATION_REMOVED;
    }
    return station.online_known && station.online ?
           XIAOTAI_APP_DOOR_STATION_ONLINE : XIAOTAI_APP_DOOR_STATION_OFFLINE;
}

xiaotai_app_door_state_t door_home_state(void)
{
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    return state_with_network(&net);
}

static bool busy(void)
{
    return door.session || door.stream_generation || door_fallback_busy() || xiaotai_activity_current(NULL) != 0;
}

static void invalidate_rows(void)
{
    door.row_generation = (door.row_generation + 1U) % (ROW_TOKEN_BASE / DOOR_ROWS_MAX);
}

void door_page(door_page_t page)
{
    if (page == DOOR_DEVICES && door.page != DOOR_DEVICES) invalidate_rows();
    if (door.overlay && page != door.page) door_overlay_close();
    door.page = page;
    door_render();
}

xiaotai_err_t xiaotai_app_door_page_step(int direction)
{
    if (direction != 1 && direction != -1) return XIAOTAI_ERR_INVALID_ARG;
    if (!door.foreground || door.page != DOOR_HOME || door.overlay || door.setup || busy())
        return XIAOTAI_ERR_INVALID_STATE;
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
    if (!door.foreground || door.page != DOOR_HOME || door.overlay || door.setup || busy()) return;
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
    const char *peer = door.rows[row].id;
    const xiaotai_err_t error = door_config.save_peer_on_reselect ?
        xiaotai_settings_confirm_text(XIAOTAI_SETTING_APP_DOOR_PEER, peer) :
        xiaotai_setting_set_text(XIAOTAI_SETTING_APP_DOOR_PEER, peer);
    if (error != XIAOTAI_OK) {
        xiaotai_view_toast(error == XIAOTAI_ERR_BUSY ? "维护中，设置暂不可修改" : "选择未保存，请重试", 0);
        return;
    }
    door_page(DOOR_HOME);
    xiaotai_view_toast("已选择门外机", 0);
}

void door_back_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground) return;
    if (door.overlay) door_overlay_close();
    else if (door.page == DOOR_OTHER_RESULT) door_finish_presentation();
    else if (door.page == DOOR_DEVICES || door.page == DOOR_MONITOR) {
        door_page(DOOR_HOME);
    }
}

void door_monitor_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (door.foreground && door.page == DOOR_HOME && !door.overlay && !door.setup) door_page(DOOR_MONITOR);
}

static void render_home(void)
{
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX] = {
        {.label = door_config.door_name, .intent = XIAOTAI_INTENT_APP_DOOR_DEVICES},
        {.label = "查看门口", .icon = XIAOTAI_ICON_VIDEO, .intent = XIAOTAI_INTENT_APP_DOOR_MONITOR},
        {.label = "开门", .icon = XIAOTAI_ICON_LOCK, .intent = XIAOTAI_INTENT_APP_DOOR_OPEN},
        {.intent = XIAOTAI_INTENT_APP_DOOR_HOME, .trigger = XIAOTAI_TRIGGER_SWIPE_LEFT, .value = 1},
        {.intent = XIAOTAI_INTENT_APP_DOOR_HOME, .trigger = XIAOTAI_TRIGGER_SWIPE_RIGHT, .value = -1},
    };
    size_t count = 5;
    if (xiaotai_intent_exists(XIAOTAI_INTENT_SYSTEM_MENU)) {
        actions[count++] = (xiaotai_view_action_t){
            .intent = XIAOTAI_INTENT_SYSTEM_MENU, .trigger = XIAOTAI_TRIGGER_LONG_PRESS,
        };
    }
    if (door.stream_generation && !door.overlay && !door.setup) {
        actions[count++] = (xiaotai_view_action_t){
            .label = "结束查看", .icon = XIAOTAI_ICON_STOP, .style = XIAOTAI_STYLE_DESTRUCTIVE,
            .intent = XIAOTAI_INTENT_APP_DOOR_STREAM_STOP, .value = (int32_t)door.stream_generation,
            .state = door.stream_stop_timer && door.stream_stop_until_us > xiaotai_time_us() ? XIAOTAI_STATE_BUSY : 0,
        };
    }
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    const xiaotai_app_door_home_view_t home = {.state = state_with_network(&net)};
    const xiaotai_view_indicator_t network = {
        .icon = XIAOTAI_ICON_WIFI,
        .level = net.state != XIAOTAI_NET_UP ? -1 : !net.signal_valid ? 0 :
                 net.signal_dbm >= -57 ? 3 : net.signal_dbm >= -70 ? 2 : 1,
        .intent = xiaotai_intent_exists(XIAOTAI_INTENT_SYSTEM_NETWORK) ? XIAOTAI_INTENT_SYSTEM_NETWORK : NULL,
    };
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_HOME, .custom = XIAOTAI_APP_DOOR_TEMPLATE_HOME,
        .title = door_config.title, .subtitle = door_config.door_name, .status = home_text[home.state],
        .custom_data = &home, .custom_size = sizeof(home),
        .indicators = &network, .indicator_count = 1, .actions = actions,
        .action_count = count,
        .flags = (!busy() ? XIAOTAI_VIEW_MAY_SLEEP : 0) |
                 (door.stream_generation && door.stream_picture ? XIAOTAI_VIEW_VIDEO : 0),
    };
    door_register_view(&view);
}

static void render_devices(void)
{
    const int kind = device_kind();
    xiaotai_contacts_status_t status;
    xiaotai_contacts_status(&status);
    xiaotai_view_item_t items[DOOR_ROWS_MAX];
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
        memcpy(row->label, c.name[0] ? c.name : c.id, sizeof(row->label));
        row->online = c.online_known && c.online;
        items[door.row_count] = (xiaotai_view_item_t){
            .label = row->label,
            .detail = row->online ? "在线" : "离线",
            .intent = XIAOTAI_INTENT_APP_DOOR_SELECT,
            .state = strcmp(door.peer, row->id) == 0 ? XIAOTAI_STATE_CHECKED : 0,
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
        .title = "选择门外机", .actions = &back, .action_count = 1,
        .items = items, .item_count = door.row_count,
    };
    if (!door.row_count) {
        if (status.loading) view.text = "正在同步设备联系人";
        else if (kind == XIAOTAI_CONTACT_ANY || !status.generation || status.error != XIAOTAI_OK)
            view.text = "设备状态暂不可用，请返回重试";
        else {
            view.subtitle = "暂无设备联系人";
            view.text = "请先在平台添加门外机，再同步通讯录";
        }
    }
    door_register_view(&view);
}

static void render_monitor(void)
{
    static const char *const reasons[] = {
        [XIAOTAI_APP_DOOR_STREAMING] = "当前功能正在使用，请稍后再试",
        [XIAOTAI_APP_DOOR_NO_NETWORK] = "网络未连接",
        [XIAOTAI_APP_DOOR_NOT_READY] = "设备尚未就绪，请检查绑定",
        [XIAOTAI_APP_DOOR_NO_STATION] = "请先选择门外机",
        [XIAOTAI_APP_DOOR_STATION_REMOVED] = "门外机已移除，请重新选择",
        [XIAOTAI_APP_DOOR_STATION_OFFLINE] = "门外机离线，请稍后再试",
        [XIAOTAI_APP_DOOR_BUSY] = "当前功能正在使用，请稍后再试",
        [XIAOTAI_APP_DOOR_NO_SERVICE] = "暂未接入门口查看服务",
    };
    xiaotai_app_door_state_t state = door.peer[0] ? door_home_state() : XIAOTAI_APP_DOOR_NO_STATION;
    if (state == XIAOTAI_APP_DOOR_STATION_ONLINE) state = busy() ? XIAOTAI_APP_DOOR_BUSY : XIAOTAI_APP_DOOR_NO_SERVICE;
    const xiaotai_app_door_home_view_t home = {.state = state};
    const xiaotai_view_action_t actions[] = {
        {.label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_DOOR_BACK},
        {.label = "开门", .icon = XIAOTAI_ICON_LOCK, .intent = XIAOTAI_INTENT_APP_DOOR_OPEN},
    };
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_MONITOR, .custom = XIAOTAI_APP_DOOR_TEMPLATE_MONITOR,
        .title = "查看门口", .text = reasons[state], .custom_data = &home, .custom_size = sizeof(home),
        .actions = actions, .action_count = 2,
    };
    door_register_view(&view);
}

void door_render_home_page(void)
{
    if (door.page == DOOR_DEVICES) render_devices();
    else if (door.page == DOOR_MONITOR) render_monitor();
    else render_home();
}
