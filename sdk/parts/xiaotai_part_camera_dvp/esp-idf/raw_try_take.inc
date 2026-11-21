// SPDX-License-Identifier: MIT
/* Included in the pinned cam_hal.c; the vendor queue still owns its pointers.
 * Only the two-bank raw internal-DMA setup uses this metadata. */
static uint64_t xt_dvp_published[2], xt_dvp_publish_next, xt_dvp_borrow_next;

static bool xt_dvp_tracks_raw(void)
{
    return cam_obj && cam_obj->frame_cnt == 2U &&
           !cam_obj->jpeg_mode && !cam_obj->psram_mode;
}

static void xt_dvp_reset_sequence(void)
{
    xt_dvp_published[0] = xt_dvp_published[1] = 0;
    xt_dvp_publish_next = xt_dvp_borrow_next = 0;
}

static void xt_dvp_stamp_raw(int bank)
{
    /* The producer still owns this bank. Publish the stamp before enqueue;
     * the borrower will read it while that same bank cannot be reused. */
    if (xt_dvp_tracks_raw() && bank >= 0 && bank < 2) {
        xt_dvp_published[bank] = xt_dvp_publish_next++;
    }
}

camera_fb_t *xiaotai_dvp_try_take_raw(uint32_t *skipped);

camera_fb_t *xiaotai_dvp_try_take_raw(uint32_t *skipped)
{
    if (!skipped || !xt_dvp_tracks_raw() || !cam_obj->frame_buffer_queue) return NULL;
    camera_fb_t *frame = NULL;
    if (xQueueReceive(cam_obj->frame_buffer_queue, &frame, 0) != pdTRUE || !frame) return NULL;
    for (unsigned bank = 0; bank < 2U; ++bank) {
        if (frame != &cam_obj->frames[bank].fb) continue;
        const uint64_t stamp = xt_dvp_published[bank];
        const uint64_t missed = stamp - xt_dvp_borrow_next;
        *skipped = missed > UINT32_MAX ? UINT32_MAX : (uint32_t)missed;
        xt_dvp_borrow_next = stamp + 1U;
        return frame;
    }
    return NULL;
}

bool xiaotai_dvp_raw_ready(void);

bool xiaotai_dvp_raw_ready(void)
{
    /* The pinned cam_config ignores task creation failure. A successful
     * public init is not enough to prove it can produce frames. */
    return xt_dvp_tracks_raw() && cam_obj->task_handle && cam_obj->event_queue &&
           cam_obj->frame_buffer_queue && cam_obj->frames &&
           cam_obj->frames[0].fb.buf && cam_obj->frames[1].fb.buf;
}
