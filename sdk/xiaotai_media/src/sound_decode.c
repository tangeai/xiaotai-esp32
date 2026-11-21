// SPDX-License-Identifier: MIT
#include "sound_decode.h"

#include "g711.h"
#include "registry.h"
#include "resample.h"

#define SOUND_PACKET_BYTES_MAX 512U
#define SOUND_RAW_PACKET_MS 20U

xiaotai_err_t media_sound_cursor_open(media_sound_cursor_t *cursor, const xiaotai_sound_t *sound)
{
    if (!cursor) return XIAOTAI_ERR_INVALID_ARG;
    *cursor = (media_sound_cursor_t){0};
    xiaotai_sound_info_t info;
    const xiaotai_err_t err = xiaotai_sounds_inspect(sound, &info);
    if (err != XIAOTAI_OK) return err;
    if (!info.payload || !info.payload_size) return XIAOTAI_ERR_INVALID_SIZE;
    if (info.rate_hz < XIAOTAI_MEDIA_RATE_MIN_HZ || info.rate_hz > XIAOTAI_MEDIA_RATE_MAX_HZ ||
        (info.channels != 1 && info.channels != 2)) return XIAOTAI_ERR_NOT_SUPPORTED;

    const bool framed = sound->container == XIAOTAI_SOUND_PACKETS;
    const bool pcm = sound->encoding == XIAOTAI_CODEC_PCM16;
    const bool alaw = sound->encoding == XIAOTAI_CODEC_G711A;
    const bool ulaw = sound->encoding == XIAOTAI_CODEC_G711U;
    if (framed && (pcm || alaw || ulaw || sound->packet_ms > XIAOTAI_MEDIA_PACKET_MS_MAX))
        return XIAOTAI_ERR_NOT_SUPPORTED;

    const xiaotai_media_format_t format = {
        .encoding = sound->encoding, .rate_hz = info.rate_hz, .channels = 1,
        .packet_ms = framed ? sound->packet_ms : SOUND_RAW_PACKET_MS,
    };
    const xiaotai_audio_decoder_t *decoder = NULL;
    if (!pcm && !alaw) {
        if (info.channels != 1) return XIAOTAI_ERR_NOT_SUPPORTED;
        decoder = media_select_decoder(&format);
        if (!decoder) return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    *cursor = (media_sound_cursor_t){.format = format, .decoder = decoder, .payload = info.payload,
                                   .size = info.payload_size, .channels = info.channels, .container = sound->container};
    return XIAOTAI_OK;
}

static int32_t sample_le16(const uint8_t *sample)
{
    const uint16_t value = (uint16_t)((uint16_t)sample[0] | (uint16_t)sample[1] << 8);
    return value <= INT16_MAX ? value : (int32_t)value - 65536;
}

static int16_t average(int32_t left, int32_t right)
{
    const int32_t sum = left + right;
    return (int16_t)((sum + (sum < 0 ? -1 : 1)) / 2);
}

static void samples_read(const media_sound_cursor_t *cursor, const uint8_t *input, int16_t *pcm, size_t count)
{
    const bool alaw = cursor->format.encoding == XIAOTAI_CODEC_G711A;
    if (alaw && cursor->channels == 1) {
        xiaotai_media_alaw_decode(input, count, pcm);
        return;
    }
    const unsigned bytes = alaw ? 1U : 2U;
    const size_t frame_bytes = bytes * cursor->channels;
    for (size_t i = 0; i < count; ++i, input += frame_bytes) {
        const int32_t left = alaw ? xiaotai_media_alaw_decode_sample(input[0]) : sample_le16(input);
        if (cursor->channels == 1) {
            pcm[i] = (int16_t)left;
        } else {
            const int32_t right = alaw ? xiaotai_media_alaw_decode_sample(input[1]) : sample_le16(input + bytes);
            pcm[i] = average(left, right);
        }
    }
}

static xiaotai_err_t decode_packet(media_sound_cursor_t *cursor, void *state, size_t prefix, size_t bytes,
                                  int16_t *pcm, size_t capacity, size_t *samples)
{
    const uint8_t *packet = cursor->payload + cursor->offset + prefix;
    const uint32_t expected = cursor->decoder->packet_samples(packet, bytes, cursor->format.rate_hz);
    const uint32_t max_frames = cursor->format.rate_hz * XIAOTAI_MEDIA_PACKET_MS_MAX / 1000U;
    if (!expected || expected > max_frames || expected > capacity || (!prefix && expected != bytes))
        return XIAOTAI_ERR_INVALID_SIZE;

    size_t written = 0;
    const xiaotai_err_t err = cursor->decoder->decode(state, packet, bytes, pcm, capacity, &written);
    if (err != XIAOTAI_OK) return err;
    if (written != expected) return XIAOTAI_FAIL;
    cursor->offset += prefix + bytes;
    *samples = written;
    return XIAOTAI_OK;
}

xiaotai_err_t media_sound_cursor_read(media_sound_cursor_t *cursor, void *decoder_state,
                                      int16_t *pcm, size_t capacity, size_t *samples)
{
    if (!samples) return XIAOTAI_ERR_INVALID_ARG;
    *samples = 0;
    if (!cursor || (!pcm && capacity)) return XIAOTAI_ERR_INVALID_ARG;
    if (!cursor->payload || cursor->offset > cursor->size) return XIAOTAI_ERR_INVALID_STATE;
    const size_t remaining = cursor->size - cursor->offset;
    if (!remaining) return XIAOTAI_OK;
    if (!capacity) return XIAOTAI_ERR_INVALID_SIZE;

    const uint8_t *input = cursor->payload + cursor->offset;
    if (cursor->container == XIAOTAI_SOUND_PACKETS) {
        if (remaining < 2U) return XIAOTAI_ERR_INVALID_SIZE;
        const size_t bytes = (size_t)input[0] | (size_t)input[1] << 8;
        if (!bytes || bytes > SOUND_PACKET_BYTES_MAX || bytes > remaining - 2U) return XIAOTAI_ERR_INVALID_SIZE;
        return decode_packet(cursor, decoder_state, 2U, bytes, pcm, capacity, samples);
    }
    if (cursor->decoder) {
        /* RAW mu-law is one byte per sample. Bound each call to the rate of
         * the reusable decoder instance and allow a final partial block. */
        size_t count = cursor->format.rate_hz * SOUND_RAW_PACKET_MS / 1000U;
        if (count > remaining) count = remaining;
        if (count > capacity) count = capacity;
        return decode_packet(cursor, decoder_state, 0, count, pcm, capacity, samples);
    }
    const size_t frame_bytes = cursor->channels * (cursor->format.encoding == XIAOTAI_CODEC_PCM16 ? 2U : 1U);
    const size_t frames = remaining / frame_bytes;
    const size_t count = frames < capacity ? frames : capacity;
    samples_read(cursor, input, pcm, count);
    cursor->offset += count * frame_bytes;
    *samples = count;
    return XIAOTAI_OK;
}
