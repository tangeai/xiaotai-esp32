// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "hal/mipi_dsi_ll.h"

static xiaotai_err_t phy_open(const xiaotai_panel_jd9165_config_t *c, void **out)
{
    const esp_ldo_channel_config_t config = {.chan_id = c->phy_ldo_channel, .voltage_mv = c->phy_ldo_mv};
    esp_ldo_channel_handle_t phy = NULL;
    const esp_err_t err = esp_ldo_acquire_channel(&config, &phy);
    if (err == ESP_OK) *out = phy;
    return err;
}

static xiaotai_err_t phy_close(void *phy) { return esp_ldo_release_channel(phy); }

static xiaotai_err_t bus_open(const xiaotai_panel_jd9165_config_t *c, void **out)
{
    /* The unchanged supplier command table programs two display lanes. */
    if (c->lanes != 2) return XIAOTAI_ERR_NOT_SUPPORTED;
    esp_lcd_dsi_bus_config_t config = JD9165BA_PANEL_BUS_DSI_2CH_CONFIG();
    config.num_data_lanes = c->lanes;
    config.lane_bit_rate_mbps = (float)c->lane_mbps;
    esp_lcd_dsi_bus_handle_t bus = NULL;
    const esp_err_t err = esp_lcd_new_dsi_bus(&config, &bus);
    if (err == ESP_OK) *out = bus;
    return err;
}

static xiaotai_err_t bus_close(void *bus) { return esp_lcd_del_dsi_bus(bus); }

static xiaotai_err_t io_open(void *bus, void **out)
{
    const esp_lcd_dbi_io_config_t config = JD9165BA_PANEL_IO_DBI_CONFIG();
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_err_t err = esp_lcd_new_panel_io_dbi(bus, &config, &io);
    if (err == ESP_OK) {
        mipi_dsi_host_ll_enable_cmd_ack(MIPI_DSI_LL_GET_HOST(0), false);
        *out = io;
    }
    return err;
}

static xiaotai_err_t io_close(void *io) { return esp_lcd_panel_io_del(io); }

static xiaotai_err_t panel_open(const xiaotai_panel_jd9165_config_t *c, void *bus, void *io, void **out)
{
    const esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = (float)c->pixel_clock_hz / 1000000.0f,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565, .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = c->buffers,
        .video_timing = {
            .h_size = c->width, .v_size = c->height,
            .hsync_back_porch = c->hsync_back_porch, .hsync_pulse_width = c->hsync_pulse,
            .hsync_front_porch = c->hsync_front_porch,
            .vsync_back_porch = c->vsync_back_porch, .vsync_pulse_width = c->vsync_pulse,
            .vsync_front_porch = c->vsync_front_porch,
        },
    };
    const jd9165_vendor_config_t vendor = {.mipi_config = {.dsi_bus = bus, .dpi_config = &dpi}};
    const esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16, .vendor_config = (void *)&vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    const esp_err_t err = esp_lcd_new_panel_jd9165(io, &config, &panel);
    if (err == ESP_OK) {
        mipi_dsi_host_ll_dpi_enable_frame_ack(MIPI_DSI_LL_GET_HOST(0), false);
        *out = panel;
    }
    return err;
}

static xiaotai_err_t panel_close(void *panel) { return esp_lcd_panel_del(panel); }

static xiaotai_err_t panel_enable(void *panel, bool soft_reset)
{
    const esp_err_t err = soft_reset ? esp_lcd_panel_reset(panel) : ESP_OK;
    /* The supplier init turns the output on; IDF allocates black PSRAM frames. */
    return err == ESP_OK ? esp_lcd_panel_init(panel) : err;
}

static xiaotai_err_t framebuffers(void *panel, uint8_t buffers, void *out[2])
{
    return buffers == 2 ? esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &out[0], &out[1])
                        : esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &out[0]);
}

static xiaotai_err_t present(void *panel, const xiaotai_rect_t *area, const void *pixels)
{
    /* No DMA2D: IDF copies foreign pixels synchronously; its own buffers are
     * switched directly. The renderer owns its completion/VSYNC handling. */
    return esp_lcd_panel_draw_bitmap(panel, area->x, area->y, area->x + area->width, area->y + area->height, pixels);
}

static void native_handles(void *panel, void *io, xiaotai_display_handles_t *out)
{
    out->panel = panel;
    out->panel_io = io;
}

const jd9165_io_t xiaotai_jd9165_io = {
    .phy_open = phy_open, .phy_close = phy_close, .bus_open = bus_open, .bus_close = bus_close,
    .io_open = io_open, .io_close = io_close, .panel_open = panel_open, .panel_close = panel_close,
    .panel_enable = panel_enable, .framebuffers = framebuffers, .present = present,
    .native_handles = native_handles,
};
