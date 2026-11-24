// SPDX-License-Identifier: MIT
#include "xiaotai/parts/panel_ili9341.h"
#include "xiaotai/port/mem.h"

#include <string.h>

static xiaotai_err_t create(void *io, const void *dev, const xiaotai_panel_spi_config_t *config, void **out)
{
    (void)dev;
    (void)config;
    *out = io;
    return XIAOTAI_OK;
}

static xiaotai_err_t prepare(const xiaotai_panel_spi_config_t *config, void **storage)
{
    const size_t bytes = config->init_count * sizeof(*config->init);
    *storage = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, bytes);
    if (!*storage) return XIAOTAI_ERR_NO_MEM;
    memcpy(*storage, config->init, bytes);
    return XIAOTAI_OK;
}

const xiaotai_panel_spi_controller_t *xiaotai_panel_ili9341(void)
{
    static const xiaotai_panel_spi_controller_t controller = {
        .name = "ili9341", .create = create, .prepare_vendor = prepare,
    };
    return &controller;
}
