// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"
#include "xiaotai/sounds.h"
#include "xiaotai/ui.h"

#include <stdio.h>
#include <string.h>

#define REQUEST_MS 1500U

typedef enum {NONE, ACCEPT, REJECT, HANGUP} operation_t;
typedef struct {
    xiaotai_call_status_t shown;
    operation_t pending;
    xiaotai_call_state_t phase;
    uint32_t timer, token;
    int32_t preview;
    bool volume_dirty;
    uint32_t rest_session;      /* the incoming call at_rest was decided for */
    bool at_rest;
    uint32_t passed_session;    /* another application's call: not taken over while it runs */
    bool other_activity;        /* an activity that is no call runs */
    bool took_over;             /* the running call ended one: it did not find the device at rest */
} call_ui_t;
static call_ui_t call;

const xiaotai_call_status_t *door_shown_call(void) { return &call.shown; }

/* What the device was doing when a call took it away. A conversation with the
 * assistant is drawn on the same home page as the resting face and is over by
 * the time the call is offered (the call's activity ended it), so the page
 * cannot tell; the activities' announcements can: the call's comes straight
 * after the other one's, with no "none" between them. */
void door_call_activity(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const char *name = event->args.text;
    if (!memchr(name, '\0', sizeof(event->args.text)) || !name[0]) {
        call.other_activity = call.took_over = false;
    } else if (!strcmp(name, XIAOTAI_CALL_ACTIVITY)) {
        call.took_over = call.other_activity;
        call.other_activity = false;
    } else {
        call.other_activity = true;
        call.took_over = false;
    }
}

/* At rest: its own home page with nothing over it, or, beside other
 * applications, the home page of the one in front (a companion's face); and
 * the call did not end something else to get there. */
static bool station_at_rest(void)
{
    if (call.took_over) return false;
    if (door.foreground) return !door.overlay && door.page == DOOR_HOME;
    xiaotai_ui_info_t display = {0};
    return xiaotai_ui_inspect(0, &display) == XIAOTAI_OK && display.ready && !display.overlay[0] &&
           display.page_kind == XIAOTAI_VIEW_HOME;
}

/* A door station at rest is there to be looked at: a device among the
 * contacts that calls it now looks at the door, chosen or not. Busy with
 * something else only the indoor stations it chose do. Decided when the call
 * arrives: presenting it brings the door page forward. */
bool door_call_looks_at_home(const xiaotai_call_status_t *snapshot)
{
    xiaotai_contact_t contact;
    if (!door.sender || snapshot->outgoing || snapshot->state != XIAOTAI_CALL_INCOMING || door.setup ||
        !snapshot->session || !door_peer_contact(snapshot->peer_id, &contact)) return false;
    if (call.rest_session != snapshot->session) {
        call.rest_session = snapshot->session;
        call.at_rest = station_at_rest();
    }
    return call.at_rest;
}

void door_call_pass(uint32_t session) { call.passed_session = session; }

bool door_call_matches(const xiaotai_call_status_t *snapshot)
{
    if (snapshot->session && snapshot->session == door.classification_session) return door.classified_door;
    if (snapshot->session && snapshot->session == call.passed_session) return false;
    /* The door station presents every device call it dialled itself (front desk or directory). */
    if (snapshot->session && door.sender && snapshot->session == door.dialed_session) return true;
    if (!snapshot->session) return false;
    if (door_peer_slot(snapshot->peer_id) >= 0 || door_call_looks_at_home(snapshot)) return true;
    /* Any other device among the contacts that calls a door station rings on
     * the door's own page: a door station shows no face for a call. */
    xiaotai_contact_t contact;
    return door.sender && !snapshot->outgoing && door_peer_contact(snapshot->peer_id, &contact);
}

static void cancel_pending(void)
{
    if (call.timer) xiaotai_timer_cancel(call.timer);
    call.timer = call.token = 0;
    call.pending = NONE;
}

void door_call_reset(void) { call = (call_ui_t){0}; }

void door_call_ring_update(void)
{
    const bool present = door.foreground && !door.setup &&
                         (door.page == DOOR_CALL || door.page == DOOR_OTHER_CALL) && door.session;
    const bool shown = door.foreground && !door.setup && door_on_call_page() && door.session;
    /* A station the device answers by itself does not ring; a look places its call silently. */
    (void)xiaotai_feedback_set(XIAOTAI_APP_DOOR_STATE_INCOMING,
                              (present && call.shown.state == XIAOTAI_CALL_INCOMING && !door_look_seen(door.session)) ||
                              (shown && door_look_rung()));
    (void)xiaotai_feedback_set(XIAOTAI_APP_DOOR_STATE_OUTGOING,
                              (present && call.shown.outgoing &&
                               (call.shown.state == XIAOTAI_CALL_OUTGOING || call.shown.state == XIAOTAI_CALL_CONNECTING)) ||
                              (shown && door_look_ringing() && !door_look_rung()));
}

static void timed_out(void *context)
{
    if (call.token != (uint32_t)(uintptr_t)context) return;
    call.timer = 0;
    cancel_pending();
    door_render();
    if (door.foreground && door.page == DOOR_CALL) xiaotai_view_toast("操作未受理，请重试", 0);
}

static void pending(operation_t operation)
{
    call.pending = operation;
    call.phase = call.shown.state;
    call.token = xiaotai_generation_next();
    if (xiaotai_timer_start(REQUEST_MS, timed_out, (void *)(uintptr_t)call.token, &call.timer) != XIAOTAI_OK) {
        cancel_pending();
        xiaotai_view_toast("未能显示操作进度", 0);
    }
    door_render();
}

static bool current_active(void)
{
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    return door.foreground && door_on_call_page() && door.session && current.session == door.session &&
           current.state == XIAOTAI_CALL_ACTIVE;
}

static bool check_mutable(void)
{
    if (!xiaotai_quiesced()) return true;
    xiaotai_view_toast("维护中，设置暂不可修改", 0);
    return false;
}

void door_on_call(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!strcmp(event->name, XIAOTAI_EVENT_CALL_ENDED)) {
        call.rest_session = call.passed_session = 0;
        if (!door.session) return;
        xiaotai_call_status_t current;
        xiaotai_call_status(&current);
        if (current.session == door.session && current.state != XIAOTAI_CALL_IDLE) return;
        const bool other = door.page == DOOR_OTHER_CALL;
        const uint32_t session = door.session;
        /* A call the indoor station only looked at was not answered. */
        const bool answered = call.shown.active_since_us != 0 && !door_look_viewed(session);
        const bool dialed = door.dialed_session == session;
        if (call.shown.state == XIAOTAI_CALL_INCOMING && door.classified_door && !door_look_seen(session)) {
            door_log_add(current.end == XIAOTAI_CALL_END_LOCAL ? DOOR_LOG_REJECTED : DOOR_LOG_MISSED, call.shown.peer_id);
        }
        door.session = door.classification_session = door.dialed_session = 0;
        door.classified_door = false;
        door_format_clear(call.shown.link, &door.call_format_noted, DOOR_CALL_FORMAT_TOAST);
        door.call_picture = false;
        door.call_picture_generation = 0;
        door.call_format_noted = 0;
        cancel_pending();
        door_open_call_ended(session);
        xiaotai_call_status_t ended = call.shown;
        ended.end = current.end;
        ended.error = current.error;
        if (door_look_ended(session, &ended)) {
            /* shown there */
        } else if (dialed && door_hunt_ended(&ended, answered)) {
            door_render();   /* the next indoor station is called */
        } else if (!door.setup) {
            if (other && door.foreground && current.state == XIAOTAI_CALL_IDLE) door_fallback_device_ended(&current);
            else {
                door_finish_presentation();
                /* The door station tells the visitor how the call went; the indoor station needs no word.
                 * A call that came in has its own words: nobody at the door placed it. */
                if (door.sender && !other && door.foreground && !ended.outgoing) {
                    xiaotai_view_toast(answered ? "通话已结束" : current.end == XIAOTAI_CALL_END_LOCAL ? "已拒接" :
                                       current.end == XIAOTAI_CALL_END_PEER_CANCEL ? "对方已取消呼叫" : "来电未接听", 0);
                } else if (door.sender && !other && door.foreground) {
                    xiaotai_view_toast(dialed && door_hunt_farewell()[0] ? door_hunt_farewell() : answered ? "通话已结束" :
                                       current.end == XIAOTAI_CALL_END_LOCAL ? "已取消呼叫" :
                                       current.end == XIAOTAI_CALL_END_BUSY ? "对方正忙，请稍后再试" :
                                       current.end == XIAOTAI_CALL_END_PEER_REJECT ? "对方现在不方便接听" :
                                       current.end == XIAOTAI_CALL_END_NETWORK ? "网络中断，请重试" :
                                       current.end == XIAOTAI_CALL_END_FAILED ? "呼叫没有接通，请重试" : "对方暂未接听", 0);
                }
            }
        }
        if (!door_hunt_active()) door.called[0] = '\0';
        door_call_ring_update();
        return;
    }
    const xiaotai_call_status_t *snapshot = event->size == sizeof(*snapshot) ? event->data : NULL;
    if (!snapshot || !snapshot->session) return;
    const bool other = door.session != snapshot->session;
    const bool selected = door_call_matches(snapshot);
    bool visitor = false;
    if (other || call.shown.link != snapshot->link) {
        door_format_clear(call.shown.link, &door.call_format_noted, DOOR_CALL_FORMAT_TOAST);
        if (door.call_picture_generation != snapshot->link) {
            door.call_picture = false;
            door.call_picture_generation = 0;
        }
        if (door.call_format_noted != snapshot->link) door.call_format_noted = 0;
    }
    if (door.overlay) {
        xiaotai_ui_activity();
        if (other || snapshot->state != XIAOTAI_CALL_ACTIVE) door_overlay_close();
    }
    if (other) {
        cancel_pending();
        if (selected) ++door.door_calls;
        if (!snapshot->outgoing && door_hunt_active()) {     /* somebody calls between two calls of the hunt */
            door_hunt_cancel();
            visitor = true;
        }
    } else if (call.pending && snapshot->state != call.phase) cancel_pending();
    door.session = door.classification_session = snapshot->session;
    door.classified_door = selected;
    const bool connected = snapshot->state == XIAOTAI_CALL_ACTIVE &&
                           (other || call.shown.state != XIAOTAI_CALL_ACTIVE || call.shown.session != snapshot->session);
    call.shown = *snapshot;
    if (selected && door_look_answer(snapshot) && visitor) door_look_visitor();
    if (other && selected) (void)door_look_take(snapshot);
    if (connected && selected) {
        /* The door station says who calls: an indoor station that did not choose it learns it. */
        if (door.sender && snapshot->outgoing) {
            const uint8_t mode = (uint8_t)door_mode();
            const xiaotai_err_t err = xiaotai_call_send_message(snapshot->session, XIAOTAI_APP_DOOR_MESSAGE_HELLO,
                                                                &mode, 1);
            if (err != XIAOTAI_OK) xiaotai_eventlog_warn("door", "hello", snapshot->session, err);
        }
        if (door_look_is(snapshot->session) || door_look_seen(snapshot->session)) door_look_connected(snapshot->session);
        else if (!door.sender && snapshot->outgoing) door_look_talks(snapshot->session);
        else if (!door.sender) door_log_add(DOOR_LOG_ANSWERED, snapshot->peer_id);
    }
    if (!door.setup)
        door_page(!selected ? DOOR_OTHER_CALL : door_look_is(snapshot->session) ? DOOR_MONITOR : DOOR_CALL);
    door_call_ring_update();
}

static void control(const xiaotai_args_t *args, void *context)
{
    if (!door.foreground || door.page != DOOR_CALL || door.overlay) return;
    operation_t operation = (operation_t)(uintptr_t)context;
    if (!operation && memchr(args->text, '\0', sizeof(args->text))) {
        if (!strcmp(args->text, "accept")) operation = ACCEPT;
        else if (!strcmp(args->text, "reject")) operation = REJECT;
        else if (!strcmp(args->text, "hangup")) operation = HANGUP;
    }
    const uint32_t session = (uint32_t)args->value;
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    if (operation == HANGUP && door_hunt_active() && !door.session) {
        door_hunt_cancel();   /* between two calls of the hunt */
        door_finish_presentation();
        xiaotai_view_toast("已取消呼叫", 0);
        return;
    }
    if (!session || session != door.session || session != current.session || !operation) {
        xiaotai_view_toast("操作未受理，请重试", 0);
        return;
    }
    if (call.pending) return;
    const xiaotai_err_t error = operation == ACCEPT ? xiaotai_call_accept(session) :
        operation == REJECT ? xiaotai_call_reject(session) : xiaotai_call_hangup(session);
    if (error == XIAOTAI_OK) pending(operation);
    else xiaotai_view_toast("操作未受理，请重试", 0);
}

static void toggle_mute(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!current_active() || door.overlay || call.pending || !check_mutable()) return;
    if (args->value && (uint32_t)args->value != door.session) return;
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    const xiaotai_err_t error = xiaotai_call_set_saved_mute(current.session, !current.muted);
    if (error == XIAOTAI_OK) {
        xiaotai_call_status(&call.shown);
        door_render();
    } else {
        xiaotai_view_toast(error == XIAOTAI_ERR_BUSY ? "维护中，设置暂不可修改" : "操作未受理，请重试", 0);
    }
}

static int32_t saved_volume(void)
{
    int32_t volume = 0;
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &volume);
    return volume;
}

static void volume_render(void)
{
    const xiaotai_view_field_t slider = {
        .label = "通话音量", .intent = XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, .kind = XIAOTAI_FIELD_SLIDER,
        .min = 0, .max = 100, .step = xiaotai_settings_volume_step_percent(), .text = "%", .flags = XIAOTAI_FIELD_LIVE,
        .value = call.volume_dirty ? call.preview : saved_volume(),
    };
    const xiaotai_view_action_t back = {
        .label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_DOOR_BACK,
    };
    const xiaotai_view_t view = {
        .layer = XIAOTAI_LAYER_OVERLAY, .id = XIAOTAI_APP_DOOR_OVERLAY_VOLUME,
        /* The theme draws a slider in a form; a custom view it does not know loses it. */
        .kind = door_own_template() ? XIAOTAI_VIEW_CUSTOM : XIAOTAI_VIEW_FORM,
        .custom = door_own_template() ? XIAOTAI_APP_DOOR_TEMPLATE_DIALOG : NULL, .title = "通话音量",
        .fields = &slider, .field_count = 1, .actions = &back, .action_count = 1,
    };
    xiaotai_view_show(&view);
}

static void volume_open(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!current_active() || door.overlay || (args->value && (uint32_t)args->value != door.session)) return;
    door.overlay = DOOR_DIALOG_VOLUME;
    xiaotai_ui_activity();
    volume_render();
}

static void volume_save(int32_t value)
{
    const xiaotai_err_t error = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, value);
    xiaotai_settings_volume_restore();
    call.volume_dirty = false;
    if (error == XIAOTAI_ERR_BUSY) xiaotai_view_toast("维护中，设置暂不可修改", 0);
    else if (error != XIAOTAI_OK) xiaotai_view_toast("音量设置未受理，请重试", 0);
}

void door_volume_commit(void)
{
    if (call.volume_dirty) volume_save(call.preview);
}

static xiaotai_err_t volume_quiesce_guard(const char *reason, void *ctx)
{
    (void)reason;
    (void)ctx;
    /* The unfinished drag belongs to the view; settings only own admitted writes. */
    return call.volume_dirty ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
}

static void volume_set(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!current_active() || door.overlay != DOOR_DIALOG_VOLUME || args->value < 0 || args->value > 100) return;
    if (!check_mutable()) {
        volume_render();
        return;
    }
    xiaotai_ui_activity();
    const int32_t step = xiaotai_settings_volume_step_percent();
    const int32_t nearest = (args->value + step / 2) / step * step;
    const int32_t value = args->value == 100 || nearest > 100 ? 100 : nearest;
    if (memchr(args->text, '\0', sizeof(args->text)) && !strcmp(args->text, XIAOTAI_ARG_LIVE)) {
        if (xiaotai_settings_volume_preview((uint8_t)value) == XIAOTAI_OK) {
            call.preview = value;
            call.volume_dirty = true;
        } else xiaotai_view_toast("音量设置未受理，请重试", 0);
    } else {
        /* The slider's release sample can predate the preview result. */
        door_volume_commit();
    }
    volume_render();
}

void door_call_settings(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!door.foreground || (strcmp(event->args.text, XIAOTAI_SETTING_AUDIO_VOLUME) &&
        strcmp(event->args.text, XIAOTAI_SETTING_AUDIO_MIC_MUTED))) return;
    if (!strcmp(event->name, XIAOTAI_EVENT_SETTINGS_REFUSED)) xiaotai_view_toast("设置未受理，请重试", 0);
    if (door.overlay == DOOR_DIALOG_VOLUME) volume_render();
    if (door_on_call_page()) door_render();
}

void door_call_render(void)
{
    if (!door.session && door_hunt_active()) {
        door_hunt_render();
        return;
    }
    if (door_look_seen(call.shown.session)) {
        door_look_station_render();
        return;
    }
    const bool ringing = call.shown.state == XIAOTAI_CALL_INCOMING;
    /* The indoor station looks before it answers: for the visitor it still rings. */
    const bool viewed = door_look_viewed(call.shown.session);
    const bool active = call.shown.state == XIAOTAI_CALL_ACTIVE && !viewed;
    const bool video = (call.shown.flags & XIAOTAI_CALL_VIDEO) != 0;
    const int32_t session = (int32_t)door.session;
    const uint8_t busy = call.pending ? XIAOTAI_STATE_BUSY : 0;
    const xiaotai_app_door_call_view_t custom = {
        .phase = ringing ? XIAOTAI_APP_DOOR_CALL_RINGING :
                 active ? XIAOTAI_APP_DOOR_CALL_ACTIVE : XIAOTAI_APP_DOOR_CALL_CONNECTING,
        .video = video, .muted = call.shown.muted,
    };
    /* The door station (role sender) calls the indoor station; the viewer is called by it. */
    const bool station = door.sender;
    const char *label = ringing ? (station ? "来电" : "门口来电") : !active ? (station ? "正在呼叫" : "正在建立通话") :
                        video ? "视频通话" : "语音通话";
    char subtitle[192], which[32] = "", peer[XIAOTAI_CALL_PEER_NAME_MAX] = "";
    if (station && !active && call.shown.outgoing) door_hunt_caption(which, sizeof(which));
    /* Who is on the other side: whom the station called (its own call only: a call that comes in between two
     * of a hunt is somebody else), else the peer by its remark, never by its raw id. */
    if (call.shown.peer_id[0]) door_device_label(call.shown.peer_id, call.shown.peer_name, peer, sizeof(peer));
    else if (call.shown.peer_name[0]) (void)snprintf(peer, sizeof(peer), "%s", call.shown.peer_name);
    const char *const who = station && call.shown.outgoing && door.called[0] ? door.called :
                            peer[0] ? peer : station ? "室内机" : door_config.door_name;
    (void)snprintf(subtitle, sizeof(subtitle), "%s · %s%s", who, label, which);
    static const char *const waiting[] = {
        [ACCEPT] = "正在接听", [REJECT] = "正在拒接", [HANGUP] = "正在结束",
    };
    xiaotai_view_action_t actions[4];
    size_t count = 0;
    if (ringing) {
        /* Answering stands first and refusing is "拒接", as on every other page that rings on this device. */
        actions[count++] = (xiaotai_view_action_t){.label = "接听", .intent = "door.call.accept", .value = session,
            .icon = XIAOTAI_ICON_ACCEPT, .style = XIAOTAI_STYLE_POSITIVE, .role = XIAOTAI_ROLE_PRIMARY, .state = busy};
        actions[count++] = (xiaotai_view_action_t){.label = "拒接", .intent = "door.call.reject", .value = session,
            .icon = XIAOTAI_ICON_HANGUP, .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK, .state = busy};
    } else {
        if (active) {
            if (!station) {
                actions[count++] = (xiaotai_view_action_t){.label = call.shown.muted ? "取消静音" : "静音",
                    .intent = XIAOTAI_INTENT_APP_DOOR_MUTE, .value = session, .state = busy,
                    .icon = call.shown.muted ? XIAOTAI_ICON_MIC_OFF : XIAOTAI_ICON_MIC};
            }
            actions[count++] = (xiaotai_view_action_t){.label = "音量", .intent = XIAOTAI_INTENT_APP_DOOR_VOLUME,
                .value = session, .icon = XIAOTAI_ICON_VOLUME};
            if (!station && !door.bell_indoor) {
                actions[count++] = (xiaotai_view_action_t){.label = "开门", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN,
                    .value = session, .icon = XIAOTAI_ICON_LOCK};
            }
        }
        actions[count++] = (xiaotai_view_action_t){.label = station && !active ? "取消" : "挂断",
            .intent = "door.call.hangup", .value = session,
            .icon = XIAOTAI_ICON_HANGUP, .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK, .state = busy};
    }
    const int64_t elapsed = active ? xiaotai_time_us() - call.shown.active_since_us : 0;
    const char *title = ringing ? (station ? "来电" : "门口有人") : active ? "正在通话" :
                        station ? "正在呼叫" : "正在连接";
    const char *text = ringing ? (station ? "接听后即可通话" : "接听后可以和访客通话") :
                       viewed ? "对方正在看门口，接听后即可通话" :
                       !active ? (station ? "请稍候，对方接听后即可通话" : "正在建立通话") :
                       station ? "请对着本机说话" : NULL;
    xiaotai_view_t view = {
        .id = XIAOTAI_APP_DOOR_PAGE_CALL, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_CALL,
        .custom_data = &custom, .custom_size = sizeof(custom),
        .title = title, .subtitle = subtitle,
        .status = call.pending ? waiting[call.pending] : ringing ? "待接听" : active ? NULL :
                  station ? "等待接听" : "连接中",
        .text = text,
        .timer_mode = active && !call.pending ? XIAOTAI_TIMER_ELAPSED : XIAOTAI_TIMER_NONE,
        .timer_s = elapsed > 0 ? (int32_t)(elapsed / 1000000LL) : 0,
        .flags = active && door.call_picture && call.shown.link == door.call_picture_generation ? XIAOTAI_VIEW_VIDEO : 0,
        .actions = actions, .action_count = count,
    };
    if (!door_own_template()) {
        /* The display's own call page: badge, name, state, round buttons. */
        view.kind = XIAOTAI_VIEW_CALL;
        view.custom = NULL;
        view.subtitle = who;
        char state[96];
        (void)snprintf(state, sizeof(state), "%s%s", viewed && !call.pending ? "对方正在看门口" : view.status ? view.status : label,
                       which);
        view.text = state;
        door_register_view(&view);
        return;
    }
    door_register_view(&view);
}

xiaotai_err_t door_init_call(void)
{
    static const xiaotai_feedback_state_t rings[] = {
        {.name = XIAOTAI_APP_DOOR_STATE_INCOMING, .priority = 3, .sound.name = XIAOTAI_SOUND_RING_INCOMING},
        {.name = XIAOTAI_APP_DOOR_STATE_OUTGOING, .priority = 3, .sound.name = XIAOTAI_SOUND_RING_OUTGOING},
    };
    static const xiaotai_feedback_table_t table = {.states = rings, .state_count = 2};
    xiaotai_err_t err = xiaotai_feedback_register(&table);
    if (err == XIAOTAI_OK) err = xiaotai_quiesce_guard_register("door.volume", volume_quiesce_guard, NULL);
    static const struct { const char *intent; operation_t operation; xiaotai_intent_fn_t handler; } controls[] = {
        {XIAOTAI_INTENT_APP_DOOR_CALL, NONE, control},
        {"door.call.accept", ACCEPT, control}, {"door.call.reject", REJECT, control}, {"door.call.hangup", HANGUP, control},
        {XIAOTAI_INTENT_APP_DOOR_MUTE, NONE, toggle_mute},
        {XIAOTAI_INTENT_APP_DOOR_VOLUME, NONE, volume_open}, {XIAOTAI_INTENT_APP_DOOR_VOLUME_SET, NONE, volume_set},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(controls) / sizeof(*controls); ++i) {
        err = xiaotai_intent_handle(controls[i].intent, controls[i].handler, (void *)(uintptr_t)controls[i].operation);
    }
    return err;
}
