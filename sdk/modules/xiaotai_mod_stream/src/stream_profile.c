// SPDX-License-Identifier: MIT
#include "stream_internal.h"

#include <stdio.h>
#include <string.h>

#include "xiaotai/platform.h"

static int overflow(void)
{
    stream_profile_overflow();
    return -1;
}

static int write_profile(char *out, size_t capacity, void *ctx)
{
    (void)ctx;
    if (!capacity) return overflow();
    const xiaotai_stream_config_t *config = stream_config();
    /* Compose in bounded local storage so the platform may give an
     * exact-sized output (the scene does not include a terminator). */
    char audio[160];
    const int n = snprintf(audio, sizeof(audio),
                           "{\"up_audio_streamid\":%u,\"down_audio_streamid\":%u,"
                           "\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
                           "\"audio_rate\":8000,\"audio_channels\":1",
                           (unsigned)config->up_audio_stream, (unsigned)config->down_audio_stream);
    if (n < 0 || (size_t)n >= sizeof(audio) || (size_t)n > capacity) return overflow();
    memcpy(out, audio, (size_t)n);
    size_t length = (size_t)n;
    const char empty[] = ",\"up_video_mt\":[],\"down_video_mt\":[],\"no_video\":true}";
    if (stream_has_video()) {
        char suffix[80];
        const int m = snprintf(suffix, sizeof(suffix),
                               ",\"up_video_streamid\":%u,\"down_video_streamid\":%u,\"no_video\":false}",
                               (unsigned)config->up_video_stream, (unsigned)config->down_video_stream);
        if (m < 0 || (size_t)m >= sizeof(suffix)) return overflow();
        if ((size_t)m < capacity - length &&
            xiaotai_platform_scene_parts(XIAOTAI_STREAM_ACTIVITY, XIAOTAI_STREAM_PROFILE_SCENE,
                                        out, capacity - (size_t)m, &length)) {
            memcpy(out + length, suffix, (size_t)m);
            return (int)(length + (size_t)m);
        }
        if (sizeof(empty) - 1U > capacity - length) return overflow();
        stream_profile_overflow();
    } else if (sizeof(empty) - 1U > capacity - length) {
        return overflow();
    }
    memcpy(out + length, empty, sizeof(empty) - 1U);
    return (int)(length + sizeof(empty) - 1U);
}

xiaotai_err_t stream_profile_register(void)
{
    return xiaotai_platform_profile_scene(XIAOTAI_STREAM_PROFILE_SCENE, write_profile, NULL);
}
