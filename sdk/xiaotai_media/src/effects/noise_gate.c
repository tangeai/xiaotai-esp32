// SPDX-License-Identifier: MIT
#include "portable_audio.h"

#include "power.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/media.h"
#include "xiaotai/port/mem.h"

enum { NOISE_MAX = 1U << 30, NOISE_MIN = 16, CLOSED_Q15 = 1036 };

typedef struct {
    uint32_t learning, hold, learning_samples, hold_samples, floor;
    uint32_t open_frames, closed_frames;
    int32_t attack, release;
    uint16_t gain;
} noise_gate_t;

static xiaotai_eventlog_topic_t *s_topic;

xiaotai_err_t media_noise_gate_topics_init(void)
{
    static const char *const fields[] = {"open_frames", "closed_frames", "noise"};
    static const xiaotai_eventlog_topic_desc_t desc = {.source = "media", .name = "noise_gate",
        .level = XIAOTAI_EVENTLOG_INFO, .fields = fields, .count = 3};
    return xiaotai_eventlog_topic_register(&desc, &s_topic);
}

void media_noise_gate_end(void *opaque)
{
    noise_gate_t *gate = opaque;
    if (!gate || (!gate->open_frames && !gate->closed_frames)) return;
    const uint32_t values[] = {gate->open_frames, gate->closed_frames, gate->floor};
    xiaotai_eventlog_post(s_topic, values);
    gate->open_frames = gate->closed_frames = 0;
}

static void reset(void *opaque)
{
    noise_gate_t *gate = opaque;
    gate->floor = NOISE_MAX;
    gate->gain = CLOSED_Q15;
    gate->learning = gate->learning_samples;
    gate->hold = 0;
}

static xiaotai_err_t open_gate(const xiaotai_processor_info_t *processor, xiaotai_effect_info_t *info, void **state)
{
    if (!processor || !info || !state) return XIAOTAI_ERR_INVALID_ARG;
    if (processor->rate_hz < 8000U || processor->rate_hz > 48000U ||
        processor->flags & (XIAOTAI_PROCESSOR_GATES | XIAOTAI_PROCESSOR_ENCODED) ||
        !xiaotai_media_param("noise_gate.enabled", 0)) return XIAOTAI_ERR_NOT_SUPPORTED;
    noise_gate_t *gate = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*gate));
    if (!gate) return XIAOTAI_ERR_NO_MEM;
    gate->learning_samples = processor->rate_hz / 5U;
    gate->hold_samples = processor->rate_hz * 3U / 10U;
    gate->attack = (int32_t)((1024U * 16000U + processor->rate_hz / 2U) / processor->rate_hz);
    gate->release = (int32_t)((16U * 16000U + processor->rate_hz / 2U) / processor->rate_hz);
    reset(gate);
    *info = (xiaotai_effect_info_t){.stack_bytes = 512};
    *state = gate;
    return XIAOTAI_OK;
}

static xiaotai_err_t process(void *opaque, int16_t *pcm, size_t samples)
{
    if (!opaque || (!pcm && samples)) return XIAOTAI_ERR_INVALID_ARG;
    if (!samples) return XIAOTAI_OK;
    noise_gate_t *gate = opaque;
    const uint32_t power = media_pcm_power(pcm, samples);
    bool speech = false;
    if (gate->learning) {
        gate->floor = gate->floor == NOISE_MAX ? power : gate->floor / 2U + power / 2U;
        gate->learning = samples < gate->learning ? gate->learning - (uint32_t)samples : 0;
    } else {
        speech = (uint64_t)power > (uint64_t)gate->floor * 20U;
        if (power < gate->floor) {
            gate->floor -= (gate->floor - power) >> 2;
        } else if (speech || gate->hold) {
            const uint32_t grown = gate->floor + (gate->floor >> 8) + 1U;
            gate->floor = grown < power ? grown : power;
        } else {
            gate->floor += ((power - gate->floor) >> 6) + 1U;
        }
    }
    if (gate->floor < NOISE_MIN) gate->floor = NOISE_MIN;
    if (speech) gate->hold = gate->hold_samples;
    else gate->hold = samples < gate->hold ? gate->hold - (uint32_t)samples : 0;
    const bool open = speech || gate->hold;
    gate->gain = media_ramp_q15(pcm, samples, gate->gain, open ? MEDIA_Q15_UNITY : CLOSED_Q15,
                               gate->attack, gate->release);
    if (open) ++gate->open_frames;
    else ++gate->closed_frames;
    return XIAOTAI_OK;
}

const xiaotai_audio_effect_t media_noise_gate = {.name = "noise_gate", .stage = XIAOTAI_EFFECT_UPLINK, .order = 30,
    .session_flags = XIAOTAI_MEDIA_GATE_IDLE_NOISE, .open = open_gate, .process = process, .reset = reset,
    .close = xiaotai_mem_free};
