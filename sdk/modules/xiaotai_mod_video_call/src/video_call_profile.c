// SPDX-License-Identifier: MIT
#include "video_call_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/json.h"

bool video_call_profile_config_valid(const xiaotai_video_call_scene_t *scene)
{
    const char *mode = scene->video_res_mode;
    if (mode && (!*mode || mode[strspn(mode, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")])) {
        return false;
    }
    const char *ratio = scene->aspect_ratio_json;
    if (!ratio) return true;
    xiaotai_json_token_t token;
    size_t count = 0;
    return xiaotai_json_parse(ratio, strlen(ratio), &token, 1, &count, NULL) == XIAOTAI_OK && count == 1 &&
           (token.type == XIAOTAI_JSON_NUMBER || token.type == XIAOTAI_JSON_STRING);
}

static unsigned gcd(unsigned a, unsigned b)
{
    while (b) {
        unsigned remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

static bool resolved_ratio(const video_call_scene_part_t *part, char *out, size_t capacity)
{
    uint16_t width = 0, height = 0;
    if (xiaotai_video_resolve(part->uplink, part->camera, &width, &height) != XIAOTAI_OK || !width || !height) {
        return false;
    }
    unsigned divisor = gcd(width, height);
    int n = snprintf(out, capacity, "\"%u:%u\"", width / divisor, height / divisor);
    return n > 0 && (size_t)n < capacity;
}

static void codec_field(xiaotai_json_writer_t *writer, const char *key, const char *codec, bool arrays)
{
    if (arrays) {
        xiaotai_json_array_begin(writer, key);
        if (codec) xiaotai_json_put_string(writer, NULL, codec);
        xiaotai_json_array_end(writer);
    } else {
        xiaotai_json_put_string(writer, key, codec ? codec : "none");
    }
}

int video_call_profile_write(char *out, size_t capacity, void *ctx)
{
    const video_call_scene_part_t *part = ctx;
    const xiaotai_video_call_scene_t *scene = part->scene;
    const bool arrays = part->host->profile_as_arrays;
    const char *receive = !part->downlink ? NULL : part->downlink->codec == XIAOTAI_VIDEO_CODEC_H264 ? "h264" : "mjpeg";
    const char *ratio = scene->aspect_ratio_json;
    char inferred[16];
    if (!ratio && part->uplink) {
        if (!resolved_ratio(part, inferred, sizeof(inferred))) return -1;
        ratio = inferred;
    }
    const char *mode = arrays ? NULL : scene->video_res_mode ? scene->video_res_mode : "auto";

    /* The platform can give exactly enough bytes for the members (no braces
     * or NUL). Build fixed fields once, then splice validated product values. */
    char fixed[192];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, fixed, sizeof(fixed));
    xiaotai_json_object_begin(&writer, NULL);
    codec_field(&writer, "up_video_mt", part->uplink ? "h264" : NULL, arrays);
    codec_field(&writer, "down_video_mt", receive, arrays);
    xiaotai_json_put_int(&writer, "camera_rotation", scene->camera_rotation);
    if (!arrays) xiaotai_json_put_int(&writer, "down_video_rotation", 0);
    xiaotai_json_put_bool(&writer, "hor_mirror", false);
    xiaotai_json_put_bool(&writer, "vert_mirror", false);
    xiaotai_json_put_string(&writer, "object_fit", "contain");
    xiaotai_json_object_end(&writer);
    size_t written;
    if (xiaotai_json_writer_end(&writer, &written) != XIAOTAI_OK) return -1;

    const char *const pieces[] = {fixed + 1, ratio ? ",\"aspect_ratio\":" : "", ratio ? ratio : "",
                                  mode ? ",\"video_res_mode\":\"" : "", mode ? mode : "", mode ? "\"" : ""};
    size_t lengths[sizeof(pieces) / sizeof(pieces[0])] = {written - 2U};
    size_t size = lengths[0];
    for (size_t i = 1; i < sizeof(pieces) / sizeof(pieces[0]); ++i) {
        lengths[i] = strlen(pieces[i]);
        if (lengths[i] > (size_t)INT_MAX - size) return -1;
        size += lengths[i];
    }
    if (!out || size > capacity) return -1;
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); ++i) {
        memcpy(out, pieces[i], lengths[i]);
        out += lengths[i];
    }
    return (int)size;
}
