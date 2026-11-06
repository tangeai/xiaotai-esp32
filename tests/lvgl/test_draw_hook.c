#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai_lvgl.h"

enum { WIDTH = 32, HEIGHT = 24 };
static uint16_t draw_buffer[WIDTH * HEIGHT], frame[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
static const uint16_t green[16] = {
    0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0,
    0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0, 0x07e0,
};
static const lv_image_dsc_t picture = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 4, .h = 4, .stride = 8},
    .data_size = sizeof(green), .data = (const uint8_t *)green,
};
static unsigned calls, direct_calls, flushes;
static bool direct;
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    assert(area->x1 == 0 && area->y1 == 0 && area->x2 == WIDTH - 1 && area->y2 == HEIGHT - 1);
    memcpy(frame, pixels, sizeof(frame));
    ++flushes;
    lv_display_flush_ready(display);
}
static bool image_hook(lv_draw_task_t *task, const lv_draw_image_dsc_t *image, const lv_area_t *area)
{
    ++calls;
    if (!direct) return false;
    assert(image->src == &picture);
    xiaotai_rgb565_surface_t surface;
    assert(xiaotai_lvgl_rgb565_surface(task, &surface));
    lv_area_t clip;
    if (lv_area_intersect(&clip, area, surface.clip_area) &&
        lv_area_intersect(&clip, &clip, surface.buf_area)) {
        for (int y = clip.y1; y <= clip.y2; ++y)
            for (int x = clip.x1; x <= clip.x2; ++x)
                surface.buf[(y - surface.buf_area->y1) * surface.stride + x - surface.buf_area->x1] = 0x07e0;
    }
    ++direct_calls;
    return true;
}
static void render(lv_display_t *display, lv_obj_t *object)
{
    unsigned before = flushes;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(object);
    lv_refr_now(display);
    assert(flushes > before);
}
int main(void)
{
    assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 5);
    lv_init();
    assert(xiaotai_lvgl_image_hook(NULL) == ESP_ERR_INVALID_ARG);
    assert(xiaotai_lvgl_image_hook(image_hook) == ESP_OK);
    assert(xiaotai_lvgl_image_hook(image_hook) == ESP_OK);
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_t *object = lv_image_create(lv_screen_active());
    lv_image_set_src(object, &picture);
    const int positions[][2] = {{3, 4}, {-2, 22}, {30, -1}};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        lv_obj_set_pos(object, positions[i][0], positions[i][1]);
        direct = false;
        render(display, object);
        memcpy(expected, frame, sizeof(expected));
        bool nonblank = false;
        for (unsigned p = 0; p < WIDTH * HEIGHT; ++p) nonblank |= frame[p] == 0x07e0;
        assert(nonblank);
        direct = true;
        render(display, object);
        assert(memcmp(expected, frame, sizeof(frame)) == 0);
    }
    assert(calls >= 6 && direct_calls == 3);
    lv_display_delete(display);
    lv_deinit();
    puts("LVGL 9 draw task: software fallback and clipped RGB565 direct path match");
    return 0;
}
