// SPDX-License-Identifier: MIT
#include "lcd_pages.h"

#include <stdio.h>
#include <string.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "xiaotai/port/mem.h"

enum { PAGE_W = 320, PAGE_H = 240, ACTION_H = 36, INERT = XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY };

typedef struct {
    lcd_control_t control;
    lv_obj_t *detail, *icon, *chevron;
    bool selectable;
} row_t;

typedef struct { int top, bottom, title_left, title_width, checked; } layout_t;

struct lcd_pages {
    lcd_state_t *owner;
    lcd_layer_t *layer;
    lcd_layout_t layout;
    xiaotai_view_kind_t kind;
    uint8_t index;
    lv_obj_t *title, *subtitle, *status, *timer, *scroller, *intro, *body, *picture, *value, *bar, *fill, *qr;
    lv_obj_t *avatar, *initial;   /* CALL: a round badge with the first character of the name */
    row_t rows[XIAOTAI_VIEW_ITEMS_MAX];
    const xiaotai_ui_image_t *image;
    lv_image_dsc_t image_desc;
    layout_t geometry;
    int32_t qr_size;
    int checked_tab;
    uint32_t progress_epoch;
    bool laid_out, qr_ready, moving, progress_started;
};

static lcd_box_t box(const lcd_pages_t *p, int x, int y, int w, int h)
{
    return lcd_scale(&p->layout, (lcd_box_t){x, y, w, h});
}

static bool text_is(const char *s, const char *value) { return s && !strcmp(s, value); }
static bool text_some(const char *s) { return s && *s; }
static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, bool centered)
{
    lv_obj_t *obj = lcd_label(parent, font, color);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    if (centered) lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    return obj;
}

static void line(lcd_pages_t *p, lv_obj_t *obj, const char *text, int x, int y, int w, int h)
{
    lcd_place(obj, box(p, x, y, w, h));
    lcd_text(obj, text);
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
    p->layout = (lcd_layout_t){layer->canvas.width, layer->canvas.height};
    p->kind = kind;
    p->index = index;
    p->checked_tab = -1;

    lv_obj_t *root = layer->content;
    p->title = label(root, lcd_body_font(), kind == XIAOTAI_VIEW_CALL ? LCD_MUTED : LCD_TEXT, kind == XIAOTAI_VIEW_CALL);
    p->subtitle = label(root, lcd_body_font(), kind == XIAOTAI_VIEW_CALL ? LCD_TEXT : LCD_ACCENT,
                        kind != XIAOTAI_VIEW_LIST);
    if (kind == XIAOTAI_VIEW_CALL) {
        p->avatar = lcd_panel(root, (lcd_box_t){0}, LCD_SURFACE_HIGH, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_color(p->avatar, lv_color_hex(LCD_ACCENT), 0);
        lv_obj_set_style_border_width(p->avatar, 2, 0);
        p->initial = label(p->avatar, lcd_body_font(), LCD_ACCENT, true);
        lv_obj_set_width(p->initial, lv_pct(100));
        lv_obj_center(p->initial);
    }
    p->status = label(root, lcd_body_font(), LCD_MUTED, kind != XIAOTAI_VIEW_LIST);
    if (kind == XIAOTAI_VIEW_CALL || kind == XIAOTAI_VIEW_CODE) p->timer = label(root, lcd_body_font(), LCD_ACCENT, true);
    if (kind == XIAOTAI_VIEW_LIST || kind == XIAOTAI_VIEW_NOTICE || kind == XIAOTAI_VIEW_CODE) {
        p->scroller = lcd_panel(root, (lcd_box_t){0}, 0, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(p->scroller, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(p->scroller, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(p->scroller, LV_SCROLLBAR_MODE_AUTO);
    }
    if (kind == XIAOTAI_VIEW_NOTICE) {
        p->intro = label(p->scroller, lcd_body_font(), LCD_ACCENT, false);
        p->body = label(p->scroller, lcd_body_font(), LCD_TEXT, false);
        lv_label_set_long_mode(p->intro, LV_LABEL_LONG_WRAP);
        lv_label_set_long_mode(p->body, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(p->intro, LV_SIZE_CONTENT);
        lv_obj_set_height(p->body, LV_SIZE_CONTENT);
        lv_obj_set_style_text_line_space(p->intro, 4, 0);
        lv_obj_set_style_text_line_space(p->body, 4, 0);
    } else if (kind == XIAOTAI_VIEW_CODE) {
        const lv_font_t *font = lcd_body_font();
#if LV_FONT_MONTSERRAT_48
        font = &lv_font_montserrat_48;
#endif
        p->value = label(root, font, LCD_TEXT, true);
        p->picture = lv_image_create(root);
        lv_image_set_inner_align(p->picture, LV_IMAGE_ALIGN_CONTAIN);
        lcd_visible(p->picture, false);
#if LV_USE_QRCODE
        p->qr = lv_qrcode_create(root);
        lv_qrcode_set_quiet_zone(p->qr, true);
        lv_qrcode_set_dark_color(p->qr, lv_color_black());
        lv_qrcode_set_light_color(p->qr, lv_color_white());
        lcd_visible(p->qr, false);
#endif
    } else if (kind == XIAOTAI_VIEW_PROGRESS) {
        p->bar = lcd_panel(root, (lcd_box_t){0}, LCD_SURFACE_HIGH, LV_OPA_COVER, 8);
        p->fill = lcd_panel(p->bar, (lcd_box_t){0}, LCD_ACCENT, LV_OPA_COVER, 8);
    }
    return p;
}

static const char *action_icon(const xiaotai_view_action_t *a)
{
    return a->role == XIAOTAI_ROLE_BACK && !text_some(a->icon) ? LV_SYMBOL_LEFT : lcd_icon(a->icon);
}

static bool action_visible(const xiaotai_view_action_t *a)
{
    return !(a->state & XIAOTAI_STATE_HIDDEN) && (text_some(a->label) || action_icon(a));
}

static void action(lcd_pages_t *p, const xiaotai_view_t *v, size_t i, lcd_box_t logical, bool tab, bool header)
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
    const bool selected = (a->state & XIAOTAI_STATE_CHECKED) != 0;
    const bool call = p->kind == XIAOTAI_VIEW_CALL;
    const char *symbol = action_icon(a);
    /* On a call page answering shows a handset, and every button is a real
     * button: a quiet mute control there read as stray text. */
    if (call && text_is(a->icon, XIAOTAI_ICON_ACCEPT)) symbol = LV_SYMBOL_CALL;
    lcd_control_update(control, box(p, logical.x, logical.y, logical.width, logical.height),
        header && symbol ? NULL : a->label, symbol, true, a->intent && !(a->state & INERT),
        (header || tab || (a->style == XIAOTAI_STYLE_QUIET && !call)) && !(tab && selected), a->style);
    lv_obj_set_style_border_width(control->obj, tab && selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(control->obj, lv_color_hex(LCD_ACCENT), 0);
    lv_obj_set_style_border_side(control->obj, LV_BORDER_SIDE_BOTTOM, 0);
}

static layout_t actions(lcd_pages_t *p, const xiaotai_view_t *v)
{
    int back = -1, refresh = -1, checked = -1, other[XIAOTAI_VIEW_ACTIONS_MAX];
    int count = 0;
    const bool headers = p->kind == XIAOTAI_VIEW_LIST || p->kind == XIAOTAI_VIEW_CODE || p->kind == XIAOTAI_VIEW_NOTICE;
    for (size_t i = 0; i < XIAOTAI_VIEW_ACTIONS_MAX; ++i) {
        if (i >= v->action_count || !action_visible(&v->actions[i])) {
            lcd_visible(p->layer->action[i].obj, false);
            continue;
        }
        const xiaotai_view_action_t *a = &v->actions[i];
        if (headers && back < 0 && (a->role == XIAOTAI_ROLE_BACK || text_is(a->icon, XIAOTAI_ICON_BACK))) back = (int)i;
        else if (headers && refresh < 0 && text_is(a->icon, XIAOTAI_ICON_REFRESH)) refresh = (int)i;
        else {
            if (p->kind == XIAOTAI_VIEW_NOTICE && (a->state & XIAOTAI_STATE_CHECKED)) checked = (int)i;
            other[count++] = (int)i;
        }
    }
    if (back >= 0) action(p, v, (size_t)back, (lcd_box_t){8, 4, 40, 32}, false, true);
    if (refresh >= 0) action(p, v, (size_t)refresh, (lcd_box_t){272, 4, 40, 32}, false, true);
    const int rows = (count + 3) / 4;
    const bool tabs = checked >= 0;
    const int start = tabs ? 39 : PAGE_H - 4 - rows * ACTION_H;
    for (int i = 0; i < count; ++i) {
        const int n = min(count - (i / 4) * 4, 4), stride = 304 / n;
        action(p, v, (size_t)other[i], (lcd_box_t){8 + (i % 4) * stride, start + (i / 4) * ACTION_H,
               stride - (n > 1 ? 4 : 0), ACTION_H - 4}, tabs, false);
    }
    const int left = back >= 0 ? 54 : 16, right = refresh >= 0 ? 270 : 304;
    return (layout_t){tabs ? start + rows * ACTION_H : 40, tabs || !rows ? 236 : start - 3, left, right - left, checked};
}

static void apply_row(lcd_pages_t *p, row_t *row, const xiaotai_view_item_t *item, size_t index,
                      lcd_box_t area, bool grid, bool focused)
{
    lcd_control_t *c = &row->control;
    if (!c->obj) {
        lcd_bind(c, p->owner, p->scroller, p->index, XIAOTAI_UI_ELEMENT_ITEM, (uint16_t)index);
        row->detail = label(c->obj, lcd_body_font(), LCD_MUTED, false);
        if (p->kind == XIAOTAI_VIEW_CODE) lv_label_set_long_mode(row->detail, LV_LABEL_LONG_SCROLL_CIRCULAR);
        row->icon = label(c->obj, LV_FONT_DEFAULT, LCD_ACCENT, true);
        row->chevron = label(c->obj, LV_FONT_DEFAULT, LCD_MUTED, true);
        lcd_text(row->chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_pad_all(c->obj, 0, 0);
        lv_obj_set_style_text_align(c->label, LV_TEXT_ALIGN_LEFT, 0);
    }
    const char *icon = lcd_icon(item->icon);
    const bool detail = text_some(item->detail);
    const bool list = p->kind == XIAOTAI_VIEW_LIST;
    const bool enabled = item->intent && !(item->state & INERT);
    const bool selected = (item->state & XIAOTAI_STATE_CHECKED) || (focused && grid && enabled);
    row->selectable = item->intent != NULL;
    const bool colon = !list && detail && text_some(item->label);
    const char *text = item->label;
    const char *shown = lv_label_get_text(c->label);
    if (colon) {
        const size_t len = strlen(text);
        if (strlen(shown) == len + 1 && !strncmp(shown, text, len) && shown[len] == ':') text = shown;
    }
    lcd_control_update(c, area, text, NULL, true, enabled, !item->intent, XIAOTAI_STYLE_NORMAL);
    lv_obj_set_style_border_width(c->obj, selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(c->obj, lv_color_hex(LCD_ACCENT), 0);
    lv_obj_set_style_border_post(c->obj, true, 0);
    if (!item->intent) lv_obj_set_style_text_color(c->label, lv_color_hex(LCD_ACCENT), 0);

    /* A row that opens something shows a chevron; grid cells and plain information do not. */
    const bool chevron = list && !grid && enabled;
    const int gap = icon ? 10 : 6, icon_w = icon ? 22 : 0, start = gap + icon_w, tail = chevron ? 24 : 0;
    const int label_w = !list && detail ? max(1, area.width * 2 / 5 - start) : max(1, area.width - start - gap - tail);
    const int h = lv_font_get_line_height(lcd_body_font());
    lv_obj_set_align(c->label, LV_ALIGN_TOP_LEFT);
    lcd_place(c->label, (lcd_box_t){start, (detail && list) ? max(0, (area.height - 2 * h) / 2) : max(0, (area.height - h) / 2), label_w, h});
    if (colon && text == item->label) lv_label_set_text_fmt(c->label, "%s:", item->label);
    if (detail) {
        const int dx = list ? start : area.width * 2 / 5 + 4;
        const int dy = list ? max(0, (area.height - 2 * h) / 2) + h : max(0, (area.height - h) / 2);
        lcd_place(row->detail, (lcd_box_t){dx, dy, max(1, area.width - dx - gap - tail), h});
        lcd_text(row->detail, item->detail);
    }
    lcd_visible(row->detail, detail);
    lcd_visible(row->icon, icon != NULL);
    if (icon) {
        lcd_place(row->icon, (lcd_box_t){8, max(0, (area.height - h) / 2), icon_w, h});
        lcd_text(row->icon, icon);
    }
    lcd_visible(row->chevron, chevron);
    if (chevron) lcd_place(row->chevron, (lcd_box_t){area.width - tail, max(0, (area.height - h) / 2), 16, h});
}

static void rows(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, int body_px)
{
    const xiaotai_view_t *v = &layer->view;
    const bool grid = p->kind == XIAOTAI_VIEW_LIST && (v->flags & XIAOTAI_VIEW_GRID);
    const int row_h = p->kind == XIAOTAI_VIEW_LIST ? 48 : 30;
    int y = 0, col = 0;
    for (size_t i = 0; i < XIAOTAI_VIEW_ITEMS_MAX; ++i) {
        row_t *row = &p->rows[i];
        if (i >= v->item_count || (v->items[i].state & XIAOTAI_STATE_HIDDEN)) {
            lcd_visible(row->control.obj, false);
            continue;
        }
        const xiaotai_view_item_t *item = &v->items[i];
        const bool cell = grid && item->intent;
        if (!cell && col) { col = 0; y += row_h; }
        lcd_box_t a = box(p, cell ? col * 98 : 0, y, cell ? 94 : 292, row_h - 4);
        a.y += body_px;
        apply_row(p, row, item, i, a, cell, layer->focus_kind == XIAOTAI_UI_FOCUS_ITEM && layer->focus == i);
        if (!cell || ++col == 3) { col = 0; y += row_h; }
    }
}

static void show_timer(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, int y)
{
    if (!p->timer) return;
    char value[20] = "";
    if (layer->view.timer_mode != XIAOTAI_TIMER_NONE) {
        const unsigned seconds = (unsigned)max(0, layer->timer_s);
        (void)snprintf(value, sizeof(value), "%02u:%02u", seconds / 60U, seconds % 60U);
    }
    line(p, p->timer, value, 20, y, 280, 24);
}

static bool set_picture(lcd_pages_t *p, const char *name)
{
    const xiaotai_ui_image_t *asset = name ? xiaotai_ui_image_find(name) : NULL;
    if (asset && asset->format != XIAOTAI_IMAGE_RGB565 && asset->format != XIAOTAI_IMAGE_RGB565A8) asset = NULL;
    if (asset && (asset->width > UINT16_MAX / 2U ||
        (uint64_t)asset->width * asset->height * (asset->format == XIAOTAI_IMAGE_RGB565 ? 2U : 3U) > UINT32_MAX))
        asset = NULL;
    if (p->image == asset) return asset != NULL;
    lv_image_set_src(p->picture, NULL);
    lv_image_cache_drop(&p->image_desc);
    lv_image_header_cache_drop(&p->image_desc);
    p->image = asset;
    if (!asset) return false;
    const bool alpha = asset->format == XIAOTAI_IMAGE_RGB565A8;
    p->image_desc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
        .cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565,
        .w = asset->width, .h = asset->height, .stride = (uint32_t)asset->width * 2U},
        .data_size = (uint32_t)asset->width * asset->height * (alpha ? 3U : 2U), .data = asset->data};
    lv_image_set_src(p->picture, &p->image_desc);
    return true;
}

static void code(lcd_pages_t *p, const xiaotai_ui_layer_t *layer, layout_t a)
{
    const xiaotai_view_t *v = &layer->view;
    const char *value = v->text ? v->text : "";
    const bool changed = strcmp(value, lv_label_get_text(p->value)) != 0;
    const bool image = set_picture(p, v->image);
    const bool want_qr = !image && (v->flags & XIAOTAI_VIEW_QR) && *value;
    /* Leave a real row for explanation and timer, even with eight actions. */
    const bool timed = v->timer_mode != XIAOTAI_TIMER_NONE;
    const int room = max(56, a.bottom - a.top - (v->item_count ? 45 : 24) - (timed ? 23 : 0));
    const int square = min(136, room);
    const lcd_box_t extent = box(p, 0, 0, square, square);
    const int side = min(extent.width, extent.height);
    const lcd_box_t visual = {(p->layout.width - side) / 2, box(p, 0, a.top, 0, 0).y, side, side};
    if (image) lcd_place(p->picture, visual);
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
    lv_obj_set_style_text_font(p->value, want_qr ? lcd_body_font() :
#if LV_FONT_MONTSERRAT_48
        &lv_font_montserrat_48,
#else
        lcd_body_font(),
#endif
        0);
    line(p, p->value, value, 12, a.top, 296, min(60, room));
    int y = a.top + (image || qr ? square : min(60, room));
    line(p, p->subtitle, v->subtitle, 10, y, 300, 24);
    y += text_some(v->subtitle) ? 24 : 0;
    show_timer(p, layer, timed ? y : a.bottom);
    if (timed) y += 24;
    lcd_place(p->scroller, box(p, 12, y, 296, max(1, a.bottom - y)));
    rows(p, layer, 0);
}

void lcd_pages_update(lcd_pages_t *p, const xiaotai_ui_layer_t *layer)
{
    const xiaotai_view_t *v = &layer->view;
    if (layer->changed == XIAOTAI_CHANGED_TIMER) {
        if (p->timer) {
            const unsigned seconds = (unsigned)max(0, layer->timer_s);
            char value[20];
            (void)snprintf(value, sizeof(value), "%02u:%02u", seconds / 60U, seconds % 60U);
            lcd_text(p->timer, value);
        }
        return;
    }
    const bool reflow = !p->laid_out || (layer->changed & (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_ACTIONS));
    if (reflow) {
        p->geometry = actions(p, v);
        p->laid_out = true;
    }
    const layout_t a = p->geometry;
    if (reflow || (layer->changed & XIAOTAI_CHANGED_TITLE))
        line(p, p->title, v->title, a.title_left, 9, a.title_width, 24);
    if (p->kind == XIAOTAI_VIEW_CODE) { code(p, layer, a); return; }
    if (p->kind == XIAOTAI_VIEW_CALL) {
        /* Badge, name, state and timer as one centred column above the buttons. */
        const int inner = a.bottom - a.top, badge = min(56, max(36, inner - 92));
        const int column = badge + 6 + 26 + 24 + 24, top = a.top + max(0, (inner - column) / 2);
        const lcd_box_t round = box(p, (PAGE_W - badge) / 2, top, badge, badge);
        const int side = min(round.width, round.height);
        lcd_place(p->avatar, (lcd_box_t){round.x + (round.width - side) / 2, round.y, side, side});
        char initial[8] = "";
        const char *name = v->subtitle;
        if (text_some(name)) {
            size_t n = 1;
            const unsigned char lead = (unsigned char)name[0];
            if (lead >= 0xf0) n = 4; else if (lead >= 0xe0) n = 3; else if (lead >= 0xc0) n = 2;
            for (size_t i = 0; i < n && name[i]; ++i) initial[i] = name[i];
        }
        lcd_text(p->initial, initial);
        line(p, p->subtitle, v->subtitle, 16, top + badge + 6, 288, 26);
        line(p, p->status, v->text ? v->text : v->status, 16, top + badge + 32, 288, 24);
        show_timer(p, layer, top + badge + 56);
        return;
    }
    if (p->kind == XIAOTAI_VIEW_PROGRESS) {
        line(p, p->subtitle, v->text ? v->text : v->subtitle, 14, 68, 292, 26);
        const bool determinate = (v->flags & XIAOTAI_VIEW_PERCENT) != 0;
        if (p->moving != !determinate) p->progress_started = false;
        p->moving = !determinate;
        lcd_visible(p->bar, determinate);
        if (determinate) {
            const lcd_box_t track = box(p, 24, 114, 272, 12);
            const unsigned percent = (unsigned)min(100, v->progress);
            lcd_place(p->bar, track);
            lcd_place(p->fill, (lcd_box_t){0, 0, (int32_t)(track.width * percent / 100U), track.height});
            lcd_visible(p->fill, percent > 0);
            char text[8];
            (void)snprintf(text, sizeof(text), "%u%%", percent);
            line(p, p->status, text, 14, 136, 292, 24);
        } else {
            lcd_place(p->status, box(p, 14, 114, 292, 24));
            if (!p->progress_started) lcd_text(p->status, "");
        }
        return;
    }
    const char *sub = p->kind == XIAOTAI_VIEW_NOTICE ? v->status : v->subtitle ? v->subtitle : v->status;
    line(p, p->subtitle, sub, 14, a.top, 292, 24);
    const int top = a.top + (text_some(sub) ? 26 : 0);
    lcd_place(p->scroller, box(p, 12, top, 296, max(1, a.bottom - top)));
    int body = 0;
    if (p->body) {
        const int width = box(p, 0, 0, 292, 0).width;
        lv_obj_set_width(p->intro, width);
        lcd_text(p->intro, v->subtitle);
        lcd_visible(p->intro, text_some(v->subtitle));
        lv_obj_set_width(p->body, width);
        lcd_text(p->body, v->text);
        lcd_visible(p->body, text_some(v->text));
        lv_obj_update_layout(p->scroller);
        if (text_some(v->subtitle)) body = lv_obj_get_height(p->intro) + 8;
        lv_obj_set_y(p->body, body);
        if (text_some(v->text)) body += lv_obj_get_height(p->body) + 8;
    }
    rows(p, layer, body);
    if (p->checked_tab != a.checked) {
        p->checked_tab = a.checked;
        lv_obj_scroll_to_y(p->scroller, 0, LV_ANIM_OFF);
    }
}

uint32_t lcd_pages_tick(lcd_pages_t *p, uint32_t now_ms)
{
    if (!p || !p->moving) return 0;
    if (!p->progress_started) {
        p->progress_epoch = now_ms;
        p->progress_started = true;
    }
    static const char *const dots[] = {"", ".", "..", "..."};
    const uint32_t elapsed = now_ms - p->progress_epoch;
    lcd_text(p->status, dots[(elapsed / 500U) % 4U]);
    return 500U - elapsed % 500U;
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
    if (p->image) {
        lv_image_cache_drop(&p->image_desc);
        lv_image_header_cache_drop(&p->image_desc);
    }
    xiaotai_mem_free(p);
}
