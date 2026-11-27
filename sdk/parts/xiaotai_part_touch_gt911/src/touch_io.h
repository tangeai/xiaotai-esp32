// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/parts/touch_gt911.h"

#define GT911_PRIMARY_ADDRESS 0x5DU
#define GT911_BACKUP_ADDRESS 0x14U

typedef struct {
    xiaotai_err_t (*io_open)(const xiaotai_touch_i2c_config_t *config, void **io);
    xiaotai_err_t (*io_close)(void *io);
    xiaotai_err_t (*controller_open)(const xiaotai_touch_i2c_config_t *config, void *io, void **controller);
    xiaotai_err_t (*controller_close)(void *controller);
    xiaotai_err_t (*read)(void *controller, xiaotai_touch_point_t *points, size_t capacity, size_t *count);
    void *(*native_handle)(void *controller);
} gt911_io_t;

extern const gt911_io_t xiaotai_gt911_io;
