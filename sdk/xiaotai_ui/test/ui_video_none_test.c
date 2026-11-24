// SPDX-License-Identifier: MIT
#include "xiaotai/ui.h"

#include <assert.h>

int main(void)
{
    assert(xiaotai_ui_video_show(0, 1) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_ui_video_show(255, 0) == XIAOTAI_ERR_NOT_SUPPORTED);
}
