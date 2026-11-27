// SPDX-License-Identifier: MIT
/* One codec vocabulary across the link (XR-CODEC): link formats and frames,
 * media formats and packets, video frames and part descriptors all carry
 * xiaotai_codec_t (xiaotai/codec.h). The transport core therefore holds no
 * conversion: the default audio receiver copies a frame's codec into the media
 * packet, a profile takes a session's media format as it is, and the former
 * link spellings keep the wire values 0..4. A separate enumeration in any of
 * these types fails to compile here. */
#include <stdio.h>
#include <string.h>
#include "xiaotai/hal.h"
#include "xiaotai/link.h"
#include "xiaotai/media_format.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

#define IS_CODEC_TYPE(expression) _Generic((expression), xiaotai_codec_t: 1, default: 0)
_Static_assert(IS_CODEC_TYPE(((const xiaotai_link_format_t *)0)->codec), "link formats name codec values");
_Static_assert(IS_CODEC_TYPE(((const xiaotai_link_frame_t *)0)->format.codec), "frames name codec values");
_Static_assert(IS_CODEC_TYPE(((const xiaotai_link_video_frame_t *)0)->codec), "video frames name codec values");
_Static_assert(IS_CODEC_TYPE(((const xiaotai_media_packet_t *)0)->encoding), "media packets name codec values");
_Static_assert(IS_CODEC_TYPE(((const xiaotai_media_format_t *)0)->encoding), "media formats name codec values");
_Static_assert(IS_CODEC_TYPE(((const xiaotai_audio_desc_t *)0)->encoding), "part encodings name codec values");

/* The former wire list keeps its values, so nothing renumbers on the wire. */
_Static_assert(XIAOTAI_LINK_CODEC_NONE == 0 && XIAOTAI_LINK_CODEC_G711A == 1 && XIAOTAI_LINK_CODEC_OPUS == 2 &&
                   XIAOTAI_LINK_CODEC_JPEG == 3 && XIAOTAI_LINK_CODEC_H264 == 4,
               "wire codec values");
_Static_assert(XIAOTAI_LINK_CODEC_G711A == XIAOTAI_CODEC_G711A && XIAOTAI_LINK_CODEC_OPUS == XIAOTAI_CODEC_OPUS &&
                   XIAOTAI_LINK_CODEC_JPEG == XIAOTAI_CODEC_JPEG && XIAOTAI_LINK_CODEC_H264 == XIAOTAI_CODEC_H264,
               "link spellings are vocabulary values");

/* What the default audio receiver does with a frame (spec 3.6): every field
 * passes as it came, the codec included. */
static xiaotai_media_packet_t packet_of(const xiaotai_link_frame_t *frame, const uint8_t *data, size_t size)
{
    const xiaotai_media_packet_t packet = {
        .encoding = frame->format.codec, .rate_hz = frame->format.rate_hz, .timestamp_ms = frame->timestamp_ms,
        .stream = frame->stream, .data = data, .size = size};
    return packet;
}

/* A profile's audio format from a session's media format, unconverted. */
static xiaotai_link_format_t link_format_of(const xiaotai_media_format_t *format)
{
    const xiaotai_link_format_t link = {.codec = format->encoding, .rate_hz = format->rate_hz,
                                        .channels = format->channels};
    return link;
}

int main(void)
{
    /* Every audio value of the vocabulary, the ones appended after the former
     * wire list included, reaches the media packet as the same number. */
    static const xiaotai_codec_t audio[] = {XIAOTAI_CODEC_G711A, XIAOTAI_CODEC_OPUS, XIAOTAI_CODEC_PCM16,
                                            XIAOTAI_CODEC_G711U};
    static const int numbers[] = {1, 2, 5, 6};
    uint8_t payload[160];
    memset(payload, 0x55, sizeof(payload));
    for (size_t i = 0; i < sizeof(audio) / sizeof(audio[0]); ++i) {
        const xiaotai_link_frame_t frame = {.kind = XIAOTAI_LINK_AUDIO, .format = {audio[i], 8000U, 1U},
                                            .stream = 14U, .timestamp_ms = 4000U + (uint32_t)i * 20U};
        const xiaotai_media_packet_t packet = packet_of(&frame, payload, sizeof(payload));
        CHECK(XIAOTAI_CODEC_IS_AUDIO(packet.encoding));
        CHECK((int)packet.encoding == numbers[i] && packet.encoding == frame.format.codec);
        CHECK(packet.rate_hz == 8000U && packet.stream == 14U && packet.timestamp_ms == 4000U + (uint32_t)i * 20U);
        CHECK(packet.data == payload && packet.size == sizeof(payload));
    }
    /* A frame the stack could not describe arrives as NONE and stays NONE: it
     * is never mistaken for PCM16, which is 5. */
    const xiaotai_link_frame_t unknown = {.kind = XIAOTAI_LINK_AUDIO, .format = {XIAOTAI_LINK_CODEC_NONE, 8000U, 1U}};
    const xiaotai_media_packet_t dropped = packet_of(&unknown, payload, 1U);
    CHECK(dropped.encoding == XIAOTAI_CODEC_NONE && !XIAOTAI_CODEC_IS_AUDIO(dropped.encoding));
    CHECK(dropped.encoding != XIAOTAI_CODEC_PCM16);

    /* The session formats modules declare become link formats as they are. */
    const xiaotai_media_format_t voice = XIAOTAI_MEDIA_FORMAT_G711A_8K, chip = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
    const xiaotai_link_format_t voice_link = link_format_of(&voice), chip_link = link_format_of(&chip);
    CHECK(voice_link.codec == XIAOTAI_LINK_CODEC_G711A && voice_link.rate_hz == 8000U && voice_link.channels == 1U);
    CHECK(chip_link.codec == XIAOTAI_LINK_CODEC_OPUS && chip_link.rate_hz == 16000U && chip_link.channels == 1U);
    /* An unused direction is a zeroed, unused link format. */
    const xiaotai_media_format_t none = XIAOTAI_MEDIA_FORMAT_NONE;
    const xiaotai_link_format_t none_link = link_format_of(&none);
    xiaotai_link_format_t zero;
    memset(&zero, 0, sizeof(zero));
    CHECK(none_link.codec == zero.codec && none_link.rate_hz == 0U && none_link.channels == 0U);

    /* A part's encoding names the link format of what it captures: a voice
     * chip's Opus goes out as wire value 2. */
    const xiaotai_audio_desc_t chip_part = {.encoding = XIAOTAI_CODEC_OPUS, .rate_hz = 16000U, .channels = 1U,
                                            .roles = {XIAOTAI_CHANNEL_MIC}, .frame_ms = 20U};
    const xiaotai_link_format_t sent = {.codec = chip_part.encoding, .rate_hz = chip_part.rate_hz,
                                        .channels = chip_part.channels};
    CHECK((int)sent.codec == 2 && sent.codec == chip_link.codec);

    printf("PASS: link frames, media packets, formats and part encodings share one codec vocabulary\n");
    return 0;
}
