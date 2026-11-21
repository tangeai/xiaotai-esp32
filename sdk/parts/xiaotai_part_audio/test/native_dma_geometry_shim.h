// SPDX-License-Identifier: MIT
#pragma once

#include <assert.h>
#include "xiaotai/parts/audio.h"

#define SOC_I2S_NUM 3
#define SOC_I2S_SUPPORTS_PDM_RX 1
#define SOC_I2S_SUPPORTS_PDM2PCM 1
#define SOC_I2S_SUPPORTS_PDM_TX 1
#define SOC_I2S_SUPPORTS_PCM2PDM 1
#define SOC_I2S_SUPPORTS_TDM 1
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_DMA_BUFFER_MAX_SIZE 4092U
#define ESP_LOGW(...) ((void)0)

enum { MAX_DMA_BYTES = I2S_DMA_BUFFER_MAX_SIZE, CACHE_LL_LEVEL_INT_MEM = 1, CACHE_TYPE_DATA = 2 };
typedef struct { uint16_t dma_frame_num; uint8_t dma_desc_num; } i2s_chan_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(id, role) {.dma_frame_num = 240, .dma_desc_num = 6}

typedef struct { uint32_t active_slot; } fixture_channel_t;
typedef fixture_channel_t *i2s_chan_handle_t;
uint32_t i2s_get_buf_size(i2s_chan_handle_t handle, uint32_t bits, uint32_t frames);
xiaotai_err_t xt_audio_io_describe(xiaotai_i2s_config_t *c, uint8_t rx, uint8_t tx);

extern uint32_t fixture_cache_line;
static inline uint32_t cache_hal_get_cache_line_size(unsigned level, unsigned type)
{
    assert(level == CACHE_LL_LEVEL_INT_MEM && type == CACHE_TYPE_DATA);
    return fixture_cache_line;
}
