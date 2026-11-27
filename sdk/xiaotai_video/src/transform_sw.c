// SPDX-License-Identifier: MIT
#include "geometry.h"

#include <string.h>
#include "xiaotai/port/mem.h"

typedef struct { uint8_t a, b, c; } pixel3_t;

static bool yuv(xiaotai_video_pixel_t pixel)
{
    return pixel == XIAOTAI_VIDEO_PIXEL_YUYV422 || pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED ||
           pixel == XIAOTAI_VIDEO_PIXEL_I420;
}

static bool limits(xiaotai_video_pixel_t from, xiaotai_video_pixel_t to, xiaotai_video_scale_limits_t *scale)
{
    if (!scale || (!yuv(from) && !video_geometry_rgb_bytes(from)) ||
        (!video_geometry_rgb_bytes(to) && !(yuv(from) && yuv(to)))) return false;
    *scale = (xiaotai_video_scale_limits_t){0}; /* unrestricted nearest neighbor */
    return true;
}

static bool inside(xiaotai_video_rect_t rect, uint16_t width, uint16_t height)
{
    return rect.width && rect.height && !((rect.x | rect.y | rect.width | rect.height) & 1U) &&
           (uint32_t)rect.x + rect.width <= width && (uint32_t)rect.y + rect.height <= height;
}

static int match(const xiaotai_video_transform_config_t *c)
{
    xiaotai_video_scale_limits_t ignored;
    if (!c || !limits(c->in_pixel, c->out_pixel, &ignored) || (unsigned)c->rotation > XIAOTAI_VIDEO_ROTATE_270 ||
        c->in_stride < c->in_width || !inside(c->crop, c->in_width, c->in_height) ||
        !inside(c->place, c->out_width, c->out_height) ||
        !video_geometry_image_bytes(c->in_pixel, c->in_stride, c->in_height) ||
        !video_geometry_image_bytes(c->out_pixel, c->out_width, c->out_height)) return 0;
    if ((c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420 || c->in_pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) &&
        c->in_width != c->in_stride) return 0;
    return 1; /* hardware and earlier equal-score transforms remain preferred */
}

static xiaotai_err_t open(const xiaotai_video_transform_config_t *c, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (!match(c)) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_video_transform_config_t *copy = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*copy));
    if (!copy) return XIAOTAI_ERR_NO_MEM;
    *copy = *c;
    *state = copy;
    return XIAOTAI_OK;
}

static xiaotai_err_t reconfigure(void *state, const xiaotai_video_transform_config_t *config)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    if (!match(config)) return XIAOTAI_ERR_NOT_SUPPORTED;
    *(xiaotai_video_transform_config_t *)state = *config;
    return XIAOTAI_OK;
}

static pixel3_t read_rgb(xiaotai_video_pixel_t layout, const uint8_t *data)
{
    if (layout == XIAOTAI_VIDEO_PIXEL_RGB888) return (pixel3_t){data[2], data[1], data[0]};
    uint16_t value;
    if (layout == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED) value = (uint16_t)((uint16_t)data[0] << 8 | data[1]);
    else memcpy(&value, data, sizeof(value));
    const uint8_t r = value >> 11, g = (value >> 5) & 63U, b = value & 31U;
    return (pixel3_t){(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

static pixel3_t read_yuv(const xiaotai_video_transform_config_t *c, const uint8_t *in, uint32_t x, uint32_t y)
{
    const size_t stride = c->in_stride;
    if (c->in_pixel == XIAOTAI_VIDEO_PIXEL_YUYV422) {
        const uint8_t *pair = in + y * stride * 2U + (x & ~1U) * 2U;
        return (pixel3_t){pair[(x & 1U) * 2U], pair[1], pair[3]};
    }
    if (c->in_pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) {
        const size_t row = stride / 2U * 3U;
        const uint8_t *pair = in + (y & ~1U) * row + (x / 2U) * 3U;
        return (pixel3_t){pair[(y & 1U) * row + 1U + (x & 1U)], pair[0], pair[row]};
    }
    const size_t luma = stride * c->in_height, chroma = (y / 2U) * (stride / 2U) + x / 2U;
    return (pixel3_t){in[y * stride + x], in[luma + chroma], in[luma + luma / 4U + chroma]};
}

static uint8_t clip(int scaled)
{
    if (scaled <= 0) return 0;
    if (scaled >= (255 << 8)) return 255;
    return (uint8_t)(scaled >> 8);
}

static pixel3_t to_rgb(pixel3_t sample, bool full)
{
    const int u = (int)sample.b - 128, v = (int)sample.c - 128;
    int luma = (int)sample.a;
    if (!full) luma = luma > 16 ? (luma - 16) * 298 : 0;
    else luma *= 256;
    const int red = full ? 359 : 409, green_u = full ? 88 : 100, green_v = full ? 183 : 208;
    const int blue = full ? 454 : 516;
    return (pixel3_t){clip(luma + red * v + 128), clip(luma - green_u * u - green_v * v + 128),
                      clip(luma + blue * u + 128)};
}

static void write_rgb(xiaotai_video_pixel_t layout, uint8_t *out, pixel3_t pixel)
{
    if (layout == XIAOTAI_VIDEO_PIXEL_RGB888) {
        out[0] = pixel.c; out[1] = pixel.b; out[2] = pixel.a;
    } else {
        const uint16_t value = (uint16_t)((pixel.a & 0xf8U) << 8 | (pixel.b & 0xfcU) << 3 | pixel.c >> 3);
        if (layout == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED) {
            out[0] = (uint8_t)(value >> 8); out[1] = (uint8_t)value;
        } else {
            memcpy(out, &value, sizeof(value));
        }
    }
}

static void write_yuv(const xiaotai_video_transform_config_t *c, uint8_t *out, uint32_t x, uint32_t y, pixel3_t pixel)
{
    const size_t width = c->out_width;
    if (c->out_pixel == XIAOTAI_VIDEO_PIXEL_YUYV422) {
        uint8_t *pair = out + y * width * 2U + (x & ~1U) * 2U;
        pair[(x & 1U) * 2U] = pixel.a;
        if (!(x & 1U)) {
            pair[1] = pixel.b;
            pair[3] = pixel.c;
        }
    } else if (c->out_pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) {
        const size_t row = width / 2U * 3U;
        uint8_t *pair = out + (y & ~1U) * row + (x / 2U) * 3U;
        pair[(y & 1U) * row + 1U + (x & 1U)] = pixel.a;
        if (!((x | y) & 1U)) {
            pair[0] = pixel.b;
            pair[row] = pixel.c;
        }
    } else {
        const size_t luma = width * c->out_height, chroma = (y / 2U) * (width / 2U) + x / 2U;
        out[y * width + x] = pixel.a;
        if (!((x | y) & 1U)) {
            out[luma + chroma] = pixel.b;
            out[luma + luma / 4U + chroma] = pixel.c;
        }
    }
}

/* Inverse mapping follows the public clockwise convention. The two ratios
 * use integer pixel indices, so their last sample stays inside the crop. */
static void source_point(const xiaotai_video_transform_config_t *c, uint32_t x, uint32_t y,
                         uint32_t *source_x, uint32_t *source_y)
{
    const uint32_t w = c->crop.width, h = c->crop.height;
    const bool turn = (c->rotation & 1U) != 0;
    const uint32_t rx = x * (turn ? h : w) / c->place.width;
    const uint32_t ry = y * (turn ? w : h) / c->place.height;
    uint32_t sx = rx, sy = ry;
    switch (c->rotation) {
    case XIAOTAI_VIDEO_ROTATE_90: sx = ry; sy = h - 1U - rx; break;
    case XIAOTAI_VIDEO_ROTATE_180: sx = w - 1U - rx; sy = h - 1U - ry; break;
    case XIAOTAI_VIDEO_ROTATE_270: sx = w - 1U - ry; sy = rx; break;
    default: break;
    }
    *source_x = c->crop.x + sx;
    *source_y = c->crop.y + sy;
}

/* An upright picture into RGB565 is what a decoded picture needs on most
 * displays: the same pixels as the general loop below writes, a row at a time,
 * without a division per pixel and with each chroma sample converted once. */
static bool upright_rgb565(const xiaotai_video_transform_config_t *c)
{
    return c->rotation == XIAOTAI_VIDEO_ROTATE_0 && video_geometry_rgb_bytes(c->out_pixel) == 2U &&
           (c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420 || video_geometry_rgb_bytes(c->in_pixel) == 2U);
}

typedef struct { uint32_t at, rest, whole, part, span; } scan_t;

/* at(x) is first + x * from / to for x = 0, 1, 2, ... */
static scan_t scan(uint32_t first, uint32_t from, uint32_t to)
{
    return (scan_t){.at = first, .whole = from / to, .part = from % to, .span = to};
}

static inline void advance(scan_t *s)
{
    s->at += s->whole;
    s->rest += s->part;
    if (s->rest >= s->span) { s->rest -= s->span; ++s->at; }
}

static inline uint32_t clip8(int scaled)
{
    return scaled <= 0 ? 0U : scaled >= (255 << 8) ? 255U : (uint32_t)scaled >> 8;
}

static void row_rgb565(const xiaotai_video_transform_config_t *c, const uint8_t *in, uint32_t sy, uint8_t *out)
{
    const uint8_t *source = in + (size_t)sy * c->in_stride * 2U;
    const bool swap = c->in_pixel != c->out_pixel;
    scan_t s = scan(c->crop.x, c->crop.width, c->place.width);
    for (uint32_t x = 0; x < c->place.width; ++x, advance(&s)) {
        const uint8_t *pixel = source + s.at * 2U;
        out[2U * x] = pixel[swap];
        out[2U * x + 1U] = pixel[!swap];
    }
}

static void row_i420(const xiaotai_video_transform_config_t *c, const uint8_t *in, uint32_t sy, uint8_t *out)
{
    const size_t stride = c->in_stride, luma = stride * c->in_height;
    const uint8_t *const py = in + sy * stride, *const pu = in + luma + (sy / 2U) * (stride / 2U),
                  *const pv = pu + luma / 4U;
    const bool full = c->in_full_range, swap = c->out_pixel == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED;
    const int red = full ? 359 : 409, green_u = full ? 88 : 100, green_v = full ? 183 : 208;
    const int blue = full ? 454 : 516;
    scan_t s = scan(c->crop.x, c->crop.width, c->place.width);
    uint32_t chroma = UINT32_MAX;
    int r = 0, g = 0, b = 0;
    for (uint32_t x = 0; x < c->place.width; ++x, advance(&s)) {
        if (s.at / 2U != chroma) {
            chroma = s.at / 2U;
            const int u = (int)pu[chroma] - 128, v = (int)pv[chroma] - 128;
            r = red * v + 128;
            g = 128 - green_u * u - green_v * v;
            b = blue * u + 128;
        }
        int y = py[s.at];
        y = full ? y * 256 : y > 16 ? (y - 16) * 298 : 0;
        const uint32_t value = (clip8(y + r) & 0xf8U) << 8 | (clip8(y + g) & 0xfcU) << 3 | clip8(y + b) >> 3;
        out[2U * x + swap] = (uint8_t)value;
        out[2U * x + !swap] = (uint8_t)(value >> 8);
    }
}

static void run_upright_rgb565(const xiaotai_video_transform_config_t *c, const uint8_t *in, uint8_t *out)
{
    const size_t bytes = (size_t)c->place.width * 2U;
    const uint8_t *last = NULL;
    uint32_t last_sy = 0;
    scan_t s = scan(c->crop.y, c->crop.height, c->place.height);
    for (uint32_t y = 0; y < c->place.height; ++y, advance(&s)) {
        uint8_t *to = out + ((size_t)(c->place.y + y) * c->out_width + c->place.x) * 2U;
        if (last && s.at == last_sy) memcpy(to, last, bytes);
        else if (c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420) row_i420(c, in, s.at, to);
        else row_rgb565(c, in, s.at, to);
        last = to;
        last_sy = s.at;
    }
}

/* Black around the picture; the picture itself is written anyway. */
static void fill_rgb(const xiaotai_video_transform_config_t *c, unsigned bytes, uint8_t *out)
{
    const size_t row = (size_t)c->out_width * bytes, left = (size_t)c->place.x * bytes,
                 right = ((size_t)c->place.x + c->place.width) * bytes;
    const uint32_t top = c->place.y, bottom = top + c->place.height;
    memset(out, 0, top * row);
    memset(out + bottom * row, 0, (c->out_height - bottom) * row);
    if (!left && right == row) return;
    for (uint32_t y = top; y < bottom; ++y) {
        memset(out + y * row, 0, left);
        memset(out + y * row + right, 0, row - right);
    }
}

static xiaotai_err_t run(void *state, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_capacity)
{
    const xiaotai_video_transform_config_t *c = state;
    if (!c || !in || !out) return XIAOTAI_ERR_INVALID_ARG;
    const size_t in_bytes = video_geometry_image_bytes(c->in_pixel, c->in_stride, c->in_height);
    const size_t out_bytes = video_geometry_image_bytes(c->out_pixel, c->out_width, c->out_height);
    if (in_size < in_bytes || out_capacity < out_bytes) return XIAOTAI_ERR_INVALID_SIZE;
    const uintptr_t i = (uintptr_t)in, o = (uintptr_t)out;
    if ((o >= i && o - i < in_bytes) || (i >= o && i - o < out_bytes)) return XIAOTAI_ERR_INVALID_ARG;
    const unsigned in_rgb = video_geometry_rgb_bytes(c->in_pixel), out_rgb = video_geometry_rgb_bytes(c->out_pixel);
    if (c->fill) {
        if (out_rgb) fill_rgb(c, out_rgb, out);
        else {
            const pixel3_t black = {c->in_full_range ? 0 : 16, 128, 128};
            for (uint32_t y = 0; y < c->out_height; ++y) {
                for (uint32_t x = 0; x < c->out_width; ++x) write_yuv(c, out, x, y, black);
            }
        }
    }
    if (upright_rgb565(c)) {
        run_upright_rgb565(c, in, out);
        return XIAOTAI_OK;
    }
    for (uint32_t y = 0; y < c->place.height; ++y) {
        uint32_t dy = c->place.y + y;
        for (uint32_t x = 0; x < c->place.width; ++x) {
            uint32_t dx = c->place.x + x, sx, sy;
            source_point(c, x, y, &sx, &sy);
            pixel3_t sample = in_rgb ? read_rgb(c->in_pixel, in + ((size_t)sy * c->in_stride + sx) * in_rgb)
                                     : read_yuv(c, in, sx, sy);
            if (out_rgb) {
                if (!in_rgb) sample = to_rgb(sample, c->in_full_range);
                write_rgb(c->out_pixel, out + ((size_t)dy * c->out_width + dx) * out_rgb, sample);
            } else {
                write_yuv(c, out, dx, dy, sample);
            }
        }
    }
    return XIAOTAI_OK;
}

const xiaotai_video_transform_t video_transform_sw = {
    .name = "nearest", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE, .output_alignment = 1,
    .match = match, .limits = limits, .open = open, .run = run, .close = xiaotai_mem_free,
    .reconfigure = reconfigure,
};
