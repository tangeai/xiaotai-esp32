// SPDX-License-Identifier: MIT
/* Downlink buffer: one byte ring of variable-length entries in BULK memory.
 *
 * An entry is a 20-byte header followed by the payload, padded to 4 bytes, and
 * never wraps: when it does not fit before the end of the ring it starts at 0
 * and the end of the ring stays unused until the consumer passes it. A short
 * critical section guards the offsets; payload copies happen outside it, into
 * a region the caller exclusively owns at that moment (producer: the reserved
 * region past the tail; consumer: the head entry it is reading), so the lock
 * is held for a few instructions only.
 *
 * A reset while the consumer copies cannot free the entry under it: the entry
 * stays in the ring as "held" (no longer counted) until the copy ends, and the
 * producer never reserves over it. */
#include "downlink_ring.h"

#include <string.h>
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

/* Stored at the start of every entry; copied with memcpy, so the ring needs no
 * alignment beyond 4 bytes. The arrival time is split into two words so the
 * header has no 8-byte member (and no padding to 24 bytes). */
typedef struct {
    uint32_t timestamp_ms;
    uint32_t duration_us;
    uint32_t received_lo, received_hi;
    uint16_t size;
    uint16_t stream;
} entry_t;
_Static_assert(sizeof(entry_t) == XIAOTAI_DOWNLINK_ENTRY_HEADER, "entry header size is part of the sizing contract");

/* The media ring knows its limits (1500 bytes, 120 ms per entry and at most
 * 16 prepared formats). Keep original packet timing without adding a header
 * word. Stand-alone users of the byte ring keep the full original fields. */
#define SIZE_BITS 11U
#define SIZE_MASK UINT16_C(0x7ff)
#define DURATION_BITS 17U
#define DURATION_MASK UINT32_C(0x1ffff)
#define ORIGINAL_SAMPLES_MASK UINT16_C(0x3fff)
#define CONTINUATION_BIT (UINT32_C(1) << 31)

struct xiaotai_downlink {
    xiaotai_critical_t lock;
    uint8_t *ring;
    size_t size, max_packet_bytes;
    /* Live data is [head, tail) when not wrapped, [head, wrap_at) and then
     * [0, tail) when wrapped. */
    size_t head, tail, wrap_at;
    bool wrapped, tagged, drop_oldest;
    size_t count;                 /* live entries */
    bool writing, reading;        /* a region is being copied outside the lock */
    bool held;                    /* the entry being read was dropped by a reset */
    size_t read_span;
    uint32_t generation, epoch;
    uint32_t queued_us, packet_limit, limit_us;
    xiaotai_downlink_stats_t stats;
};

static size_t entry_span(size_t size) { return XIAOTAI_DOWNLINK_ENTRY_BYTES(size); }

static bool ring_empty(const xiaotai_downlink_t *downlink) { return downlink->count == 0 && !downlink->held; }
static size_t entry_size(const xiaotai_downlink_t *d, const entry_t *e) { return d->tagged ? e->size & SIZE_MASK : e->size; }
static uint32_t entry_duration(const xiaotai_downlink_t *d, const entry_t *e)
{
    return d->tagged ? e->duration_us & DURATION_MASK : e->duration_us;
}

static size_t ring_used(const xiaotai_downlink_t *downlink)
{
    if (ring_empty(downlink)) return 0;
    return downlink->wrapped ? (downlink->wrap_at - downlink->head) + downlink->tail : downlink->tail - downlink->head;
}

static void advance_head(xiaotai_downlink_t *downlink, size_t span)
{
    downlink->head += span;
    if (downlink->wrapped && downlink->head == downlink->wrap_at) {
        downlink->head = 0;
        downlink->wrapped = false;
    }
}

xiaotai_err_t xiaotai_downlink_create(size_t bytes, size_t max_packet_bytes, xiaotai_downlink_t **out)
{
    if (!out || max_packet_bytes == 0 || max_packet_bytes > UINT16_MAX || bytes < entry_span(max_packet_bytes)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    /* The lock holds kernel state: keep the object itself in internal RAM. */
    xiaotai_downlink_t *downlink = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*downlink));
    if (!downlink) return XIAOTAI_ERR_NO_MEM;
    downlink->ring = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
    if (!downlink->ring) {
        xiaotai_downlink_destroy(downlink);
        return XIAOTAI_ERR_NO_MEM;
    }
    downlink->size = bytes;
    downlink->max_packet_bytes = max_packet_bytes;
    xiaotai_critical_init(&downlink->lock);
    *out = downlink;
    return XIAOTAI_OK;
}

void xiaotai_downlink_destroy(xiaotai_downlink_t *downlink)
{
    if (!downlink) return;
    xiaotai_mem_free(downlink->ring);
    xiaotai_mem_free(downlink);
}

void xiaotai_downlink_reset(xiaotai_downlink_t *downlink, uint32_t generation)
{
    xiaotai_critical_enter(&downlink->lock);
    downlink->generation = generation;
    ++downlink->epoch;
    if (downlink->reading) {
        /* Keep only the entry being copied out; it is released when the copy ends. */
        downlink->tail = downlink->head + downlink->read_span;
        downlink->wrapped = false;
        downlink->held = true;
    } else {
        downlink->head = downlink->tail = 0;
        downlink->wrapped = false;
    }
    downlink->count = 0;
    downlink->queued_us = 0;
    xiaotai_critical_exit(&downlink->lock);
}

void media_downlink_tagged(xiaotai_downlink_t *downlink, bool drop_oldest, uint32_t packet_limit, uint32_t limit_us)
{
    downlink->tagged = true;
    downlink->drop_oldest = drop_oldest;
    downlink->packet_limit = packet_limit;
    downlink->limit_us = limit_us;
}

uint32_t media_downlink_epoch(xiaotai_downlink_t *downlink)
{
    xiaotai_critical_enter(&downlink->lock);
    const uint32_t epoch = downlink->epoch;
    xiaotai_critical_exit(&downlink->lock);
    return epoch;
}

size_t media_downlink_packet_capacity(const xiaotai_downlink_t *downlink)
{
    return downlink ? downlink->max_packet_bytes : 0;
}

static size_t place(xiaotai_downlink_t *downlink, size_t span, uint32_t duration_us, bool *wrap)
{
    *wrap = false;
    if (downlink->writing || (downlink->packet_limit && downlink->count >= downlink->packet_limit) ||
        (downlink->limit_us && downlink->queued_us > downlink->limit_us - duration_us)) return SIZE_MAX;
    if (ring_empty(downlink)) {
        downlink->head = downlink->tail = 0;
        downlink->wrapped = false;
    }
    if (downlink->wrapped) {
        return span <= downlink->head - downlink->tail ? downlink->tail : SIZE_MAX;
    }
    if (span <= downlink->size - downlink->tail) return downlink->tail;
    if (span <= downlink->head) {
        *wrap = true;
        return 0;
    }
    return SIZE_MAX;
}

static bool push(xiaotai_downlink_t *downlink, const uint32_t *required_epoch, uint32_t generation,
                 uint32_t timestamp_ms, uint32_t duration_us, const void *data, size_t size, int64_t now_us,
                 uint16_t stream, uint8_t format, uint16_t original_samples, unsigned *evicted)
{
    if (!downlink || (size && !data)) return false;
    xiaotai_critical_enter(&downlink->lock);
    if (generation == 0 || generation != downlink->generation ||
        (required_epoch && *required_epoch != downlink->epoch)) {
        ++downlink->stats.stale;
        xiaotai_critical_exit(&downlink->lock);
        return false;
    }
    if (size > downlink->max_packet_bytes || (downlink->tagged &&
        (duration_us > DURATION_MASK || original_samples > ORIGINAL_SAMPLES_MASK || size > SIZE_MASK))) {
        ++downlink->stats.oversize;
        xiaotai_critical_exit(&downlink->lock);
        return false;
    }
    if (downlink->limit_us && duration_us > downlink->limit_us) {
        ++downlink->stats.overflow;
        xiaotai_critical_exit(&downlink->lock);
        return false;
    }
    /* One producer: the region past the tail is ours until published; the
     * consumer only reads published entries. */
    const size_t span = entry_span(size);
    bool wrap;
    size_t at = place(downlink, span, duration_us, &wrap);
    /* When opted in, a newly arriving packet can replace the oldest queued
     * data, but never an entry the consumer is currently copying. */
    while (at == SIZE_MAX && downlink->drop_oldest && !downlink->reading && !downlink->writing && downlink->count) {
        entry_t oldest;
        memcpy(&oldest, downlink->ring + downlink->head, sizeof(oldest));
        advance_head(downlink, entry_span(entry_size(downlink, &oldest)));
        downlink->queued_us -= entry_duration(downlink, &oldest);
        --downlink->count;
        ++downlink->stats.overflow;
        if (evicted) ++*evicted;
        at = place(downlink, span, duration_us, &wrap);
    }
    if (at == SIZE_MAX) {
        ++downlink->stats.overflow;
        xiaotai_critical_exit(&downlink->lock);
        return false;
    }
    const uint32_t epoch = downlink->epoch;
    downlink->writing = true;
    xiaotai_critical_exit(&downlink->lock);

    uint32_t stored_duration = duration_us;
    uint16_t stored_size = (uint16_t)size;
    if (downlink->tagged) {
        stored_duration |= (uint32_t)original_samples << DURATION_BITS;
        if (format & MEDIA_DOWNLINK_CONTINUATION) stored_duration |= CONTINUATION_BIT;
        stored_size |= (uint16_t)((format & ~MEDIA_DOWNLINK_CONTINUATION) << SIZE_BITS);
    }
    const entry_t entry = {.timestamp_ms = timestamp_ms, .duration_us = stored_duration,
                           .received_lo = (uint32_t)((uint64_t)now_us & 0xffffffffU),
                           .received_hi = (uint32_t)((uint64_t)now_us >> 32), .size = stored_size, .stream = stream};
    memcpy(downlink->ring + at, &entry, sizeof(entry));
    if (size) memcpy(downlink->ring + at + sizeof(entry), data, size);

    xiaotai_critical_enter(&downlink->lock);
    downlink->writing = false;
    bool published = false;
    /* A reset while copying makes the packet stale: it belongs to the old session. */
    if (downlink->epoch == epoch) {
        if (wrap) {
            downlink->wrap_at = downlink->tail;
            downlink->wrapped = true;
            downlink->tail = span;
            /* The consumer may have emptied the upper part while we copied. */
            if (downlink->head == downlink->wrap_at) {
                downlink->head = 0;
                downlink->wrapped = false;
            }
        } else {
            downlink->tail = at + span;
        }
        ++downlink->count;
        downlink->queued_us += duration_us;
        ++downlink->stats.pushed;
        if (downlink->count > downlink->stats.peak_packets) downlink->stats.peak_packets = (uint32_t)downlink->count;
        if (downlink->queued_us > downlink->stats.peak_us) downlink->stats.peak_us = downlink->queued_us;
        const size_t used = ring_used(downlink);
        if (used > downlink->stats.peak_bytes) downlink->stats.peak_bytes = (uint32_t)used;
        published = true;
    } else {
        ++downlink->stats.stale;
    }
    xiaotai_critical_exit(&downlink->lock);
    return published;
}

bool xiaotai_downlink_push(xiaotai_downlink_t *downlink, uint32_t generation, uint32_t timestamp_ms,
                           uint32_t duration_us, const void *data, size_t size, int64_t now_us)
{
    return push(downlink, NULL, generation, timestamp_ms, duration_us, data, size, now_us, 0, 0, 0, NULL);
}

bool media_downlink_push_tagged(xiaotai_downlink_t *downlink, uint32_t epoch, uint32_t generation,
                                uint32_t stamp, uint32_t duration, const void *data, size_t size,
                                int64_t received, uint16_t stream, uint8_t format, uint16_t original_samples,
                                unsigned *evicted)
{
    return push(downlink, &epoch, generation, stamp, duration, data, size, received, stream, format, original_samples, evicted);
}

bool media_downlink_pop_tagged(xiaotai_downlink_t *downlink, void *data, size_t capacity,
                               xiaotai_downlink_packet_t *packet, uint16_t *stream, uint8_t *format,
                               uint16_t *original_samples)
{
    if (!downlink || !data || !packet) return false;
    xiaotai_critical_enter(&downlink->lock);
    if (downlink->count == 0 || downlink->reading) {
        xiaotai_critical_exit(&downlink->lock);
        return false;
    }
    const size_t at = downlink->head;
    entry_t entry;
    memcpy(&entry, downlink->ring + at, sizeof(entry));
    const uint32_t duration = entry_duration(downlink, &entry);
    const size_t size = entry_size(downlink, &entry);
    const size_t span = entry_span(size);
    if (size > capacity) {
        /* Caller's buffer is too small: drop the packet rather than stall. */
        advance_head(downlink, span);
        --downlink->count;
        downlink->queued_us -= duration;
        ++downlink->stats.oversize;
        xiaotai_critical_exit(&downlink->lock);
        return false;
    }
    downlink->reading = true;
    downlink->read_span = span;
    const uint32_t epoch = downlink->epoch;
    const uint32_t generation = downlink->generation;
    xiaotai_critical_exit(&downlink->lock);

    if (size) memcpy(data, downlink->ring + at + sizeof(entry), size);

    xiaotai_critical_enter(&downlink->lock);
    downlink->reading = false;
    bool delivered = false;
    if (downlink->epoch == epoch) {
        advance_head(downlink, span);
        --downlink->count;
        downlink->queued_us -= duration;
        ++downlink->stats.popped;
        *packet = (xiaotai_downlink_packet_t){
            .generation = generation, .timestamp_ms = entry.timestamp_ms, .duration_us = duration,
            .received_us = (int64_t)(((uint64_t)entry.received_hi << 32) | entry.received_lo), .size = size,
        };
        if (stream) *stream = entry.stream;
        if (format) *format = !downlink->tagged ? 0 : (uint8_t)((entry.size >> SIZE_BITS) |
            ((entry.duration_us & CONTINUATION_BIT) ? MEDIA_DOWNLINK_CONTINUATION : 0U));
        if (original_samples) *original_samples = downlink->tagged ?
            (uint16_t)((entry.duration_us >> DURATION_BITS) & ORIGINAL_SAMPLES_MASK) : 0;
        delivered = true;
    } else if (downlink->held) {
        /* A reset dropped the entry while it was copied: release its room now. */
        advance_head(downlink, span);
        downlink->held = false;
    }
    xiaotai_critical_exit(&downlink->lock);
    return delivered;
}

bool xiaotai_downlink_pop(xiaotai_downlink_t *downlink, void *data, size_t capacity,
                          xiaotai_downlink_packet_t *packet)
{
    return media_downlink_pop_tagged(downlink, data, capacity, packet, NULL, NULL, NULL);
}

size_t xiaotai_downlink_count(xiaotai_downlink_t *downlink)
{
    xiaotai_critical_enter(&downlink->lock);
    const size_t count = downlink->count;
    xiaotai_critical_exit(&downlink->lock);
    return count;
}

uint32_t xiaotai_downlink_queued_us(xiaotai_downlink_t *downlink)
{
    xiaotai_critical_enter(&downlink->lock);
    const uint32_t queued = downlink->queued_us;
    xiaotai_critical_exit(&downlink->lock);
    return queued;
}

size_t xiaotai_downlink_capacity(const xiaotai_downlink_t *downlink) { return downlink->size; }

void xiaotai_downlink_stats(xiaotai_downlink_t *downlink, xiaotai_downlink_stats_t *stats)
{
    if (!stats) return;
    xiaotai_critical_enter(&downlink->lock);
    *stats = downlink->stats;
    xiaotai_critical_exit(&downlink->lock);
}
