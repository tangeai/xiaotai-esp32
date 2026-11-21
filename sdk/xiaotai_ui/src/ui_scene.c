// SPDX-License-Identifier: MIT
#include "ui_internal.h"

#include <limits.h>
#include <string.h>
#include "xiaotai/port/clock.h"

typedef struct {
    const char *intent;
    const char *digits;
    int32_t value;
    uint8_t kind;
} focused_entry_t;

xiaotai_ui_layer_t *ui_scene_layer(ui_display_t *display, xiaotai_view_layer_t layer)
{
    switch (layer) {
        case XIAOTAI_LAYER_PAGE: return &display->scene.page;
        case XIAOTAI_LAYER_OVERLAY: return &display->scene.overlay;
        case XIAOTAI_LAYER_TOAST: return &display->scene.toast;
        default: return NULL;
    }
}

xiaotai_view_layer_t ui_scene_top(const ui_display_t *display)
{
    return display->scene.overlay.present ? XIAOTAI_LAYER_OVERLAY : XIAOTAI_LAYER_PAGE;
}

void ui_scene_focus(ui_display_t *display, xiaotai_view_layer_t layer, uint8_t kind, uint16_t index)
{
    xiaotai_ui_layer_t *out = ui_scene_layer(display, layer);
    if (out->focus_kind == kind && out->focus == index) return;
    out->focus_kind = kind;
    out->focus = index;
    out->cursor = 0U;
    display->scene_layer[layer].digits[0] = '\0';
    display->scene_pending[layer] |= XIAOTAI_CHANGED_FOCUS | XIAOTAI_CHANGED_FIELDS;
}

static focused_entry_t focused_entry(const xiaotai_ui_layer_t *layer)
{
    focused_entry_t entry = {0};
    if (layer->present && layer->focus_kind == XIAOTAI_UI_FOCUS_ITEM && layer->focus < layer->view.item_count) {
        const xiaotai_view_item_t *item = &layer->view.items[layer->focus];
        entry.kind = XIAOTAI_UI_FOCUS_ITEM;
        entry.value = item->value;
        entry.intent = item->intent;
    } else if (layer->present && layer->focus_kind == XIAOTAI_UI_FOCUS_FIELD &&
               layer->focus < layer->view.field_count) {
        const xiaotai_view_field_t *field = &layer->view.fields[layer->focus];
        entry.kind = XIAOTAI_UI_FOCUS_FIELD;
        entry.value = field->value;
        entry.intent = field->intent;
        if (field->kind == XIAOTAI_FIELD_DIGITS) entry.digits = field->text;
    }
    return entry;
}

static bool same_text(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

static bool same_entry(const focused_entry_t *before, const focused_entry_t *after)
{
    return before->kind == after->kind && before->value == after->value &&
           same_text(before->intent, after->intent) && same_text(before->digits, after->digits);
}

static uint16_t new_focus(const xiaotai_ui_layer_t *old, const xiaotai_view_t *next, bool identity,
                          bool shown, uint8_t *kind)
{
    const size_t items = next->item_count;
    const size_t fields = next->field_count;
    *kind = items ? XIAOTAI_UI_FOCUS_ITEM : fields ? XIAOTAI_UI_FOCUS_FIELD : XIAOTAI_UI_FOCUS_NONE;
    const size_t count = items ? items : fields;
    uint16_t index = (!identity && old->focus_kind == *kind) ? old->focus : 0U;
    if (shown && next->focus && (identity || !(next->flags & XIAOTAI_VIEW_FOCUS_ON_ENTRY)))
        index = next->focus - 1U;
    return index >= count ? (count ? (uint16_t)(count - 1U) : 0U) : index;
}

bool ui_scene_focus_changed(const xiaotai_ui_layer_t *old, const xiaotai_view_t *next, bool identity)
{
    xiaotai_ui_layer_t after = {.present = true, .view = *next};
    after.focus = new_focus(old, next, identity, true, &after.focus_kind);
    const focused_entry_t before = focused_entry(old), updated = focused_entry(&after);
    return !same_entry(&before, &updated);
}

static void take_focus(ui_display_t *display, xiaotai_view_layer_t layer, bool shown)
{
    xiaotai_ui_layer_t *out = ui_scene_layer(display, layer);
    const bool identity = (out->changed & XIAOTAI_CHANGED_IDENTITY) != 0U;
    uint8_t kind;
    const uint16_t next = new_focus(out, &out->view, identity, shown, &kind);
    ui_scene_focus(display, layer, kind, next);
    /* A fresh show is authoritative even if the app refused an edit and sent
     * the identical old value. Keep the cursor but discard provisional digits. */
    if (identity || shown) display->scene_layer[layer].digits[0] = '\0';
    if (identity) out->cursor = 0U;
    if (kind == XIAOTAI_UI_FOCUS_FIELD) {
        const xiaotai_view_field_t *field = &out->view.fields[next];
        if (field->kind != XIAOTAI_FIELD_DIGITS || field->max < 1) out->cursor = 0U;
        else if (field->max <= out->cursor) out->cursor = (uint8_t)(field->max - 1);
    }
}

static bool timer_running(const xiaotai_ui_layer_t *layer)
{
    return layer->present && (layer->view.timer_mode == XIAOTAI_TIMER_ELAPSED ||
           (layer->view.timer_mode == XIAOTAI_TIMER_REMAINING && !layer->timer_expired));
}

static void take_timer(ui_display_t *display, xiaotai_view_layer_t layer, const ui_take_t *take, uint32_t now)
{
    xiaotai_ui_layer_t *out = ui_scene_layer(display, layer);
    ui_scene_layer_t *state = &display->scene_layer[layer];
    if (out->changed & (XIAOTAI_CHANGED_TIMER | XIAOTAI_CHANGED_IDENTITY)) {
        const bool source = out->present && out->view.timer_mode != XIAOTAI_TIMER_NONE;
        if (source || state->timer_source) ++display->periodic_source;
        state->timer_source = source;
        state->timer_sample_ms = take->shown ? take->shown_ms : now;
        state->timer_elapsed_ms = 0U;
        out->timer_s = out->view.timer_s;
        out->timer_expired = false;
    }
    if (!timer_running(out)) return;
    state->timer_elapsed_ms += (uint32_t)(now - state->timer_sample_ms);
    state->timer_sample_ms = now;
    const int64_t seconds = (int64_t)(state->timer_elapsed_ms / 1000U);
    int64_t value = out->view.timer_s;
    const bool remaining = out->view.timer_mode == XIAOTAI_TIMER_REMAINING;
    value += remaining ? -seconds : seconds;
    if (remaining && value <= 0) {
        value = 0;
        out->timer_expired = true;
        out->changed |= XIAOTAI_CHANGED_TIMER;
    } else if (value > INT32_MAX) {
        value = INT32_MAX;
    }
    if (out->timer_s != value) out->changed |= XIAOTAI_CHANGED_TIMER;
    out->timer_s = (int32_t)value;
}

static bool clock_visible(const ui_display_t *display)
{
    const xiaotai_ui_layer_t *page = &display->scene.page, *overlay = &display->scene.overlay;
    if (overlay->present && overlay->view.kind == XIAOTAI_VIEW_CLOCK) return true;
    if (!page->present) return false;
    return page->view.kind == XIAOTAI_VIEW_CLOCK ||
           (page->view.kind == XIAOTAI_VIEW_HOME && !overlay->present && display->theme && display->theme->clock_on_home);
}

static void take_clock(ui_display_t *display, uint32_t now)
{
    xiaotai_ui_scene_t *scene = &display->scene;
    scene->clock_changed = false;
    if (!clock_visible(display)) {
        if (display->clock_due) ++display->periodic_source;
        display->clock_due = false;
        return;
    }
    if (display->clock_due && (int32_t)(now - display->clock_due_ms) < 0) return;
    const int64_t unix_ms = xiaotai_clock_unix_ms();
    xiaotai_clock_fields_t value;
    xiaotai_ui_clock_t next = {0};
    if (xiaotai_clock_synced() && xiaotai_clock_local(unix_ms, &value) == XIAOTAI_OK) {
        next = (xiaotai_ui_clock_t){true, (uint16_t)value.year, value.month, value.day, value.weekday,
                                    value.hour, value.minute, value.second};
    }
    scene->clock_changed = next.valid != scene->clock.valid || next.year != scene->clock.year ||
        next.month != scene->clock.month || next.day != scene->clock.day || next.hour != scene->clock.hour ||
        next.minute != scene->clock.minute;
    scene->clock = next;
    if (!display->clock_due) ++display->periodic_source;
    display->clock_due = true;
    display->clock_due_ms = now + (next.valid ? (uint32_t)(1000 - unix_ms % 1000) : 1000U);
}

bool ui_scene_step(ui_display_t *display, uint32_t now)
{
    return ui_scene_update(display, now, false);
}

bool ui_scene_update(ui_display_t *display, uint32_t now, bool read_pointer)
{
    xiaotai_ui_scene_t *scene = &display->scene;
    ui_take_t take[UI_LAYER_COUNT];
    ui_layers_take(display, now, scene, take);
    scene->now_ms = now;
    bool view_changed = (scene->page.changed & XIAOTAI_CHANGED_IDENTITY) != 0U;
    for (size_t n = 0; n < UI_LAYER_COUNT; ++n) {
        take_focus(display, n, take[n].shown);
        view_changed = view_changed || (n < 2U && take[n].focus_changed);
    }
    if (view_changed || (scene->overlay.changed & XIAOTAI_CHANGED_IDENTITY)) {
        xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
        ++display->input_epoch;
        xiaotai_mutex_unlock(&g_ui.lock);
    }
    if (view_changed) {
        xiaotai_args_t args = {.value = display->host.index};
        if (scene->page.view.id) {
            const size_t len = ui_store_utf8_prefix(scene->page.view.id, strlen(scene->page.view.id),
                                                     sizeof(args.text) - 1U);
            memcpy(args.text, scene->page.view.id, len);
        }
        (void)xiaotai_event_emit(XIAOTAI_EVENT_VIEW_CHANGED, &args, NULL, 0U);
    }
    ui_input_scene_changed(display, take);
    ui_input_step(display, now);
    if (display->part->desc.touch) ui_pointer_step(display, now, read_pointer);
    for (size_t n = 0; n < UI_LAYER_COUNT; ++n) take_timer(display, n, &take[n], now);
    take_clock(display, now);
    bool changed = scene->clock_changed;
    for (size_t n = 0; n < UI_LAYER_COUNT; ++n) {
        xiaotai_ui_layer_t *layer = ui_scene_layer(display, n);
        layer->changed |= display->scene_pending[n];
        display->scene_pending[n] = 0U;
        changed = changed || layer->changed != 0U;
    }
    return changed;
}

static void earliest(uint32_t now, uint32_t candidate, bool *found, uint32_t *due)
{
    if (!*found || (int32_t)(candidate - now) < (int32_t)(*due - now)) *due = candidate;
    *found = true;
}

bool ui_scene_next_due(ui_display_t *display, uint32_t now, uint32_t *due_ms)
{
    uint32_t due = 0, period = 0;
    bool found = ui_scene_nonperiodic_due(display, now, &due);
    if (ui_scene_periodic_due(display, now, &period)) earliest(now, period, &found, &due);
    if (found && due_ms) *due_ms = due;
    return found;
}

bool ui_scene_periodic_due(ui_display_t *display, uint32_t now, uint32_t *due_ms)
{
    uint32_t due = 0;
    bool found = false;
    for (size_t n = 0; n < UI_LAYER_COUNT; ++n) {
        ui_scene_layer_t *state = &display->scene_layer[n];
        if (timer_running(ui_scene_layer(display, n))) {
            earliest(now, state->timer_sample_ms + (1000U - state->timer_elapsed_ms % 1000U), &found, &due);
        }
    }
    if (display->clock_due) earliest(now, display->clock_due_ms, &found, &due);
    if (found && due_ms) *due_ms = due;
    return found;
}

bool ui_scene_nonperiodic_due(ui_display_t *display, uint32_t now, uint32_t *due_ms)
{
    uint32_t due = 0;
    bool found = ui_layers_next_due(display, &due);
    for (size_t n = 0; n < UI_LAYER_COUNT; ++n)
        if (display->scene_pending[n]) earliest(now, now, &found, &due);
    if (display->press.hold_intent[0] || display->press.hold_name_long) {
        earliest(now, display->press.hold_due_ms, &found, &due);
    }
    xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    if (display->nav_count) earliest(now, now, &found, &due);
    if (display->rejected.pending) earliest(now, display->rejected.due, &found, &due);
    xiaotai_mutex_unlock(&g_ui.lock);
    if (found && due_ms) *due_ms = due;
    return found;
}
