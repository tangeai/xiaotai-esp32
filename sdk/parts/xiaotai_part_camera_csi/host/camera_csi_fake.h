// SPDX-License-Identifier: MIT
#pragma once

#include "camera_csi_io.h"

typedef enum {
    CSI_GUARD, CSI_START, CSI_READ, CSI_WRITE, CSI_OPEN, CSI_FORMAT, CSI_RATE, CSI_REQUEST,
    CSI_QUERY, CSI_QBUF, CSI_DQBUF, CSI_STREAM_ON, CSI_STREAM_OFF, CSI_FREE, CSI_CLOSE, CSI_QUALITY,
} csi_fake_call_t;

typedef struct {
    csi_fake_call_t calls[256];
    size_t call_count;
    unsigned guard_calls, start_calls, rate_calls, requests, closes, stops, returns, dequeues, quality_calls;
    uint32_t hz;
    uint16_t width, height;
    uint8_t fps, reg[UINT16_MAX + 1U];
    size_t format_bytes, query_bytes, cache_alignment;
    unsigned buffer_count, reduced_count;
    bool powered, full_range, guard, ready, fd, streaming;
    bool reject_reg_write;
    csi_fake_call_t fail_call;
    xiaotai_err_t failure;
    int native_errno;
    void *pool[8];
    size_t capacities[8];
    bool queued[8];
    camera_csi_buffer_t frames[16];
    unsigned frame_next, frame_count;
} csi_fake_t;

extern csi_fake_t csi_fake;
