// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "bounded_lcd_i2c.h"

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"

xiaotai_err_t xiaotai_ssd1306_probe(const xiaotai_panel_ssd1306_config_t *config, int timeout_ms)
{
    return i2c_master_probe(xiaotai_i2c_native(config->bus), config->address, timeout_ms);
}

xiaotai_err_t xiaotai_ssd1306_new_io(const xiaotai_panel_ssd1306_config_t *config, int timeout_ms, void **out)
{
    const esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = config->address,
        .scl_speed_hz = config->hz,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_err_t err = xiaotai_ssd1306_new_bounded_lcd_i2c(xiaotai_i2c_native(config->bus), &io_config,
                                                            timeout_ms, &io);
    if (err == ESP_OK) *out = io;
    return err;
}

xiaotai_err_t xiaotai_ssd1306_new_panel(void *io, uint8_t height, void **out)
{
    esp_lcd_panel_ssd1306_config_t vendor = {.height = height};
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    const esp_err_t err = esp_lcd_new_panel_ssd1306(io, &panel_config, &panel);
    if (err == ESP_OK) *out = panel;
    return err;
}

xiaotai_err_t xiaotai_ssd1306_configure(void *panel, bool mirror_x, bool mirror_y, bool invert)
{
    esp_err_t err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    if (err == ESP_OK) err = esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel, invert);
    return err;
}

xiaotai_err_t xiaotai_ssd1306_draw(void *panel, uint8_t width, uint8_t height, const void *pixels)
{
    return esp_lcd_panel_draw_bitmap(panel, 0, 0, width, height, pixels);
}

xiaotai_err_t xiaotai_ssd1306_switch(void *panel, bool on)
{
    return esp_lcd_panel_disp_on_off(panel, on);
}

void xiaotai_ssd1306_native_handles(void *panel, void *io, xiaotai_display_handles_t *handles)
{
    handles->panel = panel;
    handles->panel_io = io;
}

xiaotai_err_t xiaotai_ssd1306_del_panel(void *panel)
{
    const esp_err_t err = esp_lcd_panel_del(panel);
    if (err != ESP_OK) ESP_LOGE("panel_ssd1306", "panel cleanup failed: %s", esp_err_to_name(err));
    return err;
}

xiaotai_err_t xiaotai_ssd1306_del_io(void *io)
{
    const esp_err_t err = esp_lcd_panel_io_del(io);
    if (err != ESP_OK) ESP_LOGE("panel_ssd1306", "IO cleanup failed: %s", esp_err_to_name(err));
    return err;
}
