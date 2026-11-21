// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai_mem_host.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef UI_OWNER_STACK
#include "host_stack.h"
#endif

static atomic_uint ms;
static xiaotai_timer_fn_t timer_fn;
static void *timer_ctx;
static uint32_t timer_due, timer_serial;
static xiaotai_signal_t synchronized, waiting;
static xiaotai_task_t worker;
static const xiaotai_ui_host_t *host;
static uint32_t stack_wanted = 1024;
static uint8_t frame[1024];
static uint8_t sent[32];
static size_t writes, renders, posted, sleeps, wakes, stopped;
static uint8_t light;
static xiaotai_err_t fail_light, fail_frame, last_frame;
static bool worker_mode, rotated_mode;
static bool touch_mode, real_down;
static size_t real_polls;
static xiaotai_signal_t capture_entered, capture_release;
static atomic_bool hold_capture;

int64_t __wrap_xiaotai_time_us(void) { return (int64_t)atomic_load(&ms) * 1000; }
xiaotai_err_t __wrap_xiaotai_timer_start(uint32_t delay, xiaotai_timer_fn_t fn, void *ctx, uint32_t *id)
{
    assert(xiaotai_on_owner() && !timer_fn);
    timer_due = atomic_load(&ms) + delay;
    timer_fn = fn;
    timer_ctx = ctx;
    *id = ++timer_serial;
    return XIAOTAI_OK;
}
void __wrap_xiaotai_timer_cancel(uint32_t id)
{
    assert(xiaotai_on_owner());
    if (id == timer_serial) timer_fn = NULL;
}

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *, xiaotai_task_t *);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *out)
{
    if (strcmp(cfg->name, "xt_ui")) return __real_xiaotai_task_create(cfg, out);
    assert(worker_mode && cfg->stack == XIAOTAI_MEM_BULK && cfg->stack_bytes >= stack_wanted);
    const xiaotai_err_t err = __real_xiaotai_task_create(cfg, out);
    if (err == XIAOTAI_OK) worker = *out;
    return err;
}

bool __real_xiaotai_task_wait_notify(uint32_t);
bool __wrap_xiaotai_task_wait_notify(uint32_t delay)
{
    const ui_display_t *d = ui_display_at(0);
    if (worker_mode && d && xiaotai_task_is_current(&d->driver.task)) {
        assert(delay <= 100);
        xiaotai_signal_give(&waiting);
        return __real_xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    }
    return __real_xiaotai_task_wait_notify(delay);
}

static bool on_display(void)
{
    return worker_mode ? xiaotai_task_is_current(&ui_display_at(0)->driver.task) : xiaotai_on_owner();
}

static xiaotai_err_t set_light(void *self, uint8_t pct)
{
    (void)self;
    assert(on_display());
    if (fail_light) return fail_light;
    light = pct;
    return XIAOTAI_OK;
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    (void)self;
    assert(on_display() && !area->x && !area->y);
    if (rotated_mode) {
        assert(area->width == 16 && area->height == 8 && pixels != frame);
        uint8_t expected[16] = {0};
        expected[12] = 4U;
        expected[3] = 32U;
        assert(!memcmp(expected, pixels, sizeof(expected)));
        ++writes;
        return XIAOTAI_OK;
    }
    assert(area->width == 128 && area->height == 64);
    assert(writes < sizeof(sent) && pixels == frame);
    sent[writes++] = *(const uint8_t *)pixels;
    for (size_t i = 1; i < sizeof(frame); ++i) assert(frame[i] == frame[0]);
    return fail_frame;
}

static xiaotai_err_t read_touch(void *self, xiaotai_touch_point_t *point, size_t capacity, size_t *count)
{
    (void)self;
    assert(on_display() && capacity == 1);
    ++real_polls;
    *point = (xiaotai_touch_point_t){113, 48};
    *count = real_down;
    return XIAOTAI_OK;
}

static int match(const xiaotai_display_desc_t *desc) { return desc->width == (rotated_mode ? 8 : 128) ? 99 : 0; }
static xiaotai_err_t start(const xiaotai_ui_host_t *h, void **state)
{
    assert(on_display());
    host = h;
    *state = frame;
    return XIAOTAI_OK;
}
static void render(void *state, const xiaotai_ui_scene_t *scene)
{
    assert(on_display() && state == frame);
    assert(ui_driver_wait_guard(ui_display_at(0)) == XIAOTAI_ERR_INVALID_STATE);
    ++renders;
    if (!scene->page.present) return;
#if XIAOTAI_UI_CAPTURE
    if (ui_display_at(0)->frame.capture && atomic_load(&hold_capture)) {
        xiaotai_signal_give(&capture_entered);
        assert(xiaotai_signal_take(&capture_release, 5000U));
    }
    if (ui_display_at(0)->frame.capture) xiaotai_ui_host_invalidate(host);
#endif
    if (rotated_mode) {
        memset(frame, 0, 16);
        frame[2] = 8U;
        frame[13] = 16U;
        last_frame = xiaotai_ui_host_present(host, frame, 16);
        return;
    }
    memset(frame, (scene->page.view.title ? scene->page.view.title[0] : 0) + scene->page.timer_s, sizeof(frame));
    last_frame = xiaotai_ui_host_present(host, frame, sizeof(frame));
}
static void sleep_theme(void *state, bool asleep)
{
    assert(on_display() && state == frame);
    if (asleep) ++sleeps;
    else ++wakes;
}
static void stop(void *state)
{
    assert(on_display() && state == frame);
    ++stopped;
}
static void tap_handler(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    ++posted;
}

static void diag_guard(void *unused)
{
    (void)unused;
    xiaotai_ui_capture_t capture = {.size = 7};
    size_t count = 7;
    assert(xiaotai_ui_capture(0, &capture, 500) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_ui_controls(0, NULL, 0, &count, 500) == XIAOTAI_ERR_INVALID_STATE);
    assert(!capture.pixels && !capture.size && !count);
}

static size_t collect(void *state, xiaotai_ui_control_t *out, size_t capacity)
{
    assert(on_display() && state == frame);
    diag_guard(NULL);
    if (capacity) out[0] = (xiaotai_ui_control_t){.x = 4, .y = 8, .width = 24, .height = 16,
        .layer = XIAOTAI_LAYER_PAGE, .element = XIAOTAI_UI_ELEMENT_ACTION};
    if (capacity > 1U) out[1] = (xiaotai_ui_control_t){.layer = XIAOTAI_LAYER_PAGE,
        .element = XIAOTAI_UI_ELEMENT_OTHER, .text = "custom", .intent = "must-be-empty",
        .state = XIAOTAI_STATE_CHECKED};
    return 2;
}

static xiaotai_err_t register_theme(void)
{
    static xiaotai_theme_t theme = {.name = "fixture", .backend = XIAOTAI_UI_BACKEND_FRAME,
        .match = match, .start = start, .render = render, .sleep = sleep_theme, .controls = collect,
        .stop = stop};
    theme.stack_bytes = stack_wanted;
    const xiaotai_err_t err = xiaotai_ui_theme_register(&theme);
    return err == XIAOTAI_OK ? xiaotai_intent_handle("test.tap", tap_handler, NULL) : err;
}

typedef struct { void (*fn)(void *); void *arg; } job_t;
static void job_runner(void *arg)
{
    const job_t *job = arg;
    if (job->fn) job->fn(job->arg);
    xiaotai_signal_give(&synchronized);
}
static void run(void (*fn)(void *), void *arg)
{
    const job_t job = {fn, arg};
    assert(xiaotai_run(job_runner, (void *)&job) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&synchronized, 5000U));
}

static void setup(bool dark)
{
    atomic_store(&ms, 1000);
    xiaotai_display_desc_t desc = ui_fake_desc(rotated_mode ? 8 : 128, rotated_mode ? 16 : 64,
                                               XIAOTAI_PIXEL_MONO1_VLSB);
    if (rotated_mode) desc.rotation = 1;
    desc.touch = touch_mode;
    ui_fake_board_set_displays(&desc, 1);
    xiaotai_display_t *part = (xiaotai_display_t *)ui_fake_display(0);
    part->set_backlight = set_light;
    part->present = present;
    if (touch_mode) part->read_touch = read_touch;
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    assert(xiaotai_signal_init(&synchronized) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&waiting) == XIAOTAI_OK);
    const xiaotai_ui_config_t ui = {.dark_touch_operates = dark};
    assert(xiaotai_ui_configure(&ui) == XIAOTAI_OK);
    static const xiaotai_module_t engine = {.name = "ui", .init = xiaotai_engine_ui_init,
                                            .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "fixture", .init = register_theme};
    static const xiaotai_module_t *const modules[] = {&engine};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 1U,
                                           .applications = apps, .application_count = 1U};
    assert(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    run(NULL, NULL);
    if (worker_mode) assert(xiaotai_signal_take(&waiting, 5000U));
    assert(host && light == 100);
    assert(ui_driver_wait_guard(ui_display_at(0)) == XIAOTAI_OK);
}

static void firing(void *unused)
{
    (void)unused;
    assert(timer_fn && (int32_t)(atomic_load(&ms) - timer_due) >= 0);
    xiaotai_timer_fn_t fn = timer_fn;
    timer_fn = NULL;
    fn(timer_ctx);
}

static void advance(uint32_t at)
{
    atomic_store(&ms, at);
    if (worker_mode) {
        xiaotai_task_notify(&worker);
        assert(xiaotai_signal_take(&waiting, 5000U));
    } else {
        run(firing, NULL);
    }
}

static void show(void *opaque)
{
    const xiaotai_view_t *view = opaque;
    xiaotai_view_show(view);
}

static xiaotai_view_t view(const char *title)
{
    static const xiaotai_view_action_t action = {.intent = "test.tap", .label = "Tap",
                                                 .role = XIAOTAI_ROLE_PRIMARY};
    return (xiaotai_view_t){.id = "page", .title = title, .kind = XIAOTAI_VIEW_HOME,
        .flags = XIAOTAI_VIEW_MAY_SLEEP, .actions = &action, .action_count = 1};
}

static void show_pair(void *opaque)
{
    xiaotai_view_t *views = opaque;
    show(&views[0]);
    show(&views[1]);
}

static void wait_step(void)
{
    if (worker_mode) assert(xiaotai_signal_take(&waiting, 5000U));
    else advance(atomic_load(&ms));
}

static void frames_case(void)
{
    setup(false);
    if (!worker_mode) advance(1000);
    if (!worker_mode) assert(!timer_fn);
    xiaotai_view_t pages[] = {view("A"), view("B")};
    if (worker_mode) {
        run(show, &pages[1]);
        wait_step();
    } else {
        const uint32_t before = timer_serial;
        run(show_pair, pages);
        assert(timer_serial == before + 1);
        advance(1000);
    }
    assert(writes == 1 && sent[0] == 'B');
    pages[1].status = "changed but same pixels";
    run(show, &pages[1]);
    wait_step();
    assert(writes == 1);
    fail_frame = XIAOTAI_ERR_TIMEOUT;
    run(show, &pages[0]);
    wait_step();
    assert(last_frame == XIAOTAI_ERR_TIMEOUT && writes == 2);
    pages[0].title = "C";
    run(show, &pages[0]);
    wait_step();
    assert(writes == 2 && last_frame == XIAOTAI_OK);
    fail_frame = XIAOTAI_OK;
    assert(xiaotai_ui_refresh(0) == XIAOTAI_OK);
    run(NULL, NULL);
    wait_step();
    assert(writes == 3 && !memcmp(sent, "BAC", 3));
    xiaotai_ui_info_t info;
    assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.present_errors == 1 && !info.present_fault);
}

static void timers_case(void)
{
    setup(false);
    advance(1000);
    assert(!timer_fn);
    xiaotai_view_t page = view("J");
    page.timer_mode = XIAOTAI_TIMER_ELAPSED;
    run(show, &page);
    advance(1000);
    assert(timer_fn && timer_due == 2000);
    advance(2500);
    xiaotai_ui_info_t info;
    assert(timer_fn && timer_due == 3000);
    assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.late_steps == 0 && info.owner_timer);
    advance(4500);
    assert(timer_fn && timer_due == 5500);
    assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.late_steps == 1);
    atomic_store(&ms, 4750);
    page.id = "new-timer";
    run(show, &page);
    advance(4750);
    assert(timer_fn && timer_due == 5750 && ui_display_at(0)->scene.page.timer_s == 0);
    advance(5750);
    assert(timer_fn && timer_due == 6750 && ui_display_at(0)->scene.page.timer_s == 1);
    advance(6750);
}

static void touch(void *unused)
{
    (void)unused;
    xiaotai_ui_host_input(host, XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 0, XIAOTAI_UI_INPUT_PRESS, 0, NULL);
    xiaotai_ui_host_input(host, XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 0, XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
    xiaotai_ui_host_input(host, XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, 0, XIAOTAI_UI_INPUT_TAP, 0, NULL);
}

static void owner_sleep(void *unused)
{
    (void)unused;
    assert(xiaotai_ui_sleep(0) == XIAOTAI_OK);
}

static void power_case(bool dark)
{
    setup(dark);
    advance(1000);
    xiaotai_view_t page = view("Q");
    run(show, &page);
    advance(1000);
    assert(xiaotai_ui_set_sleep_timeout(100) == XIAOTAI_OK);
    run(NULL, NULL);
    advance(1000);
    assert(timer_due == 1100);
    fail_light = XIAOTAI_ERR_TIMEOUT;
    advance(1100);
    assert(!sleeps && light == 100 && xiaotai_ui_awake(0) && timer_due == 1200);
    fail_light = XIAOTAI_OK;
    advance(1200);
    assert(sleeps == 1 && !light && !xiaotai_ui_awake(0));
    assert(xiaotai_ui_set_sleep_timeout(0) == XIAOTAI_OK);
    run(NULL, NULL);
    run(touch, NULL);
    run(NULL, NULL);
    assert(xiaotai_ui_awake(0) && wakes == 1 && posted == (unsigned)dark);
    xiaotai_ui_info_t info;
    assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ignored == (unsigned)!dark);
    run(touch, NULL);
    run(NULL, NULL);
    assert(posted == (unsigned)dark + 1);
    run(owner_sleep, NULL);
    assert(!xiaotai_ui_awake(0));
    const xiaotai_args_t nav = {0};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_UI_CONFIRM, &nav) == XIAOTAI_OK);
    run(NULL, NULL);
    advance(1200);
    assert(xiaotai_ui_awake(0) && posted == (unsigned)dark + 1);
}

typedef struct {
    bool controls;
    uint32_t timeout;
    xiaotai_err_t result;
    size_t capacity, count;
    xiaotai_ui_control_t rows[2];
    xiaotai_ui_capture_t capture;
} diag_probe_t;

static void *do_diag(void *opaque)
{
    diag_probe_t *probe = opaque;
    if (probe->controls) probe->result = xiaotai_ui_controls(0, probe->rows, probe->capacity,
                                                           &probe->count, probe->timeout);
    else probe->result = xiaotai_ui_capture(0, &probe->capture, probe->timeout);
    return NULL;
}

static pthread_t launch(diag_probe_t *probe)
{
    pthread_t caller;
    assert(pthread_create(&caller, NULL, do_diag, probe) == 0);
    return caller;
}

static void wait_pending(void)
{
    for (unsigned i = 0; i < 5000; ++i) {
        if (ui_diag_pending(ui_display_at(0))) return;
        xiaotai_delay_ms(1);
    }
    assert(false);
}

static void pump_diag(void)
{
    if (worker_mode) return;
    wait_pending();
    run(NULL, NULL);
    advance(atomic_load(&ms));
}

static void begin_activity(void *arg)
{
    const xiaotai_activity_t activity = {.name = "fixture.capture"};
    assert(xiaotai_activity_begin(&activity, arg) == XIAOTAI_OK);
}

static void end_activity(void *arg) { xiaotai_activity_end(*(const uint32_t *)arg); }

typedef struct {
    uint32_t id;
    unsigned queued;
    atomic_uint drained;
} activity_drop_t;

static void drain_job(void *arg)
{
    activity_drop_t *test = arg;
    atomic_fetch_add(&test->drained, 1U);
}

static void end_activity_without_event(void *arg)
{
    activity_drop_t *test = arg;
    xiaotai_err_t err;
    while ((err = xiaotai_run(drain_job, test)) == XIAOTAI_OK) ++test->queued;
    assert(err == XIAOTAI_ERR_NO_MEM && test->queued > 0);
    xiaotai_activity_end(test->id);
}

static void reset_with_kick(void *arg)
{
    (void)arg;
    xiaotai_signal_give(&synchronized);
    for (unsigned i = 0; i < 5000; ++i) {
        xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
        const bool queued = ui_display_at(0)->driver.kick_pending;
        xiaotai_mutex_unlock(&g_ui.lock);
        if (queued) {
            xiaotai_ui_reset();
            return;
        }
        xiaotai_delay_ms(1);
    }
    assert(false);
}

static size_t show_diagnostic_page(void)
{
    setup(false);
    if (!worker_mode) advance(1000);
    xiaotai_view_t page = view("D");
    run(show, &page);
    wait_step();
    assert(writes == 1);
    return renders;
}

static void diag_case(void)
{
    const size_t before = show_diagnostic_page();
    if (!worker_mode) run(diag_guard, NULL);
    diag_probe_t photo = {.timeout = 5000};
    pthread_t caller = launch(&photo);
    pump_diag();
    assert(!pthread_join(caller, NULL));
    assert(photo.result == XIAOTAI_OK && photo.capture.pixel == XIAOTAI_PIXEL_MONO1_VLSB);
    assert(photo.capture.width == 128 && photo.capture.height == 64 && photo.capture.size == 1024);
    for (size_t i = 0; i < photo.capture.size; ++i) assert(photo.capture.pixels[i] == 'D');
    xiaotai_ui_capture_free(&photo.capture);
    assert(!photo.capture.pixels && !photo.capture.size);
    assert(writes == 1 && renders == before + 1U);
    for (size_t capacity = 0; capacity <= 2; ++capacity) {
        diag_probe_t list = {.controls = true, .capacity = capacity, .timeout = 5000};
        list.rows[capacity < 2 ? capacity : 0].width = UINT16_MAX;
        caller = launch(&list);
        pump_diag();
        assert(!pthread_join(caller, NULL) && list.result == XIAOTAI_OK && list.count == 2);
        if (capacity) {
            assert(list.rows[0].x == 4 && list.rows[0].width == 24 && !list.rows[0].state);
            assert(!strcmp(list.rows[0].text, "Tap") && !strcmp(list.rows[0].intent, "test.tap"));
        }
        if (capacity < 2) assert(list.rows[capacity].width == UINT16_MAX);
        else assert(!strcmp(list.rows[1].text, "custom") && !list.rows[1].intent[0] &&
                    list.rows[1].state == XIAOTAI_STATE_CHECKED);
    }
    uint32_t activity;
    run(begin_activity, &activity);
    assert(xiaotai_ui_capture(0, &photo.capture, 5000) == XIAOTAI_ERR_BUSY && !photo.capture.pixels);
    assert(!ui_diag_pending(ui_display_at(0)) && writes == 1 && renders == before + 1U);
    run(NULL, NULL);
    activity_drop_t dropped = {.id = activity};
    run(end_activity_without_event, &dropped);
    for (unsigned i = 0; i < 5000 && atomic_load(&dropped.drained) != dropped.queued; ++i) xiaotai_delay_ms(1);
    assert(atomic_load(&dropped.drained) == dropped.queued);
    caller = launch(&photo);
    pump_diag();
    assert(!pthread_join(caller, NULL) && photo.result == XIAOTAI_OK);
    xiaotai_ui_capture_free(&photo.capture);
}

static void diag_wait_case(void)
{
    const size_t before = show_diagnostic_page();
    const int64_t baseline = xiaotai_mem_host_outstanding();
    xiaotai_ui_control_t list = {.width = 1234};
    size_t count = 11;
    xiaotai_mem_host_fail_nth(2);
    assert(xiaotai_ui_controls(0, &list, 1, &count, 500) == XIAOTAI_ERR_NO_MEM);
    assert(!count && list.width == 1234 && xiaotai_mem_host_outstanding() == baseline);
    diag_probe_t photo = {.timeout = 5000};
    pthread_t caller = launch(&photo);
    wait_pending();
    xiaotai_ui_capture_t duplicate = {.size = 1};
    assert(xiaotai_ui_capture(0, &duplicate, 5000) == XIAOTAI_ERR_INVALID_STATE && !duplicate.size);
    uint32_t activity;
    run(begin_activity, &activity);
    advance(1000);
    assert(!pthread_join(caller, NULL));
    assert(photo.result == XIAOTAI_ERR_BUSY && !photo.capture.pixels);
    assert(renders == before && writes == 1 && !ui_diag_pending(ui_display_at(0)));
    run(end_activity, &activity);
    run(NULL, NULL);

    assert(xiaotai_ui_controls(0, &list, 1, &count, 0) == XIAOTAI_ERR_TIMEOUT && !count);
    assert(list.width == 1234 && !ui_diag_pending(ui_display_at(0)));
    diag_probe_t retry = {.controls = true, .capacity = 1, .timeout = 5000};
    caller = launch(&retry);
    wait_pending();
    count = 11;
    assert(xiaotai_ui_controls(0, &list, 1, &count, 5000) == XIAOTAI_ERR_TIMEOUT && !count);
    pump_diag();
    assert(!pthread_join(caller, NULL) && retry.result == XIAOTAI_OK && retry.count == 2);
    assert(xiaotai_mem_host_outstanding() == baseline);

    assert(xiaotai_signal_init(&capture_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&capture_release) == XIAOTAI_OK);
    atomic_store(&hold_capture, true);
    photo = (diag_probe_t){.timeout = 900};
    caller = launch(&photo);
    wait_pending();
    run(NULL, NULL);
    assert(xiaotai_run(firing, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&capture_entered, 5000));
    assert(!pthread_join(caller, NULL));
    assert(photo.result == XIAOTAI_ERR_TIMEOUT && !photo.capture.pixels && !photo.capture.size);
    assert(xiaotai_ui_capture(0, &duplicate, 5000) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_give(&capture_release);
    run(NULL, NULL);
    xiaotai_signal_deinit(&capture_release);
    xiaotai_signal_deinit(&capture_entered);
    assert(!photo.capture.pixels && !photo.capture.size && renders == before + 1U && writes == 1);
    assert(!ui_diag_pending(ui_display_at(0)) && xiaotai_mem_host_outstanding() == baseline);

    photo = (diag_probe_t){.timeout = 5000};
    assert(xiaotai_run(reset_with_kick, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&synchronized, 5000));
    caller = launch(&photo);
    assert(!pthread_join(caller, NULL));
    assert(photo.result == XIAOTAI_ERR_INVALID_STATE && !photo.capture.pixels && !photo.capture.size);
    run(NULL, NULL);
}

static void rotation_case(void)
{
    rotated_mode = true;
    const size_t before = show_diagnostic_page();
    assert(last_frame == XIAOTAI_OK);
    diag_probe_t photo = {.timeout = 5000};
    const pthread_t caller = launch(&photo);
    pump_diag();
    assert(!pthread_join(caller, NULL) && photo.result == XIAOTAI_OK);
    assert(photo.capture.width == 8 && photo.capture.height == 16 && photo.capture.size == 16);
    assert(!memcmp(photo.capture.pixels, frame, photo.capture.size));
    assert(writes == 1 && renders == before + 1U);
    xiaotai_ui_capture_free(&photo.capture);
}

static void inject_case(void)
{
    touch_mode = true;
    setup(false);
    advance(1000);
    assert(real_polls == 1);
    ui_display_t *d = ui_display_at(0);
    assert(xiaotai_ui_tap(0, 128, 0) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_ui_swipe(0, 1, 1, 2, 2, 99) == XIAOTAI_ERR_INVALID_ARG);
    run(owner_sleep, NULL);
    assert(xiaotai_ui_tap(0, 4, 9) == XIAOTAI_OK);
    assert(xiaotai_ui_tap(0, 5, 9) == XIAOTAI_ERR_INVALID_STATE);
    run(NULL, NULL);
    advance(1000);
    assert(real_polls == 1 && d->power.awake && !d->pointer.down && d->pointer.mode == UI_POINTER_GESTURE);
    advance(1119);
    assert(d->pointer.mode == UI_POINTER_GESTURE);
    advance(1120);
    assert(d->pointer.mode == UI_POINTER_IDLE && !d->pointer.down);

    assert(xiaotai_ui_swipe(0, 2, 2, 100, 42, 200) == XIAOTAI_OK);
    run(NULL, NULL);
    advance(1120);
    assert(d->pointer.down && d->pointer.point.x == 2 && d->pointer.point.y == 2);
    real_down = true;
    advance(1220);
    assert(d->pointer.real_down && d->pointer.down && d->pointer.point.x == 51 && d->pointer.point.y == 22);
    advance(1320);
    assert(!d->pointer.down && d->pointer.point.x == 100 && d->pointer.point.y == 42);
    assert(xiaotai_ui_tap(0, 5, 5) == XIAOTAI_ERR_INVALID_STATE);
    real_down = false;
    advance(1335);
    assert(xiaotai_ui_pointer(0, 8, 9, true) == XIAOTAI_OK);
    run(NULL, NULL);
    advance(1335);
    assert(d->pointer.down && d->pointer.point.x == 8);
    assert(xiaotai_ui_pointer(0, 10, 9, true) == XIAOTAI_OK);
    run(NULL, NULL);
    advance(1340);
    assert(d->pointer.down && d->pointer.point.x == 10);
    advance(2339);
    assert(d->pointer.down);
    advance(2340);
    assert(!d->pointer.down && d->pointer.mode == UI_POINTER_IDLE);
}

#ifdef UI_OWNER_STACK
static void measured_poll(void *ctx)
{
    (void)ctx;
    size_t used = 0;
    assert(xiaotai_host_stack_measure(firing, NULL, &used));
    printf("ui owner step used %u bytes of stack\n", (unsigned)used);
    assert(used <= XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES);
}
static void stack_case(void)
{
    setup(false);
    advance(1000);
    xiaotai_view_t page = view("R");
    run(show, &page);
    run(measured_poll, NULL);
    assert(writes == 1 && sent[0] == 'R');
}
#endif

int main(void)
{
    if (!strcmp(UI_DRIVER_CASE, "frames")) frames_case();
    else if (!strcmp(UI_DRIVER_CASE, "rotation")) rotation_case();
    else if (!strcmp(UI_DRIVER_CASE, "inject")) inject_case();
    else if (!strcmp(UI_DRIVER_CASE, "task")) {
        worker_mode = true;
        stack_wanted = 4096;
        frames_case();
        xiaotai_ui_info_t info;
        assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && !info.owner_timer && !info.late_steps);
        fail_light = XIAOTAI_ERR_TIMEOUT;
        assert(xiaotai_ui_sleep(0) == XIAOTAI_ERR_TIMEOUT && xiaotai_ui_awake(0));
        fail_light = XIAOTAI_OK;
        assert(xiaotai_ui_sleep(0) == XIAOTAI_OK && !xiaotai_ui_awake(0));
        assert(xiaotai_ui_set_brightness(0, 31) == XIAOTAI_OK && light == 0);
        fail_light = XIAOTAI_FAIL;
        assert(xiaotai_ui_wake(0) == XIAOTAI_FAIL && !xiaotai_ui_awake(0) && light == 0);
        fail_light = XIAOTAI_OK;
        assert(xiaotai_ui_wake(0) == XIAOTAI_OK && xiaotai_ui_awake(0) && light == 31);
    } else if (!strcmp(UI_DRIVER_CASE, "timer")) timers_case();
    else if (!strcmp(UI_DRIVER_CASE, "power")) power_case(false);
    else if (!strcmp(UI_DRIVER_CASE, "dark_operates")) power_case(true);
    else if (!strcmp(UI_DRIVER_CASE, "diag_owner")) diag_case();
    else if (!strcmp(UI_DRIVER_CASE, "diag_task")) {
        worker_mode = true;
        stack_wanted = 4096;
        diag_case();
    } else if (!strcmp(UI_DRIVER_CASE, "diag_wait")) diag_wait_case();
#ifdef UI_OWNER_STACK
    else if (!strcmp(UI_DRIVER_CASE, "stack")) stack_case();
#endif
    else assert(false);
    xiaotai_ui_reset();
    assert(!timer_fn && stopped == 1 && light == 0);
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&waiting);
    xiaotai_signal_deinit(&synchronized);
    return 0;
}
