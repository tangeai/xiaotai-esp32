// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>

#include "memtrace.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport_config.h"

static xiaotai_critical_t s_lock;
static atomic_bool s_ready, s_running;
static xt_memtrace_status_t s_status;
static struct { xt_memtrace_record_t record; bool resizing; } s_slots[XIAOTAI_TRANSPORT_MEMTRACE_SLOTS];
static xt_memtrace_failure_t s_failures[XIAOTAI_TRANSPORT_MEMTRACE_FAILURES];

static void increment(uint32_t *counter) { if (*counter != UINT32_MAX) ++*counter; }

static void retired(unsigned i)
{
    s_status.live -= s_slots[i].record.bytes;
    --s_status.slots;
    s_slots[i].record = (xt_memtrace_record_t){0};
    s_slots[i].resizing = false;
    increment(&s_status.frees);
    increment(&s_status.sequence);
}

static void recorded(unsigned i, uintptr_t address, size_t bytes, uintptr_t caller)
{
    increment(&s_status.allocations);
    increment(&s_status.sequence);
    if (i == XIAOTAI_TRANSPORT_MEMTRACE_SLOTS) {
        increment(&s_status.lost);
        return;
    }
    s_slots[i].record = (xt_memtrace_record_t){address, caller, bytes};
    s_status.live += bytes;
    ++s_status.slots;
    if (s_status.live > s_status.peak) s_status.peak = s_status.live;
    if (s_status.slots > s_status.peak_slots) s_status.peak_slots = s_status.slots;
}

static unsigned find(uintptr_t address)
{
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS; ++i)
        if (!s_slots[i].resizing && s_slots[i].record.address == address) return i;
    return XIAOTAI_TRANSPORT_MEMTRACE_SLOTS;
}

bool xt_memtrace_running(void) { return atomic_load(&s_running); }

void xt_memtrace_alloc(uintptr_t address, size_t bytes, uintptr_t caller)
{
    if (!address || !bytes || !xt_memtrace_running()) return;
    xiaotai_critical_enter(&s_lock);
    if (xt_memtrace_running()) recorded(find(0), address, bytes, caller);
    xiaotai_critical_exit(&s_lock);
}

void xt_memtrace_free(uintptr_t address)
{
    if (!address || !atomic_load(&s_ready)) return;
    xiaotai_critical_enter(&s_lock);
    const unsigned i = find(address);
    if (i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS) retired(i);
    xiaotai_critical_exit(&s_lock);
}

unsigned xt_memtrace_realloc_begin(uintptr_t address)
{
    if (!address || !atomic_load(&s_ready)) return 0;
    xiaotai_critical_enter(&s_lock);
    const unsigned i = find(address);
    /* Hide the address before the allocator can release it. Reserve this
     * same slot so a failure can restore ownership even under contention. */
    if (i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS) s_slots[i].resizing = true;
    xiaotai_critical_exit(&s_lock);
    return i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS ? i + 1U : 0;
}

void xt_memtrace_realloc_end(unsigned ticket, uintptr_t address, size_t bytes, uintptr_t caller)
{
    if (!ticket) {
        xt_memtrace_alloc(address, bytes, caller);
        return;
    }
    xiaotai_critical_enter(&s_lock);
    const unsigned i = ticket - 1U;
    if (!address && bytes) {
        s_slots[i].resizing = false;
    } else {
        retired(i);
        if (address && bytes) recorded(i, address, bytes, caller);
    }
    xiaotai_critical_exit(&s_lock);
}

void xt_memtrace_failed(size_t bytes, uintptr_t caller, unsigned operation, size_t free_bytes, size_t largest)
{
    if (!bytes || !xt_memtrace_running()) return;
    xiaotai_critical_enter(&s_lock);
    if (xt_memtrace_running()) {
        increment(&s_status.sequence);
        if (s_status.failures < XIAOTAI_TRANSPORT_MEMTRACE_FAILURES) {
            s_failures[s_status.failures] = (xt_memtrace_failure_t){bytes, free_bytes, largest, caller,
                s_status.sequence, operation};
        }
        increment(&s_status.failures);
    }
    xiaotai_critical_exit(&s_lock);
}

void xt_memtrace_status(xt_memtrace_status_t *out)
{
    xiaotai_critical_enter(&s_lock);
    *out = s_status;
    out->running = xt_memtrace_running();
    xiaotai_critical_exit(&s_lock);
}

bool xt_memtrace_record(unsigned i, xt_memtrace_record_t *out)
{
    if (i >= XIAOTAI_TRANSPORT_MEMTRACE_SLOTS) return false;
    xiaotai_critical_enter(&s_lock);
    const bool live = !s_slots[i].resizing && s_slots[i].record.address != 0;
    if (live) *out = s_slots[i].record;
    xiaotai_critical_exit(&s_lock);
    return live;
}

bool xt_memtrace_failure(unsigned i, xt_memtrace_failure_t *out)
{
    if (i >= XIAOTAI_TRANSPORT_MEMTRACE_FAILURES) return false;
    xiaotai_critical_enter(&s_lock);
    const bool exists = i < s_status.failures;
    if (exists) *out = s_failures[i];
    xiaotai_critical_exit(&s_lock);
    return exists;
}

static xiaotai_err_t xiaotai_transport_memtrace_report(int argc, const char *const *argv,
                                                       const xiaotai_reply_t *reply)
{
    (void)argv;
    if (argc != 1) return XIAOTAI_ERR_INVALID_ARG;
    char line[256];
    xt_memtrace_status_t s;
    xt_memtrace_status(&s);
    snprintf(line, sizeof(line), "+MEMTRACE: compiled=1 running=%u seq=%" PRIu32 " allocations=%" PRIu32
             " frees=%" PRIu32 " failures=%" PRIu32 " lost=%" PRIu32 " live=%zu peak=%zu slots=%" PRIu32
             " peak_slots=%" PRIu32, (unsigned)s.running, s.sequence, s.allocations, s.frees, s.failures,
             s.lost, s.live, s.peak, s.slots, s.peak_slots);
    reply->write(reply->ctx, line);
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS; ++i) {
        xt_memtrace_record_t r;
        if (!xt_memtrace_record(i, &r)) continue;
        snprintf(line, sizeof(line), "+MEMTRACE-LIVE: slot=%u ptr=0x%" PRIxPTR " bytes=%zu caller=0x%" PRIxPTR,
                 i, r.address, r.bytes, r.caller);
        reply->write(reply->ctx, line);
    }
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_MEMTRACE_FAILURES; ++i) {
        xt_memtrace_failure_t r;
        if (!xt_memtrace_failure(i, &r)) break;
        snprintf(line, sizeof(line), "+MEMTRACE-FAIL: slot=%u seq=%" PRIu32 " op=%u bytes=%zu caller=0x%" PRIxPTR
                 " free=%zu largest=%zu", i, r.sequence, r.operation, r.bytes, r.caller, r.free_bytes, r.largest);
        reply->write(reply->ctx, line);
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_transport_memtrace_init(void)
{
    if (!atomic_load(&s_ready)) {
        xiaotai_critical_init(&s_lock);
        atomic_store(&s_ready, true);
    }
    const xiaotai_err_t err = xiaotai_command_register("AT+MEMTRACE", "", xiaotai_transport_memtrace_report);
    if (err == XIAOTAI_OK) atomic_store(&s_running, true);
    return err;
}

void xt_memtrace_stop(void)
{
    if (!atomic_load(&s_ready)) return;
    xiaotai_critical_enter(&s_lock);
    atomic_store(&s_running, false);
    xiaotai_critical_exit(&s_lock);
}
