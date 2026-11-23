// SPDX-License-Identifier: MIT
#include "bus_io.h"
#include "driver/i2c_master.h"
#include "xiaotai/port/mem.h"

#include <assert.h>

static struct master_bus { int value; } s_bus;
static struct master_device { int value; } s_device;
static esp_err_t s_wire, s_probe;
static unsigned probes, adds, reads, writes, resets;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *bus)
{
    assert(config->i2c_port == 1 && config->sda_io_num == 2 && config->scl_io_num == 3);
    assert(config->flags.enable_internal_pullup && config->glitch_ignore_cnt == 7);
    *bus = &s_bus;
    return ESP_OK;
}

/* A failed clear does not fail the open: the devices report a held bus. */
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus)
{
    assert(bus == &s_bus && adds == 0);
    ++resets;
    return ESP_ERR_TIMEOUT;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device)
{
    assert(bus == &s_bus && config->device_address == 0x40 && config->scl_speed_hz == 400000);
    ++adds;
    *device = &s_device;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    assert(bus == &s_bus && address == 0x40 && timeout_ms == 77);
    ++probes;
    return s_probe;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *write, size_t write_size,
                                     uint8_t *read, size_t read_size, int timeout_ms)
{
    assert(device == &s_device && write_size == 1 && write[0] == 0x03 && read_size == 1 && timeout_ms == 77);
    if (s_wire == ESP_OK) *read = 0x23;
    ++reads;
    return s_wire;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *write, size_t write_size, int timeout_ms)
{
    assert(device == &s_device && write_size == 2 && write[0] == 0x03 && write[1] == 0x23 && timeout_ms == 77);
    ++writes;
    return s_wire;
}

int main(void)
{
    const xiaotai_i2c_config_t config = {.port = 1, .sda_pin = 2, .scl_pin = 3, .pullup = true, .hz = 400000};
    xiaotai_bus_io_i2c_t *io = NULL;
    uint8_t data = 0;
    assert(xiaotai_bus_io_i2c_open(&config, &io) == XIAOTAI_OK);
    assert(resets == 1);
    assert(xiaotai_bus_io_i2c_native(io) == &s_bus);
    assert(xiaotai_bus_io_i2c_read(io, 0x40, 3, &data, 1, 77) == XIAOTAI_OK && data == 0x23);
    assert(probes == 0 && adds == 1);

    /* The checked-in IDF 6 I2C driver defines this return for a NACK, not
     * INVALID_STATE. Isolate an absent address from a data-phase rejection. */
    s_wire = ESP_ERR_INVALID_RESPONSE;
    s_probe = ESP_ERR_NOT_FOUND;
    assert(xiaotai_bus_io_i2c_read(io, 0x40, 3, &data, 1, 77) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_bus_io_i2c_write(io, 0x40, 3, &data, 1, 77) == XIAOTAI_ERR_NOT_FOUND);
    assert(probes == 2);
    s_probe = ESP_OK;
    assert(xiaotai_bus_io_i2c_write(io, 0x40, 3, &data, 1, 77) == XIAOTAI_FAIL);
    assert(probes == 3);

    s_wire = ESP_ERR_INVALID_STATE; /* older synchronous driver */
    s_probe = ESP_ERR_NOT_FOUND;
    assert(xiaotai_bus_io_i2c_read(io, 0x40, 3, &data, 1, 77) == XIAOTAI_ERR_NOT_FOUND);
    s_wire = ESP_ERR_TIMEOUT;
    assert(xiaotai_bus_io_i2c_read(io, 0x40, 3, &data, 1, 77) == XIAOTAI_ERR_TIMEOUT);
    assert(probes == 4 && adds == 1 && reads == 4 && writes == 2);
    xiaotai_mem_free(io);
    return 0;
}
