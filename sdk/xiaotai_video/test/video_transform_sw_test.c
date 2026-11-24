// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>
#include "geometry.h"
#include "xiaotai_mem_host.h"

static void execute(const xiaotai_video_transform_config_t *c, const uint8_t *in, size_t size,
                    uint8_t *out, size_t capacity)
{
    void *state = NULL;
    assert(video_transform_sw.match(c) > 0);
    assert(video_transform_sw.open(c, &state) == XIAOTAI_OK && state);
    const size_t min_in = video_geometry_image_bytes(c->in_pixel, c->in_stride, c->in_height);
    const size_t min_out = video_geometry_image_bytes(c->out_pixel, c->out_width, c->out_height);
    assert(video_transform_sw.run(state, in, min_in - 1, out, capacity) == XIAOTAI_ERR_INVALID_SIZE);
    assert(video_transform_sw.run(state, in, size, out, min_out - 1) == XIAOTAI_ERR_INVALID_SIZE);
    assert(video_transform_sw.run(state, in, size, (uint8_t *)in, capacity) == XIAOTAI_ERR_INVALID_ARG);
    assert(video_transform_sw.run(state, in, size, out, capacity) == XIAOTAI_OK);
    video_transform_sw.close(state);
}

int main(void)
{
    /* R/G then B/white, plus two non-picture stride pixels. */
    const uint8_t colors[] = {0,0,255, 0,255,0, 9,9,9, 255,0,0, 255,255,255, 9,9,9};
    xiaotai_video_transform_config_t c = {
        .in_pixel = XIAOTAI_VIDEO_PIXEL_RGB888, .in_width = 2, .in_height = 2, .in_stride = 3,
        .crop = {0,0,2,2}, .out_pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED,
        .out_width = 6, .out_height = 6, .place = {2,2,2,2}, .fill = true,
    };
    const uint16_t expected[4][4] = {{0xf800,0x07e0,0x001f,0xffff}, {0x001f,0xf800,0xffff,0x07e0},
        {0xffff,0x001f,0x07e0,0xf800}, {0x07e0,0xffff,0xf800,0x001f}};
    uint8_t out[76];
    for (unsigned r = 0; r < 4; ++r) {
        c.rotation = (xiaotai_video_rotation_t)r;
        memset(out, 0x93, sizeof(out));
        execute(&c, colors, sizeof(colors), out, sizeof(out));
        for (unsigned y = 0; y < 6; ++y) {
            for (unsigned x = 0; x < 6; ++x) {
                unsigned at = (y * 6 + x) * 2;
                const uint16_t px = (uint16_t)((uint16_t)out[at] << 8 | out[at + 1]);
                assert(px == (y >= 2 && y < 4 && x >= 2 && x < 4 ? expected[r][(y - 2) * 2 + x - 2] : 0));
            }
        }
        for (size_t at = 72; at < sizeof(out); ++at) assert(out[at] == 0x93);
    }

    c.in_pixel = XIAOTAI_VIDEO_PIXEL_I420;
    c.in_stride = 2;
    c.out_width = c.out_height = 2;
    c.place.x = c.place.y = 0;
    c.rotation = XIAOTAI_VIDEO_ROTATE_0;
    c.out_pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    const uint8_t planar[] = {16,235,81,145,90,240};
    const uint8_t paired[] = {90,16,235,240,81,145,99,99,99,99,99,99}; /* last two rows are padding */
    execute(&c, planar, sizeof(planar), out, sizeof(out));
    assert(memcmp(out, paired, 6) == 0);
    c.in_pixel = XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    c.out_pixel = XIAOTAI_VIDEO_PIXEL_I420;
    c.rotation = XIAOTAI_VIDEO_ROTATE_90;
    const uint8_t turned[] = {81,16,145,235,90,240};
    execute(&c, paired, sizeof(paired), out, sizeof(out));
    assert(memcmp(out, turned, sizeof(turned)) == 0);

    c.in_pixel = XIAOTAI_VIDEO_PIXEL_YUYV422;
    c.in_stride = 4;
    c.out_pixel = XIAOTAI_VIDEO_PIXEL_RGB888;
    c.rotation = XIAOTAI_VIDEO_ROTATE_0;
    c.out_width = c.out_height = 4;
    c.place.width = c.place.height = 4;
    const uint8_t yuyv[] = {16,128,235,128, 9,9,9,9, 235,128,16,128, 8,8,8,8};
    execute(&c, yuyv, sizeof(yuyv), out, sizeof(out));
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            unsigned at = (y * 4 + x) * 3;
            const uint8_t value = (y < 2) == (x < 2) ? 0 : 255;
            assert(out[at] == value && out[at + 1] == value && out[at + 2] == value);
        }
    }
    c.in_full_range = true;
    execute(&c, yuyv, sizeof(yuyv), out, sizeof(out));
    assert(out[0] == 16 && out[6] == 235);

    void *state = NULL;
    xiaotai_mem_host_fail_nth(1);
    assert(video_transform_sw.open(&c, &state) == XIAOTAI_ERR_NO_MEM && !state);
    c.crop.x = 1;
    assert(!video_transform_sw.match(&c));
    c.crop.x = 2;
    assert(video_transform_sw.open(&c, &state) == XIAOTAI_ERR_NOT_SUPPORTED && !state);
    assert(xiaotai_mem_host_outstanding() == 0);
    return 0;
}
