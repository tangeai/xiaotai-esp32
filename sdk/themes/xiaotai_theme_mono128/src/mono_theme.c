// SPDX-License-Identifier: MIT
#include "mono_theme_private.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/ui_owner_frame.h"

#include <stdio.h>
#include <string.h>

enum {
    WIDTH = MONO_WIDTH,
    HEIGHT = MONO_HEIGHT,
    ROWS = MONO_ROWS,
    LIST_Y = MONO_LIST_Y,
    ROW_HEIGHT = MONO_ROW_HEIGHT,
    ROW_X = MONO_ROW_X,
    TEXT_WIDTH = MONO_TEXT_WIDTH,
    HINT_Y = MONO_HINT_Y,
    TOAST_Y = MONO_TOAST_Y,
    TICK_MS = 100,
};

static bool present_text(const char *text) { return text && text[0]; }

void mono_remember_control(mono_state_t *state, uint8_t layer, uint8_t element,
                           uint8_t index, uint8_t x, uint8_t y,
                           uint8_t width, uint8_t height)
{
    if (state->control_count >= sizeof(state->controls) / sizeof(state->controls[0])) return;
    state->controls[state->control_count++] =
        (mono_control_t){x, y, width, height, layer, element, index};
}

void mono_header(const mono_canvas_t *canvas, const char *title, const char *right)
{
    unsigned width = mono_utf8_width(right);
    if (width > WIDTH) width = WIDTH;
    const unsigned available = width ? (width < WIDTH - 4U ? WIDTH - width - 4U : 0U) : WIDTH;
    (void)mono_window(canvas, 0, 0, title, available, 0);
    (void)mono_window(canvas, WIDTH - (int)width, 0, right, width, 0);
    mono_rect(canvas, 0, 13, WIDTH, 1, MONO_SET);
}

static unsigned first_row(unsigned focus, unsigned count)
{
    const unsigned first = focus > 0 ? focus - 1U : 0;
    const unsigned last = count > ROWS ? count - ROWS : 0;
    return first < last ? first : last;
}

static const char *right_header(const xiaotai_ui_layer_t *layer)
{
    const xiaotai_view_t *view = &layer->view;
    if (present_text(view->status) || view->kind != XIAOTAI_VIEW_LIST) return view->status;
    if (layer->focus_kind != XIAOTAI_UI_FOCUS_ITEM || layer->focus >= view->item_count) return NULL;
    const xiaotai_view_item_t *item = &view->items[layer->focus];
    return (item->state & XIAOTAI_STATE_HIDDEN) ? NULL : item->detail;
}

static unsigned list(mono_state_t *state, const xiaotai_ui_layer_t *layer,
                     uint8_t layer_id, uint32_t tick, bool toast)
{
    const xiaotai_view_t *view = &layer->view;
    unsigned count = 0, focus = 0;
    for (size_t index = 0; index < view->item_count; ++index) {
        if (view->items[index].state & XIAOTAI_STATE_HIDDEN) continue;
        if (layer->focus_kind == XIAOTAI_UI_FOCUS_ITEM && index == layer->focus) focus = count;
        ++count;
    }
    const unsigned first = first_row(focus, count);
    const bool has_scrollbar = count > ROWS;
    const uint8_t row_width = has_scrollbar ? WIDTH - 4 : WIDTH;
    unsigned visible = 0, painted = 0;
    for (size_t index = 0; index < view->item_count && painted < ROWS; ++index) {
        const xiaotai_view_item_t *item = &view->items[index];
        if (item->state & XIAOTAI_STATE_HIDDEN) continue;
        if (visible++ < first) continue;
        const uint8_t y = (uint8_t)(LIST_Y + painted * ROW_HEIGHT);
        const bool chosen = layer->focus_kind == XIAOTAI_UI_FOCUS_ITEM && index == layer->focus;
        const bool covered = toast && y >= TOAST_Y;
        (void)mono_window(&state->canvas, ROW_X, y, item->label, TEXT_WIDTH, chosen ? tick : 0);
        if (chosen) {
            mono_rect(&state->canvas, 0, y - 2, row_width, 15, MONO_INVERT);
            if (!covered) state->scrolling = mono_utf8_width(item->label) > TEXT_WIDTH;
        }
        if (!covered)
            mono_remember_control(state, layer_id, XIAOTAI_UI_ELEMENT_ITEM, (uint8_t)index,
                                  0, y - 2, row_width, 15);
        ++painted;
    }
    if (has_scrollbar) {
        const unsigned track = ROWS * ROW_HEIGHT - 2U;
        unsigned thumb = track * ROWS / count;
        if (thumb < 4U) thumb = 4U;
        const unsigned offset = (track - thumb) * first / (count - ROWS);
        mono_rect(&state->canvas, 126, LIST_Y - 2 + (int)offset, 2, (int)thumb, MONO_SET);
    }
    return painted;
}

static void hint(mono_state_t *state, const xiaotai_view_t *view, uint8_t layer_id, mono_hint_t style)
{
    static const uint8_t roles[] = {XIAOTAI_ROLE_PRIMARY, XIAOTAI_ROLE_AUX, XIAOTAI_ROLE_BACK};
    struct { const char *label; unsigned width; uint8_t index; } lines[sizeof(roles)];
    unsigned count = 0, total = 0;
    const size_t role_count = style.primary_only ? 1U : sizeof(roles);
    for (size_t role = 0; role < role_count; ++role) {
        for (size_t index = 0; index < view->action_count; ++index) {
            const xiaotai_view_action_t *action = &view->actions[index];
            if (action->role != roles[role]) continue;
            if (!(action->state & XIAOTAI_STATE_HIDDEN) && present_text(action->label)) {
                unsigned width = mono_utf8_width(action->label);
                if (width > WIDTH) width = WIDTH;
                lines[count].label = action->label;
                lines[count].width = width;
                lines[count].index = (uint8_t)index;
                total += width + (count ? 8U : 0U);
                ++count;
            }
            break;
        }
    }
    unsigned x = total < WIDTH ? (WIDTH - total) / 2U : 0;
    for (unsigned line = 0; line < count && x < WIDTH; ++line) {
        unsigned room = WIDTH - x;
        if (room > lines[line].width) room = lines[line].width;
        const unsigned drawn = mono_window(&state->canvas, (int)x, style.y, lines[line].label, room, 0);
        if (drawn) {
            mono_remember_control(state, layer_id, XIAOTAI_UI_ELEMENT_ACTION, lines[line].index,
                                  (uint8_t)x, style.y, (uint8_t)drawn, 12);
        }
        x += room + 8U;
    }
}

static int match(const xiaotai_display_desc_t *display)
{
    if (!display || display->width != WIDTH || display->height != HEIGHT ||
        (display->flags & XIAOTAI_DISPLAY_SLOW_REFRESH)) return 0;
    return display->pixel == XIAOTAI_PIXEL_MONO1_VLSB || display->pixel == XIAOTAI_PIXEL_MONO1 ? 100 : 0;
}

xiaotai_err_t xiaotai_engine_theme_mono128_startup_error_guide(xiaotai_err_t error)
{
    const xiaotai_display_t *display = xiaotai_hal_display(0);
    if (!display || !match(&display->desc) || display->desc.rotation ||
        (display->desc.flags & XIAOTAI_DISPLAY_DMA_BUFFER) || !display->open ||
        !display->present || !display->set_backlight) return XIAOTAI_ERR_NOT_SUPPORTED;

    /* The usual UI stack is unavailable. The panel consumes this single
     * frame synchronously; no dedicated UI task or heap reservation is needed. */
    uint8_t frame[XIAOTAI_THEME_MONO128_FRAME_BYTES] = {0};
    const mono_canvas_t canvas = {frame, display->desc.pixel};
    const xiaotai_rect_t area = {.width = WIDTH, .height = HEIGHT};
    char code[16];
    snprintf(code, sizeof(code), "ERR %04X", (unsigned)error);
    mono_center(&canvas, 2, "操作未完成");
    mono_center(&canvas, 25, "启动失败");
    mono_center(&canvas, 50, code);

    xiaotai_err_t result = display->open(display->self, NULL);
    if (result == XIAOTAI_OK) result = display->present(display->self, &area, frame);
    if (result == XIAOTAI_OK) result = display->set_backlight(display->self, 100);
    return result;
}

static xiaotai_err_t start(const xiaotai_ui_host_t *host, void **out)
{
    if (!host || !out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    const xiaotai_display_desc_t *display = xiaotai_ui_host_display(host);
    if (!match(display)) return XIAOTAI_ERR_NOT_SUPPORTED;
    size_t size = sizeof(mono_state_t);
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    const bool shared = xiaotai_ui_host_prepare_owner_frame(host, XIAOTAI_THEME_MONO128_FRAME_BYTES);
    if (!shared) size += XIAOTAI_THEME_MONO128_FRAME_BYTES;
#else
    const bool shared = false;
#endif
    mono_state_t *state = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, size);
    if (!state) return XIAOTAI_ERR_NO_MEM;
    state->host = host;
    state->canvas = (mono_canvas_t){shared ? NULL : state->frame, display->pixel};
    *out = state;
    return XIAOTAI_OK;
}

static void render(void *private, const xiaotai_ui_scene_t *scene)
{
    mono_state_t *state = private;
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    const bool shared = !state->canvas.frame;
    if (shared) state->canvas.frame = xiaotai_ui_host_owner_frame(state->host);
#endif
    memset(state->canvas.frame, 0, XIAOTAI_THEME_MONO128_FRAME_BYTES);
    state->control_count = 0;
    state->scrolling = false;
    state->animated = false;
    const uint32_t tick = scene->now_ms / TICK_MS;
    state->tick_at = tick;
    const bool toast = scene->toast.present && present_text(scene->toast.view.text);
    const xiaotai_ui_layer_t *layer = scene->overlay.present ? &scene->overlay : &scene->page;
    const uint8_t layer_id = scene->overlay.present ? XIAOTAI_LAYER_OVERLAY : XIAOTAI_LAYER_PAGE;
    if (layer->present) {
        mono_hint_t footer = {0};
        if (layer->view.kind == XIAOTAI_VIEW_HOME || layer->view.kind == XIAOTAI_VIEW_LIST) {
            mono_header(&state->canvas, layer->view.title, right_header(layer));
            const unsigned rows = list(state, layer, layer_id, tick, toast);
            if (!rows && !(layer->view.image && mono_image(&state->canvas, 15, 36,
                                                         xiaotai_ui_image_find(layer->view.image))))
                mono_center(&state->canvas, 32, layer->view.text);
            if (rows < ROWS) footer.y = HINT_Y;
        } else {
            footer = mono_render_page(state, layer, layer_id, &scene->clock, tick, toast);
        }
        if (!toast && footer.y) hint(state, &layer->view, layer_id, footer);
    }
    if (toast) {
        mono_rect(&state->canvas, 0, TOAST_Y, WIDTH, HEIGHT - TOAST_Y, MONO_CLEAR);
        mono_center(&state->canvas, HINT_Y, scene->toast.view.text);
        mono_rect(&state->canvas, 0, TOAST_Y + 1, WIDTH, HEIGHT - TOAST_Y - 1, MONO_INVERT);
    }
    (void)xiaotai_ui_host_present(state->host, state->canvas.frame, XIAOTAI_THEME_MONO128_FRAME_BYTES);
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    if (shared) state->canvas.frame = NULL;
#endif
}

static uint32_t tick(void *private, uint32_t now_ms)
{
    mono_state_t *state = private;
    if (!state->scrolling && !state->animated) return 0;
    const uint32_t tick_at = now_ms / TICK_MS;
    if ((state->scrolling && tick_at / 4U != state->tick_at / 4U) ||
        (state->animated && tick_at / 3U != state->tick_at / 3U))
        xiaotai_ui_host_invalidate(state->host);
    return TICK_MS;
}

static size_t controls(void *private, xiaotai_ui_control_t *out, size_t capacity)
{
    const mono_state_t *state = private;
    for (size_t i = 0; out && i < capacity && i < state->control_count; ++i) {
        const mono_control_t *source = &state->controls[i];
        out[i] = (xiaotai_ui_control_t){.x = source->x, .y = source->y,
                                       .width = source->width, .height = source->height,
                                       .layer = source->layer, .element = source->element,
                                       .index = source->index};
    }
    return state->control_count;
}

const xiaotai_theme_t xiaotai_theme_mono128 = {
    .name = XIAOTAI_THEME_MONO128_NAME,
    .backend = XIAOTAI_UI_BACKEND_FRAME,
    .match = match,
    .start = start,
    .render = render,
    .tick = tick,
    .controls = controls,
    .stop = xiaotai_mem_free,
    .stack_bytes = XIAOTAI_UI_OWNER_THEME_STACK_BYTES,
};

xiaotai_err_t xiaotai_engine_theme_mono128_init(void)
{
    return xiaotai_ui_theme_register(&xiaotai_theme_mono128);
}
