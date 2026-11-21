// SPDX-License-Identifier: MIT
#include "call_internal.h"

#include <string.h>

#include "xiaotai/framework.h"

static int write_call(char *out, size_t capacity, void *ctx)
{
    const xiaotai_call_config_t *config = ctx;
    static const char audio[] = "{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"]";
    static const char audio_only[] = ",\"up_video_mt\":[],\"down_video_mt\":[],"
                                     "\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":true}";
    static const char audio_geometry[] = ",\"up_video_mt\":[],\"down_video_mt\":[],"
                                         "\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":true,"
                                         "\"camera_rotation\":0,\"hor_mirror\":false,\"vert_mirror\":false,"
                                         "\"aspect_ratio\":\"4:3\",\"object_fit\":\"contain\"}";
    static const char with_video[] = ",\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":false}";
    const size_t prefix = sizeof(audio) - 1U;
    const size_t suffix = sizeof(with_video) - 1U;
    const bool geometry = config->voice_profile_geometry;
    const char *voice = geometry ? audio_geometry : audio_only;
    const size_t voice_size = (geometry ? sizeof(audio_geometry) : sizeof(audio_only)) - 1U;
    if (!out || capacity < prefix) return -1;
    memcpy(out, audio, prefix);

    const void *parts[XIAOTAI_PLATFORM_SCENE_PARTS_MAX];
    size_t count = xiaotai_activity_contributions(XIAOTAI_CALL_ACTIVITY, XIAOTAI_PLATFORM_SLOT_SCENE,
                                                 XIAOTAI_CALL_SCENE, parts, XIAOTAI_PLATFORM_SCENE_PARTS_MAX);
    if (count > XIAOTAI_PLATFORM_SCENE_PARTS_MAX) {
        call_profile_parts_ignored(count - XIAOTAI_PLATFORM_SCENE_PARTS_MAX);
        count = XIAOTAI_PLATFORM_SCENE_PARTS_MAX;
    }
    size_t used = prefix;
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_platform_scene_part_t *part = parts[i];
        if (!part || !part->write) continue;
        /* Always leave space for the complete host suffix. Unlike the
         * generic scene splicer, call falls back as a whole if a part fails. */
        if (capacity - used <= suffix) {
            call_profile_fallback();
            goto fallback;
        }
        const size_t available = capacity - used - suffix - 1U;
        const int n = part->write(out + used + 1U, available, part->ctx);
        if (n < 0 || (size_t)n > available) {
            call_profile_fallback();
            goto fallback;
        }
        if (n != 0) {
            out[used] = ',';
            used += 1U + (size_t)n;
        }
    }
    if (used != prefix) {
        memcpy(out + used, with_video, suffix);
        return (int)(used + suffix);
    }

fallback:
    if (capacity - prefix < voice_size) return -1;
    memcpy(out + prefix, voice, voice_size);
    return (int)(prefix + voice_size);
}

xiaotai_err_t call_profile_register(xiaotai_call_config_t *config)
{
    return xiaotai_platform_profile_scene(XIAOTAI_CALL_SCENE, write_call, config);
}
