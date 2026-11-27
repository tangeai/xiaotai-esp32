// SPDX-License-Identifier: MIT
#include "native_poll_shim.h"
#include "xiaotai/port/mem.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int reg; size_t size; bool write; } transfer_t;
static int bus, device;
static transfer_t seen[3];
static size_t calls, fail_at, allocations;
static uint8_t status;
static bool started, fail_alloc, fail_add, fail_del;
static uint16_t expected_address = 0x5d;
static uint32_t expected_speed = 400000;

static void expect(i2c_master_dev_handle_t handle, int reg, size_t size, bool write, int timeout_ms)
{
    assert(started && handle == &device && calls < sizeof(seen) / sizeof(seen[0]));
    assert(timeout_ms == 100);
    seen[calls++] = (transfer_t){.reg = reg, .size = size, .write = write};
}

void *xiaotai_i2c_native(xiaotai_i2c_bus_t *handle)
{
    assert(handle == (void *)&bus);
    return &bus;
}

void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    assert(kind == XIAOTAI_MEM_INTERNAL && count == 1);
    if (fail_alloc) return NULL;
    ++allocations;
    return calloc(count, size);
}

void xiaotai_mem_free(void *allocation)
{
    assert(allocation && allocations);
    --allocations;
    free(allocation);
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t handle, const i2c_device_config_t *config,
                                   i2c_master_dev_handle_t *out)
{
    assert(handle == &bus && config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    assert(config->device_address == expected_address && config->scl_speed_hz == expected_speed);
    if (fail_add) return XIAOTAI_ERR_TIMEOUT;
    *out = &device;
    started = true;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    assert(started && handle == &device);
    if (fail_del) return XIAOTAI_ERR_TIMEOUT;
    started = false;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t handle)
{
    return handle->del(handle);
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t handle, int reg, const void *data, size_t size)
{
    return handle->tx_param(handle, reg, data, size);
}

esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t handle, int reg, void *data, size_t size)
{
    return handle->rx_param(handle, reg, data, size);
}

esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t handle,
                                          i2c_master_transmit_multi_buffer_info_t *buffers,
                                          size_t count, int timeout_ms)
{
    assert(count == 2 && buffers[0].buffer_size == 2);
    const uint8_t *address = buffers[0].write_buffer;
    const int reg = (address[0] << 8) | address[1];
    expect(handle, reg, buffers[1].buffer_size, true, timeout_ms);
    assert(reg == 0x814e && buffers[1].buffer_size == 1 && *buffers[1].write_buffer == 0);
    return calls == fail_at ? XIAOTAI_ERR_TIMEOUT : ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t handle, const uint8_t *address,
                                     size_t address_size, uint8_t *data, size_t size, int timeout_ms)
{
    assert(address_size == 2);
    const int reg = (address[0] << 8) | address[1];
    expect(handle, reg, size, false, timeout_ms);
    if (calls == fail_at) return XIAOTAI_ERR_TIMEOUT;
    uint8_t *p = data;
    if (reg == 0x814e) {
        assert(size == 1);
        p[0] = status;
    } else if (reg == 0x814f) {
        assert(size == (size_t)(status & 15) * 8);
        memset(p, 0, size);
        for (size_t i = 0; i < size / 8; ++i) {
            p[i * 8] = (uint8_t)i;
            p[i * 8 + 1] = (uint8_t)(23 + i);
            p[i * 8 + 3] = (uint8_t)(70 + i);
        }
    } else {
        assert(reg == 0x8093 && size == 1);
        p[0] = 1;
    }
    return ESP_OK;
}

esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t t, esp_lcd_touch_point_data_t *out,
                                 uint8_t *count, uint8_t limit)
{
    *count = t->data.points <= limit ? t->data.points : limit;
    memcpy(out, t->data.coords, *count * sizeof(*out));
    return ESP_OK;
}

static void branch(void *controller, uint8_t report, size_t fail, uint8_t expected_count,
                   xiaotai_err_t error, size_t io_count, int second, size_t payload)
{
    esp_lcd_touch_handle_t t = controller;
    memset(&t->data, 0, sizeof(t->data));
    calls = 0;
    fail_at = fail;
    status = report;
    size_t count = 0;
    xiaotai_touch_point_t points[5];
    assert(xiaotai_gt911_io.read(controller, points, 5, &count) == error);
    assert(calls == io_count && count == expected_count);
    assert(seen[0].reg == 0x814e && seen[0].size == 1 && !seen[0].write);
    if (io_count > 1) assert(seen[1].reg == second && seen[1].size == payload);
    if (io_count > 2) assert(seen[2].reg == 0x814e && seen[2].size == 1 && seen[2].write);
    if (io_count == 2 && second == 0x814e) assert(seen[1].write);
    for (size_t i = 0; i < count; ++i) assert(points[i].x == 23 + i && points[i].y == 70 + i);
}

int main(void)
{
    xiaotai_touch_i2c_config_t cfg = {
        .bus = (void *)&bus, .address = 0x5d, .hz = 400000, .width = 1024, .height = 600,
        .reset_pin = 23, .interrupt_pin = 21, .reset_active_high = true,
    };
    void *handle = NULL, *controller = NULL;
    fail_alloc = true;
    assert(xiaotai_gt911_io.io_open(&cfg, &handle) == XIAOTAI_ERR_NO_MEM && !handle && !allocations);
    fail_alloc = false;
    fail_add = true;
    assert(xiaotai_gt911_io.io_open(&cfg, &handle) == XIAOTAI_ERR_TIMEOUT && !handle && !allocations);
    fail_add = false;
    assert(xiaotai_gt911_io.io_open(&cfg, &handle) == ESP_OK);
    assert(xiaotai_gt911_io.controller_open(&cfg, handle, &controller) == ESP_OK);
    branch(controller, 0x81, 0, 1, ESP_OK, 3, 0x814f, 8);
    branch(controller, 0x85, 0, 5, ESP_OK, 3, 0x814f, 40);
    branch(controller, 0x00, 0, 0, ESP_OK, 2, 0x814e, 1);
    branch(controller, 0x80, 0, 0, ESP_OK, 2, 0x814e, 1);
    branch(controller, 0x86, 0, 0, ESP_OK, 2, 0x814e, 1);
    branch(controller, 0x90, 0, 0, ESP_OK, 3, 0x8093, 1);
    branch(controller, 0x81, 1, 0, XIAOTAI_ERR_TIMEOUT, 1, 0, 0);
    branch(controller, 0x81, 2, 0, XIAOTAI_ERR_TIMEOUT, 2, 0x814f, 8);
    branch(controller, 0x81, 3, 0, XIAOTAI_ERR_TIMEOUT, 3, 0x814f, 8);
    branch(controller, 0x00, 2, 0, ESP_OK, 2, 0x814e, 1); /* Stock not-ready ACK semantics. */
    branch(controller, 0x90, 3, 0, ESP_OK, 3, 0x8093, 1); /* Stock key ACK semantics. */
    assert(xiaotai_gt911_io.controller_close(controller) == ESP_OK);
    fail_del = true;
    assert(xiaotai_gt911_io.io_close(handle) == XIAOTAI_ERR_TIMEOUT && started && allocations == 1);
    fail_del = false;
    assert(xiaotai_gt911_io.io_close(handle) == ESP_OK && !started && !allocations);
    cfg.address = expected_address = 0x14;
    cfg.hz = 0;
    expected_speed = 100000;
    assert(xiaotai_gt911_io.io_open(&cfg, &handle) == ESP_OK);
    assert(xiaotai_gt911_io.io_close(handle) == ESP_OK && !started && !allocations);
    return 0;
}
