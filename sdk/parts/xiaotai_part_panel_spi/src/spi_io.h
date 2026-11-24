// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/parts/panel_spi.h"

xiaotai_err_t xt_spi_bus_open(const xiaotai_panel_spi_config_t *config, size_t transfer_bytes);
xiaotai_err_t xt_spi_bus_close(int host);
xiaotai_err_t xt_spi_io_open(const xiaotai_panel_spi_config_t *config, void **io);
xiaotai_err_t xt_spi_io_close(void *io);
xiaotai_err_t xt_spi_panel_open(const xiaotai_panel_spi_controller_t *controller,
                               const xiaotai_panel_spi_config_t *config, void *io, void *vendor, void **panel);
xiaotai_err_t xt_spi_panel_close(void *panel);
xiaotai_err_t xt_spi_panel_reset(void *panel);
xiaotai_err_t xt_spi_panel_setup(void *panel, const xiaotai_panel_spi_config_t *config);
xiaotai_err_t xt_spi_panel_on(void *panel);
xiaotai_err_t xt_spi_draw(void *panel, const xiaotai_rect_t *area, const void *pixels);
xiaotai_err_t xt_spi_drain(void *io);
void xt_spi_handles(void *panel, void *io, xiaotai_display_handles_t *out);
