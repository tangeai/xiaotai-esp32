// SPDX-License-Identifier: MIT
#include <string.h>

#include "internal.h"
#include "xiaotai/framework.h"

#define XT_OWNER_DRAIN_RETRY_MS 10
#define EVENT_COMMAND (UINT32_C(1) << 31)
#define EVENT_EARLY (UINT32_C(1) << 30)
#define EVENT_NO_DATA (UINT32_C(1) << 29)
#define EVENT_WRITING (UINT32_C(1) << 28)
#define EVENT_DISCARD (UINT32_C(1) << 27)
#define EVENT_ALLOC_FAILED (UINT32_C(1) << 26)
#define EVENT_DYNAMIC (UINT32_C(1) << 25)
#define EVENT_OFFSET_MASK (EVENT_DYNAMIC - 1U)

struct xt_command_payload {
    xt_command_payload_t *next;
    uint32_t size;
    uint8_t data[];
};

static xt_command_payload_t reserved_slot;

static xt_event_t *events(void) { return (xt_event_t *)(void *)xt.event_ring; }

static void peak(_Atomic uint32_t *counter, uint32_t value)
{
    if (value > atomic_load(counter)) atomic_store(counter, value); /* All writers hold xt.lock. */
}

static void remove_event_locked(uint8_t i)
{
    --xt.event_count;
    memmove(&events()[i], &events()[i + 1U], (size_t)(xt.event_count - i) * sizeof(xt_event_t));
}

static bool has_data(const xt_event_t *event)
{
    return event->generation && event->size && (event->meta & (EVENT_COMMAND | EVENT_NO_DATA)) == EVENT_COMMAND;
}

static void retire_locked(xt_command_payload_t *payload)
{
    payload->next = xt.command_retired;
    xt.command_retired = payload;
    xt.command_retired_bytes += payload->size;
}

static void retire_event_locked(const xt_event_t *event)
{
    if (!(event->meta & EVENT_DYNAMIC) || !has_data(event)) return;
    const uint32_t index = event->meta & EVENT_OFFSET_MASK;
    retire_locked(xt.command_slots[index]);
    xt.command_slots[index] = NULL;
}

static uint32_t free_payload(xt_command_payload_t *payload)
{
    const uint32_t size = payload->size;
    volatile uint8_t *p = payload->data;
    for (uint32_t i = 0; i <= size; ++i) p[i] = 0;
    xiaotai_mem_free(payload);
    xt_account(-(int64_t)(sizeof(*payload) + size + 1U));
    return size;
}

static void reap_commands(void)
{
    if (!xt.command_on_demand) return;
    xiaotai_critical_enter(&xt.lock);
    xt_command_payload_t *list = xt.command_retired;
    xt.command_retired = NULL;
    xiaotai_critical_exit(&xt.lock);

    uint32_t retired = 0;
    while (list) {
        xt_command_payload_t *payload = list;
        list = payload->next;
        retired += free_payload(payload);
    }
    if (retired) {
        xiaotai_critical_enter(&xt.lock);
        xt.command_retired_bytes -= retired;
        xiaotai_critical_exit(&xt.lock);
    }
}

void xt_command_payloads_release(void)
{
    for (unsigned i = 0; i <= xt.event_slots; ++i) {
        xt_command_payload_t *payload = xt.command_slots[i];
        if (payload && payload != &reserved_slot) (void)free_payload(payload);
    }
    reap_commands();
}

void xt_link_events_drop_locked(uint32_t generation)
{
    for (uint8_t i = 0; i < xt.event_count;) {
        xt_event_t *event = &events()[i];
        if (event->generation != generation || (event->meta & EVENT_DISCARD)) { ++i; continue; }
        if (event->meta & EVENT_EARLY) atomic_fetch_add(&xt.counters.early_dropped, 1);
        if (event->meta & EVENT_WRITING) {
            event->meta |= EVENT_DISCARD;
            ++i;
        } else {
            retire_event_locked(event);
            remove_event_locked(i);
        }
    }
}

static const xt_event_t *reservation(unsigned i)
{
    return i == xt.event_count ? &xt.owner_event : &events()[i];
}

static uint32_t find_span_locked(uint32_t offset, uint32_t needed)
{
    while (offset <= xt_command_storage_bytes() - needed) {
        uint32_t next = offset;
        for (unsigned i = 0; i <= xt.event_count; ++i) {
            const xt_event_t *event = reservation(i);
            if (!has_data(event) || (event->meta & EVENT_DYNAMIC)) continue;
            const uint32_t start = event->meta & EVENT_OFFSET_MASK;
            const uint32_t end = start + event->size + 1U;
            if (start < offset + needed && offset < end && next < end) next = end;
        }
        if (next == offset) return offset;
        offset = next;
    }
    return EVENT_OFFSET_MASK;
}

static void command_peaks_locked(uint32_t generation, uint32_t total, uint32_t link_total)
{
    peak(&xt.counters.command_pool_peak, total);
    xt_slot_t *owner = xt_link_generation_locked(generation);
    if (owner && link_total > owner->stats.command_pool_peak) owner->stats.command_pool_peak = link_total;
}

static void demand_peaks_locked(uint32_t generation)
{
    uint32_t total = xt.command_retired_bytes, link_total = 0;
    for (unsigned i = 0; i <= xt.event_count; ++i) {
        const xt_event_t *event = reservation(i);
        if (has_data(event) && !(event->meta & EVENT_WRITING)) {
            total += event->size;
            if (event->generation == generation) link_total += event->size;
        }
    }
    command_peaks_locked(generation, total, link_total);
}

static uint32_t reserve_command_locked(const xt_slot_t *slot, uint32_t generation, uint32_t size)
{
    const uint32_t maximum = slot->profile.command_max_bytes ?
        slot->profile.command_max_bytes : XIAOTAI_LINK_COMMAND_DEFAULT_BYTES;
    const uint32_t share = slot->profile.command_pool_bytes ? slot->profile.command_pool_bytes : 2U * maximum;
    if ((!xt.command_pool && !xt.command_slots) || size > maximum || size > xt.command_pool_bytes)
        return EVENT_OFFSET_MASK;

    uint32_t link_total = 0, total = xt.command_retired_bytes, resident = 0;
    for (unsigned i = 0; i <= xt.event_count; ++i) {
        const xt_event_t *event = reservation(i);
        if (has_data(event)) {
            total += event->size;
            if (event->generation == generation) link_total += event->size;
            if (!(event->meta & EVENT_DYNAMIC)) resident += event->size;
        }
    }
    if (link_total + size > share || total + size > xt.command_pool_bytes) return EVENT_OFFSET_MASK;

    if (xt.command_pool && resident + size <= xt.command_resident_bytes) {
        const uint32_t needed = size + 1U;
        uint32_t offset = find_span_locked(xt.command_tail, needed);
        /* Wrap around queued and borrowed resident spans. Dynamic entries do
         * not occupy this block, but still use the common payload budget. */
        if (offset == EVENT_OFFSET_MASK && xt.command_tail) offset = find_span_locked(0, needed);
        if (offset != EVENT_OFFSET_MASK) {
            xt.command_tail = offset + needed;
            if (!xt.command_on_demand) command_peaks_locked(generation, total + size, link_total + size);
            return offset;
        }
    }

    if (xt.command_on_demand) {
        for (unsigned i = 0; i <= xt.event_slots; ++i) {
            if (xt.command_slots[i]) continue;
            xt.command_slots[i] = &reserved_slot;
            return EVENT_DYNAMIC | i;
        }
    }
    return EVENT_OFFSET_MASK;
}

static void append_locked(xt_event_t event)
{
    events()[xt.event_count++] = event;
    peak(&xt.counters.events_peak, xt.event_count);
}

static void overflow_locked(xt_slot_t *slot)
{
    xt_diag_link("overflow", atomic_load(&slot->generation), 0, true);
    atomic_fetch_add(&xt.counters.events_lost, 1);
    (void)xt_link_end_locked(slot, XIAOTAI_LINK_END_OVERFLOW, 0, false, false);
    if (xt.config.overflow_fault) atomic_store(&xt.lifecycle_overflow, true);
}

static void store_demand_command(uint32_t index, uint32_t generation, const uint8_t *data, uint32_t bytes)
{
    const size_t storage = sizeof(xt_command_payload_t) + bytes + 1U;
    xt_command_payload_t *payload = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, storage);
    if (payload) {
        payload->size = bytes;
        memcpy(payload->data, data, bytes);
        payload->data[bytes] = 0;
        xt_account((int64_t)storage);
    }

    xiaotai_critical_enter(&xt.lock);
    for (uint8_t i = 0; i < xt.event_count; ++i) {
        xt_event_t *event = &events()[i];
        if (event->generation != generation ||
            (event->meta & (EVENT_WRITING | EVENT_DYNAMIC)) != (EVENT_WRITING | EVENT_DYNAMIC) ||
            (event->meta & EVENT_OFFSET_MASK) != index) continue;
        if (event->meta & EVENT_DISCARD) {
            xt.command_slots[index] = NULL;
            if (payload) retire_locked(payload);
            remove_event_locked(i);
        } else {
            xt.command_slots[index] = payload;
            event->meta &= ~EVENT_WRITING;
            if (payload) demand_peaks_locked(generation);
            else event->meta |= EVENT_NO_DATA | EVENT_ALLOC_FAILED;
            xt_diag_command_locked(XT_DIAG_RX, generation, event->value, bytes,
                                   payload ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM);
        }
        payload = NULL;
        break;
    }
    xiaotai_critical_exit(&xt.lock);
    if (payload) (void)free_payload(payload);
    reap_commands();
}

void xiaotai_rtc_on_command(xiaotai_rtc_conn_t conn, uint32_t request, uint32_t word, const uint8_t *data, size_t size)
{
    if (!xt.slots || !xt.event_ring) return;
    reap_commands();
#if SIZE_MAX > UINT32_MAX
    if (size > UINT32_MAX) size = UINT32_MAX;
#endif
    const uint32_t bytes = (uint32_t)size;
    xiaotai_critical_enter(&xt.lock);
    bool early = false;
    xt_slot_t *slot = xt_link_event_slot_locked(conn, request, &early);
    if (!slot || (atomic_load(&slot->flags) & XT_SLOT_ERROR)) {
        xiaotai_critical_exit(&xt.lock);
        return;
    }
    peak(&xt.counters.command_largest, bytes);
    if (bytes > slot->stats.command_largest) slot->stats.command_largest = bytes;
    if (xt.event_count == xt.event_slots) {
        overflow_locked(slot);
        xiaotai_critical_exit(&xt.lock);
        xt_link_work();
        return;
    }
    const uint32_t generation = atomic_load(&slot->generation);
    xt_event_t event = {.generation = generation, .value = word, .size = bytes,
                        .meta = EVENT_COMMAND | (early ? EVENT_EARLY : 0U)};
    const uint32_t place = bytes && data ? reserve_command_locked(slot, generation, bytes) : EVENT_OFFSET_MASK;
    if (place != EVENT_OFFSET_MASK) event.meta |= EVENT_WRITING | place;
    else if (bytes) event.meta |= EVENT_NO_DATA;
    if (!(event.meta & EVENT_DYNAMIC))
        xt_diag_command_locked(XT_DIAG_RX, generation, word, bytes,
                               event.meta & EVENT_NO_DATA ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK);
    append_locked(event);
    xiaotai_critical_exit(&xt.lock);

    const uint32_t offset = place & EVENT_OFFSET_MASK;
    if (place & EVENT_DYNAMIC) {
        store_demand_command(offset, generation, data, bytes);
    } else if (place != EVENT_OFFSET_MASK) {
        /* The reserved span stays visible even if this generation ends
         * during the copy. No large payload is copied inside the critical section. */
        uint8_t *pool = xt.command_pool;
        memcpy(&pool[offset], data, bytes);
        pool[offset + bytes] = 0;
        xiaotai_critical_enter(&xt.lock);
        for (uint8_t i = 0; i < xt.event_count; ++i) {
            xt_event_t *item = &events()[i];
            if (item->generation == generation && (item->meta & (EVENT_WRITING | EVENT_DYNAMIC)) == EVENT_WRITING &&
                (item->meta & EVENT_OFFSET_MASK) == offset) {
                if (item->meta & EVENT_DISCARD) remove_event_locked(i);
                else {
                    item->meta &= ~EVENT_WRITING;
                    if (xt.command_on_demand) demand_peaks_locked(generation);
                }
                break;
            }
        }
        xiaotai_critical_exit(&xt.lock);
    }
    xt_worker_kick();
}

xiaotai_err_t xiaotai_rtc_on_subscribe(xiaotai_rtc_conn_t conn, uint32_t request, xiaotai_link_media_kind_t kind,
                                       uint8_t stream, bool subscribed)
{
    if (!xt.slots || !xt.event_ring) return XIAOTAI_ERR_INVALID_STATE;
    if ((kind != XIAOTAI_LINK_AUDIO && kind != XIAOTAI_LINK_VIDEO) || stream > XIAOTAI_LINK_STREAM_MAX)
        return XIAOTAI_ERR_INVALID_ARG;
    bool early = false, overflow = false;
    xiaotai_err_t result = XIAOTAI_OK;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_event_slot_locked(conn, request, &early);
    if (!slot || (atomic_load(&slot->flags) & XT_SLOT_ERROR)) {
        result = XIAOTAI_ERR_INVALID_STATE;
    } else {
        const xiaotai_link_media_t *media = kind == XIAOTAI_LINK_AUDIO ? &slot->profile.audio : &slot->profile.video;
        if (!media->send || (stream != media->send_stream && media->peer_streams == XIAOTAI_LINK_PEER_STREAM_REFUSE)) {
            if (subscribed) ++slot->stats.peer_subscriptions_refused;
            result = XIAOTAI_ERR_NOT_SUPPORTED;
        } else if (xt.event_count == xt.event_slots) {
            if (subscribed) ++slot->stats.peer_subscriptions_refused;
            if (!subscribed) {
                overflow_locked(slot);
                overflow = true;
            }
            result = XIAOTAI_ERR_BUSY;
        } else {
            const uint32_t value = (uint32_t)kind | ((uint32_t)stream << 8) | (subscribed ? 1U << 16 : 0U);
            append_locked((xt_event_t){.generation = atomic_load(&slot->generation), .value = value,
                                      .meta = early ? EVENT_EARLY : 0U});
            xt_send_peer_locked(slot, kind, stream, subscribed);
        }
    }
    xiaotai_critical_exit(&xt.lock);
    if (overflow) xt_link_work();
    else if (result == XIAOTAI_OK) xt_worker_kick();
    return result;
}

static bool pending_for(uint32_t generation)
{
    for (uint8_t i = 0; i < xt.event_count; ++i)
        if (events()[i].generation == generation && !(events()[i].meta & EVENT_DISCARD)) return true;
    return false;
}

static xt_slot_t *event_owner(uint32_t generation)
{
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        xt_slot_t *slot = &xt.slots[i];
        const uint32_t flags = atomic_load(&slot->flags);
        if (((flags & XT_SLOT_REPORTED) && atomic_load(&slot->generation) == generation) ||
            ((flags & XT_SLOT_ENDED) && slot->ended_generation == generation)) return slot;
    }
    return NULL;
}

static int next_event_locked(void)
{
    for (uint8_t i = 0; i < xt.event_count; ++i) {
        const xt_event_t *event = &events()[i];
        if ((event->meta & (EVENT_WRITING | EVENT_DISCARD)) || !event_owner(event->generation)) continue;
        bool has_prior = false;
        for (uint8_t p = 0; p < i && !has_prior; ++p)
            has_prior = events()[p].generation == event->generation && !(events()[p].meta & EVENT_DISCARD);
        if (!has_prior) return i;
    }
    return -1;
}

static bool opened_ready_locked(const xt_slot_t *slot, uint32_t flags)
{
    if (!(flags & XT_SLOT_OPENED)) return false;
    if (!slot->listener_id || !slot->opening.predecessor) return true;
    const xt_slot_t *old = xt_link_stats_locked(slot->opening.predecessor);
    return !old || old->listener_id != slot->listener_id || !(atomic_load(&old->flags) & XT_SLOT_CLOSE_CONFIRMED);
}

static bool ready_locked(void)
{
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        const uint32_t flags = atomic_load(&xt.slots[i].flags);
        if (opened_ready_locked(&xt.slots[i], flags) || ((flags & XT_SLOT_ENDED) &&
            !pending_for(xt.slots[i].ended_generation) && !xt_receive_busy(xt.slots[i].ended_generation)))
            return true;
    }
    return next_event_locked() >= 0;
}

static void report_event(const xiaotai_link_handlers_t *handlers, const xt_event_t *event)
{
    if (event->meta & EVENT_COMMAND) {
        /* No command was copied on allocator failure; some zero-payload
         * control words would still act even with the dropped flag set. */
        if (!handlers->command || (event->meta & EVENT_ALLOC_FAILED)) return;
        const uint32_t word = event->value;
        const bool dropped = (event->meta & EVENT_NO_DATA) != 0;
        const uint32_t offset = event->meta & EVENT_OFFSET_MASK;
        const uint8_t *data = event->size && !dropped ?
            ((event->meta & EVENT_DYNAMIC) ? xt.command_slots[offset]->data : xt.command_pool + offset) : NULL;
        handlers->command(handlers->ctx, event->generation, &(xiaotai_link_command_t){
            .word = word, .id = (uint16_t)((word >> 1) & 0x7fffU), .sequence = (uint16_t)(word >> 16),
            .response = (word & 1U) != 0, .data = data, .size = event->size, .dropped = dropped});
    } else if (handlers->peer_subscribed) {
        handlers->peer_subscribed(handlers->ctx, event->generation, (xiaotai_link_media_kind_t)(event->value & 0xffU),
                                  (uint8_t)(event->value >> 8), (event->value & (1U << 16)) != 0);
    }
}

static void drain_owner(void *ctx)
{
    (void)ctx;
    for (;;) {
        xiaotai_link_handlers_t handlers = {0};
        xiaotai_link_end_t reason = XIAOTAI_LINK_END_STOPPED;
        uint32_t generation = 0;
        int detail = 0;
        bool opened = false, revoked = false, command_oom = false;
        xt_event_t event = {0};
        xiaotai_critical_enter(&xt.lock);
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            xt_slot_t *slot = &xt.slots[i];
            const uint32_t flags = atomic_load(&slot->flags);
            if (opened_ready_locked(slot, flags)) {
                if (!xt_inbound_open_allowed_locked(slot)) {
                    (void)xt_link_end_locked(slot, XIAOTAI_LINK_END_STOPPED, 0, true, false);
                    revoked = true;
                    break;
                }
                generation = atomic_load(&slot->generation);
                handlers = slot->handlers;
                opened = true;
                /* The owner is about to call opened without yielding to a
                 * different owner job. OFF cannot undo this handoff. */
                atomic_store(&slot->flags, (flags & ~(uint32_t)XT_SLOT_OPENED) | XT_SLOT_REPORTED);
                xt_send_sync_gate_locked(slot, XIAOTAI_LINK_AUDIO);
                xt_send_sync_gate_locked(slot, XIAOTAI_LINK_VIDEO);
                break;
            }
        }
        if (!generation && !revoked) {
            const int index = next_event_locked();
            if (index >= 0) {
                event = events()[index];
                generation = event.generation;
                xt_slot_t *slot = event_owner(generation);
                handlers = slot->handlers;
                if (event.meta & EVENT_COMMAND) {
                    ++slot->stats.commands_received;
                    if (event.meta & EVENT_NO_DATA) ++slot->stats.commands_dropped;
                    if (event.meta & EVENT_EARLY) ++slot->stats.commands_early;
                    xt_diag_command_locked(XT_DIAG_DISPATCH, generation, event.value, event.size,
                        event.meta & EVENT_NO_DATA ? XIAOTAI_ERR_NO_MEM : XIAOTAI_OK);
                }
                xt.owner_event = event;
                remove_event_locked((uint8_t)index);
                if ((event.meta & EVENT_ALLOC_FAILED) && xt.config.command_oom_ends_link &&
                    atomic_load(&slot->generation) == generation && xt_link_open_locked(slot)) {
                    xt_queue_link_error_locked(slot, XIAOTAI_LINK_END_NO_MEMORY, XIAOTAI_ERR_NO_MEM);
                    xt_link_events_drop_locked(generation);
                    command_oom = true;
                }
            } else {
                for (uint8_t i = 0; i < xt.links_max; ++i) {
                    xt_slot_t *slot = &xt.slots[i];
                    if (!(atomic_load(&slot->flags) & XT_SLOT_ENDED) || pending_for(slot->ended_generation) ||
                        xt_receive_busy(slot->ended_generation))
                        continue;
                    generation = slot->ended_generation;
                    handlers = slot->handlers;
                    reason = slot->end_reason;
                    detail = slot->end_detail;
                    atomic_store(&slot->flags, 0);
                    break;
                }
            }
        }
        if (!generation && !revoked) xt.owner_drain_posted = false;
        xiaotai_critical_exit(&xt.lock);
        if (revoked) {
            xt_link_work();
        } else if (!generation) {
            return;
        } else if (opened) {
            xt_diag_link("link_open", generation, 0, false);
            if (handlers.opened) handlers.opened(handlers.ctx, generation);
        } else if (event.generation) {
            if (command_oom) xt_link_work();
            report_event(&handlers, &event);
            xiaotai_critical_enter(&xt.lock);
            retire_event_locked(&xt.owner_event);
            xt.owner_event = (xt_event_t){0};
            xiaotai_critical_exit(&xt.lock);
            reap_commands();
        } else if (handlers.closed) {
            handlers.closed(handlers.ctx, generation, reason, detail);
        }
    }
}

void xt_link_events_tick(void)
{
    reap_commands();
    const int64_t real_now = xiaotai_time_us() / 1000;
    if (xt.owner_drain_retry_at && real_now < xt.owner_drain_retry_at) return;
    xt.owner_drain_retry_at = 0;
    xiaotai_critical_enter(&xt.lock);
    const bool pending = !xt.owner_drain_posted && ready_locked();
    if (pending) xt.owner_drain_posted = true;
    xiaotai_critical_exit(&xt.lock);
    if (pending && xiaotai_run(drain_owner, NULL) != XIAOTAI_OK) {
        xiaotai_critical_enter(&xt.lock);
        xt.owner_drain_posted = false;
        xiaotai_critical_exit(&xt.lock);
        xt.owner_drain_retry_at = real_now + XT_OWNER_DRAIN_RETRY_MS;
    }
}
