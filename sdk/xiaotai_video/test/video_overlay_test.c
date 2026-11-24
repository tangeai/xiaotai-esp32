// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "uplink_h264.h"

static uint8_t at(const uint8_t *buffer, uint16_t width, unsigned x, unsigned y)
{
    return buffer[(size_t)y * width / 2U * 3U + x / 2U * 3U + 1U + (x % 2U)];
}

static void verify(uint16_t width, unsigned scale)
{
    const uint16_t rows = 40;
    const size_t size = (size_t)width * rows / 2U * 3U;
    const xiaotai_video_image_t image = {.pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED,
                                        .width = width, .height = rows, .rows = rows, .stride = width};
    uint8_t *buffer = malloc(size), *expected = malloc(size);
    assert(buffer && expected);
    memset(expected, 77, size);
    memset(buffer, 77, size);
    assert(!video_overlay_draw(buffer, size, &image, "short"));
    assert(!video_overlay_draw(buffer, size - 1, &image, "00:00:00.000"));
    assert(memcmp(expected, buffer, size) == 0);
    assert(video_overlay_draw(expected, size, &image, "--:--:--.---"));
    assert(video_overlay_clock(buffer, size, &image)); /* clock not initialized */
    assert(memcmp(expected, buffer, size) == 0);
    assert(at(buffer, width, 7, 8) == 77 && at(buffer, width, 8, 7) == 77);
    assert(at(buffer, width, 8, 8) == 16);
    assert(buffer[(size_t)8 * width / 2U * 3U + 4U * 3U] == 128);
    assert(at(buffer, width, 12, 12 + 3U * scale - 1U) == 16);
    assert(at(buffer, width, 12, 12 + 3U * scale) == 235);
    assert(at(buffer, width, 12 + 5U * scale, 12 + 3U * scale) == 16);
    assert(at(buffer, width, width - 1U, rows - 1U) == 77);
    free(expected);
    free(buffer);
}

int main(void)
{
    verify(384, 2);
    verify(720, 3);
    return 0;
}
