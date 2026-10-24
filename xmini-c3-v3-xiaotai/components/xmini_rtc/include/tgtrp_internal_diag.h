#ifndef TGTRP_INTERNAL_DIAG_H
#define TGTRP_INTERNAL_DIAG_H

#include <stdint.h>
#include <stddef.h>

#define TGTRP_INTERNAL_DIAG_VERSION 2u
#define TGTRP_INTERNAL_DIAG_TX 1u
#define TGTRP_INTERNAL_DIAG_RX 2u
#define TGTRP_INTERNAL_DIAG_QUEUE 3u

/* Private diagnostic ABI. Identity is an opaque session-allocator address, not
 * a pointer the consumer may dereference. The hook borrows this stack record
 * synchronously on the session thread; it must only bounded-copy and return. */
typedef struct tgtrp_internal_diag {
    uintptr_t identity;
    uint32_t now_ms;
    uint32_t frame_id;
    uint32_t minimum_frame_id;
    uint32_t frames;
    uint32_t segments;
    uint32_t payload_bytes;
    uint32_t auxiliary_frames;
    uint32_t auxiliary_bytes;
    uint32_t pacer_packets;
    uint32_t pacer_bytes;
    uint32_t rx_allocation_failures;
    uint32_t first_failed_frame_id;
    uint8_t version;
    uint8_t stage;
    uint8_t kind;
    uint8_t stream_id;
} tgtrp_internal_diag_t;

/* Payload bytes are retained segment allocation lengths: TX includes its
 * reserved TGTRP fragment prefix; RX does not. Auxiliary is TX pending, RX
 * jitter, or (QUEUE stage) the thread-safe ingress send queue. For QUEUE,
 * frame_id is cumulative accepted ingress frames, frames/bytes describe the
 * queued head's kind/stream, and auxiliary describes the complete queue using
 * its original logical payload-byte counter (without fragment prefixes).
 * TX pending auxiliary_bytes likewise keeps its logical byte-counter unit.
 * RX frame_id is the latest parsed arrival, including rejected allocations.
 * RX allocation fields persist per stream; a zero failure count means there
 * is no failed frame, even when first_failed_frame_id is zero. Poll snapshots
 * select the stream with most retained frames, or the last arrival if empty. */
#ifdef TGTRP_INTERNAL_DIAG_WEAK_IMPORT
void tgtrp_internal_diag_hook(const tgtrp_internal_diag_t* record)
    __attribute__((weak));
#else
void tgtrp_internal_diag_hook(const tgtrp_internal_diag_t* record);
#endif

static inline uint32_t tgtrp_internal_diag_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

#endif
