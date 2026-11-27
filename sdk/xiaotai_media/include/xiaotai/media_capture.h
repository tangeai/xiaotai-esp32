// SPDX-License-Identifier: MIT
#pragma once
/* Capture path of the media engine and its three extension points, each a
 * registry that grows by count:
 *
 *   processors  turn what the board's audio input delivers into clean audio.
 *               The engine picks one at start by the input and output
 *               descriptors (xiaotai/hal.h): two microphones plus a reference,
 *               one microphone plus a reference, a microphone alone (echo
 *               gate), an encoded input (pass-through). A capture path nothing
 *               matches needs one more processor, nothing else.
 *   effects     in-place stages on clean PCM: gain control, filters, gates.
 *   taps        consumers of clean PCM beside the uplink: the wake-word engine,
 *               diagnostics.
 *
 *   input part -> processor -> capture effects -> taps, pre-roll, recording
 *                                              -> packets -> uplink effects
 *                                              -> encoder -> session sink
 *
 * Portable implementations (echo gate, pass-through, high-pass, noise gate)
 * are built in. Implementations that need a platform library are registered by
 * the engine's platform directory when the build selects them, and by fakes in
 * host tests. Descriptors have static lifetime.
 *
 * Tags: [init] during init, before the engine starts; [rt] on the engine's
 * real-time capture or playout tasks: never allocate, never log, count
 * instead; [any] any task. Nothing here may be called from an interrupt. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/codec.h"
#include "xiaotai/hal.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- processors */

/* Largest clean PCM frame a processor may return. */
#define XIAOTAI_PROCESSOR_SAMPLES_MAX 1024U

enum {
    /* fetch() waits for work done on another task: the engine then feeds and
     * fetches on two tasks. Without it the engine feeds and fetches on one. */
    XIAOTAI_PROCESSOR_ASYNC = 1u << 0,
    /* No reference channel: far_end() must see every frame written to the output. */
    XIAOTAI_PROCESSOR_FAR_END = 1u << 1,
    /* Frames are encoded packets (the input is not PCM): no effects, taps,
     * pre-roll or recording; the uplink passes them through or transcodes.
     * The input part queues packets itself, so the engine reads it, feeds and
     * fetches on its uplink task, only while a session sends, and builds no
     * capture task. */
    XIAOTAI_PROCESSOR_ENCODED = 1u << 2,
    /* A reference-based canceller: both ends may talk at once at full level. */
    XIAOTAI_PROCESSOR_CANCELS_ECHO = 1u << 3,
    /* The processor itself attenuates the microphone between words during a
     * session (an echo gate). Effects that would gate again leave themselves
     * out (see xiaotai_audio_effect_t.open). */
    XIAOTAI_PROCESSOR_GATES = 1u << 4,
};

/* What open() tells the engine. */
typedef struct {
    uint32_t flags;          /* XIAOTAI_PROCESSOR_* */
    uint32_t rate_hz;        /* rate of the frames fetch() returns */
    /* PCM input: frames (all channels) the engine reads from the part and
     * passes to each feed(). Encoded input: 0, one packet per feed(). */
    uint16_t feed_frames;
    /* Largest frame fetch() returns: samples for PCM (at most
     * XIAOTAI_PROCESSOR_SAMPLES_MAX), bytes for encoded frames. */
    uint16_t fetch_max;
    /* Stack the feeding and the fetching task need inside the processor. */
    uint32_t feed_stack_bytes, fetch_stack_bytes;
} xiaotai_processor_info_t;

/* One clean frame. data belongs to the processor and stays valid until the
 * next fetch() or close(). */
typedef struct {
    xiaotai_codec_t encoding;          /* XIAOTAI_CODEC_PCM16: mono samples; otherwise one encoded packet */
    const void *data;
    size_t size;                       /* bytes */
    uint32_t samples;                  /* audio the frame carries, at info.rate_hz */
    int64_t captured_us;               /* capture time of the frame's END (xiaotai_time_us clock) */
    /* Samples lost right before this frame, at info.rate_hz: what feed() was
     * told (lost_frames) plus what the processor dropped itself (a full input
     * or ring). The engine skips the uplink timeline by this much, clears
     * delay lines, fails a running recording and the pre-roll, and resets the
     * taps. A synchronous processor may leave the reported loss to the
     * engine, which adds it to the frame after the loss. */
    uint32_t lost_samples;
    /* ASYNC processors: time spent on this frame on the processor's own task
     * (feeding included), in microseconds; 0: unknown. The engine times
     * synchronous processors itself. Either way a frame that took longer than
     * its own duration counts as a capture deadline miss. */
    uint32_t process_us;
} xiaotai_processor_frame_t;

/* Counters since open(), read by diagnostics from any task. */
typedef struct {
    uint32_t feeds, frames;
    uint32_t input_full;        /* feed() dropped a block: the processor's input was full */
    uint32_t lost_samples;
    uint32_t fetch_timeouts;
    uint32_t errors;
    uint32_t clipped_mic, clipped_reference; /* input samples at or beyond +-32700 */
    uint32_t process_max_us;    /* longest processing time of one frame */
    uint32_t deadline_misses;   /* frames that took longer than their own duration */
    uint32_t restarts;          /* cancellers replaced because they removed no echo */
    uint32_t closed_frames, double_talk_frames, open_frames; /* gating states; 0 for cancellers */
    /* Gating processors: the gain applied to the latest frame (32768: unity)
     * and the far-end and near-end noise floors they track (mean absolute
     * sample value). 0 when the processor does not track them. */
    uint16_t gain_q15, far_floor, near_floor;
    bool failed;                /* permanent failure: feed() and fetch() return XIAOTAI_ERR_INVALID_STATE */
} xiaotai_processor_stats_t;

typedef struct xiaotai_audio_processor {
    const char *name;   /* "aec.dual", "aec.single", "echo_gate", "passthrough", ... */
    /* Score for this capture path; 0: cannot handle it. output is NULL on a
     * board without an audio output. The highest score wins, the earlier
     * registration on a tie; a processor accepts every path it can serve and
     * scores how well (two microphones and a reference score above one
     * microphone and a reference, which scores above a microphone alone). */
    int (*match)(const xiaotai_audio_desc_t *input, const xiaotai_audio_desc_t *output);
    /* [owner, at engine start] The chosen parts themselves, so the processor
     * reads both descriptors and the board's facts in their info (echo
     * coupling, gain range and step, volume law, settle time) instead of
     * keeping constants of its own; output is NULL without an audio output.
     * Allocates everything (by purpose, through the memory port) and reads its
     * tuning with xiaotai_media_param. Runs before the engine opens the audio
     * parts, so no capture data arrives earlier; it never calls the parts. */
    xiaotai_err_t (*open)(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                          xiaotai_processor_info_t *info, void **state);
    /* [rt] One block as read from the part: info.feed_frames interleaved
     * frames, or one encoded packet. captured_us is when the read returned
     * (the capture time of the block's end). lost_frames: input frames (or
     * packets) lost right before this block, from a failed read or from the
     * part's overrun counters; the processor carries them into the
     * lost_samples of the frame that follows. Returns at once for an ASYNC
     * processor. XIAOTAI_ERR_NO_MEM: input full, block dropped and counted
     * (the next frame reports it in lost_samples). XIAOTAI_ERR_INVALID_STATE:
     * failed for good. */
    xiaotai_err_t (*feed)(void *state, const void *data, size_t size, int64_t captured_us, uint32_t lost_frames);
    /* [rt] The next clean frame. ASYNC: waits up to timeout_ms.
     * Otherwise returns what the last feed() produced and ignores timeout_ms.
     * XIAOTAI_ERR_TIMEOUT: nothing ready. */
    xiaotai_err_t (*fetch)(void *state, xiaotai_processor_frame_t *frame, uint32_t timeout_ms);
    /* [rt, playout task] Mono PCM about to reach the speaker, every frame
     * including silence written on underrun. May run concurrently with feed()
     * and fetch(); the processor guards the state they share. NULL unless
     * XIAOTAI_PROCESSOR_FAR_END. */
    void (*far_end)(void *state, const int16_t *pcm, size_t samples, uint32_t rate_hz);
    /* [rt] Output volume or input gain changed (and once after open). NULL when unused. */
    void (*levels)(void *state, uint8_t volume_percent, int8_t input_gain_db);
    /* [rt, the task that feeds] A session starts (true) or stops (false) using the
     * capture path. Starting forgets per-conversation state (an echo gate
     * closes again for its start-up time). A processor that gates
     * (XIAOTAI_PROCESSOR_GATES) attenuates only between a start and the
     * following stop; outside sessions (idle, recording, taps) it passes the
     * microphone through and keeps tracking the far end. NULL when unused. */
    void (*session)(void *state, bool active);
    /* [any] */
    void (*stats)(void *state, xiaotai_processor_stats_t *stats);
    void (*close)(void *state);
} xiaotai_audio_processor_t;

/* [init] XIAOTAI_ERR_INVALID_ARG for an incomplete descriptor (name, match,
 * open, feed, fetch and close are required), XIAOTAI_ERR_NO_MEM when the
 * registry is full, XIAOTAI_ERR_INVALID_STATE after start. */
xiaotai_err_t xiaotai_media_processor_register(const xiaotai_audio_processor_t *processor);

/* [any] The processor chosen at start (NULL: none, or not started) and its counters. */
const xiaotai_audio_processor_t *xiaotai_media_processor(void);
void xiaotai_media_processor_stats(xiaotai_processor_stats_t *stats);

/* ------------------------------------------------------------------ effects */

typedef enum {
    /* After the processor: the uplink, taps, pre-roll and recording all hear it. */
    XIAOTAI_EFFECT_CAPTURE = 0,
    /* On packets about to be encoded: only the far end hears it. */
    XIAOTAI_EFFECT_UPLINK,
} xiaotai_effect_stage_t;

typedef struct {
    /* Samples the effect holds back (it needs whole frames of its own size).
     * The engine moves capture timestamps by this much, so taps, pre-roll and
     * recording stay on the capture timeline. */
    uint16_t delay_samples;
    /* Stack process() needs. The engine adds the largest value of each stage
     * to the stack of the task that runs that stage. */
    uint32_t stack_bytes;
} xiaotai_effect_info_t;

typedef struct xiaotai_audio_effect {
    const char *name;             /* "highpass", "agc", "boost", "noise_gate", ... */
    xiaotai_effect_stage_t stage;
    uint8_t order;                /* lower runs first within its stage */
    /* XIAOTAI_MEDIA_* session flags that must all be set for the effect to run
     * in a session; 0: always. Capture-stage effects always run. */
    uint32_t session_flags;
    /* [owner, at engine start] processor is what the chosen processor's open()
     * reported (its output rate_hz and its flags). XIAOTAI_ERR_NOT_SUPPORTED
     * leaves the effect out without failing the start: its tuning disabled it,
     * the rate is not one it handles, or the processor already does its work
     * (a gate behind a gating processor). */
    xiaotai_err_t (*open)(const xiaotai_processor_info_t *processor, xiaotai_effect_info_t *info, void **state);
    /* [rt] In place, any number of samples. A failure drops the frame or packet
     * (counted) and is followed by reset(). */
    xiaotai_err_t (*process)(void *state, int16_t *pcm, size_t samples);
    /* [rt] Drop held samples and history. Capture-stage effects are reset
     * only when the capture timeline breaks (a capture gap, capture restarting
     * after an idle close) or after a failure: their state (filter history,
     * converged gain) carries across sessions and microphone mutes.
     * Uplink-stage effects are also reset when a session starts and when the
     * microphone mute changes. NULL: stateless. */
    void (*reset)(void *state);
    void (*close)(void *state);
} xiaotai_audio_effect_t;

/* [init] Same results as xiaotai_media_processor_register. */
xiaotai_err_t xiaotai_media_effect_register(const xiaotai_audio_effect_t *effect);

/* --------------------------------------------------------------------- taps */

enum {
    /* Audio from before the capture effects (echo-controlled, not gained). */
    XIAOTAI_TAP_RAW = 1u << 0,
    /* Also while a session is active. Without it the tap hears only the idle
     * device: no active session and no recording. */
    XIAOTAI_TAP_IN_SESSION = 1u << 1,
};

/* A tap never hears a muted microphone, whatever its flags. */
typedef struct xiaotai_media_tap {
    const char *name;
    uint32_t flags;     /* XIAOTAI_TAP_* */
    /* [rt, capture task] The tap's audio starts (true) or stops (false)
     * flowing; always called before the first pcm() of a run. After a capture
     * gap the engine closes and reopens the gate, so a tap never joins audio
     * across a gap. NULL when unused. */
    void (*gate)(void *ctx, bool open);
    /* [rt, capture task] One frame of mono PCM, valid during the call only:
     * copy and return. captured_us is the capture time of the frame's END,
     * already corrected for effect delay. rate_hz is
     * xiaotai_media_info().capture_rate_hz. */
    void (*pcm)(void *ctx, const int16_t *pcm, size_t samples, uint32_t rate_hz, int64_t captured_us);
    void *ctx;
} xiaotai_media_tap_t;

/* [init] Registered disabled. Registering succeeds on every board; one without
 * PCM capture simply never opens the gate. Same results as
 * xiaotai_media_processor_register. */
xiaotai_err_t xiaotai_media_tap_register(const xiaotai_media_tap_t *tap);
/* [any] The capture path keeps running while the device is idle as long as any
 * tap is enabled or a pre-roll token is held (xiaotai/media.h); with neither
 * it may stop and the input part may power down (power.idle_close_ms), and the
 * pre-roll history stops rolling with it. XIAOTAI_ERR_NOT_FOUND for an
 * unregistered tap. */
xiaotai_err_t xiaotai_media_tap_enable(const xiaotai_media_tap_t *tap, bool enabled);

#ifdef __cplusplus
}
#endif
