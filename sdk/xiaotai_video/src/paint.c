// SPDX-License-Identifier: MIT
#include "paint.h"
#include "geometry.h"

#include <string.h>

typedef struct { int32_t left, top, right, bottom; } box_t;
static xiaotai_video_frame_t s_pin;

void video_paint_pin(const xiaotai_video_frame_t *frame)
{
    s_pin = frame ? *frame : (xiaotai_video_frame_t){0};
}

static box_t box(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    return (box_t){x, y, x + (int32_t)w, y + (int32_t)h};
}

static box_t clip(box_t a, box_t b)
{
    return (box_t){
        a.left > b.left ? a.left : b.left,
        a.top > b.top ? a.top : b.top,
        a.right < b.right ? a.right : b.right,
        a.bottom < b.bottom ? a.bottom : b.bottom,
    };
}

static bool occupied(box_t rect)
{
    return rect.left < rect.right && rect.top < rect.bottom;
}

static bool frame_box(const xiaotai_video_area_t *area, box_t *rect)
{
    if (!area || !area->width || !area->height || !s_pin.pixels || s_pin.scale < 1 || s_pin.scale > 4 ||
        !video_geometry_rgb_bytes(s_pin.pixel)) return false;
    const size_t bytes = video_geometry_image_bytes(s_pin.pixel, s_pin.width, s_pin.height);
    if (!bytes || bytes > s_pin.size) return false;
    const int32_t w = (int32_t)s_pin.width * s_pin.scale, h = (int32_t)s_pin.height * s_pin.scale;
    *rect = box((int32_t)area->x + ((int32_t)area->width - w) / 2,
                (int32_t)area->y + ((int32_t)area->height - h) / 2, (uint32_t)w, (uint32_t)h);
    return true;
}

bool xiaotai_video_frame_placement(const xiaotai_video_area_t *area, xiaotai_video_area_t *picture)
{
    box_t frame;
    if (!picture || !frame_box(area, &frame)) return false;
    const xiaotai_video_rect_t p = s_pin.picture;
    if (!p.width || !p.height || (uint32_t)p.x + p.width > s_pin.width ||
        (uint32_t)p.y + p.height > s_pin.height) return false;
    const unsigned n = s_pin.scale;
    const box_t result = clip(box(frame.left + (int32_t)(p.x * n), frame.top + (int32_t)(p.y * n),
                                  p.width * n, p.height * n), box(area->x, area->y, area->width, area->height));
    if (!occupied(result) || result.left > UINT16_MAX || result.top > UINT16_MAX) return false;
    *picture = (xiaotai_video_area_t){(uint16_t)result.left, (uint16_t)result.top,
        (uint16_t)(result.right - result.left), (uint16_t)(result.bottom - result.top)};
    return true;
}

/* One row, every pixel n times; skip of the first pixel's repeats are left of it. */
static void enlarge(uint8_t *out, const uint8_t *in, size_t count, size_t skip, unsigned n, unsigned bpp)
{
    unsigned left = n - (unsigned)skip;
    for (size_t x = 0; x < count; ++x, out += bpp) {
        out[0] = in[0];
        out[1] = in[1];
        if (bpp == 3U) out[2] = in[2];
        if (--left == 0U) { left = n; in += bpp; }
    }
}

/* RGB565 in the other byte order: a frame laid out for a panel that takes its
 * bytes swapped, painted where the renderer draws native RGB565 (it swaps the
 * whole strip on the way to the panel), or the other way round. */
static bool byte_order_only(xiaotai_video_pixel_t a, xiaotai_video_pixel_t b)
{
    return (a == XIAOTAI_VIDEO_PIXEL_RGB565 && b == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED) ||
           (a == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED && b == XIAOTAI_VIDEO_PIXEL_RGB565);
}

static void swap_row(uint8_t *row, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i, row += 2) {
        const uint8_t first = row[0];
        row[0] = row[1];
        row[1] = first;
    }
}

bool xiaotai_video_frame_paint(const xiaotai_video_surface_t *surface, const xiaotai_video_area_t *area)
{
    box_t frame;
    if (!surface || !surface->pixels || !frame_box(area, &frame)) return false;
    const bool swap = byte_order_only(surface->pixel, s_pin.pixel);
    if (surface->pixel != s_pin.pixel && !swap) return false;
    const unsigned bpp = video_geometry_rgb_bytes(s_pin.pixel), n = s_pin.scale;
    if (!surface->width || !surface->height || surface->stride < (uint32_t)surface->width * bpp ||
        (uint64_t)(surface->height - 1U) * surface->stride + (uint32_t)surface->width * bpp > SIZE_MAX) return false;
    const box_t clipped = clip(clip(frame, box(area->x, area->y, area->width, area->height)),
                               box(surface->x, surface->y, surface->width, surface->height));
    if (!occupied(clipped)) return false;
    const size_t count = (size_t)(clipped.right - clipped.left);
    const size_t first = (size_t)(clipped.left - frame.left);
    const uint8_t *painted = NULL;      /* the last source row, and where it went */
    const uint8_t *row = NULL;
    for (int32_t y = clipped.top; y < clipped.bottom; ++y) {
        const uint8_t *in = (const uint8_t *)s_pin.pixels +
                            (size_t)(y - frame.top) / n * s_pin.width * bpp;
        uint8_t *out = surface->pixels + (size_t)(y - surface->y) * surface->stride +
                       (size_t)(clipped.left - surface->x) * bpp;
        if (in == painted) {
            memcpy(out, row, count * bpp);      /* the row above, in the surface's order already */
        } else {
            if (n == 1) memcpy(out, in + first * bpp, count * bpp);
            else enlarge(out, in + first / n * bpp, count, first % n, n, bpp);
            if (swap) swap_row(out, count);
        }
        painted = in;
        row = out;
    }
    return true;
}
