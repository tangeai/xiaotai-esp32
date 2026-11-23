// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
// Replaces legacy: tests/xmini/test_command.c::command
// Replaces legacy: tests/mz28/test_boot_button_logic.c::boot_button_logic
#include "interaction_internal.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/at.h"
#ifdef INTERACTION_STACK_TEST
#include "host_stack.h"
#endif

#define PRESS XIAOTAI_INPUT_PRESSED
#define RELEASE XIAOTAI_INPUT_RELEASED
#define RULE(role_, gesture_, count_, ctx_, intent_, val_, event_) \
    {.role = role_, .gesture = XIAOTAI_GESTURE_##gesture_, .count = count_, .context = ctx_, \
     .intent = intent_, .value = val_, .event = event_}
#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct fixture {
    atomic_uint edges, calls;
    atomic_int level;
    atomic_bool read_fails;
} s_fake[9];
static xiaotai_input_t s_parts[N(s_fake)];
static size_t s_part_count = 4;
static unsigned s_opens;
static bool s_real, s_capacity, s_profiles = true;
static const char *const roles[] = {"main", "door", "slider", "knob", "key4", "key5", "key6", "key7", "key8"};
static xiaotai_signal_t s_synced;

static struct {
    const char *name;
    int32_t value;
    char text[XIAOTAI_ARG_TEXT_MAX];
} s_received[256];
static size_t s_received_count;

static void record(const char *name, const xiaotai_args_t *args)
{
    assert(s_received_count < N(s_received));
    s_received[s_received_count].name = name;
    s_received[s_received_count].value = args->value;
    memcpy(s_received[s_received_count++].text, args->text, sizeof(args->text));
}

static unsigned count(const char *name, int value)
{
    unsigned matches = 0;
    for (size_t i = 0; i < s_received_count; ++i)
        if (!strcmp(s_received[i].name, name) && (value < 0 || s_received[i].value == value)) ++matches;
    return matches;
}

static void event(const xiaotai_event_t *ev, void *ctx) { (void)ctx; record(ev->name, &ev->args); }
static void intent(const xiaotai_args_t *args, void *ctx) { record(ctx, args); }

static xiaotai_err_t open_fake(void *ctx)
{
    assert(ctx);
    ++s_opens;
    return XIAOTAI_OK;
}

static uint32_t poll_fake(void *ctx)
{
    struct fixture *input = ctx;
    atomic_fetch_add(&input->calls, 1);
    return atomic_exchange(&input->edges, 0);
}

static xiaotai_err_t read_fake(void *ctx, char *text, size_t cap)
{
    struct fixture *input = ctx;
    if (atomic_load(&input->read_fails)) return XIAOTAI_FAIL;
    static const char payload[] = "tag-\xe5\xb0\x8f";
    assert(cap >= sizeof(payload));
    memcpy(text, payload, sizeof(payload));
    return XIAOTAI_OK;
}

static xiaotai_err_t level_fake(void *ctx, int32_t *level)
{
    struct fixture *input = ctx;
    if (atomic_load(&input->read_fails)) return XIAOTAI_FAIL;
    *level = atomic_load(&input->level);
    return XIAOTAI_OK;
}

const xiaotai_input_t *xiaotai_hal_input(size_t index)
{
    return index < s_part_count ? &s_parts[index] : NULL;
}

static void setup_parts(void)
{
    for (size_t i = 0; i < N(s_parts); ++i) {
        s_parts[i] = (xiaotai_input_t){.desc = {.role = roles[i], .events = PRESS | RELEASE},
            .self = &s_fake[i], .open = open_fake, .poll = poll_fake, .read = read_fake, .read_level = level_fake};
    }
    s_parts[1].desc.kind = XIAOTAI_INPUT_CONTACT;
    s_parts[2].desc.kind = XIAOTAI_INPUT_SLIDER;
    s_parts[2].desc.events = XIAOTAI_INPUT_LEVEL;
    s_parts[2].desc.level_min = -20;
    s_parts[2].desc.level_max = 2000;
    s_parts[3].desc.kind = XIAOTAI_INPUT_ENCODER;
    s_parts[3].desc.events = XIAOTAI_INPUT_CLICK | XIAOTAI_INPUT_DOUBLE_CLICK | XIAOTAI_INPUT_LONG_PRESS |
        XIAOTAI_INPUT_TURN_UP | XIAOTAI_INPUT_TURN_DOWN | XIAOTAI_INPUT_DATA;
}

static const xiaotai_interaction_rule_t rules[] = {
    RULE("main", CLICK, 1, "busy", "acted", 99, "hint"),
    RULE("main", CLICK, 1, NULL, "acted", 1, "hint"),
    RULE("main", CLICK, 2, NULL, "acted", 2, NULL),
    RULE("main", CLICK, 4, NULL, "acted", 4, NULL),
    RULE("main", LONG_PRESS, 0, NULL, "acted", 9, NULL),
    RULE("main", PRESS, 0, NULL, "edge", 1, NULL),
    RULE("main", RELEASE, 0, NULL, "edge", 0, NULL),
    RULE("door", INACTIVE, 0, NULL, "contact", 0, NULL),
    RULE("door", ACTIVE, 0, NULL, "contact", 1, NULL),
    RULE("slider", LEVEL, 0, NULL, "level", 6001, "hint"),
    RULE("knob", TURN_UP, 0, NULL, "missing", 6, NULL),
    RULE("knob", TURN_DOWN, 0, NULL, "acted", 7, NULL),
    RULE("knob", DATA, 0, NULL, "data", 0, NULL),
    RULE(NULL, CLICK, 1, NULL, "acted", 10, NULL),
    RULE(NULL, CLICK, 2, NULL, "acted", 11, NULL),
    RULE(NULL, LONG_PRESS, 0, NULL, "acted", 12, NULL),
};

static const xiaotai_interaction_profile_t base = {.name = "base", .rules = rules, .rule_count = N(rules),
    .flags = XIAOTAI_INTERACTION_VOID_ON_CONTEXT | XIAOTAI_INTERACTION_VOID_ON_VIEW};

static xiaotai_err_t init_rules(void)
{
    static char names[][9] = {"acted", "contact", "level", "data", "edge"};
    static const char *const events[] = {XIAOTAI_EVENT_INPUT_BEGAN, XIAOTAI_EVENT_INPUT_REJECTED, "hint"};
    for (size_t i = 0; i < N(names); ++i) assert(xiaotai_intent_handle(names[i], intent, names[i]) == XIAOTAI_OK);
    for (size_t i = 0; i < N(events); ++i) assert(xiaotai_event_subscribe(events[i], event, NULL) == XIAOTAI_OK);
    if (!s_profiles) return XIAOTAI_OK;
    if (!s_capacity) return xiaotai_interaction_register(&base);
    for (size_t i = 0; i < XIAOTAI_INTERACTION_PROFILES_MAX; ++i)
        assert(xiaotai_interaction_register(&base) == XIAOTAI_OK);
    assert(xiaotai_interaction_register(&base) == XIAOTAI_ERR_NO_MEM);
    return XIAOTAI_OK;
}

static const xiaotai_interaction_rule_t rule_a[] = {RULE("main", CLICK, 1, "kept", "acted", 20, NULL)};
static const xiaotai_interaction_rule_t rule_b[] = {RULE("main", CLICK, 1, "kept", "acted", 21, NULL)};
static const xiaotai_interaction_rule_t swallowed[] = {RULE(NULL, CLICK, 1, NULL, NULL, 0, "hint")};
static const xiaotai_interaction_profile_t layer = {.name = "layer", .rules = swallowed, .rule_count = N(swallowed)};
static const xiaotai_interaction_profile_t a = {.name = "a", .application = "a", .rules = rule_a, .rule_count = N(rule_a)};
static const xiaotai_interaction_profile_t b = {.name = "b", .application = "b", .rules = rule_b, .rule_count = N(rule_b)};

static xiaotai_err_t init_a(void) { return xiaotai_interaction_register(&a); }
static xiaotai_err_t init_b(void) { return xiaotai_interaction_register(&b); }
static void foreground(bool visible) { if (visible) xiaotai_interaction_set_context("kept"); }

static const xiaotai_module_t interaction = {.name = "interaction", .init = xiaotai_engine_interaction_init,
    .start = xiaotai_engine_interaction_start};
static const xiaotai_module_t profiles = {.name = "profiles", .init = init_rules};
static const xiaotai_module_t *const modules[] = {&interaction, &profiles};
static const xiaotai_application_t app_a = {.name = "a", .init = init_a, .foreground = foreground};
static const xiaotai_application_t app_b = {.name = "b", .init = init_b, .foreground = foreground};
static const xiaotai_application_t *const apps[] = {&app_a, &app_b};

xiaotai_err_t __real_interaction_poll_start(void);
xiaotai_err_t __wrap_interaction_poll_start(void)
{
    return s_real ? __real_interaction_poll_start() : XIAOTAI_OK;
}

static void sync_job(void *ctx) { (void)ctx; xiaotai_signal_give(&s_synced); }
static void barrier(void)
{
    assert(xiaotai_run(sync_job, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_synced, 3000));
}

#if !XIAOTAI_INTERACTION_TASK
static void owner_step(void *ctx) { interaction_poll(*(const uint32_t *)ctx); }
#endif
static void step(uint32_t now, size_t part, uint32_t events)
{
    assert(!s_real);
    if (events) atomic_fetch_or(&s_fake[part].edges, events);
#if XIAOTAI_INTERACTION_TASK
    interaction_poll(now);
#else
    assert(xiaotai_run(owner_step, &now) == XIAOTAI_OK);
    /* The polling job enqueues the resulting events after it has started. */
    barrier();
#endif
    barrier();
}

static void tap(uint32_t at) { step(at, 0, PRESS); step(at + 20, 0, RELEASE); }

static void do_clicks(void)
{
    step(0, 0, RELEASE);
    tap(10);
    step(379, 0, 0);
    assert(!count("acted", -1));
    step(380, 0, 0);
    assert(count("acted", 1) == 1);
    tap(500); tap(600);
    step(970, 0, 0);
    assert(count("acted", 2) == 1);
    tap(1000); tap(1100); tap(1200);
    step(1570, 0, 0);
    assert(count("acted", -1) == 2);
    tap(1600); tap(1700); tap(1800); tap(1900);
    assert(count("acted", 4) == 1);
    xiaotai_interaction_stats_t stats;
    xiaotai_interaction_stats(&stats);
    assert(stats.orphan_releases == 1 && count(XIAOTAI_EVENT_INPUT_BEGAN, -1) == 4);
}

static void do_long(void)
{
    tap(10);
    step(240, 0, PRESS);
    step(1439, 0, 0);
    assert(!count("acted", -1));
    step(1440, 0, 0);
    step(1450, 0, 0);
    step(1460, 0, RELEASE);
    step(2000, 0, 0);
    assert(count("acted", 9) == 1 && count("acted", -1) == 1);
    assert(count("edge", 1) == 2 && count("edge", 0) == 2);
}

static void do_wrap(void)
{
    tap(UINT32_MAX - 5U);
    step(363, 0, 0);
    assert(!count("acted", -1));
    step(364, 0, 0);
    assert(count("acted", 1) == 1);
    step(UINT32_MAX - 100U, 0, PRESS);
    step(1098, 0, 0);
    assert(!count("acted", 9));
    step(1099, 0, 0);
    assert(count("acted", 9) == 1);
}

static void switch_b(void *ctx) { (void)ctx; assert(xiaotai_app_switch("b") == XIAOTAI_OK); }

static void do_void(void)
{
    tap(10);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_VIEW_CHANGED, NULL, NULL, 0) == XIAOTAI_OK);
    barrier();
    step(400, 0, 0);
    step(500, 0, PRESS);
    xiaotai_interaction_set_context("busy");
    step(1800, 0, 0);
    step(1810, 0, RELEASE);
    assert(count("edge", 0) == 1);
    tap(1900);
    xiaotai_interaction_invalidate();
    step(2300, 0, 0);
    tap(2400);
    uint32_t id;
    assert(xiaotai_interaction_push(&layer, &id) == XIAOTAI_OK);
    tap(2800);
    xiaotai_interaction_pop(id);
    step(3200, 0, 0);
    assert(!count("acted", -1));
    xiaotai_interaction_stats_t stats;
    xiaotai_interaction_stats(&stats);
    assert(stats.invalidated == 5);
}

static void do_rules(void)
{
    tap(0);
    step(370, 0, 0);
    assert(count("acted", 1) == 1);
    xiaotai_interaction_set_context("busy");
    tap(400);
    step(770, 0, 0);
    assert(count("acted", 99) == 1);
    uint32_t id;
    assert(xiaotai_interaction_push(&layer, &id) == XIAOTAI_OK);
    tap(800);
    step(1170, 0, 0);
    assert(count("acted", -1) == 2 && count("hint", -1) == 3);
    xiaotai_interaction_pop(id);
}

static void do_apps(void)
{
    tap(0);
    step(370, 0, 0);
    assert(count("acted", 20) == 1 && !count("acted", 21));
    tap(400);
    assert(xiaotai_run(switch_b, NULL) == XIAOTAI_OK);
    barrier();
    barrier();
    step(770, 0, 0);
    assert(!count("acted", 21));
    tap(800);
    step(1170, 0, 0);
    assert(count("acted", 21) == 1);
    tap(1200); tap(1300);
    step(1670, 0, 0);
    assert(count("acted", 2) == 1);
    uint32_t id;
    assert(xiaotai_interaction_push(&layer, &id) == XIAOTAI_OK);
    tap(1800);
    step(2170, 0, 0);
    assert(count("hint", -1) == 1 && count("acted", 21) == 1);
    xiaotai_interaction_pop(id);
}

static void do_passthrough(void)
{
    step(0, 3, XIAOTAI_INPUT_CLICK | XIAOTAI_INPUT_TURN_DOWN);
    step(10, 3, XIAOTAI_INPUT_DOUBLE_CLICK);
    step(20, 3, XIAOTAI_INPUT_LONG_PRESS | XIAOTAI_INPUT_DATA);
    assert(count("acted", 10) == 1 && count("acted", 11) == 1 && count("acted", 12) == 1);
    assert(count("acted", 7) == 1 && !count(XIAOTAI_EVENT_INPUT_BEGAN, -1));
    assert(!strcmp(s_received[s_received_count - 1U].text, "tag-\xe5\xb0\x8f"));
    atomic_store(&s_fake[3].read_fails, true);
    step(30, 3, XIAOTAI_INPUT_DATA);
    assert(count("data", -1) == 1);
    xiaotai_interaction_stats_t stats;
    xiaotai_interaction_stats(&stats);
    assert(stats.data_errors == 1);
}

static void do_contact(void)
{
    step(0, 1, RELEASE);
    assert(count("contact", 0) == 1);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_VIEW_CHANGED, NULL, NULL, 0) == XIAOTAI_OK);
    barrier();
    step(10, 1, PRESS);
    assert(count("contact", 1) == 1);
    assert(xiaotai_interaction_inject("door", 0, 100) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_interaction_enable(false);
    step(20, 1, RELEASE);
    step(30, 1, PRESS);
    step(40, 1, RELEASE);
    assert(count("contact", -1) == 2);
    xiaotai_interaction_enable(true);
    step(50, 1, 0);
    assert(count("contact", 0) == 2 && count("contact", 1) == 1);
    assert(xiaotai_interaction_inject("door", 1, 100) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_interaction_inject("door", 0, 100) == XIAOTAI_OK);
    step(60, 1, 0);
    step(159, 1, 0);
    assert(count("contact", 1) == 2 && count("contact", 0) == 2);
    step(160, 1, 0);
    assert(count("contact", 0) == 3 && !count(XIAOTAI_EVENT_INPUT_BEGAN, -1));
}

static void do_inject(void)
{
    assert(xiaotai_interaction_inject("slider", 1, 0) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_interaction_inject("none", 1, 0) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_interaction_inject("main", 9, 0) == XIAOTAI_ERR_INVALID_ARG);
    step(0, 0, PRESS);
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_ERR_INVALID_STATE);
    step(20, 0, RELEASE);
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_ERR_INVALID_STATE);
    step(370, 0, 0);
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_ERR_INVALID_STATE);
    for (uint32_t t = 400; t <= 1030; t += 10) step(t, 0, 0);
    assert(count("acted", 1) == 1 && count("acted", 2) == 1);
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
    step(1040, 0, 0);
    step(1050, 0, PRESS); /* real edge takes ownership of the next gesture */
    step(1090, 0, RELEASE);
    step(1440, 0, 0);
    assert(count("acted", 1) == 2 && count("acted", 2) == 1);
}

static void do_events(void)
{
    tap(0);
    step(370, 0, 0);
    const int32_t gesture = s_received[0].value;
    assert(gesture && !strcmp(s_received[0].name, XIAOTAI_EVENT_INPUT_BEGAN));
    assert(!strcmp(s_received[3].name, "hint") && s_received[3].value == gesture);
    assert(!strcmp(s_received[4].name, "acted") && s_received[4].value == 1);
    step(400, 3, XIAOTAI_INPUT_TURN_UP);
    assert(count(XIAOTAI_EVENT_INPUT_REJECTED, -1) == 1);
    assert(!strcmp(s_received[s_received_count - 1U].text, "missing"));
}

static void noop(void *ctx) { (void)ctx; }

static void deny_begin(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < 7; ++i) assert(xiaotai_run(noop, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(sync_job, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
    atomic_store(&s_fake[0].edges, PRESS);
    interaction_poll(0);
}

static void do_saturation(void)
{
    assert(xiaotai_run(deny_begin, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_synced, 3000)); /* the previously full queue is now empty */
    step(20, 0, RELEASE);
    step(370, 0, 0);
    assert(!count(XIAOTAI_EVENT_INPUT_BEGAN, -1) && !count("acted", -1) && !count("edge", -1));
    assert(xt_input.began_lost == 1);
    tap(400);
    step(770, 0, 0);
    assert(count(XIAOTAI_EVENT_INPUT_BEGAN, -1) == 1 && count("acted", 1) == 1);
}

#if XIAOTAI_INTERACTION_TASK
static xiaotai_signal_t s_pressure_ready, s_pressure_release;
#endif

static void settle_rejected_inputs(void)
{
    atomic_store(&s_fake[3].edges, XIAOTAI_INPUT_TURN_DOWN);
    interaction_poll(370);
}

static int32_t s_overflow_ids[6];

static void overflow_rejections(void)
{
    for (size_t i = 0; i < N(s_overflow_ids); ++i) {
        atomic_store(&s_fake[3].edges, XIAOTAI_INPUT_TURN_DOWN);
        interaction_poll(800U + (uint32_t)i * 10U);
        s_overflow_ids[i] = (int32_t)xt_input.inputs[3].number;
    }
}

static void deny_settlement(void *ctx)
{
    for (unsigned i = 0; i < 7; ++i) assert(xiaotai_run(noop, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(sync_job, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
#if XIAOTAI_INTERACTION_TASK
    (void)ctx;
    xiaotai_signal_give(&s_pressure_ready);
    assert(xiaotai_signal_take(&s_pressure_release, 3000));
#else
    void (*const *settle)(void) = ctx;
    (*settle)();
#endif
}

static void with_queue_full(void (*settle)(void))
{
#if XIAOTAI_INTERACTION_TASK
    assert(xiaotai_signal_init(&s_pressure_ready) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_pressure_release) == XIAOTAI_OK);
#endif
    assert(xiaotai_run(deny_settlement, &settle) == XIAOTAI_OK);
#if XIAOTAI_INTERACTION_TASK
    assert(xiaotai_signal_take(&s_pressure_ready, 3000));
    settle();
    xiaotai_signal_give(&s_pressure_release);
#endif
    assert(xiaotai_signal_take(&s_synced, 3000));
#if XIAOTAI_INTERACTION_TASK
    xiaotai_signal_deinit(&s_pressure_release);
    xiaotai_signal_deinit(&s_pressure_ready);
#endif
}

static void do_reject_saturation(void)
{
    tap(0);
    const int32_t gesture = s_received[0].value;
    assert(gesture && count(XIAOTAI_EVENT_INPUT_BEGAN, -1) == 1);
    with_queue_full(settle_rejected_inputs);
    xiaotai_interaction_stats_t stats;
    xiaotai_interaction_stats(&stats);
    assert(stats.rejected == 2 && !count("acted", -1));
    assert(!count(XIAOTAI_EVENT_INPUT_REJECTED, -1));
    const size_t first = s_received_count;
    step(380, 0, 0);
    assert(count(XIAOTAI_EVENT_INPUT_REJECTED, -1) == 2 && !count("acted", -1));
    assert(s_received[first].value == gesture && !strcmp(s_received[first].text, "acted"));
    assert(s_received[first + 1U].value && s_received[first + 1U].value != gesture);
    assert(!strcmp(s_received[first + 1U].text, "acted"));
    step(390, 0, 0);
    assert(count(XIAOTAI_EVENT_INPUT_REJECTED, -1) == 2);
    tap(400);
    step(770, 0, 0);
    assert(count("acted", 1) == 1 && !count("acted", 7));

    char temporary_name[] = "acted";
    const xiaotai_interaction_rule_t temporary_rule = RULE("knob", TURN_DOWN, 0, NULL, temporary_name, 42, NULL);
    const xiaotai_interaction_profile_t temporary = {.name = "temporary", .rules = &temporary_rule, .rule_count = 1};
    uint32_t layer_id;
    assert(xiaotai_interaction_push(&temporary, &layer_id) == XIAOTAI_OK);
    with_queue_full(overflow_rejections);
    xiaotai_interaction_pop(layer_id);
    temporary_name[0] = 'X'; /* the poll must not reference a popped layer */
    assert(xt_input.input_count == 4 && xt_input.rejection_count == 4 && xt_input.rejections_lost == 2);
    const size_t next = s_received_count;
    step(860, 0, 0);
    assert(s_received_count == next + 4 && xt_input.rejection_count == 0);
    for (size_t i = 0; i < 4; ++i) {
        assert(!strcmp(s_received[next + i].name, XIAOTAI_EVENT_INPUT_REJECTED));
        assert(!strcmp(s_received[next + i].text, "acted"));
        assert(s_received[next + i].value == s_overflow_ids[i + 2U]);
    }
    assert(!count("acted", 42));
}

static void do_capacity(void)
{
    uint32_t ids[XIAOTAI_INTERACTION_LAYERS_MAX], id;
    assert(s_opens == XIAOTAI_INTERACTION_INPUTS_MAX);
    assert(xt_input.skipped == 1);
    for (size_t i = 0; i < N(ids); ++i) {
        assert(xiaotai_interaction_push(&layer, &ids[i]) == XIAOTAI_OK && ids[i]);
        if (i) assert(ids[i - 1U] != ids[i]);
    }
    assert(xiaotai_interaction_push(&layer, &id) == XIAOTAI_ERR_NO_MEM);
    xiaotai_interaction_pop(ids[0]);
    assert(xiaotai_interaction_push(&layer, &id) == XIAOTAI_OK);
}

static void do_level(void)
{
    for (uint32_t t = 0; t < 1000; t += 10) {
        atomic_store(&s_fake[2].level, (int)t);
        step(t, 2, XIAOTAI_INPUT_LEVEL);
    }
    assert(count("level", -1) == 10 && count("level", 0) == 1 && count("level", 900) == 1);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_VIEW_CHANGED, NULL, NULL, 0) == XIAOTAI_OK);
    barrier();
    step(1000, 2, 0);
    assert(count("level", -1) == 11 && count("level", 990) == 1);
    xiaotai_interaction_enable(false);
    atomic_store(&s_fake[2].level, 123);
    step(1110, 2, XIAOTAI_INPUT_LEVEL);
    atomic_store(&s_fake[2].level, 124);
    step(1120, 2, XIAOTAI_INPUT_LEVEL);
    xiaotai_interaction_enable(true);
    step(1130, 2, 0);
    assert(count("level", 124) == 1 && !count("level", 123));
    atomic_store(&s_fake[2].read_fails, true);
    step(1240, 2, XIAOTAI_INPUT_LEVEL);
    assert(count("level", -1) == 12 && !count(XIAOTAI_EVENT_INPUT_BEGAN, -1));
    xiaotai_interaction_stats_t stats;
    xiaotai_interaction_stats(&stats);
    assert(stats.data_errors == 1);
}

static void capture(void *ctx, const char *line)
{
    char *out = ctx;
    const size_t n = strlen(out), size = strlen(line);
    assert(n + size + 2U < 1024);
    memcpy(out + n, line, size);
    memcpy(out + n + size, "\n", 2);
}

static void do_at(void)
{
    char out[1024] = {0};
    const xiaotai_reply_t reply = {.write = capture, .ctx = out};
    assert(xiaotai_command_run("AT+KEY HOLD missing", &reply, 1000) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_command_run("AT+KEY CLICK main extra", &reply, 1000) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_command_run("AT+KEY", &reply, 1000) == XIAOTAI_OK);
    assert(strstr(out, "role=main max_gap_ms=0 late_polls=0 raw=0 stable=0 clicks=0"));
    assert(strstr(out, "role=slider") && strstr(out, "level=0"));
    assert(xiaotai_command_run("AT+KEY HOLD", &reply, 1000) == XIAOTAI_OK);
    step(0, 0, 0);
    step(1200, 0, 0);
    step(1390, 0, 0);
    xiaotai_interaction_input_t snap;
    assert(xiaotai_interaction_input(0, &snap) == XIAOTAI_OK && snap.down && snap.injected);
    step(1400, 0, 0);
    assert(xiaotai_interaction_input(0, &snap) == XIAOTAI_OK && !snap.down && !snap.injected);
    assert(count("acted", 9) == 1);
}

static void pause_owner(void *ctx)
{
    xiaotai_delay_ms(*(const uint32_t *)ctx);
}

static void wait_polls(uint32_t want)
{
    for (unsigned i = 0; i < 500 && atomic_load(&s_fake[0].calls) < want; ++i) xiaotai_delay_ms(2);
    assert(atomic_load(&s_fake[0].calls) >= want);
}

static void do_scheduler(void)
{
    assert(interaction_next_due(10, 10) == 20);
    assert(interaction_next_due(10, 19) == 20);
    assert(interaction_next_due(10, 30) == 40);
    assert(interaction_next_due(UINT32_MAX - 9U, UINT32_MAX - 9U) == 0);
    wait_polls(3);
    uint32_t wait = 60;
    uint32_t before = atomic_load(&s_fake[0].calls);
    assert(xiaotai_run(pause_owner, &wait) == XIAOTAI_OK);
    barrier();
    wait_polls(before + 1U);
    xiaotai_interaction_stats_t stats;
    xiaotai_interaction_stats(&stats);
#if !XIAOTAI_INTERACTION_TASK
    assert(stats.late_polls >= 1 && stats.max_gap_ms >= 60);
#else
    assert(atomic_load(&s_fake[0].calls) >= before + 3U);
#endif
}

static void do_gap(void)
{
    xiaotai_interaction_stats_t stats;
    step(10, 0, 0);
    step(40, 0, 0);
    xiaotai_interaction_stats(&stats);
    assert(stats.polls == 2 && stats.max_gap_ms == 30 && !stats.late_polls);
    step(80, 0, 0);
    xiaotai_interaction_stats(&stats);
    assert(stats.max_gap_ms == 40 && !stats.late_polls);
    step(140, 0, 0);
    xiaotai_interaction_stats(&stats);
    assert(stats.polls == 4 && stats.max_gap_ms == 60 && stats.late_polls == 1);
}

static void reset(void *ctx) { (void)ctx; xiaotai_interaction_reset(); }

int main(int argc, char **argv)
{
#if defined(INTERACTION_STACK_TEST)
    const char *scenario = argc > 1 ? argv[1] : "stack";
#elif XIAOTAI_INTERACTION_TASK
    const char *scenario = argc > 1 ? argv[1] : "task_poll";
#else
    const char *scenario = argc > 1 ? argv[1] : "clicks";
#endif
    bool has_apps = !strcmp(scenario, "apps");
    const bool absent = !strcmp(scenario, "no_input");
    s_capacity = !strcmp(scenario, "capacity");
    s_profiles = strcmp(scenario, "no_profile") != 0;
    const bool measure = !strcmp(scenario, "stack");
    s_real = !strcmp(scenario, "owner_poll") || !strcmp(scenario, "task_poll") || !s_profiles || absent;
    s_part_count = absent ? 0 : s_capacity ? 9 : measure ? 8 : 4;
    setup_parts();
    assert(xiaotai_signal_init(&s_synced) == XIAOTAI_OK);
    const xiaotai_framework_config_t config = {
        .modules = modules, .module_count = N(modules),
        .applications = has_apps ? apps : NULL, .application_count = has_apps ? N(apps) : 0,
        .queue_depth = (!strcmp(scenario, "saturation") || !strcmp(scenario, "reject_saturation")) ? 8U : 0U,
    };
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    barrier();
    if (!s_profiles || absent) {
        assert(s_opens == 0 && xt_input.inputs == NULL && atomic_load(&s_fake[0].calls) == 0);
    } else if (s_real) {
        do_scheduler();
    } else if (measure) {
#ifdef INTERACTION_STACK_TEST
        atomic_store(&s_fake[0].edges, PRESS);
        atomic_store(&s_fake[1].edges, RELEASE);
        atomic_store(&s_fake[2].edges, XIAOTAI_INPUT_LEVEL);
        atomic_store(&s_fake[3].edges, XIAOTAI_INPUT_DATA | XIAOTAI_INPUT_TURN_UP);
        for (size_t i = 4; i < 8; ++i) atomic_store(&s_fake[i].edges, PRESS);
        uint32_t now = 0;
        size_t used;
        assert(xiaotai_host_stack_measure(owner_step, &now, &used));
        printf("interaction owner poll stack: %zu bytes\n", used);
        assert(used <= 3072);
#else
        assert(false);
#endif
    } else {
        static const struct { const char *name; void (*run)(void); } cases[] = {
            {"clicks", do_clicks}, {"long", do_long}, {"wrap", do_wrap}, {"void", do_void},
            {"rules", do_rules}, {"apps", do_apps}, {"passthrough", do_passthrough}, {"contact", do_contact},
            {"inject", do_inject}, {"events", do_events}, {"capacity", do_capacity}, {"level", do_level},
            {"at", do_at}, {"gap", do_gap}, {"saturation", do_saturation}, {"reject_saturation", do_reject_saturation},
        };
        size_t i = 0;
        while (i < N(cases) && strcmp(cases[i].name, scenario)) ++i;
        assert(i < N(cases));
        cases[i].run();
    }
    assert(xiaotai_run(reset, NULL) == XIAOTAI_OK);
    barrier();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&s_synced);
    return 0;
}
