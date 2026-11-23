// SPDX-License-Identifier: MIT
#include "frontend_pairing.h"

#include <string.h>
#include "power.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

enum { INPUT_SLOTS_MAX = 8, PACKED_CHANNELS_MAX = 3, FETCH_PER_FEED_MAX = 2, RING_FRAMES_MAX = 32 };
_Static_assert(XIAOTAI_AUDIO_CHANNELS_MAX <= 8U, "monitor lanes fit the clipping mask");

typedef struct {
    int64_t end_us;
    uint32_t loss, process_us;
} stamp_t;

typedef struct {
    int16_t *packed;
    int64_t end_us;
    uint32_t loss;
} input_slot_t;

struct media_frontend_pairing {
    xiaotai_critical_t lock;
    xiaotai_processor_stats_t stats;
    media_frontend_ops_t ops;
    xiaotai_queue_t free_slots, pending, stamps;
    bool free_ready, pending_ready, stamps_ready;
    uint8_t monitor_clip_mask;
    input_slot_t inputs[INPUT_SLOTS_MAX];
    int16_t *clean;
    uint32_t rate, capture_loss, worker_loss, fetch_loss, stamps_owed;
    uint16_t feed_samples, fetch_samples;
    uint8_t input_channels, packed_channels, sources[PACKED_CHANNELS_MAX], frames_per_feed, stamp_wait_ms;
    bool strict;
};

bool media_frontend_pairing_slots(const xiaotai_audio_desc_t *in, uint8_t microphones, uint8_t *positions)
{
    if (!in || in->encoding != XIAOTAI_CODEC_PCM16 || in->rate_hz != 16000 || in->channels == 0 ||
        in->channels > XIAOTAI_AUDIO_CHANNELS_MAX || microphones == 0 || microphones >= PACKED_CHANNELS_MAX) return false;
    uint8_t refs = 0, mics = 0;
    for (uint8_t i = 0; i < in->channels; ++i) {
        if (in->roles[i] == XIAOTAI_CHANNEL_MIC) {
            if (mics >= microphones) return false;
            if (positions) positions[mics] = i;
            ++mics;
        } else if (in->roles[i] == XIAOTAI_CHANNEL_REFERENCE) {
            if (refs++ != 0) return false;
            if (positions) positions[microphones] = i;
        }
    }
    return mics == microphones && refs == 1U;
}

static bool failed(media_frontend_pairing_t *p)
{
    xiaotai_critical_enter(&p->lock);
    const bool result = p->stats.failed;
    xiaotai_critical_exit(&p->lock);
    return result;
}

static xiaotai_err_t fatal(media_frontend_pairing_t *p)
{
    xiaotai_critical_enter(&p->lock);
    if (!p->stats.failed) ++p->stats.errors;
    p->stats.failed = true;
    xiaotai_critical_exit(&p->lock);
    return XIAOTAI_ERR_INVALID_STATE;
}

xiaotai_err_t media_frontend_pairing_open(const media_frontend_pairing_config_t *cfg,
                                          const media_frontend_ops_t *ops, media_frontend_pairing_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = NULL;
    if (!cfg || !ops || !ops->feed || !ops->fetch || !media_frontend_pairing_slots(cfg->input, cfg->microphones, NULL) ||
        cfg->input_slots < 2U || cfg->input_slots > INPUT_SLOTS_MAX || cfg->ring_frames == 0 ||
        cfg->ring_frames > RING_FRAMES_MAX || cfg->stamp_wait_ms > 20 || !cfg->fetch_samples ||
        cfg->feed_samples < cfg->fetch_samples || cfg->feed_samples > XIAOTAI_PROCESSOR_SAMPLES_MAX ||
        (cfg->fetch_samples & 1U) || cfg->feed_samples % cfg->fetch_samples ||
        cfg->feed_samples / cfg->fetch_samples > FETCH_PER_FEED_MAX) return XIAOTAI_ERR_INVALID_ARG;
    media_frontend_pairing_t *p = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*p));
    if (!p) return XIAOTAI_ERR_NO_MEM;
    xiaotai_critical_init(&p->lock);
    p->ops = *ops;
    p->rate = cfg->input->rate_hz;
    p->strict = cfg->strict_feed;
    p->input_channels = cfg->input->channels;
    p->packed_channels = cfg->microphones + 1U;
    p->feed_samples = cfg->feed_samples;
    p->fetch_samples = cfg->fetch_samples;
    p->stamp_wait_ms = cfg->stamp_wait_ms;
    p->frames_per_feed = (uint8_t)(cfg->feed_samples / cfg->fetch_samples);
    (void)media_frontend_pairing_slots(cfg->input, cfg->microphones, p->sources);
    if (cfg->count_monitor_clipping) {
        for (uint8_t c = 0; c < p->input_channels; ++c)
            if (cfg->input->roles[c] == XIAOTAI_CHANNEL_MONITOR) p->monitor_clip_mask |= (uint8_t)(1U << c);
    }
    const size_t stamp_count = (size_t)cfg->ring_frames * FETCH_PER_FEED_MAX +
                               (size_t)cfg->input_slots * (p->strict ? FETCH_PER_FEED_MAX : p->frames_per_feed);
    if (xiaotai_queue_init(&p->free_slots, cfg->input_slots, sizeof(uint8_t), XIAOTAI_MEM_BULK) != XIAOTAI_OK) goto fail;
    p->free_ready = true;
    if (xiaotai_queue_init(&p->pending, cfg->input_slots, sizeof(uint8_t), XIAOTAI_MEM_BULK) != XIAOTAI_OK) goto fail;
    p->pending_ready = true;
    if (xiaotai_queue_init(&p->stamps, stamp_count, sizeof(stamp_t), XIAOTAI_MEM_BULK) != XIAOTAI_OK) goto fail;
    p->stamps_ready = true;
    p->clean = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 16, (size_t)p->fetch_samples * sizeof(int16_t));
    if (!p->clean) goto fail;
    for (uint8_t i = 0; i < cfg->input_slots; ++i) {
        p->inputs[i].packed = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 16,
            (size_t)p->feed_samples * p->packed_channels * sizeof(int16_t));
        if (!p->inputs[i].packed || !xiaotai_queue_send(&p->free_slots, &i, 0)) goto fail;
    }
    *out = p;
    return XIAOTAI_OK;
fail:
    media_frontend_pairing_close(p);
    return XIAOTAI_ERR_NO_MEM;
}

void media_frontend_pairing_close(media_frontend_pairing_t *p)
{
    if (!p) return;
    for (size_t i = 0; i < INPUT_SLOTS_MAX; ++i) xiaotai_mem_free(p->inputs[i].packed);
    xiaotai_mem_free(p->clean);
    if (p->stamps_ready) xiaotai_queue_deinit(&p->stamps);
    if (p->pending_ready) xiaotai_queue_deinit(&p->pending);
    if (p->free_ready) xiaotai_queue_deinit(&p->free_slots);
    xiaotai_mem_free(p);
}

static bool clipped(int16_t sample) { return sample >= 32700 || sample <= -32700; }

xiaotai_err_t media_frontend_pairing_feed(media_frontend_pairing_t *p, const void *data, size_t bytes,
                                          int64_t captured_us, uint32_t lost)
{
    if (failed(p)) return XIAOTAI_ERR_INVALID_STATE;
    if (!data || bytes != (size_t)p->feed_samples * p->input_channels * sizeof(int16_t)) return XIAOTAI_ERR_INVALID_SIZE;
    p->capture_loss = media_add_sat_u32(p->capture_loss, lost);
    uint8_t index;
    if (!xiaotai_queue_receive(&p->free_slots, &index, 0)) {
        p->capture_loss = media_add_sat_u32(p->capture_loss, p->feed_samples);
        xiaotai_critical_enter(&p->lock);
        ++p->stats.input_full;
        xiaotai_critical_exit(&p->lock);
        return XIAOTAI_ERR_NO_MEM;
    }
    input_slot_t *slot = &p->inputs[index];
    slot->end_us = captured_us;
    slot->loss = p->capture_loss;
    p->capture_loss = 0;
    uint32_t clipped_mic = 0, clipped_ref = 0;
    const uint8_t *input = data;
    for (size_t i = 0; i < p->feed_samples; ++i) {
        for (size_t c = 0; c < p->packed_channels; ++c) {
            int16_t sample;
            memcpy(&sample, input + (i * p->input_channels + p->sources[c]) * sizeof(sample), sizeof(sample));
            slot->packed[i * p->packed_channels + c] = sample;
            if (clipped(sample)) {
                if (c + 1U == p->packed_channels) ++clipped_ref;
                else ++clipped_mic;
            }
        }
    }
    for (uint8_t c = 0, lanes = p->monitor_clip_mask; lanes; ++c, lanes >>= 1) {
        if (!(lanes & 1U)) continue;
        for (size_t i = 0; i < p->feed_samples; ++i) {
            int16_t sample;
            memcpy(&sample, input + (i * p->input_channels + c) * sizeof(sample), sizeof(sample));
            clipped_mic += clipped(sample);
        }
    }
    xiaotai_critical_enter(&p->lock);
    p->stats.clipped_mic += clipped_mic;
    p->stats.clipped_reference += clipped_ref;
    xiaotai_critical_exit(&p->lock);
    /* Free + pending + at most one worker slot is bounded by construction. */
    if (!xiaotai_queue_send(&p->pending, &index, 0)) {
        (void)xiaotai_queue_send(&p->free_slots, &index, 0);
        return fatal(p);
    }
    return XIAOTAI_OK;
}

static bool publish(media_frontend_pairing_t *p, const input_slot_t *slot, uint32_t process_us)
{
    const int64_t frame_us = (int64_t)p->fetch_samples * 1000000 / p->rate;
    for (unsigned i = 0; i < p->frames_per_feed; ++i) {
        const stamp_t stamp = {.end_us = slot->end_us - (p->frames_per_feed - 1U - i) * frame_us,
            .loss = i == 0 ? media_add_sat_u32(slot->loss, p->worker_loss) : 0,
            .process_us = process_us / p->frames_per_feed};
        if (!xiaotai_queue_send(&p->stamps, &stamp, 0)) return false;
    }
    p->worker_loss = 0;
    return true;
}

xiaotai_err_t media_frontend_pairing_feed_next(media_frontend_pairing_t *p, uint32_t timeout_ms)
{
    uint8_t index;
    if (!xiaotai_queue_receive(&p->pending, &index, timeout_ms)) return XIAOTAI_ERR_TIMEOUT;
    input_slot_t *slot = &p->inputs[index];
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (!failed(p)) {
        if (p->strict && !publish(p, slot, 0)) result = fatal(p);
        else {
            const int64_t started = xiaotai_time_us();
            const int feed_result = p->ops.feed(p->ops.ctx, slot->packed);
            const uint32_t process_us = (uint32_t)(xiaotai_time_us() - started);
            const bool full = !p->strict && feed_result == 0;
            const bool fed = feed_result > 0 && (p->strict || publish(p, slot, process_us));
            if (full)
                p->worker_loss = media_add_sat_u32(media_add_sat_u32(p->worker_loss, slot->loss), p->feed_samples);
            xiaotai_critical_enter(&p->lock);
            /* Retain the latency of a failed attempt without counting it as a feed. */
            if (process_us > p->stats.process_max_us) p->stats.process_max_us = process_us;
            if (fed) {
                ++p->stats.feeds;
                if (process_us > (uint32_t)p->feed_samples * 1000000U / p->rate) ++p->stats.deadline_misses;
            } else if (full) ++p->stats.input_full;
            xiaotai_critical_exit(&p->lock);
            result = fed ? XIAOTAI_OK : full ? XIAOTAI_ERR_NO_MEM : fatal(p);
        }
    }
    if (!xiaotai_queue_send(&p->free_slots, &index, 0)) return fatal(p);
    return result;
}

static uint32_t remaining(int64_t deadline_us)
{
    const int64_t us = deadline_us - xiaotai_time_us();
    return us > 0 ? (uint32_t)((us + 999) / 1000) : 0;
}

xiaotai_err_t media_frontend_pairing_fetch(media_frontend_pairing_t *p, xiaotai_processor_frame_t *frame,
                                           uint32_t timeout_ms)
{
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    if (failed(p)) return XIAOTAI_ERR_INVALID_STATE;
    const int64_t end_us = xiaotai_time_us() + (int64_t)timeout_ms * 1000;
    const int16_t *pcm = NULL;
    size_t samples = 0;
    const xiaotai_err_t result = p->ops.fetch(p->ops.ctx, &pcm, &samples, timeout_ms);
    if (result == XIAOTAI_ERR_TIMEOUT) {
        xiaotai_critical_enter(&p->lock);
        ++p->stats.fetch_timeouts;
        xiaotai_critical_exit(&p->lock);
        return result;
    }
    if (result != XIAOTAI_OK || !pcm || samples != p->fetch_samples) return fatal(p);
    if (failed(p)) return XIAOTAI_ERR_INVALID_STATE;

    /* The frontend can make PCM visible just before publishing the stamp.
       Bound the whole wait, even when older, timed-out stamps arrive here. */
    const int64_t stamp_deadline = xiaotai_time_us() + (int64_t)p->stamp_wait_ms * 1000;
    const int64_t deadline = end_us < stamp_deadline ? end_us : stamp_deadline;
    stamp_t stamp;
    for (;;) {
        if (!xiaotai_queue_receive(&p->stamps, &stamp, remaining(deadline))) {
            if (p->strict) return fatal(p);
            p->stamps_owed = media_add_sat_u32(p->stamps_owed, 1);
            p->fetch_loss = media_add_sat_u32(p->fetch_loss, p->fetch_samples);
            xiaotai_critical_enter(&p->lock);
            ++p->stats.fetch_timeouts;
            xiaotai_critical_exit(&p->lock);
            return XIAOTAI_ERR_TIMEOUT;
        }
        p->fetch_loss = media_add_sat_u32(p->fetch_loss, stamp.loss);
        if (!p->stamps_owed) break;
        --p->stamps_owed;
    }

    memcpy(p->clean, pcm, samples * sizeof(*pcm));
    *frame = (xiaotai_processor_frame_t){.encoding = XIAOTAI_CODEC_PCM16, .data = p->clean,
        .size = samples * sizeof(*pcm), .samples = p->fetch_samples, .captured_us = stamp.end_us,
        .lost_samples = p->fetch_loss, .process_us = stamp.process_us};
    p->fetch_loss = 0;
    xiaotai_critical_enter(&p->lock);
    ++p->stats.frames;
    p->stats.lost_samples = media_add_sat_u32(p->stats.lost_samples, frame->lost_samples);
    xiaotai_critical_exit(&p->lock);
    return XIAOTAI_OK;
}

void media_frontend_pairing_stats(media_frontend_pairing_t *p, xiaotai_processor_stats_t *out)
{
    xiaotai_critical_enter(&p->lock);
    *out = p->stats;
    xiaotai_critical_exit(&p->lock);
}
