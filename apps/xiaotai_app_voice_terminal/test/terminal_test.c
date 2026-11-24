// SPDX-License-Identifier: MIT
// Replaces legacy: tests/mz28/test_boot_button_logic.c::boot_button_logic
#include "fakes.h"
#include "terminal_internal.h"

#include <assert.h>
#include <string.h>
#include "xiaotai/hal.h"
#include "xiaotai/interaction.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/os.h"

#define N(a) (sizeof(a) / sizeof(*(a)))
#define INTENT(n) XIAOTAI_INTENT_APP_VOICE_TERMINAL_##n

void interaction_poll(uint32_t now);
static xiaotai_signal_t done;
static xiaotai_app_voice_terminal_status_t status;
static unsigned origin_count;
static int64_t restart_delay_at;

static void restart_delay(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    assert(event->args.value == 100);
    restart_delay_at = xiaotai_time_us();
}

static void guidance_changed(const char *name, void *ctx)
{
    (void)ctx;
    xiaotai_args_t args = {0};
    assert(xiaotai_setting_get_int(name, &args.value) == XIAOTAI_OK);
    strcpy(args.text, name);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, &args, NULL, 0) == XIAOTAI_OK);
}

static void check_origin(bool external) { assert(xiaotai_intent_from_external() == external); }
static void origin_job(void *ctx) { (void)ctx; check_origin(false); assert(++origin_count == 3); }
static void origin_local(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    assert(args->value == 1 && ++origin_count == 2);
    check_origin(false);
}
static void origin_external(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    assert(args->value == 1 && ++origin_count == 1);
    check_origin(true);
    assert(xiaotai_intent_post("test.origin.local", args) == XIAOTAI_OK);
    assert(xiaotai_run(origin_job, NULL) == XIAOTAI_OK);
    check_origin(true);
}
static xiaotai_err_t origin_init(void)
{
    static const xiaotai_setting_t guidance = {.name = XIAOTAI_SETTING_GUIDANCE_ENABLED,
        .key = XIAOTAI_SETTING_KEY_GUIDANCE_ENABLED, .kind = XIAOTAI_SETTING_BOOL,
        .default_value = 1, .changed = guidance_changed};
    assert(xiaotai_setting_register(&guidance) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_RESTARTING, restart_delay, NULL) == XIAOTAI_OK);
    assert(xiaotai_intent_handle("test.origin.external", origin_external, NULL) == XIAOTAI_OK);
    return xiaotai_intent_handle("test.origin.local", origin_local, NULL);
}

static void sync_owner(void *ctx)
{
    (void)ctx;
    check_origin(false);
    xiaotai_app_voice_terminal_status(&status);
    xiaotai_signal_give(&done);
}
static void barrier(void)
{
    assert(xiaotai_run(sync_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&done, 3000));
}
static void emit(const char *name)
{
    assert(xiaotai_event_emit(name, NULL, NULL, 0) == XIAOTAI_OK);
    barrier();
}
static void call_event(const char *name)
{
    assert(xiaotai_event_emit(name, NULL, &fake_call, sizeof(fake_call)) == XIAOTAI_OK);
    barrier();
}
static void execute(const char *name)
{
    assert(xiaotai_intent_post_external(name, NULL) == XIAOTAI_OK);
    barrier();
    barrier();
}
static void begins(int32_t number)
{
    const xiaotai_args_t args = {.value = number, .text = "main"};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_INPUT_BEGAN, &args, NULL, 0) == XIAOTAI_OK);
    barrier();
}
static size_t found(const char *name)
{
    for (size_t i = 0; i < fake_used; ++i) if (!strcmp(fake_log[i], name)) return i;
    return fake_used;
}
static void pulse(void *ctx) { interaction_poll(*(const uint32_t *)ctx); }
static void step(uint32_t ms)
{
    assert(xiaotai_run(pulse, &ms) == XIAOTAI_OK);
    barrier();
    barrier();
    barrier();
}
static void clicks(uint32_t ms, uint8_t n)
{
    assert(xiaotai_interaction_inject("main", n, 0) == XIAOTAI_OK);
    for (uint8_t i = 0; i < n; ++i) {
        step(ms + i * 180U);
        step(ms + i * 180U + 100U);
    }
    if (n != 4) step(ms + (n - 1U) * 180U + 450U);
}

static void skip(void *ctx) { (void)ctx; }
static void crowded_poll(void *ctx)
{
    for (unsigned i = 0; i < 16; ++i) {
        assert(xiaotai_run(i == 15 ? sync_owner : skip, NULL) == XIAOTAI_OK);
    }
    interaction_poll(*(const uint32_t *)ctx);
}

static void test_keys(void)
{
    const struct { xiaotai_wechat_state_t state; bool releasing; const char *main, *end; } cells[] = {
        {XIAOTAI_WECHAT_IDLE, false, "dial", NULL}, {XIAOTAI_WECHAT_INCOMING, false, "accept", "reject"},
        {XIAOTAI_WECHAT_OUTGOING, false, NULL, "hangup"}, {XIAOTAI_WECHAT_CONNECTING, false, NULL, "hangup"},
        {XIAOTAI_WECHAT_ACTIVE, false, NULL, "hangup"}, {XIAOTAI_WECHAT_IDLE, true, NULL, NULL},
    };
    for (size_t i = 0; i < N(cells); ++i) {
        fake_call = (xiaotai_wechat_status_t){.state = cells[i].state, .ready = true,
            .session = cells[i].state != XIAOTAI_WECHAT_IDLE ? 15 : 0, .releasing = cells[i].releasing};
        for (unsigned end = 0; end < 2; ++end) {
            const uint32_t last_refused = status.refused, last_blocked = status.blocked;
            const char *want = end ? cells[i].end : cells[i].main;
            fake_clear();
            execute(end ? INTENT(END) : INTENT(MAIN));
            if (want) assert(fake_action && !strcmp(want, fake_action) && (i == 0 || fake_session == 15));
            else if (!end && i >= 2 && i <= 4) assert(!fake_action && status.blocked == last_blocked + 1);
            else assert(!fake_action && status.refused == last_refused + 1);
        }
    }
    fake_call = (xiaotai_wechat_status_t){.ready = true};
    fake_lookup = XIAOTAI_ERR_NOT_FOUND;
    fake_clear();
    execute(INTENT(MAIN));
    assert(!fake_action && fake_refreshes == 1 && found("terminal.refused") < fake_used);
    fake_lookup = XIAOTAI_OK;
    fake_request = XIAOTAI_ERR_NOT_FOUND;
    execute(INTENT(MAIN));
    assert(fake_refreshes == 2 && found("terminal.refused") < fake_used);
    fake_request = XIAOTAI_OK;
    fake_call.ready = false;
    fake_clear();
    execute(INTENT(MAIN));
    assert(!fake_action && fake_refreshes == 2);

    fake_call = (xiaotai_wechat_status_t){.ready = true};
    begins(99);
    fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_INCOMING, .session = 17};
    const xiaotai_args_t from_key = {.value = TERMINAL_GESTURE_VALUE};
    const uint32_t stale = status.stale;
    fake_clear();
    assert(xiaotai_intent_post(INTENT(MAIN), &from_key) == XIAOTAI_OK);
    barrier();
    barrier();
    assert(!fake_action && status.stale == stale + 1 && found("terminal.refused") < fake_used);
    begins(99);
    fake_call.session = 18; /* same situation, another call */
    assert(xiaotai_intent_post(INTENT(MAIN), &from_key) == XIAOTAI_OK);
    barrier();
    assert(!fake_action && status.stale == stale + 2);
    execute(INTENT(MAIN)); /* no gesture, therefore no stale check */
    assert(fake_session == 18);
    begins(99);
    fake_call.state = XIAOTAI_WECHAT_ACTIVE; /* same call, a different key meaning */
    fake_clear();
    assert(xiaotai_intent_post(INTENT(END), &from_key) == XIAOTAI_OK);
    barrier();
    assert(!fake_action && status.stale == stale + 3);
    fake_call.state = XIAOTAI_WECHAT_CONNECTING;
    begins(99);
    fake_call.state = XIAOTAI_WECHAT_ACTIVE; /* both still mean hang up */
    assert(xiaotai_intent_post(INTENT(END), &from_key) == XIAOTAI_OK);
    barrier();
    assert(fake_action && !strcmp(fake_action, "hangup") && status.stale == stale + 3);

    const xiaotai_args_t rejected = {.value = 99, .text = INTENT(END)};
    const uint32_t refused = status.refused;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_INPUT_REJECTED, &rejected, NULL, 0) == XIAOTAI_OK);
    barrier();
    barrier();
    assert(status.refused == refused + 1);
    begins(100);
    const uint32_t before_obsolete = status.refused;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_INPUT_REJECTED, &rejected, NULL, 0) == XIAOTAI_OK);
    barrier();
    assert(status.refused == before_obsolete + 1 && status.superseded == 0);
    fake_clear();
    assert(xiaotai_intent_post(INTENT(END), &from_key) == XIAOTAI_OK);
    barrier();
    assert(fake_action && !strcmp(fake_action, "hangup") && status.stale == stale + 3);

    fake_call = (xiaotai_wechat_status_t){.ready = true};
    fake_clear();
    fake_edges(XIAOTAI_INPUT_RELEASED); /* no new press after coming up held */
    step(0);
    assert(!fake_action && !fake_used);
    clicks(100, 1);
    assert(fake_action && !strcmp(fake_action, "dial") && found("terminal.tap") < fake_used);
    fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_ACTIVE, .session = 5};
    fake_clear();
    clicks(900, 2);
    assert(fake_action && !strcmp(fake_action, "hangup") && found("terminal.double_tap") < fake_used);
    fake_clear();
    clicks(1900, 3);
    assert(!fake_action && !fake_used);
    clicks(2900, 4);
    assert(!fake_action && fake_begins == 1 && !fake_forgets && found("terminal.failed") < fake_used);
    assert(xiaotai_interaction_inject("main", 0, 7200) == XIAOTAI_OK);
    step(3900);
    step(10899);
    assert(!fake_forgets);
    step(10900);
    assert(fake_forgets == 1 && !fake_holds);
    step(11100);
    assert(fake_forgets == 1 && fake_saved_wifi && fake_saved_identity);
    assert(xiaotai_intent_post_external(INTENT(RESET_NETWORK), NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(xiaotai_intent_post_external(INTENT(RESET_BINDING), NULL) == XIAOTAI_ERR_NOT_ALLOWED);

    fake_call = (xiaotai_wechat_status_t){.ready = true};
    fake_clear();
    fake_edges(XIAOTAI_INPUT_PRESSED);
    step(12000);
    fake_edges(XIAOTAI_INPUT_RELEASED);
    step(12100);
    uint32_t settle = 12450;
    const uint32_t count = status.refused;
    assert(xiaotai_run(crowded_poll, &settle) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&done, 3000));
    assert(status.refused == count && !fake_action);
    step(12470);
    step(12490);
    assert(status.refused == count + 1 && !fake_action && found("terminal.refused") < fake_used);
}

static void edge_at(uint32_t ms, uint32_t edge)
{
    fake_edges(edge);
    step(ms);
}

static void test_boot_button(void)
{
    fake_call = (xiaotai_wechat_status_t){.ready = true};
    fake_clear();
    edge_at(100, XIAOTAI_INPUT_PRESSED);
    step(1000);
    edge_at(1100, XIAOTAI_INPUT_RELEASED);
    step(1449);
    assert(!fake_action && !fake_used);
    step(1450);
    assert(fake_action && !strcmp(fake_action, "dial") && found("terminal.tap") < fake_used);

    fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_ACTIVE, .session = 5};
    fake_clear();
    edge_at(2100, XIAOTAI_INPUT_PRESSED);
    edge_at(2160, XIAOTAI_INPUT_RELEASED);
    edge_at(2280, XIAOTAI_INPUT_PRESSED);
    edge_at(2340, XIAOTAI_INPUT_RELEASED);
    step(2689);
    assert(!fake_action && !fake_used);
    step(2690);
    assert(fake_action && !strcmp(fake_action, "hangup") && fake_session == 5 &&
           found("terminal.double_tap") < fake_used);

    fake_call = (xiaotai_wechat_status_t){.ready = true};
    fake_clear();
    edge_at(3100, XIAOTAI_INPUT_PRESSED);
    edge_at(3150, XIAOTAI_INPUT_RELEASED);
    edge_at(3250, XIAOTAI_INPUT_PRESSED);
    step(3600);  /* A new press keeps the pending click from firing. */
    step(10249);
    assert(!fake_action && !fake_used && !fake_forgets);
    step(10250);
    assert(!fake_action && fake_forgets == 1);
    step(12000);
    edge_at(12010, XIAOTAI_INPUT_RELEASED);
    step(12360);
    assert(!fake_action && fake_forgets == 1);
}

static void test_state(void)
{
    assert(!strcmp(status.shown, "terminal.boot"));
    fake_network = XIAOTAI_NET_BLOCKED;
    emit(XIAOTAI_EVENT_NET_STATE);
    assert(!strcmp(status.shown, "terminal.error") && found("terminal.failed") < fake_used);
    fake_network = XIAOTAI_NET_UNCONFIGURED;
    emit(XIAOTAI_EVENT_NET_STATE);
    assert(!strcmp(status.shown, "terminal.binding"));
    fake_has_identity = true;
    fake_platform = XIAOTAI_PLATFORM_PAUSED;
    emit(XIAOTAI_EVENT_PLATFORM_PAUSED);
    assert(!strcmp(status.shown, "terminal.connecting"));
    fake_platform = XIAOTAI_PLATFORM_ONLINE;
    emit(XIAOTAI_EVENT_PLATFORM_ONLINE);
    assert(!strcmp(status.shown, "terminal.ready"));
    fake_setup = true;
    emit(XIAOTAI_EVENT_PROVISIONING_OPENED);
    assert(!strcmp(status.shown, "terminal.setup") && found("terminal.setup.opened") < fake_used);

    fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_OUTGOING, .session = 51, .outgoing = true};
    call_event(XIAOTAI_EVENT_WECHAT_STATE);
    assert(!strcmp(status.shown, "terminal.setup"));
    fake_setup = false;
    emit(XIAOTAI_EVENT_PROVISIONING_CHANGED);
    assert(!strcmp(status.shown, "terminal.outgoing") && found("terminal.ringback") < fake_used);
    fake_clear();
    fake_call.state = XIAOTAI_WECHAT_CONNECTING;
    call_event(XIAOTAI_EVENT_WECHAT_STATE);
    assert(!strcmp(status.shown, "terminal.linking") && found("-ringback") == fake_used);
    fake_call.state = XIAOTAI_WECHAT_ACTIVE;
    call_event(XIAOTAI_EVENT_WECHAT_CONNECTED);
    assert(!strcmp(status.shown, "terminal.active") && found("-ringback") < found("terminal.connected"));
    fake_clear();
    fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_INCOMING, .session = 52};
    call_event(XIAOTAI_EVENT_WECHAT_INCOMING);
    assert(!strcmp(status.shown, "terminal.incoming") && found("terminal.ring") < fake_used);
    fake_clear();
    call_event(XIAOTAI_EVENT_WECHAT_STATE);
    assert(!fake_used); /* do not restart a ring when the follow-up state fits */
    execute(INTENT(MAIN)); /* accepted from console */
    fake_call.state = XIAOTAI_WECHAT_CONNECTING;
    call_event(XIAOTAI_EVENT_WECHAT_STATE);
    assert(status.ring_silenced && found("-ring") < fake_used && found("terminal.ringback") == fake_used);
    fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_INCOMING, .session = 53};
    call_event(XIAOTAI_EVENT_WECHAT_STATE);
    assert(!status.ring_silenced);
    begins(99);
    const xiaotai_args_t from_key = {.value = TERMINAL_GESTURE_VALUE};
    assert(xiaotai_intent_post(INTENT(MAIN), &from_key) == XIAOTAI_OK);
    barrier();
    fake_call.state = XIAOTAI_WECHAT_CONNECTING;
    call_event(XIAOTAI_EVENT_WECHAT_STATE);
    assert(!status.ring_silenced);
    for (unsigned i = 0; i < 3; ++i) {
        fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_INCOMING, .session = 54 + i};
        call_event(XIAOTAI_EVENT_WECHAT_STATE);
        fake_clear();
        fake_call = (xiaotai_wechat_status_t){.releasing = true, .end = XIAOTAI_WECHAT_END_PEER_HANGUP,
            .peer_reason = (int32_t)(i == 2 ? 8 : i == 1 ? 6 : 1)};
        call_event(XIAOTAI_EVENT_WECHAT_ENDED);
        assert(!strcmp(status.shown, "terminal.closing") && !status.ring_silenced);
        const size_t cue = found(i ? "terminal.failed" : "terminal.ended");
        assert(found("-ring") < cue && cue < fake_used);
    }
}

static void test_tables(void)
{
    const xiaotai_feedback_state_t *s = fake_table->states;
    const xiaotai_feedback_effect_t *e = fake_table->effects;
    static const uint8_t colors[][3] = {{25,115,255},{255,48,22},{28,255,74},{255,215,35},{20,255,92},
        {0,235,255},{255,18,12},{80,210,255},{255,118,8},{0,210,155},{50,125,255}};
    static const unsigned period[] = {1200,110,1400,95,220,75,1300,90,1800,2600,140};
    static const unsigned low[] = {45,28,70,0,0,0,0,0,35,28,0};
    static const unsigned high[] = {220,170,175,0,255,0,255,0,210,90,0};
    static const unsigned kind[] = {XIAOTAI_LIGHT_BREATHE, XIAOTAI_LIGHT_BLINK, XIAOTAI_LIGHT_BREATHE,
        XIAOTAI_LIGHT_CHASE, XIAOTAI_LIGHT_ALTERNATE, XIAOTAI_LIGHT_CHASE, XIAOTAI_LIGHT_FLASHES,
        XIAOTAI_LIGHT_CHASE, XIAOTAI_LIGHT_BREATHE, XIAOTAI_LIGHT_BREATHE, XIAOTAI_LIGHT_CHASE};
    static const uint8_t accent[][3] = {{230,250,255},{0,0,0},{180,255,215},{0,170,255},
        {0,0,0},{20,80,255},{0,0,0},{0,50,180},{0,0,0},{0,0,0},{20,30,170}};
    assert(fake_table->state_count == 13 && fake_table->effect_count == 10);
    for (size_t i = 0; i < N(period); ++i) {
        assert(!s[i].light && s[i].priority == 11 - i && s[i].pattern.period_ms == period[i]);
        assert(s[i].pattern.color.red == colors[i][0] && s[i].pattern.color.green == colors[i][1] &&
               s[i].pattern.color.blue == colors[i][2]);
        assert(s[i].pattern.level_min == low[i] && s[i].pattern.level_max == high[i]);
        assert(s[i].pattern.kind == kind[i] && s[i].pattern.accent.red == accent[i][0] &&
               s[i].pattern.accent.green == accent[i][1] && s[i].pattern.accent.blue == accent[i][2]);
    }
    assert(s[0].pattern.accent_ms == 180 && s[0].pattern.accent_level == 255);
    assert(s[2].pattern.accent_mode == XIAOTAI_LIGHT_ACCENT_CENTER && s[2].pattern.accent_level == 210);
    assert(s[6].pattern.count == 2 && s[6].pattern.accent_ms == 120);
    assert(s[11].sound.gap_ms == 560 && !strcmp(s[11].sound.name, "ring.incoming"));
    assert(s[12].sound.gap_ms == 760 && !strcmp(s[12].sound.name, "ring.outgoing"));
    assert(e[0].duration_ms == 140 && e[1].duration_ms == 320 && e[2].duration_ms == 360);
    assert(e[3].duration_ms == 360 && e[4].duration_ms == 420 && !e[4].sound.name);
    assert(e[0].pattern.color.red == 110 && e[0].pattern.color.green == 245 &&
           e[0].pattern.color.blue == 255 && e[0].pattern.level_max == 210);
    assert(e[1].pattern.period_ms == 70 && e[1].pattern.color.red == 255 &&
           e[1].pattern.color.green == 45 && e[1].pattern.color.blue == 20);
    assert(e[3].pattern.period_ms == 80 && e[3].pattern.level_min == 55 &&
           e[3].pattern.level_max == 255 && e[3].pattern.color.green == 138);
    assert(e[4].pattern.color.red == 210 && e[4].pattern.color.green == 255 && e[4].pattern.color.blue == 225);
    assert(e[8].duration_ms == 420 && e[8].sound.flags == XIAOTAI_FEEDBACK_WAIT);
    assert(!e[9].duration_ms && e[9].sound.flags == XIAOTAI_FEEDBACK_WAIT &&
           !strcmp(e[8].sound.name, e[9].sound.name));
}

static void owner(void (*job)(void *), void *ctx)
{
    assert(xiaotai_run(job, ctx) == XIAOTAI_OK);
    barrier();
    barrier();
}

static void advance_clock(void *ctx)
{
    fake_advance(*(const uint32_t *)ctx);
}

static void advance(uint32_t ms)
{
    /* Timers run after this owner job and before the first barrier; the
     * second barrier also drains the events those timers queued. */
    owner(advance_clock, &ms);
}

static void post_local(const char *intent)
{
    assert(xiaotai_intent_post(intent, NULL) == XIAOTAI_OK);
    barrier();
    barrier();
}

static void event_args(const char *name, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_event_emit(name, &args, NULL, 0) == XIAOTAI_OK);
    barrier();
    barrier();
}

static void environment_finished(void)
{
    const xiaotai_args_t args = {.value = XIAOTAI_ENVIRONMENT_CANCELLED};
    fake_environment.phase = XIAOTAI_ENVIRONMENT_IDLE;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_ENVIRONMENT_RESULT, &args, &fake_environment,
                              sizeof(fake_environment)) == XIAOTAI_OK);
    barrier();
    barrier();
}

static void test_reset(void)
{
    assert(xiaotai_intent_post_external(INTENT(RESET_NETWORK), NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(xiaotai_intent_post_external(INTENT(RESET_BINDING), NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(fake_saved_wifi && fake_saved_identity && !fake_begins && !fake_forgets);
    fake_environment.phase = XIAOTAI_ENVIRONMENT_WRITING;
    post_local(INTENT(RESET_NETWORK));
    fake_environment.phase = XIAOTAI_ENVIRONMENT_RESTARTING;
    post_local(INTENT(RESET_BINDING));
    assert(status.refused == 2 && !fake_begins && !fake_forgets && !fake_cancels);
    fake_error = XIAOTAI_OK;
    fake_environment.phase = XIAOTAI_ENVIRONMENT_ACQUIRING;
    post_local(INTENT(RESET_NETWORK));
    assert(status.reset_pending && !fake_begins && !fake_holds && fake_cancels == 1);
    post_local(INTENT(RESET_BINDING));
    assert(status.refused == 3 && !fake_forgets);
    fake_error = XIAOTAI_FAIL;
    environment_finished();
    assert(fake_begins == 1 && !status.reset_pending && !fake_holds && !xiaotai_restart_pending());
    assert(fake_saved_wifi && fake_saved_identity && found("terminal.failed") < fake_used);
    fake_error = XIAOTAI_OK;
    fake_environment.phase = XIAOTAI_ENVIRONMENT_WAITING;
    post_local(INTENT(RESET_BINDING));
    assert(fake_cancels == 2 && !fake_forgets && status.reset_pending);
    environment_finished();
    assert(fake_forgets == 1 && fake_holds == 1 && !fake_direct && found("terminal.connected") < fake_used);
    xiaotai_err_t error = XIAOTAI_FAIL;
    owner(fake_binding_done, &error);
    assert(!fake_holds && !status.reset_pending && !xiaotai_restart_pending() && !fake_feedback.done);
    assert(fake_saved_wifi && fake_saved_identity);
    fake_network = XIAOTAI_NET_UNCONFIGURED;
    fake_provision.error = XIAOTAI_FAIL;
    event_args(XIAOTAI_EVENT_PROVISIONING_FAILED, "begin", XIAOTAI_FAIL);
    assert(!strcmp(status.shown, "terminal.error"));
    fake_setup = true;
    emit(XIAOTAI_EVENT_PROVISIONING_OPENED);
    assert(!strcmp(status.shown, "terminal.setup"));
}

static void network_committed(void *ctx)
{
    (void)ctx;
    fake_saved_wifi = false;
    fake_provision.revision++;
    fake_provision.provision.restart_required = true;
    fake_provision.restart_pending = true;
    assert(xiaotai_restart_request_direct(XIAOTAI_QUIESCE_PROVISIONING) == XIAOTAI_OK);
    // Deliberately omit a changed event: the owner poll still sees handoff.
}

static void cue_done(void *ctx) { (void)ctx; fake_feedback.done(XIAOTAI_OK, fake_feedback.ctx); }

static void expect_restart(void)
{
    if (!restart_delay_at) advance(50);
    assert(restart_delay_at > 0);
    const int64_t remaining = 100 - (xiaotai_time_us() - restart_delay_at) / 1000;
    assert(remaining > 0);
    advance((uint32_t)(remaining - 1));
    assert(!fake_restarted());
    advance(1);
    for (unsigned i = 0; !fake_restarted() && i < 100; ++i) xiaotai_delay_ms(1);
    assert(fake_restarted());
}

static void test_reset_success(bool network, bool timeout, bool cue_error)
{
    fake_error = XIAOTAI_OK;
    if (cue_error) fake_sound_error = XIAOTAI_ERR_BUSY;
    fake_call = (xiaotai_wechat_status_t){.state = XIAOTAI_WECHAT_INCOMING, .session = 55};
    call_event(XIAOTAI_EVENT_WECHAT_INCOMING);
    fake_clear();
    post_local(network ? INTENT(RESET_NETWORK) : INTENT(RESET_BINDING));
    assert(status.reset_pending && fake_holds == 1 && !fake_action && fake_saved_wifi && fake_saved_identity);
    assert(!fake_feedback.done && !xiaotai_restart_pending());
    if (network) {
        assert(fake_begins == 1 && !fake_forgets && found("-ring") < fake_used);
        owner(network_committed, NULL);
        advance(50);
    } else {
        assert(fake_forgets == 1 && fake_direct && found("terminal.connected") < fake_used);
        xiaotai_err_t success = XIAOTAI_OK;
        // A late storage result is not a late cue. It still owns the hold.
        advance(31000);
        assert(fake_holds == 1 && status.reset_pending);
        owner(fake_binding_done, &success);
        assert(found("terminal.connected") < found(TERMINAL_CONFIRM_AUDIO));
        owner(fake_binding_done, &success);
    }
    assert(xiaotai_restart_pending() && !fake_restarted() && !fake_action && !xiaotai_quiesced());
    assert(fake_saved_wifi == !network && fake_saved_identity == network);
    assert(fake_feedback.done && fake_feedback.valid_ms == 400 && fake_holds == (cue_error ? 0 : 1));
    assert(fake_table->effects[8].sound.flags == (cue_error ? 0U : XIAOTAI_FEEDBACK_WAIT));
    assert(fake_table->effects[9].sound.flags == fake_table->effects[8].sound.flags);
    if (!cue_error) {
        advance(399);
        assert(fake_holds == 1 && !fake_restarted());
        if (timeout) {
            advance(1);
            assert(fake_sound_cancels == 1);
        } else owner(cue_done, NULL);
    }
    assert(!fake_holds && !status.reset_pending);
    owner(cue_done, NULL); // late player callback must not release twice
    expect_restart();
}

static void store_restart(void *ctx)
{
    (void)ctx;
    xiaotai_restart_hold(); // the provisioning module's own bridge across DONE
    assert(xiaotai_restart_request_direct(XIAOTAI_QUIESCE_PROVISIONING) == XIAOTAI_OK);
    fake_provision_result(XIAOTAI_OK, true);
}

static void store_failure(void *ctx)
{
    (void)ctx;
    fake_provision_result(XIAOTAI_FAIL, false);
}

static void test_store(void)
{
    fake_provision.phase = XIAOTAI_PROVISIONING_STORE_FAILED;
    owner(store_failure, NULL);
    assert(found("terminal.refused") < fake_used && !fake_retries);
    advance(1999);
    assert(!fake_retries);
    advance(1);
    assert(fake_retries == 1);
    fake_error = XIAOTAI_OK;
    advance(2000);
    assert(fake_retries == 2);
    // A second durable store failure re-arms the application's one retry.
    owner(store_failure, NULL);
    fake_provision.phase = XIAOTAI_PROVISIONING_DONE;
    fake_provision.revision++;
    owner(store_restart, NULL);
    xiaotai_restart_release();
    assert(fake_holds == 1 && fake_feedback.done && !fake_restarted());
    event_args(XIAOTAI_EVENT_PROVISIONING_DONE, NULL, 1);
    assert(fake_holds == 1);
    owner(cue_done, NULL);
    assert(!fake_holds);
    advance(50);
    advance(2000);
    assert(fake_retries == 2);
}

static void set_guidance(void *ctx)
{
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, *(const int32_t *)ctx) == XIAOTAI_OK);
}

static void test_binding_voice(const char *arg)
{
    fake_sound_pending = true;
    fake_binding.phase = XIAOTAI_BINDING_WAITING;
    fake_binding.seconds_left = 60;
    strcpy(fake_binding.verification, "123456");
    emit(XIAOTAI_EVENT_BINDING_CODE);
    assert(!fake_spoken);
    advance(199);
    assert(!fake_spoken);
    fake_sound_pending = false;
    advance(1);
    if (!strcmp(arg, "binding_defaults")) {
        advance(30000);
        assert(!fake_spoken);
        return;
    }
    assert(fake_spoken == 1 && fake_binding.announcing);
    advance(3000);
    fake_binding.announcing = false;
    emit(XIAOTAI_EVENT_BINDING_ANNOUNCED);
    const uint32_t first = !strcmp(arg, "binding_start_interval") ? 7000 : 10000;
    advance(first - 1);
    assert(fake_spoken == 1);
    fake_announce = XIAOTAI_ERR_BUSY;
    advance(1);
    assert(fake_spoken == 2 && !fake_binding.announcing);
    advance(9999);
    assert(fake_spoken == 2);
    fake_announce = XIAOTAI_OK;
    advance(1);
    assert(fake_spoken == 3 && fake_binding.announcing);
    int32_t guidance = 0;
    owner(set_guidance, &guidance);
    assert(fake_stops == 1 && !fake_binding.announcing);
    advance(11000);
    assert(fake_spoken == 3);
    fake_binding.announcing = true; // an external/manual announcement is not ours
    event_args(XIAOTAI_EVENT_SETTINGS_CHANGED, XIAOTAI_SETTING_GUIDANCE_ENABLED, 0);
    assert(fake_stops == 1 && fake_binding.announcing);
    fake_binding.announcing = false;
    guidance = 1;
    owner(set_guidance, &guidance);
    assert(fake_spoken == 4);
    fake_binding.phase = XIAOTAI_BINDING_STORING;
    fake_binding.seconds_left = 0;
    fake_binding.announcing = false;
    emit(XIAOTAI_EVENT_BINDING_CHANGED);
    advance(30000);
    assert(fake_spoken == 4);
}

static void test_origin(void)
{
    const xiaotai_args_t value = {.value = TERMINAL_GESTURE_VALUE, .text = "ignored"};
    assert(xiaotai_intent_post_external("test.origin.external", &value) == XIAOTAI_OK);
    barrier();
    barrier();
    assert(origin_count == 3 && !xiaotai_intent_from_external());

    fake_call = (xiaotai_wechat_status_t){.ready = true};
    const xiaotai_args_t forged = {.value = TERMINAL_GESTURE_VALUE};
    const uint32_t refused = status.refused, stale = status.stale;
    assert(xiaotai_intent_post_external(INTENT(MAIN), &forged) == XIAOTAI_OK);
    barrier();
    assert(fake_action && !strcmp(fake_action, "dial") && refused == status.refused);
    begins(99);
    fake_clear();
    assert(xiaotai_intent_post_external(INTENT(MAIN), &forged) == XIAOTAI_OK);
    barrier();
    assert(fake_action && !strcmp(fake_action, "dial") && refused == status.refused);
    fake_clear();
    assert(xiaotai_intent_post(INTENT(MAIN), &forged) == XIAOTAI_OK);
    barrier();
    assert(fake_action && !strcmp(fake_action, "dial") && stale == status.stale);
}

static void test_quiet(void)
{
    fake_call = (xiaotai_wechat_status_t){.ready = true};
    begins(33);
    begins(34);
    const xiaotai_args_t older = {.value = 33, .text = INTENT(MAIN)};
    fake_clear();
    assert(xiaotai_event_emit(XIAOTAI_EVENT_INPUT_REJECTED, &older, NULL, 0) == XIAOTAI_OK);
    barrier();
    assert(status.superseded == 1 && !status.refused && !fake_used);
    const xiaotai_args_t local = {.value = TERMINAL_GESTURE_VALUE};
    assert(xiaotai_intent_post(INTENT(MAIN), &local) == XIAOTAI_OK);
    barrier();
    assert(fake_action && !strcmp(fake_action, "dial") && found("terminal.tap") < fake_used);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_INPUT_REJECTED, &older, NULL, 0) == XIAOTAI_OK);
    barrier();
    assert(status.superseded == 2 && !status.refused && found("terminal.refused") == fake_used);
    const xiaotai_args_t current = {.value = 34, .text = INTENT(MAIN)};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_INPUT_REJECTED, &current, NULL, 0) == XIAOTAI_OK);
    barrier();
    barrier();
    assert(status.superseded == 2 && status.refused == 1 && found("terminal.refused") < fake_used);
}

int main(int argc, char **argv)
{
    const char *const arg = argc > 1 ? argv[1] : "";
    const bool no_network = !strcmp(arg, "disabled") || !strcmp(arg, "no_network");
    const bool no_binding = !strcmp(arg, "disabled") || !strcmp(arg, "no_binding");
    const bool configured = no_network || no_binding;
    const bool quiet = !strcmp(arg, "quiet");
    const bool reset = !strncmp(arg, "reset", 5);
    const bool voice = !strncmp(arg, "binding", 7) && strcmp(arg, "binding_defaults") != 0;
    const xiaotai_app_voice_terminal_config_t cfg = {.announce_repeat_s = 3601};
    assert(xiaotai_app_voice_terminal_configure(&cfg) == XIAOTAI_ERR_INVALID_ARG);
    if (configured || quiet || voice || reset) assert(xiaotai_app_voice_terminal_configure(&(xiaotai_app_voice_terminal_config_t){
        .no_network_reset = no_network, .no_binding_reset = no_binding, .dial_contact = configured ? 1 : 0,
        .quiet_superseded_gesture = quiet,
        .confirm_timeout_ms = reset ? 400U : 0U,
        .direct_binding_reset = reset && strcmp(arg, "reset") != 0,
        .skip_busy_confirmation = !strcmp(arg, "reset_cue_failure"),
        .announce_on_code = voice, .announce_repeat_s = voice ? 10U : 0U,
        .announce_follows_guidance = voice, .announce_repeat_from_end = strcmp(arg, "binding_start_interval") != 0,
    }) == XIAOTAI_OK);
    const xiaotai_module_t engine = {.name = "interaction", .init = xiaotai_engine_interaction_init,
        .start = xiaotai_engine_interaction_start};
    const xiaotai_module_t fixture = {.name = "fixture", .init = origin_init};
    const xiaotai_application_t app = {.name = "voice_terminal", .init = xiaotai_app_voice_terminal_init,
        .start = xiaotai_app_voice_terminal_start};
    const xiaotai_framework_config_t framework = {.modules = (const xiaotai_module_t *const[]){&engine, &fixture},
        .module_count = 2, .applications = (const xiaotai_application_t *const[]){&app}, .application_count = 1,
        .queue_depth = 16, .restart_delay_ms = 100, .restart = fake_restart};
    assert(xiaotai_signal_init(&done) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    barrier();
    assert(xiaotai_app_voice_terminal_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    if (configured) {
        assert(xiaotai_intent_exists(INTENT(RESET_NETWORK)) == !no_network);
        assert(xiaotai_intent_exists(INTENT(RESET_BINDING)) == !no_binding);
        fake_call.ready = true;
        execute(INTENT(MAIN));
        assert(fake_action && !strcmp(fake_action, "dial") && !strcmp(fake_dial_id, "contact2"));
        if (no_network) {
            fake_call.state = XIAOTAI_WECHAT_ACTIVE;
            fake_call.session = 5;
            fake_clear();
            clicks(100, 3);
            clicks(1200, 4);
            assert(!fake_action && !fake_used);
        }
    } else if (!strcmp(arg, "state")) test_state();
    else if (!strcmp(arg, "tables")) test_tables();
    else if (!strcmp(arg, "boot_button")) test_boot_button();
    else if (!strcmp(arg, "origin")) test_origin();
    else if (!strcmp(arg, "reset")) test_reset();
    else if (!strcmp(arg, "reset_store")) test_store();
    else if (reset) test_reset_success(!strcmp(arg, "reset_network"), !strcmp(arg, "reset_timeout"),
                                       !strcmp(arg, "reset_cue_failure"));
    else if (!strncmp(arg, "binding", 7)) test_binding_voice(arg);
    else if (quiet) test_quiet();
    else test_keys();
    xiaotai_interaction_reset();
    xiaotai_framework_reset();
    xiaotai_app_voice_terminal_reset();
    xiaotai_signal_deinit(&done);
    return 0;
}
