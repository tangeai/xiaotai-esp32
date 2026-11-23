// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "xiaotai/video.h"

#include <assert.h>

static const xiaotai_video_area_t s_region = {3, 7, 13, 19};
static xiaotai_ui_video_source_t s_source;
static void (*s_on_clear)(void);
static xiaotai_err_t s_result;
static xiaotai_video_surface_t s_target;
static unsigned s_acquires, s_placements, s_paints, s_releases;
static bool s_pending, s_pinned;
static uint32_t s_ready_generation;

xiaotai_err_t ui_video_install(uint8_t display, const xiaotai_ui_video_source_t *source,
                                uint32_t generation, void (*release)(void))
{
    assert(display == 2 && generation == 19 && source && release);
    if (s_result == XIAOTAI_OK) {
        s_source = *source;
        s_on_clear = release;
    }
    return s_result;
}

bool xiaotai_video_frame_acquire_for(uint32_t generation, xiaotai_video_frame_t *frame)
{
    assert(generation == 19 && !frame);
    ++s_acquires;
    if (!s_pending || s_ready_generation != generation) return false;
    s_pending = false;
    return s_pinned = true;
}

static void same_region(const xiaotai_video_area_t *area)
{
    assert(area->x == s_region.x && area->y == s_region.y &&
           area->width == s_region.width && area->height == s_region.height);
}

bool xiaotai_video_frame_placement(const xiaotai_video_area_t *area, xiaotai_video_area_t *picture)
{
    same_region(area);
    ++s_placements;
    if (!s_pinned) return false;
    *picture = (xiaotai_video_area_t){8, 9, 5, 11};
    return true;
}

bool xiaotai_video_frame_paint(const xiaotai_video_surface_t *surface, const xiaotai_video_area_t *area)
{
    same_region(area);
    ++s_paints;
    s_target = *surface;
    return s_pinned;
}

void xiaotai_video_frame_release(void)
{
    ++s_releases;
    s_pinned = false;
}

static bool paint(const xiaotai_ui_surface_t *surface)
{
    return s_source.paint(s_source.ctx, surface, s_region.x, s_region.y, s_region.width, s_region.height);
}

int main(void)
{
    s_result = XIAOTAI_ERR_INVALID_STATE;
    assert(xiaotai_ui_video_show(2, 19) == s_result && !s_on_clear);
    s_result = XIAOTAI_ERR_NOT_SUPPORTED;
    assert(xiaotai_ui_video_show(2, 19) == s_result && !s_on_clear);
    s_result = XIAOTAI_OK;
    assert(xiaotai_ui_video_show(2, 19) == s_result && s_on_clear == xiaotai_video_frame_release);
    assert(s_acquires == 0 && s_releases == 0);

    assert(!s_source.pending(s_source.ctx) && s_acquires == 1);
    s_pending = true;
    s_ready_generation = 20;
    assert(!s_source.pending(s_source.ctx) && s_acquires == 2 && s_pending && !s_pinned);
    s_ready_generation = 19;
    assert(s_source.pending(s_source.ctx) && s_acquires == 3 && s_pinned);
    xiaotai_ui_rect_t picture = {0};
    assert(s_source.placement(s_source.ctx, s_region.x, s_region.y, s_region.width, s_region.height, &picture));
    assert(s_placements == 1 && picture.x == 8 && picture.y == 9 && picture.width == 5 && picture.height == 11);

    uint8_t pixels[96] = {0};
    xiaotai_ui_surface_t surface = {
        .pixels = pixels, .x = 5, .y = 14, .width = 7, .height = 3, .stride = 32,
        .pixel = XIAOTAI_PIXEL_RGB565,
    };
    assert(paint(&surface) && s_target.pixel == XIAOTAI_VIDEO_PIXEL_RGB565);
    assert(s_target.pixels == pixels && s_target.x == 5 && s_target.y == 14 &&
           s_target.width == 7 && s_target.height == 3 && s_target.stride == 32);
    surface.swap_bytes = true;
    assert(paint(&surface) && s_target.pixel == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED);
    surface.pixel = XIAOTAI_PIXEL_RGB888;
    surface.swap_bytes = false;
    surface.y = 17;
    assert(paint(&surface) && s_target.pixel == XIAOTAI_VIDEO_PIXEL_RGB888 && s_target.y == 17);
    surface.pixel = XIAOTAI_PIXEL_MONO1;
    assert(!paint(&surface) && s_paints == 3);
    surface.pixel = XIAOTAI_PIXEL_MONO1_VLSB;
    assert(!paint(&surface) && !paint(NULL) && s_paints == 3);

    assert(!s_source.pending(s_source.ctx) && s_pinned && s_releases == 0);
    s_on_clear();
    assert(s_releases == 1 && !s_pinned);
    surface.pixel = XIAOTAI_PIXEL_RGB565;
    assert(!paint(&surface) && s_paints == 4);
}
