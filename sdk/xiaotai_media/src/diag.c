// SPDX-License-Identifier: MIT
#include "diag.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "trace.h"

typedef struct {
    uint32_t rms_sum;
    int32_t dc_sum;
    uint16_t rms_max, peak;
} capture_level_sum_t;

typedef struct {
    xiaotai_critical_t lock;
    capture_level_sum_t sums[4];
    media_diag_level_window_t raw[3], published[4];
    unsigned raw_frames;
    bool ready;
} capture_level_diagnostics_t;

static _Atomic(const xiaotai_audio_input_t *) s_input;
static atomic_uint s_blocks;
static atomic_uint s_peak[XIAOTAI_AUDIO_CHANNELS_MAX];
static atomic_uint s_nonzero[XIAOTAI_AUDIO_CHANNELS_MAX];
static atomic_uint s_record_id, s_play_id;
static xiaotai_eventlog_topic_t *s_capture, *s_playout, *s_uplink;
static capture_level_diagnostics_t *s_capture_levels;

void media_diag_detach(void)
{
    s_capture = s_playout = s_uplink = NULL;
    xiaotai_mem_free(s_capture_levels);
    s_capture_levels = NULL;
    media_diag_input(NULL);
    atomic_store(&s_record_id, 0);
    atomic_store(&s_play_id, 0);
#if XIAOTAI_MEDIA_TEST_COMMANDS
    media_at_test_detach();
#endif
#if XIAOTAI_MEDIA_SOUND_COMMANDS
    media_at_sound_detach();
#endif
#if XIAOTAI_MEDIA_TRACE
    media_trace_reset();
#endif
}

bool media_diag_audio_active(void)
{
    return atomic_load(&s_record_id) || atomic_load(&s_play_id);
}

void media_diag_diagnostic_begin(bool recording, uint32_t id)
{
    atomic_store(recording ? &s_record_id : &s_play_id, id);
}

void media_diag_diagnostic_end(bool recording, uint32_t id)
{
    unsigned expected = id;
    (void)atomic_compare_exchange_strong(recording ? &s_record_id : &s_play_id, &expected, 0);
}

void media_diag_input(const xiaotai_audio_input_t *input)
{
    if (s_capture_levels) {
        memset(s_capture_levels->sums, 0, sizeof(s_capture_levels->sums));
        memset(s_capture_levels->raw, 0, sizeof(s_capture_levels->raw));
        s_capture_levels->raw_frames = 0;
        s_capture_levels->ready = false;
    }
    atomic_store(&s_blocks, 0);
    for (unsigned i = 0; i < XIAOTAI_AUDIO_CHANNELS_MAX; ++i) {
        atomic_store(&s_peak[i], 0);
        atomic_store(&s_nonzero[i], 0);
    }
    atomic_store(&s_input, input);
}

void media_diag_capture(const xiaotai_audio_input_t *input, const int16_t *pcm, size_t frames)
{
    if (!input || !frames || input != atomic_load(&s_input)) return;
    atomic_fetch_add_explicit(&s_blocks, 1, memory_order_relaxed);
    if (!pcm || input->desc.encoding != XIAOTAI_CODEC_PCM16) return;
    const uint8_t channels = input->desc.channels;
    if (!channels || channels > XIAOTAI_AUDIO_CHANNELS_MAX) return;
    uint32_t peak[XIAOTAI_AUDIO_CHANNELS_MAX] = {0}, nonzero[XIAOTAI_AUDIO_CHANNELS_MAX] = {0};
    for (size_t f = 0; f < frames; ++f) {
        for (uint8_t i = 0; i < channels; ++i) {
            const int v = *pcm++;
            const uint32_t level = (uint32_t)(v < 0 ? -v : v);
            if (level > peak[i]) peak[i] = level;
            nonzero[i] += level != 0;
        }
    }
    for (uint8_t i = 0; i < channels; ++i) {
        atomic_store_explicit(&s_peak[i], peak[i], memory_order_relaxed);
        atomic_fetch_add_explicit(&s_nonzero[i], nonzero[i], memory_order_relaxed);
    }
}

static const char *role_name(xiaotai_channel_role_t role)
{
    static const char *const roles[] = {"unused", "mic", "reference", "mono", "left", "right", "monitor"};
    return (unsigned)role < sizeof(roles) / sizeof(*roles) ? roles[role] : "unknown";
}

void media_diag_audio(const xiaotai_reply_t *reply)
{
    const xiaotai_audio_input_t *input = atomic_load(&s_input);
    xiaotai_audio_stats_t stats = {0};
    if (input && input->stats) input->stats(input->self, &stats);
    xiaotai_at_printf(reply, "+AUDIO: blocks=%u overflow=%" PRIu32 " errors=%" PRIu32,
                      atomic_load(&s_blocks), stats.overruns, stats.errors);
    if (!input) return;
    for (uint8_t i = 0; i < input->desc.channels && i < XIAOTAI_AUDIO_CHANNELS_MAX; ++i) {
        xiaotai_at_printf(reply, "+AUDIO-SLOT: slot=%u role=%s peak=%u nonzero=%u", (unsigned)i,
                          role_name(input->desc.roles[i]), atomic_load(&s_peak[i]), atomic_load(&s_nonzero[i]));
    }
}

static void report(const xiaotai_reply_t *reply)
{
    xiaotai_media_status_t status;
    xiaotai_media_stats_t stats;
    xiaotai_media_info_t info;
    xiaotai_processor_stats_t proc;
    xiaotai_media_status(&status);
    xiaotai_media_stats(&stats);
    xiaotai_media_info(&info);
    xiaotai_media_processor_stats(&proc);
    const bool echo_gate = xiaotai_media_echo_gate_enabled();
    xiaotai_at_printf(reply, "+MEDIA: ready=%u generation=%" PRIu32 " active=%u draining=%u uplink_open=%u "
                      "mic_muted=%u speaker_muted=%u playing=%" PRIu32 " recording=%" PRIu32
                      " capture_rate=%" PRIu32 " playback_rate=%" PRIu32 " downlink_ms=%" PRIu32
                      " preroll_capacity_ms=%" PRIu32, (unsigned)status.ready, status.generation,
                      (unsigned)status.active, (unsigned)status.draining, (unsigned)status.uplink_open,
                      (unsigned)status.mic_muted, (unsigned)status.speaker_muted, status.playing, status.recording,
                      info.capture_rate_hz, info.playback_rate_hz, info.downlink_ms, info.preroll_ms);
    xiaotai_at_printf(reply, "+MEDIA-RX: received=%" PRIu32 " played=%" PRIu32 " dropped=%" PRIu32
                      " stale=%" PRIu32 " invalid=%" PRIu32 " blocked=%" PRIu32 " overflow=%" PRIu32
                      " decode_failed=%" PRIu32 " flushed=%" PRIu32 " underruns=%" PRIu32 " packet_max_bytes=%" PRIu32
                      " pcm_written=%" PRIu32 " write_failed=%" PRIu32,
                      stats.rx_received, stats.rx_played, stats.rx_dropped, stats.rx_stale, stats.rx_invalid,
                      stats.rx_blocked, stats.rx_overflow, stats.rx_decode_failed, stats.rx_flushed,
                      stats.rx_underruns, stats.rx_packet_max_bytes, stats.pcm_written, stats.write_failed);
    xiaotai_at_printf(reply, "+MEDIA-TX: sent=%" PRIu32 " dropped=%" PRIu32 " full=%" PRIu32 " stale=%" PRIu32
                      " gated=%" PRIu32 " effect_failed=%" PRIu32 " encode_errors=%" PRIu32 " send_errors=%" PRIu32
                      " preroll=%" PRIu32 " injected=%" PRIu32 " owner_post_retries=%" PRIu32, stats.tx_sent,
                      stats.tx_dropped, stats.tx_full, stats.tx_stale, stats.tx_gated, stats.tx_effect_failed,
                      stats.tx_encode_errors, stats.tx_send_errors, stats.tx_preroll, stats.tx_injected,
                      stats.owner_post_retries);
    xiaotai_at_printf(reply, "+MEDIA-PROCESSOR: name=%s feeds=%" PRIu32 " frames=%" PRIu32 " input_full=%" PRIu32
                      " lost_samples=%" PRIu32 " errors=%" PRIu32 " fetch_timeouts=%" PRIu32
                      " deadline_misses=%" PRIu32 " max_us=%" PRIu32 " failed=%u",
                      info.processor && *info.processor ? info.processor : "none", proc.feeds, proc.frames,
                      proc.input_full, proc.lost_samples, proc.errors, proc.fetch_timeouts, proc.deadline_misses,
                      proc.process_max_us, (unsigned)proc.failed);
    xiaotai_at_printf(reply, "+ECHO: enabled=%u closed=%" PRIu32 " dt=%" PRIu32 " open=%" PRIu32
                      " gain_q15=%u far_floor=%u noise=%u", (unsigned)echo_gate,
                      proc.closed_frames, proc.double_talk_frames, proc.open_frames, (unsigned)proc.gain_q15,
                      (unsigned)proc.far_floor, (unsigned)proc.near_floor);
    xiaotai_mem_stats_t bulk, internal;
    xiaotai_mem_stats(XIAOTAI_MEM_BULK, &bulk);
    xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &internal);
    xiaotai_at_printf(reply, "+MEDIA-MEM: bulk_free=%zu bulk_largest=%zu internal_free=%zu internal_largest=%zu",
                      bulk.free, bulk.largest_block, internal.free, internal.largest_block);
    media_diag_audio(reply);
}

static void capture_level_add(capture_level_sum_t *sum, const xiaotai_media_level_t *level)
{
    sum->rms_sum += level->rms;
    sum->dc_sum += level->dc;
    if (level->rms > sum->rms_max) sum->rms_max = (uint16_t)level->rms;
    if (level->peak > sum->peak) sum->peak = (uint16_t)level->peak;
}

static media_diag_level_window_t capture_level_finish(capture_level_sum_t *sum)
{
    const media_diag_level_window_t result = {
        .rms = (uint16_t)(sum->rms_sum / MEDIA_DIAG_CAPTURE_FRAMES),
        .rms_max = sum->rms_max,
        .peak = sum->peak,
        .dc = (int16_t)(sum->dc_sum / (int32_t)MEDIA_DIAG_CAPTURE_FRAMES),
    };
    *sum = (capture_level_sum_t){0};
    return result;
}

void media_diag_capture_raw_levels(const xiaotai_media_level_t levels[3])
{
    capture_level_diagnostics_t *const diag = s_capture_levels;
    if (!diag) return;
    for (unsigned i = 0; i < 3; ++i) capture_level_add(&diag->sums[i], &levels[i]);
    if (++diag->raw_frames == MEDIA_DIAG_CAPTURE_FRAMES) {
        diag->raw_frames = 0;
        xiaotai_critical_enter(&diag->lock);
        for (unsigned i = 0; i < 3; ++i) diag->raw[i] = capture_level_finish(&diag->sums[i]);
        xiaotai_critical_exit(&diag->lock);
    }
}

void media_diag_capture_clean_level(const xiaotai_media_level_t *level)
{
    if (s_capture_levels) capture_level_add(&s_capture_levels->sums[3], level);
}

bool media_diag_capture_levels_snapshot(media_diag_level_window_t levels[4])
{
    capture_level_diagnostics_t *const diag = s_capture_levels;
    if (!diag) return false;
    xiaotai_critical_enter(&diag->lock);
    const bool ready = diag->ready;
    if (ready) memcpy(levels, diag->published, sizeof(diag->published));
    xiaotai_critical_exit(&diag->lock);
    return ready;
}

void media_diag_capture_window(const xiaotai_media_stats_t *s, const int32_t dc[4])
{
    // Event log topics store unsigned fields; 32768 denotes a zero DC mean.
    uint32_t values[] = {s->mic_rms, s->mic_peak, s->reference_rms, s->reference_peak,
        s->monitor_rms, s->monitor_peak, s->clean_rms, s->clean_peak, s->capture_process_max_us,
        s->capture_deadline_misses, s->capture_lost_samples, s->capture_overruns, s->capture_errors,
        (uint32_t)(32768 + dc[0]), (uint32_t)(32768 + dc[1]), (uint32_t)(32768 + dc[2]), (uint32_t)(32768 + dc[3])};
    capture_level_diagnostics_t *const diag = s_capture_levels;
    if (diag) {
        const media_diag_level_window_t clean = capture_level_finish(&diag->sums[3]);
        xiaotai_critical_enter(&diag->lock);
        // Raw and clean may run on different lanes. Use only completed raw windows.
        memcpy(diag->published, diag->raw, sizeof(diag->raw));
        diag->published[3] = clean;
        diag->ready = true;
        for (unsigned i = 0; i < 4; ++i) {
            values[2U * i] = diag->published[i].rms;
            values[2U * i + 1U] = diag->published[i].peak;
            values[13U + i] = (uint32_t)(32768 + diag->published[i].dc);
        }
        xiaotai_critical_exit(&diag->lock);
    }
    xiaotai_eventlog_post(s_capture, values);
}

void media_diag_playout_window(const xiaotai_media_stats_t *s, uint32_t peak, uint32_t near_full,
                                uint32_t step_max, uint32_t resample_clips)
{
    const uint32_t values[] = {s->target_ms, s->queued_ms, s->jitter_ms, s->playout_starts, s->late_bursts,
        s->rx_underruns, s->fast_blocks, s->slow_blocks, s->write_failed, s->slow_writes, s->output_waits,
        peak, near_full, step_max, resample_clips};
    xiaotai_eventlog_post(s_playout, values);
}

void media_diag_uplink_window(const xiaotai_media_stats_t *s)
{
    const uint32_t values[] = {s->tx_sent, s->tx_full, s->tx_stale, s->tx_gated, s->tx_effect_failed,
        s->tx_encode_errors, s->tx_send_errors, s->tx_preroll, s->tx_injected, s->tx_queue_age_max_ms,
        s->tx_encode_max_us, s->tx_send_max_us, s->uplink_rms, s->uplink_peak, s->uplink_clipped};
    xiaotai_eventlog_post(s_uplink, values);
}

xiaotai_err_t media_diag_register(void)
{
    static const char *const capture[] = {"mic_rms", "mic_peak", "ref_rms", "ref_peak", "monitor_rms",
        "monitor_peak", "clean_rms", "clean_peak", "process_max_us", "deadline_misses", "lost_samples",
        "overruns", "errors", "mic_dc_offset", "ref_dc_offset", "monitor_dc_offset", "clean_dc_offset"};
    static const char *const playout[] = {"target_ms", "queued_ms", "jitter_ms", "starts", "late_bursts",
        "underruns", "fast_blocks", "slow_blocks", "write_failed", "slow_writes", "output_waits",
        "peak", "near_full", "step_max", "resample_clips"};
    static const char *const uplink[] = {"sent", "full", "stale", "gated", "effect_failed", "encode_errors",
        "send_errors", "preroll", "injected", "queue_age_max_ms", "encode_max_us", "send_max_us", "rms", "peak", "clipped"};
    static const xiaotai_eventlog_topic_desc_t topics[] = {
        {"media", "capture", XIAOTAI_EVENTLOG_INFO, capture, sizeof(capture) / sizeof(*capture), 0},
        {"media", "playout", XIAOTAI_EVENTLOG_INFO, playout, sizeof(playout) / sizeof(*playout), 0},
        {"media", "uplink", XIAOTAI_EVENTLOG_INFO, uplink, sizeof(uplink) / sizeof(*uplink), 0},
    };
    xiaotai_eventlog_topic_t **const handles[] = {&s_capture, &s_playout, &s_uplink};
    for (size_t i = 0; i < sizeof(topics) / sizeof(*topics); ++i) {
        xiaotai_err_t err = xiaotai_eventlog_topic_register(&topics[i], handles[i]);
        if (err != XIAOTAI_OK) return err;
    }
    xiaotai_err_t err = xiaotai_diag_register("media", report);
    if (err == XIAOTAI_OK) err = media_commands_register();
    if (err == XIAOTAI_OK && xiaotai_media_param("diagnostics.capture_window", 0)) {
        s_capture_levels = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*s_capture_levels));
        if (!s_capture_levels) return XIAOTAI_ERR_NO_MEM;
        xiaotai_critical_init(&s_capture_levels->lock);
    }
    return err;
}
