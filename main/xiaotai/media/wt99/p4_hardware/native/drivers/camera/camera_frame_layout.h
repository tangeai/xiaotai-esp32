#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline size_t camera_frame_pad_yuv420(uint8_t *data, size_t length, size_t capacity,
                                            uint16_t width, uint16_t height)
{
    if (!data || !width || width % 16U || height < 2U || (height & 1U)) return 0U;
    const size_t pair_bytes = (size_t)width * 3U;
    const size_t visible_bytes = pair_bytes * (height / 2U);
    const size_t dma_bytes = (size_t)width * ((height + 15U) & ~15U) * 3U / 2U;
    if (length < visible_bytes || length > capacity || dma_bytes > capacity) return 0U;
    /* OUYY/EVYY is row-pair interleaved. Only fill the encoder's 600->608 tail. */
    for (size_t offset = visible_bytes; offset < dma_bytes; offset += pair_bytes)
        memcpy(data + offset, data + visible_bytes - pair_bytes, pair_bytes);
    return dma_bytes;
}
