#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"

#define HARDWARE_BOARD_TYPE                     "esp32p4_waveshare_wifi6_touch_lcd_35"

#define HARDWARE_BOARD_I2C_NUM                  I2C_NUM_1
#define HARDWARE_BOARD_I2C_SDA                  GPIO_NUM_7
#define HARDWARE_BOARD_I2C_SCL                  GPIO_NUM_8
#define HARDWARE_BOARD_I2C_FREQ_HZ              400000

#define HARDWARE_BOARD_LCD_WIDTH                480
#define HARDWARE_BOARD_LCD_HEIGHT               320
#define HARDWARE_BOARD_LCD_PHYSICAL_WIDTH       320
#define HARDWARE_BOARD_LCD_PHYSICAL_HEIGHT      480

#define HARDWARE_BOARD_AUDIO_I2S_PORT           I2S_NUM_1
#define HARDWARE_BOARD_AUDIO_LRCK               GPIO_NUM_10
#define HARDWARE_BOARD_AUDIO_MCLK               GPIO_NUM_13
#define HARDWARE_BOARD_AUDIO_BCLK               GPIO_NUM_12
#define HARDWARE_BOARD_AUDIO_DIN                GPIO_NUM_11
#define HARDWARE_BOARD_AUDIO_DOUT               GPIO_NUM_9
#define HARDWARE_BOARD_AUDIO_PA_GPIO            GPIO_NUM_53
#define HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ     16000
#define HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE    16
/* ES8311 duplex bus: stereo TX duplicates mono playback; RX is mic + DAC reference. */
#define HARDWARE_BOARD_AUDIO_CHANNELS           2
#define HARDWARE_BOARD_AUDIO_ADC_CHANNELS       2
#define HARDWARE_BOARD_AUDIO_ADC_TDM_CHANNELS   2
#define HARDWARE_BOARD_AUDIO_ADC_CHANNEL_MASK   0x03
#define HARDWARE_BOARD_AUDIO_ADC_PRIMARY_CHANNEL 0
#define HARDWARE_BOARD_AUDIO_ADC_REFERENCE_CHANNEL 1
#define HARDWARE_BOARD_AUDIO_DEFAULT_VOLUME     70
#define HARDWARE_BOARD_AUDIO_DEFAULT_ADC_GAIN_DB 24.0f

#define HARDWARE_BOARD_CAMERA_ENABLED           1
#define HARDWARE_BOARD_CAMERA_I2C_NUM           I2C_NUM_0
#define HARDWARE_BOARD_CAMERA_I2C_FREQ_HZ       100000
#define HARDWARE_BOARD_CAMERA_WIDTH             1280
#define HARDWARE_BOARD_CAMERA_HEIGHT            960
/* Keep three CSI buffers so capture, H264 encode and requeue can overlap. */
#define HARDWARE_BOARD_CAMERA_BUFFER_COUNT      3
