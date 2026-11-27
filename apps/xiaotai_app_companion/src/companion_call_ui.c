// SPDX-License-Identifier: MIT
#include "companion_call_ui.h"

#include <string.h>

#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"

enum { ACCEPT, REJECT, HANGUP, MUTE, CAMERA, ACTIONS };

static const char *const actions[ACTIONS] = {"accept", "reject", "hangup", "mute", "camera"};
static const char *const local_intents[ACTIONS] = {"companion.call.accept", "companion.call.reject",
    "companion.call.hangup", "companion.call.mute", "companion.call.camera"};
/* Below this a voice call starts to break up. */
#define CALL_WEAK_DBM (-78)
static const char video_format_notice[] = "companion.call_format";

static struct {
    bool initialized;
    const companion_call_ops_t *owner;
    const companion_call_video_ops_t *video;
    const companion_call_log_ops_t *log;
    companion_call_snapshot_t current;
    const companion_call_ops_t *ending_owner;
    const char *pending_result;
    companion_call_follow_t follow;
    bool answered;
    uint32_t revision, warned_link;
} s;

static void clear_format_warning(uint32_t link)
{
    if (!link || s.warned_link != link) return;
    s.warned_link = 0;
    xiaotai_view_dismiss(XIAOTAI_LAYER_TOAST, video_format_notice);
}

static bool result_always(void) { return (companion.style->policy & XIAOTAI_APP_COMPANION_RESULT_ALWAYS) != 0; }

static companion_call_camera_t camera(void)
{
    return s.video && s.current.video && s.current.link ? s.video->read(s.current.link) : (companion_call_camera_t){0};
}

static int action_index(const char *name)
{
    for (int i = 0; i < ACTIONS; ++i) if (!strcmp(actions[i], name)) return i;
    return -1;
}

static uint8_t busy(unsigned action)
{
    return companion_session_pending(actions[action], s.revision) ? XIAOTAI_STATE_BUSY : 0;
}

static void append_action(xiaotai_view_t *view, unsigned action, uint8_t role, const char *icon,
                          const char *label, uint8_t style)
{
    const uint8_t state = busy(action);
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
        .intent = local_intents[action], .value = (int32_t)s.current.session, .role = role,
        .icon = icon, .label = label, .style = style, .state = state,
    };
    if (state) view->text = "正在处理…";
}

/* What matters most about a running call: what the other side cannot hear,
 * what this side cannot, and why it may sound broken. */
static const char *active_state(void)
{
    if (s.current.muted) return "麦克风已关闭";
    int32_t value = 0;
    if (xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, &value) == XIAOTAI_OK && value) return "扬声器已关闭";
    if (xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &value) == XIAOTAI_OK && !value) return "音量为 0";
    if (!companion_home_network_up()) return "网络断开，正在恢复";
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    return net.signal_valid && net.signal_dbm < CALL_WEAK_DBM ? "网络不好" : "通话中";
}

/* The call page says who the call is with: the name, and the round mark a display draws for it. It carries
 * no expression: the face is the assistant's, and a person is on the line. */
static void build_call(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    const bool wechat = s.owner && s.owner->wechat;
    const bool key_hints = (companion.style->policy & XIAOTAI_APP_COMPANION_KEY_CALL_HINTS) != 0;
    view->kind = XIAOTAI_VIEW_CALL;
    view->title = s.current.video ? (wechat ? "微信视频" : "设备视频") : (wechat ? "微信语音" : "设备语音");
    view->subtitle = s.current.peer[0] ? s.current.peer : (wechat ? "微信联系人" : "未知联系人");
    view->actions = companion.buffer->actions;
    const companion_call_camera_t video = camera();
    if (video.remote) view->flags |= XIAOTAI_VIEW_VIDEO;
    switch (s.current.phase) {
    case COMPANION_CALL_INCOMING:
        view->title = wechat ? "微信来电" : "设备来电";
        view->text = s.current.video ? "视频来电" : s.current.video_offered ? "视频来电，将用语音接听" : "语音来电";
        append_action(view, ACCEPT, XIAOTAI_ROLE_PRIMARY, XIAOTAI_ICON_ACCEPT,
                      key_hints ? "双击接听" : "接听", XIAOTAI_STYLE_POSITIVE);
        append_action(view, REJECT, XIAOTAI_ROLE_BACK, XIAOTAI_ICON_HANGUP,
                      key_hints ? "长按拒绝" : "拒接", XIAOTAI_STYLE_DESTRUCTIVE);
        break;
    case COMPANION_CALL_OUTGOING:
    case COMPANION_CALL_CONNECTING:
        view->text = s.current.phase == COMPANION_CALL_OUTGOING ? "正在呼叫…" : "正在建立通话…";
        append_action(view, HANGUP, XIAOTAI_ROLE_BACK, XIAOTAI_ICON_HANGUP,
                      key_hints ? NULL : "取消呼叫", XIAOTAI_STYLE_DESTRUCTIVE);
        break;
    case COMPANION_CALL_ACTIVE: {
        view->text = active_state();
        const int64_t now = xiaotai_time_us();
        const int64_t seconds = s.current.active_since_us > 0 && now > s.current.active_since_us ?
            (now - s.current.active_since_us) / 1000000 : 0;
        view->timer_mode = XIAOTAI_TIMER_ELAPSED;
        view->timer_s = seconds > INT32_MAX ? INT32_MAX : (int32_t)seconds;
        append_action(view, MUTE, XIAOTAI_ROLE_NONE, s.current.muted ? XIAOTAI_ICON_MIC_OFF : XIAOTAI_ICON_MIC,
                      s.current.muted ? "取消静音" : "静音", XIAOTAI_STYLE_QUIET);
        append_action(view, HANGUP, XIAOTAI_ROLE_BACK, XIAOTAI_ICON_HANGUP,
                      key_hints ? NULL : "挂断", XIAOTAI_STYLE_DESTRUCTIVE);
        if (companion_volume_available()) companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
            .intent = COMPANION_VOLUME_INTENT, .icon = XIAOTAI_ICON_VOLUME, .label = "音量", .style = XIAOTAI_STYLE_QUIET};
        if (video.active && video.camera) append_action(view, CAMERA, XIAOTAI_ROLE_NONE,
            video.enabled ? XIAOTAI_ICON_CAMERA : XIAOTAI_ICON_CAMERA_OFF,
            video.enabled ? "关摄像头" : "开摄像头", XIAOTAI_STYLE_QUIET);
        break;
    }
    default: break;
    }
}

static bool call_arg(int32_t arg) { return arg == 0 && s.current.session != 0; }

static void show_call(void)
{
    if (!s.current.session || !companion.status.foreground || companion.status.setup || !companion.has_display) return;
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL)) { companion_render(); return; }
    xiaotai_view_dismiss(XIAOTAI_LAYER_TOAST, NULL);
    const xiaotai_err_t err = companion_show_page(XIAOTAI_APP_COMPANION_PAGE_CALL, 0);
    if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_APP_COMPANION_PAGE_CALL, err);
}

static companion_feedback_phase_t feedback(const companion_call_snapshot_t *now)
{
    if (now->phase == COMPANION_CALL_INCOMING) return COMPANION_FEEDBACK_INCOMING;
    if (now->phase == COMPANION_CALL_ACTIVE) return COMPANION_FEEDBACK_CALL;
    return now->outgoing ? COMPANION_FEEDBACK_DIALING : COMPANION_FEEDBACK_CONNECTING;
}

static void maybe_result(void)
{
    if (!s.pending_result || s.current.session || !companion.status.foreground || companion.status.setup) return;
    if (companion.config.hold_refuses_sessions && xiaotai_activity_current(NULL)) return;
    const char *result = s.pending_result;
    void (*const ack)(uint32_t) = s.ending_owner && s.current.result_token ? s.ending_owner->result_dismissed : NULL;
    s.pending_result = NULL;
    s.ending_owner = NULL;
    const xiaotai_err_t err = companion_session_result_with_ack(result, s.current.failed, ack,
                                                                 ack ? s.current.result_token : 0U);
    if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_APP_COMPANION_PAGE_RESULT, err);
    else if (s.log) s.log->offer(s.follow);
}

static void update(const companion_call_ops_t *ops, const companion_call_snapshot_t *now)
{
    if (!now->session) {
        if (s.owner != ops || !s.current.session) return;
        clear_format_warning(s.current.link);
        const uint32_t previous = s.current.session;
        if (s.log) s.log->ended(&s.current, now, ops->wechat, s.answered);
        s.follow = now->follow;
        s.owner = NULL;
        s.current = *now;
        ++s.revision;
        companion_session_context(NULL, 0);
        companion_feedback_session(now->failed ? COMPANION_FEEDBACK_FAILED : COMPANION_FEEDBACK_IDLE, previous);
        companion_home_interaction();
        companion_user_activity();
        const bool visible = companion.status.foreground;
        companion_pop_call_page();
        if (visible && now->failed) {
            companion_report_refusal(XIAOTAI_APP_COMPANION_PAGE_CALL, now->error ? now->error : XIAOTAI_FAIL);
        }
        if (visible && companion.has_display && (result_always() || now->failed)) {
            s.pending_result = now->result;
            s.ending_owner = ops;
            maybe_result();
        } else if (visible) {
            (void)companion_finish_presentation();
        }
        return;
    }

    const bool new_call = s.owner != ops || s.current.session != now->session;
    const bool new_peer = !new_call && strcmp(s.current.peer_id, now->peer_id) != 0;
    const bool changed = new_call || new_peer || s.current.phase != now->phase || s.current.link != now->link ||
        s.current.muted != now->muted || s.current.video != now->video || strcmp(s.current.peer, now->peer);
    if (new_call || s.current.link != now->link) clear_format_warning(s.current.link);
    s.owner = ops;
    s.current = *now;
    s.pending_result = NULL;
    s.ending_owner = NULL;
    if (new_call) s.answered = false;
    if (now->phase == COMPANION_CALL_ACTIVE) s.answered = true;
    if (s.log && (new_call || new_peer)) s.log->began(now, ops->wechat);
    if (changed) ++s.revision;
    if (new_call) {
        companion_home_interaction();
        companion_user_activity();
        int32_t saved;
        if (xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &saved) == XIAOTAI_OK &&
            (saved != 0) != now->muted) {
            const xiaotai_err_t err = ops->mute(now->session, saved != 0);
            if (err != XIAOTAI_OK && companion.status.foreground) companion_refuse(XIAOTAI_APP_COMPANION_PAGE_CALL, err);
            ops->read(&s.current);
        }
    }
    const char *context = now->phase == COMPANION_CALL_INCOMING ? XIAOTAI_APP_COMPANION_CONTEXT_RINGING :
        XIAOTAI_APP_COMPANION_CONTEXT_CALL;
    if (new_peer && companion.status.foreground && !companion.status.setup &&
        !strcmp(companion.status.context, context)) xiaotai_interaction_invalidate();
    companion_session_context(context, now->session);
    companion_feedback_session(feedback(now), now->session);
    if (changed || !companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL)) show_call();
}

void companion_call_ui_event(const companion_call_ops_t *ops, uint32_t reported)
{
    companion_call_snapshot_t now;
    ops->read(&now);
    if (now.session != reported) return;
    update(ops, &now);
    if (!now.session && !now.releasing && s.ending_owner == ops) maybe_result();
}

void companion_call_ui_foreground(const companion_call_ops_t *ops, bool visible)
{
    if (!visible) return;
    companion_call_snapshot_t now;
    ops->read(&now);
    if (now.session) update(ops, &now);
}

bool companion_call_ui_home(const companion_call_ops_t *ops, companion_home_session_t *out)
{
    companion_call_snapshot_t now;
    ops->read(&now);
    if (!now.session) return false;
    *out = (companion_home_session_t){.mode = now.phase == COMPANION_CALL_INCOMING ? COMPANION_HOME_RINGING :
        now.phase == COMPANION_CALL_ACTIVE ? COMPANION_HOME_CALL : COMPANION_HOME_CALL_CONNECT};
    return true;
}

static xiaotai_err_t perform(const companion_call_ops_t *ops, int action, uint32_t identity)
{
    companion_call_snapshot_t now;
    ops->read(&now);
    if (!identity || now.session != identity || s.owner != ops || s.current.session != identity)
        return XIAOTAI_ERR_INVALID_STATE;
    if (companion_session_pending(actions[action], s.revision)) return XIAOTAI_OK;
    const uint32_t revision = s.revision;
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    switch (action) {
    case ACCEPT: if (now.phase == COMPANION_CALL_INCOMING) err = ops->accept(identity); break;
    case REJECT: if (now.phase == COMPANION_CALL_INCOMING) err = ops->reject(identity); break;
    case HANGUP: if (now.phase != COMPANION_CALL_INCOMING) err = ops->hangup(identity); break;
    case MUTE: if (now.phase == COMPANION_CALL_ACTIVE) err = ops->save_mute(identity, !now.muted); break;
    case CAMERA:
        if (now.phase == COMPANION_CALL_ACTIVE && now.video && now.link && s.video) {
            const companion_call_camera_t state = s.video->read(now.link);
            if (state.active) err = s.video->set(now.link, !state.enabled);
        }
        break;
    }
    if (err == XIAOTAI_OK) {
        companion_session_accepted(actions[action], revision);
        ops->read(&now);
        update(ops, &now);
        if (s.revision == revision) companion_render();
    }
    return err;
}

xiaotai_err_t companion_call_ui_main(const companion_call_ops_t *ops, int32_t action)
{
    if (s.owner != ops || !s.current.session) return XIAOTAI_ERR_NOT_SUPPORTED;
    const bool ringing = s.current.phase == COMPANION_CALL_INCOMING;
    if (action == 0 && ringing) return perform(ops, ACCEPT, s.current.session);
    if (action == XIAOTAI_APP_COMPANION_MAIN_DECLINE && ringing) return perform(ops, REJECT, s.current.session);
    /* Without a display another press would be one too many: nothing shows
     * that the call was answered. */
    if (action == 0 && companion.has_display) return perform(ops, HANGUP, s.current.session);
    return XIAOTAI_ERR_BUSY;
}

xiaotai_err_t companion_call_ui_end(const companion_call_ops_t *ops, uint32_t identity)
{
    if (s.owner != ops || !s.current.session) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (identity && identity != s.current.session) return XIAOTAI_ERR_INVALID_STATE;
    return perform(ops, s.current.phase == COMPANION_CALL_INCOMING ? REJECT : HANGUP, s.current.session);
}

xiaotai_err_t companion_call_ui_session(const companion_call_ops_t *ops, const xiaotai_args_t *args)
{
    const int action = action_index(args->text);
    if (action < 0 || s.owner != ops) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL)) return XIAOTAI_ERR_INVALID_STATE;
    return perform(ops, action, (uint32_t)args->value);
}

static void on_action(const xiaotai_args_t *args, void *ctx)
{
    const unsigned action = (unsigned)(uintptr_t)ctx;
    xiaotai_args_t request = {.value = args->value};
    strcpy(request.text, actions[action]);
    companion_session_dispatch(&request);
}

static void on_activity(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    if (s.pending_result && companion.config.hold_refuses_sessions) {
        const char *name = NULL;
        if (xiaotai_activity_current(&name) && name && strcmp(name, "call")) {
            s.pending_result = NULL;
            s.ending_owner = NULL;
            return;
        }
    }
    maybe_result();
}

static void on_condition(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    if (s.current.session && s.current.phase == COMPANION_CALL_ACTIVE && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL))
        companion_render();
}

static void on_page(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    if (s.current.session && !companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL)) show_call();
}

xiaotai_err_t companion_call_ui_init(void)
{
    if (s.initialized) return XIAOTAI_OK;
    xiaotai_err_t err = XIAOTAI_OK;
    if (companion.has_display) {
        static const companion_page_t page = {.id = XIAOTAI_APP_COMPANION_PAGE_CALL, .title = "通话",
                                               .arg_valid = call_arg, .build = build_call};
        err = companion_page_register(&page);
        for (unsigned i = 0; err == XIAOTAI_OK && i < ACTIONS; ++i)
            err = xiaotai_intent_handle_ex(local_intents[i], on_action, (void *)(uintptr_t)i, XIAOTAI_INTENT_LOCAL);
        if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ACTIVITY, on_activity, NULL);
        if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_APP_COMPANION_PAGE, on_page, NULL);
        static const xiaotai_subscriber_descriptor_t conditions[] = {
            {XIAOTAI_EVENT_SETTINGS_CHANGED, on_condition, NULL},
            {XIAOTAI_EVENT_NET_UP, on_condition, NULL},
            {XIAOTAI_EVENT_NET_DOWN, on_condition, NULL},
            {XIAOTAI_EVENT_NET_SIGNAL, on_condition, NULL},
        };
        for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(conditions) / sizeof(conditions[0]); ++i)
            err = xiaotai_event_subscribe_static(&conditions[i]);
    }
    s.initialized = err == XIAOTAI_OK;
    return err;
}

void companion_call_ui_reset(void) { memset(&s, 0, sizeof(s)); }

void companion_call_ui_set_video(const companion_call_video_ops_t *ops) { s.video = ops; }

void companion_call_ui_set_log(const companion_call_log_ops_t *ops) { s.log = ops; }

void companion_call_ui_video_event(uint32_t link, companion_video_format_t format)
{
    if ((link && s.current.link != link) || !s.current.session || !s.current.video) return;
    if (format == COMPANION_VIDEO_RECOVERED) clear_format_warning(link);
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL)) return;
    ++s.revision;
    if (format == COMPANION_VIDEO_UNSUPPORTED && link && s.warned_link != link) {
        s.warned_link = link;
        xiaotai_view_show(&(const xiaotai_view_t){
            .id = video_format_notice, .layer = XIAOTAI_LAYER_TOAST, .kind = XIAOTAI_VIEW_NOTICE,
            .text = "视频格式不支持", .duration_ms = companion_toast_ms(),
        });
    }
    companion_render();
}
