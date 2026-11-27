// SPDX-License-Identifier: MIT
/* The access machine: the door station of a building or a company. Its home
 * screen is a keypad; the room typed there, or chosen from the door numbers
 * (门牌号), is looked for in the names of the contacts (door_rooms.h). The
 * contacts of one room are one household and are called one after the other.
 * Below the door numbers lies the directory (通讯录): everybody by name, each
 * called alone. */
#include "door_internal.h"

#include "xiaotai/modules/contacts.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"

#include <stdio.h>
#include <string.h>

#define CLEAR_MS 15000U

static struct {
    char typed[DOOR_ROOM_TYPED_MAX + 1U];
    char among[DOOR_ROOM_TYPED_MAX + 1U];   /* the list shows the rooms these digits mean; "": all */
    uint32_t timer, shown;
    bool people;                            /* the directory instead of the door numbers */
} keypad;

void door_access_reset(void)
{
    if (keypad.timer) xiaotai_timer_cancel(keypad.timer);
    memset(&keypad, 0, sizeof(keypad));
}

/* ------------------------------------------------------------- households */

typedef struct {
    xiaotai_contact_t contact;
    uint16_t index;
    uint8_t pass;       /* devices, then WeChat users */
} walk_t;

/* Every contact in memory, whatever page is selected. */
static bool next(walk_t *w)
{
    for (; w->pass < 2U; ++w->pass, w->index = 0) {
#ifndef XIAOTAI_DOOR_WITH_WECHAT
        if (w->pass) continue;
#endif
        const int kind = door_contact_kind(w->pass ? "wechat" : "device");
        if (kind == XIAOTAI_CONTACT_ANY ||
            xiaotai_contacts_at_kind(kind, w->index, XIAOTAI_CONTACTS_STALE_OK, &w->contact) != XIAOTAI_OK) continue;
        ++w->index;
        return true;
    }
    return false;
}

/* The households a visitor can choose, by room; whoever has none by name
 * after them. A contact without a name is nobody a visitor could recognize. */
static void collect(const char *among)
{
    walk_t w = {.pass = 0};
    int best = among[0] ? 1 : 0;
    char found[DOOR_ROOM_SIZE];
    while (among[0] && next(&w)) {
        const int rank = door_room_of(w.contact.name, found) ? door_room_matches(found, among) : 0;
        if (rank > best) best = rank;
    }
    w = (walk_t){.pass = 0};
    door.row_count = 0;
    while (next(&w)) {
        const xiaotai_contact_t *c = &w.contact;
        char room[DOOR_ROOM_SIZE];
        const bool has = door_room_of(c->name, room);
        /* A door number lists rooms only: whoever has none is in the directory. */
        if (!c->name[0] || !has || (among[0] && door_room_matches(room, among) < best)) continue;
        const bool online = !w.pass && c->online_known && c->online;
        size_t at = 0;
        while (has && at < door.row_count && strcmp(door.rows[at].room, room)) ++at;
        if (has && at < door.row_count) {
            door.rows[at].online = door.rows[at].online || online;
            door.rows[at].wechat = door.rows[at].wechat || w.pass;
            continue;
        }
        if (door.row_count >= DOOR_ROWS_MAX) continue;
        for (at = 0; at < door.row_count; ++at) {
            if (has && (!door.rows[at].room[0] || door_room_compare(room, door.rows[at].room) < 0)) break;
        }
        memmove(&door.rows[at + 1U], &door.rows[at], (door.row_count - at) * sizeof(door.rows[0]));
        ++door.row_count;
        door_row_t *row = &door.rows[at];
        *row = (door_row_t){.online = online, .wechat = w.pass != 0};
        memcpy(row->room, room, sizeof(row->room));
        (void)snprintf(row->id, sizeof(row->id), "%s", c->id);
        door_room_plate(c->name, row->label, sizeof(row->label));     /* the door, not who lives behind it */
    }
}

/* Everybody with a name, devices first: one row each, without a room, so a tap calls that one alone. */
static void collect_people(void)
{
    walk_t w = {.pass = 0};
    door.row_count = 0;
    while (door.row_count < DOOR_ROWS_MAX && next(&w)) {
        const xiaotai_contact_t *c = &w.contact;
        if (!c->name[0]) continue;
        door_row_t *row = &door.rows[door.row_count++];
        *row = (door_row_t){.online = !w.pass && c->online_known && c->online, .wechat = w.pass != 0};
        (void)snprintf(row->id, sizeof(row->id), "%s", c->id);
        (void)snprintf(row->label, sizeof(row->label), "%s", c->name);
    }
}

static uint32_t digest(void)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < door.row_count; ++i) {
        const char *const parts[] = {door.rows[i].id, door.rows[i].room};
        for (size_t k = 0; k < 2U; ++k) {
            for (const char *p = parts[k]; *p; ++p) hash = (hash ^ (uint8_t)*p) * 16777619U;
            hash = (hash ^ '|') * 16777619U;
        }
    }
    return hash;
}

void door_access_list_render(void)
{
    /* The door numbers begin with the way to the directory. */
    const bool entry = !keypad.people && !keypad.among[0];
    if (keypad.people) collect_people();
    else collect(keypad.among);
    if (entry && door.row_count >= DOOR_ROWS_MAX) door.row_count = DOOR_ROWS_MAX - 1U;
    const uint32_t now = digest() ^ (keypad.people ? 0x9e3779b9U : 0U);
    if (now != keypad.shown) door_rows_invalidate();
    keypad.shown = now;
    xiaotai_view_item_t *items = door.items;
    if (entry) {
        items[0] = (xiaotai_view_item_t){.label = "通讯录", .detail = "按名字找人", .intent = XIAOTAI_INTENT_APP_DOOR_DIRECTORY,
            .icon = XIAOTAI_ICON_CONTACTS, .value = 1};
        ++items;
    }
    for (uint32_t i = 0; i < door.row_count; ++i) {
        const door_row_t *row = &door.rows[i];
        items[i] = (xiaotai_view_item_t){
            .label = row->label, .detail = row->online ? "在线" : row->wechat ? "微信" : "离线",
            .intent = XIAOTAI_INTENT_APP_DOOR_DIAL, .icon = XIAOTAI_ICON_PHONE,
            .value = (int32_t)(DOOR_ROW_TOKEN_BASE + door.row_generation * DOOR_ROWS_MAX + i),
        };
    }
    const xiaotai_view_action_t back = {
        .label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_DOOR_BACK,
    };
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_DIRECTORY, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DEVICES,
        .title = keypad.people ? "通讯录" : keypad.among[0] ? "请选择" : "门牌号", .actions = &back, .action_count = 1,
        .items = door.items, .item_count = door.row_count + (entry ? 1U : 0U),
    };
    if (!door.row_count) {
        xiaotai_contacts_status_t status;
        xiaotai_contacts_status(&status);
        if (status.loading) view.subtitle = "正在同步通讯录";
        else if (!status.generation || status.error != XIAOTAI_OK) view.subtitle = "通讯录暂不可用，请返回重试";
        else {
            view.subtitle = keypad.people ? "通讯录是空的" : "还没有门牌号";
            view.text = "请在小程序里添加住户，备注里写房号";
        }
        /* The way to the directory stays where there is nothing else. */
        if (!entry) view.text = view.text ? view.text : view.subtitle;
    }
    door_show_list(&view);
}

/* Devices that are online first, then WeChat; a device known to be offline
 * only where there is nothing else. */
static void call_room(const char *room, const char *label)
{
    door_target_t targets[DOOR_TARGETS_MAX];
    uint8_t count = 0;
    for (unsigned turn = 0; turn < 3U && !(turn == 2U && count); ++turn) {
        walk_t w = {.pass = 0};
        char found[DOOR_ROOM_SIZE];
        while (count < DOOR_TARGETS_MAX && next(&w)) {
            const bool online = w.contact.online_known && w.contact.online;
            if (turn == 1U ? !w.pass : (w.pass || online != (turn == 0U))) continue;
            if (!door_room_of(w.contact.name, found) || strcmp(found, room)) continue;
            targets[count].wechat = w.pass != 0;
            (void)snprintf(targets[count++].id, sizeof(targets[0].id), "%s", w.contact.id);
        }
    }
    /* The door plate is what the visitor is told about, too. */
    if (!count) xiaotai_view_toast("没有这个房号", 0);
    else door_hunt_targets(targets, count, label, label[0] ? label : room);
}

void door_access_call_row(const door_row_t *row)
{
    char label[XIAOTAI_SETTING_TEXT_MAX], room[DOOR_ROOM_SIZE];
    memcpy(label, row->label, sizeof(label));
    memcpy(room, row->room, sizeof(room));
    if (room[0]) {
        call_room(room, label);
        return;
    }
    door_target_t target = {.wechat = row->wechat};
    memcpy(target.id, row->id, sizeof(target.id));
    door_hunt_targets(&target, 1, label, label);
}

/* ----------------------------------------------------------------- keypad */

static bool typing(void)
{
    return door.foreground && door.access && door.page == DOOR_HOME && !door.overlay && !door.setup && !door_busy();
}

static void clear_typed(void)
{
    if (keypad.timer) xiaotai_timer_cancel(keypad.timer);
    keypad.timer = 0;
    keypad.typed[0] = '\0';
}

void door_access_clear(void) { clear_typed(); }

static void left(void *ctx)
{
    (void)ctx;
    keypad.timer = 0;
    keypad.typed[0] = '\0';
    if (door.foreground && door.page == DOOR_HOME) door_render();
}

static void digit(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!typing() || args->value < 0 || args->value > 9) return;
    size_t length = strlen(keypad.typed);
    if (memchr(args->text, '\0', sizeof(args->text)) && args->text[0]) {
        (void)snprintf(keypad.typed, sizeof(keypad.typed), "%.*s", (int)DOOR_ROOM_TYPED_MAX, args->text);     /* a form: all of them */
    } else if (length < DOOR_ROOM_TYPED_MAX) {
        keypad.typed[length] = (char)('0' + args->value);
        keypad.typed[length + 1U] = '\0';
    }
    door_sound_key();
    if (keypad.timer) xiaotai_timer_cancel(keypad.timer);
    if (xiaotai_timer_start(CLEAR_MS, left, NULL, &keypad.timer) != XIAOTAI_OK) keypad.timer = 0;
    door_render();
}

static void clear(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!typing()) return;
    door_sound_key();
    clear_typed();
    door_render();
}

bool door_access_call(const char *typed)
{
    char room[DOOR_ROOM_TYPED_MAX + 1U];
    size_t length = 0;
    for (; typed[length] && length < DOOR_ROOM_TYPED_MAX; ++length) {
        if (typed[length] < '0' || typed[length] > '9') return false;
        room[length] = typed[length];
    }
    room[length] = '\0';
    if (!typing() || typed[length]) return false;
    const xiaotai_app_door_state_t state = door_home_state();
    const char *why = !room[0] ? "请输入房号" : state == XIAOTAI_APP_DOOR_NO_NETWORK ? "网络未连接" :
                      state == XIAOTAI_APP_DOOR_NOT_READY ? "设备尚未就绪" : NULL;
    xiaotai_contacts_status_t status;
    xiaotai_contacts_status(&status);
    if (!why && !status.generation) {
        (void)xiaotai_contacts_refresh();
        why = "通讯录还没有同步好，请稍后再试";
    }
    if (!why) {
        collect(room);
        if (!door.row_count) why = "没有这个房号";
    }
    if (why) {
        xiaotai_view_toast(why, 0);
        return false;
    }
    clear_typed();
    if (door.row_count == 1U) {
        door_render();
        door_access_call_row(&door.rows[0]);
        return door_hunt_active();
    }
    memcpy(keypad.among, room, sizeof(keypad.among));
    keypad.people = false;
    door_page(DOOR_DIRECTORY);
    return true;
}

static void call(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!typing()) return;
    door_sound_key();
    (void)door_access_call(keypad.typed);
}

void door_access_list(bool people)
{
    keypad.among[0] = '\0';
    keypad.people = people;
    (void)xiaotai_contacts_refresh();
    door_rows_invalidate();
    door_page(DOOR_DIRECTORY);
}

bool door_access_back(void)
{
    if (!door.access || door.page != DOOR_DIRECTORY || !keypad.people) return false;
    keypad.people = false;
    door_rows_invalidate();
    door_render();
    return true;
}

void door_access_home_render(const xiaotai_view_indicator_t *network)
{
    static const char *const labels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "清除", "0", "呼叫"};
    const xiaotai_app_door_state_t state = door_home_state();
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
    size_t count = 0;
    actions[count++] = (xiaotai_view_action_t){.label = "门牌号", .intent = XIAOTAI_INTENT_APP_DOOR_DIRECTORY,
        .icon = XIAOTAI_ICON_GROUP};
    if (door.peer_count)
        actions[count++] = (xiaotai_view_action_t){.label = "管理处", .intent = XIAOTAI_INTENT_APP_DOOR_RING,
            .icon = XIAOTAI_ICON_PHONE};
    actions[count++] = (xiaotai_view_action_t){.intent = XIAOTAI_INTENT_APP_DOOR_HOME,
        .trigger = XIAOTAI_TRIGGER_SWIPE_LEFT, .value = 1, .icon = door_pager_icon(1)};
    actions[count++] = (xiaotai_view_action_t){.intent = XIAOTAI_INTENT_APP_DOOR_HOME,
        .trigger = XIAOTAI_TRIGGER_SWIPE_RIGHT, .value = -1, .icon = door_pager_icon(-1)};
    actions[count++] = (xiaotai_view_action_t){.intent = XIAOTAI_INTENT_APP_DOOR_SETTINGS,
        .trigger = XIAOTAI_TRIGGER_LONG_PRESS};
    for (size_t i = 0; i < 12U; ++i) {
        door.items[i] = (xiaotai_view_item_t){.label = labels[i],
            .intent = i == 9U ? XIAOTAI_INTENT_APP_DOOR_KEY_CLEAR : i == 11U ? XIAOTAI_INTENT_APP_DOOR_KEY_CALL :
                      XIAOTAI_INTENT_APP_DOOR_KEY_DIGIT,
            .value = i == 10U ? 0 : i < 9U ? (int32_t)i + 1 : 0};
    }
    xiaotai_app_door_keypad_view_t custom = {.ready = state != XIAOTAI_APP_DOOR_NO_NETWORK && state != XIAOTAI_APP_DOOR_NOT_READY};
    memcpy(custom.typed, keypad.typed, sizeof(keypad.typed));
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_KEYPAD, .custom = XIAOTAI_APP_DOOR_TEMPLATE_KEYPAD,
        .title = door_title(), .subtitle = keypad.typed,
        .status = state == XIAOTAI_APP_DOOR_NO_NETWORK ? "网络未连接" : state == XIAOTAI_APP_DOOR_NOT_READY ? "设备尚未就绪" : NULL,
        .text = "请输入房号", .custom_data = &custom, .custom_size = sizeof(custom),
        .indicators = network, .indicator_count = 1, .items = door.items, .item_count = 12,
        .actions = actions, .action_count = count,
        .flags = XIAOTAI_VIEW_TIME | (!door_busy() ? XIAOTAI_VIEW_MAY_SLEEP : 0),
    };
    const xiaotai_view_field_t field = {.kind = XIAOTAI_FIELD_DIGITS, .label = "房号", .max = 6, .text = keypad.typed,
                                        .intent = XIAOTAI_INTENT_APP_DOOR_KEY_DIGIT};
    if (!door_station_template()) {
        view.kind = XIAOTAI_VIEW_FORM;
        view.custom = NULL;
        view.items = NULL;
        view.item_count = 0;
        view.fields = &field;
        view.field_count = 1;
        actions[count++] = (xiaotai_view_action_t){.label = "呼叫", .intent = XIAOTAI_INTENT_APP_DOOR_KEY_CALL,
            .icon = XIAOTAI_ICON_PHONE, .role = XIAOTAI_ROLE_PRIMARY, .style = XIAOTAI_STYLE_POSITIVE};
        view.action_count = count;
    }
    door_register_view(&view);
}

xiaotai_err_t door_init_access(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_KEY_DIGIT, digit, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_KEY_CLEAR, clear, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_KEY_CALL, call, NULL);
    return err;
}
