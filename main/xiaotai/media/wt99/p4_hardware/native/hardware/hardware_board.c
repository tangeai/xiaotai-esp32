#include "hardware_board.h"

#include "bsp/wt99p461_s1.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hardware";

static const hardware_board_t s_board = {
	.type = HARDWARE_BOARD_TYPE,
	.i2c = {
		.port = HARDWARE_BOARD_I2C_NUM,
		.sda_gpio = HARDWARE_BOARD_I2C_SDA,
		.scl_gpio = HARDWARE_BOARD_I2C_SCL,
		.freq_hz = HARDWARE_BOARD_I2C_FREQ_HZ,
	},
	.display = {
		.spi_host = SPI2_HOST,
		.spi_mosi_gpio = GPIO_NUM_NC,
		.spi_clk_gpio = GPIO_NUM_NC,
		.spi_cs_gpio = GPIO_NUM_NC,
		.dc_gpio = GPIO_NUM_NC,
		.reset_gpio = BSP_LCD_TOUCH_RST,
		.backlight_gpio = BSP_LCD_BACKLIGHT,
		.backlight_ledc_timer = LEDC_TIMER_0,
		.backlight_ledc_channel = LEDC_CHANNEL_0,
		.pixel_clock_hz = 0,
		.width = HARDWARE_BOARD_LCD_WIDTH,
		.height = HARDWARE_BOARD_LCD_HEIGHT,
		.draw_buffer_height = 40,
		.cmd_bits = 0,
		.param_bits = 0,
		.bits_per_pixel = 16,
	},
	.audio = {
		.i2s_port = HARDWARE_BOARD_AUDIO_I2S_PORT,
		.lrck_gpio = HARDWARE_BOARD_AUDIO_LRCK,
		.mclk_gpio = HARDWARE_BOARD_AUDIO_MCLK,
		.bclk_gpio = HARDWARE_BOARD_AUDIO_BCLK,
		.din_gpio = HARDWARE_BOARD_AUDIO_DIN,
		.dout_gpio = HARDWARE_BOARD_AUDIO_DOUT,
		.pa_gpio = HARDWARE_BOARD_AUDIO_PA_GPIO,
		.sample_rate_hz = HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ,
		.bits_per_sample = HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
		.channels = HARDWARE_BOARD_AUDIO_CHANNELS,
		.adc_channels = HARDWARE_BOARD_AUDIO_ADC_CHANNELS,
		.default_volume_percent = HARDWARE_BOARD_AUDIO_DEFAULT_VOLUME,
		.default_adc_gain_db = HARDWARE_BOARD_AUDIO_DEFAULT_ADC_GAIN_DB,
		.speaker_codec = HARDWARE_AUDIO_CODEC_ES8311,
		.microphone_codec = HARDWARE_AUDIO_CODEC_ES8311,
		.speaker_codec_i2c_addr = ES8311_CODEC_DEFAULT_ADDR,
		.microphone_codec_i2c_addr = ES8311_CODEC_DEFAULT_ADDR,
		.microphone_select_mask = 0,
	},
	.camera = {
		.enabled = HARDWARE_BOARD_CAMERA_ENABLED != 0,
		.pwdn_gpio = GPIO_NUM_NC,
		.reset_gpio = GPIO_NUM_NC,
		.xclk_gpio = GPIO_NUM_NC,
		.sccb_sda_gpio = HARDWARE_BOARD_I2C_SDA,
		.sccb_scl_gpio = HARDWARE_BOARD_I2C_SCL,
		.data_gpio = {
			GPIO_NUM_NC,
			GPIO_NUM_NC,
			GPIO_NUM_NC,
			GPIO_NUM_NC,
			GPIO_NUM_NC,
			GPIO_NUM_NC,
			GPIO_NUM_NC,
			GPIO_NUM_NC,
		},
		.vsync_gpio = GPIO_NUM_NC,
		.href_gpio = GPIO_NUM_NC,
		.pclk_gpio = GPIO_NUM_NC,
		.xclk_ledc_timer = LEDC_TIMER_0,
		.xclk_ledc_channel = LEDC_CHANNEL_0,
		.xclk_freq_hz = 0,
		.frame_buffer_count = HARDWARE_BOARD_CAMERA_BUFFER_COUNT,
		.hmirror = false,
		.vflip = false,
	},
};

static bool s_i2c_initialized;
static bool s_audio_power_initialized;
static bool s_camera_power_initialized;
static bool s_camera_power_enabled;
static i2c_master_bus_handle_t s_i2c_bus_handle;

const hardware_board_t *hardware_board_get(void)
{
	return &s_board;
}

const hardware_i2c_config_t *hardware_board_get_i2c_config(void)
{
	return &s_board.i2c;
}

const hardware_display_config_t *hardware_board_get_display_config(void)
{
	return &s_board.display;
}

const hardware_audio_config_t *hardware_board_get_audio_config(void)
{
	return &s_board.audio;
}

const hardware_camera_config_t *hardware_board_get_camera_config(void)
{
	return &s_board.camera;
}

esp_err_t hardware_board_init_i2c(void)
{
	if (s_i2c_initialized) {
		return ESP_OK;
	}

	ESP_RETURN_ON_ERROR(hardware_board_set_camera_power(true), TAG, "camera control-bus power failed");
	ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp i2c init failed");
	s_i2c_bus_handle = bsp_i2c_get_handle();
	ESP_RETURN_ON_FALSE(s_i2c_bus_handle != NULL, ESP_FAIL, TAG, "bsp i2c handle missing");
	s_i2c_initialized = true;
	return ESP_OK;
}

i2c_master_bus_handle_t hardware_board_get_i2c_bus_handle(void)
{
	return s_i2c_bus_handle;
}

esp_err_t hardware_board_log_i2c_diagnostics(void)
{
	ESP_RETURN_ON_FALSE(s_i2c_bus_handle != NULL, ESP_ERR_INVALID_STATE,
	                    TAG, "I2C diagnostics requested before bus initialization");
	ESP_LOGI(TAG, "board state: camera_power_requested=%u touch_int=%d",
	         (unsigned)s_camera_power_enabled, gpio_get_level(BSP_LCD_TOUCH_INT));
	const uint8_t addresses[] = {0x18, 0x30, 0x36, 0x14, 0x5d, 0x38};
	for (size_t i = 0; i < sizeof(addresses); ++i) {
		const esp_err_t ret = i2c_master_probe(s_i2c_bus_handle, addresses[i], 50);
		ESP_LOGI(TAG, "I2C probe: addr=0x%02x result=%s", addresses[i], esp_err_to_name(ret));
	}
	/* This reports observations, not a pass verdict for the attached devices. */
	return ESP_OK;
}

esp_err_t hardware_board_init_io_expander(void)
{
	return ESP_OK;
}

esp_err_t hardware_board_set_lcd_chip_select(bool active)
{
	(void)active;
	return ESP_OK;
}

esp_err_t hardware_board_set_audio_power(bool enable)
{
	if (!s_audio_power_initialized) {
		gpio_config_t gpio_cfg = {
			.pin_bit_mask = 1ULL << HARDWARE_BOARD_AUDIO_PA_GPIO,
			.mode = GPIO_MODE_OUTPUT,
			.pull_up_en = GPIO_PULLUP_DISABLE,
			.pull_down_en = GPIO_PULLDOWN_DISABLE,
			.intr_type = GPIO_INTR_DISABLE,
		};
		ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG, "audio pa gpio init failed");
		s_audio_power_initialized = true;
	}
	return gpio_set_level(HARDWARE_BOARD_AUDIO_PA_GPIO, enable ? 1 : 0);
}

esp_err_t hardware_board_set_camera_power(bool enable)
{
	if (!s_camera_power_initialized) {
		const gpio_config_t cfg = {
			.pin_bit_mask = 1ULL << HARDWARE_BOARD_CAMERA_POWER_GPIO,
			.mode = GPIO_MODE_OUTPUT,
		};
		ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "camera power GPIO");
		s_camera_power_initialized = true;
	}
	if (s_camera_power_enabled == enable) return ESP_OK;
	ESP_RETURN_ON_ERROR(gpio_set_level(HARDWARE_BOARD_CAMERA_POWER_GPIO, enable ? 1 : 0),
	                    TAG, "camera power level");
	s_camera_power_enabled = enable;
	/* Supplier app_video.c specifies 100 ms after CAM_IO0 releases the rail. */
	if (enable) vTaskDelay(pdMS_TO_TICKS(100));
	return ESP_OK;
}

esp_err_t hardware_board_init(void)
{
	ESP_RETURN_ON_ERROR(hardware_board_init_i2c(), TAG, "i2c init failed");
	ESP_RETURN_ON_ERROR(hardware_board_set_audio_power(false), TAG, "audio power init failed");
	return ESP_OK;
}
