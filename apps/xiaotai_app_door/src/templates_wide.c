// SPDX-License-Identifier: MIT
#include "templates_wide.h"

#include <stdio.h>
#include <string.h>
#include "backdrop.h"
#include "door_station_icons.h"
#include "door_v4_assets.h"
#include "door_widgets.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui_theme.h"
#include "src/misc/cache/instance/lv_image_cache.h"

/* The colour roles of the colour theme kit (lcd_internal.h): one accent, green
 * to answer and to open, red to end. */
enum { BG = 0x101418, BODY = 0xf2f5f7, SUBTLE = 0x98a3ad, ACCENT = 0x5ec8f2, ACCENT_INK = 0x062330,
       GREEN = 0x1fa672, RED = 0xdc434d, SURFACE = 0x182029, PLATE = 0x293039, OUTLINE = 0x394552 };
enum { RADIUS = 14, RADIUS_CARD = 23 };
enum { WIDTH = 1024, HEIGHT = 600 };
enum { MAX_CONTROLS = XIAOTAI_VIEW_ACTIONS_MAX + XIAOTAI_VIEW_ITEMS_MAX + XIAOTAI_VIEW_INDICATORS_MAX + XIAOTAI_VIEW_FIELDS_MAX };

LV_FONT_DECLARE(door_font_small);
LV_FONT_DECLARE(door_font_button);
LV_FONT_DECLARE(door_font_body);
LV_FONT_DECLARE(door_font_dialog);
LV_FONT_DECLARE(door_font_title);
LV_FONT_DECLARE(door_font_clock);
LV_FONT_DECLARE(door_font_clock_small);

/* The pictures behind the home screens, drawn once and kept: memory instead of
 * flash. Each mode page shows its own (the door station's, the indoor
 * station's): both are kept where external memory allows, so turning between
 * them draws neither again. Where it is short, one is kept and makes room for
 * the other; a picture goes when a video page opens, or a while after the
 * last page that shows one closed. That while is counted by the framework
 * (LVGL's own timers do not run here), so the owner task takes the pictures
 * away; it never does while a page shows one. */
enum { SHORT_OF_MEMORY = 4 * 1024 * 1024, BACKDROP_KEPT_MS = 2000, BACKDROP_TRIES = 50,
       /* In strips: after a call the memory it gave back is rarely in one piece. */
       BACKDROP_STRIPS = 8, BACKDROP_SPARE = 64 * 1024, BACKDROP_KINDS = 2 };

typedef struct {
    lv_image_dsc_t images[BACKDROP_STRIPS];
    uint16_t *strips[BACKDROP_STRIPS];      /* under the lock */
} picture_t;

static struct {
    uint16_t width, height;                 /* of every picture kept */
    picture_t kinds[BACKDROP_KINDS];        /* by door_backdrop_t */
    /* Shared by the display's task and the owner's. */
    xiaotai_critical_t lock;
    unsigned pages;             /* open pages that show one */
    uint32_t visit, timed;      /* counts them: a page opened since is another visit */
    bool leaving;               /* the owner was asked to look again */
} backdrop;

static void strips_free(uint16_t *strips[BACKDROP_STRIPS])
{
    for (size_t i = 0; i < BACKDROP_STRIPS; ++i) {
        xiaotai_mem_free(strips[i]);
        strips[i] = NULL;
    }
}

/* Not while a page shows one. */
static void backdrop_free(void)
{
    uint16_t *strips[BACKDROP_KINDS][BACKDROP_STRIPS] = {0};
    xiaotai_critical_enter(&backdrop.lock);
    if (!backdrop.pages) {
        for (size_t k = 0; k < BACKDROP_KINDS; ++k) {
            memcpy(strips[k], backdrop.kinds[k].strips, sizeof(strips[k]));
            memset(backdrop.kinds[k].strips, 0, sizeof(backdrop.kinds[k].strips));
        }
    }
    xiaotai_critical_exit(&backdrop.lock);
    for (size_t k = 0; k < BACKDROP_KINDS; ++k) strips_free(strips[k]);
}

static bool short_of_memory(void)
{
    xiaotai_mem_stats_t stats;
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &stats);
    return stats.free < SHORT_OF_MEMORY;
}

static void backdrop_wait(void *ctx);

/* [owner] */
static void backdrop_left(void *ctx)
{
    xiaotai_critical_enter(&backdrop.lock);
    const bool again = !backdrop.pages && backdrop.visit != backdrop.timed;
    const bool left = !backdrop.pages && !again;
    backdrop.leaving = again;
    xiaotai_critical_exit(&backdrop.lock);
    if (again) backdrop_wait(ctx);      /* shown and left once more meanwhile */
    else if (left && short_of_memory()) backdrop_free();
}

/* [owner] */
static void backdrop_wait(void *ctx)
{
    uint32_t timer;
    xiaotai_critical_enter(&backdrop.lock);
    backdrop.timed = backdrop.visit;
    xiaotai_critical_exit(&backdrop.lock);
    if (xiaotai_timer_start(BACKDROP_KEPT_MS, backdrop_left, ctx, &timer) != XIAOTAI_OK) backdrop_left(ctx);
}

static void backdrop_enter(void)
{
    xiaotai_critical_enter(&backdrop.lock);
    ++backdrop.pages;
    ++backdrop.visit;
    xiaotai_critical_exit(&backdrop.lock);
}

static void backdrop_leave(void)
{
    xiaotai_critical_enter(&backdrop.lock);
    const bool kept = backdrop.kinds[DOOR_BACKDROP_DOOR].strips[0] || backdrop.kinds[DOOR_BACKDROP_ROOM].strips[0];
    const bool ask = !--backdrop.pages && kept && !backdrop.leaving;
    if (ask) backdrop.leaving = true;
    xiaotai_critical_exit(&backdrop.lock);
    if (!ask || xiaotai_run(backdrop_wait, NULL) == XIAOTAI_OK) return;
    xiaotai_critical_enter(&backdrop.lock);
    backdrop.leaving = false;
    xiaotai_critical_exit(&backdrop.lock);
    if (short_of_memory()) backdrop_free();
}

static size_t strip_rows(uint16_t height, size_t i)
{
    const size_t rows = (height + BACKDROP_STRIPS - 1U) / BACKDROP_STRIPS, top = i * rows;
    return top >= height ? 0 : height - top < rows ? height - top : rows;
}

/* Hands over the strips of a kept picture; LVGL forgets its images. */
static void picture_take(picture_t *picture, uint16_t *strips[BACKDROP_STRIPS])
{
    for (size_t i = 0; i < BACKDROP_STRIPS; ++i)
        if (picture->strips[i]) lv_image_cache_drop(&picture->images[i]);
    xiaotai_critical_enter(&backdrop.lock);
    memcpy(strips, picture->strips, sizeof(picture->strips));
    memset(picture->strips, 0, sizeof(picture->strips));
    xiaotai_critical_exit(&backdrop.lock);
}

/* Draws it if it is not there. A page is open: the owner does not touch the pictures. */
static bool backdrop_for(uint16_t width, uint16_t height, door_backdrop_t kind)
{
    picture_t *const want = &backdrop.kinds[kind], *const other = &backdrop.kinds[!kind];
    const bool sized = backdrop.width == width && backdrop.height == height;
    if (want->strips[0] && sized) return true;
    uint16_t *strips[BACKDROP_STRIPS] = {0};
    if (!sized) {
        /* Another size: neither picture fits. */
        for (size_t k = 0; k < BACKDROP_KINDS; ++k) {
            picture_take(&backdrop.kinds[k], strips);
            strips_free(strips);
        }
    }
    const size_t bytes = (size_t)width * height * sizeof(uint16_t);
    xiaotai_mem_stats_t stats;
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &stats);
    /* Not room for both: the other picture gives its memory, as when one was kept. */
    if (other->strips[0] && stats.total && stats.free < bytes + BACKDROP_SPARE + SHORT_OF_MEMORY)
        picture_take(other, strips);
    bool ready = strips[0] || !stats.total || stats.free >= bytes + BACKDROP_SPARE;
    uint16_t *work = ready ? xiaotai_mem_alloc(XIAOTAI_MEM_BULK, DOOR_BACKDROP_WORK(width) * sizeof(uint16_t)) : NULL;
    ready = work != NULL;
    for (size_t i = 0, y = 0; i < BACKDROP_STRIPS && ready; y += strip_rows(height, i), ++i) {
        const size_t rows = strip_rows(height, i), size = (size_t)width * rows * sizeof(uint16_t);
        if (!strips[i] && rows) strips[i] = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size);
        ready = !rows || door_backdrop_rows(strips[i], width, height, kind, work, (uint16_t)y, (uint16_t)rows);
        want->images[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = width, .h = (uint16_t)rows,
                       .stride = (uint16_t)(width * 2U)},
            .data_size = (uint32_t)size, .data = (const uint8_t *)strips[i],
        };
        lv_image_cache_drop(&want->images[i]);
    }
    xiaotai_mem_free(work);
    if (!ready) strips_free(strips);
    xiaotai_critical_enter(&backdrop.lock);
    memcpy(want->strips, strips, sizeof(strips));
    xiaotai_critical_exit(&backdrop.lock);
    backdrop.width = width;
    backdrop.height = height;
    return ready;   /* without it: the plain ground of the page */
}

/* The direct painter needs a native-size, untransformed image draw task.
 * This 1-bit fallback is transparent, so it never paints a fake video frame. */
static const uint8_t video_clear[WIDTH / 8 * HEIGHT];

typedef enum { HOME, MODE, DEVICES, MONITOR, CALL, CONFIRM, VOLUME } page_t;
typedef struct wide wide_t;
typedef struct {
    wide_t *owner;
    lv_obj_t *object;
    uint16_t index;
    uint8_t element;
    bool pressed, field_dirty;
} control_t;

struct wide {
    const xiaotai_ui_host_t *host;
    lv_obj_t *root, *sheet, *toast_box, *toast_label;
    lv_obj_t *title, *subtitle, *body, *clock, *date, *status, *online, *list, *slider, *percent, *badge;
    const lv_font_t *full;
    lv_font_t button_font, body_font, small_font;
    control_t controls[MAX_CONTROLS];
    size_t n;
    lv_image_dsc_t video_image;
    int32_t field_min, field_max, field_step;
    page_t page;
    uint8_t layer;
    uint16_t width, height;
    bool built, video, updating, sheet_pressed;
    bool pictured;       /* the picture stands behind the page */
    uint8_t tries;       /* for it */
    bool small;          /* a display the wide coordinates do not fit: home and mode only */
};

static bool equal(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }

static lv_obj_t *image(lv_obj_t *parent, const lv_image_dsc_t *asset, int x, int y)
{
    if (!parent || !asset) return NULL;
    lv_obj_t *object = lv_image_create(parent);
    if (object) {
        lv_image_set_src(object, asset);
        lv_obj_set_pos(object, x, y);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    }
    return object;
}

static int32_t slider_value(const wide_t *s)
{
    const int64_t value = s->field_min + (int64_t)lv_slider_get_value(s->slider) * s->field_step;
    return value > s->field_max ? s->field_max : (int32_t)value;
}

static void control_event(lv_event_t *event)
{
    control_t *c = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE && c->pressed) {
        /* LVGL drops the pressed object during rebuild and will not release it. */
        xiaotai_ui_host_input(c->owner->host, c->owner->layer, c->element, c->index,
                              XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
        return;
    }
    if (c->owner->updating) return;
    xiaotai_ui_input_t kind;
    int32_t value = 0;
    switch (code) {
    case LV_EVENT_PRESSED: c->pressed = true; kind = XIAOTAI_UI_INPUT_PRESS; break;
    case LV_EVENT_CLICKED: kind = XIAOTAI_UI_INPUT_TAP; break;
    case LV_EVENT_LONG_PRESSED: kind = XIAOTAI_UI_INPUT_LONG_PRESS; break;
    case LV_EVENT_VALUE_CHANGED:
        if (c->element != XIAOTAI_UI_ELEMENT_FIELD) return;
        c->field_dirty = true;
        kind = XIAOTAI_UI_INPUT_CHANGE;
        value = slider_value(c->owner);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        c->pressed = false;
        if (c->element == XIAOTAI_UI_ELEMENT_FIELD && c->field_dirty) {
            c->field_dirty = false;
            value = slider_value(c->owner);
            xiaotai_ui_host_input(c->owner->host, c->owner->layer, c->element, c->index, XIAOTAI_UI_INPUT_COMMIT, value, NULL);
        }
        kind = XIAOTAI_UI_INPUT_RELEASE;
        break;
    default: return;
    }
    xiaotai_ui_host_input(c->owner->host, c->owner->layer, c->element, c->index, kind, value, NULL);
}

static void sheet_event(lv_event_t *event)
{
    wide_t *s = lv_event_get_user_data(event);
    xiaotai_ui_input_t input;
    if (s->updating) return;
    if (lv_event_get_code(event) == LV_EVENT_GESTURE && (s->page == HOME || s->page == MODE)) {
        lv_indev_t *device = lv_indev_active();
        const lv_dir_t direction = device ? lv_indev_get_gesture_dir(device) : LV_DIR_NONE;
        if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) return;
        input = direction == LV_DIR_LEFT ? XIAOTAI_UI_INPUT_SWIPE_LEFT : XIAOTAI_UI_INPUT_SWIPE_RIGHT;
    } else {
        if (lv_event_get_target_obj(event) != s->sheet) return;
        switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED: s->sheet_pressed = true; input = XIAOTAI_UI_INPUT_PRESS; break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST: s->sheet_pressed = false; input = XIAOTAI_UI_INPUT_RELEASE; break;
        default: return;
        }
    }
    xiaotai_ui_host_input(s->host, s->layer, XIAOTAI_UI_ELEMENT_OTHER, 0, input, 0, NULL);
}

static void bind(wide_t *s, lv_obj_t *object, uint8_t element, size_t index, uint8_t state, bool has_intent)
{
    if (!object || s->n >= MAX_CONTROLS || (state & XIAOTAI_STATE_HIDDEN)) return;
    control_t *c = &s->controls[s->n++];
    *c = (control_t){.owner = s, .object = object, .element = element, .index = (uint16_t)index};
    if (state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY)) {
        lv_obj_add_state(object, LV_STATE_DISABLED);
        lv_obj_set_style_opa(object, LV_OPA_40, 0);
    }
    if (!has_intent || (state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY))) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(object, control_event, LV_EVENT_ALL, c);
}

static bool usable(const xiaotai_view_action_t *action) { return !(action->state & XIAOTAI_STATE_HIDDEN); }

static lv_obj_t *hit(wide_t *s, lv_obj_t *parent, const xiaotai_view_action_t *action, size_t index,
                     int x, int y, int w, int h, uint32_t color, lv_opa_t opacity)
{
    if (!usable(action)) return NULL;
    lv_obj_t *object = door_box(parent, x, y, w, h, color, opacity);
    if (object) {
        lv_obj_set_style_radius(object, RADIUS, 0);
        lv_obj_set_style_bg_opa(object, opacity == LV_OPA_COVER ? LV_OPA_70 : LV_OPA_20, LV_STATE_PRESSED);
        bind(s, object, XIAOTAI_UI_ELEMENT_ACTION, index, action->state, action->intent != NULL);
    }
    return object;
}

/* The round plate under a control, drawn as a circle in the colour of what it
 * does. A switched-on state (muted) is the lit one, as on the call pages. */
typedef enum { PLATE_NONE, PLATE_NEUTRAL, PLATE_GREEN, PLATE_RED } plate_t;

static void plate(lv_obj_t *parent, plate_t kind, int x, bool selected)
{
    const uint32_t color = selected ? ACCENT : kind == PLATE_GREEN ? GREEN : kind == PLATE_RED ? RED : PLATE;
    lv_obj_t *circle = door_box(parent, x, 0, 72, 72, color, LV_OPA_COVER);
    if (circle) lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
}

static void tint(lv_obj_t *icon, uint32_t color)
{
    if (!icon) return;
    lv_obj_set_style_image_recolor(icon, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
}

static void icon_action(wide_t *s, const xiaotai_view_action_t *a, size_t i, int x, int y, bool home,
                        const lv_image_dsc_t *icon, plate_t circle, bool selected)
{
    const int w = home ? 176 : 112, start = (w - 72) / 2;
    lv_obj_t *object = hit(s, s->sheet, a, i, x, y, w, home ? 124 : 116, BODY, LV_OPA_TRANSP);
    if (!object) return;
    if (circle != PLATE_NONE) plate(object, circle, start, selected);
    else {
        /* Glass over the wallpaper. */
        lv_obj_t *tile = door_box(object, start, 0, 72, 72, BG, LV_OPA_60);
        if (tile) lv_obj_set_style_radius(tile, RADIUS_CARD, 0);
    }
    /* The retained icons carry tints of their own; every control draws them in one ink. */
    tint(image(object, icon, start + 18, 18), selected ? ACCENT_INK : BODY);
    door_text(door_label(object, 0, home ? 88 : 80, w, &door_font_button, BODY, LV_TEXT_ALIGN_CENTER), a->label);
}

static void flat_action(wide_t *s, lv_obj_t *parent, const xiaotai_view_action_t *a, size_t i, int x, int y, int w,
                        const lv_image_dsc_t *icon, uint32_t color, lv_opa_t opacity)
{
    lv_obj_t *object = hit(s, parent, a, i, x, y, w, 64, color, opacity);
    if (!object) return;
    if (icon) tint(image(object, icon, 16, (64 - icon->header.h) / 2), BODY);
    const int left = icon ? 58 : 10;
    if (w > left + 10) door_text(door_label(object, left, 14, w - left - 10, &door_font_button, BODY,
                                  icon ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER), a->label);
}

static void place_video(wide_t *s, bool visible)
{
    const uint16_t stride = (s->width + 7U) / 8U;
    const size_t size = (size_t)stride * s->height;
    if (size > sizeof(video_clear)) return;
    s->video_image = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A1,
        .w = s->width, .h = s->height, .stride = stride}, .data_size = size, .data = video_clear};
    lv_obj_t *widget = image(s->sheet, &s->video_image, 0, 0);
    if (!widget) return;
    /* The video owner installs the source before it emits remote-ready. */
    door_show(widget, visible);
    if (xiaotai_ui_host_video_region(s->host, widget) == XIAOTAI_OK) s->video = true;
    else lv_obj_delete(widget);
}

/* A control of the home screen: icon and words on one plate. */
static void pill(wide_t *s, const xiaotai_view_action_t *a, size_t i, int x, int y, int w, int h,
                 const lv_image_dsc_t *icon, uint32_t color, lv_opa_t opacity, uint32_t ink)
{
    lv_obj_t *object = hit(s, s->sheet, a, i, x, y, w, h, color, opacity);
    if (!object) return;
    lv_obj_set_style_radius(object, s->small ? RADIUS : 22, 0);
    lv_obj_set_style_border_width(object, opacity == LV_OPA_COVER ? 0 : 1, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(BODY), 0);
    lv_obj_set_style_border_opa(object, LV_OPA_20, 0);
    const lv_font_t *font = s->small ? &s->small_font : &s->button_font;
    lv_point_t size = {0};
    lv_text_get_size(&size, a->label ? a->label : "", font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int picture = icon ? icon->header.w : 0, gap = icon && size.x ? (s->small ? 6 : 12) : 0;
    const int width = min(size.x, w - picture - gap - 12), left = (w - picture - gap - width) / 2;
    if (icon) tint(image(object, icon, left, (h - icon->header.h) / 2), ink);
    lv_obj_t *caption = door_label(object, left + picture + gap, (h - size.y) / 2, max(1, width + 2), font, ink, LV_TEXT_ALIGN_LEFT);
    door_text(caption, a->label);
}

static void pager_arrow(wide_t *s, const xiaotai_view_action_t *a, size_t i, bool right)
{
    if (!a->icon) return;   /* nowhere to page to */
    lv_obj_t *object = hit(s, s->sheet, a, i, right ? s->width - 30 : 0, s->height / 2 - 30, 30, 60, BODY, LV_OPA_TRANSP);
    lv_obj_t *icon = image(object, right ? &door_asset_next_24 : &door_asset_previous_24, 3, 18);
    tint(icon, BODY);
    if (icon) lv_obj_set_style_opa(icon, LV_OPA_60, 0);
}

static xiaotai_app_door_home_view_t home_data(const xiaotai_view_t *v)
{
    xiaotai_app_door_home_view_t home = {.state = XIAOTAI_APP_DOOR_NOT_READY};
    if (v->custom_data && v->custom_size == sizeof(home)) memcpy(&home, v->custom_data, sizeof(home));
    return home;
}

/* The words stand on the left, where the picture is dark; what the mode is
 * for stands below them. The rest of the screen is the picture. */
static void build_home(wide_t *s, const xiaotai_view_t *v)
{
    const bool small = s->small, station = home_data(v).station;
    const bool video = (v->flags & XIAOTAI_VIEW_VIDEO) != 0;
    const int h = s->height, left = small ? 32 : 48, words = small ? s->width / 2 - 28 : s->width / 2 + 20;
    place_video(s, video);
    if (video) (void)door_box(s->sheet, 0, 0, s->width, h, BG, 70);
    const lv_font_t *body = small ? &s->small_font : &s->body_font;
    const int top = small ? 46 : h < 560 ? 92 : 112;
    s->title = door_label(s->sheet, small ? 16 : 48, small ? 9 : 28, small ? s->width / 2 - 8 : min(520, s->width - 500),
                     small ? &s->button_font : &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    s->clock = door_label(s->sheet, left - 4, top, words, small ? &door_font_clock_small : &door_font_clock, BODY, LV_TEXT_ALIGN_LEFT);
    s->date = door_label(s->sheet, left, top + (small ? 80 : 150), words, body, BODY, LV_TEXT_ALIGN_LEFT);
    s->status = door_label(s->sheet, left + 20, top + (small ? 108 : 198), words - 20, small ? &s->small_font : s->full, BODY,
                      LV_TEXT_ALIGN_LEFT);
    s->online = door_box(s->sheet, left, 0, 10, 10, SUBTLE, LV_OPA_COVER);
    if (s->online) lv_obj_set_style_radius(s->online, LV_RADIUS_CIRCLE, 0);
    if (station) {
        /* What a visitor does. */
        s->body = door_label(s->sheet, left, top + (small ? 136 : 244), words, body, SUBTLE, LV_TEXT_ALIGN_LEFT);
        if (s->body) lv_label_set_long_mode(s->body, LV_LABEL_LONG_WRAP);
    }
    const int bh = small ? 52 : 84, by = h - bh - (small ? 16 : 44), gap = small ? 10 : 20;
    int x = small ? 16 : 48, corner = s->width - (small ? 44 : 76);
    for (size_t i = 0; i < v->indicator_count; ++i) {
        const xiaotai_view_indicator_t *a = &v->indicators[i];
        if (!equal(a->icon, XIAOTAI_ICON_WIFI)) continue;
        lv_obj_t *wifi = door_box(s->sheet, corner, small ? 4 : 24, small ? 40 : 60, small ? 40 : 56, BG, 0);
        lv_obj_t *icon = image(wifi, &door_asset_wifi_24, small ? 8 : 18, small ? 8 : 16);
        tint(icon, BODY);
        if (icon) lv_obj_set_style_opa(icon, a->level < 0 ? LV_OPA_40 : LV_OPA_COVER, 0);
        bind(s, wifi, XIAOTAI_UI_ELEMENT_INDICATOR, i, 0, a->intent != NULL);
        break;
    }
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (!equal(a->intent, XIAOTAI_INTENT_APP_DOOR_SETTINGS) || a->trigger != XIAOTAI_TRIGGER_TAP) continue;
        corner -= small ? 40 : 60;
        lv_obj_t *gear = hit(s, s->sheet, a, i, corner, small ? 4 : 24, small ? 40 : 60, small ? 40 : 56, BODY, LV_OPA_TRANSP);
        tint(image(gear, &door_asset_settings_24, small ? 8 : 18, small ? 8 : 16), BODY);
        break;
    }
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        const bool primary = a->role == XIAOTAI_ROLE_PRIMARY;
        if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_DEVICES)) {
            if (primary) {   /* nothing is chosen yet */
                pill(s, a, i, x, by, small ? 136 : 248, bh, small ? &door_asset_users_24 : &door_asset_users_36, ACCENT,
                     LV_OPA_COVER, ACCENT_INK);
                x += (small ? 136 : 248) + gap;
                continue;
            }
            const int w = small ? 120 : 220;
            lv_obj_t *peer = hit(s, s->sheet, a, i, corner - w - 4, small ? 4 : 24, w, small ? 40 : 56, BG, LV_OPA_TRANSP);
            door_text(door_label(peer, 4, small ? 8 : 12, w - 8, body, BODY, LV_TEXT_ALIGN_RIGHT), a->label);
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_MONITOR)) {
            pill(s, a, i, x, by, small ? 136 : 248, bh, small ? &door_asset_eye_24 : &door_asset_eye_36, ACCENT, LV_OPA_COVER,
                 ACCENT_INK);
            x += (small ? 136 : 248) + gap;
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_RING)) {
            pill(s, a, i, x, by, small ? 124 : 248, bh, small ? &door_asset_bell_24 : &door_asset_bell_36, GREEN, LV_OPA_COVER,
                 BODY);
            x += (small ? 124 : 248) + gap;
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_OPEN)) {
            pill(s, a, i, x, by, small ? 78 : 168, bh, small ? NULL : &door_asset_key_36, BG, LV_OPA_60, BODY);
            x += (small ? 78 : 168) + gap;
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_DIRECTORY)) {
            pill(s, a, i, x, by, small ? 92 : 196, bh, small ? NULL : &door_asset_users_36, BG, LV_OPA_60, BODY);
            x += (small ? 92 : 196) + gap;
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_LOG)) {
            const bool missed = (a->state & XIAOTAI_STATE_CHECKED) != 0;
            pill(s, a, i, x, by, small ? 92 : 196, bh, small ? NULL : &door_asset_history_36, missed ? RED : BG,
                 missed ? LV_OPA_COVER : LV_OPA_60, BODY);
            x += (small ? 92 : 196) + gap;
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_STREAM_STOP)) {
            pill(s, a, i, x, by, small ? 108 : 200, bh, NULL, RED, LV_OPA_COVER, BODY);
            x += (small ? 108 : 200) + gap;
        } else if (a->trigger == XIAOTAI_TRIGGER_LONG_PRESS &&
                   (equal(a->intent, XIAOTAI_INTENT_SYSTEM_MENU) || equal(a->intent, XIAOTAI_INTENT_APP_DOOR_SETTINGS))) {
            bind(s, s->title, XIAOTAI_UI_ELEMENT_ACTION, i, a->state, true);
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_HOME)) {
            pager_arrow(s, a, i, a->trigger == XIAOTAI_TRIGGER_SWIPE_LEFT);
        }
    }
}

/* Where the device stands, and what it does there: cards side by side, rows on a small display. */
static void build_mode(wide_t *s, const xiaotai_view_t *v)
{
    const bool small = s->small;
    (void)door_box(s->sheet, 0, 0, s->width, s->height, BG, 120);
    const int left = small ? 34 : 64, top = small ? 14 : 44;
    s->title = door_label(s->sheet, left, top, s->width - 2 * left, small ? &s->button_font : &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    s->subtitle = door_label(s->sheet, left, top + (small ? 38 : 56), s->width - 2 * left, small ? &s->small_font : &s->body_font,
                        SUBTLE, LV_TEXT_ALIGN_LEFT);
    int cards = 0;
    for (size_t i = 0; i < v->action_count; ++i) cards += equal(v->actions[i].intent, XIAOTAI_INTENT_APP_DOOR_MODE);
    if (!cards) return;
    /* Up to three side by side; four in two rows of two, so a five-character name keeps its line. */
    const bool grid = !small && cards > 3;
    const int columns = small ? 1 : grid ? 2 : cards, gap = small ? 8 : 24;
    const int w = small ? s->width - 2 * left : (s->width - 2 * left - (columns - 1) * gap) / columns;
    const int h = small ? (s->height - 92 - 14 - (cards - 1) * gap) / cards :
                  grid ? (s->height - 176 - 40 - gap) / 2 : min(300, s->height - 176 - 48);
    int x = left, y = small ? 92 : 176, placed = 0;
    size_t item = 0;
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_HOME)) {
            pager_arrow(s, a, i, a->trigger == XIAOTAI_TRIGGER_SWIPE_LEFT);
            continue;
        }
        if (!equal(a->intent, XIAOTAI_INTENT_APP_DOOR_MODE)) continue;
        lv_obj_t *card = hit(s, s->sheet, a, i, x, y, w, h, SURFACE, LV_OPA_90);
        ++placed;
        if (small) y += h + gap;
        else if (placed % columns) x += w + gap;
        else {
            x = left;
            y += h + gap;
        }
        const char *detail = item < v->item_count ? v->items[item].detail : "";
        ++item;
        if (!card) continue;
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(card, small ? RADIUS : RADIUS_CARD, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(OUTLINE), 0);
        /* Mode values are xiaotai_app_door_mode_t + 1. */
        const lv_image_dsc_t *icon = a->value == XIAOTAI_APP_DOOR_ACCESS + 1 ? &door_asset_users_36 :
                                     a->value == XIAOTAI_APP_DOOR_BELL + 1 ? &door_asset_door_36 : &door_asset_house_36;
        const uint32_t color = a->value == XIAOTAI_APP_DOOR_ACCESS + 1 ? 0x8fe3b8 : a->value == XIAOTAI_APP_DOOR_BELL + 1 ? 0xffc886 : ACCENT;
        if (small) {
            const int line = lv_font_get_line_height(&s->button_font) + lv_font_get_line_height(&s->small_font);
            tint(image(card, icon, 14, (h - 36) / 2), color);
            door_text(door_label(card, 64, (h - line) / 2, w - 72, &s->button_font, BODY, LV_TEXT_ALIGN_LEFT), a->label);
            door_text(door_label(card, 64, (h - line) / 2 + lv_font_get_line_height(&s->button_font), w - 72, &s->small_font, SUBTLE,
                                 LV_TEXT_ALIGN_LEFT), detail);
            continue;
        }
        const int pad = 24;
        tint(image(card, icon, pad, pad), color);
        door_text(door_label(card, pad + 48, pad - 4, w - pad - 56, &door_font_title, BODY, LV_TEXT_ALIGN_LEFT), a->label);
        const int below = grid ? 56 : 64;
        lv_obj_t *words = door_label(card, pad, pad + below, w - 2 * pad, &s->body_font, SUBTLE, LV_TEXT_ALIGN_LEFT);
        if (words) {
            lv_label_set_long_mode(words, LV_LABEL_LONG_WRAP);
            lv_obj_set_height(words, h - pad - below - 8);      /* what does not fit stays in the card */
        }
        door_text(words, detail);
    }
}

static void build_devices(wide_t *s, const xiaotai_view_t *v)
{
    s->title = door_label(s->sheet, 104, 24, s->width - 130, &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    for (size_t i = 0; i < v->action_count; ++i)
        if (equal(v->actions[i].intent, XIAOTAI_INTENT_APP_DOOR_BACK))
            flat_action(s, s->sheet, &v->actions[i], i, 20, 16, 64, &door_asset_back_36, BG, LV_OPA_TRANSP);
    s->list = door_box(s->sheet, 32, 112, s->width - 64, s->height - 160, BG, LV_OPA_TRANSP);
    if (!s->list) return;
    lv_obj_add_flag(s->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s->list, LV_SCROLLBAR_MODE_AUTO);
    s->subtitle = door_label(s->list, 0, 24, s->width - 100, &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    s->body = door_label(s->list, 0, v->subtitle ? 88 : 24, s->width - 100, &door_font_body, SUBTLE, LV_TEXT_ALIGN_LEFT);
    int row_y = 0;
    for (size_t i = 0; i < v->item_count; ++i) {
        const xiaotai_view_item_t *item = &v->items[i];
        const bool dial = equal(item->intent, XIAOTAI_INTENT_APP_DOOR_DIAL);   /* the station's directory */
        const bool select = equal(item->intent, XIAOTAI_INTENT_APP_DOOR_SELECT);
        if (item->state & XIAOTAI_STATE_HIDDEN) continue;
        lv_obj_t *row = door_box(s->list, 0, row_y, s->width - 80, 88, SURFACE, LV_OPA_COVER);
        row_y += 96;
        if (!row) continue;
        const bool checked = (item->state & XIAOTAI_STATE_CHECKED) != 0;
        lv_obj_set_style_radius(row, RADIUS, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(PLATE), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, checked ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(ACCENT), 0);
        /* A name may hold any character: one face for all of them, not a
         * subset that falls back to another size in the middle of a word. */
        const int line = lv_font_get_line_height(s->full), top = (88 - 2 * line - 4) / 2;
        door_text(door_label(row, 24, top, s->width - 290, s->full, BODY, LV_TEXT_ALIGN_LEFT), item->label);
        door_text(door_label(row, 24, top + line + 4, s->width - 290, s->full, item->detail && strstr(item->detail, "在线") ? ACCENT : SUBTLE,
                   LV_TEXT_ALIGN_LEFT), item->detail);
        if (dial || select)
            door_text(door_label(row, s->width - 280, 26, 176, &door_font_button, checked || dial ? ACCENT : SUBTLE,
                       LV_TEXT_ALIGN_RIGHT), dial ? "呼叫" : checked ? "已添加" : "添加");
        else if (item->intent) tint(image(row, &door_asset_next_24, s->width - 128, 32), SUBTLE);
        bind(s, row, XIAOTAI_UI_ELEMENT_ITEM, i, item->state, item->intent != NULL);
    }
}

static xiaotai_app_door_call_view_t call_data(const xiaotai_view_t *v)
{
    xiaotai_app_door_call_view_t result = {.phase = XIAOTAI_APP_DOOR_CALL_CONNECTING};
    if (v->custom_data && v->custom_size == sizeof(result)) memcpy(&result, v->custom_data, sizeof(result));
    return result;
}

static void build_video_chrome(wide_t *s, const xiaotai_view_t *v)
{
    const int cx = s->width / 2, buttons = s->height - 134;
    const xiaotai_app_door_call_view_t call = call_data(v);
    place_video(s, (v->flags & XIAOTAI_VIEW_VIDEO) != 0);
    (void)door_box(s->sheet, 0, 0, s->width, 112, BG, 82);
    s->title = door_label(s->sheet, 32, 24, s->width - 260, &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    s->subtitle = door_label(s->sheet, 32, 70, s->width - 240, &s->body_font, BODY, LV_TEXT_ALIGN_LEFT);
    s->status = door_label(s->sheet, s->width - 184, 24, 152, &door_font_body, BODY, LV_TEXT_ALIGN_RIGHT);
    s->body = door_label(s->sheet, 80, s->height / 2 - 50, s->width - 160, &door_font_body, BODY, LV_TEXT_ALIGN_CENTER);
    (void)door_box(s->sheet, 0, s->height - 146, s->width, 146, BG, 214);
    const bool monitor = s->page == MONITOR;
    const bool ringing = call.phase == XIAOTAI_APP_DOOR_CALL_RINGING;
    const bool active = call.phase == XIAOTAI_APP_DOOR_CALL_ACTIVE;
    const bool looking = monitor && v->timer_mode != XIAOTAI_TIMER_NONE;
    if (looking || (!monitor && (ringing || active))) {
        s->badge = door_box(s->sheet, 0, 0, 10, 10, ringing && !looking ? ACCENT : GREEN, LV_OPA_COVER);
        if (s->badge) lv_obj_set_style_radius(s->badge, LV_RADIUS_CIRCLE, 0);
    }
    /* The controls of the page in reading order; those the call offers now
     * stand evenly around the middle. */
    const struct { const char *intent; const lv_image_dsc_t *icon; plate_t circle; bool monitor, call, selected; } slots[] = {
        {XIAOTAI_INTENT_APP_DOOR_BACK, &door_asset_back_36, PLATE_NEUTRAL, true, false, false},
        {XIAOTAI_INTENT_APP_DOOR_MONITOR, &door_asset_refresh_36, PLATE_GREEN, true, false, false},
        {XIAOTAI_INTENT_APP_DOOR_LOOK_TALK, &door_asset_mic_36, PLATE_NEUTRAL, true, false, false},
        {XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE, &door_asset_mute_36, PLATE_NEUTRAL, true, false, false},
        {XIAOTAI_INTENT_APP_DOOR_RING, &door_asset_bell_36, PLATE_GREEN, false, true, false},
        {"door.call.accept", &door_asset_phone_36, PLATE_GREEN, false, true, false},
        {"door.call.reject", &door_asset_hangup_36, PLATE_RED, false, true, false},
        {XIAOTAI_INTENT_APP_DOOR_MUTE, &door_asset_mic_36, PLATE_NEUTRAL, false, true, call.muted},
        {XIAOTAI_INTENT_APP_DOOR_VOLUME, &door_asset_volume_36, PLATE_NEUTRAL, true, true, false},
        {XIAOTAI_INTENT_APP_DOOR_OPEN, &door_asset_key_call_36, PLATE_GREEN, true, true, false},
        {XIAOTAI_INTENT_APP_DOOR_LOOK_NEXT, &door_asset_switch_36, PLATE_NEUTRAL, true, false, false},
        {XIAOTAI_INTENT_APP_DOOR_LOOK_END, &door_asset_hangup_36, PLATE_RED, true, false, false},
        {"door.call.hangup", &door_asset_hangup_36, PLATE_RED, false, true, false},
        {XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP, &door_asset_hangup_36, PLATE_RED, false, true, false},
    };
    enum { SLOTS = sizeof(slots) / sizeof(slots[0]) };
    size_t chosen[SLOTS], slot[SLOTS];
    int count = 0;
    for (size_t k = 0; k < SLOTS; ++k) {
        if (monitor ? !slots[k].monitor : !slots[k].call) continue;
        for (size_t i = 0; i < v->action_count; ++i) {
            if (!usable(&v->actions[i]) || !equal(v->actions[i].intent, slots[k].intent)) continue;
            chosen[count] = i;
            slot[count++] = k;
            break;
        }
    }
    const int stride = count == 2 ? 224 : count ? min(160, (s->width - 24) / count) : 160;
    for (int n = 0; n < count; ++n) {
        const size_t k = slot[n];
        const xiaotai_view_action_t *a = &v->actions[chosen[n]];
        /* The visitor rings: "talk" answers. */
        const bool answer = a->style == XIAOTAI_STYLE_POSITIVE && equal(a->intent, XIAOTAI_INTENT_APP_DOOR_LOOK_TALK);
        icon_action(s, a, chosen[n], cx - 56 + (2 * n - count + 1) * stride / 2, buttons, false,
                    answer ? &door_asset_phone_36 : slots[k].icon, answer ? PLATE_GREEN : slots[k].circle,
                    slots[k].selected || (a->state & XIAOTAI_STATE_CHECKED) != 0);
    }
}

static void build_dialog(wide_t *s, const xiaotai_view_t *v)
{
    const bool volume = s->page == VOLUME;
    const int w = volume ? 460 : 552, h = volume ? 240 : 340;
    const int x = volume ? s->width - w - 32 : (s->width - w) / 2;
    const int y = volume ? s->height - h - 80 : (s->height - h) / 2;
    lv_obj_add_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s->sheet, sheet_event, LV_EVENT_ALL, s);
    lv_obj_t *panel = door_box(s->sheet, x, y, w, h, SURFACE, LV_OPA_COVER);
    if (!panel) return;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(panel, RADIUS_CARD, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(OUTLINE), 0);
    s->title = door_label(panel, volume ? 32 : 84, volume ? 32 : 25, volume ? 300 : 436,
                          volume ? &door_font_dialog : &door_font_title, BODY, LV_TEXT_ALIGN_LEFT);
    if (!volume) {
        if (equal(v->id, XIAOTAI_APP_DOOR_OVERLAY_ASK)) tint(image(panel, &door_asset_door_36, 30, 32), ACCENT);
        else tint(image(panel, &door_asset_lock_32, 32, 34), ACCENT);
        s->subtitle = door_label(panel, 32, 96, 488, &s->body_font, SUBTLE, LV_TEXT_ALIGN_LEFT);
        s->body = door_label(panel, 32, 144, 488, &s->body_font, BODY, LV_TEXT_ALIGN_LEFT);
        if (s->body) lv_label_set_long_mode(s->body, LV_LABEL_LONG_WRAP);
    }
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (s->page == CONFIRM && equal(a->intent, XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM)) {
            const bool confirm = a->role == XIAOTAI_ROLE_PRIMARY, alone = v->action_count == 1;
            flat_action(s, panel, a, i, confirm ? 288 : 32, 240, alone ? 488 : 232,
                        confirm ? &door_asset_key_28 : NULL, confirm ? GREEN : PLATE, LV_OPA_COVER);
        } else if (s->page == CONFIRM && equal(a->intent, XIAOTAI_INTENT_APP_DOOR_ASK)) {
            const bool confirm = a->role == XIAOTAI_ROLE_PRIMARY;
            flat_action(s, panel, a, i, confirm ? 288 : 32, 240, 232, NULL,
                        !confirm ? PLATE : a->style == XIAOTAI_STYLE_DESTRUCTIVE ? RED : GREEN, LV_OPA_COVER);
        } else if (equal(a->intent, XIAOTAI_INTENT_APP_DOOR_BACK))
            flat_action(s, panel, a, i, volume ? 384 : 160, volume ? 16 : 248, volume ? 60 : 232,
                        volume ? &door_asset_close_28 : NULL, PLATE, volume ? LV_OPA_TRANSP : LV_OPA_COVER);
    }
    if (!volume) return;
    for (size_t i = 0; i < v->field_count; ++i) {
        const xiaotai_view_field_t *f = &v->fields[i];
        if (!equal(f->intent, XIAOTAI_INTENT_APP_DOOR_VOLUME_SET) || f->kind != XIAOTAI_FIELD_SLIDER ||
            (f->state & XIAOTAI_STATE_HIDDEN)) continue;
        s->slider = lv_slider_create(panel);
        if (!s->slider) return;
        lv_obj_remove_style_all(s->slider);
        lv_obj_set_pos(s->slider, 32, 130);
        lv_obj_set_size(s->slider, 304, 8);
        lv_obj_set_style_radius(s->slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s->slider, lv_color_hex(OUTLINE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s->slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s->slider, lv_color_hex(ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s->slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_bg_color(s->slider, lv_color_hex(BODY), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s->slider, 9, LV_PART_KNOB);
        lv_obj_set_ext_click_area(s->slider, 24);
        bind(s, s->slider, XIAOTAI_UI_ELEMENT_FIELD, i, f->state, true);
        s->percent = door_label(panel, 336, 114, 76, &door_font_body, BODY, LV_TEXT_ALIGN_RIGHT);
        break;
    }
}

static void release_sheet(wide_t *s)
{
    if (!s->sheet_pressed) return;
    s->sheet_pressed = false;
    xiaotai_ui_host_input(s->host, s->layer, XIAOTAI_UI_ELEMENT_OTHER, 0, XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
}

static void rebuild(wide_t *s, const xiaotai_view_t *v)
{
    const int32_t scroll = s->list ? lv_obj_get_scroll_y(s->list) : 0;
    if (s->video) (void)xiaotai_ui_host_video_region(s->host, NULL);
    release_sheet(s);
    lv_obj_clean(s->sheet);
    lv_obj_remove_event_cb_with_user_data(s->sheet, sheet_event, s);
    lv_obj_remove_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s->sheet, s->page < CONFIRM ? LV_OPA_COVER : s->page == VOLUME ? 82 : 133, 0);
    s->title = s->subtitle = s->body = s->clock = s->date = s->status = s->online = s->list = s->slider = s->percent = s->badge = NULL;
    s->n = 0;
    s->video = s->pictured = false;
    s->tries = 0;
    if (s->page == HOME) {
        lv_obj_add_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s->sheet, sheet_event, LV_EVENT_ALL, s);
        build_home(s, v);
    } else if (s->page == MODE) {
        lv_obj_add_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s->sheet, sheet_event, LV_EVENT_ALL, s);
        build_mode(s, v);
    } else if (s->page == DEVICES) {
        build_devices(s, v);
        if (s->list && scroll) lv_obj_scroll_to_y(s->list, scroll, LV_ANIM_OFF);
    } else if (s->page == MONITOR || s->page == CALL) {
        if (short_of_memory()) backdrop_free();
        build_video_chrome(s, v);
    }
    else build_dialog(s, v);
    s->built = true;
}

/* The picture behind the page. The call the page follows may still hold the
 * memory for it: tried again for a while, the page shows plain meanwhile. */
static void put_picture(wide_t *s, const xiaotai_view_t *v)
{
    if (s->pictured || (v->flags & XIAOTAI_VIEW_VIDEO)) return;
    const door_backdrop_t kind = s->page == HOME && home_data(v).station ? DOOR_BACKDROP_DOOR : DOOR_BACKDROP_ROOM;
    if (backdrop_for(s->width, s->height, kind)) {
        for (size_t i = 0, y = 0; i < BACKDROP_STRIPS && strip_rows(s->height, i); y += strip_rows(s->height, i), ++i) {
            lv_obj_t *strip = image(s->sheet, &backdrop.kinds[kind].images[i], 0, (int)y);
            if (strip) lv_obj_move_to_index(strip, (int32_t)i);     /* behind the rest */
        }
        s->pictured = true;
    } else if (s->tries < BACKDROP_TRIES) {
        ++s->tries;
        xiaotai_ui_host_invalidate(s->host);
    }
}

static void update(void *state, const xiaotai_ui_canvas_t *canvas, const xiaotai_ui_layer_t *layer,
                   const xiaotai_ui_scene_t *scene)
{
    (void)canvas;
    wide_t *s = state;
    const xiaotai_view_t *v = &layer->view;
    s->updating = true;
    if (!s->built || (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ITEMS | XIAOTAI_CHANGED_ACTIONS |
                                       XIAOTAI_CHANGED_CUSTOM | XIAOTAI_CHANGED_FLAGS | XIAOTAI_CHANGED_INDICATORS)))
        rebuild(s, v);
    if (s->page == HOME || s->page == MODE) put_picture(s, v);
    door_text(s->title, v->title);
    door_text(s->subtitle, v->subtitle);
    door_text(s->body, v->text);
    if (s->page == HOME) {
        static const char *const days[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        char clock[20], date[48];
        if (scene && scene->clock.valid && scene->clock.weekday < 7) {
            (void)snprintf(clock, sizeof(clock), "%02u:%02u", scene->clock.hour, scene->clock.minute);
            (void)snprintf(date, sizeof(date), "%u 月 %u 日  %s", scene->clock.month, scene->clock.day, days[scene->clock.weekday]);
            door_text(s->clock, clock);
            door_text(s->date, date);
        } else {
            door_text(s->clock, "--:--");
            door_text(s->date, "正在同步时间");
        }
        const xiaotai_app_door_home_view_t home = home_data(v);
        if (s->online) {
            const bool online = home.state == XIAOTAI_APP_DOOR_STATION_ONLINE || home.state == XIAOTAI_APP_DOOR_STREAMING;
            lv_obj_set_style_bg_color(s->online, lv_color_hex(online ? GREEN : SUBTLE), 0);
        }
    }
    if ((s->page == CALL || s->page == MONITOR) && v->timer_mode != XIAOTAI_TIMER_NONE) {
        const unsigned sec = (unsigned)max(0, layer->timer_s);
        char timer[24];
        (void)snprintf(timer, sizeof(timer), "%02u:%02u", sec / 60, sec % 60);
        door_text(s->status, timer);
    } else door_text(s->status, s->page == MONITOR && !v->status ? "未连接" : v->status);
    /* The dot and its text are one group: at the left on the home screen,
     * against the right edge of a call. */
    if (s->page == HOME && s->status && s->online) {
        const lv_font_t *font = s->small ? &s->small_font : s->full;
        lv_obj_set_y(s->online, lv_obj_get_style_y(s->status, 0) + (lv_font_get_line_height(font) - 10) / 2);
        door_show(s->online, *lv_label_get_text(s->status) != '\0');
    } else if (s->badge && s->status) {
        lv_point_t size;
        lv_text_get_size(&size, lv_label_get_text(s->status), &door_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_pos(s->badge, s->width - 32 - min(size.x, 152) - 20, 24 + (size.y - 10) / 2);
    }
    if (s->slider) {
        for (size_t i = 0; i < s->n; ++i) if (s->controls[i].element == XIAOTAI_UI_ELEMENT_FIELD) {
            const uint16_t index = s->controls[i].index;
            if (index >= v->field_count) break;
            const xiaotai_view_field_t *field = &v->fields[index];
            char value[24];
            s->field_min = field->min;
            s->field_max = field->max;
            s->field_step = max(1, field->step);
            const int64_t span = (int64_t)field->max - field->min;
            const int32_t slots = (int32_t)((span + s->field_step - 1) / s->field_step);
            const int32_t offset = (int32_t)(((int64_t)field->value - field->min + s->field_step / 2) / s->field_step);
            lv_slider_set_range(s->slider, 0, slots);
            lv_slider_set_value(s->slider, field->value == field->max ? slots : offset, LV_ANIM_OFF);
            (void)snprintf(value, sizeof(value), "%ld%%", (long)field->value);
            door_text(s->percent, value);
            break;
        }
    }
    if (s->page == DEVICES && s->body) lv_obj_set_y(s->body, v->subtitle ? 88 : 24);
    s->updating = false;
}

bool door_templates_cover(uint16_t width, uint16_t height) { return width >= 800 && height >= 480; }
bool door_templates_home(uint16_t width, uint16_t height) { return width >= 440 && height >= 300 && width > height; }

static int match(const xiaotai_display_desc_t *d)
{
    return d && d->pixel == XIAOTAI_PIXEL_RGB565 && door_templates_cover(d->width, d->height) ? 100 : 0;
}

static int match_home(const xiaotai_display_desc_t *d)
{
    return d && d->pixel == XIAOTAI_PIXEL_RGB565 && door_templates_home(d->width, d->height) ? 100 : 0;
}

static xiaotai_err_t open(const xiaotai_ui_host_t *host, const xiaotai_ui_canvas_t *c,
                         const xiaotai_ui_layer_t *layer, void **out)
{
    if (!out || !c || !c->root || c->backend != XIAOTAI_UI_BACKEND_LVGL || !layer) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    wide_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    const char *name = layer->view.custom;
    const char *id = layer->view.id;
    s->page = equal(name, XIAOTAI_APP_DOOR_TEMPLATE_HOME) ? HOME : equal(name, XIAOTAI_APP_DOOR_TEMPLATE_MODE) ? MODE :
        equal(name, XIAOTAI_APP_DOOR_TEMPLATE_DEVICES) ? DEVICES :
        equal(name, XIAOTAI_APP_DOOR_TEMPLATE_MONITOR) ? MONITOR : equal(name, XIAOTAI_APP_DOOR_TEMPLATE_CALL) ? CALL :
        equal(id, XIAOTAI_APP_DOOR_OVERLAY_VOLUME) ? VOLUME : CONFIRM;
    s->host = host;
    s->layer = layer->view.layer;
    s->width = c->width;
    s->height = c->height;
    s->root = c->root;
    s->full = lv_obj_get_style_text_font(c->root, LV_PART_MAIN);
    s->button_font = door_font_button;
    s->button_font.fallback = s->full;
    s->body_font = door_font_body;
    s->body_font.fallback = s->full;
    s->small_font = door_font_small;
    s->small_font.fallback = s->full;
    s->small = !door_templates_cover(c->width, c->height);
    s->sheet = door_box(c->root, 0, 0, c->width, c->height, BG, LV_OPA_COVER);
    if (!s->sheet) { xiaotai_mem_free(s); return XIAOTAI_ERR_NO_MEM; }
    if (s->page == HOME || s->page == MODE) backdrop_enter();
    *out = s;
    update(s, c, layer, NULL);
    return XIAOTAI_OK;
}

static void close(void *state)
{
    wide_t *s = state;
    s->updating = true;
    if (s->video) (void)xiaotai_ui_host_video_region(s->host, NULL);
    release_sheet(s);
    if (s->toast_box) lv_obj_delete(s->toast_box);
    lv_obj_delete(s->sheet);
    if (s->page == HOME || s->page == MODE) backdrop_leave();
    xiaotai_mem_free(s);
}

static bool toast(void *state, const char *message)
{
    wide_t *s = state;
    if (!message) { door_show(s->toast_box, false); return true; }
    if (!s->toast_box) {
        const int w = min(620, s->width - 48), h = s->small ? 40 : 48;
        lv_obj_t *top = lv_display_get_layer_top(lv_obj_get_display(s->root));
        s->toast_box = door_box(top, (s->width - w) / 2, s->height - (s->small ? 124 : 224), w, h, PLATE, LV_OPA_COVER);
        if (s->toast_box) lv_obj_set_style_radius(s->toast_box, RADIUS, 0);
        s->toast_label = door_label(s->toast_box, 12, s->small ? 6 : 5, w - 24, s->small ? &s->small_font : &s->button_font, BODY,
                               LV_TEXT_ALIGN_CENTER);
    }
    if (!s->toast_label) return false;
    door_text(s->toast_label, message);
    door_show(s->toast_box, true);
    return true;
}

static size_t controls(void *state, xiaotai_ui_control_t *out, size_t capacity)
{
    const wide_t *s = state;
    size_t count = 0;
    lv_obj_update_layout(s->root);
    for (size_t i = 0; i < s->n; ++i) {
        const control_t *c = &s->controls[i];
        lv_area_t a;
        lv_obj_get_coords(c->object, &a);
        if (c->element == XIAOTAI_UI_ELEMENT_ITEM && s->list) {
            lv_area_t visible;
            lv_obj_get_coords(s->list, &visible);
            a.x1 = max(a.x1, visible.x1);
            a.y1 = max(a.y1, visible.y1);
            a.x2 = min(a.x2, visible.x2);
            a.y2 = min(a.y2, visible.y2);
            if (a.x1 > a.x2 || a.y1 > a.y2) continue;
        }
        if (out && count < capacity)
            out[count] = (xiaotai_ui_control_t){.x = (int16_t)a.x1, .y = (int16_t)a.y1,
                .width = (uint16_t)lv_area_get_width(&a), .height = (uint16_t)lv_area_get_height(&a),
                .layer = s->layer, .element = c->element, .index = c->index};
        ++count;
    }
    return count;
}

xiaotai_err_t door_templates_register(void)
{
#define TEMPLATE(id, fits) {.name = id, .backend = XIAOTAI_UI_BACKEND_LVGL, .match = fits, \
                      .open = open, .update = update, .close = close, .toast = toast, .controls = controls}
    static const xiaotai_ui_template_t templates[] = {
        TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_HOME, match_home), TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_MODE, match_home),
        TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_DEVICES, match), TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_MONITOR, match),
        TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_CALL, match), TEMPLATE(XIAOTAI_APP_DOOR_TEMPLATE_DIALOG, match),
    };
#undef TEMPLATE
    xiaotai_critical_init(&backdrop.lock);
    for (size_t i = 0; i < sizeof(templates) / sizeof(*templates); ++i) {
        const xiaotai_err_t err = xiaotai_ui_template_register(&templates[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}
