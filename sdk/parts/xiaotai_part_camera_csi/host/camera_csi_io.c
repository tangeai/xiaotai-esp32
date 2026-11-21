// SPDX-License-Identifier: MIT
#include "camera_csi_fake.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

csi_fake_t csi_fake;

xiaotai_err_t camera_csi_io_alignment(size_t *bytes)
{
    *bytes = csi_fake.cache_alignment ? csi_fake.cache_alignment : 64U;
    return XIAOTAI_OK;
}

int camera_csi_io_errno(void) { return csi_fake.native_errno; }

static xiaotai_err_t call(csi_fake_call_t op)
{
    if (csi_fake.call_count < sizeof(csi_fake.calls) / sizeof(csi_fake.calls[0])) {
        csi_fake.calls[csi_fake.call_count++] = op;
    }
    return csi_fake.fail_call == op ? csi_fake.failure : XIAOTAI_OK;
}

xiaotai_err_t camera_csi_io_start(const void *owner, const xiaotai_camera_csi_config_t *config, uint32_t hz)
{
    assert(owner && config && csi_fake.powered);
    xiaotai_err_t err;
    if (!csi_fake.guard) {
        csi_fake.guard_calls++;
        err = call(CSI_GUARD);
        if (err != XIAOTAI_OK) return err;
        csi_fake.guard = true;
    }
    csi_fake.start_calls++;
    csi_fake.hz = hz;
    err = call(CSI_START);
    csi_fake.ready = err == XIAOTAI_OK;
    return err;
}

xiaotai_err_t camera_csi_io_read(const void *owner, uint16_t reg, uint8_t *value)
{
    assert(owner && csi_fake.powered && csi_fake.ready);
    *value = csi_fake.reg[reg];
    return call(CSI_READ);
}

xiaotai_err_t camera_csi_io_write(const void *owner, uint16_t reg, uint8_t value)
{
    assert(owner && csi_fake.powered && csi_fake.fd && !csi_fake.streaming && csi_fake.format_bytes);
    xiaotai_err_t err = call(CSI_WRITE);
    if (err == XIAOTAI_OK && !csi_fake.reject_reg_write) csi_fake.reg[reg] = value;
    return err;
}

xiaotai_err_t camera_csi_io_open(int *fd)
{
    assert(csi_fake.powered && csi_fake.ready && !csi_fake.fd);
    *fd = 7;
    csi_fake.fd = true;
    return call(CSI_OPEN);
}

xiaotai_err_t camera_csi_io_format(int fd, uint16_t width, uint16_t height, bool full_range, size_t *bytes)
{
    assert(fd == 7 && csi_fake.fd);
    csi_fake.width = width;
    csi_fake.height = height;
    csi_fake.full_range = full_range;
    if (!csi_fake.format_bytes) csi_fake.format_bytes = (size_t)width * height * 3U / 2U;
    *bytes = csi_fake.format_bytes;
    return call(CSI_FORMAT);
}

xiaotai_err_t camera_csi_io_rate(int fd, uint8_t fps)
{
    assert(fd == 7 && csi_fake.fd && csi_fake.format_bytes);
    csi_fake.rate_calls++;
    csi_fake.fps = fps;
    return call(CSI_RATE);
}

xiaotai_err_t camera_csi_io_request(int fd, unsigned count, unsigned *actual)
{
    assert(fd == 7 && csi_fake.fd && !csi_fake.streaming && count <= 8);
    xiaotai_err_t err = call(count ? CSI_REQUEST : CSI_FREE);
    if (err != XIAOTAI_OK) return err;
    if (count) csi_fake.requests++;
    csi_fake.buffer_count = count && csi_fake.reduced_count ? csi_fake.reduced_count : count;
    memset(csi_fake.queued, 0, sizeof(csi_fake.queued));
    if (actual) *actual = csi_fake.buffer_count;
    return XIAOTAI_OK;
}

xiaotai_err_t camera_csi_io_buffer_size(int fd, unsigned index, size_t *size)
{
    assert(fd == 7 && csi_fake.fd && index < csi_fake.buffer_count);
    *size = csi_fake.query_bytes ? csi_fake.query_bytes : csi_fake.format_bytes;
    return call(CSI_QUERY);
}

xiaotai_err_t camera_csi_io_queue(int fd, unsigned index, void *data, size_t capacity)
{
    assert(fd == 7 && csi_fake.fd && index < csi_fake.buffer_count && !csi_fake.queued[index]);
    const size_t alignment = csi_fake.cache_alignment ? csi_fake.cache_alignment : 64U;
    if ((uintptr_t)data % alignment || capacity < csi_fake.format_bytes) {
        csi_fake.native_errno = EINVAL;
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (csi_fake.pool[index]) assert(csi_fake.pool[index] == data);
    csi_fake.pool[index] = data;
    csi_fake.capacities[index] = capacity;
    if (csi_fake.streaming) csi_fake.returns++;
    xiaotai_err_t err = call(CSI_QBUF);
    if (err == XIAOTAI_OK) csi_fake.queued[index] = true;
    return err;
}

xiaotai_err_t camera_csi_io_dequeue(int fd, camera_csi_buffer_t *buffer)
{
    assert(fd == 7 && csi_fake.fd && csi_fake.streaming);
    csi_fake.dequeues++;
    xiaotai_err_t err = call(CSI_DQBUF);
    if (err != XIAOTAI_OK) return err;
    if (csi_fake.frame_next >= csi_fake.frame_count) return XIAOTAI_ERR_NOT_FOUND;
    *buffer = csi_fake.frames[csi_fake.frame_next++];
    if (buffer->index < csi_fake.buffer_count) csi_fake.queued[buffer->index] = false;
    return XIAOTAI_OK;
}

xiaotai_err_t camera_csi_io_stream(int fd, bool start)
{
    assert(fd == 7 && csi_fake.fd && csi_fake.streaming != start);
    if (start) {
        for (unsigned i = 0; i < csi_fake.buffer_count; ++i) assert(csi_fake.queued[i]);
    } else {
        csi_fake.stops++;
    }
    xiaotai_err_t err = call(start ? CSI_STREAM_ON : CSI_STREAM_OFF);
    if (err == XIAOTAI_OK) csi_fake.streaming = start;
    return err;
}

xiaotai_err_t camera_csi_io_close(int fd)
{
    assert(fd == 7 && csi_fake.fd && !csi_fake.streaming);
    csi_fake.closes++;
    csi_fake.fd = false;
    csi_fake.buffer_count = 0;
    return call(CSI_CLOSE);
}

xiaotai_err_t camera_csi_io_quality(int fd, camera_csi_quality_t *q)
{
    assert(fd == 7 && csi_fake.fd && csi_fake.powered);
    csi_fake.quality_calls++;
    *q = (camera_csi_quality_t){.exposure = 456, .exposure_min = 1, .exposure_max = 600, .gain = 3,
        .gain_min = 0, .gain_max = 7, .gain_value = 2048, .gain_base = 1024, .gain_limit = 4096,
        .has_gain_menu = true};
    return call(CSI_QUALITY);
}
