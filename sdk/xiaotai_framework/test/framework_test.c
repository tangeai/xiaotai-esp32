// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_environment_console.py
// Replaces legacy: tests/regression/test_p4_runtime_maintenance.py
// Replaces legacy: tests/regression/test_runtime_init.py
/* Runtime framework on the host port, with fake modules and applications:
 * dependency order, needs, intents (local-only ones too), events with
 * payloads, activity arbitration with module defaults and product rules,
 * attachments and their generation, quiesce and its guards, the orderly
 * restart, settings persistence, the setting store and the quiesced refusal,
 * commands, diagnostics, the presenting application and foreground switches
 * (with the entry reason and the home application), views refused from
 * background applications, application activities, contributions of
 * attachments, the cleanup after a failed start and after a failed
 * allocation at every step of a start. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/view.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

static char s_trace[512];
static void trace(const char *text)
{
    strncat(s_trace, text, sizeof(s_trace) - strlen(s_trace) - 1);
    strncat(s_trace, " ", sizeof(s_trace) - strlen(s_trace) - 1);
}

static xiaotai_signal_t s_sync;
static void signal_sync(void *ctx) { (void)ctx; xiaotai_signal_give(&s_sync); }
/* Wait until everything queued before this point has been handled. */
static int drain(void)
{
    if (xiaotai_run(signal_sync, NULL) != XIAOTAI_OK) return 0;
    return xiaotai_signal_take(&s_sync, 2000);
}
typedef void (*owner_fn_t)(void *ctx);
static void on_owner(owner_fn_t fn, void *ctx)
{
    (void)xiaotai_run(fn, ctx);
    (void)drain();
}

/* A job may enqueue its notification after the first drain was posted. */
static int on_owner_and_drain_events(owner_fn_t fn, void *ctx)
{
    return xiaotai_run(fn, ctx) == XIAOTAI_OK && drain() && drain();
}

/* ------------------------------------------------------------ fake modules */

static int s_toggles, s_toggle_on_owner, s_event_a, s_event_b, s_revoked, s_attach_begin, s_attach_end;
static int s_attach_session;
static uint32_t s_attach_generation;
static char s_event_text[64], s_event_data[16];
static uint32_t s_ai_id, s_call_id;
static xiaotai_err_t s_room_result, s_call_result;

static void on_toggle(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    s_toggles += args->value;
    s_toggle_on_owner = xiaotai_on_owner();
}
static void on_ping_a(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    ++s_event_a;
    strcpy(s_event_text, event->args.text);
    if (event->size) memcpy(s_event_data, event->data, event->size < sizeof(s_event_data) ? event->size : sizeof(s_event_data));
}
static void on_ping_b(const xiaotai_event_t *event, void *ctx) { (void)event; (void)ctx; ++s_event_b; }
static int s_system_revoked;
static bool s_revoked_while_active;
static char s_order[64];
static void on_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)ctx;
    strncat(s_order, "revoked ", sizeof(s_order) - strlen(s_order) - 1);
    if (id == s_ai_id && reason == XIAOTAI_REVOKE_PREEMPTED) {
        ++s_revoked;
        s_revoked_while_active = xiaotai_activity_active();
    }
    if (reason == XIAOTAI_REVOKE_SYSTEM) ++s_system_revoked;
}
static int s_quiesce_on, s_quiesce_off;
static char s_quiesce_reason[XIAOTAI_ARG_TEXT_MAX];
static void on_quiesce(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->args.value) {
        ++s_quiesce_on;
        strcpy(s_quiesce_reason, event->args.text);
    } else {
        ++s_quiesce_off;
    }
}
static void vision_begin(uint32_t id, uint32_t generation, void *ctx)
{
    (void)id; (void)ctx;
    ++s_attach_begin;
    s_attach_generation = generation;
}
static void vision_end(uint32_t id, uint32_t generation, void *ctx)
{
    (void)id; (void)generation; (void)ctx;
    ++s_attach_end;
    strncat(s_order, "end ", sizeof(s_order) - strlen(s_order) - 1);
}
static void vision_session(uint32_t id, uint32_t generation, void *ctx)
{
    (void)id; (void)ctx;
    ++s_attach_session;
    s_attach_generation = generation;
}

static int s_volume_changes;
static void on_volume(const char *name, void *ctx) { (void)name; (void)ctx; ++s_volume_changes; }

static xiaotai_err_t command_ping(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    char line[64];
    snprintf(line, sizeof(line), "PONG %d %s", argc, argc > 1 ? argv[1] : "-");
    reply->write(reply->ctx, line);
    return xiaotai_on_owner() ? XIAOTAI_OK : XIAOTAI_FAIL;
}
static void diag_core(const xiaotai_reply_t *out) { out->write(out->ctx, "core ok"); }

/* A guard that refuses while s_guard_busy is set. */
static int s_guard_busy, s_guard_asked;
static xiaotai_err_t room_guard(const char *reason, void *ctx)
{
    (void)reason; (void)ctx;
    ++s_guard_asked;
    return s_guard_busy ? XIAOTAI_ERR_BUSY : XIAOTAI_OK;
}
static int s_maintenance_takes, s_maintenance_releases;
static xiaotai_err_t take_maintenance_gate(const char *reason, void *ctx)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0) ++s_maintenance_takes;
    return XIAOTAI_OK;
}
static void release_maintenance_gate(const char *reason, void *ctx)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) == 0) ++s_maintenance_releases;
}
static bool s_maintenance_ready = true;
static xiaotai_err_t s_maintenance_error;
static xiaotai_err_t check_maintenance_gate(const char *reason, void *ctx, bool *ready)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) != 0) return XIAOTAI_OK;
    *ready = s_maintenance_ready;
    return s_maintenance_error;
}
static const xiaotai_quiesce_gate_t MAINTENANCE_GATE = {
    .acquire = take_maintenance_gate, .release = release_maintenance_gate, .check = check_maintenance_gate,
};
static int s_forget;
static void on_forget(const xiaotai_args_t *args, void *ctx) { (void)args; (void)ctx; ++s_forget; }

/* Records of slots the framework does not know: it only hands the pointers back. */
static const int VISION_LINK = 1, VISION_CALL_SCENE = 2, VISION_VOIP_SCENE = 3;
static const xiaotai_contribution_t VISION_CONTRIBUTIONS[] = {
    {"link.video", NULL, &VISION_LINK},
    {"profile.scene", "call", &VISION_CALL_SCENE},
    {"profile.scene", "voip", &VISION_VOIP_SCENE},
    {NULL, NULL, NULL},
};

static int s_present_on_owner;
static xiaotai_err_t storage_init(void)
{
    trace("storage");
    s_present_on_owner = xiaotai_module_present("talk") && xiaotai_module_present("network");
    return XIAOTAI_OK;
}
static xiaotai_err_t network_init(void) { trace("network"); return XIAOTAI_OK; }
static xiaotai_err_t talk_init(void)
{
    trace("talk");
    const xiaotai_setting_t volume = {.name = "audio.volume", .key = "volume", .kind = XIAOTAI_SETTING_INT,
                                      .min = 0, .max = 100, .default_value = 60, .changed = on_volume};
    const xiaotai_setting_t wake = {.name = "wake.phrase", .key = "wake", .kind = XIAOTAI_SETTING_TEXT,
                                    .default_text = "hello"};
    const xiaotai_attachment_t vision = {.begin = vision_begin, .end = vision_end, .session = vision_session,
                                         .contributions = VISION_CONTRIBUTIONS};
    if (xiaotai_intent_handle("talk.toggle", on_toggle, NULL) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_intent_handle("talk.toggle", on_toggle, NULL) == XIAOTAI_OK) return XIAOTAI_FAIL; /* one handler */
    if (xiaotai_intent_handle_ex("talk.forget", on_forget, NULL, XIAOTAI_INTENT_LOCAL) != XIAOTAI_OK) {
        return XIAOTAI_FAIL;
    }
    if (xiaotai_event_subscribe("test.ping", on_ping_a, NULL) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_event_subscribe("test.ping", on_ping_b, NULL) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_event_subscribe(XIAOTAI_EVENT_QUIESCE, on_quiesce, NULL) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_setting_register(&volume) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_setting_register(&wake) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_activity_attach("ai", &vision) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_command_register("AT+PING", "reply PONG", command_ping) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_quiesce_guard_register("room", room_guard, NULL) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_quiesce_gate_register("maintenance", &MAINTENANCE_GATE, NULL) != XIAOTAI_OK) return XIAOTAI_FAIL;
    return xiaotai_diag_register("core", diag_core);
}
static void talk_start(void) { trace("talk-start"); }
static xiaotai_err_t camera_init(void) { trace("camera"); return XIAOTAI_OK; }

static const char *const TALK_DEPS[] = {"network", "storage", NULL};
static const char *const NETWORK_DEPS[] = {"storage", NULL};
static const char *const LOOP_A[] = {"loop_b", NULL};
static const char *const LOOP_B[] = {"loop_a", NULL};
static const char *const MISSING[] = {"absent", NULL};
/* The modules' own defaults: call > ai = room = stream = recognition. */
static const xiaotai_priority_t TALK_ACTIVITIES[] = {{"ai", 2}, {NULL, 0}};
static const xiaotai_priority_t NETWORK_ACTIVITIES[] = {{"call", 3}, {"room", 2}, {"stream", 2},
                                                        {"recognition", 2}, {NULL, 0}};

static const xiaotai_module_t TALK = {.name = "talk", .depends = TALK_DEPS, .needs = XIAOTAI_NEED_AUDIO_IN,
                                      .init = talk_init, .start = talk_start, .activities = TALK_ACTIVITIES};
static const xiaotai_module_t NETWORK = {.name = "network", .depends = NETWORK_DEPS, .init = network_init,
                                         .activities = NETWORK_ACTIVITIES};
static const xiaotai_module_t STORAGE = {.name = "storage", .init = storage_init};
static const xiaotai_module_t CAMERA = {.name = "camera", .needs = XIAOTAI_NEED_CAMERA, .init = camera_init};
static const xiaotai_module_t LOOPA = {.name = "loop_a", .depends = LOOP_A};
static const xiaotai_module_t LOOPB = {.name = "loop_b", .depends = LOOP_B};
static const xiaotai_module_t BROKEN = {.name = "broken", .depends = MISSING};

/* --------------------------------------------------------- fake applications */

static int s_home_foreground, s_clock_foreground;
static const char *const HOME_MODULES[] = {"talk", NULL};
static xiaotai_err_t home_init(void) { trace("app-home"); return XIAOTAI_OK; }
static void home_foreground(bool shown) { s_home_foreground += shown ? 1 : -1; }
static void clock_foreground(bool shown) { s_clock_foreground += shown ? 1 : -1; }
static const xiaotai_application_t HOME = {.name = "companion", .modules = HOME_MODULES,
                                           .init = home_init, .foreground = home_foreground};
static const xiaotai_application_t CLOCK = {.name = "alarm", .foreground = clock_foreground};

/* ------------------------------------------------------------ owner actions */

static uint32_t s_generation_seen;
static void begin_ai(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t ai = {.name = "ai", .resources = XIAOTAI_RESOURCE_AUDIO_IN, .revoked = on_revoked};
    (void)xiaotai_activity_begin(&ai, &s_ai_id);
    xiaotai_activity_set_generation(s_ai_id, 77);
    s_generation_seen = xiaotai_activity_generation(s_ai_id);
}
static void begin_room(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t room = {.name = "room"};
    uint32_t id = 0;
    s_room_result = xiaotai_activity_begin(&room, &id);
}
static void begin_call(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t call = {.name = "call"};
    s_call_result = xiaotai_activity_begin(&call, &s_call_id);
}
static void end_call(void *ctx) { (void)ctx; xiaotai_activity_end(s_call_id); }

/* Timers: A and C run in the order they are due, B is cancelled before it is
 * due, and A starts D from its handler. */
static char s_timer_trace[32];
static uint32_t s_timer_a, s_timer_b, s_timer_c, s_timer_d;
static xiaotai_err_t s_timer_result, s_timer_off_owner;
static int64_t s_timer_started_us, s_timer_a_us;
static void timer_note(const char *name) { strncat(s_timer_trace, name, sizeof(s_timer_trace) - strlen(s_timer_trace) - 1); }
static void timer_d(void *ctx) { timer_note(ctx); }
static void timer_a(void *ctx)
{
    timer_note(ctx);
    s_timer_a_us = xiaotai_time_us();
    if (xiaotai_timer_start(0, timer_d, "D", &s_timer_d) != XIAOTAI_OK) s_timer_result = XIAOTAI_FAIL;
}
static void timer_plain(void *ctx) { timer_note(ctx); }
static void start_timers(void *ctx)
{
    (void)ctx;
    s_timer_started_us = xiaotai_time_us();
    s_timer_result = xiaotai_timer_start(60, timer_plain, "C", &s_timer_c);
    if (s_timer_result == XIAOTAI_OK) s_timer_result = xiaotai_timer_start(30, timer_a, "A", &s_timer_a);
    if (s_timer_result == XIAOTAI_OK) s_timer_result = xiaotai_timer_start(40, timer_plain, "B", &s_timer_b);
    if (s_timer_result == XIAOTAI_OK && xiaotai_timer_start(1, NULL, NULL, &s_timer_b) != XIAOTAI_ERR_INVALID_ARG) {
        s_timer_result = XIAOTAI_FAIL;
    }
    xiaotai_timer_cancel(s_timer_b);
    xiaotai_timer_cancel(0);
}
static void fill_timers(void *ctx)
{
    (void)ctx;
    uint32_t ids[64];
    size_t count = 0;
    while (count < 64 && xiaotai_timer_start(60000, timer_plain, "X", &ids[count]) == XIAOTAI_OK) ++count;
    uint32_t extra = 0;
    s_timer_result = count > 0 && count < 64 && xiaotai_timer_start(1, timer_plain, "X", &extra) == XIAOTAI_ERR_NO_MEM
                         ? XIAOTAI_OK : XIAOTAI_FAIL;
    for (size_t i = 0; i < count; ++i) xiaotai_timer_cancel(ids[i]);
    if (xiaotai_timer_start(60000, timer_plain, "X", &extra) != XIAOTAI_OK) s_timer_result = XIAOTAI_FAIL;
    xiaotai_timer_cancel(extra);
}

/* Quiesce: revokes the current activity, refuses new ones, one holder. */
static uint32_t s_ticket;
static xiaotai_err_t s_quiesce_result, s_quiesce_again, s_quiesce_begin_activity, s_quiesced_setting;
static void quiesce(void *ctx)
{
    (void)ctx;
    uint32_t other = 0;
    s_quiesce_result = xiaotai_quiesce_begin("update", &s_ticket);
    s_quiesce_again = xiaotai_quiesce_begin("restart", &other);
    const xiaotai_activity_t call = {.name = "call"};
    uint32_t id = 0;
    s_quiesce_begin_activity = xiaotai_activity_begin(&call, &id);
    s_quiesced_setting = xiaotai_setting_set_int("audio.volume", 10);
    xiaotai_quiesce_end(s_ticket + 1); /* not the holder: ignored */
}
static void unquiesce(void *ctx) { (void)ctx; xiaotai_quiesce_end(s_ticket); }
static void release_quiesce_ticket(void *ctx) { xiaotai_quiesce_end(*(const uint32_t *)ctx); }
static struct { xiaotai_err_t result; bool ready; } s_check;
static void check_quiesce_ticket(void *ctx)
{
    s_check.result = xiaotai_quiesce_check(*(const uint32_t *)ctx, &s_check.ready);
}
static void begin_stream(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t stream = {.name = "stream", .revoked = on_revoked};
    uint32_t id = 0;
    s_room_result = xiaotai_activity_begin(&stream, &id);
}
static xiaotai_err_t s_set_result;
static void set_volume_75(void *ctx) { (void)ctx; s_set_result = xiaotai_setting_set_int("audio.volume", 75); }
static void set_volume_bad(void *ctx) { (void)ctx; s_set_result = xiaotai_setting_set_int("audio.volume", 101); }
static void set_wake_text(void *ctx) { s_set_result = xiaotai_setting_set_text("wake.phrase", ctx); }
static void switch_clock(void *ctx) { (void)ctx; (void)xiaotai_app_switch("alarm"); }
static void quiesce_guarded(void *ctx)
{
    (void)ctx;
    s_quiesce_result = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, &s_ticket);
}

static char s_reply[512];
static void reply_write(void *ctx, const char *text)
{
    (void)ctx;
    strncat(s_reply, text, sizeof(s_reply) - strlen(s_reply) - 1);
    strncat(s_reply, "\n", sizeof(s_reply) - strlen(s_reply) - 1);
}

/* ---------------------------------------------------- the product's rules */

/* Rank call 3 > ai = recognition 2 > room = stream 1; recognition never
 * preempts room or stream; ai preempts room although the room's rank is
 * raised here. */
static const xiaotai_priority_t RULE_PRIORITIES[] = {{"call", 3}, {"ai", 2}, {"recognition", 2}, {"room", 1},
                                                     {"stream", 1}, {NULL, 0}};
static const xiaotai_preemption_t RULES[] = {
    {"recognition", "room", false}, {"recognition", "stream", false}, {NULL, NULL, false},
};
static xiaotai_err_t s_rule_results[4];
static void rule_sequence(void *ctx)
{
    (void)ctx;
    uint32_t room = 0, recognition = 0, ai = 0, again = 0;
    const xiaotai_activity_t room_activity = {.name = "room"};
    const xiaotai_activity_t recognition_activity = {.name = "recognition"};
    const xiaotai_activity_t ai_activity = {.name = "ai"};
    s_rule_results[0] = xiaotai_activity_begin(&room_activity, &room);
    s_rule_results[1] = xiaotai_activity_begin(&recognition_activity, &recognition);  /* rule: BUSY */
    s_rule_results[2] = xiaotai_activity_begin(&ai_activity, &ai);                    /* rank: preempts */
    s_rule_results[3] = xiaotai_activity_begin(&recognition_activity, &again);        /* equal: BUSY */
    xiaotai_activity_end(ai);
}

/* ---------------------------------------------------------------- restart */

static _Atomic int s_restarted;
static atomic_bool s_restart_off_owner;
/* Runs on the framework's restart task, never on the owner task. */
static void fake_restart(void)
{
    atomic_store(&s_restart_off_owner, !xiaotai_on_owner());
    atomic_fetch_add(&s_restarted, 1);
}
static int s_restarting;
static char s_restarting_reason[XIAOTAI_ARG_TEXT_MAX];
static void on_restarting(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    ++s_restarting;
    strcpy(s_restarting_reason, event->args.text);
}
static uint32_t s_restart_ticket;
static xiaotai_err_t s_restart_result[3];
static void restart_takeover(void *ctx)
{
    (void)ctx;
    s_restart_result[0] = xiaotai_quiesce_begin("binding", &s_restart_ticket);
    s_restart_result[1] = xiaotai_restart_request("binding", s_restart_ticket + 1);  /* not the holder */
    s_restart_result[2] = xiaotai_restart_request("binding", s_restart_ticket);
    xiaotai_quiesce_end(s_restart_ticket);   /* taken over: ignored */
}
static xiaotai_err_t restart_module_init(void)
{
    return xiaotai_event_subscribe(XIAOTAI_EVENT_RESTARTING, on_restarting, NULL);
}
static const xiaotai_module_t RESTARTER = {.name = "restarter", .init = restart_module_init};

/* ------------------------------------------------------ presenting events */

/* WT99-like: "door" first presents only calls from its station (text
 * "station"); "companion" presents every call and every AI state. */
static int s_door_calls, s_companion_calls, s_door_shown, s_companion_shown, s_door_any;
static xiaotai_app_entry_t s_door_entry, s_companion_entry;
static int s_views;
static unsigned s_page_calls;
static int s_page_direction;
static bool s_page_blocked;
static xiaotai_err_t s_page_result;
static void count_view(const xiaotai_view_t *view, void *ctx) { (void)view; (void)ctx; ++s_views; }
static void show_view(void)
{
    xiaotai_view_t view;
    memset(&view, 0, sizeof(view));
    view.kind = XIAOTAI_VIEW_NOTICE;
    xiaotai_view_show(&view);
}
static void door_on_call(const xiaotai_event_t *event, void *ctx) { (void)event; (void)ctx; ++s_door_calls; }
/* In the background when net.up arrives: its view must not be drawn. */
static void door_on_any(const xiaotai_event_t *event, void *ctx)
{
    (void)event; (void)ctx;
    ++s_door_any;
    show_view();
}
static void door_on_page(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    s_page_result = xiaotai_app_page_step(event->args.value);
    show_view();  /* The dispatch must return to the background caller. */
}
static void companion_on_call(const xiaotai_event_t *event, void *ctx)
{
    (void)event; (void)ctx;
    ++s_companion_calls;
    show_view();
}
static xiaotai_err_t door_init(void)
{
    xiaotai_err_t err = xiaotai_event_subscribe("call.incoming", door_on_call, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe("net.up", door_on_any, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe("fixture.page", door_on_page, NULL);
    return err;
}
static xiaotai_err_t companion_init(void) { return xiaotai_event_subscribe("call.incoming", companion_on_call, NULL); }
static void door_foreground(bool shown)
{
    s_door_shown += shown ? 1 : -1;
    if (shown) s_door_entry = xiaotai_app_entry();
}
static void companion_foreground(bool shown)
{
    s_companion_shown += shown ? 1 : -1;
    if (shown) s_companion_entry = xiaotai_app_entry();
}
static xiaotai_err_t companion_page_step(int direction)
{
    ++s_page_calls;
    s_page_direction = direction;
    if (s_page_blocked) return XIAOTAI_ERR_BUSY;
    show_view();
    return XIAOTAI_OK;
}
static bool door_claim(const xiaotai_event_t *event) { return strcmp(event->args.text, "station") == 0; }
static const char *const DOOR_HANDLES[] = {"call.incoming", NULL};
static const char *const COMPANION_HANDLES[] = {"call.incoming", "ai_talk.state", NULL};
static const xiaotai_application_t DOOR = {.name = "door", .init = door_init, .foreground = door_foreground,
                                           .handles = DOOR_HANDLES, .claim = door_claim};
/* The companion rings an alarm of its own, ranked above calls by its manifest. */
static const xiaotai_priority_t COMPANION_ACTIVITIES[] = {{"alarm", 5}, {NULL, 0}};
static const xiaotai_application_t COMPANION = {.name = "companion", .init = companion_init,
                                                .foreground = companion_foreground, .handles = COMPANION_HANDLES,
                                                .activities = COMPANION_ACTIVITIES, .page_step = companion_page_step};
static xiaotai_err_t s_return_result, s_step_result;
static void app_return(void *ctx) { (void)ctx; s_return_result = xiaotai_app_return(); }
static void app_next(void *ctx) { (void)ctx; s_step_result = xiaotai_app_next(); }
static void app_previous(void *ctx) { (void)ctx; s_step_result = xiaotai_app_previous(); }
static void page_step(void *ctx) { s_page_result = xiaotai_app_page_step(*(const int *)ctx); }
static xiaotai_err_t s_page_quiesce;
static void page_step_during_quiesce(void *ctx)
{
    uint32_t ticket = 0;
    s_page_quiesce = xiaotai_quiesce_begin("page-fixture", &ticket);
    page_step(ctx);
    if (ticket) xiaotai_quiesce_end(ticket);
}
static xiaotai_err_t s_alarm_result;
static void ring_over_call(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t call = {.name = "call", .revoked = on_revoked};
    const xiaotai_activity_t alarm = {.name = "alarm"};
    uint32_t call_id = 0, alarm_id = 0;
    s_call_result = xiaotai_activity_begin(&call, &call_id);
    s_alarm_result = xiaotai_activity_begin(&alarm, &alarm_id);
    xiaotai_activity_end(alarm_id);
}
/* Contributions are answered for the slot and scene asked for. */
static size_t s_contributed[4];
static const void *s_contribution[2];
static void collect(void *ctx)
{
    (void)ctx;
    s_contributed[0] = xiaotai_activity_contributions("ai", "link.video", "call", s_contribution, 2);
    s_contributed[1] = xiaotai_activity_contributions("ai", "profile.scene", "voip", s_contribution + 1, 1);
    s_contributed[2] = xiaotai_activity_contributions("ai", "profile.scene", NULL, NULL, 0);
    s_contributed[3] = xiaotai_activity_contributions("call", "link.video", "call", NULL, 0);
}

/* ----------------------------------------------------- a failed start */

static atomic_bool s_fail_owner_start;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (config && config->name && strcmp(config->name, "xiaotai") == 0 && atomic_load(&s_fail_owner_start)) {
        return XIAOTAI_ERR_NO_MEM;
    }
    return __real_xiaotai_task_create(config, task);
}

static xiaotai_err_t registers_init(void) { return xiaotai_intent_handle("early.intent", on_toggle, NULL); }
static xiaotai_err_t failing_init(void) { return XIAOTAI_ERR_NO_MEM; }
static const char *const FAILING_DEPS[] = {"registers", NULL};
static const xiaotai_module_t REGISTERS = {.name = "registers", .init = registers_init};
static const xiaotai_module_t FAILING = {.name = "failing", .depends = FAILING_DEPS, .init = failing_init};

int framework_product_preferences_test(void);

int main(void)
{
    CHECK(xiaotai_signal_init(&s_sync) == XIAOTAI_OK);
    const uint32_t all = XIAOTAI_NEED_AUDIO_IN | XIAOTAI_NEED_AUDIO_OUT | XIAOTAI_NEED_DISPLAY;

    /* Failures are reported, never half-started. */
    const xiaotai_module_t *broken[] = {&BROKEN};
    xiaotai_framework_config_t config = {.modules = broken, .module_count = 1, .available_needs = all};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_ERR_NOT_FOUND);
    xiaotai_framework_reset();
    const xiaotai_module_t *loop[] = {&LOOPA, &LOOPB};
    config = (xiaotai_framework_config_t){.modules = loop, .module_count = 2, .available_needs = all};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_framework_reset();
    const xiaotai_module_t *camera[] = {&CAMERA};
    config = (xiaotai_framework_config_t){.modules = camera, .module_count = 1, .available_needs = all};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_ERR_NOT_SUPPORTED);
    xiaotai_framework_reset();
    const xiaotai_application_t *needs_talk[] = {&HOME};
    config = (xiaotai_framework_config_t){.applications = needs_talk, .application_count = 1, .available_needs = all};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_ERR_NOT_FOUND);
    xiaotai_framework_reset();

    /* A failed init cleans up by itself: the registrations of the modules that
     * did initialize are gone and a new start needs no reset. */
    const xiaotai_module_t *failing[] = {&FAILING, &REGISTERS};
    config = (xiaotai_framework_config_t){.modules = failing, .module_count = 2, .available_needs = all};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_ERR_NO_MEM);
    CHECK(!xiaotai_intent_exists("early.intent") && !xiaotai_module_present("registers"));
    CHECK(xiaotai_run(signal_sync, NULL) == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_module_t *registers[] = {&REGISTERS};
    config = (xiaotai_framework_config_t){.modules = registers, .module_count = 1, .available_needs = all};
    xiaotai_mem_host_clear();
    atomic_store(&s_fail_owner_start, true);
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_ERR_NO_MEM);
    atomic_store(&s_fail_owner_start, false);
    CHECK(!xiaotai_intent_exists("early.intent") && !xiaotai_module_present("registers"));
    CHECK(xiaotai_run(signal_sync, NULL) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_mem_host_outstanding() == 0);
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    CHECK(xiaotai_intent_exists("early.intent") && xiaotai_intent_exists(XIAOTAI_INTENT_SYSTEM_RESTART));
    xiaotai_framework_reset();

    /* A product: three modules listed out of order, two applications. */
    const xiaotai_module_t *modules[] = {&TALK, &NETWORK, &STORAGE};
    const xiaotai_application_t *applications[] = {&CLOCK, &HOME};
    config = (xiaotai_framework_config_t){.modules = modules, .module_count = 3, .applications = applications,
                                         .application_count = 2, .home = "companion", .available_needs = all};
    s_trace[0] = '\0';
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    CHECK(strcmp(s_trace, "storage network talk app-home talk-start ") == 0);
    CHECK(s_present_on_owner);   /* answered during the first init */
    CHECK(xiaotai_module_present("talk") && !xiaotai_module_present("camera"));
    CHECK(s_home_foreground == 1 && s_clock_foreground == 0);

    /* Intents run on the owner task; unknown ones are rejected at the door;
     * a local-only intent is refused over an open channel. */
    xiaotai_args_t args = {.value = 2};
    CHECK(xiaotai_intent_post("talk.toggle", &args) == XIAOTAI_OK);
    CHECK(xiaotai_intent_post("call.accept", NULL) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(xiaotai_intent_post_external("talk.forget", NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    CHECK(xiaotai_intent_post_external("talk.toggle", &args) == XIAOTAI_OK);
    CHECK(xiaotai_intent_post("talk.forget", NULL) == XIAOTAI_OK);
    CHECK(drain());
    CHECK(s_toggles == 4 && s_toggle_on_owner && s_forget == 1);

    /* Events reach every subscriber with a copied payload; unsubscribed ones are dropped. */
    args = (xiaotai_args_t){.value = 1};
    strcpy(args.text, "hello");
    char payload[16] = "payload";
    CHECK(xiaotai_event_emit("test.ping", &args, payload, sizeof(payload)) == XIAOTAI_OK);
    memset(payload, 0, sizeof(payload));
    CHECK(xiaotai_event_emit("nobody.listens", NULL, NULL, 0) == XIAOTAI_OK);
    CHECK(drain());
    CHECK(s_event_a == 1 && s_event_b == 1 && strcmp(s_event_text, "hello") == 0 &&
          strcmp(s_event_data, "payload") == 0);

    /* Activities, by the modules' declared defaults: equal priority is busy; a
     * call preempts AI, its attachment ends before the module hears it. */
    CHECK(!xiaotai_activity_active());
    on_owner(begin_ai, NULL);
    CHECK(xiaotai_activity_active());
    CHECK(s_ai_id != 0 && s_attach_begin == 1 && s_attach_session == 1 && s_attach_generation == 77);
    CHECK(s_generation_seen == 77);
    on_owner(begin_room, NULL);
    CHECK(s_room_result == XIAOTAI_ERR_BUSY);
    s_order[0] = '\0';
    on_owner(begin_call, NULL);
    CHECK(s_call_result == XIAOTAI_OK && s_revoked == 1 && s_attach_end == 1);
    CHECK(xiaotai_activity_active() && s_revoked_while_active);
    CHECK(strcmp(s_order, "end revoked ") == 0);
    on_owner(begin_room, NULL);
    CHECK(s_room_result == XIAOTAI_ERR_BUSY);
    on_owner(end_call, NULL);
    CHECK(!xiaotai_activity_active());
    on_owner(begin_room, NULL);
    CHECK(s_room_result == XIAOTAI_OK);
    on_owner(collect, NULL);
    CHECK(s_contributed[0] == 1 && s_contribution[0] == &VISION_LINK);
    CHECK(s_contributed[1] == 1 && s_contribution[1] == &VISION_VOIP_SCENE);
    CHECK(s_contributed[2] == 0 && s_contributed[3] == 0);

    /* Quiesce: the room is revoked for the system, nothing may begin and no
     * setting may change until the holder lets go, and subscribers hear both
     * changes. */
    CHECK(!xiaotai_quiesced());
    on_owner(quiesce, NULL);
    CHECK(s_quiesce_result == XIAOTAI_OK && s_ticket != 0 && s_quiesce_again == XIAOTAI_ERR_BUSY);
    CHECK(s_quiesce_begin_activity == XIAOTAI_ERR_BUSY && xiaotai_quiesced());
    CHECK(s_quiesced_setting == XIAOTAI_ERR_BUSY);
    CHECK(xiaotai_activity_current(NULL) == 0 && !xiaotai_activity_active());
    CHECK(drain());
    CHECK(s_quiesce_on == 1 && s_quiesce_off == 0 && strcmp(s_quiesce_reason, "update") == 0);
    on_owner(unquiesce, NULL);
    CHECK(drain());
    CHECK(!xiaotai_quiesced() && s_quiesce_off == 1);
    on_owner(begin_stream, NULL);
    CHECK(s_room_result == XIAOTAI_OK);
    on_owner(quiesce, NULL);
    CHECK(s_quiesce_result == XIAOTAI_OK && s_system_revoked == 1);
    on_owner(unquiesce, NULL);

    /* A guard's refusal is returned and nothing is quiesced. */
    s_guard_busy = 1;
    on_owner(quiesce_guarded, NULL);
    CHECK(s_quiesce_result == XIAOTAI_ERR_BUSY && !xiaotai_quiesced() && s_guard_asked > 0);
    s_guard_busy = 0;
    on_owner(quiesce_guarded, NULL);
    CHECK(s_quiesce_result == XIAOTAI_OK && xiaotai_quiesced());
    uint32_t expired_maintenance = s_ticket;
    CHECK(on_owner_and_drain_events(unquiesce, NULL) && !xiaotai_quiesced());

    /* Like the legacy maintenance owner, a delayed release for the first
     * maintenance must not release the next maintenance or notify subscribers. */
    const int starts = s_quiesce_on, releases = s_quiesce_off;
    CHECK(s_maintenance_takes == 1 && s_maintenance_releases == 1);
    CHECK(on_owner_and_drain_events(quiesce_guarded, NULL));
    CHECK(s_quiesce_result == XIAOTAI_OK && s_ticket != 0 && s_ticket != expired_maintenance);
    CHECK(s_quiesce_on == starts + 1 && s_quiesce_off == releases && s_maintenance_takes == 2);
    bool ready = true;
    CHECK(xiaotai_quiesce_check(s_ticket, &ready) == XIAOTAI_ERR_INVALID_STATE && !ready);
    CHECK(on_owner_and_drain_events(check_quiesce_ticket, &expired_maintenance));
    CHECK(s_check.result == XIAOTAI_ERR_INVALID_STATE && !s_check.ready);
    s_maintenance_ready = false;
    CHECK(on_owner_and_drain_events(check_quiesce_ticket, &s_ticket));
    CHECK(s_check.result == XIAOTAI_OK && !s_check.ready && xiaotai_quiesced());
    s_maintenance_error = XIAOTAI_ERR_TIMEOUT;
    CHECK(on_owner_and_drain_events(check_quiesce_ticket, &s_ticket));
    CHECK(s_check.result == XIAOTAI_ERR_TIMEOUT && !s_check.ready && xiaotai_quiesced());
    s_maintenance_ready = true;
    s_maintenance_error = XIAOTAI_OK;
    CHECK(on_owner_and_drain_events(check_quiesce_ticket, &s_ticket));
    CHECK(s_check.result == XIAOTAI_OK && s_check.ready && xiaotai_quiesced());
    CHECK(on_owner_and_drain_events(release_quiesce_ticket, &expired_maintenance));
    CHECK(xiaotai_quiesced() && s_quiesce_off == releases && s_maintenance_releases == 1);
    CHECK(on_owner_and_drain_events(unquiesce, NULL));
    CHECK(!xiaotai_quiesced() && s_quiesce_off == releases + 1 && s_maintenance_releases == 2);
    CHECK(on_owner_and_drain_events(release_quiesce_ticket, &s_ticket));
    CHECK(!xiaotai_quiesced() && s_quiesce_off == releases + 1 && s_maintenance_releases == 2);

    /* Generations: never 0, never repeated, from any task. */
    const uint32_t first = xiaotai_generation_next();
    const uint32_t second = xiaotai_generation_next();
    CHECK(first != 0 && second != 0 && first != second);

    /* Timers run on the owner task in the order they are due, never early;
     * a cancelled one never runs; the table is bounded and slots are reused. */
    uint32_t off_owner = 0;
    s_timer_off_owner = xiaotai_timer_start(1, timer_plain, "X", &off_owner);
    CHECK(s_timer_off_owner == XIAOTAI_ERR_INVALID_STATE && off_owner == 0);
    on_owner(start_timers, NULL);
    CHECK(s_timer_result == XIAOTAI_OK && s_timer_a && s_timer_c && s_timer_a != s_timer_c);
    xiaotai_delay_ms(200);
    CHECK(drain());
    CHECK(strcmp(s_timer_trace, "ADC") == 0);
    CHECK(s_timer_result == XIAOTAI_OK && s_timer_a_us - s_timer_started_us >= 30000);
    on_owner(fill_timers, NULL);
    CHECK(s_timer_result == XIAOTAI_OK);

    /* Settings: validated, persisted in the background, reloaded after a restart. */
    int32_t volume = 0;
    CHECK(xiaotai_setting_get_int("audio.volume", &volume) == XIAOTAI_OK && volume == 60);
    on_owner(set_volume_bad, NULL);
    CHECK(s_set_result == XIAOTAI_ERR_INVALID_ARG);
    on_owner(set_volume_75, NULL);
    CHECK(s_set_result == XIAOTAI_OK && s_volume_changes == 1);
    char text[XIAOTAI_SETTING_TEXT_MAX], full_text[XIAOTAI_SETTING_TEXT_MAX];
    CHECK(xiaotai_setting_get_text("wake.phrase", text, sizeof(text)) == XIAOTAI_OK && strcmp(text, "hello") == 0);
    memset(full_text, 'h', sizeof(full_text) - 1U);
    full_text[sizeof(full_text) - 1U] = '\0';
    CHECK(strlen(full_text) == 64U);
    on_owner(set_wake_text, full_text);
    CHECK(s_set_result == XIAOTAI_OK);
    CHECK(xiaotai_setting_get_text("wake.phrase", text, sizeof(text)) == XIAOTAI_OK && !strcmp(text, full_text));

    /* Commands run on the owner task and reply through the caller's writer. */
    const xiaotai_reply_t reply = {.write = reply_write};
    CHECK(xiaotai_command_run("AT+PING 7", &reply, 1000) == XIAOTAI_OK);
    CHECK(strcmp(s_reply, "PONG 2 7\n") == 0);
    CHECK(xiaotai_command_run("AT+NOPE", &reply, 1000) == XIAOTAI_ERR_NOT_FOUND);
    /* Words are separated by spaces and tabs only: a comma stays in its word. */
    s_reply[0] = '\0';
    CHECK(xiaotai_command_run("  AT+PING\thttp://host/a,b  x ", &reply, 1000) == XIAOTAI_OK);
    CHECK(strcmp(s_reply, "PONG 3 http://host/a,b\n") == 0);
    CHECK(xiaotai_command_run("AT+PING,7", &reply, 1000) == XIAOTAI_ERR_NOT_FOUND);
    /* A line that does not fit never runs as a shorter command. */
    CHECK(xiaotai_command_run("AT+PING 1 2 3 4 5 6 7 8 9 10 11 12", &reply, 1000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_command_run("   ", &reply, 1000) == XIAOTAI_ERR_INVALID_ARG);
    static char long_line[XIAOTAI_COMMAND_LINE_MAX + 1];
    memset(long_line, 'A', sizeof(long_line) - 1);
    memcpy(long_line, "AT+PING ", 8);
    CHECK(xiaotai_command_run(long_line, &reply, 1000) == XIAOTAI_ERR_INVALID_SIZE);
    long_line[XIAOTAI_COMMAND_LINE_MAX - 1] = '\0';
    s_reply[0] = '\0';
    CHECK(xiaotai_command_run(long_line, &reply, 1000) == XIAOTAI_OK);
    CHECK(strncmp(s_reply, "PONG 2 AAAA", 11) == 0);
    /* Diagnostics: the framework's own section comes first. */
    s_reply[0] = '\0';
    xiaotai_diag_report("core", &reply);
    CHECK(strcmp(s_reply, "core ok\n") == 0);
    s_reply[0] = '\0';
    xiaotai_diag_report(NULL, &reply);
    CHECK(strncmp(s_reply, "owner_stack=6144 queue_depth=32 ", 32) == 0);
    CHECK(strstr(s_reply, "foreground=companion activity=- view_refused=0\n"));
    CHECK(strstr(s_reply, "\n+FRAMEWORK-RESTART: quiesced=0 reason=- restart=0 holds=0 guard_refusals=1 "
                          "last_guard=room restart_forced=0 restart_inline=0 settings_refused=2\n"));
    CHECK(strstr(s_reply, "\nsupported=0 count=0\n") && strstr(s_reply, "\ncore ok\n"));

    /* Foreground switch notifies both applications. */
    on_owner(switch_clock, NULL);
    CHECK(s_home_foreground == 0 && s_clock_foreground == 1);

    /* Restart: the stored volume survives. */
    xiaotai_delay_ms(100); /* the background write */
    xiaotai_framework_reset();
    s_trace[0] = '\0';
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    CHECK(xiaotai_setting_get_int("audio.volume", &volume) == XIAOTAI_OK && volume == 75);
    CHECK(xiaotai_setting_get_text("wake.phrase", text, sizeof(text)) == XIAOTAI_OK && !strcmp(text, full_text));
    xiaotai_framework_reset();

    /* Product rules decide the pairs a single ranking cannot. */
    const xiaotai_module_t *ranked[] = {&STORAGE};
    config = (xiaotai_framework_config_t){.modules = ranked, .module_count = 1, .available_needs = all,
                                         .priorities = RULE_PRIORITIES, .preemptions = RULES};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    on_owner(rule_sequence, NULL);
    CHECK(s_rule_results[0] == XIAOTAI_OK && s_rule_results[1] == XIAOTAI_ERR_BUSY);
    CHECK(s_rule_results[2] == XIAOTAI_OK && s_rule_results[3] == XIAOTAI_ERR_BUSY);
    xiaotai_framework_reset();

    CHECK(framework_product_preferences_test() == 0);

    /* The orderly restart: guards first, then holds, then the announcement,
     * the delay and the restart; a caller's quiesce is taken over. */
    const xiaotai_module_t *restarting[] = {&TALK, &NETWORK, &STORAGE, &RESTARTER};
    config = (xiaotai_framework_config_t){.modules = restarting, .module_count = 4, .available_needs = all,
                                         .restart_delay_ms = 20, .restart = fake_restart};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    s_guard_busy = 1;
    xiaotai_restart_hold();
    args = (xiaotai_args_t){0};
    strcpy(args.text, "test");
    CHECK(xiaotai_intent_post(XIAOTAI_INTENT_SYSTEM_RESTART, &args) == XIAOTAI_OK);
    CHECK(drain());
    xiaotai_delay_ms(120);
    CHECK(xiaotai_restart_pending() && !xiaotai_quiesced() && s_restarted == 0);   /* the guard refuses */
    on_owner(begin_room, NULL);
    CHECK(s_room_result == XIAOTAI_ERR_BUSY);   /* nothing begins once a restart is pending */
    s_guard_busy = 0;
    xiaotai_delay_ms(120);
    CHECK(xiaotai_quiesced() && s_restarted == 0 && s_restarting == 0);   /* the hold keeps it */
    xiaotai_restart_release();
    xiaotai_restart_release();   /* without a hold: ignored */
    xiaotai_delay_ms(200);
    CHECK(drain());
    CHECK(s_restarting == 1 && strcmp(s_restarting_reason, "test") == 0 && s_restarted == 1);
    CHECK(atomic_load(&s_restart_off_owner));   /* on the restart task's internal stack */
    xiaotai_framework_reset();

    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    on_owner(restart_takeover, NULL);
    CHECK(s_restart_result[0] == XIAOTAI_OK && s_restart_result[1] == XIAOTAI_ERR_BUSY);
    CHECK(s_restart_result[2] == XIAOTAI_OK && xiaotai_quiesced());
    xiaotai_delay_ms(200);
    CHECK(drain());
    CHECK(s_restarted == 2 && strcmp(s_restarting_reason, "binding") == 0);
    xiaotai_framework_reset();

    /* The presenting application: the door claims calls from its station,
     * the companion every other call; the presenter comes to the foreground
     * before its handlers run, only its subscriptions get the event, and it
     * goes back with xiaotai_app_return(). Events no application lists reach
     * background applications too. */
    const xiaotai_application_t *pair[] = {&DOOR, &COMPANION};
    config = (xiaotai_framework_config_t){.applications = pair, .application_count = 2, .home = "door",
                                         .available_needs = all};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    CHECK(s_door_shown == 1 && s_companion_shown == 0 && s_door_entry == XIAOTAI_APP_ENTRY_START);
    CHECK(strcmp(xiaotai_app_home(), "door") == 0);
    xiaotai_view_set_sink(count_view, NULL);
    args = (xiaotai_args_t){0};
    strcpy(args.text, "station");
    CHECK(xiaotai_event_emit("call.incoming", &args, NULL, 0) == XIAOTAI_OK);
    CHECK(drain());
    CHECK(s_door_calls == 1 && s_companion_calls == 0 && s_door_shown == 1);
    strcpy(args.text, "phone");
    CHECK(xiaotai_event_emit("call.incoming", &args, NULL, 0) == XIAOTAI_OK);
    CHECK(drain());
    CHECK(s_door_calls == 1 && s_companion_calls == 1 && s_door_shown == 0 && s_companion_shown == 1);
    CHECK(s_companion_entry == XIAOTAI_APP_ENTRY_PRESENT && s_views == 1);
    CHECK(xiaotai_event_emit("net.up", NULL, NULL, 0) == XIAOTAI_OK);
    CHECK(drain());
    CHECK(s_door_any == 1);   /* in the background, not a presented event */
    CHECK(s_views == 1);      /* the background door's view was refused */
    int direction = 1;
    CHECK(xiaotai_app_page_step(direction) == XIAOTAI_ERR_INVALID_STATE);
    /* Even the background caller executes the pager in foreground context.
     * Exactly the companion callback can draw; the caller must still refuse. */
    const xiaotai_args_t page = {.value = -1};
    CHECK(xiaotai_event_emit("fixture.page", &page, NULL, 0) == XIAOTAI_OK && drain());
    CHECK(s_page_result == XIAOTAI_OK && s_page_calls == 1 && s_page_direction == -1 && s_views == 2);
    CHECK(s_companion_shown == 1 && s_companion_entry == XIAOTAI_APP_ENTRY_PRESENT);
    direction = 0;
    on_owner(page_step, &direction);
    CHECK(s_page_result == XIAOTAI_ERR_INVALID_ARG && s_page_calls == 1);
    direction = 2;
    on_owner(page_step, &direction);
    CHECK(s_page_result == XIAOTAI_ERR_INVALID_ARG && s_page_calls == 1);
    direction = 1;
    s_page_blocked = true;
    on_owner(page_step, &direction);
    CHECK(s_page_result == XIAOTAI_ERR_BUSY && s_page_calls == 2 && s_page_direction == 1 && s_views == 2);
    s_page_blocked = false;
    on_owner(page_step_during_quiesce, &direction);
    CHECK(s_page_quiesce == XIAOTAI_OK && s_page_result == XIAOTAI_ERR_INVALID_STATE && s_page_calls == 2);
    on_owner(app_return, NULL);
    CHECK(s_return_result == XIAOTAI_OK && s_door_shown == 1 && s_companion_shown == 0);
    CHECK(s_door_entry == XIAOTAI_APP_ENTRY_RETURN);
    on_owner(page_step, &direction);
    CHECK(s_page_result == XIAOTAI_ERR_NOT_SUPPORTED && s_door_shown == 1 && s_page_calls == 2);
    on_owner(app_return, NULL);
    CHECK(s_return_result == XIAOTAI_ERR_INVALID_STATE);
    on_owner(app_next, NULL);
    CHECK(s_step_result == XIAOTAI_OK && s_companion_shown == 1 && s_companion_entry == XIAOTAI_APP_ENTRY_NEXT);
    on_owner(app_previous, NULL);
    CHECK(s_step_result == XIAOTAI_OK && s_door_shown == 1 && s_door_entry == XIAOTAI_APP_ENTRY_PREVIOUS);
    /* An application's own activity, ranked by its manifest above a call. */
    s_revoked = 0;
    on_owner(ring_over_call, NULL);
    CHECK(s_call_result == XIAOTAI_OK && s_alarm_result == XIAOTAI_OK && xiaotai_activity_current(NULL) == 0);
    s_reply[0] = '\0';
    xiaotai_diag_report("framework", &reply);
    CHECK(strstr(s_reply, "foreground=door activity=- view_refused=2\n"));
    xiaotai_framework_reset();

    /* Every allocation of a start may fail: the start then fails or copes,
     * and whatever it allocated is released by the failure or the reset. */
    const xiaotai_module_t *walked[] = {&TALK, &NETWORK, &STORAGE};
    config = (xiaotai_framework_config_t){.modules = walked, .module_count = 3, .applications = applications,
                                         .application_count = 2, .home = "companion", .available_needs = all};
    uint32_t failed_steps = 0;
    for (uint32_t step = 1; step < 64; ++step) {
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(step);
        const xiaotai_err_t started = xiaotai_framework_start(&config);
        const int reached = !xiaotai_mem_host_fail_pending();
        xiaotai_mem_host_fail_nth(0);
        if (started != XIAOTAI_OK) ++failed_steps;
        xiaotai_framework_reset();
        if (xiaotai_mem_host_outstanding() != 0) {
            fprintf(stderr, "allocation %u: %lld blocks left\n", (unsigned)step,
                    (long long)xiaotai_mem_host_outstanding());
            return 1;
        }
        if (!reached) {
            CHECK(started == XIAOTAI_OK && step > 2);   /* every allocation was walked */
            break;
        }
        CHECK(step < 63);
    }
    CHECK(failed_steps >= 2);

    printf("PASS: framework lifecycle, failed-start cleanup, needs, intents (local-only), events, activities "
           "(module defaults, product rules, attachments with generations), quiesce and guards, orderly restart, "
           "generations, timers, settings and the setting store, commands, diagnostics, presenting applications, "
           "entry reasons, refused background views, application activities, contributions, allocation failures\n");
    return 0;
}
