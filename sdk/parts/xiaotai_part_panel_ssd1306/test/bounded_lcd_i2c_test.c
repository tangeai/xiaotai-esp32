// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_lcd_timeout.c::lcd_timeout
#include "bounded_lcd_i2c.h"
#include "esp_lcd_panel_io_interface.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct i2c_master_bus_t { int unused; } bus;
static struct i2c_master_dev_t { int unused; } device;
static bool allocated;
static int timeout_seen, callback_count, other_count;
static esp_err_t add_result, transaction_result, remove_result;
static uint8_t wire[1025];
static size_t wire_size;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b,
    const i2c_device_config_t *config, i2c_master_dev_handle_t *out)
{
    assert(b == &bus && config->device_address == 0x3c && config->scl_speed_hz == 400000);
    assert(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    if (add_result != ESP_OK) return add_result;
    assert(!allocated);
    allocated = true;
    *out = &device;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d)
{
    assert(d == &device && allocated);
    if (remove_result != ESP_OK) return remove_result;
    allocated = false;
    return ESP_OK;
}

static esp_err_t transaction(i2c_master_dev_handle_t d, int ms)
{
    assert(d == &device && allocated);
    timeout_seen = ms;
    return transaction_result;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d,
    const uint8_t *w, size_t wn, uint8_t *r, size_t rn, int ms)
{
    assert(wn <= sizeof(wire) && rn == 1);
    memcpy(wire, w, wn);
    wire_size = wn;
    if (transaction_result == ESP_OK) *r = 0x55;
    return transaction(d, ms);
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *r, size_t n, int ms)
{
    assert(n == 1);
    wire_size = 0;
    if (transaction_result == ESP_OK) *r = 0x66;
    return transaction(d, ms);
}

esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t d,
    i2c_master_transmit_multi_buffer_info_t *buf, size_t n, int ms)
{
    assert(n == 3);
    wire_size = 0;
    for (size_t i = 0; i < n; ++i) {
        assert(wire_size + buf[i].buffer_size <= sizeof(wire));
        if (buf[i].buffer_size) memcpy(wire + wire_size, buf[i].write_buffer, buf[i].buffer_size);
        wire_size += buf[i].buffer_size;
    }
    return transaction(d, ms);
}

static bool completed(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    assert(io && !event && (ctx == &callback_count || ctx == &other_count));
    ++*(int *)ctx;
    return false;
}

int main(void)
{
    esp_lcd_panel_io_i2c_config_t config = {
        .dev_addr = 0x3c, .scl_speed_hz = 400000, .control_phase_bytes = 1,
        .dc_bit_offset = 6, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = completed, .user_ctx = &callback_count,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    assert(xiaotai_ssd1306_new_bounded_lcd_i2c(&bus, &config, 0, &io) == ESP_ERR_INVALID_ARG);
    assert(xiaotai_ssd1306_new_bounded_lcd_i2c(&bus, &config, -1, &io) == ESP_ERR_INVALID_ARG);
    assert(xiaotai_ssd1306_new_bounded_lcd_i2c(NULL, &config, 100, &io) == ESP_ERR_INVALID_ARG);
    config.dc_bit_offset = 8;
    assert(xiaotai_ssd1306_new_bounded_lcd_i2c(&bus, &config, 100, &io) == ESP_ERR_INVALID_ARG);
    config.dc_bit_offset = 6;
    add_result = ESP_FAIL;
    assert(xiaotai_ssd1306_new_bounded_lcd_i2c(&bus, &config, 100, &io) == ESP_FAIL);
    assert(!io && !allocated);
    add_result = ESP_OK;

    for (int round = 0; round < 1000; ++round) {
        assert(xiaotai_ssd1306_new_bounded_lcd_i2c(&bus, &config, 100, &io) == ESP_OK);
        uint8_t value = 0x5a;
        assert(io->tx_param(io, 0xa1, &value, 1) == ESP_OK);
        assert(timeout_seen == 100 && wire_size == 3 && wire[0] == 0 && wire[1] == 0xa1 && wire[2] == value);
        int before = callback_count;
        transaction_result = ESP_ERR_TIMEOUT;
        assert(io->tx_color(io, -1, &value, 1) == ESP_ERR_TIMEOUT);
        assert(timeout_seen == 100 && callback_count == before);
        assert(io->rx_param(io, 0xa1, &value, 1) == ESP_ERR_TIMEOUT && timeout_seen == 100 && value == 0x5a);
        assert(wire_size == 2 && wire[0] == 0 && wire[1] == 0xa1);
        assert(io->rx_param(io, -1, &value, 1) == ESP_ERR_TIMEOUT && timeout_seen == 100 && value == 0x5a);
        assert(wire_size == 0);
        transaction_result = ESP_OK;
        uint8_t frame[1024] = {0x5a, [1023] = 0xa5};
        assert(io->tx_color(io, -1, frame, sizeof(frame)) == ESP_OK);
        assert(wire_size == 1025 && wire[0] == 0x40 && !memcmp(wire + 1, frame, sizeof(frame)));
        assert(callback_count == before + 1);
        remove_result = ESP_FAIL;
        assert(io->del(io) == ESP_FAIL && allocated);
        remove_result = ESP_OK;
        assert(io->del(io) == ESP_OK && !allocated);
        io = NULL;
    }

    config.lcd_cmd_bits = 16;
    config.flags.dc_low_on_data = true;
    assert(xiaotai_ssd1306_new_bounded_lcd_i2c(&bus, &config, 37, &io) == ESP_OK);
    uint8_t value = 0;
    assert(io->rx_param(io, 0x9234, &value, 1) == ESP_OK && value == 0x55 && timeout_seen == 37);
    assert(wire_size == 3 && wire[0] == 0x40 && wire[1] == 0x92 && wire[2] == 0x34);
    assert(io->rx_param(io, -1, &value, 1) == ESP_OK && value == 0x66 && timeout_seen == 37);
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = completed};
    assert(io->register_event_callbacks(io, &callbacks, &other_count) == ESP_OK);
    assert(io->tx_color(io, -1, &value, 1) == ESP_OK && other_count == 1 && callback_count == 1000);
    assert(wire_size == 2 && wire[0] == 0 && wire[1] == value && timeout_seen == 37);
    const esp_lcd_panel_io_callbacks_t none = {0};
    assert(io->register_event_callbacks(io, &none, NULL) == ESP_OK);
    assert(io->tx_color(io, -1, &value, 1) == ESP_OK && other_count == 1);
    assert(io->del(io) == ESP_OK && !allocated);

    config.flags.disable_control_phase = true;
    config.control_phase_bytes = 0;
    config.lcd_cmd_bits = 32;
    assert(xiaotai_ssd1306_new_bounded_lcd_i2c(&bus, &config, 100, &io) == ESP_OK);
    assert(io->tx_param(io, 0x12345678, NULL, 0) == ESP_OK);
    assert(wire_size == 4 && !memcmp(wire, (const uint8_t[]){0x12, 0x34, 0x56, 0x78}, wire_size));
    assert(io->del(io) == ESP_OK && !allocated);
    puts("bounded LCD I2C: timeout, wire format, callback and ownership checks passed");
    return 0;
}
