// SPDX-License-Identifier: MIT
#include "capture_ring.h"
#include <string.h>

xiaotai_err_t xt_capture_ring_init(xt_capture_ring_t *r, const xiaotai_audio_device_config_t *cfg)
{
    r->blocks = cfg->ring_blocks;
    r->timeout_ms = cfg->read_timeout_ms;
    r->wire_bytes = (size_t)cfg->i2s.rx_slots * sizeof(int16_t);
    r->frame_bytes = (size_t)cfg->capture_channels * sizeof(int16_t);
    r->block_bytes = (size_t)cfg->i2s.dma_frames * r->wire_bytes;
    r->data = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, r->blocks, r->block_bytes);
    if (!r->data) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = xiaotai_signal_init(&r->ready);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(r->data);
        r->data = NULL;
        return err;
    }
    xiaotai_critical_init(&r->lock);
    return XIAOTAI_OK;
}

void xt_capture_ring_deinit(xt_capture_ring_t *r)
{
    if (r->data) {
        xiaotai_signal_deinit(&r->ready);
        xiaotai_mem_free(r->data);
        r->data = NULL;
    }
}

xiaotai_err_t xt_capture_ring_set_readable(xt_capture_ring_t *r, bool enabled, bool discard)
{
    xiaotai_critical_enter(&r->lock);
    const bool reading = r->reading;
    if (!reading) {
        r->readable = enabled;
        if (discard) {
            r->head = r->tail;
            r->queued = 0;
            r->offset = 0;
            ++r->generation;
        }
    }
    xiaotai_critical_exit(&r->lock);
    return reading ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
}

void xt_capture_ring_receive(void *ctx, const void *dma, size_t bytes)
{
    xt_capture_ring_t *r = ctx;
    xiaotai_critical_enter(&r->lock);
    if (!dma || bytes != r->block_bytes) {
        ++r->bad_blocks;
        xiaotai_critical_exit(&r->lock);
        return;
    }
    if (r->queued == r->blocks) {
        ++r->overflows;
        xiaotai_critical_exit(&r->lock);
        return;
    }
    const uint32_t generation = r->generation;
    uint8_t *slot = r->data + (size_t)r->tail * r->block_bytes;
    xiaotai_critical_exit(&r->lock);

    /* The port serializes DMA callbacks. Publish only after the slot is fully
     * copied, and never publish a block from before a concurrent discard. */
    memcpy(slot, dma, bytes);
    xiaotai_critical_enter(&r->lock);
    const bool publish = generation == r->generation;
    if (publish) {
        if (++r->tail == r->blocks) r->tail = 0;
        ++r->queued;
    }
    xiaotai_critical_exit(&r->lock);
    if (publish) xiaotai_signal_give_from_isr(&r->ready);
}

xiaotai_err_t xt_capture_ring_read(xt_capture_ring_t *r, void *data, size_t bytes, size_t *moved)
{
    *moved = 0;
    const uintptr_t reader = xt_audio_io_reader_id();
    xiaotai_critical_enter(&r->lock);
    if (!r->readable || r->reading || !reader || (r->reader && r->reader != reader)) {
        xiaotai_critical_exit(&r->lock);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    r->reader = reader;
    r->reading = true;
    xiaotai_critical_exit(&r->lock);

    const int64_t deadline = xiaotai_time_us() + (int64_t)r->timeout_ms * 1000;
    while (*moved < bytes) {
        const int64_t remaining = deadline - xiaotai_time_us();
        if (remaining <= 0) break;
        xiaotai_critical_enter(&r->lock);
        if (!r->queued) {
            xiaotai_critical_exit(&r->lock);
            if (!xiaotai_signal_take(&r->ready, (uint32_t)((remaining + 999) / 1000))) break;
            continue;
        }
        const uint8_t *slot = r->data + (size_t)r->head * r->block_bytes;
        const size_t frames = (r->block_bytes - r->offset) / r->wire_bytes;
        const size_t wanted = (bytes - *moved) / r->frame_bytes;
        const size_t copy = frames < wanted ? frames : wanted;
        const uint8_t *source = slot + r->offset;
        uint8_t *dest = (uint8_t *)data + *moved;
        xiaotai_critical_exit(&r->lock);

        /* Keep the slot counted until the last byte has been read, so the
         * producer cannot reuse it while this task copies from it. */
        if (r->wire_bytes == r->frame_bytes) memcpy(dest, source, copy * r->frame_bytes);
        else for (size_t n = 0; n < copy; ++n)
            memcpy(dest + n * r->frame_bytes, source + n * r->wire_bytes, r->frame_bytes);

        xiaotai_critical_enter(&r->lock);
        r->offset += copy * r->wire_bytes;
        if (r->offset == r->block_bytes) {
            r->offset = 0;
            if (++r->head == r->blocks) r->head = 0;
            --r->queued;
        }
        xiaotai_critical_exit(&r->lock);
        *moved += copy * r->frame_bytes;
    }
    xiaotai_critical_enter(&r->lock);
    r->reading = false;
    xiaotai_critical_exit(&r->lock);
    return *moved == bytes ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT;
}

void xt_capture_ring_stats(xt_capture_ring_t *r, uint32_t *overflows, uint32_t *bad_blocks)
{
    xiaotai_critical_enter(&r->lock);
    *overflows = r->overflows;
    *bad_blocks = r->bad_blocks;
    xiaotai_critical_exit(&r->lock);
}
