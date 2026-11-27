// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "xiaotai/eventlog.h"

#include <stdio.h>

#define POWER_REQUEST_MS 2000U
#define POWER_RETRY_MS 100U

typedef enum { POWER_SLEEP, POWER_WAKE, POWER_BRIGHTNESS, POWER_SLEEP_GUARDED } ui_power_op_t;
struct ui_power_request {
    xiaotai_signal_t done;
    ui_power_op_t op;
    uint8_t brightness;
    xiaotai_err_t result;
};

static void lock(void) { (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g_ui.lock); }

static xiaotai_err_t backlight(ui_display_t *d, uint8_t level)
{
    return d->part->set_backlight ? d->part->set_backlight(d->part->self, level) : XIAOTAI_OK;
}

static xiaotai_err_t change(ui_display_t *d, bool awake, uint32_t now)
{
    if (d->power.awake == awake) return XIAOTAI_OK;
    if (!awake && (d->press.down || d->power.pointer)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = backlight(d, awake ? d->power.brightness : 0);
    if (err != XIAOTAI_OK) return err;
    lock();
    d->power.awake = awake;
    if (awake) {
        d->power.active_at = now;
        d->power.redraw = true;
    }
    unlock();
    if (d->theme->sleep) d->theme->sleep(d->theme_state, !awake);
    xiaotai_args_t event = {.value = awake ? 0 : 1};
    snprintf(event.text, sizeof(event.text), "%u", (unsigned)d->host.index);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_UI_SLEEP, &event, NULL, 0);
    return XIAOTAI_OK;
}

void ui_power_init(ui_display_t *d, uint32_t now)
{
    d->power.brightness = 100;
    d->power.active_at = now;
    const xiaotai_err_t err = backlight(d, d->power.brightness);
    d->power.awake = err == XIAOTAI_OK;
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn("ui", "power.start_failed", d->host.index, err);
    d->power.redraw = true;
    if (!d->power.awake && d->theme->sleep) d->theme->sleep(d->theme_state, true);
}

void ui_power_activity(ui_display_t *d, uint32_t now)
{
    d->power.active_at = now;
    d->power.retry = false;
}

bool ui_power_navigation(ui_display_t *d, uint32_t now)
{
    ui_power_activity(d, now);
    if (d->power.awake) return true;
    (void)change(d, true, now);
    return false;
}

bool ui_power_touch(ui_display_t *d, xiaotai_ui_input_t input, uint32_t now)
{
    if (input == XIAOTAI_UI_INPUT_PRESS) {
        if (d->power.pointer) return !d->power.suppressed;
        d->power.pointer = true;
        d->power.suppressed = false;
        ui_power_activity(d, now);
        if (!d->power.awake) {
            const xiaotai_err_t err = change(d, true, now);
            d->power.suppressed = err != XIAOTAI_OK || !g_ui_config.dark_touch_operates;
            if (d->power.suppressed) ui_input_ignored(d);
        }
    } else if (input == XIAOTAI_UI_INPUT_RELEASE) {
        d->power.pointer = false;
        /* Also swallow a click arriving after this release; a fresh press
         * retires the wake-only gesture. */
    }
    return d->power.awake && !d->power.suppressed;
}

static bool may_sleep(const ui_display_t *d);

static xiaotai_err_t apply(ui_display_t *d, ui_power_op_t op, uint8_t brightness, uint32_t now)
{
    if (op == POWER_SLEEP) return change(d, false, now);
    if (op == POWER_SLEEP_GUARDED) {
        if (!d->power.awake) return XIAOTAI_OK;
        if (!may_sleep(d)) return XIAOTAI_ERR_INVALID_STATE;
        return change(d, false, now);
    }
    if (op == POWER_WAKE) {
        ui_power_activity(d, now);
        return change(d, true, now);
    }
    if (d->power.awake) {
        const xiaotai_err_t err = backlight(d, brightness);
        if (err != XIAOTAI_OK) return err;
    }
    d->power.brightness = brightness;
    return XIAOTAI_OK;
}

static bool may_sleep(const ui_display_t *d)
{
    lock();
    const bool synthetic = d->pointer.mode != UI_POINTER_IDLE;
    unlock();
    return d->power.awake && !synthetic && !d->power.pointer && !d->press.down && !d->scene.overlay.present &&
           d->scene.page.present && (d->scene.page.view.flags & XIAOTAI_VIEW_MAY_SLEEP);
}

static ui_power_request_t *take_request(ui_display_t *d)
{
    lock();
    ui_power_request_t *r = d->power.request;
    d->power.request = NULL;
    unlock();
    return r;
}

void ui_power_shutdown(ui_display_t *d)
{
    ui_power_request_t *r = take_request(d);
    if (r) {
        r->result = XIAOTAI_ERR_INVALID_STATE;
        xiaotai_signal_give(&r->done);
    }
}

bool ui_power_next_due(ui_display_t *d, uint32_t now, uint32_t *due)
{
    lock();
    const bool busy = d->power.request || d->power.activity_pending || (d->power.awake && d->power.redraw);
    const uint32_t timeout = g_ui.sleep_timeout_ms;
    unlock();
    if (busy) {
        *due = now;
        return true;
    }
    if (!timeout || !may_sleep(d)) return false;
    const uint32_t elapsed = now - d->power.active_at;
    uint32_t delay = elapsed >= timeout ? 0U : timeout - elapsed;
    if (d->power.retry && (int32_t)(d->power.retry_due - now) > 0) delay = d->power.retry_due - now;
    *due = now + (delay > XIAOTAI_TIMER_DELAY_MAX_MS ? XIAOTAI_TIMER_DELAY_MAX_MS : delay);
    return true;
}

void ui_power_step(ui_display_t *d, uint32_t now)
{
    ui_power_request_t *r = take_request(d);
    if (r) {
        r->result = apply(d, r->op, r->brightness, now);
        xiaotai_signal_give(&r->done);
    }
    lock();
    const bool activity = d->power.activity_pending;
    d->power.activity_pending = false;
    const uint32_t timeout = g_ui.sleep_timeout_ms;
    unlock();
    if (activity) (void)apply(d, POWER_WAKE, 0, now);
    if (timeout && may_sleep(d) && now - d->power.active_at >= timeout &&
        (!d->power.retry || (int32_t)(now - d->power.retry_due) >= 0)) {
        const xiaotai_err_t err = change(d, false, now);
        if (err != XIAOTAI_OK && !d->power.retry) xiaotai_eventlog_warn("ui", "power.sleep_failed", d->host.index, err);
        d->power.retry = err != XIAOTAI_OK;
        d->power.retry_due = now + POWER_RETRY_MS;
    }
}

static xiaotai_err_t submit(uint8_t index, ui_power_op_t op, uint8_t brightness)
{
    ui_display_t *d = ui_display_at(index);
    if (!d) return XIAOTAI_ERR_NOT_FOUND;
    lock();
    const bool ready = d->state == UI_DISPLAY_READY && !d->driver.stop;
    unlock();
    if (!ready) return XIAOTAI_ERR_INVALID_STATE;
    if (ui_driver_context(d)) {
        const xiaotai_err_t err = apply(d, op, brightness, ui_now_ms());
        if (err == XIAOTAI_OK) (void)ui_driver_kick(d);
        return err;
    }
    for (size_t i = 0; i < g_ui.display_count; ++i) {
        if (g_ui.displays[i].driver.driven && !g_ui.displays[i].driver.owner_timer &&
            ui_driver_context(&g_ui.displays[i]))
            return XIAOTAI_ERR_INVALID_STATE; /* Do not cross-call from another display's callback. */
    }

    ui_power_request_t r = {.op = op, .brightness = brightness};
    xiaotai_err_t err = xiaotai_signal_init(&r.done);
    if (err != XIAOTAI_OK) return err;
    lock();
    if (d->power.request || d->driver.stop) {
        unlock();
        xiaotai_signal_deinit(&r.done);
        return XIAOTAI_ERR_BUSY;
    }
    d->power.request = &r;
    unlock();
    err = ui_driver_kick(d);
    if (err != XIAOTAI_OK || !xiaotai_signal_take(&r.done, POWER_REQUEST_MS)) {
        lock();
        if (d->power.request == &r) {
            d->power.request = NULL;
            unlock();
            xiaotai_signal_deinit(&r.done);
            return err != XIAOTAI_OK ? err : XIAOTAI_ERR_TIMEOUT;
        }
        unlock();
        /* Once the owner takes the request it completes the real operation.
         * The stack record cannot go out of scope before that completion. */
        (void)xiaotai_signal_take(&r.done, XIAOTAI_WAIT_FOREVER);
    }
    xiaotai_signal_deinit(&r.done);
    return r.result;
}

xiaotai_err_t xiaotai_ui_sleep(uint8_t display) { return submit(display, POWER_SLEEP, 0); }
xiaotai_err_t ui_power_sleep_guarded(uint8_t display) { return submit(display, POWER_SLEEP_GUARDED, 0); }
xiaotai_err_t xiaotai_ui_wake(uint8_t display) { return submit(display, POWER_WAKE, 0); }
xiaotai_err_t xiaotai_ui_set_brightness(uint8_t display, uint8_t percent)
{
    if (!percent || percent > 100) return XIAOTAI_ERR_INVALID_ARG;
    return submit(display, POWER_BRIGHTNESS, percent);
}

bool xiaotai_ui_awake(uint8_t display)
{
    ui_display_t *d = ui_display_at(display);
    if (!d) return false;
    lock();
    const bool awake = d->state == UI_DISPLAY_READY && d->power.awake;
    unlock();
    return awake;
}

xiaotai_err_t xiaotai_ui_set_sleep_timeout(uint32_t timeout_ms)
{
    if (!g_ui.initialized) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    g_ui.sleep_timeout_ms = timeout_ms;
    unlock();
    for (size_t i = 0; i < g_ui.display_count; ++i) (void)ui_driver_kick(&g_ui.displays[i]);
    return XIAOTAI_OK;
}

void xiaotai_ui_activity(void)
{
    if (!g_ui.initialized) return;
    for (size_t i = 0; i < g_ui.display_count; ++i) {
        ui_display_t *d = &g_ui.displays[i];
        lock();
        if (d->state == UI_DISPLAY_READY) d->power.activity_pending = true;
        unlock();
        (void)ui_driver_kick(d);
    }
}
