// SPDX-License-Identifier: MIT
#include "audio_workers.h"

#include <stdatomic.h>
#include <string.h>
#include "diag.h"
#include "level.h"
#include "portable_audio.h"
#include "preroll.h"
#include "record.h"
#include "registry.h"
#include "resample.h"
#include "trace.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define EFFECT_MAX 14U
#define TAP_MAX 8U
#define FRAME_MS 20U
#define TX_SINK_STACK_KEY "task.uplink.sink_stack_bytes"

uint32_t media_uplink_sink_stack(void)
{
    /* The locked transport advertises its conservative unmeasured budget. */
    const int32_t tuned = xiaotai_media_param(TX_SINK_STACK_KEY, 6656);
    return tuned > 0 ? (uint32_t)tuned : XIAOTAI_MEDIA_SINK_STACK_DEFAULT;
}

typedef struct {
    const xiaotai_audio_effect_t *desc;
    xiaotai_effect_info_t info;
    void *state;
} effect_t;

typedef struct {
    uint32_t generation, epoch, timestamp_ms;
    int64_t captured_us, enqueued_us;
    bool preroll;
    int16_t pcm[];
} tx_frame_t;

typedef struct { uint8_t pre, level; } trace_level_t;

/* The folded topology queues encoded audio, not full PCM frames. Clocks
 * store the low 32 bits in microseconds; a four-slot queue plus a bounded
 * send delay is far shorter than their 71-minute wrap interval. */
typedef struct {
    uint32_t generation, epoch, timestamp_ms, captured_clock, enqueued_clock;
    uint16_t size;
    trace_level_t trace;
    uint8_t data[];
} folded_packet_t;

_Static_assert(sizeof(folded_packet_t) == 24, "folded queue metadata is part of its memory budget");

typedef struct {
    uint32_t generation;
    uint8_t volume;
    int8_t gain;
} capture_controls_t;

typedef struct {
    xiaotai_task_fn_t fn;
    xiaotai_signal_t start, done;
    atomic_bool parked;
    bool created;
} lane_t;

struct media_audio_workers {
    const xiaotai_audio_input_t *input;
    const xiaotai_audio_processor_t *processor;
    void *state;
    xiaotai_processor_info_t info;
    const xiaotai_audio_decoder_t *input_decoder;
    void *input_decoder_state;
    lane_t capturing, cleaning, sending;
    atomic_bool stopping, failed, paused;
    xiaotai_queue_t tx_queue;
    bool tx_queue_ready, encoded, folded, delegated, sync_tx_on_dequeue;
    atomic_bool dispatching;
    bool reset_effects_on_mute, has_resampler, account_capture_effect_gaps;
    uint16_t tx_max_queue_age_ms;
    void *raw;
    size_t raw_size;
    effect_t effects[EFFECT_MAX];
    size_t effect_count, capture_delay;
    uint32_t effect_stack[2];
    int16_t *clean, *replay, *network_pcm;
    uint8_t *encoded_packet;
    folded_packet_t *fold_producer, *fold_consumer;
    size_t encoded_capacity, network_capacity;
    capture_controls_t folded_controls;
    uint32_t fold_tx_generation, fold_tx_epoch;
    bool fold_reset_encoder;
    atomic_bool fold_capture_gap;
    tx_frame_t *producer, *consumer;
    size_t tx_samples, queued_samples;
    uint32_t packet_generation, packet_epoch;
    uint32_t capture_mic_epoch;
    int64_t packet_next_us;
    bool timeline_ready;
    bool tap_open[TAP_MAX];
    uint32_t input_lost, input_overruns, pending_lost, voice_smooth;
    uint32_t capture_blocks, uplink_blocks;
    uint32_t raw_dc_blocks;
    int64_t raw_dc_sum[3], clean_dc_sum;
    int32_t raw_dc[3];
    uint8_t voice_level, candidate;
    int64_t candidate_since;
    atomic_uchar published_voice;
    atomic_uint tone_generation, tone_ms;
    xiaotai_critical_t counters_lock;
    xiaotai_media_stats_t counters;
    uint32_t tx_generation;
    uint32_t dispatch_generation;
    int64_t last_send_us;
    media_resampler_storage_t resampler[];
};

static bool encode_uplink(media_audio_workers_t *w, const media_capture_view_t *view, int16_t *pcm, size_t count,
                          size_t *bytes, bool restart, trace_level_t *trace);
static void tx_session(media_audio_workers_t *w, uint32_t generation, uint32_t *previous, uint32_t epoch,
                       uint32_t *prior_epoch);

static uint32_t elapsed(int64_t start, int64_t end)
{
    const uint64_t ms = end > start ? (uint64_t)(end - start) : 0;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static uint32_t plus(uint32_t a, uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }
static void maximum(uint32_t *v, uint32_t n) { if (n > *v) *v = n; }
static void take(media_audio_workers_t *w) { xiaotai_critical_enter(&w->counters_lock); }
static void give(media_audio_workers_t *w) { xiaotai_critical_exit(&w->counters_lock); }

static bool queued_uplink_current(media_audio_workers_t *w, uint32_t wait_us)
{
    const uint32_t wait_ms = wait_us / 1000U;
    const bool expired = w->tx_max_queue_age_ms && wait_ms >= w->tx_max_queue_age_ms;
    take(w);
    maximum(&w->counters.tx_queue_age_max_ms, wait_ms);
    if (expired) ++w->counters.tx_stale;
    give(w);
    return !expired;
}

static void effect_reset(media_audio_workers_t *w, xiaotai_effect_stage_t stage, bool summarize)
{
    for (size_t i = 0; i < w->effect_count; ++i) {
        const effect_t *e = &w->effects[i];
        if (e->desc->stage != stage) continue;
        if (summarize && e->desc == &media_noise_gate) media_noise_gate_end(e->state);
        if (e->desc->reset) e->desc->reset(e->state);
    }
}

static bool effects_process(media_audio_workers_t *w, xiaotai_effect_stage_t stage, uint32_t flags,
                            int16_t *pcm, size_t count)
{
    for (size_t i = 0; i < w->effect_count; ++i) {
        const effect_t *e = &w->effects[i];
        if (e->desc->stage != stage || (stage == XIAOTAI_EFFECT_UPLINK &&
            (flags & e->desc->session_flags) != e->desc->session_flags)) continue;
        if (e->desc->process(e->state, pcm, count) != XIAOTAI_OK) {
            effect_reset(w, stage, false);
            return false;
        }
    }
    return true;
}

static xiaotai_err_t open_effect(media_audio_workers_t *w, const xiaotai_audio_effect_t *desc)
{
    if (w->effect_count >= EFFECT_MAX || desc->stage > XIAOTAI_EFFECT_UPLINK) return XIAOTAI_ERR_INVALID_ARG;
    effect_t effect = {.desc = desc};
    const xiaotai_err_t err = desc->open(&w->info, &effect.info, &effect.state);
    if (err == XIAOTAI_ERR_NOT_SUPPORTED) return XIAOTAI_OK;
    if (err != XIAOTAI_OK) return err;
    size_t at = w->effect_count++;
    while (at && w->effects[at - 1U].desc->order > desc->order) {
        w->effects[at] = w->effects[at - 1U];
        --at;
    }
    w->effects[at] = effect;
    maximum(&w->effect_stack[desc->stage], effect.info.stack_bytes);
    if (desc->stage == XIAOTAI_EFFECT_CAPTURE) w->capture_delay += effect.info.delay_samples;
    return XIAOTAI_OK;
}

static void taps(media_audio_workers_t *w, uint32_t stage, bool enabled, bool activity,
                 bool gap, const int16_t *pcm, size_t count, int64_t end_us)
{
    for (size_t i = 0; i < TAP_MAX; ++i) {
        bool requested;
        const xiaotai_media_tap_t *tap = media_tap_at(i, &requested);
        if (!tap) break;
        if ((tap->flags & XIAOTAI_TAP_RAW) != stage) continue;
        const bool open = enabled && requested && (!activity || (tap->flags & XIAOTAI_TAP_IN_SESSION));
        if ((!open || gap) && w->tap_open[i]) {
            if (tap->gate) tap->gate(tap->ctx, false);
            w->tap_open[i] = false;
        }
        if (open) {
            if (!w->tap_open[i] && tap->gate) tap->gate(tap->ctx, true);
            w->tap_open[i] = true;
            tap->pcm(tap->ctx, pcm, count, w->info.rate_hz, end_us);
        }
    }
}

static void close_taps(media_audio_workers_t *w)
{
    taps(w, XIAOTAI_TAP_RAW, false, false, false, NULL, 0, 0);
    taps(w, 0, false, false, false, NULL, 0, 0);
}

static void voice_level(media_audio_workers_t *w, uint32_t rms, int64_t now)
{
    w->voice_smooth = (3U * w->voice_smooth + rms) / 4U;
    const uint8_t next = w->voice_smooth >= 850 ? 3 : w->voice_smooth >= 300 ? 2 : w->voice_smooth >= 100 ? 1 : 0;
    if (next != w->candidate) {
        w->candidate = next;
        w->candidate_since = now;
    } else if (next != w->voice_level && now - w->candidate_since >= 300000) {
        w->voice_level = next;
    }
    if (atomic_load(&w->published_voice) != w->voice_level &&
        xiaotai_event_emit(XIAOTAI_EVENT_MEDIA_VOICE, &(xiaotai_args_t){.value = w->voice_level}, NULL, 0) == XIAOTAI_OK) {
        atomic_store(&w->published_voice, w->voice_level);
    }
}

static bool offer(media_audio_workers_t *w, const media_capture_view_t *view, int64_t start_us,
                  int64_t end_us, bool preroll, const int16_t *pcm)
{
    bool tail;
    /* A pre-roll replay holds the history mutex. Its caller checked the
     * grant already, and the queued generation is checked again on send. */
    if (!preroll && !media_runtime_uplink_allowed(view->session.generation, view->uplink_epoch, end_us, &tail)) {
        take(w);
        ++w->counters.tx_stale;
        give(w);
        return false;
    }
    tx_frame_t *queue = w->producer;
    if (pcm != queue->pcm) memcpy(queue->pcm, pcm, w->tx_samples * sizeof(*pcm));
    const void *item = queue;
    if (w->folded) {
        folded_packet_t *packet = w->fold_producer;
        size_t bytes = 0;
        trace_level_t trace = {0};
        if (!encode_uplink(w, view, queue->pcm, w->tx_samples, &bytes, w->fold_reset_encoder, &trace)) return false;
        w->fold_reset_encoder = false;
        if (!media_runtime_uplink_allowed(view->session.generation, view->uplink_epoch, end_us, &tail)) {
            take(w);
            ++w->counters.tx_stale;
            give(w);
            return false;
        }
        packet->generation = view->session.generation;
        packet->epoch = view->uplink_epoch;
        packet->timestamp_ms = (uint32_t)(start_us / 1000);
        packet->captured_clock = (uint32_t)end_us;
        packet->enqueued_clock = (uint32_t)xiaotai_time_us();
        packet->size = (uint16_t)bytes;
        packet->trace = trace;
        item = packet;
    } else {
        queue->generation = view->session.generation;
        queue->epoch = view->uplink_epoch;
        queue->timestamp_ms = (uint32_t)(start_us / 1000);
        queue->captured_us = end_us;
        queue->enqueued_us = xiaotai_time_us();
        queue->preroll = preroll;
    }
    if (xiaotai_queue_send(&w->tx_queue, item, 0)) {
        if (w->delegated) media_runtime_uplink_notify();
        return true;
    }
    take(w);
    ++w->counters.tx_full;
    give(w);
    return false;
}

typedef struct { media_audio_workers_t *w; const media_capture_view_t *view; } replay_offer_t;

static bool offer_replay(const int16_t *pcm, size_t samples, int64_t started, void *ctx)
{
    const replay_offer_t *a = ctx;
    return offer(a->w, a->view, started, started + (int64_t)samples * 1000000 / a->w->info.rate_hz, true, pcm);
}

static void packetize(media_audio_workers_t *w, const media_capture_view_t *view, const int16_t *pcm,
                     size_t count, int64_t end_us, uint32_t lost)
{
    if (w->folded && w->tx_queue_ready) {
        const uint32_t active = view->active ? view->session.generation : 0;
        w->fold_reset_encoder |= active != w->fold_tx_generation || view->uplink_epoch != w->fold_tx_epoch;
        tx_session(w, active, &w->fold_tx_generation, view->uplink_epoch, &w->fold_tx_epoch);
    }
    if (!w->tx_queue_ready || !view->session.sink) return;
    const uint32_t generation = view->session.generation;
    const bool new_epoch = generation != w->packet_generation || view->uplink_epoch != w->packet_epoch;
    if (new_epoch) {
        w->queued_samples = 0;
        w->timeline_ready = false;
        if (w->folded) w->fold_reset_encoder = true;
        w->packet_generation = generation;
        w->packet_epoch = view->uplink_epoch;
    }
    if (!view->active || !view->gate || view->muted ||
        (view->session.sink->ready && !view->session.sink->ready(view->session.sink->ctx, generation))) {
        w->queued_samples = 0;
        w->timeline_ready = false;
        return;
    }
    media_preroll_t *history = media_preroll_current();
    if (!w->folded && view->preroll && media_preroll_token(history)) {
        bool tail;
        if (!media_runtime_uplink_allowed(generation, view->uplink_epoch, end_us, &tail)) return;
        replay_offer_t ctx = {w, view};
        for (unsigned attempts = 0; attempts < 3U; ++attempts) {
            const xiaotai_err_t result = media_preroll_replay(history, generation, w->replay,
                                                            w->tx_samples, offer_replay, &ctx);
            if (result == XIAOTAI_OK) continue;
            if (result == XIAOTAI_ERR_NO_MEM || result == XIAOTAI_ERR_TIMEOUT) return;
            size_t suffix = 0;
            int64_t begin = 0;
            if (media_preroll_finish(history, generation, w->producer->pcm, w->tx_samples, &suffix, &begin) == XIAOTAI_OK) {
                w->queued_samples = suffix;
                w->packet_next_us = suffix ? begin : end_us;
                w->timeline_ready = true;
            }
            /* This input block already went into the replay ring. */
            return;
        }
        return;
    }
    if (!w->timeline_ready) {
        w->packet_next_us = end_us - (int64_t)count * 1000000 / w->info.rate_hz;
        w->timeline_ready = true;
    } else if (lost) {
        w->packet_next_us += (int64_t)(lost + w->queued_samples) * 1000000 / w->info.rate_hz;
        w->queued_samples = 0;
    }
    size_t at = 0;
    while (at < count) {
        size_t n = w->tx_samples - w->queued_samples;
        if (n > count - at) n = count - at;
        memcpy(w->producer->pcm + w->queued_samples, pcm + at, n * sizeof(*pcm));
        w->queued_samples += n;
        at += n;
        if (w->queued_samples == w->tx_samples) {
            const int64_t frame_end = end_us - (int64_t)(count - at) * 1000000 / w->info.rate_hz;
            (void)offer(w, view, w->packet_next_us, frame_end, false, w->producer->pcm);
            w->packet_next_us += FRAME_MS * 1000U;
            w->queued_samples = 0;
        }
    }
}

static void fail_input(media_audio_workers_t *w)
{
    if (!atomic_exchange(&w->failed, true)) media_runtime_input_failed();
}

static bool wait_for_resume(media_audio_workers_t *w, lane_t *lane)
{
    const bool pause = atomic_load(&w->paused);
    if (!pause) return false;
    if (lane == &w->cleaning || (lane == &w->capturing && !(w->info.flags & XIAOTAI_PROCESSOR_ASYNC))) close_taps(w);
    atomic_store(&lane->parked, true);
    while (atomic_load(&w->paused) && !atomic_load(&w->stopping)) xiaotai_delay_ms(1);
    atomic_store(&lane->parked, false);
    return true;
}

static void clean_frame(media_audio_workers_t *w, const xiaotai_processor_frame_t *frame, uint32_t process_us)
{
    if (!frame->data || !frame->samples || frame->samples > w->info.fetch_max ||
        frame->encoding != XIAOTAI_CODEC_PCM16 || frame->size != frame->samples * sizeof(int16_t)) return;
    media_capture_view_t view;
    media_runtime_capture_view(&view);
    const bool recording = media_record_active();
    const int64_t frame_us = (int64_t)frame->samples * 1000000 / w->info.rate_hz;
    const int64_t raw_begin = frame->captured_us - frame_us;
    const bool audible = !view.muted && !recording && (!view.mic_epoch || raw_begin >= view.mic_cutover_us);
    const uint32_t lost = frame->lost_samples;
    const int64_t captured = frame->captured_us - (int64_t)w->capture_delay * 1000000 / w->info.rate_hz;
    if (lost || (w->reset_effects_on_mute && view.mic_epoch != w->capture_mic_epoch))
        effect_reset(w, XIAOTAI_EFFECT_CAPTURE, false);
    w->capture_mic_epoch = view.mic_epoch;
    taps(w, XIAOTAI_TAP_RAW, audible, view.active, lost != 0, frame->data, frame->samples, frame->captured_us);
    memcpy(w->clean, frame->data, frame->size);
    const bool effects_ok = effects_process(w, XIAOTAI_EFFECT_CAPTURE, 0, w->clean, frame->samples);
    const xiaotai_media_level_t level = xiaotai_media_level_measure(w->clean, frame->samples, 1);
    const bool effect_gap = !effects_ok && w->account_capture_effect_gaps;
    take(w);
    w->counters.capture_lost_samples += effect_gap ? plus(lost, frame->samples) : lost;
    maximum(&w->counters.capture_process_max_us, process_us);
    w->counters.capture_deadline_misses += (uint64_t)process_us * w->info.rate_hz > (uint64_t)frame->samples * 1000000U;
    w->counters.clean_peak = (uint16_t)level.peak;
    w->counters.clean_rms = (uint16_t)level.rms;
    if (!effects_ok) ++w->counters.capture_errors;
    give(w);
    voice_level(w, level.rms, frame->captured_us);
    media_diag_capture_clean_level(&level);
    if (++w->capture_blocks % MEDIA_DIAG_CAPTURE_FRAMES == 0) {
        xiaotai_media_stats_t snapshot = {0};
        media_audio_stats(w, &snapshot);
        int32_t dc[4];
        take(w);
        memcpy(dc, w->raw_dc, sizeof(w->raw_dc));
        give(w);
        dc[3] = (int32_t)((w->clean_dc_sum + level.dc) / (int32_t)MEDIA_DIAG_CAPTURE_FRAMES);
        w->clean_dc_sum = 0;
        media_diag_capture_window(&snapshot, dc);
    } else w->clean_dc_sum += level.dc;
    if (!effects_ok) {
        close_taps(w);
        media_preroll_discard(media_preroll_current(), true);
        media_record_end(0, XIAOTAI_FAIL);
        /* Reanchoring on the next frame must not reuse PCM from before the gap. */
        if (effect_gap) w->queued_samples = 0;
        w->timeline_ready = false;
        return;
    }
    media_capture_view_t latest;
    media_runtime_capture_view(&latest);
    if (latest.muted || latest.mic_epoch != view.mic_epoch || latest.uplink_epoch != view.uplink_epoch ||
        (latest.mic_epoch && captured - frame_us < latest.mic_cutover_us) || media_record_active() != recording) {
        close_taps(w);
        return;
    }
    view = latest;
    taps(w, 0, audible, view.active, lost != 0, w->clean, frame->samples, captured);
    bool tap_enabled = false;
    for (size_t i = 0; i < TAP_MAX; ++i) {
        bool enabled;
        if (!media_tap_at(i, &enabled)) break;
        tap_enabled |= enabled;
    }
    if (!view.muted) {
        if (recording) media_record_feed(w->clean, frame->samples, captured, lost);
        else if ((view.preroll && media_preroll_token(media_preroll_current())) ||
                 (!view.active && (tap_enabled || media_preroll_token(media_preroll_current())))) {
            (void)media_preroll_append_granted(media_preroll_current(), w->clean, frame->samples, captured, lost, view.mic_epoch);
        }
    }
    packetize(w, &view, w->clean, frame->samples, captured, lost);
    if (media_record_service()) media_session_wake_playout();
}

static void raw_levels(media_audio_workers_t *w)
{
    const xiaotai_audio_desc_t *d = &w->input->desc;
    xiaotai_media_level_t best[3] = {{0}};
    for (size_t i = 0; i < d->channels; ++i) {
        const int role = d->roles[i] == XIAOTAI_CHANNEL_MIC ? 0 : d->roles[i] == XIAOTAI_CHANNEL_REFERENCE ? 1 :
                         d->roles[i] == XIAOTAI_CHANNEL_MONITOR ? 2 : -1;
        if (role < 0) continue;
        const xiaotai_media_level_t level = xiaotai_media_level_measure((const int16_t *)w->raw + i, w->info.feed_frames, d->channels);
        if (level.peak > best[role].peak) best[role] = level;
    }
    take(w);
    w->counters.mic_peak = (uint16_t)best[0].peak;
    w->counters.mic_rms = (uint16_t)best[0].rms;
    w->counters.reference_peak = (uint16_t)best[1].peak;
    w->counters.reference_rms = (uint16_t)best[1].rms;
    w->counters.monitor_peak = (uint16_t)best[2].peak;
    w->counters.monitor_rms = (uint16_t)best[2].rms;
    for (size_t i = 0; i < 3; ++i) w->raw_dc_sum[i] += best[i].dc;
    if (++w->raw_dc_blocks % MEDIA_DIAG_CAPTURE_FRAMES == 0) {
        for (size_t i = 0; i < 3; ++i) {
            w->raw_dc[i] = (int32_t)(w->raw_dc_sum[i] / (int32_t)MEDIA_DIAG_CAPTURE_FRAMES);
            w->raw_dc_sum[i] = 0;
        }
    }
    give(w);
    media_diag_capture_raw_levels(best);
}

static xiaotai_err_t read_input(media_audio_workers_t *w, size_t *bytes, int64_t *now, uint32_t *lost)
{
    *bytes = 0;
    const xiaotai_err_t ret = w->input->read(w->input->self, w->raw, w->raw_size, bytes);
    *now = xiaotai_time_us();
    if (ret != XIAOTAI_OK || !*bytes || *bytes > w->raw_size || (!w->encoded && *bytes != w->raw_size)) {
        w->pending_lost = plus(w->pending_lost, w->encoded ? 1U : w->info.feed_frames);
        take(w);
        ++w->counters.capture_errors;
        give(w);
        if (ret != XIAOTAI_ERR_TIMEOUT || w->input->info.read_timeout_ms) {
            if (media_record_active()) media_record_end(0, XIAOTAI_FAIL);
            media_preroll_t *history = media_preroll_current();
            if (media_preroll_token(history)) media_preroll_discard(history, true);
            media_session_wake_playout();
        }
        return ret == XIAOTAI_OK ? XIAOTAI_ERR_INVALID_SIZE : ret;
    }
    if (w->input->stats) {
        xiaotai_audio_stats_t hardware = {0};
        w->input->stats(w->input->self, &hardware);
        uint32_t missing = hardware.lost_frames - w->input_lost;
        if (!missing && hardware.overruns != w->input_overruns) {
            const uint32_t block = w->input->info.block_frames ? w->input->info.block_frames : w->info.feed_frames;
            const uint64_t samples = (uint64_t)(hardware.overruns - w->input_overruns) * block;
            missing = samples > UINT32_MAX ? UINT32_MAX : (uint32_t)samples;
        }
        w->pending_lost = plus(w->pending_lost, missing);
        w->input_lost = hardware.lost_frames;
        w->input_overruns = hardware.overruns;
        take(w);
        w->counters.capture_overruns = hardware.overruns;
        give(w);
    }
    *lost = w->pending_lost;
    w->pending_lost = 0;
    return XIAOTAI_OK;
}

static void update_input(media_audio_workers_t *w, const media_capture_view_t *view, uint32_t *prior,
                         int8_t *gain, uint8_t *volume)
{
    const uint32_t generation = view->active ? view->session.generation : 0;
    if (generation != *prior && w->processor->session) {
        if (*prior) w->processor->session(w->state, false);
        if (generation) w->processor->session(w->state, true);
    }
    *prior = generation;
    if (view->gain_db != *gain || view->volume != *volume) {
        const bool change_gain = view->gain_db != *gain && w->input->set_gain_db;
        const xiaotai_err_t result = change_gain ? w->input->set_gain_db(w->input->self, view->gain_db) : XIAOTAI_OK;
        if (change_gain) media_runtime_gain_applied(view->gain_db, result);
        if (w->processor->levels) {
            const int8_t applied = change_gain && result == XIAOTAI_OK ? view->gain_db : view->gain_applied_db;
            w->processor->levels(w->state, view->volume, applied);
        }
        *gain = view->gain_db;
        *volume = view->volume;
    }
}

static void reset_capture_effects(media_audio_workers_t *w)
{
    effect_reset(w, XIAOTAI_EFFECT_CAPTURE, false);
    w->queued_samples = 0;
    w->timeline_ready = false;
}

static void capture_pass(media_audio_workers_t *w, capture_controls_t *controls)
{
    media_capture_view_t view;
    media_runtime_capture_view(&view);
    if (view.muted && !(w->info.flags & XIAOTAI_PROCESSOR_ASYNC)) close_taps(w);
    update_input(w, &view, &controls->generation, &controls->gain, &controls->volume);
    size_t bytes;
    int64_t now;
    uint32_t lost;
    if (read_input(w, &bytes, &now, &lost) != XIAOTAI_OK) {
        if (w->folded) xiaotai_delay_ms(1);
        return;
    }
    media_diag_capture(w->input, w->raw, w->info.feed_frames);
    raw_levels(w);
    const int64_t start = xiaotai_time_us();
    const xiaotai_err_t fed = w->processor->feed(w->state, w->raw, bytes, now, lost);
    if (fed == XIAOTAI_ERR_INVALID_STATE) fail_input(w);
    if (fed == XIAOTAI_OK && !(w->info.flags & XIAOTAI_PROCESSOR_ASYNC)) {
        xiaotai_processor_frame_t frame;
        const xiaotai_err_t fetched = w->processor->fetch(w->state, &frame, 0);
        if (fetched == XIAOTAI_ERR_INVALID_STATE) fail_input(w);
        if (fetched == XIAOTAI_OK) {
            if (frame.lost_samples < lost) frame.lost_samples = lost;
            clean_frame(w, &frame, elapsed(start, xiaotai_time_us()));
        } else w->pending_lost = plus(w->pending_lost, plus(lost, w->info.feed_frames));
    }
}

bool media_audio_folded_pass(media_audio_workers_t *w)
{
    if (!w || !w->folded || atomic_load(&w->paused) || atomic_load(&w->stopping) || atomic_load(&w->failed)) return false;
    if (atomic_exchange(&w->fold_capture_gap, false)) reset_capture_effects(w);
    capture_pass(w, &w->folded_controls);
    return true;
}

static void capture_loop(void *arg)
{
    media_audio_workers_t *w = arg;
    capture_controls_t controls = {.volume = UINT8_MAX, .gain = w->input->info.gain_db};
    while (!atomic_load(&w->stopping) && !atomic_load(&w->failed)) {
        if (wait_for_resume(w, &w->capturing) && !(w->info.flags & XIAOTAI_PROCESSOR_ASYNC)) reset_capture_effects(w);
        if (atomic_load(&w->stopping)) break;
        capture_pass(w, &controls);
        xiaotai_delay_ms(1);
    }
    if (!(w->info.flags & XIAOTAI_PROCESSOR_ASYNC)) close_taps(w);
}

static void clean_loop(void *arg)
{
    media_audio_workers_t *w = arg;
    while (!atomic_load(&w->stopping) && !atomic_load(&w->failed)) {
        if (wait_for_resume(w, &w->cleaning)) reset_capture_effects(w);
        if (atomic_load(&w->stopping)) break;
        media_capture_view_t view;
        media_runtime_capture_view(&view);
        if (view.muted) close_taps(w);
        xiaotai_processor_frame_t frame;
        const xiaotai_err_t ret = w->processor->fetch(w->state, &frame, 20);
        if (ret == XIAOTAI_OK) clean_frame(w, &frame, frame.process_us);
        else if (ret == XIAOTAI_ERR_INVALID_STATE) fail_input(w);
        else if (ret != XIAOTAI_ERR_TIMEOUT) xiaotai_delay_ms(1);
    }
    close_taps(w);
}

static void tx_session(media_audio_workers_t *w, uint32_t generation, uint32_t *previous, uint32_t epoch,
                       uint32_t *prior_epoch)
{
    if (generation == *previous && epoch == *prior_epoch) return;
    effect_reset(w, XIAOTAI_EFFECT_UPLINK, *previous && generation != *previous);
    if (w->has_resampler) memset(w->resampler, 0, sizeof(*w->resampler));
    *previous = generation;
    *prior_epoch = epoch;
}

static size_t resample_uplink(media_audio_workers_t *w, const int16_t *pcm, size_t count, uint32_t rate, uint32_t dest)
{
    switch (xiaotai_media_resample_plan(rate, dest)) {
        case XIAOTAI_MEDIA_RESAMPLE_COPY:
            if (count > w->network_capacity) return 0;
            memcpy(w->network_pcm, pcm, count * sizeof(*pcm));
            return count;
        case XIAOTAI_MEDIA_RESAMPLE_DOWN2:
            if (!w->has_resampler) return 0;
            return xiaotai_media_down2_process(&w->resampler->down, pcm, count, w->network_pcm, w->network_capacity);
        case XIAOTAI_MEDIA_RESAMPLE_UP2:
            if (!w->has_resampler) return 0;
            if (!w->resampler->up.initialized) xiaotai_media_up2_reset(&w->resampler->up, pcm[0]);
            return xiaotai_media_up2_process(&w->resampler->up, pcm, count, w->network_pcm, w->network_capacity);
        case XIAOTAI_MEDIA_RESAMPLE_RATE: {
            if (!w->has_resampler) return 0;
            xiaotai_media_rate_t *state = &w->resampler->rate;
            if (!state->in_hz && xiaotai_media_rate_open(state, rate, 1, dest) != XIAOTAI_OK) return 0;
            size_t consumed = 0;
            const size_t n = xiaotai_media_rate_process(state, (const uint8_t *)pcm, count, &consumed,
                                                        w->network_pcm, w->network_capacity);
            return consumed == count ? n : 0;
        }
        default: return 0;
    }
}

static bool encode_uplink(media_audio_workers_t *w, const media_capture_view_t *view, int16_t *pcm, size_t count,
                          size_t *bytes, bool restart, trace_level_t *trace)
{
    const xiaotai_media_format_t *fmt = &view->session.uplink;
    const bool tracing = media_trace_active();
    if (tracing) trace->pre = media_trace_db(pcm, count);
    if (!effects_process(w, XIAOTAI_EFFECT_UPLINK, view->session.flags, pcm, count)) {
        take(w);
        ++w->counters.tx_effect_failed;
        give(w);
        return false;
    }
    const size_t n = resample_uplink(w, pcm, count, w->info.rate_hz, fmt->rate_hz);
    media_tx_codec_t codec;
    if (!n || n != (uint64_t)fmt->packet_ms * fmt->rate_hz / 1000U ||
        !media_tx_codec(fmt, &codec) || !codec.encoder) goto invalid;
    const bool injected = atomic_load(&w->tone_generation) == view->session.generation && atomic_load(&w->tone_ms);
    if (injected) {
        /* Triangle at 400 Hz; fixed-point phase and bounded amplitude. */
        for (size_t i = 0; i < n; ++i) {
            const int32_t phase = (int32_t)((i * 400U * 12000U / fmt->rate_hz) % 12000U);
            w->network_pcm[i] = (int16_t)(phase < 6000 ? phase - 3000 : 9000 - phase);
        }
        unsigned ms = atomic_load(&w->tone_ms);
        while (!atomic_compare_exchange_weak(&w->tone_ms, &ms, ms > fmt->packet_ms ? ms - fmt->packet_ms : 0)) {}
    }
    const int64_t start = xiaotai_time_us();
    if ((restart && codec.encoder->reset && codec.encoder->reset(codec.state) != XIAOTAI_OK) ||
        codec.encoder->encode(codec.state, w->network_pcm, n, w->encoded_packet, w->encoded_capacity, bytes) != XIAOTAI_OK ||
        !*bytes || *bytes > w->encoded_capacity) goto invalid;
    if (((fmt->encoding == XIAOTAI_CODEC_G711A || fmt->encoding == XIAOTAI_CODEC_G711U) && *bytes != n) ||
        (fmt->encoding == XIAOTAI_CODEC_PCM16 && *bytes != n * sizeof(int16_t))) goto invalid;
    if (tracing) trace->level = media_trace_db(w->network_pcm, n);
    const uint32_t took = elapsed(start, xiaotai_time_us());
    const xiaotai_media_level_t level = xiaotai_media_level_measure(w->network_pcm, n, 1);
    take(w);
    uint32_t hash = w->counters.pcm_in_hash ? w->counters.pcm_in_hash : UINT32_C(2166136261);
    give(w);
    uint32_t clipped = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint16_t s = (uint16_t)w->network_pcm[i];
        hash = (hash ^ (uint8_t)s) * UINT32_C(16777619);
        hash = (hash ^ (uint8_t)(s >> 8)) * UINT32_C(16777619);
        clipped += w->network_pcm[i] >= 32700 || w->network_pcm[i] <= -32700;
    }
    take(w);
    if (w->tx_generation != view->session.generation) {
        give(w);
        return false;
    }
    w->counters.pcm_in_hash = hash;
    w->counters.tx_injected += injected;
    maximum(&w->counters.tx_encode_max_us, took);
    w->counters.uplink_rms = (uint16_t)level.rms;
    w->counters.uplink_peak = (uint16_t)level.peak;
    w->counters.uplink_clipped += clipped;
    give(w);
    return true;
invalid:
    take(w);
    ++w->counters.tx_encode_errors;
    give(w);
    return false;
}

static bool allowed(media_audio_workers_t *w, uint32_t generation, uint32_t epoch, int64_t captured, bool *tail)
{
    if (!media_runtime_uplink_allowed(generation, epoch, captured, tail)) {
        media_capture_view_t latest;
        media_runtime_capture_view(&latest);
        take(w);
        if (latest.session.generation != generation || latest.uplink_epoch != epoch) ++w->counters.tx_stale;
        else ++w->counters.tx_gated;
        give(w);
        return false;
    }
    return true;
}

static void send_admitted(media_audio_workers_t *w, const xiaotai_media_sink_t *sink,
                          uint32_t generation, uint32_t stamp, const uint8_t *data, size_t bytes,
                          uint16_t packet_ms, bool replay, bool tail, trace_level_t trace)
{
    const int64_t before = xiaotai_time_us();
    const xiaotai_err_t sent = sink->send(sink->ctx, generation, stamp, data, bytes);
    const uint32_t took = elapsed(before, xiaotai_time_us());
    media_trace_frame(true, (uint32_t)(before / 1000), data, bytes, trace.level, trace.pre,
                      sent == XIAOTAI_OK ? 3U : 11U, stamp, packet_ms);
    take(w);
    if (w->tx_generation != generation) {
        give(w);
        return;
    }
    maximum(&w->counters.tx_send_max_us, took);
    if (sent == XIAOTAI_OK) {
        ++w->counters.tx_sent;
        w->counters.tx_preroll += replay;
        w->counters.ptt_tail_packets += tail;
    } else ++w->counters.tx_send_errors;
    if (w->last_send_us) {
        const uint32_t gap = elapsed(w->last_send_us, before) / 1000U;
        w->counters.tx_gaps += gap > 60U;
        w->counters.tx_bursts += gap < 5U;
        maximum(&w->counters.tx_gap_max_ms, gap);
    }
    w->last_send_us = before;
    give(w);
    if (++w->uplink_blocks % 50U == 0) {
        xiaotai_media_stats_t snapshot = {0};
        media_audio_stats(w, &snapshot);
        media_diag_uplink_window(&snapshot);
    }
    if (!w->delegated) xiaotai_delay_ms(1);
}

static void send_data(media_audio_workers_t *w, const media_capture_view_t *view, uint32_t generation, uint32_t epoch,
                      int64_t captured, uint32_t stamp, const uint8_t *data, size_t bytes, bool replay,
                      trace_level_t trace)
{
    bool tail;
    const xiaotai_media_sink_t *sink = view->session.sink;
    if (!sink || (sink->ready && !sink->ready(sink->ctx, generation)) || !allowed(w, generation, epoch, captured, &tail)) return;
    send_admitted(w, sink, generation, stamp, data, bytes, view->session.uplink.packet_ms, replay, tail, trace);
}

static void encoded_send(media_audio_workers_t *w, const media_capture_view_t *view, bool restart)
{
    const xiaotai_media_sink_t *sink = view->session.sink;
    const uint32_t generation = view->session.generation;
    if (!sink || !view->active || !view->gate || view->muted || (sink->ready && !sink->ready(sink->ctx, generation))) {
        xiaotai_delay_ms(5);
        return;
    }
    size_t bytes;
    int64_t now;
    uint32_t lost;
    if (read_input(w, &bytes, &now, &lost) != XIAOTAI_OK) {
        xiaotai_delay_ms(1);
        return;
    }
    media_diag_capture(w->input, NULL, 1);
    bool tail;
    if (!allowed(w, generation, view->uplink_epoch, now, &tail)) return;
    const xiaotai_err_t fed = w->processor->feed(w->state, w->raw, bytes, now, lost);
    if (fed == XIAOTAI_ERR_INVALID_STATE) fail_input(w);
    if (fed != XIAOTAI_OK) return;
    xiaotai_processor_frame_t frame;
    const xiaotai_err_t fetched = w->processor->fetch(w->state, &frame, 0);
    if (fetched == XIAOTAI_ERR_INVALID_STATE) fail_input(w);
    if (fetched != XIAOTAI_OK || !frame.data || !frame.size || !frame.samples || frame.size > w->raw_size) return;
    if (frame.lost_samples) {
        take(w);
        w->counters.capture_lost_samples += frame.lost_samples;
        give(w);
        restart = true;
        if (w->has_resampler) memset(w->resampler, 0, sizeof(*w->resampler));
    }
    const uint32_t stamp = (uint32_t)((frame.captured_us - (int64_t)frame.samples * 1000000 / w->info.rate_hz) / 1000);
    media_tx_codec_t codec;
    if (!media_tx_codec(&view->session.uplink, &codec)) return;
    const bool inject = atomic_load(&w->tone_generation) == generation && atomic_load(&w->tone_ms);
    if (!codec.encoder && !inject) {
        send_data(w, view, generation, view->uplink_epoch, frame.captured_us, stamp, frame.data, frame.size,
                  false, (trace_level_t){0});
    } else {
        size_t count = 0, out = 0;
        if (!w->input_decoder || (restart && w->input_decoder->reset &&
            w->input_decoder->reset(w->input_decoder_state) != XIAOTAI_OK) ||
            w->input_decoder->decode(w->input_decoder_state, frame.data, frame.size, w->clean,
                                     XIAOTAI_PROCESSOR_SAMPLES_MAX, &count) != XIAOTAI_OK || count != frame.samples) {
            take(w);
            ++w->counters.tx_encode_errors;
            give(w);
            return;
        }
        trace_level_t trace = {0};
        if (encode_uplink(w, view, w->clean, count, &out, restart, &trace))
            send_data(w, view, generation, view->uplink_epoch, frame.captured_us, stamp, w->encoded_packet,
                      out, false, trace);
    }
}

static void folded_send(media_audio_workers_t *w)
{
    folded_packet_t *packet = w->fold_consumer;
    if (!xiaotai_queue_receive(&w->tx_queue, packet, 20)) return;
    media_capture_view_t view;
    media_runtime_capture_view(&view);
    const int64_t now = xiaotai_time_us();
    const int64_t captured = now - (uint32_t)((uint32_t)now - packet->captured_clock);
    bool tail;
    if (!allowed(w, packet->generation, packet->epoch, captured, &tail)) return;
    if (!queued_uplink_current(w, (uint32_t)now - packet->enqueued_clock)) return;
    send_data(w, &view, packet->generation, packet->epoch, captured, packet->timestamp_ms,
              packet->data, packet->size, false, packet->trace);
}

bool media_audio_uplink_delegated(const media_audio_workers_t *w)
{
    return w && w->delegated;
}

bool media_audio_uplink_claim(media_audio_workers_t *w)
{
    if (!media_audio_uplink_delegated(w)) return false;
    take(w);
    const bool ready = !atomic_load(&w->dispatching) && !atomic_load(&w->paused) &&
                       !atomic_load(&w->stopping) && !atomic_load(&w->failed);
    if (ready) atomic_store(&w->dispatching, true);
    give(w);
    if (!ready) return false;
    if (xiaotai_queue_receive(&w->tx_queue, w->fold_consumer, 0)) return true;
    atomic_store(&w->dispatching, false);
    return false;
}

bool media_audio_uplink_finish(media_audio_workers_t *w, const media_uplink_view_t *view)
{
    const folded_packet_t *packet = w->fold_consumer;
    const int64_t now = xiaotai_time_us();
    const int64_t captured = now - (uint32_t)((uint32_t)now - packet->captured_clock);
    const bool stale = packet->generation != view->generation || packet->epoch != view->epoch;
    const bool tail = !view->gate && view->tail && captured <= view->tail_end_us && now <= view->tail_deadline_us;
    if (stale || !view->eligible || (!view->gate && !tail)) {
        take(w);
        if (stale) ++w->counters.tx_stale;
        else ++w->counters.tx_gated;
        give(w);
    } else if ((!view->sink->ready || view->sink->ready(view->sink->ctx, packet->generation)) &&
               queued_uplink_current(w, (uint32_t)now - packet->enqueued_clock)) {
        if (w->dispatch_generation != packet->generation) {
            w->last_send_us = 0;
            w->dispatch_generation = packet->generation;
        }
        send_admitted(w, view->sink, packet->generation, packet->timestamp_ms, packet->data, packet->size,
                      view->packet_ms, false, tail, packet->trace);
    }
    const bool pending = !atomic_load(&w->paused) && !atomic_load(&w->stopping) &&
                         xiaotai_queue_count(&w->tx_queue) != 0U;
    atomic_store(&w->dispatching, false);
    return pending;
}

static void uplink_loop(void *arg)
{
    media_audio_workers_t *w = arg;
    uint32_t generation = 0, epoch = 0, input_generation = 0;
    uint8_t volume = UINT8_MAX;
    int8_t gain = w->input->info.gain_db;
    bool reset_encoder = true;
    while (!atomic_load(&w->stopping) && !atomic_load(&w->failed)) {
        if (wait_for_resume(w, &w->sending)) reset_encoder = true;
        if (atomic_load(&w->stopping)) break;
        media_capture_view_t view;
        media_runtime_capture_view(&view);
        const uint32_t active = view.active ? view.session.generation : 0;
        if (active != generation) w->last_send_us = 0;
        if (w->folded) {
            generation = active;
            folded_send(w);
            continue;
        }
        reset_encoder |= active != generation || view.uplink_epoch != epoch;
        tx_session(w, active, &generation, view.uplink_epoch, &epoch);
        if (w->encoded) {
            update_input(w, &view, &input_generation, &gain, &volume);
            encoded_send(w, &view, reset_encoder);
            reset_encoder = false;
            continue;
        }
        tx_frame_t *f = w->consumer;
        if (!xiaotai_queue_receive(&w->tx_queue, f, 20)) continue;
        media_runtime_capture_view(&view);
        bool tail;
        if (!allowed(w, f->generation, f->epoch, f->captured_us, &tail)) continue;
        if (w->sync_tx_on_dequeue && (f->generation != generation || f->epoch != epoch)) {
            /* A new session or capture epoch can bring the frame that ends
             * an empty wait. Reset from the admitted frame, not the pre-wait view. */
            if (f->generation != generation) w->last_send_us = 0;
            tx_session(w, f->generation, &generation, f->epoch, &epoch);
            reset_encoder = true;
        }
        if (!view.session.sink || (view.session.sink->ready && !view.session.sink->ready(view.session.sink->ctx, f->generation))) {
            reset_encoder = true;
            continue;
        }
        if (!queued_uplink_current(w, elapsed(f->enqueued_us, xiaotai_time_us()))) continue;
        size_t bytes = 0;
        trace_level_t trace = {0};
        if (encode_uplink(w, &view, f->pcm, w->tx_samples, &bytes, reset_encoder, &trace)) {
            reset_encoder = false;
            send_data(w, &view, f->generation, f->epoch, f->captured_us, f->timestamp_ms, w->encoded_packet,
                      bytes, f->preroll, trace);
        }
    }
    if (!w->folded) effect_reset(w, XIAOTAI_EFFECT_UPLINK, generation != 0);
}

static void lane_entry(media_audio_workers_t *w, lane_t *lane)
{
    (void)xiaotai_signal_take(&lane->start, XIAOTAI_WAIT_FOREVER);
    if (!atomic_load(&w->stopping)) lane->fn(w);
    atomic_store(&lane->parked, true);
    xiaotai_signal_give(&lane->done);
    xiaotai_task_exit();
}

static void capture_entry(void *w) { lane_entry(w, &((media_audio_workers_t *)w)->capturing); }
static void clean_entry(void *w) { lane_entry(w, &((media_audio_workers_t *)w)->cleaning); }
static void uplink_entry(void *w) { lane_entry(w, &((media_audio_workers_t *)w)->sending); }

static xiaotai_err_t lane_create(media_audio_workers_t *w, lane_t *lane, const char *name,
                                xiaotai_task_fn_t fn, xiaotai_task_fn_t entry, uint32_t stack,
                                int32_t priority, int32_t core)
{
    xiaotai_err_t ret = xiaotai_signal_init(&lane->start);
    if (ret != XIAOTAI_OK) return ret;
    ret = xiaotai_signal_init(&lane->done);
    if (ret == XIAOTAI_OK) {
        lane->fn = fn;
        const xiaotai_task_config_t config = {.name = name, .arg = w, .entry = entry,
            .stack_bytes = stack, .stack = XIAOTAI_MEM_BULK, .priority = (unsigned)(priority < 1 ? 1 : priority), .core = core};
        xiaotai_task_t id;
        ret = xiaotai_task_create(&config, &id);
        if (ret == XIAOTAI_OK) {
            lane->created = true;
            return XIAOTAI_OK;
        }
        xiaotai_signal_deinit(&lane->done);
    }
    xiaotai_signal_deinit(&lane->start);
    return ret;
}

xiaotai_err_t media_audio_workers_create(const xiaotai_audio_input_t *input, media_topology_t topology,
                                         bool delegated, media_audio_workers_t **out)
{
    if (!input || !input->read) return XIAOTAI_OK;
    xiaotai_processor_info_t info;
    void *state;
    const xiaotai_audio_processor_t *processor = media_capture_prepared(&state, &info);
    if (!processor) return XIAOTAI_OK;
    const bool resampler = xiaotai_mem_has_external() || media_prepared_uplink_needs_resample(info.rate_hz);
    const size_t worker_bytes = sizeof(media_audio_workers_t) + (resampler ? sizeof(media_resampler_storage_t) : 0);
    media_audio_workers_t *w = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, worker_bytes);
    if (!w) return XIAOTAI_ERR_NO_MEM;
    xiaotai_critical_init(&w->counters_lock);
    w->input = input;
    w->processor = processor;
    w->state = state;
    w->info = info;
    w->sync_tx_on_dequeue = xiaotai_media_param("uplink.sync_on_dequeue", 0) != 0;
    w->tx_max_queue_age_ms = (uint16_t)xiaotai_media_param(MEDIA_UPLINK_MAX_QUEUE_AGE_KEY, 0);
    w->has_resampler = resampler;
    w->encoded = topology == MEDIA_TOPOLOGY_ENCODED;
    w->folded = topology == MEDIA_TOPOLOGY_FOLDED;
    w->folded_controls = (capture_controls_t){.volume = UINT8_MAX, .gain = input->info.gain_db};
    w->fold_reset_encoder = true;
    w->reset_effects_on_mute = xiaotai_media_param("capture.reset_effects_on_mute", 0) != 0;
    w->account_capture_effect_gaps = xiaotai_media_param("capture.account_effect_gaps", 0) != 0;
    const bool async = !w->encoded && (info.flags & XIAOTAI_PROCESSOR_ASYNC);
    const bool uplink = media_prepared_uplink();
    w->delegated = w->folded && uplink && delegated;
    w->raw_size = w->encoded ? info.fetch_max : info.feed_frames * input->desc.channels * sizeof(int16_t);
    w->tx_samples = info.rate_hz * FRAME_MS / 1000U;
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_SIZE;
    if (!w->raw_size || !w->tx_samples || w->tx_samples > XIAOTAI_PROCESSOR_SAMPLES_MAX) goto failed;
    err = XIAOTAI_ERR_NO_MEM;
    w->raw = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, w->raw_size);
    if (!w->encoded) w->clean = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, info.fetch_max * sizeof(int16_t));
    if (!w->raw || (!w->encoded && !w->clean)) goto failed;
    if (!w->encoded) {
        for (size_t i = 0; i < EFFECT_MAX; ++i) {
            const xiaotai_audio_effect_t *desc = media_effect_at(i);
            if (!desc) break;
            err = open_effect(w, desc);
            if (err != XIAOTAI_OK) goto failed;
        }
        err = open_effect(w, &media_highpass);
        if (err == XIAOTAI_OK) err = open_effect(w, &media_noise_gate);
        if (err != XIAOTAI_OK) goto failed;
    }
    if (uplink) {
        w->input_decoder = media_capture_decoder(&w->input_decoder_state);
        const bool encodes = !w->encoded || w->input_decoder;
        if (encodes) {
            err = XIAOTAI_ERR_NO_MEM;
            w->encoded_capacity = w->folded ? media_max_uplink_packet() : XIAOTAI_MEDIA_PACKET_BYTES_MAX;
            w->network_capacity = w->folded ? media_max_uplink_samples() : XIAOTAI_PROCESSOR_SAMPLES_MAX;
            if (w->folded) {
                const size_t bytes = sizeof(folded_packet_t) + w->encoded_capacity;
                w->fold_producer = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
                w->fold_consumer = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
                if (!w->fold_producer || !w->fold_consumer) goto failed;
                w->encoded_packet = w->fold_producer->data;
            } else {
                w->encoded_packet = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, w->encoded_capacity);
            }
            w->network_pcm = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, w->network_capacity * sizeof(int16_t));
            if (w->encoded) w->clean = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, XIAOTAI_PROCESSOR_SAMPLES_MAX * sizeof(int16_t));
            if (!w->encoded_packet || !w->network_pcm || !w->clean) goto failed;
        }
        if (!w->encoded) {
            const bool external = xiaotai_mem_has_external();
            const size_t bytes = sizeof(tx_frame_t) + w->tx_samples * sizeof(int16_t);
            err = XIAOTAI_ERR_NO_MEM;
            w->producer = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
            if (!w->folded) {
                w->consumer = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
                if (external) w->replay = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, w->tx_samples * sizeof(int16_t));
                if (!w->consumer || (external && !w->replay)) goto failed;
            }
            if (!w->producer) goto failed;
            const size_t item = w->folded ? sizeof(folded_packet_t) + w->encoded_capacity : bytes;
            err = xiaotai_queue_init(&w->tx_queue, external ? 12U : 4U, item, XIAOTAI_MEM_BULK);
            if (err != XIAOTAI_OK) goto failed;
            w->tx_queue_ready = true;
        }
        if (!w->delegated) {
            const uint32_t sink = media_uplink_sink_stack();
            const uint64_t stack = w->folded ? UINT64_C(1024) + sink :
                UINT64_C(2048) + sink + media_max_encoder_stack() + w->effect_stack[XIAOTAI_EFFECT_UPLINK] +
                (w->encoded ? (uint64_t)info.feed_stack_bytes + info.fetch_stack_bytes : 0) +
                (w->input_decoder ? w->input_decoder->stack_bytes : 0);
            if (stack > UINT32_MAX) { err = XIAOTAI_ERR_INVALID_SIZE; goto failed; }
            err = lane_create(w, &w->sending, "xt_uplink", uplink_loop, uplink_entry, (uint32_t)stack,
                               xiaotai_media_param("task.uplink.priority", 7), xiaotai_media_param("task.uplink.core", XIAOTAI_CORE_ANY));
            if (err != XIAOTAI_OK) goto failed;
        }
    }
    if (!w->encoded && !w->folded) {
        const uint64_t capture_stack = UINT64_C(4096) + info.feed_stack_bytes +
            (!async ? (uint64_t)2048 + info.fetch_stack_bytes + w->effect_stack[XIAOTAI_EFFECT_CAPTURE] : 0);
        const uint64_t clean_stack = UINT64_C(6144) + info.fetch_stack_bytes + w->effect_stack[XIAOTAI_EFFECT_CAPTURE];
        if (capture_stack > UINT32_MAX || clean_stack > UINT32_MAX) { err = XIAOTAI_ERR_INVALID_SIZE; goto failed; }
        err = lane_create(w, &w->capturing, "xt_capture", capture_loop, capture_entry, (uint32_t)capture_stack,
                           xiaotai_media_param("task.capture.priority", 9), xiaotai_media_param("task.capture.core", 1));
        if (err != XIAOTAI_OK) goto failed;
        if (async) {
            err = lane_create(w, &w->cleaning, "xt_clean", clean_loop, clean_entry, (uint32_t)clean_stack,
                               xiaotai_media_param("task.clean.priority", 7), xiaotai_media_param("task.clean.core", 0));
            if (err != XIAOTAI_OK) goto failed;
        }
    }
    *out = w;
    return XIAOTAI_OK;
failed:
    media_audio_workers_destroy(w);
    return err;
}

void media_audio_workers_start(media_audio_workers_t *w)
{
    if (!w) return;
    lane_t *const lanes[] = {&w->cleaning, &w->capturing, &w->sending};
    for (size_t i = 0; i < sizeof(lanes) / sizeof(*lanes); ++i)
        if (lanes[i]->created) xiaotai_signal_give(&lanes[i]->start);
}

void media_audio_workers_pause(media_audio_workers_t *w, bool paused)
{
    if (!w) return;
    if (w->delegated) take(w);
    const bool previous = atomic_exchange(&w->paused, paused);
    if (w->delegated) give(w);
    if (w->folded && paused && !previous) {
        close_taps(w);
        atomic_store(&w->fold_capture_gap, true);
    }
    if (w->delegated && previous && !paused && xiaotai_queue_count(&w->tx_queue)) media_runtime_uplink_notify();
}

uint64_t media_audio_folded_stack(const media_audio_workers_t *w, uint32_t decoder_stack)
{
    if (!w || !w->folded) return 0;
    const uint64_t capture = (uint64_t)w->info.feed_stack_bytes + w->info.fetch_stack_bytes +
                             w->effect_stack[XIAOTAI_EFFECT_CAPTURE];
    const uint64_t encode = (uint64_t)media_max_encoder_stack() + w->effect_stack[XIAOTAI_EFFECT_UPLINK];
    uint64_t largest = capture > encode ? capture : encode;
    if (decoder_stack > largest) largest = decoder_stack;
    /* The compact folded worker leaves 2560 bytes for the loop and the port
     * underneath plugins that declare at most 1024 bytes on any path. A
     * larger declaration retains the full legacy worker budget. */
    if (largest <= 1024U && !xiaotai_mem_has_external() &&
            xiaotai_media_param(MEDIA_COMPACT_CAPTURE_STACK_KEY, 0) == 1) return 3584U;
    const uint64_t stack = 5120U + largest;
    return stack < 6144U ? 6144U : stack;
}

bool media_audio_workers_parked(const media_audio_workers_t *w)
{
    return !w || ((!w->capturing.created || atomic_load(&w->capturing.parked)) &&
                  (!w->cleaning.created || atomic_load(&w->cleaning.parked)) &&
                  (!w->sending.created || atomic_load(&w->sending.parked)) &&
                  (!w->delegated || !atomic_load(&w->dispatching)));
}

void media_audio_workers_stop(media_audio_workers_t *w)
{
    if (!w) return;
    if (w->delegated) {
        take(w);
        atomic_store(&w->stopping, true);
        give(w);
        while (atomic_load(&w->dispatching)) xiaotai_delay_ms(1);
    } else atomic_store(&w->stopping, true);
    media_audio_workers_start(w);
    lane_t *const lanes[] = {&w->cleaning, &w->capturing, &w->sending};
    for (size_t i = 0; i < sizeof(lanes) / sizeof(*lanes); ++i) {
        if (!lanes[i]->created) continue;
        (void)xiaotai_signal_take(&lanes[i]->done, XIAOTAI_WAIT_FOREVER);
        xiaotai_signal_deinit(&lanes[i]->done);
        xiaotai_signal_deinit(&lanes[i]->start);
        lanes[i]->created = false;
    }
    if (w->folded) {
        close_taps(w);
        if (w->fold_tx_generation) {
            effect_reset(w, XIAOTAI_EFFECT_UPLINK, true);
            w->fold_tx_generation = 0;
        }
    }
}

void media_audio_workers_destroy(media_audio_workers_t *w)
{
    if (!w) return;
    media_audio_workers_stop(w);
    if (w->tx_queue_ready) xiaotai_queue_deinit(&w->tx_queue);
    while (w->effect_count) {
        const effect_t *e = &w->effects[--w->effect_count];
        e->desc->close(e->state);
    }
    xiaotai_mem_free(w->raw);
    xiaotai_mem_free(w->clean);
    xiaotai_mem_free(w->replay);
    if (!w->folded) xiaotai_mem_free(w->encoded_packet);
    xiaotai_mem_free(w->fold_producer);
    xiaotai_mem_free(w->fold_consumer);
    xiaotai_mem_free(w->network_pcm);
    xiaotai_mem_free(w->producer);
    xiaotai_mem_free(w->consumer);
    xiaotai_mem_free(w);
}

void media_audio_far_end(media_audio_workers_t *w, const int16_t *pcm, size_t frames, uint32_t rate)
{
    if (w && (w->info.flags & XIAOTAI_PROCESSOR_FAR_END)) w->processor->far_end(w->state, pcm, frames, rate);
}

void media_audio_control_stats(media_audio_workers_t *w, xiaotai_media_status_t *status)
{
    if (!w) return;
    status->voice_level = atomic_load(&w->published_voice);
    status->voice_active = status->voice_level >= 2;
}

void media_audio_tone(media_audio_workers_t *w, uint32_t generation, uint32_t ms)
{
    if (!w) return;
    atomic_store(&w->tone_generation, generation);
    atomic_store(&w->tone_ms, ms);
}

void media_audio_tx_reset(media_audio_workers_t *w, uint32_t generation)
{
    if (!w) return;
    take(w);
    w->tx_generation = generation;
#define CLEAR(field) w->counters.field = 0
    CLEAR(tx_sent); CLEAR(tx_full); CLEAR(tx_stale); CLEAR(tx_gated); CLEAR(tx_effect_failed);
    CLEAR(tx_encode_errors); CLEAR(tx_send_errors); CLEAR(tx_preroll); CLEAR(tx_injected);
    CLEAR(tx_queue_age_max_ms); CLEAR(tx_encode_max_us); CLEAR(tx_send_max_us);
    CLEAR(tx_gaps); CLEAR(tx_bursts); CLEAR(tx_gap_max_ms); CLEAR(ptt_tail_packets); CLEAR(pcm_in_hash);
    CLEAR(uplink_peak); CLEAR(uplink_rms); CLEAR(uplink_clipped);
#undef CLEAR
    give(w);
}

void media_audio_stats(media_audio_workers_t *w, xiaotai_media_stats_t *dst)
{
    if (!w) return;
    take(w);
    const xiaotai_media_stats_t s = w->counters;
    give(w);
#define COPY(field) dst->field = s.field
    COPY(tx_sent); COPY(tx_full); COPY(tx_stale); COPY(tx_gated); COPY(tx_effect_failed);
    COPY(tx_encode_errors); COPY(tx_send_errors); COPY(tx_preroll); COPY(tx_injected);
    COPY(tx_queue_age_max_ms); COPY(tx_encode_max_us); COPY(tx_send_max_us);
    COPY(tx_gaps); COPY(tx_bursts); COPY(tx_gap_max_ms); COPY(ptt_tail_packets); COPY(pcm_in_hash);
    COPY(capture_errors); COPY(capture_lost_samples); COPY(capture_overruns);
    COPY(capture_deadline_misses); COPY(capture_process_max_us); COPY(mic_peak); COPY(mic_rms);
    COPY(reference_peak); COPY(reference_rms); COPY(monitor_peak); COPY(monitor_rms); COPY(clean_peak);
    COPY(clean_rms); COPY(uplink_peak); COPY(uplink_rms); COPY(uplink_clipped);
#undef COPY
    dst->tx_dropped = s.tx_full + s.tx_stale + s.tx_gated + s.tx_effect_failed + s.tx_encode_errors + s.tx_send_errors;
}
