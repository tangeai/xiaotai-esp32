// SPDX-License-Identifier: MIT
#include "mel.h"
#include "mel_filterbank.h"
#include <math.h>
#include <string.h>

_Static_assert(MEL_NFFT == WAKE_MEL_FFT_SIZE && MEL_NFREQ == WAKE_MEL_FFT_SIZE / 2 + 1, "FFT bins");
_Static_assert(MEL_HOP == WAKE_MEL_HOP && MEL_NBINS == WAKE_MEL_BINS, "window and tensor shapes");

int wake_mel_init(wake_mel_t *mel, float *fft_scratch, wake_fft_fn fft, void *fft_ctx)
{
    if (!mel) return -1;
    *mel = (wake_mel_t){0};
    if (!fft_scratch || ((uintptr_t)fft_scratch & (WAKE_MEL_FFT_ALIGN - 1U)) || !fft) return -1;
    mel->fft_scratch = fft_scratch;
    mel->fft = fft;
    mel->fft_ctx = fft_ctx;
    for (size_t filter = 0; filter < WAKE_MEL_BINS; ++filter) {
        size_t first = 0, last = MEL_NFREQ;
        while (first < last && MEL_W[first][filter] == 0.0f) ++first;
        while (last > first && MEL_W[last - 1][filter] == 0.0f) --last;
        mel->first[filter] = (uint16_t)first;
        mel->last[filter] = (uint16_t)last;
    }
    return 0;
}

static int extract(wake_mel_t *mel, const int16_t *pcm, size_t first_frame,
                   float out[WAKE_MEL_FRAMES][WAKE_MEL_BINS])
{
    if (!mel || !mel->fft || !pcm || !out) return -1;
    float *fft = mel->fft_scratch;
    for (size_t frame = first_frame; frame < WAKE_MEL_FRAMES; ++frame) {
        const int16_t *samples = pcm + frame * WAKE_MEL_HOP;
        for (size_t i = 0; i < WAKE_MEL_FFT_SIZE; ++i) {
            /* Training uses raw PCM16 converted to float, without pre-emphasis. */
            fft[2U * i] = (float)samples[i] * MEL_WINDOW[i];
            fft[2U * i + 1U] = 0.0f;
        }
        if (mel->fft(mel->fft_ctx, fft, WAKE_MEL_FFT_SIZE) != 0) return -1;
        float power[MEL_NFREQ];
        for (size_t k = 0; k < MEL_NFREQ; ++k) {
            const float re = fft[2U * k], im = fft[2U * k + 1U];
            power[k] = re * re + im * im;
        }
        for (size_t m = 0; m < WAKE_MEL_BINS; ++m) {
            float sum = 0.0f;
            for (size_t k = mel->first[m]; k < mel->last[m]; ++k) sum += power[k] * MEL_W[k][m];
            const float minimum = 1e-10f;
            out[frame][m] = 10.0f * log10f(sum < minimum ? minimum : sum);
        }
    }
    return 0;
}

static void normalize(const float powers[WAKE_MEL_FRAMES][WAKE_MEL_BINS],
                      float out[WAKE_MEL_FRAMES][WAKE_MEL_BINS])
{
    float max = -1e30f;
    for (size_t t = 0; t < WAKE_MEL_FRAMES; ++t)
        for (size_t m = 0; m < WAKE_MEL_BINS; ++m)
            if (powers[t][m] > max) max = powers[t][m];
    const float floor = max - 80.0f;
    for (size_t t = 0; t < WAKE_MEL_FRAMES; ++t) {
        for (size_t m = 0; m < WAKE_MEL_BINS; ++m) {
            const float p = powers[t][m] < floor ? floor : powers[t][m];
            out[t][m] = p / 10.0f + 2.0f;
        }
    }
}

int wake_mel_full(wake_mel_t *mel, const int16_t *pcm,
                  float out[WAKE_MEL_FRAMES][WAKE_MEL_BINS])
{
    if (extract(mel, pcm, 0, out) != 0) return -1;
    normalize(out, out);
    return 0;
}

int wake_mel_stream(wake_mel_t *mel, wake_mel_stream_t *stream, const int16_t *pcm,
                    uint32_t generation, uint64_t end_sample,
                    float out[WAKE_MEL_FRAMES][WAKE_MEL_BINS], uint32_t *reused_frames)
{
    if (reused_frames) *reused_frames = 0;
    if (!stream || !pcm || !out) return -1;
    size_t reused = 0;
    if (stream->valid && stream->generation == generation && end_sample > stream->end_sample) {
        const uint64_t delta = end_sample - stream->end_sample;
        if (delta % WAKE_MEL_HOP == 0 && delta / WAKE_MEL_HOP < WAKE_MEL_FRAMES) {
            const size_t advance = (size_t)(delta / WAKE_MEL_HOP);
            reused = WAKE_MEL_FRAMES - advance;
            memmove(stream->powers, stream->powers + advance, reused * sizeof(stream->powers[0]));
        }
    }
    stream->valid = false;
    if (extract(mel, pcm, reused, stream->powers) != 0) return -1;
    normalize(stream->powers, out);
    stream->generation = generation;
    stream->end_sample = end_sample;
    stream->valid = true;
    if (reused_frames) *reused_frames = (uint32_t)reused;
    return 0;
}
