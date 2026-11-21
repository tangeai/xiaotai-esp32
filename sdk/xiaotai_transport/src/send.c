// SPDX-License-Identifier: MIT
#include <limits.h>

#include "internal.h"
#include "xiaotai/media_format.h"

static xiaotai_media_sink_t audio_sink;

static void wake_audio_worker(void)
{
    if (!atomic_exchange(&xt.audio_wake_posted, true)) xt_worker_kick();
}

const xiaotai_media_uplink_executor_t xt_audio_executor = {
    .sink = &audio_sink, .wake = wake_audio_worker, .is_current = xt_on_worker,
};

void xt_send_sync_gate_locked(xt_slot_t *slot, xiaotai_link_media_kind_t kind)
{
    const bool audio = kind == XIAOTAI_LINK_AUDIO;
    const bool allowed = (audio ? slot->stats.audio_permitted || slot->stats.audio_subscribed :
                                  slot->stats.video_permitted || slot->stats.video_subscribed);
    atomic_store(&slot->send_ready[kind], xt_link_open_locked(slot) && xt_profile_media(slot, kind)->send && allowed
        ? atomic_load(&slot->generation) : 0);
}

void xt_send_peer_locked(xt_slot_t *slot, xiaotai_link_media_kind_t kind, uint8_t stream, bool on)
{
    const xiaotai_link_media_t *media = xt_profile_media(slot, kind);
    if (kind == XIAOTAI_LINK_AUDIO) {
        if (on && (stream == media->send_stream || media->peer_streams == XIAOTAI_LINK_PEER_STREAM_FOLLOW)) {
            slot->stats.audio_send_stream = stream;
            slot->stats.audio_subscribed = true;
        } else if (!on && stream == slot->stats.audio_send_stream) {
            slot->stats.audio_subscribed = false;
            if (xt.config.audio_unsubscribe_revokes_permit) slot->stats.audio_permitted = false;
        }
    } else if (stream == media->send_stream) {
        slot->stats.video_subscribed = on;
        if (on) {
            slot->video_needs_key = true;
            ++slot->video_key_epoch;
            atomic_store(&slot->key_frame_for, atomic_load(&slot->generation));
        }
    }
    xt_send_sync_gate_locked(slot, kind);
}

bool xiaotai_link_can_send(uint32_t generation, xiaotai_link_media_kind_t kind)
{
    if (!generation || !xt_media_kind(kind) || !xt.slots || atomic_load(&xt.admission_closed)) return false;
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        if (atomic_load(&xt.slots[i].send_ready[kind]) == generation) return true;
    }
    return false;
}

xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool allowed)
{
    if (!xt_media_kind(kind)) return XIAOTAI_ERR_INVALID_ARG;
    if (!xt.sync_ready) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    if (!slot) atomic_fetch_add(&xt.counters.stale_calls, 1);
    else if (xt_link_open_locked(slot)) {
        if (!xt_profile_media(slot, kind)->send) result = XIAOTAI_ERR_NOT_SUPPORTED;
        else {
            if (kind == XIAOTAI_LINK_AUDIO) slot->stats.audio_permitted = allowed;
            else slot->stats.video_permitted = allowed;
            xt_send_sync_gate_locked(slot, kind);
            result = XIAOTAI_OK;
        }
    }
    xiaotai_critical_exit(&xt.lock);
    return result;
}

static void note_dwell(int64_t began)
{
    const int64_t delta = xiaotai_time_us() - began;
    const uint32_t value = delta <= 0 ? 0 : delta >= UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
    uint32_t prior = atomic_load(&xt.counters.send_call_max_us);
    while (value > prior && !atomic_compare_exchange_weak(&xt.counters.send_call_max_us, &prior, value)) {}
}

static bool g711_size(const xiaotai_link_format_t *format, size_t size)
{
    if (format->codec != XIAOTAI_CODEC_G711A && format->codec != XIAOTAI_CODEC_G711U) return true;
    const uint64_t samples = (uint64_t)format->rate_hz * format->channels;
    return samples % 50 == 0 && size == samples / 50;
}

static xiaotai_err_t send_frame(uint32_t generation, xiaotai_link_frame_t frame, const uint8_t *data, size_t size,
                                uint32_t backlog_limit)
{
    if (!size) return XIAOTAI_ERR_INVALID_SIZE;
    if (!data) return XIAOTAI_ERR_INVALID_ARG;
    if (!xt.sync_ready) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_link_media_kind_t kind = frame.kind;
    const bool audio = kind == XIAOTAI_LINK_AUDIO;
    uint32_t key_epoch = 0;
    xiaotai_rtc_conn_t conn = 0;
    xiaotai_err_t result = XIAOTAI_OK;

    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    if (!slot) {
        atomic_fetch_add(&xt.counters.stale_calls, 1);
        result = XIAOTAI_ERR_INVALID_STATE;
    } else if (atomic_load(&slot->send_ready[kind]) != generation || !xt_link_open_locked(slot)) {
        ++xt_media_stats(slot, kind)->send_gated;
        result = XIAOTAI_ERR_INVALID_STATE;
    } else {
        const xiaotai_link_media_t *media = xt_profile_media(slot, kind);
        frame.stream = audio ? slot->stats.audio_send_stream : media->send_stream;
        if (audio) frame.format = media->send_format;
        if (audio && !g711_size(&frame.format, size)) result = XIAOTAI_ERR_INVALID_SIZE;
        else if (!audio && media->send_format.codec != XIAOTAI_LINK_CODEC_NONE &&
                 media->send_format.codec != frame.format.codec) result = XIAOTAI_ERR_NOT_SUPPORTED;
        else if (!audio && slot->video_needs_key && !frame.key_frame) {
            ++slot->stats.video.send_gated;
            result = XIAOTAI_ERR_INVALID_STATE;
        } else if (slot->tx_busy[kind] || (!xt.external && xt.sending) || !xt_link_pin_locked(slot)) {
            ++xt_media_stats(slot, kind)->send_congested;
            result = XIAOTAI_ERR_BUSY;
        } else {
            ++xt.sending;
            slot->tx_busy[kind] = true;
            key_epoch = slot->video_key_epoch;
            conn = slot->conn;
        }
    }
    xiaotai_critical_exit(&xt.lock);
    if (!conn) {
        atomic_fetch_add(&xt.counters.media_tx_dropped, 1U);
        return result;
    }

    int detail = 0;
    const bool unsupported = !audio && !xiaotai_rtc_format_supported(kind, &frame.format);
    if (unsupported) result = XIAOTAI_ERR_NOT_SUPPORTED;
    else if (backlog_limit && xiaotai_rtc_backlog(conn) >= backlog_limit) result = XIAOTAI_ERR_BUSY;
    else {
        const int64_t began = xiaotai_time_us();
        result = xiaotai_rtc_send(conn, &frame, data, size, &detail);
        note_dwell(began);
    }

    bool fault = false;
    xiaotai_critical_enter(&xt.lock);
    --xt.sending;
    slot = xt_link_stats_locked(generation);
    if (slot) {
        slot->tx_busy[kind] = false;
        xiaotai_link_media_stats_t *stats = xt_media_stats(slot, kind);
        if (result == XIAOTAI_OK) {
            ++stats->sent;
            if (audio) slot->audio_failures = 0;
            else if (frame.key_frame && slot->video_key_epoch == key_epoch) slot->video_needs_key = false;
        } else if (result == XIAOTAI_ERR_BUSY || !unsupported) {
            if (result == XIAOTAI_ERR_BUSY) ++stats->send_congested;
            else ++stats->send_failed;
            if (!audio) {
                slot->video_needs_key = true;
                ++slot->video_key_epoch;
                if (xt_link_open_locked(slot)) atomic_store(&slot->key_frame_for, generation);
            } else if (result != XIAOTAI_ERR_BUSY && xt.config.audio_failures_end && xt_link_open_locked(slot) &&
                       ++slot->audio_failures >= xt.config.audio_failures_end) {
                xt_queue_link_error_locked(slot, XIAOTAI_LINK_END_NETWORK, detail ? detail : result);
                fault = true;
            }
        }
    }
    xiaotai_critical_exit(&xt.lock);
    atomic_fetch_add(result == XIAOTAI_OK ? &xt.counters.media_tx : &xt.counters.media_tx_dropped, 1U);
    xt_link_unpin(generation);
    if (fault) xt_worker_kick();
    return result;
}

xiaotai_err_t xiaotai_link_send_audio(uint32_t generation, uint32_t timestamp_ms, const uint8_t *data, size_t size)
{
    return send_frame(generation, (xiaotai_link_frame_t){.kind = XIAOTAI_LINK_AUDIO, .timestamp_ms = timestamp_ms},
                      data, size, xt.config.audio_backlog_limit_bytes);
}

xiaotai_err_t xiaotai_link_send_video(uint32_t generation, const xiaotai_link_video_frame_t *frame)
{
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    return send_frame(generation, (xiaotai_link_frame_t){
        .kind = XIAOTAI_LINK_VIDEO, .format.codec = frame->codec, .key_frame = frame->key_frame,
        .timestamp_ms = frame->timestamp_ms}, frame->data, frame->size, frame->backlog_limit_bytes);
}

size_t xiaotai_link_backlog(uint32_t generation)
{
    if (!xt.sync_ready) return 0;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    const xiaotai_rtc_conn_t conn = xt_link_pin_locked(slot) ? slot->conn : 0;
    xiaotai_critical_exit(&xt.lock);
    if (!conn) {
        if (!slot) atomic_fetch_add(&xt.counters.stale_calls, 1);
        return 0;
    }
    const size_t backlog = xiaotai_rtc_backlog(conn);
    xt_link_unpin(generation);
    return backlog;
}

static bool sink_ready(void *ctx, uint32_t generation)
{
    (void)ctx;
    return xiaotai_link_can_send(generation, XIAOTAI_LINK_AUDIO);
}

static xiaotai_err_t sink_send(void *ctx, uint32_t generation, uint32_t timestamp_ms, const uint8_t *data, size_t size)
{
    (void)ctx;
    return xiaotai_link_send_audio(generation, timestamp_ms, data, size);
}

void xt_audio_sink_init(void)
{
    const uint32_t stack = xiaotai_rtc_caps()->send_stack_bytes;
    audio_sink = (xiaotai_media_sink_t){.ready = sink_ready, .send = sink_send,
        .send_stack_bytes = stack ? stack : XIAOTAI_RTC_SEND_STACK_UNMEASURED};
}

const xiaotai_media_sink_t *xiaotai_link_audio_sink(void) { return &audio_sink; }

void xiaotai_rtc_on_key_frame_request(xiaotai_rtc_conn_t conn, uint8_t stream)
{
    if (!xt.slots) return;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_conn_locked(conn);
    if (xt_link_open_locked(slot) && slot->profile.video.send && slot->profile.video.send_stream == stream) {
        slot->video_needs_key = true;
        ++slot->video_key_epoch;
        atomic_store(&slot->key_frame_for, atomic_load(&slot->generation));
    }
    xiaotai_critical_exit(&xt.lock);
}

bool xiaotai_link_take_key_frame_request(uint32_t generation)
{
    if (generation && xt.slots && !atomic_load(&xt.admission_closed)) {
        for (uint8_t i = 0; i < xt.links_max; ++i) {
            uint32_t expected = generation;
            if (atomic_compare_exchange_strong(&xt.slots[i].key_frame_for, &expected, 0)) return true;
        }
    }
    return false;
}

void xiaotai_rtc_on_video_bitrate(xiaotai_rtc_conn_t conn, uint8_t stream, uint32_t bps)
{
    if (!xt.slots || !bps) return;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_conn_locked(conn);
    if (xt_link_open_locked(slot) && slot->bitrate_enabled && slot->profile.video.send_stream == stream) {
        atomic_fetch_add(&slot->bitrate_sequence, 1);
        atomic_store(&slot->bitrate_bps, bps);
        atomic_fetch_add(&slot->bitrate_sequence, 1);
    }
    xiaotai_critical_exit(&xt.lock);
}

bool xiaotai_link_take_video_bitrate(uint32_t generation, uint32_t *bps)
{
    if (!generation || !bps || !xt.slots || atomic_load(&xt.admission_closed)) return false;
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        xt_slot_t *slot = &xt.slots[i];
        const uint32_t sequence = atomic_load(&slot->bitrate_sequence);
        if (sequence & 1U || atomic_load(&slot->bitrate_for) != generation) continue;
        const uint32_t value = atomic_load(&slot->bitrate_bps);
        uint32_t consumed = atomic_load(&slot->bitrate_consumed);
        if (consumed == sequence || atomic_load(&slot->bitrate_sequence) != sequence ||
            atomic_load(&slot->bitrate_for) != generation) return false;
        if (!atomic_compare_exchange_strong(&slot->bitrate_consumed, &consumed, sequence)) return false;
        if (atomic_load(&slot->bitrate_for) != generation) return false;
        *bps = value;
        return true;
    }
    return false;
}
