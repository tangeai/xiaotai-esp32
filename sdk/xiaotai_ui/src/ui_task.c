// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "xiaotai/eventlog.h"

#define UI_TASK_WAIT_MS 100U

static bool manual_mode;

void ui_driver_manual(bool manual) { manual_mode = manual; }

static void lock(void) { (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g_ui.lock); }
static bool reached(uint32_t now, uint32_t due) { return (int32_t)(now - due) >= 0; }

static bool stopping(ui_display_t *d)
{
    lock();
    const bool stop = d->driver.stop;
    unlock();
    return stop;
}

bool ui_driver_context(const ui_display_t *d)
{
    if (!d->driver.driven) return true;
    return d->driver.owner_timer ? xiaotai_on_owner() : xiaotai_task_is_current(&d->driver.task);
}

xiaotai_err_t ui_driver_wait_guard(const ui_display_t *d)
{
    if (!d) return XIAOTAI_ERR_NOT_FOUND;
    lock();
    const bool ready = d->state == UI_DISPLAY_READY && !d->driver.stop;
    unlock();
    return ready && !ui_driver_context(d) ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

static void late(ui_display_t *d)
{
    lock();
    ++d->late_steps;
    unlock();
}

static void render(ui_display_t *d, bool force)
{
    /* Changes belong to the scene since the last paint, not the last step.
     * Wake/refresh repaints the same view; only real scene changes rebuild it. */
    for (size_t layer = 0; layer < UI_LAYER_COUNT; ++layer) {
        xiaotai_ui_layer_t *out = ui_scene_layer(d, (xiaotai_view_layer_t)layer);
        out->changed |= d->driver.unpainted[layer];
        d->driver.unpainted[layer] = 0;
    }
    if (force) d->scene.clock_changed = true;
    const uint32_t start = ui_now_ms();
    if (!ui_render_scene(d)) {
        xiaotai_eventlog_warn("ui", "frame.owner_workspace_unavailable", 0U, d->host.index);
        return;
    }
    const uint32_t elapsed = ui_now_ms() - start;
    const size_t visible = d->theme->controls ? d->theme->controls(d->theme_state, NULL, 0) : 0;
    const xiaotai_ui_layer_t *top = ui_scene_layer(d, ui_scene_top(d));
    lock();
    ++d->frames;
    d->render_ms += elapsed;
    d->controls_visible = visible;
    d->focus_visible = top->present && top->focus_kind != XIAOTAI_UI_FOCUS_NONE ? top->focus + 1U : 0U;
    if (elapsed > d->max_frame_ms) d->max_frame_ms = elapsed;
    if (elapsed > XIAOTAI_UI_SLOW_FRAME_MS) ++d->slow_frames;
    unlock();
    /* The next render starts animation timing afresh. */
    d->driver.tick = d->theme->tick != NULL;
    d->driver.tick_due = ui_now_ms();
}

static void periodic(ui_display_t *d, uint32_t now)
{
    ui_driver_t *s = &d->driver;
    uint32_t desired = 0;
    const bool has = ui_scene_periodic_due(d, now, &desired);
    const bool source_changed = s->periodic_source != d->periodic_source;
    s->periodic_source = d->periodic_source;
    if (!has || !s->owner_timer || source_changed) {
        s->periodic = has;
        s->periodic_due = desired;
        s->rebased = false;
        return;
    }
    if (s->periodic && reached(now, s->periodic_due)) {
        if (now - s->periodic_due > 1000U) {
            s->rebased = true;
            s->periodic_due = now + 1000U;
            late(d);
            return;
        }
        if (s->rebased) {
            s->periodic_due += 1000U;
            return;
        }
    }
    if (!s->rebased) s->periodic_due = desired;
    s->periodic = true;
}

void ui_driver_step(ui_display_t *d, uint32_t now)
{
    ui_driver_t *s = &d->driver;
    const int64_t began = xiaotai_time_us();
    const uint32_t flushed = d->flushes;
#if XIAOTAI_UI_LVGL
    ui_lvgl_enter(d);
#endif
    /* With a task for it the pointer is read beside this one. */
    const bool sampled = d->touch && d->touch->sampling;
    const bool read_pointer = d->part->desc.touch && !sampled && reached(now, s->pointer_due);
    if (read_pointer) {
        const uint32_t period = XIAOTAI_UI_POINTER_READ_MS;
        if (s->owner_timer && now - s->pointer_due > period) {
            s->pointer_due = now + period;
            late(d);
        } else {
            if (now - s->pointer_due > period) s->pointer_due = now;
            s->pointer_due += period;
        }
    }
    const bool changed = ui_scene_update(d, now, read_pointer);
    ui_power_step(d, now);
    periodic(d, now);

    if (d->power.awake) {
        lock();
        const bool redraw = d->power.redraw;
        const bool force_frame = d->power.force_frame;
        d->power.redraw = d->power.force_frame = false;
        unlock();
        if (force_frame) {
            ui_frame_unlatch(d);
#if XIAOTAI_UI_LVGL
            ui_lvgl_invalidate_screen(d);
#endif
        }
        if (changed || redraw) render(d, redraw);
        if (s->tick && reached(now, s->tick_due)) {
            const uint32_t delay = d->theme->tick(d->theme_state, now);
            s->tick = delay != 0;
            s->tick_due = now + (delay > XIAOTAI_TIMER_DELAY_MAX_MS ? XIAOTAI_TIMER_DELAY_MAX_MS : delay);
        }
    } else {
        for (size_t layer = 0; layer < UI_LAYER_COUNT; ++layer)
            s->unpainted[layer] |= ui_scene_layer(d, (xiaotai_view_layer_t)layer)->changed;
    }
#if XIAOTAI_UI_LVGL
    ui_lvgl_step(d, now);
#endif
    ui_pointer_settle(d);
    if (d->flushes != flushed) {
        ui_trace_painted(d, changed);
        ui_trace_span(d, UI_SPAN_STEP, xiaotai_time_us() - began);
    }
    ui_diag_step(d);
#if XIAOTAI_UI_LVGL
    s->busy = ui_lvgl_busy(d);
    ui_lvgl_leave(d);
#endif
}

static void choose(uint32_t now, uint32_t due, bool *found, uint32_t *best)
{
    const uint32_t delay = reached(now, due) ? 0U : due - now;
    if (!*found || delay < *best) *best = delay;
    *found = true;
}

static bool next_delay(ui_display_t *d, uint32_t now, uint32_t *delay)
{
    ui_driver_t *s = &d->driver;
    bool found = false;
    uint32_t due;
    if (ui_pointer_due(d, now, &due)) choose(now, due, &found, delay);
    if (ui_diag_pending(d)) choose(now, now, &found, delay);
    if (ui_scene_nonperiodic_due(d, now, &due)) choose(now, due, &found, delay);
    if (ui_power_next_due(d, now, &due)) choose(now, due, &found, delay);
    if (s->periodic) choose(now, s->periodic_due, &found, delay);
    if (d->part->desc.touch && !(d->touch && d->touch->sampling)) choose(now, s->pointer_due, &found, delay);
#if XIAOTAI_UI_LVGL
    /* Something moves, or video may have a frame; otherwise a touch, a view
     * or a timer wakes the task. */
    if (d->lvgl && d->power.awake && s->busy) choose(now, now + XIAOTAI_UI_VIDEO_POLL_MS, &found, delay);
#endif
    if (d->power.awake && s->tick) choose(now, s->tick_due, &found, delay);
    return found;
}

static void fire(void *ctx);
static void kicked(void *ctx);

static xiaotai_err_t arm(ui_display_t *d, uint32_t now, uint32_t delay)
{
    ui_driver_t *s = &d->driver;
    if (s->id != 0U) {
        uint32_t current = reached(now, s->due) ? 0U : s->due - now;
        if (current <= delay) return XIAOTAI_OK;
        xiaotai_timer_cancel(s->id);
        s->id = 0;
    }
    const xiaotai_err_t err = xiaotai_timer_start(delay, fire, d, &s->id);
    if (err == XIAOTAI_OK) s->due = now + delay;
    else xiaotai_eventlog_warn("ui", "driver.timer_failed", d->host.index, err);
    return err;
}

static void fire(void *ctx)
{
    ui_display_t *d = ctx;
    d->driver.id = 0;
    if (stopping(d)) return;
    ui_driver_step(d, ui_now_ms());
    const uint32_t now = ui_now_ms();
    uint32_t delay = 0;
    if (next_delay(d, now, &delay)) (void)arm(d, now, delay);
}

static bool dispatchable(const ui_display_t *d)
{
    return d->driver.driven && d->state == UI_DISPLAY_READY && !d->driver.stop;
}

xiaotai_err_t ui_driver_post_locked(ui_display_t *d)
{
    if (!dispatchable(d)) return XIAOTAI_OK;
    if (!d->driver.owner_timer) {
        xiaotai_task_notify(&d->driver.task);
        return XIAOTAI_OK;
    }
    if (d->driver.kick_pending) return XIAOTAI_OK;
    d->driver.kick_pending = true;
    /* The owner may stop the engine before the framework drains this job. */
    const uintptr_t token = ((uintptr_t)g_ui.generation << 8U) | d->host.index;
    const xiaotai_err_t err = xiaotai_run(kicked, (void *)token);
    if (err != XIAOTAI_OK) d->driver.kick_pending = false;
    return err;
}

xiaotai_err_t ui_driver_kick(ui_display_t *d)
{
    lock();
    if (dispatchable(d) && d->driver.owner_timer && xiaotai_on_owner()) {
        unlock();
        return arm(d, ui_now_ms(), 0);
    }
    const xiaotai_err_t err = ui_driver_post_locked(d);
    unlock();
    return err;
}

static void kicked(void *ctx)
{
    const uintptr_t token = (uintptr_t)ctx;
    if (!g_ui.initialized || (token >> 8U) != g_ui.generation) return;
    ui_display_t *d = ui_display_at(token & UINT8_MAX);
    if (!d) return;
    lock();
    d->driver.kick_pending = false;
    unlock();
    (void)ui_driver_kick(d);
}

static void run_task(void *arg)
{
    ui_display_t *d = arg;
    d->driver.start_result = ui_display_open(d);
    xiaotai_signal_give(&d->driver.lifecycle);
    if (d->driver.start_result == XIAOTAI_OK) {
        ui_pointer_start(d);
        while (!stopping(d)) {
            ui_driver_step(d, ui_now_ms());
            uint32_t delay = 0;
            if (!next_delay(d, ui_now_ms(), &delay) || delay > UI_TASK_WAIT_MS) delay = UI_TASK_WAIT_MS;
            lock();
            d->stack_free = xiaotai_task_current_stack_free();
            unlock();
            if (!stopping(d)) (void)xiaotai_task_wait_notify(delay);
        }
        ui_pointer_stop(d);
        ui_display_stop(d);
    }
    lock();
    d->driver.stopped = true;
    xiaotai_signal_give(&d->driver.lifecycle);
    unlock();
    xiaotai_task_exit();
}

xiaotai_err_t ui_driver_start(ui_display_t *d)
{
    if (manual_mode || !xiaotai_on_owner()) return ui_display_open(d);
    ui_driver_t *s = &d->driver;
    s->driven = true;
    s->owner_timer = !XIAOTAI_UI_TASK && d->theme->backend == XIAOTAI_UI_BACKEND_FRAME &&
                     d->theme->stack_bytes <= XIAOTAI_UI_OWNER_THEME_STACK_BYTES;
    if (s->owner_timer) {
        xiaotai_err_t err = ui_display_open(d);
        if (err == XIAOTAI_OK) err = arm(d, ui_now_ms(), 0);
        if (err != XIAOTAI_OK) ui_display_stop(d);
        return err;
    }
    xiaotai_err_t err = xiaotai_signal_init(&s->lifecycle);
    if (err != XIAOTAI_OK) {
        s->driven = false;
        return err;
    }
    const xiaotai_task_config_t config = {
        .name = "xt_ui", .entry = run_task, .arg = d,
        .stack_bytes = d->theme->stack_bytes > XIAOTAI_UI_TASK_STACK_BYTES
                           ? d->theme->stack_bytes : XIAOTAI_UI_TASK_STACK_BYTES,
        .stack = XIAOTAI_MEM_BULK, .priority = XIAOTAI_UI_TASK_PRIORITY, .core = XIAOTAI_UI_TASK_CORE,
    };
    err = xiaotai_task_create(&config, &s->task);
    if (err != XIAOTAI_OK) {
        s->driven = false;
        xiaotai_signal_deinit(&s->lifecycle);
        return err;
    }
    (void)xiaotai_signal_take(&s->lifecycle, XIAOTAI_WAIT_FOREVER);
    if (s->start_result != XIAOTAI_OK) ui_driver_stop(d);
    return s->start_result;
}

void ui_driver_stop(ui_display_t *d)
{
    ui_driver_t *s = &d->driver;
    lock();
    s->stop = true;
    const bool already = s->stopped;
    unlock();
    if (!s->driven || s->owner_timer) {
        if (s->owner_timer && xiaotai_on_owner()) xiaotai_timer_cancel(s->id);
        ui_display_stop(d);
    } else {
        if (!already) {
            xiaotai_task_notify(&s->task);
            (void)xiaotai_signal_take(&s->lifecycle, XIAOTAI_WAIT_FOREVER);
        }
        /* The worker gives the signal under the lock and has no further
         * access to display state after releasing it. */
        lock();
        unlock();
        xiaotai_signal_deinit(&s->lifecycle);
    }
    s->driven = false;
}
