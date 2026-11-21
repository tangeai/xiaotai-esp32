// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/parts/panel_spi.h"

typedef enum {
    XT_SPI_BUS_OPEN, XT_SPI_IO_OPEN, XT_SPI_PANEL_OPEN, XT_SPI_RESET, XT_SPI_SETUP,
    XT_SPI_DRAW, XT_SPI_DRAIN, XT_SPI_ON, XT_SPI_PANEL_CLOSE, XT_SPI_IO_CLOSE, XT_SPI_BUS_CLOSE,
    XT_SPI_OP_COUNT,
} xt_spi_fake_op_t;

typedef struct {
    xt_spi_fake_op_t log[2048];
    size_t calls;
    unsigned count[XT_SPI_OP_COUNT];
    xiaotai_panel_spi_config_t config;
    size_t limit, pixels_painted;
    bool nonblack;
    xiaotai_rect_t drawn[1024];
    size_t draws;
    const uint8_t *queued;
    uint8_t last[8];
} xt_spi_fake_t;

const xt_spi_fake_t *xt_spi_fake_state(void);
void xt_spi_fake_reset(void);
void xt_spi_fake_fail(xt_spi_fake_op_t op, unsigned nth, xiaotai_err_t err);
