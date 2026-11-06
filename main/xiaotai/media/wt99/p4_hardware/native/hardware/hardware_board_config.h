#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "bsp/wt99p461_s1.h"

#define HARDWARE_BOARD_TYPE                     "wt99p461_s1_1v0"

#define HARDWARE_BOARD_I2C_NUM                  I2C_NUM_1
#define HARDWARE_BOARD_I2C_SDA                  BSP_I2C_SDA
#define HARDWARE_BOARD_I2C_SCL                  BSP_I2C_SCL
#define HARDWARE_BOARD_I2C_FREQ_HZ              400000

#define HARDWARE_BOARD_LCD_WIDTH                BSP_LCD_H_RES
#define HARDWARE_BOARD_LCD_HEIGHT               BSP_LCD_V_RES
#define HARDWARE_BOARD_LCD_PHYSICAL_WIDTH       BSP_LCD_H_RES
#define HARDWARE_BOARD_LCD_PHYSICAL_HEIGHT      BSP_LCD_V_RES
/* Keep the 20-slot video pool bounded; LVGL scales this 1:2 onto the panel. */
#define HARDWARE_BOARD_VIDEO_RENDER_WIDTH       512U
#define HARDWARE_BOARD_VIDEO_RENDER_HEIGHT      300U
#define HARDWARE_BOARD_VIDEO_ZOOM               512U
#define HARDWARE_BOARD_VIDEO_RGB565_BYTE_SWAP   false
#define HARDWARE_BOARD_CAMERA_WECHAT_ROTATE_CCW90 false

#define HARDWARE_BOARD_AUDIO_I2S_PORT           I2S_NUM_1
#define HARDWARE_BOARD_AUDIO_LRCK               GPIO_NUM_10
#define HARDWARE_BOARD_AUDIO_MCLK               BSP_I2S_MCLK
#define HARDWARE_BOARD_AUDIO_BCLK               BSP_I2S_BCLK
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
#define HARDWARE_BOARD_CAMERA_I2C_NUM           HARDWARE_BOARD_I2C_NUM
#define HARDWARE_BOARD_CAMERA_I2C_FREQ_HZ       HARDWARE_BOARD_I2C_FREQ_HZ
#define HARDWARE_BOARD_CAMERA_WIDTH             1024
#define HARDWARE_BOARD_CAMERA_HEIGHT            600
#define HARDWARE_BOARD_CAMERA_I2C_ADDR           0x30
#define HARDWARE_BOARD_CAMERA_POWER_GPIO         BSP_CAMERA_POWER
/* Keep three CSI buffers so capture, H264 encode and requeue can overlap. */
#define HARDWARE_BOARD_CAMERA_BUFFER_COUNT      3
