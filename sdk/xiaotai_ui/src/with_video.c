// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "xiaotai/video.h"

static bool pending(void *ctx)
{
    return xiaotai_video_frame_acquire_for((uint32_t)(uintptr_t)ctx, NULL);
}

static bool placement(void *ctx, uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      xiaotai_ui_rect_t *picture)
{
    (void)ctx;
    const xiaotai_video_area_t area = {x, y, width, height};
    return xiaotai_video_frame_placement(&area, picture);
}

static bool paint(void *ctx, const xiaotai_ui_surface_t *target,
                  uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    (void)ctx;
    if (!target) return false;
    xiaotai_video_pixel_t pixel;
    switch (target->pixel) {
    case XIAOTAI_PIXEL_RGB565:
        pixel = target->swap_bytes ? XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED : XIAOTAI_VIDEO_PIXEL_RGB565;
        break;
    case XIAOTAI_PIXEL_RGB888:
        pixel = XIAOTAI_VIDEO_PIXEL_RGB888;
        break;
    default:
        return false;
    }
    const xiaotai_video_surface_t surface = {
        .pixels = target->pixels, .stride = target->stride, .pixel = pixel,
        .x = target->x, .y = target->y, .width = target->width, .height = target->height,
    };
    const xiaotai_video_area_t area = {x, y, width, height};
    return xiaotai_video_frame_paint(&surface, &area);
}

xiaotai_err_t xiaotai_ui_video_show(uint8_t display, uint32_t generation)
{
    const xiaotai_ui_video_source_t source = {.pending = pending, .paint = paint, .placement = placement,
        .ctx = (void *)(uintptr_t)generation};
    return ui_video_install(display, &source, generation, xiaotai_video_frame_release);
}
