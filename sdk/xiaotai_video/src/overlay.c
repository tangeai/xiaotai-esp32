// SPDX-License-Identifier: MIT
#include "uplink_h264.h"

#include <string.h>
#include "geometry.h"
#include "xiaotai/port/clock.h"

enum { CHARACTERS = 12, MARGIN = 8, PAD = 4 };

/* Keep timing marks legible at both supported scales. */
static const uint8_t glyph[][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}, {0,12,12,0,12,12,0}, {0,0,0,0,0,12,12}, {0,0,0,31,0,0,0},
};

static unsigned glyph_id(char c)
{
    if (c == ':') return 10U;
    if (c == '.') return 11U;
    if (c == '-') return 12U;
    return (unsigned)(c - '0');
}

static void put_luma(uint8_t *buffer, xiaotai_video_pixel_t pixel, size_t stride, unsigned x, unsigned y, uint8_t value)
{
    if (pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) buffer[y * (stride / 2U * 3U) + x / 2U * 3U + 1U + x % 2U] = value;
    else if (pixel == XIAOTAI_VIDEO_PIXEL_YUYV422) buffer[(y * stride + x) * 2U] = value;
    else buffer[y * stride + x] = value;
}

static void black_pair(uint8_t *buffer, xiaotai_video_pixel_t pixel, size_t stride, size_t rows, unsigned x, unsigned y)
{
    if (pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) buffer[y * (stride / 2U * 3U) + x / 2U * 3U] = 128;
    else if (pixel == XIAOTAI_VIDEO_PIXEL_YUYV422) {
        buffer[(y * stride + x) * 2U + 1U] = 128;
        buffer[(y * stride + x) * 2U + 3U] = 128;
    } else {
        const size_t plane = stride * rows, uv = plane + y / 2U * (stride / 2U) + x / 2U;
        buffer[uv] = buffer[uv + plane / 4U] = 128;
    }
    put_luma(buffer, pixel, stride, x, y, 16);
    put_luma(buffer, pixel, stride, x + 1U, y, 16);
}

bool video_overlay_draw(uint8_t *buffer, size_t size, const xiaotai_video_image_t *image, const char *stamp)
{
    if (!buffer || !image || !stamp || strlen(stamp) != CHARACTERS) return false;
    const xiaotai_video_pixel_t pixel = image->pixel;
    if (pixel != XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED && pixel != XIAOTAI_VIDEO_PIXEL_I420 &&
        pixel != XIAOTAI_VIDEO_PIXEL_YUYV422) return false;
    const uint16_t stride = image->stride ? image->stride : image->width;
    const uint16_t rows = image->rows ? image->rows : image->height;
    const size_t needed = video_geometry_image_bytes(pixel, stride, rows);
    if (!needed || needed > size || stride < image->width || rows < image->height || (stride & 1U)) return false;
    for (unsigned i = 0; i < CHARACTERS; ++i) {
        const char c = stamp[i];
        if (i == 2U || i == 5U) { if (c != ':') return false; }
        else if (i == 8U) { if (c != '.') return false; }
        else if ((c < '0' || c > '9') && c != '-') return false;
    }
    const unsigned scale = image->width >= 720U ? 3U : 2U;
    const unsigned right = (MARGIN + (CHARACTERS * 6U - 1U) * scale + 2U * PAD + 1U) & ~1U;
    const unsigned bottom = (MARGIN + 7U * scale + 2U * PAD + 1U) & ~1U;
    if (right > image->width || bottom > image->height) return false;
    for (unsigned y = MARGIN; y < bottom; ++y)
        for (unsigned x = MARGIN; x < right; x += 2U) black_pair(buffer, pixel, stride, rows, x, y);
    for (unsigned i = 0; i < CHARACTERS; ++i) {
        const uint8_t *form = glyph[glyph_id(stamp[i])];
        const unsigned x0 = MARGIN + PAD + i * 6U * scale;
        for (unsigned y = 0; y < 7U * scale; ++y)
            for (unsigned x = 0; x < 5U * scale; ++x)
                if (form[y / scale] & (0x10U >> (x / scale)))
                    put_luma(buffer, pixel, stride, x0 + x, MARGIN + PAD + y, 235);
    }
    return true;
}

bool video_overlay_clock(uint8_t *buffer, size_t size, const xiaotai_video_image_t *image)
{
    char stamp[CHARACTERS + 1] = "--:--:--.---";
    if (xiaotai_clock_synced()) {
        const int64_t now = xiaotai_clock_unix_ms();
        xiaotai_clock_fields_t local;
        if (now >= 0 && xiaotai_clock_local(now, &local) == XIAOTAI_OK &&
            local.hour < 24U && local.minute < 60U && local.second <= 60U) {
            const uint8_t fields[] = {local.hour, local.minute, local.second};
            for (size_t i = 0; i < 3; ++i) {
                stamp[i * 3] = (char)('0' + fields[i] / 10U);
                stamp[i * 3 + 1] = (char)('0' + fields[i] % 10U);
            }
            const unsigned subsecond = (unsigned)(now % 1000);
            stamp[9] = (char)('0' + subsecond / 100U);
            stamp[10] = (char)('0' + subsecond / 10U % 10U);
            stamp[11] = (char)('0' + subsecond % 10U);
        }
    }
    return video_overlay_draw(buffer, size, image, stamp);
}
