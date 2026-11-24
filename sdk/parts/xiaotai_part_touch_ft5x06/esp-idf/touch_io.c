// SPDX-License-Identifier: MIT
#include "touch_io.h"
#include "xiaotai/port/mem.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

#define FT_TIMEOUT_MS 100
static const char *TAG = "touch_ft5x06";

typedef struct {
    esp_lcd_panel_io_t base;
    i2c_master_dev_handle_t device;
} ft_io_t;

static esp_err_t read_register(esp_lcd_panel_io_t *base, int command, void *data, size_t size)
{
    ft_io_t *io = (ft_io_t *)base;
    if (command < 0 || command > UINT8_MAX || !size || !data) return ESP_ERR_INVALID_ARG;
    const uint8_t reg = (uint8_t)command;
    return i2c_master_transmit_receive(io->device, &reg, 1, data, size, FT_TIMEOUT_MS);
}

static esp_err_t write_register(esp_lcd_panel_io_t *base, int command, const void *data, size_t size)
{
    ft_io_t *io = (ft_io_t *)base;
    if (command < 0 || command > UINT8_MAX || size != 1 || !data) return ESP_ERR_INVALID_ARG;
    const uint8_t wire[] = {(uint8_t)command, *(const uint8_t *)data};
    return i2c_master_transmit(io->device, wire, sizeof(wire), FT_TIMEOUT_MS);
}

static esp_err_t delete_io(esp_lcd_panel_io_t *base)
{
    ft_io_t *io = (ft_io_t *)base;
    const esp_err_t err = i2c_master_bus_rm_device(io->device);
    if (err == ESP_OK) xiaotai_mem_free(io);
    return err;
}

xiaotai_err_t xt_ft_io_open(const xiaotai_touch_i2c_config_t *config, void **out)
{
    if ((config->reset_pin >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(config->reset_pin)) ||
        (config->interrupt_pin >= 0 && !GPIO_IS_VALID_GPIO(config->interrupt_pin)))
        return XIAOTAI_ERR_INVALID_ARG;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = config->address, .scl_speed_hz = config->hz,
    };
    ft_io_t *io = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*io));
    if (!io) return XIAOTAI_ERR_NO_MEM;
    const esp_err_t err = i2c_master_bus_add_device(xiaotai_i2c_native(config->bus), &device, &io->device);
    if (err != ESP_OK) {
        xiaotai_mem_free(io);
        return err;
    }
    io->base = (esp_lcd_panel_io_t){.del = delete_io, .rx_param = read_register, .tx_param = write_register};
    *out = &io->base;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_ft_io_close(void *io)
{
    const esp_err_t err = esp_lcd_panel_io_del(io);
    if (err != ESP_OK) ESP_LOGE(TAG, "touch IO cleanup failed: %s", esp_err_to_name(err));
    return err;
}

xiaotai_err_t xt_ft_controller_open(const xiaotai_touch_i2c_config_t *c, void *io, void **out)
{
    const esp_lcd_touch_config_t config = {
        .x_max = c->width, .y_max = c->height, .rst_gpio_num = c->reset_pin, .int_gpio_num = c->interrupt_pin,
        .levels = {.reset = c->reset_active_high, .interrupt = 0},
        .flags = {.swap_xy = c->swap_xy, .mirror_x = c->mirror_x, .mirror_y = c->mirror_y},
    };
    esp_lcd_touch_handle_t touch = NULL;
    const esp_err_t err = esp_lcd_touch_new_i2c_ft5x06(io, &config, &touch);
    if (err == ESP_OK) *out = touch;
    return err;
}

xiaotai_err_t xt_ft_controller_close(void *controller)
{
    const esp_err_t err = esp_lcd_touch_del(controller);
    if (err != ESP_OK) ESP_LOGE(TAG, "touch cleanup failed: %s", esp_err_to_name(err));
    return err;
}

void *xt_ft_controller_native(void *controller)
{
    return controller;
}

size_t xt_ft_max_points(void)
{
    return CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
}

xiaotai_err_t xt_ft_io_read(void *io, uint8_t *frame, size_t size)
{
    return read_register(io, XT_FT_POINT_REGISTER, frame, size);
}
