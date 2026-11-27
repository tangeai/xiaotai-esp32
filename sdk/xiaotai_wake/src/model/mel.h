// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAKE_MEL_RATE 16000U
#define WAKE_MEL_FFT_SIZE 512U
#define WAKE_MEL_FFT_FLOATS (2U * WAKE_MEL_FFT_SIZE)
#define WAKE_MEL_FFT_ALIGN 16U
#define WAKE_MEL_HOP 160U
#define WAKE_MEL_BINS 32U
#define WAKE_MEL_FRAMES 98U
/* The model's one-hop input margin stays in the window contract. */
#define WAKE_MEL_SAMPLES (WAKE_MEL_FRAMES * WAKE_MEL_HOP + WAKE_MEL_FFT_SIZE)

/* In place, interleaved real and imaginary float32, unnormalized forward FFT
 * in natural bin order on success. The caller owns preparation and teardown. */
typedef int (*wake_fft_fn)(void *ctx, float *complex_pairs, size_t n);

typedef struct {
    float *fft_scratch;
    wake_fft_fn fft;
    void *fft_ctx;
    uint16_t first[WAKE_MEL_BINS], last[WAKE_MEL_BINS];
} wake_mel_t;

typedef struct {
    /* Cache unnormalized log powers; top_db depends on the current window. */
    float powers[WAKE_MEL_FRAMES][WAKE_MEL_BINS];
    uint64_t end_sample;
    uint32_t generation;
    bool valid;
} wake_mel_stream_t;

/* One owner per front end. Scratch is borrowed, 16-byte aligned, 4096 bytes;
 * the built-in model supplies it from BULK along with features and stream. */
int wake_mel_init(wake_mel_t *mel, float *fft_scratch, wake_fft_fn fft, void *fft_ctx);
int wake_mel_full(wake_mel_t *mel, const int16_t *pcm,
                  float out[WAKE_MEL_FRAMES][WAKE_MEL_BINS]);
int wake_mel_stream(wake_mel_t *mel, wake_mel_stream_t *stream, const int16_t *pcm,
                    uint32_t generation, uint64_t end_sample,
                    float out[WAKE_MEL_FRAMES][WAKE_MEL_BINS], uint32_t *reused_frames);

/* Host reference transform. ESP lends its own FFT to the same front end. */
int wake_fft_host(void *ctx, float *complex_pairs, size_t n);

#ifdef __cplusplus
}
#endif
