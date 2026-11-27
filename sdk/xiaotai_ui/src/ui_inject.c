// SPDX-License-Identifier: MIT
#include "ui_internal.h"

#define TAP_MS 120U
#define RAW_PRESS_MS 1000U

static void lock(void) { (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g_ui.lock); }

static bool inside(const xiaotai_display_desc_t *desc, int16_t x, int16_t y)
{
    return x >= 0 && y >= 0 && (uint16_t)x < desc->width && (uint16_t)y < desc->height;
}

static xiaotai_err_t submit(uint8_t index, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             uint32_t duration, ui_pointer_mode_t mode, bool down)
{
    ui_display_t *d = ui_display_at(index);
    if (!d) return XIAOTAI_ERR_NOT_FOUND;
    const xiaotai_display_desc_t *desc = &d->part->desc;
    if (!desc->touch || !d->part->read_touch) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!inside(desc, x0, y0) || !inside(desc, x1, y1)) return XIAOTAI_ERR_INVALID_ARG;
    lock();
    ui_pointer_t *p = &d->pointer;
    if (d->state != UI_DISPLAY_READY || d->driver.stop || p->real_down || p->sample_pending ||
        (p->mode != UI_POINTER_IDLE && (p->mode != UI_POINTER_RAW || mode != UI_POINTER_RAW))) {
        unlock();
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const ui_pointer_t before = *p;
    p->mode = mode;
    p->sample_pending = true;
    p->requested_down = down;
    p->duration = duration;
    p->start = (xiaotai_touch_point_t){(uint16_t)x0, (uint16_t)y0};
    p->end = (xiaotai_touch_point_t){(uint16_t)x1, (uint16_t)y1};
    const xiaotai_err_t err = ui_driver_post_locked(d);
    if (err != XIAOTAI_OK) *p = before;
    unlock();
    return err;
}

xiaotai_err_t xiaotai_ui_tap(uint8_t display, int16_t x, int16_t y)
{
    return submit(display, x, y, x, y, TAP_MS, UI_POINTER_GESTURE, true);
}

xiaotai_err_t xiaotai_ui_swipe(uint8_t display, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                               uint32_t duration_ms)
{
    if (duration_ms < 100U || duration_ms > 2000U) return XIAOTAI_ERR_INVALID_ARG;
    return submit(display, x0, y0, x1, y1, duration_ms, UI_POINTER_GESTURE, true);
}

xiaotai_err_t xiaotai_ui_pointer(uint8_t display, int16_t x, int16_t y, bool pressed)
{
    return submit(display, x, y, x, y, RAW_PRESS_MS, UI_POINTER_RAW, pressed);
}

static uint16_t interpolate(uint16_t start, uint16_t end, uint32_t at, uint32_t duration)
{
    return (uint16_t)((int32_t)start + ((int32_t)end - start) * (int32_t)at / (int32_t)duration);
}

/* One contact: the wake gate, the press it may have to cancel, and the queue
 * the toolkit reads in this step. */
static void deliver(ui_display_t *d, const ui_contact_t *c, uint32_t now)
{
    ui_touch_state_t *t = d->touch;
    const bool was_down = d->pointer.down;
    if (c->down != t->contact) ui_trace_contact(d, c->down, c->at_us);
    if (t->contact && !c->down) t->ended = true;
    t->contact = c->down;
    /* A toolkit may omit RELEASED after the pressed widget is removed.
     * The sampled contact still owns the local HOLD's falling edge. */
    if (d->press.hold_ticket && was_down && !c->down) ui_input_cancel_press(d);
    if (c->down && !d->power.pointer) (void)ui_power_touch(d, XIAOTAI_UI_INPUT_PRESS, now);
    else if (!c->down && d->power.pointer) (void)ui_power_touch(d, XIAOTAI_UI_INPUT_RELEASE, now);
    lock();
    d->pointer.point = (xiaotai_touch_point_t){(uint16_t)c->x, (uint16_t)c->y};
    d->pointer.down = c->down && d->power.awake && !d->power.suppressed;
    const bool down = d->pointer.down;
    unlock();
    if (t->queued == UI_CONTACTS) return;
    t->queue[t->queued] = *c;
    t->queue[t->queued++].down = down;
}

void ui_pointer_step(ui_display_t *d, uint32_t now, bool poll_real)
{
    ui_touch_state_t *t = d->touch;
    if (!t) return;
    t->queued = 0;
    if (poll_real && d->part->read_touch) {
        const ui_sample_t sample = ui_pointer_read(d, (int64_t)now * 1000);
        ui_pointer_push(d, &sample);
    }
    ui_sample_t samples[UI_SAMPLES];
    const size_t count = ui_pointer_take(d, samples);
    lock();
    ui_pointer_t *p = &d->pointer;
    const bool synthetic = p->mode != UI_POINTER_IDLE;
    if (count) p->real_down = samples[count - 1U].down;
    ui_contact_t made = {.at_us = xiaotai_time_us()};
    if (synthetic) {
        if (p->sample_pending) {
            p->begun_at = now;
            p->sample_pending = false;
        }
        uint32_t elapsed = now - p->begun_at;
        made.down = p->requested_down && elapsed < p->duration;
        if (!made.down) elapsed = p->duration;
        made.x = interpolate(p->start.x, p->end.x, elapsed, p->duration);
        made.y = interpolate(p->start.y, p->end.y, elapsed, p->duration);
        if (!made.down) p->mode = UI_POINTER_IDLE;
    }
    unlock();
    if (synthetic) {
        /* A finger meanwhile is seen again once the gesture is over. */
        ui_touch_reset(&t->filter);
        if (!made.down && p->duration) {
            made.vx = ((float)p->end.x - (float)p->start.x) * 1000.0f / (float)p->duration;
            made.vy = ((float)p->end.y - (float)p->start.y) * 1000.0f / (float)p->duration;
        }
        deliver(d, &made, now);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        ui_contact_t contacts[2];
        const size_t n = ui_touch_feed(&t->filter, &samples[i], contacts);
        if (samples[i].failed) ui_input_cancel_press(d);
        for (size_t k = 0; k < n; ++k) deliver(d, &contacts[k], now);
    }
    /* Whatever was pressed, nothing touches the display any more. */
    if (count && !samples[count - 1U].down && d->pointer.down && !t->filter.down) {
        const ui_contact_t up = {.at_us = samples[count - 1U].at_us, .x = d->pointer.point.x, .y = d->pointer.point.y};
        deliver(d, &up, now);
    }
}

/* The toolkit has read this step's contacts. A press it still holds lost its
 * widget before the finger left: it must neither reach into the next contact
 * nor keep the display awake. */
void ui_pointer_settle(ui_display_t *d)
{
    ui_touch_state_t *t = d->touch;
    if (!t || !t->ended) return;
    t->ended = false;
    if (!t->contact && d->press.down) ui_input_cancel_press(d);
}

bool ui_pointer_due(ui_display_t *d, uint32_t now, uint32_t *due)
{
    lock();
    const ui_pointer_t *p = &d->pointer;
    const bool work = p->mode != UI_POINTER_IDLE;
    if (work) *due = p->sample_pending ? now : p->begun_at + p->duration;
    unlock();
    return work;
}
