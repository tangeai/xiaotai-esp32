// SPDX-License-Identifier: MIT
#include "lcd480_background.h"
#include "xiaotai/port/mem.h"

static uint8_t weight(int32_t x, int32_t y)
{
    const uint32_t radius = (uint32_t)(x * x + y * y);
    /* Keep the face canvas and its sampling margin on the flat centre. */
    if (radius <= 40000U) return 255;
    if (radius >= 65536U) return 0;
    const uint32_t t = (radius - 40000U) * 256U / (65536U - 40000U);
    const uint32_t curve = (t * t * (768U - 2U * t) + 32768U) / 65536U;
    return (uint8_t)((256U - curve) * 255U / 256U);
}

bool lcd480_background(lv_image_dsc_t *image, uint16_t width, uint16_t height)
{
    if (image->data) return true;
    const uint32_t stride = (uint32_t)width * sizeof(uint16_t);
    const uint64_t bytes = (uint64_t)stride * height;
    if (width < 2U || height < 2U || stride > UINT16_MAX || bytes > UINT32_MAX || bytes > SIZE_MAX) return false;
    uint16_t *pixels = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (size_t)bytes);
    if (!pixels) return false;

    const uint16_t centre = lv_color_to_u16(lv_color_hex(LCD480_BACKGROUND_CENTER));
    const uint16_t edge = lv_color_to_u16(lv_color_hex(0x101315));
    for (int32_t y = 0; y < height; ++y) {
        const int32_t ny = (2 * y - (height - 1)) * 256 / (height - 1);
        for (int32_t x = 0; x < width; ++x) {
            const int32_t nx = (2 * x - (width - 1)) * 256 / (width - 1);
            pixels[(size_t)y * width + (size_t)x] = lv_color_16_16_mix(centre, edge, weight(nx, ny));
        }
    }
    *image = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
        .stride = stride, .w = width, .h = height}, .data_size = (uint32_t)bytes, .data = (const uint8_t *)pixels};
    return true;
}
