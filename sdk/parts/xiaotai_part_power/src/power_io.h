// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/parts/power.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define POWER_LOCK_MS 500U
#define POWER_ADDRESS_MAX 0x7fU

typedef struct {
    xiaotai_i2c_bus_t *bus;
    xiaotai_mutex_t lock;
    uint16_t lock_ms;
    uint8_t address;
    uint8_t registers[256U / 8U]; /* [board]: registers used by setup and published lines */
} power_io_t;

xiaotai_err_t power_io_init(power_io_t *io, xiaotai_i2c_bus_t *bus, uint8_t address, uint16_t lock_ms);
xiaotai_err_t power_io_setup(power_io_t *io, const xiaotai_reg_value_t *registers, size_t count);
void power_io_report(power_io_t *io, const char *name, const char *tail, xiaotai_hal_write_fn_t write, void *ctx);

static inline void power_io_track(power_io_t *io, uint8_t reg)
{
    io->registers[reg / 8U] |= (uint8_t)(1U << (reg % 8U));
}

static inline xiaotai_err_t power_io_read(power_io_t *io, uint8_t reg, uint8_t *value)
{
    return xiaotai_i2c_read(io->bus, io->address, reg, value, 1);
}

static inline xiaotai_err_t power_io_write(power_io_t *io, uint8_t reg, uint8_t value)
{
    return xiaotai_i2c_write(io->bus, io->address, reg, &value, 1);
}

static inline xiaotai_err_t power_io_update(power_io_t *io, uint8_t reg, uint8_t mask, bool active)
{
    return xiaotai_i2c_update(io->bus, io->address, reg, mask, active ? mask : 0);
}
