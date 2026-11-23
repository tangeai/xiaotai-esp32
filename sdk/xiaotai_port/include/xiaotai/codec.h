// SPDX-License-Identifier: MIT
#pragma once
/* Codec identity: the one list of encodings for audio and video. Part
 * descriptors (xiaotai/hal.h), the media engine, the transport's link
 * profiles and frames, and the video engine all name an encoding with these
 * values, so a new encoding is one more line here plus the encoder, decoder
 * or framing that handles it, and no component converts between lists. It is
 * a vocabulary like xiaotai/needs.h and is published by the port component,
 * on which everything is built, so none of those components depends on
 * another for it.
 *
 * A value names a format only. Whether a build can capture, encode, decode,
 * frame or play it is the fact of the component that would do so: it refuses
 * a format it lacks with XIAOTAI_ERR_NOT_SUPPORTED.
 *
 * Values are appended and never change. The first ones equal the transport's
 * former wire codec list (xiaotai_link_codec_t), so that list becomes an alias
 * without renumbering. */

typedef enum {
    XIAOTAI_CODEC_NONE = 0,   /* unknown or absent; never a valid stream format */
    XIAOTAI_CODEC_G711A = 1,  /* audio: ITU-T G.711 A-law, one byte per sample */
    XIAOTAI_CODEC_OPUS = 2,   /* audio: one Opus packet per frame */
    XIAOTAI_CODEC_JPEG = 3,   /* video: one complete JPEG per frame; every frame decodes alone */
    XIAOTAI_CODEC_H264 = 4,   /* video: one Annex-B access unit per frame */
    XIAOTAI_CODEC_PCM16 = 5,  /* audio: interleaved signed 16-bit samples, native byte order */
    XIAOTAI_CODEC_G711U = 6,  /* audio: ITU-T G.711 mu-law, one byte per sample */
} xiaotai_codec_t;

/* Audio or video, for the values above; NONE and values this build does not
 * know are neither. Constant expressions, usable in tables. */
#define XIAOTAI_CODEC_IS_AUDIO(codec) \
    ((codec) == XIAOTAI_CODEC_G711A || (codec) == XIAOTAI_CODEC_OPUS || (codec) == XIAOTAI_CODEC_PCM16 || \
     (codec) == XIAOTAI_CODEC_G711U)
#define XIAOTAI_CODEC_IS_VIDEO(codec) ((codec) == XIAOTAI_CODEC_JPEG || (codec) == XIAOTAI_CODEC_H264)
