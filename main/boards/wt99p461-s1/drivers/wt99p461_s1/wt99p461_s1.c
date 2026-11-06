/* SPDX-License-Identifier: MIT */
#include "bsp/wt99p461_s1.h"

#include "esp_check.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mipi_dsi_ll.h"
#include "hal/mipi_dsi_brg_ll.h"

static const char *TAG = "wt99p461";
static i2c_master_bus_handle_t s_i2c;
static esp_lcd_dsi_bus_handle_t s_dsi;
static esp_lcd_panel_io_handle_t s_panel_io, s_touch_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_ldo_channel_handle_t s_phy_ldo;
static lv_disp_t *s_display;
static lv_indev_t *s_indev;
static bool s_backlight_ready;
static bool s_lvgl_ready;
static bool s_cleanup_failed;

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c != NULL) return ESP_OK;
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) { return s_i2c; }

static esp_err_t display_power_init(void)
{
    const gpio_config_t gpio = {
        .pin_bit_mask = (1ULL << BSP_LCD_ENABLE) | (1ULL << BSP_LCD_UPDN) |
                        (1ULL << BSP_LCD_SHLR) | (1ULL << BSP_LCD_TOUCH_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio), TAG, "display GPIO init");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_ENABLE, 1), TAG, "LCD power");
    /* Match the supplier's scan straps. The reset transistor inverts GPIO23. */
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_UPDN, 1), TAG, "UPDN");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_SHLR, 1), TAG, "SHLR");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_TOUCH_RST, 1), TAG, "assert shared reset");
    vTaskDelay(pdMS_TO_TICKS(20));
    const esp_ldo_channel_config_t ldo = {.chan_id = 3, .voltage_mv = 2500};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &s_phy_ldo), TAG, "DSI PHY power");
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1, .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    s_backlight_ready = true;
    return ESP_OK;
}

static esp_err_t touch_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "touch I2C");
    esp_lcd_panel_io_i2c_config_t io = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io, &s_touch_io), TAG, "touch IO");
    esp_lcd_touch_io_gt911_config_t addr = {.dev_addr = 0x5d};
    const esp_lcd_touch_config_t cfg = {
        .x_max = BSP_LCD_H_RES, .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST, .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {.reset = 1, .interrupt = 0},
        .driver_data = &addr,
    };
    /* This is the only owner of the shared LCD/GT911 reset. Do it before DSI
     * panel initialization, never reset the touch controller after LCD init. */
    return esp_lcd_touch_new_i2c_gt911(s_touch_io, &cfg, &s_touch);
}

static esp_err_t panel_init(void)
{
    const esp_lcd_dsi_bus_config_t bus = JD9165BA_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus, &s_dsi), TAG, "DSI bus");
    const esp_lcd_dbi_io_config_t io = JD9165BA_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi, &io, &s_panel_io), TAG, "DSI DBI");
    mipi_dsi_host_ll_enable_cmd_ack(MIPI_DSI_LL_GET_HOST(0), false);
    esp_lcd_dpi_panel_config_t dpi = JD9165BA_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB565);
    dpi.num_fbs = 2;
    dpi.out_color_format = LCD_COLOR_FMT_RGB565;
    const jd9165_vendor_config_t vendor = {
        .mipi_config = {.dsi_bus = s_dsi, .dpi_config = &dpi},
    };
    const esp_lcd_panel_dev_config_t cfg = {
        .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16, .vendor_config = (void *)&vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(s_panel_io, &cfg, &s_panel), TAG, "JD9165");
    mipi_dsi_host_ll_dpi_enable_frame_ack(MIPI_DSI_LL_GET_HOST(0), false);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel software reset");
    return esp_lcd_panel_init(s_panel);
}

static esp_err_t display_cleanup(void)
{
    if (s_indev) {
        ESP_RETURN_ON_ERROR(lvgl_port_remove_touch(s_indev), TAG, "remove touch input");
        s_indev = NULL;
    }
    /* The caller holds the LVGL lock when a display exists. Stop DSI callbacks
     * before releasing their LVGL context; panel framebuffers remain panel-owned. */
    if (s_panel) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_del(s_panel), TAG, "delete display panel");
        s_panel = NULL;
    }
    if (s_display) {
        ESP_RETURN_ON_ERROR(lvgl_port_remove_disp(s_display), TAG, "remove LVGL display");
        s_display = NULL;
    }
    if (s_panel_io) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(s_panel_io), TAG, "delete panel IO");
        s_panel_io = NULL;
    }
    if (s_dsi) {
        ESP_RETURN_ON_ERROR(esp_lcd_del_dsi_bus(s_dsi), TAG, "delete DSI bus");
        s_dsi = NULL;
    }
    if (s_touch) {
        ESP_RETURN_ON_ERROR(esp_lcd_touch_del(s_touch), TAG, "delete touch controller");
        s_touch = NULL;
    }
    if (s_touch_io) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(s_touch_io), TAG, "delete touch IO");
        s_touch_io = NULL;
    }
    if (s_phy_ldo) {
        ESP_RETURN_ON_ERROR(esp_ldo_release_channel(s_phy_ldo), TAG, "release DSI power");
        s_phy_ldo = NULL;
    }
    if (s_backlight_ready) {
        ESP_RETURN_ON_ERROR(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0), TAG, "stop backlight");
        s_backlight_ready = false;
    }
    return gpio_set_level(BSP_LCD_ENABLE, 0);
}

lv_disp_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    if (s_cleanup_failed) {
        ESP_LOGE(TAG, "display cleanup incomplete; retained resources prevent reinitialization");
        return NULL;
    }
    if (s_display) return s_display;
    if (cfg == NULL) return NULL;
    bool locked = false;
    esp_err_t ret = display_power_init();
    if (ret == ESP_OK) ret = touch_init();
    if (ret == ESP_OK) ret = panel_init();
    if (ret == ESP_OK && !s_lvgl_ready) {
        ret = lvgl_port_init(&cfg->lvgl_port_cfg);
        if (ret == ESP_OK) s_lvgl_ready = true;
    }
    if (ret != ESP_OK) goto failed;
    locked = lvgl_port_lock(1000);
    if (!locked) { ret = ESP_ERR_TIMEOUT; goto failed; }
    const lvgl_port_display_cfg_t display = {
        .io_handle = s_panel_io, .panel_handle = s_panel,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES, .double_buffer = true,
        .hres = BSP_LCD_H_RES, .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* LVGL's double-buffer direct-mode sync preserves unchanged regions.
         * Keep the shared scanout buffers and VSYNC completion in the port. */
        .flags = {.buff_spiram = true, .direct_mode = true},
    };
    const lvgl_port_display_dsi_cfg_t dsi = {.flags.avoid_tearing = true};
    s_display = lvgl_port_add_disp_dsi(&display, &dsi);
    if (!s_display) { ret = ESP_ERR_NO_MEM; goto failed; }
    const lvgl_port_touch_cfg_t touch = {.disp = s_display, .handle = s_touch};
    s_indev = lvgl_port_add_touch(&touch);
    if (!s_indev) { ret = ESP_FAIL; goto failed; }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "JD9165 1024x600 RGB565, GT911, two shared DSI/LVGL framebuffers");
    return s_display;
failed:
    ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
    s_cleanup_failed = display_cleanup() != ESP_OK;
    if (locked) lvgl_port_unlock();
    return NULL;
}

lv_indev_t *bsp_display_get_input_dev(void) { return s_indev; }
esp_lcd_panel_handle_t bsp_display_get_panel_handle(void) { return s_panel; }
esp_lcd_panel_io_handle_t bsp_display_get_io_handle(void) { return s_panel_io; }

esp_err_t bsp_display_brightness_set(int percent)
{
    ESP_RETURN_ON_FALSE(s_backlight_ready, ESP_ERR_INVALID_STATE, TAG, "backlight not ready");
    ESP_RETURN_ON_FALSE(percent >= 0 && percent <= 100, ESP_ERR_INVALID_ARG, TAG, "brightness range");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1023U * percent / 100U),
                        TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

esp_err_t bsp_display_backlight_on(void) { return bsp_display_brightness_set(100); }
esp_err_t bsp_display_backlight_off(void) { return bsp_display_brightness_set(0); }
