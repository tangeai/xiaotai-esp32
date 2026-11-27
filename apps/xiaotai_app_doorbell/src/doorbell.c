// SPDX-License-Identifier: MIT
/* The door station of a household's doorbell without a display
 * (xiaotai/apps/doorbell.h): the key, the round of indoor stations, looks,
 * the door messages, and what is heard and lit meanwhile. */
#include "xiaotai/apps/doorbell.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/feedback.h"
#include "xiaotai/framework.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/view.h"

#define SPACE "xt_doorbell"
#define KEY "peers"
#define KEY_FRONT "front"         /* 1: the doorbell was in front when the device stopped */
#define RECORD_VERSION 1U
#define ID_SIZE (XIAOTAI_CONTACT_ID_MAX + 1U)
#define ROUND_MAX XIAOTAI_APP_DOORBELL_PEERS_MAX   /* indoor stations one press rings at most */
#define KV_TIMEOUT_MS 3000U
#define ANNOUNCE_REPEAT_MS 30000U      /* the binding code is spoken again this often while it is shown */
#define ANNOUNCE_WAIT_MS 500U          /* a sound is still playing: try again this soon */
#define N(a) (sizeof(a) / sizeof((a)[0]))

#define STATE_RINGING "doorbell.ringing"
#define STATE_TALKING "doorbell.talking"
#define STATE_LOOKED "doorbell.looked"
#define EFFECT_DIALING "doorbell.dialing"
#define EFFECT_ANSWERED "doorbell.answered"
#define EFFECT_UNANSWERED "doorbell.unanswered"
#define EFFECT_OPENED "doorbell.opened"
#define EFFECT_ENDED "doorbell.ended"

typedef char peer_id_t[ID_SIZE];

static struct {
    bool initialized;
    xiaotai_app_doorbell_config_t config;
    peer_id_t peers[XIAOTAI_APP_DOORBELL_PEERS_MAX];
    uint8_t peer_count;
    /* This press's round: the stations to ring and the next one. */
    peer_id_t round[ROUND_MAX];
    uint8_t round_count, round_next;
    uint32_t session;       /* the call of this application (dialled in the round, or a look) */
    bool ringing;           /* a round is on (between its calls too) */
    bool look;              /* the call is a look an indoor station placed */
    bool talking;           /* an indoor station talks: it answered, or LOOK 1 */
    bool rang_looker;       /* the visitor rang the station that looks (RING 1) */
    bool timed_out;         /* the round hung up the station that did not answer */
    uint32_t timer;
    uint32_t announce_timer;    /* the next time the binding code is spoken */
    xiaotai_app_doorbell_status_t counts;
    const char *shown;
    bool front;             /* the doorbell is the foreground application */
    bool front_saved;       /* what KEY_FRONT holds */
} bell;

/* ------------------------------------------------------------------ tables */

static const xiaotai_feedback_state_t states[] = {
    {.name = STATE_RINGING, .priority = 3, .pattern = {.kind = XIAOTAI_LIGHT_CHASE, .color = {0, 235, 255},
        .accent = {20, 80, 255}, .period_ms = 75}, .sound = {.name = "ring.outgoing", .gap_ms = 760}},
    {.name = STATE_TALKING, .priority = 2, .pattern = {.kind = XIAOTAI_LIGHT_BREATHE, .color = {28, 255, 74},
        .level_min = 70, .level_max = 175, .period_ms = 1400}},
    {.name = STATE_LOOKED, .priority = 1, .pattern = {.kind = XIAOTAI_LIGHT_BREATHE, .color = {255, 176, 32},
        .level_min = 40, .level_max = 160, .period_ms = 1800}},
};
static const xiaotai_feedback_effect_t effects[] = {
    {.name = EFFECT_DIALING, .sound = {.name = "guide.dialing", .flags = XIAOTAI_FEEDBACK_INTERRUPT}},
    {.name = EFFECT_ANSWERED, .sound = {.name = "cue.success"}},
    {.name = EFFECT_UNANSWERED, .sound = {.name = "cue.warning"},
        .pattern = {.kind = XIAOTAI_LIGHT_BLINK, .color = {255, 138, 0}, .level_min = 55, .level_max = 255,
                    .period_ms = 80}, .duration_ms = 600},
    {.name = EFFECT_OPENED, .sound = {.name = "cue.success"},
        .pattern = {.kind = XIAOTAI_LIGHT_SOLID, .color = {120, 255, 150}, .level_max = 255}, .duration_ms = 1200},
    {.name = EFFECT_ENDED, .sound = {.name = "cue.ended"}},
};
static const xiaotai_feedback_table_t feedback = {
    .states = states, .state_count = N(states), .effects = effects, .effect_count = N(effects),
};

static const xiaotai_interaction_rule_t rules[] = {
    {.role = "main", .gesture = XIAOTAI_GESTURE_CLICK, .count = 1, .intent = XIAOTAI_INTENT_APP_DOORBELL_PRESS},
    {.role = "main", .gesture = XIAOTAI_GESTURE_LONG_PRESS, .intent = XIAOTAI_INTENT_APP_DOORBELL_LEAVE},
};
/* While the doorbell is in front the key is the doorbell's. */
static const xiaotai_interaction_profile_t profile = {.name = "doorbell-key", .rules = rules,
    .rule_count = N(rules), .application = "doorbell", .long_press_ms = 1500};

/* ----------------------------------------------------------------- helpers */

/* The doorbell's page on a device with a display (a call page: what happens
 * now, and what the key does). Without a UI system nothing is drawn. */
static void draw(void)
{
    if (!bell.front) return;
    const bool others = xiaotai_app_count() > 1U;
    xiaotai_view_t view = {.kind = XIAOTAI_VIEW_CALL, .id = "doorbell", .title = "门铃"};
    switch (bell.counts.state) {
    case XIAOTAI_APP_DOORBELL_RINGING:
        view.subtitle = "正在呼叫屋里";
        view.text = "再按一下取消";
        view.emotion = XIAOTAI_EMOTION_DIALING;
        break;
    case XIAOTAI_APP_DOORBELL_TALKING: {
        view.subtitle = "通话中";
        view.text = "按一下挂断";
        xiaotai_call_status_t call;
        xiaotai_call_status(&call);
        if (call.active_since_us) {
            view.timer_mode = XIAOTAI_TIMER_ELAPSED;
            view.timer_s = (int32_t)((xiaotai_time_us() - call.active_since_us) / 1000000);
        }
        break;
    }
    case XIAOTAI_APP_DOORBELL_LOOKED:
        view.subtitle = bell.rang_looker ? "正在呼叫屋里" : "屋里在看门口";
        view.text = bell.rang_looker ? "等屋里接听" : "按一下请屋里接听";
        break;
    default:
        view.subtitle = "按一下呼叫屋里";
        view.text = others ? "长按返回" : NULL;
        break;
    }
    xiaotai_view_show(&view);
}

static void show(void)
{
    const xiaotai_app_doorbell_state_t state = bell.counts.state;
    const char *next = state == XIAOTAI_APP_DOORBELL_RINGING || bell.rang_looker ? STATE_RINGING :
                       state == XIAOTAI_APP_DOORBELL_TALKING ? STATE_TALKING :
                       state == XIAOTAI_APP_DOORBELL_LOOKED ? STATE_LOOKED : NULL;
    if (next != bell.shown) {
        if (bell.shown) (void)xiaotai_feedback_set(bell.shown, false);
        bell.shown = next;
        if (next) (void)xiaotai_feedback_set(next, true);
    }
    draw();
}

static void set_state(xiaotai_app_doorbell_state_t state)
{
    bell.counts.state = state;
    show();
}

static int device_kind(void)
{
    int kind = -1;
    return xiaotai_contacts_kind_find("device", &kind) == XIAOTAI_OK ? kind : -1;
}

static bool is_contact(const char *id)
{
    const int kind = device_kind();
    xiaotai_contact_t contact;
    return id && id[0] && kind >= 0 && xiaotai_contacts_find(id, kind, XIAOTAI_CONTACTS_STALE_OK, &contact) == XIAOTAI_OK;
}

static int peer_index(const char *id)
{
    for (uint8_t i = 0; i < bell.peer_count; ++i)
        if (!strcmp(bell.peers[i], id)) return i;
    return -1;
}

/* Record: version, count, then the ids, each with its length. */
static void save(void)
{
    uint8_t record[2U + XIAOTAI_APP_DOORBELL_PEERS_MAX * ID_SIZE];
    size_t size = 0;
    record[size++] = RECORD_VERSION;
    record[size++] = bell.peer_count;
    for (uint8_t i = 0; i < bell.peer_count; ++i) {
        const size_t length = strlen(bell.peers[i]);
        record[size++] = (uint8_t)length;
        memcpy(record + size, bell.peers[i], length);
        size += length;
    }
    const xiaotai_err_t err = xiaotai_kv_set_async(SPACE, KEY, record, size);
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn("doorbell", "peers.save", 0, err);
}

static void load(void)
{
    uint8_t record[2U + XIAOTAI_APP_DOORBELL_PEERS_MAX * ID_SIZE];
    size_t size = sizeof(record);
    bell.peer_count = 0;
    if (xiaotai_kv_get(SPACE, KEY, record, &size, KV_TIMEOUT_MS) != XIAOTAI_OK || size < 2U ||
        record[0] != RECORD_VERSION || record[1] > XIAOTAI_APP_DOORBELL_PEERS_MAX) return;
    size_t at = 2;
    for (uint8_t i = 0; i < record[1]; ++i) {
        if (at >= size || record[at] == 0 || record[at] >= ID_SIZE || at + 1U + record[at] > size) {
            bell.peer_count = 0;
            return;
        }
        memcpy(bell.peers[i], record + at + 1, record[at]);
        bell.peers[i][record[at]] = '\0';
        at += 1U + record[at];
        bell.peer_count = (uint8_t)(i + 1U);
    }
    bell.counts.peers = bell.peer_count;
}

/* The indoor station that answered or looked is this household's: first in
 * the next round; the oldest one gives way when the list is full. */
static void remember(const char *id)
{
    if (!id || !id[0] || strlen(id) >= ID_SIZE) return;
    const int at = peer_index(id);
    if (at == 0) return;
    const uint8_t count = at > 0 ? (uint8_t)at :
                          bell.peer_count < XIAOTAI_APP_DOORBELL_PEERS_MAX ? bell.peer_count : (uint8_t)(bell.peer_count - 1U);
    memmove(bell.peers[1], bell.peers[0], count * sizeof(peer_id_t));
    (void)snprintf(bell.peers[0], ID_SIZE, "%s", id);
    if (at < 0 && bell.peer_count < XIAOTAI_APP_DOORBELL_PEERS_MAX) ++bell.peer_count;
    bell.counts.peers = bell.peer_count;
    save();
}

static void cancel_timer(void)
{
    if (bell.timer) xiaotai_timer_cancel(bell.timer);
    bell.timer = 0;
}

static uint32_t ring_ms(void)
{
    return bell.config.ring_ms ? bell.config.ring_ms : XIAOTAI_APP_DOORBELL_RING_MS;
}

static void message(uint8_t type, uint8_t value)
{
    const xiaotai_err_t err = xiaotai_call_send_message(bell.session, type, &value, 1);
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn("doorbell", "message", bell.session, err);
}

static void idle(void)
{
    cancel_timer();
    bell.session = 0;
    bell.ringing = bell.look = bell.talking = bell.rang_looker = bell.timed_out = false;
    bell.round_count = bell.round_next = 0;
    set_state(XIAOTAI_APP_DOORBELL_IDLE);
}

/* ------------------------------------------------------------------- round */

static void round_build(void)
{
    bell.round_count = bell.round_next = 0;
    for (uint8_t i = 0; i < bell.peer_count && bell.round_count < ROUND_MAX; ++i)
        if (is_contact(bell.peers[i])) memcpy(bell.round[bell.round_count++], bell.peers[i], ID_SIZE);
    if (bell.round_count) return;
    const int kind = device_kind();
    xiaotai_contact_t contact;
    for (uint16_t i = 0; kind >= 0 && bell.round_count < ROUND_MAX &&
                         xiaotai_contacts_at_kind(kind, i, XIAOTAI_CONTACTS_STALE_OK, &contact) == XIAOTAI_OK; ++i) {
        if (contact.online_known && !contact.online) continue;
        (void)snprintf(bell.round[bell.round_count++], ID_SIZE, "%s", contact.id);
    }
}

static void round_passed(void *ctx);

/* Rings the next station of the round; false when there is none left. */
static bool round_ring(void)
{
    while (bell.round_next < bell.round_count) {
        const char *id = bell.round[bell.round_next++];
        uint32_t session = 0;
        const uint32_t flags = xiaotai_module_present("video_call") ? XIAOTAI_CALL_VIDEO : 0;
        xiaotai_err_t err = xiaotai_call_dial(id, flags, &session);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED && flags) err = xiaotai_call_dial(id, 0, &session);
        if (err != XIAOTAI_OK) {
            xiaotai_eventlog_warn("doorbell", "dial", bell.round_next, err);
            continue;
        }
        bell.session = session;
        bell.timed_out = false;
        ++bell.counts.rings;
        bell.counts.rung = bell.round_next;
        cancel_timer();
        if (xiaotai_timer_start(ring_ms(), round_passed, (void *)(uintptr_t)session, &bell.timer) != XIAOTAI_OK)
            bell.timer = 0;
        return true;
    }
    return false;
}

static void round_over(void)
{
    ++bell.counts.unanswered;
    xiaotai_eventlog_note("doorbell", "unanswered", bell.round_count, 0);
    (void)xiaotai_feedback_fire(EFFECT_UNANSWERED, NULL);
    idle();
}

static void round_passed(void *ctx)
{
    const uint32_t session = (uint32_t)(uintptr_t)ctx;
    bell.timer = 0;
    if (bell.rang_looker && session == bell.session) {        /* nobody inside took the visitor's ring */
        bell.rang_looker = false;
        message(XIAOTAI_APP_DOORBELL_MESSAGE_RING, 0);
        ++bell.counts.unanswered;
        (void)xiaotai_feedback_fire(EFFECT_UNANSWERED, NULL);
        show();
        return;
    }
    if (!bell.ringing || session != bell.session) return;
    bell.timed_out = true;
    if (xiaotai_call_hangup(session) != XIAOTAI_OK) bell.timed_out = false;
}

static void round_start(void)
{
    round_build();
    bell.ringing = true;
    set_state(XIAOTAI_APP_DOORBELL_RINGING);
    (void)xiaotai_feedback_fire(EFFECT_DIALING, NULL);
    if (!round_ring()) round_over();
}

/* --------------------------------------------------------------------- key */

static void on_press(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (xiaotai_quiesced()) return;
    ++bell.counts.presses;
    xiaotai_call_status_t call;
    xiaotai_call_status(&call);
    const bool mine = bell.session && call.session == bell.session && call.state != XIAOTAI_CALL_IDLE;
    if (mine && bell.look && !bell.talking && call.state == XIAOTAI_CALL_ACTIVE) {
        if (bell.rang_looker) return;                          /* already rings inside */
        bell.rang_looker = true;
        message(XIAOTAI_APP_DOORBELL_MESSAGE_RING, 1);
        (void)xiaotai_feedback_fire(EFFECT_DIALING, NULL);
        cancel_timer();
        if (xiaotai_timer_start(ring_ms(), round_passed, (void *)(uintptr_t)bell.session, &bell.timer) != XIAOTAI_OK)
            bell.timer = 0;
        show();
    } else if (mine) {
        bell.ringing = false;                                  /* the visitor ends it: no next station */
        (void)xiaotai_call_hangup(call.session);
    } else if (bell.ringing) {
        idle();
    } else if (call.state == XIAOTAI_CALL_IDLE && !call.releasing) {
        round_start();
    }
}

/* ------------------------------------------------------------------- calls */

static void on_call(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    const xiaotai_call_status_t *call = event->size == sizeof(*call) ? event->data : NULL;
    if (!call) return;
    if (!strcmp(event->name, XIAOTAI_EVENT_CALL_INCOMING)) {
        if (bell.session || bell.ringing || !is_contact(call->peer_id)) {
            ++bell.counts.rejected;
            (void)xiaotai_call_reject(call->session);
            return;
        }
        if (xiaotai_call_accept(call->session) != XIAOTAI_OK) return;
        bell.session = call->session;
        bell.look = true;
        bell.talking = false;
        ++bell.counts.looks;
        remember(call->peer_id);
        set_state(XIAOTAI_APP_DOORBELL_LOOKED);
    } else if (!strcmp(event->name, XIAOTAI_EVENT_CALL_CONNECTED)) {
        if (call->session != bell.session) return;
        if (bell.look) return;
        cancel_timer();
        bell.ringing = false;
        bell.talking = true;
        ++bell.counts.answered;
        remember(call->peer_id);
        /* The doorbell says who calls: an indoor station that did not choose it learns it. */
        message(XIAOTAI_APP_DOORBELL_MESSAGE_HELLO, XIAOTAI_APP_DOORBELL_MODE_BELL);
        (void)xiaotai_feedback_fire(EFFECT_ANSWERED, NULL);
        set_state(XIAOTAI_APP_DOORBELL_TALKING);
    } else if (!strcmp(event->name, XIAOTAI_EVENT_CALL_ENDED)) {
        /* The status of a call that ended has no session any more; there is
         * one call at a time, so while the doorbell holds one the end is its. */
        if (!bell.session || (call->session && call->session != bell.session)) return;
        const xiaotai_call_end_t end = (xiaotai_call_end_t)event->args.value;
        if (bell.ringing && !bell.talking && (end != XIAOTAI_CALL_END_LOCAL || bell.timed_out)) {
            cancel_timer();
            bell.session = 0;
            if (!round_ring()) round_over();
            return;
        }
        if (bell.talking || bell.look) (void)xiaotai_feedback_fire(EFFECT_ENDED, NULL);
        idle();
    }
}

static void on_message(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    const xiaotai_call_message_t *in = event->size == sizeof(*in) ? event->data : NULL;
    if (!in || !bell.session || in->session != bell.session) return;
    if (in->type == XIAOTAI_APP_DOORBELL_MESSAGE_LOOK && in->size >= 1U) {
        bell.talking = in->data[0] != 0;
        if (bell.talking && bell.rang_looker) {
            bell.rang_looker = false;
            cancel_timer();
            ++bell.counts.answered;
            (void)xiaotai_feedback_fire(EFFECT_ANSWERED, NULL);
        }
        set_state(bell.talking ? XIAOTAI_APP_DOORBELL_TALKING : XIAOTAI_APP_DOORBELL_LOOKED);
    } else if (in->type == XIAOTAI_APP_DOORBELL_MESSAGE_OPEN) {
        xiaotai_call_status_t call;
        xiaotai_call_status(&call);
        const bool known = call.session == bell.session && peer_index(call.peer_id) >= 0;
        const xiaotai_err_t result = !known ? XIAOTAI_ERR_NOT_ALLOWED :
                                     bell.config.open_on_word ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
        const int32_t word = result;
        const uint8_t reply[4] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16), (uint8_t)(word >> 24)};
        const xiaotai_err_t err = xiaotai_call_send_message(bell.session, XIAOTAI_APP_DOORBELL_MESSAGE_OPEN_RESULT,
                                                             reply, sizeof(reply));
        if (err != XIAOTAI_OK) xiaotai_eventlog_warn("doorbell", "open.reply", bell.session, err);
        if (result == XIAOTAI_OK) {
            ++bell.counts.opens;
            (void)xiaotai_feedback_fire(EFFECT_OPENED, NULL);
        } else ++bell.counts.refused_opens;
    }
}

/* ------------------------------------------------------------------ setup */

/* Without a display the binding code is heard (xiaotai/modules/binding.h): it
 * is spoken when it appears and every ANNOUNCE_REPEAT_MS while it is shown.
 * A board without sound gets XIAOTAI_ERR_NOT_SUPPORTED; the code is then read
 * with AT+BIND. Beside other applications the setup is theirs (the companion
 * shows and speaks the code), so the doorbell speaks it only when alone. */
static bool code_shown(void)
{
    if (xiaotai_app_count() > 1U) return false;
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    return binding.phase == XIAOTAI_BINDING_WAITING && binding.seconds_left && binding.verification[0];
}

static void announce(void *ctx)
{
    (void)ctx;
    bell.announce_timer = 0;
    if (!code_shown()) return;
    uint32_t next = ANNOUNCE_REPEAT_MS;
    const xiaotai_err_t err = xiaotai_feedback_sound_pending() ? XIAOTAI_ERR_BUSY : xiaotai_binding_announce();
    if (err == XIAOTAI_ERR_NOT_SUPPORTED) return;
    if (err == XIAOTAI_ERR_BUSY) next = ANNOUNCE_WAIT_MS;
    else if (err != XIAOTAI_OK) xiaotai_eventlog_warn("doorbell", "binding.announce", 0, err);
    if (xiaotai_timer_start(next, announce, NULL, &bell.announce_timer) != XIAOTAI_OK) bell.announce_timer = 0;
}

static void on_binding(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!code_shown()) {
        xiaotai_timer_cancel(bell.announce_timer);
        bell.announce_timer = 0;
    } else if (!bell.announce_timer && !strcmp(event->name, XIAOTAI_EVENT_BINDING_CODE)) {
        announce(NULL);
    }
}

/* ------------------------------------------------------------ foreground */

/* With other applications the doorbell is a mode: it acts on calls and the
 * key only while it is in front, and comes back to the front after a restart
 * when the user left it there. Only the user's choice (open, leave) is
 * remembered: another application brought forward to present something (a
 * WeChat call) does not end the mode for the next start. */
static void save_front(bool front)
{
    if (xiaotai_app_count() < 2U || front == bell.front_saved) return;
    const uint8_t value = front ? 1U : 0U;
    if (xiaotai_kv_set_async(SPACE, KEY_FRONT, &value, 1) == XIAOTAI_OK) bell.front_saved = front;
}

/* In front the doorbell presents every device call event ("call.state"
 * too, which another application would otherwise take to the front with it):
 * a call that comes in is answered as a look or turned away, and calls start
 * only from its key. */
bool xiaotai_app_doorbell_claim(const xiaotai_event_t *event)
{
    return bell.front && !strncmp(event->name, "call.", 5);
}

void xiaotai_app_doorbell_foreground(bool foreground)
{
    bell.front = foreground;
    if (foreground) {
        draw();
        return;
    }
    /* Leaving the front stops a round: nobody is at this doorbell any more. */
    if (bell.ringing && !bell.talking) {
        xiaotai_call_status_t call;
        xiaotai_call_status(&call);
        bell.ringing = false;
        if (bell.session && call.session == bell.session && call.state != XIAOTAI_CALL_IDLE)
            (void)xiaotai_call_hangup(bell.session);
        idle();
    }
}

static void on_open(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    const xiaotai_err_t err = xiaotai_app_switch("doorbell");
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn("doorbell", "open", 0, err);
    else save_front(true);
}

static void on_leave(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!bell.front || xiaotai_app_count() < 2U || bell.session || bell.ringing) return;
    const char *home = xiaotai_app_home();
    const xiaotai_err_t err = home && strcmp(home, "doorbell") ? xiaotai_app_switch(home) : xiaotai_app_next();
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn("doorbell", "leave", 0, err);
    else save_front(false);
}

/* -------------------------------------------------------------- lifecycle */

xiaotai_err_t xiaotai_app_doorbell_configure(const xiaotai_app_doorbell_config_t *config)
{
    if (bell.initialized) return XIAOTAI_ERR_INVALID_STATE;
    bell.config = config ? *config : (xiaotai_app_doorbell_config_t){0};
    return XIAOTAI_OK;
}

void xiaotai_app_doorbell_status(xiaotai_app_doorbell_status_t *status)
{
    if (status) *status = bell.counts;
}

static void report(const xiaotai_reply_t *out)
{
    char line[200];
    (void)snprintf(line, sizeof(line), "state=%d peers=%u rung=%u presses=%" PRIu32 " rings=%" PRIu32
                   " answered=%" PRIu32 " unanswered=%" PRIu32 " looks=%" PRIu32 " rejected=%" PRIu32
                   " opens=%" PRIu32 " refused_opens=%" PRIu32,
                   (int)bell.counts.state, (unsigned)bell.counts.peers, (unsigned)bell.counts.rung,
                   bell.counts.presses, bell.counts.rings, bell.counts.answered, bell.counts.unanswered,
                   bell.counts.looks, bell.counts.rejected, bell.counts.opens, bell.counts.refused_opens);
    out->write(out->ctx, line);
}

xiaotai_err_t xiaotai_app_doorbell_init(void)
{
    if (bell.initialized) return XIAOTAI_ERR_INVALID_STATE;
    bell.initialized = true;
    xiaotai_err_t err = xiaotai_feedback_register(&feedback);
    if (err == XIAOTAI_OK) err = xiaotai_interaction_register(&profile);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOORBELL_PRESS, on_press, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOORBELL_OPEN, on_open, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOORBELL_LEAVE, on_leave, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CALL_INCOMING, on_call, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CALL_CONNECTED, on_call, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CALL_ENDED, on_call, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CALL_MESSAGE, on_message, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_BINDING_CODE, on_binding, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_BINDING_CHANGED, on_binding, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("app.doorbell", report);
    return err;
}

/* Queued from start: the framework shows its home application after every
 * start, so the doorbell comes forward after that. */
static void front_again(void *unused)
{
    (void)unused;
    if (!bell.front) (void)xiaotai_app_switch("doorbell");
}

void xiaotai_app_doorbell_start(void)
{
    load();
    uint8_t front = 0;
    size_t size = sizeof(front);
    if (xiaotai_app_count() > 1U && xiaotai_kv_get(SPACE, KEY_FRONT, &front, &size, KV_TIMEOUT_MS) == XIAOTAI_OK &&
        size == 1U && front == 1U) {
        bell.front_saved = true;
        const xiaotai_err_t err = xiaotai_run(front_again, NULL);
        if (err != XIAOTAI_OK) xiaotai_eventlog_warn("doorbell", "front", 0, err);
    }
}

void xiaotai_app_doorbell_reset(void)
{
    cancel_timer();
    xiaotai_timer_cancel(bell.announce_timer);
    memset(&bell, 0, sizeof(bell));
}
