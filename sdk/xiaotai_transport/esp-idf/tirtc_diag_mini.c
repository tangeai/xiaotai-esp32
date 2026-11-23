// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "diag_mini.h"
#include "include/tgtrp_internal_diag.h"
#include "xiaotai/at_command.h"
#include "xiaotai/port/os.h"

enum { MINI_STAGES = 3 };
_Static_assert(TGTRP_INTERNAL_DIAG_VERSION == 2 && TGTRP_INTERNAL_DIAG_TX == 1 &&
               TGTRP_INTERNAL_DIAG_QUEUE == MINI_STAGES, "unexpected private Mini diagnostic ABI");

typedef struct {
    uint32_t sequence, time_ms, frame, minimum_frame, frames, segments, bytes;
    uint32_t auxiliary_frames, auxiliary_bytes, pacer_packets, pacer_bytes;
    uint32_t rx_alloc_failures, rx_first_failed_frame;
    uint8_t kind, stream;
} mini_row_t;

typedef struct { uint32_t captures; mini_row_t rows[MINI_STAGES]; } mini_snapshot_t;
_Static_assert(sizeof(mini_snapshot_t) < 256U, "Mini diagnostic snapshot memory budget");

static mini_snapshot_t s_snapshot;
static xiaotai_critical_t s_lock;
static atomic_bool s_enabled;
static bool s_lock_ready;

void xt_diag_mini_enable(bool enabled)
{
    /* The framework serializes init/reset. The SDK and readers only use the
     * lock after the release-store publishes its one-time initialization. */
    if (!s_lock_ready) {
        if (!enabled) return;
        xiaotai_critical_init(&s_lock);
        s_lock_ready = true;
    }
    xiaotai_critical_enter(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    atomic_store_explicit(&s_enabled, enabled, memory_order_release);
    xiaotai_critical_exit(&s_lock);
}

/* The SDK borrows this stack record on its session thread. No string, pointer
 * or payload crosses the adapter; only copy the bounded counters. */
void tgtrp_internal_diag_hook(const tgtrp_internal_diag_t *record)
{
    if (!record || !atomic_load_explicit(&s_enabled, memory_order_acquire) ||
        record->version != TGTRP_INTERNAL_DIAG_VERSION ||
        record->stage < TGTRP_INTERNAL_DIAG_TX || record->stage > TGTRP_INTERNAL_DIAG_QUEUE) return;
    xiaotai_critical_enter(&s_lock);
    if (atomic_load_explicit(&s_enabled, memory_order_relaxed)) {
        if (++s_snapshot.captures == 0) ++s_snapshot.captures;
        s_snapshot.rows[record->stage - TGTRP_INTERNAL_DIAG_TX] = (mini_row_t){
            .sequence = s_snapshot.captures, .time_ms = record->now_ms,
            .frame = record->frame_id, .minimum_frame = record->minimum_frame_id,
            .frames = record->frames, .segments = record->segments, .bytes = record->payload_bytes,
            .auxiliary_frames = record->auxiliary_frames, .auxiliary_bytes = record->auxiliary_bytes,
            .pacer_packets = record->pacer_packets, .pacer_bytes = record->pacer_bytes,
            .rx_alloc_failures = record->rx_allocation_failures,
            .rx_first_failed_frame = record->first_failed_frame_id,
            .kind = record->kind, .stream = record->stream_id,
        };
    }
    xiaotai_critical_exit(&s_lock);
}

void xt_diag_mini_report(const xiaotai_reply_t *reply)
{
    if (!atomic_load_explicit(&s_enabled, memory_order_acquire)) return;
    xiaotai_critical_enter(&s_lock);
    const mini_snapshot_t snapshot = s_snapshot;
    xiaotai_critical_exit(&s_lock);

    char line[XIAOTAI_AT_OUT_LINE_MAX + 1U];
    snprintf(line, sizeof(line), "+LINK-TG: captures=%" PRIu32, snapshot.captures);
    reply->write(reply->ctx, line);
    static const char *const names[MINI_STAGES] = {"TX", "RX", "QUEUE"};
    for (unsigned i = 0; i < MINI_STAGES; ++i) {
        const mini_row_t *row = &snapshot.rows[i];
        if (!row->sequence) continue;
        snprintf(line, sizeof(line), "+LINK-TG-%s: seq=%" PRIu32 " time_ms=%" PRIu32
                 " frame=%" PRIu32 " min_frame=%" PRIu32 " kind=%u stream=%u"
                 " frames=%" PRIu32 " segments=%" PRIu32 " bytes=%" PRIu32
                 " aux_frames=%" PRIu32 " aux_bytes=%" PRIu32 " pacer_packets=%" PRIu32
                 " pacer_bytes=%" PRIu32 " rx_alloc_failures=%" PRIu32 " rx_first_failed_frame=%" PRIu32,
                 names[i], row->sequence, row->time_ms, row->frame, row->minimum_frame,
                 (unsigned)row->kind, (unsigned)row->stream, row->frames, row->segments, row->bytes,
                 row->auxiliary_frames, row->auxiliary_bytes, row->pacer_packets, row->pacer_bytes,
                 row->rx_alloc_failures, row->rx_first_failed_frame);
        reply->write(reply->ctx, line);
    }
}
