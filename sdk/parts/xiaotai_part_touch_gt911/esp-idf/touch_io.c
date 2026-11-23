// SPDX-License-Identifier: MIT
#include "touch_io.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_touch_gt911.h"
#include "xiaotai/port/mem.h"

_Static_assert(GT911_PRIMARY_ADDRESS == ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, "GT911 primary address");
_Static_assert(GT911_BACKUP_ADDRESS == ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, "GT911 backup address");

enum { GT911_TIMEOUT_MS = 100 };

typedef struct {
    esp_lcd_panel_io_t panel;
    i2c_master_dev_handle_t device;
} gt911_panel_io_t;

/* Use the stock GT911 controller with a bounded I2C transport. IDF 6.0.2's
 * stock panel I2C transport waits indefinitely; its GT911 wiring uses a
 * big-endian 16-bit register with no additional control byte. */
static esp_err_t rx_param(esp_lcd_panel_io_t *panel, int reg, void *data, size_t size)
{
    if ((unsigned)reg > UINT16_MAX || !data || !size) return ESP_ERR_INVALID_ARG;
    gt911_panel_io_t *io = (gt911_panel_io_t *)panel;
    const uint8_t address[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(io->device, address, sizeof(address), data, size, GT911_TIMEOUT_MS);
}

static esp_err_t tx_param(esp_lcd_panel_io_t *panel, int reg, const void *data, size_t size)
{
    if ((unsigned)reg > UINT16_MAX || !data || !size) return ESP_ERR_INVALID_ARG;
    gt911_panel_io_t *io = (gt911_panel_io_t *)panel;
    const uint8_t address[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {.write_buffer = address, .buffer_size = sizeof(address)},
        {.write_buffer = data, .buffer_size = size},
    };
    return i2c_master_multi_buffer_transmit(io->device, buffers, 2, GT911_TIMEOUT_MS);
}

static esp_err_t del(esp_lcd_panel_io_t *panel)
{
    gt911_panel_io_t *io = (gt911_panel_io_t *)panel;
    const esp_err_t err = i2c_master_bus_rm_device(io->device);
    if (err == ESP_OK) xiaotai_mem_free(io);
    return err;
}

static xiaotai_err_t io_open(const xiaotai_touch_i2c_config_t *c, void **out)
{
    const i2c_master_bus_handle_t bus = xiaotai_i2c_native(c->bus);
    if (!bus) return XIAOTAI_ERR_NOT_SUPPORTED;
    const esp_lcd_panel_io_i2c_config_t defaults = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = c->address,
        .scl_speed_hz = c->hz ? c->hz : defaults.scl_speed_hz,
    };
    gt911_panel_io_t *io = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*io));
    if (!io) return XIAOTAI_ERR_NO_MEM;
    const esp_err_t err = i2c_master_bus_add_device(bus, &config, &io->device);
    if (err != ESP_OK) {
        xiaotai_mem_free(io);
        return err;
    }
    io->panel = (esp_lcd_panel_io_t){.rx_param = rx_param, .tx_param = tx_param, .del = del};
    *out = &io->panel;
    return XIAOTAI_OK;
}

static xiaotai_err_t io_close(void *io) { return esp_lcd_panel_io_del(io); }

static xiaotai_err_t controller_open(const xiaotai_touch_i2c_config_t *c, void *io, void **out)
{
    const bool interrupt_ok = c->interrupt_pin == -1 ||
        (c->reset_pin == -1 ? GPIO_IS_VALID_GPIO(c->interrupt_pin) : GPIO_IS_VALID_OUTPUT_GPIO(c->interrupt_pin));
    if ((c->reset_pin != -1 && !GPIO_IS_VALID_OUTPUT_GPIO(c->reset_pin)) ||
        !interrupt_ok) return XIAOTAI_ERR_INVALID_ARG;
    /* IDF retains driver_data in the handle. The vendor reads but does not
     * change this address descriptor; do not pass a stack address. */
    static esp_lcd_touch_io_gt911_config_t primary = {.dev_addr = GT911_PRIMARY_ADDRESS};
    static esp_lcd_touch_io_gt911_config_t backup = {.dev_addr = GT911_BACKUP_ADDRESS};
    const esp_lcd_touch_config_t config = {
        .x_max = c->width, .y_max = c->height,
        .rst_gpio_num = c->reset_pin, .int_gpio_num = c->interrupt_pin,
        .levels = {.reset = c->reset_active_high, .interrupt = 0},
        .flags = {.swap_xy = c->swap_xy, .mirror_x = c->mirror_x, .mirror_y = c->mirror_y},
        .driver_data = c->address == GT911_BACKUP_ADDRESS ? &backup : &primary,
    };
    esp_lcd_touch_handle_t touch = NULL;
    const esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &config, &touch);
    if (err == ESP_OK) *out = touch;
    return err;
}

static xiaotai_err_t controller_close(void *controller) { return esp_lcd_touch_del(controller); }
static void *native_handle(void *controller) { return controller; }

static xiaotai_err_t read_touch(void *controller, xiaotai_touch_point_t *points, size_t capacity, size_t *count)
{
    esp_lcd_touch_point_data_t sample[5]; /* GT911 reports at most five contacts. */
    const uint8_t limit = capacity < sizeof(sample) / sizeof(sample[0]) ? (uint8_t)capacity : 5;
    uint8_t received = 0;
    esp_err_t err = esp_lcd_touch_read_data(controller);
    if (err == ESP_OK) err = esp_lcd_touch_get_data(controller, sample, &received, limit);
    if (err != ESP_OK) return err;
    if (received > limit) return XIAOTAI_ERR_INVALID_SIZE;
    for (size_t i = 0; i < received; ++i) {
        points[i] = (xiaotai_touch_point_t){.x = sample[i].x, .y = sample[i].y};
    }
    *count = received;
    return XIAOTAI_OK;
}

const gt911_io_t xiaotai_gt911_io = {
    .io_open = io_open, .io_close = io_close, .controller_open = controller_open,
    .controller_close = controller_close, .read = read_touch, .native_handle = native_handle,
};
