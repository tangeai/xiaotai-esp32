// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_p4_h5_local_stop.py
#include "fakes.h"
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/modules/stream.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_stream;
extern const xiaotai_app_companion_part_t xiaotai_app_companion_with_ui;
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {
    &xiaotai_app_companion_with_ui, &xiaotai_app_companion_with_stream, NULL,
};

static xiaotai_signal_t fence, release_owner;
static xiaotai_stream_status_t stream;
static unsigned stop_requests;
static uint32_t stopped;
static bool hold_stop;
static ui_display_t *display;
static const xiaotai_ui_host_t *host;
static atomic_uint now = 1000;
static const char local_toast[] = "\xe6\x93\x8d\xe4\xbd\x9c\xe6\x9c\xaa\xe5\x8f\x97\xe7\x90\x86"
                                 "\xef\xbc\x8c\xe8\xaf\xb7\xe9\x87\x8d\xe8\xaf\x95";

static uint32_t clock_ms(void) { return atomic_load(&now); }

void xiaotai_stream_status(xiaotai_stream_status_t *status)
{
    assert(xiaotai_on_owner());
    *status = stream;
}

xiaotai_err_t xiaotai_stream_stop(uint32_t generation)
{
    assert(xiaotai_on_owner() && stream.phase == XIAOTAI_STREAM_ACTIVE && stream.generation == generation);
    stopped = generation;
    ++stop_requests;
    return hold_stop ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK;
}

static void notify(void *ctx) { xiaotai_signal_give(ctx); }
static void noop(void *unused) { (void)unused; }

static void block(void *unused)
{
    (void)unused;
    notify(&fence);
    assert(xiaotai_signal_take(&release_owner, 5000));
}

static void sync(void)
{
    assert(xiaotai_run(notify, &fence) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&fence, 5000));
}

static void render(void)
{
    sync();
    sync();
    (void)ui_scene_step(display, atomic_fetch_add(&now, 10));
}

static void set_stream(void *ctx)
{
    const uint32_t gen = *(const uint32_t *)ctx;
    stream = (xiaotai_stream_status_t){.generation = gen, .phase = XIAOTAI_STREAM_ACTIVE, .video_declared = true};
}

static void open_stream(void *ctx)
{
    set_stream(ctx);
    const uint32_t gen = stream.generation;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_STREAM_STARTED, &(const xiaotai_args_t){.value = (int32_t)gen}, NULL, 0) == XIAOTAI_OK);
}

static void close_stream(void *unused)
{
    (void)unused;
    const xiaotai_stream_ended_t ended = {.generation = stream.generation, .started = true,
        .end = XIAOTAI_STREAM_END_LOCAL, .error = XIAOTAI_OK};
    stream = (xiaotai_stream_status_t){.phase = XIAOTAI_STREAM_LISTENING};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_STREAM_ENDED, NULL, &ended, sizeof(ended)) == XIAOTAI_OK);
}

static uint16_t action(uint32_t generation, bool busy)
{
    const xiaotai_view_t *view = &display->scene.page.view;
    assert(display->scene.page.present && !strcmp(view->id, "home"));
    for (uint16_t i = 0; i < view->action_count; ++i) {
        const xiaotai_view_action_t *stop = &view->actions[i];
        if (!stop->icon || strcmp(stop->icon, XIAOTAI_ICON_STOP)) continue;
        assert(generation && (uint32_t)stop->value == generation && stop->intent);
        assert(!strcmp(stop->label, "\xe7\xbb\x93\xe6\x9d\x9f\xe6\x9f\xa5\xe7\x9c\x8b"));
        assert(!(stop->state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_HIDDEN)));
        assert(((stop->state & XIAOTAI_STATE_BUSY) != 0) == busy);
        return i;
    }
    assert(!generation);
    return UINT16_MAX;
}

static void pointer(uint16_t index, xiaotai_ui_input_t input)
{
    xiaotai_ui_host_input(host, XIAOTAI_LAYER_PAGE, XIAOTAI_UI_ELEMENT_ACTION, index, input, 0, NULL);
}

static void tap(uint16_t index)
{
    pointer(index, XIAOTAI_UI_INPUT_PRESS);
    pointer(index, XIAOTAI_UI_INPUT_RELEASE);
    pointer(index, XIAOTAI_UI_INPUT_TAP);
}

static xiaotai_err_t theme_init(void)
{
    const ui_fake_theme_config_t theme = {.name = "fixture", .backend = XIAOTAI_UI_BACKEND_FRAME,
        .width = 240, .height = 240, .pixel = XIAOTAI_PIXEL_RGB565, .exact = 10};
    return xiaotai_ui_theme_register(ui_fake_theme(0, &theme));
}

static void setup(bool local)
{
    xiaotai_display_desc_t board = ui_fake_desc(240, 240, XIAOTAI_PIXEL_RGB565);
    board.touch = true;
    ui_fake_board_set_displays(&board, 1);
    ui_core_set_clock(clock_ms);
    ui_driver_manual(true);
    if (local) assert(xiaotai_ui_configure(&(const xiaotai_ui_config_t){.local_rejection_toast = local_toast})
                      == XIAOTAI_OK);
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&release_owner) == XIAOTAI_OK);
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    fixture.saved_credentials = true;
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const char *const deps[] = {"ui", NULL};
    static const xiaotai_module_t theme = {.name = "theme", .init = theme_init, .depends = deps};
    static const xiaotai_module_t *const modules[] = {&ui, &theme};
    static const xiaotai_application_t companion = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&companion};
    assert(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 2,
        .applications = apps, .application_count = 1, .available_needs = xiaotai_hal_needs(),
        .queue_depth = 32}) == XIAOTAI_OK);
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
    display = ui_display_at(0);
    host = ui_fake_theme_host(0, 0);
    assert(display && host);
    render();
    assert(action(0, false) == UINT16_MAX);
}

static void drain_owner(void)
{
    notify(&release_owner);
    for (unsigned i = 0; i < 5000; ++i) {
        if (xiaotai_run(notify, &fence) == XIAOTAI_OK) break;
        xiaotai_delay_ms(1);
    }
    assert(xiaotai_signal_take(&fence, 5000));
}

static void saturate_owner(void)
{
    assert(xiaotai_run(block, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&fence, 5000));
    for (unsigned i = 0; i < 32; ++i) assert(xiaotai_run(noop, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
}

static void full_queue(uint16_t index, const char *scenario)
{
    xiaotai_ui_info_t before, after;
    assert(xiaotai_ui_inspect(0, &before) == XIAOTAI_OK);
    saturate_owner();
    tap(index);
    assert(xiaotai_ui_inspect(0, &after) == XIAOTAI_OK);
    assert(after.rejected == before.rejected + 1 && after.posted == before.posted);
    assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
    uint32_t due;
    assert(display->rejected.pending && ui_scene_nonperiodic_due(display, clock_ms(), &due));
    atomic_fetch_add(&now, 20);
    (void)ui_scene_step(display, clock_ms());  /* Notice still encounters a full queue. */
    drain_owner();
    if (!strcmp(scenario, "retry")) {
        const uint32_t ticket = display->rejected.ticket;
        assert(ticket && !display->scene.toast.present && !stop_requests);
        tap(index);
        render();
        assert(stop_requests == 1 && stopped == 18U && !display->rejected.pending && !display->rejected.ticket);
        xiaotai_args_t late = {.text = "companion.stream.stop"};
        memcpy(&late.value, &ticket, sizeof(ticket));
        assert(xiaotai_event_emit(XIAOTAI_EVENT_UI_REJECTED, &late, NULL, 0) == XIAOTAI_OK);
        atomic_fetch_add(&now, 20);
        render();
        assert(!display->scene.toast.present && stop_requests == 1);
        puts("accepted retry cancels deferred UI feedback; obsolete rejection does not paint");
        return;
    }
    const bool stale = !strcmp(scenario, "stale");
    const bool rollback = !strcmp(scenario, "rollback");
    const bool expiry = !strcmp(scenario, "expiry");
    const bool recovered = !stale && !rollback && !expiry;
    uint32_t next = 19;
    if (stale) {
        assert(xiaotai_run(block, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 5000));
        assert(xiaotai_run(set_stream, &next) == XIAOTAI_OK);
        assert(xiaotai_event_emit(XIAOTAI_EVENT_STREAM_STARTED, &(const xiaotai_args_t){.value = 19}, NULL, 0) == XIAOTAI_OK);
    } else if (rollback) {
        ui_display_stop(display);
        assert(ui_display_open(display) == XIAOTAI_OK);
    } else if (expiry) {
        atomic_fetch_add(&now, 1000);
    }
    atomic_fetch_add(&now, 20);
    (void)ui_scene_step(display, clock_ms());
    if (stale) {
        assert(!display->rejected.pending && display->rejected.ticket);
        drain_owner();
    }
    render();
    assert(!stop_requests);
    index = action(stale ? 19 : 18, false);
    const bool toast = display->scene.toast.present;
    printf("%s queue: rejected=1 stop_requests=%u still_enabled=1 toast=%u\n", scenario, stop_requests, toast);
    assert(toast == recovered);
    tap(index);
    render();
    assert(stop_requests == 1 && stopped == (stale ? 19U : 18U));
}

static void stage_other_page(void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner());
    xiaotai_view_show(&(const xiaotai_view_t){.kind = XIAOTAI_VIEW_NOTICE, .id = "other"});
}

static void local_queue(uint16_t index, const char *scenario)
{
    const bool replace_before_paint = !strcmp(scenario, "local-stale");
    const bool change_page = !strcmp(scenario, "local-page");
    xiaotai_ui_info_t before, rejected, settled;
    assert(xiaotai_ui_inspect(0, &before) == XIAOTAI_OK);
    saturate_owner();
    tap(index);
    assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_ui_inspect(0, &rejected) == XIAOTAI_OK);
    assert(!stop_requests && rejected.rejected == before.rejected + 1 && rejected.posted == before.posted);
    assert(!display->rejected.pending && action(18, false) == index);
    uint32_t due;
    if (!replace_before_paint) {
        (void)ui_scene_step(display, clock_ms());
        assert(display->scene.toast.present && !strcmp(display->scene.toast.view.text, local_toast));
        assert(ui_layers_next_due(display, &due) && due == clock_ms() + XIAOTAI_TOAST_DEFAULT_MS);
        assert(xiaotai_run(noop, NULL) == XIAOTAI_ERR_NO_MEM);
        printf("blocked owner: toast=1 stopped=0 rejected=%u\n", rejected.rejected - before.rejected);
    }
    drain_owner();
    if (change_page) {
        assert(xiaotai_run(stage_other_page, NULL) == XIAOTAI_OK);
        render();
        assert(display->scene.page.present && !strcmp(display->scene.page.view.id, "other"));
        assert(!display->scene.toast.present && !display->rejected.pending && !stop_requests);
        puts("page replaced after first paint: no stale toast on the new page");
        return;
    }
    uint32_t gen = 18;
    if (replace_before_paint) {
        gen = 19;
        assert(xiaotai_run(open_stream, &gen) == XIAOTAI_OK);
        render();
        assert(!display->scene.toast.present && !display->rejected.pending);
        puts("replacement before first paint: no stale toast");
    } else {
        render();
        uint32_t unchanged;
        assert(ui_layers_next_due(display, &unchanged) && unchanged == due);
        assert(xiaotai_ui_inspect(0, &settled) == XIAOTAI_OK && settled.shown == rejected.shown);
        atomic_fetch_add(&now, XIAOTAI_TOAST_DEFAULT_MS);
        render();
        assert(!display->scene.toast.present);
        puts("owner drained: no second toast, original deadline unchanged");
    }
    assert(!stop_requests);
    tap(action(gen, false));
    render();
    assert(stop_requests == 1 && stopped == gen);
}

int main(int argc, char **argv)
{
    const bool local = argc == 2 && !strncmp(argv[1], "local-", sizeof("local-") - 1U);
    setup(local);
    uint32_t gen = 18;
    assert(xiaotai_run(open_stream, &gen) == XIAOTAI_OK);
    render();
    uint16_t index = action(18, false);
    if (local) local_queue(index, argv[1]);
    else if (argc == 2) full_queue(index, argv[1]);
    else {
        pointer(index, XIAOTAI_UI_INPUT_PRESS);
        gen = 19;
        assert(xiaotai_run(open_stream, &gen) == XIAOTAI_OK);
        render();
        index = action(19, false);
        pointer(index, XIAOTAI_UI_INPUT_RELEASE);
        pointer(index, XIAOTAI_UI_INPUT_TAP);
        render();
        assert(!stop_requests);
        assert(xiaotai_event_emit(XIAOTAI_EVENT_UI_REJECTED,
            &(const xiaotai_args_t){.value = 12345, .text = "companion.stream.stop"}, NULL, 0) == XIAOTAI_OK);
        render();
        assert(!display->scene.toast.present);
        hold_stop = true;
        tap(index);
        render();
        assert(stop_requests == 1 && stopped == 19 && display->scene.toast.present);
        assert(action(19, false) == index);
        hold_stop = false;
        tap(index);
        render();
        assert(action(19, true) == index);
        tap(index);
        render();
        assert(stop_requests == 2);
        assert(xiaotai_run(close_stream, NULL) == XIAOTAI_OK);
        render();
        assert(action(0, false) == UINT16_MAX);
        puts("stop touch: stale press ignored, owner refusal visible, retry accepted, duplicate ignored, stop hidden");
    }
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_ui_reset();
    xiaotai_signal_deinit(&release_owner);
    xiaotai_signal_deinit(&fence);
    return 0;
}
