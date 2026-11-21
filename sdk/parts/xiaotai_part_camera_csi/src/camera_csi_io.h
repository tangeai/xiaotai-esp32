// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/parts/camera_csi.h"

typedef struct {
    uint32_t index, sequence;
    size_t bytes;
    int64_t captured_us;
    bool error;
} camera_csi_buffer_t;

typedef struct {
    int32_t exposure, gain;
    int64_t exposure_min, exposure_max, gain_min, gain_max;
    int64_t gain_value, gain_base, gain_limit;
    bool has_gain_menu;
} camera_csi_quality_t;

xiaotai_err_t camera_csi_io_alignment(size_t *bytes);
/* Read immediately after a failed device open or ioctl, before cleanup. */
int camera_csi_io_errno(void);
xiaotai_err_t camera_csi_io_start(const void *owner, const xiaotai_camera_csi_config_t *config, uint32_t hz);
xiaotai_err_t camera_csi_io_read(const void *owner, uint16_t reg, uint8_t *value);
xiaotai_err_t camera_csi_io_write(const void *owner, uint16_t reg, uint8_t value);
xiaotai_err_t camera_csi_io_open(int *fd);
xiaotai_err_t camera_csi_io_format(int fd, uint16_t width, uint16_t height, bool full_range, size_t *bytes);
xiaotai_err_t camera_csi_io_rate(int fd, uint8_t fps);
xiaotai_err_t camera_csi_io_request(int fd, unsigned count, unsigned *actual);
xiaotai_err_t camera_csi_io_buffer_size(int fd, unsigned index, size_t *bytes);
xiaotai_err_t camera_csi_io_queue(int fd, unsigned index, void *data, size_t capacity);
xiaotai_err_t camera_csi_io_dequeue(int fd, camera_csi_buffer_t *buffer);
xiaotai_err_t camera_csi_io_stream(int fd, bool start);
xiaotai_err_t camera_csi_io_close(int fd);
xiaotai_err_t camera_csi_io_quality(int fd, camera_csi_quality_t *quality);
