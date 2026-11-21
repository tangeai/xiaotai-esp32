// SPDX-License-Identifier: MIT
#include "portable_audio.h"

#include <string.h>
#include "level.h"
#include "power.h"
#include "xiaotai/media.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

enum {
    GATE_MS = 20,
    HANG_MS = 160,
    FAR_HISTORY = 12,
    FAR_WINDOW = 64,
    FAR_FLOOR_MAX = 400 * 400,
    NOISE_MIN = 64,
    NOISE_MAX = 1U << 30,
    EXCESS_REF_DB = 6,
    UNMEASURED_COUPLING_DB = 12,
};

typedef struct {
    /* Only short history/level/counter snapshots are shared with playout. */
    xiaotai_critical_t lock;
    uint32_t far[FAR_HISTORY], far_min, far_previous_min, far_floor;
    uint8_t far_next, far_count;
    uint8_t requested_volume;
    int8_t requested_gain;
    xiaotai_processor_stats_t stats;

    uint32_t rate, settle, power_ratio_q8, noise, startup, hang, pending_loss;
    uint16_t block, volume_span, gain_q15, closed_q15, double_q15;
    uint8_t mic, channels, gain_step;
    int8_t coupling, gain_min, gain_max;
    int mic_gain;
    bool active, enabled, ready;
    int64_t capture_us;
    uint32_t frame_loss;
    int16_t pcm[];
} echo_gate_t;

static uint32_t power_ratio_q8(int db)
{
    static const uint16_t tenths[] = {256, 322, 406, 511, 643, 810, 1019, 1283, 1615, 2033};
    if (db < -30) return 0;
    if (db > 60) db = 60;
    uint32_t value = tenths[(db + 30) % 10];
    for (int decade = (db + 30) / 10; decade < 3; ++decade) value = (value + 5U) / 10U;
    for (int decade = (db + 30) / 10; decade > 3; --decade) value *= 10U;
    return value;
}

static uint16_t attenuation_q15(int db_below_unity)
{
    static const uint16_t values[] = {32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627,
                                      10362, 9235, 8231, 7336, 6538, 5827, 5193, 4629, 4125, 3677};
    uint32_t value = values[db_below_unity % 20];
    for (int decade = db_below_unity / 20; decade > 0 && value; --decade) value = (value + 5U) / 10U;
    return (uint16_t)value;
}

static void apply_levels(echo_gate_t *gate, uint8_t volume, int gain)
{
    if (gate->gain_step != 0) {
        int rem = gain % gate->gain_step;
        if (rem < 0) rem += gate->gain_step;
        gain -= rem;
    }
    if (gain < gate->gain_min) gain = gate->gain_min;
    if (gain > gate->gain_max) gain = gate->gain_max;
    if (gain != gate->mic_gain) {
        const uint64_t adjusted = (uint64_t)gate->noise * power_ratio_q8(gain - gate->mic_gain) >> 8;
        gate->noise = adjusted > NOISE_MAX ? NOISE_MAX : (uint32_t)adjusted;
        gate->mic_gain = gain;
    }
    const int volume_db = -(int)gate->volume_span * (100 - volume) / 9900;
    const int erl = gate->coupling + gain + volume_db;
    const int excess = volume != 0 && erl > EXCESS_REF_DB ? erl - EXCESS_REF_DB : 0;
    gate->power_ratio_q8 = volume != 0 ? power_ratio_q8(erl) : 0;
    gate->closed_q15 = attenuation_q15(40 + 2 * excess);
    gate->double_q15 = attenuation_q15(6 + excess);
}

static int match(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    (void)out;
    if (!in || in->encoding != XIAOTAI_CODEC_PCM16 || in->channels == 0 ||
        in->channels > XIAOTAI_AUDIO_CHANNELS_MAX || in->rate_hz == 0 ||
        in->rate_hz % 50U || in->rate_hz / 50U > XIAOTAI_PROCESSOR_SAMPLES_MAX) return 0;
    unsigned microphones = 0;
    for (size_t i = 0; i < in->channels; ++i) {
        if (in->roles[i] == XIAOTAI_CHANNEL_REFERENCE) return 0;
        microphones += in->roles[i] == XIAOTAI_CHANNEL_MIC;
    }
    return microphones == 1 ? 1 : 0;
}

static void levels(void *opaque, uint8_t volume_percent, int8_t gain_db)
{
    echo_gate_t *gate = opaque;
    xiaotai_critical_enter(&gate->lock);
    gate->requested_volume = volume_percent > 100 ? 100 : volume_percent;
    gate->requested_gain = gain_db;
    xiaotai_critical_exit(&gate->lock);
}

void media_echo_gate_set(void *opaque, bool enabled)
{
    echo_gate_t *gate = opaque;
    xiaotai_critical_enter(&gate->lock);
    gate->enabled = enabled;
    xiaotai_critical_exit(&gate->lock);
}

bool media_echo_gate_enabled(void *opaque)
{
    echo_gate_t *gate = opaque;
    xiaotai_critical_enter(&gate->lock);
    const bool enabled = gate->enabled;
    xiaotai_critical_exit(&gate->lock);
    return enabled;
}

static void session(void *opaque, bool active)
{
    echo_gate_t *gate = opaque;
    gate->active = active;
    if (!active) return;
    gate->hang = 0;
    gate->startup = gate->settle;
    xiaotai_critical_enter(&gate->lock);
    memset(gate->far, 0, sizeof(gate->far));
    gate->far_min = gate->far_previous_min = UINT32_MAX;
    gate->far_floor = 0;
    gate->far_count = gate->far_next = 0;
    const uint8_t volume = gate->requested_volume;
    const int8_t gain = gate->requested_gain;
    xiaotai_critical_exit(&gate->lock);
    apply_levels(gate, volume, gain);
    gate->noise = NOISE_MAX;
    gate->gain_q15 = gate->closed_q15;
}

static xiaotai_err_t open_gate(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                               xiaotai_processor_info_t *info, void **state)
{
    if (!info || !state || !match(in ? &in->desc : NULL, out ? &out->desc : NULL) ||
        in->info.gain_min_db > in->info.gain_max_db) return XIAOTAI_ERR_INVALID_ARG;
    const uint32_t rate = in->desc.rate_hz;
    const uint16_t block = (uint16_t)(rate * GATE_MS / 1000U);
    echo_gate_t *gate = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*gate) + block * sizeof(int16_t));
    if (!gate) return XIAOTAI_ERR_NO_MEM;
    xiaotai_critical_init(&gate->lock);
    gate->rate = rate;
    gate->block = block;
    gate->settle = (uint32_t)((uint64_t)rate * in->info.settle_ms / 1000U);
    gate->gain_min = in->info.gain_min_db;
    gate->gain_max = in->info.gain_max_db;
    gate->gain_step = in->info.gain_step_db;
    gate->coupling = in->info.echo_coupling_db == XIAOTAI_AUDIO_COUPLING_UNKNOWN ? UNMEASURED_COUPLING_DB
                                                                              : in->info.echo_coupling_db;
    gate->volume_span = out ? out->info.volume_span_cdb : 0;
    gate->enabled = out != NULL && xiaotai_media_param("echo_gate.enabled", 1) != 0;
    gate->channels = in->desc.channels;
    while (in->desc.roles[gate->mic] != XIAOTAI_CHANNEL_MIC) ++gate->mic;
    levels(gate, 0, in->info.gain_db);
    session(gate, true);
    session(gate, false);
    gate->stats.gain_q15 = MEDIA_Q15_UNITY;
    *info = (xiaotai_processor_info_t){.flags = gate->enabled ? XIAOTAI_PROCESSOR_FAR_END | XIAOTAI_PROCESSOR_GATES : 0,
        .rate_hz = rate, .feed_frames = block, .fetch_max = block, .feed_stack_bytes = 512, .fetch_stack_bytes = 512};
    *state = gate;
    return XIAOTAI_OK;
}

static void far_end(void *opaque, const int16_t *pcm, size_t samples, uint32_t rate_hz)
{
    echo_gate_t *gate = opaque;
    if (!pcm || !samples || !rate_hz) return;
    const uint32_t power = media_pcm_power(pcm, samples);
    xiaotai_critical_enter(&gate->lock);
    if (power < gate->far_min) gate->far_min = power;
    if (++gate->far_count >= FAR_WINDOW) {
        gate->far_previous_min = gate->far_min;
        gate->far_min = UINT32_MAX;
        gate->far_count = 0;
    }
    const uint32_t now = gate->far_min > FAR_FLOOR_MAX ? 0 : gate->far_min;
    const uint32_t prev = gate->far_previous_min > FAR_FLOOR_MAX ? 0 : gate->far_previous_min;
    gate->far_floor = now < prev ? now : prev;
    gate->far[gate->far_next] = power;
    gate->far_next = (uint8_t)((gate->far_next + 1U) % FAR_HISTORY);
    xiaotai_critical_exit(&gate->lock);
}

static xiaotai_err_t feed(void *opaque, const void *data, size_t size, int64_t end_us, uint32_t lost)
{
    echo_gate_t *gate = opaque;
    if (!data || size != (size_t)gate->block * gate->channels * sizeof(int16_t)) return XIAOTAI_ERR_INVALID_SIZE;
    if (gate->ready) {
        gate->pending_loss = media_add_sat_u32(gate->pending_loss, media_add_sat_u32(lost, gate->block));
        xiaotai_critical_enter(&gate->lock);
        ++gate->stats.input_full;
        xiaotai_critical_exit(&gate->lock);
        return XIAOTAI_ERR_NO_MEM;
    }

    uint32_t clipped = 0;
    const uint8_t *input = data;
    for (size_t i = 0; i < gate->block; ++i) {
        int16_t sample;
        memcpy(&sample, input + (i * gate->channels + gate->mic) * sizeof(sample), sizeof(sample));
        gate->pcm[i] = sample;
        clipped += sample <= -32700 || sample >= 32700;
    }

    uint32_t far = 0, floor = 0;
    xiaotai_critical_enter(&gate->lock);
    const uint8_t volume = gate->requested_volume;
    const int8_t gain = gate->requested_gain;
    const bool gating = gate->active && gate->enabled;
    for (size_t i = 0; i < FAR_HISTORY; ++i) if (gate->far[i] > far) far = gate->far[i];
    floor = gate->far_floor;
    xiaotai_critical_exit(&gate->lock);

    uint16_t applied = MEDIA_Q15_UNITY;
    bool open = false, echoed = false;
    if (gating) {
        apply_levels(gate, volume, gain);
        const uint32_t power = media_pcm_power(gate->pcm, gate->block);
        const uint32_t rise = gate->noise + (gate->noise >> 6) + 1U;
        gate->noise = power < rise ? power : rise;
        if (gate->noise < NOISE_MIN) gate->noise = NOISE_MIN;
        const uint64_t floor4 = 4ULL * floor;
        const uint64_t echo = ((far > floor4 ? far - floor4 : 0) * gate->power_ratio_q8) >> 8;
        const uint64_t threshold = echo > gate->noise ? echo : gate->noise;
        const bool speaking = gate->startup == 0 && power > threshold * 8U;
        open = speaking || gate->hang > 0;
        echoed = echo > 2ULL * gate->noise && echo * 100U > power;
        gate->startup = gate->startup > gate->block ? gate->startup - gate->block : 0;
        if (speaking) gate->hang = gate->rate * HANG_MS / 1000U;
        else gate->hang = gate->hang > gate->block ? gate->hang - gate->block : 0;
        const int32_t target = !open ? gate->closed_q15 : echoed ? gate->double_q15 : MEDIA_Q15_UNITY;
        gate->gain_q15 = media_ramp_q15(gate->pcm, gate->block, gate->gain_q15, target, 1024, 82);
        applied = gate->gain_q15;
    }

    gate->capture_us = end_us;
    gate->frame_loss = media_add_sat_u32(lost, gate->pending_loss);
    gate->pending_loss = 0;
    gate->ready = true;
    const uint16_t far_floor = (uint16_t)xiaotai_media_isqrt(floor);
    const uint16_t near_floor = (uint16_t)xiaotai_media_isqrt(gate->noise);
    xiaotai_critical_enter(&gate->lock);
    ++gate->stats.feeds;
    gate->stats.clipped_mic += clipped;
    gate->stats.gain_q15 = applied;
    gate->stats.far_floor = far_floor;
    gate->stats.near_floor = near_floor;
    if (gating) {
        if (!open) ++gate->stats.closed_frames;
        else if (echoed) ++gate->stats.double_talk_frames;
        else ++gate->stats.open_frames;
    }
    xiaotai_critical_exit(&gate->lock);
    return XIAOTAI_OK;
}

static xiaotai_err_t fetch(void *opaque, xiaotai_processor_frame_t *frame, uint32_t timeout_ms)
{
    (void)timeout_ms;
    echo_gate_t *gate = opaque;
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    if (!gate->ready) return XIAOTAI_ERR_TIMEOUT;
    gate->ready = false;
    *frame = (xiaotai_processor_frame_t){.encoding = XIAOTAI_CODEC_PCM16, .data = gate->pcm,
        .size = (size_t)gate->block * sizeof(int16_t), .samples = gate->block, .captured_us = gate->capture_us,
        .lost_samples = gate->frame_loss};
    xiaotai_critical_enter(&gate->lock);
    ++gate->stats.frames;
    gate->stats.lost_samples = media_add_sat_u32(gate->stats.lost_samples, frame->lost_samples);
    xiaotai_critical_exit(&gate->lock);
    return XIAOTAI_OK;
}

static void stats(void *opaque, xiaotai_processor_stats_t *out)
{
    echo_gate_t *gate = opaque;
    xiaotai_critical_enter(&gate->lock);
    *out = gate->stats;
    xiaotai_critical_exit(&gate->lock);
}

const xiaotai_audio_processor_t media_echo_gate = {.name = "echo_gate", .match = match, .open = open_gate,
    .feed = feed, .fetch = fetch, .far_end = far_end, .levels = levels, .session = session, .stats = stats,
    .close = xiaotai_mem_free};
