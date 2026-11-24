// SPDX-License-Identifier: MIT
#include "lcd_face_live.h"
#include "lcd_face.h"

#include "src/misc/cache/instance/lv_image_cache.h"
#include "xiaotai/port/mem.h"

typedef struct lcd_face_live {
    lcd_face_t *face;
    lv_image_dsc_t source;
    uint64_t time_ms;
    uint32_t last_sample;
    bool sampled, attached;
    bool canvas_dirty;
    bool direct;
} lcd_face_live_t;

xiaotai_err_t lcd_face_live_start(lcd_state_t *s)
{
    if (!s->variant || !(s->variant->features & XIAOTAI_THEME_LCD_EMOTION)) return XIAOTAI_OK;
    lcd_face_live_t *live = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*live));
    if (!live) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = lcd_face_create(LCD_BACKGROUND, &live->face);
    if (err == XIAOTAI_OK) s->emotion = live;
    else xiaotai_mem_free(live);
    return err;
}

static void prepare_canvas(void *ctx)
{
    lcd_face_live_t *live = ctx;
    if (!live->canvas_dirty) return;
    const xiaotai_ui_surface_t canvas = {.pixels = (uint8_t *)live->source.data,
        .width = LCD_FACE_WIDTH, .height = LCD_FACE_HEIGHT, .stride = live->source.header.stride,
        .pixel = XIAOTAI_PIXEL_RGB565};
    if (lcd_face_canvas(live->face, &canvas)) live->canvas_dirty = false;
}

static bool paint_face(void *ctx, const xiaotai_ui_surface_t *target, int16_t x, int16_t y)
{
    lcd_face_live_t *live = ctx;
    return lcd_face_paint(live->face, target, x, y);
}

static bool allocate_canvas(lcd_face_live_t *live)
{
    if (live->source.data) return true;
    const uint32_t stride = lv_draw_buf_width_to_stride(LCD_FACE_WIDTH, LV_COLOR_FORMAT_RGB565);
    const size_t bytes = (size_t)stride * LCD_FACE_HEIGHT;
    const size_t alignment = LV_DRAW_BUF_ALIGN > sizeof(void *) ? LV_DRAW_BUF_ALIGN : sizeof(void *);
    const uint8_t *data = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, bytes);
    if (!data) return false;
    live->source = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
        .w = LCD_FACE_WIDTH, .h = LCD_FACE_HEIGHT, .stride = stride}, .data_size = (uint32_t)bytes, .data = data};
    live->canvas_dirty = true;
    return true;
}

void lcd_face_live_place(lcd_state_t *s, lv_obj_t *image, const xiaotai_view_t *view)
{
    if (!image) return;
    lcd_place(image, lcd_face_box(&s->layout, LCD_FACE_WIDTH, LCD_FACE_HEIGHT, lcd_face_preview(view)));
}

lv_obj_t *lcd_face_live_build(lcd_state_t *s, lv_obj_t *parent, const xiaotai_view_t *view)
{
    lcd_face_live_t *live = s->emotion;
    if (!live || !allocate_canvas(live)) return NULL;
    lv_obj_t *image = lv_image_create(parent);
    if (!image) return NULL;
    const uint32_t by_height = (uint32_t)s->layout.height * LV_SCALE_NONE / XIAOTAI_THEME_LCD_LOGICAL_HEIGHT;
    const uint32_t by_width = (uint32_t)s->layout.width * LV_SCALE_NONE / LCD_FACE_WIDTH;
    const uint32_t scale = by_height < by_width ? by_height : by_width;
    lv_image_set_src(image, &live->source);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_image_set_pivot(image, 0, 0);
    lv_image_set_scale(image, scale);
    lcd_face_live_place(s, image, view);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    /* Keep a valid source for software draws; unhide only at the first tick. */
    lcd_visible(image, false);
    live->direct = scale == LV_SCALE_NONE && (s->variant->features & XIAOTAI_THEME_LCD_DIRECT_EMOTION) &&
        xiaotai_ui_host_painter_ex(s->host, image, paint_face, prepare_canvas, live) == XIAOTAI_OK;
    return image;
}

static uint64_t sample_time(lcd_face_live_t *live, uint32_t now_ms)
{
    /* The scene clock wraps, while cue ages and wave phases keep advancing. */
    live->time_ms += live->sampled ? (uint32_t)(now_ms - live->last_sample) : now_ms;
    live->last_sample = now_ms;
    live->sampled = true;
    return live->time_ms;
}

void lcd_face_live_set(lcd_state_t *s, const xiaotai_view_t *view, const xiaotai_ui_scene_t *scene)
{
    lcd_face_live_t *live = s->emotion;
    if (!live || !s->layers[XIAOTAI_LAYER_PAGE].face) return;
    const uint64_t now = sample_time(live, scene->now_ms);
    if (!live->attached) {
        uint32_t background = LCD_BACKGROUND, variant_rgb;
        if (s->variant->emotion_background && s->variant->emotion_background(s->variant->self, view, &variant_rgb))
            background = variant_rgb;
        lcd_face_set_background(live->face, background);
        lcd_face_attach(live->face, now);
        live->attached = true;
        live->canvas_dirty = true;
        /* A newly attached visible canvas publishes neutral before the first
         * view update, like reattaching the original page. */
        if (!s->asleep && !scene->overlay.present) {
            lcd_face_rect_t unused[LCD_FACE_REGIONS];
            (void)lcd_face_step(live->face, now, true, unused);
        }
    }
    lcd_face_set(live->face, view->emotion, view->phase, view->level, now);
}

static void invalidate_regions(lv_obj_t *image, const lcd_face_rect_t *regions, size_t count)
{
    lv_area_t coords;
    lv_obj_get_coords(image, &coords);
    const int32_t scale_x = lv_image_get_scale_x(image), scale_y = lv_image_get_scale_y(image);
    const int32_t angle = lv_image_get_rotation(image);
    const bool transformed = angle || scale_x != LV_SCALE_NONE || scale_y != LV_SCALE_NONE;
    lv_point_t pivot;
    if (transformed) lv_image_get_pivot(image, &pivot);
    for (size_t i = 0; i < count; ++i) {
        const lcd_face_rect_t r = regions[i];
        lv_area_t area = {r.x, r.y, r.x + r.width - 1, r.y + r.height - 1};
        if (transformed && !angle && scale_x == scale_y && pivot.x == 0 && pivot.y == 0) {
            /* Preserve the deployed canvas zoom and its sampling margin. */
            area = (lv_area_t){r.x * scale_x / LV_SCALE_NONE - 2, r.y * scale_y / LV_SCALE_NONE - 2,
                ((r.x + r.width) * scale_x + LV_SCALE_NONE - 1) / LV_SCALE_NONE + 2,
                ((r.y + r.height) * scale_y + LV_SCALE_NONE - 1) / LV_SCALE_NONE + 2};
        } else if (transformed) {
            /* Transform the region's half-open edges as LVGL does for the
             * image bounds, plus adjacent pixels for software sampling. */
            lv_point_t corners[4] = {{area.x1, area.y1}, {area.x2 + 1, area.y1},
                                    {area.x1, area.y2 + 1}, {area.x2 + 1, area.y2 + 1}};
            lv_point_array_transform(corners, 4, angle, scale_x, scale_y, &pivot, true);
            area = (lv_area_t){LV_MIN4(corners[0].x, corners[1].x, corners[2].x, corners[3].x) - 2,
                              LV_MIN4(corners[0].y, corners[1].y, corners[2].y, corners[3].y) - 2,
                              LV_MAX4(corners[0].x, corners[1].x, corners[2].x, corners[3].x) + 1,
                              LV_MAX4(corners[0].y, corners[1].y, corners[2].y, corners[3].y) + 1};
        }
        lv_area_move(&area, coords.x1, coords.y1);
        lv_obj_invalidate_area(image, &area);
    }
}

uint32_t lcd_face_live_tick(lcd_state_t *s, uint32_t now_ms)
{
    lcd_face_live_t *live = s->emotion;
    if (!live) return 0;
    lv_obj_t *image = s->layers[XIAOTAI_LAYER_PAGE].face;
    const uint64_t now = sample_time(live, now_ms);
    const bool show = live->attached && image && !s->asleep && !s->layers[XIAOTAI_LAYER_OVERLAY].root;
    lcd_face_rect_t regions[LCD_FACE_REGIONS];
    const size_t count = lcd_face_step(live->face, now, show, regions);
    if (!show) {
        lcd_visible(image, false);
        return 0;
    }
    if (count) {
        live->canvas_dirty = true;
        invalidate_regions(image, regions, count);
    }
    if (!live->direct) prepare_canvas(live);
    lcd_visible(image, true);
    return lcd_face_delay(live->face, now);
}

void lcd_face_live_pause(lcd_state_t *s)
{
    lcd_face_live_t *live = s->emotion;
    if (!live || !live->attached) return;
    lcd_face_rect_t unused[LCD_FACE_REGIONS];
    (void)lcd_face_step(live->face, live->time_ms, false, unused);
    lcd_visible(s->layers[XIAOTAI_LAYER_PAGE].face, false);
}

void lcd_face_live_detach(lcd_state_t *s)
{
    lcd_face_live_t *live = s->emotion;
    if (!live || !live->attached) return;
    lcd_face_detach(live->face);
    live->attached = false;
}

void lcd_face_live_stop(lcd_state_t *s)
{
    lcd_face_live_t *live = s->emotion;
    if (!live) return;
    if (live->source.data) lv_image_cache_drop(&live->source);
    xiaotai_mem_free((void *)live->source.data);
    lcd_face_destroy(live->face);
    xiaotai_mem_free(live);
}

void lcd_face_live_report(const lcd_state_t *s, char *line, size_t capacity)
{
    lcd_face_report(s && s->emotion ? s->emotion->face : NULL, line, capacity);
}
