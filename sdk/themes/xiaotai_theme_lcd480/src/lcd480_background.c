// SPDX-License-Identifier: MIT
#include "lcd480_background.h"
#include "xiaotai/port/mem.h"

static uint32_t weight(int32_t x, int32_t y)
{
    const uint32_t radius = (uint32_t)(x * x + y * y);
    /* Keep the face canvas and its sampling margin on the flat centre. */
    if (radius <= 40000U) return 256;
    if (radius >= 120000U) return 0;
    const uint32_t t = (radius - 40000U) * 256U / (120000U - 40000U);
    return 256U - t * t * (768U - 2U * t) / 65536U;
}

/* The centre and the edge are one to two RGB565 steps apart, which drawn
 * plainly is a set of visible rings. An ordered 4 x 4 pattern spreads each
 * step over its transition instead; the flat centre stays one exact colour. */
static uint32_t channel(uint32_t from, uint32_t to, uint32_t amount, uint32_t threshold)
{
    const uint32_t mixed = to * 256U + (from - to) * amount;   /* 8.8; from >= to */
    return (mixed + threshold * 16U + 8U) / 256U;
}

bool lcd480_background(lv_image_dsc_t *image, uint16_t width, uint16_t height)
{
    if (image->data) return true;
    const uint32_t stride = (uint32_t)width * sizeof(uint16_t);
    const uint64_t bytes = (uint64_t)stride * height;
    if (width < 2U || height < 2U || stride > UINT16_MAX || bytes > UINT32_MAX || bytes > SIZE_MAX) return false;
    uint16_t *pixels = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)bytes);
    if (!pixels) return false;

    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const uint32_t c = lv_color_to_u16(lv_color_hex(LCD480_BACKGROUND_CENTER));
    const uint32_t e = lv_color_to_u16(lv_color_hex(LCD480_BACKGROUND_EDGE));
    for (int32_t y = 0; y < height; ++y) {
        const int32_t ny = (2 * y - (height - 1)) * 256 / (height - 1);
        for (int32_t x = 0; x < width; ++x) {
            const int32_t nx = (2 * x - (width - 1)) * 256 / (width - 1);
            const uint32_t w = weight(nx, ny);
            uint32_t color = c;
            if (w < 256U) {
                const uint32_t threshold = bayer[y & 3][x & 3];
                color = channel(c >> 11, e >> 11, w, threshold) << 11 |
                        channel(c >> 5 & 0x3f, e >> 5 & 0x3f, w, threshold) << 5 |
                        channel(c & 0x1f, e & 0x1f, w, threshold);
            }
            pixels[(size_t)y * width + (size_t)x] = (uint16_t)color;
        }
    }
    *image = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
        .stride = stride, .w = width, .h = height}, .data_size = (uint32_t)bytes, .data = (const uint8_t *)pixels};
    return true;
}
