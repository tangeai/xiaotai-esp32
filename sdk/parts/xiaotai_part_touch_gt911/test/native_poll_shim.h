// SPDX-License-Identifier: MIT
#pragma once

#include "touch_io.h"

typedef int esp_err_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t, *esp_lcd_panel_io_handle_t;
enum { ESP_OK = XIAOTAI_OK, ESP_ERR_INVALID_ARG = XIAOTAI_ERR_INVALID_ARG };
enum { I2C_ADDR_BIT_LEN_7 = 0 };
enum {
    ESP_LCD_TOUCH_GT911_READ_XY_REG = 0x814e,
    ESP_LCD_TOUCH_GT911_READ_KEY_REG = 0x8093,
    ESP_GT911_TOUCH_MAX_BUTTONS = 4,
    ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS = 0x5d,
    ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP = 0x14,
};
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#define CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS 1
#define ESP_RETURN_ON_FALSE(c, e, ...) do { if (!(c)) return (e); } while (0)
#define ESP_RETURN_ON_ERROR(e, ...) do { const esp_err_t status_ = (e); if (status_ != ESP_OK) return status_; } while (0)
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 64)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) GPIO_IS_VALID_GPIO(pin)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))

typedef struct { uint32_t dev_addr; } esp_lcd_touch_io_gt911_config_t;
typedef struct {
    uint32_t dev_addr, scl_speed_hz;
} esp_lcd_panel_io_i2c_config_t;
#define ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() {.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, .scl_speed_hz = 100000}
typedef struct {
    int dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;
typedef struct {
    const uint8_t *write_buffer;
    size_t buffer_size;
} i2c_master_transmit_multi_buffer_info_t;
struct esp_lcd_panel_io_t {
    esp_err_t (*rx_param)(esp_lcd_panel_io_t *, int, void *, size_t);
    esp_err_t (*tx_param)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*tx_color)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*del)(esp_lcd_panel_io_t *);
};
typedef struct {
    uint16_t x_max, y_max;
    int rst_gpio_num, int_gpio_num;
    struct { bool reset, interrupt; } levels;
    struct { bool swap_xy, mirror_x, mirror_y; } flags;
    void *driver_data;
} esp_lcd_touch_config_t;
typedef struct {
    uint16_t x, y, strength;
    uint8_t track_id;
} esp_lcd_touch_point_data_t;
typedef struct gt911_fixture {
    esp_lcd_panel_io_handle_t io;
    struct {
        int lock;
        uint8_t points, buttons;
        struct { int status; } button[CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS];
        esp_lcd_touch_point_data_t coords[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    } data;
} *esp_lcd_touch_handle_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *,
                                   i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *address, size_t address_size,
                                     uint8_t *data, size_t size, int timeout_ms);
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t, i2c_master_transmit_multi_buffer_info_t *,
                                          size_t buffer_count, int timeout_ms);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int reg, const void *data, size_t size);
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t, int reg, void *data, size_t size);
esp_err_t esp_lcd_touch_new_i2c_gt911(esp_lcd_panel_io_handle_t, const esp_lcd_touch_config_t *, esp_lcd_touch_handle_t *);
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t, esp_lcd_touch_point_data_t *, uint8_t *count, uint8_t limit);
esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t);
