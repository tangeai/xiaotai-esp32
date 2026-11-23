// SPDX-License-Identifier: MIT
#pragma once
/* Sample-rate conversion of mono PCM16, integer arithmetic only (no FPU, no
 * library, no allocation). Three converters, each a caller-owned object with
 * the whole history inside, so the caller places it by purpose
 * (XIAOTAI_MEDIA_BULK) and nothing allocates per block:
 *
 *   down2  halves the rate: 31-tap low-pass and decimation, the uplink path
 *          from the processing rate to the network rate (16 kHz to 8 kHz).
 *   up2    doubles the rate: 63-tap half-band interpolation, the playout path
 *          from the network rate to the output rate (8 kHz to 16 kHz).
 *   rate   any source rate from 8000 to 48000 Hz, mono or stereo little-endian
 *          PCM16 as stored in a WAV data chunk, to an output rate that has a
 *          low-pass table here (8000 or 16000 Hz): cubic interpolation to
 *          48 kHz, then a windowed-sinc low-pass decimating to the output.
 *
 * Every converter keeps its state across calls: converting a stream in pieces
 * of any size gives exactly the samples of converting it at once. Calls on one
 * object are not synchronized; each object has a single owner task. All
 * process calls are real-time safe. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ down2 */

#define XIAOTAI_MEDIA_DOWN2_TAPS 31U

typedef struct {
    int16_t delay[XIAOTAI_MEDIA_DOWN2_TAPS];
    uint8_t write_index;
    bool emit_phase;   /* true: the next input sample is dropped after filtering */
    bool initialized;  /* false: the next sample first fills the whole history */
} xiaotai_media_down2_t;

/* Forget the history: the next sample fills it, so a new stream starts
 * without an onset transient. Call when the session or the mute epoch changes. */
void xiaotai_media_down2_reset(xiaotai_media_down2_t *down2);
/* Converts count samples (even) into count / 2. Returns the samples written;
 * 0, with the state untouched, for an odd count, a capacity below count / 2
 * or a NULL argument. */
size_t xiaotai_media_down2_process(xiaotai_media_down2_t *down2, const int16_t *input, size_t count,
                                   int16_t *output, size_t capacity);

/* -------------------------------------------------------------------- up2 */

#define XIAOTAI_MEDIA_UP2_HISTORY 32U
/* Input samples a natural end flushes, and the output samples that gives. */
#define XIAOTAI_MEDIA_UP2_TAIL_INPUT 31U
#define XIAOTAI_MEDIA_UP2_TAIL_SAMPLES (2U * XIAOTAI_MEDIA_UP2_TAIL_INPUT)
/* Output delay, in output samples: 31 / 16000 s at 16 kHz output. */
#define XIAOTAI_MEDIA_UP2_DELAY_SAMPLES 31U

typedef struct {
    int16_t history[XIAOTAI_MEDIA_UP2_HISTORY]; /* ring, newest at newest */
    uint8_t newest;
    bool initialized;
    bool pending_tail;  /* input since the last finish: a tail can be flushed */
    uint32_t clipped;   /* output samples saturated at the 16-bit limits since reset */
} xiaotai_media_up2_t;

/* Starts a stream: the history is filled with first (the stream's first
 * sample, or 0), so the output starts without a step from silence. Clears the
 * clip counter. */
void xiaotai_media_up2_reset(xiaotai_media_up2_t *up2, int16_t first);
/* Converts count samples into exactly 2 * count. Returns the samples written;
 * 0 when not reset, for count 0, a capacity below 2 * count or a NULL
 * argument. Overshoot beyond the 16-bit range saturates and is counted. */
size_t xiaotai_media_up2_process(xiaotai_media_up2_t *up2, const int16_t *input, size_t count, int16_t *output,
                                 size_t capacity);
/* At a natural end or gap of the stream only: writes the
 * XIAOTAI_MEDIA_UP2_TAIL_SAMPLES samples still inside the filter. Returns 0
 * when nothing was converted since the last finish, or for a capacity below
 * the tail. A cancelled or superseded stream is reset, never finished. */
size_t xiaotai_media_up2_finish(xiaotai_media_up2_t *up2, int16_t *output, size_t capacity);

/* ------------------------------------------------------------------- rate */

#define XIAOTAI_MEDIA_RATE_MIN_HZ 8000U
#define XIAOTAI_MEDIA_RATE_MAX_HZ 48000U
/* Intermediate rate of the cubic interpolation. */
#define XIAOTAI_MEDIA_RATE_MID_HZ 48000U
/* Longest low-pass table (the 8 kHz output). */
#define XIAOTAI_MEDIA_RATE_TAPS_MAX 192U
/* Converted samples a stream carries at most beyond frames * out / in: the
 * interpolation's flush (up to 4 source samples) and the filter's tail (every
 * table spans 32 output samples) plus the last partial group, all kept. */
#define XIAOTAI_MEDIA_RATE_EXTRA_MAX(out_hz) (4U * (out_hz) / XIAOTAI_MEDIA_RATE_MIN_HZ + 33U)

typedef struct {
    const int32_t *taps;  /* low-pass at 48 kHz for the output rate */
    uint16_t tap_count;
    uint8_t decimation;   /* 48 kHz samples per output sample */
    uint8_t channels;     /* source frame: 1 or 2 little-endian PCM16 samples */
    uint8_t stage;        /* converting, flushing the interpolation, flushing the filter, done */
    uint8_t tail_inputs;  /* zero source samples the interpolation flush still pushes */
    uint8_t group;        /* 48 kHz samples entered since the last output */
    bool copy;            /* source rate equals output rate: frames pass through */
    bool started;         /* at least one source frame was taken */
    uint32_t in_hz;
    uint32_t phase;       /* next 48 kHz instant past window[1], in 1/48000 of a source sample */
    uint16_t tail_zeros;  /* zero 48 kHz samples the filter flush still enters */
    uint16_t delay_pos;   /* next slot of delay */
    int32_t window[4];    /* last four source samples, oldest first, at twice the sample value */
    int32_t delay[XIAOTAI_MEDIA_RATE_TAPS_MAX]; /* 48 kHz samples at four times the sample value */
} xiaotai_media_rate_t;

/* Session workers reserve this bank only when their prepared formats need it. */
typedef struct {
    xiaotai_media_up2_t up;
    xiaotai_media_down2_t down;
    xiaotai_media_rate_t rate;
} media_resampler_storage_t;

/* Whether a source rate converts to an output rate here: equal rates of 8000
 * to 48000 Hz pass through, and any source rate in that range converts to an
 * output rate with a low-pass table (8000, 16000 Hz). */
bool xiaotai_media_rate_supported(uint32_t in_hz, uint32_t out_hz);
/* Prepares a conversion. XIAOTAI_ERR_INVALID_ARG for a NULL object or a
 * channel count other than 1 or 2, XIAOTAI_ERR_NOT_SUPPORTED for a rate pair
 * xiaotai_media_rate_supported() refuses. */
xiaotai_err_t xiaotai_media_rate_open(xiaotai_media_rate_t *rate, uint32_t in_hz, uint8_t channels,
                                      uint32_t out_hz);
/* Converts up to count source frames (little-endian PCM16, channels samples
 * each, any alignment; two channels are averaged). Stops when capacity output
 * samples are written or the frames are used up; *consumed receives the
 * frames taken, and the caller passes the rest again. Returns the samples
 * written; 0 with nothing consumed after finish or before open. */
size_t xiaotai_media_rate_process(xiaotai_media_rate_t *rate, const uint8_t *frames, size_t count,
                                  size_t *consumed, int16_t *output, size_t capacity);
/* After the last frame: writes what the interpolation and the filter still
 * hold (the causal filter's tail, not cropped). Call until it returns 0;
 * then the stream is complete. */
size_t xiaotai_media_rate_finish(xiaotai_media_rate_t *rate, int16_t *output, size_t capacity);

/* ------------------------------------------------------------------- plan */

typedef enum {
    XIAOTAI_MEDIA_RESAMPLE_NONE = 0, /* not convertible here */
    XIAOTAI_MEDIA_RESAMPLE_COPY,     /* equal rates */
    XIAOTAI_MEDIA_RESAMPLE_UP2,      /* out = 2 * in */
    XIAOTAI_MEDIA_RESAMPLE_DOWN2,    /* in = 2 * out */
    XIAOTAI_MEDIA_RESAMPLE_RATE,     /* any other supported pair */
} xiaotai_media_resample_t;

/* Converter for mono PCM at in_hz played or sent at out_hz: the exact factor
 * of two where it applies, the general converter otherwise. */
xiaotai_media_resample_t xiaotai_media_resample_plan(uint32_t in_hz, uint32_t out_hz);

#ifdef __cplusplus
}
#endif
