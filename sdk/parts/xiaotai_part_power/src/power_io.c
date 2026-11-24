// SPDX-License-Identifier: MIT
#include "power_io.h"
#include <stdio.h>

xiaotai_err_t power_io_init(power_io_t *io, xiaotai_i2c_bus_t *bus, uint8_t address, uint16_t lock_ms)
{
    io->bus = bus;
    io->address = address;
    io->lock_ms = lock_ms ? lock_ms : POWER_LOCK_MS;
    return xiaotai_mutex_init(&io->lock);
}

xiaotai_err_t power_io_setup(power_io_t *io, const xiaotai_reg_value_t *registers, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_err_t err = power_io_write(io, registers[i].reg, registers[i].value);
        if (err != XIAOTAI_OK) return err;
        power_io_track(io, registers[i].reg);
    }
    return XIAOTAI_OK;
}

void power_io_report(power_io_t *io, const char *name, const char *tail, xiaotai_hal_write_fn_t write, void *ctx)
{
    if (!write) return;
    /* One complete line; exactly six characters per address/value pair. */
    char line[64 + 256U * 6U];
    size_t used = (size_t)snprintf(line, sizeof(line), "%s addr=0x%02x", name, (unsigned)io->address);
    const bool locked = xiaotai_mutex_lock(&io->lock, io->lock_ms);
    for (unsigned r = 0; r < 256U; ++r) {
        if (!(io->registers[r / 8U] & (1U << (r % 8U)))) continue;
        uint8_t v = 0;
        if (locked && power_io_read(io, (uint8_t)r, &v) == XIAOTAI_OK)
            used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x=%02x", r, (unsigned)v);
        else used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x=??", r);
    }
    if (locked) xiaotai_mutex_unlock(&io->lock);
    (void)snprintf(line + used, sizeof(line) - used, "%s", tail ? tail : "");
    write(ctx, line);
}
