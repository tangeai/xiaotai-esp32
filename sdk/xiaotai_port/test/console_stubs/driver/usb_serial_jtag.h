// SPDX-License-Identifier: MIT
#pragma once
#include "esp_heap_caps.h"
#include <stdbool.h>
typedef struct { uint32_t tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
bool usb_serial_jtag_is_driver_installed(void);
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config);
bool usb_serial_jtag_is_connected(void);
int usb_serial_jtag_read_bytes(void *data, uint32_t size, uint32_t ticks);
int usb_serial_jtag_write_bytes(const void *data, size_t size, uint32_t ticks);
esp_err_t usb_serial_jtag_wait_tx_done(uint32_t ticks);
