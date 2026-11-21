// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/parts/touch_ft5x06.h"

enum {
    XT_FT_ADDRESS = 0x38,
    XT_FT_MAX_POINTS = 5,
    XT_FT_POINT_BYTES = 6,
    XT_FT_POINT_REGISTER = 0x02,
    XT_FT_FRAME_BYTES = 1 + XT_FT_MAX_POINTS * XT_FT_POINT_BYTES,
};

xiaotai_err_t xt_ft_io_open(const xiaotai_touch_i2c_config_t *config, void **io);
xiaotai_err_t xt_ft_io_close(void *io);
xiaotai_err_t xt_ft_controller_open(const xiaotai_touch_i2c_config_t *config, void *io, void **controller);
xiaotai_err_t xt_ft_controller_close(void *controller);
void *xt_ft_controller_native(void *controller);
size_t xt_ft_max_points(void);
xiaotai_err_t xt_ft_io_read(void *io, uint8_t *frame, size_t size);
