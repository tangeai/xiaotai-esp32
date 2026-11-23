/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Derived from ESP-IDF's LCD I2C panel IO via the legacy Xmini backend.
 * All data paths are synchronous and use the caller's finite timeout. */
#include "bounded_lcd_i2c.h"
#include "driver/i2c_master.h"
#include "esp_compiler.h"
#include "esp_lcd_panel_io_interface.h"

#include <stdlib.h>

typedef struct {
    esp_lcd_panel_io_t base;
    i2c_master_dev_handle_t device;
    int timeout_ms;
    size_t cmd_bytes;
    bool control_phase;
    uint8_t cmd_control, data_control;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_done;
    void *ctx;
} bounded_io_t;

static esp_err_t del(esp_lcd_panel_io_t *base)
{
    bounded_io_t *io = (bounded_io_t *)base;
    const esp_err_t err = i2c_master_bus_rm_device(io->device);
    if (err == ESP_OK) free(io);
    return err;
}

static esp_err_t register_callbacks(esp_lcd_panel_io_handle_t base,
                                    const esp_lcd_panel_io_callbacks_t *callbacks, void *ctx)
{
    bounded_io_t *io = (bounded_io_t *)base;
    if (!callbacks) return ESP_ERR_INVALID_ARG;
    io->on_color_done = callbacks->on_color_trans_done;
    io->ctx = ctx;
    return ESP_OK;
}

static size_t encode_command(const bounded_io_t *io, int cmd, uint8_t *bytes)
{
    if (cmd == -1) return 0;
    for (size_t i = 0; i < io->cmd_bytes; ++i)
        bytes[i] = (uint8_t)((uint32_t)cmd >> (8 * (io->cmd_bytes - 1 - i)));
    return io->cmd_bytes;
}

static esp_err_t rx_param(esp_lcd_panel_io_t *base, int cmd, void *param, size_t size)
{
    bounded_io_t *io = (bounded_io_t *)base;
    if (cmd == -1) return i2c_master_receive(io->device, param, size, io->timeout_ms);
    uint8_t write[1 + sizeof(uint32_t)] = {io->cmd_control};
    const size_t control_bytes = io->control_phase ? 1 : 0;
    const size_t cmd_bytes = encode_command(io, cmd, write + control_bytes);
    return i2c_master_transmit_receive(io->device, write, control_bytes + cmd_bytes, param, size,
                                       io->timeout_ms);
}

static esp_err_t tx_buffer(esp_lcd_panel_io_t *base, int cmd, const void *data, size_t size, bool is_param)
{
    bounded_io_t *io = (bounded_io_t *)base;
    if (size && !data) return ESP_ERR_INVALID_ARG;
    uint8_t control = is_param ? io->cmd_control : io->data_control;
    uint8_t bytes[sizeof(uint32_t)];
    const size_t cmd_bytes = encode_command(io, cmd, bytes);
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {.write_buffer = &control, .buffer_size = io->control_phase ? 1 : 0},
        {.write_buffer = bytes, .buffer_size = cmd_bytes},
        {.write_buffer = data, .buffer_size = size},
    };
    const esp_err_t err = i2c_master_multi_buffer_transmit(io->device, buffers, sizeof(buffers) / sizeof(buffers[0]),
                                                          io->timeout_ms);
    if (err == ESP_OK && !is_param && io->on_color_done)
        io->on_color_done(base, NULL, io->ctx);
    return err;
}

static esp_err_t tx_param(esp_lcd_panel_io_t *base, int cmd, const void *param, size_t size)
{
    return tx_buffer(base, cmd, param, size, true);
}

static esp_err_t tx_color(esp_lcd_panel_io_t *base, int cmd, const void *pixels, size_t size)
{
    return tx_buffer(base, cmd, pixels, size, false);
}

esp_err_t xiaotai_ssd1306_new_bounded_lcd_i2c(i2c_master_bus_handle_t bus,
    const esp_lcd_panel_io_i2c_config_t *config, int timeout_ms, esp_lcd_panel_io_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!bus || !config || timeout_ms <= 0 || config->dev_addr > 0x7f || !config->scl_speed_hz ||
        config->lcd_cmd_bits < 8 || config->lcd_cmd_bits > 32 || config->lcd_cmd_bits % 8 ||
        (!config->flags.disable_control_phase &&
         (config->control_phase_bytes != 1 || config->dc_bit_offset >= 8)))
        return ESP_ERR_INVALID_ARG;

    ESP_COMPILER_DIAGNOSTIC_PUSH_IGNORE("-Wanalyzer-malloc-leak")
    bounded_io_t *io = calloc(1, sizeof(*io));
    if (!io) return ESP_ERR_NO_MEM;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->dev_addr,
        .scl_speed_hz = config->scl_speed_hz,
    };
    const esp_err_t err = i2c_master_bus_add_device(bus, &device, &io->device);
    if (err != ESP_OK) {
        free(io);
        return err;
    }

    io->base = (esp_lcd_panel_io_t){
        .del = del, .rx_param = rx_param, .tx_param = tx_param, .tx_color = tx_color,
        .register_event_callbacks = register_callbacks,
    };
    io->timeout_ms = timeout_ms;
    io->cmd_bytes = (size_t)config->lcd_cmd_bits / 8;
    io->control_phase = !config->flags.disable_control_phase;
    if (io->control_phase) {
        io->cmd_control = config->flags.dc_low_on_data ? (uint8_t)(1U << config->dc_bit_offset) : 0;
        io->data_control = config->flags.dc_low_on_data ? 0 : (uint8_t)(1U << config->dc_bit_offset);
    }
    io->on_color_done = config->on_color_trans_done;
    io->ctx = config->user_ctx;
    *out = &io->base;
    return ESP_OK;
    ESP_COMPILER_DIAGNOSTIC_POP("-Wanalyzer-malloc-leak")
}
