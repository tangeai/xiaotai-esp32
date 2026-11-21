// SPDX-License-Identifier: MIT
#include "ui_test.h"
#include "ui_internal.h"
#include "xiaotai_clock_host.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai/theme_lcd480.h"
#include "lvgl.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

enum { WAIT_MS = 4000, SMALL_W = 480, SMALL_H = 320, WIDE_W = 1024, WIDE_H = 600 };
enum { VIDEO_X = 16, VIDEO_Y = 160, VIDEO_COLOR = 0xf800, LINK = 23 };
static xiaotai_signal_t owner_done;
static bool running, signal_ready, check_video, check_clock;
static atomic_uint small_backgrounds, wide_backgrounds;
static atomic_uint video_polls, video_frames, panel_color, viewport_deletes;
static atomic_bool frame_ready, viewport_found;
static bool track_viewport;
static void *video_display_part;
static uint32_t clock_now;

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);

void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes)
{
    void *ptr = __real_xiaotai_mem_alloc(kind, bytes);
    if (ptr && kind == XIAOTAI_MEM_BULK) {
        if (bytes == (size_t)SMALL_W * SMALL_H * 2U) atomic_fetch_add(&small_backgrounds, 1);
        if (bytes == (size_t)WIDE_W * WIDE_H * 2U) atomic_fetch_add(&wide_backgrounds, 1);
    }
    return ptr;
}

static void viewport_deleted(lv_event_t *event)
{
    (void)event;
    atomic_fetch_add(&viewport_deletes, 1);
}

static lv_obj_t *find_viewport(lv_obj_t *obj)
{
    if (lv_obj_check_type(obj, &lv_image_class)) {
        const lv_image_dsc_t *image = lv_image_get_src(obj);
        if (image && lv_image_src_get_type(image) == LV_IMAGE_SRC_VARIABLE &&
            image->header.cf == LV_COLOR_FORMAT_A1 && image->header.w == SMALL_W && image->header.h == SMALL_H)
            return obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = find_viewport(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}

static xiaotai_err_t present(void *self, const xiaotai_rect_t *area, const void *pixels)
{
    assert(!xiaotai_on_owner() && pixels && area->width && area->height);
    if (self != video_display_part) return XIAOTAI_OK;
    if (!track_viewport) {
        lv_obj_t *screen = lv_display_get_screen_active(lv_display_get_default());
        lv_obj_t *viewport = find_viewport(screen);
        if (viewport) {
            assert(lv_obj_has_flag(viewport, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_add_event_cb(viewport, viewport_deleted, LV_EVENT_DELETE, NULL));
            track_viewport = true;
            atomic_store(&viewport_found, true);
        }
    }
    if (VIDEO_X >= area->x && VIDEO_X < area->x + area->width && VIDEO_Y >= area->y && VIDEO_Y < area->y + area->height) {
        const uint8_t *p = (const uint8_t *)pixels + ((VIDEO_Y - area->y) * area->width + VIDEO_X - area->x) * 2U;
        atomic_store(&panel_color, p[0] | ((unsigned)p[1] << 8U));
    }
    return XIAOTAI_OK;
}

static int plain_match(const xiaotai_display_desc_t *desc)
{
    return desc->pixel == XIAOTAI_PIXEL_RGB565 && desc->width == 320 && desc->height == 240 ? 100 : 0;
}

static xiaotai_err_t plain_start(const xiaotai_ui_host_t *host, void **state)
{
    return xiaotai_theme_lcd_start(host, NULL, state);
}

static xiaotai_err_t app_init(void)
{
    static const xiaotai_theme_t plain = {.name = "no-viewport", .backend = XIAOTAI_UI_BACKEND_LVGL,
        .match = plain_match, .start = plain_start, .render = xiaotai_theme_lcd_render,
        .stop = xiaotai_theme_lcd_stop, .controls = xiaotai_theme_lcd_controls, .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES};
    xiaotai_err_t err = xiaotai_engine_theme_lcd480_init();
    if (err == XIAOTAI_OK && (check_video || check_clock)) err = xiaotai_ui_theme_register(&plain);
    return err;
}

static int setup(const uint16_t dims[][2], size_t count)
{
    xiaotai_display_desc_t descs[UI_FAKE_DISPLAYS_MAX];
    CHECK(count <= UI_FAKE_DISPLAYS_MAX);
    for (size_t i = 0; i < count; ++i) {
        descs[i] = ui_fake_desc(dims[i][0], dims[i][1], XIAOTAI_PIXEL_RGB565);
        descs[i].transfer_pixels = (uint32_t)dims[i][0] * 16U;
        descs[i].touch = true;
    }
    ui_fake_board_set_displays(descs, count);
    if (check_video) {
        xiaotai_display_t *part = (xiaotai_display_t *)ui_fake_display(0);
        video_display_part = part->self;
        part->present = present;
    }
    CHECK(xiaotai_signal_init(&owner_done) == XIAOTAI_OK);
    signal_ready = true;
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = xiaotai_engine_ui_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_application_t app = {.name = "lcd480-fixture", .init = app_init};
    static const xiaotai_module_t *const modules[] = {&ui};
    static const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t cfg = {.modules = modules, .module_count = 1, .applications = apps, .application_count = 1};
    CHECK(xiaotai_framework_start(&cfg) == XIAOTAI_OK);
    running = true;
    for (uint8_t i = 0; i < count; ++i) {
        xiaotai_ui_info_t info;
        CHECK(xiaotai_ui_inspect(i, &info) == XIAOTAI_OK && info.ready && !info.owner_timer);
        CHECK(!strcmp(info.theme, dims[i][0] == 320 ? "no-viewport" : XIAOTAI_THEME_LCD480_NAME));
        if (check_clock) continue;
        size_t controls = 1;
        CHECK(xiaotai_ui_controls(i, NULL, 0, &controls, WAIT_MS) == XIAOTAI_OK && !controls);
    }
    return 0;
}

static void run_owner(void (*work)(void *), void *ctx)
{
    assert(xiaotai_run(work, ctx) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&owner_done, WAIT_MS));
}

static void show_on_owner(void *ctx)
{
    assert(xiaotai_on_owner());
    xiaotai_view_show(ctx);
    xiaotai_signal_give(&owner_done);
}

static void stage(xiaotai_view_t *view)
{
    xiaotai_ui_info_t info;
    assert(xiaotai_ui_inspect(view->display, &info) == XIAOTAI_OK);
    const uint32_t before = info.frames;
    run_owner(show_on_owner, view);
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        assert(xiaotai_ui_inspect(view->display, &info) == XIAOTAI_OK && !info.present_fault && !info.present_errors);
        const char *id = view->layer == XIAOTAI_LAYER_OVERLAY ? info.overlay : info.page;
        const char *expected = view->flags & XIAOTAI_VIEW_DISMISS ? "" : view->id;
        if (info.frames != before && !strcmp(id, expected)) {
            /* A diagnostics request completes after this display's LVGL
             * step; `frames` alone may already count a not-yet-flushed view. */
            size_t controls;
            assert(xiaotai_ui_controls(view->display, NULL, 0, &controls, WAIT_MS) == XIAOTAI_OK);
            return;
        }
        xiaotai_delay_ms(1);
    }
    assert(!"display did not render staged LCD480 view");
}

static int match(void)
{
    static const struct { uint16_t w, h; xiaotai_pixel_t format; uint32_t flags; int score; } cases[] = {
        {480, 320, XIAOTAI_PIXEL_RGB565, 0, 100},
        {1024, 600, XIAOTAI_PIXEL_RGB565, 0, 60},
        {481, 320, XIAOTAI_PIXEL_RGB565, 0, 60},
        {480, 479, XIAOTAI_PIXEL_RGB565, 0, 60},
        {479, 320, XIAOTAI_PIXEL_RGB565, 0, 0},
        {480, 319, XIAOTAI_PIXEL_RGB565, 0, 0},
        {480, 480, XIAOTAI_PIXEL_RGB565, 0, 0},
        {480, 800, XIAOTAI_PIXEL_RGB565, 0, 0},
        {480, 320, XIAOTAI_PIXEL_MONO1_VLSB, 0, 0},
        {480, 320, XIAOTAI_PIXEL_RGB888, 0, 0},
        {480, 320, XIAOTAI_PIXEL_RGB565, XIAOTAI_DISPLAY_SLOW_REFRESH, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        xiaotai_display_desc_t desc = ui_fake_desc(cases[i].w, cases[i].h, cases[i].format);
        desc.flags = cases[i].flags;
        CHECK(xiaotai_theme_lcd480.match(&desc) == cases[i].score);
    }
    CHECK(xiaotai_theme_lcd480.match(NULL) == 0);
    return 0;
}

static int scale(void)
{
    static const uint16_t dims[][2] = {{SMALL_W, SMALL_H}, {WIDE_W, WIDE_H}};
    static const xiaotai_view_action_t actions[] = {
        {.role = XIAOTAI_ROLE_PRIMARY, .intent = "lcd.start"},
        {.icon = XIAOTAI_ICON_MENU, .intent = "lcd.menu"},
        {.icon = XIAOTAI_ICON_PHONE, .intent = "lcd.phone"},
    };
    CHECK(setup(dims, 2) == 0);
    for (uint8_t index = 0; index < 2; ++index) {
        xiaotai_view_t view = {.id = "home", .kind = XIAOTAI_VIEW_HOME, .display = index,
            .title = "XiaoTai", .actions = actions, .action_count = sizeof(actions) / sizeof(*actions)};
        stage(&view);
        xiaotai_ui_control_t controls[3];
        size_t count = 0;
        CHECK(xiaotai_ui_controls(index, controls, 3, &count, WAIT_MS) == XIAOTAI_OK && count == 3);
        const unsigned w = dims[index][0], h = dims[index][1];
        unsigned found = 0;
        for (size_t i = 0; i < count; ++i) {
            const xiaotai_ui_control_t *c = &controls[i];
            CHECK(c->index < 3 && !strcmp(c->intent, actions[c->index].intent));
            CHECK(c->layer == XIAOTAI_LAYER_PAGE && c->element == XIAOTAI_UI_ELEMENT_ACTION && !c->state);
            CHECK(!(found & (1U << c->index)));
            found |= 1U << c->index;
            if (!c->index) {
                const unsigned fw = 224U * h / 240U, fh = 108U * h / 240U;
                CHECK(c->width == fw && c->height == fh && c->x == (int)((w - fw) / 2U) && c->y == (int)((h - fh) / 2U));
            } else {
                static const unsigned base[][4] = {{280, 6, 32, 24}, {266, 150, 46, 46}};
                const unsigned *box = base[c->index - 1U];
                CHECK(c->x == (int)(box[0] * w / 320U) && c->y == (int)(box[1] * h / 240U));
                CHECK(c->width == box[2] * w / 320U && c->height == box[3] * h / 240U);
            }
        }
        CHECK(found == 7U);
    }
    return 0;
}

static bool pending(void *ctx)
{
    (void)ctx;
    assert(!xiaotai_on_owner());
    atomic_fetch_add(&video_polls, 1);
    if (!atomic_exchange(&frame_ready, false)) return false;
    atomic_fetch_add(&video_frames, 1);
    return true;
}

static bool paint(void *ctx, const xiaotai_ui_surface_t *target,
                  uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)ctx;
    assert(!xiaotai_on_owner() && !x && !y && w == SMALL_W && h == SMALL_H && target->pixel == XIAOTAI_PIXEL_RGB565);
    if (!atomic_load(&video_frames)) return false;
    for (unsigned row = 0; row < target->height; ++row) {
        uint8_t *p = target->pixels + row * target->stride;
        for (unsigned col = 0; col < target->width; ++col) {
            p[col * 2U] = (uint8_t)VIDEO_COLOR;
            p[col * 2U + 1U] = (uint8_t)(VIDEO_COLOR >> 8U);
        }
    }
    return true;
}

typedef struct { uint8_t display; uint32_t generation; xiaotai_err_t result; } source_request_t;

static void attach_on_owner(void *ctx)
{
    source_request_t *request = ctx;
    assert(xiaotai_on_owner());
    const xiaotai_ui_video_source_t source = {.pending = pending, .paint = paint};
    request->result = xiaotai_ui_video_set(request->display, &source, request->generation);
    xiaotai_signal_give(&owner_done);
}

static void clear_on_owner(void *ctx)
{
    const uint32_t generation = *(const uint32_t *)ctx;
    assert(xiaotai_on_owner());
    xiaotai_ui_video_clear(0, generation);
    xiaotai_signal_give(&owner_done);
}

static bool wait_color(bool red)
{
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        if ((atomic_load(&panel_color) == VIDEO_COLOR) == red) return true;
        xiaotai_delay_ms(1);
    }
    return false;
}

static int video(void)
{
    static const uint16_t dims[][2] = {{SMALL_W, SMALL_H}, {320, 240}};
    check_video = true;
    CHECK(setup(dims, 2) == 0);
    /* An incoming link installs its source before any view/ready event. */
    source_request_t request = {.generation = LINK};
    atomic_store(&frame_ready, true);
    run_owner(attach_on_owner, &request);
    CHECK(request.result == XIAOTAI_OK);
    request.display = 1;
    run_owner(attach_on_owner, &request);
    CHECK(request.result == XIAOTAI_ERR_NOT_SUPPORTED);

    xiaotai_view_t call = {.kind = XIAOTAI_VIEW_CALL, .id = "call", .title = "Incoming call"};
    stage(&call);
    CHECK(atomic_load(&viewport_found) && !atomic_load(&video_polls) && !atomic_load(&video_frames));
    call.flags = XIAOTAI_VIEW_VIDEO;
    stage(&call);
    CHECK(wait_color(true) && atomic_load(&video_frames) == 1);

    xiaotai_view_t overlay = {.kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_OVERLAY,
        .id = "hold", .title = "Pause?"};
    stage(&overlay);
    CHECK(wait_color(false) && atomic_load(&panel_color) != 0);
    overlay.flags = XIAOTAI_VIEW_DISMISS;
    stage(&overlay);
    CHECK(wait_color(true) && !atomic_load(&viewport_deletes));

    uint32_t old = LINK - 1;
    run_owner(clear_on_owner, &old);
    xiaotai_ui_info_t info;
    CHECK(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.video_stale == 1);
    CHECK(wait_color(true));
    xiaotai_view_t stream = {.kind = XIAOTAI_VIEW_NOTICE, .id = "stream", .title = "Monitoring"};
    stage(&stream);
    CHECK(wait_color(false) && !atomic_load(&viewport_deletes));
    const unsigned polls = atomic_load(&video_polls);
    xiaotai_delay_ms(30);
    CHECK(atomic_load(&video_polls) == polls);
    atomic_store(&frame_ready, true);
    stream.flags = XIAOTAI_VIEW_VIDEO;
    stage(&stream);
    CHECK(wait_color(true) && atomic_load(&video_frames) == 2 && !atomic_load(&viewport_deletes));

    uint32_t current = LINK;
    run_owner(clear_on_owner, &current);
    CHECK(wait_color(false));
    return 0;
}

static uint16_t color(const xiaotai_ui_capture_t *frame, unsigned x, unsigned y)
{
    const uint8_t *p = frame->pixels + (y * frame->width + x) * 2U;
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8U));
}

static bool gradient(uint8_t index, unsigned w, unsigned h)
{
    xiaotai_ui_capture_t frame;
    assert(xiaotai_ui_capture(index, &frame, WAIT_MS) == XIAOTAI_OK);
    assert(frame.width == w && frame.height == h && frame.size == w * h * 2U);
    const uint16_t centre = lv_color_to_u16(lv_color_hex(0x1c1f22));
    const uint16_t edge = lv_color_to_u16(lv_color_hex(0x101315));
    const bool expected = color(&frame, 1, 1) == edge && color(&frame, w / 3U, 3U * h / 4U) == centre;
    xiaotai_ui_capture_free(&frame);
    return expected;
}

static int background(void)
{
    static const uint16_t dims[][2] = {{SMALL_W, SMALL_H}, {WIDE_W, WIDE_H}};
    CHECK(setup(dims, 2) == 0);
    CHECK(!atomic_load(&small_backgrounds) && !atomic_load(&wide_backgrounds));
    for (uint8_t index = 0; index < 2; ++index) {
        const unsigned w = dims[index][0], h = dims[index][1];
        xiaotai_view_t home = {.kind = XIAOTAI_VIEW_HOME, .id = "home", .display = index};
        xiaotai_view_t clock = {.kind = XIAOTAI_VIEW_CLOCK, .id = "clock", .display = index};
        xiaotai_view_t page = {.kind = XIAOTAI_VIEW_NOTICE, .id = "notice", .display = index};
        stage(&home);
        CHECK(gradient(index, w, h));
        stage(&clock);
        CHECK(gradient(index, w, h));
        stage(&page);
        stage(&home);
        CHECK(gradient(index, w, h));
        CHECK(atomic_load(&small_backgrounds) == 1 && atomic_load(&wide_backgrounds) == index);
    }
    return 0;
}

static uint32_t manual_clock(void) { return clock_now; }

static void step_clock(uint32_t now)
{
    clock_now = now;
    for (uint8_t i = 0; i < 3; ++i) ui_driver_step(ui_display_at(i), now);
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *match = find_label(lv_obj_get_child(obj, i), text);
        if (match) return match;
    }
    return NULL;
}

static lv_obj_t *clock_label(uint8_t index, const char *text)
{
    ui_display_t *d = ui_display_at(index);
    ui_lvgl_enter(d);
    lv_obj_t *out = find_label(lv_display_get_screen_active(xiaotai_ui_host_root(&d->host)), text);
    ui_lvgl_leave(d);
    return out;
}

static int home_clock(void)
{
    static const uint16_t dims[][2] = {{SMALL_W, SMALL_H}, {WIDE_W, WIDE_H}, {320, 240}};
    check_clock = true;
    xiaotai_clock_host_reset();
    static const char *const servers[] = {"clock.invalid"};
    const xiaotai_clock_config_t config = {.servers = servers, .server_count = 1, .timezone = "CST-8"};
    CHECK(xiaotai_clock_init(&config) == XIAOTAI_OK);
    ui_core_set_clock(manual_clock);
    ui_driver_manual(true);
    CHECK(setup(dims, 3) == 0);

    xiaotai_view_t home = {.id = "home", .kind = XIAOTAI_VIEW_HOME};
    ui_view_accept(&home);
    home.display = 2;
    ui_view_accept(&home);
    step_clock(0);
    lv_obj_t *small = clock_label(0, "--:--");
    CHECK(small && !clock_label(2, "--:--") && !ui_display_at(2)->clock_due);

    const int64_t unix_ms = INT64_C(1704067198000); /* 07:59:58 local time */
    CHECK(xiaotai_clock_set_unix_ms(unix_ms) == XIAOTAI_OK);
    xiaotai_clock_host_set_wall_ms(unix_ms);
    home.display = 1;
    ui_view_accept(&home);
    step_clock(0);
    lv_obj_t *wide = clock_label(1, "07:59");
    CHECK(wide && ui_display_at(1)->clock_due);
    xiaotai_clock_host_advance_ms(1000);
    step_clock(1000);
    CHECK(clock_label(0, "07:59") == small);
    xiaotai_clock_host_advance_ms(1000);
    step_clock(2000);
    CHECK(clock_label(0, "08:00") == small && clock_label(1, "08:00") == wide);

    xiaotai_view_t overlay = {.id = "overlay", .kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_OVERLAY};
    ui_view_accept(&overlay);
    ui_view_accept(&(xiaotai_view_t){.id = "away", .kind = XIAOTAI_VIEW_NOTICE, .display = 1});
    step_clock(2001);
    CHECK(!ui_display_at(0)->clock_due && !ui_display_at(1)->clock_due && !ui_display_at(2)->clock_due);
    const uint32_t covered_frames = ui_display_at(0)->frames, away_frames = ui_display_at(1)->frames;
    xiaotai_clock_host_advance_ms(60000);
    step_clock(62000);
    CHECK(ui_display_at(0)->frames == covered_frames && ui_display_at(1)->frames == away_frames);
    CHECK(!clock_label(0, "08:01") && !clock_label(1, "08:01") && !clock_label(2, "08:01"));

    overlay.flags = XIAOTAI_VIEW_DISMISS;
    ui_view_accept(&overlay);
    ui_view_accept(&home);
    ui_view_accept(&(xiaotai_view_t){.id = "clock", .kind = XIAOTAI_VIEW_CLOCK, .display = 2});
    step_clock(62000);
    CHECK(clock_label(0, "08:01") == small && clock_label(1, "08:01") && clock_label(2, "08:01"));
    xiaotai_clock_host_reset();
    return 0;
}

int main(void)
{
    const char *name = LCD480_TEST_CASE;
    int result = 1;
    if (!strcmp(name, "match")) result = match();
    else if (!strcmp(name, "scale")) result = scale();
    else if (!strcmp(name, "video")) result = video();
    else if (!strcmp(name, "background")) result = background();
    else if (!strcmp(name, "clock")) result = home_clock();
    if (running) {
        xiaotai_ui_reset();
        xiaotai_framework_reset();
        if (check_video && !result) CHECK(atomic_load(&viewport_deletes) == 1);
    }
    if (signal_ready) xiaotai_signal_deinit(&owner_done);
    return result;
}
