#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_TIMEOUT 3
#define ESP_FAIL 4
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
typedef esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t, void *, void *);
typedef struct { esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done; } esp_lcd_panel_io_callbacks_t;
struct esp_lcd_panel_io_t {
    esp_err_t (*del)(esp_lcd_panel_io_t *);
    esp_err_t (*rx_param)(esp_lcd_panel_io_t *, int, void *, size_t);
    esp_err_t (*tx_param)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*tx_color)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*register_event_callbacks)(esp_lcd_panel_io_handle_t, const esp_lcd_panel_io_callbacks_t *, void *);
};
typedef struct {
    uint32_t dev_addr, scl_speed_hz;
    unsigned control_phase_bytes, dc_bit_offset;
    int lcd_cmd_bits, lcd_param_bits;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    struct { unsigned disable_control_phase:1, dc_low_on_data:1; } flags;
} esp_lcd_panel_io_i2c_config_t;
typedef struct { uint32_t device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { uint8_t *write_buffer; size_t buffer_size; } i2c_master_transmit_multi_buffer_info_t;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t, uint8_t *, size_t, int);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t, uint8_t *, size_t, int);
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t, i2c_master_transmit_multi_buffer_info_t *, size_t, int);
#define __containerof(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_GOTO_ON_FALSE(c, e, l, ...) do { if (!(c)) { ret = (e); goto l; } } while (0)
#define ESP_GOTO_ON_ERROR(e, l, ...) do { ret = (e); if (ret != ESP_OK) goto l; } while (0)
#define ESP_RETURN_ON_ERROR(e, ...) do { esp_err_t r_ = (e); if (r_ != ESP_OK) return r_; } while (0)
#define ESP_COMPILER_DIAGNOSTIC_PUSH_IGNORE(x)
#define ESP_COMPILER_DIAGNOSTIC_POP(x)
