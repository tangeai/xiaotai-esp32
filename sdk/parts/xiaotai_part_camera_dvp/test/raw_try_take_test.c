// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const void *buf;
    size_t len;
} camera_fb_t;

typedef struct {
    camera_fb_t *head;
    bool ready;
    unsigned receives;
} frame_queue_t;

typedef struct { camera_fb_t fb; } frame_bank_t;

typedef struct {
    frame_queue_t *frame_buffer_queue;
    void *task_handle, *event_queue;
    bool jpeg_mode, psram_mode;
    unsigned frame_cnt;
    frame_bank_t *frames;
} camera_hal_t;

static camera_hal_t *cam_obj;
enum { pdFALSE, pdTRUE };

static int xQueueReceive(frame_queue_t *queue, void *p, unsigned wait_ticks)
{
    assert(wait_ticks == 0U);
    ++queue->receives;
    if (!queue->ready) return pdFALSE;
    *(camera_fb_t **)p = queue->head;
    queue->ready = false;
    return pdTRUE;
}

/* The same extension injected into the real, hash-checked vendor TU. */
#include "raw_try_take.inc"

int main(void)
{
    const char pixels[] = {1, 2, 3, 4};
    frame_bank_t banks[2] = {0};
    camera_hal_t hal = {.frame_cnt = 2, .frames = banks};
    camera_fb_t *older = &hal.frames[0].fb, *newest = &hal.frames[1].fb;
    *older = (camera_fb_t){.buf = pixels, .len = 2};
    *newest = (camera_fb_t){.buf = pixels, .len = sizeof(pixels)};
    frame_queue_t queue = {.head = older, .ready = true};
    uint32_t skipped = 123;
    assert(!xiaotai_dvp_raw_ready());
    assert(!xiaotai_dvp_try_take_raw(&skipped)); /* before open */
    cam_obj = &hal;
    xt_dvp_reset_sequence();
    assert(!xiaotai_dvp_try_take_raw(&skipped)); /* no usable queue yet */
    hal.frame_buffer_queue = &queue;
    assert(!xiaotai_dvp_raw_ready());
    hal.event_queue = &queue;
    assert(!xiaotai_dvp_raw_ready());
    hal.task_handle = &hal;
    assert(xiaotai_dvp_raw_ready());
    hal.frames = NULL;
    assert(!xiaotai_dvp_raw_ready());
    hal.frames = banks;
    newest->buf = NULL;
    assert(!xiaotai_dvp_raw_ready());
    newest->buf = pixels;
    hal.jpeg_mode = true;
    assert(!xiaotai_dvp_raw_ready());
    xt_dvp_stamp_raw(0);
    assert(!xiaotai_dvp_try_take_raw(&skipped) && queue.receives == 0 && queue.ready);
    hal.jpeg_mode = false;
    hal.psram_mode = true;
    xt_dvp_stamp_raw(0);
    assert(!xiaotai_dvp_try_take_raw(&skipped) && queue.receives == 0 && queue.ready);
    hal.psram_mode = false;
    hal.frame_cnt = 1;
    assert(!xiaotai_dvp_try_take_raw(&skipped) && queue.receives == 0 && queue.ready);
    hal.frame_cnt = 2;
    assert(!xiaotai_dvp_try_take_raw(NULL) && queue.receives == 0);
    assert(skipped == 123);

    /* GRAB_LATEST producer has popped the old frame but not pushed the new one. */
    xt_dvp_stamp_raw(0);
    xt_dvp_stamp_raw(1);
    queue.ready = false;
    assert(!xiaotai_dvp_try_take_raw(&skipped) && queue.receives == 1 && skipped == 123);
    queue = (frame_queue_t){.head = newest, .ready = true, .receives = 1};
    camera_fb_t *const frame = xiaotai_dvp_try_take_raw(&skipped);
    assert(frame == newest && frame->buf == pixels && frame->len == sizeof(pixels) && skipped == 1);
    assert(!queue.ready && queue.receives == 2);
    assert(!xiaotai_dvp_try_take_raw(&skipped) && queue.receives == 3);

    /* The locked driver also checks for a NULL result from a successful pop. */
    queue.head = NULL;
    queue.ready = true;
    assert(!xiaotai_dvp_try_take_raw(&skipped) && queue.receives == 4 && !queue.ready);

    /* A filled bank whose enqueue loses the consumer race is still older
     * than the next delivered frame. It is not counted twice on reuse. */
    xt_dvp_stamp_raw(0);
    xt_dvp_stamp_raw(0);
    queue.head = older;
    queue.ready = true;
    assert(xiaotai_dvp_try_take_raw(&skipped) == older && skipped == 1);
    xt_dvp_stamp_raw(1);
    queue.head = newest;
    queue.ready = true;
    assert(xiaotai_dvp_try_take_raw(&skipped) == newest && skipped == 0);

    /* Reopen the same driver address; history must not leak across opens. */
    xt_dvp_reset_sequence();
    xt_dvp_stamp_raw(0);
    queue.head = older;
    queue.ready = true;
    assert(xiaotai_dvp_try_take_raw(&skipped) == older && skipped == 0);
    xt_dvp_publish_next = (uint64_t)UINT32_MAX + 7U;
    xt_dvp_stamp_raw(1);
    queue.head = newest;
    queue.ready = true;
    assert(xiaotai_dvp_try_take_raw(&skipped) == newest && skipped == UINT32_MAX);
    xt_dvp_stamp_raw(0);
    queue.head = older;
    queue.ready = true;
    assert(xiaotai_dvp_try_take_raw(&skipped) == older && skipped == 0);
    return 0;
}
