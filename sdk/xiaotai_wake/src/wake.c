// SPDX-License-Identifier: MIT
/* Wake-word engine core (xiaotai/wake.h): the task, model loading with
 * retries, the audio hand-off from the media tap, the latest window, scoring,
 * the acceptance rules, delivery to the owner task, listening, the threshold
 * and the counters.
 *
 * Threads:
 *   capture task  tap_gate() and tap_pcm(): atomics and the lock-free queue
 *                 only; never locks, allocates, logs or scores.
 *   engine task   wake_engine_step() in a loop: the only caller of the model,
 *                 the only owner of the window buffers, the snapshot and the
 *                 pending detection. Its stack may be external RAM, so it
 *                 never touches flash or storage.
 *   owner task    listen, threshold, the delivery job and the subscribers.
 * The state lock guards the window, the counters, arming, cooldown, the
 * threshold and the posted detections; scoring and delivery run outside it. */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"
#include "wake_queue.h"
#include "wake_window.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/wake.h"
#include "xiaotai/wake_config.h"

_Static_assert(WAKE_QUEUE_BLOCK_SAMPLES == XIAOTAI_PROCESSOR_SAMPLES_MAX,
               "a queue block must hold the largest clean frame a processor returns");

#define TAG "wake"

/* ------------------------------------------------------------ registrations */

static const xiaotai_wake_model_t *s_model;
static struct {
    xiaotai_wake_fn_t handler;
    void *ctx;
} s_subscribers[WAKE_SUBSCRIBERS_MAX];
static size_t s_subscriber_count;
static bool s_initialized;
/* The task (or a test's first step) runs: the model is fixed from here on. */
static atomic_bool s_started;

/* ---------------------------------------------- shared with the capture task */

static atomic_bool s_ready;          /* release-publishes s_queue and s_rate_hz */
static atomic_bool s_wanted;         /* xiaotai_wake_listen(true) in force */
static atomic_bool s_gate_open;      /* the media engine lets audio through */
static atomic_bool s_gap;            /* the producer lost a frame: restart the window */
static atomic_uint s_input_generation;
static atomic_uint s_dropped, s_wrong_rate;
static atomic_int s_init_error;
static wake_queue_t *s_queue;
static uint32_t s_rate_hz;

static void tap_gate(void *ctx, bool open);
static void tap_pcm(void *ctx, const int16_t *pcm, size_t samples, uint32_t rate_hz, int64_t captured_us);
/* The flags are set at init from the tuning table; the descriptor stays put
 * for the media engine afterwards. */
static xiaotai_media_tap_t s_tap = {.name = "wake_engine", .gate = tap_gate, .pcm = tap_pcm};

/* -------------------------------------------------------- under the state lock */

static xiaotai_mutex_t s_lock;
static wake_window_t s_window;
static uint32_t s_window_input_generation;
static xiaotai_wake_status_t s_status;   /* the counters; flags and atomics are filled in when read */
static int64_t s_armed_us, s_cooldown_us;
static uint64_t s_detect_total_us;
static uint32_t s_sequence;
static xiaotai_wake_detection_t s_posted[WAKE_POSTED_MAX];
static size_t s_posted_head, s_posted_count;

/* ------------------------------------------------------------ engine task only */

static void *s_model_state;
static int16_t *s_ring, *s_snapshot;
static uint32_t s_retry_ms = WAKE_RETRY_FIRST_MS;
static int64_t s_next_load_us;
static xiaotai_wake_detection_t s_pending;
static bool s_pending_valid;
static wake_engine_memory_t s_memory;

static xiaotai_task_t s_task;
static xiaotai_task_config_t s_task_config;
static xiaotai_signal_t s_stopped;
static atomic_bool s_stop;
static bool s_task_running;

static int64_t (*s_clock)(void) = xiaotai_time_us;

/* ------------------------------------------------------------------- helpers */

static uint16_t default_threshold(void)
{
    int32_t value = xiaotai_media_param(WAKE_TUNE_THRESHOLD, (int32_t)XIAOTAI_WAKE_THRESHOLD_DEFAULT_MILLI);
    if (value < (int32_t)XIAOTAI_WAKE_THRESHOLD_MIN_MILLI || value > (int32_t)XIAOTAI_WAKE_THRESHOLD_MAX_MILLI) {
        return (uint16_t)XIAOTAI_WAKE_THRESHOLD_DEFAULT_MILLI;
    }
    return (uint16_t)value;
}

static bool accepting(void)
{
    return atomic_load(&s_wanted) && atomic_load(&s_gate_open);
}

/* Probability in thousandths: rounded down (bias 0) or to nearest (bias 0.5),
 * saturated, 0 for what is not a positive number. */
static uint16_t to_milli(float probability, float bias)
{
    float value = probability * 1000.0f + bias;
    if (!(value > 0.0f)) return 0;
    if (value >= 65535.0f) return UINT16_MAX;
    return (uint16_t)value;
}

static uint32_t ms_until(int64_t delta_us)
{
    if (delta_us <= 0) return 1U;
    int64_t ms = (delta_us + 999) / 1000;
    return ms > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

/* A new window: frames queued before now carry an older input generation and
 * are dropped by the consumer. The producer's indices are never touched. */
static void restart_window_locked(void)
{
    s_window_input_generation = atomic_fetch_add(&s_input_generation, 1U) + 1U;
    wake_window_reset(&s_window);
    ++s_status.resets;
}

/* ------------------------------------------------------- capture task (tap) */

/* [rt] Every change of the gate is a new input generation, so audio from
 * before a gap, a mute or a session is never joined to audio after it. */
static void tap_gate(void *ctx, bool open)
{
    (void)ctx;
    if (atomic_exchange(&s_gate_open, open) != open) atomic_fetch_add(&s_input_generation, 1U);
}

/* [rt] Copies one frame into the queue: no lock, no allocation, no log. */
static void tap_pcm(void *ctx, const int16_t *pcm, size_t samples, uint32_t rate_hz, int64_t captured_us)
{
    (void)ctx;
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) return;
    if (!accepting()) return;
    if (rate_hz != s_rate_hz) {
        atomic_fetch_add(&s_wrong_rate, 1U);
        /* These samples cannot join the window. Start a new generation now
         * so the next valid frame is kept even before the consumer polls. */
        atomic_fetch_add(&s_input_generation, 1U);
        return;
    }
    if (!pcm || samples == 0) return;
    uint32_t generation = atomic_load(&s_input_generation);
    /* A frame that cannot be queued is a hole in the audio: the consumer
     * restarts the window instead of joining across it. */
    if (captured_us <= 0 || !wake_queue_push(s_queue, pcm, samples, captured_us, generation)) {
        atomic_store(&s_gap, true);
        atomic_fetch_add(&s_dropped, 1U);
    }
}

/* ---------------------------------------------------------------- delivery */

/* [owner] One job per posted detection, run in posting order. */
static void deliver_job(void *ctx)
{
    (void)ctx;
    xiaotai_wake_detection_t detection;
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (s_posted_count == 0) {
        xiaotai_mutex_unlock(&s_lock);
        return;
    }
    detection = s_posted[s_posted_head];
    s_posted_head = (s_posted_head + 1U) % WAKE_POSTED_MAX;
    --s_posted_count;
    xiaotai_mutex_unlock(&s_lock);
    for (size_t i = 0; i < s_subscriber_count; ++i) s_subscribers[i].handler(&detection, s_subscribers[i].ctx);
}

static void count_lost(void)
{
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    ++s_status.detections_lost;
    xiaotai_mutex_unlock(&s_lock);
}

/* Hands the pending detection to the owner task, or keeps it for the next
 * poll while the owner's queue is full. Once its window ended more than the
 * age limit ago the pre-roll can no longer hold the audio before the phrase,
 * so it is dropped and counted. */
static void post_pending(int64_t now_us)
{
    if (!s_pending_valid) return;
    if (now_us - s_pending.captured_us > WAKE_RESULT_AGE_MAX_US) {
        s_pending_valid = false;
        count_lost();
        return;
    }
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (s_posted_count == WAKE_POSTED_MAX) {
        xiaotai_mutex_unlock(&s_lock);
        return;
    }
    s_posted[(s_posted_head + s_posted_count) % WAKE_POSTED_MAX] = s_pending;
    ++s_posted_count;
    xiaotai_mutex_unlock(&s_lock);
    if (xiaotai_run(deliver_job, NULL) != XIAOTAI_OK) {
        /* Jobs already queued pop older entries from the head, so the tail
         * is still the one just added. */
        xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
        --s_posted_count;
        xiaotai_mutex_unlock(&s_lock);
        return;
    }
    s_pending_valid = false;
}

static void hold_detection(const xiaotai_wake_detection_t *detection)
{
    /* Only possible when the owner stayed blocked for a whole cooldown: the
     * newer detection wins. */
    if (s_pending_valid) count_lost();
    s_pending = *detection;
    s_pending_valid = true;
    post_pending(s_clock());
}

/* ------------------------------------------------------------------ loading */

static void release_buffers(void)
{
    xiaotai_mem_free(s_ring);
    xiaotai_mem_free(s_snapshot);
    xiaotai_mem_free(s_queue);
    s_ring = s_snapshot = NULL;
    s_queue = NULL;
    memset(&s_memory, 0, sizeof(s_memory));
}

/* Takes every buffer, opens the model and scores one silent window (the
 * self-test exercises the real operators; its score never counts). On any
 * failure everything taken is released. */
static xiaotai_err_t load_model(void)
{
    const xiaotai_wake_model_t *model = s_model;
    size_t ring_bytes = (size_t)wake_window_capacity(model->window_samples, model->grid_samples) * sizeof(int16_t);
    size_t snapshot_bytes = (size_t)model->window_samples * sizeof(int16_t);
    /* All of it may live in external RAM: only the engine task reads it, and
     * the contiguous internal memory stays with the transports. */
    s_ring = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, ring_bytes);
    s_snapshot = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, snapshot_bytes);
    s_queue = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(wake_queue_t));
    xiaotai_err_t err = (!s_ring || !s_snapshot || !s_queue) ? XIAOTAI_ERR_NO_MEM : model->open(&s_model_state);
    if (err == XIAOTAI_OK) {
        memset(s_snapshot, 0, snapshot_bytes);
        float probability = 0.0f;
        xiaotai_wake_timing_t timing = {0};
        int64_t started_us = s_clock();
        err = model->detect(s_model_state, s_snapshot, 0U, model->window_samples, &probability, &timing);
        if (err == XIAOTAI_OK && !isfinite(probability)) err = XIAOTAI_FAIL;
        XIAOTAI_LOGD(TAG, "self-test of %s: err=0x%x silence=%.3f elapsed=%ld ms features/inference/head=%u/%u/%u us",
                     model->name, (unsigned)err, (double)probability, (long)((s_clock() - started_us) / 1000),
                     (unsigned)timing.features_us, (unsigned)timing.inference_us, (unsigned)timing.head_us);
        if (err != XIAOTAI_OK) {
            model->close(s_model_state);
            s_model_state = NULL;
        }
    }
    if (err != XIAOTAI_OK) {
        release_buffers();
        return err;
    }
    wake_queue_init(s_queue);
    s_memory.ring_bytes = ring_bytes;
    s_memory.snapshot_bytes = snapshot_bytes;
    s_memory.queue_bytes = sizeof(wake_queue_t);
    s_memory.bulk_bytes = ring_bytes + snapshot_bytes + sizeof(wake_queue_t);
    return XIAOTAI_OK;
}

static uint32_t load_step(int64_t now_us)
{
    if (now_us < s_next_load_us) return ms_until(s_next_load_us - now_us);
    const xiaotai_wake_model_t *model = s_model;
    xiaotai_err_t err = load_model();
    atomic_store(&s_init_error, err);
    int64_t done_us = s_clock();
    if (err != XIAOTAI_OK) {
        uint32_t wait_ms = s_retry_ms;
        XIAOTAI_LOGE(TAG, "%s failed to load (err 0x%x); retry in %u ms; conversations still start by hand",
                     model->name, (unsigned)err, (unsigned)wait_ms);
        s_next_load_us = done_us + (int64_t)wait_ms * 1000;
        s_retry_ms = wait_ms < WAKE_RETRY_LAST_MS / 2U ? wait_ms * 2U : WAKE_RETRY_LAST_MS;
        return wait_ms;
    }
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    wake_window_init(&s_window, s_ring, model->window_samples, model->grid_samples, model->stride_samples);
    s_armed_us = done_us + WAKE_ARM_US;
    restart_window_locked();
    uint16_t threshold = s_status.threshold_milli;
    xiaotai_mutex_unlock(&s_lock);
    s_rate_hz = model->rate_hz;
    atomic_store_explicit(&s_ready, true, memory_order_release);
    XIAOTAI_LOGI(TAG, "wake word %s ready, threshold %u/1000", model->name, (unsigned)threshold);
    XIAOTAI_LOGD(TAG, "window=%u grid=%u stride=%u rate=%u queue=%u bytes in %u blocks",
                 (unsigned)model->window_samples, (unsigned)model->grid_samples, (unsigned)model->stride_samples,
                 (unsigned)model->rate_hz, (unsigned)sizeof(wake_queue_t), (unsigned)WAKE_QUEUE_BLOCKS);
    return WAKE_POLL_MS;
}

/* ------------------------------------------------------------------ scoring */

static void score_step(int64_t now_us)
{
    const xiaotai_wake_model_t *model = s_model;
    int64_t captured_us = 0;
    uint32_t generation = 0;
    uint64_t end_sample = 0;

    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    if (atomic_exchange(&s_gap, false)) restart_window_locked();
    uint32_t input_generation = atomic_load(&s_input_generation);
    if (s_window_input_generation != input_generation) {
        s_window_input_generation = input_generation;
        wake_window_reset(&s_window);
        ++s_status.resets;
    }
    bool listening = accepting();
    /* A bounded backlog per pass; whatever is older than the newest window
     * is cheap to append and never scored on its own. */
    for (unsigned n = 0; n < WAKE_QUEUE_BLOCKS; ++n) {
        const wake_block_t *block = wake_queue_peek(s_queue);
        if (!block) break;
        if (block->generation != input_generation || !listening || block->captured_us < s_armed_us ||
            block->captured_us < s_cooldown_us) {
            ++s_status.skipped_unarmed;
        } else {
            uint32_t previous = s_window.generation;
            wake_window_append(&s_window, block->pcm, block->samples, block->captured_us);
            if (previous != s_window.generation) {
                ++s_status.resets;
                ++s_status.stale;
            }
            ++s_status.fed;
        }
        wake_queue_release(s_queue);
    }
    bool available = listening && now_us >= s_armed_us && now_us >= s_cooldown_us &&
                     wake_window_snapshot(&s_window, s_snapshot);
    if (available) {
        captured_us = wake_window_snapshot_us(&s_window, model->rate_hz);
        generation = s_window.generation;
        end_sample = s_window.snapshot_end;
    }
    xiaotai_mutex_unlock(&s_lock);
    if (!available) return;

    float probability = 0.0f;
    xiaotai_wake_timing_t timing = {0};
    int64_t started_us = s_clock();
    xiaotai_err_t err = model->detect(s_model_state, s_snapshot, generation, end_sample, &probability, &timing);
    int64_t elapsed64 = s_clock() - started_us;
    uint32_t elapsed_us = elapsed64 < 0 ? 0U : elapsed64 > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed64;
    if (err == XIAOTAI_OK && !isfinite(probability)) err = XIAOTAI_FAIL;
    uint64_t stride_us = (uint64_t)model->stride_samples * 1000000U / model->rate_hz;

    bool accepted = false;
    xiaotai_wake_detection_t detection = {0};
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    int64_t now = s_clock();
    ++s_status.inferences;
    s_detect_total_us += elapsed_us;
    s_status.detect_avg_us = (uint32_t)(s_detect_total_us / s_status.inferences);
    if (elapsed_us > s_status.detect_max_us) s_status.detect_max_us = elapsed_us;
    if (timing.features_us > s_status.features_max_us) s_status.features_max_us = timing.features_us;
    if (timing.inference_us > s_status.inference_max_us) s_status.inference_max_us = timing.inference_us;
    if (timing.head_us > s_status.head_max_us) s_status.head_max_us = timing.head_us;
    s_status.reused_frames += timing.reused_frames;
    if (err == XIAOTAI_OK) s_status.computed_frames += timing.computed_frames;
    if (elapsed_us > stride_us) ++s_status.detect_over_budget;
    uint32_t age_ms = now > captured_us ? (uint32_t)((now - captured_us) / 1000) : 0U;
    if (age_ms > s_status.window_age_max_ms) s_status.window_age_max_ms = age_ms;
    if (err != XIAOTAI_OK) {
        ++s_status.reject_error;
    } else {
        s_status.last_score_milli = to_milli(probability, 0.0f);
        s_status.last_score_us = now;
        if (probability >= (float)s_status.threshold_milli / 1000.0f) {
            ++s_status.raw_hits;
            bool changed = atomic_load(&s_gap) || input_generation != atomic_load(&s_input_generation);
            switch (wake_verdict(accepting(), changed, generation, s_window.generation, captured_us, now,
                                 s_cooldown_us)) {
            case WAKE_REJECT_STREAM:
                ++s_status.reject_stream;
                break;
            case WAKE_REJECT_AGE:
                ++s_status.reject_age;
                break;
            case WAKE_REJECT_COOLDOWN:
                ++s_status.skipped_cooldown;
                break;
            case WAKE_ACCEPT:
                ++s_status.detections;
                s_cooldown_us = now + WAKE_COOLDOWN_US;
                restart_window_locked();
                if (++s_sequence == 0) s_sequence = 1;
                detection.sequence = s_sequence;
                detection.captured_us = captured_us;
                detection.score_milli = to_milli(probability, 0.5f);
                detection.threshold_milli = s_status.threshold_milli;
                accepted = true;
                break;
            }
        }
    }
    xiaotai_mutex_unlock(&s_lock);
    if (accepted) {
        XIAOTAI_LOGI(TAG, "detected #%u: probability=%.3f threshold=%u/1000 scoring=%u ms",
                     (unsigned)detection.sequence, (double)probability, (unsigned)detection.threshold_milli,
                     (unsigned)(elapsed_us / 1000U));
        hold_detection(&detection);
    }
}

/* -------------------------------------------------------------------- task */

uint32_t wake_engine_step(void)
{
    atomic_store(&s_started, true);
    if (!s_initialized || !s_model) return WAKE_POLL_MS;
    int64_t now_us = s_clock();
    if (!atomic_load(&s_ready)) return load_step(now_us);
    post_pending(now_us);
    score_step(now_us);
    return WAKE_POLL_MS;
}

static void wake_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_stop)) {
        uint32_t wait_ms = wake_engine_step();
        /* A plain delay in production: only xiaotai_wake_reset notifies. */
        (void)xiaotai_task_wait_notify(wait_ms);
    }
    xiaotai_signal_give(&s_stopped);
    xiaotai_task_exit();
}

/* ---------------------------------------------------------------- public API */

xiaotai_err_t xiaotai_wake_model_register(const xiaotai_wake_model_t *model)
{
    if (atomic_load(&s_started)) return XIAOTAI_ERR_INVALID_STATE;
    if (!model || !model->name || !model->open || !model->detect || !model->close || model->rate_hz == 0 ||
        model->window_samples == 0 || model->grid_samples == 0 || model->stride_samples == 0 ||
        model->stack_bytes == 0 || model->stride_samples > model->window_samples ||
        model->grid_samples > model->window_samples) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    s_model = model;
    return XIAOTAI_OK;
}

const xiaotai_wake_model_t *xiaotai_wake_model(void) { return s_model; }

xiaotai_err_t xiaotai_wake_subscribe(xiaotai_wake_fn_t handler, void *ctx)
{
    if (!handler) return XIAOTAI_ERR_INVALID_ARG;
    if (s_subscriber_count == WAKE_SUBSCRIBERS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_subscribers[s_subscriber_count].handler = handler;
    s_subscribers[s_subscriber_count].ctx = ctx;
    ++s_subscriber_count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_wake_listen(bool enabled)
{
    if (!s_initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (enabled) {
        if (!s_model) return XIAOTAI_ERR_NOT_SUPPORTED;
        xiaotai_media_info_t info;
        xiaotai_media_info(&info);
        /* The engine never resamples: another known capture rate can never
         * reach the model. */
        if (!info.capture_pcm) return XIAOTAI_ERR_NOT_SUPPORTED;
        if (info.capture_rate_hz != 0 && info.capture_rate_hz != s_model->rate_hz) return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    bool was = atomic_load(&s_wanted);
    if (was == enabled) return XIAOTAI_OK;
    /* Wanted before the tap opens, so the first frame after the gate is not
     * refused; every change is a new window. */
    atomic_store(&s_wanted, enabled);
    atomic_fetch_add(&s_input_generation, 1U);
    xiaotai_err_t err = xiaotai_media_tap_enable(&s_tap, enabled);
    if (err != XIAOTAI_OK) {
        atomic_store(&s_wanted, was);
        atomic_fetch_add(&s_input_generation, 1U);
    }
    return err;
}

xiaotai_err_t xiaotai_wake_set_threshold(uint16_t threshold_milli)
{
    if (threshold_milli < XIAOTAI_WAKE_THRESHOLD_MIN_MILLI || threshold_milli > XIAOTAI_WAKE_THRESHOLD_MAX_MILLI) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    s_status.threshold_milli = threshold_milli;
    restart_window_locked();
    xiaotai_mutex_unlock(&s_lock);
    return XIAOTAI_OK;
}

void xiaotai_wake_status(xiaotai_wake_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!s_initialized) {
        status->threshold_milli = default_threshold();
        return;
    }
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    *status = s_status;
    int64_t armed_us = s_armed_us, cooldown_us = s_cooldown_us;
    xiaotai_mutex_unlock(&s_lock);
    int64_t now_us = s_clock();
    status->ready = atomic_load(&s_ready);
    status->wanted = atomic_load(&s_wanted);
    status->listening = status->ready && accepting() && now_us >= armed_us && now_us >= cooldown_us;
    status->init_error = atomic_load(&s_init_error);
    status->dropped = atomic_load(&s_dropped);
    status->wrong_rate = atomic_load(&s_wrong_rate);
}

void wake_engine_diagnostics(wake_diagnostics_t *sample)
{
    xiaotai_wake_status(&sample->status);
    sample->now_us = s_clock();
    sample->raw = (s_tap.flags & XIAOTAI_TAP_RAW) != 0U;
}

xiaotai_err_t xiaotai_engine_wake_engine_init(void)
{
    if (s_initialized) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = xiaotai_mutex_init(&s_lock);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_signal_init(&s_stopped);
    if (err != XIAOTAI_OK) {
        xiaotai_mutex_deinit(&s_lock);
        return err;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.threshold_milli = default_threshold();
    s_tap.flags = xiaotai_media_param(WAKE_TUNE_RAW, 0) != 0 ? XIAOTAI_TAP_RAW : 0U;
    err = xiaotai_media_tap_register(&s_tap);
#if XIAOTAI_WAKE_BUILTIN_MODEL
    if (err == XIAOTAI_OK) err = wake_builtin_model_register();
#endif
    if (err == XIAOTAI_OK) err = wake_diagnostics_register();
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "init failed: 0x%x", (unsigned)err);
        xiaotai_signal_deinit(&s_stopped);
        xiaotai_mutex_deinit(&s_lock);
        return err;
    }
    s_initialized = true;
    return XIAOTAI_OK;
}

void xiaotai_engine_wake_engine_start(void)
{
    if (!s_initialized || atomic_load(&s_started)) return;
    atomic_store(&s_started, true);
    if (!s_model) {
        XIAOTAI_LOGI(TAG, "no wake model registered: local wake is unavailable");
        return;
    }
    int32_t priority = xiaotai_media_param(WAKE_TUNE_PRIORITY, WAKE_TASK_PRIORITY_DEFAULT);
    int32_t core = xiaotai_media_param(WAKE_TUNE_CORE, WAKE_TASK_CORE_DEFAULT);
    s_task_config = (xiaotai_task_config_t){
        .name = WAKE_TASK_NAME,
        .entry = wake_task,
        .arg = NULL,
        .stack_bytes = s_model->stack_bytes,
        /* The task never touches flash or storage, so its stack may be
         * external RAM. */
        .stack = XIAOTAI_MEM_BULK,
        .priority = priority < 0 ? (unsigned)WAKE_TASK_PRIORITY_DEFAULT : (unsigned)priority,
        .core = core < 0 ? XIAOTAI_CORE_ANY : (int)core,
    };
    xiaotai_err_t err = xiaotai_task_create(&s_task_config, &s_task);
    if (err != XIAOTAI_OK) {
        atomic_store(&s_init_error, XIAOTAI_ERR_NO_MEM);
        XIAOTAI_LOGE(TAG, "task not created (0x%x): local wake is unavailable", (unsigned)err);
        return;
    }
    s_task_running = true;
}

void xiaotai_wake_reset(void)
{
    if (s_task_running) {
        atomic_store(&s_stop, true);
        xiaotai_task_notify(&s_task);
        xiaotai_signal_take(&s_stopped, XIAOTAI_WAIT_FOREVER);
        s_task_running = false;
    }
    if (s_model_state && s_model) s_model->close(s_model_state);
    s_model_state = NULL;
    release_buffers();
    if (s_initialized) {
        xiaotai_signal_deinit(&s_stopped);
        xiaotai_mutex_deinit(&s_lock);
    }
    s_model = NULL;
    memset(s_subscribers, 0, sizeof(s_subscribers));
    s_subscriber_count = 0;
    s_initialized = false;
    atomic_store(&s_started, false);
    atomic_store(&s_ready, false);
    atomic_store(&s_wanted, false);
    atomic_store(&s_gate_open, false);
    atomic_store(&s_gap, false);
    atomic_store(&s_stop, false);
    atomic_store(&s_input_generation, 0U);
    atomic_store(&s_dropped, 0U);
    atomic_store(&s_wrong_rate, 0U);
    atomic_store(&s_init_error, XIAOTAI_OK);
    s_rate_hz = 0;
    s_tap.flags = 0;
    memset(&s_window, 0, sizeof(s_window));
    s_window_input_generation = 0;
    memset(&s_status, 0, sizeof(s_status));
    s_armed_us = s_cooldown_us = 0;
    s_detect_total_us = 0;
    s_sequence = 0;
    memset(s_posted, 0, sizeof(s_posted));
    s_posted_head = s_posted_count = 0;
    s_retry_ms = WAKE_RETRY_FIRST_MS;
    s_next_load_us = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending_valid = false;
    memset(&s_task, 0, sizeof(s_task));
    memset(&s_task_config, 0, sizeof(s_task_config));
    s_clock = xiaotai_time_us;
}

/* ------------------------------------------------------------ test seams */

void wake_engine_set_clock(int64_t (*now_us)(void)) { s_clock = now_us ? now_us : xiaotai_time_us; }

void wake_engine_memory(wake_engine_memory_t *memory)
{
    if (memory) *memory = s_memory;
}

void wake_engine_task_config(xiaotai_task_config_t *config)
{
    if (config) *config = s_task_config;
}
