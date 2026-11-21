// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_feedback.c
#include "feedback_internal.h"
#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#ifdef FEEDBACK_STACK
#include "host_stack.h"
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void feedback_fixture_outputs(xiaotai_output_t *parts, size_t size);

#define SOLID(r, g, b) {.kind = XIAOTAI_LIGHT_SOLID, .level_max = 255, .color = {r, g, b}}

enum { NFC_RELAY, NFC_GATE, CLAIMED_LIGHT, OTHER_RELAY, STATUS, AUX, VIBRATION, PART_COUNT };
static xiaotai_output_t parts[PART_COUNT];
static atomic_uint clock_ms;
static struct {
    size_t calls;
    size_t pixels;
    uint8_t rgb[3U * 17U];
    uint32_t written_at;
    bool fail;
} frames[PART_COUNT];

static xiaotai_signal_t barrier, waiting;
#if XIAOTAI_FEEDBACK_TASK
static atomic_uint worker_wait_ms, worker_wait_at;
static _Thread_local uint32_t worker_sample;
static _Thread_local bool worker_has_sample;
#endif
static atomic_uint worker_starts;
/* The port deliberately retains detached host task handles after exit. */
static xiaotai_task_t worker_handle;
static xiaotai_timer_fn_t timer_callback;
static void *timer_ctx;
static uint32_t timer_due;
static unsigned timer_starts;

int64_t __wrap_xiaotai_time_us(void)
{
    const uint32_t now = atomic_load(&clock_ms);
#if XIAOTAI_FEEDBACK_TASK
    if (!worker_has_sample && xiaotai_task_is_current(&g_feedback.task)) {
        worker_sample = now;
        worker_has_sample = true;
    }
#endif
    return (int64_t)now * 1000;
}

xiaotai_err_t __wrap_xiaotai_timer_start(uint32_t delay, xiaotai_timer_fn_t handler, void *ctx, uint32_t *id)
{
    assert(xiaotai_on_owner() && !timer_callback && id);
    timer_due = atomic_load(&clock_ms) + delay;
    timer_callback = handler;
    timer_ctx = ctx;
    *id = ++timer_starts;
    return XIAOTAI_OK;
}

void __wrap_xiaotai_timer_cancel(uint32_t id)
{
    assert(xiaotai_on_owner());
    if (id == timer_starts) timer_callback = NULL;
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task)
{
    const xiaotai_err_t result = __real_xiaotai_task_create(cfg, task);
    if (!strcmp(cfg->name, "xt_feedback") && result == XIAOTAI_OK) {
        worker_handle = *task;
        atomic_fetch_add(&worker_starts, 1U);
    }
    return result;
}

bool __real_xiaotai_task_wait_notify(uint32_t timeout_ms);
bool __wrap_xiaotai_task_wait_notify(uint32_t timeout_ms)
{
#if XIAOTAI_FEEDBACK_TASK
    if (xiaotai_task_is_current(&g_feedback.task)) {
        assert(worker_has_sample);
        atomic_store(&worker_wait_ms, timeout_ms);
        atomic_store(&worker_wait_at, worker_sample);
        worker_has_sample = false;
        xiaotai_signal_give(&waiting);
        /* Test monotonic time only advances explicitly. The worker runs and
         * selects its real delay, but a test notification is its clock edge. */
        return __real_xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    }
#endif
    return __real_xiaotai_task_wait_notify(timeout_ms);
}

static xiaotai_err_t write_pixels(void *self, const uint8_t *rgb, size_t n)
{
    const ptrdiff_t i = (xiaotai_output_t *)self - parts;
    assert(i >= 0 && i < PART_COUNT && n == parts[i].desc.pixels && n <= 17U);
    assert(xiaotai_on_owner() == !XIAOTAI_FEEDBACK_TASK);
    frames[i].calls++;
    frames[i].pixels = n;
    frames[i].written_at = atomic_load(&clock_ms);
    memcpy(frames[i].rgb, rgb, n * 3U);
    return frames[i].fail ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static xiaotai_err_t set_pixel(void *self, uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t rgb[] = {r, g, b};
    return write_pixels(self, rgb, 1U);
}

static void board(bool none)
{
    const xiaotai_output_desc_t descs[] = {
        {XIAOTAI_OUTPUT_RELAY, "door", 1}, {XIAOTAI_OUTPUT_RELAY, "door.gate", 1},
        {XIAOTAI_OUTPUT_RGB_LIGHT, "door.lamp", 1}, {XIAOTAI_OUTPUT_RELAY, "doorbell_chime", 1},
        {XIAOTAI_OUTPUT_RGB_LIGHT, "status", 17}, {XIAOTAI_OUTPUT_LIGHT, "aux", 1},
        {XIAOTAI_OUTPUT_VIBRATOR, "buzz", 1},
    };
    for (size_t i = 0; i < PART_COUNT; ++i) {
        parts[i] = (xiaotai_output_t){.desc = descs[i], .set = set_pixel, .write = write_pixels, .self = &parts[i]};
    }
#ifdef FEEDBACK_STACK
    parts[STATUS].desc.pixels = 16;
#endif
    feedback_fixture_outputs(parts, none ? 0U : PART_COUNT);
}

static const xiaotai_feedback_output_t blocked_routes[] = {
    {"door", SOLID(11, 0, 0)}, {"door.gate", SOLID(22, 0, 0)}, {"door.lamp", SOLID(33, 0, 0)},
    {"doorbell_chime", SOLID(44, 0, 0)},
};
static const xiaotai_feedback_output_t auxiliary[] = {
    {"aux", SOLID(1, 17, 37)}, {"missing", SOLID(9, 9, 9)},
};
static const xiaotai_feedback_state_t states[] = {
    {.name = "first", .priority = 1, .pattern = SOLID(26, 0, 0), .outputs = blocked_routes, .output_count = 4},
    {.name = "high", .priority = 10, .pattern = SOLID(0, 87, 0), .outputs = auxiliary, .output_count = 2,
     .vibration = {.strength = 200, .on_ms = 40, .off_ms = 40}},
    {.name = "tie", .priority = 10, .pattern = SOLID(0, 0, 46)},
};
static const xiaotai_feedback_effect_t effects[] = {
    {.name = "pressed", .event = "fixture.press", .pattern = SOLID(89, 99, 109), .duration_ms = 180},
    {.name = "warning", .pattern = SOLID(91, 101, 111), .duration_ms = 55},
    {.name = "pulse", .vibration = {.strength = 177, .repeat = 1, .on_ms = 60}},
    {.name = "scan", .pattern = {.kind = XIAOTAI_LIGHT_ALTERNATE, .color = {229, 0, 0},
                               .level_max = 255, .period_ms = 70}, .duration_ms = 180},
};
static const xiaotai_feedback_table_t table = {
    .states = states, .state_count = sizeof(states) / sizeof(states[0]),
    .effects = effects, .effect_count = sizeof(effects) / sizeof(effects[0]),
};

static xiaotai_err_t register_table(void)
{
    static xiaotai_output_claim_t claim = {.family = "door", .owner = "fixture"};
    assert(xiaotai_hal_output_claim(&claim) == XIAOTAI_OK);
    return xiaotai_feedback_register(&table);
}

static void sync_job(void *arg) { xiaotai_signal_give(arg); }
static void owner_barrier(void)
{
    assert(xiaotai_run(sync_job, &barrier) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&barrier, 5000U));
}

static void setup(bool none)
{
    board(none);
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    assert(xiaotai_signal_init(&barrier) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&waiting) == XIAOTAI_OK);
    static const xiaotai_module_t engine = {.name = "feedback", .init = xiaotai_engine_feedback_init,
                                           .start = xiaotai_engine_feedback_start};
    static const xiaotai_application_t app = {.name = "fixture", .init = register_table};
    static const xiaotai_module_t *const modules[] = {&engine};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t config = {
        .modules = modules, .module_count = 1U, .applications = apps, .application_count = 1U,
    };
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    owner_barrier();
#if XIAOTAI_FEEDBACK_TASK
    if (!none) assert(xiaotai_signal_take(&waiting, 5000U));
#endif
}

#if XIAOTAI_FEEDBACK_TASK
static void wait_worker(uint32_t now)
{
    do {
        assert(xiaotai_signal_take(&waiting, 5000U));
    } while (atomic_load(&worker_wait_at) != now);
}
#else
static void timer_job(void *arg)
{
    (void)arg;
    assert(timer_callback && (int32_t)(atomic_load(&clock_ms) - timer_due) >= 0);
    xiaotai_timer_fn_t callback = timer_callback;
    timer_callback = NULL;
    callback(timer_ctx);
}
#endif

static void step(uint32_t now)
{
    atomic_store(&clock_ms, now);
#if XIAOTAI_FEEDBACK_TASK
    /* Consume the previous idle receipt before forcing work at this time. */
    (void)xiaotai_signal_take(&waiting, 0U);
    fb_output_kick(NULL);
    wait_worker(now);
#else
    assert(xiaotai_run(timer_job, NULL) == XIAOTAI_OK);
    owner_barrier();
#endif
}

static void check_rgb(size_t part, unsigned r, unsigned g, unsigned b)
{
    assert(frames[part].pixels);
    for (size_t i = 0; i < 3U * frames[part].pixels; i += 3U) {
        assert(frames[part].rgb[i] == r && frames[part].rgb[i + 1U] == g && frames[part].rgb[i + 2U] == b);
    }
}

static void priority(void)
{
    const uint32_t base = 1000U;
    atomic_store(&clock_ms, base);
    assert(xiaotai_feedback_set("first", true) == XIAOTAI_OK);
    owner_barrier();
    check_rgb(STATUS, 26, 0, 0);
    assert(!strcmp(xiaotai_feedback_shown(), "first"));
    assert(xiaotai_feedback_set("high", true) == XIAOTAI_OK);
    assert(xiaotai_feedback_set("tie", true) == XIAOTAI_OK);
    assert(xiaotai_feedback_is_set("tie"));
    step(base + 40U);
    check_rgb(STATUS, 0, 87, 0);
    check_rgb(AUX, 37, 0, 0);
    check_rgb(VIBRATION, 0, 0, 0);
    assert(!strcmp(xiaotai_feedback_shown(), "high"));
    step(base + 80U);
    check_rgb(VIBRATION, 200, 0, 0);
    assert(xiaotai_feedback_set("high", false) == XIAOTAI_OK);
    step(base + 120U);
    check_rgb(STATUS, 0, 0, 46);
    check_rgb(AUX, 0, 0, 0);
    assert(!strcmp(xiaotai_feedback_shown(), "tie"));
    assert(xiaotai_feedback_set("unknown", true) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_feedback_register(&table) == XIAOTAI_ERR_INVALID_STATE);
}

static xiaotai_err_t result;
static unsigned completions;
static void completion(xiaotai_err_t status, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &result);
    result = status;
    ++completions;
}

static void covers(uint32_t base)
{
    atomic_store(&clock_ms, base);
    assert(xiaotai_feedback_set("first", true) == XIAOTAI_OK);
    owner_barrier();
    assert(xiaotai_event_emit("fixture.press", NULL, NULL, 0U) == XIAOTAI_OK);
    owner_barrier();
    step(base + 40U);
    check_rgb(STATUS, 89, 99, 109);
    assert(!xiaotai_feedback_shown());
    const xiaotai_feedback_options_t options = {.done = completion, .ctx = &result};
    assert(xiaotai_feedback_fire("warning", &options) == XIAOTAI_OK);
    owner_barrier();
    assert(completions == 1U && result == XIAOTAI_OK);
    assert(xiaotai_feedback_fire("pulse", NULL) == XIAOTAI_OK);
    assert(xiaotai_feedback_fire("missing", NULL) == XIAOTAI_ERR_NOT_FOUND);
    step(base + 80U);
    check_rgb(STATUS, 91, 101, 111);
    check_rgb(VIBRATION, 177, 0, 0);
    step(base + 120U);
    check_rgb(STATUS, 26, 0, 0);
    check_rgb(VIBRATION, 0, 0, 0);
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    assert(stats.fired == 3U);
}

static void roles(void)
{
    const uint32_t base = 1000U;
    atomic_store(&clock_ms, base);
    assert(xiaotai_feedback_set("first", true) == XIAOTAI_OK);
    owner_barrier();
    assert(xiaotai_feedback_set("high", true) == XIAOTAI_OK);
    step(base + 40U);
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    assert(stats.skipped_outputs == 4U);
    for (size_t i = 0; i < STATUS; ++i) assert(!frames[i].calls);
    assert(frames[STATUS].pixels == 17U && frames[AUX].pixels == 1U);
    check_rgb(AUX, 37, 0, 0);
    assert(xiaotai_feedback_fire("scan", NULL) == XIAOTAI_OK);
    step(base + 80U);
    assert(frames[STATUS].rgb[0] == 0U && frames[STATUS].rgb[3] == 229U);
    assert(!memcmp(frames[STATUS].rgb, frames[STATUS].rgb + 16U * 3U, 3U));
}

static void no_task(void)
{
    assert(!atomic_load(&worker_starts) && !timer_starts && !g_feedback.output_count);
    assert(xiaotai_feedback_set("first", true) == XIAOTAI_OK);
    assert(xiaotai_feedback_fire("pressed", NULL) == XIAOTAI_OK);
    owner_barrier();
    assert(!atomic_load(&worker_starts) && !timer_starts && !g_feedback.scheduled);
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    assert(stats.fired == 1U && stats.skipped_outputs == 0U);
}

static void scheduler(void)
{
    atomic_store(&clock_ms, 1000);
    assert(xiaotai_feedback_set("first", true) == XIAOTAI_OK);
    owner_barrier();
#if XIAOTAI_FEEDBACK_TASK
    wait_worker(1000U);
    assert(atomic_load(&worker_starts) == 1U && atomic_load(&worker_wait_ms) == 40U);
    assert(!timer_starts);
#else
    assert(!atomic_load(&worker_starts) && timer_callback && timer_due == 1040U);
#endif
    assert(frames[STATUS].calls == 1U && frames[STATUS].written_at == 1000U);
    step(1040U);
    assert(frames[STATUS].calls == 2U && frames[STATUS].written_at == 1040U);
    check_rgb(STATUS, 26, 0, 0);
    frames[STATUS].fail = true;
    step(1180U);
    assert(frames[STATUS].calls == 3U && frames[STATUS].written_at == 1180U);
    check_rgb(STATUS, 26, 0, 0);
    xiaotai_feedback_stats_t stats;
    xiaotai_feedback_stats(&stats);
    assert(stats.late_frames == 1U && stats.light_errors == 1U);
#if XIAOTAI_FEEDBACK_TASK
    assert(atomic_load(&worker_wait_ms) == 40U);
#else
    assert(timer_callback && timer_due == 1220U);
#endif
    frames[STATUS].fail = false;
    assert(xiaotai_feedback_set("first", false) == XIAOTAI_OK);
    step(1220U);
    assert(frames[STATUS].calls == 4U);
    check_rgb(STATUS, 0, 0, 0);
#if XIAOTAI_FEEDBACK_TASK
    assert(atomic_load(&worker_wait_ms) == XIAOTAI_WAIT_FOREVER);
#else
    assert(!timer_callback);
#endif
    /* An idle-to-active transition may not bypass the 40 ms wire cadence. */
    atomic_store(&clock_ms, 1221U);
    assert(xiaotai_feedback_set("first", true) == XIAOTAI_OK);
    owner_barrier();
#if XIAOTAI_FEEDBACK_TASK
    wait_worker(1221U);
    assert(atomic_load(&worker_wait_ms) == 39U);
#else
    assert(timer_callback && timer_due == 1260U);
#endif
    assert(frames[STATUS].calls == 4U);
    step(1260U);
    assert(frames[STATUS].calls == 5U);
    check_rgb(STATUS, 26, 0, 0);
}

static xiaotai_err_t capacity(void)
{
    assert(xiaotai_feedback_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    static xiaotai_feedback_state_t rows[XIAOTAI_FEEDBACK_STATES_MAX];
    static char names[XIAOTAI_FEEDBACK_STATES_MAX][16];
    for (size_t i = 0; i < XIAOTAI_FEEDBACK_STATES_MAX; ++i) {
        const int length = snprintf(names[i], sizeof(names[i]), "row%u", (unsigned)i);
        assert(length > 0 && (size_t)length < sizeof(names[i]));
        rows[i].name = names[i];
    }
    static const xiaotai_feedback_table_t full = {.states = rows, .state_count = XIAOTAI_FEEDBACK_STATES_MAX};
    assert(xiaotai_feedback_register(&full) == XIAOTAI_OK);
    const xiaotai_feedback_table_t too_many = {.states = states, .state_count = 1U};
    assert(xiaotai_feedback_register(&too_many) == XIAOTAI_ERR_NO_MEM);
    const xiaotai_feedback_effect_t duplicate = {.name = rows[0].name};
    const xiaotai_feedback_table_t clash = {.effects = &duplicate, .effect_count = 1};
    assert(xiaotai_feedback_register(&clash) == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_feedback_output_t out = {.role = "status"};
    const xiaotai_feedback_effect_t invalid = {.name = "bad", .outputs = &out,
                                               .output_count = XIAOTAI_FEEDBACK_OUTPUTS_MAX + 1U};
    const xiaotai_feedback_table_t invalid_table = {.effects = &invalid, .effect_count = 1};
    assert(xiaotai_feedback_register(&invalid_table) == XIAOTAI_ERR_INVALID_ARG);
    static const xiaotai_feedback_table_t spare[XIAOTAI_FEEDBACK_TABLES_MAX];
    for (size_t i = 1; i < XIAOTAI_FEEDBACK_TABLES_MAX; ++i) assert(xiaotai_feedback_register(&spare[i]) == XIAOTAI_OK);
    assert(xiaotai_feedback_register(&spare[0]) == XIAOTAI_ERR_NO_MEM);
    return XIAOTAI_OK;
}

#ifdef FEEDBACK_STACK
static void measure(void *ctx)
{
    (void)ctx;
    size_t used = 0;
    assert(xiaotai_host_stack_measure(timer_job, NULL, &used));
    printf("feedback owner frame used %u bytes of stack\n", (unsigned)used);
    assert(used <= XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES);
}

static void stack(void)
{
    atomic_store(&clock_ms, 1000U);
    assert(xiaotai_feedback_set("first", true) == XIAOTAI_OK);
    owner_barrier();
    assert(frames[STATUS].pixels == 16U);
    atomic_store(&clock_ms, 1040U);
    assert(xiaotai_run(measure, NULL) == XIAOTAI_OK);
    owner_barrier();
    assert(frames[STATUS].calls == 2U);
}
#endif

int main(void)
{
    if (!strcmp(FEEDBACK_TEST, "capacity")) {
        static const xiaotai_module_t engine = {.name = "feedback", .init = xiaotai_engine_feedback_init};
        static const xiaotai_application_t app = {.name = "capacity", .init = capacity};
        static const xiaotai_module_t *const modules[] = {&engine};
        static const xiaotai_application_t *const apps[] = {&app};
        static const xiaotai_framework_config_t config = {
            .modules = modules, .module_count = 1, .applications = apps, .application_count = 1,
        };
        assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
        xiaotai_framework_reset();
        xiaotai_feedback_reset();
        xiaotai_at_reset();
        return 0;
    }
    setup(!strcmp(FEEDBACK_TEST, "no_task"));
    if (!strcmp(FEEDBACK_TEST, "priority")) priority();
    else if (!strcmp(FEEDBACK_TEST, "effect_cover")) covers(1000U);
    else if (!strcmp(FEEDBACK_TEST, "time_wrap")) covers(UINT32_MAX - 50U);
    else if (!strcmp(FEEDBACK_TEST, "roles")) roles();
    else if (!strcmp(FEEDBACK_TEST, "no_task")) no_task();
    else if (!strcmp(FEEDBACK_TEST, "frames")) scheduler();
#ifdef FEEDBACK_STACK
    else if (!strcmp(FEEDBACK_TEST, "stack")) stack();
#endif
    else assert(false);
    xiaotai_feedback_reset();
    assert(!timer_callback);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&waiting);
    xiaotai_signal_deinit(&barrier);
    return 0;
}
