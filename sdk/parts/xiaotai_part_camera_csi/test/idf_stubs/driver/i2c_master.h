// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
enum { I2C_ADDR_BIT_LEN_7 = 0 };
typedef struct {
    unsigned dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

int i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                              i2c_master_dev_handle_t *device);
int i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
int i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *cmd, size_t cmd_size,
                                uint8_t *rx, size_t rx_size, int timeout);
int i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *cmd, size_t size, int timeout);
