// SPDX-License-Identifier: MIT
#include "spi_io.h"

#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#ifdef XIAOTAI_SPI_NATIVE_CORRECTION
#include "spi_lcd_ownership.h"
#endif

static const char *TAG = "panel_spi";

xiaotai_err_t xt_spi_bus_open(const xiaotai_panel_spi_config_t *config, size_t transfer_bytes)
{
#ifndef XIAOTAI_SPI_EXTERNAL_DMA
    if (config->external_dma) return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
    const spi_bus_config_t bus = {
        .mosi_io_num = config->mosi_pin, .sclk_io_num = config->sclk_pin,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = (int)transfer_bytes,
    };
    return spi_bus_initialize(config->host, &bus, SPI_DMA_CH_AUTO);
}

static esp_err_t cleanup_result(const char *resource, esp_err_t err)
{
    if (err != ESP_OK) ESP_LOGE(TAG, "%s cleanup failed: %s", resource, esp_err_to_name(err));
    return err;
}

xiaotai_err_t xt_spi_bus_close(int host)
{
    return cleanup_result("bus", spi_bus_free(host));
}

xiaotai_err_t xt_spi_io_open(const xiaotai_panel_spi_config_t *config, void **out)
{
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = config->cs_pin, .dc_gpio_num = config->dc_pin,
        .pclk_hz = config->hz, .spi_mode = config->spi_mode,
        .trans_queue_depth = config->queue_depth, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .flags.psram_dma_direct = config->external_dma,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t (*new_io)(esp_lcd_spi_bus_handle_t, const esp_lcd_panel_io_spi_config_t *,
                        esp_lcd_panel_io_handle_t *) = esp_lcd_new_panel_io_spi;
#ifdef XIAOTAI_SPI_NATIVE_CORRECTION
    if (config->external_dma) new_io = xiaotai_lcd_new_panel_io_spi;
#endif
    const esp_err_t err = new_io((esp_lcd_spi_bus_handle_t)config->host, &io_config, &io);
    if (err == ESP_OK) *out = io;
    return err;
}

xiaotai_err_t xt_spi_io_close(void *io)
{
    return cleanup_result("panel IO", esp_lcd_panel_io_del(io));
}

xiaotai_err_t xt_spi_panel_open(const xiaotai_panel_spi_controller_t *controller,
                               const xiaotai_panel_spi_config_t *config, void *io, void *vendor, void **panel)
{
    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1, .bits_per_pixel = 16,
        .rgb_ele_order = config->bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .vendor_config = vendor,
    };
    return controller->create(io, &dev, config, panel);
}

xiaotai_err_t xt_spi_panel_close(void *panel)
{
    return cleanup_result("panel", esp_lcd_panel_del(panel));
}

xiaotai_err_t xt_spi_panel_reset(void *panel)
{
    return esp_lcd_panel_reset(panel);
}

xiaotai_err_t xt_spi_panel_setup(void *panel, const xiaotai_panel_spi_config_t *c)
{
    esp_err_t err = esp_lcd_panel_init(panel);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel, c->invert);
    if (err == ESP_OK) err = esp_lcd_panel_swap_xy(panel, c->swap_xy);
    if (err == ESP_OK) err = esp_lcd_panel_mirror(panel, c->mirror_x, c->mirror_y);
    if (err == ESP_OK) err = esp_lcd_panel_set_gap(panel, c->gap_x, c->gap_y);
    return err;
}

xiaotai_err_t xt_spi_panel_on(void *panel)
{
    return esp_lcd_panel_disp_on_off(panel, true);
}

xiaotai_err_t xt_spi_draw(void *panel, const xiaotai_rect_t *a, const void *pixels)
{
    return esp_lcd_panel_draw_bitmap(panel, a->x, a->y, a->x + a->width, a->y + a->height, pixels);
}

xiaotai_err_t xt_spi_drain(void *io)
{
    /* No command goes onto the wire; native TX-param collects all queued
     * colour transactions with portMAX_DELAY. The display owner must not
     * overlap a native-handle operation with this fence. */
    return esp_lcd_panel_io_tx_param(io, -1, NULL, 0);
}

void xt_spi_handles(void *panel, void *io, xiaotai_display_handles_t *out)
{
    out->panel = panel;
    out->panel_io = io;
}

static xiaotai_err_t create_st7789(void *io, const void *device,
                                  const xiaotai_panel_spi_config_t *config, void **out)
{
    (void)config;
    esp_lcd_panel_handle_t panel = NULL;
    const esp_err_t err = esp_lcd_new_panel_st7789(io, device, &panel);
    if (err == ESP_OK) *out = panel;
    return err;
}

const xiaotai_panel_spi_controller_t *xiaotai_panel_st7789(void)
{
    static const xiaotai_panel_spi_controller_t controller = {.name = "st7789", .create = create_st7789};
    return &controller;
}
