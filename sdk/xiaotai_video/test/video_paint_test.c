// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>
#include "paint.h"

static xiaotai_display_t display = {
    .desc = {.width = 320, .height = 480, .pixel = XIAOTAI_PIXEL_RGB565, .rotation = 1,
             .flags = XIAOTAI_DISPLAY_SWAP_BYTES}
};

const xiaotai_display_t *xiaotai_hal_display(size_t index) { return index ? NULL : &display; }

/* Every pixel n times, wherever the surface cuts the picture. */
static void enlarged(void)
{
    enum { W = 10, H = 6, SW = 13, SH = 9 };
    static uint8_t source[W * H * 3], canvas[SW * SH * 3];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i * 37U + 11U);
    for (unsigned bpp = 2; bpp <= 3; ++bpp) {
        const xiaotai_video_pixel_t pixel = bpp == 3 ? XIAOTAI_VIDEO_PIXEL_RGB888 : XIAOTAI_VIDEO_PIXEL_RGB565;
        for (uint8_t n = 1; n <= 4; ++n) {
            const xiaotai_video_frame_t frame = {.width = W, .height = H, .scale = n, .picture = {0, 0, W, H},
                .pixel = pixel, .pixels = source, .size = W * H * bpp};
            const xiaotai_video_area_t area = {20, 30, (uint16_t)(W * n), (uint16_t)(H * n)};
            video_paint_pin(&frame);
            for (unsigned dx = 0; dx < 5; ++dx) {
                for (unsigned dy = 0; dy < 5; ++dy) {
                    const xiaotai_video_surface_t surface = {.x = (uint16_t)(18 + dx), .y = (uint16_t)(27 + dy), .width = SW,
                        .height = SH, .stride = SW * bpp, .pixel = pixel, .pixels = canvas};
                    memset(canvas, 0xab, sizeof(canvas));
                    assert(xiaotai_video_frame_paint(&surface, &area));
                    for (unsigned y = 0; y < SH; ++y) {
                        for (unsigned x = 0; x < SW; ++x) {
                            const int fx = (int)(surface.x + x) - area.x, fy = (int)(surface.y + y) - area.y;
                            const bool in = fx >= 0 && fy >= 0 && fx < W * n && fy < H * n;
                            for (unsigned b = 0; b < bpp; ++b)
                                assert(canvas[(y * SW + x) * bpp + b] ==
                                       (in ? source[((unsigned)(fy / n) * W + (unsigned)(fx / n)) * bpp + b] : 0xab));
                        }
                    }
                }
            }
        }
    }
    video_paint_pin(NULL);
}

int main(void)
{
    enlarged();
    xiaotai_video_viewport_t viewport;
    assert(xiaotai_video_viewport_for_display(0, &viewport) == XIAOTAI_OK);
    assert(viewport.width == 320 && viewport.height == 480 && viewport.rotation == XIAOTAI_VIDEO_ROTATE_0 &&
           viewport.pixel == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED);
    display.desc.flags = 0;
    assert(xiaotai_video_viewport_for_display(0, &viewport) == XIAOTAI_OK &&
           viewport.pixel == XIAOTAI_VIDEO_PIXEL_RGB565);
    display.desc.pixel = XIAOTAI_PIXEL_RGB888;
    assert(xiaotai_video_viewport_for_display(0, &viewport) == XIAOTAI_OK &&
           viewport.pixel == XIAOTAI_VIDEO_PIXEL_RGB888);
    display.desc.pixel = XIAOTAI_PIXEL_MONO1_VLSB;
    assert(xiaotai_video_viewport_for_display(0, &viewport) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_video_viewport_for_display(1, &viewport) == XIAOTAI_ERR_NOT_FOUND);

    uint8_t pixels[12] = {0,0,0, 0,0,0, 0,0,0, 3,4,5};
    xiaotai_video_frame_t pinned = {.width = 2, .height = 2, .scale = 2, .picture = {1,1,1,1},
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB888, .pixels = pixels, .size = sizeof(pixels)};
    const xiaotai_video_area_t area = {7,9,7,7};
    xiaotai_video_area_t placement = {99,99,99,99};
    uint8_t canvas[7 * 23];
    xiaotai_video_surface_t surface = {.x = 7, .y = 9, .width = 7, .height = 7, .stride = 23,
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB888, .pixels = canvas};
    memset(canvas, 0xab, sizeof(canvas));
    assert(!xiaotai_video_frame_paint(&surface, &area));
    assert(!xiaotai_video_frame_placement(&area, &placement) && placement.x == 99);
    video_paint_pin(&pinned);
    assert(xiaotai_video_frame_placement(&area, &placement));
    assert(placement.x == 10 && placement.y == 12 && placement.width == 2 && placement.height == 2);
    assert(xiaotai_video_frame_paint(&surface, &area));
    for (unsigned y = 0; y < 7; ++y) {
        for (unsigned x = 0; x < 7; ++x) {
            const uint8_t *p = canvas + y * 23 + x * 3;
            const bool inside_picture = x + 7 >= placement.x && x + 7 < placement.x + placement.width &&
                                        y + 9 >= placement.y && y + 9 < placement.y + placement.height;
            assert((p[0] == 3 && p[1] == 4 && p[2] == 5) == inside_picture);
            if ((x < 1 || x >= 5 || y < 1 || y >= 5)) assert(p[0] == 0xab && p[2] == 0xab);
        }
        assert(canvas[y * 23 + 21] == 0xab && canvas[y * 23 + 22] == 0xab);
    }

    uint8_t strip[12];
    memset(strip, 0xab, sizeof(strip));
    surface = (xiaotai_video_surface_t){.x = 11, .y = 12, .width = 3, .height = 1, .stride = sizeof(strip),
        .pixels = strip, .pixel = XIAOTAI_VIDEO_PIXEL_RGB888};
    assert(xiaotai_video_frame_paint(&surface, &area));
    assert(!memcmp(strip, pixels + 9, 3));
    for (size_t i = 3; i < sizeof(strip); ++i) assert(strip[i] == 0xab);
    surface.pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED;
    assert(!xiaotai_video_frame_paint(&surface, &area));

    uint8_t rgb565[] = {0x12,0x34,0x56,0x78, 0xab,0xcd,0xef,0x90};
    pinned = (xiaotai_video_frame_t){.width = 2, .height = 2, .scale = 1, .picture = {0,0,2,2},
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, .pixels = rgb565, .size = sizeof(rgb565)};
    video_paint_pin(&pinned);
    const xiaotai_video_area_t single = {0,0,3,3};
    surface = (xiaotai_video_surface_t){.width = 3, .height = 2, .stride = 6,
        .pixels = strip, .pixel = pinned.pixel};
    memset(strip, 0, sizeof(strip));
    assert(xiaotai_video_frame_paint(&surface, &single) && xiaotai_video_frame_placement(&single, &placement));
    assert(!memcmp(strip, rgb565, 4) && !memcmp(strip + 6, rgb565 + 4, 4));
    assert(placement.x == 0 && placement.y == 0 && placement.width == 2 && placement.height == 2);
    surface.stride = 5;
    assert(!xiaotai_video_frame_paint(&surface, &single));

    /* A frame for a panel that takes its bytes swapped, where the renderer draws native RGB565 (it
     * swaps the whole strip on the way out): each pixel turned round, the rest of the surface left. */
    static const uint8_t native[] = {0x34,0x12,0x78,0x56, 0xcd,0xab,0x90,0xef};
    surface = (xiaotai_video_surface_t){.width = 3, .height = 2, .stride = 6, .pixels = strip,
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB565};
    memset(strip, 0, sizeof(strip));
    assert(xiaotai_video_frame_paint(&surface, &single));
    assert(!memcmp(strip, native, 4) && !memcmp(strip + 6, native + 4, 4) && !strip[4] && !strip[5]);
    /* Enlarged: a repeated row is copied as painted, not turned round twice. */
    uint8_t big[4 * 4 * 2];
    pinned.scale = 2;
    video_paint_pin(&pinned);
    const xiaotai_video_area_t doubled = {0, 0, 4, 4};
    surface = (xiaotai_video_surface_t){.width = 4, .height = 4, .stride = 8, .pixels = big,
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB565};
    assert(xiaotai_video_frame_paint(&surface, &doubled));
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
            assert(!memcmp(big + y * 8 + x * 2, native + ((y / 2) * 2 + x / 2) * 2, 2));
    /* Native into a swapping surface the same way; any other pair is still refused. */
    rgb565[0] = 0x01; rgb565[1] = 0x02;
    pinned.pixel = XIAOTAI_VIDEO_PIXEL_RGB565;
    pinned.scale = 1;
    video_paint_pin(&pinned);
    surface = (xiaotai_video_surface_t){.width = 1, .height = 1, .stride = 2, .pixels = strip,
        .pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED};
    assert(xiaotai_video_frame_paint(&surface, &single) && strip[0] == 0x02 && strip[1] == 0x01);
    surface.pixel = XIAOTAI_VIDEO_PIXEL_RGB888;
    assert(!xiaotai_video_frame_paint(&surface, &single));
    video_paint_pin(NULL);
    assert(!xiaotai_video_frame_placement(&single, &placement));
    return 0;
}
