// SPDX-License-Identifier: MIT
/* The pages that fill the display whatever its size: somebody looks at the
 * door, what became of opening it, the access machine's keypad, and the
 * doorbell's home where the display has no picture of its own (smaller than
 * the wide templates' home). One drawing for every colour display:
 * the sizes follow the display, the words come in its theme's font (in the
 * door's own where the product carries it). Nothing here is a bitmap. */
#include "templates_station.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "door_widgets.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/ui_theme.h"
LV_FONT_DECLARE(door_font_digits);
#ifdef XIAOTAI_DOOR_WIDE
#include "templates_wide.h"
LV_FONT_DECLARE(door_font_clock_small);
LV_FONT_DECLARE(door_font_button);
LV_FONT_DECLARE(door_font_body);
LV_FONT_DECLARE(door_font_title);
#endif

enum { INK = 0xf2f5f7, AMBER = 0xffb020, BLUE = 0x5ec8f2, GREEN = 0x1fa672, LIGHT_GREEN = 0x54e0a2, RED = 0xdc434d,
       SUBTLE = 0x98a3ad, GROUND = 0x101418, KEY = 0x232b34, OUTLINE = 0x394552,
       AMBER_GROUND = 0x1c1406, BLUE_GROUND = 0x07161f, GREEN_GROUND = 0x05281a, OPEN_GROUND = 0x0c7a4f, SHUT_GROUND = 0x5c1219,
       PANEL_TOP = 0x2a323b, PANEL_BOTTOM = 0x0e1216, PLATE = 0x1a2027 };
/* Steps far enough apart that a small chip has time for the sound between them. */
enum { BARS = 7, STEP_MS = 90, BREATH_MS = 1600, OPENING_MS = 420, MAX_KNOBS = XIAOTAI_VIEW_ACTIONS_MAX + 12 };
/* What the other end has to be at least as loud as to move the bars all the way. */
enum { LOUD = 3000 };

typedef enum { LOOKED, OPENED, KEYPAD, BELL } page_t;
typedef struct pane pane_t;
typedef struct {
    pane_t *owner;
    lv_obj_t *object;
    uint16_t index;
    uint8_t element;
    bool pressed, hold;     /* hold: a long press is what it is for */
} knob_t;

struct pane {
    const xiaotai_ui_host_t *host;
    lv_obj_t *root, *sheet, *title, *subtitle, *body, *dot, *shackle, *gap, *clock, *date, *typed;
    lv_obj_t *bar[BARS];
    const lv_font_t *full, *digits, *name;           /* a name may hold any character: one face for all of them */
    lv_font_t heading, words, caption;      /* with the theme's font behind them */
    knob_t knobs[MAX_KNOBS];
    lv_point_precise_t chevrons[2][3];      /* the pager's arrows on the keypad, kept while drawn */
    size_t n;
    int bars_y, bars_h, level;
    int32_t lift;           /* how far the shackle goes */
    uint32_t stepped;
    page_t page;
    uint8_t layer;
    int width, height;                      /* inside the frame */
    bool built, updating, wide;
};

/* Pages that cover the whole display now: what moves under them stands still. */
static unsigned covering;

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }

/* One line: what does not fit ends in dots instead of a second line. */
static lv_obj_t *line_of(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, uint32_t color, int align)
{
    lv_obj_t *object = door_label(parent, x, y, w, font, color, align);
    if (object) lv_obj_set_height(object, lv_font_get_line_height(font));
    return object;
}

/* A narrow title gives up the brand before it loses what the device is. */
static const char *fitting_title(const pane_t *s, const char *title)
{
    static const char brand[] = "小钛";
    if (!s->title || !title || strncmp(title, brand, sizeof(brand) - 1U) || !title[sizeof(brand) - 1U]) return title;
    const lv_font_t *font = lv_obj_get_style_text_font(s->title, LV_PART_MAIN);
    if (!font) return title;
    lv_point_t size;
    lv_text_get_size(&size, title, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    /* The width the label was given, not the one laid out: a label just built has none yet, and the
     * title must read the same the first time as every time after. */
    return size.x <= lv_obj_get_style_width(s->title, LV_PART_MAIN) ? title : title + sizeof(brand) - 1U;
}

static void knob_event(lv_event_t *event)
{
    knob_t *k = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    xiaotai_ui_input_t input;
    if (code == LV_EVENT_DELETE) {
        if (!k->pressed) return;
        input = XIAOTAI_UI_INPUT_RELEASE;
    } else if (k->owner->updating) return;
    else if (code == LV_EVENT_PRESSED) input = XIAOTAI_UI_INPUT_PRESS;
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) input = XIAOTAI_UI_INPUT_RELEASE;
    else if (code == LV_EVENT_CLICKED) input = XIAOTAI_UI_INPUT_TAP;
    else if (code == LV_EVENT_LONG_PRESSED && k->hold) input = XIAOTAI_UI_INPUT_LONG_PRESS;
    else return;
    if (input == XIAOTAI_UI_INPUT_PRESS || input == XIAOTAI_UI_INPUT_RELEASE) k->pressed = input == XIAOTAI_UI_INPUT_PRESS;
    xiaotai_ui_host_input(k->owner->host, k->owner->layer, k->element, k->index, input, 0, NULL);
}

static void bind(pane_t *s, lv_obj_t *object, uint8_t element, size_t index, uint8_t state, bool hold)
{
    if (!object || s->n >= MAX_KNOBS) return;
    knob_t *k = &s->knobs[s->n++];
    *k = (knob_t){.owner = s, .object = object, .index = (uint16_t)index, .element = element, .hold = hold};
    if (state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY)) {
        lv_obj_set_style_opa(object, LV_OPA_50, 0);
        return;
    }
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(object, knob_event, LV_EVENT_ALL, k);
}

static void knob(pane_t *s, const xiaotai_view_action_t *a, size_t index, int x, int y, int w, int h)
{
    const uint32_t color = a->style == XIAOTAI_STYLE_DESTRUCTIVE ? RED : a->style == XIAOTAI_STYLE_POSITIVE ? GREEN : INK;
    lv_obj_t *object = door_box(s->sheet, x, y, w, h, color, color == INK ? LV_OPA_20 : LV_OPA_COVER);
    if (!object) return;
    lv_obj_set_style_radius(object, h / 2, 0);
    lv_obj_set_style_bg_opa(object, color == INK ? LV_OPA_40 : LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t *words = line_of(object, 8, (h - lv_font_get_line_height(&s->caption)) / 2, w - 16, &s->caption, INK, LV_TEXT_ALIGN_CENTER);
    door_text(words, a->label);
    bind(s, object, XIAOTAI_UI_ELEMENT_ACTION, index, a->state, false);
}

/* The controls stand evenly around the middle of the bottom. Returns where they begin. */
static int build_knobs(pane_t *s, const xiaotai_view_t *v, int margin)
{
    int count = 0;
    for (size_t i = 0; i < v->action_count; ++i) count += v->actions[i].label != NULL && !(v->actions[i].state & XIAOTAI_STATE_HIDDEN);
    const int h = s->wide ? 76 : clamp(s->height / 5, 40, 64), top = s->height - margin - h;
    if (!count) return s->height - margin;
    const int gap = clamp(s->width / 40, 10, 24), room = (s->width - 2 * margin - (count - 1) * gap) / count;
    const int w = clamp(room, 60, s->wide ? 240 : 150);
    int x = (s->width - count * w - (count - 1) * gap) / 2;
    for (size_t i = 0; i < v->action_count; ++i) {
        if (!v->actions[i].label || (v->actions[i].state & XIAOTAI_STATE_HIDDEN)) continue;
        knob(s, &v->actions[i], i, x, top, w, h);
        x += w + gap;
    }
    return top;
}

static lv_obj_t *ring(lv_obj_t *parent, int cx, int cy, int diameter, int border, uint32_t color)
{
    lv_obj_t *object = door_box(parent, cx - diameter / 2, cy - diameter / 2, diameter, diameter, color,
                                border ? LV_OPA_TRANSP : LV_OPA_COVER);
    if (!object) return NULL;
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(object, border, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(color), 0);
    return object;
}

/* A lens: somebody sees through this device. */
static void draw_lens(pane_t *s, int cx, int cy, int d, uint32_t color, uint32_t ground)
{
    (void)ring(s->sheet, cx, cy, d, clamp(d / 12, 3, 14), color);
    (void)ring(s->sheet, cx, cy, d * 62 / 100, 0, color);
    (void)ring(s->sheet, cx, cy, d * 36 / 100, 0, ground);
    (void)ring(s->sheet, cx - d * 13 / 100, cy - d * 13 / 100, d * 13 / 100, 0, INK);
}

/* A bell that swings is a call; here it only stands. */
static void draw_bell(pane_t *s, int cx, int cy, int d, uint32_t color)
{
    const int w = d * 64 / 100, h = d * 58 / 100, top = cy - d / 2 + d * 8 / 100;
    lv_obj_t *dome = door_box(s->sheet, cx - w / 2, top, w, h, color, LV_OPA_COVER);
    if (dome) lv_obj_set_style_radius(dome, w / 2, 0);
    (void)door_box(s->sheet, cx - w / 2, top + h / 2, w, h / 2, color, LV_OPA_COVER);
    lv_obj_t *rim = door_box(s->sheet, cx - d * 44 / 100, top + h - d * 2 / 100, d * 88 / 100, d * 11 / 100, color, LV_OPA_COVER);
    if (rim) lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, 0);
    (void)ring(s->sheet, cx, top + h + d * 20 / 100, d * 18 / 100, 0, color);
}

static void build_bars(pane_t *s, int cx, int cy, int d, uint32_t color)
{
    const int width = clamp(d / 10, 5, 18), pitch = width * 2;
    s->bars_y = cy;
    s->bars_h = d;
    s->level = 0;
    for (int i = 0; i < BARS; ++i) {
        s->bar[i] = door_box(s->sheet, cx - width / 2 + (i - BARS / 2) * pitch, cy - width / 2, width, width, color, LV_OPA_COVER);
        if (s->bar[i]) lv_obj_set_style_radius(s->bar[i], width / 2, 0);
    }
}

static void bars_step(void *var, int32_t unused)
{
    (void)unused;
    static const uint8_t share[BARS] = {38, 62, 86, 100, 80, 66, 42};
    pane_t *s = var;
    const uint32_t now = lv_tick_get();
    if (covering || now - s->stepped < STEP_MS) return;
    s->stepped = now;
    xiaotai_media_loudness_t sound;
    xiaotai_media_loudness(&sound);
    const int loud = (int)(sound.playout > 2U * sound.capture ? sound.playout : 2U * sound.capture);
    const int target = clamp(loud * 100 / LOUD, 0, 100);
    s->level += (target - s->level) * (target > s->level ? 6 : 3) / 10;
    for (int i = 0; i < BARS; ++i) {
        if (!s->bar[i]) continue;
        const int width = lv_obj_get_style_width(s->bar[i], 0);
        const int idle = 6 + (int)((now / STEP_MS + (uint32_t)i * 3U) % 8U);       /* a ripple that is always there */
        const int part = s->level * share[i] / 100;
        const int h = clamp(s->bars_h * (part > idle ? part : idle) / 100, width, s->bars_h);
        if (h == lv_obj_get_style_height(s->bar[i], 0)) continue;
        lv_obj_set_pos(s->bar[i], lv_obj_get_style_x(s->bar[i], 0), s->bars_y - h / 2);
        lv_obj_set_height(s->bar[i], h);
    }
}

static void breathe(void *var, int32_t step)
{
    if (covering) return;
    const lv_opa_t opa = (lv_opa_t)(60 + step * 24);       /* eight steps: the dot is drawn a few times a second */
    if (lv_obj_get_style_opa(var, 0) != opa) lv_obj_set_style_opa(var, opa, 0);
}

static void shackle_step(void *var, int32_t lift)
{
    pane_t *s = var;
    const uint32_t now = lv_tick_get();
    if (!s->shackle || (lift != s->lift && now - s->stepped < STEP_MS)) return;
    s->stepped = now;
    lv_obj_set_style_translate_y(s->shackle, -lift, 0);
    if (s->gap) door_show(s->gap, lift * 2 >= lv_obj_get_style_height(s->gap, 0));   /* one leg is out */
}

static void animate(void *var, lv_anim_exec_xcb_t step, int32_t from, int32_t to, uint32_t ms, bool forever)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, step);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    if (forever) {
        lv_anim_set_playback_duration(&a, ms);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    } else lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    (void)lv_anim_start(&a);
}

static xiaotai_app_door_looked_view_t looked_data(const xiaotai_view_t *v)
{
    xiaotai_app_door_looked_view_t result = {.camera = true};
    if (v->custom_data && v->custom_size == sizeof(result)) memcpy(&result, v->custom_data, sizeof(result));
    return result;
}

/* A frame in the colour of what goes on: amber while somebody looks, blue
 * while the visitor calls them, green while both talk. */
static void build_looked(pane_t *s, const xiaotai_view_t *v)
{
    const xiaotai_app_door_looked_view_t data = looked_data(v);
    const bool talking = data.state == XIAOTAI_APP_DOOR_LOOKED_TALKING, ringing = data.state == XIAOTAI_APP_DOOR_LOOKED_RINGING;
    const uint32_t color = talking ? LIGHT_GREEN : ringing ? BLUE : AMBER;
    const uint32_t ground = talking ? GREEN_GROUND : ringing ? BLUE_GROUND : AMBER_GROUND;
    const int edge = clamp(s->height / 70, 4, 9), margin = clamp(s->height / 28, 8, 28);
    s->width -= 2 * edge;
    s->height -= 2 * edge;
    lv_obj_set_style_bg_color(s->sheet, lv_color_hex(ground), 0);
    lv_obj_set_style_border_width(s->sheet, edge, 0);
    lv_obj_set_style_border_color(s->sheet, lv_color_hex(color), 0);
    const int knobs = build_knobs(s, v, margin), cx = s->width / 2;
    const int heading = lv_font_get_line_height(&s->heading), line = lv_font_get_line_height(&s->words);
    const int gap = clamp(s->height / 48, 4, 14), words = heading + 2 * line + 3 * gap;
    const int room = knobs - margin - gap - words, d = clamp(room, 30, s->height * 36 / 100);
    int y = (knobs - gap - words - d) / 2;
    if (y < margin) y = margin;
    if (talking || !data.camera) {
        build_bars(s, cx, y + d / 2, d, color);
        if (ringing) s->bars_h = d / 2;
    } else if (ringing) draw_bell(s, cx, y + d / 2, d, color);
    else draw_lens(s, cx, y + d / 2, d, color, ground);
    y += d + gap;
    s->title = line_of(s->sheet, margin, y, s->width - 2 * margin, &s->heading, color, LV_TEXT_ALIGN_CENTER);
    y += heading + gap;
    s->subtitle = line_of(s->sheet, margin, y + (line - lv_font_get_line_height(s->name)) / 2, s->width - 2 * margin, s->name, INK, LV_TEXT_ALIGN_CENTER);
    s->dot = ring(s->sheet, 0, y + line / 2, clamp(line / 2, 8, 16), 0, color);
    y += line + gap;
    s->body = line_of(s->sheet, margin, y, s->width - 2 * margin, &s->words, 0xb9c2ca, LV_TEXT_ALIGN_CENTER);
    if (s->dot) animate(s->dot, breathe, 0, 8, BREATH_MS, true);
    if (s->bar[0]) animate(s, bars_step, 0, 1, 1000, true);
}

static void build_opened(pane_t *s, const xiaotai_view_t *v)
{
    xiaotai_app_door_result_view_t result = {0};
    if (v->custom_data && v->custom_size == sizeof(result)) memcpy(&result, v->custom_data, sizeof(result));
    const uint32_t ground = result.opened ? OPEN_GROUND : SHUT_GROUND;
    const int margin = clamp(s->height / 22, 10, 36), cx = s->width / 2;
    lv_obj_set_style_bg_color(s->sheet, lv_color_hex(ground), 0);
    const int knobs = build_knobs(s, v, margin);
    const int heading = lv_font_get_line_height(&s->heading), line = lv_font_get_line_height(&s->words);
    const int gap = clamp(s->height / 32, 6, 20), words = heading + line + 2 * gap;
    const int d = clamp(knobs - 2 * margin - words, 40, s->height * 42 / 100);
    int y = (knobs - words - d) / 2;
    if (y < margin) y = margin;
    /* The shackle goes up out of the body. */
    const int lift = d * 13 / 100, bw = d * 72 / 100, bh = d * 50 / 100, sw = d * 48 / 100, border = clamp(d / 11, 4, 18);
    s->shackle = door_box(s->sheet, cx - sw / 2, y + lift, sw, d - lift, INK, LV_OPA_TRANSP);
    if (s->shackle) {
        lv_obj_set_style_radius(s->shackle, sw / 2, 0);
        lv_obj_set_style_border_width(s->shackle, border, 0);
        lv_obj_set_style_border_color(s->shackle, lv_color_hex(INK), 0);
    }
    lv_obj_t *lock = door_box(s->sheet, cx - bw / 2, y + d - bh, bw, bh, INK, LV_OPA_COVER);
    if (lock) lv_obj_set_style_radius(lock, clamp(d / 12, 4, 18), 0);
    if (result.opened && s->shackle) {
        s->gap = door_box(s->sheet, cx + sw / 2 - border - 1, y + d - bh - lift, border + 2, lift, ground, LV_OPA_COVER);
        door_show(s->gap, false);
        s->lift = lift;
        animate(s, shackle_step, 0, lift, OPENING_MS, false);
    }
    (void)ring(s->sheet, cx, y + d - bh * 58 / 100, d * 13 / 100, 0, ground);
    (void)door_box(s->sheet, cx - d * 2 / 100, y + d - bh * 58 / 100, d * 4 / 100 + 1, bh * 34 / 100, ground, LV_OPA_COVER);
    y += d + gap;
    s->title = line_of(s->sheet, margin, y, s->width - 2 * margin, &s->heading, INK, LV_TEXT_ALIGN_CENTER);
    y += heading + gap;
    s->body = line_of(s->sheet, margin, y, s->width - 2 * margin, &s->words, 0xd6ece2, LV_TEXT_ALIGN_CENTER);
    if (s->body && !result.opened) lv_obj_set_style_text_color(s->body, lv_color_hex(INK), 0);
}

static void sheet_event(lv_event_t *event)
{
    pane_t *s = lv_event_get_user_data(event);
    lv_indev_t *device = lv_indev_active();
    const lv_dir_t direction = device ? lv_indev_get_gesture_dir(device) : LV_DIR_NONE;
    if (s->updating || (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT)) return;
    xiaotai_ui_host_input(s->host, s->layer, XIAOTAI_UI_ELEMENT_OTHER, 0,
                          direction == LV_DIR_LEFT ? XIAOTAI_UI_INPUT_SWIPE_LEFT : XIAOTAI_UI_INPUT_SWIPE_RIGHT, 0, NULL);
}

/* A panel of brushed dark metal, lighter at the top: what a door station is made of. */
static void panel(pane_t *s)
{
    lv_obj_set_style_bg_color(s->sheet, lv_color_hex(PANEL_TOP), 0);
    lv_obj_set_style_bg_grad_color(s->sheet, lv_color_hex(PANEL_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(s->sheet, LV_GRAD_DIR_VER, 0);
}

/* A rounded plate with words in the middle. */
static lv_obj_t *key(pane_t *s, int x, int y, int w, int h, const char *words, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *object = door_box(s->sheet, x, y, w, h, color, LV_OPA_COVER);
    if (!object) return NULL;
    lv_obj_set_style_radius(object, clamp(h / 5, 8, 22), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_50, LV_STATE_PRESSED);
    door_text(line_of(object, 2, (h - lv_font_get_line_height(font)) / 2, w - 4, font, INK, LV_TEXT_ALIGN_CENTER), words);
    return object;
}

/* The time and what was typed on the left, the twelve keys on the right; on a
 * tall display the keys stand below. */
static void build_keypad(pane_t *s, const xiaotai_view_t *v)
{
    const int w = s->width, h = s->height, margin = s->wide ? 24 : clamp(h / 40, 6, 24), gap = clamp(h / 45, 5, 14);
    const bool tall = h > w;
    const int side = tall ? w - 2 * margin : w * 40 / 100 - margin, keys_x = tall ? margin : margin + side + margin;
    const int keys_y = tall ? h * 46 / 100 : margin, keys_w = w - margin - keys_x, keys_h = h - margin - keys_y;
    const lv_font_t *digits = s->digits, *words = &s->words;
    panel(s);
    lv_obj_add_event_cb(s->sheet, sheet_event, LV_EVENT_GESTURE, s);
    for (size_t i = 0; i < v->item_count && i < 12U; ++i) {
        const int cw = (keys_w - 2 * gap) / 3, ch = (keys_h - 3 * gap) / 4;
        const bool call = i == 11U, number = i != 9U && !call;
        lv_obj_t *object = key(s, keys_x + (int)(i % 3U) * (cw + gap), keys_y + (int)(i / 3U) * (ch + gap), cw, ch, v->items[i].label,
                               number ? digits : &s->caption, call ? GREEN : KEY);
        if (object && !call) {      /* metal keys: an edge that catches the light */
            lv_obj_set_style_border_width(object, 1, 0);
            lv_obj_set_style_border_color(object, lv_color_hex(OUTLINE), 0);
            lv_obj_set_style_border_side(object, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
        }
        bind(s, object, XIAOTAI_UI_ELEMENT_ITEM, i, v->items[i].state, false);
    }
    const int line = lv_font_get_line_height(words), big = lv_font_get_line_height(digits);
    int y = margin;
    /* The pager's arrows at the end of the title line, clear of the keys:
     * swiping pages too, the arrows say that there is more. */
    const int caption = lv_font_get_line_height(&s->caption), arrow = clamp(caption + (side < 160 ? 4 : 12), 22, 52);
    int arrows = 0;
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (!a->icon || (a->trigger != XIAOTAI_TRIGGER_SWIPE_LEFT && a->trigger != XIAOTAI_TRIGGER_SWIPE_RIGHT)) continue;
        const bool next = a->trigger == XIAOTAI_TRIGGER_SWIPE_LEFT;
        const int x = margin + side - (next ? arrow : 2 * arrow + gap);
        lv_obj_t *object = door_box(s->sheet, x, y + (caption - arrow) / 2, arrow, arrow, KEY, LV_OPA_60);
        if (!object) continue;
        lv_obj_set_style_radius(object, arrow / 2, 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_point_precise_t *points = s->chevrons[next ? 1 : 0];
        const int lo = arrow * 38 / 100, hi = arrow * 60 / 100;
        points[0] = (lv_point_precise_t){next ? lo : hi, arrow * 27 / 100};
        points[1] = (lv_point_precise_t){next ? hi : lo, arrow / 2};
        points[2] = (lv_point_precise_t){next ? lo : hi, arrow * 73 / 100};
        lv_obj_t *chevron = lv_line_create(object);
        if (chevron) {
            lv_line_set_points(chevron, points, 3);
            lv_obj_set_pos(chevron, 0, 0);
            lv_obj_set_style_line_width(chevron, clamp(arrow / 12, 2, 4), 0);
            lv_obj_set_style_line_rounded(chevron, true, 0);
            lv_obj_set_style_line_color(chevron, lv_color_hex(INK), 0);
            lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
        }
        bind(s, object, XIAOTAI_UI_ELEMENT_ACTION, i, a->state, false);
        ++arrows;
    }
    s->title = line_of(s->sheet, margin, y, side - (arrows ? 2 * arrow + gap + gap / 2 : 0), &s->caption, SUBTLE,
                       LV_TEXT_ALIGN_LEFT);
    for (size_t i = 0; i < v->action_count; ++i) {
        if (v->actions[i].trigger != XIAOTAI_TRIGGER_LONG_PRESS || !s->title) continue;
        lv_obj_set_ext_click_area(s->title, margin);
        bind(s, s->title, XIAOTAI_UI_ELEMENT_ACTION, i, 0, true);
    }
    /* From below: the two ways besides the keys, then the number. */
    const int bottom = tall ? keys_y - margin : h - margin, button = s->wide ? 72 : clamp(h * 15 / 100, 36, 52);
    int count = 0, top = bottom;
    for (size_t i = 0; i < v->action_count; ++i) count += v->actions[i].label != NULL;
    const bool beside = s->wide || tall || side >= 2 * 74 + gap;
    const int bw = beside && count > 1 ? (side - gap) / 2 : side;
    for (size_t i = v->action_count, k = 0; i-- > 0;) {
        if (!v->actions[i].label) continue;
        const int x = beside && count > 1 ? margin + (k == 0 ? bw + gap : 0) : margin;
        if (!beside || k == 0) top -= button;
        bind(s, key(s, x, top, bw, button, v->actions[i].label, &s->caption, KEY), XIAOTAI_UI_ELEMENT_ACTION, i, v->actions[i].state, false);
        if (!beside) top -= gap;
        ++k;
    }
    if (beside) top -= gap;
    const int box_h = clamp(big + gap, 40, 108), box_y = top - box_h - (beside ? gap : 0);
    lv_obj_t *field = door_box(s->sheet, margin, box_y, side, box_h, 0x000000, LV_OPA_40);
    if (field) {
        lv_obj_set_style_radius(field, clamp(box_h / 6, 8, 16), 0);
        lv_obj_set_style_border_width(field, 1, 0);
        lv_obj_set_style_border_color(field, lv_color_hex(OUTLINE), 0);
        s->typed = line_of(field, 8, (box_h - 2 - big) / 2, side - 18, digits, INK, LV_TEXT_ALIGN_CENTER);
        s->body = line_of(field, 8, (box_h - 2 - line) / 2, side - 18, words, SUBTLE, LV_TEXT_ALIGN_CENTER);
    }
    /* The time has what is left. */
    const int first = margin + lv_font_get_line_height(&s->caption) + gap, room = box_y - gap - first;
    const bool clock = room >= big, date = room >= big + line;
    y = first + (room - (clock ? big : 0) - (date ? line : 0)) / 2;
    s->clock = clock ? line_of(s->sheet, margin, y, side, digits, INK, LV_TEXT_ALIGN_LEFT) : NULL;
    s->date = date ? line_of(s->sheet, margin, y + big, side, words, SUBTLE, LV_TEXT_ALIGN_LEFT) : NULL;
}

/* The doorbell at one household's door, where the display has no picture of
 * its own: the time and how things stand on the left, the bell's face plate
 * on the right with the camera on top and the big round button below it. The
 * other controls are quiet words under the time. */
static void build_bell(pane_t *s, const xiaotai_view_t *v)
{
    xiaotai_app_door_home_view_t home = {0};
    if (v->custom_data && v->custom_size == sizeof(home)) memcpy(&home, v->custom_data, sizeof(home));
    const int w = s->width, h = s->height, margin = clamp(h / 20, 8, 24), gap = clamp(h / 40, 4, 12);
    panel(s);
    lv_obj_add_event_cb(s->sheet, sheet_event, LV_EVENT_GESTURE, s);
    /* The plate. */
    const int plate_w = clamp(w * 38 / 100, 96, 360), plate_h = h - 2 * margin, px = w - margin - plate_w, py = margin;
    lv_obj_t *plate = door_box(s->sheet, px, py, plate_w, plate_h, PLATE, LV_OPA_COVER);
    if (plate) {
        lv_obj_set_style_radius(plate, clamp(plate_w / 6, 10, 40), 0);
        lv_obj_set_style_border_width(plate, 1, 0);
        lv_obj_set_style_border_color(plate, lv_color_hex(OUTLINE), 0);
        lv_obj_set_style_bg_grad_color(plate, lv_color_hex(PANEL_BOTTOM), 0);
        lv_obj_set_style_bg_grad_dir(plate, LV_GRAD_DIR_VER, 0);
    }
    const int cx = px + plate_w / 2, lens = clamp(plate_w * 26 / 100, 22, 90);
    draw_lens(s, cx, py + margin + lens / 2, lens, OUTLINE, PLATE);
    /* The button: the bell, or what has to come first (adding an indoor station). */
    const int words = lv_font_get_line_height(&s->caption);
    const int room = plate_h - (margin + lens + gap) - (gap + words + margin);
    const int d = clamp(room < plate_w * 72 / 100 ? room : plate_w * 72 / 100, 40, 260);
    const int by = py + margin + lens + gap + (room - d) / 2;
    const xiaotai_view_action_t *press = NULL;
    size_t press_index = 0;
    for (size_t i = 0; i < v->action_count; ++i) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (a->label && a->role == XIAOTAI_ROLE_PRIMARY && !(a->state & XIAOTAI_STATE_HIDDEN)) {
            press = a;
            press_index = i;
            break;
        }
    }
    if (press) {
        const bool bell = press->intent && !strcmp(press->intent, XIAOTAI_INTENT_APP_DOOR_RING);
        lv_obj_t *button = ring(s->sheet, cx, by + d / 2, d, clamp(d / 14, 3, 12), bell ? LIGHT_GREEN : BLUE);
        if (button) {
            lv_obj_set_style_bg_color(button, lv_color_hex(bell ? GREEN : BLUE_GROUND), 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_STATE_PRESSED);
            bind(s, button, XIAOTAI_UI_ELEMENT_ACTION, press_index, press->state, false);
        }
        if (bell) {
            /* The bell inside it is drawn on the sheet: the button keeps the touch. */
            draw_bell(s, cx, by + d / 2, d * 56 / 100, INK);
        }
        lv_obj_t *label = line_of(s->sheet, px + 4, by + d + gap, plate_w - 8, &s->caption, INK, LV_TEXT_ALIGN_CENTER);
        door_text(label, bell ? "按门铃" : press->label);
    }
    /* Left: who this is, the time, how things stand. */
    const int left_w = px - 2 * margin, line = lv_font_get_line_height(&s->words), big = lv_font_get_line_height(s->digits);
    int y = margin;
    s->title = line_of(s->sheet, margin, y, left_w, &s->caption, SUBTLE, LV_TEXT_ALIGN_LEFT);
    for (size_t i = 0; i < v->action_count; ++i) {
        if (v->actions[i].trigger != XIAOTAI_TRIGGER_LONG_PRESS || !s->title) continue;
        lv_obj_set_ext_click_area(s->title, margin);
        bind(s, s->title, XIAOTAI_UI_ELEMENT_ACTION, i, 0, true);
    }
    y += lv_font_get_line_height(&s->caption) + gap;
    s->subtitle = line_of(s->sheet, margin, y, left_w, &s->heading, INK, LV_TEXT_ALIGN_LEFT);
    y += lv_font_get_line_height(&s->heading) + gap;
    /* From below: the quiet controls, then what the visitor is told. */
    int bottom = h - margin;
    const int button = clamp(h * 15 / 100, 30, 52);
    for (size_t i = v->action_count; i-- > 0;) {
        const xiaotai_view_action_t *a = &v->actions[i];
        if (a == press || !a->label || (a->state & XIAOTAI_STATE_HIDDEN)) continue;
        bottom -= button;
        bind(s, key(s, margin, bottom, left_w, button, a->label, &s->caption, KEY), XIAOTAI_UI_ELEMENT_ACTION, i, a->state, false);
        bottom -= gap;
    }
    const uint32_t mark = home.state == XIAOTAI_APP_DOOR_STATION_ONLINE ? LIGHT_GREEN : AMBER;
    s->body = line_of(s->sheet, margin, bottom - line, left_w, &s->words, SUBTLE, LV_TEXT_ALIGN_LEFT);
    if (s->body) {
        lv_label_set_long_mode(s->body, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(s->body, 2 * line);
        lv_obj_set_y(s->body, bottom - 2 * line);
    }
    const int status_y = bottom - 2 * line - gap - line, dot = clamp(line / 2, 6, 14);
    s->dot = ring(s->sheet, margin + dot / 2, status_y + line / 2, dot, 0, mark);
    s->typed = line_of(s->sheet, margin + dot + gap, status_y, left_w - dot - gap, &s->words, mark, LV_TEXT_ALIGN_LEFT);
    /* The time has what is left between the name and the status. */
    const int clock_room = status_y - gap - y;
    s->clock = clock_room >= big ? line_of(s->sheet, margin, y + (clock_room - big - (clock_room >= big + line ? line : 0)) / 2,
                                           left_w, s->digits, INK, LV_TEXT_ALIGN_LEFT) : NULL;
    s->date = s->clock && clock_room >= big + line ?
              line_of(s->sheet, margin, lv_obj_get_style_y(s->clock, 0) + big, left_w, &s->words, SUBTLE, LV_TEXT_ALIGN_LEFT) : NULL;
    if (s->dot) animate(s->dot, breathe, 0, 8, BREATH_MS, true);
}

static void update_bell(pane_t *s, const xiaotai_view_t *v, const xiaotai_ui_scene_t *scene)
{
    static const char *const days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    door_text(s->subtitle, v->subtitle);
    door_text(s->typed, v->status);
    door_text(s->body, v->text);
    if (!scene) return;
    char clock[16] = "--:--", date[48] = "正在同步时间";
    if (scene->clock.valid && scene->clock.weekday < 7) {
        (void)snprintf(clock, sizeof(clock), "%02u:%02u", scene->clock.hour, scene->clock.minute);
        (void)snprintf(date, sizeof(date), "%u月%u日 %s", scene->clock.month, scene->clock.day, days[scene->clock.weekday]);
    }
    door_text(s->clock, clock);
    door_text(s->date, date);
}

static void update_keypad(pane_t *s, const xiaotai_view_t *v, const xiaotai_ui_scene_t *scene)
{
    static const char *const days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    xiaotai_app_door_keypad_view_t pad = {0};
    if (v->custom_data && v->custom_size == sizeof(pad)) memcpy(&pad, v->custom_data, sizeof(pad));
    pad.typed[sizeof(pad.typed) - 1U] = '\0';
    /* The last digits, where not all of them fit. */
    const char *shown = pad.typed;
    lv_point_t size;
    const int32_t room = s->typed ? lv_obj_get_style_width(s->typed, 0) : 0;
    for (;; ++shown) {
        lv_text_get_size(&size, shown, s->digits, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= room || !shown[0]) break;
    }
    door_text(s->typed, shown);
    door_text(s->body, pad.typed[0] ? "" : v->status ? v->status : v->text);
    if (!scene) return;
    char clock[16] = "--:--", date[48] = "正在同步时间";
    if (scene->clock.valid && scene->clock.weekday < 7) {
        (void)snprintf(clock, sizeof(clock), "%02u:%02u", scene->clock.hour, scene->clock.minute);
        (void)snprintf(date, sizeof(date), "%u月%u日 %s", scene->clock.month, scene->clock.day, days[scene->clock.weekday]);
    }
    door_text(s->clock, clock);
    door_text(s->date, date);
}

static void stop_animations(pane_t *s)
{
    (void)lv_anim_delete(s, NULL);
    if (s->dot) (void)lv_anim_delete(s->dot, NULL);
}

static void rebuild(pane_t *s, const xiaotai_view_t *v)
{
    stop_animations(s);
    lv_obj_clean(s->sheet);
    lv_obj_set_style_border_width(s->sheet, 0, 0);
    (void)lv_obj_remove_event_cb_with_user_data(s->sheet, sheet_event, s);
    s->title = s->subtitle = s->body = s->dot = s->shackle = s->gap = s->clock = s->date = s->typed = NULL;
    memset(s->bar, 0, sizeof(s->bar));
    s->n = 0;
    s->width = lv_obj_get_style_width(s->sheet, 0);
    s->height = lv_obj_get_style_height(s->sheet, 0);
    if (s->page == LOOKED) build_looked(s, v);
    else if (s->page == KEYPAD) build_keypad(s, v);
    else if (s->page == BELL) build_bell(s, v);
    else build_opened(s, v);
    s->built = true;
}

static void update(void *state, const xiaotai_ui_canvas_t *canvas, const xiaotai_ui_layer_t *layer, const xiaotai_ui_scene_t *scene)
{
    (void)canvas;
    pane_t *s = state;
    const xiaotai_view_t *v = &layer->view;
    s->updating = true;
    /* What is typed changes the words alone: a key stays the key under the finger. */
    const uint32_t shape = s->page == KEYPAD ? XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ACTIONS | XIAOTAI_CHANGED_ITEMS :
                           XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ACTIONS | XIAOTAI_CHANGED_CUSTOM;
    if (!s->built || (layer->changed & shape)) rebuild(s, v);
    door_text(s->title, fitting_title(s, v->title));
    if (s->page == KEYPAD) update_keypad(s, v, scene);
    else if (s->page == BELL) update_bell(s, v, scene);
    else door_text(s->body, v->text);
    if (s->subtitle) {
        char words[XIAOTAI_SETTING_TEXT_MAX + 32];
        const unsigned sec = layer->timer_s > 0 ? (unsigned)layer->timer_s : 0;
        if (v->timer_mode == XIAOTAI_TIMER_NONE) (void)snprintf(words, sizeof(words), "%s", v->subtitle ? v->subtitle : "");
        else (void)snprintf(words, sizeof(words), "%s  %02u:%02u", v->subtitle ? v->subtitle : "", sec / 60U, sec % 60U);
        door_text(s->subtitle, words);
        if (s->dot) {
            lv_point_t size;
            lv_text_get_size(&size, words, s->name, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            const int room = lv_obj_get_style_width(s->subtitle, 0), taken = size.x < room ? size.x : room;
            const int dot = lv_obj_get_style_width(s->dot, 0);
            lv_obj_set_x(s->dot, lv_obj_get_style_x(s->subtitle, 0) + (room - taken) / 2 - dot - dot / 2);
        }
    }
    s->updating = false;
}

bool door_station_pages_cover(uint16_t width, uint16_t height) { return width >= 240 && height >= 200; }

static int match(const xiaotai_display_desc_t *d)
{
    return d && d->pixel == XIAOTAI_PIXEL_RGB565 && door_station_pages_cover(d->width, d->height) ? 100 : 0;
}

static xiaotai_err_t open(const xiaotai_ui_host_t *host, const xiaotai_ui_canvas_t *c, const xiaotai_ui_layer_t *layer, void **out)
{
    if (!out || !c || !c->root || c->backend != XIAOTAI_UI_BACKEND_LVGL || !layer) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    pane_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    const char *name = layer->view.custom ? layer->view.custom : "";
    s->page = !strcmp(name, XIAOTAI_APP_DOOR_TEMPLATE_OPENED) ? OPENED : !strcmp(name, XIAOTAI_APP_DOOR_TEMPLATE_KEYPAD) ? KEYPAD :
              !strcmp(name, XIAOTAI_APP_DOOR_TEMPLATE_BELL) ? BELL : LOOKED;
    s->host = host;
    s->layer = layer->view.layer;
    s->root = c->root;
    s->full = lv_obj_get_style_text_font(c->root, LV_PART_MAIN);
    s->name = s->full;
    s->digits = &door_font_digits;
    s->heading = s->words = s->caption = *s->full;
#ifdef XIAOTAI_DOOR_WIDE
    s->wide = door_templates_cover(c->width, c->height);
    if (s->wide) {
        s->heading = door_font_title;
        s->words = door_font_body;
        s->caption = door_font_button;
        s->digits = &door_font_clock_small;
        s->words.fallback = s->caption.fallback = s->full;
    } else if (door_templates_home(c->width, c->height)) s->heading = door_font_button;
    if (s->heading.dsc != s->full->dsc) s->heading.fallback = s->full;
#endif
    s->sheet = door_box(c->root, 0, 0, c->width, c->height, GREEN_GROUND, LV_OPA_COVER);
    if (!s->sheet) {
        xiaotai_mem_free(s);
        return XIAOTAI_ERR_NO_MEM;
    }
    lv_obj_add_flag(s->sheet, LV_OBJ_FLAG_CLICKABLE);       /* nothing below it is touched */
    covering += s->page == OPENED;
    *out = s;
    update(s, c, layer, NULL);
    return XIAOTAI_OK;
}

static void close(void *state)
{
    pane_t *s = state;
    s->updating = true;
    stop_animations(s);
    covering -= s->page == OPENED;
    lv_obj_delete(s->sheet);
    xiaotai_mem_free(s);
}

static size_t controls(void *state, xiaotai_ui_control_t *out, size_t capacity)
{
    const pane_t *s = state;
    lv_obj_update_layout(s->root);
    for (size_t i = 0; i < s->n && out && i < capacity; ++i) {
        lv_area_t a;
        lv_obj_get_coords(s->knobs[i].object, &a);
        out[i] = (xiaotai_ui_control_t){.x = (int16_t)a.x1, .y = (int16_t)a.y1, .width = (uint16_t)lv_area_get_width(&a),
            .height = (uint16_t)lv_area_get_height(&a), .layer = s->layer, .element = s->knobs[i].element,
            .index = s->knobs[i].index};
    }
    return s->n;
}

xiaotai_err_t door_station_pages_register(void)
{
#define PAGE(id) {.name = id, .backend = XIAOTAI_UI_BACKEND_LVGL, .match = match, .open = open, .update = update, \
                  .close = close, .controls = controls}
    static const xiaotai_ui_template_t pages[] = {PAGE(XIAOTAI_APP_DOOR_TEMPLATE_LOOKED), PAGE(XIAOTAI_APP_DOOR_TEMPLATE_OPENED),
                                                  PAGE(XIAOTAI_APP_DOOR_TEMPLATE_KEYPAD), PAGE(XIAOTAI_APP_DOOR_TEMPLATE_BELL)};
#undef PAGE
    for (size_t i = 0; i < sizeof(pages) / sizeof(*pages); ++i) {
        const xiaotai_err_t err = xiaotai_ui_template_register(&pages[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}
