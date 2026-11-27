// SPDX-License-Identifier: MIT
#include "internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool safe_hint(const char *hint)
{
    if (!hint) return true;
    for (; *hint; ++hint) {
        if ((*hint < 'a' || *hint > 'z') && (*hint < 'A' || *hint > 'Z') &&
            (*hint < '0' || *hint > '9') && *hint != ':' && *hint != '.') return false;
    }
    return true;
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool positive_decimal(const char *value)
{
    if (!value || !digit(*value) || (*value == '0' && digit(value[1]))) return false;
    bool nonzero = false;
    for (; digit(*value); ++value) nonzero |= *value != '0';
    if (*value == '.') {
        if (!digit(*++value)) return false;
        for (; digit(*value); ++value) nonzero |= *value != '0';
    }
    return nonzero && !*value;
}

bool stream_video_profile_valid(const xiaotai_stream_video_config_t *config)
{
    return safe_hint(config->object_fit) && (config->aspect_ratio_number ?
           positive_decimal(config->aspect_ratio) : safe_hint(config->aspect_ratio));
}

static const char *wire_codec(xiaotai_video_codec_t codec)
{
    switch (codec) {
    case XIAOTAI_VIDEO_CODEC_H264: return "[\"h264\"]";
    case XIAOTAI_VIDEO_CODEC_JPEG: return "[\"mjpeg\"]";
    default: return "[]";
    }
}

int stream_video_profile_write(char *out, size_t capacity, void *ctx)
{
    const stream_video_profile_t *profile = ctx;
    const xiaotai_stream_video_config_t *config = profile->config;
    char rotation[12];
    const int n = snprintf(rotation, sizeof(rotation), "%u", (unsigned)profile->rotation * 90U);
    if (!out || n < 0 || (size_t)n >= sizeof(rotation)) return -1;
    const char *quote = config->aspect_ratio_number ? "" : "\"";
    const char *pieces[] = {
        "\"up_video_mt\":", wire_codec(profile->uplink),
        ",\"down_video_mt\":", wire_codec(profile->downlink),
        ",\"camera_rotation\":", rotation,
        ",\"hor_mirror\":", config->mirror_horizontal ? "true" : "false",
        ",\"vert_mirror\":", config->mirror_vertical ? "true" : "false",
        ",\"aspect_ratio\":", quote, config->aspect_ratio ? config->aspect_ratio : "4:3", quote,
        ",\"object_fit\":\"", config->object_fit ? config->object_fit : "contain", "\"",
    };
    size_t bytes[sizeof(pieces) / sizeof(pieces[0])];
    size_t size = 0;
    for (size_t i = 0; i < sizeof(bytes) / sizeof(bytes[0]); ++i) {
        bytes[i] = strlen(pieces[i]);
        if (bytes[i] > capacity - size || bytes[i] > INT_MAX - size) return -1;
        size += bytes[i];
    }
    for (size_t i = 0; i < sizeof(bytes) / sizeof(bytes[0]); ++i) {
        memcpy(out, pieces[i], bytes[i]);
        out += bytes[i];
    }
    return (int)size;
}
