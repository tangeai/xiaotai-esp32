// SPDX-License-Identifier: MIT
#include "mono_theme_private.h"

#include <string.h>

static bool has_text(const char *s) { return s && *s; }

static void two_digits(char *out, unsigned value)
{
    out[0] = (char)('0' + (value / 10U) % 10U);
    out[1] = (char)('0' + value % 10U);
}

static void time_text(char out[6], unsigned hours_or_minutes, unsigned minutes_or_seconds)
{
    two_digits(out, hours_or_minutes);
    out[2] = ':';
    two_digits(out + 3, minutes_or_seconds);
    out[5] = '\0';
}

static char *decimal(char *out, int32_t value)
{
    char reversed[10];
    size_t used = 0;
    uint32_t magnitude = (uint32_t)value;
    if (value < 0) {
        *out++ = '-';
        magnitude = 0U - magnitude;
    }
    do {
        reversed[used++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    while (used) *out++ = reversed[--used];
    *out = '\0';
    return out;
}

static bool draw_image(const mono_canvas_t *canvas, int y, int height, const char *name)
{
    return name && mono_image(canvas, y, height, xiaotai_ui_image_find(name));
}

static void session_header(const mono_canvas_t *canvas, const xiaotai_ui_layer_t *layer)
{
    char time[6];
    const unsigned elapsed = layer->timer_s > 0 ? (unsigned)layer->timer_s : 0U;
    time_text(time, elapsed / 60U, elapsed % 60U);
    mono_header(canvas, layer->view.title, layer->view.timer_mode != XIAOTAI_TIMER_NONE ? time : NULL);
}

static mono_hint_t talk(mono_state_t *state, const xiaotai_ui_layer_t *layer, uint32_t tick, bool toast)
{
    const mono_canvas_t *canvas = &state->canvas;
    const xiaotai_view_t *view = &layer->view;
    session_header(canvas, layer);
    /* A 128x64 display has room for the state, not the live caption. */
    mono_center(canvas, 26, view->status);
    if (view->phase == XIAOTAI_TALK_IDLE) return (mono_hint_t){MONO_HINT_Y, false};
    if (toast) return (mono_hint_t){0};
    state->animated = true;
    const unsigned step = tick / 3U;
    if (view->phase == XIAOTAI_TALK_SPEAKING) {
        static const uint8_t bars[] = {4, 9, 14, 8, 12, 5, 10, 13, 6, 11};
        for (unsigned i = 0; i < 9; ++i) {
            const int height = bars[(i * 3U + step) % sizeof(bars)];
            mono_rect(canvas, 30 + (int)i * 8, 60 - height, 4, height, MONO_SET);
        }
    } else {
        for (unsigned i = 0; i < 3; ++i) {
            const int size = i == step % 3U ? 6 : 3;
            const int inset = (6 - size) / 2;
            mono_rect(canvas, 50 + (int)i * 12 + inset, 52 + inset, size, size, MONO_SET);
        }
    }
    return (mono_hint_t){0};
}

static void call(mono_state_t *state, const xiaotai_ui_layer_t *layer, uint32_t tick)
{
    session_header(&state->canvas, layer);
    (void)mono_center_line(&state->canvas, 18, layer->view.subtitle, tick, &state->scrolling);
    mono_center(&state->canvas, 34, layer->view.text);
}

static bool code(mono_state_t *state, const xiaotai_ui_layer_t *layer, uint32_t tick, bool toast)
{
    const mono_canvas_t *canvas = &state->canvas;
    const xiaotai_view_t *view = &layer->view;
    mono_header(canvas, view->title, view->status);
    if (draw_image(canvas, 16, (toast ? MONO_TOAST_Y : MONO_HEIGHT) - 16, view->image))
        return false;
    /* QR payloads are not verification numbers. */
    if (!(view->flags & XIAOTAI_VIEW_QR) && !layer->timer_expired)
        mono_center5(canvas, 20, view->text, 2);
    (void)mono_center_line(canvas, 36, view->subtitle, tick, &state->scrolling);
    if (view->timer_mode == XIAOTAI_TIMER_REMAINING && !layer->timer_expired) {
        static const char suffix[] = "秒后过期";
        char expiry[11 + sizeof(suffix)];
        char *end = decimal(expiry, layer->timer_s > 0 ? layer->timer_s : 0);
        memcpy(end, suffix, sizeof(suffix));
        mono_center(canvas, 50, expiry);
        return false;
    }
    return true;
}

static void progress(mono_state_t *state, const xiaotai_view_t *view, uint32_t tick)
{
    const mono_canvas_t *canvas = &state->canvas;
    mono_header(canvas, view->title, view->status);
    if (!(view->flags & XIAOTAI_VIEW_PERCENT)) {
        (void)mono_center_line(canvas, 28, view->text, tick, &state->scrolling);
        return;
    }
    (void)mono_center_line(canvas, 20, view->text, tick, &state->scrolling);
    const unsigned value = view->progress > 100U ? 100U : view->progress;
    enum { LEFT = 8, TOP = 36, WIDTH = 112, HEIGHT = 8 };
    mono_rect(canvas, LEFT, TOP, WIDTH, HEIGHT, MONO_SET);
    mono_rect(canvas, LEFT + 1, TOP + 1, WIDTH - 2, HEIGHT - 2, MONO_CLEAR);
    mono_rect(canvas, LEFT + 1, TOP + 1, (int)((WIDTH - 2U) * value / 100U), HEIGHT - 2, MONO_SET);
}

static void wall_clock(const mono_canvas_t *canvas, const xiaotai_view_t *view, const xiaotai_ui_clock_t *clock)
{
    mono_header(canvas, view->title, view->status);
    if (!clock->valid) {
        mono_center5(canvas, 20, "--:--", 2);
        return;
    }
    char time[6], date[11];
    time_text(time, clock->hour, clock->minute);
    mono_center5(canvas, 20, time, 2);
    two_digits(date, clock->year / 100U);
    two_digits(date + 2, clock->year);
    date[4] = '-';
    two_digits(date + 5, clock->month);
    date[7] = '-';
    two_digits(date + 8, clock->day);
    date[10] = '\0';
    mono_center(canvas, 36, date);
}

static void pair(const mono_canvas_t *canvas, int x, int y, unsigned width, const char *label, const char *value)
{
    unsigned right = mono_utf8_width(value);
    if (has_text(label) && right + 8U > width) right = width > 8U ? width - 8U : 0;
    else if (right > width) right = width;
    const unsigned left = width > right + 4U ? width - right - 4U : right ? 0U : width;
    (void)mono_window(canvas, x, y, label, left, 0);
    (void)mono_window(canvas, x + (int)(width - right), y, value, right, 0);
}

static void notice(mono_state_t *state, const xiaotai_ui_layer_t *layer, uint32_t tick)
{
    const mono_canvas_t *canvas = &state->canvas;
    const xiaotai_view_t *view = &layer->view;
    const bool elapsed = view->kind == XIAOTAI_VIEW_NOTICE && view->timer_mode == XIAOTAI_TIMER_ELAPSED;
    const bool overflow = has_text(view->status) && (elapsed ||
        (uint64_t)mono_utf8_width(view->title) + mono_utf8_width(view->status) + 4U > MONO_WIDTH);
    if (elapsed) session_header(canvas, layer);
    else mono_header(canvas, view->title, overflow ? NULL : view->status);
    if (draw_image(canvas, 15, 36, view->image)) return;
    if (!overflow && !has_text(view->subtitle) && !view->item_count &&
        (!view->text || !strchr(view->text, '\n'))) {
        (void)mono_center_line(canvas, 32, view->text, tick, &state->scrolling);
        return;
    }
    int y = 15;
    const int last = overflow ? 39 : 51;
    const char *line = view->text;
    while (has_text(line) && y < last) {
        line = mono_center_line(canvas, y, line, tick, &state->scrolling);
        y += 12;
    }
    if (has_text(view->subtitle) && y < last) {
        (void)mono_center_line(canvas, y, view->subtitle, tick, &state->scrolling);
        y += 12;
    }
    for (size_t i = 0; i < view->item_count && y < last; ++i) {
        const xiaotai_view_item_t *item = &view->items[i];
        if (item->state & XIAOTAI_STATE_HIDDEN) continue;
        pair(canvas, MONO_ROW_X, y, MONO_TEXT_WIDTH, item->label, item->detail);
        y += 12;
    }
    if (overflow) (void)mono_center_line(canvas, 39, view->status, tick, &state->scrolling);
}

static uint8_t digit_count(const xiaotai_view_field_t *field)
{
    return field->max > 0 && field->max <= (int32_t)XIAOTAI_VIEW_DIGITS_MAX ? (uint8_t)field->max : 0;
}

static void digits_text(const xiaotai_view_field_t *field, uint8_t count, uint8_t shown,
                        char text[XIAOTAI_VIEW_DIGITS_MAX + 1U])
{
    const char *value = field->text;
    for (uint8_t i = 0; i < count; ++i) {
        const char c = has_text(value) ? *value++ : '0';
        text[i] = (field->flags & XIAOTAI_FIELD_MASKED) && i != shown ? '*' :
                  c >= '0' && c <= '9' ? c : '0';
    }
    text[count] = '\0';
}

static void digits(mono_state_t *state, const xiaotai_view_field_t *field,
                   uint8_t count, uint8_t cursor, uint8_t index, uint8_t layer)
{
    char text[XIAOTAI_VIEW_DIGITS_MAX + 1U];
    if (cursor >= count) cursor = count - 1U;
    digits_text(field, count, cursor, text);
    const unsigned scale = count * 12U <= MONO_WIDTH ? 2U : 1U;
    const uint8_t width = (uint8_t)(count * 6U * scale);
    const uint8_t x = (MONO_WIDTH - width) / 2U;
    mono_text5(&state->canvas, x, 20, text, scale);
    mono_rect(&state->canvas, (int)(x + cursor * 6U * scale), 36, (int)(5U * scale), 2, MONO_SET);
    mono_remember_control(state, layer, XIAOTAI_UI_ELEMENT_FIELD, index, x, 20, width, 18);
}

static const char *field_value(const xiaotai_view_field_t *field, char *scratch, size_t size)
{
    switch (field->kind) {
    case XIAOTAI_FIELD_TEXT: return field->text;
    case XIAOTAI_FIELD_SWITCH: return field->value ? "ON" : "OFF";
    case XIAOTAI_FIELD_CHOICE:
        return field->options && field->value >= 0 && (size_t)field->value < field->option_count
            ? field->options[field->value] : "-";
    case XIAOTAI_FIELD_DIGITS:
        digits_text(field, digit_count(field), UINT8_MAX, scratch);
        return scratch;
    default:
        if (size < 12) return "-";
        char *end = decimal(scratch, field->value);
        const size_t left = size - (size_t)(end - scratch);
        if (has_text(field->text) && left > 2U) {
            size_t bytes = strnlen(field->text, left - 2U);
            while (bytes && ((uint8_t)field->text[bytes] & 0xc0U) == 0x80U) --bytes;
            if (bytes) {
                *end++ = ' ';
                memcpy(end, field->text, bytes);
                end[bytes] = '\0';
            }
        }
        return scratch;
    }
}

static mono_hint_t form(mono_state_t *state, const xiaotai_ui_layer_t *layer, uint8_t layer_id, bool toast)
{
    const xiaotai_view_t *view = &layer->view;
    const bool focused = layer->focus_kind == XIAOTAI_UI_FOCUS_FIELD && layer->focus < view->field_count;
    mono_header(&state->canvas, view->title, view->status);
    const xiaotai_view_field_t *field = focused ? &view->fields[layer->focus] : NULL;
    if (field && !(field->state & XIAOTAI_STATE_HIDDEN) &&
        field->kind == XIAOTAI_FIELD_DIGITS && digit_count(field)) {
        digits(state, field, digit_count(field), layer->cursor, (uint8_t)layer->focus, layer_id);
        return (mono_hint_t){48, true};
    }
    unsigned visible = 0, focus_row = 0;
    for (size_t i = 0; i < view->field_count; ++i) {
        if (view->fields[i].state & XIAOTAI_STATE_HIDDEN) continue;
        if (focused && i == layer->focus) focus_row = visible;
        ++visible;
    }
    unsigned start = focus_row > 0 ? focus_row - 1U : 0U;
    const unsigned last = visible > MONO_ROWS ? visible - MONO_ROWS : 0U;
    if (start > last) start = last;
    unsigned row = 0;
    char value[40];
    for (size_t i = 0; i < view->field_count && row < MONO_ROWS; ++i) {
        const xiaotai_view_field_t *entry = &view->fields[i];
        if (entry->state & XIAOTAI_STATE_HIDDEN) continue;
        if (start) { --start; continue; }
        const uint8_t y = (uint8_t)(MONO_LIST_Y + row * MONO_ROW_HEIGHT);
        pair(&state->canvas, MONO_ROW_X, y, MONO_TEXT_WIDTH, entry->label,
             field_value(entry, value, sizeof(value)));
        if (focused && i == layer->focus) mono_rect(&state->canvas, 0, y - 2, MONO_WIDTH, 15, MONO_INVERT);
        if (!toast || y < MONO_TOAST_Y)
            mono_remember_control(state, layer_id, XIAOTAI_UI_ELEMENT_FIELD, (uint8_t)i, 0, y - 2, MONO_WIDTH, 15);
        ++row;
    }
    if (!row) mono_center(&state->canvas, 32, view->text);
    return (mono_hint_t){row < MONO_ROWS ? MONO_HINT_Y : 0, false};
}

mono_hint_t mono_render_page(mono_state_t *state, const xiaotai_ui_layer_t *layer, uint8_t layer_id,
                             const xiaotai_ui_clock_t *clock, uint32_t tick, bool toast)
{
    switch (layer->view.kind) {
    case XIAOTAI_VIEW_TALK: return talk(state, layer, tick, toast);
    case XIAOTAI_VIEW_CALL: call(state, layer, tick); break;
    case XIAOTAI_VIEW_CODE: if (!code(state, layer, tick, toast)) return (mono_hint_t){0}; break;
    case XIAOTAI_VIEW_PROGRESS: progress(state, &layer->view, tick); break;
    case XIAOTAI_VIEW_CLOCK: wall_clock(&state->canvas, &layer->view, clock); break;
    case XIAOTAI_VIEW_FORM: return form(state, layer, layer_id, toast);
    default: notice(state, layer, tick); break;
    }
    return (mono_hint_t){MONO_HINT_Y, false};
}
