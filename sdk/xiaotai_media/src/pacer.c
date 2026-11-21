// SPDX-License-Identifier: MIT
#include "pacer.h"
#include "xiaotai/port/os.h"

static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t max(uint32_t a, uint32_t b) { return a > b ? a : b; }

enum {
    TIMED_INITIAL_MS = 200, TIMED_MIN_MS = 60, TIMED_MAX_MS = 500, TIMED_IDLE_MS = 120,
    TIMED_STABLE_MS = 10000, TIMED_WARMUP_MS = 2000, TIMED_RATE_FLOOR_MS = 120,
    TIMED_TAIL_MS = 60, TIMED_FIFO_PACKETS = 64, PREFILL_MS = 60, PREFILL_PACKETS = 3,
};

static uint64_t elapsed(int64_t now, int64_t then)
{
    return now >= then ? (uint64_t)now - (uint64_t)then : 0;
}

void media_pacer_open(media_pacer_t *p, xiaotai_media_latency_t profile, uint32_t capacity_ms,
                      media_pacer_policy_t policy)
{
    if (!profile.max_ms) profile = (xiaotai_media_latency_t)XIAOTAI_MEDIA_LATENCY_BURSTY;
    const uint32_t available = capacity_ms > profile.surplus_ms ? capacity_ms - profile.surplus_ms : 0;
    profile.max_ms = (uint16_t)min(profile.max_ms, available);
    profile.min_ms = (uint16_t)min(profile.min_ms, profile.max_ms);
    profile.start_ms = (uint16_t)min(profile.start_ms, capacity_ms);
    if (!profile.steady_jitter_ms) profile.steady_jitter_ms = 8;
    if (!profile.steady_peak_ms) profile.steady_peak_ms = 10;
    if (!profile.step_down_ms) profile.step_down_ms = 10;
    if (!profile.slow_permille) profile.slow_permille = 12;
    *p = (media_pacer_t){.profile = profile, .policy = policy};
    switch (policy) {
        case MEDIA_PACER_TIMED:
            p->profile.max_ms = (uint16_t)min(capacity_ms, TIMED_MAX_MS);
            p->profile.min_ms = (uint16_t)min(p->profile.max_ms, TIMED_MIN_MS);
            p->target_ms = min(TIMED_INITIAL_MS, p->profile.max_ms);
            p->required_prefill_packets = TIMED_FIFO_PACKETS;
            break;
        case MEDIA_PACER_PACKET_PREFILL:
            p->target_ms = PREFILL_MS;
            p->required_prefill_packets = PREFILL_PACKETS;
            break;
        case MEDIA_PACER_FIXED: break;
        default:
            p->policy = MEDIA_PACER_ADAPTIVE;
            p->target_ms = min(max(profile.base_ms, profile.min_ms), profile.max_ms);
            break;
    }
}

void media_pacer_set_prefill_packets(media_pacer_t *p, int32_t packets)
{
    if (p->required_prefill_packets && packets > 0) p->required_prefill_packets = (uint32_t)packets;
}

static uint32_t timed_bound(const media_pacer_t *p, uint32_t ms)
{
    return min(max(p->profile.min_ms, ((ms + 19U) / 20U) * 20U), p->profile.max_ms);
}

static void timed_arrival(media_pacer_t *p, uint32_t stamp, int64_t now, bool new_stream)
{
    if (!p->have_arrival) {
        p->last_adjust_us = p->stable_since_us = now;
        return;
    }
    if (new_stream || now < p->last_arrival_us) {
        p->stable_since_us = now;
        p->timed_run = 0;
        return;
    }
    const uint64_t gap = elapsed(now, p->last_arrival_us);
    const uint32_t step = stamp - p->last_ts;
    const uint64_t media_us = (uint64_t)step * 1000U;
    const uint64_t duration = p->last_duration_us;
    const uint64_t error = media_us > duration ? media_us - duration : duration - media_us;
    const bool continuous = step && error <= 2000U;
    if (continuous) {
        if (p->timed_run < UINT8_MAX) ++p->timed_run;
    } else {
        p->timed_run = 0;
        if (step || gap > 250000U) {
            /* Timestamp discontinuities are not evidence of network loss. */
            p->stable_since_us = now;
            return;
        }
    }

    const uint64_t expected = continuous ? media_us : duration;
    const uint64_t deviation = gap > expected ? gap - expected : expected - gap;
    p->jitter_us = (15U * p->jitter_us + (deviation < 250000U ? (uint32_t)deviation : 250000U)) / 16U;
    p->jitter_q8 = p->jitter_us * 256U / 1000U;
    uint32_t desired = timed_bound(p, TIMED_MIN_MS + (4U * p->jitter_us + 999U) / 1000U);
    if (continuous && gap > media_us + (uint64_t)p->target_ms * 1000U)
        desired = max(desired, timed_bound(p, p->target_ms + 40U));
    if (desired >= p->target_ms) p->stable_since_us = now;
    if (desired > p->target_ms) {
        p->target_ms = desired;
        p->last_adjust_us = now;
    } else if (desired < p->target_ms && elapsed(now, p->stable_since_us) >= TIMED_STABLE_MS * 1000U &&
               elapsed(now, p->last_adjust_us) >= TIMED_STABLE_MS * 1000U) {
        p->target_ms = timed_bound(p, p->target_ms > 20U ? p->target_ms - 20U : 0);
        p->last_adjust_us = now;
    }
}

void media_pacer_arrival(media_pacer_t *p, uint32_t stamp, int64_t now, uint32_t duration_us, bool new_stream)
{
    if (!p->started) {
        if (!p->have_prefill && p->policy != MEDIA_PACER_PACKET_PREFILL) {
            p->prefill_since_us = now;
            p->have_prefill = true;
        }
        if (p->prefill_packets < UINT32_MAX) ++p->prefill_packets;
    }
    if (p->policy == MEDIA_PACER_TIMED) {
        timed_arrival(p, stamp, now, new_stream);
    } else if (p->policy == MEDIA_PACER_ADAPTIVE && p->have_arrival && !new_stream) {
        const int64_t sender_ms = (int32_t)(stamp - p->last_ts);
        const int64_t delta = (now - p->last_arrival_us) / 1000 - sender_ms;
        const uint64_t magnitude = (uint64_t)(delta < 0 ? -delta : delta);
        const uint32_t error_ms = magnitude > UINT16_MAX ? UINT16_MAX : (uint32_t)magnitude;
        p->peak_ms = max(p->peak_ms, error_ms);
        p->jitter_q8 = (15U * p->jitter_q8 + 256U * error_ms) / 16U;
    }
    if (!p->have_arrival) p->window_us = now;
    p->have_arrival = true;
    p->last_ts = stamp;
    p->last_arrival_us = now;
    p->last_duration_us = duration_us;
}

void media_pacer_tick(media_pacer_t *p, int64_t now, bool keeping_up, bool local_pressure)
{
    if (p->policy != MEDIA_PACER_ADAPTIVE || !p->have_arrival || elapsed(now, p->window_us) < 1000000U) return;
    const uint32_t jitter = p->jitter_q8 / 256U;
    const uint32_t raw = p->profile.base_ms + max(4U * jitter, p->peak_ms);
    const uint32_t suggested = min(p->profile.max_ms, max(p->profile.min_ms, (raw + 9U) / 10U * 10U));
    if (suggested > p->target_ms) p->target_ms = min(suggested, p->target_ms + 40U);
    if (p->pressure || local_pressure || !keeping_up || p->peak_ms > p->profile.steady_peak_ms ||
        jitter > p->profile.steady_jitter_ms) {
        p->quiet_windows = 0;
    } else if (++p->quiet_windows >= p->profile.settle_s) {
        const uint32_t step = min(p->target_ms, p->profile.step_down_ms);
        if (suggested < p->target_ms) p->target_ms = max(suggested, p->target_ms - step);
        p->quiet_windows = 0;
    }
    p->peak_ms = 0;
    p->pressure = false;
    p->window_us = now;
}

bool media_pacer_start(media_pacer_t *p, uint32_t queued_ms, uint32_t queued_packets, int64_t now,
                       bool armed, bool draining)
{
    if (p->started) return true;
    if (!queued_ms) return false;
    if (!p->have_prefill && p->policy == MEDIA_PACER_PACKET_PREFILL) {
        p->prefill_since_us = now;
        p->have_prefill = true;
    }
    if (!armed && !draining) {
        const uint64_t oldest = p->have_prefill ? elapsed(now, p->prefill_since_us) : 0;
        if (p->policy == MEDIA_PACER_PACKET_PREFILL &&
            queued_packets < p->required_prefill_packets && oldest < PREFILL_MS * 1000U)
            return false;
        if (p->policy == MEDIA_PACER_TIMED && queued_ms < p->target_ms && queued_packets < p->required_prefill_packets &&
            oldest < p->target_ms * 1000U) return false;
        if (p->policy == MEDIA_PACER_ADAPTIVE && queued_ms < p->profile.start_ms &&
            (!p->have_arrival || elapsed(now, p->last_arrival_us) < 120000U)) return false;
    }
    p->started_us = now;
    p->rate_delta = 0;
    p->have_written = false;
    p->have_prefill = false;
    p->prefill_packets = 0;
    return p->started = true;
}

void media_pacer_written(media_pacer_t *p, int64_t now)
{
    p->have_written = true;
    p->last_written_us = now;
}

void media_pacer_underrun(media_pacer_t *p)
{
    if (p->policy == MEDIA_PACER_ADAPTIVE) {
        p->pressure = true;
        p->quiet_windows = 0;
        p->target_ms = min(p->target_ms + 40U, p->profile.max_ms);
    }
    p->rate_delta = 0;
    p->have_prefill = false;
    p->prefill_packets = 0;
    p->started = false;
}

bool media_pacer_on_empty(media_pacer_t *p, int64_t now)
{
    if (!p->started) return false;
    if (p->policy == MEDIA_PACER_FIXED || p->policy == MEDIA_PACER_PACKET_PREFILL) return false;
    if (p->policy == MEDIA_PACER_TIMED && (!p->have_written || elapsed(now, p->last_written_us) < TIMED_IDLE_MS * 1000U))
        return false;
    media_pacer_underrun(p);
    return true;
}

uint32_t media_pacer_empty_wait(const media_pacer_t *p, int64_t now)
{
    if (p->policy != MEDIA_PACER_TIMED || !p->started || !p->have_written) return XIAOTAI_WAIT_FOREVER;
    const uint64_t age = elapsed(now, p->last_written_us);
    return age < TIMED_IDLE_MS * 1000U ? (TIMED_IDLE_MS * 1000U - (uint32_t)age + 999U) / 1000U : 0;
}

int media_pacer_speed(const media_pacer_t *p, uint32_t queued_ms, uint32_t output_ms, uint32_t block_ms)
{
    if (p->policy != MEDIA_PACER_ADAPTIVE) return 0;
    const uint32_t base = max(p->target_ms, output_ms + block_ms * 2U);
    if (p->profile.surplus_ms && queued_ms >= p->target_ms + p->profile.surplus_ms) return 25;
    if (queued_ms > base + block_ms) return 12;
    if (queued_ms > block_ms && queued_ms + block_ms < base) return -(int)p->profile.slow_permille;
    return 0;
}

int media_pacer_timed_delta(media_pacer_t *p, uint32_t queued_ms, int64_t now, size_t available, size_t block)
{
    if (p->policy != MEDIA_PACER_TIMED) return 0;
    const uint32_t target = max(p->target_ms, TIMED_RATE_FLOOR_MS);
    if (!p->started || p->timed_run < 8U || now < p->started_us || now < p->last_arrival_us ||
        elapsed(now, p->started_us) < TIMED_WARMUP_MS * 1000U ||
        elapsed(now, p->last_arrival_us) > target * 1000U) p->rate_delta = 0;
    else if (p->rate_delta < 0) {
        if (queued_ms >= target - 20U) p->rate_delta = 0;
    } else if (p->rate_delta > 0) {
        if (queued_ms <= target + 20U) p->rate_delta = 0;
    } else if (queued_ms < target - 60U) p->rate_delta = -1;
    else if (queued_ms > target + 60U) p->rate_delta = 1;
    if (p->rate_delta > 0 && available <= block) p->rate_delta = 0;
    return p->rate_delta;
}

bool media_pacer_short_tail(const media_pacer_t *p, int64_t now, int64_t first_pending_us,
                            int64_t last_arrival_us, uint32_t output_ms, uint32_t block_ms, uint32_t nominal_ms)
{
    if (p->policy == MEDIA_PACER_TIMED) return elapsed(now, first_pending_us) >= TIMED_TAIL_MS * 1000U;
    if (p->policy == MEDIA_PACER_FIXED) return true;
    const uint64_t quiet_us = elapsed(now, last_arrival_us);
    return quiet_us >= 120000U || (output_ms <= 30U && quiet_us >= ((uint64_t)nominal_ms + block_ms) * 1000U);
}
