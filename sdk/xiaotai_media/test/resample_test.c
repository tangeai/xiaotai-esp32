// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_format.py
// Replaces legacy: tests/regression/test_wav_reader.py
/* Sample-rate converters (src/resample.h).
 *
 * down2 (16 kHz to 8 kHz): bit-exact with the uplink decimator it replaces,
 * whose code is transcribed below as the reference; one second converted at
 * once equals fifty 20 ms packets; odd counts and short output are refused
 * without touching the state; pass band and alias rejection.
 *
 * up2 (8 kHz to 16 kHz): within one step of a double-precision run of the
 * float playback interpolator it replaces (its coefficients transcribed
 * below); every second sample is the input delayed by 15; constant input
 * stays exact; continuity across any block split; tail flush; clip counting;
 * image rejection.
 *
 * rate (WAV source rates to 8 or 16 kHz): the scenarios of the WAV reader it
 * replaces (every rate and length, chunk independence, 16 kHz pass-through
 * with stereo averaging, lead-in and tail kept, impulses at both ends, 1 kHz
 * pass band, alias rejection beyond 40 dB), the low-pass tables regenerated
 * from their formula, unaligned input, and every error path.
 *
 * plan: which converter a rate pair takes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resample.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- helpers */

/* Power of the component at frequency hz over count samples at rate, relative
 * to a full sine of amplitude: a single DFT bin (whole cycles are used). */
static double tone_db(const int16_t *pcm, size_t count, double hz, double rate, double amplitude)
{
    double re = 0, im = 0;
    for (size_t i = 0; i < count; ++i) {
        re += pcm[i] * cos(2 * M_PI * hz * (double)i / rate);
        im += pcm[i] * sin(2 * M_PI * hz * (double)i / rate);
    }
    const double magnitude = 2 * sqrt(re * re + im * im) / (double)count;
    return 20 * log10((magnitude > 1e-9 ? magnitude : 1e-9) / amplitude);
}

static double energy(const int16_t *pcm, size_t count)
{
    double sum = 0;
    for (size_t i = 0; i < count; ++i) sum += (double)pcm[i] * pcm[i];
    return sum / (double)count;
}

static uint32_t s_seed = 12345;
static int16_t noise(void)
{
    s_seed = s_seed * 1103515245U + 12345U;
    return (int16_t)(s_seed >> 16);
}

/* ------------------------------------------------------------------ down2 */

/* The uplink decimator being replaced, transcribed as the reference. */
typedef struct {
    int16_t delay[31];
    size_t write_index;
    int emit_phase;
    int initialized;
} reference_down2_t;

static const int16_t s_reference_down2_q15[31] = {
    -9, 65, 33, -132, -112, 261, 305, -438, -691, 635, 1429, -813, -3041, 937, 10266, 15378,
    10266, 937, -3041, -813, 1429, 635, -691, -438, 305, 261, -112, -132, 33, 65, -9,
};

static int16_t reference_down2_push(reference_down2_t *r, int16_t sample, int emit)
{
    if (!r->initialized) {
        for (size_t i = 0; i < 31; ++i) r->delay[i] = sample;
        r->initialized = 1;
    }
    r->delay[r->write_index] = sample;
    size_t index = r->write_index;
    r->write_index = (r->write_index + 1U) % 31U;
    if (!emit) return 0;
    int64_t accumulator = 0;
    for (size_t tap = 0; tap < 31; ++tap) {
        accumulator += (int64_t)r->delay[index] * s_reference_down2_q15[tap];
        index = index == 0U ? 30U : index - 1U;
    }
    accumulator = (accumulator + (1 << 14)) >> 15;
    if (accumulator > INT16_MAX) return INT16_MAX;
    if (accumulator < INT16_MIN) return INT16_MIN;
    return (int16_t)accumulator;
}

static size_t reference_down2(reference_down2_t *r, const int16_t *input, size_t count, int16_t *output)
{
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        const int16_t filtered = reference_down2_push(r, input[i], !r->emit_phase);
        if (!r->emit_phase) output[written++] = filtered;
        r->emit_phase = !r->emit_phase;
    }
    return written;
}

static int check_down2(void)
{
    static int16_t input[16000], whole[8000], packets[8000], reference[8000];
    for (unsigned i = 0; i < 16000; ++i) input[i] = (int16_t)((int)(i % 200) * 100 - 10000);
    xiaotai_media_down2_t a, b;
    xiaotai_media_down2_reset(&a);
    xiaotai_media_down2_reset(&b);
    CHECK(xiaotai_media_down2_process(&a, input, 16000, whole, 8000) == 8000);
    /* Fifty 20 ms packets give the second converted at once, sample for sample. */
    size_t count = 0;
    for (unsigned frame = 0; frame < 50; ++frame) {
        const size_t n = xiaotai_media_down2_process(&b, input + frame * 320, 320, packets + count, 160);
        CHECK(n == 160);
        count += n;
    }
    CHECK(count == 8000 && memcmp(whole, packets, sizeof(whole)) == 0);
    /* Refusals leave the state as it was. */
    const xiaotai_media_down2_t before = b;
    CHECK(xiaotai_media_down2_process(&b, input, 320, packets, 159) == 0);
    CHECK(xiaotai_media_down2_process(&b, input, 319, packets, 160) == 0);
    CHECK(xiaotai_media_down2_process(&b, NULL, 320, packets, 160) == 0);
    CHECK(xiaotai_media_down2_process(&b, input, 320, NULL, 160) == 0);
    CHECK(xiaotai_media_down2_process(NULL, input, 320, packets, 160) == 0);
    CHECK(memcmp(&b, &before, sizeof(b)) == 0);
    /* After a reset the first packet repeats the start of the stream. */
    xiaotai_media_down2_reset(&b);
    CHECK(xiaotai_media_down2_process(&b, input, 320, packets, 160) == 160);
    CHECK(memcmp(whole, packets, 160 * sizeof(int16_t)) == 0);

    /* Bit-exact with the decimator it replaces, on full-scale noise over
     * uneven packets (the first sample fills the history). */
    static int16_t loud[16000];
    for (unsigned i = 0; i < 16000; ++i) loud[i] = noise();
    reference_down2_t r = {0};
    xiaotai_media_down2_reset(&a);
    size_t done = 0, offset = 0;
    const size_t sizes[] = {2, 320, 64, 1000, 6, 318};
    for (size_t k = 0; offset < 16000; ++k) {
        size_t n = sizes[k % 6];
        if (n > 16000 - offset) n = 16000 - offset;
        CHECK(xiaotai_media_down2_process(&a, loud + offset, n, whole + done, n / 2) == n / 2);
        CHECK(reference_down2(&r, loud + offset, n, reference + done) == n / 2);
        offset += n;
        done += n / 2;
    }
    CHECK(memcmp(whole, reference, done * sizeof(int16_t)) == 0);

    /* Pass band and alias rejection: 1 kHz keeps its level; 5 and 6 kHz,
     * which would fold to 3 and 2 kHz, are attenuated by more than 50 dB. */
    for (int t = 0; t < 3; ++t) {
        const double hz = t == 0 ? 1000 : t == 1 ? 5000 : 6000;
        for (unsigned i = 0; i < 16000; ++i) input[i] = (int16_t)lround(12000 * sin(2 * M_PI * hz * i / 16000));
        xiaotai_media_down2_reset(&a);
        CHECK(xiaotai_media_down2_process(&a, input, 16000, whole, 8000) == 8000);
        const double alias_hz = hz <= 4000 ? hz : 8000 - hz;
        const double db = tone_db(whole + 100, 8000 - 200, alias_hz, 8000, 12000);
        printf("down2: %.0f Hz -> %.0f Hz at 8 kHz: %.2f dB\n", hz, alias_hz, db);
        if (t == 0) CHECK(fabs(db) < 0.05);
        else CHECK(db < -50);
    }
    return 0;
}

/* -------------------------------------------------------------------- up2 */

/* The float playback interpolator being replaced: its 64 coefficients, run
 * in double precision as the reference (output pairs m = 0, 1 per input). */
static const double s_reference_up2[64] = {
    0.0, 0.0, 0.00008235793522, 0.0, -0.0003687317315, 0.0, 0.0009524530465, 0.0,
    -0.0019780798, 0.0, 0.003646515747, 0.0, -0.006220342853, 0.0, 0.01003444902, 0.0,
    -0.01552229541, 0.0, 0.02327967172, 0.0, -0.03421711438, 0.0, 0.04993939897, 0.0,
    -0.07380189683, 0.0, 0.1145268133, 0.0, -0.204298041, 0.0, 0.6339448423, 1.0,
    0.6339448423, 0.0, -0.204298041, 0.0, 0.1145268133, 0.0, -0.07380189683, 0.0,
    0.04993939897, 0.0, -0.03421711438, 0.0, 0.02327967172, 0.0, -0.01552229541, 0.0,
    0.01003444902, 0.0, -0.006220342853, 0.0, 0.003646515747, 0.0, -0.0019780798, 0.0,
    0.0009524530465, 0.0, -0.0003687317315, 0.0, 0.00008235793522, 0.0, 0.0, 0.0,
};

static void reference_up2(const int16_t *input, size_t count, int16_t first, double *output)
{
    double history[32];
    for (int k = 0; k < 32; ++k) history[k] = first; /* history[k]: age k */
    for (size_t i = 0; i < count; ++i) {
        memmove(history + 1, history, 31 * sizeof(double));
        history[0] = input[i];
        for (int m = 0; m < 2; ++m) {
            double sum = 0;
            for (int k = 0; k < 32; ++k) sum += history[k] * s_reference_up2[2 * k + m];
            output[2 * i + (size_t)m] = sum;
        }
    }
}

static int check_up2(void)
{
    enum { N = 8000 };
    static int16_t input[N], output[2 * N], pieces[2 * N];
    static double expected[2 * N];
    /* A quarter of full scale: the filter's absolute tap sum (2.35) cannot overshoot. */
    for (int i = 0; i < N; ++i) input[i] = (int16_t)(noise() / 4);
    xiaotai_media_up2_t up;
    xiaotai_media_up2_reset(&up, input[0]);
    CHECK(xiaotai_media_up2_process(&up, input, N, output, 2 * N) == 2 * N);
    reference_up2(input, N, input[0], expected);
    for (int i = 0; i < 2 * N; ++i) {
        const double difference = fabs(output[i] - expected[i]);
        if (difference > 1.0) {
            fprintf(stderr, "up2 sample %d: %d, reference %.3f\n", i, output[i], expected[i]);
            return 1;
        }
    }
    /* Every second sample is an input sample delayed by 15 inputs (31 output
     * samples), the first ones being the reset value. */
    for (int i = 0; i < N; ++i) CHECK(output[2 * i + 1] == (i >= 15 ? input[i - 15] : input[0]));
    CHECK(up.clipped == 0);

    /* Continuity: any block split gives the same samples. */
    xiaotai_media_up2_reset(&up, input[0]);
    size_t offset = 0, written = 0;
    const size_t sizes[] = {1, 7, 160, 80, 3, 999};
    for (size_t k = 0; offset < N; ++k) {
        size_t n = sizes[k % 6];
        if (n > N - offset) n = N - offset;
        CHECK(xiaotai_media_up2_process(&up, input + offset, n, pieces + written, 2 * n) == 2 * n);
        offset += n;
        written += 2 * n;
    }
    CHECK(written == 2 * N && memcmp(output, pieces, sizeof(output)) == 0);

    /* Constant input stays exact (unity DC gain), even at both extremes. */
    const int16_t levels[] = {12345, INT16_MAX, INT16_MIN, -1};
    for (int l = 0; l < 4; ++l) {
        int16_t constant[100];
        for (int i = 0; i < 100; ++i) constant[i] = levels[l];
        xiaotai_media_up2_reset(&up, levels[l]);
        CHECK(xiaotai_media_up2_process(&up, constant, 100, output, 200) == 200);
        for (int i = 0; i < 200; ++i) CHECK(output[i] == levels[l]);
        CHECK(up.clipped == 0);
    }

    /* Errors: not reset, empty, short output, NULL. */
    xiaotai_media_up2_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    CHECK(xiaotai_media_up2_process(&fresh, input, 10, output, 20) == 0);
    CHECK(xiaotai_media_up2_finish(&fresh, output, 2 * N) == 0);
    xiaotai_media_up2_reset(&up, 0);
    CHECK(xiaotai_media_up2_process(&up, input, 0, output, 20) == 0);
    CHECK(xiaotai_media_up2_process(&up, input, 10, output, 19) == 0);
    CHECK(xiaotai_media_up2_process(&up, NULL, 10, output, 20) == 0);
    CHECK(xiaotai_media_up2_process(NULL, input, 10, output, 20) == 0);

    /* Tail: nothing before any input; after input exactly the 62 samples once.
     * Input m sits at output 2 m + 31: an impulse as the last of 20 inputs
     * (output 69) comes out whole at index 29 of the tail, its interpolated
     * neighbours at 0.634 of it, and nothing after the filter's span. */
    CHECK(xiaotai_media_up2_finish(&up, output, 2 * N) == 0);
    int16_t impulse[20] = {0};
    impulse[19] = 16000;
    CHECK(xiaotai_media_up2_process(&up, impulse, 20, output, 40) == 40);
    for (int i = 0; i < 40; ++i) CHECK(output[i] == 0); /* causal: nothing before its time */
    CHECK(xiaotai_media_up2_finish(&up, output, XIAOTAI_MEDIA_UP2_TAIL_SAMPLES - 1) == 0);
    CHECK(xiaotai_media_up2_finish(&up, output, 2 * N) == XIAOTAI_MEDIA_UP2_TAIL_SAMPLES);
    CHECK(40 + 29 == 2 * 19 + XIAOTAI_MEDIA_UP2_DELAY_SAMPLES);
    CHECK(output[29] == 16000);
    CHECK(abs(output[28] - 10143) <= 1 && abs(output[30] - 10143) <= 1);
    CHECK(output[XIAOTAI_MEDIA_UP2_TAIL_SAMPLES - 1] == 0 && output[XIAOTAI_MEDIA_UP2_TAIL_SAMPLES - 2] == 0);
    CHECK(xiaotai_media_up2_finish(&up, output, 2 * N) == 0);

    /* A full-scale square wave overshoots: saturated, counted, never wrapped. */
    int16_t square[160];
    for (int i = 0; i < 160; ++i) square[i] = (i / 8) % 2 ? INT16_MIN : INT16_MAX;
    xiaotai_media_up2_reset(&up, INT16_MAX);
    CHECK(xiaotai_media_up2_process(&up, square, 160, output, 320) == 320);
    CHECK(up.clipped > 0);
    reference_up2(square, 160, INT16_MAX, expected);
    uint32_t over = 0;
    for (int i = 0; i < 320; ++i) {
        if (expected[i] > 32767.5) {
            CHECK(output[i] == INT16_MAX);
            ++over;
        } else if (expected[i] < -32768.5) {
            CHECK(output[i] == INT16_MIN);
            ++over;
        } else {
            CHECK(fabs(output[i] - expected[i]) <= 1.0);
        }
    }
    CHECK(over == up.clipped);
    xiaotai_media_up2_reset(&up, 0);
    CHECK(up.clipped == 0);

    /* Pass band and image rejection: a 1 kHz tone keeps its level and its
     * 7 kHz image is gone; the 5 kHz image of 3 kHz is 70 dB down. */
    for (int t = 0; t < 2; ++t) {
        const double hz = t == 0 ? 1000 : 3000;
        for (int i = 0; i < N; ++i) input[i] = (int16_t)lround(12000 * sin(2 * M_PI * hz * i / 8000));
        xiaotai_media_up2_reset(&up, input[0]);
        CHECK(xiaotai_media_up2_process(&up, input, N, output, 2 * N) == 2 * N);
        const double pass = tone_db(output + 200, 2 * N - 400, hz, 16000, 12000);
        const double image = tone_db(output + 200, 2 * N - 400, 8000 - hz, 16000, 12000);
        printf("up2: %.0f Hz %.3f dB, image at %.0f Hz %.1f dB\n", hz, pass, 8000 - hz, image);
        CHECK(fabs(pass) < 0.05);
        CHECK(image < (t == 0 ? -80 : -70));
    }
    return 0;
}

/* ------------------------------------------------------------------- rate */

/* Converts a whole stream with the given frame chunk and output capacity. */
static size_t convert(uint32_t in_hz, uint8_t channels, uint32_t out_hz, const uint8_t *frames, size_t count,
                      size_t frame_chunk, size_t capacity, int16_t *result, size_t room)
{
    xiaotai_media_rate_t rate;
    if (xiaotai_media_rate_open(&rate, in_hz, channels, out_hz) != XIAOTAI_OK) return SIZE_MAX;
    struct {
        int16_t before, samples[1024], after;
    } out = {.before = 123, .after = 456};
    size_t total = 0, offset = 0;
    while (offset < count) {
        const size_t n = count - offset < frame_chunk ? count - offset : frame_chunk;
        size_t taken = 0;
        while (taken < n) {
            for (size_t i = capacity; i < 1024; ++i) out.samples[i] = 12345;
            size_t consumed = 0;
            const size_t written = xiaotai_media_rate_process(&rate, frames + (offset + taken) * channels * 2U,
                                                              n - taken, &consumed, out.samples, capacity);
            if (written > capacity || consumed > n - taken || out.before != 123 || out.after != 456) return SIZE_MAX;
            for (size_t i = capacity; i < 1024; ++i) {
                if (out.samples[i] != 12345) return SIZE_MAX;
            }
            if (written == 0 && consumed == 0) return SIZE_MAX; /* no progress */
            if (total + written > room) return SIZE_MAX;
            memcpy(result + total, out.samples, written * sizeof(int16_t));
            total += written;
            taken += consumed;
        }
        offset += n;
    }
    for (;;) {
        const size_t written = xiaotai_media_rate_finish(&rate, out.samples, capacity);
        if (written > capacity || out.before != 123 || out.after != 456) return SIZE_MAX;
        if (written == 0) break;
        if (total + written > room) return SIZE_MAX;
        memcpy(result + total, out.samples, written * sizeof(int16_t));
        total += written;
    }
    /* Complete: nothing more, and no further conversion. */
    size_t consumed = 99;
    if (xiaotai_media_rate_finish(&rate, out.samples, capacity) != 0) return SIZE_MAX;
    if (count && (xiaotai_media_rate_process(&rate, frames, 1, &consumed, out.samples, capacity) != 0 || consumed))
        return SIZE_MAX;
    return total;
}

static void put16(uint8_t *p, int32_t value)
{
    p[0] = (uint8_t)((uint32_t)value & 0xFFU);
    p[1] = (uint8_t)(((uint32_t)value >> 8) & 0xFFU);
}

/* Source frames: mono, or the mono samples left with 32767 right. */
static void make_frames(const int32_t *mono, size_t count, uint8_t channels, uint8_t *frames)
{
    for (size_t i = 0; i < count; ++i) {
        put16(frames + i * channels * 2U, mono[i]);
        if (channels == 2) put16(frames + i * 4U + 2U, 32767);
    }
}

static const uint32_t s_rates[] = {8000, 8001, 11025, 12000, 15999, 16000, 16001,
                                   22050, 24000, 32000, 44100, 47999, 48000};
#define RATES (sizeof(s_rates) / sizeof(s_rates[0]))
#define ROOM 100000U

static int check_tables(void)
{
    const uint32_t outs[] = {16000, 8000};
    const unsigned taps[] = {96, 192};
    for (int o = 0; o < 2; ++o) {
        xiaotai_media_rate_t rate;
        CHECK(xiaotai_media_rate_open(&rate, 11025, 1, outs[o]) == XIAOTAI_OK);
        CHECK(rate.tap_count == taps[o] && rate.decimation == 48000U / outs[o]);
        CHECK(rate.tap_count / rate.decimation == 32U); /* every table spans 32 output samples */
        /* Regenerate: Hamming-windowed sinc, fc = 7/16 of the output rate, unity DC. */
        const double fc = outs[o] * 7.0 / 16.0, mid = (taps[o] - 1) / 2.0;
        double h[192], gain = 0;
        for (unsigned i = 0; i < taps[o]; ++i) {
            const double x = i - mid;
            h[i] = sin(2 * M_PI * (fc / 48000.0) * x) / (M_PI * x) * (0.54 - 0.46 * cos(2 * M_PI * i / (taps[o] - 1)));
            gain += h[i];
        }
        int64_t sum = 0;
        for (unsigned i = 0; i < taps[o]; ++i) {
            CHECK(rate.taps[i] == (int32_t)lround(h[i] / gain * 1048576.0));
            sum += rate.taps[i];
        }
        CHECK(llabs(sum - 1048576) <= 2); /* unity DC gain within 2e-6 */
    }
    return 0;
}

static int check_rate_matrix(uint32_t out_hz)
{
    static int16_t outputs[3][ROOM];
    static uint8_t frames[997 * 4 + 1], unaligned[997 * 4 + 1], copy[997 * 4];
    static int32_t mono[997];
    const size_t lengths[] = {1, 3, 257, 997};
    const size_t extra = XIAOTAI_MEDIA_RATE_EXTRA_MAX(out_hz);
    for (size_t r = 0; r < RATES; ++r) {
        for (uint8_t channels = 1; channels <= 2; ++channels) {
            for (size_t l = 0; l < 4; ++l) {
                const size_t count = lengths[l];
                for (size_t i = 0; i < count; ++i) mono[i] = (int32_t)((i * 7919U) % 65536U) - 32768;
                make_frames(mono, count, channels, frames);
                memcpy(copy, frames, count * channels * 2U);
                /* Output chunks of 1, 317 and 1024 samples with frame chunks
                 * of all, 1 and 100 frames: the same samples every time. */
                const size_t capacities[] = {1, 317, 1024}, chunks[] = {997, 1, 100};
                size_t lens[3];
                for (int c = 0; c < 3; ++c) {
                    lens[c] = convert(s_rates[r], channels, out_hz, frames, count, chunks[c], capacities[c],
                                      outputs[c], ROOM);
                    CHECK(lens[c] != SIZE_MAX);
                }
                CHECK(lens[0] == lens[1] && lens[1] == lens[2]);
                CHECK(memcmp(outputs[0], outputs[1], lens[0] * 2) == 0);
                CHECK(memcmp(outputs[1], outputs[2], lens[0] * 2) == 0);
                /* Any alignment of the source bytes. */
                memcpy(unaligned + 1, frames, count * channels * 2U);
                CHECK(convert(s_rates[r], channels, out_hz, unaligned + 1, count, 997, 317, outputs[1], ROOM) ==
                      lens[0]);
                CHECK(memcmp(outputs[0], outputs[1], lens[0] * 2) == 0);
                CHECK(memcmp(copy, frames, count * channels * 2U) == 0); /* source untouched */
                if (s_rates[r] == out_hz) {
                    CHECK(lens[0] == count);
                    for (size_t i = 0; i < count; ++i) {
                        /* Stereo: the average, half away from zero. */
                        const int32_t expected = channels == 1 ? mono[i] : mono[i] >= -32767 ? (mono[i] + 32768) / 2 : -1;
                        CHECK(outputs[0][i] == expected);
                    }
                } else {
                    /* Lead-in and tail are kept, never cropped. */
                    const double nominal = (double)count * out_hz / s_rates[r];
                    CHECK((double)lens[0] >= nominal && (double)lens[0] - nominal <= (double)extra);
                }
            }
        }
    }
    return 0;
}

/* Runs a mono stream through the converter at once. */
static size_t convert_mono(uint32_t in_hz, uint32_t out_hz, const int32_t *mono, size_t count, int16_t *result)
{
    static uint8_t frames[48001 * 2 * 2];
    make_frames(mono, count, 1, frames);
    return convert(in_hz, 1, out_hz, frames, count, count, 320, result, ROOM);
}

static int check_rate_signal(uint32_t out_hz)
{
    static int32_t mono[2 * 48000 + 1];
    static int16_t result[ROOM];
    const size_t extra = XIAOTAI_MEDIA_RATE_EXTRA_MAX(out_hz);
    /* First and last impulses survive the causal filter and its full tail,
     * and the impulse's area is kept (DC gain) within 6 %. */
    for (size_t r = 0; r < RATES; ++r) {
        for (int position = 0; position < 2; ++position) {
            memset(mono, 0, 1000 * sizeof(mono[0]));
            mono[position ? 999 : 0] = 20000;
            const size_t n = convert_mono(s_rates[r], out_hz, mono, 1000, result);
            CHECK(n != SIZE_MAX && n > 100);
            const int16_t *region = position ? result + n - 100 : result;
            int peak = 0;
            int64_t sum = 0;
            for (size_t i = 0; i < 100; ++i) peak = abs(region[i]) > peak ? abs(region[i]) : peak;
            for (size_t i = 0; i < n; ++i) sum += result[i];
            CHECK(peak > 1000);
            CHECK(fabs((double)sum / (20000.0 * out_hz / s_rates[r]) - 1) < 0.06);
        }
    }
    /* 1 kHz pass band: the level within 5 %, the length within the bound. */
    for (size_t r = 0; r < RATES; ++r) {
        const size_t count = 2 * s_rates[r] + 1;
        for (size_t i = 0; i < count; ++i) mono[i] = (int32_t)lround(12000 * sin(2 * M_PI * 1000.0 * i / s_rates[r]));
        const size_t n = convert_mono(s_rates[r], out_hz, mono, count, result);
        CHECK(n != SIZE_MAX);
        CHECK(fabs(energy(result + 200, n - 400) / (12000.0 * 12000.0 / 2) - 1) < 0.05);
        CHECK(fabs((double)n - (double)count * out_hz / s_rates[r]) <= (double)extra + 1);
    }
    /* Tones above the output's Nyquist frequency do not fold into speech. */
    const struct {
        uint32_t rate, tone;
    } wide[] = {{32000, 10000}, {44100, 12000}, {48000, 10000}, {48000, 15000}},
      narrow[] = {{16000, 5000}, {32000, 6000}, {44100, 5500}, {48000, 7500}, {48000, 15000}};
    const bool to16 = out_hz == 16000;
    const size_t tones = to16 ? 4 : 5;
    for (size_t t = 0; t < tones; ++t) {
        const uint32_t rate = to16 ? wide[t].rate : narrow[t].rate, tone = to16 ? wide[t].tone : narrow[t].tone;
        for (size_t i = 0; i < rate; ++i) mono[i] = (int32_t)lround(12000 * sin(2 * M_PI * tone * (double)i / rate));
        const size_t n = convert_mono(rate, out_hz, mono, rate, result);
        CHECK(n != SIZE_MAX);
        const double attenuation =
            10 * log10(fmax(energy(result + 200, n - 400), 1e-12) / (12000.0 * 12000.0 / 2));
        printf("rate to %u Hz: %u Hz source, %u Hz tone: %.1f dB\n", (unsigned)out_hz, (unsigned)rate,
               (unsigned)tone, attenuation);
        CHECK(attenuation < -40);
    }
    return 0;
}

/* The conversion in double precision with the unquantized low-pass: cubic
 * (Catmull-Rom) interpolation to 48 kHz at the exact rational phase, four
 * zero source samples after the end (the interpolation stops as the fourth
 * enters), taps - 1 zero 48 kHz samples, decimation with the last partial
 * group completed by zeros. The integer converter must stay within one step. */
static size_t reference_rate(const int16_t *x, size_t n, uint32_t in_hz, uint32_t out_hz, int16_t *result)
{
    const unsigned taps = out_hz == 16000 ? 96 : 192, d = 48000 / out_hz;
    double h[192], gain = 0;
    const double fc = out_hz * 7.0 / 16.0, mid = (taps - 1) / 2.0;
    for (unsigned i = 0; i < taps; ++i) {
        const double t = i - mid;
        h[i] = sin(2 * M_PI * (fc / 48000.0) * t) / (M_PI * t) * (0.54 - 0.46 * cos(2 * M_PI * i / (taps - 1)));
        gain += h[i];
    }
    double w[4] = {0, 0, 0, 0}, delay[192] = {0};
    uint32_t phase = 0;
    size_t used = 0, written = 0;
    unsigned tails = 4, pos = 0, group = 0, zeros = taps - 1;
    for (;;) {
        double y;
        if (phase >= 48000) {
            double next;
            if (used < n) next = x[used++];
            else if (tails > 1) { --tails; next = 0; }
            else if (zeros) { --zeros; y = 0; goto filter; }
            else if (group) { y = 0; goto filter; }
            else break;
            memmove(w, w + 1, 3 * sizeof(double));
            w[3] = next;
            phase -= 48000;
            continue;
        }
        {
            const double p = phase / 48000.0;
            const double c0 = w[1], c1 = 0.5 * (w[2] - w[0]), c2 = w[0] - 2.5 * w[1] + 2 * w[2] - 0.5 * w[3],
                         c3 = 0.5 * (w[3] - w[0]) + 1.5 * (w[1] - w[2]);
            y = c0 + p * (c1 + p * (c2 + p * c3));
            phase += in_hz;
        }
    filter:
        delay[pos] = y;
        pos = (pos + 1) % taps;
        if (++group == d) {
            group = 0;
            double acc = 0;
            for (unsigned k = 0; k < taps; ++k) acc += delay[(pos + k) % taps] * h[k] / gain;
            const double rounded = acc >= 0 ? floor(acc + 0.5) : -floor(-acc + 0.5);
            result[written++] = (int16_t)(rounded > 32767 ? 32767 : rounded < -32768 ? -32768 : rounded);
        }
    }
    return written;
}

static int check_rate_reference(uint32_t out_hz)
{
    static int16_t x[24000], expected[ROOM], actual[ROOM];
    static int32_t mono[24000];
    for (size_t r = 0; r < RATES; ++r) {
        if (s_rates[r] == out_hz) continue;
        const size_t n = s_rates[r] / 2; /* half a second of half-scale noise */
        for (size_t i = 0; i < n; ++i) mono[i] = x[i] = (int16_t)(noise() / 2);
        const size_t want = reference_rate(x, n, s_rates[r], out_hz, expected);
        const size_t got = convert_mono(s_rates[r], out_hz, mono, n, actual);
        CHECK(got == want);
        int worst = 0;
        for (size_t i = 0; i < got; ++i) worst = abs(actual[i] - expected[i]) > worst ? abs(actual[i] - expected[i]) : worst;
        if (worst > 1) fprintf(stderr, "rate %u to %u: %d steps off\n", (unsigned)s_rates[r], (unsigned)out_hz, worst);
        CHECK(worst <= 1);
    }
    return 0;
}

static int check_rate_edges(void)
{
    /* Stereo pass-through: the average, rounded half away from zero. */
    uint8_t frames[16];
    const int32_t values[] = {-32768, -32768, 32767, 32767, -2, 1, -32768, 32767};
    for (int i = 0; i < 8; ++i) put16(frames + 2 * i, values[i]);
    int16_t out[8];
    xiaotai_media_rate_t rate;
    CHECK(xiaotai_media_rate_open(&rate, 16000, 2, 16000) == XIAOTAI_OK);
    size_t consumed = 0;
    CHECK(xiaotai_media_rate_process(&rate, frames, 4, &consumed, out, 1) == 1 && consumed == 1);
    CHECK(xiaotai_media_rate_process(&rate, frames + 4, 3, &consumed, out + 1, 8) == 3 && consumed == 3);
    CHECK(out[0] == -32768 && out[1] == 32767 && out[2] == -1 && out[3] == -1);
    CHECK(xiaotai_media_rate_finish(&rate, out, 8) == 0);

    /* Errors. */
    CHECK(xiaotai_media_rate_open(NULL, 16000, 1, 16000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_rate_open(&rate, 16000, 0, 16000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_rate_process(&rate, frames, 1, &consumed, out, 8) == 0 && consumed == 0);
    CHECK(xiaotai_media_rate_open(&rate, 16000, 3, 16000) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_media_rate_open(&rate, 7999, 1, 16000) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_media_rate_open(&rate, 48001, 1, 16000) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_media_rate_open(&rate, 11025, 1, 12000) == XIAOTAI_ERR_NOT_SUPPORTED); /* no table */
    CHECK(xiaotai_media_rate_process(&rate, frames, 1, &consumed, out, 8) == 0 && consumed == 0);
    CHECK(xiaotai_media_rate_open(&rate, 12000, 1, 12000) == XIAOTAI_OK); /* equal rates pass through */
    CHECK(xiaotai_media_rate_process(&rate, frames, 1, NULL, out, 8) == 0);
    CHECK(xiaotai_media_rate_process(&rate, NULL, 1, &consumed, out, 8) == 0);
    CHECK(xiaotai_media_rate_process(&rate, frames, 1, &consumed, NULL, 8) == 0);
    CHECK(xiaotai_media_rate_process(&rate, frames, 0, &consumed, out, 8) == 0 && consumed == 0);
    /* Nothing converted: finish has nothing to flush. */
    CHECK(xiaotai_media_rate_open(&rate, 22050, 1, 16000) == XIAOTAI_OK);
    CHECK(xiaotai_media_rate_finish(&rate, out, 8) == 0);
    CHECK(xiaotai_media_rate_process(&rate, frames, 1, &consumed, out, 8) == 0 && consumed == 0);

    CHECK(xiaotai_media_rate_supported(8000, 16000) && xiaotai_media_rate_supported(48000, 8000));
    CHECK(xiaotai_media_rate_supported(44100, 44100) && !xiaotai_media_rate_supported(96000, 96000));
    CHECK(!xiaotai_media_rate_supported(44100, 22050) && !xiaotai_media_rate_supported(0, 16000));
    return 0;
}

static int check_plan(void)
{
    CHECK(xiaotai_media_resample_plan(8000, 16000) == XIAOTAI_MEDIA_RESAMPLE_UP2);
    CHECK(xiaotai_media_resample_plan(16000, 8000) == XIAOTAI_MEDIA_RESAMPLE_DOWN2);
    CHECK(xiaotai_media_resample_plan(16000, 16000) == XIAOTAI_MEDIA_RESAMPLE_COPY);
    CHECK(xiaotai_media_resample_plan(8000, 8000) == XIAOTAI_MEDIA_RESAMPLE_COPY);
    CHECK(xiaotai_media_resample_plan(44100, 16000) == XIAOTAI_MEDIA_RESAMPLE_RATE);
    CHECK(xiaotai_media_resample_plan(48000, 8000) == XIAOTAI_MEDIA_RESAMPLE_RATE);
    CHECK(xiaotai_media_resample_plan(12000, 16000) == XIAOTAI_MEDIA_RESAMPLE_RATE);
    CHECK(xiaotai_media_resample_plan(8000, 12000) == XIAOTAI_MEDIA_RESAMPLE_NONE);
    CHECK(xiaotai_media_resample_plan(64000, 16000) == XIAOTAI_MEDIA_RESAMPLE_NONE);
    CHECK(xiaotai_media_resample_plan(0, 16000) == XIAOTAI_MEDIA_RESAMPLE_NONE);
    CHECK(xiaotai_media_resample_plan(16000, 0) == XIAOTAI_MEDIA_RESAMPLE_NONE);
    return 0;
}

int main(void)
{
    if (check_down2() || check_up2() || check_tables() || check_rate_matrix(16000) || check_rate_matrix(8000) ||
        check_rate_signal(16000) || check_rate_signal(8000) || check_rate_reference(16000) ||
        check_rate_reference(8000) || check_rate_edges() || check_plan()) {
        return 1;
    }
    printf("PASS: media_resample\n");
    return 0;
}
