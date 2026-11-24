// SPDX-License-Identifier: MIT
#pragma once
/* Fixed-frame delay line: feeds a stream of blocks of any length to a
 * processing step that only takes whole frames of a fixed size (a platform
 * gain control needs exactly 10 ms), without padding partial frames and
 * without breaking the sample timeline across blocks.
 *
 * The output is the processed stream delayed by exactly one frame, the first
 * frame being silence; an effect built on it declares that frame as its
 * delay_samples, and the engine moves the capture timestamps back by it. A
 * failed frame clears the line (history and the processed frame) and the call
 * reports the failure; the effect then drops the block and is reset.
 *
 * The caller owns the storage (two frames of samples, placed by purpose), so
 * processing never allocates: real-time safe on the task that owns the line. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Processes one whole frame of frame samples. input is scratch the step may
 * modify; output receives the processed frame. false: the frame failed. */
typedef bool (*xiaotai_media_frame_fn)(void *ctx, int16_t *input, int16_t *output, size_t frame);

typedef struct {
    int16_t *input;   /* the frame being collected */
    int16_t *output;  /* the last processed frame, being played out */
    size_t frame;     /* samples per frame */
    size_t position;  /* samples of the current frame collected */
} xiaotai_media_frame_stream_t;

/* storage holds 2 * frame samples; it is cleared, so the first frame out is silence. */
static inline void xiaotai_media_frame_stream_init(xiaotai_media_frame_stream_t *stream, int16_t *storage,
                                                   size_t frame)
{
    stream->input = storage;
    stream->output = storage + frame;
    stream->frame = frame;
    stream->position = 0;
    memset(storage, 0, 2U * frame * sizeof(int16_t));
}

/* Forgets the history: the next frame out is silence again. */
static inline void xiaotai_media_frame_stream_reset(xiaotai_media_frame_stream_t *stream)
{
    memset(stream->input, 0, stream->frame * sizeof(int16_t));
    memset(stream->output, 0, stream->frame * sizeof(int16_t));
    stream->position = 0;
}

/* Passes samples through the line: output[i] is the processed sample from one
 * frame earlier. input and output may be the same buffer (each input sample
 * is read before the output sample at its index is written). Returns false,
 * with the line cleared, when a frame failed; the output written before the
 * failure is valid, the rest of the block is not. */
static inline bool xiaotai_media_frame_stream_process(xiaotai_media_frame_stream_t *stream, const int16_t *input,
                                                      int16_t *output, size_t samples, xiaotai_media_frame_fn process,
                                                      void *ctx)
{
    for (size_t i = 0; i < samples; ++i) {
        const int16_t sample = input[i];
        output[i] = stream->output[stream->position];
        stream->input[stream->position] = sample;
        if (++stream->position == stream->frame) {
            stream->position = 0;
            if (!process(ctx, stream->input, stream->output, stream->frame)) {
                xiaotai_media_frame_stream_reset(stream);
                return false;
            }
        }
    }
    return true;
}

#ifdef __cplusplus
}
#endif
