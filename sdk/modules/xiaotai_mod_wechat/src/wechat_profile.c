// SPDX-License-Identifier: MIT
#include "wechat_internal.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/hal.h"

static const xiaotai_media_format_t alaw = XIAOTAI_MEDIA_FORMAT_G711A_8K;
static const xiaotai_media_format_t opus = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
static uint32_t s_video_fallbacks;

static bool supported(const xiaotai_media_format_t *format)
{
    return xiaotai_media_can_send(format) && xiaotai_media_can_receive(format);
}

const xiaotai_media_format_t *wx_profile_audio(void)
{
    xiaotai_media_info_t info = {0};
    xiaotai_media_info(&info);
    if (!info.capture_pcm && info.capture_encoding == XIAOTAI_CODEC_OPUS && supported(&opus)) return &opus;
    if (supported(&alaw)) return &alaw;
    return supported(&opus) ? &opus : NULL;
}

static size_t append_video(char *out, size_t capacity, size_t length)
{
    const void *parts[XIAOTAI_PLATFORM_SCENE_PARTS_MAX];
    const size_t count = xiaotai_activity_contributions(XIAOTAI_WECHAT_ACTIVITY, XIAOTAI_PLATFORM_SLOT_SCENE,
                                                        XIAOTAI_WECHAT_SCENE, parts, XIAOTAI_PLATFORM_SCENE_PARTS_MAX);
    const size_t base = length;
    for (size_t i = 0; i < count; ++i) {
        if (i >= XIAOTAI_PLATFORM_SCENE_PARTS_MAX || length >= capacity) break;
        const xiaotai_platform_scene_part_t *part = parts[i];
        if (!part || !part->write) break;
        const size_t available = capacity - length - 1U;
        const int len = part->write(out + length + 1U, available, part->ctx);
        if (len < 0 || (size_t)len > available) break;
        if (len) {
            out[length] = ',';
            length += (size_t)len + 1U;
        }
        if (i + 1U == count) return length;
    }
    if (count) ++s_video_fallbacks;
    return base;
}

static int write_scene(char *out, size_t capacity, void *ctx)
{
    (void)ctx;
    static const char omitted[] = ",\"up_video_mt\":\"none\",\"down_video_mt\":\"none\",\"no_video\":true}";
    static const char omitted_geometry[] = ",\"up_video_mt\":\"none\",\"down_video_mt\":\"none\",\"no_video\":true,"
                                           "\"camera_rotation\":0,\"down_video_rotation\":0,"
                                           "\"hor_mirror\":false,\"vert_mirror\":false,\"aspect_ratio\":\"4:3\","
                                           "\"object_fit\":\"contain\",\"video_res_mode\":\"auto\"}";
    static const char present[] = ",\"no_video\":false}";
    const xiaotai_media_format_t *format = wx_audio_format();
    if (!format) return 0;
    if (!out || capacity < sizeof(present)) return -1;

    xiaotai_json_writer_t json;
    xiaotai_json_writer_init(&json, out, capacity);
    xiaotai_json_object_begin(&json, NULL);
    const xiaotai_display_t *display = xiaotai_hal_display(0);
    if (display) {
        xiaotai_json_put_int(&json, "screen_width", display->desc.width);
        xiaotai_json_put_int(&json, "screen_height", display->desc.height);
    }
    xiaotai_json_put_int(&json, "audio_rate", format->rate_hz);
    xiaotai_json_put_int(&json, "audio_channels", format->channels);
    xiaotai_json_put_string(&json, "down_audio_mt", format->encoding == XIAOTAI_CODEC_OPUS ? "opus" : "alaw");
    xiaotai_json_put_int(&json, "calling_timeout_sec", wx_config()->calling_timeout_s);
    xiaotai_json_object_end(&json);
    size_t base;
    if (xiaotai_json_writer_end(&json, &base) != XIAOTAI_OK) return -1;
    --base; /* Replace the closing brace with the complete video suffix. */
    if (base > capacity - (sizeof(present) - 1U)) return -1;
    size_t length = append_video(out, capacity - (sizeof(present) - 1U), base);
    const bool video = length > base;
    const bool geometry = wx_config()->voice_profile_geometry;
    const char *suffix = video ? present : geometry ? omitted_geometry : omitted;
    const size_t suffix_size = (video ? sizeof(present) : geometry ? sizeof(omitted_geometry) : sizeof(omitted)) - 1U;
    if (suffix_size > capacity - length) return -1;
    memcpy(out + length, suffix, suffix_size);
    return (int)(length + suffix_size);
}

xiaotai_err_t wx_profile_init(void)
{
    const xiaotai_media_requirement_t g711 = {
        .uplink = alaw, .downlink = alaw, .packet_ms = 20, .latency = wx_config()->latency,
    };
    const xiaotai_media_requirement_t compressed = {
        .uplink = opus, .downlink = opus, .packet_ms = 20, .latency = wx_config()->latency,
        .max_packet_bytes = XIAOTAI_WECHAT_OPUS_PACKET_MAX, .max_bitrate_bps = XIAOTAI_WECHAT_OPUS_BITRATE_MAX,
    };
    xiaotai_err_t err = xiaotai_media_require(&g711);
    if (err == XIAOTAI_OK) err = xiaotai_media_require(&compressed);
    if (err == XIAOTAI_OK) err = xiaotai_platform_profile_scene(XIAOTAI_WECHAT_SCENE, write_scene, NULL);
    return err;
}

uint32_t wx_profile_video_fallbacks(void) { return s_video_fallbacks; }
void wx_profile_reset(void) { s_video_fallbacks = 0; }
