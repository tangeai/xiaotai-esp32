// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_PLAY_STREAM_H
#define XIAOTAI_MEDIA_PLAY_STREAM_H

#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"

typedef struct media_stream_buffer media_stream_buffer_t;
typedef enum {
    MEDIA_STREAM_DATA,
    MEDIA_STREAM_STARVED,
    MEDIA_STREAM_END,
    MEDIA_STREAM_TIMEOUT,
    MEDIA_STREAM_REVOKED,
} media_stream_read_t;

/* The registry instance belongs to the playback pool; only buffer and scratch
 * are provisioned here, before publication. Each writer/owner holds a ref. */
xiaotai_err_t media_stream_buffer_create(const xiaotai_media_format_t *format,
                                         const xiaotai_audio_decoder_t *decoder, media_stream_buffer_t **buffer);
void media_stream_buffer_retain(media_stream_buffer_t *buffer);
void media_stream_buffer_release(media_stream_buffer_t *buffer); /* non-RT */
void media_stream_buffer_revoke(media_stream_buffer_t *buffer); /* atomic, no wake or allocation */
void media_stream_buffer_notify(media_stream_buffer_t *buffer);
const xiaotai_media_format_t *media_stream_buffer_format(const media_stream_buffer_t *buffer);
const xiaotai_audio_decoder_t *media_stream_buffer_decoder(const media_stream_buffer_t *buffer);

xiaotai_err_t media_stream_buffer_write(media_stream_buffer_t *buffer, const void *data, size_t size,
                                        uint32_t timeout_ms);
xiaotai_err_t media_stream_buffer_finish(media_stream_buffer_t *buffer);

/* Output worker only. Borrowed scratch stays valid until its next read. A
 * raw read is at most raw_limit frames; framed reads preserve packet bounds. */
media_stream_read_t media_stream_buffer_read(media_stream_buffer_t *buffer, size_t raw_limit,
                                             const uint8_t **data, size_t *bytes, size_t *samples);
int16_t *media_stream_buffer_pcm(media_stream_buffer_t *buffer, size_t *capacity);

#endif
