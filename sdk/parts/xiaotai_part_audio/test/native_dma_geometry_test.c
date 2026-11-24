// SPDX-License-Identifier: MIT
#include "native_dma_geometry_shim.h"

uint32_t fixture_cache_line = 64;

static uint32_t driver_bytes(uint32_t frames, uint32_t slots)
{
    fixture_channel_t channel = {.active_slot = slots};
    return i2s_get_buf_size(&channel, 16, frames);
}

int main(void)
{
    xiaotai_i2s_config_t c = {.rx_slots = 2, .tx_slots = 2, .dma_frames = 240};
    assert(xt_audio_io_describe(&c, 2, 2) == XIAOTAI_OK);
    assert(c.dma_frames == 240 && c.dma_descriptors == 6);
    assert(driver_bytes(c.dma_frames, 2) == 960);

    c.dma_frames = 241;
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
    assert(driver_bytes(241, 2) == 1024);
    assert(xt_audio_io_describe(&c, 2, 2) == XIAOTAI_ERR_INVALID_ARG);
    assert(c.dma_frames == 241);

    /* The queried cache line, not the board name, decides admission. */
    c.dma_frames = 244;
    fixture_cache_line = 32;
    assert(xt_audio_io_describe(&c, 2, 2) == XIAOTAI_ERR_INVALID_ARG);
    fixture_cache_line = 16;
    assert(xt_audio_io_describe(&c, 2, 2) == XIAOTAI_OK && driver_bytes(c.dma_frames, 2) == 976);
    fixture_cache_line = 64;
    c.dma_frames = 1023;
    assert(driver_bytes(1023, 2) == 4032);
    assert(xt_audio_io_describe(&c, 2, 2) == XIAOTAI_ERR_INVALID_ARG);

    /* Resolve a platform default to the frames the mono PDM driver takes.
     * The inactive direction need not have valid geometry. */
    c = (xiaotai_i2s_config_t){.rx_mode = XIAOTAI_I2S_PDM, .rx_slots = 1, .tx_slots = 255};
    assert(xt_audio_io_describe(&c, 1, 0) == XIAOTAI_OK && c.dma_frames == 256);
    assert(driver_bytes(240, 1) == 512 && driver_bytes(c.dma_frames, 1) == 512);
    c = (xiaotai_i2s_config_t){.tx_mode = XIAOTAI_I2S_PDM, .tx_slots = 1, .rx_slots = 255};
    assert(xt_audio_io_describe(&c, 0, 1) == XIAOTAI_OK && c.dma_frames == 256);

    /* The two directions must use one descriptor frame count; the wider
     * side alone need not determine whether the narrower side is aligned. */
    c = (xiaotai_i2s_config_t){.rx_slots = 4, .tx_slots = 2, .dma_frames = 248};
    assert(driver_bytes(248, 4) == 1984 && driver_bytes(248, 2) == 1024);
    assert(xt_audio_io_describe(&c, 4, 2) == XIAOTAI_ERR_INVALID_ARG);
#else
    assert(xt_audio_io_describe(&c, 2, 2) == XIAOTAI_OK);
    assert(c.dma_frames == 241 && driver_bytes(241, 2) == 964);
#endif

    c = (xiaotai_i2s_config_t){.rx_slots = 4, .tx_slots = 2, .dma_frames = 256};
    assert(xt_audio_io_describe(&c, 4, 2) == XIAOTAI_OK && c.dma_frames == 256);
    assert(driver_bytes(c.dma_frames, 4) == 2048 && driver_bytes(c.dma_frames, 2) == 1024);
    return 0;
}
