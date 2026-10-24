// SPDX-License-Identifier: MIT
#include "xmini_board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"

static const char *TAG = "xmini_board";
static i2c_master_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

esp_err_t xmini_board_init(void)
{
    esp_err_t ret;
    if (s_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << XMINI_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa), TAG, "PA GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(XMINI_PA_GPIO, 0), TAG, "PA off");
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << XMINI_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button), TAG, "BOOT GPIO");
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = XMINI_I2C_SDA,
        .scl_io_num = XMINI_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), TAG, "I2C bus");
    ESP_GOTO_ON_ERROR(i2c_master_probe(s_bus, 0x18, 100), fail, TAG, "ES8311 absent at 0x18");
    ESP_GOTO_ON_ERROR(i2c_master_probe(s_bus, 0x3c, 100), fail, TAG, "OLED absent at 0x3c");
    esp_lcd_panel_io_i2c_config_t io = {
        .dev_addr = 0x3c,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .transaction_timeout_ms = 100,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io, &s_io), fail, TAG, "OLED IO");
    esp_lcd_panel_ssd1306_config_t vendor = {.height = XMINI_OLED_HEIGHT};
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
        .vendor_config = &vendor,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &panel, &s_panel), fail, TAG, "OLED panel");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel), fail, TAG, "OLED reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_panel), fail, TAG, "OLED init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), fail, TAG, "OLED orientation");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(s_panel, false), fail, TAG, "OLED polarity");
    // Clear panel RAM before enabling it, avoiding an uninitialized first frame.
    uint8_t blank[XMINI_FRAMEBUFFER_BYTES] = {0};
    ESP_GOTO_ON_ERROR(xmini_board_display(blank), fail, TAG, "OLED clear");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), fail, TAG, "OLED on");
    ESP_LOGI(TAG, "Xmini-C3-V3: SSD1306 128x64, ES8311, BOOT GPIO9");
    return ESP_OK;

fail:
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    return ret;
}

i2c_master_bus_handle_t xmini_board_i2c(void)
{
    return s_bus;
}

bool xmini_board_button_down(void)
{
    return gpio_get_level(XMINI_BOOT_GPIO) == 0;
}

esp_err_t xmini_board_display(const uint8_t frame[XMINI_FRAMEBUFFER_BYTES])
{
    if (!s_panel || !frame) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, XMINI_OLED_WIDTH,
                                     XMINI_OLED_HEIGHT, frame);
}
