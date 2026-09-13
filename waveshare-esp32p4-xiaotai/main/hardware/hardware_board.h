#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_types.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#include "hardware_board_config.h"

typedef struct {
	i2c_port_num_t port;
	gpio_num_t sda_gpio;
	gpio_num_t scl_gpio;
	uint32_t freq_hz;
} hardware_i2c_config_t;

typedef struct {
	spi_host_device_t spi_host;
	gpio_num_t spi_mosi_gpio;
	gpio_num_t spi_clk_gpio;
	gpio_num_t spi_cs_gpio;
	gpio_num_t dc_gpio;
	gpio_num_t reset_gpio;
	gpio_num_t backlight_gpio;
	ledc_timer_t backlight_ledc_timer;
	ledc_channel_t backlight_ledc_channel;
	uint32_t pixel_clock_hz;
	uint16_t width;
	uint16_t height;
	uint16_t draw_buffer_height;
	uint8_t cmd_bits;
	uint8_t param_bits;
	uint8_t bits_per_pixel;
} hardware_display_config_t;

typedef enum {
	HARDWARE_AUDIO_CODEC_NONE = 0,
	HARDWARE_AUDIO_CODEC_ES8311,
	HARDWARE_AUDIO_CODEC_ES7210,
} hardware_audio_codec_t;

typedef struct {
	i2s_port_t i2s_port;
	gpio_num_t lrck_gpio;
	gpio_num_t mclk_gpio;
	gpio_num_t bclk_gpio;
	gpio_num_t din_gpio;
	gpio_num_t dout_gpio;
	gpio_num_t pa_gpio;
	uint32_t sample_rate_hz;
	uint8_t bits_per_sample;
	uint8_t channels;
	uint8_t adc_channels;
	uint8_t default_volume_percent;
	float default_adc_gain_db;
	hardware_audio_codec_t speaker_codec;
	hardware_audio_codec_t microphone_codec;
	uint8_t speaker_codec_i2c_addr;
	uint8_t microphone_codec_i2c_addr;
	uint8_t microphone_select_mask;
} hardware_audio_config_t;

typedef struct {
	bool enabled;
	gpio_num_t pwdn_gpio;
	gpio_num_t reset_gpio;
	gpio_num_t xclk_gpio;
	gpio_num_t sccb_sda_gpio;
	gpio_num_t sccb_scl_gpio;
	gpio_num_t data_gpio[8];
	gpio_num_t vsync_gpio;
	gpio_num_t href_gpio;
	gpio_num_t pclk_gpio;
	ledc_timer_t xclk_ledc_timer;
	ledc_channel_t xclk_ledc_channel;
	uint32_t xclk_freq_hz;
	uint8_t frame_buffer_count;
	bool hmirror;
	bool vflip;
} hardware_camera_config_t;

typedef struct {
	const char *type;
	hardware_i2c_config_t i2c;
	hardware_display_config_t display;
	hardware_audio_config_t audio;
	hardware_camera_config_t camera;
} hardware_board_t;

const hardware_board_t *hardware_board_get(void);
const hardware_i2c_config_t *hardware_board_get_i2c_config(void);
const hardware_display_config_t *hardware_board_get_display_config(void);
const hardware_audio_config_t *hardware_board_get_audio_config(void);
const hardware_camera_config_t *hardware_board_get_camera_config(void);

esp_err_t hardware_board_init(void);
esp_err_t hardware_board_init_i2c(void);
esp_err_t hardware_board_init_io_expander(void);
i2c_master_bus_handle_t hardware_board_get_i2c_bus_handle(void);
esp_err_t hardware_board_set_lcd_chip_select(bool active);
esp_err_t hardware_board_set_audio_power(bool enable);
esp_err_t hardware_board_set_camera_power(bool enable);
