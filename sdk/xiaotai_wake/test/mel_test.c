// SPDX-License-Identifier: MIT
#include "model/mel.h"
#include "model/mel_filterbank.h"
#include "wake_window.h"
#include "xiaotai/port/mem.h"
#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <string.h>

static alignas(WAKE_MEL_FFT_ALIGN) float reference_fft[WAKE_MEL_FFT_FLOATS];
static float expected[WAKE_MEL_FRAMES][WAKE_MEL_BINS], features[WAKE_MEL_FRAMES][WAKE_MEL_BINS];
static int16_t pcm[WAKE_MEL_SAMPLES + 21000U];
static wake_mel_stream_t stream;
static wake_mel_t mel;
static struct {unsigned calls, fail_at;} fft;

static int counted_fft(void *ctx, float *data, size_t n)
{
    (void)ctx;
    if (++fft.calls == fft.fail_at) return -1;
    return wake_fft_host(NULL, data, n);
}

/* Independent whole-window reference: unoptimized complete filterbank and
 * two passes, as in the old whole-window implementation. Same injected FFT
 * so an ordering or a normalization difference is detected bit-for-bit. */
static void reference(const int16_t *samples)
{
    float maximum = -1e30f;
    for (size_t t = 0; t < WAKE_MEL_FRAMES; ++t) {
        for (size_t k = 0; k < MEL_NFFT; ++k) {
            reference_fft[k * 2] = MEL_WINDOW[k] * (float)samples[t * MEL_HOP + k];
            reference_fft[k * 2 + 1] = 0.0f;
        }
        assert(wake_fft_host(NULL, reference_fft, MEL_NFFT) == 0);
        float spectrum[MEL_NFREQ];
        for (size_t k = 0; k < MEL_NFREQ; ++k)
            spectrum[k] = reference_fft[2 * k] * reference_fft[2 * k] +
                          reference_fft[2 * k + 1] * reference_fft[2 * k + 1];
        for (size_t m = 0; m < MEL_NBINS; ++m) {
            float p = 0.0f;
            for (size_t k = 0; k < MEL_NFREQ; ++k) p += spectrum[k] * MEL_W[k][m];
            float value = 10.0f * log10f(fmaxf(1e-10f, p));
            if (value > maximum) maximum = value;
            expected[t][m] = value;
        }
    }
    for (size_t t = 0; t < WAKE_MEL_FRAMES; ++t)
        for (size_t m = 0; m < MEL_NBINS; ++m)
            expected[t][m] = fmaxf(expected[t][m], maximum - 80.0f) / 10.0f + 2.0f;
}

static void score(size_t start, uint32_t epoch, uint64_t reported_end, unsigned expected_reuse)
{
    const unsigned before = fft.calls;
    uint32_t reused = UINT32_MAX;
    assert(wake_mel_stream(&mel, &stream, pcm + start, epoch, reported_end, features, &reused) == 0);
    assert(reused == expected_reuse && fft.calls - before == WAKE_MEL_FRAMES - expected_reuse);
    reference(pcm + start);
    assert(!memcmp(features, expected, sizeof(expected)));
}

static void fft_matches_direct_fourier(void)
{
    static const float samples[] = {1, 2, -4, 6, 7, 0, 8, -9, 2, 3, -6, 7, 8, 9, 1, -2};
    float data[sizeof(samples) / sizeof(float)];
    memcpy(data, samples, sizeof(data));
    assert(wake_fft_host(NULL, data, 3) != 0);
    assert(wake_fft_host(NULL, data, 8) == 0);
    for (size_t k = 0; k < 8; ++k) {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < 8; ++i) {
            const double angle = -6.28318530717958647692 * (double)(k * i) / 8.0;
            re += samples[2 * i] * cos(angle) - samples[2 * i + 1] * sin(angle);
            im += samples[2 * i] * sin(angle) + samples[2 * i + 1] * cos(angle);
        }
        assert(fabs(data[2 * k] - re) < 0.00003 && fabs(data[2 * k + 1] - im) < 0.00003);
    }
}

static void grid_snapshots(void)
{
    static int16_t ring[WAKE_MEL_SAMPLES + WAKE_MEL_HOP - 1];
    static int16_t window[WAKE_MEL_SAMPLES];
    const size_t stride = 16U * WAKE_MEL_HOP;
    wake_window_t w;
    wake_window_init(&w, ring, WAKE_MEL_SAMPLES, WAKE_MEL_HOP, (uint32_t)stride);
    stream.valid = false;
    /* Take real engine snapshots with a different amount of unaligned tail at
     * each score; compare them to the independently indexed raw windows. */
    const size_t first = WAKE_MEL_SAMPLES + 73;
    wake_window_append(&w, pcm, first, 1LL);
    assert(wake_window_snapshot(&w, window) && w.snapshot_end == WAKE_MEL_SAMPLES);
    assert(!memcmp(window, pcm, sizeof(window)));
    uint32_t reused = 99;
    assert(wake_mel_stream(&mel, &stream, window, w.generation, w.snapshot_end, features, &reused) == 0);
    assert(reused == 0);
    reference(pcm);
    assert(!memcmp(features, expected, sizeof(expected)));
    wake_window_append(&w, pcm + first, stride + 12, 20000LL);
    assert(wake_window_snapshot(&w, window) && w.snapshot_end == WAKE_MEL_SAMPLES + stride);
    assert(!memcmp(window, pcm + stride, sizeof(window)));
    assert(wake_mel_stream(&mel, &stream, window, w.generation, w.snapshot_end, features, &reused) == 0);
    assert(reused == WAKE_MEL_FRAMES - 16);
    reference(pcm + stride);
    assert(!memcmp(features, expected, sizeof(expected)));
}

int main(void)
{
    fft_matches_direct_fourier();
    float *scratch = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, WAKE_MEL_FFT_ALIGN, sizeof(reference_fft));
    assert(scratch);
    assert(wake_mel_init(&mel, scratch + 1, counted_fft, NULL) == -1);
    assert(wake_mel_full(&mel, pcm, features) == -1);
    assert(wake_mel_init(&mel, scratch, counted_fft, NULL) == 0);
    assert(wake_mel_full(&mel, pcm, features) == 0);
    for (size_t t = 0; t < WAKE_MEL_FRAMES; ++t)
        for (size_t m = 0; m < WAKE_MEL_BINS; ++m) assert(features[t][m] == -8.0f);

    /* When a large transient leaves the window, reused silence must be
     * re-normalized: the previous maximum must not clip the next window. */
    pcm[250] = 32767;
    score(0, 22, WAKE_MEL_SAMPLES, 0);
    assert(features[1][0] > -8.0f);
    score(2 * WAKE_MEL_HOP, 22, WAKE_MEL_SAMPLES + 2 * WAKE_MEL_HOP, 96);
    assert(features[0][0] == -8.0f);
    uint32_t noise = 0x12a5b2dc;
    for (size_t i = 0; i < sizeof(pcm) / sizeof(*pcm); ++i) {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        pcm[i] = (int16_t)((int32_t)(noise >> 16) - 32768);
    }
    score(0, 23, WAKE_MEL_SAMPLES, 0);
    score(160, 23, WAKE_MEL_SAMPLES + 160, 97);
    score(161, 23, WAKE_MEL_SAMPLES + 161, 0);
    score(161 + 2560, 23, WAKE_MEL_SAMPLES + 161 + 2560, 82);
    score(20100, 23, WAKE_MEL_SAMPLES + 20100, 0);
    score(0, 23, WAKE_MEL_SAMPLES, 0);
    score(0, 23, WAKE_MEL_SAMPLES, 0);
    score(160, 24, WAKE_MEL_SAMPLES + 160, 0);
    fft.fail_at = fft.calls + 1;
    uint32_t reuse = 99;
    assert(wake_mel_stream(&mel, &stream, pcm + 320, 24, WAKE_MEL_SAMPLES + 320, features, &reuse) != 0);
    assert(!stream.valid && reuse == 0);
    score(480, 24, WAKE_MEL_SAMPLES + 480, 0);
    assert(wake_mel_full(&mel, pcm, features) == 0);
    reference(pcm);
    assert(!memcmp(features, expected, sizeof(expected)));
    grid_snapshots();
    xiaotai_mem_free(scratch);
}
