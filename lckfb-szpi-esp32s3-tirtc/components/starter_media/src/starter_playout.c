#include "starter_playout.h"

#include <string.h>

static uint32_t bounded_target(uint32_t ms)
{
    ms = ((ms + 19U) / 20U) * 20U;
    if (ms < STARTER_PLAYOUT_MIN_MS) return STARTER_PLAYOUT_MIN_MS;
    if (ms > STARTER_PLAYOUT_MAX_MS) return STARTER_PLAYOUT_MAX_MS;
    return ms;
}

void starter_playout_init(starter_playout_t *q)
{
    memset(q, 0, sizeof(*q));
    q->target_ms = STARTER_PLAYOUT_INITIAL_MS;
    q->buffering = true;
}

static void observe_arrival(starter_playout_t *q, uint32_t duration_us,
                            uint32_t timestamp_ms, uint8_t stream, uint64_t received_us)
{
    if (!q->have_arrival) {
        q->last_adjust_us = q->stable_since_us = received_us;
    } else if (stream == q->last_stream && received_us >= q->last_received_us) {
        uint64_t gap = received_us - q->last_received_us;
        uint32_t timestamp_step = timestamp_ms - q->last_timestamp_ms;
        uint64_t media_step_us = (uint64_t)timestamp_step * 1000U;
        uint64_t sample_step_us = q->last_duration_us;
        uint64_t timing_error = media_step_us > sample_step_us ?
            media_step_us - sample_step_us : sample_step_us - media_step_us;
        bool continuous = timestamp_step != 0 && timing_error <= 2000U;
        if (continuous) {
            if (q->timed_run < UINT8_MAX) ++q->timed_run;
        } else {
            q->timed_run = 0;
        }
        bool estimated = false;
        if (timestamp_step == 0) {
            /* Some peers have constant timestamps. Use only short arrival gaps
             * as a bounded estimate; do not call these packet loss/underruns. */
            ++q->untimed_pairs;
            estimated = gap <= 250000U;
            media_step_us = sample_step_us;
        } else if (!continuous) {
            /* DTX, an utterance timestamp reset and missing source frames are
             * indistinguishable here. Keep SDK delivery order; do not invent
             * samples, reorder or penalize a normal inter-utterance pause. */
            ++q->timestamp_breaks;
        }
        if (continuous || estimated) {
            if (gap > q->gap_max_us)
                q->gap_max_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
            uint64_t deviation = gap > media_step_us ? gap - media_step_us : media_step_us - gap;
            if (deviation > 250000U) deviation = 250000U;
            /* RFC 3550 interarrival smoothing (1/16), in microseconds rather
             * than RTP clock units. The margin/limits below are product policy,
             * not NetEq, loss concealment or a claim about SDK transport. */
            q->jitter_us = (uint32_t)((15U * (uint64_t)q->jitter_us + deviation) / 16U);
            uint32_t desired = bounded_target(STARTER_PLAYOUT_MIN_MS +
                                              (4U * q->jitter_us + 999U) / 1000U);
            if (continuous && gap > media_step_us + q->target_ms * 1000U) {
                ++q->late_bursts;
                uint32_t raised = bounded_target(q->target_ms + 40U);
                if (desired < raised) desired = raised;
            }
            if (desired >= q->target_ms) q->stable_since_us = received_us;
            if (desired > q->target_ms) {
                q->target_ms = desired;
                q->last_adjust_us = received_us;
            } else if (desired < q->target_ms &&
                       received_us - q->stable_since_us >= STARTER_PLAYOUT_STABLE_MS * 1000ULL &&
                       received_us - q->last_adjust_us >= STARTER_PLAYOUT_STABLE_MS * 1000ULL) {
                q->target_ms = bounded_target(q->target_ms - 20U);
                q->last_adjust_us = received_us;
            }
        } else {
            q->stable_since_us = received_us;
        }
    } else {
        q->stable_since_us = received_us;
        q->timed_run = 0;
    }
    q->last_received_us = received_us;
    q->last_timestamp_ms = timestamp_ms;
    q->last_duration_us = duration_us;
    q->last_stream = stream;
    q->have_arrival = true;
}

bool starter_playout_push(starter_playout_t *q, uint8_t slot, uint32_t duration_us,
                          uint32_t timestamp_ms, uint8_t stream, uint64_t received_us)
{
    if (q->count >= STARTER_PLAYOUT_CAPACITY || duration_us == 0 ||
        duration_us > UINT32_MAX - q->queued_us) return false;
    unsigned tail = (q->head + q->count) % STARTER_PLAYOUT_CAPACITY;
    q->entries[tail] = (starter_playout_entry_t) {
        .slot = slot, .duration_us = duration_us, .received_us = received_us,
    };
    ++q->count;
    q->queued_us += duration_us;
    observe_arrival(q, duration_us, timestamp_ms, stream, received_us);
    return true;
}

bool starter_playout_pop(starter_playout_t *q, uint64_t now_us, bool force, uint8_t *slot)
{
    if (q->count == 0 || slot == NULL) return false;
    const starter_playout_entry_t *head = &q->entries[q->head];
    uint64_t age = now_us >= head->received_us ? now_us - head->received_us : 0;
    if (!force && q->buffering) {
        /* The oldest frame's deadline also releases a short phrase that can
         * never fill the target. A fixed short quiet timeout would bypass the
         * adaptive target precisely when a jitter burst needs more buffering. */
        if (q->queued_us < q->target_ms * 1000U &&
            age < q->target_ms * 1000U &&
            q->count < STARTER_PLAYOUT_CAPACITY) return false;
        q->buffering = false;
        q->rate_start_us = now_us;
        q->rate_mode = 0;
        ++q->starts;
    }
    if (!force && age > q->residence_max_us)
        q->residence_max_us = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
    *slot = head->slot;
    q->queued_us -= head->duration_us;
    q->head = (q->head + 1U) % STARTER_PLAYOUT_CAPACITY;
    --q->count;
    return true;
}

void starter_playout_written(starter_playout_t *q, uint64_t now_us)
{
    q->last_write_us = now_us;
}

void starter_playout_idle(starter_playout_t *q, uint64_t now_us)
{
    /* An empty software FIFO is NOT an I2S underrun: DMA may still contain
     * audio. Allow the existing 6 x 240 / 16 kHz DMA horizon (90 ms) to drain
     * before restarting prebuffering. No loss counter is inferred from silence. */
    if (q->count == 0 && q->last_write_us != 0 && now_us >= q->last_write_us &&
        now_us - q->last_write_us >= STARTER_PLAYOUT_IDLE_MS * 1000U) {
        q->buffering = true;
        q->rate_mode = 0;
    }
}

uint32_t starter_playout_output_estimate(const starter_playout_t *q, uint64_t now_us)
{
    if (now_us < q->output_updated_us) return 0;
    uint64_t elapsed = now_us - q->output_updated_us;
    return elapsed < q->output_estimate_us ? q->output_estimate_us - (uint32_t)elapsed : 0;
}

void starter_playout_output_written(starter_playout_t *q, uint64_t now_us, size_t samples)
{
    uint64_t estimate = starter_playout_output_estimate(q, now_us) + (uint64_t)samples * 125U;
    q->output_estimate_us = estimate > STARTER_PLAYOUT_DMA_ESTIMATE_US ?
        STARTER_PLAYOUT_DMA_ESTIMATE_US : (uint32_t)estimate;
    q->output_updated_us = now_us;
    starter_playout_written(q, now_us);
}

uint16_t starter_playout_quantum(starter_playout_t *q, size_t pending_samples, uint64_t now_us)
{
    uint64_t available = q->queued_us + (uint64_t)pending_samples * 125U;
    uint64_t level = available + starter_playout_output_estimate(q, now_us);
    /* The prebuffer still ranges from 60 to 500 ms. Rate control must not try
     * to drain a 90 ms DMA pipeline below its own scheduling headroom. */
    uint32_t target = (q->target_ms < 120U ? 120U : q->target_ms) * 1000U;
    if (q->buffering || q->timed_run < 8U || now_us < q->rate_start_us ||
        now_us - q->rate_start_us < STARTER_PLAYOUT_RATE_WARMUP_US ||
        now_us < q->last_received_us || now_us - q->last_received_us > target) {
        q->rate_mode = 0;
    } else if (q->rate_mode < 0) {
        if (level >= target - 20000U) q->rate_mode = 0;
    } else if (q->rate_mode > 0) {
        if (level <= target + 20000U) q->rate_mode = 0;
    } else if (level < target - 60000U) {
        q->rate_mode = -1;
    } else if (level > target + 60000U) {
        q->rate_mode = 1;
    }
    /* Never wait for an extra source sample just to speed up a short tail. */
    if (q->rate_mode > 0 && available < STARTER_PLAYOUT_PCM_CAPACITY * 125U)
        q->rate_mode = 0;
    return (uint16_t)((int)STARTER_PLAYOUT_PCM_SAMPLES + q->rate_mode);
}

void starter_playout_pcm_init(starter_playout_pcm_t *pcm)
{
    memset(pcm, 0, sizeof(*pcm));
    pcm->limit = STARTER_PLAYOUT_PCM_SAMPLES;
}

size_t starter_playout_pcm_append(starter_playout_pcm_t *pcm,
                                  const int16_t *samples, size_t count)
{
    if (samples == NULL || count == 0) return 0;
    if (pcm->limit < STARTER_PLAYOUT_PCM_SAMPLES - 1U ||
        pcm->limit > STARTER_PLAYOUT_PCM_CAPACITY || pcm->count > pcm->limit) return 0;
    size_t take = pcm->limit - pcm->count;
    if (take > count) take = count;
    if (take == 0) return 0;
    memcpy(pcm->samples + pcm->count, samples, take * sizeof(*samples));
    memset(pcm->packet_ends + pcm->count, 0, take);
    pcm->count += take;
    pcm->packet_ends[pcm->count - 1U] = take == count;
    return take;
}

unsigned starter_playout_pcm_commit(starter_playout_pcm_t *pcm, bool written)
{
    unsigned completed = 0;
    for (unsigned i = 0; i < pcm->count; ++i) {
        if (!written) pcm->failed_packet = true;
        if (pcm->packet_ends[i]) {
            if (!pcm->failed_packet) ++completed;
            pcm->failed_packet = false;
        }
    }
    pcm->count = 0;
    return completed;
}

bool starter_playout_tail_ready(uint64_t first_sample_us, uint64_t now_us)
{
    return now_us >= first_sample_us &&
           now_us - first_sample_us >= STARTER_PLAYOUT_TAIL_WAIT_US;
}

uint32_t starter_playout_pcm_hash(uint32_t hash, const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint16_t sample = (uint16_t)samples[i];
        hash = (hash ^ (sample & 0xffU)) * UINT32_C(16777619);
        hash = (hash ^ (sample >> 8)) * UINT32_C(16777619);
    }
    return hash;
}

size_t starter_playout_pcm_render(const starter_playout_pcm_t *pcm, int16_t *output,
                                  size_t capacity, bool tail, uint16_t *fade_remaining)
{
    if (!pcm || !output || !fade_remaining || pcm->count == 0 ||
        pcm->count > STARTER_PLAYOUT_PCM_CAPACITY) return 0;
    size_t count = tail ? pcm->count : STARTER_PLAYOUT_PCM_SAMPLES;
    if (count > capacity || count > STARTER_PLAYOUT_PCM_SAMPLES) return 0;
    if (!tail && (pcm->limit < STARTER_PLAYOUT_PCM_SAMPLES - 1U ||
                  pcm->limit > STARTER_PLAYOUT_PCM_CAPACITY || pcm->count != pcm->limit)) return 0;
    if (pcm->count == count && *fade_remaining == 0) {
        memcpy(output, pcm->samples, count * sizeof(*output));
        return count;
    }
    if (*fade_remaining > STARTER_PLAYOUT_FADE_SAMPLES)
        *fade_remaining = STARTER_PLAYOUT_FADE_SAMPLES;
    for (size_t i = 0; i < count; ++i) {
        int32_t value;
        if (pcm->count == count || count == 1) {
            value = pcm->samples[i];
        } else {
            size_t position = i * (pcm->count - 1U);
            size_t left = position / (count - 1U);
            size_t fraction = position % (count - 1U);
            size_t right = left + 1U < pcm->count ? left + 1U : left;
            value = pcm->samples[left] +
                ((int32_t)pcm->samples[right] - pcm->samples[left]) * (int32_t)fraction /
                    (int32_t)(count - 1U);
        }
        if (*fade_remaining != 0) {
            value = value * (int32_t)(STARTER_PLAYOUT_FADE_SAMPLES - *fade_remaining + 1U) /
                (int32_t)STARTER_PLAYOUT_FADE_SAMPLES;
            --*fade_remaining;
        }
        output[i] = (int16_t)value;
    }
    return count;
}
