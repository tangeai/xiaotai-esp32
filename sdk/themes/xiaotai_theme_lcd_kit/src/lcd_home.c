// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "lcd_internal.h"
#include "lcd_clock.h"
#include "lcd_face_live.h"

enum { COLUMNS_MAX = 4, BAR_Y = 3, BAR_H = 28, FOOT_H = 38, FOOT_GAP = 6, FOOT_BOTTOM = 8, MARGIN = 12 };

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

static lv_obj_t *line(lcd_state_t *s, lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *label = lcd_label(parent, s->m.text, color);
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

/* The brand mark: a rounded square with two eyes, as tall as a line of text. */
static lv_obj_t *brand_mark(lcd_state_t *s, lv_obj_t *parent)
{
    const lcd_box_t bar = lcd_scale(&s->layout, (lcd_box_t){MARGIN, BAR_Y, 0, BAR_H});
    const int32_t side = lcd_line_height(s);
    const lcd_box_t box = {bar.x, bar.y + (bar.height - side) / 2, side, side};
    lv_obj_t *mark = lcd_panel(parent, box, 0x72def8, LV_OPA_COVER, side * 3 / 10);
    for (int32_t i = 0; i < 2; ++i) {
        (void)lcd_panel(mark, (lcd_box_t){side * (4 + i * 9) / 20, side * 7 / 20, side * 3 / 20, side * 5 / 20},
                        0x07151c, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
    return mark;
}

bool lcd_home_desk(const xiaotai_view_t *view)
{
    return view->kind == XIAOTAI_VIEW_CLOCK && (view->flags & XIAOTAI_VIEW_CALENDAR);
}

void lcd_home_build(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view, uint8_t layer)
{
    out->mark = brand_mark(s, out->root);
    out->title = line(s, out->root, LCD_TEXT);
    out->status = line(s, out->root, LCD_ACCENT);
    if (lcd_home_desk(view)) out->desk = lcd_clock_open(s, out->root);
    if (out->desk) {
        /* drawn by the desk clock */
    } else if (view->kind == XIAOTAI_VIEW_CLOCK) {
        out->clock = line(s, out->root, LCD_TEXT);
        lv_obj_set_style_text_font(out->clock, s->m.number, 0);
        center_line(out->clock);
        /* Time and date are one group around the upper third; the date follows
         * the digits, whatever their size. */
        const int32_t digits = lv_font_get_line_height(s->m.number), text = lcd_line_height(s), gap = lcd_dp(s, 12);
        const lcd_box_t middle = lcd_scale(&s->layout, (lcd_box_t){16, 96, 288, 0});
        const int32_t top = middle.y - (digits + gap + text) / 2;
        lcd_place(out->clock, (lcd_box_t){middle.x, top, middle.width, digits});
        out->date = line(s, out->root, LCD_MUTED);
        center_line(out->date);
        lcd_place(out->date, (lcd_box_t){middle.x, top + digits + gap, middle.width, text});
    } else {
        if (s->emotion) out->face = lcd_face_live_build(s, out->root, view);
        else face_placeholder(s, out, view);
        if (s->variant && (s->variant->features & XIAOTAI_THEME_LCD_HOME_CLOCK) && view->kind == XIAOTAI_VIEW_HOME)
            out->clock = line(s, out->root, LCD_MUTED);
    }
    out->caption_panel = lcd_panel(out->root, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
    out->body = lcd_label(out->caption_panel, s->m.text, LCD_MUTED);
    center_line(out->body);
    lv_obj_set_style_text_line_space(out->body, lcd_dp(s, 4), 0);
    lv_obj_set_width(out->body, lcd_scale(&s->layout, (lcd_box_t){0, 0, 288, 0}).width);
    lv_label_set_long_mode(out->body, LV_LABEL_LONG_WRAP);
    lv_obj_align(out->body, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (uint16_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i)
        lcd_bind(&out->action[i], s, out->root, layer, XIAOTAI_UI_ELEMENT_ACTION, i);
    for (uint16_t i = 0; i < XIAOTAI_VIEW_INDICATORS_MAX; ++i)
        lcd_bind(&out->indicator[i], s, out->root, layer, XIAOTAI_UI_ELEMENT_INDICATOR, i);
}

static bool special_action(const xiaotai_view_action_t *a, lcd_box_t *box)
{
    if (named(a->icon, XIAOTAI_ICON_MENU)) *box = (lcd_box_t){282, BAR_Y, 34, BAR_H};
    else if (named(a->icon, XIAOTAI_ICON_PHONE)) *box = (lcd_box_t){266, 136, 46, 46};   /* clear of the edge and of the caption */
    else if (named(a->icon, XIAOTAI_ICON_PREVIOUS)) *box = (lcd_box_t){0, 94, 36, 52};
    else if (named(a->icon, XIAOTAI_ICON_NEXT)) *box = (lcd_box_t){284, 94, 36, 52};
    else if (a->role == XIAOTAI_ROLE_BACK && !has_label(a)) *box = (lcd_box_t){2, BAR_Y, 34, BAR_H};
    else return false;
    return true;
}

static bool face_action(const xiaotai_view_action_t *action)
{
    return action->role == XIAOTAI_ROLE_PRIMARY && !has_label(action) && !lcd_icon(action->icon);
}

/* Whether an action of a simple layout is a footer button. `header_back`: the
 * layout draws the way back in its header, so it is none. */
static bool simple_footer(const xiaotai_view_action_t *a, bool header_back)
{
    if (!action_visible(a)) return false;
    if (a->role == XIAOTAI_ROLE_BACK && header_back) return false;
    return has_label(a) || lcd_icon(a->icon) || a->role == XIAOTAI_ROLE_BACK;
}

static size_t footer_actions(const xiaotai_view_t *view, bool home, bool header_back)
{
    size_t count = 0;
    for (size_t i = 0; i < view->action_count && i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        const xiaotai_view_action_t *a = &view->actions[i];
        lcd_box_t box;
        if (home ? action_visible(a) && (has_label(a) || lcd_icon(a->icon)) && !special_action(a, &box) :
                   simple_footer(a, header_back)) ++count;
    }
    return count;
}

/* Buttons along the bottom of a canvas `width` x `height` logical units, in
 * rows of at most four. */
static lcd_box_t footer_box(size_t index, size_t total, int32_t width, int32_t height)
{
    const int32_t rows = (int32_t)((total + COLUMNS_MAX - 1) / COLUMNS_MAX);
    const size_t row = index / COLUMNS_MAX;
    const size_t left = total - row * COLUMNS_MAX;
    const int32_t columns = left < COLUMNS_MAX ? (int32_t)left : COLUMNS_MAX;
    const int32_t stride = (width - 2 * MARGIN + FOOT_GAP) / columns;
    return (lcd_box_t){MARGIN + (int32_t)(index % COLUMNS_MAX) * stride,
        height - FOOT_BOTTOM - (rows - (int32_t)row) * FOOT_H - (rows - 1 - (int32_t)row) * FOOT_GAP,
        stride - FOOT_GAP, FOOT_H};
}

/* The same on a home screen, in display pixels: on a large screen the buttons
 * stay in the column of the other pages instead of spreading over the glass. */
static lcd_box_t home_footer(const lcd_state_t *s, size_t index, size_t total)
{
    const lcd_layout_t unit = lcd_page_unit(s);
    const lcd_box_t area = lcd_page_area(s);
    const int32_t width = area.width * (int32_t)XIAOTAI_THEME_LCD_LOGICAL_WIDTH / unit.width;
    lcd_box_t box = lcd_scale(&unit, footer_box(index, total, width, XIAOTAI_THEME_LCD_LOGICAL_HEIGHT));
    box.x += area.x;
    box.y += s->layout.height - unit.height;   /* measured from the bottom edge */
    return box;
}

static int32_t footer_top(const lcd_state_t *s, size_t total)
{
    return total ? home_footer(s, 0, total).y : s->layout.height;
}

static int32_t footer_units_top(size_t total, int32_t width, int32_t height)
{
    return total ? footer_box(0, total, width, height).y : height;
}

/* Signal strength: the whole fan faint, the reached arcs lit over it. */
static void wifi_indicator(lcd_state_t *s, lcd_control_t *control, int32_t level)
{
    static const char *const lit[] = {LCD_ICON_WIFI_0, LCD_ICON_WIFI_1, LCD_ICON_WIFI_2, LCD_ICON_WIFI};
    const bool down = level < 0;
    lv_obj_set_style_text_font(control->label, s->m.icons, 0);
    lv_obj_set_width(control->label, LV_SIZE_CONTENT);
    lcd_text(control->label, down ? LCD_ICON_WIFI_OFF : LCD_ICON_WIFI);
    lv_obj_set_style_text_color(control->label, lv_color_hex(down ? LCD_MUTED : LCD_FAINT), 0);
    lv_obj_align(control->label, LV_ALIGN_CENTER, 0, 0);
    lcd_visible(control->label, true);
    lcd_visible(control->icon, !down);
    if (down) return;
    lcd_text(control->icon, lit[level > 3 ? 3 : level]);
    lv_obj_set_style_text_color(control->icon, lv_color_hex(level <= 1 ? LCD_WARNING : LCD_TEXT), 0);
    lv_obj_align(control->icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(control->icon);
}

static void top_bar(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view, int32_t right)
{
    const lcd_box_t bar = lcd_scale(&s->layout, (lcd_box_t){MARGIN, BAR_Y, 0, BAR_H});
    const int32_t h = lcd_line_height(s), gap = lcd_dp(s, 8), y = bar.y + (bar.height - h) / 2;
    lv_point_t size = {0};
    if (view->title && view->title[0])
        lv_text_get_size(&size, view->title, s->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    /* A page with a way back shows no brand mark: the arrow takes its place. */
    bool back = false;
    for (size_t i = 0; i < view->action_count && i < XIAOTAI_VIEW_ACTIONS_MAX; ++i)
        back |= action_visible(&view->actions[i]) && view->actions[i].role == XIAOTAI_ROLE_BACK &&
                !has_label(&view->actions[i]);
    lcd_visible(out->mark, !back);
    int32_t x = back ? lcd_scale(&s->layout, (lcd_box_t){38, 0, 0, 0}).x : bar.x + h + gap;
    if (out->clock && view->kind == XIAOTAI_VIEW_HOME) {
        lv_point_t time;
        lv_text_get_size(&time, "00:00", s->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        right -= time.x + gap;
        lcd_place(out->clock, (lcd_box_t){right, y, time.x, h});
        lv_obj_set_style_text_align(out->clock, LV_TEXT_ALIGN_RIGHT, 0);
    }
    const int32_t room = right - gap - x;
    const int32_t title = size.x < room ? size.x : room;
    lcd_place(out->title, (lcd_box_t){x, y, title > 1 ? title : 1, h});
    x += title + gap;
    lcd_place(out->status, (lcd_box_t){x, y, right - gap - x > 1 ? right - gap - x : 1, h});
}

/* The desk clock lies between the top bar and the footer, and between the
 * arrows that page to the other home screens. Returns where it left room for
 * the call shortcut. */
static lcd_box_t desk_place(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view, size_t footer)
{
    bool pager = false, phone = false;
    for (size_t i = 0; i < view->action_count && i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        const xiaotai_view_action_t *a = &view->actions[i];
        if (!action_visible(a)) continue;
        pager |= named(a->icon, XIAOTAI_ICON_PREVIOUS) || named(a->icon, XIAOTAI_ICON_NEXT);
        phone |= named(a->icon, XIAOTAI_ICON_PHONE);
    }
    lcd_box_t area = lcd_scale(&s->layout, (lcd_box_t){pager ? 30 : MARGIN, BAR_Y + BAR_H + 2, 0, 0});
    area.width = s->layout.width - 2 * area.x;
    area.height = (footer ? footer_top(s, footer) - lcd_dp(s, 8) : s->layout.height - lcd_dp(s, 14)) - area.y;
    return lcd_clock_place(out->desk, view, area, phone);
}

static void home_controls(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view)
{
    const size_t indicators = view->indicator_count < XIAOTAI_VIEW_INDICATORS_MAX ?
                              view->indicator_count : XIAOTAI_VIEW_INDICATORS_MAX;
    size_t j = 0, total = footer_actions(view, true, false);
    bool has_face = false, menu = false;
    const lcd_box_t desk = out->desk ? desk_place(s, out, view, total) : (lcd_box_t){0};
    for (size_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        lcd_control_t *control = &out->action[i];
        if (i >= view->action_count || !action_visible(&view->actions[i])) {
            lcd_visible(control->obj, false);
            continue;
        }
        const xiaotai_view_action_t *a = &view->actions[i];
        const char *symbol = a->role == XIAOTAI_ROLE_BACK && !a->icon ? LCD_ICON_BACK : lcd_icon(a->icon);
        const bool face = !has_face && face_action(a);
        const bool usable = action_enabled(a);
        lcd_box_t box;
        const bool special = special_action(a, &box);
        if (face) {
            box = lcd_face_box(&s->layout, 224, 108, lcd_face_preview(view));
            has_face = true;
            lcd_control_update(control, box, NULL, NULL, true, usable, LCD_TONE_QUIET);
            lv_obj_set_style_bg_opa(control->obj, LV_OPA_TRANSP, LV_STATE_PRESSED);
            lv_obj_move_to_index(control->obj, 0);
        } else if (special && named(a->icon, XIAOTAI_ICON_PHONE)) {
            /* The call shortcut is the round green button of the call pages. */
            box = lcd_scale(&s->layout, box);
            const int32_t side = box.width < box.height ? box.width : box.height;
            const int32_t limit = lcd_line_height(s) * 3;
            const int32_t round = side < limit ? side : limit;
            box = (lcd_box_t){box.x + box.width - round, box.y + box.height - round, round, round};
            if (out->desk) box = desk;
            lcd_round_update(control, box, box.width, NULL, LCD_ICON_PHONE, usable, LCD_TONE_POSITIVE);
        } else if (special) {
            const bool pager = named(a->icon, XIAOTAI_ICON_PREVIOUS) || named(a->icon, XIAOTAI_ICON_NEXT);
            menu |= named(a->icon, XIAOTAI_ICON_MENU);
            lcd_control_update(control, lcd_scale(&s->layout, box), NULL, symbol, true, usable, LCD_TONE_QUIET);
            if (pager) lv_obj_set_style_text_color(control->icon, lv_color_hex(LCD_FAINT), 0);
            lv_obj_set_ext_click_area(control->obj, named(a->icon, XIAOTAI_ICON_MENU) ? lcd_dp(s, 14) : 0);
        } else if (has_label(a) || symbol) {
            /* On a home screen the labelled buttons lead on; none of them is
             * "the" answer, so none takes the accent. */
            const lcd_tone_t tone = lcd_action_tone(a);
            lcd_control_update(control, home_footer(s, j++, total), a->label, symbol, true,
                               usable, tone == LCD_TONE_ACCENT && view->kind != XIAOTAI_VIEW_TALK ? LCD_TONE_NEUTRAL : tone);
        } else {
            lcd_visible(control->obj, false);
        }
    }
    /* Indicators sit left of the menu, or at the edge when there is none. */
    const int32_t first = menu ? 256 : 290;
    int32_t right = lcd_scale(&s->layout, (lcd_box_t){first + 26, 0, 0, 0}).x;
    for (size_t i = 0; i < XIAOTAI_VIEW_INDICATORS_MAX; ++i) {
        lcd_control_t *control = &out->indicator[i];
        if (i >= indicators) { lcd_visible(control->obj, false); continue; }
        const xiaotai_view_indicator_t *item = &view->indicators[i];
        const char *symbol = lcd_icon(item->icon);
        const lcd_box_t box = lcd_scale(&s->layout, (lcd_box_t){first - (int32_t)i * 26, BAR_Y, 24, BAR_H});
        lcd_control_update(control, box, NULL, symbol, symbol != NULL, item->intent != NULL, LCD_TONE_QUIET);
        if (!symbol) continue;
        right = box.x;
        if (named(item->icon, XIAOTAI_ICON_WIFI)) wifi_indicator(s, control, item->level);
        else lv_obj_set_style_text_color(control->icon, lv_color_hex(LCD_MUTED), 0);
    }
    top_bar(s, out, view, right);
}

static void update_clock(lcd_layer_t *out, const xiaotai_ui_clock_t *clock)
{
    if (!out->clock) return;
    char time[16] = "--:--", date[40] = "";
    if (clock->valid) {
        static const char *const weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        snprintf(time, sizeof(time), "%02u:%02u", (unsigned)clock->hour, (unsigned)clock->minute);
        snprintf(date, sizeof(date), "%u月%u日 %s", (unsigned)clock->month, (unsigned)clock->day,
                 weekdays[clock->weekday % 7U]);
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
    const bool replace = out->desk && lcd_clock_update(out->desk, view, scene);
    if (replace || (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_TITLE | XIAOTAI_CHANGED_ACTIONS |
                                      XIAOTAI_CHANGED_INDICATORS | XIAOTAI_CHANGED_FLAGS)))
        home_controls(s, out, view);
    if (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ACTIONS | XIAOTAI_CHANGED_TEXT)) {
        const size_t total = footer_actions(view, true, false);
        const int32_t bottom = footer_top(s, total) - lcd_dp(s, total ? 4 : 6);
        const int32_t h = 2 * lcd_line_height(s) + lcd_dp(s, 4);
        const lcd_box_t box = lcd_scale(&s->layout, (lcd_box_t){16, 0, 288, 0});
        lcd_place(out->caption_panel, (lcd_box_t){box.x, bottom - h, box.width, h});
        if (out->speaker != view->speaker) {
            out->speaker = view->speaker;
            const uint32_t color = view->speaker == XIAOTAI_SPEAKER_ASSISTANT ? LCD_CAPTION_AI :
                                   view->speaker == XIAOTAI_SPEAKER_USER ? LCD_CAPTION_USER : LCD_MUTED;
            lv_obj_set_style_text_color(out->body, lv_color_hex(color), 0);
            lv_obj_set_style_text_align(out->body,
                                       view->speaker == XIAOTAI_SPEAKER_NONE ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
        }
        lcd_prose(s, out->body, view->text, lcd_scale(&s->layout, (lcd_box_t){0, 0, 288, 0}).width);
    }
    if (out->desk) {
        lcd_visible(out->caption_panel, false);
    } else if (scene->clock_changed || (layer->changed & XIAOTAI_CHANGED_IDENTITY)) {
        update_clock(out, &scene->clock);
    }
}

int32_t lcd_simple_actions(lcd_state_t *s, lcd_layer_t *out, const xiaotai_view_t *view, uint8_t layer, int32_t height)
{
    size_t j = 0;
    const bool header_back = out->form != NULL;
    const size_t total = footer_actions(view, false, header_back);
    const int32_t width = layer == XIAOTAI_LAYER_OVERLAY ? 272 : lcd_canvas_units(out).width;
    for (uint16_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        if (i >= view->action_count || !action_visible(&view->actions[i])) {
            lcd_visible(out->action[i].obj, false);
            continue;
        }
        const xiaotai_view_action_t *a = &view->actions[i];
        const char *symbol = a->role == XIAOTAI_ROLE_BACK && !a->icon ? LCD_ICON_BACK : lcd_icon(a->icon);
        if (!simple_footer(a, header_back)) {
            lcd_visible(out->action[i].obj, false);
            continue;
        }
        lcd_bind(&out->action[i], s, out->content, layer, XIAOTAI_UI_ELEMENT_ACTION, i);
        lcd_control_update(&out->action[i], lcd_scale(&out->unit, footer_box(j++, total, width, height)), a->label,
                           symbol, true, action_enabled(a), lcd_action_tone(a));
    }
    return footer_units_top(total, width, height);
}
