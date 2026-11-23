// SPDX-License-Identifier: MIT
/* bus_io I2C on the ESP-IDF master driver (driver/i2c_master.h), as every
 * existing board opened its bus: default clock source, glitch filter of 7
 * cycles, 7-bit device addresses at the bus speed, synchronous transfers.
 * Devices are added on first use and kept; this runs under the bus object's
 * lock, so the table needs no lock of its own. */
#include "bus_io.h"

#include "driver/i2c_master.h"
#include "xiaotai/port/mem.h"

/* Distinct addresses one bus reads or writes through this API (probes add no
 * device); the existing boards use at most two (an expander, or a power chip
 * and an expander). Drivers on xiaotai_i2c_native() add their own. */
#define DEVICES 8

typedef struct {
    uint8_t address;
    i2c_master_dev_handle_t handle;
} device_t;

struct xiaotai_bus_io_i2c {
    i2c_master_bus_handle_t bus;
    uint32_t hz;
    device_t devices[DEVICES];
    size_t device_count;
};

xiaotai_err_t xiaotai_bus_io_i2c_open(const xiaotai_i2c_config_t *config, xiaotai_bus_io_i2c_t **io)
{
    struct xiaotai_bus_io_i2c *state = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*state));
    if (!state) return XIAOTAI_ERR_NO_MEM;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = config->port,
        .sda_io_num = config->sda_pin,
        .scl_io_num = config->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = config->pullup,
    };
    /* ESP_ERR_INVALID_STATE when the port is already acquired, which is the
     * result the bus header promises; the codes are passed through. */
    esp_err_t err = i2c_new_master_bus(&bus_config, &state->bus);
    if (err != ESP_OK) {
        xiaotai_mem_free(state);
        return err;
    }
    /* A chip reset in the middle of a read leaves the slave driving SDA low,
     * and the driver clears the bus only after a transaction has timed out:
     * the first device to open after such a reset failed. Clock the bus free
     * (SCL pulses with SDA released) before any device uses it. A bus still
     * held after that shows up as each device's own error. */
    (void)i2c_master_bus_reset(state->bus);
    state->hz = config->hz;
    *io = state;
    return XIAOTAI_OK;
}

static esp_err_t device(xiaotai_bus_io_i2c_t *io, uint8_t address, i2c_master_dev_handle_t *handle)
{
    for (size_t i = 0; i < io->device_count; ++i) {
        if (io->devices[i].address == address) {
            *handle = io->devices[i].handle;
            return ESP_OK;
        }
    }
    if (io->device_count == DEVICES) return ESP_ERR_NO_MEM;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = io->hz,
    };
    esp_err_t err = i2c_master_bus_add_device(io->bus, &config, handle);
    if (err == ESP_OK) io->devices[io->device_count++] = (device_t){.address = address, .handle = *handle};
    return err;
}

/* IDF 6 reports NACK as INVALID_RESPONSE; older drivers can report
 * INVALID_STATE when the transfer does not reach DONE. Probe only on these
 * errors to distinguish a missing address from a data-phase rejection. */
static xiaotai_err_t result(xiaotai_bus_io_i2c_t *io, uint8_t address, esp_err_t err, uint32_t timeout_ms)
{
    if (err != ESP_ERR_INVALID_RESPONSE && err != ESP_ERR_INVALID_STATE) return err;
    return i2c_master_probe(io->bus, address, (int)timeout_ms) == ESP_ERR_NOT_FOUND ? XIAOTAI_ERR_NOT_FOUND : XIAOTAI_FAIL;
}

xiaotai_err_t xiaotai_bus_io_i2c_probe(xiaotai_bus_io_i2c_t *io, uint8_t address, uint32_t timeout_ms)
{
    return i2c_master_probe(io->bus, address, (int)timeout_ms);
}

xiaotai_err_t xiaotai_bus_io_i2c_read(xiaotai_bus_io_i2c_t *io, uint8_t address, uint8_t reg, uint8_t *data,
                                      size_t size, uint32_t timeout_ms)
{
    i2c_master_dev_handle_t handle;
    esp_err_t err = device(io, address, &handle);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(handle, &reg, 1, data, size, (int)timeout_ms);
    return result(io, address, err, timeout_ms);
}

xiaotai_err_t xiaotai_bus_io_i2c_write(xiaotai_bus_io_i2c_t *io, uint8_t address, uint8_t reg,
                                       const uint8_t *data, size_t size, uint32_t timeout_ms)
{
    i2c_master_dev_handle_t handle;
    esp_err_t err = device(io, address, &handle);
    if (err != ESP_OK) return err;
    uint8_t bytes[1 + XIAOTAI_BUS_IO_I2C_WRITE_MAX];
    bytes[0] = reg;
    for (size_t i = 0; i < size; ++i) bytes[1 + i] = data[i];
    err = i2c_master_transmit(handle, bytes, 1 + size, (int)timeout_ms);
    return result(io, address, err, timeout_ms);
}

void *xiaotai_bus_io_i2c_native(xiaotai_bus_io_i2c_t *io)
{
    return io->bus;
}
