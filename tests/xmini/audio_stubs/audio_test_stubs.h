#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_CODEC_DEV_OK 0
#define ESP_AUDIO_ERR_OK 0
const char *esp_err_to_name(esp_err_t error);
void test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expression, ...) do { esp_err_t check = (expression); if (check != ESP_OK) return check; } while (0)
#define ESP_GOTO_ON_ERROR(expression, label, ...) do { ret = (expression); if (ret != ESP_OK) goto label; } while (0)
#define ESP_GOTO_ON_FALSE(expression, error, label, ...) do { if (!(expression)) { ret = (error); goto label; } } while (0)

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_enter_critical(portMUX_TYPE *lock);
void test_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) test_enter_critical(lock)
#define portEXIT_CRITICAL(lock) test_exit_critical(lock)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
typedef struct {
    unsigned capacity, item_size, count, head;
    uint8_t *buffer;
    bool dynamic;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned item_size);
QueueHandle_t xQueueCreateStatic(unsigned count, unsigned item_size, uint8_t *buffer, StaticQueue_t *queue);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
void xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
int xTaskCreate(void (*function)(void *), const char *name, unsigned stack, void *argument, unsigned priority, TaskHandle_t *handle);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
int64_t esp_timer_get_time(void);

#define I2S_NUM_0 0
#define I2C_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
typedef void *i2s_chan_handle_t;
typedef struct { unsigned dma_desc_num, dma_frame_num; bool auto_clear_after_cb; } i2s_chan_config_t;
typedef struct {
    int clk_cfg, slot_cfg;
    struct { int mclk, bclk, ws, din, dout; } gpio_cfg;
} i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) (bits)
int i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
int i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config);
int i2s_channel_enable(i2s_chan_handle_t channel);
int i2s_channel_disable(i2s_chan_handle_t channel);
int i2s_del_channel(i2s_chan_handle_t channel);
int gpio_set_level(int pin, int value);

typedef struct { int dummy; } audio_codec_data_if_t;
typedef struct { int dummy; } audio_codec_ctrl_if_t;
typedef struct { int dummy; } audio_codec_gpio_if_t;
typedef struct { int dummy; } audio_codec_if_t;
typedef void *esp_codec_dev_handle_t;
typedef struct { unsigned sample_rate, bits_per_sample, channel, mclk_multiple; } esp_codec_dev_sample_info_t;
typedef struct { int port; i2s_chan_handle_t rx_handle, tx_handle; } audio_codec_i2s_cfg_t;
typedef struct { int port, addr, clock_speed_hz; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    int codec_mode, pa_pin;
    bool use_mclk;
    unsigned mclk_div;
    struct { float pa_voltage, codec_dac_voltage; } hw_gain;
} es8311_codec_cfg_t;
typedef struct { int dev_type; const audio_codec_if_t *codec_if; const audio_codec_data_if_t *data_if; } esp_codec_dev_cfg_t;
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#define ESP_CODEC_DEV_WORK_MODE_BOTH 3
#define ESP_CODEC_DEV_TYPE_IN_OUT 3
const audio_codec_data_if_t *audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t *config);
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t *config);
const audio_codec_gpio_if_t *audio_codec_new_gpio(void);
const audio_codec_if_t *es8311_codec_new(const es8311_codec_cfg_t *config);
esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *config);
void esp_codec_dev_delete(esp_codec_dev_handle_t device);
void audio_codec_delete_codec_if(const audio_codec_if_t *object);
void audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *object);
void audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *object);
void audio_codec_delete_data_if(const audio_codec_data_if_t *object);
int esp_codec_dev_open(esp_codec_dev_handle_t device, const esp_codec_dev_sample_info_t *format);
int esp_codec_dev_close(esp_codec_dev_handle_t device);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t device, int volume);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t device, int gain);
int esp_codec_dev_read(esp_codec_dev_handle_t device, void *buffer, unsigned length);
int esp_codec_dev_write(esp_codec_dev_handle_t device, void *buffer, unsigned length);

typedef struct { uint8_t *buffer; unsigned len; } esp_audio_enc_in_frame_t;
typedef struct { uint8_t *buffer; unsigned len, encoded_bytes; } esp_audio_enc_out_frame_t;
typedef struct { uint8_t *buffer; unsigned len, consumed; } esp_audio_dec_in_raw_t;
typedef struct { uint8_t *buffer; unsigned len, decoded_size; } esp_audio_dec_out_frame_t;
typedef struct { int dummy; } esp_audio_dec_info_t;
typedef struct { unsigned frame_duration; } esp_g711_enc_config_t;
#define ESP_G711_ENC_CONFIG_DEFAULT() ((esp_g711_enc_config_t){0})
int esp_g711a_enc_open(const esp_g711_enc_config_t *config, unsigned size, void **encoder);
int esp_g711_enc_process(void *encoder, esp_audio_enc_in_frame_t *input, esp_audio_enc_out_frame_t *output);
void esp_g711_enc_close(void *encoder);
int esp_g711_dec_open(void *config, unsigned size, void **decoder);
int esp_g711a_dec_decode(void *decoder, esp_audio_dec_in_raw_t *input, esp_audio_dec_out_frame_t *output, esp_audio_dec_info_t *info);
void esp_g711_dec_close(void *decoder);

#define XMINI_PA_GPIO 10
#define XMINI_I2S_MCLK 8
#define XMINI_I2S_BCLK 7
#define XMINI_I2S_WS 5
#define XMINI_I2S_DIN 6
#define XMINI_I2S_DOUT 4
void *xmini_board_i2c(void);
