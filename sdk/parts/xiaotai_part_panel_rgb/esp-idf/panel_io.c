// SPDX-License-Identifier: MIT
#include "panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "soc/soc_caps.h"

#if SOC_AXI_GDMA_SUPPORTED
#include "esp_system.h"
#include "hal/axi_dma_ll.h"
#include "hal/gdma_ll.h"

/* A software restart resets only the CPUs, while the scan-out DMA is in the
 * middle of AXI reads from PSRAM, and an AXI transfer cannot be cut short.
 * Where the restart path does not stop AXI-DMA first (ESP32-S31 on IDF 6.1),
 * the LCD and its DMA came back without any interrupt. Stop every channel
 * before the restart, as the ESP32-P4 restart path does. */
static void stop_axi_dma(void)
{
    for (uint32_t i = 0; i < GDMA_LL_AXI_PAIRS_PER_GROUP; ++i) {
        axi_dma_ll_tx_abort(AXI_DMA_LL_GET_HW(0), i, true);
        axi_dma_ll_rx_abort(AXI_DMA_LL_GET_HW(0), i, true);
        while (!axi_dma_ll_tx_is_reset_avail(AXI_DMA_LL_GET_HW(0), i)) {
        }
        while (!axi_dma_ll_rx_is_reset_avail(AXI_DMA_LL_GET_HW(0), i)) {
        }
    }
}

static unsigned open_panels;
#endif

static xiaotai_err_t panel_open(const xiaotai_panel_rgb_config_t *c, void **out)
{
    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = c->pixel_clock_hz,
            .h_res = c->width, .v_res = c->height,
            .hsync_pulse_width = c->hsync_pulse, .hsync_back_porch = c->hsync_back_porch,
            .hsync_front_porch = c->hsync_front_porch,
            .vsync_pulse_width = c->vsync_pulse, .vsync_back_porch = c->vsync_back_porch,
            .vsync_front_porch = c->vsync_front_porch,
            .flags = {
                .hsync_idle_low = c->hsync_idle_low, .vsync_idle_low = c->vsync_idle_low,
                .de_idle_high = c->de_idle_high, .pclk_active_neg = c->pclk_active_falling,
                .pclk_idle_high = c->pclk_idle_high,
            },
        },
        .data_width = c->data_lines,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = c->buffers,
        .dma_burst_size = c->dma_burst_bytes,
        .hsync_gpio_num = c->hsync_pin, .vsync_gpio_num = c->vsync_pin,
        .de_gpio_num = c->de_pin, .pclk_gpio_num = c->pclk_pin, .disp_gpio_num = c->disp_pin,
        /* Frame buffers live in external RAM where the board has it. */
        .flags = {.disp_active_low = c->disp_active_low, .fb_in_psram = 1},
    };
    for (size_t i = 0; i < c->data_lines; ++i) config.data_gpio_nums[i] = c->data_pins[i];
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = esp_lcd_new_rgb_panel(&config, &panel);
#if SOC_AXI_GDMA_SUPPORTED
    if (err == ESP_OK && !open_panels) {
        err = esp_register_shutdown_handler(stop_axi_dma);
        if (err != ESP_OK) (void)esp_lcd_panel_del(panel);
    }
    if (err == ESP_OK) ++open_panels;
#endif
    if (err == ESP_OK) *out = panel;
    return err;
}

static xiaotai_err_t panel_close(void *panel)
{
    const esp_err_t err = esp_lcd_panel_del(panel);
#if SOC_AXI_GDMA_SUPPORTED
    if (err == ESP_OK && open_panels && !--open_panels) (void)esp_unregister_shutdown_handler(stop_axi_dma);
#endif
    return err;
}

static xiaotai_err_t panel_enable(void *panel)
{
    /* IDF allocates zeroed (black) frames; init starts the scan-out. */
    const esp_err_t err = esp_lcd_panel_reset(panel);
    return err == ESP_OK ? esp_lcd_panel_init(panel) : err;
}

static xiaotai_err_t framebuffers(void *panel, uint8_t buffers, void *out[2])
{
    return buffers == 2 ? esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &out[0], &out[1])
                        : esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &out[0]);
}

static xiaotai_err_t present(void *panel, const xiaotai_rect_t *area, const void *pixels)
{
    /* One of the panel's own buffers is switched to at the next frame; any
     * other source is copied. The renderer waits for the frame-buffer event. */
    return esp_lcd_panel_draw_bitmap(panel, area->x, area->y, area->x + area->width, area->y + area->height, pixels);
}

static void native_handles(void *panel, xiaotai_display_handles_t *out) { out->panel = panel; }

const rgb_io_t xiaotai_rgb_io = {
    .panel_open = panel_open, .panel_close = panel_close, .panel_enable = panel_enable,
    .framebuffers = framebuffers, .present = present, .native_handles = native_handles,
};
