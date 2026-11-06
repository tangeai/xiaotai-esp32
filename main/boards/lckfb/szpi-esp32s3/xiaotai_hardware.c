/* LCKFB physical resources. Keep the four-slot MMR and duplex codec contract.
 * UI and media borrow handles; only this module creates shared buses. */
#include "xiaotai_hardware.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/semphr.h"
static const char *TAG = "LichuangHardware";
#define AUDIO_HW_SAMPLE_RATE_HZ 16000U
#define AUDIO_TDM_SLOTS 4U
#define AUDIO_MCLK_MULTIPLE 256U
#define AUDIO_MCLK_HZ (AUDIO_HW_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE)
#define LCK_MIC_PGA_DB 24.0f
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_touch_io;
static bool s_spi_owned;
static bool s_backlight_configured;
static bool s_prepared;
static bool s_audio_ready;
static bool s_display_ready;
static bool s_cleanup_failed;
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA GPIO_NUM_1
#define BOARD_I2C_SCL GPIO_NUM_2
#define BOARD_I2C_HZ 100000U

#define PCA9557_ADDRESS 0x19U
#define PCA9557_INPUT_REG 0x00U
#define PCA9557_OUTPUT_REG 0x01U
#define PCA9557_CONFIG_REG 0x03U
#define PCA9557_LCD_CS_MASK BIT(0)
#define PCA9557_PA_EN_MASK BIT(1)
#define PCA9557_DVP_PWDN_MASK BIT(2)

#define I2S_MCLK GPIO_NUM_38
#define I2S_BCLK GPIO_NUM_14
#define I2S_WS GPIO_NUM_13
#define I2S_ADC_DIN GPIO_NUM_12
#define I2S_DAC_DOUT GPIO_NUM_45
#define I2S_AUDIO_PORT 0

#define LCD_HOST SPI3_HOST
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_NATIVE_H_RES 240
#define LCD_NATIVE_V_RES 320
#define LCD_PIN_MOSI GPIO_NUM_40
#define LCD_PIN_SCLK GPIO_NUM_41
#define LCD_PIN_DC GPIO_NUM_39
#define LCD_PIN_BL GPIO_NUM_42
#define LCD_BACKLIGHT_ON_LEVEL 0
#define LCD_BACKLIGHT_OFF_LEVEL 1
#define LCD_DRAW_LINES 10
#define LCD_PIXEL_CLOCK_HZ (80U * 1000U * 1000U)
#define TOUCH_I2C_HZ 100000U

static i2s_chan_handle_t s_i2s_rx;
static i2s_chan_handle_t s_i2s_tx;
static const audio_codec_data_if_t *s_i2s_data_if;
static const audio_codec_ctrl_if_t *s_es7210_ctrl_if;
static const audio_codec_ctrl_if_t *s_es8311_ctrl_if;
static const audio_codec_gpio_if_t *s_es8311_gpio_if;
static const audio_codec_if_t *s_es7210_codec_if;
static const audio_codec_if_t *s_es8311_codec_if;
static esp_codec_dev_handle_t s_microphone_dev;
static esp_codec_dev_handle_t s_speaker_dev;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pca9557_i2c_dev;
static uint8_t s_pca9557_output;
static StaticSemaphore_t s_output_mutex_storage;
static SemaphoreHandle_t s_output_mutex;
static esp_err_t pca9557_set_output(uint8_t mask, bool high);

static void cleanup_error(const char *resource, int error)
{
    if (error == 0) return;
    s_cleanup_failed = true;
    ESP_LOGE(TAG, "rollback %s failed: %d; resource state requires inspection", resource, error);
}

/* Called only before handles are published to media/UI. Failed deletion keeps
 * its handle and blocks reinitialization; never discard evidence of a live bus. */
static void release_prepare(void)
{
    if (s_i2s_rx) {
        esp_err_t err = i2s_del_channel(s_i2s_rx);
        cleanup_error("I2S RX", err);
        if (err == ESP_OK) s_i2s_rx = NULL;
    }
    if (s_i2s_tx) {
        esp_err_t err = i2s_del_channel(s_i2s_tx);
        cleanup_error("I2S TX", err);
        if (err == ESP_OK) s_i2s_tx = NULL;
    }
    if (s_pca9557_i2c_dev) {
        esp_err_t err = i2c_master_bus_rm_device(s_pca9557_i2c_dev);
        cleanup_error("PCA9557 device", err);
        if (err == ESP_OK) s_pca9557_i2c_dev = NULL;
    }
    if (s_i2c_bus && !s_pca9557_i2c_dev) {
        esp_err_t err = i2c_del_master_bus(s_i2c_bus);
        cleanup_error("I2C bus", err);
        if (err == ESP_OK) s_i2c_bus = NULL;
    }
    if (s_output_mutex && !s_i2c_bus) {
        vSemaphoreDelete(s_output_mutex);
        s_output_mutex = NULL;
    }
}

static void release_codecs(void)
{
    if (s_microphone_dev) {
        cleanup_error("microphone close", esp_codec_dev_close(s_microphone_dev));
        esp_codec_dev_delete(s_microphone_dev);
        s_microphone_dev = NULL;
    }
    if (s_es7210_codec_if) {
        cleanup_error("ES7210 codec", audio_codec_delete_codec_if(s_es7210_codec_if));
        s_es7210_codec_if = NULL;
    }
    if (s_es7210_ctrl_if) {
        cleanup_error("ES7210 control", audio_codec_delete_ctrl_if(s_es7210_ctrl_if));
        s_es7210_ctrl_if = NULL;
    }
    if (s_speaker_dev) {
        cleanup_error("speaker close", esp_codec_dev_close(s_speaker_dev));
        esp_codec_dev_delete(s_speaker_dev);
        s_speaker_dev = NULL;
    }
    if (s_es8311_codec_if) {
        cleanup_error("ES8311 codec", audio_codec_delete_codec_if(s_es8311_codec_if));
        s_es8311_codec_if = NULL;
    }
    if (s_es8311_gpio_if) {
        cleanup_error("codec GPIO", audio_codec_delete_gpio_if(s_es8311_gpio_if));
        s_es8311_gpio_if = NULL;
    }
    if (s_es8311_ctrl_if) {
        cleanup_error("ES8311 control", audio_codec_delete_ctrl_if(s_es8311_ctrl_if));
        s_es8311_ctrl_if = NULL;
    }
    if (s_i2s_data_if) {
        cleanup_error("codec data", audio_codec_delete_data_if(s_i2s_data_if));
        s_i2s_data_if = NULL;
    }
}

static void release_display(void)
{
    if (s_touch) {
        esp_err_t err = esp_lcd_touch_del(s_touch);
        cleanup_error("touch", err);
        if (err == ESP_OK) s_touch = NULL;
    }
    if (s_touch_io && !s_touch) {
        esp_err_t err = esp_lcd_panel_io_del(s_touch_io);
        cleanup_error("touch IO", err);
        if (err == ESP_OK) s_touch_io = NULL;
    }
    if (s_lcd_panel) {
        esp_err_t err = esp_lcd_panel_del(s_lcd_panel);
        cleanup_error("panel", err);
        if (err == ESP_OK) s_lcd_panel = NULL;
    }
    if (s_lcd_io && !s_lcd_panel) {
        esp_err_t err = esp_lcd_panel_io_del(s_lcd_io);
        cleanup_error("panel IO", err);
        if (err == ESP_OK) s_lcd_io = NULL;
    }
    if (s_spi_owned && !s_lcd_io) {
        esp_err_t err = spi_bus_free(LCD_HOST);
        cleanup_error("LCD SPI", err);
        if (err == ESP_OK) s_spi_owned = false;
    }
    if (s_prepared) cleanup_error("LCD deselect", pca9557_set_output(PCA9557_LCD_CS_MASK, true));
    if (s_backlight_configured) {
        cleanup_error("backlight off", gpio_set_level(LCD_PIN_BL, LCD_BACKLIGHT_OFF_LEVEL));
    }
}
static esp_err_t pca9557_write_register(uint8_t reg, uint8_t value)
{
    uint8_t bytes[2] = {reg, value};
    if (s_pca9557_i2c_dev == NULL) {
        i2c_device_config_t device = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = PCA9557_ADDRESS,
            .scl_speed_hz = BOARD_I2C_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus,
                                                   &device,
                                                   &s_pca9557_i2c_dev);
        if (err != ESP_OK) {
            return err;
        }
    }
    return i2c_master_transmit(s_pca9557_i2c_dev,
                               bytes,
                               sizeof(bytes),
                               100);
}

static esp_err_t pca9557_set_output(uint8_t mask, bool high)
{
    /* Serialize all board clients, including display initialization, before
     * computing the new shadow. Failed writes never advance the shadow. */
    if (s_output_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_output_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    uint8_t value = high ? s_pca9557_output | mask : s_pca9557_output & (uint8_t)~mask;
    esp_err_t err = pca9557_write_register(PCA9557_OUTPUT_REG, value);
    if (err == ESP_OK) s_pca9557_output = value;
    xSemaphoreGive(s_output_mutex);
    return err;
}

static esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus != NULL) return ESP_ERR_INVALID_STATE;
    s_output_mutex = xSemaphoreCreateMutexStatic(&s_output_mutex_storage);
    if (s_output_mutex == NULL) return ESP_ERR_NO_MEM;
    const i2c_master_bus_config_t config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&config, &s_i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    /* LCD_CS=1、PA_EN=0、DVP_PWDN=1；I/O0..2 为输出，其余保持输入。 */
    s_pca9557_output = PCA9557_LCD_CS_MASK | PCA9557_DVP_PWDN_MASK;
    err = pca9557_write_register(PCA9557_OUTPUT_REG, s_pca9557_output);
    if (err != ESP_OK) {
        return err;
    }
    return pca9557_write_register(PCA9557_CONFIG_REG, 0xF8U);
}

static esp_err_t audio_i2s_init(void)
{
    /*
     * ESP32-S3 I2S v2 支持在一次 i2s_new_channel() 中注册配对 TX/RX，硬件
     * 会共享 BCLK/WS。ES7210 RX 使用四槽 TDM，而 ES8311 TX 保持板级
     * 参考实现验证过的 16-bit stereo standard-I2S；两路共享 256 Fs MCLK。
     */
    i2s_chan_config_t audio_channel =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_AUDIO_PORT, I2S_ROLE_MASTER);
    audio_channel.auto_clear = true;
    esp_err_t err = i2s_new_channel(&audio_channel, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t tx_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HW_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DAC_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(s_i2s_tx, &tx_config);
    if (err != ESP_OK) {
        return err;
    }

    i2s_tdm_config_t rx_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_HW_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_ADC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_tdm_mode(s_i2s_rx, &rx_config);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t audio_i2s_start(void)
{
    /* esp_codec_dev opens both directions as one duplex transaction.  Calling
     * i2s_channel_enable here would bypass its paired-channel state machine. */
    return ESP_OK;
}

static esp_err_t audio_codecs_init(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_AUDIO_PORT,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_i2s_data_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* The PCA9557 and codecs share the driver_ng I2C family.  Codec
     * addresses remain in the library's 8-bit wire representation. */
    audio_codec_i2c_cfg_t es8311_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    s_es8311_ctrl_if = audio_codec_new_i2c_ctrl(&es8311_i2c);
    s_es8311_gpio_if = audio_codec_new_gpio();
    if (s_es8311_ctrl_if == NULL || s_es8311_gpio_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_es8311_ctrl_if,
        .gpio_if = s_es8311_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    s_es8311_codec_if = es8311_codec_new(&es8311_cfg);
    if (s_es8311_codec_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t speaker_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_es8311_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_speaker_dev = esp_codec_dev_new(&speaker_cfg);
    if (s_speaker_dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_sample_info_t speaker_format = {
        .sample_rate = AUDIO_HW_SAMPLE_RATE_HZ,
        .bits_per_sample = 16,
        .channel = 2,
    };
    if (esp_codec_dev_open(s_speaker_dev, &speaker_format) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(s_speaker_dev, 70) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_speaker_dev, false) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }

    audio_codec_i2c_cfg_t es7210_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR | 0x02U,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    s_es7210_ctrl_if = audio_codec_new_i2c_ctrl(&es7210_i2c);
    if (s_es7210_ctrl_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = s_es7210_ctrl_if,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                        ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    s_es7210_codec_if = es7210_codec_new(&es7210_cfg);
    if (s_es7210_codec_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t microphone_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_es7210_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_microphone_dev = esp_codec_dev_new(&microphone_cfg);
    if (s_microphone_dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Capture both people microphones without changing the shared BCLK/WS.
     * The physical order is MIC1/MIC3(reference)/MIC2/MIC4(unused). */
    esp_codec_dev_sample_info_t microphone_format = {
        .sample_rate = AUDIO_HW_SAMPLE_RATE_HZ,
        .bits_per_sample = 16,
        .channel = AUDIO_TDM_SLOTS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
    };
    if (esp_codec_dev_open(s_microphone_dev, &microphone_format) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_channel_gain(s_microphone_dev,
                                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
                                          LCK_MIC_PGA_DB) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_channel_gain(s_microphone_dev,
                                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
                                          LCK_MIC_PGA_DB) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_channel_gain(s_microphone_dev,
                                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
                                          0.0f) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t display_hardware_init(void)
{
    const gpio_config_t backlight = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight), TAG, "backlight GPIO");
    s_backlight_configured = true;
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_BL, LCD_BACKLIGHT_OFF_LEVEL), TAG,
                        "backlight off");

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "LCD SPI bus");
    s_spi_owned = true;
    const esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = GPIO_NUM_NC,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 2,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            (esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &s_lcd_io),
                        TAG, "LCD panel IO");
    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel, &s_lcd_panel),
                        TAG, "ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(pca9557_set_output(PCA9557_LCD_CS_MASK, false), TAG, "cannot select LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG,
                        "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_lcd_panel, true), TAG, "panel swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, true, false), TAG, "panel mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG, "panel on");

    esp_lcd_panel_io_i2c_config_t touch_io_config =
        ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    /* Reuse the board's sole driver_ng I2C0 master.  Touch must never create
     * the deprecated panel-IO v1 path because it conflicts before app_main. */
    touch_io_config.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(
                            s_i2c_bus, &touch_io_config, &s_touch_io),
                        TAG, "touch I2C IO");
    const esp_lcd_touch_config_t touch = {
        .x_max = LCD_NATIVE_H_RES,
        .y_max = LCD_NATIVE_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 1, .mirror_x = 1, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(s_touch_io, &touch, &s_touch),
                        TAG, "FT6336 touch");
    return ESP_OK;
}


esp_err_t lck_audio_prepare(void)
{
    if (s_cleanup_failed) return ESP_ERR_INVALID_STATE;
    if (s_prepared) return ESP_OK;
    esp_err_t err = board_i2c_init();
    if (err == ESP_OK) err = audio_i2s_init();
    if (err == ESP_OK) s_prepared = true;
    else {
        ESP_LOGE(TAG, "prepare failed: %s", esp_err_to_name(err));
        release_prepare();
    }
    return err;
}

esp_err_t lck_audio_open(void)
{
    if (s_cleanup_failed || !s_prepared) return ESP_ERR_INVALID_STATE;
    if (s_audio_ready) return ESP_OK;
    esp_err_t err = audio_codecs_init();
    if (err == ESP_OK) err = audio_i2s_start();
    if (err == ESP_OK) s_audio_ready = true;
    else {
        ESP_LOGE(TAG, "audio open failed: %s", esp_err_to_name(err));
        release_codecs();
    }
    return err;
}

esp_err_t lck_display_open(void)
{
    if (s_cleanup_failed || !s_prepared) return ESP_ERR_INVALID_STATE;
    if (s_display_ready) return ESP_OK;
    esp_err_t err = display_hardware_init();
    if (err == ESP_OK) s_display_ready = true;
    else {
        ESP_LOGE(TAG, "display open failed: %s", esp_err_to_name(err));
        release_display();
    }
    return err;
}
esp_err_t lck_output_set(uint8_t mask, bool high) { return pca9557_set_output(mask, high); }
esp_err_t lck_backlight_set(bool awake)
{
    return gpio_set_level(LCD_PIN_BL, awake ? LCD_BACKLIGHT_ON_LEVEL : LCD_BACKLIGHT_OFF_LEVEL);
}
void lck_hardware_get(xiaotai_hardware_t *hardware)
{
    *hardware = (xiaotai_hardware_t){
        .i2c = s_i2c_bus, .microphone = s_microphone_dev, .speaker = s_speaker_dev,
        .panel_io = s_lcd_io, .panel = s_lcd_panel, .touch = s_touch,
    };
}
