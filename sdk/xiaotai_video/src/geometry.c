// SPDX-License-Identifier: MIT
#include "geometry.h"

static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t even(uint32_t n) { return n & ~1U; }

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) {
        const uint32_t rest = a % b;
        a = b;
        b = rest;
    }
    return a;
}

unsigned video_geometry_rgb_bytes(xiaotai_video_pixel_t pixel)
{
    if (pixel == XIAOTAI_VIDEO_PIXEL_RGB888) return 3;
    if (pixel == XIAOTAI_VIDEO_PIXEL_RGB565 || pixel == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED) return 2;
    return 0;
}

size_t video_geometry_image_bytes(xiaotai_video_pixel_t pixel, uint16_t stride, uint16_t rows)
{
    const uint64_t pixels = (uint64_t)stride * rows;
    uint64_t bytes = pixels * video_geometry_rgb_bytes(pixel);
    if (pixel == XIAOTAI_VIDEO_PIXEL_YUYV422 && !(stride & 1U)) bytes = pixels * 2U;
    if ((pixel == XIAOTAI_VIDEO_PIXEL_I420 || pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) &&
        !((stride | rows) & 1U)) bytes = pixels * 3U / 2U;
    return bytes <= SIZE_MAX ? (size_t)bytes : 0;
}

video_luma_t video_luma_sample(const xiaotai_camera_frame_t *frame)
{
    if (!frame) return (video_luma_t){0};
    if (!frame->data || !frame->width || !frame->height || frame->rows < frame->height || !frame->size)
        return (video_luma_t){.state = VIDEO_LUMA_INVALID};
    const bool paired = frame->format == XIAOTAI_CAMERA_YUV420_PAIRED;
    if (!paired && frame->format != XIAOTAI_CAMERA_YUV422)
        return (video_luma_t){.state = VIDEO_LUMA_UNAVAILABLE};
    const xiaotai_video_pixel_t pixel = paired ? XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED : XIAOTAI_VIDEO_PIXEL_YUYV422;
    const size_t bytes = video_geometry_image_bytes(pixel, frame->width, frame->rows);
    if (!bytes || frame->size < bytes) return (video_luma_t){.state = VIDEO_LUMA_INVALID};

    enum { GRID = 8 };
    const unsigned nx = min(GRID, frame->width), ny = min(GRID, frame->height);
    const size_t row_bytes = paired ? (size_t)frame->width / 2U * 3U : (size_t)frame->width * 2U;
    unsigned sum = 0;
    for (unsigned j = 0; j < ny; ++j) {
        const size_t row = (size_t)((2U * j + 1U) * frame->height / (2U * ny)) * row_bytes;
        for (unsigned i = 0; i < nx; ++i) {
            const size_t x = (2U * i + 1U) * frame->width / (2U * nx);
            /* YUYV: even bytes; paired CSI: UYY or VYY in each row. */
            const size_t offset = paired ? (x / 2U) * 3U + 1U + (x & 1U) : x * 2U;
            sum += frame->data[row + offset];
        }
    }
    const unsigned samples = nx * ny;
    return (video_luma_t){.state = VIDEO_LUMA_VALID, .mean = (sum + samples / 2U) / samples, .samples = samples};
}

const char *video_luma_name(video_luma_state_t state)
{
    switch (state) {
    case VIDEO_LUMA_VALID: return "valid";
    case VIDEO_LUMA_INVALID: return "invalid";
    case VIDEO_LUMA_UNAVAILABLE: return "unavailable";
    default: return "no_frame";
    }
}

int video_luma_value(video_luma_t luma) { return luma.state == VIDEO_LUMA_VALID ? luma.mean : -1; }

static bool valid_limits(const xiaotai_video_scale_limits_t *limits)
{
    return limits && (!limits->denominator || (limits->step && limits->step <= limits->max));
}

static xiaotai_video_rect_t centered(uint16_t width, uint16_t height, uint16_t inner_w, uint16_t inner_h)
{
    return (xiaotai_video_rect_t){even((width - inner_w) / 2U), even((height - inner_h) / 2U), inner_w, inner_h};
}

bool video_geometry_derive(uint16_t width, uint16_t height, uint32_t max_pixels,
                           const xiaotai_video_scale_limits_t *limits, uint16_t *out_width, uint16_t *out_height)
{
    if (!width || !height || !out_width || !out_height || !valid_limits(limits)) return false;
    const uint32_t cap = max_pixels ? max_pixels : UINT32_MAX;
    if (!limits->denominator) {
        const uint32_t div = gcd(width, height);
        const uint32_t unit_w = width / div, unit_h = height / div;
        /* One reduced axis is odd. Each admissible multiplier is even; use
         * division to compare area without overflowing a 32-bit product. */
        for (uint32_t n = even(div); n >= 2U; n -= 2U) {
            if (unit_w * n <= cap / (unit_h * n)) {
                *out_width = (uint16_t)(unit_w * n);
                *out_height = (uint16_t)(unit_h * n);
                return true;
            }
        }
    } else {
        const uint32_t d = limits->denominator, step = limits->step;
        for (uint32_t n = min(limits->max, d) / step * step; n >= step; n -= step) {
            const uint32_t x = (uint32_t)width * n, y = (uint32_t)height * n;
            if (!(x % (2U * d)) && !(y % (2U * d)) && x / d <= cap / (y / d)) {
                *out_width = (uint16_t)(x / d);
                *out_height = (uint16_t)(y / d);
                return true;
            }
        }
    }
    return false;
}

/* Fixed uplink walks smallest scale first: this keeps the biggest exact crop.
 * The unconstrained path finds the same crop by reducing the target ratio. */
static bool fixed_crop(uint16_t width, uint16_t height, uint16_t want_w, uint16_t want_h,
                       const xiaotai_video_scale_limits_t *limits, xiaotai_video_rect_t *crop)
{
    if (!want_w || !want_h || want_w > width || want_h > height || ((want_w | want_h) & 1U)) return false;
    uint32_t w, h;
    if (!limits->denominator) {
        uint32_t d = gcd(want_w, want_h);
        uint32_t x = want_w / d, y = want_h / d;
        uint32_t n = even(min(width / x, height / y));
        w = x * n;
        h = y * n;
    } else {
        w = h = 0;
        const uint32_t max = min(limits->denominator, limits->max);
        const uint32_t want_x = (uint32_t)want_w * limits->denominator;
        const uint32_t want_y = (uint32_t)want_h * limits->denominator;
        for (uint32_t n = limits->step; n <= max; n += limits->step) {
            if ((want_x % (2U * n)) || (want_y % (2U * n))) continue;
            if (want_x / n <= width && want_y / n <= height) {
                w = want_x / n;
                h = want_y / n;
                break;
            }
        }
    }
    if (!w || !h) return false;
    *crop = centered(width, height, (uint16_t)w, (uint16_t)h);
    return true;
}

static xiaotai_video_transform_config_t setup(const xiaotai_video_image_t *source, xiaotai_video_pixel_t out)
{
    return (xiaotai_video_transform_config_t){
        .in_pixel = source->pixel, .in_width = source->width, .in_height = source->height,
        .in_stride = source->stride ? source->stride : source->width,
        .in_full_range = source->full_range, .out_pixel = out,
    };
}

static bool valid_source(const xiaotai_video_image_t *source)
{
    return source && source->width && source->height && (!source->stride || source->stride >= source->width);
}

bool video_geometry_uplink(const xiaotai_video_image_t *source, const xiaotai_video_profile_t *profile,
                           xiaotai_video_rotation_t mounting, xiaotai_video_pixel_t output,
                           const xiaotai_video_scale_limits_t *limits, xiaotai_video_transform_config_t *plan)
{
    if (!valid_source(source) || !profile || !plan || !valid_limits(limits) ||
        (unsigned)mounting > XIAOTAI_VIDEO_ROTATE_270 || (!!profile->width != !!profile->height)) return false;
    xiaotai_video_transform_config_t next = setup(source, output);
    next.rotation = profile->upright ? mounting : XIAOTAI_VIDEO_ROTATE_0;
    const bool turn = (next.rotation & 1U) != 0;
    uint16_t w, h;
    if (profile->width) {
        w = turn ? profile->height : profile->width;
        h = turn ? profile->width : profile->height;
        if (!fixed_crop(source->width, source->height, w, h, limits, &next.crop)) return false;
    } else {
        if ((source->width | source->height) & 1U ||
            !video_geometry_derive(source->width, source->height, profile->max_pixels, limits, &w, &h)) return false;
        next.crop = (xiaotai_video_rect_t){0, 0, source->width, source->height};
    }
    next.out_width = turn ? h : w;
    next.out_height = turn ? w : h;
    next.place = (xiaotai_video_rect_t){0, 0, next.out_width, next.out_height};
    *plan = next;
    return true;
}

bool video_geometry_downlink(const xiaotai_video_image_t *source, const xiaotai_video_viewport_t *viewport,
                             const xiaotai_video_placement_t *placement, const xiaotai_video_scale_limits_t *limits,
                             xiaotai_video_transform_config_t *plan)
{
    if (!valid_source(source) || !viewport || !plan || !valid_limits(limits) ||
        !video_geometry_rgb_bytes(viewport->pixel) || viewport->width < 2 || viewport->height < 2) return false;
    const xiaotai_video_placement_t p = placement ? *placement : (xiaotai_video_placement_t){
        .fit = viewport->fit, .rotation = viewport->rotation, .match_orientation = viewport->match_orientation,
    };
    if ((unsigned)p.rotation > XIAOTAI_VIDEO_ROTATE_270 || (unsigned)p.fit > XIAOTAI_VIDEO_FIT_COVER) return false;
    unsigned rotation = p.rotation;
    if (p.match_orientation && source->width != source->height && viewport->width != viewport->height &&
        (((source->width > source->height) != ((rotation & 1U) != 0)) != (viewport->width > viewport->height))) {
        rotation = (rotation + 1U) & 3U;
    }
    const bool turn = (rotation & 1U) != 0;
    uint32_t crop_w = even(source->width), crop_h = even(source->height);
    const uint32_t target_w = even(turn ? viewport->height : viewport->width);
    const uint32_t target_h = even(turn ? viewport->width : viewport->height);
    if (!crop_w || !crop_h) return false;
    if (p.fit == XIAOTAI_VIDEO_FIT_COVER) {
        if (crop_w * target_h > crop_h * target_w) crop_w = even(crop_h * target_w / target_h);
        else crop_h = even(crop_w * target_h / target_w);
        if (!crop_w || !crop_h) return false;
    }
    uint32_t w, h;
    if (limits->denominator) {
        const uint32_t d = limits->denominator;
        uint32_t n = min(limits->max, min(target_w * d / crop_w, target_h * d / crop_h))
                     / limits->step * limits->step;
        w = h = 0;
        /* A hardware backend must produce these dimensions as planned. If
         * its scale would produce odd dimensions, take the next usable step. */
        for (; n >= limits->step; n -= limits->step) {
            w = crop_w * n / d;
            h = crop_h * n / d;
            if (!w || !h) return false;
            if (!((w | h) & 1U)) break;
            w = h = 0;
        }
    } else if (crop_w * target_h > crop_h * target_w) {
        w = target_w;
        h = even(crop_h * w / crop_w);
    } else {
        h = target_h;
        w = even(crop_w * h / crop_h);
    }
    if (!w || !h) return false;
    xiaotai_video_transform_config_t next = setup(source, viewport->pixel);
    next.rotation = (xiaotai_video_rotation_t)rotation;
    next.crop = centered(source->width, source->height, (uint16_t)crop_w, (uint16_t)crop_h);
    next.out_width = viewport->width;
    next.out_height = viewport->height;
    next.place = centered(viewport->width, viewport->height, (uint16_t)(turn ? h : w), (uint16_t)(turn ? w : h));
    next.fill = true;
    *plan = next;
    return true;
}

uint8_t video_geometry_render_scale(uint16_t width, uint16_t height, uint16_t max_width, uint16_t max_height)
{
    if (!width || !height) return 0;
    for (uint8_t n = 4; n > 1; --n) {
        if (width % n || height % n) continue;
        const uint16_t w = width / n, h = height / n;
        if ((w >= max_width && h >= max_height) || (w >= max_height && h >= max_width)) return n;
    }
    return 1;
}
