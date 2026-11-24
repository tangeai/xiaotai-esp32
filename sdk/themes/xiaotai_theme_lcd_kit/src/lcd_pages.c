// SPDX-License-Identifier: MIT
#include "lcd_pages.h"

#include <stdio.h>
#include <string.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "xiaotai/port/mem.h"

/* Logical units of the 320 x 240 design canvas. */
enum {
    PAGE_W = 320, PAGE_H = 240, MARGIN = 12, HEADER_H = 40, ROW_H = 44, TWO_H = 48, ROW_GAP = 4, INFO_H = 28,
    FOOT_H = 38, FOOT_GAP = 6, FOOT_BOTTOM = 8, TAB_H = 30, BAR_W = 8,
    DIALOG_W = 272, DIALOG_MIN = 104, DIALOG_MAX = 224,
    INERT = XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY,
};

typedef struct {
    lcd_control_t control;
    lv_obj_t *detail, *icon, *chevron;
    bool selectable;
} row_t;

typedef struct {
    int back, refresh, checked, count;
    int other[XIAOTAI_VIEW_ACTIONS_MAX];
    int top, bottom;            /* logical y of the body's first and last line */
    int title_left, title_right;
} layout_t;

struct lcd_pages {
    lcd_state_t *owner;
    lcd_layer_t *layer;
    lcd_layout_t layout;
    xiaotai_view_kind_t kind;
    uint8_t index;
    int w, h;                   /* logical size of the canvas: the page, or the dialog panel */
    lv_obj_t *title, *subtitle, *status, *timer, *scroller, *intro, *body, *plate, *picture, *value, *bar, *fill, *qr;
    lv_obj_t *avatar, *initial, *halo, *ring, *shade;   /* CALL: a round badge with the first character of the name */
    lv_obj_t *spinner, *mark;                   /* PROGRESS: the busy ring, or the mark of a stopped step */
    row_t rows[XIAOTAI_VIEW_ITEMS_MAX];
    const xiaotai_ui_image_t *image;
    lv_image_dsc_t image_desc;
    uint8_t *pixels;            /* the MONO1 picture as drawn, and the side it was drawn for */
    int32_t pixels_side;
    layout_t geometry;
    int32_t qr_size, label_column;
    int checked_tab;
    bool laid_out, qr_ready, overlay;
};

static lcd_box_t box(const lcd_pages_t *p, int x, int y, int w, int h)
{
    return lcd_scale(&p->layout, (lcd_box_t){x, y, w, h});
}

static bool text_is(const char *s, const char *value) { return s && !strcmp(s, value); }
static bool text_some(const char *s) { return s && *s; }
static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }
/* Display pixels back to logical units of this canvas, rounded up. */
static int to_logical(const lcd_pages_t *p, int32_t pixels)
{
    return (int)((pixels * (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT + p->layout.height - 1) / p->layout.height);
}

static int to_logical_x(const lcd_pages_t *p, int32_t pixels)
{
    return (int)((pixels * (int32_t)XIAOTAI_THEME_LCD_LOGICAL_WIDTH + p->layout.width - 1) / p->layout.width);
}

static lv_obj_t *label(lcd_pages_t *p, lv_obj_t *parent, const lv_font_t *font, uint32_t color, bool centered)
{
    lv_obj_t *obj = lcd_label(parent, font ? font : p->owner->m.text, color);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    if (centered) lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    return obj;
}

/* One line of text, vertically centred in its logical box. */
static void line(lcd_pages_t *p, lv_obj_t *obj, const char *text, int x, int y, int w, int h)
{
    lcd_box_t area = box(p, x, y, w, h);
    const int32_t font = lv_font_get_line_height(lv_obj_get_style_text_font(obj, 0));
    if (area.height > font) {
        area.y += (area.height - font) / 2;
        area.height = font;
    }
    lcd_place(obj, area);
    lcd_text(obj, text);
}

static lv_obj_t *scroller(lcd_pages_t *p, lv_obj_t *root)
{
    lv_obj_t *obj = lcd_panel(root, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lcd_scroll_style(p->owner, obj);
    return obj;
}

bool lcd_pages_matches(xiaotai_view_kind_t kind)
{
    return kind == XIAOTAI_VIEW_LIST || kind == XIAOTAI_VIEW_CODE || kind == XIAOTAI_VIEW_CALL ||
           kind == XIAOTAI_VIEW_NOTICE || kind == XIAOTAI_VIEW_PROGRESS;
}

lcd_pages_t *lcd_pages_open(lcd_state_t *s, lcd_layer_t *layer, xiaotai_view_kind_t kind, uint8_t index)
{
    lcd_pages_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*p));
    if (!p) return NULL;
    p->owner = s;
    p->layer = layer;
    p->layout = layer->unit;
    p->kind = kind;
    p->index = index;
    p->overlay = index == XIAOTAI_LAYER_OVERLAY;
    const lcd_layout_t canvas = lcd_canvas_units(layer);
    p->w = p->overlay ? DIALOG_W : canvas.width;
    p->h = p->overlay ? DIALOG_MAX : canvas.height;
    p->checked_tab = -1;

    lv_obj_t *root = layer->content;
    const bool call = kind == XIAOTAI_VIEW_CALL;
    if (call) {
        p->shade = lcd_panel(root, (lcd_box_t){0}, LCD_BACKGROUND, LV_OPA_60, 0);
        lcd_visible(p->shade, false);
        /* The ring of a waiting call sits behind the badge, in a box it can grow in. */
        p->halo = lcd_panel(root, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
        p->ring = lcd_panel(p->halo, (lcd_box_t){0}, 0, LV_OPA_TRANSP, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_color(p->ring, lv_color_hex(LCD_ACCENT), 0);
        lv_obj_set_style_border_width(p->ring, lcd_dp(s, 2), 0);
        lv_obj_set_style_border_opa(p->ring, LV_OPA_TRANSP, 0);
        p->avatar = lcd_panel(root, (lcd_box_t){0}, LCD_ACCENT_DIM, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        p->initial = label(p, p->avatar, NULL, LCD_ACCENT, true);
        lv_obj_set_width(p->initial, lv_pct(100));
        lv_obj_center(p->initial);
    }
    p->title = label(p, root, NULL, call ? LCD_MUTED : LCD_TEXT, call);
    p->subtitle = label(p, root, NULL, call ? LCD_TEXT : LCD_MUTED, kind != XIAOTAI_VIEW_LIST);
    p->status = label(p, root, NULL, LCD_MUTED, call || kind == XIAOTAI_VIEW_PROGRESS);
    if (call || kind == XIAOTAI_VIEW_CODE) p->timer = label(p, root, NULL, call ? LCD_ACCENT : LCD_MUTED, true);
    if (kind == XIAOTAI_VIEW_LIST || kind == XIAOTAI_VIEW_NOTICE || kind == XIAOTAI_VIEW_CODE)
        p->scroller = scroller(p, root);
    if (kind == XIAOTAI_VIEW_NOTICE || kind == XIAOTAI_VIEW_LIST) {
        /* LIST uses them for the line shown when it has nothing to list. */
        p->intro = label(p, p->scroller, NULL, LCD_MUTED, false);
        p->body = label(p, p->scroller, NULL, kind == XIAOTAI_VIEW_LIST ? LCD_MUTED : LCD_TEXT, false);
        lv_label_set_long_mode(p->intro, LV_LABEL_LONG_WRAP);
        lv_label_set_long_mode(p->body, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(p->intro, LV_SIZE_CONTENT);
        lv_obj_set_height(p->body, LV_SIZE_CONTENT);
        lv_obj_set_style_text_line_space(p->intro, lcd_dp(s, 4), 0);
        lv_obj_set_style_text_line_space(p->body, lcd_dp(s, 4), 0);
    } else if (kind == XIAOTAI_VIEW_CODE) {
        p->value = label(p, root, s->m.number, LCD_TEXT, true);
        p->plate = lcd_panel(root, (lcd_box_t){0}, 0xffffff, LV_OPA_COVER, lcd_dp(s, 6));
        lcd_visible(p->plate, false);
        p->picture = lv_image_create(root);
        lv_image_set_inner_align(p->picture, LV_IMAGE_ALIGN_CONTAIN);
        lcd_visible(p->picture, false);
#if LV_USE_QRCODE
        p->qr = lv_qrcode_create(root);
        lv_qrcode_set_quiet_zone(p->qr, true);
        lv_qrcode_set_dark_color(p->qr, lv_color_black());
        lv_qrcode_set_light_color(p->qr, lv_color_white());
        lv_obj_set_style_radius(p->qr, lcd_dp(s, 6), 0);
        lv_obj_set_style_clip_corner(p->qr, true, 0);
        lcd_visible(p->qr, false);
#endif
    } else if (kind == XIAOTAI_VIEW_PROGRESS) {
        p->bar = lcd_panel(root, (lcd_box_t){0}, LCD_SURFACE_HIGH, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        p->fill = lcd_panel(p->bar, (lcd_box_t){0}, LCD_ACCENT, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        p->spinner = lv_arc_create(root);
        lv_obj_remove_style_all(p->spinner);
        lv_obj_remove_flag(p->spinner, LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_bg_angles(p->spinner, 0, 360);
        lv_arc_set_angles(p->spinner, 0, 96);
        for (int part = 0; part < 2; ++part) {
            const lv_style_selector_t selector = part ? LV_PART_INDICATOR : LV_PART_MAIN;
            lv_obj_set_style_arc_width(p->spinner, lcd_dp(s, 4), selector);
            lv_obj_set_style_arc_rounded(p->spinner, true, selector);
            lv_obj_set_style_arc_opa(p->spinner, LV_OPA_COVER, selector);
            lv_obj_set_style_arc_color(p->spinner, lv_color_hex(part ? LCD_ACCENT : LCD_SURFACE_HIGH), selector);
        }
        p->mark = label(p, root, s->m.icons, LCD_WARNING, true);
        lcd_text(p->mark, LCD_ICON_ALERT);
    }
    return p;
}

static const char *action_icon(const xiaotai_view_action_t *a)
{
    /* Answering shows a handset, not a check mark. */
    if (a->style == XIAOTAI_STYLE_POSITIVE && text_is(a->icon, XIAOTAI_ICON_ACCEPT)) return LCD_ICON_PHONE;
    return a->role == XIAOTAI_ROLE_BACK && !text_some(a->icon) ? LCD_ICON_BACK : lcd_icon(a->icon);
}

static bool action_visible(const lcd_pages_t *p, const xiaotai_view_action_t *a)
{
    if (a->state & XIAOTAI_STATE_HIDDEN) return false;
    /* A dialog closes by a tap beside it: its way back needs no arrow unless the view names one. */
    if (p->overlay && a->role == XIAOTAI_ROLE_BACK && !text_some(a->label) && !text_some(a->icon)) return false;
    return text_some(a->label) || action_icon(a);
}

static lcd_control_t *bind_action(lcd_pages_t *p, const xiaotai_view_t *v, size_t i)
{
    const xiaotai_view_action_t *a = &v->actions[i];
    lcd_control_t *control = &p->layer->action[i];
    lcd_bind(control, p->owner, p->layer->content, p->index, XIAOTAI_UI_ELEMENT_ACTION, (uint16_t)i);
    const bool pointer_hold = a->trigger == XIAOTAI_TRIGGER_HOLD && (a->state & XIAOTAI_STATE_HOLD_FROM_POINTER);
    if (pointer_hold != control->pointer_hold) {
        if (pointer_hold) control->normal_press_lock = lv_obj_has_flag(control->obj, LV_OBJ_FLAG_PRESS_LOCK);
        else control->pointer_down = false; /* the scene already cancelled the old binding */
        lv_obj_set_flag(control->obj, LV_OBJ_FLAG_PRESS_LOCK, !pointer_hold && control->normal_press_lock);
        control->pointer_hold = pointer_hold;
    }
    return control;
}

static bool action_enabled(const xiaotai_view_action_t *a) { return a->intent && !(a->state & INERT); }

/* A header icon: back on the left, refresh on the right. */
static void header_action(lcd_pages_t *p, const xiaotai_view_t *v, int i, int x)
{
    const xiaotai_view_action_t *a = &v->actions[i];
    lcd_control_t *control = bind_action(p, v, (size_t)i);
    lcd_control_update(control, box(p, x, 2, 40, HEADER_H - 4), NULL, action_icon(a), true, action_enabled(a),
                       LCD_TONE_QUIET);
    lv_obj_set_style_border_width(control->obj, 0, 0);
}

/* A tab is a pill; the current one is filled with the dimmed accent. */
static void tab_action(lcd_pages_t *p, const xiaotai_view_t *v, int i, lcd_box_t logical)
{
    const xiaotai_view_action_t *a = &v->actions[i];
    lcd_control_t *control = bind_action(p, v, (size_t)i);
    const bool selected = (a->state & XIAOTAI_STATE_CHECKED) != 0;
    lcd_control_update(control, box(p, logical.x, logical.y, logical.width, logical.height), a->label, NULL, true,
                       action_enabled(a), LCD_TONE_QUIET);
    lv_obj_set_style_radius(control->obj, LV_RADIUS_CIRCLE, 0);
    if (selected) {
        lv_obj_set_style_bg_color(control->obj, lv_color_hex(LCD_ACCENT_DIM), 0);
        lv_obj_set_style_bg_opa(control->obj, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_text_color(control->label, lv_color_hex(selected ? LCD_ACCENT : LCD_MUTED), 0);
}

static void footer_action(lcd_pages_t *p, const xiaotai_view_t *v, int i, lcd_box_t logical)
{
    const xiaotai_view_action_t *a = &v->actions[i];
    lcd_control_t *control = bind_action(p, v, (size_t)i);
    lcd_control_update(control, box(p, logical.x, logical.y, logical.width, logical.height), a->label,
                       p->overlay ? lcd_icon(a->icon) : action_icon(a), true, action_enabled(a), lcd_action_tone(a));
}

/* A dialog reads cancel first and the answer last, whatever order they came in. */
static void dialog_order(const xiaotai_view_t *v, layout_t *a)
{
    for (int i = 1; i < a->count; ++i) {
        const int moved = a->other[i];
        if (v->actions[moved].role != XIAOTAI_ROLE_BACK) continue;
        for (int j = i; j > 0; --j) a->other[j] = a->other[j - 1];
        a->other[0] = moved;
    }
}

static void classify(const lcd_pages_t *p, const xiaotai_view_t *v, layout_t *a)
{
    *a = (layout_t){.back = -1, .refresh = -1, .checked = -1};
    for (size_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        if (i >= v->action_count || !action_visible(p, &v->actions[i])) {
            lcd_visible(p->layer->action[i].obj, false);
            continue;
        }
        const xiaotai_view_action_t *action = &v->actions[i];
        const bool back = (action->role == XIAOTAI_ROLE_BACK || text_is(action->icon, XIAOTAI_ICON_BACK)) &&
                          action->style != XIAOTAI_STYLE_DESTRUCTIVE;
        /* A page's way back is the arrow in its header, and so is a chooser's;
         * a dialog that asks spells its answers out. */
        /* ... a call's is the red button that ends it. */
        const bool ends = p->kind == XIAOTAI_VIEW_CALL && !text_is(action->icon, XIAOTAI_ICON_BACK);
        const bool spelled = p->overlay && p->kind != XIAOTAI_VIEW_LIST && text_some(action->label);
        if (a->back < 0 && back && !ends && !spelled) a->back = (int)i;
        else if (p->kind != XIAOTAI_VIEW_CALL && a->refresh < 0 && text_is(action->icon, XIAOTAI_ICON_REFRESH) && !p->overlay)
            a->refresh = (int)i;
        else {
            if (p->kind == XIAOTAI_VIEW_NOTICE && (action->state & XIAOTAI_STATE_CHECKED)) a->checked = (int)a->count;
            a->other[a->count++] = (int)i;
        }
    }
    if (p->overlay) dialog_order(v, a);
}

/* Buttons side by side; a row holds fewer when their labels would not fit. */
static int per_row(const lcd_pages_t *p, const xiaotai_view_t *v, const layout_t *a, int width)
{
    int columns = min(a->count, 4);
    int32_t widest = 0;
    for (int i = 0; i < a->count; ++i) {
        lv_point_t size = {0};
        const char *text = v->actions[a->other[i]].label;
        if (text_some(text)) lv_text_get_size(&size, text, p->owner->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > widest) widest = size.x;
    }
    while (columns > 1 && widest + lcd_dp(p->owner, 12) > box(p, 0, 0, (width + FOOT_GAP) / columns - FOOT_GAP, 0).width)
        --columns;
    return columns;
}

/* Header icons, then tabs under the header or buttons along the bottom. */
static void place_actions(lcd_pages_t *p, const xiaotai_view_t *v, layout_t *a)
{
    if (a->back >= 0) header_action(p, v, a->back, 4);
    if (a->refresh >= 0) header_action(p, v, a->refresh, p->w - 44);
    a->title_left = a->back >= 0 ? 46 : MARGIN;
    a->title_right = a->refresh >= 0 ? p->w - 46 : p->w - MARGIN;
    a->top = HEADER_H;
    a->bottom = p->h - FOOT_BOTTOM;
    if (!a->count || p->kind == XIAOTAI_VIEW_CALL) return;
    const int width = p->w - 2 * MARGIN;
    if (a->checked >= 0) {
        const int columns = min(a->count, 4), rows = (a->count + columns - 1) / columns;
        for (int i = 0; i < a->count; ++i) {
            const int n = min(a->count - (i / columns) * columns, columns), stride = (width + ROW_GAP) / n;
            tab_action(p, v, a->other[i], (lcd_box_t){MARGIN + (i % columns) * stride,
                       HEADER_H + (i / columns) * (TAB_H + ROW_GAP), stride - ROW_GAP, TAB_H});
        }
        a->top = HEADER_H + rows * (TAB_H + ROW_GAP) + 2;
        return;
    }
    const int columns = per_row(p, v, a, width), rows = (a->count + columns - 1) / columns;
    const int start = p->h - FOOT_BOTTOM - rows * FOOT_H - (rows - 1) * FOOT_GAP;
    for (int i = 0; i < a->count; ++i) {
        const int n = min(a->count - (i / columns) * columns, columns), stride = (width + FOOT_GAP) / n;
        footer_action(p, v, a->other[i], (lcd_box_t){MARGIN + (i % columns) * stride,
                      start + (i / columns) * (FOOT_H + FOOT_GAP), stride - FOOT_GAP, FOOT_H});
    }
    a->bottom = start - FOOT_GAP;
}

static void apply_row(lcd_pages_t *p, row_t *row, const xiaotai_view_item_t *item, size_t index,
                      lcd_box_t area, bool grid, bool focused)
{
    lcd_state_t *s = p->owner;
    lcd_control_t *c = &row->control;
    if (!c->obj) {
        lcd_bind(c, s, p->scroller, p->index, XIAOTAI_UI_ELEMENT_ITEM, (uint16_t)index);
        row->detail = label(p, c->obj, NULL, LCD_MUTED, false);
        if (p->kind == XIAOTAI_VIEW_CODE) lv_label_set_long_mode(row->detail, LV_LABEL_LONG_SCROLL_CIRCULAR);
        row->icon = label(p, c->obj, s->m.icons, LCD_ACCENT, true);
        row->chevron = label(p, c->obj, s->m.icons, LCD_FAINT, true);
        lcd_text(row->chevron, LCD_ICON_NEXT);
        lv_obj_set_style_pad_all(c->obj, 0, 0);
    }
    const char *icon = lcd_icon(item->icon);
    const bool detail = text_some(item->detail);
    const bool list = p->kind == XIAOTAI_VIEW_LIST || item->intent;
    const bool enabled = item->intent && !(item->state & INERT);
    const bool selected = (item->state & XIAOTAI_STATE_CHECKED) || (focused && grid && enabled);
    row->selectable = item->intent != NULL;
    lcd_control_update(c, area, item->label, NULL, true, enabled, LCD_TONE_QUIET);
    lcd_visible(c->icon, false);
    lcd_visible(c->label, true);
    /* Something to operate is a raised row; plain information lies flat. */
    if (row->selectable) lcd_surface_style(s, c->obj, true);
    if (row->selectable && p->overlay) {
        lv_obj_set_style_bg_color(c->obj, lv_color_hex(LCD_SURFACE_HIGH), 0);
        lv_obj_set_style_bg_color(c->obj, lv_color_hex(LCD_OUTLINE), LV_STATE_PRESSED);
    }
    lv_obj_set_style_bg_opa(c->obj, row->selectable ? (enabled ? LV_OPA_COVER : LV_OPA_50) : LV_OPA_TRANSP, 0);
    /* The frame is drawn over the row and does not move what is in it. */
    const int32_t frame = selected ? lcd_dp(s, 2) : 0;
    lv_obj_set_style_border_width(c->obj, frame, 0);
    lv_obj_set_style_border_color(c->obj, lv_color_hex(LCD_ACCENT), 0);
    lv_obj_set_style_border_post(c->obj, true, 0);
    lv_obj_set_style_pad_all(c->obj, -frame, 0);

    const bool chevron = list && !grid && !p->overlay && enabled;
    const int32_t pad = row->selectable ? lcd_dp(s, 12) : 0, icon_w = icon ? lv_font_get_line_height(s->m.icons) : 0;
    const int32_t chevron_w = lv_font_get_line_height(s->m.icons);
    const int32_t start = pad + (icon ? icon_w + lcd_dp(s, 10) : 0), tail = chevron ? chevron_w + lcd_dp(s, 8) : 0;
    const int32_t h = lcd_line_height(s), room = max(1, area.width - start - pad - tail);
    const bool stacked = detail && list;        /* a name with a line under it */
    const bool pair = detail && !list;          /* "label   value" on one line */
    int32_t label_w = room;
    if (pair) {
        lv_point_t size;
        lv_text_get_size(&size, item->label ? item->label : "", s->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        label_w = size.x > p->label_column ? size.x : p->label_column;
        label_w = min(label_w, area.width * 9 / 20);
    }
    const uint32_t ink = !row->selectable ? (pair || p->kind == XIAOTAI_VIEW_CODE ? LCD_MUTED : LCD_TEXT) :
                         enabled ? (selected && grid ? LCD_ACCENT : LCD_TEXT) : LCD_FAINT;
    lv_obj_set_style_text_color(c->label, lv_color_hex(ink), 0);
    const bool centred = (grid && row->selectable) || (p->kind == XIAOTAI_VIEW_CODE && !pair) || (p->overlay && list);
    lv_obj_set_style_text_align(c->label, centred ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_align(c->label, LV_ALIGN_TOP_LEFT);
    lcd_place(c->label, (lcd_box_t){centred ? pad : start, stacked ? max(0, (area.height - 2 * h) / 2) :
              max(0, (area.height - h) / 2), centred ? max(1, area.width - 2 * pad) : label_w, h});
    if (detail) {
        const int32_t dx = stacked ? start : start + label_w + lcd_dp(s, 12);
        const int32_t dy = stacked ? max(0, (area.height - 2 * h) / 2) + h : max(0, (area.height - h) / 2);
        lcd_place(row->detail, (lcd_box_t){dx, dy, max(1, area.width - dx - pad - tail), h});
        lv_obj_set_style_text_color(row->detail, lv_color_hex(pair ? LCD_TEXT : enabled ? LCD_MUTED : LCD_FAINT), 0);
        lcd_text(row->detail, item->detail);
    }
    lcd_visible(row->detail, detail);
    lcd_visible(row->icon, icon != NULL);
    if (icon) {
        lv_obj_set_style_text_color(row->icon, lv_color_hex(enabled ? LCD_ACCENT : LCD_FAINT), 0);
        lcd_place(row->icon, (lcd_box_t){pad, max(0, (area.height - icon_w) / 2), icon_w, icon_w});
        lcd_text(row->icon, icon);
    }
    lcd_visible(row->chevron, chevron);
    if (chevron) lcd_place(row->chevron, (lcd_box_t){area.width - pad - chevron_w + lcd_dp(s, 4),
                                                    max(0, (area.height - chevron_w) / 2), chevron_w, chevron_w});
}

/* Lays the rows out below `body_px` and returns the display pixels they end at. */
static int32_t rows(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, int32_t body_px)
{
    const xiaotai_view_t *v = &layer->view;
    const bool list = p->kind == XIAOTAI_VIEW_LIST;
    const bool grid = list && (v->flags & XIAOTAI_VIEW_GRID);
    const int width = p->w - 2 * MARGIN;
    int y = 0, col = 0, last = 0;
    /* "label   value" rows share one value column. */
    p->label_column = 0;
    for (size_t i = 0; !list && i < v->item_count && i < XIAOTAI_VIEW_ITEMS_MAX; ++i) {
        lv_point_t size;
        if (!text_some(v->items[i].detail) || !text_some(v->items[i].label)) continue;
        lv_text_get_size(&size, v->items[i].label, p->owner->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > p->label_column) p->label_column = size.x;
    }
    for (size_t i = 0; i < XIAOTAI_VIEW_ITEMS_MAX; ++i) {
        row_t *row = &p->rows[i];
        if (i >= v->item_count || (v->items[i].state & XIAOTAI_STATE_HIDDEN)) {
            lcd_visible(row->control.obj, false);
            continue;
        }
        const xiaotai_view_item_t *item = &v->items[i];
        const bool cell = grid && item->intent;
        const bool tall = item->intent || (list && text_some(item->detail));
        const bool two = tall && !cell && text_some(item->detail);
        const int row_h = !tall ? (p->kind == XIAOTAI_VIEW_CODE ? 24 : INFO_H) : p->overlay ? 40 : two ? TWO_H : ROW_H;
        const int stride = row_h + (tall ? ROW_GAP : 0);
        if (!cell && col) { col = 0; y += stride; }
        const int cell_w = (width + ROW_GAP) / 3;
        lcd_box_t a = box(p, cell ? col * cell_w : 0, y, cell ? cell_w - ROW_GAP : width, row_h);
        a.y += body_px;
        apply_row(p, row, item, i, a, cell, layer->focus_kind == XIAOTAI_UI_FOCUS_ITEM && layer->focus == i);
        last = y + row_h;
        if (!cell || ++col == 3) { col = 0; y += stride; }
    }
    return body_px + box(p, 0, last, 0, 0).y;
}

static void timer_text(char *out, size_t capacity, int32_t seconds)
{
    const unsigned s = (unsigned)max(0, seconds);
    if (s >= 3600U) (void)snprintf(out, capacity, "%u:%02u:%02u", s / 3600U, s / 60U % 60U, s % 60U);
    else (void)snprintf(out, capacity, "%02u:%02u", s / 60U, s % 60U);
}

static void show_timer(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, int y)
{
    if (!p->timer) return;
    char value[20] = "";
    if (layer->view.timer_mode != XIAOTAI_TIMER_NONE) timer_text(value, sizeof(value), layer->timer_s);
    line(p, p->timer, value, MARGIN, y, p->w - 2 * MARGIN, 22);
}

static const xiaotai_ui_image_t *picture_find(const char *name)
{
    const xiaotai_ui_image_t *asset = name ? xiaotai_ui_image_find(name) : NULL;
    if (!asset || asset->width > UINT16_MAX / 2U) return NULL;
    if (asset->format == XIAOTAI_IMAGE_MONO1) return asset;
    if (asset->format != XIAOTAI_IMAGE_RGB565 && asset->format != XIAOTAI_IMAGE_RGB565A8) return NULL;
    const uint64_t bytes = (uint64_t)asset->width * asset->height * (asset->format == XIAOTAI_IMAGE_RGB565 ? 2U : 3U);
    return bytes <= UINT32_MAX ? asset : NULL;
}

static void picture_clear(lcd_pages_t *p)
{
    if (!p->image) return;
    lv_image_cache_drop(&p->image_desc);
    lv_image_header_cache_drop(&p->image_desc);
    xiaotai_mem_free(p->pixels);
    p->pixels = NULL;
    p->pixels_side = 0;
    p->image = NULL;
}

static bool mono_ink(const xiaotai_ui_image_t *m, unsigned x, unsigned y)
{
    return m->data[(size_t)y * ((m->width + 7U) / 8U) + x / 8U] & (0x80U >> (x % 8U));
}

static unsigned gcd(unsigned a, unsigned b)
{
    while (b) { const unsigned rest = a % b; a = b; b = rest; }
    return a;
}

/* The side of the squares a one-bit picture is made of: five pixels for a code
 * whose modules are five pixels wide, one for a drawing on no such grid. */
static unsigned mono_cell(const xiaotai_ui_image_t *m)
{
    unsigned cell = gcd(m->width, m->height);
    for (unsigned y = 0; y < m->height && cell > 1; ++y) {
        bool differs = false;
        for (unsigned x = 0; x < m->width; ++x) {
            if (x && mono_ink(m, x, y) != mono_ink(m, x - 1, y)) cell = gcd(cell, x);
            if (y) differs |= mono_ink(m, x, y) != mono_ink(m, x, y - 1);
        }
        if (differs) cell = gcd(cell, y);
    }
    return cell;
}

/* A one-bit picture is a code to scan: dark on a white plate, every module the
 * same whole number of pixels, as large as the plate takes. */
static bool mono_draw(lcd_pages_t *p, const xiaotai_ui_image_t *m, int32_t side)
{
    const unsigned cell = mono_cell(m), columns = m->width / cell, lines = m->height / cell;
    const unsigned longest = columns > lines ? columns : lines;
    unsigned width = m->width, height = m->height;
    if (longest <= (unsigned)side) {
        width = columns * ((unsigned)side / longest);
        height = lines * ((unsigned)side / longest);
    } else {
        width = (unsigned)max(1, (int)(m->width * (uint32_t)side / max(m->width, m->height)));
        height = (unsigned)max(1, (int)(m->height * (uint32_t)side / max(m->width, m->height)));
    }
    const size_t bytes = (size_t)width * height * 2U;
    p->pixels = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
    if (!p->pixels) return false;
    uint16_t *out = (uint16_t *)(void *)p->pixels;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            *out++ = mono_ink(m, (unsigned)((uint32_t)x * m->width / width), (unsigned)((uint32_t)y * m->height / height)) ?
                     0x0000 : 0xffff;
    p->image_desc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
        .w = width, .h = height, .stride = width * 2U}, .data_size = (uint32_t)bytes, .data = p->pixels};
    return true;
}

/* Shows the named picture in a square of `side` display pixels. */
static bool set_picture(lcd_pages_t *p, const char *name, int32_t side)
{
    const xiaotai_ui_image_t *asset = picture_find(name);
    const bool mono = asset && asset->format == XIAOTAI_IMAGE_MONO1;
    if (p->image == asset && (!mono || p->pixels_side == side)) return asset != NULL;
    lv_image_set_src(p->picture, NULL);
    picture_clear(p);
    if (!asset) return false;
    if (mono) {
        /* Inside the plate's rounded corners. */
        if (!mono_draw(p, asset, max(1, side - 2 * lcd_dp(p->owner, 4)))) return false;
        p->pixels_side = side;
    } else {
        const bool alpha = asset->format == XIAOTAI_IMAGE_RGB565A8;
        p->image_desc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
            .cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565,
            .w = asset->width, .h = asset->height, .stride = (uint32_t)asset->width * 2U},
            .data_size = (uint32_t)asset->width * asset->height * (alpha ? 3U : 2U), .data = asset->data};
    }
    p->image = asset;
    /* A code keeps its own pixels: no scale, not even one left by a picture before it. */
    lv_image_set_inner_align(p->picture, mono ? LV_IMAGE_ALIGN_CENTER : LV_IMAGE_ALIGN_CONTAIN);
    if (mono) lv_image_set_scale(p->picture, LV_SCALE_NONE);
    lv_image_set_src(p->picture, &p->image_desc);
    return true;
}

/* The page title, and beside it the short state of pages that keep their
 * second line for content. A state too long for that gets a line of its own
 * under the header; returns the logical height that takes. */
static int header(lcd_pages_t *p, const xiaotai_view_t *v, const layout_t *a, const char *state)
{
    const int width = a->title_right - a->title_left;
    lv_point_t size = {0}, title = {0};
    if (text_some(state)) lv_text_get_size(&size, state, p->owner->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (text_some(v->title)) lv_text_get_size(&title, v->title, p->owner->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int state_w = text_some(state) ? to_logical_x(p, size.x) + 2 : 0;
    const bool below = state_w && to_logical_x(p, title.x) + 8 + state_w > width;
    line(p, p->title, v->title, a->title_left, 0, max(1, width - (state_w && !below ? state_w + 8 : 0)), HEADER_H);
    if (p->kind == XIAOTAI_VIEW_LIST || p->kind == XIAOTAI_VIEW_CALL || p->kind == XIAOTAI_VIEW_PROGRESS) return 0;
    lv_obj_set_style_text_align(p->status, below ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    if (below) line(p, p->status, state, MARGIN, a->top, p->w - 2 * MARGIN, 24);
    else line(p, p->status, state, a->title_right - state_w, 0, max(1, state_w), HEADER_H);
    return below ? 26 : 0;
}

static void code(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, layout_t a)
{
    const xiaotai_view_t *v = &layer->view;
    const char *value = v->text ? v->text : "";
    const bool changed = strcmp(value, lv_label_get_text(p->value)) != 0;
    /* Leave a real row for explanation and timer, even with eight actions. */
    const bool timed = v->timer_mode != XIAOTAI_TIMER_NONE;
    const int number_h = to_logical(p, lv_font_get_line_height(p->owner->m.number));
    const int below = v->item_count ? 46 : p->overlay && !text_some(v->subtitle) ? 0 : 24;
    const int room = max(56, a.bottom - a.top - below - (timed ? 22 : 0));
    const int square = p->overlay ? room : min(136, room);
    const lcd_box_t extent = box(p, 0, 0, square, square);
    const int side = min(extent.width, extent.height);
    const lcd_box_t visual = {box(p, p->w, 0, 0, 0).x / 2 - side / 2, box(p, 0, a.top, 0, 0).y, side, side};
    const bool image = set_picture(p, v->image, side);
    const bool want_qr = !image && (v->flags & XIAOTAI_VIEW_QR) && *value;
    if (image) lcd_place(p->picture, visual);
    if (image && p->pixels) lcd_place(p->plate, visual);
    lcd_visible(p->plate, image && p->pixels);
    lcd_visible(p->picture, image);
#if LV_USE_QRCODE
    /* Leaving QR mode invalidates the payload, even though its canvas can
     * stay allocated. A named picture may cover a change of QR text. */
    if (!want_qr) p->qr_ready = false;
    if (want_qr && (!p->qr_ready || changed || p->qr_size != side || (layer->changed & XIAOTAI_CHANGED_FLAGS))) {
        if (p->qr_size != side) {
            lv_qrcode_set_size(p->qr, side);
            const lv_draw_buf_t *pixels = lv_canvas_get_draw_buf(p->qr);
            p->qr_size = pixels ? (int32_t)pixels->header.w : 0;
        }
        p->qr_ready = p->qr_size == side && lv_qrcode_update(p->qr, value, (uint32_t)strlen(value)) == LV_RESULT_OK;
    }
    if (want_qr && p->qr_ready) lcd_place(p->qr, visual);
#else
    (void)side;
    (void)changed;
#endif
    const bool qr = want_qr && p->qr_ready;
    lcd_visible(p->qr, qr);
    lcd_visible(p->value, !image && !qr);
    /* A code is drawn in the digit face; a payload that could not become a
     * picture and a code still on its way are plain text. */
    const bool digits = !want_qr && strspn(value, "0123456789") == strlen(value) && *value;
    lv_obj_set_style_text_font(p->value, digits ? p->owner->m.number : p->owner->m.text, 0);
    lv_obj_set_style_text_color(p->value, lv_color_hex(digits ? LCD_TEXT : LCD_FAINT), 0);
    const int value_h = min(max(number_h, 44), room);
    line(p, p->value, value, MARGIN, a.top, p->w - 2 * MARGIN, value_h);
    int y = a.top + (image || qr ? square + 2 : value_h);
    lv_obj_set_style_text_color(p->subtitle, lv_color_hex(LCD_TEXT), 0);
    line(p, p->subtitle, v->subtitle, MARGIN, y, p->w - 2 * MARGIN, 22);
    y += text_some(v->subtitle) ? 22 : 0;
    show_timer(p, layer, timed ? y : a.bottom);
    if (timed) y += 22;
    lcd_place(p->scroller, box(p, MARGIN, y + 2, p->w - 2 * MARGIN + BAR_W, max(1, a.bottom - y - 2)));
    (void)rows(p, layer, 0);
}

static void first_character(const char *name, char out[8])
{
    out[0] = '\0';
    if (!text_some(name)) return;
    size_t n = 1;
    const unsigned char lead = (unsigned char)name[0];
    if (lead >= 0xf0) n = 4; else if (lead >= 0xe0) n = 3; else if (lead >= 0xc0) n = 2;
    size_t i = 0;
    for (; i < n && name[i]; ++i) out[i] = name[i];
    out[i] = '\0';
}

/* The state of a call, and its timer beside it once it runs, centred as a pair. */
static void call_state(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, int y)
{
    const xiaotai_view_t *v = &layer->view;
    const char *state = v->text ? v->text : v->status;
    const bool timed = v->timer_mode != XIAOTAI_TIMER_NONE;
    lcd_visible(p->timer, timed);
    if (!timed) {
        line(p, p->status, state, 16, y, p->w - 32, 22);
        return;
    }
    char value[20];
    timer_text(value, sizeof(value), layer->timer_s);
    lv_point_t state_size = {0}, timer_size;
    if (text_some(state)) lv_text_get_size(&state_size, state, p->owner->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&timer_size, value, p->owner->m.text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    /* Room for an hour from the start: the pair does not jump at 59:59. */
    const int timer_w = to_logical_x(p, timer_size.x) + 4, gap = text_some(state) ? 10 : 0;
    const int state_w = min(to_logical_x(p, state_size.x) + 2, p->w - 32 - timer_w - gap);
    const int x = (p->w - state_w - gap - timer_w) / 2;
    line(p, p->status, state, x, y, max(1, state_w), 22);
    line(p, p->timer, value, x + state_w + gap, y, timer_w + 40, 22);
    lv_obj_set_style_text_align(p->timer, LV_TEXT_ALIGN_LEFT, 0);
}

/* Badge, name and state as one centred column; round buttons below. */
static void call(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, const layout_t *a)
{
    lcd_state_t *s = p->owner;
    const xiaotai_view_t *v = &layer->view;
    const lcd_box_t unit = box(p, 0, 0, 48, 48);
    /* Circles grow with the display, but not past what the text beside them carries. */
    const int32_t circle = min(min(unit.width, unit.height), lcd_line_height(s) * 17 / 5);
    const int label_h = to_logical(p, lcd_line_height(s)) + 2;
    const int buttons_h = to_logical(p, circle) + 2 + label_h;
    const int buttons_y = p->h - 6 - buttons_h;
    if (a->count) {
        const int stride = min(100, (p->w - 16) / a->count), start = (p->w - stride * a->count) / 2;
        for (int i = 0; i < a->count; ++i) {
            const xiaotai_view_action_t *action = &v->actions[a->other[i]];
            lcd_control_t *control = bind_action(p, v, (size_t)a->other[i]);
            const char *symbol = action_icon(action);
            /* A switched-off microphone or camera is the lit one; placing a call is green. */
            if (text_is(action->icon, XIAOTAI_ICON_ACCEPT)) symbol = LCD_ICON_PHONE;
            lcd_tone_t tone = lcd_action_tone(action);
            if (tone == LCD_TONE_QUIET || tone == LCD_TONE_ACCENT) tone = LCD_TONE_NEUTRAL;
            if (text_is(action->icon, XIAOTAI_ICON_MIC_OFF) || text_is(action->icon, XIAOTAI_ICON_CAMERA_OFF))
                tone = LCD_TONE_ACCENT;
            else if (text_is(action->icon, XIAOTAI_ICON_PHONE) && tone == LCD_TONE_NEUTRAL) tone = LCD_TONE_POSITIVE;
            const lcd_box_t area = box(p, start + i * stride + 2, buttons_y, stride - 4, buttons_h);
            if (symbol) lcd_round_update(control, area, circle, action->label, symbol, action_enabled(action), tone);
            else lcd_control_update(control, box(p, start + i * stride + 2, buttons_y + (buttons_h - FOOT_H) / 2,
                                    stride - 4, FOOT_H), action->label, NULL, true, action_enabled(action), tone);
        }
    }
    const int bottom = a->count ? buttons_y - 4 : p->h - 8;
    const bool timed = v->timer_mode != XIAOTAI_TIMER_NONE;
    const int text_h = 24 + 22;
    const int badge = min(min(64, to_logical(p, lcd_line_height(s) * 10 / 3)), max(36, bottom - 34 - text_h - 8));
    const int column = badge + 8 + text_h, top = 34 + max(0, (bottom - 34 - column) / 2);
    const bool video = (v->flags & XIAOTAI_VIEW_VIDEO) != 0;
    lcd_visible(p->title, !video);
    lcd_visible(p->avatar, !video);
    lcd_visible(p->halo, !video);
    lcd_visible(p->shade, video);
    if (video) {
        /* Over a picture: name and state at the top, on a shade that keeps
         * them readable, the buttons on one of their own. */
        lcd_place(p->shade, (lcd_box_t){0, 0, p->layer->canvas.width, box(p, 0, 58, 0, 0).y});
        line(p, p->subtitle, v->subtitle, 16, 6, p->w - 32, 24);
        call_state(p, layer, 30);
        lcd_move_drop(s, p->ring);
        return;
    }
    line(p, p->title, v->title, 16, 6, p->w - 32, 24);
    const lcd_box_t round = box(p, (p->w - badge) / 2, top, badge, badge);
    const int32_t side = min(min(round.width, round.height), lcd_line_height(s) * 10 / 3);
    const lcd_box_t disc = {round.x + (round.width - side) / 2, round.y + (round.height - side) / 2, side, side};
    lcd_place(p->avatar, disc);
    /* Room for the ring at its widest, one and a half times the badge. */
    lcd_place(p->halo, (lcd_box_t){disc.x - side / 4 - 2, disc.y - side / 4 - 2, side + side / 2 + 4, side + side / 2 + 4});
    char initial[8];
    first_character(v->subtitle, initial);
    lcd_text(p->initial, initial);
    line(p, p->subtitle, v->subtitle, 16, top + badge + 8, p->w - 32, 24);
    call_state(p, layer, top + badge + 32);
    /* Until the call runs its timer, somebody is waiting: the badge breathes.
     * A contact's page is drawn the same way but has nothing to end. */
    bool ends = false;
    for (int i = 0; i < a->count; ++i) ends |= v->actions[a->other[i]].style == XIAOTAI_STYLE_DESTRUCTIVE;
    const bool waiting = !timed && ends;
    lcd_visible(p->ring, waiting);
    if (waiting) {
        lv_obj_set_size(p->ring, side, side);
        lv_obj_center(p->ring);
        lcd_move_loop(s, p->ring, LCD_MOVE_PULSE, 1800);
    } else {
        lcd_move_drop(s, p->ring);
    }
}

static void progress(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, const layout_t *a, uint32_t now_ms)
{
    lcd_state_t *s = p->owner;
    const xiaotai_view_t *v = &layer->view;
    const bool determinate = (v->flags & XIAOTAI_VIEW_PERCENT) != 0;
    /* A step with a button waits for the user: it is not busy any more. */
    const bool busy = !determinate && !a->count;
    const int middle = (a->top + a->bottom) / 2;
    line(p, p->title, v->title, a->title_left, 0, a->title_right - a->title_left, HEADER_H);
    lv_obj_set_style_text_color(p->subtitle, lv_color_hex(LCD_TEXT), 0);
    lcd_visible(p->bar, determinate);
    lcd_visible(p->spinner, busy);
    lcd_visible(p->mark, !determinate && !busy);
    char percent[8] = "";
    if (determinate) {
        const lcd_box_t track = box(p, 32, middle - 22, p->w - 64, 8);
        const unsigned value = (unsigned)min(100, v->progress);
        lcd_place(p->bar, track);
        lv_obj_set_height(p->fill, track.height);
        lcd_visible(p->fill, value > 0);
        lcd_move_width(s, p->fill, (int32_t)(track.width * value / 100U), now_ms);
        (void)snprintf(percent, sizeof(percent), "%u%%", value);
        line(p, p->subtitle, v->text ? v->text : v->subtitle, MARGIN, middle - 4, p->w - 2 * MARGIN, 24);
        line(p, p->status, text_some(v->status) ? v->status : percent, MARGIN, middle + 20, p->w - 2 * MARGIN, 22);
        lcd_move_drop(s, p->spinner);
        return;
    }
    const lcd_box_t ring = box(p, p->w / 2 - 20, middle - 44, 40, 40);
    const int32_t side = min(ring.width, ring.height);
    const lcd_box_t centred = {ring.x + (ring.width - side) / 2, ring.y + (ring.height - side) / 2, side, side};
    lcd_place(p->spinner, centred);
    lcd_place(p->mark, (lcd_box_t){ring.x, ring.y + (ring.height - lv_font_get_line_height(s->m.icons)) / 2,
                                   ring.width, lv_font_get_line_height(s->m.icons)});
    if (busy) lcd_move_loop(s, p->spinner, LCD_MOVE_SPIN, 1100);
    else lcd_move_drop(s, p->spinner);
    line(p, p->subtitle, v->text ? v->text : v->subtitle, MARGIN, middle + 4, p->w - 2 * MARGIN, 24);
    line(p, p->status, v->status, MARGIN, middle + 28, p->w - 2 * MARGIN, 22);
}

/* A dialog is as tall as what it says, between a floor and the page. */
static void fit_dialog(lcd_pages_t *p, const xiaotai_view_t *v, const layout_t *a)
{
    if (!p->overlay) return;
    lcd_state_t *s = p->owner;
    const int32_t width = box(p, 0, 0, DIALOG_W - 2 * MARGIN, 0).width;
    int32_t content = 0;
    if (p->body) {
        lv_point_t size;
        const int32_t space = lcd_dp(s, 4);
        if (text_some(v->subtitle)) {
            lv_text_get_size(&size, v->subtitle, s->m.text, 0, space, width, LV_TEXT_FLAG_NONE);
            content += size.y + lcd_dp(s, 8);
        }
        if (text_some(v->text)) {
            lv_text_get_size(&size, v->text, s->m.text, 0, space, width, LV_TEXT_FLAG_NONE);
            content += size.y + lcd_dp(s, 8);
        }
    }
    int logical = to_logical(p, content);
    for (size_t i = 0; i < v->item_count && i < XIAOTAI_VIEW_ITEMS_MAX; ++i)
        if (!(v->items[i].state & XIAOTAI_STATE_HIDDEN)) logical += p->kind == XIAOTAI_VIEW_LIST ? 44 : INFO_H;
    if (p->kind == XIAOTAI_VIEW_LIST && text_some(v->status ? v->status : v->subtitle)) logical += 26;
    const int columns = a->count && a->checked < 0 ? per_row(p, v, a, DIALOG_W - 2 * MARGIN) : 1;
    const int rows = a->count && a->checked < 0 ? (a->count + columns - 1) / columns : 0;
    const int wanted = HEADER_H + logical + 4 + (rows ? rows * (FOOT_H + FOOT_GAP) : 0) + FOOT_BOTTOM;
    /* A code to scan is as large as a dialog can show it. */
    const bool scan = p->kind == XIAOTAI_VIEW_CODE && (picture_find(v->image) ||
                      ((v->flags & XIAOTAI_VIEW_QR) && text_some(v->text)));
    p->h = scan ? DIALOG_MAX : min(DIALOG_MAX, max(DIALOG_MIN, wanted));
    lcd_place(p->layer->content, box(p, (PAGE_W - DIALOG_W) / 2, (PAGE_H - p->h) / 2, DIALOG_W, p->h));
}

void lcd_pages_update(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, uint32_t now_ms)
{
    const xiaotai_view_t *v = &layer->view;
    if (layer->changed == XIAOTAI_CHANGED_TIMER) {
        if (p->timer) {
            char value[20];
            timer_text(value, sizeof(value), layer->timer_s);
            lcd_text(p->timer, value);
        }
        return;
    }
    layout_t a;
    classify(p, v, &a);
    fit_dialog(p, v, &a);
    place_actions(p, v, &a);
    p->geometry = a;
    p->laid_out = true;
    if (p->kind == XIAOTAI_VIEW_CALL) { call(p, layer, &a); return; }
    if (p->kind == XIAOTAI_VIEW_PROGRESS) { progress(p, layer, &a, now_ms); return; }
    a.top += header(p, v, &a, p->kind == XIAOTAI_VIEW_LIST ? NULL : v->status);
    if (p->kind == XIAOTAI_VIEW_CODE) { code(p, layer, a); return; }

    const bool list = p->kind == XIAOTAI_VIEW_LIST;
    size_t shown = 0;
    for (size_t i = 0; i < v->item_count && i < XIAOTAI_VIEW_ITEMS_MAX; ++i)
        shown += !(v->items[i].state & XIAOTAI_STATE_HIDDEN);
    /* A list's second line says what the list is about; with nothing to list
     * it becomes the message in the middle of the page. */
    const char *sub = list ? (v->subtitle ? v->subtitle : v->status) : NULL;
    const bool empty = list && !shown && text_some(sub);
    lcd_visible(p->subtitle, list && !empty);
    if (list && !empty) {
        lv_obj_set_style_text_align(p->subtitle, p->overlay ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
        line(p, p->subtitle, sub, MARGIN + 2, a.top, p->w - 2 * MARGIN - 4, 24);
    }
    const int top = a.top + (list && !empty && text_some(sub) ? 26 : 0);
    const lcd_box_t body = box(p, MARGIN, top, p->w - 2 * MARGIN, max(1, a.bottom - top));
    /* The scroll bar lives in the margin, beside the rows. */
    lcd_place(p->scroller, box(p, MARGIN, top, p->w - 2 * MARGIN + BAR_W, max(1, a.bottom - top)));
    const int32_t width = body.width;
    const char *intro = list ? NULL : v->subtitle, *text = list ? (empty ? sub : NULL) : v->text;
    lv_obj_set_width(p->intro, width);
    lcd_prose(p->owner, p->intro, intro, width);
    lcd_visible(p->intro, text_some(intro));
    lv_obj_set_width(p->body, width);
    lcd_prose(p->owner, p->body, text, width);
    lcd_visible(p->body, text_some(text));
    lv_obj_update_layout(p->scroller);
    int32_t used = 0;
    const int32_t gap = lcd_dp(p->owner, 8);
    if (text_some(intro)) used = lv_obj_get_height(p->intro) + gap;
    const int32_t text_h = text_some(text) ? lv_obj_get_height(p->body) : 0;
    /* A short message on a page of its own sits in the middle, like a dialog's. */
    const bool message = text_some(text) && !shown && a.checked < 0 && !text_some(intro) && text_h < body.height;
    lv_obj_set_style_text_align(p->body, message && !p->overlay ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_y(p->body, message && !p->overlay ? (body.height - text_h) * 2 / 5 : used);
    if (text_some(text)) used += text_h + gap;
    (void)rows(p, layer, used);
    if (p->checked_tab != a.checked) {
        p->checked_tab = a.checked;
        lv_obj_scroll_to_y(p->scroller, 0, LV_ANIM_OFF);
    }
}

uint32_t lcd_pages_tick(lcd_pages_t *p, uint32_t now_ms)
{
    (void)p;
    (void)now_ms;
    return 0;
}

size_t lcd_pages_controls(lcd_pages_t *p, xiaotai_ui_control_t *out, size_t capacity)
{
    if (!p || !p->scroller) return 0;
    lv_area_t viewport;
    lv_obj_get_coords(p->scroller, &viewport);
    size_t count = 0;
    for (size_t i = 0; i < XIAOTAI_VIEW_ITEMS_MAX; ++i) {
        xiaotai_ui_control_t row;
        if (!p->rows[i].selectable || !lcd_measure_control(&p->rows[i].control, &row)) continue;
        const int x = max(row.x, viewport.x1), y = max(row.y, viewport.y1);
        const int w = min(row.x + row.width, viewport.x2 + 1) - x;
        const int h = min(row.y + row.height, viewport.y2 + 1) - y;
        if (w <= 0 || h <= 0) continue;
        row.x = (int16_t)x; row.y = (int16_t)y;
        row.width = (uint16_t)w; row.height = (uint16_t)h;
        if (out && count < capacity) out[count] = row;
        ++count;
    }
    return count;
}

void lcd_pages_free(lcd_pages_t *p)
{
    if (!p) return;
    picture_clear(p);
    xiaotai_mem_free(p);
}
