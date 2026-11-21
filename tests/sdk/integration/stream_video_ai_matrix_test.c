// SPDX-License-Identifier: MIT
#include "stream_video_matrix_fixture.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/ai_vision.h"

const xiaotai_module_t stream_video_matrix_extension = {
    .name = "ai_vision",
    .init = xiaotai_module_ai_vision_init,
};
const char stream_video_matrix_extension_activity[] = XIAOTAI_AI_TALK_ACTIVITY;
const char stream_video_matrix_extension_scene[] = XIAOTAI_AI_TALK_SCENE;

void stream_video_matrix_extension_reset(void)
{
    xiaotai_ai_vision_reset();
}
