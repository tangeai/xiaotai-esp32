// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_TIMEOUT 0x107

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;
typedef enum { I2C_ADDR_BIT_LEN_7, I2C_ADDR_BIT_LEN_10 } i2c_addr_bit_len_t;
typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;
typedef struct { const uint8_t *write_buffer; size_t buffer_size; } i2c_master_transmit_multi_buffer_info_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t, uint8_t *, size_t, int);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t, uint8_t *, size_t, int);
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t, i2c_master_transmit_multi_buffer_info_t *,
                                          size_t, int);
