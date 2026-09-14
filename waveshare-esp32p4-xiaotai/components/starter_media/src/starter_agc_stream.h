#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ESP-SR's WebRTC AGC requires exactly 10 ms. The capture/AEC block is 32 ms.
 * Preserve the sample timeline across blocks; never pad each partial frame.
 * Output has one fixed 10 ms delay, including a single initial silent frame. */
#define STARTER_AGC_SAMPLES 160U
typedef struct {
    int16_t input[STARTER_AGC_SAMPLES];
    int16_t output[STARTER_AGC_SAMPLES];
    size_t position;
} starter_agc_stream_t;
typedef bool (*starter_agc_frame_fn)(void *, int16_t *, int16_t *);

static inline bool starter_agc_stream_process(starter_agc_stream_t *stream,
    const int16_t *input, int16_t *output, size_t samples,
    starter_agc_frame_fn process, void *context)
{
    for (size_t i = 0; i < samples; ++i) {
        stream->input[stream->position] = input[i];
        output[i] = stream->output[stream->position];
        if (++stream->position == STARTER_AGC_SAMPLES) {
            stream->position = 0;
            if (!process(context, stream->input, stream->output)) {
                memset(stream, 0, sizeof(*stream));
                return false;
            }
        }
    }
    return true;
}
