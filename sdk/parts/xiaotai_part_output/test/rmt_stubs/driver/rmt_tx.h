// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;
enum { ESP_OK = 0, ESP_ERR_TIMEOUT = 0x107, RMT_CLK_SRC_DEFAULT = 0 };
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
typedef struct {
    int gpio_num, clk_src;
    uint32_t resolution_hz;
    size_t mem_block_symbols, trans_queue_depth;
} rmt_tx_channel_config_t;
typedef struct { uint16_t level0, level1, duration0, duration1; } fake_symbol_t;
typedef struct {
    fake_symbol_t bit0, bit1;
    struct { uint32_t msb_first; } flags;
} rmt_bytes_encoder_config_t;
typedef struct {
    int loop_count;
    struct { uint32_t eot_level, queue_nonblocking; } flags;
} rmt_transmit_config_t;
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *, rmt_channel_handle_t *);
esp_err_t rmt_new_bytes_encoder(const rmt_bytes_encoder_config_t *, rmt_encoder_handle_t *);
esp_err_t rmt_enable(rmt_channel_handle_t);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t);
esp_err_t rmt_del_channel(rmt_channel_handle_t);
esp_err_t rmt_transmit(rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t,
                       const rmt_transmit_config_t *);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t, int);
