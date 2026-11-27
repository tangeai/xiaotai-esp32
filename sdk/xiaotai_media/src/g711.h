// SPDX-License-Identifier: MIT
#pragma once
/* G.711 A-law (ITU-T G.711, the values of its reference software G.191),
 * portable table code. The engine's built-in codec: every session format of
 * XIAOTAI_CODEC_G711A, at any rate, is served by the two descriptors below
 * without a registration, and sounds stored as A-law decode through the same
 * functions.
 *
 * The sample functions are pure and allocation free: safe on every task,
 * real-time paths included. */
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/media_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stack the codec calls need below the caller's frame (table look-ups only). */
#define XIAOTAI_MEDIA_G711_STACK_BYTES 512U

/* One 16-bit linear sample to its A-law code. Only the 12 most significant
 * bits count, as in the standard: values that share them share a code. */
uint8_t xiaotai_media_alaw_encode_sample(int16_t pcm);
/* One A-law code to the centre of its quantization interval (16-bit scale). */
int16_t xiaotai_media_alaw_decode_sample(uint8_t code);

/* samples linear samples to as many codes; pcm and alaw may not overlap. */
void xiaotai_media_alaw_encode(const int16_t *pcm, size_t samples, uint8_t *alaw);
/* size codes to as many linear samples; alaw and pcm may not overlap. */
void xiaotai_media_alaw_decode(const uint8_t *alaw, size_t size, int16_t *pcm);

/* The built-in codec descriptors (name "g711a"). match() scores 1 for a mono
 * A-law format of any non-zero rate, so a registered implementation with a
 * higher score takes precedence. Instances are stateless: open() allocates
 * nothing and close() frees nothing. A packet carries one sample per byte;
 * packet_samples() and decode() refuse a packet longer than
 * XIAOTAI_MEDIA_PACKET_MS_MAX at the instance's rate, so they always agree. */
extern const xiaotai_audio_encoder_t xiaotai_media_g711a_encoder;
extern const xiaotai_audio_decoder_t xiaotai_media_g711a_decoder;

#ifdef __cplusplus
}
#endif
