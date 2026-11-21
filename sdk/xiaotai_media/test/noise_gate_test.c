// SPDX-License-Identifier: MIT
// Replaces legacy: tests/native/cores3/test_uplink_gate.py
#include <assert.h>
#include <math.h>
#include <string.h>
#include "portable_audio.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/media.h"

static void alternate(int16_t *pcm, size_t samples, int16_t magnitude)
{
    for (size_t i = 0; i < samples; ++i) pcm[i] = i & 1U ? -magnitude : magnitude;
}

enum { FRAME = 320, RATE = 16000 };
static uint64_t rng = 1;

static double uniform(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return ((rng >> 11) + 0.5) / 9007199254740992.0;
}

static double gaussian(void) { return sqrt(-2 * log(uniform())) * cos(6.283185307179586 * uniform()); }

static void signal_frame(int16_t pcm[FRAME], double noise, unsigned index, bool speech)
{
    for (unsigned i = 0; i < FRAME; ++i) {
        const double voice = speech ? 2000 * sin(6.283185307179586 * 300 * (index * FRAME + i) / RATE) : 0;
        pcm[i] = (int16_t)lround(voice + noise * gaussian());
    }
}

static double energy(const int16_t pcm[FRAME])
{
    double sum = 0;
    for (unsigned i = 0; i < FRAME; ++i) sum += (double)pcm[i] * pcm[i];
    return sum;
}

static xiaotai_eventlog_topic_snapshot_t counters(void *state)
{
    xiaotai_eventlog_topic_snapshot_t snapshot;
    media_noise_gate_end(state);
    assert(xiaotai_eventlog_topic_read(0, &snapshot) && !strcmp(snapshot.desc->name, "noise_gate"));
    return snapshot;
}

static void cores3_room_noise(const xiaotai_processor_info_t *input)
{
    xiaotai_effect_info_t info;
    void *state, *cold;
    assert(media_noise_gate.open(input, &info, &state) == XIAOTAI_OK);
    int16_t pcm[FRAME], expected[FRAME];
    for (unsigned i = 0; i < 100; ++i) {
        signal_frame(pcm, 104, i, false);
        const double before = energy(pcm);
        assert(media_noise_gate.process(state, pcm, FRAME) == XIAOTAI_OK);
        if (i > 2) assert(energy(pcm) < before * 0.04 * 0.04);
    }
    xiaotai_eventlog_topic_snapshot_t snap = counters(state);
    assert(snap.values[0] == 0 && snap.values[1] == 100);
    for (unsigned i = 0; i < 300; ++i) {
        const double level = 104 * pow(10, gaussian() * 3.5 / 20);
        signal_frame(pcm, level, i, false);
        assert(media_noise_gate.process(state, pcm, FRAME) == XIAOTAI_OK);
    }
    assert(counters(state).values[1] > 270);

    assert(media_noise_gate.open(input, &info, &cold) == XIAOTAI_OK);
    for (unsigned i = 0; i <= 100; ++i) {
        signal_frame(pcm, i ? 110 : 20, i, false);
        assert(media_noise_gate.process(cold, pcm, FRAME) == XIAOTAI_OK);
    }
    assert(counters(cold).values[0] < 10);
    media_noise_gate.close(cold);

    for (unsigned i = 0; i < 30; ++i) {
        signal_frame(expected, 104, i, true);
        memcpy(pcm, expected, sizeof(pcm));
        assert(media_noise_gate.process(state, pcm, FRAME) == XIAOTAI_OK);
        const size_t first = i ? 0 : 32;
        assert(memcmp(pcm + first, expected + first, (FRAME - first) * sizeof(*pcm)) == 0);
    }
    for (unsigned i = 0; i < 14; ++i) {
        signal_frame(expected, 104, i, false);
        memcpy(pcm, expected, sizeof(pcm));
        assert(media_noise_gate.process(state, pcm, FRAME) == XIAOTAI_OK);
        assert(memcmp(pcm, expected, sizeof(pcm)) == 0);
    }
    int32_t gain = 32768;
    for (unsigned i = 0; i < 20; ++i) {
        signal_frame(expected, 104, i, false);
        memcpy(pcm, expected, sizeof(pcm));
        assert(media_noise_gate.process(state, pcm, FRAME) == XIAOTAI_OK);
        for (unsigned j = 0; j < FRAME; ++j) {
            gain = gain > 1052 ? gain - 16 : 1036;
            assert(pcm[j] == (int16_t)(((int32_t)expected[j] * gain + 16384) >> 15));
        }
    }
    (void)counters(state);
    for (unsigned i = 0; i < 400; ++i) {
        signal_frame(pcm, 416, i, false);
        assert(media_noise_gate.process(state, pcm, FRAME) == XIAOTAI_OK);
        if (i == 299) assert(counters(state).values[0] < 220);
    }
    assert(counters(state).values[0] == 0);
    for (unsigned i = 0; i < FRAME; ++i) expected[i] = i & 1U ? INT16_MAX : INT16_MIN;
    memcpy(pcm, expected, sizeof(pcm));
    assert(media_noise_gate.process(state, pcm, FRAME) == XIAOTAI_OK);
    assert(memcmp(pcm + 32, expected + 32, (FRAME - 32) * sizeof(*pcm)) == 0);
    snap = counters(state);
    assert(media_noise_gate.process(state, NULL, 0) == XIAOTAI_OK);
    assert(counters(state).posts == snap.posts);
    media_noise_gate.close(state);
}

int main(void)
{
    const xiaotai_processor_info_t input = {.rate_hz = 16000, .flags = XIAOTAI_PROCESSOR_CANCELS_ECHO};
    const xiaotai_processor_info_t gated = {.rate_hz = 8000, .flags = XIAOTAI_PROCESSOR_GATES};
    xiaotai_effect_info_t info;
    void *state;
    assert(media_noise_gate.open(&input, &info, &state) == XIAOTAI_ERR_NOT_SUPPORTED);
    static const xiaotai_media_param_t parameters[] = {{"noise_gate.enabled", 1}};
    assert(xiaotai_media_tune(parameters, 1) == XIAOTAI_OK);
    assert(media_noise_gate.session_flags == XIAOTAI_MEDIA_GATE_IDLE_NOISE);
    assert(media_noise_gate.open(&gated, &info, &state) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(media_noise_gate.open(&input, &info, &state) == XIAOTAI_OK && info.delay_samples == 0);
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(media_noise_gate_topics_init() == XIAOTAI_OK);
    int16_t pcm[320];
    for (unsigned i = 0; i < 10; ++i) {
        alternate(pcm, 320, i == 0 ? 20 : 110);
        assert(media_noise_gate.process(state, pcm, 320) == XIAOTAI_OK);
        assert(pcm[318] <= 4); /* the first quiet frame cannot set the whole noise floor */
    }
    alternate(pcm, 320, 2000);
    assert(media_noise_gate.process(state, pcm, 320) == XIAOTAI_OK);
    assert(pcm[0] < 2000 && pcm[32] == 2000 && pcm[319] == -2000);
    for (unsigned i = 0; i < 14; ++i) {
        alternate(pcm, 320, 104);
        assert(media_noise_gate.process(state, pcm, 320) == XIAOTAI_OK && pcm[318] == 104);
    }
    alternate(pcm, 320, 104);
    assert(media_noise_gate.process(state, pcm, 320) == XIAOTAI_OK && pcm[318] < 104 && pcm[318] > 3);
    for (unsigned i = 0; i < 10; ++i) {
        alternate(pcm, 320, 104);
        assert(media_noise_gate.process(state, pcm, 320) == XIAOTAI_OK);
    }
    assert(pcm[318] == 3);
    xiaotai_eventlog_topic_snapshot_t topic;
    assert(xiaotai_eventlog_topic_read(0, &topic) && topic.posts == 0);
    media_noise_gate_end(state);
    assert(xiaotai_eventlog_topic_read(0, &topic) && topic.posts == 1 && !strcmp(topic.desc->name, "noise_gate"));
    assert(topic.values[0] == 15 && topic.values[1] == 21 && topic.values[2] >= 104 * 104);
    media_noise_gate_end(state);
    assert(xiaotai_eventlog_topic_read(0, &topic) && topic.posts == 1);
    media_noise_gate.reset(state);
    alternate(pcm, 320, 1000);
    assert(media_noise_gate.process(state, pcm, 320) == XIAOTAI_OK && pcm[318] == 32);
    media_noise_gate.close(state);
    cores3_room_noise(&input);
    xiaotai_eventlog_reset();
    return 0;
}
