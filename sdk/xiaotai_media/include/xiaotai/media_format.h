// SPDX-License-Identifier: MIT
#pragma once
/* Network audio: the format a session exchanges with its peer, the packet the
 * transport hands to the media engine, and the sink the engine hands uplink
 * packets to. The transport implements the sink; modules describe sessions with
 * these types and never name a board, a codec chip or a processing rate.
 *
 * Encodings are the device's one codec vocabulary (xiaotai/codec.h), the same
 * values the audio parts (xiaotai/hal.h) and the links (xiaotai/link.h) use, so
 * a packet passes between them without conversion. Only audio values
 * (XIAOTAI_CODEC_IS_AUDIO) form a format: a used direction whose encoding is
 * NONE or a video codec is malformed. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/codec.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest encoded packet the engine stores or produces, in either direction. */
#define XIAOTAI_MEDIA_PACKET_BYTES_MAX 1500U
/* Longest packet a decoder is asked to decode at once (Opus allows 120 ms). */
#define XIAOTAI_MEDIA_PACKET_MS_MAX 120U

/* One direction of a session on the wire. rate_hz 0 means the direction is
 * unused; a zero-filled format is unused. */
typedef struct {
    xiaotai_codec_t encoding;   /* an audio codec; XIAOTAI_CODEC_NONE only when unused */
    uint32_t rate_hz;
    uint8_t channels;   /* 1; other counts are refused until a codec accepts them */
    uint8_t packet_ms;  /* audio per packet: what the engine sends, and the nominal downlink packet */
} xiaotai_media_format_t;

/* The platform's voice format: G.711 A-law, 8 kHz, mono, 20 ms (160 bytes). */
#define XIAOTAI_MEDIA_FORMAT_G711A_8K {XIAOTAI_CODEC_G711A, 8000U, 1U, 20U}
/* Opus, 16 kHz, mono, 20 ms: what an encoded-input voice chip produces. */
#define XIAOTAI_MEDIA_FORMAT_OPUS_16K {XIAOTAI_CODEC_OPUS, 16000U, 1U, 20U}
/* An unused direction. */
#define XIAOTAI_MEDIA_FORMAT_NONE {XIAOTAI_CODEC_NONE, 0U, 0U, 0U}

/* Default for xiaotai_media_sink_t.send_stack_bytes. */
#define XIAOTAI_MEDIA_SINK_STACK_DEFAULT 2048U

/* Where a session's uplink goes. The object and ctx have static lifetime: a
 * packet already being sent when the session closes still completes, so send()
 * receives the generation and drops what is stale.
 *
 * send() is an [uplink] call (xiaotai/link.h): the sink's own part never
 * blocks, allocates or logs, but it may enter a network stack that takes its
 * own locks and allocates inside its bounded send buffer. The engine therefore
 * calls send() from its uplink task or a separately selected executor, never
 * from a task that reads PCM from an input part or writes to an output part.
 * Capture hands it off through a short packet queue (2 to 4 packets
 * on a board without external RAM), or, for an input that delivers encoded
 * packets from a queue of its own, the uplink task reads that input itself.
 * A slow send() does not block capture or playout: the queue fills, further
 * packets are dropped and counted (tx_full), and capture and playout keep
 * their pace. */
typedef struct xiaotai_media_sink {
    /* [rt] True when a packet sent now would reach the peer: the link of this
     * generation is up and the peer takes the stream. Called from the task
     * that packetizes (which may be a capture task): never blocks, allocates
     * or logs. While false the engine neither encodes nor sends, and restarts
     * the packet timeline when it turns true again. NULL: always ready. */
    bool (*ready)(void *ctx, uint32_t generation);
    /* [uplink] One encoded packet of the session's uplink format; data is valid
     * during the call only. timestamp_ms is the media timeline: the local
     * millisecond clock at the first packet, then advancing by the audio sent
     * (capture gaps and pre-roll replay keep it equal to capture time).
     * XIAOTAI_OK: accepted. Anything else is counted (tx_send_errors) and not
     * retried. */
    xiaotai_err_t (*send)(void *ctx, uint32_t generation, uint32_t timestamp_ms, const uint8_t *data, size_t size);
    void *ctx;
    /* Stack one send() uses below its caller's frame, as measured on the
     * platform (the network stack included). The engine adds it to the stack
     * of its uplink task. 0: XIAOTAI_MEDIA_SINK_STACK_DEFAULT. */
    uint32_t send_stack_bytes;
} xiaotai_media_sink_t;

/* One downlink packet as received. data is copied by xiaotai_media_push.
 *
 * The transport hands packets over as the peer sent them. For an encoding
 * that is one byte or one word per sample (G.711, PCM) a packet may carry any
 * whole number of samples up to the byte limit (80, 160, 320, 640 bytes of
 * G.711 are all in use): the engine cuts it into pieces of the session's
 * nominal packet duration before it is stored, so buffering, pacing and
 * decoding see nominal packets whatever the peer chose, and the downlink only
 * ever stores nominal pieces of such encodings. A packet of a framed encoding
 * (Opus) is stored whole and must not exceed XIAOTAI_MEDIA_PACKET_MS_MAX. */
typedef struct {
    xiaotai_codec_t encoding;   /* as the link delivered it (xiaotai_link_codec_t is the same type) */
    uint32_t rate_hz;       /* 0: the session's downlink rate */
    uint32_t timestamp_ms;  /* sender's media timestamp */
    uint16_t stream;        /* sender's stream id; a change only restarts arrival statistics */
    const uint8_t *data;
    size_t size;            /* 1..XIAOTAI_MEDIA_PACKET_BYTES_MAX */
} xiaotai_media_packet_t;

#ifdef __cplusplus
}
#endif
