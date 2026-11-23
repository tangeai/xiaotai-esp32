// SPDX-License-Identifier: MIT
#include "xiaotai/video_link.h"

static bool ready(void *ctx, uint32_t generation)
{
    (void)ctx;
    return xiaotai_link_can_send(generation, XIAOTAI_LINK_VIDEO);
}

static size_t backlog(void *ctx, uint32_t generation)
{
    (void)ctx;
    return xiaotai_link_backlog(generation);
}

static xiaotai_err_t send(void *ctx, uint32_t generation, const xiaotai_video_packet_t *packet, uint32_t limit)
{
    (void)ctx;
    if (!packet) return XIAOTAI_ERR_INVALID_ARG;
    if (!XIAOTAI_CODEC_IS_VIDEO(packet->codec)) return XIAOTAI_ERR_NOT_SUPPORTED;
    return xiaotai_link_send_video(generation, &(const xiaotai_link_video_frame_t){
        .codec = packet->codec, .key_frame = packet->key_frame, .timestamp_ms = packet->timestamp_ms,
        .data = packet->data, .size = packet->size, .backlog_limit_bytes = limit,
    });
}

static bool take_key(void *ctx, uint32_t generation)
{
    (void)ctx;
    return xiaotai_link_take_key_frame_request(generation);
}

static xiaotai_err_t request_key(void *ctx, uint32_t generation)
{
    (void)ctx;
    return xiaotai_link_request_key_frame(generation);
}

static xiaotai_err_t set_bitrate(void *ctx, uint32_t generation, uint32_t min, uint32_t start, uint32_t max)
{
    (void)ctx;
    return xiaotai_link_set_video_bitrate(generation, min, start, max);
}

static bool take_bitrate(void *ctx, uint32_t generation, uint32_t *target)
{
    (void)ctx;
    return xiaotai_link_take_video_bitrate(generation, target);
}

const xiaotai_video_link_t *xiaotai_video_transport_link(void)
{
    static const xiaotai_video_link_t adapter = {
        .ready = ready, .backlog = backlog, .send = send,
        .take_key_frame_request = take_key, .request_key_frame = request_key,
        .set_bitrate_range = set_bitrate, .take_bitrate_target = take_bitrate,
    };
    return &adapter;
}

bool xiaotai_video_transport_receive(void *ctx, uint32_t generation, const xiaotai_link_frame_t *frame,
                                     const uint8_t *data, size_t size)
{
    (void)ctx;
    if (!frame || frame->kind != XIAOTAI_LINK_VIDEO || !XIAOTAI_CODEC_IS_VIDEO(frame->format.codec)) return false;
    return xiaotai_video_push(generation, &(const xiaotai_video_packet_t){
        .codec = frame->format.codec, .key_frame = frame->key_frame, .timestamp_ms = frame->timestamp_ms,
        .data = data, .size = size,
    });
}
