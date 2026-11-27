// SPDX-License-Identifier: MIT
/* Engine lifecycle, configuration and the view slots of the UI system.
 *
 * The owner task hands views over through the framework's view sink
 * (xiaotai_view_show): ui_view_accept() routes each one to its display and
 * layer and copies it into the layer's staged copy under the engine lock.
 * The layer rules of xiaotai/view.h are applied there: a new PAGE identity
 * removes the overlay (it belonged to the page) and keeps the toast, which
 * runs out its own duration; a new OVERLAY identity removes the toast. The
 * UI task takes the slots with ui_layers_take(), which also removes a toast
 * or a timed overlay once its duration ran out.
 *
 * start opens each display part whose descriptor a registered theme matches
 * and starts that theme on it; a display no theme matches stays closed and
 * its views are dropped and counted. Views shown between init and start are
 * kept and drawn after start. */
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/mem.h"

ui_engine_t g_ui;
xiaotai_ui_config_t g_ui_config;
char g_ui_theme_name[UI_THEME_NAME_MAX + 1];
static uint32_t next_generation;

/* Most displays one product can address: xiaotai_view_t.display is a byte. */
#define UI_DISPLAYS_MAX 256U

/* ------------------------------------------------------------------- clock */

static uint32_t port_clock_ms(void) { return (uint32_t)(xiaotai_time_us() / 1000); }

static uint32_t (*s_clock_ms)(void) = port_clock_ms;

uint32_t ui_now_ms(void) { return s_clock_ms(); }

void ui_core_set_clock(uint32_t (*clock_ms)(void)) { s_clock_ms = clock_ms ? clock_ms : port_clock_ms; }

/* Unsigned elapsed time works for the full uint32_t duration, including
 * intervals longer than half a clock cycle. Only timed copies get here. */
static uint32_t remaining_ms(const ui_copy_t *copy, uint32_t now)
{
    const uint32_t duration = copy->view.duration_ms ? copy->view.duration_ms : XIAOTAI_TOAST_DEFAULT_MS;
    const uint32_t elapsed = now - (copy->deadline_ms - duration);
    return elapsed < duration ? duration - elapsed : 0U;
}

static void lock(void) { (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g_ui.lock); }

/* --------------------------------------------------------------- slots */

static uint32_t layer_capacity(xiaotai_view_layer_t layer)
{
    return layer == XIAOTAI_LAYER_TOAST ? XIAOTAI_UI_TOAST_BYTES : g_ui.layer_bytes;
}

static size_t slot_stride(uint32_t capacity)
{
    /* Both arenas hold typed objects. Pad the stride, not the copy budget. */
    const uint64_t mask = _Alignof(max_align_t) - 1U;
    const uint64_t stride = ((uint64_t)capacity + mask) & ~mask;
    return stride <= SIZE_MAX / 2U ? (size_t)stride : 0;
}

/* The block of both copies; outside the lock (the owner task is the only
 * writer of `block`, and the UI task does not look at a slot without it). */
static uint8_t *slot_allocate(uint32_t capacity)
{
    const size_t stride = slot_stride(capacity);
    return stride ? xiaotai_mem_aligned(XIAOTAI_MEM_BULK, _Alignof(max_align_t), stride * 2U) : NULL;
}

static void slot_attach(ui_slot_t *slot, uint8_t *block, uint32_t capacity)
{
    memset(slot, 0, sizeof(*slot));
    slot->block = block;
    slot->capacity = capacity;
    slot->copies[0].arena = block;
    slot->copies[1].arena = block + slot_stride(capacity);
    slot->staged = &slot->copies[0];
    slot->current = &slot->copies[1];
}

static ui_copy_t *slot_latest(ui_slot_t *slot) { return slot->pending ? slot->staged : slot->current; }

static void copy_clear(ui_copy_t *copy)
{
    memset(&copy->view, 0, sizeof(copy->view));
    copy->present = false;
    copy->has_deadline = false;
    copy->deadline_ms = 0U;
}

/* Under the lock: the layer shows nothing from the next take on. */
static void slot_remove(ui_slot_t *slot)
{
    if (!slot->block || !slot_latest(slot)->present) return;
    copy_clear(slot->staged);
    slot->pending = true;
    slot->identity_changed = true;
}

static const char local_rejection_id[] = "ui.reject.local";

static bool is_local_rejection(const ui_copy_t *copy)
{
    return copy->present && copy->view.id && !strcmp(copy->view.id, local_rejection_id);
}

static void change_rejection_scope(ui_display_t *display)
{
    ++display->rejection_scope;
    /* Keep the local feedback on the originating controls, including if a
     * different view is staged before the UI task has painted the notice. */
    ui_slot_t *toast = &display->slots[XIAOTAI_LAYER_TOAST];
    if (!toast->block) return;
    if (toast->pending && is_local_rejection(toast->staged)) {
        toast->pending = toast->shown = toast->identity_changed = false;
    }
    if (!toast->pending && is_local_rejection(toast->current)) slot_remove(toast);
}

static void display_free_slots(ui_display_t *display)
{
    uint8_t *blocks[UI_LAYER_COUNT];
    lock();
    for (size_t layer = 0; layer < UI_LAYER_COUNT; ++layer) {
        blocks[layer] = display->slots[layer].block;
        memset(&display->slots[layer], 0, sizeof(display->slots[layer]));
    }
    unlock();
    for (size_t layer = 0; layer < UI_LAYER_COUNT; ++layer) xiaotai_mem_free(blocks[layer]);
}

/* ------------------------------------------------------------- accepting */

static void show_view(ui_display_t *display, xiaotai_view_layer_t layer, const xiaotai_view_t *view, uint32_t now)
{
    ui_slot_t *slot = &display->slots[layer];
    const ui_copy_t *latest = slot_latest(slot);
    const bool new_identity =
        !latest->present || !ui_store_same_identity(view, slot->capacity, layer, &latest->view);
    if (layer != XIAOTAI_LAYER_TOAST &&
        (new_identity || !ui_store_same_controls(&latest->view, view))) change_rejection_scope(display);
    const bool had_deadline = latest->present && latest->has_deadline;
    const uint32_t old_deadline = latest->deadline_ms;
    const uint32_t old_duration = latest->view.duration_ms;

    ui_copy_t *staged = slot->staged;
    display->counters.truncated += ui_store_copy(staged, slot->capacity, view, layer);
    staged->shown_ms = now;
    staged->has_deadline = false;
    if (layer == XIAOTAI_LAYER_TOAST) {
        /* Every toast restarts the duration: a new notice replaces the old. */
        staged->has_deadline = true;
        staged->deadline_ms = now + (view->duration_ms ? view->duration_ms : XIAOTAI_TOAST_DEFAULT_MS);
    } else if (layer == XIAOTAI_LAYER_OVERLAY && view->duration_ms) {
        /* An update of a timed dialog keeps its deadline unless the duration
         * itself changed, so refreshing its content does not keep it up. */
        staged->has_deadline = true;
        staged->deadline_ms = (!new_identity && had_deadline && old_duration == view->duration_ms)
                                  ? old_deadline
                                  : now + view->duration_ms;
    }
    slot->pending = true;
    slot->shown = true;
    if (new_identity) {
        slot->identity_changed = true;
        ++display->counters.shown;
        if (layer == XIAOTAI_LAYER_PAGE) slot_remove(&display->slots[XIAOTAI_LAYER_OVERLAY]);
        if (layer == XIAOTAI_LAYER_OVERLAY) slot_remove(&display->slots[XIAOTAI_LAYER_TOAST]);
    } else {
        ++display->counters.updated;
    }
}

static void dismiss_view(ui_display_t *display, xiaotai_view_layer_t layer, const char *id)
{
    ui_slot_t *slot = &display->slots[layer];
    if (!slot->block) return;
    const ui_copy_t *latest = slot_latest(slot);
    if (!latest->present) return;
    /* With an id, only that view goes: a dialog that was already replaced stays. */
    if (id && (!latest->view.id || strcmp(latest->view.id, id) != 0)) return;
    if (layer != XIAOTAI_LAYER_TOAST) change_rejection_scope(display);
    slot_remove(slot);
    /* Without its page the dialog has nothing to belong to. */
    if (layer == XIAOTAI_LAYER_PAGE) slot_remove(&display->slots[XIAOTAI_LAYER_OVERLAY]);
}

void ui_view_accept(const xiaotai_view_t *view)
{
    if (!view || !g_ui.initialized || g_ui.display_count == 0U) return;
    const uint32_t now = ui_now_ms();
    ui_display_t *display = view->display < g_ui.display_count ? &g_ui.displays[view->display] : NULL;
    const xiaotai_view_layer_t layer = (xiaotai_view_layer_t)view->layer;
    const bool dismiss = (view->flags & XIAOTAI_VIEW_DISMISS) != 0U;

    /* The overlay's buffers are taken when a dialog is first shown, and kept. */
    uint8_t *block = NULL;
    if (display && display->state != UI_DISPLAY_CLOSED && layer == XIAOTAI_LAYER_OVERLAY && !dismiss &&
        !display->slots[layer].block) {
        block = slot_allocate(layer_capacity(layer));
    }

    lock();
    if (!display) {
        /* A view for a display the board does not have. */
        ++g_ui.displays[0].counters.dropped;
    } else if (display->state == UI_DISPLAY_CLOSED || layer > XIAOTAI_LAYER_TOAST) {
        ++display->counters.dropped;
    } else if (dismiss) {
        dismiss_view(display, layer, view->id);
    } else {
        ui_slot_t *slot = &display->slots[layer];
        if (!slot->block && block) {
            slot_attach(slot, block, layer_capacity(layer));
            block = NULL;
        }
        if (slot->block) {
            show_view(display, layer, view, now);
        } else {
            ++display->counters.dropped;   /* no memory for the overlay */
        }
    }
    unlock();
    xiaotai_mem_free(block);
    if (display && layer != XIAOTAI_LAYER_TOAST) ui_trace_shown(display);
    if (display) (void)ui_driver_kick(display);
}

static void view_sink(const xiaotai_view_t *view, void *ctx)
{
    (void)ctx;
    ui_view_accept(view);
}

void ui_local_rejection_toast(ui_display_t *display, const char *text, uint32_t now)
{
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_NOTICE, .layer = XIAOTAI_LAYER_TOAST, .id = local_rejection_id, .text = text,
    };
    lock();
    const bool current = display->state == UI_DISPLAY_READY && display->slots[XIAOTAI_LAYER_TOAST].block &&
        display->rendered_rejection_scope == display->rejection_scope;
    if (current) show_view(display, XIAOTAI_LAYER_TOAST, &view, now);
    unlock();
    if (current) (void)ui_driver_kick(display);
}

/* ---------------------------------------------------------------- taking */

bool ui_layers_take(ui_display_t *display, uint32_t now_ms, xiaotai_ui_scene_t *scene,
                    ui_take_t take[UI_LAYER_COUNT])
{
    xiaotai_ui_layer_t *out[UI_LAYER_COUNT] = {&scene->page, &scene->overlay, &scene->toast};
    bool any = false;
    lock();
    for (size_t layer = 0; layer < UI_LAYER_COUNT; ++layer) {
        ui_slot_t *slot = &display->slots[layer];
        uint32_t changed = 0U;
        take[layer] = (ui_take_t){0};
        if (slot->block && slot->pending) {
            ui_copy_t *staged = slot->staged;
            ui_copy_t *current = slot->current;
            if (!staged->present && !current->present) {
                changed = 0U;
            } else if (slot->identity_changed || staged->present != current->present) {
                changed = UI_CHANGED_ALL;
            } else {
                changed = ui_store_diff(&current->view, &staged->view);
            }
            if (out[layer] == ui_scene_layer(display, layer) && staged->present) {
                take[layer].focus_changed = ui_scene_focus_changed(out[layer], &staged->view,
                                                  (changed & XIAOTAI_CHANGED_IDENTITY) != 0U);
                if (changed && display->press.down && display->press.layer == (xiaotai_view_layer_t)layer) {
                    take[layer].press_changed = ui_input_target_changed(&display->press, &current->view,
                                                                        &staged->view);
                }
            }
            slot->current = staged;
            slot->staged = current;
            slot->pending = false;
            slot->identity_changed = false;
            take[layer].shown = slot->shown;
            take[layer].shown_ms = staged->shown_ms;
            slot->shown = false;
        }
        ui_copy_t *current = slot->block ? slot->current : NULL;
        if (current && current->present && current->has_deadline && remaining_ms(current, now_ms) == 0U) {
            copy_clear(current);
            changed = UI_CHANGED_ALL;
            if (layer != XIAOTAI_LAYER_TOAST) change_rejection_scope(display);
        }
        out[layer]->present = current && current->present;
        if (out[layer]->present) {
            out[layer]->view = current->view;
        } else {
            memset(&out[layer]->view, 0, sizeof(out[layer]->view));
        }
        out[layer]->changed = changed;
        any = any || changed != 0U;
    }
    display->rendered_rejection_scope = display->rejection_scope;
    unlock();
    return any;
}

bool ui_layers_next_due(ui_display_t *display, uint32_t *due_ms)
{
    bool found = false;
    uint32_t due = 0U;
    uint32_t earliest = 0U;
    const uint32_t now = ui_now_ms();
    lock();
    for (size_t layer = XIAOTAI_LAYER_OVERLAY; layer < UI_LAYER_COUNT; ++layer) {
        ui_slot_t *slot = &display->slots[layer];
        if (!slot->block) continue;
        const ui_copy_t *copies[2] = {slot->current, slot->pending ? slot->staged : NULL};
        for (size_t i = 0; i < 2U; ++i) {
            const ui_copy_t *copy = copies[i];
            if (!copy || !copy->present || !copy->has_deadline) continue;
            const uint32_t remaining = remaining_ms(copy, now);
            if (!found || remaining < earliest) {
                due = copy->deadline_ms;
                earliest = remaining;
            }
            found = true;
        }
    }
    unlock();
    if (found && due_ms) *due_ms = due;
    return found;
}

ui_display_t *ui_display_at(size_t index)
{
    return g_ui.initialized && index < g_ui.display_count ? &g_ui.displays[index] : NULL;
}

/* ----------------------------------------------------------- configuration */

xiaotai_err_t xiaotai_ui_configure(const xiaotai_ui_config_t *config)
{
    if (!config) return XIAOTAI_ERR_INVALID_ARG;
    if (config->theme && strnlen(config->theme, UI_THEME_NAME_MAX + 1U) > UI_THEME_NAME_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (g_ui.started) return XIAOTAI_ERR_INVALID_STATE;
    /* The glue configures before any init. From an init after the engine's
     * own, layer_bytes no longer applies: the page buffers are already taken
     * (the other members are read later and do apply). */
    g_ui_config = *config;
    memset(g_ui_theme_name, 0, sizeof(g_ui_theme_name));
    if (config->theme) strncpy(g_ui_theme_name, config->theme, UI_THEME_NAME_MAX);
    g_ui_config.theme = g_ui_theme_name[0] ? g_ui_theme_name : NULL;
    return XIAOTAI_OK;
}

/* ---------------------------------------------------------------- lifecycle */

static void engine_free(void)
{
#if XIAOTAI_UI_LVGL
    ui_lvgl_shutdown();
#endif
    if (g_ui.displays) {
        for (size_t i = 0; i < g_ui.display_count; ++i) {
            ui_input_reset(&g_ui.displays[i]);
            for (size_t layer = 0; layer < UI_LAYER_COUNT; ++layer) {
                xiaotai_mem_free(g_ui.displays[i].slots[layer].block);
            }
        }
        xiaotai_mem_free(g_ui.displays);
    }
    ui_registry_free();
    if (g_ui.lock_ready) xiaotai_mutex_deinit(&g_ui.lock);
    memset(&g_ui, 0, sizeof(g_ui));
}

xiaotai_err_t xiaotai_engine_ui_init(void)
{
    if (g_ui.initialized) return XIAOTAI_ERR_INVALID_STATE;
    memset(&g_ui, 0, sizeof(g_ui));
    if (++next_generation > (UINT32_MAX >> 8U)) next_generation = 1U;
    g_ui.generation = next_generation;
    g_ui.layer_bytes = g_ui_config.layer_bytes
                           ? g_ui_config.layer_bytes
                           : (xiaotai_mem_has_external() ? UI_LAYER_BYTES_EXTERNAL : UI_LAYER_BYTES_INTERNAL);
    g_ui.sleep_timeout_ms = g_ui_config.sleep_timeout_ms;
    xiaotai_err_t err = xiaotai_mutex_init(&g_ui.lock);
    if (err != XIAOTAI_OK) return err;
    g_ui.lock_ready = true;
    err = ui_registry_init();
    if (err != XIAOTAI_OK) {
        engine_free();
        return err;
    }

    size_t count = 0U;
    while (count < UI_DISPLAYS_MAX && xiaotai_hal_display(count)) ++count;
    if (count > 0U) {
        g_ui.displays = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, count, sizeof(*g_ui.displays));
        if (!g_ui.displays) {
            engine_free();
            return XIAOTAI_ERR_NO_MEM;
        }
        g_ui.display_count = count;
    }
    for (size_t i = 0; i < count; ++i) {
        ui_display_t *display = &g_ui.displays[i];
        display->part = xiaotai_hal_display(i);
        display->host.index = (uint8_t)i;
        display->host.part = display->part;
        /* The page and the toast are there from the start; the overlay when
         * a dialog is first shown. */
        const xiaotai_view_layer_t eager[2] = {XIAOTAI_LAYER_PAGE, XIAOTAI_LAYER_TOAST};
        for (size_t e = 0; e < 2U; ++e) {
            const uint32_t capacity = layer_capacity(eager[e]);
            uint8_t *block = slot_allocate(capacity);
            if (!block) {
                engine_free();
                return XIAOTAI_ERR_NO_MEM;
            }
            slot_attach(&display->slots[eager[e]], block, capacity);
        }
    }
    /* Standalone host fixtures initialize the engine without a framework. */
    if (xiaotai_on_owner()) {
        err = ui_input_register();
        if (err == XIAOTAI_OK) err = ui_diag_init();
        if (err == XIAOTAI_OK) err = ui_commands_init();
        if (err != XIAOTAI_OK) {
            engine_free();
            return err;
        }
    }
    g_ui.initialized = true;
    xiaotai_view_set_sink(view_sink, NULL);
    return XIAOTAI_OK;
}

static void display_close(ui_display_t *display, const char *label, int32_t value)
{
    xiaotai_eventlog_warn("ui", label, 0U, value);
    ++g_ui.displays_closed;
    lock();
    display->state = UI_DISPLAY_CLOSED;
    unlock();
    display_free_slots(display);
}

/* The theme for one display: the configured one for display 0 when it is
 * registered and draws that display, else the best match. */
static const xiaotai_theme_t *display_theme(size_t index, ui_display_t *display)
{
    const xiaotai_display_desc_t *desc = &display->part->desc;
    if (index == 0U && g_ui_theme_name[0]) {
        const xiaotai_theme_t *named = ui_registry_theme_named(g_ui_theme_name);
        if (named && ui_registry_score(named, desc) > 0) return named;
        /* 1: no theme of that name; 2: it cannot draw this display. */
        lock();
        display->theme_refused = true;
        unlock();
        xiaotai_eventlog_warn("ui", "theme.refused", 0U, named ? 2 : 1);
    }
    return xiaotai_ui_theme_select(desc);
}

static xiaotai_err_t bind_display(void *ctx, const xiaotai_display_handles_t *handles, bool *released)
{
    ui_display_t *display = ctx;
    const xiaotai_theme_t *theme = display->theme;
    xiaotai_err_t err;
    display->handles = *handles;
#if XIAOTAI_UI_LVGL
    display->native_cleanup_error = XIAOTAI_OK;
    if (theme->backend == XIAOTAI_UI_BACKEND_LVGL) {
        display->driver.start_stage = "display.backend_failed";
        err = ui_lvgl_open(display);
        if (err != XIAOTAI_OK) {
            goto failed;
        }
    }
#endif
#if XIAOTAI_UI_LVGL
    ui_lvgl_enter(display);
#endif
    display->driver.start_stage = "theme.start_failed";
    err = theme->start(&display->host, &display->theme_state);
    if (err != XIAOTAI_OK) {
        display->theme_state = NULL;
#if XIAOTAI_UI_LVGL
        ui_lvgl_leave(display);
        ui_lvgl_close(display);
#endif
        goto failed;
    }
    /* Keep the LVGL lock through publication, after the display commits. */
    return XIAOTAI_OK;
failed:
    if (released) {
        *released = true;
#if XIAOTAI_UI_LVGL
        if (display->native_cleanup_error != XIAOTAI_OK) {
            *released = false;
            xiaotai_eventlog_warn("ui", "display.native_cleanup_failed", 0U, display->native_cleanup_error);
        }
#endif
        display->handles = (xiaotai_display_handles_t){0};
    }
    return err;
}

xiaotai_err_t ui_display_open(ui_display_t *display)
{
    xiaotai_err_t err = ui_pointer_open(display);
    if (err != XIAOTAI_OK) return err;
    if (display->theme->backend == XIAOTAI_UI_BACKEND_FRAME) {
        display->driver.start_stage = "frame.open_failed";
        err = ui_frame_open(display);
        if (err != XIAOTAI_OK) {
            ui_pointer_close(display);
            return err;
        }
    }
    display->driver.start_stage = "display.open_failed";
    const xiaotai_display_t *part = display->part;
    if (part->open_bound) {
        err = part->open_bound(part->self, bind_display, display);
    } else {
        xiaotai_display_handles_t handles = {0};
        err = part->open(part->self, &handles);
        if (err == XIAOTAI_OK) err = bind_display(display, &handles, NULL);
    }
    if (err != XIAOTAI_OK) {
        ui_frame_close(display);
        ui_pointer_close(display);
        return err;
    }
    const uint32_t now = ui_now_ms();
    ui_power_init(display, now);
#if XIAOTAI_UI_LVGL
    ui_lvgl_leave(display);
#endif
    display->driver.pointer_due = now;
    lock();
    display->state = UI_DISPLAY_READY;
    unlock();
    return XIAOTAI_OK;
}

void ui_display_stop(ui_display_t *display)
{
    ui_input_end_local_hold(display);
    ui_power_shutdown(display);
    ui_diag_shutdown(display);
    lock();
    const bool ready = display->state == UI_DISPLAY_READY;
    if (ready) display->state = UI_DISPLAY_IDLE;
    display->rejected.ticket = 0U;
    display->rejected.pending = false;
    unlock();
    if (ready) {
        if (display->part->set_backlight) (void)display->part->set_backlight(display->part->self, 0);
#if XIAOTAI_UI_LVGL
        ui_lvgl_enter(display);
#endif
        if (display->theme->stop) display->theme->stop(display->theme_state);
#if XIAOTAI_UI_LVGL
        ui_lvgl_leave(display);
        ui_lvgl_close(display);
#endif
        display->theme_state = NULL;
    }
    ui_frame_close(display);
    ui_pointer_close(display);
}

static void display_start(size_t index, ui_display_t *display)
{
    display->theme = display_theme(index, display);
    if (!display->theme) {
        display_close(display, "display.no_theme", (int32_t)index);
        return;
    }
#if !XIAOTAI_UI_LVGL
    if (display->theme->backend == XIAOTAI_UI_BACKEND_LVGL) {
        display->theme = NULL;
        display_close(display, "theme.no_backend", (int32_t)index);
        return;
    }
#endif
    display->driver.start_stage = "display.start_failed";
    const xiaotai_err_t err = ui_driver_start(display);
    if (err != XIAOTAI_OK) {
        display->theme = NULL;
        display_close(display, display->driver.start_stage, (int32_t)err);
    }
}

void xiaotai_engine_ui_start(void)
{
    if (!g_ui.initialized || g_ui.started) return;
    g_ui.started = true;
    for (size_t i = 0; i < g_ui.display_count; ++i) display_start(i, &g_ui.displays[i]);
}

static void reset_ui(void *arg)
{
    if (g_ui.initialized) {
        xiaotai_view_set_sink(NULL, NULL);
        for (size_t i = 0; i < g_ui.display_count; ++i) {
            ui_driver_stop(&g_ui.displays[i]);
#if XIAOTAI_UI_LVGL
            if (g_ui.displays[i].closed_video_release) g_ui.displays[i].closed_video_release();
#endif
        }
    }
    engine_free();
    memset(&g_ui_config, 0, sizeof(g_ui_config));
    memset(g_ui_theme_name, 0, sizeof(g_ui_theme_name));
    if (arg) xiaotai_signal_give(arg);
}

void xiaotai_ui_reset(void)
{
    /* Host-test entry: cancel owner timers and release presenter pins on the
     * owner. After the framework stopped no queued timer can still run. */
    if (!xiaotai_on_owner()) {
        bool route_owner = false;
        for (size_t i = 0; i < g_ui.display_count; ++i) {
            const ui_display_t *display = &g_ui.displays[i];
            if (!display->driver.driven) continue;
            route_owner |= display->driver.owner_timer;
#if XIAOTAI_UI_LVGL
            route_owner |= display->theme && display->theme->backend == XIAOTAI_UI_BACKEND_LVGL;
#endif
        }
        if (route_owner) {
            xiaotai_signal_t done;
            if (xiaotai_signal_init(&done) != XIAOTAI_OK) return;
            const xiaotai_err_t queued = xiaotai_run(reset_ui, &done);
            if (queued == XIAOTAI_OK) (void)xiaotai_signal_take(&done, XIAOTAI_WAIT_FOREVER);
            xiaotai_signal_deinit(&done);
            if (queued != XIAOTAI_ERR_INVALID_STATE) return;
        }
    }
    reset_ui(NULL);
}

/* ------------------------------------------------------------------ inspect */

static const char *const KIND_NAMES[] = {"home", "talk", "call", "list", "code",
                                         "notice", "progress", "clock", "custom", "form"};

void ui_view_name(const xiaotai_view_t *view, char *out, size_t capacity)
{
    out[0] = '\0';
    if (!view) return;
    const char *name = view->id;
    if (!name && view->kind == XIAOTAI_VIEW_CUSTOM) name = view->custom;
    if (!name) {
        name = (size_t)view->kind < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]) ? KIND_NAMES[view->kind] : "unknown";
    }
    size_t length = ui_store_utf8_prefix(name, strlen(name), capacity - 1U);
    memcpy(out, name, length);
    out[length] = '\0';
}

xiaotai_err_t xiaotai_ui_inspect(uint8_t index, xiaotai_ui_info_t *out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    ui_display_t *display = ui_display_at(index);
    if (!display) return XIAOTAI_ERR_NOT_FOUND;
    memset(out, 0, sizeof(*out));
    out->width = display->part->desc.width;
    out->height = display->part->desc.height;
    out->pixel = display->part->desc.pixel;
    lock();
    out->ready = display->state == UI_DISPLAY_READY;
    /* The owner installs the theme before calling start so that the theme
     * can resolve its templates. Publish it to other tasks only on READY. */
    if (out->ready) snprintf(out->theme, sizeof(out->theme), "%s", display->theme->name);
    if (display->slots[XIAOTAI_LAYER_PAGE].block) {
        const ui_copy_t *page = slot_latest(&display->slots[XIAOTAI_LAYER_PAGE]);
        ui_view_name(page->present ? &page->view : NULL, out->page, sizeof(out->page));
        out->page_kind = page->present ? (uint8_t)page->view.kind : 0U;
    }
    if (display->slots[XIAOTAI_LAYER_OVERLAY].block) {
        const ui_copy_t *overlay = slot_latest(&display->slots[XIAOTAI_LAYER_OVERLAY]);
        ui_view_name(overlay->present ? &overlay->view : NULL, out->overlay, sizeof(out->overlay));
    }
    out->focus = display->focus_visible;
    out->shown = display->counters.shown;
    out->updated = display->counters.updated;
    out->dropped = display->counters.dropped;
    out->truncated = display->counters.truncated;
    out->ignored = display->counters.ignored;
    out->posted = display->counters.posted;
    out->rejected = display->counters.rejected;
    out->theme_refused = display->theme_refused;
    out->owner_timer = out->ready && display->driver.owner_timer;
    out->awake = out->ready && display->power.awake;
    out->present_fault = display->frame.fault;
    out->present_errors = display->frame.errors;
    out->frames = display->frames;
    out->slow_frames = display->slow_frames;
    out->max_frame_ms = display->max_frame_ms;
    out->late_steps = display->late_steps;
    out->video_stale = display->video_stale;
    out->stack_free = display->stack_free;
    unlock();
    return XIAOTAI_OK;
}

/* ---------------------------------------------------------------- host */

const xiaotai_display_desc_t *xiaotai_ui_host_display(const xiaotai_ui_host_t *host)
{
    return host ? &host->part->desc : NULL;
}

uint8_t xiaotai_ui_host_index(const xiaotai_ui_host_t *host) { return host ? host->index : 0U; }

void *xiaotai_ui_host_root(const xiaotai_ui_host_t *host) { return host ? host->root : NULL; }
