// SPDX-License-Identifier: MIT
#include "downlink_rx.h"

#include <stdatomic.h>
#include <string.h>
#include "downlink_ring.h"
#include "internal.h"
#include "trace.h"
#include "xiaotai/media_rx_ingress.h"
#include "xiaotai/port/os.h"

typedef struct {
    xiaotai_downlink_t *ring;
    xiaotai_media_format_t format;
    uint32_t generation, epoch;
    uint16_t alaw_max_packet_samples;
    bool accepting, armed, muted;
} admission_t;

static xiaotai_critical_t s_rx_lock;
static atomic_bool s_initialized;
static admission_t s_rx;
static xiaotai_media_stats_t s_stats;
static unsigned s_writers;
static uint32_t s_last_stamp;
static uint16_t s_last_stream;
static int64_t s_last_us;
static bool s_have_last;

static void lock(void) { xiaotai_critical_enter(&s_rx_lock); }
static void unlock(void) { xiaotai_critical_exit(&s_rx_lock); }

#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
enum {ALAW_BYTES = 160U, ALAW_US = 20000U, ALAW_BURST = 8U};
static struct {
    uint32_t epoch, pts, ceiling_us;
    uint16_t stream;
    uint8_t used, bytes[ALAW_BYTES];
} s_alaw;

/* A retirement never writes scratch bytes: a pinned producer may already
 * be publishing a completed block. Subsequent fragments must use a new epoch. */
static void alaw_break_locked(void)
{
    if (!++s_alaw.epoch) ++s_alaw.epoch;
    s_alaw.used = 0;
}

uint32_t xiaotai_media_ingress_epoch(uint32_t generation)
{
    if (!atomic_load(&s_initialized)) return 0;
    lock();
    const uint32_t epoch = s_rx.generation == generation && s_rx.accepting &&
                           s_alaw.ceiling_us ? s_alaw.epoch : 0;
    unlock();
    return epoch;
}

void xiaotai_media_ingress_break(uint32_t generation)
{
    if (!atomic_load(&s_initialized)) return;
    lock();
    if (s_rx.generation == generation) alaw_break_locked();
    unlock();
}

static bool alaw_current(const admission_t *use, uint32_t epoch)
{
    return s_rx.generation == use->generation && s_rx.epoch == use->epoch && s_rx.accepting &&
           !s_rx.muted && s_alaw.epoch == epoch;
}

static unsigned alaw_dropped_blocks(uint32_t bytes, unsigned carry)
{
    /* The borrowed input length need not fit the ring or leave room to round up. */
    return bytes / ALAW_BYTES + (bytes % ALAW_BYTES + carry + ALAW_BYTES - 1U) / ALAW_BYTES;
}

static bool alaw_push(const admission_t *use, const xiaotai_media_packet_t *packet,
                      uint8_t format, int64_t now, uint32_t epoch,
                      unsigned *kept, unsigned *overflow, unsigned *evicted, bool *broken)
{
    uint32_t pts = packet->timestamp_ms;
    size_t at = 0;
    while (at < packet->size) {
        const uint32_t queued = xiaotai_downlink_queued_us(use->ring);
        lock();
        if (!alaw_current(use, epoch)) { *broken = true; unlock(); return false; }
        if (s_alaw.used && s_alaw.stream != packet->stream) s_alaw.used = 0;
        if (*kept == ALAW_BURST || (uint64_t)queued + ALAW_US > s_alaw.ceiling_us) {
            *overflow = alaw_dropped_blocks((uint32_t)(packet->size - at), s_alaw.used);
            s_alaw.used = 0;
            unlock();
            return false;
        }
        if (!s_alaw.used) {
            s_alaw.stream = packet->stream;
            s_alaw.pts = pts;
        }
        const size_t space = ALAW_BYTES - s_alaw.used;
        const size_t bytes = packet->size - at < space ? packet->size - at : space;
        memcpy(s_alaw.bytes + s_alaw.used, packet->data + at, bytes);
        s_alaw.used += (uint8_t)bytes;
        at += bytes;
        if (s_alaw.used < ALAW_BYTES) { unlock(); return true; }
        pts = s_alaw.pts;
        s_alaw.used = 0;
        unlock();
        if (!media_downlink_push_tagged(use->ring, use->epoch, use->generation, pts, ALAW_US,
                                        s_alaw.bytes, ALAW_BYTES, now, packet->stream,
                                        format, ALAW_BYTES, evicted)) {
            *overflow = alaw_dropped_blocks((uint32_t)(packet->size - at), 0) + 1U;
            return false;
        }
        ++*kept;
        pts += 20U;
    }
    return true;
}
#endif

void media_rx_init(void)
{
    if (!atomic_load(&s_initialized)) {
        xiaotai_critical_init(&s_rx_lock);
        atomic_store(&s_initialized, true);
    }
}

/* Only the owner changes admission. Publish a closed window while resetting
 * the ring, so the receive callback never takes two critical sections. */
static void reset(bool accepts)
{
    lock();
    s_rx.accepting = false;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    alaw_break_locked();
#endif
    const admission_t gate = s_rx;
    unlock();
    const uint32_t flushed = gate.ring ? (uint32_t)xiaotai_downlink_count(gate.ring) : 0;
    if (gate.ring) xiaotai_downlink_reset(gate.ring, accepts && gate.format.rate_hz ? gate.generation : 0);
    const uint32_t epoch = gate.ring ? media_downlink_epoch(gate.ring) : 0;
    lock();
    s_stats.rx_flushed += flushed;
    s_rx.epoch = epoch;
    s_rx.accepting = accepts;
    unlock();
}

void media_rx_open(xiaotai_downlink_t *ring, const xiaotai_media_session_t *session, bool armed, bool muted)
{
    const int32_t max_samples = xiaotai_media_param("downlink.alaw_max_packet_samples", INT32_MIN);
    const uint16_t alaw_limit = max_samples == INT32_MIN ? 0 :
        max_samples > 0 && max_samples < (int32_t)XIAOTAI_MEDIA_PACKET_BYTES_MAX ?
        (uint16_t)max_samples : XIAOTAI_MEDIA_PACKET_BYTES_MAX;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    const int32_t slots = xiaotai_media_param("downlink.capacity_packets", 0);
#endif
    lock();
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    s_alaw.ceiling_us = slots > 0 && (uint32_t)slots <= UINT32_MAX / ALAW_US ? (uint32_t)slots * ALAW_US : 0;
#endif
    s_rx = (admission_t){.ring = ring, .format = session->downlink, .generation = session->generation,
                         .armed = armed, .muted = muted, .alaw_max_packet_samples = alaw_limit};
    s_stats = (xiaotai_media_stats_t){0};
    s_last_us = 0;
    s_have_last = false;
    unlock();
    reset(true);
}

void media_rx_activate(void) { lock(); s_rx.armed = false; unlock(); }
void media_rx_drain(void)
{
    lock();
    s_rx.accepting = false;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    alaw_break_locked();
#endif
    unlock();
}

void media_rx_flush(bool accepts, bool muted)
{
    lock();
    s_rx.muted = muted;
    s_rx.accepting = false;
    s_have_last = false;
    s_last_us = 0;
    unlock();
    reset(accepts);
}

void media_rx_close(void)
{
    reset(false);
    lock();
    s_rx = (admission_t){0};
    s_last_us = 0;
    s_have_last = false;
    unlock();
}

unsigned media_rx_writers(void)
{
    lock();
    const unsigned writers = s_writers;
    unlock();
    return writers;
}

int64_t media_rx_last_us(void)
{
    lock();
    const int64_t time = s_last_us;
    unlock();
    return time;
}

static void arrived(const xiaotai_media_packet_t *packet, int64_t now, uint32_t nominal_ms)
{
    if (s_have_last && s_last_stream == packet->stream) {
        const uint32_t gap_ms = (uint32_t)((now - s_last_us) / 1000);
        const int32_t sender_ms = (int32_t)(packet->timestamp_ms - s_last_stamp);
        if (gap_ms > 60U) ++s_stats.rx_gaps;
        if (gap_ms < 5U) ++s_stats.rx_bursts;
        if (gap_ms > s_stats.rx_gap_max_ms) s_stats.rx_gap_max_ms = gap_ms;
        if (!sender_ms) ++s_stats.rx_ts_repeats;
        else if (sender_ms < 0) ++s_stats.rx_ts_backwards;
        else if ((uint64_t)sender_ms > (uint64_t)gap_ms + nominal_ms) ++s_stats.rx_ts_jumps;
    }
    s_have_last = true;
    s_last_stream = packet->stream;
    s_last_stamp = packet->timestamp_ms;
    s_last_us = now;
}

static bool push(uint32_t generation, const xiaotai_media_packet_t *packet, uint32_t alaw_epoch)
{
    (void)alaw_epoch;
    if (!atomic_load(&s_initialized)) return false;
    lock();
    const admission_t use = s_rx;
    if (!generation || generation != use.generation || !use.accepting) {
        ++s_stats.rx_stale;
        unlock();
        return false;
    }
    if (packet) {
        const uint32_t size = packet->size > UINT32_MAX ? UINT32_MAX : (uint32_t)packet->size;
        if (size > s_stats.rx_packet_max_bytes) s_stats.rx_packet_max_bytes = size;
    }
    /* Pin the prepared formats across a concurrent stop/reset while validating. */
    ++s_writers;
    unlock();

    unsigned kept = 0, overflow = 0, evicted = 0;
    bool carried = false, broken = false;
    size_t input_limit = XIAOTAI_MEDIA_PACKET_BYTES_MAX;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    /* Only selected PCMA byte chunks bypass the general packet limit. Their
     * data is borrowed from the SDK and split into bounded ring writes. */
    if (alaw_epoch && packet && packet->encoding == XIAOTAI_CODEC_G711A) input_limit = UINT32_MAX;
#endif
    bool invalid = !use.ring || !use.format.rate_hz || !packet || !packet->data || !packet->size ||
                   packet->size > input_limit;
    media_rx_format_t prepared;
    uint8_t format = 0;
    uint32_t frames = 0;
    size_t sample_bytes = 0;
    if (!invalid) {
        xiaotai_media_format_t wanted = use.format;
        wanted.encoding = packet->encoding;
        if (packet->rate_hz) wanted.rate_hz = packet->rate_hz;
        invalid = !media_rx_format_select(&wanted, &format, &prepared);
    }
    if (!invalid) {
        if (packet->encoding == XIAOTAI_CODEC_G711A || packet->encoding == XIAOTAI_CODEC_G711U) sample_bytes = 1;
        if (packet->encoding == XIAOTAI_CODEC_PCM16) sample_bytes = 2;
        if (sample_bytes) {
            invalid = packet->size % sample_bytes != 0 || !prepared.max_bytes ||
                      (packet->encoding == XIAOTAI_CODEC_G711A && use.alaw_max_packet_samples &&
                       packet->size > use.alaw_max_packet_samples);
        } else {
            frames = prepared.decoder->packet_samples(packet->data, packet->size, prepared.format.rate_hz);
            invalid = !frames || packet->size > prepared.max_bytes ||
                      (uint64_t)frames * 1000U > (uint64_t)prepared.format.rate_hz * XIAOTAI_MEDIA_PACKET_MS_MAX;
        }
    }
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    if (!invalid && alaw_epoch)
        invalid = packet->encoding != XIAOTAI_CODEC_G711A || prepared.format.rate_hz != 8000U ||
                  prepared.format.channels != 1U || prepared.max_bytes != ALAW_BYTES;
#endif
    const int64_t now = xiaotai_time_us();
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    if (alaw_epoch && invalid) xiaotai_media_ingress_break(generation);
#endif
    if (!invalid && !use.muted) {
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
        if (alaw_epoch) {
            carried = alaw_push(&use, packet, format, now, alaw_epoch, &kept, &overflow, &evicted, &broken);
        } else
#endif
        {
            const size_t piece = sample_bytes ? prepared.max_bytes : packet->size;
            for (size_t at = 0; at < packet->size; at += piece) {
                const size_t bytes = packet->size - at < piece ? packet->size - at : piece;
                const size_t samples = sample_bytes ? bytes / sample_bytes : frames;
                const uint32_t dur = (uint32_t)((uint64_t)samples * 1000000U / prepared.format.rate_hz);
                const uint32_t stamp = packet->timestamp_ms +
                    (sample_bytes ? (uint32_t)((uint64_t)(at / sample_bytes) * 1000U / prepared.format.rate_hz) : 0);
                if (!media_downlink_push_tagged(use.ring, use.epoch, generation, stamp, dur,
                                                packet->data + at, bytes, now, packet->stream,
                                                (uint8_t)(format | (at ? MEDIA_DOWNLINK_CONTINUATION : 0U)),
                                                (uint16_t)(sample_bytes ? packet->size / sample_bytes : frames), &evicted)) {
                    overflow = (unsigned)((packet->size - at + piece - 1U) / piece);
                    break;
                }
                ++kept;
            }
        }
    }
    media_trace_arrival(generation, (uint32_t)(now / 1000), packet);
    if (media_trace_active() && media_trace_rx_ingress() && packet && packet->data && packet->size &&
        packet->size <= XIAOTAI_MEDIA_PACKET_BYTES_MAX) {
        const uint64_t n = invalid ? 0 : (sample_bytes ? packet->size / sample_bytes : frames);
        const uint16_t ms = invalid ? 0 : (uint16_t)(n * 1000U / prepared.format.rate_hz);
        media_trace_frame(false, (uint32_t)(now / 1000), packet->data, packet->size, 0, 0,
                          invalid || overflow || broken || use.muted ? 11U : 3U, packet->timestamp_ms, ms);
    }
    lock();
    const bool current = generation == s_rx.generation && use.epoch == s_rx.epoch;
    if (!current) ++s_stats.rx_stale;
    else if (invalid) ++s_stats.rx_invalid;
    else {
        arrived(packet, now, prepared.format.packet_ms);
        if (use.muted) ++s_stats.rx_blocked;
        else {
            if (broken) ++s_stats.rx_stale;
            s_stats.rx_received += kept;
            s_stats.rx_overflow += overflow + evicted;
            if (use.armed) s_stats.rx_before_active += kept;
        }
    }
    unlock();
    if (current && kept) media_session_wake_playout();
    lock();
    --s_writers;
    unlock();
    return current && (kept || carried) && !overflow && !broken;
}

bool xiaotai_media_push(uint32_t generation, const xiaotai_media_packet_t *packet)
{
    return push(generation, packet, 0);
}

#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
bool xiaotai_media_ingress_selected(uint32_t generation, const xiaotai_media_packet_t *packet, uint32_t epoch)
{
    return push(generation, packet, epoch);
}
#endif

void media_rx_stats(xiaotai_media_stats_t *stats)
{
    lock();
    stats->rx_received = s_stats.rx_received;
    stats->rx_stale = s_stats.rx_stale;
    stats->rx_invalid = s_stats.rx_invalid;
    stats->rx_blocked = s_stats.rx_blocked;
    stats->rx_overflow = s_stats.rx_overflow;
    stats->rx_flushed = s_stats.rx_flushed;
    stats->rx_before_active = s_stats.rx_before_active;
    stats->rx_packet_max_bytes = s_stats.rx_packet_max_bytes;
    stats->rx_gaps = s_stats.rx_gaps;
    stats->rx_bursts = s_stats.rx_bursts;
    stats->rx_gap_max_ms = s_stats.rx_gap_max_ms;
    stats->rx_ts_repeats = s_stats.rx_ts_repeats;
    stats->rx_ts_backwards = s_stats.rx_ts_backwards;
    stats->rx_ts_jumps = s_stats.rx_ts_jumps;
    unlock();
}
