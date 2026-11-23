// SPDX-License-Identifier: MIT
#pragma once
/* Codec registry of the media engine: encoders and decoders, any number of
 * each. A session's network format, a sound's format and an encoded input are
 * all served by whichever registered codec matches; a new format is one more
 * registered codec, never a change to the engine or to this interface.
 *
 * A codec names its encoding with the device's one codec vocabulary
 * (xiaotai/codec.h): the value a format, a sound or an audio part's descriptor
 * carries, so nothing converts between lists and a new encoding is one value
 * appended there plus the codec registered here. Only audio values
 * (XIAOTAI_CODEC_IS_AUDIO) can be registered.
 *
 * G.711 A-law is portable and built in. Codecs that need a platform library
 * (Opus) are registered by the engine's platform directory when the build
 * selects them, and by fakes in host tests.
 *
 * Calls on one codec instance are serialized by the engine. encode(), decode()
 * and reset() run on real-time audio tasks [rt]: no allocation after open(), no
 * logging, no blocking. Descriptors have static lifetime. */
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/media_format.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xiaotai_audio_encoder {
    const char *name;                    /* "g711a", "opus" */
    xiaotai_codec_t encoding;            /* the audio codec produced */
    /* Stack the calling task needs inside encode(); the engine sizes the task
     * that encodes (its uplink task, or the single capture-and-playout task of
     * a board without external RAM) for the largest encoder any module
     * declared. */
    uint32_t stack_bytes;
    /* Score for producing this format; 0: cannot. The highest score wins, the
     * earlier registration on a tie. */
    int (*match)(const xiaotai_media_format_t *format);
    /* [owner, at engine start] One instance per declared uplink format, opened once and kept. */
    xiaotai_err_t (*open)(const xiaotai_media_format_t *format, void **state);
    /* samples is exactly one packet (format.packet_ms) of mono PCM at the
     * format's rate; the engine resamples before calling. *size receives the
     * packet length, at most capacity (XIAOTAI_MEDIA_PACKET_BYTES_MAX). */
    xiaotai_err_t (*encode)(void *state, const int16_t *pcm, size_t samples, uint8_t *out, size_t capacity,
                            size_t *size);
    /* Forget history; called when the session or the mute epoch changes. NULL: stateless. */
    xiaotai_err_t (*reset)(void *state);
    void (*close)(void *state);
    /* encode/reset are bounded and use no allocation, wait or device I/O.
     * Required for sharing the capture cadence with scheduled transmission. */
    bool capture_safe;
} xiaotai_audio_encoder_t;

typedef struct xiaotai_audio_decoder {
    const char *name;
    xiaotai_codec_t encoding;            /* the audio codec consumed */
    uint32_t stack_bytes;                /* stack needed inside decode() */
    int (*match)(const xiaotai_media_format_t *format);
    /* [rt, any task] Samples (at rate_hz) the packet carries, from its header
     * alone: pure, no state. 0: malformed, or longer than
     * XIAOTAI_MEDIA_PACKET_MS_MAX. Used on the transport callback to time a
     * packet before it is stored. */
    uint32_t (*packet_samples)(const uint8_t *data, size_t size, uint32_t rate_hz);
    /* [owner, at engine start] One instance per use (session downlink, sound playback), kept. */
    xiaotai_err_t (*open)(const xiaotai_media_format_t *format, void **state);
    /* One whole packet to mono PCM at the format's rate. Succeeds only when the
     * packet is consumed completely and *samples equals packet_samples();
     * XIAOTAI_ERR_INVALID_SIZE when pcm is too small. */
    xiaotai_err_t (*decode)(void *state, const uint8_t *data, size_t size, int16_t *pcm, size_t capacity,
                            size_t *samples);
    /* Forget history; called when the stream or the playout epoch changes. NULL: stateless. */
    xiaotai_err_t (*reset)(void *state);
    void (*close)(void *state);
} xiaotai_audio_decoder_t;

/* [init] Before the engine starts. XIAOTAI_ERR_INVALID_ARG for an incomplete
 * descriptor or an encoding that is not an audio codec (NONE, a video codec,
 * a value this build does not know), XIAOTAI_ERR_NO_MEM when the registry is
 * full, XIAOTAI_ERR_INVALID_STATE after start. */
xiaotai_err_t xiaotai_media_encoder_register(const xiaotai_audio_encoder_t *encoder);
xiaotai_err_t xiaotai_media_decoder_register(const xiaotai_audio_decoder_t *decoder);

#ifdef __cplusplus
}
#endif
