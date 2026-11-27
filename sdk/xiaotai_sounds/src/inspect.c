// SPDX-License-Identifier: MIT
/* Container check of a sound (xiaotai_sounds_inspect).
 *
 * Reads the data in place and never plays, decodes, allocates or blocks, so it
 * runs on any task, on built-in sounds and on borrowed ones (a downloaded file
 * the caller describes). The size limit is checked before the first byte is
 * read: a caller's size above XIAOTAI_SOUND_BYTES_MAX is refused without
 * touching the data. */
#include <stdint.h>
#include "xiaotai/sounds.h"
#include "wav.h"

/* Largest packet of XIAOTAI_SOUND_PACKETS: the decoder's input buffer of the
 * framed prompt players this container replaces. */
#define SOUNDS_PACKET_BYTES_MAX 512U

/* Bytes of one sample of an encoding RAW can hold; 0 for framed encodings,
 * which have no fixed size per sample. */
static unsigned sample_bytes(xiaotai_codec_t encoding)
{
    switch (encoding) {
    case XIAOTAI_CODEC_PCM16:
        return 2;
    case XIAOTAI_CODEC_G711A:
    case XIAOTAI_CODEC_G711U:
        return 1;
    default:
        return 0;
    }
}

/* Rounded up to the next millisecond; saturates instead of wrapping for an
 * absurd caller rate. */
static uint32_t duration_ms(uint64_t frames, uint32_t rate_hz)
{
    uint64_t ms = (frames * 1000U + rate_hz - 1U) / rate_hz;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static xiaotai_err_t inspect_wav(const xiaotai_sound_t *sound, xiaotai_sound_info_t *info)
{
    if (sound->encoding != XIAOTAI_CODEC_PCM16) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_sounds_wav_t wav;
    xiaotai_err_t err = xiaotai_sounds_wav_parse(sound->data, sound->size, &wav);
    if (err != XIAOTAI_OK) return err;
    info->rate_hz = wav.sample_rate;
    info->channels = (uint8_t)wav.channels;
    info->frames = (uint32_t)wav.frames;
    info->duration_ms = duration_ms(wav.frames, wav.sample_rate);
    info->payload = wav.samples;
    info->payload_size = wav.frames * wav.channels * 2U;
    return XIAOTAI_OK;
}

static xiaotai_err_t inspect_raw(const xiaotai_sound_t *sound, xiaotai_sound_info_t *info)
{
    unsigned bytes = sample_bytes(sound->encoding);
    if (!bytes) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!sound->rate_hz || !sound->channels) return XIAOTAI_ERR_INVALID_ARG;
    size_t frame = (size_t)bytes * sound->channels;
    if (sound->size % frame) return XIAOTAI_ERR_INVALID_SIZE;
    size_t frames = sound->size / frame;
    info->rate_hz = sound->rate_hz;
    info->channels = sound->channels;
    info->frames = (uint32_t)frames;
    info->duration_ms = duration_ms(frames, sound->rate_hz);
    info->payload = sound->data;
    info->payload_size = sound->size;
    return XIAOTAI_OK;
}

/* Walks every length prefix: 1..512 and inside the data, as the framed prompt
 * player checked before decoding each packet. */
static xiaotai_err_t inspect_packets(const xiaotai_sound_t *sound, xiaotai_sound_info_t *info)
{
    if (!sound->rate_hz || !sound->channels || !sound->packet_ms) return XIAOTAI_ERR_INVALID_ARG;
    const uint8_t *cursor = sound->data;
    size_t remaining = sound->size;
    uint32_t packets = 0;
    while (remaining) {
        if (remaining < 2U) return XIAOTAI_ERR_INVALID_SIZE;
        size_t length = (size_t)cursor[0] | ((size_t)cursor[1] << 8U);
        cursor += 2;
        remaining -= 2U;
        if (length == 0U || length > SOUNDS_PACKET_BYTES_MAX || length > remaining)
            return XIAOTAI_ERR_INVALID_SIZE;
        cursor += length;
        remaining -= length;
        ++packets;
    }
    info->rate_hz = sound->rate_hz;
    info->channels = sound->channels;
    info->frames = packets;
    /* At most XIAOTAI_SOUND_BYTES_MAX / 3 packets of at most 255 ms: no overflow. */
    info->duration_ms = packets * (uint32_t)sound->packet_ms;
    info->payload = sound->data;
    info->payload_size = sound->size;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_sounds_inspect(const xiaotai_sound_t *sound, xiaotai_sound_info_t *info)
{
    if (info) *info = (xiaotai_sound_info_t){0};
    if (!sound || !info || !sound->data || !sound->size) return XIAOTAI_ERR_INVALID_ARG;
    if (sound->size > XIAOTAI_SOUND_BYTES_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    if (!XIAOTAI_CODEC_IS_AUDIO(sound->encoding)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_err_t err;
    switch (sound->container) {
    case XIAOTAI_SOUND_RAW:
        err = inspect_raw(sound, info);
        break;
    case XIAOTAI_SOUND_WAV:
        err = inspect_wav(sound, info);
        break;
    case XIAOTAI_SOUND_PACKETS:
        err = inspect_packets(sound, info);
        break;
    default:
        err = XIAOTAI_ERR_NOT_SUPPORTED;
        break;
    }
    if (err != XIAOTAI_OK) *info = (xiaotai_sound_info_t){0};
    return err;
}
