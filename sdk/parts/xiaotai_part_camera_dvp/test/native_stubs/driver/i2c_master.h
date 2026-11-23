// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
esp_err_t i2c_master_get_bus_handle(int port, i2c_master_bus_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
