// SPDX-License-Identifier: MIT
#include "touch_io.h"
#include "xiaotai/port/mem.h"

xiaotai_err_t xt_ft_io_open(const xiaotai_touch_i2c_config_t *config, void **out)
{
    xiaotai_touch_i2c_config_t *io = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*io));
    if (!io) return XIAOTAI_ERR_NO_MEM;
    *io = *config;
    *out = io;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_ft_io_close(void *io)
{
    xiaotai_mem_free(io);
    return XIAOTAI_OK;
}

xiaotai_err_t xt_ft_controller_open(const xiaotai_touch_i2c_config_t *config, void *io, void **out)
{
    const xiaotai_err_t err = xiaotai_i2c_probe(config->bus, config->address);
    if (err == XIAOTAI_OK) *out = io;
    return err;
}

xiaotai_err_t xt_ft_controller_close(void *controller)
{
    (void)controller;
    return XIAOTAI_OK;
}

void *xt_ft_controller_native(void *controller)
{
    (void)controller;
    return NULL;
}

size_t xt_ft_max_points(void)
{
    return XT_FT_MAX_POINTS;
}

xiaotai_err_t xt_ft_io_read(void *ptr, uint8_t *frame, size_t size)
{
    const xiaotai_touch_i2c_config_t *io = ptr;
    return xiaotai_i2c_read(io->bus, io->address, XT_FT_POINT_REGISTER, frame, size);
}
