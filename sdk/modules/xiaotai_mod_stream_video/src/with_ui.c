// SPDX-License-Identifier: MIT
#include "internal.h"
#include "xiaotai/ui.h"

xiaotai_err_t stream_video_present(uint8_t display, uint32_t generation)
{
    return xiaotai_ui_video_show(display, generation);
}

void stream_video_unpresent(uint8_t display, uint32_t generation)
{
    xiaotai_ui_video_clear(display, generation);
}
