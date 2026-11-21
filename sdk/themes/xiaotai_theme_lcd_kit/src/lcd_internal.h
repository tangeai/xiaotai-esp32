// SPDX-License-Identifier: MIT
#pragma once

#include "lcd_layout.h"
#include "xiaotai/theme_lcd.h"

/* One palette for every colour page: a deep background, two raised surfaces,
 * the brand sky blue as accent, and green/red only for accept and end. */
enum {
    LCD_BACKGROUND = 0x0f1316, LCD_SURFACE = 0x1c232a, LCD_SURFACE_HIGH = 0x28323b,
    LCD_TEXT = 0xf2f5f7, LCD_MUTED = 0x98a3ad, LCD_ACCENT = 0x5ec8f2,
    LCD_POSITIVE = 0x1f9e6e, LCD_DESTRUCTIVE = 0xd8434d,
    LCD_CAPTION_AI = 0xb5dfff, LCD_CAPTION_USER = 0xbce8cf,
};
enum { LCD_RADIUS = 10 };

typedef struct lcd_state lcd_state_t;
typedef struct {
    lcd_state_t *state;
    lv_obj_t *obj, *label, *image, *icon;
    uint16_t index;
    uint8_t layer, element;
    bool pointer_hold, pointer_down, normal_press_lock;
} lcd_control_t;

typedef struct {
    lv_obj_t *root, *content, *title, *status, *subtitle, *caption_panel, *body, *face, *clock, *date, *background;
    lcd_control_t action[XIAOTAI_VIEW_ACTIONS_MAX], indicator[XIAOTAI_VIEW_INDICATORS_MAX];
    const xiaotai_ui_template_t *custom;
    void *custom_state;
    struct lcd_pages *pages;
    struct lcd_form *form;
    xiaotai_ui_canvas_t canvas;
    uint8_t speaker;
    bool home;
} lcd_layer_t;

struct lcd_state {
    const xiaotai_ui_host_t *host;
    const xiaotai_theme_lcd_variant_t *variant;
    struct lcd_face_live *emotion;
    lcd_layout_t layout;
    lv_obj_t *root, *video;
    lcd_layer_t layers[3];
    bool template_toast, video_bound, asleep;
};

lv_obj_t *lcd_panel(lv_obj_t *parent, lcd_box_t box, uint32_t color, lv_opa_t opacity, int32_t radius);
lv_obj_t *lcd_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);
const lv_font_t *lcd_body_font(void);
void lcd_text(lv_obj_t *obj, const char *text);
void lcd_visible(lv_obj_t *obj, bool visible);
static inline bool lcd_face_preview(const xiaotai_view_t *view)
{
    return view->kind == XIAOTAI_VIEW_TALK && (view->flags & XIAOTAI_VIEW_EMOTION_PREVIEW);
}
void lcd_bind(lcd_control_t *control, lcd_state_t *state, lv_obj_t *parent,
              uint8_t layer, uint8_t element, uint16_t index);
void lcd_control_update(lcd_control_t *control, lcd_box_t box, const char *label, const char *symbol,
                        bool visible, bool enabled, bool quiet, uint8_t style);
const char *lcd_icon(const char *name);
bool lcd_measure_control(const lcd_control_t *control, xiaotai_ui_control_t *out);
void lcd_home_build(lcd_state_t *state, lcd_layer_t *out, const xiaotai_view_t *view, uint8_t layer);
void lcd_home_update(lcd_state_t *state, lcd_layer_t *out, const xiaotai_ui_layer_t *layer,
                     const xiaotai_ui_scene_t *scene);
void lcd_simple_actions(lcd_state_t *state, lcd_layer_t *out, const xiaotai_view_t *view,
                        uint8_t layer, int32_t height);
