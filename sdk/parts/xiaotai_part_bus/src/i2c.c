// SPDX-License-Identifier: MIT
/* I2C bus object: argument checks, the per-bus lock that serializes every
 * transfer made through this API, and the locked read-modify-write that the
 * power and expander chips rely on. The wire work is in bus_io. */
#include "xiaotai/parts/bus.h"

#include "bus_io.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

/* Bus timeout when the board gives 0: every bus of the existing boards uses
 * 100 ms per transfer. */
#define DEFAULT_TIMEOUT_MS 100U
#define ADDRESS_MAX 0x7fU

struct xiaotai_i2c_bus {
    xiaotai_mutex_t lock;
    uint32_t timeout_ms;
    xiaotai_bus_io_i2c_t *io;
};

xiaotai_err_t xiaotai_i2c_create(const xiaotai_i2c_config_t *config, xiaotai_i2c_bus_t **bus)
{
    if (bus) *bus = NULL;
    if (!config || !bus || config->port < 0 || config->sda_pin < 0 || config->scl_pin < 0 || config->hz == 0) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    xiaotai_i2c_config_t resolved = *config;
    if (resolved.timeout_ms == 0) resolved.timeout_ms = DEFAULT_TIMEOUT_MS;

    struct xiaotai_i2c_bus *object = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*object));
    if (!object) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = xiaotai_mutex_init(&object->lock);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(object);
        return err;
    }
    err = xiaotai_bus_io_i2c_open(&resolved, &object->io);
    if (err != XIAOTAI_OK) {
        xiaotai_mutex_deinit(&object->lock);
        xiaotai_mem_free(object);
        return err;
    }
    object->timeout_ms = resolved.timeout_ms;
    *bus = object;
    return XIAOTAI_OK;
}

/* The wait for the bus and each transfer are both limited by the bus timeout,
 * so a single call blocks for at most a few timeouts, never indefinitely. */
static bool take(xiaotai_i2c_bus_t *bus)
{
    return xiaotai_mutex_lock(&bus->lock, bus->timeout_ms);
}

xiaotai_err_t xiaotai_i2c_probe(xiaotai_i2c_bus_t *bus, uint8_t address)
{
    if (!bus || address > ADDRESS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (!take(bus)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t err = xiaotai_bus_io_i2c_probe(bus->io, address, bus->timeout_ms);
    xiaotai_mutex_unlock(&bus->lock);
    return err;
}

xiaotai_err_t xiaotai_i2c_read(xiaotai_i2c_bus_t *bus, uint8_t address, uint8_t reg, uint8_t *data, size_t size)
{
    if (!bus || address > ADDRESS_MAX || !data || size == 0) return XIAOTAI_ERR_INVALID_ARG;
    if (!take(bus)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t err = xiaotai_bus_io_i2c_read(bus->io, address, reg, data, size, bus->timeout_ms);
    xiaotai_mutex_unlock(&bus->lock);
    return err;
}

xiaotai_err_t xiaotai_i2c_write(xiaotai_i2c_bus_t *bus, uint8_t address, uint8_t reg, const uint8_t *data,
                                size_t size)
{
    if (!bus || address > ADDRESS_MAX || (!data && size > 0)) return XIAOTAI_ERR_INVALID_ARG;
    if (size > XIAOTAI_BUS_IO_I2C_WRITE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    if (!take(bus)) return XIAOTAI_ERR_TIMEOUT;
    xiaotai_err_t err = xiaotai_bus_io_i2c_write(bus->io, address, reg, data, size, bus->timeout_ms);
    xiaotai_mutex_unlock(&bus->lock);
    return err;
}

xiaotai_err_t xiaotai_i2c_update(xiaotai_i2c_bus_t *bus, uint8_t address, uint8_t reg, uint8_t mask, uint8_t bits)
{
    if (!bus || address > ADDRESS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (!take(bus)) return XIAOTAI_ERR_TIMEOUT;
    /* Read and write under one hold: another task's update of other bits of
     * the same register cannot slip in between and be overwritten. The value
     * is written even when unchanged, as the existing read-modify-write did. */
    uint8_t current = 0;
    xiaotai_err_t err = xiaotai_bus_io_i2c_read(bus->io, address, reg, &current, 1, bus->timeout_ms);
    if (err == XIAOTAI_OK) {
        const uint8_t value = (uint8_t)((current & (uint8_t)~mask) | (bits & mask));
        err = xiaotai_bus_io_i2c_write(bus->io, address, reg, &value, 1, bus->timeout_ms);
    }
    xiaotai_mutex_unlock(&bus->lock);
    return err;
}

void *xiaotai_i2c_native(xiaotai_i2c_bus_t *bus)
{
    return bus ? xiaotai_bus_io_i2c_native(bus->io) : NULL;
}
