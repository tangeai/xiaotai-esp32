// SPDX-License-Identifier: MIT
#pragma once
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include <stdbool.h>

enum { UART_DATA_8_BITS = 8, UART_PARITY_DISABLE = 0, UART_STOP_BITS_1 = 1,
       UART_HW_FLOWCTRL_DISABLE = 0, UART_SCLK_DEFAULT = 2, UART_FIFO_OVF, UART_BUFFER_FULL };
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;
typedef struct { int type; } uart_event_t;
bool uart_is_driver_installed(int port);
esp_err_t uart_driver_install(int port, int rx, int tx, int events, QueueHandle_t *queue, int flags);
esp_err_t uart_driver_delete(int port);
esp_err_t uart_param_config(int port, const uart_config_t *config);
esp_err_t uart_flush_input(int port);
int uart_read_bytes(int port, void *data, uint32_t size, uint32_t ticks);
int uart_tx_chars(int port, const char *data, uint32_t size);
esp_err_t uart_wait_tx_done(int port, uint32_t ticks);
