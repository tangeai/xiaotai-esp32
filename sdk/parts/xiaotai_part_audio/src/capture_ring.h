// SPDX-License-Identifier: MIT
#pragma once
#include "audio_io.h"
#include "xiaotai/port/os.h"

typedef struct {
    xiaotai_critical_t lock;
    xiaotai_signal_t ready;
    uint8_t *data;
    size_t block_bytes, wire_bytes, frame_bytes, offset;
    uint8_t blocks, head, tail, queued;
    uint16_t timeout_ms;
    uintptr_t reader;
    uint32_t generation, overflows, bad_blocks;
    bool readable, reading;
} xt_capture_ring_t;

xiaotai_err_t xt_capture_ring_init(xt_capture_ring_t *ring, const xiaotai_audio_device_config_t *cfg);
void xt_capture_ring_deinit(xt_capture_ring_t *ring);
/* Start/stop delivery to the reader. Discard also invalidates a DMA copy which
 * began before this boundary. The caller must quiesce its reader first. */
xiaotai_err_t xt_capture_ring_set_readable(xt_capture_ring_t *ring, bool enabled, bool discard);
void xt_capture_ring_receive(void *ctx, const void *dma, size_t bytes);
xiaotai_err_t xt_capture_ring_read(xt_capture_ring_t *ring, void *data, size_t bytes, size_t *moved);
void xt_capture_ring_stats(xt_capture_ring_t *ring, uint32_t *overflows, uint32_t *bad_blocks);
