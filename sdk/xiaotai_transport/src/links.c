// SPDX-License-Identifier: MIT
#include <string.h>

#include "internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/media_rx_ingress.h"

#define XT_CANCEL_POLL_MS 20

/* Only the worker touches submitted input after it takes it from a slot.
 * The SDK call may report completion before returning, and the owner may
 * withdraw an in-flight request before the SDK gives back its handle. */
static xt_connect_args_t *s_submitted;
static struct {
    uint32_t generation;
    xiaotai_link_release_state_t state;
    bool tagged_close, closed_record, args_reaped;
} s_release_watch;

static bool retirement_supported(void)
{
    const xiaotai_rtc_caps_t *caps = xiaotai_rtc_caps();
    return caps->cancel_pending && caps->published_retirement_barrier;
}

void xt_link_tag_peer_retirement(xiaotai_rtc_conn_t conn, uint32_t generation, xiaotai_rtc_peer_tag_fn tag)
{
    if (!tag || !retirement_supported() || !tag(conn, generation)) return;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    /* The SDK may publish first; opened() need not have reached the owner yet. */
    if (slot && slot->conn == conn && atomic_load(&slot->state) == XT_SLOT_OPEN &&
        !(atomic_load(&slot->flags) & (XT_SLOT_ERROR | XT_SLOT_FINISHING)) && !atomic_load(&xt.admission_closed))
        atomic_fetch_or(&slot->flags, (uint32_t)XT_SLOT_PEER_RETIREMENT_TAGGED);
    xiaotai_critical_exit(&xt.lock);
}

xiaotai_err_t xiaotai_link_watch_release(uint32_t generation)
{
    if (!generation) return XIAOTAI_ERR_INVALID_ARG;
    if (!retirement_supported()) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!xt.sync_ready) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (s_release_watch.generation) result = XIAOTAI_ERR_BUSY;
    else if (xt_link_open_locked(slot) && (slot->kind == XIAOTAI_LINK_WHIP ||
             (atomic_load(&slot->flags) & XT_SLOT_PEER_RETIREMENT_TAGGED))) {
        s_release_watch.generation = generation;
        s_release_watch.state = XIAOTAI_LINK_RELEASE_PENDING;
        s_release_watch.tagged_close = s_release_watch.closed_record = false;
        /* An inbound link never borrowed request arguments. */
        s_release_watch.args_reaped = slot->kind == XIAOTAI_LINK_INBOUND;
        result = XIAOTAI_OK;
    }
    xiaotai_critical_exit(&xt.lock);
    return result;
}

xiaotai_link_release_state_t xiaotai_link_release_state(uint32_t generation)
{
    if (!generation || !xt.sync_ready) return XIAOTAI_LINK_RELEASE_UNWATCHED;
    xiaotai_critical_enter(&xt.lock);
    const xiaotai_link_release_state_t state = s_release_watch.generation == generation
        ? s_release_watch.state : XIAOTAI_LINK_RELEASE_UNWATCHED;
    xiaotai_critical_exit(&xt.lock);
    return state;
}

void xiaotai_link_unwatch_release(uint32_t generation)
{
    if (!generation || !xt.sync_ready) return;
    xiaotai_critical_enter(&xt.lock);
    if (s_release_watch.generation == generation) s_release_watch.generation = 0;
    xiaotai_critical_exit(&xt.lock);
}

static size_t table_bytes(void)
{
    return (size_t)xt.links_max * (sizeof(xt_release_t) + sizeof(xt_slot_t));
}

static void forget_args(xt_connect_args_t *args)
{
    if (!args) return;
    volatile char *p = args->text;
    for (size_t i = 0; i < args->bytes; ++i) p[i] = 0;
    xt_account(-(int64_t)(sizeof(*args) + args->bytes));
    xiaotai_mem_free(args);
}

static void reap_submitted(bool stack_released)
{
    for (xt_connect_args_t **at = &s_submitted; *at;) {
        xt_connect_args_t *args = *at;
        bool retained = false;
        if (!stack_released) {
            xiaotai_critical_enter(&xt.lock);
            retained = xt.orphan_link_inputs || xt_link_generation_locked(args->generation) != NULL;
            for (uint8_t i = 0; !retained && i < xt.links_max; ++i)
                retained = xt.releases[i].used && xt.releases[i].generation == args->generation;
            xiaotai_critical_exit(&xt.lock);
        }
        if (retained) {
            at = &args->next_submitted;
        } else {
            *at = args->next_submitted;
            const uint32_t generation = args->generation;
            forget_args(args);
            if (!stack_released) {
                xiaotai_critical_enter(&xt.lock);
                if (s_release_watch.generation == generation && s_release_watch.closed_record &&
                    s_release_watch.state == XIAOTAI_LINK_RELEASE_PENDING)
                    s_release_watch.args_reaped = true;
                xiaotai_critical_exit(&xt.lock);
            }
        }
    }
}

static void finish_watched_retirement(int64_t now)
{
    if (!retirement_supported()) return;
    xiaotai_critical_enter(&xt.lock);
    const uint32_t generation = s_release_watch.state == XIAOTAI_LINK_RELEASE_PENDING &&
        s_release_watch.closed_record && s_release_watch.args_reaped ? s_release_watch.generation : 0;
    xiaotai_critical_exit(&xt.lock);
    if (!generation) return;
    /* The vendor lock is independent of xt.lock. Do not enter the backend
     * from a callback or while holding the link lock. */
    if (xiaotai_rtc_pending() != 0) {
        xt_worker_wake_at(now + XT_CANCEL_POLL_MS);
        return;
    }
    xiaotai_critical_enter(&xt.lock);
    if (s_release_watch.generation == generation && s_release_watch.state == XIAOTAI_LINK_RELEASE_PENDING &&
        s_release_watch.closed_record && s_release_watch.args_reaped) s_release_watch.state = XIAOTAI_LINK_RELEASE_COMPLETE;
    xiaotai_critical_exit(&xt.lock);
}

xiaotai_err_t xt_links_alloc(void)
{
    if (xt.slots) return XIAOTAI_OK;
    /* Release records are first for 64-bit alignment; slot atomics are internal. */
    void *block = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, table_bytes());
    if (!block) return XIAOTAI_ERR_NO_MEM;
    xt.releases = block;
    xt.slots = (xt_slot_t *)(void *)(xt.releases + xt.links_max);
    xt_account((int64_t)table_bytes());
    return XIAOTAI_OK;
}

void xt_links_free(void)
{
    reap_submitted(true); /* reset has already joined the worker */
    if (xt.sync_ready) {
        xiaotai_critical_enter(&xt.lock);
        s_release_watch.generation = 0;
        xiaotai_critical_exit(&xt.lock);
    }
    if (!xt.releases) return;
    for (uint8_t i = 0; i < xt.links_max; ++i) forget_args(xt.slots[i].args);
    xiaotai_mem_free(xt.releases);
    xt_account(-(int64_t)table_bytes());
    xt.releases = NULL;
    xt.slots = NULL;
}

void xt_link_work(void)
{
    atomic_store(&xt.links_dirty, true);
    xt_worker_kick();
}

xt_slot_t *xt_link_free_locked(void)
{
    if (!xt.slots) return NULL;
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        xt_slot_t *slot = &xt.slots[i];
        if (atomic_load(&slot->state) == XT_SLOT_FREE && !atomic_load(&slot->flags)) return slot;
    }
    return NULL;
}

bool xt_link_physical_room_locked(void)
{
    unsigned occupied = 0;
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        occupied += xt.slots[i].conn || xt.slots[i].request;
        occupied += xt.releases[i].used;
    }
    return occupied < xt.links_max;
}

static void init_slot(xt_slot_t *slot, xiaotai_link_kind_t kind, const xiaotai_link_profile_t *profile,
                      const xiaotai_link_handlers_t *handlers)
{
    slot->kind = kind;
    slot->profile = *profile;
    slot->handlers = *handlers;
    slot->conn = 0;
    slot->request = 0;
    slot->args = NULL;
    slot->opening.deadline = 0;
    slot->listener_id = 0;
    slot->guarded_open = false;
    slot->uses = 0;
    slot->ended_generation = 0;
    slot->stats = (xiaotai_link_stats_t){.kind = kind, .audio_send_stream = profile->audio.send_stream};
    slot->rx[0] = (xt_receiver_t){0};
    slot->rx[1] = (xt_receiver_t){0};
    atomic_store(&slot->send_ready[0], 0);
    atomic_store(&slot->send_ready[1], 0);
    atomic_store(&slot->key_frame_for, 0);
    atomic_store(&slot->bitrate_for, 0);
    atomic_store(&slot->bitrate_consumed, atomic_load(&slot->bitrate_sequence));
    slot->tx_busy[0] = slot->tx_busy[1] = false;
    slot->video_needs_key = true;
    slot->bitrate_enabled = false;
    slot->video_key_epoch = 0;
    slot->audio_failures = 0;
    slot->receive_accepted = slot->receive_rejected = 0;
}

static uint32_t closed_predecessor_locked(uint32_t listener_id)
{
    uint32_t generation = 0;
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        const xt_slot_t *old = &xt.slots[i];
        if (old->listener_id != listener_id) continue;
        const uint32_t flags = atomic_load(&old->flags);
        if (flags & XT_SLOT_REPORTED && atomic_load(&old->state) == XT_SLOT_OPEN) return 0;
        if (flags & XT_SLOT_CLOSE_CONFIRMED) {
            if (generation) return 0;
            generation = old->ended_generation;
        }
    }
    return generation;
}

void xt_link_open_inbound_locked(xt_slot_t *slot, xiaotai_rtc_conn_t conn, uint32_t listener_id,
                                 const xt_listener_t *listener)
{
    init_slot(slot, XIAOTAI_LINK_INBOUND, &listener->listener.profile, &listener->listener.handlers);
    slot->conn = conn;
    slot->listener_id = listener_id;
    slot->guarded_open = listener->mode == XIAOTAI_LINK_LISTEN_OPEN && listener->listener.admit_open;
    /* Under the same lock as the SDK close callback: a later close cannot
     * turn an originally concurrent admission into a replacement. */
    slot->opening.predecessor = listener->mode == XIAOTAI_LINK_LISTEN_OPEN
        ? closed_predecessor_locked(listener_id) : 0;
    atomic_store(&slot->flags, XT_SLOT_OPENED);
    atomic_store(&slot->state, XT_SLOT_OPEN);
    atomic_store(&slot->generation, xiaotai_generation_next());
}

static xt_release_t *take_record(xt_release_t *table, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        if (!table[i].used) return &table[i];
    return NULL;
}

static void keep_release(xiaotai_rtc_conn_t conn, uint32_t request, uint32_t generation, const xt_slot_t *slot,
                         bool closed)
{
    if (!conn && !request) return;
    xt_release_t *record = take_record(xt.releases, xt.links_max);
    if (record) {
        *record = (xt_release_t){.conn = conn, .request = request, .since_ms = xt_now_ms(),
                                .listener_id = slot->listener_id, .strict_retire = slot->guarded_open,
                                .generation = generation, .uses = slot->uses, .closed = closed, .used = true};
    } else {
        if (slot->listener_id) xt.orphan_listeners |= (uint16_t)(1U << (slot->listener_id - 1U));
        /* Without a release record, only a confirmed stop proves the SDK no
         * longer borrows the inputs. The existing overflow fault stops it. */
        xt.orphan_link_inputs = true;
        atomic_store(&xt.lifecycle_overflow, true);
    }
}

static void refuse_locked(xiaotai_rtc_conn_t conn, uint32_t generation)
{
    xt_release_t *record = take_record(xt.refused, XIAOTAI_TRANSPORT_LINKS_MAX);
    if (record) {
        *record = (xt_release_t){.conn = conn, .since_ms = xt_now_ms(), .generation = generation, .used = true};
    } else {
        atomic_store(&xt.lifecycle_overflow, true);
    }
}

void xt_link_refuse_locked(xiaotai_rtc_conn_t conn) { refuse_locked(conn, 0); }

xt_connect_args_t *xt_link_end_locked(xt_slot_t *slot, xiaotai_link_end_t reason, int detail,
                                     bool silent, bool closed)
{
    const uint32_t generation = atomic_load(&slot->generation);
    const uint8_t state = atomic_load(&slot->state);
    if (!generation || state == XT_SLOT_FREE) return NULL;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    xiaotai_media_ingress_break(generation);
#endif
    const uint32_t flags = atomic_load(&slot->flags);
    const uint32_t tentative = flags & XT_SLOT_REPORTED ? 0 : slot->listener_id;
    if (silent || !(flags & XT_SLOT_REPORTED)) xt_link_events_drop_locked(generation);
    if (!closed || slot->uses) {
        if (state == XT_SLOT_REQUESTED) keep_release(0, slot->request, generation, slot, closed);
        else keep_release(slot->conn, 0, generation, slot, closed);
    }
    xt_connect_args_t *args = slot->args;
    slot->args = NULL;
    slot->conn = 0;
    slot->request = 0;
    slot->uses = 0;
    slot->rx[0].on = slot->rx[1].on = false;
    slot->stats.audio_permitted = slot->stats.video_permitted = false;
    slot->stats.audio_subscribed = slot->stats.video_subscribed = false;
    slot->end_reason = reason;
    slot->end_detail = detail;
    slot->ended_generation = generation;
    atomic_store(&slot->generation, 0);
    atomic_store(&slot->send_ready[0], 0);
    atomic_store(&slot->send_ready[1], 0);
    atomic_store(&slot->key_frame_for, 0);
    atomic_store(&slot->bitrate_for, 0);
    atomic_store(&slot->state, XT_SLOT_FREE);
    atomic_store(&slot->flags, silent || tentative ? 0U :
                 XT_SLOT_ENDED | (flags & (XT_SLOT_REPORTED | XT_SLOT_FINISHING)) |
                 (state == XT_SLOT_OPEN ? XT_SLOT_WAS_OPEN : 0U));
    xt_diag_link("link_end", generation, detail,
                 reason != XIAOTAI_LINK_END_PEER && reason != XIAOTAI_LINK_END_PEER_FAULT);
    return args;
}

xt_slot_t *xt_link_generation_locked(uint32_t generation)
{
    if (generation && xt.slots) {
        for (uint8_t i = 0; i < xt.links_max; ++i)
            if (atomic_load(&xt.slots[i].generation) == generation) return &xt.slots[i];
    }
    return NULL;
}

xt_slot_t *xt_link_stats_locked(uint32_t generation)
{
    xt_slot_t *slot = xt_link_generation_locked(generation);
    if (slot || !generation || !xt.slots) return slot;
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        slot = &xt.slots[i];
        if (!atomic_load(&slot->generation) && slot->ended_generation == generation) return slot;
    }
    return NULL;
}

bool xt_link_open_locked(const xt_slot_t *slot)
{
    return slot && slot->conn && atomic_load(&slot->state) == XT_SLOT_OPEN &&
           (atomic_load(&slot->flags) & (XT_SLOT_ERROR | XT_SLOT_REPORTED)) == XT_SLOT_REPORTED &&
           !atomic_load(&xt.admission_closed);
}

static bool pin_locked(xt_slot_t *slot)
{
    if (slot->uses == UINT16_MAX) return false;
    ++slot->uses;
    atomic_fetch_add(&xt.backend_users, 1);
    return true;
}

bool xt_link_pin_locked(xt_slot_t *slot)
{
    return xt_link_open_locked(slot) && pin_locked(slot);
}

xiaotai_err_t xt_link_finish_opening(uint32_t generation, bool *opened, bool pin, xiaotai_rtc_conn_t *conn)
{
    if (!generation || !xt.slots) return XIAOTAI_ERR_INVALID_STATE;
    xt_connect_args_t *args = NULL;
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    bool changed = false;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    if (slot && slot->kind != XIAOTAI_LINK_INBOUND) {
        const uint32_t flags = atomic_load(&slot->flags);
        if (atomic_load(&slot->state) == XT_SLOT_REQUESTED) {
            args = xt_link_end_locked(slot, XIAOTAI_LINK_END_STOPPED, 0, true, false);
            changed = true;
            result = XIAOTAI_OK;
        } else if ((flags & XT_SLOT_OPENED) && slot->conn) {
            /* Claim the physical link without handing off a session or opening
             * its media gates. Stack callbacks cannot change the decision. */
            *opened = true;
            atomic_store(&slot->flags, (flags & ~(uint32_t)XT_SLOT_OPENED) | XT_SLOT_FINISHING);
            xt_link_events_drop_locked(generation);
            if (!pin) result = XIAOTAI_OK;
            else if (!(flags & XT_SLOT_ERROR) && !atomic_load(&xt.admission_closed) && pin_locked(slot)) {
                *conn = slot->conn;
                result = XIAOTAI_OK;
            }
        }
    } else if (!slot) {
        slot = xt_link_stats_locked(generation);
        if (slot && slot->kind != XIAOTAI_LINK_INBOUND) {
            const uint32_t flags = atomic_load(&slot->flags);
            if ((flags & (XT_SLOT_ENDED | XT_SLOT_REPORTED | XT_SLOT_FINISHING)) == XT_SLOT_ENDED) {
                *opened = (flags & XT_SLOT_WAS_OPEN) != 0;
                xt_link_events_drop_locked(generation);
                atomic_store(&slot->flags, 0);
                changed = true;
                if (!*opened || !pin) result = XIAOTAI_OK;
            }
        }
    }
    xiaotai_critical_exit(&xt.lock);
    forget_args(args);
    if (changed) xt_link_work();
    return result;
}

void xt_link_unpin(uint32_t generation)
{
    bool retiring = false;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    if (slot && slot->uses) --slot->uses;
    else {
        retiring = true;
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            xt_release_t *record = &xt.releases[i];
            if (record->used && record->generation == generation && record->uses) {
                --record->uses;
                break;
            }
        }
    }
    const bool drained = atomic_fetch_sub(&xt.backend_users, 1) == 1;
    xiaotai_critical_exit(&xt.lock);
    if (retiring || (drained && atomic_load(&xt.admission_closed))) xt_worker_kick();
}

void xt_queue_link_error_locked(xt_slot_t *slot, xiaotai_link_end_t reason, int detail)
{
    if (!xt_link_open_locked(slot)) return;
    slot->end_reason = reason;
    slot->end_detail = detail;
    atomic_fetch_or(&slot->flags, (uint32_t)XT_SLOT_ERROR);
    atomic_store(&slot->send_ready[0], 0);
    atomic_store(&slot->send_ready[1], 0);
}

xt_slot_t *xt_link_conn_locked(xiaotai_rtc_conn_t conn)
{
    if (conn) {
        for (uint8_t i = 0; i < xt.links_max; ++i)
            if (xt.slots[i].conn == conn) return &xt.slots[i];
    }
    return NULL;
}

static xt_slot_t *by_request(uint32_t request)
{
    if (request) {
        for (uint8_t i = 0; i < xt.links_max; ++i)
            if (xt.slots[i].request == request) return &xt.slots[i];
    }
    return NULL;
}

xt_slot_t *xt_link_event_slot_locked(xiaotai_rtc_conn_t conn, uint32_t request, bool *early)
{
    if (!conn) return NULL;
    xt_slot_t *slot = xt_link_conn_locked(conn);
    if (slot && (atomic_load(&slot->flags) & XT_SLOT_FINISHING)) return NULL;
    if (slot && atomic_load(&slot->state) == XT_SLOT_OPEN) {
        *early = false;
        return slot;
    }
    if (request) {
        if (!slot) slot = by_request(request);
        if (slot && slot->request != request) slot = NULL;
    } else if (!slot) {
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            xt_slot_t *candidate = &xt.slots[i];
            if (candidate->request && atomic_load(&candidate->state) == XT_SLOT_REQUESTED) {
                if (slot) { slot = NULL; break; } /* Anonymous callback is ambiguous. */
                slot = candidate;
            }
        }
    }
    if (!slot || (slot->conn && slot->conn != conn) || xt.config.early_events_dropped) {
        atomic_fetch_add(&xt.counters.early_dropped, 1);
        return NULL;
    }
    slot->conn = conn;
    *early = true;
    return slot;
}

static xt_release_t *release_by_conn(xt_release_t *table, size_t len, xiaotai_rtc_conn_t conn, bool awaiting_close,
                                     uint32_t generation)
{
    for (size_t i = 0; i < len; ++i)
        if (table[i].used && table[i].conn == conn && (!awaiting_close || !table[i].closed) &&
            (!generation || generation == table[i].generation)) return &table[i];
    return NULL;
}

static xiaotai_link_end_t reported_end(xiaotai_link_end_t reason, int detail)
{
    return xt.config.distinguish_reported_peer_close && reason == XIAOTAI_LINK_END_PEER && detail
           ? XIAOTAI_LINK_END_PEER_FAULT : reason;
}

void xiaotai_rtc_on_connected_peer(uint32_t request, xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail,
                                   xiaotai_rtc_peer_tag_fn tag)
{
    if (!xt.slots || !request) return;
    uint32_t to_tag = 0;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = by_request(request);
    if (slot && conn && (!slot->conn || slot->conn == conn)) {
        slot->conn = conn;
        if (tag && detail == 0 && slot->kind == XIAOTAI_LINK_P2P && slot->profile.track_peer_retirement)
            to_tag = atomic_load(&slot->generation);
        slot->request = 0;
        atomic_store(&slot->state, XT_SLOT_OPEN);
        atomic_fetch_or(&slot->flags, (uint32_t)XT_SLOT_OPENED);
    } else if (slot) {
        (void)xt_link_end_locked(slot, conn ? XIAOTAI_LINK_END_CONNECT_FAILED : reported_end(reason, detail),
                                 detail, false, true);
        if (conn) refuse_locked(conn, request);
    } else {
        xt_release_t *release = NULL;
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            if (xt.releases[i].used && xt.releases[i].request == request) {
                release = &xt.releases[i];
                break;
            }
        }
        if (release) {
            if (conn) {
                release->conn = conn;
                release->request = 0;
            } else memset(release, 0, sizeof(*release));
        } else if (conn && !xt_link_conn_locked(conn) && !release_by_conn(xt.releases, xt.links_max, conn, false, 0) &&
                   !release_by_conn(xt.refused, XIAOTAI_TRANSPORT_LINKS_MAX, conn, false, 0)) {
            refuse_locked(conn, request);
        }
        if (conn && (release || !xt_link_conn_locked(conn))) atomic_fetch_add(&xt.counters.late_links_closed, 1);
    }
    if (request == xt.run.connect_loan_generation)
        atomic_store(&xt.connect_result_generation, request);
    xiaotai_critical_exit(&xt.lock);
    if (to_tag) xt_link_tag_peer_retirement(conn, to_tag, tag);
    xt_link_work();
}

void xiaotai_rtc_on_connected(uint32_t request, xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail)
{
    xiaotai_rtc_on_connected_peer(request, conn, reason, detail, NULL);
}

void xiaotai_rtc_on_error(xiaotai_rtc_conn_t conn, xiaotai_link_end_t reason, int detail)
{
    if (!xt.slots || !conn) return;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_conn_locked(conn);
    if (slot) (void)xt_link_end_locked(slot, reported_end(reason, detail), detail, false, false);
    xiaotai_critical_exit(&xt.lock);
    if (slot) xt_link_work();
}

static void confirm_close_locked(uint32_t generation)
{
    xt_slot_t *slot = xt_link_stats_locked(generation);
    if (!slot || !slot->listener_id || slot->ended_generation != generation) return;
    const uint32_t required = XT_SLOT_ENDED | XT_SLOT_REPORTED | XT_SLOT_WAS_OPEN;
    if ((atomic_load(&slot->flags) & required) == required)
        atomic_fetch_or(&slot->flags, XT_SLOT_CLOSE_CONFIRMED);
}

static void closed_conn(xiaotai_rtc_conn_t conn, uint32_t observed)
{
    if (!xt.slots || !conn) return;
    xiaotai_critical_enter(&xt.lock);
    /* The SDK may reuse a handle after an earlier close callback returns,
     * before the worker reaps the acknowledged record for its old pins. */
    xt_release_t *release = release_by_conn(xt.releases, xt.links_max, conn, true, observed);
    if (!release) release = release_by_conn(xt.refused, XIAOTAI_TRANSPORT_LINKS_MAX, conn, true, observed);
    xt_slot_t *slot = release ? NULL : xt_link_conn_locked(conn);
    if (slot && observed && observed != atomic_load(&slot->generation)) slot = NULL;
    const bool watched = observed && observed == s_release_watch.generation &&
                         s_release_watch.state == XIAOTAI_LINK_RELEASE_PENDING;
    if (release) {
        release->closed = true;
        if (watched) s_release_watch.tagged_close = true;
        confirm_close_locked(release->generation);
    } else if (slot) {
        const uint32_t generation = atomic_load(&slot->generation);
        if (watched) {
            s_release_watch.tagged_close = true;
            /* A remote close without pins would otherwise have no record. */
            if (!slot->uses) keep_release(conn, 0, generation, slot, true);
        }
        (void)xt_link_end_locked(slot, XIAOTAI_LINK_END_PEER, 0, false, true);
        confirm_close_locked(generation);
    }
    xiaotai_critical_exit(&xt.lock);
    if (slot || release) xt_link_work();
}

void xiaotai_rtc_on_closed(xiaotai_rtc_conn_t conn) { closed_conn(conn, 0); }
void xiaotai_rtc_on_closed_for(xiaotai_rtc_conn_t conn, uint32_t generation)
{
    if (generation) closed_conn(conn, generation);
}

static size_t text_size(const char *text, size_t limit)
{
    if (!text) return 0;
    for (size_t i = 0; i < limit; ++i)
        if (!text[i]) return i + 1U;
    return limit + 1U;
}

xiaotai_err_t xiaotai_link_connect(const xiaotai_link_request_t *request, const xiaotai_link_handlers_t *handlers,
                                  uint32_t *generation)
{
    if (generation) *generation = 0;
    if (!request || !handlers || !generation || !request->peer || !request->peer[0]) return XIAOTAI_ERR_INVALID_ARG;
    if (request->kind != XIAOTAI_LINK_WHIP && request->kind != XIAOTAI_LINK_P2P) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_err_t result = xt_link_profile_check(&request->profile);
    if (result != XIAOTAI_OK) return result;
    result = xt_link_admissible();
    if (result != XIAOTAI_OK) return result;
    const size_t peer = text_size(request->peer, XIAOTAI_LINK_PEER_MAX);
    const size_t token = request->token ? text_size(request->token, XIAOTAI_LINK_TOKEN_MAX) : 1U;
    if (peer > XIAOTAI_LINK_PEER_MAX || token > XIAOTAI_LINK_TOKEN_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    xiaotai_critical_enter(&xt.lock);
    const bool room = xt_link_free_locked() != NULL;
    xiaotai_critical_exit(&xt.lock);
    if (!room) return XIAOTAI_ERR_BUSY;
    if ((result = xt_connect_memory_check()) != XIAOTAI_OK) return result;

    const size_t bytes = peer + token;
    xt_connect_args_t *args = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*args) + bytes);
    if (!args) return XIAOTAI_ERR_NO_MEM;
    xt_account((int64_t)(sizeof(*args) + bytes));
    args->bytes = bytes;
    args->token_offset = peer;
    memcpy(args->text, request->peer, peer);
    if (request->token) memcpy(args->text + peer, request->token, token);
    else args->text[peer] = 0;
    const int64_t now = xt_now_ms();

    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_free_locked();
    result = xt_link_admissible();
    if (slot && result == XIAOTAI_OK) {
        init_slot(slot, request->kind, &request->profile, handlers);
        slot->args = args;
        slot->opening.deadline = request->timeout_ms ? now + (int64_t)request->timeout_ms : 0;
        *generation = xiaotai_generation_next();
        atomic_store(&slot->state, XT_SLOT_REQUESTED);
        atomic_store(&slot->generation, *generation);
        xt_diag_link("link_request", *generation, 0, false);
    }
    xiaotai_critical_exit(&xt.lock);
    if (!*generation) {
        forget_args(args);
        return result == XIAOTAI_OK ? XIAOTAI_ERR_BUSY : result;
    }
    xt_link_work();
    return XIAOTAI_OK;
}

void xiaotai_link_disconnect(uint32_t generation)
{
    if (!generation || !xt.slots) return;
    xt_connect_args_t *args = NULL;
    bool found = false;
    xiaotai_critical_enter(&xt.lock);
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        if (atomic_load(&xt.slots[i].generation) == generation) {
            args = xt_link_end_locked(&xt.slots[i], XIAOTAI_LINK_END_STOPPED, 0, true, false);
            found = true;
            break;
        }
        if (xt.slots[i].ended_generation == generation &&
            (atomic_load(&xt.slots[i].flags) & XT_SLOT_ENDED)) {
            xt_link_events_drop_locked(generation);
            atomic_store(&xt.slots[i].flags, 0);
            found = true;
            break;
        }
    }
    xiaotai_critical_exit(&xt.lock);
    forget_args(args);
    xt_receive_wait(generation, XIAOTAI_LINK_AUDIO);
    xt_receive_wait(generation, XIAOTAI_LINK_VIDEO);
    if (found) xt_link_work();
    else atomic_fetch_add(&xt.counters.stale_calls, 1);
}

void xt_links_end_all(xiaotai_link_end_t reason, int detail)
{
    if (!xt.slots) return;
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        xiaotai_critical_enter(&xt.lock);
        xt_connect_args_t *args = xt_link_end_locked(&xt.slots[i], reason, detail, false, false);
        xiaotai_critical_exit(&xt.lock);
        forget_args(args);
    }
    xt_link_work();
}

static bool release_tick(xt_release_t *record, int64_t now, bool stopping)
{
    xiaotai_rtc_conn_t disconnect = 0;
    uint32_t cancel = 0;
    bool poll = false, expired = false, changed = false;
    const bool cancellable = xiaotai_rtc_caps()->cancel_pending;
    xiaotai_critical_enter(&xt.lock);
    if (record->used && record->uses) {
        xiaotai_critical_exit(&xt.lock);
        if (!stopping) xt_worker_wake_at(now + 1);
        return true;
    }
    if (record->used) {
        if (record->closed) {
            if (!stopping && s_release_watch.tagged_close && s_release_watch.generation == record->generation &&
                s_release_watch.state == XIAOTAI_LINK_RELEASE_PENDING)
                s_release_watch.closed_record = true;
            memset(record, 0, sizeof(*record));
            changed = true;
        } else if (!stopping && now >= record->since_ms + (int64_t)xt.release_timeout_ms) {
            expired = true;
        } else if (record->conn && !record->disconnect_sent) {
            record->disconnect_sent = true;
            disconnect = record->conn;
        } else if (!stopping && record->request && cancellable) {
            if (!record->cancel_sent) {
                record->cancel_sent = true;
                cancel = record->request;
                record->poll_at = now + XT_CANCEL_POLL_MS;
            } else if (now >= record->poll_at) {
                poll = true;
                record->poll_at = now + XT_CANCEL_POLL_MS;
            }
        }
    }
    const int64_t deadline = record->used && !stopping ? record->since_ms + (int64_t)xt.release_timeout_ms : 0;
    const int64_t poll_at = record->used && record->request && !stopping ? record->poll_at : 0;
    const uint32_t request = record->request;
    const bool tolerate = xt.config.release_failures_tolerated && !record->strict_retire;
    xiaotai_critical_exit(&xt.lock);
    int detail = 0;
    xiaotai_err_t result = XIAOTAI_OK;
    if (disconnect) result = xiaotai_rtc_disconnect(disconnect, &detail);
    if (cancel) (void)xiaotai_rtc_cancel(cancel); /* Poll confirms release, not the request's return. */
    if (poll && xiaotai_rtc_pending() == 0U) {
        xiaotai_critical_enter(&xt.lock);
        if (record->used && record->request == request) {
            memset(record, 0, sizeof(*record));
            changed = true;
        }
        xiaotai_critical_exit(&xt.lock);
        if (changed) xt_connect_memory_restore(request);
    }
    if (result != XIAOTAI_OK || expired) {
        atomic_fetch_add(&xt.counters.release_stalls, 1);
        if (!stopping && !tolerate) {
            xt_fault(expired ? XT_FAULT_RELEASE_TIMEOUT : XT_FAULT_RELEASE_FAILED,
                     expired ? XIAOTAI_ERR_TIMEOUT : detail ? detail : result);
            return false;
        }
        if (tolerate) {
            xiaotai_critical_enter(&xt.lock);
            memset(record, 0, sizeof(*record));
            xiaotai_critical_exit(&xt.lock);
            changed = true;
        }
    }
    if (changed) xt_link_work();
    else {
        if (deadline) xt_worker_wake_at(deadline);
        if (poll_at) xt_worker_wake_at(poll_at);
    }
    return true;
}

void xt_links_release_all(void)
{
    if (!xt.releases) return;
    const int64_t now = xt_now_ms();
    for (uint8_t i = 0; i < xt.links_max; ++i) (void)release_tick(&xt.releases[i], now, true);
    for (uint8_t i = 0; i < XIAOTAI_TRANSPORT_LINKS_MAX; ++i) (void)release_tick(&xt.refused[i], now, true);
}

void xt_links_stack_released(void)
{
    if (!xt.releases) return;
    xiaotai_critical_enter(&xt.lock);
    memset(xt.releases, 0, (size_t)xt.links_max * sizeof(*xt.releases));
    memset(xt.refused, 0, sizeof(xt.refused));
    xt.orphan_listeners = 0;
    xt.orphan_link_inputs = false;
    xiaotai_critical_exit(&xt.lock);
    reap_submitted(true);
    xiaotai_critical_enter(&xt.lock);
    if (s_release_watch.generation && s_release_watch.state == XIAOTAI_LINK_RELEASE_PENDING)
        s_release_watch.state = XIAOTAI_LINK_RELEASE_ABORTED;
    xiaotai_critical_exit(&xt.lock);
    xt_link_work();
}

void xt_links_count(uint8_t *requested, uint8_t *open, uint8_t *releasing)
{
    uint8_t r = 0, o = 0, l = 0;
    if (xt.slots && xt.sync_ready) {
        xiaotai_critical_enter(&xt.lock);
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            const uint8_t state = atomic_load(&xt.slots[i].state);
            if (state == XT_SLOT_REQUESTED) ++r;
            if (state == XT_SLOT_OPEN) ++o;
            if (xt.releases[i].used) ++l;
        }
        for (uint8_t i = 0; i < XIAOTAI_TRANSPORT_LINKS_MAX; ++i) l += xt.refused[i].used;
        if (xt.orphan_listeners) ++l;
        xiaotai_critical_exit(&xt.lock);
    }
    if (requested) *requested = r;
    if (open) *open = o;
    if (releasing) *releasing = l;
}

static void submit_one(void)
{
    if (xt_link_admissible() != XIAOTAI_OK) return;
    uint8_t releasing = 0;
    xt_links_count(NULL, NULL, &releasing);
    if (releasing && !xt.config.release_failures_tolerated) return;
    xt_slot_t *slot = NULL;
    uint32_t request = 0;
    const uint32_t waiting = xt.run.connect_wait_generation;
    xiaotai_critical_enter(&xt.lock);
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        if (!xt.slots[i].args) continue;
        const uint32_t generation = atomic_load(&xt.slots[i].generation);
        if (waiting && waiting != generation) continue;
        slot = &xt.slots[i];
        request = generation;
        break;
    }
    xiaotai_critical_exit(&xt.lock);
    if (!slot) return;
    xiaotai_err_t memory_error = XIAOTAI_OK;
    const xt_connect_memory_t memory = xt_connect_memory_lend(request, &memory_error);
    if (memory == XT_CONNECT_MEMORY_WAIT) return;
    xiaotai_critical_enter(&xt.lock);
    xt_connect_args_t *args = atomic_load(&slot->generation) == request ? slot->args : NULL;
    const xiaotai_link_kind_t kind = slot->kind;
    if (args) {
        if (memory == XT_CONNECT_MEMORY_FAILED) {
            const xiaotai_link_end_t reason = memory_error == XIAOTAI_ERR_NO_MEM
                ? XIAOTAI_LINK_END_NO_MEMORY : XIAOTAI_LINK_END_CONNECT_FAILED;
            args = xt_link_end_locked(slot, reason, memory_error, false, true);
        } else {
            slot->args = NULL;
            slot->request = request;
        }
    }
    xiaotai_critical_exit(&xt.lock);
    if (memory == XT_CONNECT_MEMORY_FAILED) {
        forget_args(args);
        xt_link_work();
        return;
    }
    if (!args) {
        xt_connect_memory_restore(request);
        return;
    }
    args->generation = request;
    args->next_submitted = s_submitted;
    s_submitted = args;
    int detail = 0;
    const xiaotai_err_t result = xiaotai_rtc_connect(kind, args->text, args->text + args->token_offset, request, &detail);
    if (result != XIAOTAI_OK) {
        xiaotai_rtc_on_connected(request, 0,
            result == XIAOTAI_ERR_NO_MEM ? XIAOTAI_LINK_END_NO_MEMORY : XIAOTAI_LINK_END_CONNECT_FAILED,
            detail ? detail : result);
    }
    xt_worker_kick();
}

void xt_links_tick(int64_t now_ms)
{
    if (!xt.slots) return;
    if (atomic_exchange(&xt.lifecycle_overflow, false)) xt_fault(XT_FAULT_OVERFLOW, XIAOTAI_ERR_NO_MEM);
    if (xt.run.stack == XT_STACK_UP && !xt.run.fault && !xt.run.stopped_pending) {
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            xt_slot_t *slot = &xt.slots[i];
            xiaotai_critical_enter(&xt.lock);
            const bool error = (atomic_load(&slot->flags) & XT_SLOT_ERROR) != 0;
            if (error) (void)xt_link_end_locked(slot, slot->end_reason, slot->end_detail, false, false);
            const int64_t deadline = atomic_load(&slot->state) == XT_SLOT_REQUESTED ? slot->opening.deadline : 0;
            const bool expired = deadline && now_ms >= deadline;
            xt_connect_args_t *args = expired ? xt_link_end_locked(slot, XIAOTAI_LINK_END_TIMEOUT, 0, false, false) : NULL;
            xiaotai_critical_exit(&xt.lock);
            forget_args(args);
            if (error) xt_link_work();
            if (expired) {
                xt_link_work();
                if (xt.config.connect_timeout_fault) {
                    xt_fault(XT_FAULT_CONNECT_TIMEOUT, XIAOTAI_ERR_TIMEOUT);
                    break;
                }
            } else if (deadline) xt_worker_wake_at(deadline);
        }
    }
    xt_connect_memory_abandon_unsubmitted();
    if (xt.run.stack == XT_STACK_UP && !xt.run.fault && !atomic_load(&xt.admission_closed)) {
        xt_receive_stop_deferred();
        bool good = true;
        for (uint8_t i = 0; good && i < xt.links_max; ++i) good = release_tick(&xt.releases[i], now_ms, false);
        for (uint8_t i = 0; good && i < XIAOTAI_TRANSPORT_LINKS_MAX; ++i)
            good = release_tick(&xt.refused[i], now_ms, false);
        if (good) submit_one();
    }
    const uint32_t completed = atomic_exchange(&xt.connect_result_generation, 0);
    if (completed) xt_connect_memory_restore(completed);
    if (atomic_exchange(&xt.links_dirty, false)) {
        xt_run_links_changed();
        xt_publish_links();
    }
    reap_submitted(false);
    if (xt.run.stack == XT_STACK_UP && !xt.run.fault && !atomic_load(&xt.admission_closed))
        finish_watched_retirement(now_ms);
    xt_link_events_tick();
}

bool xiaotai_link_stats(uint32_t generation, xiaotai_link_stats_t *stats)
{
    if (!stats || !xt.slots) return false;
    xiaotai_critical_enter(&xt.lock);
    const xt_slot_t *slot = xt_link_stats_locked(generation);
    if (slot) {
        *stats = slot->stats;
        const uint8_t state = atomic_load(&slot->state);
        stats->state = state == XT_SLOT_OPEN ? XIAOTAI_LINK_STATE_OPEN :
                       state == XT_SLOT_REQUESTED ? XIAOTAI_LINK_STATE_REQUESTED : XIAOTAI_LINK_STATE_NONE;
    }
    xiaotai_critical_exit(&xt.lock);
    return slot != NULL;
}
