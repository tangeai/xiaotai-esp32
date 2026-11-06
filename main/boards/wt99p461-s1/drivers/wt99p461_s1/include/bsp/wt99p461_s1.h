#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "bsp/display.h"

#define BSP_LCD_H_RES          1024
#define BSP_LCD_V_RES          600
#define BSP_I2C_SDA            GPIO_NUM_48
#define BSP_I2C_SCL            GPIO_NUM_47
#define BSP_I2S_MCLK           GPIO_NUM_8
#define BSP_I2S_BCLK           GPIO_NUM_7
#define BSP_LCD_BACKLIGHT      GPIO_NUM_20
#define BSP_LCD_ENABLE         GPIO_NUM_22
#define BSP_LCD_TOUCH_RST      GPIO_NUM_23
#define BSP_LCD_TOUCH_INT      GPIO_NUM_21
#define BSP_LCD_UPDN           GPIO_NUM_26
#define BSP_LCD_SHLR           GPIO_NUM_27
#define BSP_CAMERA_POWER       GPIO_NUM_0

typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;
    uint32_t buffer_size;
    uint32_t trans_size;
    bool double_buffer;
    struct {
        unsigned buff_dma : 1;
        unsigned buff_spiram : 1;
    } flags;
} bsp_display_cfg_t;

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
lv_disp_t *bsp_display_start_with_config(const bsp_display_cfg_t *config);
lv_indev_t *bsp_display_get_input_dev(void);
esp_lcd_panel_handle_t bsp_display_get_panel_handle(void);
esp_lcd_panel_io_handle_t bsp_display_get_io_handle(void);
