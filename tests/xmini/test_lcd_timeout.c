#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bounded_lcd_i2c.h"

static int bus, device, allocated, timeout_seen, callback_count;
static esp_err_t add_result, transaction_result, remove_result;
static uint8_t wire[16];
static size_t wire_size;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b,
    const i2c_device_config_t *c, i2c_master_dev_handle_t *out)
{
    assert(b == &bus && c->device_address == 0x3c && c->scl_speed_hz == 400000);
    if (add_result) return add_result;
    assert(!allocated);
    allocated = 1;
    *out = &device;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d)
{
    assert(d == &device && allocated);
    if (remove_result) return remove_result;
    allocated = 0;
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
    assert(wn == 2 && w[0] == 0 && w[1] == 0xa1 && rn == 1);
    *r = 0x55;
    return transaction(d, ms);
}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *r, size_t n, int ms)
{
    assert(n == 1);
    *r = 0x66;
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
static bool completed(esp_lcd_panel_io_handle_t io, void *event, void *ctx)
{
    assert(io && !event && ctx == &callback_count);
    ++callback_count;
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
    assert(xiaotai_new_bounded_lcd_i2c(&bus, &config, 0, &io) == ESP_ERR_INVALID_ARG);
    assert(xiaotai_new_bounded_lcd_i2c(NULL, &config, 100, &io) == ESP_ERR_INVALID_ARG);
    add_result = ESP_FAIL;
    assert(xiaotai_new_bounded_lcd_i2c(&bus, &config, 100, &io) == ESP_FAIL);
    assert(!io && !allocated);
    add_result = ESP_OK;
    for (int round = 0; round < 1000; ++round) {
        assert(xiaotai_new_bounded_lcd_i2c(&bus, &config, 100, &io) == ESP_OK);
        uint8_t value = 0x5a;
        assert(io->tx_param(io, 0xa1, &value, 1) == ESP_OK);
        assert(timeout_seen == 100 && wire_size == 3 && wire[0] == 0 && wire[1] == 0xa1 && wire[2] == value);
        int before = callback_count;
        transaction_result = ESP_ERR_TIMEOUT;
        assert(io->tx_color(io, -1, &value, 1) == ESP_ERR_TIMEOUT);
        assert(timeout_seen == 100 && callback_count == before);
        assert(io->rx_param(io, 0xa1, &value, 1) == ESP_ERR_TIMEOUT && timeout_seen == 100);
        assert(io->rx_param(io, -1, &value, 1) == ESP_ERR_TIMEOUT && timeout_seen == 100);
        transaction_result = ESP_OK;
        assert(io->tx_color(io, -1, &value, 1) == ESP_OK);
        assert(wire_size == 2 && wire[0] == 0x40 && callback_count == before + 1);
        remove_result = ESP_FAIL;
        assert(io->del(io) == ESP_FAIL && allocated);
        remove_result = ESP_OK;
        assert(io->del(io) == ESP_OK && !allocated);
        io = NULL;
    }
    puts("bounded LCD I2C: timeout, wire format, callback and ownership checks passed");
    return 0;
}
