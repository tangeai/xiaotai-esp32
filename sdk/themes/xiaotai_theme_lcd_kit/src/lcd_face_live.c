// SPDX-License-Identifier: MIT
#include "lcd_face_live.h"
#include "lcd_face.h"

#include "src/misc/cache/instance/lv_image_cache.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#ifdef XIAOTAI_THEME_LCD_WITH_MEDIA
#include "xiaotai/media.h"
#endif

/* The face is drawn as often as there is room for: its work takes at most so much of the time,
 * and so much with waiting for the panel. The time is the clock's: where sound,
 * which comes first, takes the processor away, frames become fewer, down to 25 a second. */
enum { WORK_PERCENT = 30, BUSY_PERCENT = 60, QUICKEST_MS = 16, SLOWEST_MS = 40, PACE_STEPS = 16 };

typedef struct lcd_face_live {
    lcd_face_t *face;
    /* An actor that registered for this display draws instead of the face. */
    const xiaotai_theme_lcd_actor_t *actor;
    void *actor_state;
    lv_image_dsc_t source;
    uint64_t time_ms;
    uint32_t last_sample;
    bool sampled, attached;
    uint8_t *canvas;
    bool canvas_dirty;
    bool direct;
    uint16_t frame_ms;
    uint32_t pace_steps, pace_cost_us, busy_us;     /* since the pace was last set; the cost as of then */
} lcd_face_live_t;

xiaotai_err_t lcd_face_live_start(lcd_state_t *s)
{
    if (!s->variant || !(s->variant->features & XIAOTAI_THEME_LCD_EMOTION)) return XIAOTAI_OK;
    lcd_face_live_t *live = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*live));
    if (!live) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_theme_lcd_actor_t *actor = xiaotai_theme_lcd_actor_select(xiaotai_ui_host_display(s->host));
    if (actor && actor->start(s->host, &live->actor_state) == XIAOTAI_OK) {
        live->actor = actor;
        s->emotion = live;
        return XIAOTAI_OK;
    }
    const xiaotai_err_t err = lcd_face_create(LCD_BACKGROUND, &live->face);
    if (err == XIAOTAI_OK) {
        s->emotion = live;
    } else {
        xiaotai_mem_free(live);
    }
    return err;
}

/* The face as a picture: for displays that do not take its pixels straight
 * into the frame, and for screenshots. Made when it is first needed. */
static void prepare_canvas(void *ctx)
{
    lcd_face_live_t *live = ctx;
    if (!live->canvas) {
        const size_t bytes = (size_t)live->source.header.stride * live->source.header.h;
        const size_t alignment = LV_DRAW_BUF_ALIGN > sizeof(void *) ? LV_DRAW_BUF_ALIGN : sizeof(void *);
        live->canvas = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, bytes);
        live->source.data = live->canvas;           /* LVGL draws a picture without data as nothing */
        if (!live->canvas) return;
        live->source.data_size = (uint32_t)bytes;
        live->canvas_dirty = true;
    }
    if (!live->canvas_dirty) return;
    const xiaotai_ui_surface_t canvas = {.pixels = live->canvas,
        .width = live->source.header.w, .height = live->source.header.h, .stride = live->source.header.stride,
        .pixel = XIAOTAI_PIXEL_RGB565};
    if (lcd_face_canvas(live->face, &canvas)) live->canvas_dirty = false;
}

static bool paint_face(void *ctx, const xiaotai_ui_surface_t *target, int16_t x, int16_t y)
{
    lcd_face_live_t *live = ctx;
    return lcd_face_paint(live->face, target, x, y);
}

/* The face in as many pixels as it is shown in. */
static bool size_canvas(lcd_face_live_t *live, int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0 || lcd_face_size(live->face, (uint16_t)width, (uint16_t)height) != XIAOTAI_OK) return false;
    if (live->source.header.w == width && live->source.header.h == height) return true;
    if (live->source.header.w) lv_image_cache_drop(&live->source);
    xiaotai_mem_free(live->canvas);
    live->canvas = NULL;
    /* A picture needs data to be taken for one; nobody reads it before prepare_canvas(). */
    static const uint8_t none[4];
    live->source = (lv_image_dsc_t){.data = none, .data_size = sizeof(none), .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = (uint16_t)width,
        .h = (uint16_t)height, .stride = lv_draw_buf_width_to_stride((uint32_t)width, LV_COLOR_FORMAT_RGB565)}};
    live->canvas_dirty = true;
    return true;
}

static xiaotai_theme_lcd_stage_t stage_of(const lcd_state_t *s, const xiaotai_view_t *view)
{
    return (xiaotai_theme_lcd_stage_t){s->layout.width, s->layout.height, lcd_face_preview(view)};
}

bool lcd_face_live_acted(const lcd_state_t *s)
{
    const lcd_face_live_t *live = s->emotion;
    return live && live->actor;
}

lcd_box_t lcd_face_live_hit(lcd_state_t *s, const xiaotai_view_t *view)
{
    const lcd_face_live_t *live = s->emotion;
    if (!live || !live->actor) return lcd_home_face_box(s, view, 224, 108);
    const xiaotai_ui_rect_t area = live->actor->hit(live->actor_state);
    return (lcd_box_t){area.x, area.y, area.width, area.height};
}

void lcd_face_live_place(lcd_state_t *s, lv_obj_t *image, const xiaotai_view_t *view)
{
    if (!image) return;
    const lcd_face_live_t *live = s->emotion;
    if (live && live->actor) {
        const xiaotai_theme_lcd_stage_t stage = stage_of(s, view);
        live->actor->place(live->actor_state, &stage);
        return;
    }
    lcd_place(image, lcd_home_face_box(s, view, LCD_FACE_WIDTH, LCD_FACE_HEIGHT));
}

static lv_obj_t *build_image(lcd_state_t *s, lcd_face_live_t *live, lv_obj_t *parent, lcd_box_t box)
{
    if (!size_canvas(live, box.width, box.height)) return NULL;
    lv_obj_t *image = lv_image_create(parent);
    if (!image) return NULL;
    lv_image_set_src(image, &live->source);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lcd_place(image, box);
    /* Hidden until the first frame is there. */
    lcd_visible(image, false);
    if (!live->frame_ms) live->frame_ms = LCD_FACE_FRAME_MS;
    lcd_face_pace(live->face, live->frame_ms);
    live->direct = (s->variant->features & XIAOTAI_THEME_LCD_DIRECT_EMOTION) &&
        xiaotai_ui_host_painter_ex(s->host, image, paint_face, prepare_canvas, live) == XIAOTAI_OK;
    return image;
}

lv_obj_t *lcd_face_live_build_in(lcd_state_t *s, lv_obj_t *parent, lcd_box_t box)
{
    lcd_face_live_t *live = s->emotion;
    /* An actor has a page to itself; there is no face to draw beside something else. */
    if (!live || live->actor || box.width <= 0 || box.height <= 0) return NULL;
    return build_image(s, live, parent, box);
}

lv_obj_t *lcd_face_live_build(lcd_state_t *s, lv_obj_t *parent, const xiaotai_view_t *view)
{
    lcd_face_live_t *live = s->emotion;
    if (live && live->actor) {
        const xiaotai_theme_lcd_stage_t stage = stage_of(s, view);
        lv_obj_t *widget = live->actor->build(live->actor_state, parent, &stage);
        lcd_visible(widget, false);
        return widget;
    }
    if (!live) return NULL;
    return build_image(s, live, parent, lcd_home_face_box(s, view, LCD_FACE_WIDTH, LCD_FACE_HEIGHT));
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
    if (live->actor) {
        live->actor->set(live->actor_state, view, scene->now_ms);
        return;
    }
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

/* Straight to the panel where that does, else with the next refresh. */
static void show_regions(const lcd_state_t *s, const lcd_face_live_t *live, lv_obj_t *image, const lcd_face_rect_t *regions, size_t count)
{
    lv_area_t coords;
    lv_obj_get_coords(image, &coords);
    uint32_t shown = 0;
    if (live->direct && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) && coords.x1 >= 0 && coords.y1 >= 0) {
        xiaotai_ui_rect_t areas[LCD_FACE_REGIONS];
        for (size_t i = 0; i < count; ++i)
            areas[i] = (xiaotai_ui_rect_t){(uint16_t)(coords.x1 + regions[i].x), (uint16_t)(coords.y1 + regions[i].y),
                                           (uint16_t)regions[i].width, (uint16_t)regions[i].height};
        shown = xiaotai_ui_host_paint(s->host, image, areas, count);
    }
    for (size_t i = 0; i < count; ++i) {
        if (shown & UINT32_C(1) << i) continue;
        const lcd_face_rect_t r = regions[i];
        const lv_area_t area = {coords.x1 + r.x, coords.y1 + r.y, coords.x1 + r.x + r.width - 1, coords.y1 + r.y + r.height - 1};
        lv_obj_invalidate_area(image, &area);
    }
}

/* Where a finger is on the face as it is designed, and how loud it is here and there. */
static void sense(const lcd_face_live_t *live, lv_obj_t *image)
{
    lcd_face_senses_t senses = {.x = -1000, .y = -1000};
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER || lv_indev_get_display(indev) != lv_obj_get_display(image) ||
            lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) continue;
        lv_point_t point;
        lv_area_t area;
        lv_indev_get_point(indev, &point);
        lv_obj_get_coords(image, &area);
        senses.down = true;
        senses.x = (int16_t)((point.x - area.x1) * LCD_FACE_WIDTH / live->source.header.w);
        senses.y = (int16_t)((point.y - area.y1) * LCD_FACE_HEIGHT / live->source.header.h);
    }
#ifdef XIAOTAI_THEME_LCD_WITH_MEDIA
    xiaotai_media_loudness_t loudness;
    xiaotai_media_loudness(&loudness);
    senses.hears = true;
    senses.capture = loudness.capture;
    senses.playout = loudness.playout;
#endif
    lcd_face_sense(live->face, &senses);
}

bool lcd_face_live_stroked(const lcd_state_t *s)
{
    return s && s->emotion && lcd_face_stroked(s->emotion->face);
}

static void keep_pace(lcd_face_live_t *live, uint32_t busy_us)
{
    live->busy_us += busy_us;
    if (++live->pace_steps < PACE_STEPS) return;
    const uint32_t cost = lcd_face_cost(live->face);
    const uint32_t work = (cost - live->pace_cost_us) / PACE_STEPS, busy = live->busy_us / PACE_STEPS;
    live->pace_cost_us = cost;
    live->pace_steps = live->busy_us = 0;
    uint32_t ms = work / (10U * WORK_PERCENT), by_panel = busy / (10U * BUSY_PERCENT);
    if (by_panel > ms) ms = by_panel;
    ms = ms < QUICKEST_MS ? QUICKEST_MS : ms > SLOWEST_MS ? SLOWEST_MS : ms;
    /* At once where it is too much, a little at a time when there is room again. */
    if (ms > live->frame_ms) live->frame_ms = (uint16_t)((live->frame_ms + ms + 1U) / 2U);
    else if (ms < live->frame_ms) --live->frame_ms;
    lcd_face_pace(live->face, live->frame_ms);
}

uint32_t lcd_face_live_tick(lcd_state_t *s, uint32_t now_ms)
{
    lcd_face_live_t *live = s->emotion;
    if (!live) return 0;
    lv_obj_t *image = s->layers[XIAOTAI_LAYER_PAGE].face;
    if (live->actor) {
        const bool shown = image && !s->asleep && !s->layers[XIAOTAI_LAYER_OVERLAY].root;
        const uint32_t delay = live->actor->tick(live->actor_state, now_ms, shown);
        lcd_visible(image, shown);
        return shown ? delay : 0;
    }
    const uint64_t now = sample_time(live, now_ms);
    const bool show = live->attached && image && !s->asleep && !s->layers[XIAOTAI_LAYER_OVERLAY].root;
    lcd_face_rect_t regions[LCD_FACE_REGIONS];
    if (show) sense(live, image);
    const int64_t began = xiaotai_time_us();
    const size_t count = lcd_face_step(live->face, now, show, regions);
    if (!show) {
        lcd_visible(image, false);
        return 0;
    }
    if (count) {
        live->canvas_dirty = true;
        show_regions(s, live, image, regions, count);
    }
    if (!live->direct) prepare_canvas(live);
    lcd_visible(image, true);
    keep_pace(live, (uint32_t)(xiaotai_time_us() - began));
    return lcd_face_delay(live->face, now);
}

void lcd_face_live_pause(lcd_state_t *s)
{
    lcd_face_live_t *live = s->emotion;
    if (live && live->actor) return;
    if (!live || !live->attached) return;
    lcd_face_rect_t unused[LCD_FACE_REGIONS];
    (void)lcd_face_step(live->face, live->time_ms, false, unused);
    lcd_visible(s->layers[XIAOTAI_LAYER_PAGE].face, false);
}

void lcd_face_live_detach(lcd_state_t *s)
{
    lcd_face_live_t *live = s->emotion;
    if (live && live->actor) {
        live->actor->detach(live->actor_state);
        return;
    }
    if (!live || !live->attached) return;
    lcd_face_detach(live->face);
    live->attached = false;
}

void lcd_face_live_stop(lcd_state_t *s)
{
    lcd_face_live_t *live = s->emotion;
    if (!live) return;
    if (live->actor) live->actor->stop(live->actor_state);
    if (live->source.header.w) lv_image_cache_drop(&live->source);
    xiaotai_mem_free(live->canvas);
    lcd_face_destroy(live->face);
    xiaotai_mem_free(live);
}

void lcd_face_live_report(const lcd_state_t *s, char *line, size_t capacity)
{
    const lcd_face_live_t *live = s ? s->emotion : NULL;
    if (live && live->actor) {
        if (line && capacity) line[0] = 0;
        if (live->actor->report) live->actor->report(live->actor_state, line, capacity);
        return;
    }
    lcd_face_report(live ? live->face : NULL, line, capacity);
}

void lcd_face_live_sleep(lcd_state_t *s, bool asleep)
{
    const lcd_face_live_t *live = s->emotion;
    if (live && live->actor && live->actor->sleep) live->actor->sleep(live->actor_state, asleep);
}
