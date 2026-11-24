// SPDX-License-Identifier: MIT
#include "ui_internal.h"

xiaotai_err_t xiaotai_ui_video_set(uint8_t display, const xiaotai_ui_video_source_t *source,
                                 uint32_t generation)
{
    return ui_video_install(display, source, generation, NULL);
}
