// SPDX-License-Identifier: MIT
#include "internal.h"

bool xt_media_kind(xiaotai_link_media_kind_t kind)
{
    return kind == XIAOTAI_LINK_AUDIO || kind == XIAOTAI_LINK_VIDEO;
}

xiaotai_link_media_stats_t *xt_media_stats(xt_slot_t *slot, xiaotai_link_media_kind_t kind)
{
    return kind == XIAOTAI_LINK_AUDIO ? &slot->stats.audio : &slot->stats.video;
}

const xiaotai_link_media_t *xt_profile_media(const xt_slot_t *slot, xiaotai_link_media_kind_t kind)
{
    return kind == XIAOTAI_LINK_AUDIO ? &slot->profile.audio : &slot->profile.video;
}

bool xt_control_begin(uint32_t generation)
{
    if (xiaotai_mutex_lock(&xt.control, XIAOTAI_LINK_CONTROL_WAIT_MS)) return true;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_stats_locked(generation);
    if (slot) ++slot->stats.control_busy;
    xiaotai_critical_exit(&xt.lock);
    return false;
}

void xt_control_end(void) { xiaotai_mutex_unlock(&xt.control); }

static xiaotai_err_t send_command(uint32_t generation, xiaotai_rtc_conn_t conn, uint32_t word,
                                 const void *data, size_t size)
{
    const xiaotai_err_t result = xiaotai_rtc_command(conn, word, data, size);
    xiaotai_critical_enter(&xt.lock);
    xt_diag_command_locked(XT_DIAG_TX, generation, word, (uint32_t)size, result);
    xt_slot_t *slot = xt_link_stats_locked(generation);
    if (slot) {
        if (result == XIAOTAI_OK) ++slot->stats.commands_sent;
        else ++slot->stats.commands_send_failed;
    }
    xiaotai_critical_exit(&xt.lock);
    xt_link_unpin(generation);
    return result;
}

xiaotai_err_t xiaotai_link_command(uint32_t generation, uint32_t word, const void *data, size_t size)
{
    if (size && !data) return XIAOTAI_ERR_INVALID_ARG;
    if (size > XIAOTAI_LINK_COMMAND_LIMIT_BYTES) return XIAOTAI_ERR_INVALID_SIZE;
    if (!xt.sync_ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!xt_control_begin(generation)) return XIAOTAI_ERR_BUSY;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    const bool pinned = xt_link_pin_locked(slot);
    const xiaotai_rtc_conn_t conn = pinned ? slot->conn : 0;
    xiaotai_critical_exit(&xt.lock);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (pinned) result = send_command(generation, conn, word, data, size);
    else if (!slot) atomic_fetch_add(&xt.counters.stale_calls, 1);
    xt_control_end();
    return result;
}

xiaotai_err_t xiaotai_link_finish_opening(uint32_t generation, bool *opened, bool send,
                                        uint32_t word, const void *data, size_t size)
{
    if (!opened) return XIAOTAI_ERR_INVALID_ARG;
    *opened = false;
    if (send && size && !data) return XIAOTAI_ERR_INVALID_ARG;
    if (send && size > XIAOTAI_LINK_COMMAND_LIMIT_BYTES) return XIAOTAI_ERR_INVALID_SIZE;
    if (!xt.sync_ready || !generation) return XIAOTAI_ERR_INVALID_STATE;
    const bool locked = send && xt_control_begin(generation);
    xiaotai_rtc_conn_t conn = 0;
    xiaotai_err_t result = xt_link_finish_opening(generation, opened, locked, &conn);
    if (conn) result = send_command(generation, conn, word, data, size);
    else if (result == XIAOTAI_OK && *opened && send && !locked) result = XIAOTAI_ERR_BUSY;
    if (locked) xt_control_end();
    return result;
}

xiaotai_err_t xiaotai_link_request_key_frame(uint32_t generation)
{
    if (!xt.sync_ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!xt_control_begin(generation)) return XIAOTAI_ERR_BUSY;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    const bool open = xt_link_open_locked(slot);
    const bool supported = open && slot->profile.video.receive &&
                           slot->profile.video.receive_stream <= XIAOTAI_LINK_STREAM_MAX;
    const bool pinned = supported && xt_link_pin_locked(slot);
    const xiaotai_rtc_conn_t conn = pinned ? slot->conn : 0;
    const uint8_t stream = pinned ? slot->profile.video.receive_stream : 0;
    xiaotai_critical_exit(&xt.lock);
    if (!slot) atomic_fetch_add(&xt.counters.stale_calls, 1);
    xiaotai_err_t result = !open ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_NOT_SUPPORTED;
    if (pinned) {
        result = xiaotai_rtc_request_key_frame(conn, stream);
        xt_link_unpin(generation);
    }
    xt_control_end();
    return result;
}

xiaotai_err_t xiaotai_link_set_video_bitrate(uint32_t generation, uint32_t min_bps, uint32_t start_bps,
                                             uint32_t max_bps)
{
    if (!min_bps || min_bps >= start_bps || start_bps >= max_bps) return XIAOTAI_ERR_INVALID_ARG;
    if (!xt.sync_ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!xt_control_begin(generation)) return XIAOTAI_ERR_BUSY;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    const bool open = xt_link_open_locked(slot);
    const bool pinned = open && slot->profile.video.send && xt_link_pin_locked(slot);
    const xiaotai_rtc_conn_t conn = pinned ? slot->conn : 0;
    const uint8_t stream = pinned ? slot->profile.video.send_stream : 0;
    if (pinned) {
        slot->bitrate_enabled = true;
        atomic_store(&slot->bitrate_for, 0);
        atomic_store(&slot->bitrate_consumed, atomic_load(&slot->bitrate_sequence));
    }
    xiaotai_critical_exit(&xt.lock);
    if (!slot) atomic_fetch_add(&xt.counters.stale_calls, 1);
    xiaotai_err_t result = !open ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_NOT_SUPPORTED;
    if (pinned) {
        result = xiaotai_rtc_set_video_bitrate(conn, stream, min_bps, start_bps, max_bps);
        xiaotai_critical_enter(&xt.lock);
        slot = xt_link_generation_locked(generation);
        if (slot) {
            if (result == XIAOTAI_OK) atomic_store(&slot->bitrate_for, generation);
            else {
                slot->bitrate_enabled = false;
                atomic_store(&slot->bitrate_consumed, atomic_load(&slot->bitrate_sequence));
            }
        }
        xiaotai_critical_exit(&xt.lock);
        xt_link_unpin(generation);
    }
    xt_control_end();
    return result;
}
