// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <stdio.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/port/os.h"

#define ROOM XIAOTAI_APP_COMPANION_PAGE_ROOM
#define FORM XIAOTAI_APP_COMPANION_PAGE_ROOM_FORM
#define CONTROL XIAOTAI_INTENT_APP_COMPANION_ROOM
#define TALK XIAOTAI_INTENT_APP_COMPANION_ROOM_TALK
#define EDIT_CODE "companion.room.code"
#define EDIT_PASSWORD "companion.room.password"
#define LOCAL_HOLD_REFUSED "companion.room.local_refused"
#define OFFLINE_MS 10000U
#define PTT_STALE_US INT64_C(750000)
#define ROWS 3U

typedef enum { JOIN_OPTIONAL = 1, JOIN_OPEN, JOIN_PASSWORD, CREATE_OPTIONAL, CREATE_PASSWORD } form_t;
typedef enum { OPEN_CREATE = 1, OPEN_JOIN, OPEN_PASSWORD_JOIN, OPEN_PASSWORD_CREATE,
               CONNECT, DISCONNECT, REFRESH, LEAVE, PREVIOUS, NEXT, SUBMIT, TOGGLE_TALK } command_t;

static struct {
    bool on_room, on_form, attendance, attendance_retry, offline_elapsed, submitted;
    uint32_t offline_timer, attendance_timer, held, rejected, leave_version;
    int64_t last_press_us;
    uint16_t page;
    form_t form;
    char code[XIAOTAI_ROOM_CODE_DIGITS + 1U], password[XIAOTAI_ROOM_PASSWORD_DIGITS + 1U];
    char leave_room[XIAOTAI_ROOM_ID_MAX], title[XIAOTAI_ROOM_CODE_DIGITS + 1U], subtitle[64];
    char names[ROWS][XIAOTAI_ROOM_NAME_MAX];
} room;

static bool keys(void) { return !strcmp(companion.style->name, XIAOTAI_APP_COMPANION_STYLE_KEYS); }
static bool visible(void) { return room.on_room || room.on_form; }
static bool joined(const xiaotai_room_status_t *s)
{
    return s->generation && (s->phase == XIAOTAI_ROOM_LISTENING || s->phase == XIAOTAI_ROOM_SPEAKING);
}

static const char *error_text(int32_t code)
{
    switch (code) {
    case XIAOTAI_ROOM_CODE_UNBOUND: return "请检查设备绑定";
    case XIAOTAI_ROOM_CODE_WRONG_PASSWORD: return "密码错误";
    case XIAOTAI_ROOM_CODE_NOT_FOUND: return "房间不存在";
    case XIAOTAI_ROOM_CODE_FULL: return "房间已满";
    case XIAOTAI_ROOM_CODE_SUPERSEDED: return "等待旧连接释放";
    case XIAOTAI_ROOM_CODE_ALREADY_MEMBER: return "请先退出当前房间";
    case XIAOTAI_ROOM_CODE_PASSWORD_LOCKED: return "密码错误过多，请稍后";
    case 42900: return "操作过快，请稍后";
    default: return NULL;
    }
}

static const char paused_text[] = "正在维护，稍后同步";
static const char refused_while_paused[] = "正在维护，请稍后再试";

static const char *status_text(const xiaotai_room_status_t *s)
{
    if (companion_room_maintenance()) return paused_text;
    if (!companion_home_network_up()) return "网络未连接";
    if (s->busy) return "正在处理";
    if (s->error) {
        const char *text = error_text(s->code);
        return text ? text : "连接失败，请刷新";
    }
    switch (s->phase) {
    case XIAOTAI_ROOM_SYNCING: return "正在同步房间";
    case XIAOTAI_ROOM_EMPTY: return "尚未加入房间";
    case XIAOTAI_ROOM_CONNECTING: return "正在连接房间";
    case XIAOTAI_ROOM_LISTENING: return "收听中";
    case XIAOTAI_ROOM_SPEAKING: return "讲话中";
    case XIAOTAI_ROOM_LEAVING: return "正在退出房间";
    case XIAOTAI_ROOM_BLOCKED: return "连接失败，请刷新";
    case XIAOTAI_ROOM_WAITING:
        if (s->wait == XIAOTAI_ROOM_WAIT_MANUAL) return "等待连接";
        if (s->wait == XIAOTAI_ROOM_WAIT_ACTIVITY) return "其他会话中，稍后恢复";
        if (s->wait == XIAOTAI_ROOM_WAIT_RELEASE) return "等待旧连接释放";
        if (s->wait == XIAOTAI_ROOM_WAIT_OFFLINE) return "未连接";
        return "等待连接";
    default: return "未连接";
    }
}

static void release(void)
{
    const uint32_t generation = room.held;
    room.held = 0;
    room.last_press_us = 0;
    if (generation) (void)xiaotai_room_talk(generation, false);
}

static void leave_confirmed(bool accepted);

static void cancel_offline(void)
{
    if (room.offline_timer) xiaotai_timer_cancel(room.offline_timer);
    room.offline_timer = 0;
    room.offline_elapsed = false;
}

static void offline_expired(void *ctx)
{
    (void)ctx;
    room.offline_timer = 0;
    room.offline_elapsed = true;
    companion_setup_refresh();
}

static void refresh_setup(void *ctx)
{
    (void)ctx;
    companion_setup_refresh();
}

static bool defer_offline_setup(void)
{
    if (!visible() || room.offline_elapsed) return false;
    if (!room.offline_timer && xiaotai_timer_start(OFFLINE_MS, offline_expired, NULL, &room.offline_timer) != XIAOTAI_OK)
        return false;
    return true;
}

static void reconcile_attendance(void *ctx)
{
    (void)ctx;
    if (room.attendance_timer) xiaotai_timer_cancel(room.attendance_timer);
    room.attendance_timer = 0;
    if (room.attendance == visible()) return;
    const xiaotai_err_t error = xiaotai_room_attend(visible());
    if (error == XIAOTAI_OK) room.attendance = visible();
    else if (!room.attendance_retry && error == XIAOTAI_ERR_NO_MEM &&
             xiaotai_timer_start(50, reconcile_attendance, NULL, &room.attendance_timer) == XIAOTAI_OK) {
        room.attendance_retry = true;
    } else {
        xiaotai_eventlog_warn("companion", "room.attendance", 0, error);
        if (visible()) companion_refuse(CONTROL, error);
    }
}

static void page_visible(const char *id)
{
    const bool was_visible = visible();
    room.on_room = id && !strcmp(id, ROOM);
    room.on_form = id && !strcmp(id, FORM);
    if (!room.on_room) {
        release();
        room.rejected = 0;
        companion_confirm_cancel(leave_confirmed);
    }
    if (!room.on_form) room.submitted = false;
    if (!visible()) {
        if (room.offline_timer && xiaotai_run(refresh_setup, NULL) != XIAOTAI_OK)
            xiaotai_eventlog_warn("companion", "room.offline.refresh", 0, XIAOTAI_ERR_NO_MEM);
        room.page = 0;
        cancel_offline();
    }
    if (was_visible != visible()) {
        room.attendance_retry = false;
        reconcile_attendance(NULL);
    }
}

static uint8_t control_state(const xiaotai_room_status_t *s, bool requires_assignment)
{
    if (s->busy) return XIAOTAI_STATE_BUSY;
    return !companion_home_network_up() || !s->attending ||
        (requires_assignment && !s->assignment_known) ? XIAOTAI_STATE_DISABLED : 0;
}

static void add_item(xiaotai_view_t *view, const char *label, command_t op, uint8_t state)
{
    if (view->item_count >= XIAOTAI_VIEW_ITEMS_MAX) return;
    companion.buffer->items[view->item_count++] = (xiaotai_view_item_t){
        .label = label, .intent = CONTROL, .value = op, .state = state};
}

static void add_action(xiaotai_view_t *view, const xiaotai_view_action_t *action)
{
    if (view->action_count < XIAOTAI_VIEW_ACTIONS_MAX) companion.buffer->actions[view->action_count++] = *action;
}

static const char *member_name(const xiaotai_room_member_t *member, char *out)
{
    if (member->device_id[0]) {
        for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
            const xiaotai_app_companion_part_t *p = xiaotai_app_companion_parts[i];
            if (p->room_member_name && p->room_member_name(member->device_id, out, XIAOTAI_ROOM_NAME_MAX)) return out;
        }
    }
    const char *name = member->device_id[0] ? member->device_id : member->name[0] ? member->name : member->participant_id;
    size_t length = strnlen(name, XIAOTAI_ROOM_NAME_MAX);
    if (length == XIAOTAI_ROOM_NAME_MAX) {
        --length;
        while (length && ((unsigned char)name[length] & 0xc0U) == 0x80U) --length;
    }
    memcpy(out, name, length);
    out[length] = '\0';
    return out;
}

static uint16_t page_count(const xiaotai_room_status_t *s)
{
    return s->listed_count ? (uint16_t)((s->listed_count + ROWS - 1U) / ROWS) : 1U;
}

static void members(xiaotai_view_t *view, const xiaotai_room_status_t *s)
{
    const unsigned pages = page_count(s);
    if (room.page >= pages) room.page = (uint16_t)(pages - 1U);
    snprintf(room.subtitle, sizeof(room.subtitle), "%u 人在线 %u/%u", (unsigned)s->member_count,
             (unsigned)room.page + 1U, pages);
    view->subtitle = room.subtitle;
    for (unsigned i = 0; i < ROWS && s->members_ready; ++i) {
        xiaotai_room_member_t member;
        uint32_t revision = 0;
        if (!xiaotai_room_members((uint16_t)(room.page * ROWS + i), &member, 1, &revision) ||
            revision != s->members_revision) break;
        static const char *const state[][3] = {{"收听中", "麦克风开启", "讲话中"},
            {"（本机） 收听中", "（本机） 麦克风开启", "（本机） 讲话中"}};
        const unsigned mic = (unsigned)member.mic <= XIAOTAI_ROOM_MIC_SPEAKING ? (unsigned)member.mic : 0U;
        companion.buffer->items[view->item_count++] = (xiaotai_view_item_t){
            .label = member_name(&member, room.names[i]), .detail = state[member.self][mic]};
    }
    if (pages > 1U) {
        add_action(view, &(xiaotai_view_action_t){.icon = XIAOTAI_ICON_PREVIOUS, .intent = CONTROL,
            .value = PREVIOUS, .state = room.page ? 0 : XIAOTAI_STATE_DISABLED});
        add_action(view, &(xiaotai_view_action_t){.icon = XIAOTAI_ICON_NEXT, .intent = CONTROL,
            .value = NEXT, .state = room.page + 1U < pages ? 0 : XIAOTAI_STATE_DISABLED});
    }
}

static void build_room(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    view->kind = XIAOTAI_VIEW_LIST;
    view->status = status_text(&s);
    view->items = companion.buffer->items;
    view->actions = companion.buffer->actions;
    add_action(view, &(xiaotai_view_action_t){.icon = XIAOTAI_ICON_BACK, .role = XIAOTAI_ROLE_BACK,
        .intent = keys() && s.generation ? XIAOTAI_INTENT_APP_COMPANION_SESSION : XIAOTAI_INTENT_APP_COMPANION_BACK,
        .value = keys() ? (int32_t)s.generation : 0});
    const bool paused = companion_room_maintenance();
    const uint8_t general = paused ? XIAOTAI_STATE_DISABLED : control_state(&s, false);
    if (!keys()) add_action(view, &(xiaotai_view_action_t){.icon = XIAOTAI_ICON_REFRESH, .intent = CONTROL,
        .value = REFRESH, .state = general});
    if (!s.assigned) {
        const uint8_t edit = control_state(&s, true);
        const bool key_menu = keys();
        const bool full_key_menu = key_menu && companion.config.room_full_unassigned_key_menu;
        if (full_key_menu) {
            add_item(view, "开始发言", TOGGLE_TALK, XIAOTAI_STATE_DISABLED);
            add_item(view, "加入房间", OPEN_JOIN, edit);
        }
        add_item(view, "创建房间", OPEN_CREATE, key_menu && paused ? XIAOTAI_STATE_DISABLED : edit);
        if (!full_key_menu) add_item(view, "加入房间", OPEN_JOIN, edit);
        if (key_menu) {
            add_item(view, "加入密码房间", OPEN_PASSWORD_JOIN, edit);
            add_item(view, "创建密码房间", OPEN_PASSWORD_CREATE, edit);
            add_item(view, "连接房间", CONNECT, XIAOTAI_STATE_DISABLED);
            add_item(view, "断开连接", DISCONNECT, XIAOTAI_STATE_DISABLED);
            if (full_key_menu) add_item(view, "退出房间", LEAVE, XIAOTAI_STATE_DISABLED);
        }
    } else {
        memcpy(room.title, s.room_code, sizeof(room.title));
        view->title = room.title;
        members(view, &s);
        if (keys()) {
            add_item(view, s.transmitting ? "停止发言" : "开始发言", TOGGLE_TALK,
                joined(&s) && !general ? 0 : XIAOTAI_STATE_DISABLED);
            add_item(view, "连接房间", CONNECT, joined(&s) ? XIAOTAI_STATE_DISABLED : general);
            add_item(view, "断开连接", DISCONNECT, s.generation && !paused ? 0 : XIAOTAI_STATE_DISABLED);
            add_item(view, "退出房间", LEAVE, general);
        } else {
            add_action(view, &(xiaotai_view_action_t){.label = "退出房间", .intent = CONTROL, .value = LEAVE, .state = general});
        }
        const bool pointer = companion.config.room_pointer_hold;
        const bool held = companion.config.room_local_hold || pointer ? s.transmitting : room.held != 0;
        const bool ready = !general && (joined(&s) || (pointer && !s.generation));
        if (!keys()) add_action(view, &(xiaotai_view_action_t){.label = held ? "松开结束讲话" : pointer ? "按下讲话" : "按住讲话",
            .role = XIAOTAI_ROLE_PRIMARY,
            .intent = TALK, .trigger = XIAOTAI_TRIGGER_HOLD, .value = (int32_t)s.generation,
            .state = ready ? (pointer ? XIAOTAI_STATE_HOLD_FROM_POINTER : 0) : XIAOTAI_STATE_DISABLED});
    }
    if (keys()) add_item(view, "刷新", REFRESH, general);
}

static bool is_create(form_t mode) { return mode == CREATE_OPTIONAL || mode == CREATE_PASSWORD; }
static bool form_arg(int32_t mode) { return mode >= JOIN_OPTIONAL && mode <= CREATE_PASSWORD; }
static const char *form_action(void) { return is_create(room.form) ? "room.create" : "room.join"; }

static void build_form(xiaotai_view_t *view, int32_t mode)
{
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    room.form = (form_t)mode;
    view->kind = XIAOTAI_VIEW_FORM;
    view->title = is_create(room.form) ? "创建房间" : "加入房间";
    const bool pending = companion_session_pending(form_action(), s.revision);
    const bool paused = companion_room_maintenance();
    view->status = pending && !paused ? "正在处理" : status_text(&s);
    if (!is_create(room.form)) companion.buffer->fields[view->field_count++] = (xiaotai_view_field_t){
        .kind = XIAOTAI_FIELD_DIGITS, .label = "六位房间号", .intent = EDIT_CODE,
        .max = XIAOTAI_ROOM_CODE_DIGITS, .text = room.code};
    if (room.form != JOIN_OPEN) {
        const bool optional = room.form == JOIN_OPTIONAL || room.form == CREATE_OPTIONAL;
        companion.buffer->fields[view->field_count++] = (xiaotai_view_field_t){.kind = XIAOTAI_FIELD_DIGITS,
            .label = optional ? "密码（可选）" : "密码", .intent = EDIT_PASSWORD,
            .max = XIAOTAI_ROOM_PASSWORD_DIGITS, .text = room.password,
            .flags = XIAOTAI_FIELD_MASKED | (optional ? XIAOTAI_FIELD_ALLOW_EMPTY : 0)};
    }
    view->fields = companion.buffer->fields;
    view->actions = companion.buffer->actions;
    add_action(view, &(xiaotai_view_action_t){.label = "确认", .role = XIAOTAI_ROLE_PRIMARY, .intent = CONTROL,
        .value = SUBMIT, .state = paused ? XIAOTAI_STATE_DISABLED : pending ? XIAOTAI_STATE_BUSY : control_state(&s, true)});
    add_action(view, &(xiaotai_view_action_t){.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK,
        .intent = XIAOTAI_INTENT_APP_COMPANION_BACK});
}

static bool digits(const char *text, size_t size, size_t max, bool exact)
{
    const size_t n = strnlen(text, size);
    if (n > max || n == size || (exact && n != max)) return false;
    for (size_t i = 0; i < n; ++i) if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

static void edit(const xiaotai_args_t *args, void *ctx)
{
    const bool password = ctx != NULL;
    if (!room.on_form || (password ? room.form == JOIN_OPEN : is_create(room.form))) return;
    const size_t max = password ? XIAOTAI_ROOM_PASSWORD_DIGITS : XIAOTAI_ROOM_CODE_DIGITS;
    if (!digits(args->text, sizeof(args->text), max, false)) companion_refuse(CONTROL, XIAOTAI_ERR_INVALID_ARG);
    else strcpy(password ? room.password : room.code, args->text);
    companion_render();
}

static bool ai_holds_room(void)
{
    const char *activity = NULL;
    /* A product that leaves AI running still refuses the Room button while
     * AI owns it; joining would otherwise mutate membership before admission. */
    return companion.config.room_keeps_talk && xiaotai_activity_current(&activity) && activity && !strcmp(activity, "ai");
}

static xiaotai_err_t submit(const xiaotai_room_status_t *s)
{
    if (control_state(s, true)) return s->busy ? XIAOTAI_ERR_BUSY : XIAOTAI_ERR_INVALID_STATE;
    const bool create = is_create(room.form);
    const char *pass = room.form == JOIN_OPEN ? "" : room.password;
    const bool required = room.form == JOIN_PASSWORD || room.form == CREATE_PASSWORD;
    if ((!create && !digits(room.code, sizeof(room.code), XIAOTAI_ROOM_CODE_DIGITS, true)) ||
        ((required || pass[0]) && !digits(pass, sizeof(room.password), XIAOTAI_ROOM_PASSWORD_DIGITS, true))) {
        companion_refuse_text(CONTROL, XIAOTAI_ERR_INVALID_ARG,
            create ? "请输入四位密码" : "请输入六位房间号和四位密码（如需）");
        return XIAOTAI_OK;
    }
    if (ai_holds_room()) return XIAOTAI_ERR_BUSY;
    const char *action = form_action();
    if (companion_session_pending(action, s->revision)) return XIAOTAI_OK;
    const xiaotai_err_t err = create ? xiaotai_room_create(pass) : xiaotai_room_join(room.code, pass);
    if (err == XIAOTAI_OK) {
        room.submitted = true;
        companion_session_accepted(action, s->revision);
    }
    return err;
}

static xiaotai_err_t post_leave(void) { return xiaotai_intent_post(XIAOTAI_INTENT_ROOM_LEAVE, NULL); }

static void leave_confirmed(bool accepted)
{
    if (!accepted || !room.on_room) return;
    if (companion_room_maintenance()) {
        companion_refuse_text(CONTROL, XIAOTAI_ERR_BUSY, refused_while_paused);
        return;
    }
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    const xiaotai_err_t err = s.assigned && s.assignment_version == room.leave_version &&
        !strcmp(s.room_id, room.leave_room) ? post_leave() : XIAOTAI_ERR_INVALID_STATE;
    if (err != XIAOTAI_OK) companion_refuse(CONTROL, err);
}

static xiaotai_err_t do_command(command_t op)
{
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    if (room.on_form) return op == SUBMIT ? submit(&s) : XIAOTAI_ERR_INVALID_STATE;
    if (op == PREVIOUS || op == NEXT) {
        if (op == PREVIOUS && room.page) --room.page;
        if (op == NEXT && room.page + 1U < page_count(&s)) ++room.page;
        companion_render();
        return XIAOTAI_OK;
    }
    if (op == DISCONNECT) return s.generation ? xiaotai_room_disconnect() : XIAOTAI_ERR_INVALID_STATE;
    if (op == CONNECT && ai_holds_room()) return XIAOTAI_ERR_BUSY;
    if (op == OPEN_JOIN && ai_holds_room()) return XIAOTAI_ERR_BUSY;
    const uint8_t state = control_state(&s, op == OPEN_JOIN || op == OPEN_CREATE || op == OPEN_PASSWORD_CREATE || op == OPEN_PASSWORD_JOIN);
    if (state) return s.busy ? XIAOTAI_ERR_BUSY : XIAOTAI_ERR_INVALID_STATE;
    switch (op) {
    case TOGGLE_TALK: {
        if (!keys() || !joined(&s)) return XIAOTAI_ERR_INVALID_STATE;
        bool pressed;
        const xiaotai_err_t err = xiaotai_room_toggle_talk(s.generation, &pressed);
        if (err == XIAOTAI_OK) room.held = pressed ? s.generation : 0;
        return err;
    }
    case OPEN_CREATE:
        if (keys()) return xiaotai_room_create(NULL);
        return companion_show_page(FORM, CREATE_OPTIONAL);
    case OPEN_JOIN: return companion_show_page(FORM, keys() ? JOIN_OPEN : JOIN_OPTIONAL);
    case OPEN_PASSWORD_JOIN: return keys() ? companion_show_page(FORM, JOIN_PASSWORD) : XIAOTAI_ERR_INVALID_ARG;
    case OPEN_PASSWORD_CREATE: return keys() ? companion_show_page(FORM, CREATE_PASSWORD) : XIAOTAI_ERR_INVALID_ARG;
    case CONNECT: return s.assigned ? xiaotai_room_connect() : XIAOTAI_ERR_INVALID_STATE;
    case REFRESH: return xiaotai_room_refresh();
    case LEAVE:
        if (!s.assigned) return XIAOTAI_ERR_INVALID_STATE;
        if (!companion.config.room_leave_confirm) return post_leave();
        if (!companion_confirm_open("退出房间", "退出后需要重新加入", leave_confirmed)) return XIAOTAI_ERR_BUSY;
        room.rejected = room.held;
        release();
        room.leave_version = s.assignment_version;
        memcpy(room.leave_room, s.room_id, sizeof(room.leave_room));
        return XIAOTAI_OK;
    default: return XIAOTAI_ERR_INVALID_ARG;
    }
}

static command_t command(const xiaotai_args_t *args)
{
    if (!memchr(args->text, '\0', sizeof(args->text))) return 0;
    if (!args->text[0]) return (command_t)args->value;
    if (!strcmp(args->text, "page")) return args->value == -1 ? PREVIOUS : args->value == 1 ? NEXT : 0;
    static const char *const names[] = {[OPEN_CREATE] = "create", [OPEN_JOIN] = "join", [CONNECT] = "connect",
        [DISCONNECT] = "disconnect", [REFRESH] = "refresh", [LEAVE] = "leave", [SUBMIT] = "submit"};
    for (size_t i = 1; i < sizeof(names) / sizeof(*names); ++i) if (names[i] && !strcmp(args->text, names[i])) return (command_t)i;
    return 0;
}

static void control(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!visible()) { ++companion.status.stale; return; }
    const command_t op = command(args);
    const bool navigation = !room.on_form && (op == PREVIOUS || op == NEXT || op == OPEN_JOIN ||
        op == OPEN_PASSWORD_JOIN || op == OPEN_PASSWORD_CREATE || (op == OPEN_CREATE && !keys()));
    if (companion_room_maintenance() && !navigation) {
        companion_refuse_text(CONTROL, XIAOTAI_ERR_BUSY, refused_while_paused);
        return;
    }
    const xiaotai_err_t err = do_command(op);
    if (err != XIAOTAI_OK) companion_refuse_text(CONTROL, err, err == XIAOTAI_ERR_BUSY ? "设备正忙，请稍后" : NULL);
    else if (visible()) companion_render();
}

static void talk(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!memchr(args->text, '\0', sizeof(args->text))) return;
    const uint32_t generation = (uint32_t)args->value;
    if (!strcmp(args->text, XIAOTAI_ARG_RELEASE)) {
        if (room.held && room.held == generation) {
            release();
            if (room.on_room) companion_render();
        }
        if (room.rejected == generation) room.rejected = 0;
        return;
    }
    if (!room.on_room || strcmp(args->text, XIAOTAI_ARG_PRESS) || !generation || room.rejected == generation ||
        companion.confirm_resolved == leave_confirmed || !companion_home_network_up() || companion_room_maintenance()) return;
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    if (!joined(&s) || s.generation != generation) return;
    const int64_t now = xiaotai_time_us();
    if (room.held && !s.transmitting && now - room.last_press_us >= PTT_STALE_US) {
        room.rejected = generation;
        release();
        companion_render();
        return;
    }
    const xiaotai_err_t err = xiaotai_room_talk(generation, true);
    if (err == XIAOTAI_OK) {
        const bool first = !room.held;
        room.held = generation;
        room.last_press_us = now;
        if (first) companion_render();
    } else {
        room.rejected = generation;
        if (room.held) { release(); companion_render(); }
        else companion_refuse_text(TALK, err, "尚未就绪或麦克风已关闭");
    }
}

static xiaotai_err_t local_hold(uint32_t ticket, uint32_t generation, bool pressed)
{
    const xiaotai_err_t err = xiaotai_room_hold_local(generation, ticket, pressed);
    if (pressed && err != XIAOTAI_OK) {
        const xiaotai_args_t args = {.value = err};
        (void)xiaotai_event_emit(LOCAL_HOLD_REFUSED, &args, &generation, sizeof(generation));
    }
    return err;
}

static xiaotai_err_t register_local_hold(void)
{
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->register_local_room_hold) return part->register_local_room_hold(TALK, local_hold,
            companion.config.room_pointer_hold ? xiaotai_room_hold_local_refresh : NULL);
    }
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t session(const xiaotai_args_t *args)
{
    if (!memchr(args->text, '\0', sizeof(args->text))) return XIAOTAI_ERR_INVALID_ARG;
    if (strcmp(args->text, "room.disconnect") && !(room.on_room && keys() && !args->text[0]))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    if (companion_room_maintenance()) return XIAOTAI_ERR_BUSY;
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    if (!room.on_room || !s.generation || (uint32_t)args->value != s.generation) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xiaotai_room_disconnect();
    if (err == XIAOTAI_OK) (void)companion_reset_home(0);
    return err;
}

static bool home(companion_home_session_t *session_view)
{
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    if (!s.attending || (s.phase != XIAOTAI_ROOM_CONNECTING && !joined(&s))) return false;
    *session_view = (companion_home_session_t){.mode = joined(&s) ? COMPANION_HOME_ROOM : COMPANION_HOME_ROOM_CONNECT};
    return true;
}

static void changed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_CHANGED)) {
        if (companion_home_network_up()) cancel_offline();
        else if (room.held) { room.rejected = room.held; release(); }
    }
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    if (!strcmp(event->name, LOCAL_HOLD_REFUSED)) {
        uint32_t generation = 0;
        if (!event->data || event->size != sizeof(generation)) return;
        memcpy(&generation, event->data, sizeof(generation));
        const bool relevant_zero = companion.config.room_pointer_hold && s.assigned && !control_state(&s, false) &&
            !companion_room_maintenance() && companion.confirm_resolved != leave_confirmed;
        if (room.on_room && (generation || relevant_zero) && s.generation == generation)
            companion_refuse_text(TALK, event->args.value, "尚未就绪或麦克风已关闭");
        return;
    }
    if (room.held && (!joined(&s) || s.generation != room.held)) release();
    if (visible() && !strcmp(event->name, XIAOTAI_EVENT_ROOM_RESULT) && event->data && event->size == sizeof(xiaotai_room_result_t)) {
        const xiaotai_room_result_t *result = event->data;
        if (result->error != XIAOTAI_OK) companion_refuse_text(CONTROL, result->error, error_text(result->code));
        if (room.on_form && room.submitted && result->action ==
            (is_create(room.form) ? XIAOTAI_ROOM_ACTION_CREATE : XIAOTAI_ROOM_ACTION_JOIN)) {
            room.submitted = false;
            if (result->error == XIAOTAI_OK) { companion_back_page(); return; }
        }
    }
    if (visible() || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) companion_render();
}

static void attempt(uint32_t activity, xiaotai_room_attempt_t phase)
{
    companion_feedback_room_attempt(phase == XIAOTAI_ROOM_ATTEMPT_STARTED,
                                    phase == XIAOTAI_ROOM_ATTEMPT_FAILED, activity);
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = XIAOTAI_OK;
    if (companion.config.room_end_feedback) {
        err = xiaotai_room_observe_attempts(attempt);
        if (err != XIAOTAI_OK) return err;
    }
    if (companion.config.room_local_hold || companion.config.room_pointer_hold) {
        if (!companion.has_display || keys()) return XIAOTAI_ERR_NOT_SUPPORTED;
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(LOCAL_HOLD_REFUSED, changed, NULL);
        if (err == XIAOTAI_OK) err = register_local_hold();
        if (err != XIAOTAI_OK) return err;
    }
    if (companion.has_display) {
        static const companion_page_t main = {.id = ROOM, .title = "多人对讲", .build = build_room,
            .icon = XIAOTAI_ICON_GROUP};
        static const companion_page_t form = {.id = FORM, .title = "加入房间", .arg_valid = form_arg, .build = build_form};
        err = companion_page_register(&main);
        if (err == XIAOTAI_OK) err = companion_page_register(&form);
        static const struct { const char *name; xiaotai_intent_fn_t run; bool password; } intents[] = {
            {CONTROL, control, false}, {TALK, talk, false}, {EDIT_CODE, edit, false}, {EDIT_PASSWORD, edit, true},
        };
        for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(intents) / sizeof(*intents); ++i)
            err = xiaotai_intent_handle_ex(intents[i].name, intents[i].run,
                intents[i].password ? (void *)&room : NULL, XIAOTAI_INTENT_LOCAL);
    }
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_ROOM_STATE, changed, NULL},
        {XIAOTAI_EVENT_ROOM_MEMBERS, changed, NULL},
        {XIAOTAI_EVENT_ROOM_VOICE, changed, NULL},
        {XIAOTAI_EVENT_ROOM_RESULT, changed, NULL},
        {XIAOTAI_EVENT_PROVISIONING_CHANGED, changed, NULL},
        {"contacts.changed", changed, NULL},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(*events); ++i)
        err = xiaotai_event_subscribe_static(&events[i]);
    return err;
}

static void foreground(bool entered) { if (!entered) page_visible(NULL); }

static void decorate(xiaotai_view_t *view)
{
    (void)view;
    if (!companion_room_maintenance()) return;
    companion_confirm_cancel(leave_confirmed);
    release();
}

static void reset(void)
{
    release();
    cancel_offline();
    if (room.attendance_timer) xiaotai_timer_cancel(room.attendance_timer);
    memset(&room, 0, sizeof(room));
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_room = {
    .name = "room", .init = init, .page_visible = page_visible, .defer_offline_setup = defer_offline_setup,
    .decorate_view = decorate,
    .home_session = home, .foreground = foreground, .reset = reset, .session = session,
};
