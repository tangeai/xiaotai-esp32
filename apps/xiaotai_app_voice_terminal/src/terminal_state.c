// SPDX-License-Identifier: MIT
#include "terminal_internal.h"

#include <string.h>
#include "xiaotai/feedback.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"

#define STATE(n) XIAOTAI_APP_VOICE_TERMINAL_STATE_##n
#define EFFECT(n) XIAOTAI_APP_VOICE_TERMINAL_EFFECT_##n

terminal_situation_t terminal_situation(const xiaotai_wechat_status_t *call)
{
    if (call->releasing) return TERMINAL_CLOSING;
    if (call->state == XIAOTAI_WECHAT_IDLE) return TERMINAL_IDLE;
    return call->state == XIAOTAI_WECHAT_INCOMING ? TERMINAL_RINGING : TERMINAL_BUSY;
}

static void switch_state(const char **present, const char *next)
{
    if (*present == next || (*present && next && strcmp(*present, next) == 0)) return;
    if (*present) (void)xiaotai_feedback_set(*present, false);
    *present = next;
    if (next) (void)xiaotai_feedback_set(next, true);
}

static const char *light_for(const xiaotai_wechat_status_t *call, xiaotai_net_state_t network)
{
    if (xiaotai_provisioning_active()) return STATE(SETUP);
    if (call->releasing) return STATE(CLOSING);
    switch (call->state) {
    case XIAOTAI_WECHAT_ACTIVE: return STATE(ACTIVE);
    case XIAOTAI_WECHAT_CONNECTING: return STATE(LINKING);
    case XIAOTAI_WECHAT_INCOMING: return STATE(INCOMING);
    case XIAOTAI_WECHAT_OUTGOING: return STATE(OUTGOING);
    default: break;
    }
    if (network == XIAOTAI_NET_BLOCKED || terminal.portal_error) return STATE(ERROR);
    if (network == XIAOTAI_NET_STOPPED) return STATE(BOOT);
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    if (binding.phase != XIAOTAI_BINDING_BOUND && !binding.device_id[0]) return STATE(BINDING);
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    return platform.phase == XIAOTAI_PLATFORM_ONLINE ? STATE(READY) : STATE(CONNECTING);
}

void terminal_show(const xiaotai_wechat_status_t *call)
{
    if (call->session != terminal.call_session || call->state == XIAOTAI_WECHAT_IDLE) {
        terminal.ring_silenced = false;
        terminal.key_answer = 0;
    } else if (terminal.call_state == XIAOTAI_WECHAT_INCOMING &&
               call->state == XIAOTAI_WECHAT_CONNECTING && terminal.key_answer != call->session) {
        /* Accepting from outside this key's handler silences this call. */
        terminal.ring_silenced = true;
    }
    terminal.call_state = call->state;
    terminal.call_session = call->session;
    terminal.outgoing = call->outgoing;
    terminal.releasing = call->releasing;
    const char *ring = NULL;
    if (!terminal.ring_silenced && !call->releasing) {
        if (call->state == XIAOTAI_WECHAT_INCOMING) ring = STATE(RING);
        else if (call->outgoing &&
            (call->state == XIAOTAI_WECHAT_OUTGOING || call->state == XIAOTAI_WECHAT_CONNECTING)) {
            ring = STATE(RINGBACK);
        }
    }
    switch_state(&terminal.ring, ring);

    xiaotai_net_status_t network;
    xiaotai_net_status(&network);
    switch_state(&terminal.light, light_for(call, network.state));
    const bool fault = network.state == XIAOTAI_NET_BLOCKED;
    if (fault && !terminal.network_error) (void)xiaotai_feedback_fire(EFFECT(FAILED), NULL);
    terminal.network_error = fault;
}

static bool failed_call(const xiaotai_wechat_status_t *call)
{
    switch (call->end) {
    case XIAOTAI_WECHAT_END_NO_ANSWER: case XIAOTAI_WECHAT_END_MISSED:
    case XIAOTAI_WECHAT_END_CONTACT: case XIAOTAI_WECHAT_END_FAILED:
    case XIAOTAI_WECHAT_END_NETWORK: case XIAOTAI_WECHAT_END_NO_MEMORY: case XIAOTAI_WECHAT_END_AUDIO:
        return true;
    case XIAOTAI_WECHAT_END_PEER_HANGUP:
        return call->peer_reason == 6 || call->peer_reason == 8;
    default:
        return false;
    }
}

static void call_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event->data || event->size != sizeof(xiaotai_wechat_status_t)) return;
    const xiaotai_wechat_status_t *call = event->data;
    terminal_show(call);
    if (strcmp(event->name, XIAOTAI_EVENT_WECHAT_CONNECTED) == 0 && call->state == XIAOTAI_WECHAT_ACTIVE) {
        (void)xiaotai_feedback_fire(EFFECT(CONNECTED), NULL);
    } else if (strcmp(event->name, XIAOTAI_EVENT_WECHAT_ENDED) == 0 && call->end != XIAOTAI_WECHAT_END_NONE) {
        /* Clearing the loop above must precede queuing the cue. */
        (void)xiaotai_feedback_fire(failed_call(call) ? EFFECT(FAILED) : EFFECT(ENDED), NULL);
    }
}

static void status_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_OPENED) == 0) terminal.portal_error = false;
    terminal_refresh();
    if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_OPENED) == 0) {
        (void)xiaotai_feedback_fire(EFFECT(SETUP), NULL);
    }
}

void terminal_refresh(void)
{
    const xiaotai_wechat_status_t call = {.state = terminal.call_state, .session = terminal.call_session,
        .outgoing = terminal.outgoing, .releasing = terminal.releasing};
    terminal_show(&call);
}

void terminal_silence_ring(void)
{
    terminal.ring_silenced = true;
    switch_state(&terminal.ring, NULL);
}

xiaotai_err_t terminal_state_init(void)
{
    static const xiaotai_subscriber_descriptor_t calls[] = {
        {XIAOTAI_EVENT_WECHAT_STATE, call_event, NULL},
        {XIAOTAI_EVENT_WECHAT_INCOMING, call_event, NULL},
        {XIAOTAI_EVENT_WECHAT_CONNECTED, call_event, NULL},
        {XIAOTAI_EVENT_WECHAT_ENDED, call_event, NULL},
    };
    static const xiaotai_subscriber_descriptor_t status[] = {
        {XIAOTAI_EVENT_STARTED, status_event, NULL},
        {XIAOTAI_EVENT_NET_STATE, status_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_ONLINE, status_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, status_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_PAUSED, status_event, NULL},
        {XIAOTAI_EVENT_BINDING_CHANGED, status_event, NULL},
        {XIAOTAI_EVENT_PROVISIONING_CHANGED, status_event, NULL},
        {XIAOTAI_EVENT_PROVISIONING_OPENED, status_event, NULL},
    };
    for (size_t i = 0; i < sizeof(calls) / sizeof(*calls); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe_static(&calls[i]);
        if (err != XIAOTAI_OK) return err;
    }
    for (size_t i = 0; i < sizeof(status) / sizeof(*status); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe_static(&status[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}
