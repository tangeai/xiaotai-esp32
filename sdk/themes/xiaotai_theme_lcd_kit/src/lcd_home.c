// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "lcd_internal.h"
#include "lcd_face_live.h"

/* Existing immutable shortcut artwork, not an import of the old UI. */
#define s3_phone_shortcut_pixels lcd_phone_pixels
#define s3_phone_shortcut_image lcd_phone_image
#include XIAOTAI_LCD_PHONE_IMAGE
#undef s3_phone_shortcut_image
#undef s3_phone_shortcut_pixels

enum { CONTROL_ROWS_MAX = 4 };

static bool named(const char *key, const char *name) { return key && !strcmp(key, name); }

static bool action_visible(const xiaotai_view_action_t *action)
{
    return !(action->state & XIAOTAI_STATE_HIDDEN);
}

static bool has_label(const xiaotai_view_action_t *action) { return action->label && action->label[0]; }

static bool action_enabled(const xiaotai_view_action_t *action)
{
    return action->intent && !(action->state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY));
}

static lv_obj_t *line(lcd_state_t *s, lv_obj_t *parent, lcd_box_t logical, uint32_t color)
{
    lv_obj_t *label = lcd_label(parent, lcd_body_font(), color);
    lcd_place(label, lcd_scale(&s->layout, logical));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static void center_line(lv_obj_t *label)
{
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void face_placeholder(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view)
{
    const lcd_box_t face = lcd_face_box(&s->layout, 220, 108, lcd_face_preview(view));
    out->face = lcd_panel(out->root, face, LCD_BACKGROUND, LV_OPA_TRANSP, 0);
    /* The temporary expression occupies the real face viewport; the drawing
     * added by the face work package can replace it without moving the hitbox. */
    for (int32_t i = 0; i < 2; ++i) {
        const lcd_box_t eye = {(62 + 71 * i) * face.width / 220,
            42 * face.height / 108, 16 * face.width / 220, 7 * face.height / 108};
        (void)lcd_panel(out->face, eye, LCD_ACCENT, LV_OPA_COVER, 4);
    }
    (void)lcd_panel(out->face, (lcd_box_t){98 * face.width / 220, 69 * face.height / 108,
        24 * face.width / 220, 4 * face.height / 108}, LCD_ACCENT, LV_OPA_COVER, 2);
}

void lcd_home_build(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view, uint8_t layer)
{
    lv_obj_t *mark = lcd_panel(out->root, lcd_scale(&s->layout, (lcd_box_t){14, 7, 20, 20}),
                              0x72def8, LV_OPA_COVER, 6);
    for (int32_t i = 0; i < 2; ++i) {
        (void)lcd_panel(mark, lcd_scale(&s->layout, (lcd_box_t){4 + i * 9, 7, 3, 5}),
                        0x07151c, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
    out->title = line(s, out->root, (lcd_box_t){40, 8, 36, 20}, 0xbfe9f3);
    out->status = line(s, out->root, (lcd_box_t){80, 8, 100, 20}, LCD_ACCENT);
    if (view->kind == XIAOTAI_VIEW_CLOCK) {
        out->clock = line(s, out->root, (lcd_box_t){24, 60, 272, 54}, LCD_TEXT);
#if LV_FONT_MONTSERRAT_48
        lv_obj_set_style_text_font(out->clock, &lv_font_montserrat_48, 0);
#endif
        center_line(out->clock);
        out->date = line(s, out->root, (lcd_box_t){40, 118, 240, 28}, LCD_MUTED);
        center_line(out->date);
    } else {
        if (s->emotion) out->face = lcd_face_live_build(s, out->root, view);
        else face_placeholder(s, out, view);
        if (s->variant && (s->variant->features & XIAOTAI_THEME_LCD_HOME_CLOCK) && view->kind == XIAOTAI_VIEW_HOME)
            out->clock = line(s, out->root, (lcd_box_t){183, 8, 50, 20}, LCD_MUTED);
    }
    out->caption_panel = lcd_panel(out->root, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
    out->body = lcd_label(out->caption_panel, lcd_body_font(), LCD_MUTED);
    center_line(out->body);
    lv_obj_set_style_text_line_space(out->body, 4, 0);
    lv_obj_set_width(out->body, lcd_scale(&s->layout, (lcd_box_t){0, 0, 284, 0}).width);
    lv_label_set_long_mode(out->body, LV_LABEL_LONG_WRAP);
    lv_obj_align(out->body, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (uint16_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i)
        lcd_bind(&out->action[i], s, out->root, layer, XIAOTAI_UI_ELEMENT_ACTION, i);
    for (uint16_t i = 0; i < XIAOTAI_VIEW_INDICATORS_MAX; ++i)
        lcd_bind(&out->indicator[i], s, out->root, layer, XIAOTAI_UI_ELEMENT_INDICATOR, i);
}

static bool special_action(const xiaotai_view_action_t *a, lcd_box_t *box)
{
    if (named(a->icon, XIAOTAI_ICON_MENU)) *box = (lcd_box_t){280, 6, 32, 24};
    else if (named(a->icon, XIAOTAI_ICON_PHONE)) *box = (lcd_box_t){266, 150, 46, 46};   /* clear of the edge */
    else if (named(a->icon, XIAOTAI_ICON_PREVIOUS)) *box = (lcd_box_t){0, 94, 44, 52};
    else if (named(a->icon, XIAOTAI_ICON_NEXT)) *box = (lcd_box_t){276, 94, 44, 52};
    else if (a->role == XIAOTAI_ROLE_BACK && !has_label(a)) *box = (lcd_box_t){0, 6, 32, 24};
    else return false;
    return true;
}

static bool face_action(const xiaotai_view_action_t *action)
{
    return action->role == XIAOTAI_ROLE_PRIMARY && !has_label(action) && !lcd_icon(action->icon);
}

static size_t footer_actions(const xiaotai_view_t *view, bool home)
{
    size_t count = 0;
    for (size_t i = 0; i < view->action_count && i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        const xiaotai_view_action_t *a = &view->actions[i];
        lcd_box_t box;
        if (action_visible(a) && (has_label(a) || lcd_icon(a->icon) || (!home && a->role == XIAOTAI_ROLE_BACK)) &&
            (!home || !special_action(a, &box))) ++count;
    }
    return count;
}

static lcd_box_t footer_box(size_t index, size_t total, int32_t height)
{
    const size_t row = index / CONTROL_ROWS_MAX;
    const size_t left = total - row * CONTROL_ROWS_MAX;
    const int32_t columns = left < CONTROL_ROWS_MAX ? (int32_t)left : CONTROL_ROWS_MAX;
    const int32_t w = 304 / columns;
    const int32_t rows = (int32_t)((total + CONTROL_ROWS_MAX - 1) / CONTROL_ROWS_MAX);
    return (lcd_box_t){8 + (int32_t)(index % CONTROL_ROWS_MAX) * w, height - 3 - (rows - (int32_t)row) * 37,
        w - 4, 33};
}

static void phone_art(lcd_control_t *control, lcd_box_t box, bool phone, bool enabled)
{
    if (phone && !control->image) {
        control->image = lv_image_create(control->obj);
        lv_image_set_src(control->image, &lcd_phone_image);
        lv_obj_remove_flag(control->image, LV_OBJ_FLAG_CLICKABLE);
    }
    lcd_visible(control->image, phone);
    if (!phone) return;
    const int32_t side = box.width < box.height ? box.width : box.height;
    lv_image_set_scale(control->image, (uint32_t)(side * LV_SCALE_NONE / 44));
    lv_obj_center(control->image);
    lv_obj_set_style_image_opa(control->image, enabled ? LV_OPA_COVER : LV_OPA_50, 0);
    lcd_visible(control->label, false);
}

static void home_controls(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view)
{
    const size_t indicators = view->indicator_count < XIAOTAI_VIEW_INDICATORS_MAX ?
                              view->indicator_count : XIAOTAI_VIEW_INDICATORS_MAX;
    const int32_t left = indicators ? 238 - (int32_t)(indicators - 1) * 24 : 260;
    const int32_t status_width = left < 182 ? left - 82 : 100;
    lv_obj_set_width(out->status, lcd_scale(&s->layout, (lcd_box_t){0, 0, status_width, 0}).width);
    if (out->clock && view->kind == XIAOTAI_VIEW_HOME)
        lcd_place(out->clock, lcd_scale(&s->layout, (lcd_box_t){183, indicators > 1 ? 32 : 8, 50, 20}));
    size_t j = 0, total = footer_actions(view, true);
    bool has_face = false;
    for (size_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        lcd_control_t *control = &out->action[i];
        if (i >= view->action_count || !action_visible(&view->actions[i])) {
            lcd_visible(control->obj, false);
            continue;
        }
        const xiaotai_view_action_t *a = &view->actions[i];
        const char *symbol = a->role == XIAOTAI_ROLE_BACK && !a->icon ? LV_SYMBOL_LEFT : lcd_icon(a->icon);
        const bool face = !has_face && face_action(a);
        lcd_box_t box;
        const bool special = special_action(a, &box);
        if (face) {
            box = lcd_face_box(&s->layout, 224, 108, lcd_face_preview(view));
            has_face = true;
        } else if (special) {
            box = lcd_scale(&s->layout, box);
        } else if (has_label(a) || symbol) {
            box = lcd_scale(&s->layout, footer_box(j++, total, 240));
        } else {
            lcd_visible(control->obj, false);
            continue;
        }
        lcd_visible(control->label, true);
        lcd_control_update(control, box, face ? NULL : a->label, symbol, true, action_enabled(a),
                           face || special || a->style == XIAOTAI_STYLE_QUIET, a->style);
        lv_obj_set_ext_click_area(control->obj, named(a->icon, XIAOTAI_ICON_MENU) ? 20 : 0);
        phone_art(control, box, named(a->icon, XIAOTAI_ICON_PHONE), action_enabled(a));
        if (face) lv_obj_move_to_index(control->obj, 0);
    }
    for (size_t i = 0; i < XIAOTAI_VIEW_INDICATORS_MAX; ++i) {
        lcd_control_t *control = &out->indicator[i];
        if (i >= view->indicator_count) { lcd_visible(control->obj, false); continue; }
        const xiaotai_view_indicator_t *item = &view->indicators[i];
        const char *symbol = lcd_icon(item->icon);
        const lcd_box_t box = lcd_scale(&s->layout, (lcd_box_t){238 - (int32_t)i * 24, 7, 22, 24});
        lcd_control_update(control, box, NULL, symbol, symbol != NULL, item->intent != NULL, true, XIAOTAI_STYLE_QUIET);
        if (named(item->icon, XIAOTAI_ICON_WIFI)) {
            const uint32_t color = item->level < 0 ? LCD_MUTED : item->level >= 3 ? 0x8bd9ad :
                                   item->level == 2 ? 0xf2cc84 : 0xf08d89;
            lv_obj_set_style_text_color(control->label, lv_color_hex(color), 0);
        }
    }
}

static void update_clock(lcd_layer_t *out, const xiaotai_ui_clock_t *clock)
{
    if (!out->clock) return;
    char time[16] = "--:--", date[40] = "";
    if (clock->valid) {
        static const char *const weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        snprintf(time, sizeof(time), "%02u:%02u", (unsigned)clock->hour, (unsigned)clock->minute);
        snprintf(date, sizeof(date), "%04u-%02u-%02u %s", (unsigned)clock->year, (unsigned)clock->month,
                 (unsigned)clock->day, weekdays[clock->weekday % 7U]);
    }
    lcd_text(out->clock, time);
    lcd_text(out->date, date);
}

void lcd_home_update(lcd_state_t *s, lcd_layer_t *out, const xiaotai_ui_layer_t *layer,
                     const xiaotai_ui_scene_t *scene)
{
    const xiaotai_view_t *view = &layer->view;
    if (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_EMOTION))
        lcd_face_live_set(s, view, scene);
    if (layer->changed & XIAOTAI_CHANGED_FLAGS) lcd_face_live_place(s, out->face, view);
    if (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_TITLE)) {
        lcd_text(out->title, view->title);
        lcd_text(out->status, view->status);
    }
    if (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ACTIONS | XIAOTAI_CHANGED_INDICATORS | XIAOTAI_CHANGED_FLAGS))
        home_controls(s, out, view);
    if (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ACTIONS | XIAOTAI_CHANGED_TEXT)) {
        const size_t total = footer_actions(view, true);
        const int32_t bottom = total ? lcd_scale(&s->layout, footer_box(0, total, 240)).y - 2 : s->layout.height - 2;
        const int32_t h = 2 * lv_font_get_line_height(lcd_body_font()) + 4;
        const lcd_box_t box = lcd_scale(&s->layout, (lcd_box_t){8, 0, 304, 0});
        lcd_place(out->caption_panel, (lcd_box_t){box.x, bottom - h, box.width, h});
        if (out->speaker != view->speaker) {
            out->speaker = view->speaker;
            const uint32_t color = view->speaker == XIAOTAI_SPEAKER_ASSISTANT ? LCD_CAPTION_AI :
                                   view->speaker == XIAOTAI_SPEAKER_USER ? LCD_CAPTION_USER : LCD_MUTED;
            lv_obj_set_style_text_color(out->body, lv_color_hex(color), 0);
            lv_obj_set_style_text_align(out->body,
                                       view->speaker == XIAOTAI_SPEAKER_NONE ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
        }
        lcd_text(out->body, view->text);
    }
    if (scene->clock_changed || (layer->changed & XIAOTAI_CHANGED_IDENTITY)) update_clock(out, &scene->clock);
}

void lcd_simple_actions(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view, uint8_t layer, int32_t height)
{
    size_t j = 0;
    const size_t total = footer_actions(view, false);
    /* A page lays its footer out on its own canvas; an overlay's box is a
     * share of the whole screen, so it scales by the screen height. */
    const lcd_layout_t inner = {.width = out->canvas.width,
                                .height = layer == XIAOTAI_LAYER_OVERLAY ? s->layout.height : out->canvas.height};
    for (uint16_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        if (i >= view->action_count || !action_visible(&view->actions[i])) {
            lcd_visible(out->action[i].obj, false);
            continue;
        }
        const xiaotai_view_action_t *a = &view->actions[i];
        const char *symbol = a->role == XIAOTAI_ROLE_BACK && !a->icon ? LV_SYMBOL_LEFT : lcd_icon(a->icon);
        if (!has_label(a) && !symbol) { lcd_visible(out->action[i].obj, false); continue; }
        lcd_bind(&out->action[i], s, out->content, layer, XIAOTAI_UI_ELEMENT_ACTION, i);
        lcd_control_update(&out->action[i], lcd_scale(&inner, footer_box(j++, total, height)), a->label,
                           symbol, true, action_enabled(a), a->style == XIAOTAI_STYLE_QUIET, a->style);
    }
}
