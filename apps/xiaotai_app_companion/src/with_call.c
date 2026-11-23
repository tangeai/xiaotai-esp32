// SPDX-License-Identifier: MIT
#include "companion_call_ui.h"

#include <string.h>

#include "xiaotai/modules/call.h"

static const char *result(xiaotai_call_end_t end, bool *failure)
{
    switch (end) {
    case XIAOTAI_CALL_END_PEER_HANGUP: return "对方已挂断";
    case XIAOTAI_CALL_END_PEER_REJECT: return "对方已拒绝";
    case XIAOTAI_CALL_END_PEER_CANCEL: return "来电已结束";
    case XIAOTAI_CALL_END_MISSED: *failure = true; return "来电已结束";
    case XIAOTAI_CALL_END_NO_ANSWER: *failure = true; return "无人接听";
    case XIAOTAI_CALL_END_BUSY: *failure = true; return "对方忙线";
    case XIAOTAI_CALL_END_NETWORK: *failure = true; return "网络中断";
    case XIAOTAI_CALL_END_FAILED:
    case XIAOTAI_CALL_END_NO_MEMORY:
    case XIAOTAI_CALL_END_AUDIO: *failure = true; return "通话未完成";
    default: return "通话已结束";
    }
}

static companion_call_phase_t phase(xiaotai_call_state_t state)
{
    switch (state) {
    case XIAOTAI_CALL_OUTGOING: return COMPANION_CALL_OUTGOING;
    case XIAOTAI_CALL_INCOMING: return COMPANION_CALL_INCOMING;
    case XIAOTAI_CALL_CONNECTING: return COMPANION_CALL_CONNECTING;
    case XIAOTAI_CALL_ACTIVE: return COMPANION_CALL_ACTIVE;
    default: return COMPANION_CALL_IDLE;
    }
}

static void read(companion_call_snapshot_t *out)
{
    xiaotai_call_status_t status;
    xiaotai_call_status(&status);
    *out = (companion_call_snapshot_t){.session = status.session, .link = status.link, .phase = phase(status.state),
        .muted = status.muted, .video = (status.flags & XIAOTAI_CALL_VIDEO) != 0, .outgoing = status.outgoing,
        .releasing = status.releasing, .active_since_us = status.active_since_us, .error = status.error};
    out->result = result(status.end, &out->failed);
    size_t length = strnlen(status.peer_name, sizeof(out->peer) - 1U);
    memcpy(out->peer, status.peer_name, length);
    if (companion.style->policy & XIAOTAI_APP_COMPANION_KEY_CALL_HINTS) {
        length = strnlen(status.peer_id, sizeof(out->peer_id) - 1U);
        memcpy(out->peer_id, status.peer_id, length);
    }
}

static const companion_call_ops_t ops = {.read = read, .accept = xiaotai_call_accept,
    .reject = xiaotai_call_reject, .hangup = xiaotai_call_hangup, .mute = xiaotai_call_set_muted,
    .save_mute = xiaotai_call_set_saved_mute};

static void event(const xiaotai_event_t *message, void *unused);

static const xiaotai_subscriber_descriptor_t events[] = {
    {XIAOTAI_EVENT_CALL_INCOMING, event, NULL},
    {XIAOTAI_EVENT_CALL_STATE, event, NULL},
    {XIAOTAI_EVENT_CALL_CONNECTED, event, NULL},
    {XIAOTAI_EVENT_CALL_ENDED, event, NULL},
};

static bool claim(const xiaotai_event_t *event)
{
    if (companion.status.setup || !event->data || event->size != sizeof(xiaotai_call_status_t)) return false;
    if (strcmp(event->name, events[0].name) && strcmp(event->name, events[1].name) &&
        strcmp(event->name, events[2].name)) return false;
    const xiaotai_call_status_t *reported = event->data;
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    return current.session && current.session == reported->session;
}

static void event(const xiaotai_event_t *message, void *unused)
{
    (void)unused;
    if (message->data && message->size == sizeof(xiaotai_call_status_t))
        companion_call_ui_event(&ops, ((const xiaotai_call_status_t *)message->data)->session);
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
    if (strcmp(kind, XIAOTAI_CALL_CONTACT_KIND)) return XIAOTAI_ERR_NOT_SUPPORTED;
    return xiaotai_call_dial(id, video ? XIAOTAI_CALL_VIDEO : 0, NULL);
}

static bool home(companion_home_session_t *out) { return companion_call_ui_home(&ops, out); }
static void foreground(bool visible) { companion_call_ui_foreground(&ops, visible); }
static xiaotai_err_t main_action(int32_t action) { return companion_call_ui_main(&ops, action); }
static xiaotai_err_t end(uint32_t identity) { return companion_call_ui_end(&ops, identity); }
static xiaotai_err_t session(const xiaotai_args_t *args) { return companion_call_ui_session(&ops, args); }

const xiaotai_app_companion_part_t xiaotai_app_companion_with_call = {
    .name = "call", .init = init, .claim = claim, .home_session = home, .foreground = foreground,
    .reset = companion_call_ui_reset, .main = main_action, .end = end, .session = session, .dial_contact = dial,
};
