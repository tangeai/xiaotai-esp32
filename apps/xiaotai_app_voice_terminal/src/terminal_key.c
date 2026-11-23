// SPDX-License-Identifier: MIT
#include "terminal_internal.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/contacts.h"

#define INTENT(n) XIAOTAI_INTENT_APP_VOICE_TERMINAL_##n
#define EFFECT(n) XIAOTAI_APP_VOICE_TERMINAL_EFFECT_##n

static void key_event(const char *name, const char *effect, const char *intent, xiaotai_err_t value)
{
    xiaotai_args_t args = {.value = value};
    (void)snprintf(args.text, sizeof(args.text), "%s", intent);
    if (xiaotai_event_emit(name, &args, NULL, 0) != XIAOTAI_OK) (void)xiaotai_feedback_fire(effect, NULL);
}

void terminal_refuse(const char *intent, xiaotai_err_t result, bool stale)
{
    ++terminal.refused;
    if (stale) ++terminal.stale;
    xiaotai_eventlog_warn("terminal", stale ? "key.stale" : "key.refused", terminal.call_session, result);
    key_event(XIAOTAI_EVENT_APP_VOICE_TERMINAL_REFUSED, EFFECT(REFUSED), intent, result);
}

static xiaotai_err_t dial(void)
{
    xiaotai_contact_t contact;
    int kind;
    xiaotai_err_t err = xiaotai_contacts_kind_find(XIAOTAI_WECHAT_CONTACT_KIND, &kind);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_contacts_at_kind(kind, terminal_config.dial_contact, XIAOTAI_CONTACTS_STALE_OK, &contact);
    if (err == XIAOTAI_OK) err = xiaotai_wechat_dial(contact.id, 0, NULL);
    if (err == XIAOTAI_ERR_NOT_FOUND || err == XIAOTAI_ERR_INVALID_STATE) (void)xiaotai_contacts_refresh();
    return err;
}

static void handle_key(const xiaotai_args_t *args, void *ctx)
{
    if (xiaotai_quiesced()) return;
    const char *intent = ctx;
    const bool main = strcmp(intent, INTENT(MAIN)) == 0;
    const bool gesture = !xiaotai_intent_from_external() && args->value == TERMINAL_GESTURE_VALUE;
    xiaotai_wechat_status_t call;
    xiaotai_wechat_status(&call);
    const terminal_situation_t situation = terminal_situation(&call);
    if (gesture) {
        const bool stale = !terminal.gesture_pending || terminal.gesture_session != call.session ||
            terminal.gesture_situation != situation;
        terminal.gesture_pending = false;
        if (stale) {
            terminal_refuse(intent, XIAOTAI_ERR_INVALID_STATE, true);
            return;
        }
    }
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (main) {
        if (situation == TERMINAL_IDLE && call.ready) result = dial();
        else if (situation == TERMINAL_RINGING) {
            result = xiaotai_wechat_accept(call.session);
            if (gesture && result == XIAOTAI_OK) terminal.key_answer = call.session;
        } else if (situation == TERMINAL_BUSY) {
            ++terminal.blocked;
            key_event(XIAOTAI_EVENT_APP_VOICE_TERMINAL_BLOCKED, EFFECT(BLOCKED), intent, XIAOTAI_OK);
            return;
        }
    } else if (situation == TERMINAL_RINGING) result = xiaotai_wechat_reject(call.session);
    else if (situation == TERMINAL_BUSY) result = xiaotai_wechat_hangup(call.session);

    if (result == XIAOTAI_OK) {
        ++terminal.accepted;
        if (gesture) (void)xiaotai_feedback_fire(main ? EFFECT(TAP) : EFFECT(DOUBLE_TAP), NULL);
    } else terminal_refuse(intent, result, false);
}

static void input_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (strcmp(event->name, XIAOTAI_EVENT_QUIESCE) == 0) {
        if (event->args.value) terminal.gesture_pending = false;
        return;
    }
    if (xiaotai_quiesced()) return;
    if (strcmp(event->name, XIAOTAI_EVENT_INPUT_BEGAN) == 0) {
        if (strcmp(event->args.text, "main") != 0 || !event->args.value) return;
        xiaotai_wechat_status_t call;
        xiaotai_wechat_status(&call);
        terminal.gesture_number = (uint32_t)event->args.value;
        terminal.gesture_pending = true;
        terminal.gesture_session = call.session;
        terminal.gesture_situation = terminal_situation(&call);
    } else {
        const char *const names[] = {INTENT(MAIN), INTENT(END), INTENT(RESET_NETWORK), INTENT(RESET_BINDING)};
        for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
            if (strcmp(event->args.text, names[i]) != 0) continue;
            const bool current = (uint32_t)event->args.value == terminal.gesture_number;
            if (current) terminal.gesture_pending = false;
            if (!current && terminal.gesture_number && terminal_config.quiet_superseded_gesture) {
                ++terminal.superseded;
                xiaotai_eventlog_warn("terminal", "key.refused_superseded", terminal.call_session, XIAOTAI_ERR_NO_MEM);
            } else terminal_refuse(names[i], XIAOTAI_ERR_NO_MEM, false);
            break;
        }
    }
}

static void reset_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    terminal_reset_request(strcmp(ctx, INTENT(RESET_NETWORK)) == 0);
}

xiaotai_err_t terminal_key_init(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(INTENT(MAIN), handle_key, INTENT(MAIN));
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(INTENT(END), handle_key, INTENT(END));
    if (err == XIAOTAI_OK && !terminal_config.no_network_reset) {
        err = xiaotai_intent_handle_ex(INTENT(RESET_NETWORK), reset_intent, INTENT(RESET_NETWORK),
                                       XIAOTAI_INTENT_LOCAL);
    }
    if (err == XIAOTAI_OK && !terminal_config.no_binding_reset) {
        err = xiaotai_intent_handle_ex(INTENT(RESET_BINDING), reset_intent, INTENT(RESET_BINDING),
                                       XIAOTAI_INTENT_LOCAL);
    }
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_INPUT_BEGAN, input_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_INPUT_REJECTED, input_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_QUIESCE, input_event, NULL);
    return err;
}
