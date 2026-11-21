// SPDX-License-Identifier: MIT
#include "door_internal.h"
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
} call_ui_t;
static call_ui_t call;

const xiaotai_call_status_t *door_shown_call(void) { return &call.shown; }

bool door_call_matches(const xiaotai_call_status_t *snapshot)
{
    if (snapshot->session && snapshot->session == door.classification_session) return door.classified_door;
    return snapshot->session && door.peer[0] && !strcmp(snapshot->peer_id, door.peer);
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
    (void)xiaotai_feedback_set(XIAOTAI_APP_DOOR_STATE_INCOMING, present && call.shown.state == XIAOTAI_CALL_INCOMING);
    (void)xiaotai_feedback_set(XIAOTAI_APP_DOOR_STATE_OUTGOING,
                              present && call.shown.outgoing &&
                              (call.shown.state == XIAOTAI_CALL_OUTGOING || call.shown.state == XIAOTAI_CALL_CONNECTING));
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
    return door.foreground && door.page == DOOR_CALL && door.session && current.session == door.session &&
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
        if (!door.session) return;
        xiaotai_call_status_t current;
        xiaotai_call_status(&current);
        if (current.session == door.session && current.state != XIAOTAI_CALL_IDLE) return;
        const bool other = door.page == DOOR_OTHER_CALL;
        door.session = door.classification_session = 0;
        door.classified_door = false;
        door_format_clear(call.shown.link, &door.call_format_noted, DOOR_CALL_FORMAT_TOAST);
        door.call_picture = false;
        door.call_picture_generation = 0;
        door.call_format_noted = 0;
        cancel_pending();
        if (!door.setup) {
            if (other && door.foreground && current.state == XIAOTAI_CALL_IDLE) door_fallback_device_ended(&current);
            else door_finish_presentation();
        }
        door_call_ring_update();
        return;
    }
    const xiaotai_call_status_t *snapshot = event->size == sizeof(*snapshot) ? event->data : NULL;
    if (!snapshot || !snapshot->session) return;
    const bool other = door.session != snapshot->session;
    const bool selected = door_call_matches(snapshot);
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
    } else if (call.pending && snapshot->state != call.phase) cancel_pending();
    door.session = door.classification_session = snapshot->session;
    door.classified_door = selected;
    call.shown = *snapshot;
    if (!door.setup) door_page(selected ? DOOR_CALL : DOOR_OTHER_CALL);
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
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_OVERLAY_VOLUME,
        .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG, .title = "通话音量",
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
    if (door.page == DOOR_CALL) door_render();
}

void door_call_render(void)
{
    const bool ringing = call.shown.state == XIAOTAI_CALL_INCOMING;
    const bool active = call.shown.state == XIAOTAI_CALL_ACTIVE;
    const bool video = (call.shown.flags & XIAOTAI_CALL_VIDEO) != 0;
    const int32_t session = (int32_t)door.session;
    const uint8_t busy = call.pending ? XIAOTAI_STATE_BUSY : 0;
    const xiaotai_app_door_call_view_t custom = {
        .phase = ringing ? XIAOTAI_APP_DOOR_CALL_RINGING :
                 active ? XIAOTAI_APP_DOOR_CALL_ACTIVE : XIAOTAI_APP_DOOR_CALL_CONNECTING,
        .video = video, .muted = call.shown.muted,
    };
    const char *label = ringing ? "门口来电" : !active ? "正在建立通话" :
                        video ? "双向视频通话" : "双向语音通话";
    char subtitle[192];
    (void)snprintf(subtitle, sizeof(subtitle), "%s · %s",
                   call.shown.peer_name[0] ? call.shown.peer_name : door_config.door_name, label);
    static const char *const waiting[] = {
        [ACCEPT] = "正在接听", [REJECT] = "正在拒接", [HANGUP] = "正在结束",
    };
    xiaotai_view_action_t actions[4];
    size_t count = 0;
    if (ringing) {
        actions[count++] = (xiaotai_view_action_t){.label = "拒绝", .intent = "door.call.reject", .value = session,
            .icon = XIAOTAI_ICON_HANGUP, .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK, .state = busy};
        actions[count++] = (xiaotai_view_action_t){.label = "接听", .intent = "door.call.accept", .value = session,
            .icon = XIAOTAI_ICON_ACCEPT, .style = XIAOTAI_STYLE_POSITIVE, .role = XIAOTAI_ROLE_PRIMARY, .state = busy};
    } else {
        if (active) {
            actions[count++] = (xiaotai_view_action_t){.label = call.shown.muted ? "取消静音" : "静音",
                .intent = XIAOTAI_INTENT_APP_DOOR_MUTE, .value = session, .state = busy,
                .icon = call.shown.muted ? XIAOTAI_ICON_MIC_OFF : XIAOTAI_ICON_MIC};
            actions[count++] = (xiaotai_view_action_t){.label = "音量", .intent = XIAOTAI_INTENT_APP_DOOR_VOLUME,
                .value = session, .icon = XIAOTAI_ICON_VOLUME};
            actions[count++] = (xiaotai_view_action_t){.label = "开门", .intent = XIAOTAI_INTENT_APP_DOOR_OPEN,
                .value = session, .icon = XIAOTAI_ICON_LOCK};
        }
        actions[count++] = (xiaotai_view_action_t){.label = "挂断", .intent = "door.call.hangup", .value = session,
            .icon = XIAOTAI_ICON_HANGUP, .style = XIAOTAI_STYLE_DESTRUCTIVE, .role = XIAOTAI_ROLE_BACK, .state = busy};
    }
    const int64_t elapsed = active ? xiaotai_time_us() - call.shown.active_since_us : 0;
    const xiaotai_view_t view = {
        .id = XIAOTAI_APP_DOOR_PAGE_CALL, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_CALL,
        .custom_data = &custom, .custom_size = sizeof(custom),
        .title = ringing ? "门口有人" : active ? "正在通话" : "正在连接", .subtitle = subtitle,
        .status = call.pending ? waiting[call.pending] : ringing ? "待接听" : active ? NULL : "连接中",
        .text = ringing ? "接听后显示门口画面" : !active ? "正在建立通话" : NULL,
        .timer_mode = active && !call.pending ? XIAOTAI_TIMER_ELAPSED : XIAOTAI_TIMER_NONE,
        .timer_s = elapsed > 0 ? (int32_t)(elapsed / 1000000LL) : 0,
        .flags = active && door.call_picture && call.shown.link == door.call_picture_generation ? XIAOTAI_VIEW_VIDEO : 0,
        .actions = actions, .action_count = count,
    };
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
