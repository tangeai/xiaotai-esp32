// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_echo_guard.c::echo_guard
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "g711.h"
#include "portable_audio.h"
#include "registry.h"
#include "xiaotai/media.h"

enum { N = 160, FS = 8000, DELAY = 8 };
static const xiaotai_audio_output_t speaker = {.info = {.volume_span_cdb = 4950}};
static const xiaotai_audio_input_t xmini = {.desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = FS,
    .channels = 1, .roles = {XIAOTAI_CHANNEL_MIC}}, .info = {.gain_db = 24, .gain_min_db = 0, .gain_max_db = 36,
    .gain_step_db = 6, .echo_coupling_db = 12, .settle_ms = 300}};

static void *open_gate(const xiaotai_audio_input_t *input)
{
    void *state;
    xiaotai_processor_info_t info;
    assert(media_echo_gate.open(input, &speaker, &info, &state) == XIAOTAI_OK);
    assert(info.flags == (XIAOTAI_PROCESSOR_GATES | XIAOTAI_PROCESSOR_FAR_END));
    assert(info.rate_hz == FS && info.fetch_max == N && info.feed_frames == N);
    xiaotai_effect_info_t details;
    void *effect;
    assert(media_noise_gate.open(&info, &details, &effect) == XIAOTAI_ERR_NOT_SUPPORTED);
    return state;
}

static xiaotai_processor_frame_t step(void *state, const int16_t *pcm)
{
    assert(media_echo_gate.feed(state, pcm, N * sizeof(*pcm), 98765, 0) == XIAOTAI_OK);
    xiaotai_processor_frame_t frame;
    assert(media_echo_gate.fetch(state, &frame, 0) == XIAOTAI_OK);
    assert(frame.encoding == XIAOTAI_CODEC_PCM16 && frame.samples == N && frame.size == N * sizeof(*pcm));
    assert(frame.captured_us == 98765 && frame.lost_samples == 0);
    return frame;
}

static uint16_t gain(void *state)
{
    xiaotai_processor_stats_t counters;
    media_echo_gate.stats(state, &counters);
    return counters.gain_q15;
}

static void alternate(int16_t *pcm, int16_t magnitude)
{
    for (size_t i = 0; i < N; ++i) pcm[i] = i & 1U ? -magnitude : magnitude;
}

static void live_switch(void)
{
    void *state = open_gate(&xmini);
    int16_t pcm[N];
    alternate(pcm, 123);
    media_echo_gate.levels(state, 40, 24);
    assert(media_echo_gate_enabled(state));
    media_echo_gate.session(state, true);
    for (unsigned frame = 0; frame < 4; ++frame) {
        if (frame == 2) media_echo_gate_set(state, false);
        const xiaotai_processor_frame_t out = step(state, pcm);
        xiaotai_processor_stats_t counters;
        media_echo_gate.stats(state, &counters);
        assert(counters.closed_frames == (frame < 2 ? frame + 1U : 2U));
        assert(!counters.open_frames && !counters.double_talk_frames);
        if (frame < 2) assert(memcmp(out.data, pcm, sizeof(pcm)) && gain(state) < 32768U);
        else assert(!memcmp(out.data, pcm, sizeof(pcm)) && gain(state) == 32768U);
    }
    assert(!media_echo_gate_enabled(state));
    media_echo_gate_set(state, true);
    assert(media_echo_gate_enabled(state) && memcmp(step(state, pcm).data, pcm, sizeof(pcm)));
    media_echo_gate.session(state, false);
    assert(!memcmp(step(state, pcm).data, pcm, sizeof(pcm)));
    media_echo_gate.close(state);
}

static void levels(void)
{
    void *state = open_gate(&xmini);
    xiaotai_audio_input_t unknown_coupling = xmini;
    unknown_coupling.info.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
    void *unknown = open_gate(&unknown_coupling);
    int16_t pcm[N], far[N];
    /* Each volume and gain request: board volume law and step reproduce the
       reference echo attenuation over the complete supported input range. */
    for (int volume = 0; volume <= 100; ++volume) {
        for (int requested_db = 0; requested_db <= 36; ++requested_db) {
            const int erl = 6 + requested_db - requested_db % 6 - 24 + (volume ? (volume + 1) / 2 - 20 : -66);
            const int excess = erl > 6 ? erl - 6 : 0;
            const int expected = (int)lround(32768 * pow(10.0, (-40 - 2.0 * excess) / 20.0));
            media_echo_gate.levels(state, (uint8_t)volume, (int8_t)requested_db);
            media_echo_gate.levels(unknown, (uint8_t)volume, (int8_t)requested_db);
            media_echo_gate.session(state, true);
            media_echo_gate.session(unknown, true);
            alternate(pcm, 3000);
            xiaotai_processor_frame_t first = step(state, pcm), second = step(unknown, pcm);
            assert(abs((int)gain(state) - expected) <= 1);
            assert(gain(state) == gain(unknown) && !memcmp(first.data, second.data, first.size));
        }
    }
    media_echo_gate.close(unknown);
    media_echo_gate.levels(state, 40, 24);
    media_echo_gate.session(state, true);
    for (unsigned frame = 0; frame < 15; ++frame) {
        alternate(pcm, frame < 3 ? 40 : 10000);
        (void)step(state, pcm);
        assert(gain(state) == 328);
    }
    alternate(pcm, 40);
    (void)step(state, pcm);
    alternate(far, 300);
    media_echo_gate.far_end(state, far, N, FS);
    alternate(pcm, 3500);
    (void)step(state, pcm);
    assert(gain(state) == 16423);
    alternate(pcm, 20000);
    (void)step(state, pcm);
    assert(gain(state) == 32768);
    for (unsigned frame = 0; frame < 12; ++frame) {
        for (size_t i = 0; i < N; ++i) far[i] = frame & 1 ? 8 : 0;
        media_echo_gate.far_end(state, far, N, FS);
    }
    media_echo_gate.session(state, false);
    xiaotai_processor_frame_t frame = step(state, pcm);
    assert(!memcmp(pcm, frame.data, frame.size) && gain(state) == 32768);
    assert(media_echo_gate.feed(state, pcm, sizeof(pcm), 10, 5) == XIAOTAI_OK);
    assert(media_echo_gate.feed(state, pcm, sizeof(pcm), 20, 1) == XIAOTAI_ERR_NO_MEM);
    assert(media_echo_gate.fetch(state, &frame, 0) == XIAOTAI_OK && frame.lost_samples == 5);
    assert(media_echo_gate.feed(state, pcm, sizeof(pcm), 30, 2) == XIAOTAI_OK);
    assert(media_echo_gate.fetch(state, &frame, 0) == XIAOTAI_OK && frame.lost_samples == 163);
    media_echo_gate.close(state);
}

static uint32_t random_state = 0xdb0e99a7;

static int noise(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return (int)(random_state % 81U) - 40;
}

static int pcm_clip(double sample)
{
    return sample > 32767 ? 32767 : sample < -32767 ? -32767 : (int)lround(sample);
}

static xiaotai_processor_stats_t counters(void *state)
{
    xiaotai_processor_stats_t stats;
    media_echo_gate.stats(state, &stats);
    return stats;
}

static void tone(int16_t *pcm, int amplitude, int hz, unsigned block, int hiss)
{
    for (unsigned i = 0; i < N; ++i) {
        double phase = 6.283185307179586 * hz * (block * N + i) / FS;
        pcm[i] = (int16_t)pcm_clip(amplitude * sin(phase) + hiss * noise());
    }
}

static void ramps_and_hangover(void)
{
    void *state = open_gate(&xmini);
    int16_t pcm[N];
    media_echo_gate.levels(state, 40, 24);
    media_echo_gate.session(state, true);
    for (unsigned block = 0; block < 16; ++block) {
        const int peak = block < 3 || block == 15 ? 0 : (int)(20000.0 * exp(-(block - 3.0) / 4));
        tone(pcm, peak, 30, block, 2);
        (void)step(state, pcm);
        assert(counters(state).closed_frames == block + 1 && gain(state) == 328);
    }

    alternate(pcm, 32767);
    const int16_t *out = step(state, pcm).data;
    int previous = 328;
    for (unsigned i = 0; i < N; ++i) {
        int magnitude = abs(out[i]);
        assert(magnitude >= previous && magnitude <= previous + 1025);
        assert(i >= 31 ? out[i] == pcm[i] : magnitude < 32766);
        previous = magnitude;
    }
    assert(gain(state) == 32768 && counters(state).open_frames == 1);

    /* Force the falling ramp at full scale so a quiet payload cannot hide a step. */
    for (unsigned block = 0; block < 12; ++block) media_echo_gate.far_end(state, pcm, N, FS);
    unsigned falling = 0;
    for (unsigned block = 0; block < 11; ++block) {
        out = step(state, pcm).data;
        for (unsigned i = 0; i < N; ++i) {
            const int magnitude = abs(out[i]);
            assert(magnitude <= previous && magnitude >= previous - 83);
            falling += magnitude < previous - 1;
            previous = magnitude;
        }
        const xiaotai_processor_stats_t st = counters(state);
        if (block < 8) assert(st.closed_frames == 16 && st.double_talk_frames == block + 1);
        else assert(st.closed_frames == 16 + block - 7 && st.double_talk_frames == 8);
    }
    assert(falling >= 395 && gain(state) == 328);
    media_echo_gate.close(state);
}

static void gain_step_and_far_floor(void)
{
    void *state = open_gate(&xmini);
    int16_t near[N], far[N];
    media_echo_gate.levels(state, 40, 24);
    media_echo_gate.session(state, true);
    for (unsigned block = 0; block < 40; ++block) {
        tone(near, 0, 700, block, 2);
        (void)step(state, near);
    }
    media_echo_gate.levels(state, 40, 36);
    for (unsigned block = 0; block < 50; ++block) {
        tone(near, 0, 700, block, 8);
        (void)step(state, near);
    }
    xiaotai_processor_stats_t st = counters(state);
    assert(st.closed_frames == 90 && !st.open_frames && !st.double_talk_frames);

    for (unsigned stale = 0; stale < 2; ++stale) {
        media_echo_gate.levels(state, 40, 24);
        media_echo_gate.session(state, true);
        for (unsigned block = 0; block < 20; ++block) {
            tone(near, 0, 700, block, 2);
            (void)step(state, near);
        }
        unsigned closed = counters(state).closed_frames;
        if (!stale) media_echo_gate.levels(state, 60, 24);
        for (unsigned block = 0; block < 30; ++block) {
            tone(far, 3000, 1970, block, 0);
            media_echo_gate.far_end(state, far, N, FS);
            tone(near, 18929, 1970, block, 2);
            (void)step(state, near);
        }
        assert(stale ? counters(state).closed_frames < closed + 30 : counters(state).closed_frames == closed + 30);
    }
    media_echo_gate.levels(state, 80, 36);
    media_echo_gate.session(state, true);
    for (unsigned block = 0; block < 150; ++block) {
        tone(far, 0, 1970, block, 5);
        media_echo_gate.far_end(state, far, N, FS);
        tone(near, 0, 700, block, 8);
        (void)step(state, near);
    }
    st = counters(state);
    assert(st.far_floor > 0);
    for (unsigned block = 0; block < 20; ++block) {
        tone(far, 0, 1970, block, 5);
        media_echo_gate.far_end(state, far, N, FS);
        tone(near, 3000, 700, block, 8);
        (void)step(state, near);
    }
    assert(counters(state).open_frames >= st.open_frames + 19 && counters(state).closed_frames == st.closed_frames);
    for (unsigned block = 0; block < 12; ++block) {
        tone(near, 0, 700, block, 8);
        (void)step(state, near);
    }
    st = counters(state);
    for (unsigned block = 0; block < 30; ++block) {
        tone(far, 1040, 1970, block, 5);
        media_echo_gate.far_end(state, far, N, FS);
        tone(near, 3000, 1970, block, 8);
        (void)step(state, near);
    }
    assert(counters(state).closed_frames == st.closed_frames + 30);
    for (unsigned block = 0; block < 200; ++block) {
        tone(far, 3000, 1970, block, 0);
        media_echo_gate.far_end(state, far, N, FS);
    }
    alternate(near, 40);
    (void)step(state, near);
    assert(counters(state).far_floor == 0);

    media_echo_gate.levels(state, 40, 24);
    media_echo_gate.session(state, true);
    for (unsigned i = 0; i < N; ++i) near[i] = INT16_MIN;
    media_echo_gate.far_end(state, near, N, FS);
    const int16_t *out = step(state, near).data;
    for (unsigned i = 0; i < N; ++i) assert(out[i] == -328);
    media_echo_gate.close(state);
}

static void two_device_loop(double peak_db, uint8_t volume)
{
    void *state[2] = {open_gate(&xmini), open_gate(&xmini)};
    int16_t history[2][DELAY][N] = {0};
    const double tau = 6.283185307179586;
    const double w = tau * 1970 / FS;
    const double alpha = sin(w) / 12, a0 = 1 + alpha;
    const double scale = pow(10, (peak_db + volume * 0.5 - 20) / 20);
    const double b = alpha * scale / a0, a1 = -2 * cos(w) / a0, a2 = (1 - alpha) / a0;
    double z[2][2] = {{0}};
    double source_energy = 0, heard_energy = 0;
    unsigned speech_blocks = 0;
    for (unsigned device = 0; device < 2; ++device) {
        media_echo_gate.levels(state[device], volume, 24);
        media_echo_gate.session(state[device], true);
    }
    for (unsigned block = 0; block < 3000; ++block) {
        const bool speaking = block >= 50 && block < 200;
        const bool equal = block >= 320 && block < 470, louder = block >= 650 && block < 800;
        const unsigned slot = block % DELAY;
        int16_t decoded[2][N];
        for (unsigned device = 0; device < 2; ++device) {
            int16_t capture[N];
            double energy = 0;
            for (unsigned i = 0; i < N; ++i) {
                const double arriving = history[1U - device][slot][i];
                const double echo = b * arriving + z[device][0];
                z[device][0] = z[device][1] - a1 * echo;
                z[device][1] = -b * arriving - a2 * echo;
                const double t = (double)(block * N + i) / FS;
                const unsigned amplitude = equal ? 5000 : louder ? (device ? 15811 : 5000) :
                    speaking && device == 0 ? 5000 : 0;
                const double speech = amplitude * (0.7 + 0.3 * sin(tau * 3 * t)) *
                    sin(tau * (device ? 773 : 677) * t);
                capture[i] = (int16_t)pcm_clip(speech + echo + 0.04 * arriving + noise());
                if (speaking) source_energy += speech * speech;
            }
            xiaotai_processor_frame_t output = step(state[device], capture);
            const int16_t *clean = output.data;
            for (unsigned i = 0; i < N; ++i) {
                decoded[device][i] = xiaotai_media_alaw_decode_sample(xiaotai_media_alaw_encode_sample(clean[i]));
                energy += (double)decoded[device][i] * decoded[device][i];
                if (speaking && device == 0) heard_energy += (double)decoded[device][i] * decoded[device][i];
                if ((block >= 255 && block < 300) || block < 50) assert(abs(decoded[device][i]) < 100);
                if (speaking && device == 1) assert(abs(decoded[device][i]) < 1000);
            }
            if (equal || louder) assert(energy / N < 30000.0 * 30000.0);
            if ((block >= 545 && block < 650) || block >= 875) assert(energy / N < 100.0 * 100.0);
        }
        speech_blocks += speaking;
        for (unsigned device = 0; device < 2; ++device) {
            media_echo_gate.far_end(state[device], history[1U - device][slot], N, FS);
            memcpy(history[device][slot], decoded[device], sizeof(decoded[device]));
        }
    }
    assert(speech_blocks == 150 && source_energy > 0 && heard_energy > 0);
    assert(-10 * log10(heard_energy / source_energy) < 1.0);
    media_echo_gate.close(state[1]);
    media_echo_gate.close(state[0]);
}

int main(void)
{
    static const xiaotai_media_param_t params[] = {{"noise_gate.enabled", 1}};
    assert(xiaotai_media_tune(params, 1) == XIAOTAI_OK);
    assert(media_select_processor(&xmini.desc, &speaker.desc) == &media_echo_gate);
    xiaotai_audio_desc_t reference = xmini.desc;
    reference.channels = 2;
    reference.roles[1] = XIAOTAI_CHANNEL_REFERENCE;
    assert(!media_select_processor(&reference, &speaker.desc));
    levels();
    ramps_and_hangover();
    gain_step_and_far_floor();
    live_switch();
    two_device_loop(4, 40);
    two_device_loop(10, 40);
    two_device_loop(16, 40);
    two_device_loop(4, 100);
    return 0;
}
