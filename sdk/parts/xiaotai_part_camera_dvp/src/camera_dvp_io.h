// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/parts/camera_dvp.h"

#define XT_DVP_BUFFERS 2U

typedef struct xt_dvp_io xt_dvp_io_t;
typedef enum { XT_DVP_160_120, XT_DVP_240_176, XT_DVP_320_240 } xt_dvp_size_t;

typedef struct {
    const uint8_t *data;
    size_t size, width, height;
    uint32_t format;
    int64_t timestamp_us;
    uint32_t skipped;
    void *token;
} xt_dvp_frame_t;

/* The platform owns the two raw frame buffers and internal DMA staging. A
 * failure with a non-NULL handle still owns resources; the caller must close. */
xiaotai_err_t xt_dvp_io_open(const xiaotai_camera_dvp_config_t *config, xt_dvp_size_t size, xt_dvp_io_t **io);
xiaotai_err_t xt_dvp_io_sensor(xt_dvp_io_t *io, uint16_t *sensor_id, bool *external_dma);
xiaotai_err_t xt_dvp_io_upright(xt_dvp_io_t *io);
xiaotai_err_t xt_dvp_io_close(xt_dvp_io_t *io);

/* Never waits for a frame. The newest acquired frame, or NOT_FOUND while no
 * frame is ready to loan. The stock driver acquires on a separate task. */
xiaotai_err_t xt_dvp_io_take(xt_dvp_io_t *io, xt_dvp_frame_t *out);
void xt_dvp_io_return(xt_dvp_io_t *io, void *token);
