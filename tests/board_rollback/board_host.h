#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define ESP_CODEC_DEV_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define BIT(x) (1U << (x))
#define GPIO_NUM_NC -1
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_45 45
#define I2C_NUM_0 0
#define I2C_ADDR_BIT_LEN_7 7
#define I2C_CLK_SRC_DEFAULT 0
#define I2S_GPIO_UNUSED -1
#define I2S_AUDIO_PORT 0
#define I2S_ROLE_MASTER 1
#define I2S_MCLK_MULTIPLE_256 256
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_TDM_SLOT0 1
#define I2S_TDM_SLOT1 2
#define I2S_TDM_SLOT2 4
#define I2S_TDM_SLOT3 8
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#define ES7210_CODEC_DEFAULT_ADDR 0x80
#define ES7210_SEL_MIC1 1
#define ES7210_SEL_MIC2 2
#define ES7210_SEL_MIC3 4
#define ES7210_SEL_MIC4 8
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(x) BIT(x)
#define ESP_CODEC_DEV_TYPE_OUT 1
#define ESP_CODEC_DEV_TYPE_IN 2
#define ESP_CODEC_DEV_WORK_MODE_DAC 1
#define SPI3_HOST 3
#define SPI_DMA_CH_AUTO 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define LCD_RGB_ELEMENT_ORDER_RGB 0
#define I2S_CHANNEL_DEFAULT_CONFIG(a,b) { .port = (a) }
#define I2S_STD_CLK_DEFAULT_CONFIG(a) { .sample_rate = (a) }
#define I2S_TDM_CLK_DEFAULT_CONFIG(a) { .sample_rate = (a) }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(a,b) { .channel = (b) }
#define I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(a,b,c) { .channel = (b) }
#define ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG() {0}
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef void *i2s_chan_handle_t;
typedef void *esp_codec_dev_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_touch_handle_t;
typedef intptr_t esp_lcd_spi_bus_handle_t;
typedef void *SemaphoreHandle_t;
typedef int StaticSemaphore_t;
typedef struct { int unused; } audio_codec_data_if_t;
typedef audio_codec_data_if_t audio_codec_ctrl_if_t;
typedef audio_codec_data_if_t audio_codec_gpio_if_t;
typedef audio_codec_data_if_t audio_codec_if_t;
typedef struct {
    int port, dev_addr_length, device_address, scl_speed_hz, i2c_port, sda_io_num, scl_io_num;
    int clk_source, glitch_ignore_cnt, auto_clear, mclk_multiple, sample_rate, channel;
    int mclk, bclk, ws, dout, din, addr, clock_speed_hz, codec_mode, pa_pin, pa_reverted;
    int master_mode, use_mclk, digital_mic, invert_mclk, invert_sclk, mclk_div, mic_selected;
    int dev_type, bits_per_sample, channel_mask, mode, pull_up_en, pull_down_en, intr_type;
    int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz;
    int dc_gpio_num, cs_gpio_num, pclk_hz, lcd_cmd_bits, lcd_param_bits, spi_mode, trans_queue_depth;
    int reset_gpio_num, rgb_ele_order, bits_per_pixel, x_max, y_max, rst_gpio_num, int_gpio_num;
    uint64_t pin_bit_mask;
    const void *rx_handle, *tx_handle, *bus_handle, *ctrl_if, *gpio_if, *codec_if, *data_if;
    struct { bool enable_internal_pullup, swap_xy, mirror_x, mirror_y; } flags;
    struct { bool reset, interrupt; } levels;
    struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags;
} host_config;
typedef host_config i2c_device_config_t;
typedef host_config i2c_master_bus_config_t;
typedef host_config i2s_chan_config_t;
typedef struct { host_config clk_cfg, slot_cfg, gpio_cfg; } i2s_std_config_t;
typedef i2s_std_config_t i2s_tdm_config_t;
typedef host_config audio_codec_i2s_cfg_t;
typedef host_config audio_codec_i2c_cfg_t;
typedef host_config es8311_codec_cfg_t;
typedef host_config es7210_codec_cfg_t;
typedef host_config esp_codec_dev_cfg_t;
typedef host_config esp_codec_dev_sample_info_t;
typedef host_config gpio_config_t;
typedef host_config spi_bus_config_t;
typedef host_config esp_lcd_panel_io_spi_config_t;
typedef host_config esp_lcd_panel_dev_config_t;
typedef host_config esp_lcd_panel_io_i2c_config_t;
typedef host_config esp_lcd_touch_config_t;
static int steps, fail_step, live, cleanup_fail, spi_live;
static int step(void) { return ++steps == fail_step ? ESP_FAIL : ESP_OK; }
static void *make_resource(void) { if (step()) return NULL; ++live; return calloc(1, 1); }
static int create_resource(void **out) { void *p = make_resource(); if (!p) return ESP_FAIL; *out = p; return ESP_OK; }
static int remove_resource(const void *p) {
    assert(p && live > 0);
    if (cleanup_fail) { cleanup_fail = 0; return ESP_FAIL; }
    --live; free((void *)p); return ESP_OK;
}
static int new_channels(void **tx, void **rx) {
    if (step()) return ESP_FAIL;
    *tx = calloc(1, 1); *rx = calloc(1, 1); live += 2; return ESP_OK;
}
static int spi_create(void) { if (step()) return ESP_FAIL; assert(!spi_live); spi_live = 1; return ESP_OK; }
static int spi_remove(void) { assert(spi_live); spi_live = 0; return ESP_OK; }
#define xSemaphoreCreateMutexStatic(s) ((void)(s), make_resource())
#define vSemaphoreDelete(s) ((void)remove_resource(s))
#define xSemaphoreTake(s,t) ((void)(s), (void)(t), pdTRUE)
#define xSemaphoreGive(s) ((void)(s))
#define i2c_new_master_bus(c,o) ((void)(c), create_resource(o))
#define i2c_master_bus_add_device(b,c,o) ((void)(b), (void)(c), create_resource(o))
#define i2c_master_transmit(d,b,n,t) ((void)(d), (void)(b), (void)(n), (void)(t), step())
#define i2c_master_bus_rm_device(p) remove_resource(p)
#define i2c_del_master_bus(p) remove_resource(p)
#define i2s_new_channel(c,t,r) ((void)(c), new_channels(t,r))
#define i2s_channel_init_std_mode(h,c) ((void)(h), (void)(c), step())
#define i2s_channel_init_tdm_mode(h,c) ((void)(h), (void)(c), step())
#define i2s_del_channel(p) remove_resource(p)
#define audio_codec_new_i2s_data(c) ((void)(c), make_resource())
#define audio_codec_new_i2c_ctrl(c) ((void)(c), make_resource())
#define audio_codec_new_gpio() make_resource()
#define es8311_codec_new(c) ((void)(c), make_resource())
#define es7210_codec_new(c) ((void)(c), make_resource())
#define esp_codec_dev_new(c) ((void)(c), make_resource())
#define esp_codec_dev_open(h,c) ((void)(h), (void)(c), step())
#define esp_codec_dev_set_out_vol(h,v) ((void)(h), (void)(v), step())
#define esp_codec_dev_set_out_mute(h,v) ((void)(h), (void)(v), step())
#define esp_codec_dev_set_in_channel_gain(h,c,g) ((void)(h), (void)(c), (void)(g), step())
#define esp_codec_dev_close(h) ((void)(h), ESP_OK)
#define esp_codec_dev_delete(h) ((void)remove_resource(h))
#define audio_codec_delete_codec_if(h) remove_resource(h)
#define audio_codec_delete_ctrl_if(h) remove_resource(h)
#define audio_codec_delete_gpio_if(h) remove_resource(h)
#define audio_codec_delete_data_if(h) remove_resource(h)
#define gpio_config(c) ((void)(c), step())
#define gpio_set_level(p,v) ((void)(p), (void)(v), step())
#define spi_bus_initialize(h,c,d) ((void)(h), (void)(c), (void)(d), spi_create())
#define spi_bus_free(h) ((void)(h), spi_remove())
#define esp_lcd_new_panel_io_spi(b,c,o) ((void)(b), (void)(c), create_resource(o))
#define esp_lcd_new_panel_st7789(b,c,o) ((void)(b), (void)(c), create_resource(o))
#define esp_lcd_new_panel_io_i2c(b,c,o) ((void)(b), (void)(c), create_resource(o))
#define esp_lcd_touch_new_i2c_ft5x06(b,c,o) ((void)(b), (void)(c), create_resource(o))
#define esp_lcd_panel_reset(h) ((void)(h), step())
#define esp_lcd_panel_init(h) ((void)(h), step())
#define esp_lcd_panel_invert_color(h,v) ((void)(h), (void)(v), step())
#define esp_lcd_panel_swap_xy(h,v) ((void)(h), (void)(v), step())
#define esp_lcd_panel_mirror(h,x,y) ((void)(h), (void)(x), (void)(y), step())
#define esp_lcd_panel_disp_on_off(h,v) ((void)(h), (void)(v), step())
#define esp_lcd_touch_del(h) remove_resource(h)
#define esp_lcd_panel_io_del(h) remove_resource(h)
#define esp_lcd_panel_del(h) remove_resource(h)
static const char *esp_err_to_name(int error) { (void)error; return "injected"; }
static void host_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr,...) do { int r_ = (expr); if (r_) return r_; } while (0)
