// SPDX-License-Identifier: MIT
// The doorbell without a display (doorbell.c) on the real framework, with
// fakes of the call module, the contacts, feedback, interaction and the
// event log: who is rung and in which order, what the key does in each
// state, looks, the door messages, the household it remembers, and when the
// binding code is spoken.
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/apps/doorbell.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/framework.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"

/* ------------------------------------------------------------------- fakes */

static atomic_llong now_us = 1000000;
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&now_us); }

static xiaotai_call_status_t call;
static char dialled[8][XIAOTAI_CONTACT_ID_MAX + 1];
static unsigned dials, accepts, rejects, hangups;
static uint32_t next_session = 10;
static struct { uint8_t type, size, data[8]; } sent[16];
static unsigned sent_count;
static const char *fired[32];
static unsigned fired_count;
static const char *shown;

void xiaotai_call_status(xiaotai_call_status_t *out) { *out = call; }
xiaotai_err_t xiaotai_call_dial(const char *id, uint32_t flags, uint32_t *session)
{
    assert(xiaotai_on_owner() && !flags && session && dials < 8);
    (void)snprintf(dialled[dials++], sizeof(dialled[0]), "%s", id);
    call = (xiaotai_call_status_t){.state = XIAOTAI_CALL_OUTGOING, .session = next_session++, .outgoing = true};
    (void)snprintf(call.peer_id, sizeof(call.peer_id), "%s", id);
    *session = call.session;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_call_accept(uint32_t session) { assert(session == call.session); ++accepts; call.state = XIAOTAI_CALL_CONNECTING; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_call_reject(uint32_t session) { (void)session; ++rejects; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_call_hangup(uint32_t session) { assert(session == call.session); ++hangups; return XIAOTAI_OK; }
static unsigned hellos;      /* who calls, said beside what the call is about: counted on its own */

xiaotai_err_t xiaotai_call_send_message(uint32_t session, uint8_t type, const void *data, size_t size)
{
    if (type == XIAOTAI_APP_DOORBELL_MESSAGE_HELLO) {
        assert(session == call.session && size == 1 && *(const uint8_t *)data == XIAOTAI_APP_DOORBELL_MODE_BELL);
        ++hellos;
        return XIAOTAI_OK;
    }
    assert(session == call.session && sent_count < 16 && size <= 8);
    sent[sent_count].type = type;
    sent[sent_count].size = (uint8_t)size;
    memcpy(sent[sent_count++].data, data, size);
    return XIAOTAI_OK;
}

/* Contacts: devices a (online), b (offline), c (online). */
static const char *const devices[] = {"a", "b", "c"};
static const bool online[] = {true, false, true};
static unsigned device_count = 3;
xiaotai_err_t xiaotai_contacts_kind_find(const char *name, int *kind)
{
    assert(!strcmp(name, "device"));
    *kind = 1;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_contacts_at_kind(int kind, uint16_t index, uint32_t flags, xiaotai_contact_t *contact)
{
    assert(kind == 1 && flags == XIAOTAI_CONTACTS_STALE_OK);
    if (index >= device_count) return XIAOTAI_ERR_NOT_FOUND;
    *contact = (xiaotai_contact_t){.online_known = true, .online = online[index]};
    (void)snprintf(contact->id, sizeof(contact->id), "%s", devices[index]);
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_contacts_find(const char *id, int kind, uint32_t flags, xiaotai_contact_t *contact)
{
    for (uint16_t i = 0; i < device_count; ++i)
        if (!strcmp(id, devices[i])) return xiaotai_contacts_at_kind(kind, i, flags, contact);
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_feedback_register(const xiaotai_feedback_table_t *table) { assert(table->state_count == 3); return XIAOTAI_OK; }
xiaotai_err_t xiaotai_feedback_set(const char *state, bool active)
{
    if (active) shown = state;
    else if (shown && !strcmp(shown, state)) shown = NULL;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_feedback_fire(const char *effect, const xiaotai_feedback_options_t *options)
{
    (void)options;
    assert(fired_count < 32);
    fired[fired_count++] = effect;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_interaction_register(const xiaotai_interaction_profile_t *profile)
{
    assert(profile->rule_count == 2 && !strcmp(profile->rules[0].intent, XIAOTAI_INTENT_APP_DOORBELL_PRESS) &&
           !strcmp(profile->rules[1].intent, XIAOTAI_INTENT_APP_DOORBELL_LEAVE));
    assert(profile->application && !strcmp(profile->application, "doorbell"));
    return XIAOTAI_OK;
}
void xiaotai_eventlog_emit(const xiaotai_eventlog_event_t *event) { (void)event; }

static xiaotai_binding_status_t binding = {.phase = XIAOTAI_BINDING_BOUND};
static unsigned announces;
static xiaotai_err_t announce_result = XIAOTAI_OK;
static bool sound_pending;
void xiaotai_binding_status(xiaotai_binding_status_t *out) { *out = binding; }
xiaotai_err_t xiaotai_binding_announce(void) { ++announces; return announce_result; }
bool xiaotai_feedback_sound_pending(void) { return sound_pending; }

/* ----------------------------------------------------------------- helpers */

static xiaotai_signal_t done;
static xiaotai_app_doorbell_status_t status;
static void sync_owner(void *ctx) { (void)ctx; xiaotai_app_doorbell_status(&status); xiaotai_signal_give(&done); }
static void barrier(void) { assert(xiaotai_run(sync_owner, NULL) == XIAOTAI_OK); assert(xiaotai_signal_take(&done, 3000)); }
static void owner(void (*fn)(void *), void *ctx) { assert(xiaotai_run(fn, ctx) == XIAOTAI_OK); barrier(); barrier(); }

static void post_press(void *ctx) { (void)ctx; assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_DOORBELL_PRESS, &(xiaotai_args_t){0}) == XIAOTAI_OK); }
static void press(void) { owner(post_press, NULL); }

static void advance_clock(void *ctx) { atomic_fetch_add(&now_us, (int64_t)*(const uint32_t *)ctx * 1000); }
static void advance(uint32_t ms) { owner(advance_clock, &ms); }

static const char *event_name;
static int32_t event_value;
static void emit_job(void *ctx)
{
    (void)ctx;
    xiaotai_args_t args = {.value = event_value};
    size_t length = strlen(call.peer_id);
    if (length >= sizeof(args.text)) length = sizeof(args.text) - 1U;
    memcpy(args.text, call.peer_id, length);
    args.text[length] = 0;
    assert(xiaotai_event_emit(event_name, &args, &call, sizeof(call)) == XIAOTAI_OK);
}
static void emit(const char *name, int32_t value) { event_name = name; event_value = value; owner(emit_job, NULL); }

static void incoming(const char *peer)
{
    call = (xiaotai_call_status_t){.state = XIAOTAI_CALL_INCOMING, .session = next_session++};
    (void)snprintf(call.peer_id, sizeof(call.peer_id), "%s", peer);
    emit(XIAOTAI_EVENT_CALL_INCOMING, 0);
}
static void connected(void) { call.state = XIAOTAI_CALL_ACTIVE; emit(XIAOTAI_EVENT_CALL_CONNECTED, 0); }
/* As the call module: the status of an ended call has no session. */
static void ended(xiaotai_call_end_t end)
{
    call.state = XIAOTAI_CALL_IDLE;
    call.session = 0;
    emit(XIAOTAI_EVENT_CALL_ENDED, (int32_t)end);
}

static xiaotai_call_message_t inbound;
static void message_job(void *ctx)
{
    (void)ctx;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_CALL_MESSAGE, &(xiaotai_args_t){.value = (int32_t)inbound.session},
                              &inbound, sizeof(inbound)) == XIAOTAI_OK);
}
static void receive(uint8_t type, int value)
{
    inbound = (xiaotai_call_message_t){.session = call.session, .type = type, .size = value >= 0 ? 1 : 0,
                                       .data = {(uint8_t)(value >= 0 ? value : 0)}};
    owner(message_job, NULL);
}

static bool fired_last(const char *effect) { return fired_count && !strcmp(fired[fired_count - 1], effect); }
static bool sent_last(uint8_t type, uint8_t value)
{
    return sent_count && sent[sent_count - 1].type == type && sent[sent_count - 1].data[0] == value;
}
static int32_t open_result(void)
{
    assert(sent_count && sent[sent_count - 1].type == XIAOTAI_APP_DOORBELL_MESSAGE_OPEN_RESULT && sent[sent_count - 1].size == 4);
    const uint8_t *d = sent[sent_count - 1].data;
    return (int32_t)((uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24);
}

static void boot(bool open_on_word)
{
    assert(xiaotai_app_doorbell_configure(&(xiaotai_app_doorbell_config_t){.ring_ms = 1000, .open_on_word = open_on_word})
           == XIAOTAI_OK);
    /* The framework keeps these for as long as it runs. */
    static const xiaotai_application_t app = {.name = "doorbell", .init = xiaotai_app_doorbell_init,
        .start = xiaotai_app_doorbell_start};
    static const xiaotai_application_t *const apps[] = {&app};
    static const xiaotai_framework_config_t framework = {.applications = apps, .application_count = 1,
        .queue_depth = 16};
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    barrier();
    assert(xiaotai_app_doorbell_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
}

static void stop(void)
{
    xiaotai_framework_reset();
    xiaotai_app_doorbell_reset();
}

/* ------------------------------------------------------------------- cases */

/* Nobody paired: every online device in turn, each for ring_ms; then the household is who answered. */
static void round_test(void)
{
    boot(false);
    assert(status.state == XIAOTAI_APP_DOORBELL_IDLE && !status.peers);
    press();
    assert(dials == 1 && !strcmp(dialled[0], "a") && status.state == XIAOTAI_APP_DOORBELL_RINGING);
    assert(fired_last("doorbell.dialing") && shown && !strcmp(shown, "doorbell.ringing"));
    advance(999);
    assert(!hangups);
    advance(1);
    assert(hangups == 1);                                   /* a did not answer in time */
    ended(XIAOTAI_CALL_END_LOCAL);
    assert(dials == 2 && !strcmp(dialled[1], "c"));         /* b is offline */
    ended(XIAOTAI_CALL_END_NO_ANSWER);
    assert(status.state == XIAOTAI_APP_DOORBELL_IDLE && status.unanswered == 1 && fired_last("doorbell.unanswered"));
    assert(!shown);

    /* c answers this time: it becomes the household. The door is not opened without a lock. */
    press();
    ended(XIAOTAI_CALL_END_PEER_REJECT);                    /* a rejects: next */
    assert(dials == 4 && !strcmp(dialled[3], "c") && !hellos);
    connected();
    assert(status.state == XIAOTAI_APP_DOORBELL_TALKING && status.answered == 1 && status.peers == 1);
    assert(hellos == 1);                                    /* the answered ring says that the doorbell calls */
    assert(fired_last("doorbell.answered") && !strcmp(shown, "doorbell.talking"));
    advance(5000);
    assert(hangups == 1);                                   /* an answered call is not timed */
    receive(XIAOTAI_APP_DOORBELL_MESSAGE_OPEN, -1);
    assert(open_result() == XIAOTAI_ERR_NOT_SUPPORTED && status.refused_opens == 1);
    ended(XIAOTAI_CALL_END_PEER_HANGUP);
    assert(status.state == XIAOTAI_APP_DOORBELL_IDLE && fired_last("doorbell.ended"));

    /* Now only c is rung; the key stops the ring, and nobody else is rung after it. */
    press();
    assert(dials == 5 && !strcmp(dialled[4], "c"));
    press();
    assert(hangups == 2);
    ended(XIAOTAI_CALL_END_LOCAL);
    assert(dials == 5 && status.state == XIAOTAI_APP_DOORBELL_IDLE);

    /* Remembered across a restart. */
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    stop();
    boot(false);
    xiaotai_app_doorbell_status(&status);
    barrier();
    assert(status.peers == 1);
    press();
    assert(dials == 6 && !strcmp(dialled[5], "c"));
    press();
    ended(XIAOTAI_CALL_END_LOCAL);
    stop();
}

/* An indoor station looks; the visitor rings it with the key; it talks, opens the door. Strangers are turned away. */
static void look_test(void)
{
    boot(true);
    incoming("stranger");
    assert(rejects == 1 && !accepts && status.state == XIAOTAI_APP_DOORBELL_IDLE);
    incoming("a");
    assert(accepts == 1 && status.looks == 1 && status.peers == 1 && status.state == XIAOTAI_APP_DOORBELL_LOOKED);
    connected();
    assert(status.state == XIAOTAI_APP_DOORBELL_LOOKED && !strcmp(shown, "doorbell.looked"));
    receive(XIAOTAI_APP_DOORBELL_MESSAGE_LOOK, 0);
    assert(status.state == XIAOTAI_APP_DOORBELL_LOOKED);

    press();                                                /* the visitor rings the one that looks */
    assert(sent_last(XIAOTAI_APP_DOORBELL_MESSAGE_RING, 1) && !strcmp(shown, "doorbell.ringing"));
    press();
    assert(sent_count == 1);                                /* already ringing inside */
    advance(1000);
    assert(sent_last(XIAOTAI_APP_DOORBELL_MESSAGE_RING, 0) && status.unanswered == 1 && !strcmp(shown, "doorbell.looked"));
    press();
    assert(sent_last(XIAOTAI_APP_DOORBELL_MESSAGE_RING, 1));
    receive(XIAOTAI_APP_DOORBELL_MESSAGE_LOOK, 1);
    assert(status.state == XIAOTAI_APP_DOORBELL_TALKING && status.answered == 1 && fired_last("doorbell.answered"));
    advance(2000);
    assert(sent_count == 3);                                /* the answered ring is not given up */

    receive(XIAOTAI_APP_DOORBELL_MESSAGE_OPEN, -1);
    assert(open_result() == XIAOTAI_OK && status.opens == 1 && fired_last("doorbell.opened"));

    press();                                                /* the key ends the talk */
    assert(hangups == 1);
    ended(XIAOTAI_CALL_END_LOCAL);
    assert(status.state == XIAOTAI_APP_DOORBELL_IDLE && fired_last("doorbell.ended") && !shown);

    /* While a round rings, a look is turned away. */
    press();
    assert(dials == 1 && !strcmp(dialled[0], "a"));
    const uint32_t ringing = call.session;
    incoming("c");
    assert(rejects == 2);
    call.session = ringing;
    call.state = XIAOTAI_CALL_OUTGOING;
    press();
    ended(XIAOTAI_CALL_END_LOCAL);
    stop();
}

/* The binding code is spoken when it appears and every 30 s while it is shown;
 * a sound still playing delays it; a board without sound is not asked again. */
static void binding_job(void *ctx)
{
    assert(xiaotai_event_emit((const char *)ctx, &(xiaotai_args_t){0}, &binding, sizeof(binding)) == XIAOTAI_OK);
}
static void binding_event(const char *name) { owner(binding_job, (void *)name); }

static void setup_test(void)
{
    boot(false);
    binding = (xiaotai_binding_status_t){.phase = XIAOTAI_BINDING_WAITING, .seconds_left = 300};
    (void)snprintf(binding.verification, sizeof(binding.verification), "123456");
    binding_event(XIAOTAI_EVENT_BINDING_CODE);
    assert(announces == 1);
    binding_event(XIAOTAI_EVENT_BINDING_CHANGED);
    assert(announces == 1);                                 /* no second voice for a change */
    advance(29999);
    assert(announces == 1);
    advance(1);
    assert(announces == 2);
    sound_pending = true;
    advance(30000);
    assert(announces == 2);                                 /* a sound plays: wait for it */
    sound_pending = false;
    advance(500);
    assert(announces == 3);

    binding.phase = XIAOTAI_BINDING_BOUND;                  /* bound: no more */
    binding_event(XIAOTAI_EVENT_BINDING_CHANGED);
    advance(60000);
    assert(announces == 3);

    binding.phase = XIAOTAI_BINDING_WAITING;                /* no sound on the board: once */
    announce_result = XIAOTAI_ERR_NOT_SUPPORTED;
    binding_event(XIAOTAI_EVENT_BINDING_CODE);
    assert(announces == 4);
    advance(60000);
    assert(announces == 4);
    stop();
}

/* A mode beside a home application: in front only when opened; there it takes
 * the device calls, elsewhere the home application does; a long press goes
 * back; leaving stops a round; the mode is in front again after a restart. */
static unsigned home_shown;
static const char *last_name;
static void home_foreground(bool shown) { if (shown) ++home_shown; }
static void home_call(const xiaotai_event_t *event, void *ctx) { (void)ctx; last_name = event->name; }
static xiaotai_err_t home_init(void) { return XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CALL_INCOMING, home_call, NULL); }
static void post_job(void *ctx) { assert(xiaotai_intent_post((const char *)ctx, &(xiaotai_args_t){0}) == XIAOTAI_OK); }
static void post(const char *intent) { owner(post_job, (void *)intent); }
static const char *front_app;
static void front_job(void *ctx) { (void)ctx; front_app = xiaotai_app_foreground(); }
static const char *front(void) { owner(front_job, NULL); return front_app; }
static void switch_home_job(void *ctx) { (void)ctx; assert(xiaotai_app_switch("home") == XIAOTAI_OK); }

static void boot_mode(void)
{
    static const char *const handles[] = {XIAOTAI_EVENT_CALL_INCOMING, XIAOTAI_EVENT_CALL_STATE,
                                          XIAOTAI_EVENT_CALL_CONNECTED, XIAOTAI_EVENT_CALL_ENDED, NULL};
    static const xiaotai_application_t bell_app = {.name = "doorbell", .init = xiaotai_app_doorbell_init,
        .start = xiaotai_app_doorbell_start, .foreground = xiaotai_app_doorbell_foreground, .handles = handles,
        .claim = xiaotai_app_doorbell_claim};
    static const xiaotai_application_t home_app = {.name = "home", .init = home_init, .foreground = home_foreground,
        .handles = handles};
    static const xiaotai_application_t *const apps[] = {&bell_app, &home_app};
    static const xiaotai_framework_config_t framework = {.applications = apps, .application_count = 2,
        .home = "home", .queue_depth = 16};
    assert(xiaotai_app_doorbell_configure(&(xiaotai_app_doorbell_config_t){.ring_ms = 1000}) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    barrier();
    barrier();
}

static void mode_test(void)
{
    boot_mode();
    assert(!strcmp(front(), "home"));
    binding = (xiaotai_binding_status_t){.phase = XIAOTAI_BINDING_WAITING, .seconds_left = 300};
    (void)snprintf(binding.verification, sizeof(binding.verification), "123456");
    binding_event(XIAOTAI_EVENT_BINDING_CODE);
    assert(!announces);                                     /* the setup is the home application's */
    binding.phase = XIAOTAI_BINDING_BOUND;
    incoming("a");                                          /* not in front: the home application presents it */
    assert(!accepts && !rejects && last_name && !strcmp(last_name, XIAOTAI_EVENT_CALL_INCOMING));
    call.state = XIAOTAI_CALL_IDLE;
    last_name = NULL;

    post(XIAOTAI_INTENT_APP_DOORBELL_OPEN);
    assert(!strcmp(front(), "doorbell"));
    incoming("a");                                          /* in front: a look */
    assert(accepts == 1 && !last_name && status.state == XIAOTAI_APP_DOORBELL_LOOKED);
    emit(XIAOTAI_EVENT_CALL_STATE, XIAOTAI_CALL_CONNECTING);   /* the doorbell presents its state changes */
    assert(!strcmp(front(), "doorbell") && !last_name);
    post(XIAOTAI_INTENT_APP_DOORBELL_LEAVE);
    assert(!strcmp(front(), "doorbell"));                   /* not while somebody looks */
    ended(XIAOTAI_CALL_END_PEER_HANGUP);
    assert(status.state == XIAOTAI_APP_DOORBELL_IDLE);

    press();                                                /* a round; leaving the front stops it */
    assert(dials == 1 && status.state == XIAOTAI_APP_DOORBELL_RINGING);
    emit(XIAOTAI_EVENT_CALL_STATE, XIAOTAI_CALL_OUTGOING);
    assert(!strcmp(front(), "doorbell") && status.state == XIAOTAI_APP_DOORBELL_RINGING);
    post(XIAOTAI_INTENT_APP_DOORBELL_LEAVE);
    assert(!strcmp(front(), "doorbell"));                   /* the key cancels a round first */
    press();
    assert(hangups == 1);
    ended(XIAOTAI_CALL_END_LOCAL);
    assert(status.state == XIAOTAI_APP_DOORBELL_IDLE);
    owner(switch_home_job, NULL);                           /* another application takes the front: not a leave */
    assert(!strcmp(front(), "home"));
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);

    /* Restart: the doorbell is in front again (it was opened and not left). */
    stop();
    boot_mode();
    assert(!strcmp(front(), "doorbell"));
    post(XIAOTAI_INTENT_APP_DOORBELL_LEAVE);
    assert(!strcmp(front(), "home"));
    assert(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    stop();
    boot_mode();
    assert(!strcmp(front(), "home"));
    stop();
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&done) == XIAOTAI_OK);
    if (argc == 1) round_test();
    else if (argc == 2 && !strcmp(argv[1], "look")) look_test();
    else if (argc == 2 && !strcmp(argv[1], "setup")) setup_test();
    else if (argc == 2 && !strcmp(argv[1], "mode")) mode_test();
    else assert(!"unknown case");
    xiaotai_signal_deinit(&done);
    return 0;
}
