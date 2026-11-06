// SPDX-License-Identifier: MIT
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xmini_echo_guard.h"

#define N 160
#define FS 8000.0
#define PI 3.14159265358979323846
#define DMA_FRAMES 5
#define NET_FRAMES 3
#define LOOP_FRAMES 3000
#define TALK_FRAMES 500

static uint64_t rng_state;

static double uniform(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return ((rng_state >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

static double gaussian(void) { return sqrt(-2 * log(uniform())) * cos(2 * PI * uniform()); }

static int16_t clip(double value, double limit)
{
    value = round(value);
    return (int16_t)(value > limit ? limit : value < -limit ? -limit : value);
}

static double rms(const int16_t *pcm)
{
    double energy = 0;
    for (unsigned i = 0; i < N; ++i) energy += (double)pcm[i] * pcm[i];
    return sqrt(energy / N);
}

static void noise(int16_t *pcm, double sigma)
{
    for (unsigned i = 0; i < N; ++i) pcm[i] = clip(sigma * gaussian(), 32767);
}

static void tone(int16_t *pcm, double amplitude, double hz, int frame, double sigma)
{
    for (unsigned i = 0; i < N; ++i)
        pcm[i] = clip(amplitude * sin(2 * PI * hz * ((double)frame * N + i) / FS) + sigma * gaussian(), 32767);
}

static void settle(xmini_echo_guard_t *g, unsigned frames)
{
    int16_t pcm[N];
    for (unsigned f = 0; f < frames; ++f) {
        noise(pcm, 40);
        xmini_echo_guard_uplink(g, pcm, N);
    }
}

static void test_idle(void)
{
    xmini_echo_guard_t g;
    int16_t pcm[N];
    rng_state = 1;
    xmini_echo_guard_reset(&g, 40, 24);
    assert(g.gain_q15 == 328 && g.closed_q15 == 328 && g.dt_q15 == 16423 && g.erl_q8 == 1019);
    for (unsigned f = 0; f < 200; ++f) {
        noise(pcm, 40);
        xmini_echo_guard_uplink(&g, pcm, N);
        assert(rms(pcm) < 1);
    }
    memset(pcm, 0, sizeof(pcm));
    xmini_echo_guard_uplink(&g, pcm, N);
    for (unsigned i = 0; i < N; ++i) assert(pcm[i] == 0);
    assert(g.closed_frames == 201 && !g.dt_frames && !g.open_frames && g.gain_q15 == 328);
}

static void test_startup_open_and_hangover(void)
{
    xmini_echo_guard_t g;
    int16_t pcm[N], input[N];
    rng_state = 2;
    xmini_echo_guard_reset(&g, 40, 24);
    // A low-frequency ADC start-up transient after quiet first frames never opens the guard.
    for (unsigned f = 0; f < 15; ++f) {
        tone(pcm, f < 3 ? 0 : 20000 * exp(-(f - 3.0) / 4), 30, (int)f, 40);
        double before = rms(pcm);
        xmini_echo_guard_uplink(&g, pcm, N);
        assert(g.closed_frames == f + 1 && g.gain_q15 == 328 && rms(pcm) <= before * 0.011 + 1);
    }
    settle(&g, 1);
    assert(g.closed_frames == 16);
    // Loud near speech opens within its first frame and then passes bit-exact.
    tone(input, 8000, 700, 0, 40);
    memcpy(pcm, input, sizeof(pcm));
    xmini_echo_guard_uplink(&g, pcm, N);
    assert(g.open_frames == 1 && g.gain_q15 == XMINI_ECHO_GUARD_UNITY);
    for (unsigned i = 32; i < N; ++i) assert(pcm[i] == input[i]);
    // Eight hangover frames stay open, the ninth begins the 50 ms close.
    for (unsigned f = 1; f <= 8; ++f) {
        noise(input, 40);
        memcpy(pcm, input, sizeof(pcm));
        xmini_echo_guard_uplink(&g, pcm, N);
        assert(g.open_frames == 1 + f && g.closed_frames == 16);
        for (unsigned i = 0; i < N; ++i) assert(pcm[i] == input[i]);
    }
    settle(&g, 1);
    assert(g.closed_frames == 17 && g.gain_q15 == XMINI_ECHO_GUARD_UNITY - N * 82);
    settle(&g, 2);
    assert(g.gain_q15 == 328);
}

static void test_far_only_and_double_talk(void)
{
    xmini_echo_guard_t g;
    int16_t far[N], pcm[N];
    rng_state = 3;
    xmini_echo_guard_reset(&g, 40, 24);
    settle(&g, 20);
    unsigned closed = g.closed_frames;
    // Echo at the expected ERL and 8 dB above it stays below the +9 dB speech margin.
    for (unsigned f = 0; f < 100; ++f) {
        tone(far, 3000, 1970, (int)f, 0);
        xmini_echo_guard_downlink(&g, far, N);
        tone(pcm, 3000 * pow(10, (f < 50 ? 6.0 : 14.0) / 20), 1970, (int)f - 5, 40);
        double before = rms(pcm);
        xmini_echo_guard_uplink(&g, pcm, N);
        assert(g.closed_frames == ++closed && rms(pcm) <= before * 0.011 + 1);
    }
    // Near speech 9 dB over a weak far echo opens at the double-talk gain.
    for (unsigned f = 0; f < 12; ++f) {
        tone(far, 300, 1970, (int)f, 0);
        xmini_echo_guard_downlink(&g, far, N);
    }
    tone(pcm, 3500, 700, 0, 40);
    xmini_echo_guard_uplink(&g, pcm, N);
    assert(g.dt_frames == 1 && g.gain_q15 == 16423);
    // Far audio that is irrelevant next to loud near speech leaves it at unity.
    tone(pcm, 20000, 700, 1, 40);
    xmini_echo_guard_uplink(&g, pcm, N);
    assert(g.open_frames == 1 && g.gain_q15 == XMINI_ECHO_GUARD_UNITY);
    // An underrun (zeros) and decoded A-law silence (constant +8) carry no echo.
    for (unsigned f = 0; f < XMINI_ECHO_GUARD_FAR_FRAMES; ++f) {
        assert(g.far[g.far_next] > 40000 && g.far[g.far_next] < 50000);
        for (unsigned i = 0; i < N; ++i) far[i] = f & 1 ? 8 : 0;
        xmini_echo_guard_downlink(&g, far, N);
    }
    for (unsigned i = 0; i < XMINI_ECHO_GUARD_FAR_FRAMES; ++i) assert(g.far[i] == 0);
}

static void test_gain_steps(void)
{
    xmini_echo_guard_t g;
    int16_t pcm[N], far[N];
    int previous = 328;
    unsigned opened_at = 0, rising = 0, falling = 0;
    rng_state = 4;
    xmini_echo_guard_reset(&g, 40, 24);
    settle(&g, 20);
    for (unsigned f = 0; f < 12; ++f) {
        for (unsigned i = 0; i < N; ++i) pcm[i] = i & 1 ? -32767 : 32767;
        if (f == 1)
            for (unsigned k = 0; k < XMINI_ECHO_GUARD_FAR_FRAMES; ++k) {
                memcpy(far, pcm, sizeof(far));
                xmini_echo_guard_downlink(&g, far, N);
            }
        xmini_echo_guard_uplink(&g, pcm, N);
        for (unsigned i = 0; i < N; ++i) {
            int magnitude = abs(pcm[i]), step = magnitude - previous;
            assert(step <= 1024 + 1 && step >= -82 - 1);
            if (step > 1) ++rising;
            if (step < -1) ++falling;
            if (!opened_at && magnitude >= 32766) opened_at = f * N + i + 1;
            previous = magnitude;
        }
    }
    // Opening takes 4 ms; full-scale far audio then holds through hangover at -6 dB
    // and closes to -40 dB, each fall bounded to 82/32768 per sample (~50 ms full range).
    assert(opened_at == 32 && rising == 32);
    assert(falling >= 395 && g.gain_q15 == 328 && g.dt_frames == 8 && g.open_frames == 1);
}

static void test_extremes(void)
{
    xmini_echo_guard_t g;
    int16_t pcm[N], input[N];
    rng_state = 5;
    xmini_echo_guard_reset(&g, 40, 24);
    settle(&g, 20);
    for (unsigned f = 0; f < 3; ++f) {
        for (unsigned i = 0; i < N; ++i) input[i] = i & 1 ? 32767 : -32768;
        memcpy(pcm, input, sizeof(pcm));
        xmini_echo_guard_uplink(&g, pcm, N);
        for (unsigned i = f ? 0 : 32; i < N; ++i) assert(pcm[i] == input[i]);
    }
    for (unsigned i = 0; i < N; ++i) pcm[i] = -32768;
    xmini_echo_guard_downlink(&g, pcm, N);
    assert(g.far[g.far_next ? g.far_next - 1 : XMINI_ECHO_GUARD_FAR_FRAMES - 1] == 0);
    // Largest echo estimate with full-scale far and near audio stays in range.
    xmini_echo_guard_reset(&g, 100, 42);
    assert(g.erl_q8 == 64300000 && g.closed_q15 == 0 && g.dt_q15 == 65);
    settle(&g, 20);
    for (unsigned f = 0; f < 30; ++f) {
        for (unsigned i = 0; i < N; ++i) pcm[i] = i & 1 ? 32767 : -32768;
        xmini_echo_guard_downlink(&g, pcm, N);
        xmini_echo_guard_uplink(&g, pcm, N);
        for (unsigned i = 0; i < N; ++i) assert(abs(pcm[i]) <= 32768);
    }
    assert(g.gain_q15 == 0 && g.closed_frames == 50);
    xmini_echo_guard_uplink(&g, pcm, 0);
    xmini_echo_guard_downlink(&g, pcm, 0);
}

static void test_levels(void)
{
    xmini_echo_guard_t g;
    int16_t far[N], pcm[N];
    rng_state = 6;
    xmini_echo_guard_reset(&g, 0, 24);
    assert(g.erl_q8 == 0 && g.closed_q15 == 328 && g.dt_q15 == 16423);
    xmini_echo_guard_levels(&g, 40, 29);
    assert(g.erl_q8 == 1019 && g.closed_q15 == 328 && g.dt_q15 == 16423);
    xmini_echo_guard_levels(&g, 100, 24);
    assert(g.erl_q8 == 1019000 && g.closed_q15 == 0 && g.dt_q15 == 519);
    xmini_echo_guard_levels(&g, 40, 36);
    assert(g.erl_q8 == 16150 && g.closed_q15 == 21 && g.dt_q15 == 4125);
    xmini_echo_guard_levels(&g, 41, 24);
    assert(g.erl_q8 == 1283);
    xmini_echo_guard_levels(&g, 1, 0);
    assert(g.erl_q8 == 0 && g.closed_q15 == 328);
    // A microphone gain step rescales the noise floor instead of opening on noise.
    xmini_echo_guard_reset(&g, 40, 24);
    settle(&g, 40);
    uint32_t floor_24 = g.noise;
    xmini_echo_guard_levels(&g, 40, 36);
    assert(g.noise > floor_24 * 15 && g.noise < floor_24 * 17);
    for (unsigned f = 0; f < 50; ++f) {
        noise(pcm, 160);
        xmini_echo_guard_uplink(&g, pcm, N);
    }
    assert(!g.open_frames && !g.dt_frames);
    // A 10 dB louder echo after volume 40 -> 60 stays closed only with the new ERL.
    for (unsigned stale = 0; stale < 2; ++stale) {
        xmini_echo_guard_reset(&g, 40, 24);
        settle(&g, 20);
        if (!stale) xmini_echo_guard_levels(&g, 60, 24);
        for (unsigned f = 0; f < 30; ++f) {
            tone(far, 3000, 1970, (int)f, 0);
            xmini_echo_guard_downlink(&g, far, N);
            tone(pcm, 3000 * pow(10, 16.0 / 20), 1970, (int)f - 5, 40);
            xmini_echo_guard_uplink(&g, pcm, N);
        }
        assert(stale ? g.dt_frames + g.open_frames > 0 : g.closed_frames == 50);
    }
}

typedef struct {
    double b0, b2, a1, a2, z1, z2;
} resonator_t;

typedef struct {
    resonator_t resonance;
    int16_t speaker[DMA_FRAMES][N];
    uint8_t network[NET_FRAMES][N];
    xmini_echo_guard_t guard;
} device_t;

static device_t s_devices[2];
static double s_rms[LOOP_FRAMES][2];
static int16_t s_speech[2][TALK_FRAMES * N];
static double s_clean[TALK_FRAMES];

static double resonate(resonator_t *r, double x)
{
    double y = r->b0 * x + r->z1;
    r->z1 = r->z2 - r->a1 * y;
    r->z2 = r->b2 * x - r->a2 * y;
    return y;
}

static uint8_t alaw_encode(int16_t sample)
{
    static const int ends[8] = {0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff};
    int value = sample >> 3, mask = 0xd5, segment = 0;
    if (value < 0) {
        mask = 0x55;
        value = -value - 1;
    }
    while (segment < 8 && value > ends[segment]) ++segment;
    if (segment == 8) return (uint8_t)(0x7f ^ mask);
    return (uint8_t)(((segment << 4) | ((value >> (segment < 2 ? 1 : segment)) & 0x0f)) ^ mask);
}

static int16_t alaw_decode(uint8_t code)
{
    code ^= 0x55;
    int segment = (code & 0x70) >> 4, value = ((code & 0x0f) << 4) + (segment ? 0x108 : 8);
    if (segment > 1) value <<= segment - 1;
    return (int16_t)(code & 0x80 ? value : -value);
}

// Syllable-modulated 300..2700 Hz noise, as in the investigation model.
static void speech_like(int16_t *out, double dbfs, double start, double stop)
{
    static double shaped[TALK_FRAMES * N];
    double ring[25] = {0}, short_sum = 0, long_sum = 0, energy = 0;
    for (unsigned i = 0; i < TALK_FRAMES * N; ++i) {
        double x = gaussian(), envelope = 0.5 + 0.5 * sin(2 * PI * 4 * i / FS);
        short_sum += x - ring[(i + 22) % 25];
        long_sum += x - ring[i % 25];
        ring[i % 25] = x;
        shaped[i] = (short_sum / 3 - long_sum / 25) * envelope * envelope;
        energy += shaped[i] * shaped[i];
    }
    double scale = 32768 * pow(10, dbfs / 20) / sqrt(energy / (TALK_FRAMES * N));
    for (unsigned i = 0; i < TALK_FRAMES * N; ++i)
        out[i] = i >= start * FS && i < stop * FS ? clip(shaped[i] * scale, 32767) : 0;
}

// Two devices in one call: 5-frame DMA, resonant and -28 dB broadband echo, PCMA, 3-frame network.
static void simulate(double peak_db, int volume, bool guarded, bool talk_a, bool talk_b, unsigned frames)
{
    double louder = volume * 0.5 - 20, peak = pow(10, (peak_db + louder) / 20);
    double w = 2 * PI * 1970 / FS, alpha = sin(w) / 12, a0 = 1 + alpha, broadband = pow(10, (louder - 28) / 20);
    memset(s_devices, 0, sizeof(s_devices));
    for (unsigned k = 0; k < 2; ++k) {
        s_devices[k].resonance = (resonator_t){.b0 = alpha / a0 * peak, .b2 = -alpha / a0 * peak,
                                               .a1 = -2 * cos(w) / a0, .a2 = (1 - alpha) / a0};
        memset(s_devices[k].network, alaw_encode(0), sizeof(s_devices[k].network));
        xmini_echo_guard_reset(&s_devices[k].guard, volume, 24);
    }
    for (unsigned f = 0; f < frames; ++f) {
        uint8_t up[2][N];
        for (unsigned k = 0; k < 2; ++k) {
            device_t *d = &s_devices[k];
            const int16_t *speaker = d->speaker[f % DMA_FRAMES];
            int16_t pcm[N];
            bool talking = k ? talk_b : talk_a;
            for (unsigned i = 0; i < N; ++i) {
                double mic = resonate(&d->resonance, speaker[i]) + broadband * speaker[i] + 40 * gaussian();
                if (talking) mic += s_speech[k][f * N + i];
                pcm[i] = clip(mic, 32767);
            }
            if (guarded) xmini_echo_guard_uplink(&d->guard, pcm, N);
            for (unsigned i = 0; i < N; ++i) pcm[i] = alaw_decode(up[k][i] = alaw_encode(pcm[i]));
            s_rms[f][k] = rms(pcm);
        }
        for (unsigned k = 0; k < 2; ++k) {
            device_t *d = &s_devices[k];
            int16_t *arriving = d->speaker[f % DMA_FRAMES];
            for (unsigned i = 0; i < N; ++i) arriving[i] = alaw_decode(s_devices[1 - k].network[f % NET_FRAMES][i]);
            if (guarded) xmini_echo_guard_downlink(&d->guard, arriving, N);
        }
        for (unsigned k = 0; k < 2; ++k) memcpy(s_devices[k].network[f % NET_FRAMES], up[k], sizeof(up[k]));
    }
}

static double peak_rms(unsigned first, unsigned last)
{
    double peak = 0;
    for (unsigned f = first; f < last; ++f)
        for (unsigned k = 0; k < 2; ++k)
            if (s_rms[f][k] > peak) peak = s_rms[f][k];
    return peak;
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void test_loop(double peak_db, int volume)
{
    rng_state = 7;
    simulate(peak_db, volume, false, false, false, 200);
    unsigned saturated = 0;
    while (saturated < 200 && s_rms[saturated][0] < 30000 && s_rms[saturated][1] < 30000) ++saturated;
    assert(saturated < 200);
    double idle_peak = 0;
    for (unsigned seed = 8; seed < 10; ++seed) {
        rng_state = seed;
        simulate(peak_db, volume, true, false, false, LOOP_FRAMES);
        if (peak_rms(0, LOOP_FRAMES) > idle_peak) idle_peak = peak_rms(0, LOOP_FRAMES);
    }
    assert(idle_peak < 100);

    rng_state = 10;
    speech_like(s_speech[0], -20, 1, 5);
    speech_like(s_speech[1], -20, 2, 4);
    for (unsigned f = 0; f < TALK_FRAMES; ++f) s_clean[f] = rms(s_speech[0] + f * N);
    simulate(peak_db, volume, true, true, false, TALK_FRAMES);
    double ratios[TALK_FRAMES], listener = 0;
    unsigned count = 0;
    for (unsigned f = 50; f < 250; ++f) {
        if (s_clean[f] > 328) ratios[count++] = s_rms[f][0] / s_clean[f];
        if (s_rms[f][1] > listener) listener = s_rms[f][1];
    }
    qsort(ratios, count, sizeof(ratios[0]), compare);
    double loss = -20 * log10(ratios[count / 2]);
    assert(count > 100 && loss < 1 && listener < 1000);
    assert(peak_rms(275, TALK_FRAMES) < 100);

    simulate(peak_db, volume, true, true, true, TALK_FRAMES);
    double overlap = peak_rms(50, 250), after = peak_rms(275, TALK_FRAMES);
    assert(overlap < 30000 && after < 100);
    // Equal simultaneous talkers, then a near talker 10 dB louder than the far one.
    speech_like(s_speech[1], -20, 1, 5);
    simulate(peak_db, volume, true, true, true, TALK_FRAMES);
    double equal = peak_rms(275, TALK_FRAMES);
    assert(peak_rms(50, 250) < 30000 && equal < 100);
    speech_like(s_speech[1], -10, 2, 4);
    simulate(peak_db, volume, true, true, true, TALK_FRAMES);
    double louder = peak_rms(275, TALK_FRAMES);
    assert(peak_rms(50, 250) < 30000 && louder < 100);
    printf("echo loop %+.0f dB volume %d: unguarded saturates at %.2f s; guarded idle peak %.1f; "
           "single-talk median loss %.2f dB, listener peak %.0f; double-talk peak %.0f, after %.1f/%.1f/%.1f\n",
           peak_db, volume, saturated * 0.02, idle_peak, loss, listener, overlap, after, equal, louder);
}

int main(void)
{
    test_idle();
    test_startup_open_and_hangover();
    test_far_only_and_double_talk();
    test_gain_steps();
    test_extremes();
    test_levels();
    test_loop(4, 40);
    test_loop(10, 40);
    test_loop(16, 40);
    test_loop(4, 100);
    return 0;
}
