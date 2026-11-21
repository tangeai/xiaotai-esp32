// SPDX-License-Identifier: MIT
#include "xiaotai/video.h"

xiaotai_err_t xiaotai_video_viewport_for_display(size_t display, xiaotai_video_viewport_t *viewport)
{
    if (!viewport || display > UINT8_MAX) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_display_t *part = xiaotai_hal_display(display);
    if (!part) return XIAOTAI_ERR_NOT_FOUND;
    const xiaotai_display_desc_t *desc = &part->desc;
    xiaotai_video_pixel_t pixel;
    if (desc->pixel == XIAOTAI_PIXEL_RGB888) pixel = XIAOTAI_VIDEO_PIXEL_RGB888;
    else if (desc->pixel == XIAOTAI_PIXEL_RGB565) {
        pixel = desc->flags & XIAOTAI_DISPLAY_SWAP_BYTES ? XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED
                                                         : XIAOTAI_VIDEO_PIXEL_RGB565;
    } else return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!desc->width || !desc->height) return XIAOTAI_ERR_INVALID_STATE;
    *viewport = (xiaotai_video_viewport_t){
        .display = (uint8_t)display, .width = desc->width, .height = desc->height,
        .pixel = pixel, .fit = XIAOTAI_VIDEO_FIT_CONTAIN,
    };
    return XIAOTAI_OK;
}
