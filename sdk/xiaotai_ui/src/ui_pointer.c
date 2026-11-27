// SPDX-License-Identifier: MIT
/* Reading the touch controller beside the UI task. A frame takes tens of
 * milliseconds on a large display: a finger that came and went meanwhile would
 * be missed, and one that moves would be seen in steps as long as a frame. The
 * reads have a small task of their own, quick while there is a contact and
 * slow while there is none, and the UI task is woken for what it read. */
#include "ui_internal.h"
#include "xiaotai/port/mem.h"

#include <string.h>

/* Reads after the finger left, so that a lost sample is told from a release. */
#define TAIL_SAMPLES 3U
#define SAMPLER_STACK_BYTES 3072U

static void lock(void) { (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g_ui.lock); }

xiaotai_err_t ui_pointer_open(ui_display_t *d)
{
    if (!d->part->desc.touch || d->touch) return XIAOTAI_OK;
    ui_touch_state_t *t = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*t));
    if (!t) return XIAOTAI_ERR_NO_MEM;
    ui_touch_init(&t->filter, d->part->desc.width, d->part->desc.height);
    lock();
    d->touch = t;
    unlock();
    return XIAOTAI_OK;
}

void ui_pointer_close(ui_display_t *d)
{
    lock();
    ui_touch_state_t *t = d->touch;
    d->touch = NULL;
    unlock();
    xiaotai_mem_free(t);
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

ui_sample_t ui_pointer_read(ui_display_t *d, int64_t at_us)
{
    xiaotai_touch_point_t point = {0};
    size_t contact = 0;
    const int64_t began = xiaotai_time_us();
    const xiaotai_err_t err = d->part->read_touch(d->part->self, &point, 1, &contact);
    ui_trace_poll(d, began, xiaotai_time_us());
    ui_sample_t sample = {.at_us = at_us, .failed = err != XIAOTAI_OK};
    sample.down = !sample.failed && contact && logical_point(&d->part->desc, &point);
    sample.x = point.x;
    sample.y = point.y;
    return sample;
}

void ui_pointer_push(ui_display_t *d, const ui_sample_t *sample)
{
    lock();
    ui_touch_state_t *t = d->touch;
    if (t->count == UI_SAMPLES) {
        /* Nothing was taken for that long: keep where the contact began and
         * went lately, which is what a press and its release need. */
        size_t drop = 1;
        while (drop + 1U < t->count && !(t->ring[drop].down && t->ring[drop - 1U].down && t->ring[drop + 1U].down)) ++drop;
        memmove(&t->ring[drop], &t->ring[drop + 1U], (t->count - drop - 1U) * sizeof(*t->ring));
        --t->count;
    }
    t->ring[t->count++] = *sample;
    unlock();
}

size_t ui_pointer_take(ui_display_t *d, ui_sample_t out[UI_SAMPLES])
{
    lock();
    ui_touch_state_t *t = d->touch;
    const size_t count = t->count;
    memcpy(out, t->ring, count * sizeof(*out));
    t->count = 0;
    unlock();
    return count;
}

static void sample_task(void *arg)
{
    ui_display_t *d = arg;
    ui_touch_state_t *t = d->touch;
    unsigned tail = 0, turns = 0;
    bool failed = false;
    for (;;) {
        const uint32_t stack = (turns++ & 255U) ? 0U : xiaotai_task_current_stack_free();
        lock();
        const bool stop = t->stop;
        if (stack) t->stack_free = stack;
        unlock();
        if (stop) break;
        const int64_t began = xiaotai_time_us();
        const ui_sample_t sample = ui_pointer_read(d, began);
        const bool news = sample.down || tail || sample.failed != failed;
        tail = sample.down ? TAIL_SAMPLES : tail ? tail - 1U : 0U;
        failed = sample.failed;
        if (news) {
            ui_pointer_push(d, &sample);
            (void)ui_driver_kick(d);
        }
        const int64_t spent = (xiaotai_time_us() - began) / 1000;
        const int64_t period = tail ? XIAOTAI_UI_POINTER_CONTACT_MS : XIAOTAI_UI_POINTER_READ_MS;
        (void)xiaotai_task_wait_notify(spent + 1 < period ? (uint32_t)(period - spent) : 1U);
    }
    xiaotai_signal_give(&t->done);
    xiaotai_task_exit();
}

void ui_pointer_start(ui_display_t *d)
{
    ui_touch_state_t *t = d->touch;
    if (!t || !d->part->read_touch) return;
    if (xiaotai_signal_init(&t->done) != XIAOTAI_OK) return;
    const xiaotai_task_config_t config = {
        .name = "xt_touch", .entry = sample_task, .arg = d, .stack_bytes = SAMPLER_STACK_BYTES,
        .stack = XIAOTAI_MEM_BULK, .priority = XIAOTAI_UI_TASK_PRIORITY + 1U, .core = XIAOTAI_UI_TASK_CORE,
    };
    /* Without the task the UI task reads between its frames, as before. */
    t->sampling = xiaotai_task_create(&config, &t->task) == XIAOTAI_OK;
    if (!t->sampling) xiaotai_signal_deinit(&t->done);
}

void ui_pointer_stop(ui_display_t *d)
{
    ui_touch_state_t *t = d->touch;
    if (!t || !t->sampling) return;
    lock();
    t->stop = true;
    unlock();
    xiaotai_task_notify(&t->task);
    (void)xiaotai_signal_take(&t->done, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_deinit(&t->done);
    t->sampling = t->stop = false;
}
