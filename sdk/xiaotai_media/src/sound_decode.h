// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_SOUND_DECODE_H
#define XIAOTAI_MEDIA_SOUND_DECODE_H

#include "xiaotai/media_codec.h"
#include "xiaotai/sounds.h"

typedef struct {
    xiaotai_media_format_t format; /* source rate, nominal packet duration, mono decoder output */
    const xiaotai_audio_decoder_t *decoder; /* NULL for portable PCM16 and A-law samples */
    const uint8_t *payload;
    size_t size, offset;
    uint8_t channels; /* interleaved source channels, before downmix */
    xiaotai_sound_container_t container;
} media_sound_cursor_t;

/* Validate the whole container and choose the decoder if one is needed. No
 * codec state is opened here. The clip data is borrowed and stays immutable. */
xiaotai_err_t media_sound_cursor_open(media_sound_cursor_t *cursor, const xiaotai_sound_t *sound);

/* One source-rate mono PCM block, or one whole framed packet. The engine owns
 * decoder_state, prepared for cursor->format off the real-time path. Success
 * with samples == 0 is EOF. On failure, samples is 0, cursor position is
 * unchanged and PCM output must be discarded. Abandon or reset a decoder on
 * decode failure; only a capacity refusal before decode can be retried as-is. */
xiaotai_err_t media_sound_cursor_read(media_sound_cursor_t *cursor, void *decoder_state,
                                      int16_t *pcm, size_t capacity, size_t *samples);

#endif
