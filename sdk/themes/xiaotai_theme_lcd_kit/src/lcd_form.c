// SPDX-License-Identifier: MIT
#include "lcd_form.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/port/mem.h"

enum { FORM_ROW = 48, FORM_GAP = 4, KEY_COUNT = 12, KEY_BACKSPACE = 10, KEY_CONFIRM = 11 };
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
    keypad_key_t keys[KEY_COUNT];
    uint8_t index, selected, primary;
    bool applying, show_keypad, can_submit, primary_allowed;
};

static const char *const key_text[KEY_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "确认",
};

static const char *text(const char *s) { return s ? s : ""; }
static bool present_text(const char *s) { return s && *s; }
static bool unrestricted(uint8_t state)
{
    return !(state & (XIAOTAI_STATE_BUSY | XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_HIDDEN));
}

static lcd_box_t layout(const lcd_form_t *form, lcd_box_t box)
{
    const lcd_layout_t inner = {form->layer->canvas.width, form->layer->canvas.height};
    return lcd_scale(&inner, box);
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

static lv_obj_t *button(lv_obj_t *parent, const char *title, bool symbol, bool primary, lv_obj_t **label_out)
{
    lv_obj_t *obj = lcd_panel(parent, (lcd_box_t){0}, primary ? LCD_POSITIVE : LCD_SURFACE_HIGH, LV_OPA_COVER, LCD_RADIUS);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = lcd_label(obj, symbol ? &lv_font_montserrat_14 : lcd_body_font(), LCD_TEXT);
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
        enabled(form->keys[i].object, i == KEY_CONFIRM ? submit : form->selected != INACTIVE);
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
        key->object = button(form->layer->content, key_text[i], i == KEY_BACKSPACE, i == KEY_CONFIRM, &key->label);
        position(form, key->object, (lcd_box_t){8 + (i % 4U) * 77, 84 + (i / 4U) * 52, 73, 48});
        lv_obj_add_event_cb(key->object, keypad_event, LV_EVENT_ALL, key);
    }
}

static void create_field(lcd_form_t *form, uint8_t index, uint8_t kind)
{
    field_t *field = &form->fields[index];
    if (field->row) lv_obj_delete(field->row);
    *field = (field_t){.form = form, .index = index, .kind = kind};
    field->row = lcd_panel(form->scroll, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
    field->name = lcd_label(field->row, lcd_body_font(), LCD_TEXT);
    lv_label_set_long_mode(field->name, LV_LABEL_LONG_DOT);
    switch (kind) {
    case XIAOTAI_FIELD_SWITCH:
        field->widget = lv_switch_create(field->row);
        lv_obj_set_style_bg_color(field->widget, lv_color_hex(LCD_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
        break;
    case XIAOTAI_FIELD_SLIDER: field->widget = lv_slider_create(field->row); break;
    case XIAOTAI_FIELD_CHOICE: {
        field->widget = lv_dropdown_create(field->row);
        lv_obj_set_style_text_font(field->widget, lcd_body_font(), 0);
        lv_obj_set_style_text_font(lv_dropdown_get_list(field->widget), lcd_body_font(), 0);
        lv_dropdown_set_symbol(field->widget, NULL);
        lv_obj_t *arrow = lcd_label(field->widget, &lv_font_montserrat_14, LCD_MUTED);
        lcd_text(arrow, LV_SYMBOL_DOWN);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
        break;
    }
    case XIAOTAI_FIELD_DIGITS:
        field->widget = lv_textarea_create(field->row);
        lv_textarea_set_one_line(field->widget, true);
        lv_textarea_set_accepted_chars(field->widget, "0123456789");
        lv_obj_set_style_text_font(field->widget, lcd_body_font(), 0);
        lv_obj_set_style_pad_all(field->widget, 6, 0);
        lv_obj_set_style_border_color(field->widget, lv_color_hex(LCD_ACCENT), LV_STATE_FOCUSED);
        break;
    case XIAOTAI_FIELD_STEPPER:
        field->minus = button(field->row, LV_SYMBOL_MINUS, true, false, NULL);
        field->plus = button(field->row, LV_SYMBOL_PLUS, true, false, NULL);
        lv_obj_add_event_cb(field->minus, field_event, LV_EVENT_ALL, field);
        lv_obj_add_event_cb(field->plus, field_event, LV_EVENT_ALL, field);
        break;
    default: break;
    }
    if (field->widget) lv_obj_add_event_cb(field->widget, field_event, LV_EVENT_ALL, field);
    if (kind == XIAOTAI_FIELD_TEXT || kind == XIAOTAI_FIELD_STEPPER || kind == XIAOTAI_FIELD_SLIDER) {
        field->value_text = lcd_label(field->row, lcd_body_font(), LCD_MUTED);
        lv_label_set_long_mode(field->value_text, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(field->value_text, LV_TEXT_ALIGN_CENTER, 0);
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
    (void)snprintf(value, sizeof(value), "%ld%s%s", (long)field->value,
                   present_text(view->text) ? " " : "", text(view->text));
    lcd_text(field->value_text, value);
}

static void update_field(field_t *field, const xiaotai_view_field_t *view, int32_t width, bool compact)
{
    lcd_form_t *form = field->form;
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
    position(form, field->name, (lcd_box_t){4, 13, width / 2 - 8, 22});
    switch (field->kind) {
    case XIAOTAI_FIELD_TEXT:
        lcd_text(field->value_text, view->text);
        position(form, field->value_text, (lcd_box_t){width / 2, 13, width / 2, 22});
        break;
    case XIAOTAI_FIELD_SWITCH:
        position(form, field->widget, (lcd_box_t){width - 62, 10, 56, 28});
        lv_obj_set_ext_click_area(field->widget, 8);
        lv_obj_set_state(field->widget, LV_STATE_CHECKED, view->value != 0);
        field->value = view->value != 0;
        break;
    case XIAOTAI_FIELD_SLIDER:
        position(form, field->widget, (lcd_box_t){width / 2, 16, width / 2 - 52, 16});
        position(form, field->value_text, (lcd_box_t){width - 44, 13, 44, 22});
        lv_obj_set_ext_click_area(field->widget, 8);
        lv_slider_set_range(field->widget, field->min, field->max);
        if (!field->dragging || !field->editable) lv_slider_set_value(field->widget, field->value, LV_ANIM_OFF);
        numeric_label(field, view);
        break;
    case XIAOTAI_FIELD_STEPPER:
        position(form, field->name, (lcd_box_t){48, 4, width - 96, 19});
        lv_obj_set_style_text_align(field->name, LV_TEXT_ALIGN_CENTER, 0);
        position(form, field->minus, (lcd_box_t){0, 2, 44, 44});
        position(form, field->plus, (lcd_box_t){width - 44, 2, 44, 44});
        position(form, field->value_text, (lcd_box_t){48, 25, width - 96, 20});
        numeric_label(field, view);
        enabled(field->minus, field->editable && field->value > field->min);
        enabled(field->plus, field->editable && field->value < field->max);
        break;
    case XIAOTAI_FIELD_CHOICE:
        position(form, field->widget, (lcd_box_t){width - 140, 2, 138, 44});
        field->value = view->value >= 0 && (size_t)view->value < view->option_count ? view->value : 0;
        if (choice_options(field->widget, view)) lv_dropdown_set_selected(field->widget, (uint32_t)field->value);
        else field->editable = false;
        break;
    case XIAOTAI_FIELD_DIGITS: {
        const uint8_t count = view->max > 0 && view->max <= (int32_t)XIAOTAI_VIEW_DIGITS_MAX ? (uint8_t)view->max : 0;
        const char *accepted = exact_digits(text(view->text), count) ? view->text : "";
        lcd_visible(field->name, !compact);
        position(form, field->widget, compact ? (lcd_box_t){0, 0, width, 36} :
                                               (lcd_box_t){width / 2, 2, width / 2, 44});
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
    const int32_t height = form->index == XIAOTAI_LAYER_OVERLAY ? 174 : 240;
    lcd_simple_actions(form->state, form->layer, view, form->index, height);
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
            lcd_control_update(control, layout(form, (lcd_box_t){5, 2, 42, 40}), NULL,
                               LV_SYMBOL_LEFT, true, unrestricted(action->state), true, action->style);
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
    form->selected = INACTIVE;
    form->primary = INACTIVE;
    layer->form = form;
    layer->title = lcd_label(layer->content, lcd_body_font(), LCD_TEXT);
    layer->status = lcd_label(layer->content, lcd_body_font(), LCD_ACCENT);
    position(form, layer->title, (lcd_box_t){52, 11, 152, 23});
    position(form, layer->status, (lcd_box_t){210, 11, 100, 22});
    lv_label_set_long_mode(layer->title, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(layer->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(layer->status, LV_TEXT_ALIGN_RIGHT, 0);
    form->scroll = lcd_panel(layer->content, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(form->scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(form->scroll, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_scroll_dir(form->scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(form->scroll, LV_SCROLLBAR_MODE_AUTO);
    form->note = lcd_label(form->scroll, lcd_body_font(), LCD_MUTED);
    lv_label_set_long_mode(form->note, LV_LABEL_LONG_WRAP);
    return true;
}

void lcd_form_update(lcd_layer_t *layer, const xiaotai_ui_layer_t *shown, uint32_t now_ms)
{
    (void)now_ms;
    lcd_form_t *form = layer->form;
    const xiaotai_view_t *view = &shown->view;
    if (!form) return;
    form->applying = true;
    lcd_text(layer->title, view->title);
    lcd_text(layer->status, view->status);
    unsigned digits = 0, visible = 0;
    for (size_t i = 0; i < view->field_count; ++i) {
        if (view->fields[i].state & XIAOTAI_STATE_HIDDEN) continue;
        digits += view->fields[i].kind == XIAOTAI_FIELD_DIGITS;
        ++visible;
    }
    form->show_keypad = digits != 0;
    const bool compact = digits > 0 && digits == visible && digits <= 2U && !view->item_count;
    bool footer = false;
    for (size_t i = 0; i < view->action_count; ++i) {
        const xiaotai_view_action_t *a = &view->actions[i];
        footer |= a->role != XIAOTAI_ROLE_BACK && !(a->state & XIAOTAI_STATE_HIDDEN) &&
                  (present_text(a->label) || present_text(a->icon));
    }
    position(form, form->scroll, (lcd_box_t){8, compact ? 44 : 46, 304,
                                              compact ? 36 : digits ? 38 : footer ? 136 : 190});
    if (compact) {
        lv_obj_remove_flag(form->scroll, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_scroll_to_y(form->scroll, 0, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(form->scroll, LV_OBJ_FLAG_SCROLLABLE);
    }
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
        const int32_t width = compact && digits == 2U ? (column ? 150 : 146) : 296;
        position(form, field->row, (lcd_box_t){compact && column ? 154 : 0, y, width, compact ? 36 : FORM_ROW});
        update_field(field, entry, width, compact);
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
        lcd_bind(item, form->state, form->scroll, form->index, XIAOTAI_UI_ELEMENT_ITEM, i);
        lcd_control_update(item, layout(form, (lcd_box_t){0, y, 296, FORM_ROW}), entry->label,
                           lcd_icon(entry->icon), true, unrestricted(entry->state) && present_text(entry->intent),
                           false, XIAOTAI_STYLE_NORMAL);
        y += FORM_ROW + FORM_GAP;
    }
    lcd_visible(form->note, !compact && present_text(view->text));
    if (!compact && present_text(view->text)) {
        const lcd_box_t note = layout(form, (lcd_box_t){4, y + 2, 296, 0});
        lv_obj_set_pos(form->note, note.x, note.y);
        lv_obj_set_size(form->note, note.width, LV_SIZE_CONTENT);
        lcd_text(form->note, view->text);
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

static bool button_area(const lv_obj_t *obj, xiaotai_ui_control_t *out, uint8_t layer,
                        uint8_t element, uint16_t index, const char *title)
{
    if (!obj || !lv_obj_is_visible(obj)) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
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
            if (button_area(field->minus, count < capacity && out ? out + count : NULL,
                            form->index, XIAOTAI_UI_ELEMENT_FIELD, i, NULL)) ++count;
            if (button_area(field->plus, count < capacity && out ? out + count : NULL,
                            form->index, XIAOTAI_UI_ELEMENT_FIELD, i, NULL)) ++count;
        } else if (button_area(field->widget, count < capacity && out ? out + count : NULL,
                               form->index, XIAOTAI_UI_ELEMENT_FIELD, i, NULL)) ++count;
    }
    for (size_t i = 0; i < XIAOTAI_VIEW_ITEMS_MAX; ++i)
        if (lcd_measure_control(&form->items[i], count < capacity && out ? out + count : NULL)) ++count;
    for (uint16_t i = 0; i < KEY_COUNT; ++i) {
        const uint8_t element = i == KEY_CONFIRM && form->can_submit ? XIAOTAI_UI_ELEMENT_ACTION : XIAOTAI_UI_ELEMENT_OTHER;
        const uint16_t index = element == XIAOTAI_UI_ELEMENT_ACTION ? form->primary : i;
        if (button_area(form->keys[i].object, count < capacity && out ? out + count : NULL,
                        form->index, element, index, key_text[i])) ++count;
    }
    return count;
}
