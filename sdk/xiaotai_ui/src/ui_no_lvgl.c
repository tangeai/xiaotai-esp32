// SPDX-License-Identifier: MIT
#include "ui_internal.h"

xiaotai_err_t xiaotai_ui_host_painter_ex(const xiaotai_ui_host_t *host, void *widget,
                                      xiaotai_ui_paint_fn_t paint,
                                      xiaotai_ui_paint_prepare_fn_t prepare_fallback, void *ctx)
{
    (void)host;
    (void)widget;
    (void)paint;
    (void)prepare_fallback;
    (void)ctx;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t xiaotai_ui_host_painter(const xiaotai_ui_host_t *host, void *widget,
                                     xiaotai_ui_paint_fn_t paint, void *ctx)
{
    return xiaotai_ui_host_painter_ex(host, widget, paint, NULL, ctx);
}

xiaotai_err_t xiaotai_ui_host_video_region(const xiaotai_ui_host_t *host, void *widget)
{
    (void)host;
    (void)widget;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t ui_video_install(uint8_t display, const xiaotai_ui_video_source_t *source,
                              uint32_t generation, void (*release)(void))
{
    (void)display;
    (void)release;
    return !source || !source->pending || !source->paint || !generation
        ? XIAOTAI_ERR_INVALID_ARG : XIAOTAI_ERR_NOT_SUPPORTED;
}

void xiaotai_ui_video_clear(uint8_t display, uint32_t generation)
{
    (void)display;
    (void)generation;
}
