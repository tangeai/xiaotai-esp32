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

static bool logical_point(const xiaotai_display_desc_t *d, xiaotai_touch_point_t *sample)
{
    const uint16_t x = sample->x, y = sample->y;
    const bool swapped = (d->rotation & 1U) != 0U;
    if (x >= (swapped ? d->height : d->width) || y >= (swapped ? d->width : d->height)) return false;
    switch (d->rotation) {
        case 0: break;
        case 1: *sample = (xiaotai_touch_point_t){y, (uint16_t)(d->height - 1U - x)}; break;
        case 2: *sample = (xiaotai_touch_point_t){(uint16_t)(d->width - 1U - x),
                                                  (uint16_t)(d->height - 1U - y)}; break;
        case 3: *sample = (xiaotai_touch_point_t){(uint16_t)(d->width - 1U - y), x}; break;
        default: return false;
    }
    return true;
}

static uint16_t interpolate(uint16_t start, uint16_t end, uint32_t at, uint32_t duration)
{
    return (uint16_t)((int32_t)start + ((int32_t)end - start) * (int32_t)at / (int32_t)duration);
}

void ui_pointer_step(ui_display_t *d, uint32_t now, bool poll_real)
{
    xiaotai_touch_point_t point = {0};
    size_t contact = 0;
    xiaotai_err_t err = XIAOTAI_OK;
    if (poll_real && d->part->read_touch) err = d->part->read_touch(d->part->self, &point, 1, &contact);

    lock();
    ui_pointer_t *p = &d->pointer;
    const bool was_down = p->down;
    if (poll_real) {
        p->real_down = err == XIAOTAI_OK && contact && logical_point(&d->part->desc, &point);
        if (p->real_down) p->real = point;
    }
    const bool synthetic = p->mode != UI_POINTER_IDLE;
    bool down = p->real_down;
    point = p->real;
    if (synthetic) {
        if (p->sample_pending) {
            p->begun_at = now;
            p->sample_pending = false;
        }
        uint32_t elapsed = now - p->begun_at;
        down = p->requested_down && elapsed < p->duration;
        if (!down) elapsed = p->duration;
        point.x = interpolate(p->start.x, p->end.x, elapsed, p->duration);
        point.y = interpolate(p->start.y, p->end.y, elapsed, p->duration);
        if (!down) {
            p->mode = UI_POINTER_IDLE;
        }
    }
    unlock();

    /* A toolkit may omit RELEASED after the pressed widget is removed.
     * The sampled contact still owns the local HOLD's falling edge. */
    if ((!synthetic && err != XIAOTAI_OK) || (d->press.hold_ticket && was_down && !down))
        ui_input_cancel_press(d);
    if (down && !d->power.pointer) (void)ui_power_touch(d, XIAOTAI_UI_INPUT_PRESS, now);
    else if (!down && d->power.pointer) (void)ui_power_touch(d, XIAOTAI_UI_INPUT_RELEASE, now);
    lock();
    p->point = point;           /* read by the LVGL pointer in this UI step */
    p->down = down && d->power.awake && !d->power.suppressed;
    unlock();
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
