// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/stream_video.h"

typedef struct {
    xiaotai_video_codec_t uplink;
    xiaotai_video_codec_t downlink;
    xiaotai_video_rotation_t rotation;
    const xiaotai_stream_video_config_t *config;
} stream_video_profile_t;

bool stream_video_profile_valid(const xiaotai_stream_video_config_t *config);
int stream_video_profile_write(char *out, size_t capacity, void *ctx);

#ifdef XIAOTAI_STREAM_VIDEO_WITH_UI
xiaotai_err_t stream_video_present(uint8_t display, uint32_t generation);
void stream_video_unpresent(uint8_t display, uint32_t generation);
#endif
