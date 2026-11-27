// SPDX-License-Identifier: MIT
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/media.h"
#include "xiaotai/media_rx_ingress.h"
#include "xiaotai/net.h"

static xiaotai_eventlog_topic_t *accepted_topic, *rejected_topic;

void xt_receive_topics_init(void)
{
    static const char *const fields[] = {"kind", "gen", "stream", "codec", "rate", "len", "n", "why"};
    static const xiaotai_eventlog_topic_desc_t accepted = {
        .source = "link", .name = "downlink.accepted", .level = XIAOTAI_EVENTLOG_INFO, .fields = fields, .count = 7};
    static const xiaotai_eventlog_topic_desc_t rejected = {
        .source = "link", .name = "downlink.rejected", .level = XIAOTAI_EVENTLOG_WARN, .fields = fields, .count = 8};
    (void)xiaotai_eventlog_topic_register(&accepted, &accepted_topic);
    (void)xiaotai_eventlog_topic_register(&rejected, &rejected_topic);
}

enum {RX_CLOSED = 1, RX_FILTERED, RX_REFUSED, RX_OVERLAPPED};

static void note_frame(xiaotai_link_kind_t kind, uint32_t gen, const xiaotai_link_frame_t *frame, size_t length,
                        uint32_t count, uint32_t why)
{
    if (count % 100U && (why ? count > 3U : count != 1U)) return;
    const uint32_t values[] = {(uint32_t)kind, gen, frame->stream, frame->format.codec,
        frame->format.rate_hz, (uint32_t)length, count, why};
    xiaotai_eventlog_post(why ? rejected_topic : accepted_topic, values);
}

static bool forward_audio(uint32_t generation, const xiaotai_link_frame_t *frame,
                          const uint8_t *data, size_t size, uint32_t epoch)
{
    (void)epoch;
    const xiaotai_media_packet_t packet = {.encoding = frame->format.codec,
        .rate_hz = frame->format.rate_hz, .timestamp_ms = frame->timestamp_ms, .stream = frame->stream,
        .data = data, .size = size};
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    if (epoch) return xiaotai_media_ingress_selected(generation, &packet, epoch);
#endif
    return xiaotai_media_push(generation, &packet);
}

static bool default_audio(void *ctx, uint32_t generation, const xiaotai_link_frame_t *frame,
                          const uint8_t *data, size_t size)
{
    (void)ctx;
    return forward_audio(generation, frame, data, size, 0);
}

#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
static bool selected_default(const xiaotai_link_media_t *media, const xt_receiver_t *rx)
{
    return rx->fn == default_audio && !media->accept_peer_stream &&
           media->receive_stream != XIAOTAI_LINK_STREAM_ANY;
}

static bool fixed_alaw(const xiaotai_link_format_t *format)
{
    return format->codec == XIAOTAI_LINK_CODEC_G711A && format->rate_hz == 8000U && format->channels == 1U;
}
#endif

bool xt_receive_busy(uint32_t generation)
{
    return generation && (atomic_load(&xt.receiving[XIAOTAI_LINK_AUDIO]) == generation ||
                          atomic_load(&xt.receiving[XIAOTAI_LINK_VIDEO]) == generation);
}

void xt_receive_wait(uint32_t generation, xiaotai_link_media_kind_t kind)
{
    if (!generation) return;
    while (atomic_load(&xt.receiving[kind]) == generation) xiaotai_delay_ms(1);
}

xiaotai_err_t xiaotai_link_receive(uint32_t generation, xiaotai_link_media_kind_t kind,
                                   xiaotai_link_receive_fn_t receiver, void *ctx)
{
    if (!xt_media_kind(kind) || (!receiver && kind == XIAOTAI_LINK_VIDEO)) return XIAOTAI_ERR_INVALID_ARG;
    if (!xt.sync_ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!xt_control_begin(generation)) return XIAOTAI_ERR_BUSY;
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    xiaotai_rtc_conn_t conn = 0;
    uint32_t revision = 0;
    uint8_t stream = 0;
    bool install = false, pinned = false;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    if (!slot) atomic_fetch_add(&xt.counters.stale_calls, 1);
    if (xt_link_open_locked(slot)) {
        const xiaotai_link_media_t *media = xt_profile_media(slot, kind);
        xt_receiver_t *rx = &slot->rx[kind];
        if (!media->receive) result = XIAOTAI_ERR_NOT_SUPPORTED;
        else if (media->receive_request && !rx->subscribed && !(pinned = xt_link_pin_locked(slot)))
            result = XIAOTAI_ERR_BUSY;
        else {
            if (pinned) conn = slot->conn;
            stream = media->receive_stream;
            revision = ++rx->revision;
            rx->on = false;
            rx->stop_pending = false;
            install = true;
        }
    }
    xiaotai_critical_exit(&xt.lock);
    if (install) {
        xt_receive_wait(generation, kind);
        xiaotai_critical_enter(&xt.lock);
        slot = xt_link_generation_locked(generation);
        if (xt_link_open_locked(slot) && slot->rx[kind].revision == revision) {
            xt_receiver_t *rx = &slot->rx[kind];
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
            if (receiver && kind == XIAOTAI_LINK_AUDIO && selected_default(xt_profile_media(slot, kind), rx))
                xiaotai_media_ingress_break(generation);
#endif
            rx->on = true;
            rx->fn = receiver ? receiver : default_audio;
            rx->ctx = ctx;
            result = XIAOTAI_OK;
        }
        xiaotai_critical_exit(&xt.lock);
    }
    if (pinned && result == XIAOTAI_OK) {
        result = xiaotai_rtc_subscribe(conn, kind, stream, true);
        if (result != XIAOTAI_OK) {
            xiaotai_critical_enter(&xt.lock);
            slot = xt_link_generation_locked(generation);
            if (slot && slot->rx[kind].revision == revision) {
                slot->rx[kind].on = false;
                slot->rx[kind].fn = NULL;
                slot->rx[kind].ctx = NULL;
            }
            xiaotai_critical_exit(&xt.lock);
            xt_receive_wait(generation, kind);
        } else {
            xiaotai_critical_enter(&xt.lock);
            slot = xt_link_generation_locked(generation);
            if (slot && slot->conn == conn) {
                xt_receiver_t *rx = &slot->rx[kind];
                /* Keep the installed backend subscription visible to stop even
                 * when the receiver was superseded while the call was unlocked. */
                rx->subscribed = true;
                if (!rx->on) rx->stop_pending = true;
                if (!xt_link_open_locked(slot) || rx->revision != revision || !rx->on)
                    result = XIAOTAI_ERR_INVALID_STATE;
            } else {
                result = XIAOTAI_ERR_INVALID_STATE;
            }
            xiaotai_critical_exit(&xt.lock);
            xt_worker_kick();
        }
    }
    if (pinned) xt_link_unpin(generation);
    xt_control_end();
    return result;
}

/* The control lock is held. A stop may be superseded by a later start. */
static void stop_request(uint32_t generation, xiaotai_link_media_kind_t kind)
{
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    const bool pinned = xt_link_open_locked(slot) && slot->rx[kind].stop_pending && !slot->rx[kind].on &&
                        slot->rx[kind].subscribed && xt_link_pin_locked(slot);
    const xiaotai_rtc_conn_t conn = pinned ? slot->conn : 0;
    const uint8_t stream = pinned ? xt_profile_media(slot, kind)->receive_stream : 0;
    if (pinned) slot->rx[kind].stop_pending = false;
    xiaotai_critical_exit(&xt.lock);
    if (pinned) {
        if (xiaotai_rtc_subscribe(conn, kind, stream, false) == XIAOTAI_OK) {
            xiaotai_critical_enter(&xt.lock);
            slot = xt_link_generation_locked(generation);
            if (slot) slot->rx[kind].subscribed = false;
            xiaotai_critical_exit(&xt.lock);
        }
        xt_link_unpin(generation);
    }
}

void xiaotai_link_receive_stop(uint32_t generation, xiaotai_link_media_kind_t kind)
{
    if (!xt_media_kind(kind) || !xt.sync_ready) return;
    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_generation_locked(generation);
    if (slot) {
        xt_receiver_t *rx = &slot->rx[kind];
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
        if (kind == XIAOTAI_LINK_AUDIO && selected_default(xt_profile_media(slot, kind), rx))
            xiaotai_media_ingress_break(generation);
#endif
        ++rx->revision;
        rx->on = false;
        rx->fn = NULL;
        rx->ctx = NULL;
        rx->stop_pending = rx->subscribed;
    } else atomic_fetch_add(&xt.counters.stale_calls, 1);
    xiaotai_critical_exit(&xt.lock);
    xt_receive_wait(generation, kind);
    if (!slot) return;
    if (xt_control_begin(generation)) {
        stop_request(generation, kind);
        xt_control_end();
    } else xt_worker_kick();
}

void xt_receive_stop_deferred(void)
{
    for (uint8_t i = 0; i < xt.links_max; ++i) {
        for (unsigned k = 0; k < 2; ++k) {
            xiaotai_critical_enter(&xt.lock);
            const xt_slot_t *slot = &xt.slots[i];
            const uint32_t gen = slot->rx[k].stop_pending && !slot->rx[k].on ? atomic_load(&slot->generation) : 0;
            xiaotai_critical_exit(&xt.lock);
            if (!gen) continue;
            if (xiaotai_mutex_lock(&xt.control, 0)) {
                stop_request(gen, (xiaotai_link_media_kind_t)k);
                xt_control_end();
            } else xt_worker_wake_at(xt_now_ms() + 1);
        }
    }
}

static bool same_format(const xiaotai_link_format_t *expected, const xiaotai_link_format_t *actual)
{
    return expected->codec != XIAOTAI_LINK_CODEC_NONE && expected->codec == actual->codec &&
           expected->rate_hz == actual->rate_hz && expected->channels == actual->channels;
}

static bool matches_format(const xiaotai_link_media_t *media, const xiaotai_link_format_t *format)
{
    for (size_t i = 0; i < XIAOTAI_LINK_RECEIVE_FORMATS; ++i)
        if (same_format(&media->receive_formats[i], format)) return true;
    return false;
}

void xiaotai_rtc_on_frame(xiaotai_rtc_conn_t conn, const xiaotai_link_frame_t *frame, const uint8_t *data, size_t size)
{
    if (!xt.slots || !frame || !xt_media_kind(frame->kind) || !data || !size) return;
    const xiaotai_link_media_kind_t kind = frame->kind;
    if (xiaotai_net_trace_active()) xiaotai_net_trace_media(kind == XIAOTAI_LINK_VIDEO);
    static const xiaotai_link_format_t pcma = {XIAOTAI_LINK_CODEC_G711A, 8000U, 1U};
    static const xiaotai_link_format_t opus = {XIAOTAI_LINK_CODEC_OPUS, 16000U, 1U};
    static const xiaotai_media_format_t opus_media = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
    const bool compatible_opus = xt.config.accept_pcma_opus_downlink && kind == XIAOTAI_LINK_AUDIO &&
        same_format(&opus, &frame->format) && xiaotai_media_can_receive(&opus_media);
    xiaotai_link_receive_fn_t deliver = NULL;
    void *ctx = NULL;
    uint32_t generation = 0;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    uint32_t ingress_epoch = 0;
    bool fixed_frame = false;
#endif
    uint32_t sample_count = 0, sample_why = 0;
    xiaotai_link_kind_t link_kind = XIAOTAI_LINK_INBOUND;
    bool fault = false;

    xiaotai_critical_enter(&xt.lock);
    xt_slot_t *slot = xt_link_conn_locked(conn);
    if (slot && atomic_load(&slot->state) == XT_SLOT_OPEN) {
        generation = atomic_load(&slot->generation);
        xt_receiver_t *rx = &slot->rx[kind];
        xiaotai_link_media_stats_t *stats = xt_media_stats(slot, kind);
        const xiaotai_link_media_t *media = xt_profile_media(slot, kind);
        const bool format_valid = frame->stream <= XIAOTAI_LINK_STREAM_MAX &&
            (matches_format(media, &frame->format) ||
             (compatible_opus && rx->fn == default_audio && matches_format(media, &pcma)));
        const bool stream_selected = media->accept_peer_stream || media->receive_stream == XIAOTAI_LINK_STREAM_ANY ||
                                     media->receive_stream == frame->stream;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
        const bool fixed_route = kind == XIAOTAI_LINK_AUDIO && stream_selected && selected_default(media, rx);
        fixed_frame = fixed_route && fixed_alaw(&frame->format);
        const bool fixed_pending = fixed_frame && media->receive_request && !rx->subscribed;
#endif
        uint32_t why = 0;
        if (!rx->on || !xt_link_open_locked(slot)) {
            ++stats->receive_closed;
            why = RX_CLOSED;
        } else if (!format_valid || !stream_selected) {
            ++stats->receive_filtered;
            why = RX_FILTERED;
            if (!format_valid && kind == XIAOTAI_LINK_AUDIO && xt.config.format_error_ends_link) {
                xt_queue_link_error_locked(slot, XIAOTAI_LINK_END_FORMAT, 0);
                fault = true;
            }
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
        } else if (fixed_pending) {
            ++stats->receive_closed;
            why = RX_CLOSED;
#endif
        } else if (atomic_load(&xt.receiving[kind])) {
            ++stats->receive_overlapped;
            why = RX_OVERLAPPED;
        } else if (xt_link_pin_locked(slot)) {
            atomic_store(&xt.receiving[kind], generation);
            deliver = rx->fn;
            ctx = rx->ctx;
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
            if (fixed_frame) ingress_epoch = xiaotai_media_ingress_epoch(generation);
            else if (fixed_route) xiaotai_media_ingress_break(generation);
#endif
        }
        if (why) {
#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
            if (fixed_route && !fixed_pending) xiaotai_media_ingress_break(generation);
#endif
            link_kind = slot->kind;
            sample_count = ++slot->receive_rejected;
            sample_why = why;
        }
    }
    xiaotai_critical_exit(&xt.lock);
    if (sample_count) note_frame(link_kind, generation, frame, size, sample_count, sample_why);
    if (fault) xt_worker_kick();
    if (!deliver) {
        atomic_fetch_add(&xt.counters.media_rx_dropped, 1U);
        return;
    }

#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
    const bool taken = fixed_frame ? (ingress_epoch && forward_audio(generation, frame, data, size, ingress_epoch)) :
                                     deliver(ctx, generation, frame, data, size);
#else
    const bool taken = deliver(ctx, generation, frame, data, size);
#endif
    xiaotai_critical_enter(&xt.lock);
    slot = xt_link_stats_locked(generation);
    if (slot) {
        link_kind = slot->kind;
        xiaotai_link_media_stats_t *stats = xt_media_stats(slot, kind);
        if (taken) {
            ++stats->received;
            sample_count = ++slot->receive_accepted;
        } else {
            ++stats->receive_refused;
            sample_count = ++slot->receive_rejected;
            sample_why = RX_REFUSED;
        }
    }
    atomic_store(&xt.receiving[kind], 0);
    xiaotai_critical_exit(&xt.lock);
    atomic_fetch_add(taken ? &xt.counters.media_rx : &xt.counters.media_rx_dropped, 1U);
    if (sample_count) note_frame(link_kind, generation, frame, size, sample_count, sample_why);
    xt_link_unpin(generation);
}
