// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "lcd_clock.h"
#include "lcd_face_live.h"
#include "lcd_form.h"
#include "lcd_internal.h"
#include "lcd_pages.h"
#include "xiaotai/port/mem.h"

LV_FONT_DECLARE(ui_font_cn_18);
LV_FONT_DECLARE(lcd_icons_20);
LV_FONT_DECLARE(lcd_font_num_48);
LV_FONT_DECLARE(lcd_font_num_64);
#if XIAOTAI_THEME_LCD_LARGE_TEXT
LV_FONT_DECLARE(lcd_font_cn_26);
LV_FONT_DECLARE(lcd_icons_28);
LV_FONT_DECLARE(lcd_font_num_96);
LV_FONT_DECLARE(lcd_font_num_160);
#endif

/* The faces of a display class. A display of 800 x 480 and more is read from
 * further away: where the product carries the 26 px face it gets that, the
 * larger icons and the larger digits; every other display uses the 18 px set. */
static lcd_metrics_t metrics(const xiaotai_display_desc_t *desc)
{
#if XIAOTAI_THEME_LCD_LARGE_TEXT
    if (desc->width >= 800 && desc->height >= 480)
        return (lcd_metrics_t){.text = &lcd_font_cn_26, .icons = &lcd_icons_28, .number = &lcd_font_num_96,
                               .clock = &lcd_font_num_160, .dp = 23};
#else
    (void)desc;
#endif
    return (lcd_metrics_t){.text = &ui_font_cn_18, .icons = &lcd_icons_20, .number = &lcd_font_num_48,
                           .clock = &lcd_font_num_64, .dp = 16};
}

static void park_video(lcd_state_t *s)
{
    if (!s->video) return;
    lcd_visible(s->video, false);
    if (lv_obj_get_parent(s->video) != s->root) lv_obj_set_parent(s->video, s->root);
}

static xiaotai_err_t bind_video(lcd_state_t *s, bool bound)
{
    if (!s->video || s->video_bound == bound) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_ui_host_video_region(s->host, bound ? s->video : NULL);
    if (err == XIAOTAI_OK) s->video_bound = bound;
    return err;
}

static void clear_layer(lcd_state_t *s, lcd_layer_t *layer)
{
    if (layer->root) lcd_move_drop(s, layer->root);
    if (layer->face) lcd_face_live_detach(s);
    if (s->video && layer->root && lv_obj_get_parent(s->video) == layer->root) park_video(s);
    if (layer->custom && layer->custom->close) layer->custom->close(layer->custom_state);
    if (layer->root) lv_obj_delete(layer->root);
    lcd_pages_free(layer->pages);
    lcd_form_free(layer);
    lcd_clock_free(layer->desk);
    *layer = (lcd_layer_t){0};
}

xiaotai_err_t xiaotai_theme_lcd_start(const xiaotai_ui_host_t *host, const xiaotai_theme_lcd_variant_t *variant,
                                      void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (!host) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_display_desc_t *desc = xiaotai_ui_host_display(host);
    lv_display_t *display = xiaotai_ui_host_root(host);
    if (!display || !desc || desc->pixel != XIAOTAI_PIXEL_RGB565 || desc->width < XIAOTAI_THEME_LCD_LOGICAL_WIDTH ||
        desc->height < XIAOTAI_THEME_LCD_LOGICAL_HEIGHT || (desc->flags & XIAOTAI_DISPLAY_SLOW_REFRESH))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    lcd_state_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    s->host = host;
    s->variant = variant;
    s->layout = (lcd_layout_t){desc->width, desc->height};
    s->m = metrics(desc);
    const xiaotai_err_t face = lcd_face_live_start(s);
    if (face != XIAOTAI_OK) {
        xiaotai_mem_free(s);
        return face;
    }
    s->root = lcd_panel(lv_display_get_screen_active(display), (lcd_box_t){0, 0, desc->width, desc->height},
                        LCD_BACKGROUND, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s->root, s->m.text, 0);
    if (variant && variant->video_region) {
        s->video = variant->video_region(variant->self, host, s->root);
        park_video(s);
        const xiaotai_err_t err = s->video ? bind_video(s, true) : XIAOTAI_ERR_NO_MEM;
        if (err != XIAOTAI_OK) {
            lv_obj_delete(s->root);
            lcd_face_live_stop(s);
            xiaotai_mem_free(s);
            return err;
        }
    }
    *state = s;
    return XIAOTAI_OK;
}

static void overlay_background(lv_event_t *event)
{
    const lcd_state_t *s = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) != s->layers[XIAOTAI_LAYER_OVERLAY].root) return;
    xiaotai_ui_input_t input;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: input = XIAOTAI_UI_INPUT_PRESS; break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: input = XIAOTAI_UI_INPUT_RELEASE; break;
    case LV_EVENT_CLICKED: input = XIAOTAI_UI_INPUT_TAP; break;
    default: return;
    }
    xiaotai_ui_host_input(s->host, XIAOTAI_LAYER_OVERLAY, XIAOTAI_UI_ELEMENT_OTHER, 0, input, 0, NULL);
}

static void gesture(lv_event_t *event)
{
    const lcd_state_t *s = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    /* What a finger drags along keeps the drag: a slider. */
    lv_obj_t *const touched = lv_indev_get_active_obj();
    if (touched && lv_obj_is_editable(touched)) return;
    xiaotai_ui_input_t input;
    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_LEFT: input = XIAOTAI_UI_INPUT_SWIPE_LEFT; break;
    case LV_DIR_RIGHT: input = XIAOTAI_UI_INPUT_SWIPE_RIGHT; break;
    case LV_DIR_TOP: input = XIAOTAI_UI_INPUT_SWIPE_UP; break;
    case LV_DIR_BOTTOM: input = XIAOTAI_UI_INPUT_SWIPE_DOWN; break;
    default: return;
    }
    const uint8_t layer = lv_event_get_current_target_obj(event) == s->layers[XIAOTAI_LAYER_OVERLAY].root ?
                          XIAOTAI_LAYER_OVERLAY : XIAOTAI_LAYER_PAGE;
    xiaotai_ui_host_input(s->host, layer, XIAOTAI_UI_ELEMENT_OTHER, 0, input, 0, NULL);
}

static void fallback_layout(lcd_state_t *s, lcd_layer_t *out, bool overlay)
{
    const int32_t width = out->canvas.width;
    const int32_t height = out->canvas.height;
    const int32_t line = lcd_line_height(s), pad = lcd_dp(s, 16);
    out->title = lcd_label(out->content, s->m.text, LCD_TEXT);
    lcd_place(out->title, (lcd_box_t){pad, lcd_dp(s, 10), width - 2 * pad, line});
    lv_label_set_long_mode(out->title, LV_LABEL_LONG_DOT);
    out->status = lcd_label(out->content, s->m.text, LCD_MUTED);
    lcd_place(out->status, (lcd_box_t){pad, lcd_dp(s, 12) + line, width - 2 * pad, line});
    lv_label_set_long_mode(out->status, LV_LABEL_LONG_DOT);
    out->body = lcd_label(out->content, s->m.text, LCD_MUTED);
    const int32_t top = lcd_dp(s, 18) + 2 * line;
    lcd_place(out->body, (lcd_box_t){pad, top, width - 2 * pad, height - top > 3 * line ? height - top - 2 * line : line});
    lv_label_set_long_mode(out->body, LV_LABEL_LONG_WRAP);
    if (overlay) lv_obj_set_style_text_align(out->body, LV_TEXT_ALIGN_CENTER, 0);
}

/* The pages are drawn in units of the 320 x 240 design and scaled; text is
 * not. A page fills the screen while that keeps it within twice the design
 * size. On anything larger the scale stops (at 1.75 with the 18 px face, at 2
 * with the 26 px one) and the page grows instead: it uses the whole height,
 * so a list shows more rows, in a centred column of readable width. The home
 * screen, templates and the background use the whole screen. */
enum { COLUMN_UNITS = 400 };

lcd_layout_t lcd_page_unit(const lcd_state_t *s)
{
    const int32_t lw = XIAOTAI_THEME_LCD_LOGICAL_WIDTH, lh = XIAOTAI_THEME_LCD_LOGICAL_HEIGHT;
    if (s->layout.width <= 2 * lw || s->layout.height <= 2 * lh) return s->layout;
    const int32_t quarters = s->m.dp > 16 ? 8 : 7;
    return (lcd_layout_t){(uint16_t)(lw * quarters / 4), (uint16_t)(lh * quarters / 4)};
}

lcd_box_t lcd_page_area(const lcd_state_t *s)
{
    const lcd_layout_t unit = lcd_page_unit(s);
    const int32_t w = s->layout.width, h = s->layout.height;
    if (unit.width == w && unit.height == h) return (lcd_box_t){0, 0, w, h};
    const int32_t column = COLUMN_UNITS * unit.width / (int32_t)XIAOTAI_THEME_LCD_LOGICAL_WIDTH;
    const int32_t width = column < w ? column : w;
    return (lcd_box_t){(w - width) / 2, 0, width, h};
}

static void build(lcd_state_t *s, lcd_layer_t *out, const xiaotai_ui_layer_t *layer, uint8_t index)
{
    const bool overlay = index == XIAOTAI_LAYER_OVERLAY;
    const xiaotai_view_t *view = &layer->view;
    out->custom = view->kind == XIAOTAI_VIEW_CUSTOM ? xiaotai_ui_template_find(s->host, view->custom) : NULL;
    lcd_box_t box = {0, 0, s->layout.width, s->layout.height};
    out->root = lcd_panel(s->root, box, 0, LV_OPA_TRANSP, 0);
    out->content = out->root;
    out->unit = s->layout;
    /* A swipe anywhere on the layer ends here, also one that starts where
     * there is nothing to press, instead of going on to the screen. */
    lv_obj_add_flag(out->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(out->root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(out->root, gesture, LV_EVENT_GESTURE, s);
    if (out->custom) {
        if (!overlay) (void)bind_video(s, false);
        out->canvas = (xiaotai_ui_canvas_t){.backend = XIAOTAI_UI_BACKEND_LVGL, .root = out->root,
            .width = s->layout.width, .height = s->layout.height};
        if (out->custom->open(s->host, &out->canvas, layer, &out->custom_state) == XIAOTAI_OK) {
            return;
        }
        lv_obj_clean(out->root);
        out->custom = NULL;
        out->custom_state = NULL;
    }
    out->home = !overlay && (view->kind == XIAOTAI_VIEW_HOME || view->kind == XIAOTAI_VIEW_TALK ||
                             view->kind == XIAOTAI_VIEW_CLOCK);
    const lcd_box_t full = box;
    const lcd_box_t area = lcd_page_area(s);
    const lcd_layout_t unit = lcd_page_unit(s);
    if (overlay) {
        /* A dialog: the page dims and a card holds the question. The card is
         * laid out in the page's units in the middle of the screen; what
         * draws the dialog sizes the card to what it says. */
        lv_obj_set_style_bg_color(out->root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(out->root, LV_OPA_60, 0);
        lv_obj_add_flag(out->root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(out->root, overlay_background, LV_EVENT_ALL, s);
        lv_obj_t *frame = lcd_panel(out->root, (lcd_box_t){(box.width - unit.width) / 2, (box.height - unit.height) / 2,
                                    unit.width, unit.height}, 0, LV_OPA_TRANSP, 0);
        out->unit = unit;
        box = lcd_scale(&out->unit, (lcd_box_t){24, 33, 272, 174});
        out->content = lcd_panel(frame, box, LCD_SURFACE, LV_OPA_COVER, lcd_dp(s, LCD_RADIUS_CARD));
        lv_obj_set_style_border_color(out->content, lv_color_hex(LCD_OUTLINE), 0);
        lv_obj_set_style_border_width(out->content, 1, 0);
        lv_obj_add_flag(out->content, LV_OBJ_FLAG_CLICKABLE);
    } else if (!out->home) {
        /* The content is a panel of its own even where it fills the screen:
         * it settles into place while background and video stay put. */
        lcd_box_t place = area;
        out->unit = unit;
        if (view->kind == XIAOTAI_VIEW_CALL && area.width != box.width) {
            /* A call uses the whole height and a column no wider than keeps its
             * proportions, so the buttons stay within reach of one hand. */
            const int32_t wide = box.height * (int32_t)XIAOTAI_THEME_LCD_LOGICAL_WIDTH /
                                 (int32_t)XIAOTAI_THEME_LCD_LOGICAL_HEIGHT;
            const int32_t width = wide < box.width ? wide : box.width;
            place = (lcd_box_t){(box.width - width) / 2, 0, width, box.height};
            out->unit = (lcd_layout_t){(uint16_t)width, (uint16_t)box.height};
        }
        out->content = lcd_panel(out->root, place, 0, LV_OPA_TRANSP, 0);
        box = (lcd_box_t){0, 0, place.width, place.height};
    }
    out->canvas = (xiaotai_ui_canvas_t){.backend = XIAOTAI_UI_BACKEND_LVGL, .root = out->content,
        .width = (uint16_t)box.width, .height = (uint16_t)box.height};
    if (out->home) {
        lcd_home_build(s, out, view, index);
    } else {
        const xiaotai_view_kind_t kind = view->kind == XIAOTAI_VIEW_CUSTOM ? XIAOTAI_VIEW_NOTICE : view->kind;
        if (lcd_pages_matches(kind)) out->pages = lcd_pages_open(s, out, kind, index);
        if (!out->pages && !lcd_form_build(s, out, index, kind)) fallback_layout(s, out, overlay);
    }
    if (!overlay && s->variant && s->variant->page_built) {
        out->background = lcd_panel(out->root, full, 0, LV_OPA_TRANSP, 0);
        lv_obj_move_to_index(out->background, 0);
        s->variant->page_built(s->variant->self, s->host, out->background, layer);
    }
}

static void update_video(lcd_state_t *s, lcd_layer_t *out, bool requested)
{
    if (!s->video) return;
    const xiaotai_err_t err = bind_video(s, true);
    if (!requested || err != XIAOTAI_OK) { park_video(s); return; }
    if (lv_obj_get_parent(s->video) != out->root) lv_obj_set_parent(s->video, out->root);
    lv_obj_set_pos(s->video, 0, 0);
    lv_obj_move_to_index(s->video, out->background ? 1 : 0);
    lcd_visible(s->video, true);
}

static void render_content(lcd_state_t *s, lcd_layer_t *out, const xiaotai_ui_layer_t *layer,
                           const xiaotai_ui_scene_t *scene, uint8_t i, bool fresh)
{
    xiaotai_ui_layer_t first;
    if (fresh && !(layer->changed & XIAOTAI_CHANGED_IDENTITY)) {
        first = *layer;
        first.changed |= XIAOTAI_CHANGED_IDENTITY;
        layer = &first;
    }
    if (out->custom) out->custom->update(out->custom_state, &out->canvas, layer, scene);
    else if (out->home) lcd_home_update(s, out, layer, scene);
    else if (out->pages) {
        lcd_pages_update(out->pages, layer, scene->now_ms);
        if (out->face) lcd_face_live_set(s, &layer->view, scene);
    }
    else if (out->form) lcd_form_update(out, layer, scene->now_ms);
    else {
        lcd_text(out->title, layer->view.title);
        lcd_text(out->status, layer->view.status ? layer->view.status : layer->view.subtitle);
        lcd_text(out->body, layer->view.text);
        const int32_t height = i == XIAOTAI_LAYER_OVERLAY ? 174 : lcd_canvas_units(out).height;
        (void)lcd_simple_actions(s, out, &layer->view, i, height);
    }
}

static void render_layer(lcd_state_t *s, const xiaotai_ui_layer_t *layer, const xiaotai_ui_scene_t *scene, uint8_t i)
{
    lcd_layer_t *out = &s->layers[i];
    if (!layer->present) {
        if (out->root) clear_layer(s, out);
        if (i == XIAOTAI_LAYER_PAGE) (void)bind_video(s, true);
        return;
    }
    const bool desk = i == XIAOTAI_LAYER_PAGE && lcd_home_desk(&layer->view);
    const bool fresh = !out->root || (layer->changed & XIAOTAI_CHANGED_IDENTITY) || desk != out->calendar;
    bool arrived = false;
    if (fresh) {
        /* Another page, not the same one rebuilt: only that eases in. */
        const char *id = layer->view.id ? layer->view.id : "";
        arrived = i == XIAOTAI_LAYER_OVERLAY ? !out->root :
                  out->root && strncmp(s->page_id, id, sizeof(s->page_id) - 1U) != 0;
        if (i == XIAOTAI_LAYER_PAGE) (void)snprintf(s->page_id, sizeof(s->page_id), "%s", id);
        clear_layer(s, out);
        if (i == XIAOTAI_LAYER_PAGE) s->template_toast = false;
        /* A new page sets hundreds of styles. Each would lay its labels out
         * and mark the screen again; once at the end does for all of them. A
         * template may measure what it builds, so it goes on as usual. */
        lv_obj_enable_style_refresh(layer->view.kind == XIAOTAI_VIEW_CUSTOM);
        build(s, out, layer, i);
        out->calendar = desk;
    }
    if (!out->custom && i == XIAOTAI_LAYER_PAGE && (fresh || (layer->changed & XIAOTAI_CHANGED_FLAGS)))
        update_video(s, out, (layer->view.flags & XIAOTAI_VIEW_VIDEO) != 0);
    if (!fresh && !layer->changed && !scene->clock_changed) return;
    render_content(s, out, layer, scene, i, fresh);
    if (!fresh) return;
    lv_obj_enable_style_refresh(true);
    lv_obj_refresh_style(out->root, LV_PART_ANY, LV_STYLE_PROP_ANY);
    /* The face and the video paint straight into the frame and templates
     * draw themselves: those appear at once. */
    if (arrived && !out->custom && !out->home && !(layer->view.flags & XIAOTAI_VIEW_VIDEO) && !s->asleep)
        lcd_move_enter(s, out->content, scene->now_ms);
}

static void render_toast(lcd_state_t *s, const xiaotai_ui_layer_t *view)
{
    lcd_layer_t *page = &s->layers[XIAOTAI_LAYER_PAGE];
    lcd_layer_t *toast = &s->layers[XIAOTAI_LAYER_TOAST];
    if (page->custom && page->custom->toast && (view->present || s->template_toast)) {
        if (page->custom->toast(page->custom_state, view->present ? view->view.text : NULL)) {
            s->template_toast = view->present;
            clear_layer(s, toast);
            return;
        }
    }
    s->template_toast = false;
    if (!view->present) { clear_layer(s, toast); return; }
    /* A notice drops in over the header: the line it covers is the page's
     * name, which the reader already knows; the content below stays readable. */
    const lcd_box_t area = lcd_page_area(s);
    const lcd_layout_t unit = lcd_page_unit(s);
    lcd_box_t box = lcd_scale(&unit, (lcd_box_t){10, 4, 300, 32});
    box.x = (area.width - box.width) / 2;
    const bool fresh = !toast->root;
    if (fresh) {
        toast->root = lcd_panel(s->root, (lcd_box_t){0}, LCD_SURFACE_HIGH, LV_OPA_COVER, lcd_dp(s, LCD_RADIUS));
        lv_obj_set_style_border_color(toast->root, lv_color_hex(LCD_OUTLINE), 0);
        lv_obj_set_style_border_width(toast->root, 1, 0);
        toast->body = lcd_label(toast->root, s->m.text, LCD_TEXT);
        lv_label_set_long_mode(toast->body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(toast->body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(toast->body, lcd_dp(s, 2), 0);
    }
    const char *text = view->view.text ? view->view.text : "";
    const int32_t pad = lcd_dp(s, 12), line = lcd_line_height(s), space = lcd_dp(s, 2);
    lv_point_t size;
    lv_text_get_size(&size, text, s->m.text, 0, space, box.width - 2 * pad, LV_TEXT_FLAG_NONE);
    /* The width of the header it stands in for, and as tall as its lines, up to three. */
    const int32_t rows = size.y > 3 * line + 2 * space ? 3 * line + 2 * space : size.y;
    const int32_t height = rows + (box.height > line ? box.height - line : lcd_dp(s, 10));
    const int32_t width = box.width;
    lcd_place(toast->root, (lcd_box_t){area.x + box.x, area.y + box.y, width, height});
    lv_obj_set_size(toast->body, width - 2 * pad, rows);
    lv_obj_center(toast->body);
    lcd_prose(s, toast->body, text, width - 2 * pad);
    if (fresh && !s->asleep) lcd_move_enter(s, toast->root, 0);
}

void xiaotai_theme_lcd_render(void *state, const xiaotai_ui_scene_t *scene)
{
    lcd_state_t *s = state;
    if (!s || !scene) return;
    render_layer(s, &scene->page, scene, XIAOTAI_LAYER_PAGE);
    render_layer(s, &scene->overlay, scene, XIAOTAI_LAYER_OVERLAY);
    render_toast(s, &scene->toast);
    if (s->layers[XIAOTAI_LAYER_PAGE].root) lv_obj_move_to_index(s->layers[XIAOTAI_LAYER_PAGE].root, 0);
    if (s->layers[XIAOTAI_LAYER_TOAST].root) lv_obj_move_foreground(s->layers[XIAOTAI_LAYER_TOAST].root);
    (void)lcd_face_live_tick(s, scene->now_ms);
}

uint32_t xiaotai_theme_lcd_tick(void *state, uint32_t now_ms)
{
    lcd_state_t *s = state;
    if (!s) return 0;
    const lcd_layer_t *overlay = &s->layers[XIAOTAI_LAYER_OVERLAY];
    const uint32_t pages = lcd_pages_tick(overlay->root ? overlay->pages : s->layers[XIAOTAI_LAYER_PAGE].pages, now_ms);
    const uint32_t moves = lcd_move_tick(s, now_ms);
    const uint32_t face = lcd_face_live_tick(s, now_ms);
    const uint32_t clock = s->asleep ? 0 : lcd_clock_tick(s->layers[XIAOTAI_LAYER_PAGE].desk, now_ms);
    uint32_t next = face;
    if (clock && (!next || clock < next)) next = clock;
    if (pages && (!next || pages < next)) next = pages;
    if (moves && (!next || moves < next)) next = moves;
    return next;
}
void xiaotai_theme_lcd_sleep(void *state, bool asleep)
{
    lcd_state_t *s = state;
    if (!s) return;
    s->asleep = asleep;
    if (asleep) lcd_face_live_pause(s);
    lcd_face_live_sleep(s, asleep);
}

size_t xiaotai_theme_lcd_controls(void *state, xiaotai_ui_control_t *out, size_t capacity)
{
    lcd_state_t *s = state;
    if (!s) return 0;
    if (!out) capacity = 0;
    else lcd_move_settle(s);   /* a reported control is where it will be */
    lv_obj_update_layout(s->root);
    size_t count = 0;
    for (int i = XIAOTAI_LAYER_OVERLAY; i >= XIAOTAI_LAYER_PAGE; --i) {
        const lcd_layer_t *layer = &s->layers[i];
        if (layer->custom && layer->custom->controls) {
            count += layer->custom->controls(layer->custom_state, count < capacity ? out + count : NULL,
                                             count < capacity ? capacity - count : 0);
            continue;
        }
        if (layer->form) {
            count += lcd_form_controls(layer, count < capacity ? out + count : NULL,
                                       count < capacity ? capacity - count : 0);
            continue;
        }
        if (layer->pages)
            count += lcd_pages_controls(layer->pages, count < capacity ? out + count : NULL,
                                        count < capacity ? capacity - count : 0);
        for (size_t j = 0; j < XIAOTAI_VIEW_ACTIONS_MAX; ++j)
            if (lcd_measure_control(&layer->action[j], count < capacity ? out + count : NULL)) ++count;
        for (size_t j = 0; j < XIAOTAI_VIEW_INDICATORS_MAX; ++j)
            if (lcd_measure_control(&layer->indicator[j], count < capacity ? out + count : NULL)) ++count;
    }
    return count;
}

void xiaotai_theme_lcd_stop(void *state)
{
    lcd_state_t *s = state;
    if (!s) return;
    for (size_t i = 0; i < sizeof(s->layers) / sizeof(s->layers[0]); ++i) clear_layer(s, &s->layers[i]);
    (void)bind_video(s, false);
    lv_obj_delete(s->root);
    lcd_face_live_stop(s);
    if (s->variant && s->variant->stop) s->variant->stop(s->variant->self);
    xiaotai_mem_free(s);
}

void xiaotai_theme_lcd_report(void *state, char *line, size_t capacity)
{
    lcd_face_live_report(state, line, capacity);
}
