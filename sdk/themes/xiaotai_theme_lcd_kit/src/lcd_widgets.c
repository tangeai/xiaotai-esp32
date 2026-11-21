// SPDX-License-Identifier: MIT
#include <string.h>
#include "lcd_internal.h"

LV_FONT_DECLARE(ui_font_cn_18);

const lv_font_t *lcd_body_font(void) { return &ui_font_cn_18; }

lv_obj_t *lcd_panel(lv_obj_t *parent, lcd_box_t box, uint32_t color, lv_opa_t opacity, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lcd_place(obj, box);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opacity, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

lv_obj_t *lcd_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text_static(obj, "");   /* LVGL starts a label as "Text"; one never filled must stay blank */
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

void lcd_text(lv_obj_t *obj, const char *text)
{
    if (!text) text = "";
    if (obj && strcmp(lv_label_get_text(obj), text)) lv_label_set_text(obj, text);
}

void lcd_visible(lv_obj_t *obj, bool visible)
{
    if (!obj) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void control_event(lv_event_t *event)
{
    lcd_control_t *c = lv_event_get_user_data(event);
    xiaotai_ui_input_t input;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_CLICKED: input = XIAOTAI_UI_INPUT_TAP; break;
    case LV_EVENT_PRESSED:
        c->pointer_down = c->pointer_hold;
        input = XIAOTAI_UI_INPUT_PRESS;
        break;
    case LV_EVENT_PRESSING:
        if (!c->pointer_down) return;
        input = XIAOTAI_UI_INPUT_HOLD_REFRESH;
        break;
    case LV_EVENT_DELETE:
        if (!c->pointer_hold && !c->pointer_down) return;
        c->obj = c->label = c->image = c->icon = NULL;
        if (!c->pointer_down) return;
        c->pointer_down = false;
        input = XIAOTAI_UI_INPUT_RELEASE;
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (c->pointer_hold && !c->pointer_down) return;
        c->pointer_down = false;
        input = XIAOTAI_UI_INPUT_RELEASE;
        break;
    case LV_EVENT_LONG_PRESSED: input = XIAOTAI_UI_INPUT_LONG_PRESS; break;
    default: return;
    }
    xiaotai_ui_host_input(c->state->host, c->layer, c->element, c->index, input, 0, NULL);
}

void lcd_bind(lcd_control_t *control, lcd_state_t *state, lv_obj_t *parent,
              uint8_t layer, uint8_t element, uint16_t index)
{
    if (control->obj) return;
    *control = (lcd_control_t){.state = state, .layer = layer, .element = element, .index = index};
    control->obj = lcd_panel(parent, (lcd_box_t){0}, 0, LV_OPA_TRANSP, LCD_RADIUS);
    control->icon = lcd_label(control->obj, LV_FONT_DEFAULT, LCD_TEXT);
    lv_obj_add_flag(control->icon, LV_OBJ_FLAG_HIDDEN);
    control->label = lcd_label(control->obj, lcd_body_font(), LCD_TEXT);
    lv_obj_set_width(control->label, lv_pct(100));
    lv_obj_set_style_text_align(control->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(control->label, LV_LABEL_LONG_DOT);
    lv_obj_center(control->label);
    lv_obj_add_event_cb(control->obj, control_event, LV_EVENT_ALL, control);
}

void lcd_control_update(lcd_control_t *control, lcd_box_t box, const char *label, const char *symbol,
                        bool visible, bool enabled, bool quiet, uint8_t style)
{
    lcd_visible(control->obj, visible);
    if (!visible) return;
    const bool icon_only = symbol && (!label || !label[0]);
    const uint32_t fill = style == XIAOTAI_STYLE_POSITIVE ? LCD_POSITIVE :
                          style == XIAOTAI_STYLE_DESTRUCTIVE ? LCD_DESTRUCTIVE : LCD_SURFACE_HIGH;
    lcd_place(control->obj, box);
    lv_obj_set_style_bg_color(control->obj, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(control->obj, quiet ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(control->obj, quiet ? LV_OPA_20 : LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(control->label, lv_color_hex(enabled ? LCD_TEXT : LCD_MUTED), 0);
    lv_obj_set_style_text_font(control->label, icon_only ? LV_FONT_DEFAULT : lcd_body_font(), 0);
    lcd_text(control->label, icon_only ? symbol : label);
    /* A labelled button with an icon shows both, centred as one group. Rows
     * place their own label and pass no symbol, so they never get here. */
    const bool paired = symbol && !icon_only && !quiet && box.width >= 96;
    lcd_visible(control->icon, paired);
    if (control->icon) {
        lv_obj_set_style_text_color(control->icon, lv_color_hex(enabled ? LCD_TEXT : LCD_MUTED), 0);
        if (paired) {
            lcd_text(control->icon, symbol);
            lv_point_t icon_size, text_size;
            lv_text_get_size(&icon_size, symbol, LV_FONT_DEFAULT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_text_get_size(&text_size, label, lcd_body_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            const int32_t icon_w = icon_size.x, text_w = text_size.x;
            const int32_t gap = 8;
            lv_obj_set_width(control->label, LV_SIZE_CONTENT);
            lv_obj_align(control->icon, LV_ALIGN_CENTER, -(text_w + gap) / 2, 0);
            lv_obj_align(control->label, LV_ALIGN_CENTER, (icon_w + gap) / 2, 0);
        } else if (lv_obj_get_style_width(control->label, 0) == LV_SIZE_CONTENT &&
                   lv_obj_get_style_align(control->label, 0) == LV_ALIGN_CENTER) {
            lv_obj_set_width(control->label, lv_pct(100));
            lv_obj_center(control->label);
        }
    }
    if (enabled) {
        lv_obj_remove_state(control->obj, LV_STATE_DISABLED);
        lv_obj_add_flag(control->obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(control->obj, LV_STATE_DISABLED);
        lv_obj_remove_flag(control->obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

const char *lcd_icon(const char *name)
{
    static const struct { const char *name, *glyph; } icons[] = {
        {XIAOTAI_ICON_BACK, LV_SYMBOL_LEFT}, {XIAOTAI_ICON_PREVIOUS, LV_SYMBOL_LEFT},
        {XIAOTAI_ICON_NEXT, LV_SYMBOL_RIGHT}, {XIAOTAI_ICON_MENU, LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET},
        {XIAOTAI_ICON_PHONE, LV_SYMBOL_CALL}, {XIAOTAI_ICON_ACCEPT, LV_SYMBOL_OK},
        {XIAOTAI_ICON_HANGUP, LV_SYMBOL_CLOSE}, {XIAOTAI_ICON_STOP, LV_SYMBOL_STOP},
        {XIAOTAI_ICON_WIFI, LV_SYMBOL_WIFI}, {XIAOTAI_ICON_VOLUME, LV_SYMBOL_VOLUME_MAX},
        {XIAOTAI_ICON_MIC, LV_SYMBOL_AUDIO}, {XIAOTAI_ICON_MIC_OFF, LV_SYMBOL_MUTE},
        {XIAOTAI_ICON_CAMERA, LV_SYMBOL_IMAGE}, {XIAOTAI_ICON_CAMERA_OFF, LV_SYMBOL_CLOSE},
        {XIAOTAI_ICON_SETTINGS, LV_SYMBOL_SETTINGS}, {XIAOTAI_ICON_REFRESH, LV_SYMBOL_REFRESH},
        {XIAOTAI_ICON_VIDEO, LV_SYMBOL_VIDEO},
    };
    for (size_t i = 0; name && i < sizeof(icons) / sizeof(icons[0]); ++i)
        if (!strcmp(name, icons[i].name)) return icons[i].glyph;
    return NULL;
}

bool lcd_measure_control(const lcd_control_t *control, xiaotai_ui_control_t *out)
{
    if (!control->obj || !lv_obj_is_visible(control->obj)) return false;
    if (out) {
        lv_area_t area;
        lv_obj_get_coords(control->obj, &area);
        *out = (xiaotai_ui_control_t){.x = area.x1, .y = area.y1, .width = lv_area_get_width(&area),
            .height = lv_area_get_height(&area), .layer = control->layer, .element = control->element,
            .index = control->index};
    }
    return true;
}
