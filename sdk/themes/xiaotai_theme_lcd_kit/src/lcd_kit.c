// SPDX-License-Identifier: MIT
#include "lcd_face_live.h"
#include "lcd_form.h"
#include "lcd_internal.h"
#include "lcd_pages.h"
#include "xiaotai/port/mem.h"

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
    if (layer->face) lcd_face_live_detach(s);
    if (s->video && layer->root && lv_obj_get_parent(s->video) == layer->root) park_video(s);
    if (layer->custom && layer->custom->close) layer->custom->close(layer->custom_state);
    if (layer->root) lv_obj_delete(layer->root);
    lcd_pages_free(layer->pages);
    lcd_form_free(layer);
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
    const xiaotai_err_t face = lcd_face_live_start(s);
    if (face != XIAOTAI_OK) {
        xiaotai_mem_free(s);
        return face;
    }
    s->root = lcd_panel(lv_display_get_screen_active(display), (lcd_box_t){0, 0, desc->width, desc->height},
                        LCD_BACKGROUND, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s->root, lcd_body_font(), 0);
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

static void fallback_layout(lcd_layer_t *out, bool overlay)
{
    const int32_t width = out->canvas.width;
    const int32_t height = out->canvas.height;
    out->title = lcd_label(out->content, lcd_body_font(), LCD_TEXT);
    lcd_place(out->title, (lcd_box_t){16, 10, width - 32, 24});
    lv_label_set_long_mode(out->title, LV_LABEL_LONG_DOT);
    out->status = lcd_label(out->content, lcd_body_font(), LCD_ACCENT);
    lcd_place(out->status, (lcd_box_t){16, 36, width - 32, 22});
    lv_label_set_long_mode(out->status, LV_LABEL_LONG_DOT);
    out->body = lcd_label(out->content, lcd_body_font(), LCD_MUTED);
    lcd_place(out->body, (lcd_box_t){16, 64, width - 32, height > 138 ? height - 138 : 34});
    lv_label_set_long_mode(out->body, LV_LABEL_LONG_WRAP);
    if (overlay) lv_obj_set_style_text_align(out->body, LV_TEXT_ALIGN_CENTER, 0);
}

/* The pages are drawn for 320 x 240 and scaled, but text stays 18 px. On a
 * screen more than twice that size, an ordinary page uses a centred area at
 * most 1.75 times the design size instead of spreading thin across it; the
 * home screen, templates and the background still use the whole screen. */
static lcd_box_t page_area(const lcd_state_t *s)
{
    const int32_t w = s->layout.width, h = s->layout.height;
    const int32_t lw = XIAOTAI_THEME_LCD_LOGICAL_WIDTH, lh = XIAOTAI_THEME_LCD_LOGICAL_HEIGHT;
    if (w <= 2 * lw || h <= 2 * lh) return (lcd_box_t){0, 0, w, h};
    const int32_t pw = lw * 7 / 4, ph = lh * 7 / 4;
    return (lcd_box_t){(w - pw) / 2, (h - ph) / 2, pw, ph};
}

static void build(lcd_state_t *s, lcd_layer_t *out, const xiaotai_ui_layer_t *layer, uint8_t index)
{
    const bool overlay = index == XIAOTAI_LAYER_OVERLAY;
    const xiaotai_view_t *view = &layer->view;
    out->custom = view->kind == XIAOTAI_VIEW_CUSTOM ? xiaotai_ui_template_find(s->host, view->custom) : NULL;
    lcd_box_t box = {0, 0, s->layout.width, s->layout.height};
    out->root = lcd_panel(s->root, box, 0, LV_OPA_TRANSP, 0);
    out->content = out->root;
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
    if (overlay) {
        lv_obj_set_style_bg_opa(out->root, LV_OPA_60, 0);
        lv_obj_add_flag(out->root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(out->root, overlay_background, LV_EVENT_ALL, s);
        box = lcd_scale(&s->layout, (lcd_box_t){20, 33, 280, 174});
        out->content = lcd_panel(out->root, box, LCD_SURFACE, LV_OPA_COVER, 14);
        lv_obj_add_flag(out->content, LV_OBJ_FLAG_CLICKABLE);
    }
    out->home = !overlay && (view->kind == XIAOTAI_VIEW_HOME || view->kind == XIAOTAI_VIEW_TALK ||
                             view->kind == XIAOTAI_VIEW_CLOCK);
    const lcd_box_t full = box;
    if (!overlay && !out->home) {
        const lcd_box_t area = page_area(s);
        if (area.width != box.width || area.height != box.height) {
            out->content = lcd_panel(out->root, area, 0, LV_OPA_TRANSP, 0);
            box = (lcd_box_t){0, 0, area.width, area.height};
        }
    }
    out->canvas = (xiaotai_ui_canvas_t){.backend = XIAOTAI_UI_BACKEND_LVGL, .root = out->content,
        .width = (uint16_t)box.width, .height = (uint16_t)box.height};
    if (out->home) {
        lcd_home_build(s, out, view, index);
    } else {
        const xiaotai_view_kind_t kind = view->kind == XIAOTAI_VIEW_CUSTOM ? XIAOTAI_VIEW_NOTICE : view->kind;
        if (lcd_pages_matches(kind)) out->pages = lcd_pages_open(s, out, kind, index);
        if (!out->pages && !lcd_form_build(s, out, index, kind)) fallback_layout(out, overlay);
    }
    if (!overlay && s->variant && s->variant->page_built) {
        out->background = lcd_panel(out->root, overlay ? box : full, 0, LV_OPA_TRANSP, 0);
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

static void render_layer(lcd_state_t *s, const xiaotai_ui_layer_t *layer, const xiaotai_ui_scene_t *scene, uint8_t i)
{
    lcd_layer_t *out = &s->layers[i];
    if (!layer->present) {
        if (out->root) clear_layer(s, out);
        if (i == XIAOTAI_LAYER_PAGE) (void)bind_video(s, true);
        return;
    }
    const bool fresh = !out->root || (layer->changed & XIAOTAI_CHANGED_IDENTITY);
    if (fresh) {
        clear_layer(s, out);
        if (i == XIAOTAI_LAYER_PAGE) s->template_toast = false;
        build(s, out, layer, i);
    }
    if (!out->custom && i == XIAOTAI_LAYER_PAGE && (fresh || (layer->changed & XIAOTAI_CHANGED_FLAGS)))
        update_video(s, out, (layer->view.flags & XIAOTAI_VIEW_VIDEO) != 0);
    if (!fresh && !layer->changed && !scene->clock_changed) return;
    xiaotai_ui_layer_t first;
    if (fresh && !(layer->changed & XIAOTAI_CHANGED_IDENTITY)) {
        first = *layer;
        first.changed |= XIAOTAI_CHANGED_IDENTITY;
        layer = &first;
    }
    if (out->custom) out->custom->update(out->custom_state, &out->canvas, layer, scene);
    else if (out->home) lcd_home_update(s, out, layer, scene);
    else if (out->pages) lcd_pages_update(out->pages, layer);
    else if (out->form) lcd_form_update(out, layer, scene->now_ms);
    else {
        lcd_text(out->title, layer->view.title);
        lcd_text(out->status, layer->view.status ? layer->view.status : layer->view.subtitle);
        lcd_text(out->body, layer->view.text);
        const int32_t height = i == XIAOTAI_LAYER_OVERLAY ? 174 : 240;
        lcd_simple_actions(s, out, &layer->view, i, height);
    }
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
    if (!toast->root) {
        const lcd_box_t box = lcd_scale(&s->layout, (lcd_box_t){8, 42, 304, 30});
        toast->root = lcd_panel(s->root, box, LCD_SURFACE_HIGH, LV_OPA_COVER, LCD_RADIUS);
        toast->body = lcd_label(toast->root, lcd_body_font(), LCD_TEXT);
        lv_obj_set_width(toast->body, box.width - 12);
        lv_label_set_long_mode(toast->body, LV_LABEL_LONG_DOT);
        lv_obj_center(toast->body);
    }
    lcd_text(toast->body, view->view.text);
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
    const uint32_t face = lcd_face_live_tick(s, now_ms);
    return !face || (pages && pages < face) ? pages : face;
}
void xiaotai_theme_lcd_sleep(void *state, bool asleep)
{
    lcd_state_t *s = state;
    if (!s) return;
    s->asleep = asleep;
    if (asleep) lcd_face_live_pause(s);
}

size_t xiaotai_theme_lcd_controls(void *state, xiaotai_ui_control_t *out, size_t capacity)
{
    const lcd_state_t *s = state;
    if (!s) return 0;
    if (!out) capacity = 0;
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
