// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_s3_touch_wake.py
#include "lvgl/ui_lvgl_private.h"
#include "ui_test.h"

#include <string.h>

enum { WIDTH = 48, HEIGHT = 40, BOX = 20 };
static uint16_t panel[WIDTH * HEIGHT];
static const uint8_t placeholder[BOX * BOX * 2];
static xiaotai_rect_t regions[64];
static size_t region_count;
static unsigned reads, clicks, releases;
static unsigned wake_attempts;
static uint8_t backlight;
static bool fail_wake;
static bool down;
static uint32_t time_ms;
static unsigned swipes;
static int finger_x = 8, finger_y = 8;
static lv_obj_t *widget;
static const xiaotai_ui_host_t *s_host;

static const lv_image_dsc_t image = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .w = BOX, .h = BOX, .stride = BOX * 2},
    .data_size = sizeof(placeholder),
    .data = placeholder,
};

static uint32_t test_clock(void) { return time_ms; }

static xiaotai_err_t display_present(void *self, const xiaotai_rect_t *area, const void *data)
{
    (void)self;
    if (area->x + area->width > WIDTH || area->y + area->height > HEIGHT || !data ||
        region_count >= sizeof(regions) / sizeof(regions[0])) return XIAOTAI_ERR_INVALID_ARG;
    regions[region_count++] = *area;
    const uint8_t *source = data;
    for (uint16_t y = 0; y < area->height; ++y) {
        memcpy(&panel[(area->y + y) * WIDTH + area->x], source + (size_t)y * area->width * 2,
               (size_t)area->width * 2);
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t touch_read(void *self, xiaotai_touch_point_t *points, size_t capacity, size_t *n)
{
    (void)self;
    ++reads;
    *n = 0;
    if (down && capacity) {
        points[0] = (xiaotai_touch_point_t){(uint16_t)finger_x, (uint16_t)finger_y};
        *n = 1;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t set_backlight(void *self, uint8_t level)
{
    (void)self;
    if (level) {
        ++wake_attempts;
        if (fail_wake) return XIAOTAI_ERR_TIMEOUT;
    }
    backlight = level;
    return XIAOTAI_OK;
}

static void clicked(lv_event_t *event)
{
    (void)event;
    ++clicks;
}

static void swiped(lv_event_t *event)
{
    (void)event;
    ++swipes;
}

static int score(const xiaotai_display_desc_t *desc)
{
    return desc->pixel == XIAOTAI_PIXEL_RGB565 ? 100 : 0;
}

static xiaotai_err_t start(const xiaotai_ui_host_t *host, void **state)
{
    lv_display_t *root = xiaotai_ui_host_root(host);
    lv_obj_t *screen = root ? lv_display_get_screen_active(root) : NULL;
    if (!screen) return XIAOTAI_ERR_INVALID_STATE;
    s_host = host;
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    *state = screen;
    if (!strcmp(LVGL_CASE, "video")) {
        widget = lv_image_create(screen);
        lv_image_set_src(widget, &image);
        lv_obj_set_pos(widget, 4, 6);
        return xiaotai_ui_host_video_region(host, widget);
    }
    if (!strcmp(LVGL_CASE, "scroll") || !strcmp(LVGL_CASE, "sampler")) {
        lv_obj_add_event_cb(screen, swiped, LV_EVENT_GESTURE, NULL);
        lv_obj_t *list = lv_obj_create(screen);
        lv_obj_remove_style_all(list);
        lv_obj_set_size(list, 24, HEIGHT);
        for (int i = 0; i < 200; ++i) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_remove_style_all(row);
            lv_obj_set_pos(row, 0, i * 20);
            lv_obj_set_size(row, 24, 20);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(row, clicked, LV_EVENT_CLICKED, NULL);
        }
        widget = list;
        return XIAOTAI_OK;
    }
    if (!strcmp(LVGL_CASE, "pointer")) {
        widget = lv_button_create(screen);
        lv_obj_set_pos(widget, 2, 2);
        lv_obj_set_size(widget, 20, 15);
        lv_obj_add_event_cb(widget, clicked, LV_EVENT_CLICKED, NULL);
    }
    return XIAOTAI_OK;
}

static void render(void *state, const xiaotai_ui_scene_t *scene) { (void)state; (void)scene; }

static const xiaotai_theme_t theme = {
    .name = "fixture", .backend = XIAOTAI_UI_BACKEND_LVGL,
    .match = score, .start = start, .render = render,
};

static void step(uint32_t now)
{
    time_ms = now;
    ui_driver_step(ui_display_at(0), now);
}

static int setup(bool touch, uint32_t flags)
{
    xiaotai_display_desc_t desc = ui_fake_desc(WIDTH, HEIGHT, XIAOTAI_PIXEL_RGB565);
    desc.touch = touch;
    desc.flags = flags;
    ui_fake_board_set_displays(&desc, 1);
    xiaotai_display_t *part = (xiaotai_display_t *)ui_fake_display(0);
    part->present = display_present;
    if (touch) {
        part->read_touch = touch_read;
        part->set_backlight = set_backlight;
    }
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    ui_core_set_clock(test_clock);
    ui_driver_manual(true);
    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK);
    CHECK(xiaotai_ui_theme_register(&theme) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    xiaotai_ui_info_t info;
    CHECK(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.ready);
    step(0);
    CHECK(region_count && s_host);
    region_count = 0;
    return 0;
}

static xiaotai_err_t slow_present(void *self, const xiaotai_rect_t *area, const void *data)
{
    (void)self;
    (void)area;
    (void)data;
    xiaotai_delay_ms(120);   /* a frame longer than a tap */
    return XIAOTAI_OK;
}

static xiaotai_err_t sampler_init(void)
{
    const xiaotai_err_t err = xiaotai_engine_ui_init();
    return err == XIAOTAI_OK ? xiaotai_ui_theme_register(&theme) : err;
}

static int physical_contact(bool held_sample, bool forwarded)
{
    const unsigned before = reads;
    down = true;
    for (unsigned sample = 0; sample < (held_sample ? 3U : 2U); ++sample) {
        if (sample == (held_sample ? 2U : 1U)) down = false;
        step(time_ms + XIAOTAI_UI_POINTER_READ_MS);
        const ui_pointer_t *pointer = &ui_display_at(0)->pointer;
        CHECK(reads == before + sample + 1U);
        CHECK(pointer->point.x == 8 && pointer->point.y == 8);
        CHECK(pointer->down == (down && forwarded));
    }
    return 0;
}

static int pointer_wake_case(void)
{
    const unsigned baseline_reads = reads, baseline_clicks = clicks, baseline_wakes = wake_attempts;
    CHECK(xiaotai_ui_sleep(0) == XIAOTAI_OK && !xiaotai_ui_awake(0) && backlight == 0);
    CHECK(physical_contact(true, false) == 0);
    CHECK(xiaotai_ui_awake(0) && backlight == 100 && wake_attempts == baseline_wakes + 1U);
    CHECK(clicks == baseline_clicks);
    CHECK(physical_contact(false, true) == 0);
    CHECK(clicks == baseline_clicks + 1U && wake_attempts == baseline_wakes + 1U);

    CHECK(xiaotai_ui_sleep(0) == XIAOTAI_OK && !xiaotai_ui_awake(0) && backlight == 0);
    fail_wake = true;
    CHECK(physical_contact(true, false) == 0);
    CHECK(!xiaotai_ui_awake(0) && backlight == 0 && wake_attempts == baseline_wakes + 2U);
    CHECK(clicks == baseline_clicks + 1U);
    fail_wake = false;
    CHECK(physical_contact(false, false) == 0);
    CHECK(xiaotai_ui_awake(0) && backlight == 100 && wake_attempts == baseline_wakes + 3U);
    CHECK(clicks == baseline_clicks + 1U);
    CHECK(physical_contact(false, true) == 0);
    CHECK(clicks == baseline_clicks + 2U && wake_attempts == baseline_wakes + 3U && reads == baseline_reads + 12U);
    return 0;
}

static int pointer_case(void)
{
    CHECK(setup(true, 0) == 0);
    CHECK(reads == 1 && clicks == 0);
    down = true;
    step(15);
    step(16);
    CHECK(reads == 2 && clicks == 0); /* read from the shared logical sample */
    down = false;
    step(30);
    CHECK(reads == 3 && clicks == 1);
    CHECK(xiaotai_ui_tap(0, 8, 8) == XIAOTAI_OK);
    step(31);
    step(150);
    step(151);
    CHECK(clicks == 2); /* synthetic input came through the same LVGL indev */
    return pointer_wake_case();
}

static int32_t scrolled(void)
{
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    const int32_t y = lv_obj_get_scroll_y(widget);
    ui_lvgl_leave(display);
    return y;
}

/* One finger step: the read, and between it and the next one the steps the UI
 * task makes for other reasons. */
static void finger(int x, int y, bool touching)
{
    finger_x = x;
    finger_y = y;
    down = touching;
    region_count = 0;
    step(time_ms + 5);
    step(time_ms + 5);
    step(time_ms + 5);
}

/* Drag a list, let go, and see how far it coasts when frames take `frame` ms. */
static int32_t coast(unsigned frame)
{
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    lv_obj_scroll_to_y(widget, 0, LV_ANIM_OFF);
    ui_lvgl_leave(display);
    for (int i = 0; i <= 8; ++i) finger(10, 38 - 4 * i, true);
    for (int i = 0; i < 4; ++i) finger(10, 6, false);
    const uint32_t until = time_ms + 3000U;
    while (time_ms < until) {
        region_count = 0;
        step(time_ms + frame);
    }
    return scrolled();
}

static int scroll_case(void)
{
    CHECK(setup(true, 0) == 0);
    /* A swipe beside the list, slower than any one step would count alone:
     * steps without a new sample do not start it over. */
    for (int i = 0; i <= 11; ++i) finger(46 - 2 * i, 30, true);
    CHECK(swipes == 1);
    for (int i = 0; i < 4; ++i) finger(24, 30, false);

    const int32_t quick = coast(10), slow = coast(70);
    printf("coasts %d px with 10 ms frames, %d px with 70 ms frames\n", (int)quick, (int)slow);
    CHECK(quick > 40 && slow > 40 && LV_ABS(quick - slow) * 10 < quick);
    CHECK(clicks == 0);   /* a drag is no tap */

    /* A touch stops a coasting list, and is no tap on what is under it. */
    for (int i = 0; i <= 8; ++i) finger(10, 38 - 4 * i, true);
    for (int i = 0; i < 4; ++i) finger(10, 6, false);
    const int32_t moving = scrolled();
    finger(10, 20, true);
    const int32_t caught = scrolled();
    finger(10, 20, true);
    finger(10, 20, false);
    finger(10, 20, false);
    CHECK(caught >= moving && scrolled() == caught && clicks == 0);
    /* At rest a tap is one. */
    finger(10, 20, true);
    finger(10, 20, false);
    CHECK(clicks == 1);

    /* The list goes away while it coasts. */
    for (int i = 0; i <= 8; ++i) finger(10, 38 - 4 * i, true);
    for (int i = 0; i < 3; ++i) finger(10, 6, false);
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    lv_obj_delete(widget);
    ui_lvgl_leave(display);
    for (int i = 0; i < 5; ++i) {
        region_count = 0;
        step(time_ms + 10);
    }
    return 0;
}

/* The display's own task and the one that reads its touch. */
static int sampler_case(void)
{
    xiaotai_display_desc_t desc = ui_fake_desc(WIDTH, HEIGHT, XIAOTAI_PIXEL_RGB565);
    desc.touch = true;
    ui_fake_board_set_displays(&desc, 1);
    xiaotai_display_t *part = (xiaotai_display_t *)ui_fake_display(0);
    part->present = slow_present;
    part->read_touch = touch_read;
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    static const xiaotai_module_t ui = {.name = "ui", .init = sampler_init, .start = xiaotai_engine_ui_start};
    static const xiaotai_module_t *const modules[] = {&ui};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    xiaotai_delay_ms(150);
    CHECK(ui_display_at(0)->touch && ui_display_at(0)->touch->sampling);
    /* A tap shorter than one frame: both its ends arrive. */
    for (int i = 1; i <= 3; ++i) {
        finger_y = 8;
        down = true;
        xiaotai_delay_ms(40);
        down = false;
        xiaotai_delay_ms(260);
        CHECK(clicks == (unsigned)i);
    }
    xiaotai_ui_reset();
    xiaotai_framework_reset();
    return 0;
}

static int dma_case(void)
{
    xiaotai_display_desc_t desc = ui_fake_desc(WIDTH, HEIGHT, XIAOTAI_PIXEL_RGB565);
    desc.buffer_align = 128;
    desc.transfer_pixels = WIDTH * HEIGHT;
    ui_fake_board_set_displays(&desc, 1);
    ((xiaotai_display_t *)ui_fake_display(0))->present = display_present;
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    ui_core_set_clock(test_clock);
    ui_driver_manual(true);
    CHECK(xiaotai_engine_ui_init() == XIAOTAI_OK && xiaotai_ui_theme_register(&theme) == XIAOTAI_OK);
    xiaotai_engine_ui_start();
    step(0);
    region_count = 0;
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    const lv_area_t fixed = {.x1 = 5, .x2 = 8, .y1 = 2, .y2 = 9};
    lv_obj_invalidate_area(lv_display_get_screen_active(xiaotai_ui_host_root(s_host)), &fixed);
    ui_lvgl_leave(display);
    step(15);
    CHECK(region_count >= 1);
    for (size_t i = 0; i < region_count; ++i)
        CHECK(regions[i].x % 8 == 0 && regions[i].width * regions[i].height * 2U % 128U == 0);
    return 0;
}

static int even_case(uint32_t flags)
{
    CHECK(setup(false, flags) == 0);
    ui_display_t *display = ui_display_at(0);
    ui_lvgl_enter(display);
    lv_obj_t *screen = lv_display_get_screen_active(xiaotai_ui_host_root(s_host));
    lv_area_t fixed = {.x1 = 5, .x2 = 8, .y1 = 2, .y2 = 4};
    lv_obj_invalidate_area(screen, &fixed);
    ui_lvgl_leave(display);
    step(15);
    CHECK(region_count == 1 && regions[0].y == 2 && regions[0].height == 3);
    if (flags) {
        CHECK(regions[0].x == 4 && regions[0].width == 6);
    } else {
        CHECK(regions[0].x == 5 && regions[0].width == 4);
    }
    return 0;
}

typedef struct {
    unsigned pending, paints, n, frame;
    bool pinned;
} demo_video_t;

static bool pending(void *ctx)
{
    demo_video_t *video = ctx;
    ++video->pending;
    if (video->n == video->frame) return false;
    video->frame = video->n;
    video->pinned = true;
    return true;
}

static bool placement(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, xiaotai_ui_rect_t *picture)
{
    demo_video_t *video = ctx;
    if (!video->pinned || w != BOX || h != BOX) return false;
    *picture = (xiaotai_ui_rect_t){.x = (uint16_t)(x + video->frame * 2U), .y = (uint16_t)(y + 2U),
                                  .width = 2, .height = 2};
    return true;
}

static bool paint(void *ctx, const xiaotai_ui_surface_t *s, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    demo_video_t *video = ctx;
    xiaotai_ui_rect_t picture;
    if (!placement(ctx, x, y, w, h, &picture) || s->pixel != XIAOTAI_PIXEL_RGB565 || s->swap_bytes)
        return false;
    ++video->paints;
    bool painted = false;
    const uint16_t color = video->frame == 1 ? 0xf800 : 0x001f;
    for (uint16_t row = picture.y; row < picture.y + picture.height; ++row) {
        if (row < s->y || row >= s->y + s->height) continue;
        for (uint16_t col = picture.x; col < picture.x + picture.width; ++col) {
            if (col < s->x || col >= s->x + s->width) continue;
            uint8_t *out = s->pixels + (size_t)(row - s->y) * s->stride + (col - s->x) * 2U;
            out[0] = (uint8_t)color;
            out[1] = (uint8_t)(color >> 8U);
            painted = true;
        }
    }
    return painted;
}

static void release(void) { ++releases; }

static int video_case(void)
{
    CHECK(setup(false, 0) == 0);
    ui_view_accept(&(xiaotai_view_t){.id = "remote", .flags = XIAOTAI_VIEW_VIDEO});
    step(1);
    demo_video_t video = {.n = 1};
    xiaotai_ui_video_source_t a = {.pending = pending, .paint = paint, .ctx = &video, .placement = placement};
    CHECK(xiaotai_ui_video_set(0, &a, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(ui_video_install(0, &a, 7, release) == XIAOTAI_OK);
    step(10);
    CHECK(video.pending == 1 && video.paints > 0 && panel[8 * WIDTH + 6] == 0xf800);
    region_count = 0;
    step(15);
    CHECK(video.pending == 1 && !region_count);
    video.n = 2;
    step(20);
    CHECK(video.pending == 2 && region_count == 1);
    CHECK(regions[0].x == 6 && regions[0].width == 4 && regions[0].y == 8 && regions[0].height == 2);
    CHECK(panel[8 * WIDTH + 6] == 0 && panel[8 * WIDTH + 8] == 0x001f);
    CHECK(xiaotai_ui_video_set(0, &a, 6) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_ui_video_clear(0, 6);
    CHECK(releases == 0);
    xiaotai_ui_info_t info;
    CHECK(xiaotai_ui_inspect(0, &info) == XIAOTAI_OK && info.video_stale == 2);
    xiaotai_ui_video_clear(0, 7);
    CHECK(releases == 1);
    const unsigned paints = video.paints;
    step(30);
    CHECK(video.paints == paints && video.pending == 2 && panel[8 * WIDTH + 8] == 0);
    xiaotai_ui_video_clear(0, 7);
    CHECK(releases == 1);
    return 0;
}

int main(void)
{
    if (!strcmp(LVGL_CASE, "pointer")) {
        RUN(pointer_case);
    } else if (!strcmp(LVGL_CASE, "even_areas")) {
        CHECK(even_case(XIAOTAI_DISPLAY_EVEN_AREAS) == 0);
        xiaotai_ui_reset();
        CHECK(even_case(0) == 0);
    } else if (!strcmp(LVGL_CASE, "scroll")) {
        RUN(scroll_case);
    } else if (!strcmp(LVGL_CASE, "dma_areas")) {
        RUN(dma_case);
    } else if (!strcmp(LVGL_CASE, "sampler")) {
        RUN(sampler_case);
    } else {
        RUN(video_case);
    }
    xiaotai_ui_reset();
    return 0;
}
