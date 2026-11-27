// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_cores3_screen_wire.py
// Replaces legacy: tests/test_at_diagnostics.py
#include "ui_internal.h"
#include "ui_test.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/mem.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef UI_CORE_SCREEN
#define UI_CORE_SCREEN 0
#endif

#if UI_CORE_SCREEN
#include "lvgl.h"
static atomic_bool short_capture, fail_capture_alloc;
void *__real_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size);
void *__wrap_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size);

void *__wrap_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size)
{
    if (kind == XIAOTAI_MEM_BULK && size == 153600 && atomic_exchange(&fail_capture_alloc, false)) return NULL;
    return __real_xiaotai_mem_aligned(kind, alignment, size);
}

lv_result_t __real_lv_draw_buf_init(lv_draw_buf_t *, uint32_t, uint32_t, lv_color_format_t,
                                    uint32_t, void *, uint32_t);
lv_result_t __wrap_lv_draw_buf_init(lv_draw_buf_t *, uint32_t, uint32_t, lv_color_format_t,
                                    uint32_t, void *, uint32_t);

lv_result_t __wrap_lv_draw_buf_init(lv_draw_buf_t *buf, uint32_t w, uint32_t h,
                                    lv_color_format_t format, uint32_t stride, void *data, uint32_t bytes)
{
    if (w == 320 && h == 240 && stride == 640 && bytes == 153600 && atomic_exchange(&short_capture, false))
        --bytes;
    return __real_lv_draw_buf_init(buf, w, h, format, stride, data, bytes);
}
#endif

#define WAIT_MS 3000U

static xiaotai_signal_t start_at, done_at, owner_done, entered, proceed;
static const xiaotai_ui_host_t *host;
static atomic_bool exit_at, hold_capture, hold_controls, physical, awake;
static uint32_t activity;
static const char *input;
static char output[UI_CORE_SCREEN ? 350000 : 10000];
static size_t written;

static size_t read_at(void *unused, uint8_t *data, size_t capacity, uint32_t timeout)
{
    (void)unused;
    size_t length = strlen(input);
    if (length > capacity) length = capacity;
    if (!length) xiaotai_delay_ms(timeout);
    else memcpy(data, input, length);
    input += length;
    return length;
}

static xiaotai_err_t write_at(void *unused, const char *data, size_t size)
{
    (void)unused;
    assert(!xiaotai_on_owner());
    assert(written + size < sizeof(output));
    memcpy(output + written, data, size);
    written += size;
    output[written] = '\0';
    return XIAOTAI_OK;
}

/* A command's reply is complete once its input is read and its final OK or
   ERROR line is out (the console's first +READY line is neither). */
static bool replied(void)
{
    return !*input && ((written >= 4 && !strcmp(output + written - 4, "OK\r\n")) || strstr(output, "ERROR: "));
}

static void *run_at(void *unused)
{
    (void)unused;
    while (xiaotai_signal_take(&start_at, XIAOTAI_WAIT_FOREVER) && !atomic_load(&exit_at)) {
        /* Serve until the reply is complete: on a busy host one 20 ms slice
           can end after +READY and before a screen capture has been written. */
        const int64_t end = xiaotai_time_us() + (int64_t)WAIT_MS * 1000;
        do assert(xiaotai_at_service(20) == XIAOTAI_OK);
        while (!replied() && xiaotai_time_us() < end);
        xiaotai_signal_give(&done_at);
    }
    return NULL;
}

static void start_command(const char *command)
{
    written = 0;
    output[0] = '\0';
    input = command;
    xiaotai_signal_give(&start_at);
}

static void expect(const char *text)
{
    assert(xiaotai_signal_take(&done_at, WAIT_MS));
    if (!strstr(output, text)) fprintf(stderr, "expected %s; got %s\n", text, output);
    assert(strstr(output, text));
    if (!strncmp(text, "ERROR: ", 7)) assert(!strstr(output, "\r\nOK\r\n"));
}

static void signal_owner(void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner());
    xiaotai_signal_give(&owner_done);
}

static void owner_unblocked(void)
{
    assert(xiaotai_signal_take(&entered, WAIT_MS));
    assert(xiaotai_run(signal_owner, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_done, 500));
    xiaotai_signal_give(&proceed);
}

static xiaotai_err_t light(void *unused, uint8_t pct)
{
    (void)unused;
    assert(!xiaotai_on_owner());
    atomic_store(&awake, pct != 0);
    return XIAOTAI_OK;
}

static xiaotai_err_t touch(void *unused, xiaotai_touch_point_t *out, size_t cap, size_t *count)
{
    (void)unused;
    assert(cap >= 1U && !xiaotai_on_owner());
    *count = atomic_load(&physical);
    out[0] = (xiaotai_touch_point_t){4, 4};
    return XIAOTAI_OK;
}

static int match(const xiaotai_display_desc_t *desc) { return desc->width == (UI_CORE_SCREEN ? 320 : 128) ? 100 : 0; }
static xiaotai_err_t start(const xiaotai_ui_host_t *h, void **state)
{
    assert(!xiaotai_on_owner());
    host = h;
    *state = NULL;
#if UI_CORE_SCREEN
    lv_obj_t *screen = lv_display_get_screen_active(xiaotai_ui_host_root(h));
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    const uint32_t colours[] = {0xff0000, 0x00ff00, 0x0000ff};
    for (unsigned x = 0; x < 3; ++x) {
        lv_obj_t *pixel = lv_obj_create(screen);
        lv_obj_remove_style_all(pixel);
        lv_obj_set_pos(pixel, (int32_t)x, 0);
        lv_obj_set_size(pixel, 1, 1);
        lv_obj_set_style_bg_color(pixel, lv_color_hex(colours[x]), 0);
        lv_obj_set_style_bg_opa(pixel, LV_OPA_COVER, 0);
    }
#endif
    return XIAOTAI_OK;
}

static void render(void *unused, const xiaotai_ui_scene_t *scene)
{
    (void)unused;
    assert(!xiaotai_on_owner());
    if (UI_CORE_SCREEN) return;
    if (atomic_exchange(&hold_capture, false)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&proceed, WAIT_MS));
    }
    if (!scene->page.present) return;
    static uint8_t frame[1024];
    memset(frame, 0xa5, sizeof(frame));
    assert(xiaotai_ui_host_present(host, frame, sizeof(frame)) == XIAOTAI_OK);
}

static size_t controls(void *unused, xiaotai_ui_control_t *out, size_t capacity)
{
    (void)unused;
    assert(!xiaotai_on_owner());
    if (atomic_exchange(&hold_controls, false)) {
        xiaotai_signal_give(&entered);
        assert(xiaotai_signal_take(&proceed, WAIT_MS));
    }
    if (capacity) out[0] = (xiaotai_ui_control_t){.x = 1, .y = 2, .width = 8, .height = 4,
        .layer = XIAOTAI_LAYER_PAGE, .element = XIAOTAI_UI_ELEMENT_ACTION};
    return 1;
}

static void report(void *unused, char *text, size_t capacity)
{
    (void)unused;
    assert(xiaotai_on_owner());
    (void)snprintf(text, capacity, "direct=4 fallback=2");
}

static xiaotai_err_t theme(void)
{
    static const xiaotai_theme_t impl = {.name = "screen-test", .backend = UI_CORE_SCREEN
        ? XIAOTAI_UI_BACKEND_LVGL : XIAOTAI_UI_BACKEND_FRAME,
        .match = match, .start = start, .render = render, .controls = controls, .report = report, .stack_bytes = 4096};
    return xiaotai_ui_theme_register(&impl);
}

static void show(void *arg)
{
    const unsigned phase = (unsigned)(uintptr_t)arg;
    static const xiaotai_view_action_t action = {.label = "Play", .intent = "fixture.play"};
    xiaotai_view_t page = {.id = "test page", .title = phase ? "Idle" : "Locked", .kind = XIAOTAI_VIEW_HOME,
        .flags = phase ? XIAOTAI_VIEW_MAY_SLEEP : 0U, .actions = &action, .action_count = 1};
    xiaotai_view_show(&page);
    if (phase == 2) {
        xiaotai_view_t overlay = {.id = "question", .layer = XIAOTAI_LAYER_OVERLAY, .title = "Question"};
        xiaotai_view_show(&overlay);
    } else if (phase == 3) {
        xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, NULL);
    }
}

static void show_page(unsigned phase)
{
    xiaotai_ui_info_t info;
    /* Not the empty frame of the start. */
    do assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK);
    while (!info.frames && (xiaotai_delay_ms(1), true));
    const uint32_t before = info.frames;
    assert(xiaotai_run(show, (void *)(uintptr_t)phase) == XIAOTAI_OK);
    for (unsigned ms = 0; ms < WAIT_MS; ++ms) {
        assert(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK);
        if (info.frames != before) return;
        xiaotai_delay_ms(1);
    }
    assert(!"page did not render");
}

static void change_activity(void *running)
{
    const xiaotai_activity_t fixture = {.name = "fixture", .resources = XIAOTAI_RESOURCE_SCREEN};
    if (running) assert(xiaotai_activity_begin(&fixture, &activity) == XIAOTAI_OK);
    else xiaotai_activity_end(activity);
    xiaotai_signal_give(&owner_done);
}

static void set_activity(bool running)
{
    assert(xiaotai_run(change_activity, running ? &activity : NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_done, WAIT_MS));
}

#if UI_CORE_SCREEN
static void screen_wire(bool dump)
{
    show_page(0);
    start_command("AT+SCREEN\r\n");
    expect("+SCREEN: format=RGB565LE width=320 height=240 view=test%20page bytes=153600 fnv=");
    assert(strstr(output, "+SCREEN-DATA: seq=0 hex=00F8E0071F00"));
    assert(strstr(output, "+SCREEN-END: chunks=1200 fnv=") && strstr(output, "OK\r\n"));
    if (dump) assert(fwrite(output, 1, written, stdout) == written);

    atomic_store(&short_capture, true);
    start_command("AT+SCREEN\r\n");
    expect("ERROR: INVALID_SIZE\r\n");
    assert(!atomic_load(&short_capture) && !strstr(output, "+SCREEN") && !strstr(output, "OK\r\n"));

    atomic_store(&fail_capture_alloc, true);
    start_command("AT+SCREEN\r\n");
    expect("ERROR: NO_MEM\r\n");
    assert(!atomic_load(&fail_capture_alloc) && !strstr(output, "+SCREEN") && !strstr(output, "OK\r\n"));

    start_command("AT+SCREEN 1\r\n");
    expect("ERROR: NOT_FOUND\r\n");
    assert(!strstr(output, "+SCREEN") && !strstr(output, "OK\r\n"));

    start_command("AT+SCREEN\r\n");
    expect("OK\r\n");
    assert(strstr(output, "+SCREEN-DATA: seq=0 hex=00F8E0071F00") && strstr(output, "+SCREEN-END: chunks=1200"));

    start_command("AT+UI TAP 319 0 extra\r\n");
    expect("ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    start_command("AT+UI TAP 319 -1\r\n");
    expect("ERROR: INVALID_ARG\r\n");
    start_command("AT+UI TAP 319 0\r\n");
    expect("OK\r\n");
    assert(!strcmp(output, "OK\r\n"));
}
#endif

static void mono_commands(void)
{
    show_page(0);
    start_command("AT+UI\r\n");
    expect("view=test%20page objects=1");
    assert(strstr(output, "\r\nOK\r\n"));
    start_command("AT+STATUS ui\r\n");
    expect("view=test%20page objects=1");
    assert(strstr(output, "\r\nOK\r\n"));
    start_command("AT+UI PERF\r\n");
    expect("+UI-THEME: direct=4 fallback=2\r\n");
    start_command("AT+UI SLEEP\r\n");
    expect("ERROR: INVALID_STATE\r\n");
    show_page(2);
    start_command("AT+UI SLEEP\r\n");
    expect("ERROR: INVALID_STATE\r\n");
    show_page(3);
    atomic_store(&physical, true);
    xiaotai_delay_ms(3 * XIAOTAI_UI_POINTER_READ_MS);
    start_command("AT+UI SLEEP\r\n");
    expect("ERROR: INVALID_STATE\r\n");
    atomic_store(&physical, false);
    xiaotai_delay_ms(3 * XIAOTAI_UI_POINTER_READ_MS);

    atomic_store(&hold_controls, true);
    start_command("AT+UI ACTIONS\r\n");
    owner_unblocked();
    expect("+UI-ACTION: x=1 y=2 w=8 h=4 enabled=1 intent=fixture.play text_hex=506C6179\r\n");
    set_activity(true);
    start_command("AT+SCREEN\r\n");
    expect("ERROR: BUSY ACTIVITY\r\n");
    assert(!ui_diag_pending(ui_display_at(0)));
    set_activity(false);
    const uint32_t writes = ui_display_at(0)->flushes;
    atomic_store(&hold_capture, true);
    start_command("AT+SCREEN\r\n");
    owner_unblocked();
    expect("+SCREEN: format=MONO1_VLSB width=128 height=64 view=test%20page bytes=1024 fnv=");
    assert(strstr(output, "+SCREEN-END: chunks=8 fnv=") && strstr(output, "OK\r\n"));
    assert(ui_display_at(0)->flushes == writes);
    start_command("AT+UI SLEEP\r\n");
    expect("OK\r\n");
    assert(!atomic_load(&awake));
    start_command("AT+UI TAP 128 0\r\n");
    expect("ERROR: INVALID_ARG\r\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xiaotai_signal_t *const signals[] = {&start_at, &done_at, &owner_done, &entered, &proceed};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    xiaotai_display_desc_t d = UI_CORE_SCREEN ? ui_fake_desc(320, 240, XIAOTAI_PIXEL_RGB565)
                                             : ui_fake_desc(128, 64, XIAOTAI_PIXEL_MONO1_VLSB);
    d.touch = true;
    if (UI_CORE_SCREEN) {
        d.render = XIAOTAI_RENDER_PARTIAL;
        d.transfer_pixels = 3200;
        d.flags = XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_DMA_BUFFER | XIAOTAI_DISPLAY_EVEN_AREAS;
    }
    ui_fake_board_set_displays(&d, 1);
    xiaotai_display_t *display = (xiaotai_display_t *)ui_fake_display(0);
    display->read_touch = touch;
    display->set_backlight = light;
    assert(xiaotai_hal_init() == XIAOTAI_OK);
    assert(xiaotai_at_configure(&(xiaotai_at_config_t){.task_stack_bytes = XIAOTAI_AT_NO_TASK,
        .no_platform_console = true, .stream_timeout_ms = WAIT_MS}) == XIAOTAI_OK);
    static const xiaotai_at_io_t io = {.name = "ui_fixture", .read = read_at, .write = write_at};
    assert(xiaotai_at_attach(&io) == XIAOTAI_OK && xiaotai_at_open() == XIAOTAI_OK);
    static const xiaotai_module_t log = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "fixture", .init = theme};
    static const xiaotai_module_t *const modules[] = {&log, &at, &ui};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 3, .applications = apps,
        .application_count = 1};
    assert(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    pthread_t console;
    assert(!pthread_create(&console, NULL, run_at, NULL));

    if (UI_CORE_SCREEN) {
#if UI_CORE_SCREEN
        screen_wire(argc == 2 && !strcmp(argv[1], "--dump"));
#endif
    } else mono_commands();
    atomic_store(&exit_at, true);
    xiaotai_signal_give(&start_at);
    assert(!pthread_join(console, NULL));
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) xiaotai_signal_deinit(signals[i]);
    return 0;
}
