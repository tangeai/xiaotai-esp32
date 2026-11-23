// SPDX-License-Identifier: MIT
#include <assert.h>
#include "xiaotai/video_link.h"

static unsigned calls;
static xiaotai_err_t send_result = XIAOTAI_ERR_BUSY;
static xiaotai_link_video_frame_t sent;
static xiaotai_video_packet_t received;
static bool take;

bool xiaotai_link_can_send(uint32_t generation, xiaotai_link_media_kind_t kind)
{
    assert(generation == 17 && kind == XIAOTAI_LINK_VIDEO);
    ++calls;
    return true;
}

size_t xiaotai_link_backlog(uint32_t generation)
{
    assert(generation == 17);
    ++calls;
    return 325;
}

xiaotai_err_t xiaotai_link_send_video(uint32_t generation, const xiaotai_link_video_frame_t *frame)
{
    assert(generation == 17);
    ++calls;
    sent = *frame;
    return send_result;
}

bool xiaotai_link_take_key_frame_request(uint32_t generation)
{
    assert(generation == 17);
    ++calls;
    return true;
}

xiaotai_err_t xiaotai_link_request_key_frame(uint32_t generation)
{
    assert(generation == 17);
    ++calls;
    return XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t xiaotai_link_set_video_bitrate(uint32_t generation, uint32_t min, uint32_t start, uint32_t max)
{
    assert(generation == 17 && min == 100000 && start == 200000 && max == 300000);
    ++calls;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

bool xiaotai_link_take_video_bitrate(uint32_t generation, uint32_t *bps)
{
    assert(generation == 17 && bps);
    ++calls;
    *bps = 110000;
    return true;
}

bool xiaotai_video_push(uint32_t generation, const xiaotai_video_packet_t *packet)
{
    assert(generation == 17);
    ++calls;
    received = *packet;
    return take;
}

int main(void)
{
    const xiaotai_video_link_t *link = xiaotai_video_transport_link();
    assert(link == xiaotai_video_transport_link());
    assert(link->ready(link->ctx, 17) && link->backlog(link->ctx, 17) == 325);
    assert(link->take_key_frame_request(link->ctx, 17));
    assert(link->request_key_frame(link->ctx, 17) == XIAOTAI_ERR_INVALID_STATE);
    assert(link->set_bitrate_range(link->ctx, 17, 100000, 200000, 300000) == XIAOTAI_ERR_NOT_SUPPORTED);
    uint32_t bps = 0;
    assert(link->take_bitrate_target(link->ctx, 17, &bps) && bps == 110000 && calls == 6);

    const uint8_t bytes[] = {0x12, 0x34, 0x56};
    xiaotai_video_packet_t packet = {.codec = XIAOTAI_VIDEO_CODEC_H264, .key_frame = true,
                                    .timestamp_ms = 0x43215432U, .data = bytes, .size = sizeof(bytes)};
    assert(link->send(link->ctx, 17, &packet, 325) == XIAOTAI_ERR_BUSY);
    assert(sent.codec == XIAOTAI_LINK_CODEC_H264 && sent.key_frame && sent.timestamp_ms == packet.timestamp_ms);
    assert(sent.data == bytes && sent.size == sizeof(bytes) && sent.backlog_limit_bytes == 325);
    packet.codec = XIAOTAI_VIDEO_CODEC_JPEG;
    send_result = XIAOTAI_OK;
    assert(link->send(link->ctx, 17, &packet, 0) == XIAOTAI_OK && sent.codec == XIAOTAI_LINK_CODEC_JPEG);
    send_result = XIAOTAI_FAIL;
    assert(link->send(link->ctx, 17, &packet, 0) == XIAOTAI_FAIL);
    send_result = XIAOTAI_ERR_INVALID_STATE;
    assert(link->send(link->ctx, 17, &packet, 0) == XIAOTAI_ERR_INVALID_STATE);
    packet.codec = XIAOTAI_CODEC_PCM16;
    assert(link->send(link->ctx, 17, &packet, 0) == XIAOTAI_ERR_NOT_SUPPORTED && calls == 10);

    xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .format = {.codec = XIAOTAI_LINK_CODEC_H264},
                                  .stream = 7, .key_frame = true, .timestamp_ms = 780};
    assert(!xiaotai_video_transport_receive(NULL, 17, NULL, bytes, sizeof(bytes)));
    assert(!xiaotai_video_transport_receive(NULL, 17, &frame, bytes, sizeof(bytes)) && calls == 10);
    frame.kind = XIAOTAI_LINK_VIDEO;
    frame.format.codec = XIAOTAI_LINK_CODEC_OPUS;
    assert(!xiaotai_video_transport_receive(NULL, 17, &frame, bytes, sizeof(bytes)) && calls == 10);
    frame.format.codec = XIAOTAI_LINK_CODEC_H264;
    assert(!xiaotai_video_transport_receive(NULL, 17, &frame, bytes, sizeof(bytes)) && calls == 11);
    assert(received.codec == XIAOTAI_VIDEO_CODEC_H264 && received.key_frame && received.timestamp_ms == 780);
    assert(received.data == bytes && received.size == sizeof(bytes) && !received.width && !received.height);
    take = true;
    frame.format.codec = XIAOTAI_LINK_CODEC_JPEG;
    assert(xiaotai_video_transport_receive(NULL, 17, &frame, bytes, sizeof(bytes)) && calls == 12);
    assert(received.codec == XIAOTAI_VIDEO_CODEC_JPEG);
    return 0;
}
