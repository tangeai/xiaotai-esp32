// SPDX-License-Identifier: MIT
#include "internal.h"

xiaotai_err_t xiaotai_media_play_clips(const xiaotai_media_play_t *request, uint32_t *id)
{
    return media_session_play(request, id);
}

xiaotai_err_t xiaotai_media_play(const char *sound, xiaotai_play_priority_t priority)
{
    const xiaotai_media_play_t request = {.clips = {{.name = sound}}, .clip_count = 1, .priority = priority};
    return media_session_play(&request, NULL);
}

void xiaotai_media_stop(uint32_t id)
{
    media_session_stop_play(id);
}
