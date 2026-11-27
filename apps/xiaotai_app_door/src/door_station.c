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
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- directory */

void door_render_directory(void)
{
    if (door.access) {
        door_access_list_render();
        return;
    }
    const int devices = door_contact_kind("device");
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    const int users = door_contact_kind("wechat");
#else
    const int users = XIAOTAI_CONTACT_ANY;
#endif
    xiaotai_contacts_status_t status;
    xiaotai_contacts_status(&status);
    xiaotai_view_item_t *const items = door.items;
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
            if (row->wechat && !c.name[0]) (void)snprintf(row->label, sizeof(row->label), "微信联系人");
            else door_device_label(c.id, c.name, row->label, sizeof(row->label));
            const bool chosen = !row->wechat && door_peer_slot(row->id) >= 0;
            items[door.row_count] = (xiaotai_view_item_t){
                .label = row->label,
                .detail = row->wechat ? (door_wechat_video() ? "微信视频" : "微信语音") : row->online ? (chosen ? "室内机 · 在线" : "在线") :
                          chosen ? "室内机 · 离线" : "离线",
                .intent = XIAOTAI_INTENT_APP_DOOR_DIAL, .icon = XIAOTAI_ICON_PHONE,
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
    door_show_list(&view);
}

/* -------------------------------------------------------------- the visitor */

/* A device has one call at a time, so the chosen indoor stations are called
 * one after the other: those online first, in the stored order, until one
 * answers. Each but the last rings for HUNT_RING_MS. */
#define HUNT_GAP_MS 400U
#define HUNT_RING_MS 20000U
#define ROOM_RING_MS 30000U
#define HUNT_BUSY_MS 1500U
#define HUNT_TRIES 6U

typedef struct {
    door_target_t targets[DOOR_TARGETS_MAX];
    char label[XIAOTAI_SETTING_TEXT_MAX];   /* what the screen says, "": the name of each */
    char room[XIAOTAI_SETTING_TEXT_MAX];    /* what is said of it when nobody answered, "": the usual */
    uint8_t count, index, tries;
    bool active, retried, voice, passed;   /* passed: given up here, for the next one */
    uint32_t timer, ring_ms;
} hunt_t;
static hunt_t *hunt;       /* from init on */
static char farewell[XIAOTAI_SETTING_TEXT_MAX + 32];
_Static_assert(DOOR_TARGETS_MAX >= XIAOTAI_APP_DOOR_PEERS_MAX, "every chosen station is called");

const char *door_hunt_farewell(void) { return farewell; }

bool door_hunt_active(void) { return hunt && hunt->active; }

void door_hunt_cancel(void)
{
    if (!hunt) return;
    if (hunt->timer) xiaotai_timer_cancel(hunt->timer);
    memset(hunt, 0, sizeof(*hunt));
}

void door_hunt_caption(char *out, size_t size)
{
    out[0] = '\0';
    if (hunt->active && hunt->count > 1)
        (void)snprintf(out, size, "（%u/%u）", (unsigned)hunt->index + 1U, (unsigned)hunt->count);
}

static xiaotai_err_t dial_device(const char *id, bool voice)
{
    uint32_t session = 0;
    const xiaotai_err_t err = door_dial(id, voice, &session);
    if (err == XIAOTAI_OK) door.dialed_session = session;
    return err;
}

static xiaotai_err_t dial_wechat(const char *id)
{
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    uint32_t session = 0;
    xiaotai_err_t err = xiaotai_wechat_dial(id, XIAOTAI_WECHAT_VIDEO, &session);
    if (err == XIAOTAI_ERR_NOT_SUPPORTED) err = xiaotai_wechat_dial(id, 0, &session);
    if (err == XIAOTAI_OK) door.wechat_session = session;
    return err;
#else
    (void)id;
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}

static void hunt_dial(void);

static void hunt_passed(void *ctx)
{
    (void)ctx;
    hunt->timer = 0;
    if (hunt->active) hunt_dial();
}

/* At least the delay, and until an answer would be let in. */
static bool hunt_wait(uint32_t delay_ms)
{
    const uint32_t hold = xiaotai_call_answer_hold_ms();
    return xiaotai_timer_start(hold > delay_ms ? hold : delay_ms, hunt_passed, NULL, &hunt->timer) == XIAOTAI_OK;
}

static void rung_out(void *ctx)
{
    hunt->timer = 0;
    const uint32_t session = (uint32_t)(uintptr_t)ctx;
    if (!hunt->active) return;
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    if (hunt->targets[hunt->index].wechat) {
        xiaotai_wechat_status_t wechat;
        xiaotai_wechat_status(&wechat);
        if (session != door.wechat_session || wechat.session != session || wechat.state != XIAOTAI_WECHAT_OUTGOING) return;
        hunt->passed = xiaotai_wechat_hangup(session) == XIAOTAI_OK;
        return;
    }
#endif
    if (!door.session || door.session != session) return;
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    if (call.session != door.session || call.state != XIAOTAI_CALL_OUTGOING) return;
    hunt->passed = true;
    if (xiaotai_call_hangup(door.session) != XIAOTAI_OK) hunt->passed = false;
}

void door_hunt_render(void)
{
    char name[XIAOTAI_SETTING_TEXT_MAX], which[32], subtitle[XIAOTAI_SETTING_TEXT_MAX + 48];
    if (hunt->label[0]) (void)snprintf(name, sizeof(name), "%s", hunt->label);
    else door_peer_label(hunt->targets[hunt->index < hunt->count ? hunt->index : 0].id, name, sizeof(name));
    door_hunt_caption(which, sizeof(which));
    (void)snprintf(subtitle, sizeof(subtitle), "%s · 请稍候%s", name, which);
    const xiaotai_view_action_t cancel = {
        .label = "取消", .intent = "door.call.hangup", .icon = XIAOTAI_ICON_HANGUP, .style = XIAOTAI_STYLE_DESTRUCTIVE,
        .role = XIAOTAI_ROLE_BACK,
    };
    const xiaotai_app_door_call_view_t custom = {.phase = XIAOTAI_APP_DOOR_CALL_CONNECTING};
    xiaotai_view_t view = {
        .id = XIAOTAI_APP_DOOR_PAGE_CALL, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_CALL,
        .custom_data = &custom, .custom_size = sizeof(custom),
        .title = "正在呼叫", .subtitle = subtitle, .status = "请稍候",
        .text = hunt->index ? "上一台无人接听，正在呼叫下一台" : "正在准备呼叫", .actions = &cancel, .action_count = 1,
    };
    if (!door_own_template()) {
        view.kind = XIAOTAI_VIEW_CALL;
        view.custom = NULL;
        view.subtitle = name;
        view.text = hunt->index ? "正在呼叫下一台，请稍候" : "请稍候";
    }
    door_register_view(&view);
}

static void hunt_give_up(const char *why)
{
    door_hunt_cancel();
    if (door.page == DOOR_CALL && !door.session) door_finish_presentation();
    xiaotai_view_toast(why, 0);
}

static void hunt_dial(void)
{
    /* Wherever the screen is by now (an alarm rang, somebody paged on): the call brings the door back. */
    if (door.setup || !door.sender || door.session || door.wechat_session) {
        door_hunt_cancel();
        return;
    }
    (void)snprintf(door.called, sizeof(door.called), "%s", hunt->label);
    xiaotai_err_t err = XIAOTAI_ERR_NOT_FOUND;
    for (; hunt->index < hunt->count; ++hunt->index, hunt->tries = 0, hunt->voice = false) {
        const door_target_t *target = &hunt->targets[hunt->index];
        err = target->wechat ? dial_wechat(target->id) : dial_device(target->id, hunt->voice);
        if (err == XIAOTAI_OK) {
            const uint32_t session = target->wechat ? door.wechat_session : door.dialed_session;
            xiaotai_eventlog_note("door", target->wechat ? "ring.wechat" : "ring", session, hunt->index);
            hunt->passed = false;
            if ((hunt->index + 1U < hunt->count || target->wechat) &&
                xiaotai_timer_start(hunt->ring_ms, rung_out, (void *)(uintptr_t)session, &hunt->timer) != XIAOTAI_OK)
                hunt->timer = 0;   /* the call's own timeout ends it */
            return;
        }
        /* The call before is still being closed. */
        if (err == XIAOTAI_ERR_BUSY && (hunt->index > 0 || hunt->voice || hunt->retried) && hunt->tries++ < HUNT_TRIES &&
            hunt_wait(HUNT_GAP_MS)) return;
        if (err == XIAOTAI_ERR_BUSY) break;
    }
    door.called[0] = '\0';
    hunt_give_up(err == XIAOTAI_ERR_BUSY ? "设备正忙，请稍后再试" :
                 err == XIAOTAI_ERR_NOT_FOUND ? (door.access ? "暂时呼叫不到，请稍后再试" : "室内机已移除，请联系管理员") :
                 "呼叫没有发出，请重试");
}

static void hunt_begin(uint32_t delay_ms);

void door_hunt_start(uint32_t delay_ms, const char *except)
{
    door_hunt_cancel();
    for (int online = 1; online >= 0; --online) {
        for (uint8_t i = 0; i < door.peer_count; ++i) {
            if (except && !strcmp(door.peers[i], except)) continue;
            if (door_peer_online(door.peers[i]) == (online != 0))
                memcpy(hunt->targets[hunt->count++].id, door.peers[i], sizeof(hunt->targets[0].id));
        }
        /* Nobody is asked to wait for a device known to be offline while another one is online. */
        if (hunt->count) break;
    }
    if (!hunt->count) return;
    hunt->ring_ms = HUNT_RING_MS;
    if (door.access) {      /* the visitor called no name */
        (void)snprintf(hunt->label, sizeof(hunt->label), "管理处");
        memcpy(hunt->room, hunt->label, sizeof(hunt->room));
    }
    hunt_begin(delay_ms);
}

void door_hunt_targets(const door_target_t *targets, uint8_t count, const char *label, const char *room)
{
    door_hunt_cancel();
    for (uint8_t i = 0; i < count && i < DOOR_TARGETS_MAX; ++i) hunt->targets[hunt->count++] = targets[i];
    (void)snprintf(hunt->label, sizeof(hunt->label), "%s", label);
    (void)snprintf(hunt->room, sizeof(hunt->room), "%s", room);
    hunt->ring_ms = ROOM_RING_MS;
    hunt_begin(0);
}

static void hunt_begin(uint32_t delay_ms)
{
    hunt->active = true;
    if ((delay_ms || xiaotai_call_answer_hold_ms()) && hunt_wait(delay_ms)) {
        door_page(DOOR_CALL);
        return;
    }
    hunt_dial();
}

/* The call just ended was one of a hunt, or of none. */
static bool hunt_stopped(bool answered, bool cancelled, bool broken, bool busy, bool declined)
{
    farewell[0] = '\0';
    if (!hunt->active) return false;
    if (hunt->timer) xiaotai_timer_cancel(hunt->timer);
    hunt->timer = 0;
    if (hunt->room[0] && !answered && !broken && (!cancelled || hunt->passed))
        (void)snprintf(farewell, sizeof(farewell), "%s %s", hunt->room, hunt->count > 1U ? "暂时无人应答" :
                       busy ? "正忙，请稍后再试" : declined ? "现在不方便接听" : "暂时无人应答");
    else if (!answered && !broken && cancelled && hunt->passed)
        (void)snprintf(farewell, sizeof(farewell), "暂时无人应答");
    return true;
}

static bool hunt_next(void)
{
    if (hunt->index + 1U >= hunt->count) return false;
    ++hunt->index;
    hunt->tries = 0;
    hunt->voice = false;
    return hunt_wait(HUNT_GAP_MS);
}

bool door_hunt_ended(const xiaotai_call_status_t *ended, bool answered)
{
    if (!hunt_stopped(answered, ended->end == XIAOTAI_CALL_END_LOCAL, ended->end == XIAOTAI_CALL_END_NETWORK,
                      ended->end == XIAOTAI_CALL_END_BUSY, ended->end == XIAOTAI_CALL_END_PEER_REJECT)) return false;
    const bool over = answered || (ended->end == XIAOTAI_CALL_END_LOCAL && !hunt->passed) || ended->end == XIAOTAI_CALL_END_PREEMPTED ||
                      ended->end == XIAOTAI_CALL_END_SYSTEM || ended->end == XIAOTAI_CALL_END_NO_MEMORY;
    if (!over && !hunt->voice && door_refused_video(ended)) {
        hunt->voice = true;
        hunt->tries = 0;
        if (hunt_wait(HUNT_GAP_MS)) return true;
    } else if (!over && hunt->index + 1U < hunt->count) {
        if (hunt_next()) return true;
    } else if (!over && ended->end == XIAOTAI_CALL_END_BUSY && !hunt->retried) {
        /* Both ends called each other at once, or the last call is still being released. */
        hunt->retried = true;
        if (hunt_wait(HUNT_BUSY_MS)) return true;
    }
    door_hunt_cancel();
    return false;
}

void door_hunt_give_way(uint32_t session)
{
    if (!session || session != door.session) return;
    if (hunt->active) hunt->passed = true;
    if (xiaotai_call_hangup(session) != XIAOTAI_OK) hunt->passed = false;
}

void door_ring_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground || !door.sender || door.overlay || door.setup) return;
    if (door.page == DOOR_CALL && door_look_ring()) return;
    if (door.page != DOOR_HOME || door_busy()) return;
    if (!door.peer_count) {
        xiaotai_view_toast("请先添加室内机", 0);
        return;
    }
    door_hunt_start(0, NULL);
}

static void directory_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    /* The access machine's door numbers lead on to its directory (args.value 1). */
    const bool people = door.access && args->value == 1;
    if (!door.foreground || !door.sender || (door.page != DOOR_HOME && !(people && door.page == DOOR_DIRECTORY)) ||
        door.overlay || door.setup || door_busy()) return;
    if (door.access) {
        door_access_list(people);
        return;
    }
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
    if (door.access) {
        door_access_call_row(row);
        return;
    }
    uint32_t session = 0;
    xiaotai_err_t err;
    if (row->wechat) {
        err = dial_wechat(row->id);
        session = door.wechat_session;
    } else {
        err = xiaotai_call_answer_hold_ms() ? XIAOTAI_ERR_BUSY : dial_device(row->id, false);
        session = door.dialed_session;
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
    char which[32] = "";
    if (!active) door_hunt_caption(which, sizeof(which));
    (void)snprintf(subtitle, sizeof(subtitle), "%s · %s%s", door.called[0] ? door.called : call->peer_name[0] ? call->peer_name : "微信用户",
                   video ? "微信视频通话" : "微信语音通话", which);
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
        .text = active ? "请对着本机说话" : "对方在微信里接听后即可通话",
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
    const bool answered = call->active_since_us != 0;
    door.wechat_session = 0;
    door.call_picture = false;
    door.call_picture_generation = 0;
    if (hunt_stopped(answered, call->end == XIAOTAI_WECHAT_END_LOCAL, call->end == XIAOTAI_WECHAT_END_NETWORK, false,
                     call->end == XIAOTAI_WECHAT_END_PEER_REJECT)) {
        const bool over = answered || (call->end == XIAOTAI_WECHAT_END_LOCAL && !hunt->passed) ||
                          call->end == XIAOTAI_WECHAT_END_PREEMPTED || call->end == XIAOTAI_WECHAT_END_SYSTEM;
        if (!over && hunt_next()) {
            door_page(DOOR_CALL);   /* the next one of the household is called */
            return;
        }
        door_hunt_cancel();
    }
    door.called[0] = '\0';
    door_finish_presentation();
    xiaotai_view_toast(farewell[0] ? farewell : wechat_ending(call), 0);
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

void door_station_reset(void)
{
    door_hunt_cancel();
    xiaotai_mem_free(hunt);
    hunt = NULL;
}

xiaotai_err_t door_init_station(void)
{
    hunt = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*hunt));
    if (!hunt) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_DIRECTORY, directory_intent, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_DIAL, dial_intent, NULL);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP, wechat_hangup, NULL);
#endif
    return err;
}
