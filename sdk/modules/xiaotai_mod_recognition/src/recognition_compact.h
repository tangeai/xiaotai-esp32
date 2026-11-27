// SPDX-License-Identifier: MIT
#pragma once
/* Speech that does not fit in memory as it comes (a PCM16 WAV of a few hundred
 * KB on a board short of external RAM) is kept as G.711 A-law while it is
 * downloaded: half the bytes, the same playback. The kept form is
 * RC_COMPACT_MAGIC, the rate (4 bytes, little endian), the channels (1 byte),
 * 3 bytes of padding, then the A-law samples; rc_playback_wav() knows it. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaotai/port/err.h"

#define RC_COMPACT_MAGIC "XTA1"
#define RC_COMPACT_HEAD 12U
#define RC_COMPACT_WAV_HEAD_MAX 512U

typedef struct {
    uint8_t head[RC_COMPACT_WAV_HEAD_MAX];  /* the WAV's chunks before its samples */
    size_t head_size;
    size_t left;            /* sample bytes of the data chunk still to come */
    uint32_t rate;
    uint8_t channels, odd;
    bool in_data, has_odd;
} rc_compact_t;

/* The A-law bytes a WAV of `wav_bytes` needs at most, with the head. */
size_t rc_compact_capacity(size_t wav_bytes);
/* Feeds the next bytes of the WAV; what they hold goes to out[*stored...],
 * never past capacity (XIAOTAI_ERR_INVALID_SIZE). RC_INVALID_RESPONSE-like
 * XIAOTAI_FAIL: not a PCM16 WAV this can keep. */
xiaotai_err_t rc_compact_feed(rc_compact_t *compact, const uint8_t *data, size_t size, uint8_t *out, size_t capacity,
                              size_t *stored);
/* The samples began (the head was whole). */
bool rc_compact_started(const rc_compact_t *compact);
/* G.711 A-law of one sample. */
uint8_t rc_alaw(int16_t sample);
