// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_layout.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai/theme_lcd_config.h"
#include "../assets/lcd_icons.h"

/* One palette for every colour page: a deep background, two raised surfaces
 * and a hairline, three text weights, the brand sky blue as the one accent,
 * and green, red and amber only for accept, end and warn. The dark tones are
 * exact RGB565 values: rounding would tint them green. */
enum {
    LCD_BACKGROUND = 0x101418, LCD_SURFACE = 0x182029, LCD_SURFACE_HIGH = 0x293039, LCD_OUTLINE = 0x394552,
    LCD_TEXT = 0xf2f5f7, LCD_MUTED = 0x98a3ad, LCD_FAINT = 0x66727d,
    LCD_ACCENT = 0x5ec8f2, LCD_ACCENT_DIM = 0x18394a, LCD_ACCENT_INK = 0x062330,
    LCD_POSITIVE = 0x1fa672, LCD_DESTRUCTIVE = 0xdc434d, LCD_WARNING = 0xf2b950,
    LCD_CAPTION_AI = 0xb5dfff, LCD_CAPTION_USER = 0xbce8cf,
};
/* Corner radii in design pixels (lcd_dp): rows and buttons, cards and dialogs. */
enum { LCD_RADIUS = 10, LCD_RADIUS_CARD = 16 };

/* How a button is filled. Colour carries meaning: the accent marks the one
 * main action of a page, green answers or places a call, red ends one. */
typedef enum {
    LCD_TONE_NEUTRAL = 0,   /* raised surface */
    LCD_TONE_ACCENT,        /* the page's main action */
    LCD_TONE_POSITIVE,
    LCD_TONE_DESTRUCTIVE,
    LCD_TONE_QUIET,         /* no fill until pressed */
} lcd_tone_t;

/* What a display class draws text with. A design pixel (dp) is one pixel on
 * the 18 px text face and grows with the face, so paddings keep their
 * proportion to the text. */
typedef struct {
    const lv_font_t *text;      /* body: every string an application can show */
    const lv_font_t *icons;
    const lv_font_t *number;    /* clock, timer and code digits */
    const lv_font_t *clock;     /* the time on the desk clock */
    int16_t dp;                 /* sixteenths: 16 is 1:1 */
} lcd_metrics_t;

typedef struct lcd_state lcd_state_t;
typedef struct {
    lcd_state_t *state;
    lv_obj_t *obj, *label, *image, *icon, *badge;
    uint16_t index;
    uint8_t layer, element;
    bool pointer_hold, pointer_down, normal_press_lock;
} lcd_control_t;

typedef struct {
    lv_obj_t *root, *content, *title, *status, *subtitle, *caption_panel, *body, *face, *clock, *date, *mark, *background;
    lcd_control_t action[XIAOTAI_VIEW_ACTIONS_MAX], indicator[XIAOTAI_VIEW_INDICATORS_MAX];
    const xiaotai_ui_template_t *custom;
    void *custom_state;
    struct lcd_pages *pages;
    struct lcd_form *form;
    struct lcd_clock *desk;     /* CLOCK with the calendar */
    xiaotai_ui_canvas_t canvas;
    /* The size 320 x 240 logical units map to on this layer: the page area
     * for a page, and the same for an overlay, whose panel is a part of it. */
    lcd_layout_t unit;
    uint8_t speaker;
    bool home, calendar;
} lcd_layer_t;

/* A value that moves over a short time, stepped from the theme's tick: LVGL's
 * own animations need its timer handler, which this UI system does not run. */
typedef enum { LCD_MOVE_NONE = 0, LCD_MOVE_ENTER, LCD_MOVE_WIDTH, LCD_MOVE_SPIN, LCD_MOVE_PULSE } lcd_move_kind_t;
typedef struct {
    lv_obj_t *obj;
    uint32_t start_ms;
    int32_t from, to;
    uint16_t duration_ms;
    uint8_t kind;
    bool started;
} lcd_move_t;
enum { LCD_MOVES = 6 };

struct lcd_state {
    const xiaotai_ui_host_t *host;
    const xiaotai_theme_lcd_variant_t *variant;
    struct lcd_face_live *emotion;
    lcd_layout_t layout;
    lcd_metrics_t m;
    lv_obj_t *root, *video;
    lcd_layer_t layers[3];
    lcd_move_t moves[LCD_MOVES];
    char page_id[32];
    bool template_toast, video_bound, asleep;
};

/* Widgets (lcd_widgets.c). */
lv_obj_t *lcd_panel(lv_obj_t *parent, lcd_box_t box, uint32_t color, lv_opa_t opacity, int32_t radius);
lv_obj_t *lcd_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);
void lcd_text(lv_obj_t *obj, const char *text);
/* Running text for a label `width` pixels wide: no line begins with a closing
 * mark (，。？”) or ends with an opening one, which LVGL's breaking allows. */
void lcd_prose(lcd_state_t *s, lv_obj_t *obj, const char *text, int32_t width);
void lcd_visible(lv_obj_t *obj, bool visible);
int32_t lcd_dp(const lcd_state_t *s, int32_t pixels);
int32_t lcd_line_height(const lcd_state_t *s);
/* A row or card that answers a press: SURFACE, a shade lighter while pressed. */
void lcd_surface_style(lcd_state_t *s, lv_obj_t *obj, bool pressable);
/* The scroll bar of a list: a thin faint thumb. */
void lcd_scroll_style(lcd_state_t *s, lv_obj_t *obj);
static inline bool lcd_face_preview(const xiaotai_view_t *view)
{
    return view->kind == XIAOTAI_VIEW_TALK && (view->flags & XIAOTAI_VIEW_EMOTION_PREVIEW);
}
void lcd_bind(lcd_control_t *control, lcd_state_t *state, lv_obj_t *parent,
              uint8_t layer, uint8_t element, uint16_t index);
/* Draw a button: `symbol` is an LCD_ICON_* string or NULL. */
void lcd_control_update(lcd_control_t *control, lcd_box_t box, const char *label, const char *symbol,
                        bool visible, bool enabled, lcd_tone_t tone);
/* A round icon button with its label underneath (calls). `box` is the whole
 * touch area; the circle is `diameter` wide at its top centre. */
void lcd_round_update(lcd_control_t *control, lcd_box_t box, int32_t diameter, const char *label,
                      const char *symbol, bool enabled, lcd_tone_t tone);
lcd_tone_t lcd_action_tone(const xiaotai_view_action_t *action);
const char *lcd_icon(const char *name);
bool lcd_measure_control(const lcd_control_t *control, xiaotai_ui_control_t *out);

/* The size of a layer's canvas in the units its boxes are written in. */
lcd_layout_t lcd_canvas_units(const lcd_layer_t *layer);

/* The units an ordinary page is laid out in, and where it lies on the screen:
 * the whole screen, or a column of readable width on a large one. */
lcd_layout_t lcd_page_unit(const lcd_state_t *s);
lcd_box_t lcd_page_area(const lcd_state_t *s);

/* Motion (lcd_motion.c). */
void lcd_move_enter(lcd_state_t *s, lv_obj_t *obj, uint32_t now_ms);
void lcd_move_width(lcd_state_t *s, lv_obj_t *obj, int32_t to, uint32_t now_ms);
void lcd_move_loop(lcd_state_t *s, lv_obj_t *obj, lcd_move_kind_t kind, uint16_t period_ms);
void lcd_move_drop(lcd_state_t *s, const lv_obj_t *obj);
/* Put everything that is settling where it ends up. */
void lcd_move_settle(lcd_state_t *s);
uint32_t lcd_move_tick(lcd_state_t *s, uint32_t now_ms);

/* A CLOCK that shows the calendar with the time. */
bool lcd_home_desk(const xiaotai_view_t *view);
void lcd_home_build(lcd_state_t *state, lcd_layer_t *out, const xiaotai_view_t *view, uint8_t layer);
void lcd_home_update(lcd_state_t *state, lcd_layer_t *out, const xiaotai_ui_layer_t *layer,
                     const xiaotai_ui_scene_t *scene);
/* Footer buttons of a simple layout; returns the logical y of their top edge
 * (the layer's logical height when there are none). */
int32_t lcd_simple_actions(lcd_state_t *state, lcd_layer_t *out, const xiaotai_view_t *view,
                           uint8_t layer, int32_t height);
