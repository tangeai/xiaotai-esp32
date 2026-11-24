// SPDX-License-Identifier: MIT
#include "lcd_form.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/port/mem.h"

/* Logical units of the 320 x 240 design canvas. */
enum {
    PAGE_W = 320, PAGE_H = 240, MARGIN = 12, HEADER_H = 40, FORM_ROW = 48, FORM_GAP = 4,
    FOOT_H = 38, FOOT_GAP = 6, FOOT_BOTTOM = 8, DIALOG_W = 272, DIALOG_MAX = 224,
    KEY_COUNT = 12, KEY_BACKSPACE = 10, KEY_CONFIRM = 11,
};
#define INACTIVE UINT8_MAX

typedef struct lcd_form lcd_form_t;

typedef struct {
    lcd_form_t *form;
    lv_obj_t *row, *name, *widget, *value_text, *minus, *plus;
    uint8_t index, kind;
    uint8_t flags, digits;
    int32_t value, min, max, step;
    char authoritative[XIAOTAI_VIEW_DIGITS_MAX + 1U];
    char submitted[XIAOTAI_VIEW_DIGITS_MAX + 1U];
    bool editable, active, dragging;
} field_t;

typedef struct {
    lcd_form_t *form;
    lv_obj_t *object, *label;
    uint8_t number, pressed_element, pressed_index;
    bool pressed;
} keypad_key_t;

struct lcd_form {
    lcd_state_t *state;
    lcd_layer_t *layer;
    lv_obj_t *scroll, *note;
    field_t fields[XIAOTAI_VIEW_FIELDS_MAX];
    lcd_control_t items[XIAOTAI_VIEW_ITEMS_MAX];
    lv_obj_t *chevrons[XIAOTAI_VIEW_ITEMS_MAX];
    keypad_key_t keys[KEY_COUNT];
    int w, h;                    /* logical size of the canvas: the page, or the dialog panel */
    uint8_t index, selected, primary;
    bool applying, show_keypad, can_submit, primary_allowed, overlay;
};

static const char *const key_text[KEY_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LCD_ICON_BACKSPACE, "确认",
};

static const char *text(const char *s) { return s ? s : ""; }
static int min_int(int a, int b) { return a < b ? a : b; }
static bool present_text(const char *s) { return s && *s; }
static bool unrestricted(uint8_t state)
{
    return !(state & (XIAOTAI_STATE_BUSY | XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_HIDDEN));
}

static lcd_box_t layout(const lcd_form_t *form, lcd_box_t box)
{
    return lcd_scale(&form->layer->unit, box);
}

static void position(const lcd_form_t *form, lv_obj_t *obj, lcd_box_t box)
{
    lcd_place(obj, layout(form, box));
}

static void enabled(lv_obj_t *obj, bool on)
{
    if (on) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void report(const field_t *field, xiaotai_ui_input_t input, int32_t value, const char *digits)
{
    const lcd_form_t *form = field->form;
    xiaotai_ui_host_input(form->state->host, form->index, XIAOTAI_UI_ELEMENT_FIELD,
                          field->index, input, value, digits);
}

static int32_t clamp_value(int64_t value, int32_t minimum, int32_t maximum)
{
    return value < minimum ? minimum : value > maximum ? maximum : (int32_t)value;
}

static void slider_restore(field_t *field)
{
    lcd_form_t *form = field->form;
    form->applying = true;
    lv_slider_set_value(field->widget, field->value, LV_ANIM_OFF);
    form->applying = false;
}

static bool choice_list_selected(lv_event_t *event, lv_obj_t *widget)
{
    /* LVGL emits the dropdown event inside the input's list release, after
     * closing the list. A widget release or programmatic update is different. */
    lv_indev_t *input = lv_indev_active();
    if (!input || lv_indev_get_active_obj() != lv_dropdown_get_list(widget) || !lv_event_get_param(event))
        return false;
    const lv_indev_type_t type = lv_indev_get_type(input);
    return type == LV_INDEV_TYPE_POINTER || type == LV_INDEV_TYPE_BUTTON;
}

static void field_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_PRESSED &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_CLICKED) return;
    field_t *field = lv_event_get_user_data(event);
    lcd_form_t *form = field->form;
    if (form->applying || !field->active || !field->editable) return;
    lv_obj_t *widget = lv_event_get_current_target_obj(event);
    if (code == LV_EVENT_PRESSED) {
        if (field->kind == XIAOTAI_FIELD_SLIDER) field->dragging = true;
        report(field, XIAOTAI_UI_INPUT_PRESS, 0, NULL);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (field->kind == XIAOTAI_FIELD_SLIDER && field->dragging) {
            field->dragging = false;
            if (code == LV_EVENT_RELEASED)
                report(field, XIAOTAI_UI_INPUT_COMMIT, lv_slider_get_value(widget), NULL);
            slider_restore(field);
        }
        report(field, XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
        return;
    }
    if (code == LV_EVENT_CLICKED && field->kind == XIAOTAI_FIELD_STEPPER) {
        const int64_t delta = widget == field->minus ? -(int64_t)field->step : field->step;
        const int32_t next = clamp_value((int64_t)field->value + delta, field->min, field->max);
        if (next != field->value) report(field, XIAOTAI_UI_INPUT_COMMIT, next, NULL);
    } else if (code == LV_EVENT_CLICKED && field->kind == XIAOTAI_FIELD_DIGITS) {
        if (form->selected != INACTIVE) lv_obj_remove_state(form->fields[form->selected].widget, LV_STATE_FOCUSED);
        form->selected = field->index;
        lv_obj_add_state(widget, LV_STATE_FOCUSED);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (field->kind == XIAOTAI_FIELD_SWITCH) {
            const bool next = lv_obj_has_state(widget, LV_STATE_CHECKED);
            form->applying = true;
            lv_obj_set_state(widget, LV_STATE_CHECKED, field->value != 0);
            form->applying = false;
            if (next != (field->value != 0)) report(field, XIAOTAI_UI_INPUT_COMMIT, next, NULL);
        } else if (field->kind == XIAOTAI_FIELD_CHOICE) {
            const uint32_t next = lv_dropdown_get_selected(widget);
            form->applying = true;
            lv_dropdown_set_selected(widget, (uint32_t)field->value);
            form->applying = false;
            if (next != (uint32_t)field->value) {
                report(field, XIAOTAI_UI_INPUT_COMMIT, (int32_t)next, NULL);
            } else if ((field->flags & XIAOTAI_FIELD_CONFIRM_CURRENT) && choice_list_selected(event, widget)) {
                report(field, XIAOTAI_UI_INPUT_CONFIRM_CHOICE, (int32_t)next, NULL);
            }
        } else if (field->kind == XIAOTAI_FIELD_SLIDER && field->dragging &&
                   (field->flags & XIAOTAI_FIELD_LIVE)) {
            report(field, XIAOTAI_UI_INPUT_CHANGE, lv_slider_get_value(widget), NULL);
        }
    }
}

/* A small button of the form itself: a stepper's ends, a key of the keypad. */
static lv_obj_t *button(lcd_form_t *form, lv_obj_t *parent, const char *title, bool symbol, bool primary,
                        lv_obj_t **label_out)
{
    lcd_state_t *s = form->state;
    lv_obj_t *obj = lcd_panel(parent, (lcd_box_t){0}, primary ? LCD_ACCENT : LCD_SURFACE_HIGH, LV_OPA_COVER,
                              lcd_dp(s, LCD_RADIUS));
    lv_obj_set_style_bg_color(obj, lv_color_hex(primary ? 0x46a6cc : LCD_OUTLINE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = lcd_label(obj, symbol ? s->m.icons : s->m.text, primary ? LCD_ACCENT_INK : LCD_TEXT);
    lv_obj_set_style_text_color(label, lv_color_hex(LCD_FAINT), LV_STATE_DISABLED);
    lcd_text(label, title);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return obj;
}

static bool exact_digits(const char *digits, unsigned count)
{
    return count && strlen(digits) == count && strspn(digits, "0123456789") == count;
}

static bool digits_admitted(const field_t *field, const char *digits)
{
    return field->digits &&
        (((field->flags & XIAOTAI_FIELD_ALLOW_EMPTY) && !*digits) || exact_digits(digits, field->digits));
}

static void sync_keypad(lcd_form_t *form)
{
    bool submit = form->primary != INACTIVE && form->primary_allowed;
    for (size_t i = 0; i < XIAOTAI_VIEW_FIELDS_MAX; ++i) {
        const field_t *field = &form->fields[i];
        if (field->active && field->kind == XIAOTAI_FIELD_DIGITS)
            submit &= digits_admitted(field, lv_textarea_get_text(field->widget));
    }
    form->can_submit = submit;
    for (size_t i = 0; i < KEY_COUNT; ++i) {
        if (!form->keys[i].object) continue;
        lcd_visible(form->keys[i].object, form->show_keypad);
        const bool on = i == KEY_CONFIRM ? submit : form->selected != INACTIVE;
        enabled(form->keys[i].object, on);
        lv_obj_set_state(form->keys[i].label, LV_STATE_DISABLED, !on);
    }
}

static void accept_digits(field_t *field)
{
    const char *digits = lv_textarea_get_text(field->widget);
    if (digits_admitted(field, digits) && strcmp(field->submitted, digits)) {
        report(field, XIAOTAI_UI_INPUT_COMMIT, 0, digits);
        memcpy(field->submitted, digits, strlen(digits) + 1U);
    }
}

static void keypad_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST && code != LV_EVENT_CLICKED && code != LV_EVENT_DELETE) return;
    keypad_key_t *key = lv_event_get_user_data(event);
    lcd_form_t *form = key->form;
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) {
        if (key->pressed) {
            key->pressed = false;
            xiaotai_ui_host_input(form->state->host, form->index, key->pressed_element, key->pressed_index,
                                  XIAOTAI_UI_INPUT_RELEASE, 0, NULL);
        }
        return;
    }
    if (!form->show_keypad) return;
    if (code == LV_EVENT_PRESSED) {
        key->pressed_element = key->number == KEY_CONFIRM ? XIAOTAI_UI_ELEMENT_ACTION : XIAOTAI_UI_ELEMENT_FIELD;
        key->pressed_index = key->number == KEY_CONFIRM ? form->primary : form->selected;
        if (key->pressed_index == INACTIVE) return;
        key->pressed = true;
        xiaotai_ui_host_input(form->state->host, form->index, key->pressed_element, key->pressed_index,
                              XIAOTAI_UI_INPUT_PRESS, 0, NULL);
        return;
    }
    if (key->number == KEY_CONFIRM) {
        if (form->can_submit)
            xiaotai_ui_host_input(form->state->host, form->index, XIAOTAI_UI_ELEMENT_ACTION, form->primary,
                                  XIAOTAI_UI_INPUT_TAP, 0, NULL);
        return;
    }
    if (form->selected == INACTIVE) return;
    field_t *field = &form->fields[form->selected];
    if (!field->editable) return;
    if (key->number == KEY_BACKSPACE) lv_textarea_delete_char(field->widget);
    else lv_textarea_add_char(field->widget, (uint32_t)key_text[key->number][0]);
    accept_digits(field);
    sync_keypad(form);
}

static void create_keypad(lcd_form_t *form)
{
    if (form->keys[0].object) return;
    for (uint8_t i = 0; i < KEY_COUNT; ++i) {
        keypad_key_t *key = &form->keys[i];
        key->form = form;
        key->number = i;
        key->object = button(form, form->layer->content, key_text[i], i == KEY_BACKSPACE, i == KEY_CONFIRM, &key->label);
        const int stride = (form->w - 2 * MARGIN + 4) / 4, row = min_int(60, (form->h - 86 - FOOT_BOTTOM + 4) / 3);
        position(form, key->object, (lcd_box_t){MARGIN + (int)(i % 4U) * stride, 86 + (int)(i / 4U) * row,
                                                stride - 4, row - 4});
        lv_obj_add_event_cb(key->object, keypad_event, LV_EVENT_ALL, key);
    }
}

static void style_switch(lcd_state_t *s, lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_OUTLINE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(obj, -lcd_dp(s, 3), LV_PART_KNOB);
    lv_obj_set_style_opa(obj, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_anim_duration(obj, 0, 0);
}

static void style_slider(lcd_state_t *s, lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_OUTLINE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(obj, lcd_dp(s, 6), LV_PART_KNOB);
    lv_obj_set_style_opa(obj, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_anim_duration(obj, 0, 0);
}

static void style_choice(lcd_state_t *s, lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(obj, lcd_dp(s, 8), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_SURFACE_HIGH), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_OUTLINE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(obj, s->m.text, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(LCD_TEXT), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(LCD_FAINT), LV_STATE_DISABLED);
    /* LVGL centres the text of a list without a symbol; the field draws the
     * chosen option itself, beside the arrow (update_field). */
    lv_obj_set_style_text_opa(obj, LV_OPA_TRANSP, 0);
    lv_dropdown_set_symbol(obj, NULL);
    lv_obj_t *arrow = lcd_label(obj, s->m.icons, LCD_MUTED);
    lv_obj_set_style_text_opa(arrow, LV_OPA_COVER, 0);
    lcd_text(arrow, LCD_ICON_DOWN);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -lcd_dp(s, 8), 0);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);

    /* The open list: a raised card, the current option in the accent. */
    lv_obj_t *list = lv_dropdown_get_list(obj);
    lv_obj_remove_style_all(list);
    lv_obj_set_style_radius(list, lcd_dp(s, LCD_RADIUS), 0);
    lv_obj_set_style_clip_corner(list, true, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(LCD_SURFACE_HIGH), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(LCD_OUTLINE), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_text_font(list, s->m.text, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(LCD_TEXT), 0);
    lv_obj_set_style_text_line_space(list, lcd_dp(s, 14), 0);
    lv_obj_set_style_pad_hor(list, lcd_dp(s, 12), 0);
    lv_obj_set_style_pad_ver(list, lcd_dp(s, 9), 0);
    lv_obj_set_style_max_height(list, s->layout.height * 3 / 5, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(LCD_ACCENT_DIM), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, lv_color_hex(LCD_ACCENT), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(list, lv_color_hex(LCD_OUTLINE), LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_PRESSED);
    lcd_scroll_style(s, list);
}

static void style_digits(lcd_state_t *s, lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(obj, lcd_dp(s, LCD_RADIUS), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(LCD_SURFACE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(LCD_OUTLINE), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(LCD_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, lcd_dp(s, 1), 0);
    lv_obj_set_style_border_width(obj, lcd_dp(s, 2), LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(obj, s->m.text, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(LCD_TEXT), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(LCD_FAINT), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_pad_hor(obj, lcd_dp(s, 10), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(LCD_ACCENT), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, lcd_dp(s, 2), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(obj, 0, LV_PART_CURSOR);
}

static void create_field(lcd_form_t *form, uint8_t index, uint8_t kind)
{
    lcd_state_t *s = form->state;
    field_t *field = &form->fields[index];
    if (field->row) lv_obj_delete(field->row);
    *field = (field_t){.form = form, .index = index, .kind = kind};
    field->row = lcd_panel(form->scroll, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
    lcd_surface_style(s, field->row, false);
    field->name = lcd_label(field->row, s->m.text, kind == XIAOTAI_FIELD_TEXT ? LCD_MUTED : LCD_TEXT);
    lv_label_set_long_mode(field->name, LV_LABEL_LONG_DOT);
    switch (kind) {
    case XIAOTAI_FIELD_SWITCH:
        field->widget = lv_switch_create(field->row);
        style_switch(s, field->widget);
        break;
    case XIAOTAI_FIELD_SLIDER:
        field->widget = lv_slider_create(field->row);
        style_slider(s, field->widget);
        break;
    case XIAOTAI_FIELD_CHOICE:
        field->widget = lv_dropdown_create(field->row);
        style_choice(s, field->widget);
        field->value_text = lcd_label(field->widget, s->m.text, LCD_TEXT);
        lv_obj_set_style_text_opa(field->value_text, LV_OPA_COVER, 0);
        lv_obj_set_style_text_align(field->value_text, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(field->value_text, LV_LABEL_LONG_DOT);
        lv_obj_remove_flag(field->value_text, LV_OBJ_FLAG_CLICKABLE);
        break;
    case XIAOTAI_FIELD_DIGITS:
        field->widget = lv_textarea_create(field->row);
        lv_textarea_set_one_line(field->widget, true);
        lv_textarea_set_accepted_chars(field->widget, "0123456789");
        style_digits(s, field->widget);
        break;
    case XIAOTAI_FIELD_STEPPER:
        field->minus = button(form, field->row, LCD_ICON_MINUS, true, false, NULL);
        field->plus = button(form, field->row, LCD_ICON_PLUS, true, false, NULL);
        lv_obj_add_event_cb(field->minus, field_event, LV_EVENT_ALL, field);
        lv_obj_add_event_cb(field->plus, field_event, LV_EVENT_ALL, field);
        break;
    default: break;
    }
    if (field->widget) lv_obj_add_event_cb(field->widget, field_event, LV_EVENT_ALL, field);
    if (kind == XIAOTAI_FIELD_TEXT || kind == XIAOTAI_FIELD_STEPPER || kind == XIAOTAI_FIELD_SLIDER) {
        field->value_text = lcd_label(field->row, s->m.text, kind == XIAOTAI_FIELD_TEXT ? LCD_TEXT : LCD_MUTED);
        lv_label_set_long_mode(field->value_text, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(field->value_text,
                                    kind == XIAOTAI_FIELD_STEPPER ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static bool same_options(const char *existing, const xiaotai_view_field_t *view)
{
    if (!existing) return false;
    for (size_t i = 0; i < view->option_count; ++i) {
        const char *at = text(view->options[i]);
        for (; *at; ++at) {
            const char c = *at == '\n' || *at == '\r' ? ' ' : *at;
            if (*existing != c) return false;
            ++existing;
        }
        if (i + 1U != view->option_count && *existing++ != '\n') return false;
    }
    return !*existing;
}

static bool choice_options(lv_obj_t *widget, const xiaotai_view_field_t *view)
{
    if (!view->options || !view->option_count) return false;
    if (same_options(lv_dropdown_get_options(widget), view)) return true;
    size_t bytes = view->option_count;
    for (size_t i = 0; i < view->option_count; ++i) bytes += strlen(text(view->options[i]));
    char *options = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
    if (!options) return false;
    char *out = options;
    for (size_t i = 0; i < view->option_count; ++i) {
        for (const char *at = text(view->options[i]); *at; ++at)
            *out++ = *at == '\n' || *at == '\r' ? ' ' : *at;
        if (i + 1 != view->option_count) *out++ = '\n';
    }
    *out = '\0';
    lv_dropdown_set_options(widget, options);
    xiaotai_mem_free(options);
    return true;
}

static void numeric_label(field_t *field, const xiaotai_view_field_t *view)
{
    char value[48];
    /* "70%", "12 dB", "5 分钟": a word keeps its space, a sign does not. */
    const char *unit = text(view->text);
    const bool spaced = unit[0] && unit[0] != '%';
    (void)snprintf(value, sizeof(value), "%ld%s%s", (long)field->value, spaced ? " " : "", unit);
    lcd_text(field->value_text, value);
}

/* One line of the body face, centred in a row `row_h` display pixels tall. */
static void row_text(lcd_form_t *form, lv_obj_t *obj, int32_t x, int32_t width, int32_t row_h)
{
    const int32_t h = lcd_line_height(form->state);
    lcd_place(obj, (lcd_box_t){x, (row_h - h) / 2, width > 1 ? width : 1, h});
}

static void update_field(field_t *field, const xiaotai_view_field_t *view, lcd_box_t row, bool compact)
{
    lcd_form_t *form = field->form;
    lcd_state_t *s = form->state;
    field->active = true;
    field->editable = unrestricted(view->state) && present_text(view->intent);
    if (!field->editable) field->dragging = false;
    field->flags = view->flags;
    field->min = view->min;
    field->max = view->max >= view->min ? view->max : view->min;
    field->value = clamp_value(view->value, field->min, field->max);
    field->step = view->step > 0 ? view->step : 1;
    lcd_visible(field->row, true);
    lcd_text(field->name, view->label);
    const int32_t pad = lcd_dp(s, 12), gap = lcd_dp(s, 8), w = row.width, h = row.height;
    lv_obj_set_style_bg_opa(field->row, compact ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(field->name, lv_color_hex(!unrestricted(view->state) ? LCD_FAINT :
        field->kind == XIAOTAI_FIELD_TEXT ? LCD_MUTED : LCD_TEXT), 0);
    row_text(form, field->name, pad, w * 2 / 5 - pad, h);
    switch (field->kind) {
    case XIAOTAI_FIELD_TEXT:
        lcd_text(field->value_text, view->text);
        row_text(form, field->value_text, w * 2 / 5 + gap, w - w * 2 / 5 - gap - pad, h);
        break;
    case XIAOTAI_FIELD_SWITCH: {
        const int32_t sw = lcd_dp(s, 48), sh = lcd_dp(s, 28);
        row_text(form, field->name, pad, w - sw - 2 * pad - gap, h);
        lcd_place(field->widget, (lcd_box_t){w - pad - sw, (h - sh) / 2, sw, sh});
        lv_obj_set_ext_click_area(field->widget, lcd_dp(s, 10));
        lv_obj_set_state(field->widget, LV_STATE_CHECKED, view->value != 0);
        field->value = view->value != 0;
        break;
    }
    case XIAOTAI_FIELD_SLIDER: {
        const int32_t value_w = lcd_dp(s, 50), track = lcd_dp(s, 6), knob = lcd_dp(s, 9);
        const int32_t x = w * 2 / 5 + knob;
        lcd_place(field->widget, (lcd_box_t){x, (h - track) / 2, w - x - pad - value_w - gap - knob, track});
        row_text(form, field->value_text, w - pad - value_w, value_w, h);
        lv_obj_set_ext_click_area(field->widget, lcd_dp(s, 14));
        lv_slider_set_range(field->widget, field->min, field->max);
        if (!field->dragging || !field->editable) lv_slider_set_value(field->widget, field->value, LV_ANIM_OFF);
        numeric_label(field, view);
        break;
    }
    case XIAOTAI_FIELD_STEPPER: {
        /* label ........ [-] value [+] */
        const int32_t side = h - lcd_dp(s, 10), value_w = lcd_dp(s, 66), edge = lcd_dp(s, 5);
        const int32_t plus = w - edge - side, minus = plus - value_w - side;
        lcd_place(field->minus, (lcd_box_t){minus, (h - side) / 2, side, side});
        lcd_place(field->plus, (lcd_box_t){plus, (h - side) / 2, side, side});
        row_text(form, field->value_text, minus + side, value_w, h);
        row_text(form, field->name, pad, minus - pad - gap, h);
        numeric_label(field, view);
        const bool down = field->editable && field->value > field->min, up = field->editable && field->value < field->max;
        enabled(field->minus, down);
        enabled(field->plus, up);
        lv_obj_set_state(lv_obj_get_child(field->minus, 0), LV_STATE_DISABLED, !down);
        lv_obj_set_state(lv_obj_get_child(field->plus, 0), LV_STATE_DISABLED, !up);
        break;
    }
    case XIAOTAI_FIELD_CHOICE: {
        const int32_t edge = lcd_dp(s, 5), cw = w * 23 / 50, ch = h - 2 * edge;
        lcd_place(field->widget, (lcd_box_t){w - edge - cw, edge, cw, ch});
        row_text(form, field->value_text, gap, cw - 2 * gap - lv_font_get_line_height(s->m.icons) - lcd_dp(s, 6), ch);
        row_text(form, field->name, pad, w - cw - edge - pad - gap, h);
        field->value = view->value >= 0 && (size_t)view->value < view->option_count ? view->value : 0;
        if (choice_options(field->widget, view)) lv_dropdown_set_selected(field->widget, (uint32_t)field->value);
        else field->editable = false;
        const char *const option = field->editable || view->options ?
            (view->option_count > (size_t)field->value ? view->options[field->value] : NULL) : NULL;
        lcd_text(field->value_text, option);
        lv_obj_set_style_text_color(field->value_text, lv_color_hex(field->editable ? LCD_TEXT : LCD_FAINT), 0);
        break;
    }
    case XIAOTAI_FIELD_DIGITS: {
        const uint8_t count = view->max > 0 && view->max <= (int32_t)XIAOTAI_VIEW_DIGITS_MAX ? (uint8_t)view->max : 0;
        const char *accepted = exact_digits(text(view->text), count) ? view->text : "";
        lcd_visible(field->name, !compact);
        const int32_t edge = lcd_dp(s, 4);
        const lcd_box_t area = compact ? (lcd_box_t){0, 0, w, h} : (lcd_box_t){w / 2, edge, w / 2 - edge, h - 2 * edge};
        lcd_place(field->widget, area);
        lv_obj_set_style_pad_top(field->widget, (area.height - lcd_line_height(s)) / 2 - lcd_dp(s, 1), 0);
        lv_textarea_set_placeholder_text(field->widget, text(view->label));
        lv_textarea_set_password_mode(field->widget, (view->flags & XIAOTAI_FIELD_MASKED) != 0);
        lv_textarea_set_max_length(field->widget, count);
        if (field->digits != count || strcmp(field->authoritative, accepted)) {
            memcpy(field->authoritative, accepted, strlen(accepted) + 1U);
            memcpy(field->submitted, accepted, strlen(accepted) + 1U);
            lv_textarea_set_text(field->widget, accepted);
        }
        field->digits = count;
        field->editable &= count != 0;
        break;
    }
    default: break;
    }
    if (field->widget) enabled(field->widget, field->editable);
}

static void form_actions(lcd_form_t *form, const xiaotai_view_t *view)
{
    (void)lcd_simple_actions(form->state, form->layer, view, form->index, form->h);
    form->primary = INACTIVE;
    form->primary_allowed = false;
    for (uint8_t i = 0; i < view->action_count; ++i) {
        const xiaotai_view_action_t *action = &view->actions[i];
        lcd_control_t *control = &form->layer->action[i];
        if (action->role == XIAOTAI_ROLE_PRIMARY && form->primary == INACTIVE) {
            form->primary = i;
            form->primary_allowed = unrestricted(action->state) && present_text(action->intent);
            if (form->show_keypad) lcd_visible(control->obj, false);
        }
        if (action->role == XIAOTAI_ROLE_BACK && !(action->state & XIAOTAI_STATE_HIDDEN)) {
            lcd_bind(control, form->state, form->layer->content, form->index, XIAOTAI_UI_ELEMENT_ACTION, i);
            lcd_control_update(control, layout(form, (lcd_box_t){4, 2, 40, HEADER_H - 4}), NULL,
                               LCD_ICON_BACK, true, unrestricted(action->state), LCD_TONE_QUIET);
        } else if (form->show_keypad) {
            lcd_visible(control->obj, false);
        }
    }
}

bool lcd_form_build(lcd_state_t *state, lcd_layer_t *layer, uint8_t index, xiaotai_view_kind_t kind)
{
    if (kind != XIAOTAI_VIEW_FORM) return false;
    lcd_form_t *form = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*form));
    if (!form) return false;
    form->state = state;
    form->layer = layer;
    form->index = index;
    form->overlay = index == XIAOTAI_LAYER_OVERLAY;
    const lcd_layout_t canvas = lcd_canvas_units(layer);
    form->w = form->overlay ? DIALOG_W : canvas.width;
    form->h = form->overlay ? DIALOG_MAX : canvas.height;
    form->selected = INACTIVE;
    form->primary = INACTIVE;
    layer->form = form;
    layer->title = lcd_label(layer->content, state->m.text, LCD_TEXT);
    layer->status = lcd_label(layer->content, state->m.text, LCD_MUTED);
    lv_label_set_long_mode(layer->title, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(layer->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(layer->status, LV_TEXT_ALIGN_RIGHT, 0);
    form->scroll = lcd_panel(layer->content, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(form->scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(form->scroll, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_scroll_dir(form->scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(form->scroll, LV_SCROLLBAR_MODE_AUTO);
    lcd_scroll_style(state, form->scroll);
    form->note = lcd_label(form->scroll, state->m.text, LCD_MUTED);
    lv_label_set_long_mode(form->note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(form->note, lcd_dp(state, 4), 0);
    return true;
}

static void header_line(lcd_form_t *form, lv_obj_t *obj, int x, int width)
{
    lcd_box_t area = layout(form, (lcd_box_t){x, 0, width, HEADER_H});
    const int32_t h = lcd_line_height(form->state);
    area.y += (area.height - h) / 2;
    area.height = h;
    lcd_place(obj, area);
}

void lcd_form_update(lcd_layer_t *layer, const xiaotai_ui_layer_t *shown, uint32_t now_ms)
{
    (void)now_ms;
    lcd_form_t *form = layer->form;
    const xiaotai_view_t *view = &shown->view;
    if (!form) return;
    lcd_state_t *s = form->state;
    form->applying = true;
    unsigned digits = 0, visible = 0, rows = 0;
    bool back = false, footer = false;
    for (size_t i = 0; i < view->field_count; ++i) {
        if (view->fields[i].state & XIAOTAI_STATE_HIDDEN) continue;
        digits += view->fields[i].kind == XIAOTAI_FIELD_DIGITS;
        ++visible;
    }
    for (size_t i = 0; i < view->item_count && i < XIAOTAI_VIEW_ITEMS_MAX; ++i)
        rows += !(view->items[i].state & XIAOTAI_STATE_HIDDEN);
    form->show_keypad = digits != 0;
    const bool compact = digits > 0 && digits == visible && digits <= 2U && !view->item_count;
    for (size_t i = 0; i < view->action_count; ++i) {
        const xiaotai_view_action_t *a = &view->actions[i];
        if (a->state & XIAOTAI_STATE_HIDDEN) continue;
        if (a->role == XIAOTAI_ROLE_BACK) back = true;
        else footer |= present_text(a->label) || present_text(a->icon);
    }
    const int width = form->w - 2 * MARGIN;
    if (form->overlay) {
        /* A dialog is as tall as its rows, between a floor and the page. */
        lv_point_t note = {0};
        if (present_text(view->text))
            lv_text_get_size(&note, view->text, s->m.text, 0, lcd_dp(s, 4),
                             layout(form, (lcd_box_t){0, 0, width - 8, 0}).width, LV_TEXT_FLAG_NONE);
        const int note_h = present_text(view->text) ? 8 + (int)((note.y * (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT +
            layer->unit.height - 1) / layer->unit.height) : 0;
        const int wanted = HEADER_H + 2 + (int)(visible + rows) * (FORM_ROW + FORM_GAP) + note_h
                           + (footer ? FOOT_H + FOOT_GAP : 0) + FOOT_BOTTOM;
        form->h = wanted > DIALOG_MAX ? DIALOG_MAX : wanted;
        position(form, layer->content, (lcd_box_t){(PAGE_W - DIALOG_W) / 2, (PAGE_H - form->h) / 2, DIALOG_W, form->h});
    }
    lcd_text(layer->title, view->title);
    lcd_text(layer->status, view->status);
    const int left = back ? 46 : MARGIN;
    header_line(form, layer->title, left, form->w - left - MARGIN - (present_text(view->status) ? 104 : 0));
    header_line(form, layer->status, form->w - MARGIN - 100, 100);
    const int bottom = form->h - FOOT_BOTTOM - (footer && !form->show_keypad ? FOOT_H + FOOT_GAP : 0);
    /* The scroll bar lives in the margin, beside the rows. */
    position(form, form->scroll, (lcd_box_t){MARGIN, HEADER_H + 2, width + (form->overlay ? 0 : 8),
                                              compact ? 40 : digits ? 40 : bottom - HEADER_H - 2});
    if (compact) {
        lv_obj_remove_flag(form->scroll, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_scroll_to_y(form->scroll, 0, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(form->scroll, LV_OBJ_FLAG_SCROLLABLE);
    }
    const int row_w = width;
    uint8_t first = INACTIVE;
    int32_t y = 0;
    unsigned column = 0;
    for (uint8_t i = 0; i < XIAOTAI_VIEW_FIELDS_MAX; ++i) {
        field_t *field = &form->fields[i];
        if (i >= view->field_count || (view->fields[i].state & XIAOTAI_STATE_HIDDEN)) {
            if (field->active && field->kind == XIAOTAI_FIELD_DIGITS) {
                lv_textarea_set_text(field->widget, "");
                field->digits = 0;
            }
            field->active = false;
            field->dragging = false;
            lcd_visible(field->row, false);
            continue;
        }
        const xiaotai_view_field_t *entry = &view->fields[i];
        if (!field->row || field->kind != entry->kind) create_field(form, i, entry->kind);
        const int half = (width - 6) / 2;
        const lcd_box_t row = layout(form, compact ?
            (lcd_box_t){digits == 2U && column ? half + 6 : 0, 0, digits == 2U ? half : width, 40} :
            (lcd_box_t){0, y, row_w, FORM_ROW});
        lcd_place(field->row, row);
        update_field(field, entry, row, compact);
        if (field->kind == XIAOTAI_FIELD_DIGITS && field->editable && first == INACTIVE) first = i;
        if (compact) ++column;
        else y += FORM_ROW + FORM_GAP;
    }
    for (uint16_t i = 0; i < XIAOTAI_VIEW_ITEMS_MAX; ++i) {
        lcd_control_t *item = &form->items[i];
        if (i >= view->item_count || (view->items[i].state & XIAOTAI_STATE_HIDDEN)) {
            lcd_visible(item->obj, false);
            continue;
        }
        const xiaotai_view_item_t *entry = &view->items[i];
        const bool usable = unrestricted(entry->state) && present_text(entry->intent);
        lcd_bind(item, s, form->scroll, form->index, XIAOTAI_UI_ELEMENT_ITEM, i);
        const lcd_box_t row = layout(form, (lcd_box_t){0, y, row_w, FORM_ROW});
        /* A row that leads on: the name on the left, a chevron on the right. */
        lcd_control_update(item, row, entry->label, NULL, true, usable, LCD_TONE_QUIET);
        lcd_surface_style(s, item->obj, true);
        lv_obj_set_style_bg_opa(item->obj, usable ? LV_OPA_COVER : LV_OPA_50, 0);
        const int32_t pad = lcd_dp(s, 12), icon = lv_font_get_line_height(s->m.icons);
        lv_obj_set_style_text_align(item->label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_align(item->label, LV_ALIGN_TOP_LEFT);
        row_text(form, item->label, pad, row.width - 2 * pad - icon, row.height);
        if (!form->chevrons[i]) {
            form->chevrons[i] = lcd_label(item->obj, s->m.icons, LCD_FAINT);
            lcd_text(form->chevrons[i], LCD_ICON_NEXT);
        }
        lv_obj_set_pos(form->chevrons[i], row.width - pad - icon + lcd_dp(s, 4), (row.height - icon) / 2);
        lcd_visible(form->chevrons[i], usable);
        y += FORM_ROW + FORM_GAP;
    }
    lcd_visible(form->note, !compact && present_text(view->text));
    if (!compact && present_text(view->text)) {
        const lcd_box_t note = layout(form, (lcd_box_t){4, y + 2, row_w - 8, 0});
        lv_obj_set_pos(form->note, note.x, note.y);
        lv_obj_set_size(form->note, note.width, LV_SIZE_CONTENT);
        lcd_prose(s, form->note, view->text, note.width);
    }
    if (form->selected == INACTIVE || !form->fields[form->selected].active ||
        !form->fields[form->selected].editable || form->fields[form->selected].kind != XIAOTAI_FIELD_DIGITS)
        form->selected = first;
    for (uint8_t i = 0; i < XIAOTAI_VIEW_FIELDS_MAX; ++i) {
        field_t *field = &form->fields[i];
        if (field->active && field->kind == XIAOTAI_FIELD_DIGITS)
            lv_obj_set_state(field->widget, LV_STATE_FOCUSED, i == form->selected);
    }
    if (form->show_keypad) create_keypad(form);
    form_actions(form, view);
    sync_keypad(form);
    form->applying = false;
}

void lcd_form_free(lcd_layer_t *layer)
{
    xiaotai_mem_free(layer->form);
    layer->form = NULL;
}

/* A control inside the scrolling part is reported as far as it shows: what
 * lies outside the list's window cannot be operated. */
static bool shown_part(const lcd_form_t *form, const lv_obj_t *obj, lv_area_t *area)
{
    lv_obj_get_coords(obj, area);
    for (const lv_obj_t *at = lv_obj_get_parent(obj); at; at = lv_obj_get_parent(at)) {
        if (at != form->scroll) continue;
        lv_area_t window;
        lv_obj_get_coords(form->scroll, &window);
        if (area->x1 < window.x1) area->x1 = window.x1;
        if (area->y1 < window.y1) area->y1 = window.y1;
        if (area->x2 > window.x2) area->x2 = window.x2;
        if (area->y2 > window.y2) area->y2 = window.y2;
        return area->x1 <= area->x2 && area->y1 <= area->y2;
    }
    return true;
}

static bool button_area(const lcd_form_t *form, const lv_obj_t *obj, xiaotai_ui_control_t *out, uint8_t layer,
                        uint8_t element, uint16_t index, const char *title)
{
    if (!obj || !lv_obj_is_visible(obj)) return false;
    lv_area_t area;
    if (!shown_part(form, obj, &area)) return false;
    if (out) {
        *out = (xiaotai_ui_control_t){.x = (int16_t)area.x1, .y = (int16_t)area.y1,
            .width = (uint16_t)lv_area_get_width(&area), .height = (uint16_t)lv_area_get_height(&area),
            .layer = layer, .element = element, .index = index,
            .state = lv_obj_has_state(obj, LV_STATE_DISABLED) ? XIAOTAI_STATE_DISABLED : 0};
        if (title) (void)snprintf(out->text, sizeof(out->text), "%s", title);
    }
    return true;
}

size_t lcd_form_controls(const lcd_layer_t *layer, xiaotai_ui_control_t *out, size_t capacity)
{
    const lcd_form_t *form = layer->form;
    if (!form) return 0;
    size_t count = 0;
    for (size_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i)
        if (lcd_measure_control(&layer->action[i], count < capacity && out ? out + count : NULL)) ++count;
    for (uint16_t i = 0; i < XIAOTAI_VIEW_FIELDS_MAX; ++i) {
        const field_t *field = &form->fields[i];
        if (!field->active) continue;
        if (field->kind == XIAOTAI_FIELD_STEPPER) {
            if (button_area(form, field->minus, count < capacity && out ? out + count : NULL,
                            form->index, XIAOTAI_UI_ELEMENT_FIELD, i, NULL)) ++count;
            if (button_area(form, field->plus, count < capacity && out ? out + count : NULL,
                            form->index, XIAOTAI_UI_ELEMENT_FIELD, i, NULL)) ++count;
        } else if (button_area(form, field->widget, count < capacity && out ? out + count : NULL,
                               form->index, XIAOTAI_UI_ELEMENT_FIELD, i, NULL)) ++count;
    }
    for (uint16_t i = 0; i < XIAOTAI_VIEW_ITEMS_MAX; ++i)
        if (button_area(form, form->items[i].obj, count < capacity && out ? out + count : NULL,
                        form->index, XIAOTAI_UI_ELEMENT_ITEM, i, NULL)) ++count;
    for (uint16_t i = 0; i < KEY_COUNT; ++i) {
        const uint8_t element = i == KEY_CONFIRM && form->can_submit ? XIAOTAI_UI_ELEMENT_ACTION : XIAOTAI_UI_ELEMENT_OTHER;
        const uint16_t index = element == XIAOTAI_UI_ELEMENT_ACTION ? form->primary : i;
        if (button_area(form, form->keys[i].object, count < capacity && out ? out + count : NULL,
                        form->index, element, index, i == KEY_BACKSPACE ? "backspace" : key_text[i])) ++count;
    }
    return count;
}
