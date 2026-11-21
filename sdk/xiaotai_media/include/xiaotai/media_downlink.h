// SPDX-License-Identifier: MIT
#pragma once
/* Downlink buffer: holds encoded packets between the network and playback.
 *
 * One contract for every product: the buffer absorbs the largest burst any
 * selected module declares (AI talk delivers about one second at once) plus
 * headroom, and drops a packet only when it is truly full. Packets are stored
 * encoded, back to back as variable-length entries in one byte ring (one
 * second of 8 kHz G.711 is 50 entries of 160 + 20 bytes, about 9 KB), in BULK
 * memory: PSRAM where the board has it, internal RAM otherwise. A small packet
 * never occupies the room of the largest one.
 *
 * One producer (the transport callback), one consumer (playout), and the owner
 * task for reset; all calls are short and never block.
 *
 * Tags (xiaotai/framework.h, plus two of this header): create and destroy are
 * [any] (they allocate and free; destroy only once producer and consumer have
 * stopped); [producer] the one task that pushes, [consumer] the one task that
 * pops, both real-time; count, queued_us, capacity and stats are [any, rt]
 * (a critical section of a few instructions). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default headroom on top of the declared burst, for network jitter around it
 * (the media tuning "downlink.headroom_ms" overrides it). */
#define XIAOTAI_DOWNLINK_HEADROOM_MS 200U
/* Packets needed for a burst at a packet duration, with the default headroom. */
#define XIAOTAI_DOWNLINK_PACKETS(burst_ms, packet_ms) \
    (((burst_ms) + XIAOTAI_DOWNLINK_HEADROOM_MS + (packet_ms) - 1U) / (packet_ms))
/* Bytes one stored packet of size bytes takes: a header and 4-byte alignment. */
#define XIAOTAI_DOWNLINK_ENTRY_HEADER 20U
#define XIAOTAI_DOWNLINK_ENTRY_BYTES(size) ((((size_t)(size) + 3U) & ~(size_t)3U) + XIAOTAI_DOWNLINK_ENTRY_HEADER)
/* Ring bytes that hold packets packets of up to packet_bytes each at any
 * moment: one entry more than that, because an entry never wraps and the
 * unused end of the ring is at most one entry. */
#define XIAOTAI_DOWNLINK_BYTES(packets, packet_bytes) \
    (((size_t)(packets) + 1U) * XIAOTAI_DOWNLINK_ENTRY_BYTES(packet_bytes))
/* Ring bytes that hold hold_ms of a framed encoding whose sender never exceeds
 * bitrate_bps, in packets packets of at most max_packet_bytes each: the audio
 * itself, a header and at most 3 alignment bytes per entry, and one largest
 * entry for the unused end of the ring. Variable-size packets then cost what
 * they carry, not packets x max_packet_bytes. */
#define XIAOTAI_DOWNLINK_BYTES_AT_RATE(hold_ms, bitrate_bps, packets, max_packet_bytes)            \
    ((size_t)(((uint64_t)(hold_ms) * (uint64_t)(bitrate_bps) + 7999U) / 8000U) +                   \
     (size_t)(packets) * (XIAOTAI_DOWNLINK_ENTRY_HEADER + 3U) + XIAOTAI_DOWNLINK_ENTRY_BYTES(max_packet_bytes))

typedef struct xiaotai_downlink xiaotai_downlink_t;

typedef struct {
    uint32_t generation;
    uint32_t timestamp_ms;   /* sender's media timestamp */
    uint32_t duration_us;    /* audio the packet carries */
    int64_t received_us;     /* local arrival time */
    size_t size;
} xiaotai_downlink_packet_t;

typedef struct {
    uint32_t pushed, popped;
    uint32_t overflow;       /* dropped: buffer full */
    uint32_t stale;          /* dropped: not the current session's generation */
    uint32_t oversize;       /* dropped: larger than max_packet_bytes, or than the consumer's buffer */
    uint32_t peak_packets;
    uint32_t peak_us;        /* most audio held at once */
    uint32_t peak_bytes;     /* most ring bytes in use at once */
} xiaotai_downlink_stats_t;

/* A ring of bytes (at least XIAOTAI_DOWNLINK_ENTRY_BYTES(max_packet_bytes)) in
 * BULK memory; packets above max_packet_bytes are refused. */
xiaotai_err_t xiaotai_downlink_create(size_t bytes, size_t max_packet_bytes, xiaotai_downlink_t **out);
void xiaotai_downlink_destroy(xiaotai_downlink_t *downlink);

/* [owner] Start accepting a session's packets; everything held is dropped. 0 accepts none. */
void xiaotai_downlink_reset(xiaotai_downlink_t *downlink, uint32_t generation);

/* [producer] Copy a packet in. False when dropped (full, stale or oversize). */
bool xiaotai_downlink_push(xiaotai_downlink_t *downlink, uint32_t generation, uint32_t timestamp_ms,
                           uint32_t duration_us, const void *data, size_t size, int64_t now_us);

/* [consumer] Copy the oldest packet out. False when empty. */
bool xiaotai_downlink_pop(xiaotai_downlink_t *downlink, void *data, size_t capacity,
                          xiaotai_downlink_packet_t *packet);

size_t xiaotai_downlink_count(xiaotai_downlink_t *downlink);
uint32_t xiaotai_downlink_queued_us(xiaotai_downlink_t *downlink);
/* Ring size in bytes, as created. */
size_t xiaotai_downlink_capacity(const xiaotai_downlink_t *downlink);
void xiaotai_downlink_stats(xiaotai_downlink_t *downlink, xiaotai_downlink_stats_t *stats);

#ifdef __cplusplus
}
#endif
