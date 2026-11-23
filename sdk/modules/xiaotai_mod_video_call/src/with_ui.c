// SPDX-License-Identifier: MIT
#include "video_call_internal.h"
#include "xiaotai/ui.h"

const video_call_ui_t video_call_with_ui = {
    .show = xiaotai_ui_video_show,
    .clear = xiaotai_ui_video_clear,
};
