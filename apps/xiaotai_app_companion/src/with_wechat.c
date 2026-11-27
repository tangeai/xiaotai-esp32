// SPDX-License-Identifier: MIT
#include "companion_call_ui.h"

#include <string.h>

#include "xiaotai/modules/settings.h"
#include "xiaotai/modules/wechat.h"

static const char *result(xiaotai_wechat_end_t end, bool *failure, companion_call_follow_t *follow)
{
    *follow = COMPANION_CALL_REDIAL;
    switch (end) {
    case XIAOTAI_WECHAT_END_PEER_HANGUP: *follow = COMPANION_CALL_NOTHING; return "对方已挂断";
    case XIAOTAI_WECHAT_END_PEER_REJECT: return "对方没有接听\n对方拒绝了这次呼叫";
    case XIAOTAI_WECHAT_END_PEER_CANCEL: return "对方已取消呼叫";
    case XIAOTAI_WECHAT_END_MISSED: *failure = true; return "未接来电";
    case XIAOTAI_WECHAT_END_NO_ANSWER: *failure = true; return "无人接听\n可以过一会儿再拨";
    case XIAOTAI_WECHAT_END_NETWORK: *failure = true; return "网络中断\n请检查网络后再拨";
    case XIAOTAI_WECHAT_END_CONTACT:
        *failure = true;
        *follow = COMPANION_CALL_AUTHORIZE;
        return "微信授权失效\n请让对方在“小钛”小程序里重新给这台设备授权微信通话";
    case XIAOTAI_WECHAT_END_FAILED: *failure = true; return "没有打通\n微信没有受理这次呼叫，请稍后再拨";
    case XIAOTAI_WECHAT_END_NO_MEMORY: *failure = true; return "设备正忙\n请过几秒再拨";
    case XIAOTAI_WECHAT_END_AUDIO: *failure = true; return "声音没有启动\n请再拨一次";
    default: *follow = COMPANION_CALL_NOTHING; return "通话已结束";
    }
}

static companion_call_phase_t phase(xiaotai_wechat_state_t state)
{
    switch (state) {
    case XIAOTAI_WECHAT_OUTGOING: return COMPANION_CALL_OUTGOING;
    case XIAOTAI_WECHAT_INCOMING: return COMPANION_CALL_INCOMING;
    case XIAOTAI_WECHAT_CONNECTING: return COMPANION_CALL_CONNECTING;
    case XIAOTAI_WECHAT_ACTIVE: return COMPANION_CALL_ACTIVE;
    default: return COMPANION_CALL_IDLE;
    }
}

static void read(companion_call_snapshot_t *out)
{
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    *out = (companion_call_snapshot_t){.session = status.session, .link = status.link, .phase = phase(status.state),
        .muted = status.muted, .video = (status.flags & XIAOTAI_WECHAT_VIDEO) != 0,
        .video_offered = (status.flags & XIAOTAI_WECHAT_VIDEO_OFFERED) != 0,
        .local_end = status.end == XIAOTAI_WECHAT_END_LOCAL, .outgoing = status.outgoing,
        .releasing = status.releasing, .active_since_us = status.active_since_us, .error = status.error,
        .result_token = status.authorization_result};
    out->result = result(status.end, &out->failed, &out->follow);
    const size_t length = strnlen(status.peer_name, sizeof(out->peer) - 1U);
    memcpy(out->peer, status.peer_name, length);
}

static xiaotai_err_t save_mute(uint32_t session, bool muted)
{
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    if (!session || status.session != session) return XIAOTAI_ERR_INVALID_STATE;
    return xiaotai_settings_set_mic_mute(muted);
}

static const companion_call_ops_t ops = {.wechat = true, .read = read, .accept = xiaotai_wechat_accept,
    .reject = xiaotai_wechat_reject, .hangup = xiaotai_wechat_hangup, .mute = xiaotai_wechat_set_muted,
    .save_mute = save_mute, .result_dismissed = xiaotai_wechat_result_dismissed};

static void event(const xiaotai_event_t *message, void *unused);

static const xiaotai_subscriber_descriptor_t events[] = {
    {XIAOTAI_EVENT_WECHAT_INCOMING, event, NULL},
    {XIAOTAI_EVENT_WECHAT_STATE, event, NULL},
    {XIAOTAI_EVENT_WECHAT_CONNECTED, event, NULL},
    {XIAOTAI_EVENT_WECHAT_ENDED, event, NULL},
};

static bool claim(const xiaotai_event_t *event)
{
    if (companion.status.setup || !event->data || event->size != sizeof(xiaotai_wechat_status_t)) return false;
    if (strcmp(event->name, events[0].name) && strcmp(event->name, events[1].name) &&
        strcmp(event->name, events[2].name)) return false;
    const xiaotai_wechat_status_t *reported = event->data;
    xiaotai_wechat_status_t current;
    xiaotai_wechat_status(&current);
    return current.session && current.session == reported->session;
}

static void event(const xiaotai_event_t *message, void *unused)
{
    (void)unused;
    if (message->data && message->size == sizeof(xiaotai_wechat_status_t))
        companion_call_ui_event(&ops, ((const xiaotai_wechat_status_t *)message->data)->session);
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = companion_call_ui_init();
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(events[0]); ++i)
        err = xiaotai_event_subscribe_static(&events[i]);
    return err;
}

static xiaotai_err_t dial(const char *kind, const char *id, bool video)
{
    if (strcmp(kind, XIAOTAI_WECHAT_CONTACT_KIND)) return XIAOTAI_ERR_NOT_SUPPORTED;
    return xiaotai_wechat_dial(id, video ? XIAOTAI_WECHAT_VIDEO : 0, NULL);
}

static bool home(companion_home_session_t *out) { return companion_call_ui_home(&ops, out); }
static void foreground(bool visible) { companion_call_ui_foreground(&ops, visible); }
static xiaotai_err_t main_action(int32_t action) { return companion_call_ui_main(&ops, action); }
static xiaotai_err_t end(uint32_t identity) { return companion_call_ui_end(&ops, identity); }
static xiaotai_err_t session(const xiaotai_args_t *args) { return companion_call_ui_session(&ops, args); }

const xiaotai_app_companion_part_t xiaotai_app_companion_with_wechat = {
    .name = "wechat", .init = init, .claim = claim, .home_session = home, .foreground = foreground,
    .reset = companion_call_ui_reset, .main = main_action, .end = end, .session = session, .dial_contact = dial,
};
