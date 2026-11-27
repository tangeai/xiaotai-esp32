// SPDX-License-Identifier: MIT
/* Looking at the door: a device call the indoor station places with its
 * microphone muted and its camera off, and which the door station answers by
 * itself because the caller is one of its chosen indoor stations
 * (xiaotai/apps/door.h, "Looking"). Both ends live here, and so does the
 * other way round: a door station calls an indoor station that shows the
 * door's home page, which looks first and answers with "接听". */
#include "door_internal.h"

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* After a call ended its link is released for a moment, in which a new call is refused as busy. */
#define REDIAL_MS 400U
#define REDIAL_TRIES 6U
/* A door station that has sent no picture this long after the call connected
 * sends none (it has no camera, or nothing this device shows). */
#define PICTURE_WAIT_MS 8000U

typedef struct {
    /* Indoor station. */
    uint32_t session;       /* the look's call; 0: none */
    uint8_t slot;           /* which door station */
    uint8_t tries;
    bool talking;
    bool rung;              /* the visitor calls */
    bool redial;            /* the look ended on purpose to look at `slot` */
    bool limited;           /* ended by XIAOTAI_APP_DOOR_LOOK_MS */
    bool voice;             /* the door station takes no video call */
    bool blank;             /* no picture came: the page says so instead of waiting */
    bool taken;             /* the visitor's call, taken to look first: nobody talks until "接听" */
    const char *reason;     /* why nothing is shown */
    uint32_t limit_timer, limit_token, redial_timer, blank_timer;
    /* Door station. */
    uint32_t seen_session;  /* the call it answered by itself */
    bool seen_talking, ringing;
    bool visitor;           /* the visitor's call goes on over it */
    bool pass_on;           /* ended here to call the other indoor stations */
    uint32_t ring_timer, ring_token;
    uint32_t viewed_session;    /* a call it dialled that the indoor station took to look first */
    uint32_t view_timer, view_token;
} look_t;
static look_t look;

static void cancel(uint32_t *timer)
{
    if (*timer) xiaotai_timer_cancel(*timer);
    *timer = 0;
}

void door_look_reset(void)
{
    cancel(&look.limit_timer);
    cancel(&look.redial_timer);
    cancel(&look.ring_timer);
    cancel(&look.blank_timer);
    cancel(&look.view_timer);
    look = (look_t){0};
}

bool door_look_is(uint32_t session) { return session && session == look.session; }
bool door_look_running(void) { return look.session || look.redial; }
bool door_look_seen_talking(void) { return look.seen_talking; }
bool door_look_seen(uint32_t session) { return session && session == look.seen_session; }
bool door_look_viewed(uint32_t session) { return session && session == look.viewed_session; }

static void label_of(uint8_t slot, char *out, size_t size)
{
    if (slot < door.peer_count) door_peer_label(door.peers[slot], out, size);
    else (void)snprintf(out, size, "%s", door_config.door_name);
}

static void tell(uint32_t session, uint8_t type, bool on)
{
    const uint8_t value = on;
    const xiaotai_err_t err = xiaotai_call_send_message(session, type, &value, 1);
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn("door", "look.message", session, err);
}

/* ------------------------------------------------------------ indoor station */

static void limit_passed(void *ctx)
{
    if (!look.session || look.limit_token != (uint32_t)(uintptr_t)ctx) return;
    look.limit_timer = 0;
    look.limited = true;
    if (xiaotai_call_hangup(look.session) != XIAOTAI_OK) look.limited = false;
}

static void limit_start(void)
{
    cancel(&look.limit_timer);
    look.limit_token = xiaotai_generation_next();
    if (xiaotai_timer_start(XIAOTAI_APP_DOOR_LOOK_MS, limit_passed, (void *)(uintptr_t)look.limit_token,
                            &look.limit_timer) != XIAOTAI_OK) look.limit_timer = 0;
}

static void blank_passed(void *ctx)
{
    if (!look.session || look.session != (uint32_t)(uintptr_t)ctx) return;
    look.blank_timer = 0;
    look.blank = true;
    door_render();
}

static const char *not_ready(xiaotai_app_door_state_t state)
{
    switch (state) {
    case XIAOTAI_APP_DOOR_STREAMING: return "本机正在被远程查看，请稍后再试";
    case XIAOTAI_APP_DOOR_NO_NETWORK: return "网络未连接";
    case XIAOTAI_APP_DOOR_NOT_READY: return "设备尚未就绪，请检查绑定";
    case XIAOTAI_APP_DOOR_NO_STATION: return "请先选择室外机";
    default: return NULL;
    }
}

static void dial(void);

static void redial_passed(void *ctx)
{
    (void)ctx;
    look.redial_timer = 0;
    if (!look.redial) return;
    if (door.page != DOOR_MONITOR || door.setup) {
        look.redial = false;
        return;
    }
    dial();
}

static void dial(void)
{
    look.reason = NULL;
    look.talking = look.rung = look.limited = false;
    xiaotai_app_door_state_t state = door_home_state();
    xiaotai_contact_t contact;
    if (look.slot >= door.peer_count) look.slot = 0;
    const char *reason = not_ready(state);
    if (!reason && !door_peer_contact(door.peers[look.slot], &contact)) reason = "室外机已移除，请重新选择";
    if (reason) {
        look.redial = false;
        look.reason = reason;
        door_page(DOOR_MONITOR);
        return;
    }
    /* A look given up before it connected: its answer may still be on the way. */
    const uint32_t hold = xiaotai_call_answer_hold_ms();
    if (hold && xiaotai_timer_start(hold, redial_passed, NULL, &look.redial_timer) == XIAOTAI_OK) {
        look.redial = true;
        door_page(DOOR_MONITOR);
        return;
    }
    uint32_t session = 0;
    const xiaotai_err_t err = door_dial(door.peers[look.slot], look.voice, &session);
    if (err == XIAOTAI_ERR_BUSY && look.redial && look.tries < REDIAL_TRIES) {
        ++look.tries;
        if (xiaotai_timer_start(REDIAL_MS, redial_passed, NULL, &look.redial_timer) == XIAOTAI_OK) {
            door_page(DOOR_MONITOR);
            return;
        }
    }
    look.redial = false;
    look.tries = 0;
    if (err == XIAOTAI_OK) {
        look.session = session;
        ++door.looks;
        /* Looking is silent: what is said indoors is not heard at the door. */
        (void)xiaotai_call_set_muted(session, true);
        xiaotai_eventlog_note("door", "look", session, look.slot);
    } else {
        look.reason = err == XIAOTAI_ERR_BUSY ? "设备正忙，请稍后再试" :
                      err == XIAOTAI_ERR_NOT_FOUND ? "室外机已移除，请重新选择" :
                      err == XIAOTAI_ERR_PLATFORM_OFFLINE ? "网络未连接" : "查看没有发出，请重试";
    }
    door_page(DOOR_MONITOR);
}

void door_monitor_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground || door.sender || door.overlay || door.setup || door_look_running()) return;
    if (door.page != DOOR_HOME && door.page != DOOR_MONITOR) return;
    if (door.session || (door.page == DOOR_HOME && !door_mode_known())) return;
    if (door.page == DOOR_HOME) look.slot = 0;
    look.voice = false;
    dial();
}

static bool active(void)
{
    const xiaotai_call_status_t *shown = door_shown_call();
    return look.session && shown->session == look.session && shown->state == XIAOTAI_CALL_ACTIVE;
}

static bool ours(const xiaotai_args_t *args)
{
    return door.foreground && door.page == DOOR_MONITOR && !door.overlay && !door.setup &&
           (!args->value || (uint32_t)args->value == look.session);
}

static void talk(bool on)
{
    look.rung = false;
    look.talking = on;
    (void)xiaotai_call_set_muted(look.session, !on);
    tell(look.session, XIAOTAI_APP_DOOR_MESSAGE_LOOK, on);
    if (on) cancel(&look.limit_timer);
    else limit_start();
    xiaotai_call_status_t now;
    xiaotai_call_status(&now);
    /* The lasting setting lies under the call's mute. */
    if (on && now.session == look.session && now.muted) xiaotai_view_toast("麦克风已在设置里静音", 0);
    xiaotai_ui_activity();
    door_call_ring_update();
    door_render();
}

static void talk_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!ours(args) || !active()) return;
    if (look.rung) door_log_add(DOOR_LOG_ANSWERED, door_shown_call()->peer_id);
    talk(!look.talking);
}

static void ignore_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!ours(args) || !look.rung || (!look.taken && !active())) return;
    door_log_add(DOOR_LOG_REJECTED, door_shown_call()->peer_id);
    if (!look.taken) {
        talk(false);
        return;
    }
    /* Not answered here: the door station calls the next one, or tells the visitor. */
    look.rung = false;
    if (xiaotai_call_hangup(look.session) != XIAOTAI_OK) xiaotai_view_toast("操作未受理，请重试", 0);
    door_call_ring_update();
    door_render();
}

static void next_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!ours(args) || look.redial || door.peer_count < 2) return;
    look.slot = (uint8_t)((look.slot + 1U) % door.peer_count);
    look.redial = true;
    look.voice = false;
    look.tries = 0;
    if (!look.session) dial();
    else if (xiaotai_call_hangup(look.session) != XIAOTAI_OK) look.redial = false;
}

static void end_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!ours(args)) return;
    look.redial = false;
    cancel(&look.redial_timer);
    if (look.taken && look.rung) {
        door_log_add(DOOR_LOG_REJECTED, door_shown_call()->peer_id);
        look.rung = false;
    }
    if (!look.session) door_page(DOOR_HOME);
    else if (xiaotai_call_hangup(look.session) != XIAOTAI_OK) xiaotai_view_toast("操作未受理，请重试", 0);
}

void door_look_connected(uint32_t session)
{
    if (door_look_is(session)) {
        (void)xiaotai_call_set_muted(session, !look.talking);
        tell(session, XIAOTAI_APP_DOOR_MESSAGE_LOOK, look.talking);
        /* Nobody is cut off while the visitor waits for an answer. */
        if (!look.talking && !look.rung) limit_start();
        cancel(&look.blank_timer);
        look.blank = false;
        if (xiaotai_timer_start(PICTURE_WAIT_MS, blank_passed, (void *)(uintptr_t)session, &look.blank_timer) != XIAOTAI_OK)
            look.blank_timer = 0;
    } else if (look.visitor) {
        look.visitor = false;
        (void)door_look_ring();
    }
}

void door_look_talks(uint32_t session) { tell(session, XIAOTAI_APP_DOOR_MESSAGE_LOOK, true); }

/* A door station calls while the door's home page is on the screen: like the
 * indoor station of an intercom, this one shows the door at once. The call is
 * taken with the microphone muted (the camera stays off indoors, as for every
 * call here) and becomes a look the visitor rings: "接听" talks, "忽略" hangs
 * up, so the door station calls the next one. The door station learns it from
 * the look's own message (LOOK 0) and keeps ringing. Elsewhere, or without a
 * picture to show, the call rings as any call does. */
bool door_look_take(const xiaotai_call_status_t *incoming)
{
    /* Brought forward to present this very call: another page was on the screen. */
    const bool shown_before = door.foreground && xiaotai_app_entry() != XIAOTAI_APP_ENTRY_PRESENT;
    if (door.sender || incoming->outgoing || incoming->state != XIAOTAI_CALL_INCOMING || door_look_running() ||
        !shown_before || door.page != DOOR_HOME || door.overlay || door.setup ||
        !(incoming->flags & XIAOTAI_CALL_VIDEO) || !door_shows_video()) return false;
    const int slot = door_peer_slot(incoming->peer_id);
    if (slot < 0) return false;
    const xiaotai_err_t err = xiaotai_call_accept(incoming->session);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_warn("door", "look.take", incoming->session, err);
        return false;
    }
    (void)xiaotai_call_set_muted(incoming->session, true);
    cancel(&look.limit_timer);
    cancel(&look.blank_timer);
    cancel(&look.redial_timer);
    look.session = incoming->session;
    look.slot = (uint8_t)slot;
    look.rung = look.taken = true;
    look.talking = look.limited = look.voice = look.blank = look.redial = false;
    look.tries = 0;
    look.reason = NULL;
    xiaotai_eventlog_note("door", "look.take", incoming->session, (uint32_t)slot);
    return true;
}
void door_look_visitor(void) { look.visitor = true; }

/* A look that ended stays on its page with the reason; one the user ended goes home. */
static void look_ended(const xiaotai_call_status_t *ended)
{
    const bool taken = look.taken, talked = look.talking;
    look.session = 0;
    look.blank = look.taken = false;
    cancel(&look.limit_timer);
    cancel(&look.blank_timer);
    if (look.rung) door_log_add(DOOR_LOG_MISSED, ended->peer_id);
    look.rung = look.talking = false;
    if (door.setup) {   /* setup guidance owns the screen */
        look.redial = false;
        return;
    }
    if (taken) {        /* the visitor's call: over, whoever ended it */
        look.redial = false;
        door_page(DOOR_HOME);
        if (ended->end != XIAOTAI_CALL_END_LOCAL) xiaotai_view_toast(talked ? "通话已结束" : "门口的呼叫已结束", 0);
        return;
    }
    if (!look.redial && !look.voice && door_refused_video(ended)) {
        look.voice = look.redial = true;
        look.tries = 0;
    }
    if (look.redial) {
        look.reason = NULL;
        if (xiaotai_timer_start(REDIAL_MS, redial_passed, NULL, &look.redial_timer) != XIAOTAI_OK) look.redial = false;
        door_page(DOOR_MONITOR);
        return;
    }
    switch (ended->end) {
    case XIAOTAI_CALL_END_LOCAL:
        if (!look.limited) {
            door_page(DOOR_HOME);
            return;
        }
        look.reason = "已查看两分钟，自动结束";
        break;
    case XIAOTAI_CALL_END_PEER_HANGUP:
    case XIAOTAI_CALL_END_PEER_CANCEL: look.reason = "室外机结束了这次查看"; break;
    case XIAOTAI_CALL_END_BUSY:
    case XIAOTAI_CALL_END_PEER_REJECT: look.reason = "室外机正忙，请稍后再试"; break;
    case XIAOTAI_CALL_END_NO_ANSWER: look.reason = "室外机没有应答。请在室外机上把本机设为室内机"; break;
    case XIAOTAI_CALL_END_NETWORK: look.reason = "网络中断，请重新查看"; break;
    case XIAOTAI_CALL_END_FAILED: look.reason = "室外机暂时连不上，请稍后再试"; break;
    case XIAOTAI_CALL_END_PREEMPTED:
    case XIAOTAI_CALL_END_SYSTEM: look.reason = "查看被打断了，请重新查看"; break;
    default: look.reason = "查看已结束"; break;
    }
    door_page(DOOR_MONITOR);
}

void door_look_render(void)
{
    const xiaotai_call_status_t *shown = door_shown_call();
    const bool running = active();
    const bool waiting = !running && door_look_running();
    const int32_t session = (int32_t)look.session;
    char name[XIAOTAI_SETTING_TEXT_MAX];
    label_of(look.slot, name, sizeof(name));
    xiaotai_view_action_t actions[6];
    size_t count = 0;
    if (running) {
        actions[count++] = (xiaotai_view_action_t){
            .label = look.rung ? "接听" : look.talking ? "停止对讲" : "对讲", .intent = XIAOTAI_INTENT_APP_DOOR_LOOK_TALK,
            .value = session, .icon = look.rung ? XIAOTAI_ICON_ACCEPT : XIAOTAI_ICON_MIC,
            .role = XIAOTAI_ROLE_PRIMARY, .style = look.rung ? XIAOTAI_STYLE_POSITIVE : XIAOTAI_STYLE_NORMAL,
            .state = look.talking ? XIAOTAI_STATE_CHECKED : 0};
        if (look.rung) {
            actions[count++] = (xiaotai_view_action_t){.label = "忽略", .intent = XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE,
                .value = session, .icon = XIAOTAI_ICON_STOP};
        } else {
            actions[count++] = (xiaotai_view_action_t){.label = "音量", .intent = XIAOTAI_INTENT_APP_DOOR_VOLUME,
                .value = session, .icon = XIAOTAI_ICON_VOLUME};
        }
        if (!door.bell_indoor)
            actions[count++] = (xiaotai_view_action_t){.label = "开门", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN,
                .value = session, .icon = XIAOTAI_ICON_KEY};
    } else if (!waiting) {
        actions[count++] = (xiaotai_view_action_t){.label = "返回", .role = XIAOTAI_ROLE_BACK,
            .intent = XIAOTAI_INTENT_APP_DOOR_BACK, .icon = XIAOTAI_ICON_BACK};
        if (door.peer_count) {
            actions[count++] = (xiaotai_view_action_t){.label = "重新查看", .role = XIAOTAI_ROLE_PRIMARY,
                .intent = XIAOTAI_INTENT_APP_DOOR_MONITOR, .icon = XIAOTAI_ICON_REFRESH};
        }
    }
    if (door.peer_count > 1 && !door.bell_indoor && !look.rung && !waiting) {
        actions[count++] = (xiaotai_view_action_t){.label = "切换", .intent = XIAOTAI_INTENT_APP_DOOR_LOOK_NEXT,
            .value = session, .icon = XIAOTAI_ICON_NEXT};
    }
    if (running || waiting) {
        actions[count++] = (xiaotai_view_action_t){.label = running ? "结束" : "取消",
            .intent = XIAOTAI_INTENT_APP_DOOR_LOOK_END, .value = session, .icon = XIAOTAI_ICON_HANGUP,
            .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK};
    }
    const bool picture = running && door.call_picture && shown->link == door.call_picture_generation;
    const bool video = (shown->flags & XIAOTAI_CALL_VIDEO) != 0;
    const int64_t elapsed = running ? xiaotai_time_us() - shown->active_since_us : 0;
    const xiaotai_app_door_home_view_t home = {.state = running ? XIAOTAI_APP_DOOR_STATION_ONLINE : XIAOTAI_APP_DOOR_BUSY};
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_MONITOR, .custom = XIAOTAI_APP_DOOR_TEMPLATE_MONITOR,
        .title = look.rung ? "门口有人按铃" : "查看门口", .subtitle = name,
        .status = running ? NULL : waiting ? "正在接通" : "未连接",
        .text = running ? (picture ? NULL : video && door_shows_video() && !look.blank ? "正在等待门口画面" : "没有画面，可以听和对讲") :
                waiting ? "正在接通室外机" : look.reason,
        .timer_mode = running ? XIAOTAI_TIMER_ELAPSED : XIAOTAI_TIMER_NONE,
        .timer_s = elapsed > 0 ? (int32_t)(elapsed / 1000000LL) : 0,
        .custom_data = &home, .custom_size = sizeof(home),
        .flags = picture ? XIAOTAI_VIEW_VIDEO : 0,
        .actions = actions, .action_count = count,
    };
    if (!door_own_template()) {
        /* The display's own pages: a call while it runs, a notice with the reason otherwise. */
        view.custom = NULL;
        view.kind = running || waiting ? XIAOTAI_VIEW_CALL : XIAOTAI_VIEW_NOTICE;
        if (running && !view.text) view.text = look.rung ? "接听后可与访客说话" : look.talking ? "正在对讲" : "正在查看";
    }
    door_register_view(&view);
}

/* -------------------------------------------------------------- door station */

/* The door station's own announcement of what is going on. */
bool door_look_answer(const xiaotai_call_status_t *incoming)
{
    if (!door.sender || incoming->outgoing || incoming->state != XIAOTAI_CALL_INCOMING ||
        door_look_seen(incoming->session)) return false;
    xiaotai_contact_t contact;
    if (!door_peer_contact(incoming->peer_id, &contact) ||
        (door_peer_slot(incoming->peer_id) < 0 && !door_call_looks_at_home(incoming))) return false;
    const xiaotai_err_t err = xiaotai_call_accept(incoming->session);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_warn("door", "look.accept", incoming->session, err);
        return false;
    }
    look.seen_session = incoming->session;
    look.seen_talking = look.ringing = look.visitor = look.pass_on = false;
    ++door.looks;
    xiaotai_eventlog_note("door", "look.answered", incoming->session, 0);
    return true;
}

bool door_look_ringing(void) { return look.ringing || look.rung || look.viewed_session; }

static void view_passed(void *ctx)
{
    if (!look.viewed_session || look.view_token != (uint32_t)(uintptr_t)ctx) return;
    look.view_timer = 0;
    door_hunt_give_way(look.viewed_session);
}

/* The indoor station it called took the call to look first (LOOK 0), or
 * answers now (LOOK 1). Until it answers the visitor waits, as for a ring. */
static void viewed(uint32_t session, bool talks)
{
    if (!talks && !look.viewed_session) {
        look.viewed_session = session;
        look.view_token = xiaotai_generation_next();
        if (xiaotai_timer_start(XIAOTAI_APP_DOOR_RING_MS, view_passed, (void *)(uintptr_t)look.view_token,
                                &look.view_timer) != XIAOTAI_OK) look.view_timer = 0;
        xiaotai_eventlog_note("door", "viewed", session, 0);
    } else if (talks && look.viewed_session == session) {
        look.viewed_session = 0;
        cancel(&look.view_timer);
        xiaotai_eventlog_note("door", "viewed.answer", session, 0);
    }
    xiaotai_ui_activity();
}
bool door_look_rung(void) { return look.rung; }

static void ring_stop(void)
{
    cancel(&look.ring_timer);
    look.ringing = false;
}

static void ring_passed(void *ctx)
{
    if (!look.ringing || look.ring_token != (uint32_t)(uintptr_t)ctx) return;
    look.ring_timer = 0;
    /* Whoever looks does not answer: the others are called, once this call is over. */
    for (uint8_t i = 0; i < door.peer_count; ++i) {
        xiaotai_contact_t other;
        if (!strcmp(door.peers[i], door_shown_call()->peer_id) || !door_peer_contact(door.peers[i], &other) ||
            (other.online_known && !other.online)) continue;
        look.pass_on = xiaotai_call_hangup(look.seen_session) == XIAOTAI_OK;
        if (look.pass_on) return;
        break;
    }
    ring_stop();
    tell(look.seen_session, XIAOTAI_APP_DOOR_MESSAGE_RING, false);
    door_call_ring_update();
    door_render();
    if (door.foreground) xiaotai_view_toast("暂时无人应答", 0);
}

/* The visitor calls while an indoor station looks: the call is already there. */
bool door_look_ring(void)
{
    const xiaotai_call_status_t *shown = door_shown_call();
    if (!look.seen_session || shown->session != look.seen_session || shown->state != XIAOTAI_CALL_ACTIVE) return false;
    if (look.seen_talking || look.ringing || look.pass_on) return true;
    const uint8_t on = 1;
    const xiaotai_err_t err = xiaotai_call_send_message(look.seen_session, XIAOTAI_APP_DOOR_MESSAGE_RING, &on, 1);
    if (err != XIAOTAI_OK) {
        xiaotai_view_toast("呼叫没有发出，请重试", 0);
        return true;
    }
    look.ringing = true;
    look.ring_token = xiaotai_generation_next();
    if (xiaotai_timer_start(XIAOTAI_APP_DOOR_RING_MS, ring_passed, (void *)(uintptr_t)look.ring_token,
                            &look.ring_timer) != XIAOTAI_OK) look.ring_timer = 0;
    xiaotai_eventlog_note("door", "look.ring", look.seen_session, 0);
    xiaotai_ui_activity();
    door_call_ring_update();
    door_render();
    return true;
}

void door_look_message(const xiaotai_call_message_t *m)
{
    const bool on = m->size == 1 && m->data[0];
    if (m->type == XIAOTAI_APP_DOOR_MESSAGE_LOOK && door_look_seen(m->session)) {
        door_peer_learn_indoor(door_shown_call()->peer_id);
        const bool declined = look.ringing && !on;
        ring_stop();
        look.seen_talking = on;
        xiaotai_ui_activity();
        if (declined && door.foreground) xiaotai_view_toast("对方现在不方便接听", 0);
    } else if (m->type == XIAOTAI_APP_DOOR_MESSAGE_LOOK && door.sender && m->session == door.dialed_session) {
        viewed(m->session, on);
    } else if (m->type == XIAOTAI_APP_DOOR_MESSAGE_RING && door_look_is(m->session) && !look.talking && look.rung != on) {
        look.rung = on;
        /* Nobody is cut off while the visitor calls; a box would cover the answer. */
        if (on) cancel(&look.limit_timer);
        else limit_start();
        if (on && door.overlay == DOOR_DIALOG_VOLUME) door_overlay_close();
        if (!on) door_log_add(DOOR_LOG_MISSED, door_shown_call()->peer_id);
        xiaotai_ui_activity();
    } else return;
    door_call_ring_update();
    door_render();
}

void door_look_station_render(void)
{
    const xiaotai_call_status_t *shown = door_shown_call();
    const bool running = shown->state == XIAOTAI_CALL_ACTIVE;
    const int32_t session = (int32_t)shown->session;
    char name[XIAOTAI_SETTING_TEXT_MAX];
    door_device_label(shown->peer_id, shown->peer_name, name, sizeof(name));
    xiaotai_view_action_t actions[2];
    size_t count = 0;
    if (running && look.seen_talking) {
        actions[count++] = (xiaotai_view_action_t){.label = "音量", .intent = XIAOTAI_INTENT_APP_DOOR_VOLUME,
            .value = session, .icon = XIAOTAI_ICON_VOLUME};
        actions[count++] = (xiaotai_view_action_t){.label = "挂断", .intent = "door.call.hangup", .value = session,
            .icon = XIAOTAI_ICON_HANGUP, .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK};
    } else if (running) {
        actions[count++] = (xiaotai_view_action_t){.label = look.ringing ? "呼叫中" : "呼叫", .icon = XIAOTAI_ICON_PHONE,
            .intent = XIAOTAI_INTENT_APP_DOOR_RING, .role = XIAOTAI_ROLE_PRIMARY, .style = XIAOTAI_STYLE_POSITIVE,
            .state = look.ringing ? XIAOTAI_STATE_BUSY : 0};
    }
    const xiaotai_app_door_looked_view_t custom = {
        .state = look.seen_talking ? XIAOTAI_APP_DOOR_LOOKED_TALKING :
                 look.ringing ? XIAOTAI_APP_DOOR_LOOKED_RINGING : XIAOTAI_APP_DOOR_LOOKED_ONLY,
        .camera = door_has_camera(),
    };
    const int64_t elapsed = running ? xiaotai_time_us() - shown->active_since_us : 0;
    xiaotai_view_t view = {
        .id = XIAOTAI_APP_DOOR_PAGE_LOOKED, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_LOOKED,
        .custom_data = &custom, .custom_size = sizeof(custom),
        .title = look.seen_talking ? "正在通话" : look.ringing ? "正在呼叫" : "正在被查看", .subtitle = name,
        .text = !running ? "正在接通" : look.seen_talking ? "请对着本机说话" : look.ringing ? "请稍候，对方接听后即可通话" :
                custom.camera ? "门口的画面和声音正在传到室内" : "正在听门口的声音",
        .timer_mode = running ? XIAOTAI_TIMER_ELAPSED : XIAOTAI_TIMER_NONE,
        .timer_s = elapsed > 0 ? (int32_t)(elapsed / 1000000LL) : 0,
        .actions = actions, .action_count = count,
    };
    if (!door_station_template()) {
        view.kind = XIAOTAI_VIEW_CALL;
        view.custom = NULL;
    }
    door_register_view(&view);
}

/* ------------------------------------------------------------------- shared */

/* The call ended. True: the look's page was shown, nothing else is to be presented. */
bool door_look_ended(uint32_t session, const xiaotai_call_status_t *ended)
{
    if (door_look_viewed(session)) {     /* not answered: the hunt goes on as after any call */
        look.viewed_session = 0;
        cancel(&look.view_timer);
        return false;
    }
    if (door_look_seen(session)) {
        const bool pass_on = look.pass_on && look.ringing;
        const bool visitor_waits = pass_on || look.visitor || (look.ringing && ended->end == XIAOTAI_CALL_END_NETWORK);
        const bool declined = look.ringing && !visitor_waits, talked = look.seen_talking;
        ring_stop();
        look.visitor = look.pass_on = false;
        look.seen_session = 0;
        look.seen_talking = false;
        if (door.setup) return true;
        /* Said while the door is still in front: brought forward over another application for the
         * look, it gives the screen back below, and only the application in front draws. */
        if (!visitor_waits) xiaotai_view_toast(declined ? "对方现在不方便接听" : talked ? "通话已结束" : "查看结束", 0);
        door_finish_presentation();
        /* The visitor still wants somebody. */
        if (visitor_waits) door_hunt_start(REDIAL_MS, pass_on ? ended->peer_id : NULL);
        return true;
    }
    if (!door_look_is(session)) return false;
    look_ended(ended);
    return true;
}

xiaotai_err_t door_init_look(void)
{
    static const struct { const char *intent; xiaotai_intent_fn_t handler; } intents[] = {
        {XIAOTAI_INTENT_APP_DOOR_MONITOR, door_monitor_intent},
        {XIAOTAI_INTENT_APP_DOOR_LOOK_TALK, talk_intent}, {XIAOTAI_INTENT_APP_DOOR_LOOK_NEXT, next_intent},
        {XIAOTAI_INTENT_APP_DOOR_LOOK_END, end_intent}, {XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE, ignore_intent},
    };
    for (size_t i = 0; i < sizeof(intents) / sizeof(*intents); ++i) {
        const xiaotai_err_t err = xiaotai_intent_handle(intents[i].intent, intents[i].handler, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}
