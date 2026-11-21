// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_runtime_maintenance.py
#include "fakes.h"
#include "feedback_fakes.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/room.h"
#include "xiaotai/port/os.h"

#define fixture room_http
#include "fake_platform.h"
#undef fixture
#include "fake_link.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef COMPANION_TEST_ROOM_LCD
extern const xiaotai_module_t room_pointer_test_ui;
void room_pointer_test_prepare(void);
xiaotai_err_t room_pointer_test_init(void);
void room_pointer_test_run(void);
void room_pointer_test_reset(void);
uint32_t room_pointer_test_advance(unsigned advance);
void room_pointer_test_drain(void);
void room_pointer_test_join(void);
#endif

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_room, xiaotai_app_companion_with_environment;
#ifdef COMPANION_TEST_ROOM_LCD
extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_ui;
#endif
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
#ifdef COMPANION_TEST_ROOM_LCD
    &xiaotai_app_companion_with_ui,
#endif
    &xiaotai_app_companion_with_room, &xiaotai_app_companion_with_environment, NULL,
};

static xiaotai_signal_t fence;
static uint32_t quiet;
static bool empty_room;
static _Atomic int64_t admission_clock_us;
static const char paused[] = "正在维护，稍后同步";

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t now = atomic_load(&admission_clock_us);
    return now ? now : __real_xiaotai_time_us();
}

static void barrier(void *unused) { (void)unused; xiaotai_signal_give(&fence); }
static void flush(void)
{
    for (unsigned i = 0; i < 5; ++i) {
        assert(xiaotai_run(barrier, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void owner(void (*fn)(void *), void *ctx)
{
    assert(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    flush();
}

static void send(const char *intent, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) snprintf(args.text, sizeof(args.text), "%s", text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}

static void door_page(bool shown)
{
    if (shown) xiaotai_view_show(&(const xiaotai_view_t){.id = "door-home", .kind = XIAOTAI_VIEW_NOTICE});
}

static void begin(void *reason)
{
    assert(!xiaotai_quiesced_for(reason));
    assert(xiaotai_quiesce_begin(reason, &quiet) == XIAOTAI_OK);
    assert(xiaotai_quiesced_for(reason) && !xiaotai_quiesced_for("wrong-reason"));
    assert(!xiaotai_quiesced_for(NULL) && !xiaotai_quiesced_for(""));
    xiaotai_quiesce_end(quiet + 1U);
    assert(xiaotai_quiesced_for(reason));
}

static void end(void *unused)
{
    (void)unused;
    xiaotai_quiesce_end(quiet);
    assert(!xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT));
}

static void answer_assignment(void *arg)
{
    const unsigned index = *(const unsigned *)arg;
    assert(room_http.requests[index].method == XIAOTAI_PLATFORM_GET);
    assert(!strcmp(room_http.requests[index].path, "/v1/call/group/device/assignment"));
    fake_reply(index, XIAOTAI_OK, 200, 200, empty_room ?
        "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"\",\"assignment_version\":0}}" :
        "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"joined\","
        "\"room_id\":\"held-room\",\"room_code\":\"001234\",\"assignment_version\":3}}");
}

static void room_state(bool attending)
{
    xiaotai_room_status_t status;
    xiaotai_room_status(&status);
    assert(status.attending == attending && status.assigned && status.assignment_version == 3);
    assert(!strcmp(status.room_id, "held-room") && !status.generation);
}

static void reset_room(void *unused) { (void)unused; xiaotai_room_reset(); }

static const xiaotai_view_item_t *item(const char *label)
{
    for (size_t i = 0; i < fixture.item_count; ++i) {
        if (!strcmp(fixture.items[i].label, label)) return &fixture.items[i];
    }
    assert(false);
    return NULL;
}

static void select_room(void)
{
    const xiaotai_view_item_t *room = item("多人对讲");
    assert(!room->state);
    send(room->intent, NULL, room->value);
}

static void verify_form(void)
{
    send(XIAOTAI_INTENT_APP_COMPANION_ROOM, "join", 0);
    assert(!strcmp(fixture.id, "room.form") && fixture.field_count);
    send(fixture.fields[0].intent, "001234", fixture.fields[0].value);
    owner(begin, XIAOTAI_QUIESCE_ENVIRONMENT);
    assert(!strcmp(fixture.id, "room.form") && !strcmp(fixture.status, paused));
    assert(xiaotai_quiesced() && !xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT));
    assert(!strcmp(fixture.fields[0].text, "001234"));
    const unsigned before = fixture.refused;
    send(XIAOTAI_INTENT_APP_COMPANION_ROOM, "submit", 0);
    assert(fixture.refused == before + 1 && room_http.sent == 1);
    owner(end, NULL);
    assert(!strcmp(fixture.id, "room.form") && strcmp(fixture.status, paused));
    assert(!strcmp(fixture.fields[0].text, "001234") && room_http.sent == 2);
}

static void token_failed(void *unused)
{
    (void)unused;
    fake_reply(1, XIAOTAI_ERR_TIMEOUT, 0, 0, NULL);
}

static void token_received(void *index)
{
    fake_reply(index ? *(const unsigned *)index : 1U, XIAOTAI_OK, 200, 200, "{\"code\":200,\"data\":{"
        "\"peer_id\":\"peer\",\"token\":\"fixture-token\",\"heartbeat_seconds\":1,\"lease_seconds\":9}}");
}

static void slot_occupied(void *unused)
{
    (void)unused;
    fake_link.can_connect = false;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_TRANSPORT_STATE, NULL, NULL, 0) == XIAOTAI_OK);
}

static void link_failed(void *unused)
{
    (void)unused;
    fake_close(XIAOTAI_LINK_END_TIMEOUT);
}

struct old_link {
    xiaotai_link_handlers_t handlers;
    uint32_t generation;
};

static void stale_failure(void *ctx)
{
    const struct old_link *old = ctx;
    old->handlers.closed(old->handlers.ctx, old->generation, XIAOTAI_LINK_END_TIMEOUT, 0);
}

static void assignment_notice(void *unused)
{
    (void)unused;
    fake_notify(room_http.binding);
}

static void new_assignment(void *unused)
{
    (void)unused;
    assert(!strcmp(room_http.requests[2].path, "/v1/call/group/device/assignment"));
    fake_reply(2, XIAOTAI_OK, 200, 200, "{\"code\":200,\"data\":{\"device_id\":\"self\","
        "\"desired_state\":\"joined\",\"room_id\":\"next-room\",\"room_code\":\"001235\",\"assignment_version\":4}}");
}

static void complete_release(void *unused)
{
    (void)unused;
    assert(!strcmp(room_http.requests[3].path, "/v1/call/group/device/presence"));
    fake_link.can_connect = true;
    fake_reply(3, XIAOTAI_OK, 200, 200, NULL);
}

static void preempt_room(void *unused)
{
    (void)unused;
    const xiaotai_activity_t activity = {.name = "incoming", .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    uint32_t id = 0;
    assert(xiaotai_activity_begin(&activity, &id) == XIAOTAI_OK && id);
}

struct admission_probe {
    xiaotai_err_t refusal;
    uint32_t activity;
};

static void reject_token_admission(void *ctx)
{
    struct admission_probe *probe = ctx;
    room_http.token_admission_result = probe->refusal;
    unsigned first_assignment = 0;
    answer_assignment(&first_assignment);
    probe->activity = xiaotai_activity_current(NULL);
}

static void advance_retry(unsigned elapsed_ms, xiaotai_err_t result)
{
    room_http.token_admission_result = result;
    atomic_fetch_add(&admission_clock_us, (int64_t)elapsed_ms * 1000);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_TRANSPORT_STATE, NULL, NULL, 0) == XIAOTAI_OK);
}

static void tick_retry(void *ctx)
{
    advance_retry(*(const unsigned *)ctx, XIAOTAI_OK);
}

static void retry_without_memory(void *ctx)
{
    advance_retry(*(const unsigned *)ctx, XIAOTAI_ERR_NO_MEM);
}

static void room_feedback(const char *mode)
{
    const bool no_cue = !strcmp(mode, "feedback-admission-disabled");
    const bool hard_refusal = !strcmp(mode, "feedback-admission-failed") || no_cue;
    const bool memory_cue = !strcmp(mode, "feedback-admission-memory-cue");
    const bool gated = !strcmp(mode, "feedback-admission-platform-gated");
    const bool deferred = memory_cue || gated || !strcmp(mode, "feedback-admission-deferred");
    const bool active_link_failed = !strcmp(mode, "feedback-link-failed");
    const bool failed = !strcmp(mode, "feedback-failed") || active_link_failed;
    const bool disabled = !strcmp(mode, "feedback-disabled");
    const bool preempt = !strcmp(mode, "feedback-preempt");
    const bool offline = !strcmp(mode, "feedback-offline");
    const bool maintenance = !strcmp(mode, "feedback-maintenance");
    const bool leave = !strcmp(mode, "feedback-leave");
    const bool changed = !strcmp(mode, "feedback-assignment");
    const bool early_leave = !strcmp(mode, "feedback-early-leave");
    const bool suppress = disabled || preempt || offline || maintenance || early_leave;
    feedback_fixture.enabled = true;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
    flush();
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    assert(room_http.sent == 1U);
    if (hard_refusal || deferred) {
        const unsigned old_ended = companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED);
        const unsigned old_failed = companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED);
        struct admission_probe probe = {.refusal = hard_refusal ? XIAOTAI_ERR_INVALID_SIZE :
            gated ? XIAOTAI_ERR_PLATFORM_GATED : XIAOTAI_ERR_NO_MEM};
        owner(reject_token_admission, &probe);
        xiaotai_room_status_t status;
        xiaotai_room_status(&status);
        assert(status.assigned && !status.generation && !fake_link.connects);
        assert(hard_refusal ? (status.error == probe.refusal && status.attempts == 1U) :
                               (status.error == XIAOTAI_OK && !status.attempts));
        const unsigned new_failed = companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) - old_failed;
        fprintf(stderr, "Owner Room %s: remaining_activity=%u failed=%u\n", mode, probe.activity, new_failed);
        assert(!probe.activity && new_failed == (unsigned)((hard_refusal && !no_cue) || memory_cue));
        if (hard_refusal) {
            assert(room_http.sent == 2U);
            unsigned refresh = 1U;
            owner(answer_assignment, &refresh);
        }
        unsigned next = room_http.sent;
        if (memory_cue) {
            unsigned again = 500U;
            owner(retry_without_memory, &again);
            xiaotai_room_status(&status);
            assert(!status.attempts && status.error == XIAOTAI_OK && !status.generation);
            assert(room_http.sent == next && !fake_link.connects);
            assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) == old_failed + 1U);
        }
        unsigned step = hard_refusal ? 3999U : 499U;
        owner(tick_retry, &step);
        assert(room_http.sent == next && !fake_link.connects);
        step = 1U;
        owner(tick_retry, &step);
        assert(room_http.sent == next + 1U && strstr(room_http.requests[next].path, "/connect-token"));
        owner(token_received, &next);
        xiaotai_room_status(&status);
        assert(status.phase == XIAOTAI_ROOM_CONNECTING && status.generation && fake_link.connects == 1U);
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED) == old_ended);
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) == old_failed + new_failed);
        fprintf(stderr, "Owner Room %s: retry_generation=%u elapsed_ms=%u; no additional cue\n",
                mode, status.generation, hard_refusal ? 4000U : 500U);
        return;
    }
    unsigned index = 0;
    owner(answer_assignment, &index);
    assert(room_http.sent == 2U && strstr(room_http.requests[1].path, "/connect-token"));
    xiaotai_room_status_t status;
    xiaotai_room_status(&status);
    assert(status.attending && status.phase == XIAOTAI_ROOM_CONNECTING && !status.generation);
    const unsigned ended = companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED);
    const unsigned errors = companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED);
    const bool owns_link = leave || active_link_failed || changed;
    if (owns_link) {
        owner(token_received, NULL);
        owner(slot_occupied, NULL);
        xiaotai_room_status(&status);
        assert(status.phase == XIAOTAI_ROOM_CONNECTING && status.generation && fake_link.connects == 1U);
        xiaotai_transport_status_t transport;
        xiaotai_transport_status(&transport);
        assert(transport.state == XIAOTAI_TRANSPORT_ONLINE && !transport.connect_ready);
    }
    struct old_link old = {.handlers = fake_link.handlers, .generation = status.generation};
    if (active_link_failed) owner(link_failed, NULL);
    else if (failed) owner(token_failed, NULL);
    else if (preempt) owner(preempt_room, NULL);
    else if (changed) {
        owner(assignment_notice, NULL);
        assert(room_http.sent == 3U);
        owner(new_assignment, NULL);
    }
    else if (leave || early_leave) {
        assert(xiaotai_room_leave() == XIAOTAI_OK);
        flush();
    }
    else if (offline) {
        fake_link.online = fake_link.can_connect = false;
        assert(xiaotai_event_emit(XIAOTAI_EVENT_TRANSPORT_STATE, NULL, NULL, 0) == XIAOTAI_OK);
        flush();
    } else if (maintenance) {
        owner(begin, XIAOTAI_QUIESCE_COPROCESSOR);
        assert(xiaotai_room_attend(false) == XIAOTAI_OK);
        flush();
    } else {
        assert(xiaotai_room_disconnect() == XIAOTAI_OK);
        flush();
    }
    xiaotai_room_status(&status);
    assert(status.phase != XIAOTAI_ROOM_CONNECTING && !status.generation && fake_link.connects == (unsigned)owns_link);
    if (failed) assert(status.error == XIAOTAI_ERR_TIMEOUT && status.attempts == 1U);
    printf("Owner room feedback %s: ended=%u failed=%u\n", mode,
           companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED) - ended,
           companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) - errors);
    for (unsigned i = 0; i < 2; ++i) {
        assert(xiaotai_event_emit(XIAOTAI_EVENT_ROOM_STATE, NULL, NULL, 0) == XIAOTAI_OK);
        flush();
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED) == ended + (!suppress && !failed));
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) == errors + (!suppress && failed));
    }
    if (changed) {
        assert(status.assignment_version == 4U && !strcmp(status.room_id, "next-room") && room_http.sent == 4U);
        owner(complete_release, NULL);
        assert(room_http.sent == 5U && strstr(room_http.requests[4].path, "/connect-token"));
        index = 4U;
        owner(token_received, &index);
        owner(slot_occupied, NULL);
        xiaotai_room_status(&status);
        const uint32_t new_generation = status.generation;
        assert(new_generation && new_generation != old.generation && fake_link.connects == 2U);
        owner(stale_failure, &old);
        xiaotai_room_status(&status);
        assert(status.generation == new_generation && status.phase == XIAOTAI_ROOM_CONNECTING && !status.error);
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_ENDED) == ended + 1U);
        assert(companion_fixture_fire_count(XIAOTAI_APP_COMPANION_EFFECT_FAILED) == errors);
    }
}

#ifdef COMPANION_TEST_ROOM_LCD
uint32_t room_pointer_test_advance(unsigned advance)
{
    const int64_t t = atomic_fetch_add(&admission_clock_us, (int64_t)advance * 1000) + (int64_t)advance * 1000;
    flush();
    return (uint32_t)(t / 1000);
}

void room_pointer_test_drain(void) { flush(); }

static void room_wire_join(void *unused)
{
    (void)unused;
    fake_open();
    char body[320];
    const int n = snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"session_id\":\"j\","
        "\"input_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1},"
        "\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}}}", fake_link.generation);
    assert(n > 0 && (size_t)n < sizeof(body));
    fake_command(body);
}

void room_pointer_test_join(void)
{
    owner(token_received, NULL);
    owner(room_wire_join, NULL);
}
#endif

int main(int argc, char **argv)
{
    const bool door_home = argc == 2 && !strcmp(argv[1], "door");
    const bool feedback_case = argc == 2 && !strncmp(argv[1], "feedback-", 9);
    if (feedback_case && !strncmp(argv[1], "feedback-admission-", 19))
        atomic_store(&admission_clock_us, INT64_C(1000000));
    empty_room = argc == 2 && !strcmp(argv[1], "form");
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | (feedback_case ? XIAOTAI_NEED_KEY : XIAOTAI_NEED_TOUCH);
    fake_reset(!feedback_case);
    room_http.online = true;
#ifdef COMPANION_TEST_ROOM_LCD
    assert(argc == 1);
    atomic_store(&admission_clock_us, INT64_C(1000000));
    room_pointer_test_prepare();
    fake_link.online = fake_link.can_connect = true;
    assert(xiaotai_room_configure(&(const xiaotai_room_config_t){.talk_keepalive_ms = 750,
        .talk_keepalive_exclusive = true}) == XIAOTAI_OK);
#endif
    if (feedback_case) {
        fake_link.online = fake_link.can_connect = true;
        assert(xiaotai_room_configure(&(const xiaotai_room_config_t){.request_downlink = true,
            .notify_deferred_no_mem = !strcmp(argv[1], "feedback-admission-memory-cue") ||
                                     !strcmp(argv[1], "feedback-admission-platform-gated")}) == XIAOTAI_OK);
    }
    fixture = (companion_test_fixture_t){.needs = needs, .net.state = XIAOTAI_NET_UP,
        .binding.phase = XIAOTAI_BINDING_BOUND,
        .platform = {.phase = XIAOTAI_PLATFORM_ONLINE, .generation = 1}};
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_at_configure(&(const xiaotai_at_config_t){.task_stack_bytes = XIAOTAI_AT_NO_TASK,
        .no_platform_console = true}) == XIAOTAI_OK);
    assert(xiaotai_app_companion_configure(&(const xiaotai_app_companion_config_t){.room_leave_confirm = !feedback_case,
#ifdef COMPANION_TEST_ROOM_LCD
        .room_pointer_hold = true,
#endif
        .room_keeps_talk = !feedback_case, .room_navigation_during_environment = !feedback_case,
        .room_end_feedback = feedback_case && strcmp(argv[1], "feedback-disabled") &&
                             strcmp(argv[1], "feedback-admission-disabled")}) == XIAOTAI_OK);
    static const xiaotai_module_t room = {.name = "room", .init = xiaotai_module_room_init,
        .start = xiaotai_module_room_start};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
#ifdef COMPANION_TEST_ROOM_LCD
    static const xiaotai_module_t probe = {.name = "probe", .init = room_pointer_test_init};
    static const xiaotai_module_t *const modules[] = {&at, &room, &room_pointer_test_ui, &probe};
#else
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const xiaotai_module_t *const modules[] = {&at, &room, &probe};
#endif
    static const xiaotai_application_t companion_app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t door_app = {.name = "door", .foreground = door_page};
    static const xiaotai_application_t *const apps[] = {&companion_app, &door_app};
    static const xiaotai_priority_t priorities[] = {{"room", 2}, {"incoming", 8}, {NULL, 0}};
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules,
        .module_count = sizeof(modules) / sizeof(modules[0]),
        .applications = apps, .application_count = door_home ? 2 : 1, .home = door_home ? "door" : "companion",
        .available_needs = UINT32_MAX, .priorities = priorities}) == XIAOTAI_OK);
    flush();
    assert(!strcmp(fixture.id, door_home ? "door-home" : "home"));
    if (feedback_case) { room_feedback(argv[1]); goto reset; }
    send(XIAOTAI_INTENT_SYSTEM_MENU, NULL, 0);
    select_room();
    assert(!strcmp(fixture.id, "room") && room_http.sent == 1U);
    unsigned index = 0;
    owner(answer_assignment, &index);
#ifdef COMPANION_TEST_ROOM_LCD
    room_pointer_test_run();
    room_pointer_test_reset();
    goto reset;
#endif
    if (empty_room) { verify_form(); goto reset; }
    room_state(true);

    send(XIAOTAI_INTENT_APP_COMPANION_ROOM, "leave", 0);
    assert(companion.confirm_resolved);
    owner(begin, XIAOTAI_QUIESCE_ENVIRONMENT);
    assert(!strcmp(fixture.id, "room") && !strcmp(fixture.status, paused));
    assert(companion.status.setup && !companion.confirm_resolved);
    const xiaotai_args_t failed = {.value = XIAOTAI_ENVIRONMENT_ERROR_HELD};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_ENVIRONMENT_CHANGED, &failed, NULL, 0) == XIAOTAI_OK);
    flush();
    assert(!strcmp(fixture.id, "room") && !strcmp(fixture.status, paused));
    room_state(true);
    send(XIAOTAI_INTENT_APP_COMPANION_CONFIRM, NULL, 1);
    const unsigned before = fixture.refused;
    send(XIAOTAI_INTENT_APP_COMPANION_ROOM, "leave", 0);
    send(XIAOTAI_INTENT_APP_COMPANION_ROOM, "refresh", 0);
    assert(fixture.refused == before + 2);
    assert(!strcmp(fixture.toast, "正在维护，请稍后再试"));
    assert(room_http.sent == 1U);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!strcmp(fixture.id, "menu"));
    room_state(false);
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, XIAOTAI_APP_COMPANION_PAGE_SETTINGS, 0);
    assert(!strcmp(fixture.id, "menu"));
    select_room();
    assert(!strcmp(fixture.id, "room") && !strcmp(fixture.status, paused));
    room_state(true);
    assert(room_http.sent == 1U);
    owner(end, NULL);
    assert(!strcmp(fixture.id, "room") && strcmp(fixture.status, paused));
    room_state(true);
    assert(room_http.sent == 2U);
    index = 1;
    owner(answer_assignment, &index);
    room_state(true);

    owner(begin, XIAOTAI_QUIESCE_COPROCESSOR);
    assert(!strcmp(fixture.id, "maintenance"));
    room_state(false);
    owner(end, NULL);
    assert(!strcmp(fixture.id, "home") && room_http.sent == 2U);
    if (door_home) {
        send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
        assert(!strcmp(fixture.id, "door-home"));
    }
    owner(begin, XIAOTAI_QUIESCE_ENVIRONMENT);
    assert(!strcmp(fixture.id, door_home ? "door-home" : "home"));
    if (!door_home)
        assert(fixture.action_count == 1 && !strcmp(fixture.actions[0].intent, XIAOTAI_INTENT_SYSTEM_MENU));
    send(XIAOTAI_INTENT_SYSTEM_MENU, NULL, 0);
    assert(!strcmp(fixture.id, "menu"));
    select_room();
    assert(!strcmp(fixture.id, "room") && room_http.sent == 2U);
    room_state(true);
    owner(end, NULL);
    assert(!strcmp(fixture.id, "room") && room_http.sent == 3U);

reset:
    owner(reset_room, NULL);
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
