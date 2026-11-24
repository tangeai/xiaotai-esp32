// SPDX-License-Identifier: MIT
#include "internal.h"
#include "xiaotai/net.h"

static xiaotai_err_t check_media(xiaotai_link_media_kind_t kind, const xiaotai_link_media_t *media)
{
    if (!media->send && !media->receive) return XIAOTAI_OK;
    if (kind == XIAOTAI_LINK_VIDEO && !xiaotai_rtc_caps()->video) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (media->send) {
        if (media->send_stream > XIAOTAI_LINK_STREAM_MAX ||
            media->peer_streams < XIAOTAI_LINK_PEER_STREAM_REFUSE || media->peer_streams > XIAOTAI_LINK_PEER_STREAM_FOLLOW ||
            (kind == XIAOTAI_LINK_VIDEO && media->peer_streams == XIAOTAI_LINK_PEER_STREAM_FOLLOW))
            return XIAOTAI_ERR_INVALID_ARG;
        if ((kind != XIAOTAI_LINK_VIDEO || media->send_format.codec != XIAOTAI_LINK_CODEC_NONE) &&
            !xiaotai_rtc_format_supported(kind, &media->send_format))
            return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    if (media->receive) {
        if (media->receive_stream > XIAOTAI_LINK_STREAM_MAX &&
            (media->receive_stream != XIAOTAI_LINK_STREAM_ANY || media->receive_request))
            return XIAOTAI_ERR_INVALID_ARG;
        for (size_t i = 0; i < XIAOTAI_LINK_RECEIVE_FORMATS; ++i) {
            const xiaotai_link_format_t *format = &media->receive_formats[i];
            if (format->codec != XIAOTAI_LINK_CODEC_NONE && !xiaotai_rtc_format_supported(kind, format))
                return XIAOTAI_ERR_NOT_SUPPORTED;
        }
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xt_link_profile_check(const xiaotai_link_profile_t *profile)
{
    if (profile->command_max_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES ||
        profile->command_pool_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES)
        return XIAOTAI_ERR_INVALID_SIZE;
    if (profile->command_pool_bytes &&
        profile->command_pool_bytes < (profile->command_max_bytes ?
            profile->command_max_bytes : XIAOTAI_LINK_COMMAND_DEFAULT_BYTES))
        return XIAOTAI_ERR_INVALID_ARG;
    if ((profile->audio.send && profile->video.send && profile->audio.send_stream == profile->video.send_stream) ||
        (profile->audio.receive && profile->video.receive &&
         profile->audio.receive_stream != XIAOTAI_LINK_STREAM_ANY &&
         profile->audio.receive_stream == profile->video.receive_stream))
        return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t audio = check_media(XIAOTAI_LINK_AUDIO, &profile->audio);
    return audio == XIAOTAI_OK ? check_media(XIAOTAI_LINK_VIDEO, &profile->video) : audio;
}

void xt_listeners_free(void)
{
    xt.inbound_blocks = 0;
    xt.open_blocks = 0;
    xt.paused_open = 0;
    if (!xt.listeners) return;
    xt_account(-(int64_t)(XIAOTAI_LINK_LISTENERS_MAX * sizeof(*xt.listeners)));
    xiaotai_mem_free(xt.listeners);
    xt.listeners = NULL;
}

xiaotai_err_t xiaotai_link_listen(const xiaotai_link_listener_t *listener, uint32_t *listener_id)
{
    if (listener_id) *listener_id = 0;
    if (!listener || !listener_id || !listener->name || !listener->name[0]) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&xt.initialized)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t result = xt_link_profile_check(&listener->profile);
    if (result != XIAOTAI_OK) return result;
    if (xt.listener_count >= XIAOTAI_LINK_LISTENERS_MAX) return XIAOTAI_ERR_NO_MEM;
    if (atomic_load(&xt.provisioning) || xiaotai_net_provisioning_boot()) {
        *listener_id = ++xt.listener_count;
        return XIAOTAI_OK;
    }
    if (!xt.listeners) {
        xt_listener_t *table = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_LINK_LISTENERS_MAX, sizeof(*table));
        if (!table) return XIAOTAI_ERR_NO_MEM;
        xt_account((int64_t)(XIAOTAI_LINK_LISTENERS_MAX * sizeof(*table)));
        xiaotai_critical_enter(&xt.lock);
        xt.listeners = table;
        xiaotai_critical_exit(&xt.lock);
    }
    xiaotai_critical_enter(&xt.lock);
    xt.listeners[xt.listener_count++].listener = *listener;
    *listener_id = xt.listener_count;
    xiaotai_critical_exit(&xt.lock);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_listen_set(uint32_t listener_id, xiaotai_link_listen_t mode)
{
    if (!listener_id || mode < XIAOTAI_LINK_LISTEN_OFF || mode > XIAOTAI_LINK_LISTEN_OPEN ||
        !xt.sync_ready || listener_id > xt.listener_count) return XIAOTAI_ERR_INVALID_ARG;
    if (!xt.listeners) return XIAOTAI_OK;
    bool revoked = false;
    xiaotai_critical_enter(&xt.lock);
    xt.listeners[listener_id - 1U].mode = mode;
    if (mode == XIAOTAI_LINK_LISTEN_OFF && xt.slots) {
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            xt_slot_t *slot = &xt.slots[i];
            const uint32_t flags = atomic_load(&slot->flags);
            if (slot->listener_id == listener_id && (flags & XT_SLOT_OPENED)) {
                if (flags & XT_SLOT_REPORT_RETIRE_OPEN)
                    xt_diag_link("inbound_open_retired", atomic_load(&slot->generation), 0, false);
                (void)xt_link_end_locked(slot, XIAOTAI_LINK_END_STOPPED, 0, true, false);
                revoked = true;
            }
        }
    }
    xiaotai_critical_exit(&xt.lock);
    if (revoked) xt_link_work();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_listener_block_inbound(uint32_t listener_id, bool blocked)
{
    if (!listener_id || !xt.sync_ready || listener_id > xt.listener_count) return XIAOTAI_ERR_INVALID_ARG;
    if (!xt.listeners) return XIAOTAI_OK;
    const uint16_t mask = (uint16_t)(1U << (listener_id - 1U));
    xiaotai_critical_enter(&xt.lock);
    if (blocked) xt.inbound_blocks |= mask;
    else xt.inbound_blocks &= (uint16_t)~mask;
    xiaotai_critical_exit(&xt.lock);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_link_listener_open_gate(uint32_t listener_id, bool acquire)
{
    if (!listener_id || !xt.sync_ready || listener_id > xt.listener_count) return XIAOTAI_ERR_INVALID_ARG;
    const uint16_t mask = (uint16_t)(1U << (listener_id - 1U));
    xiaotai_err_t err = XIAOTAI_OK;
    xiaotai_critical_enter(&xt.lock);
    if (xt.listeners && acquire) {
        if (xt.open_blocks & mask) err = XIAOTAI_ERR_BUSY;
        for (uint8_t i = 0; err == XIAOTAI_OK && xt.slots && i < xt.links_max; ++i) {
            const xt_slot_t *slot = &xt.slots[i];
            if (slot->listener_id == listener_id && slot->conn && atomic_load(&slot->state) == XT_SLOT_OPEN)
                err = XIAOTAI_ERR_BUSY;
        }
        if (err == XIAOTAI_OK) xt.open_blocks |= mask;
    } else if (!acquire) {
        xt.open_blocks &= (uint16_t)~mask;
    }
    xiaotai_critical_exit(&xt.lock);
    return err;
}

xiaotai_err_t xiaotai_link_listener_pause_open(uint32_t listener_id, bool paused)
{
    if (!listener_id || !xt.sync_ready || listener_id > xt.listener_count) return XIAOTAI_ERR_INVALID_ARG;
    if (!xt.listeners) return XIAOTAI_OK;
    const uint16_t mask = (uint16_t)(1U << (listener_id - 1U));
    xiaotai_critical_enter(&xt.lock);
    if (paused) xt.paused_open |= mask;
    else xt.paused_open &= (uint16_t)~mask;
    xiaotai_critical_exit(&xt.lock);
    return XIAOTAI_OK;
}

bool xiaotai_link_listener_retiring(uint32_t listener_id)
{
    if (!listener_id || listener_id > xt.listener_count || !xt.releases) return false;
    xiaotai_critical_enter(&xt.lock);
    bool retiring = (xt.orphan_listeners & (1U << (listener_id - 1U))) != 0;
    for (uint8_t i = 0; !retiring && i < xt.links_max; ++i)
        retiring = xt.releases[i].used && xt.releases[i].listener_id == listener_id;
    xiaotai_critical_exit(&xt.lock);
    return retiring;
}

bool xt_inbound_open_allowed_locked(const xt_slot_t *slot)
{
    if (!slot->listener_id || !slot->guarded_open) return true;
    const xiaotai_link_listener_t *listener = &xt.listeners[slot->listener_id - 1U].listener;
    return listener->admit_open(listener->handlers.ctx);
}

void xiaotai_rtc_on_accepted_peer(xiaotai_rtc_conn_t conn, xiaotai_rtc_peer_tag_fn tag)
{
    if (!conn || !xt.slots) return;
    uint32_t to_tag = 0;
    xiaotai_critical_enter(&xt.lock);
    uint32_t once = 0, open = 0;
    for (uint32_t i = 0; !xt.inbound_blocks && i < xt.listener_count && xt.listeners; ++i) {
        if (xt.listeners[i].mode == XIAOTAI_LINK_LISTEN_ONCE) {
            once = i + 1U;
            break;
        }
        if (!open && xt.listeners[i].mode == XIAOTAI_LINK_LISTEN_OPEN) open = i + 1U;
    }
    const uint32_t id = once ? once : open;
    const bool blocked = !once && open && ((xt.open_blocks | xt.paused_open) & (1U << (open - 1U)));
    xt_listener_t *entry = id && !blocked ? &xt.listeners[id - 1U] : NULL;
    xt_slot_t *slot = entry && xt_link_admissible() == XIAOTAI_OK && xt_link_physical_room_locked()
        ? xt_link_free_locked() : NULL;
    /* This read is the guarded OPEN admission point. No generation has been
     * assigned yet; a refusal does not fall through to a different listener. */
    if (slot && !once && entry->listener.admit_open && !entry->listener.admit_open(entry->listener.handlers.ctx))
        slot = NULL;
    if (slot) {
        xt_link_open_inbound_locked(slot, conn, id, entry);
        if (!once && !entry->listener.admit_open && xt.config.report_retired_open)
            atomic_fetch_or(&slot->flags, XT_SLOT_REPORT_RETIRE_OPEN);
        if (tag && slot->profile.track_peer_retirement) to_tag = atomic_load(&slot->generation);
        atomic_fetch_add(&xt.counters.inbound_admitted, 1);
    } else {
        xt_link_refuse_locked(conn);
        atomic_fetch_add(&xt.counters.inbound_refused, 1);
        xt_diag_link("inbound_refused", 0, 0, true);
    }
    if (once && (slot || xt.config.once_consumed_on_refusal)) entry->mode = XIAOTAI_LINK_LISTEN_OFF;
    xiaotai_critical_exit(&xt.lock);
    if (to_tag) xt_link_tag_peer_retirement(conn, to_tag, tag);
    xt_link_work();
}

void xiaotai_rtc_on_accepted(xiaotai_rtc_conn_t conn) { xiaotai_rtc_on_accepted_peer(conn, NULL); }
