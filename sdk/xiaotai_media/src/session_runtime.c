// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <string.h>

#include "audio_workers.h"
#include "clip_playout.h"
#include "diag.h"
#include "downlink_rx.h"
#include "internal.h"
#include "playout.h"
#include "preroll.h"
#include "record.h"
#include "registry.h"
#include "session.h"
#include "session_events.h"
#include "trace.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

#define COMPLETION_RETRY_MS 5U

static const char *TAG = "media";
static xiaotai_mutex_t s_lock;
static xiaotai_signal_t s_wake, s_stopped;
static atomic_bool s_initialized, s_ready, s_stop, s_open_failure;
static atomic_bool s_startup_complete;
static _Atomic xiaotai_err_t s_startup_error;
typedef struct {
    uint8_t volume;
    int8_t gain;
    bool volume_set, gain_set, speaker_muted, mic_muted;
} initial_controls_t;
static initial_controls_t s_initial_controls;
static bool s_worker, s_output_free;
static media_session_control_t s_control;
static media_session_events_t s_events;
static const xiaotai_audio_input_t *s_input;
static const xiaotai_audio_output_t *s_output;
static media_topology_t s_topology;
static xiaotai_downlink_t *s_ring;
static media_clip_player_t *s_clips;
static media_playout_t *s_playout;
static uint32_t s_output_epoch;
static bool s_had_armed_audio, s_speaker_muted, s_mute_applied, s_volume_dirty, s_mute_dirty, s_mixer;
static bool s_mute_after_accept, s_mute_target;
static bool s_mic_muted;
static bool s_diagnostic_wait, s_preserve_diagnostic;
static int8_t s_gain, s_gain_applied;
static uint32_t s_uplink_epoch, s_mic_epoch, s_ptt_open, s_ptt_close;
static int64_t s_tail_end_us, s_tail_deadline_us, s_mic_cutover_us;
static media_audio_workers_t *s_audio;
static _Atomic(const xiaotai_media_uplink_executor_t *) s_uplink_executor;
static atomic_bool s_uplink_executor_enabled;
static atomic_uint s_uplink_notifiers;
static uint8_t s_volume, s_volume_applied;
static xiaotai_err_t s_control_error[4];
static xiaotai_err_t s_power_error;

typedef struct {
    bool powered, wanted;
    uint32_t epoch, clip_id;
    xiaotai_err_t refused;
} power_state_t;

static void lock(void) { (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&s_lock); }

xiaotai_err_t xiaotai_media_uplink_attach(const xiaotai_media_uplink_executor_t *executor)
{
    if (!executor || !executor->sink || !executor->sink->send || !executor->wake || !executor->is_current)
        return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || atomic_load(&s_ready) || atomic_load(&s_uplink_executor_enabled))
        return XIAOTAI_ERR_INVALID_STATE;
    atomic_store(&s_uplink_executor, executor);
    atomic_store(&s_uplink_executor_enabled, true);
    return XIAOTAI_OK;
}

bool media_runtime_uplink_is_executor(void)
{
    const xiaotai_media_uplink_executor_t *executor = atomic_load(&s_uplink_executor);
    return executor && executor->is_current();
}

void xiaotai_media_uplink_detach(const xiaotai_media_uplink_executor_t *executor)
{
    if (!executor || atomic_load(&s_uplink_executor) != executor) return;
    if (executor->is_current()) {
        XIAOTAI_LOGE(TAG, "cannot detach from the uplink executor");
        return;
    }
    atomic_store(&s_uplink_executor_enabled, false);
    while (atomic_load(&s_uplink_notifiers)) xiaotai_delay_ms(1);
    /* Keep the static identity for a send that was admitted before detach:
     * it must still be refused if it tries to reset itself. */
}

void media_runtime_uplink_notify(void)
{
    if (!atomic_load(&s_uplink_executor_enabled)) return;
    atomic_fetch_add(&s_uplink_notifiers, 1U);
    if (atomic_load(&s_uplink_executor_enabled)) atomic_load(&s_uplink_executor)->wake();
    atomic_fetch_sub(&s_uplink_notifiers, 1U);
}

xiaotai_media_uplink_work_t xiaotai_media_uplink_step(const xiaotai_media_uplink_executor_t *executor)
{
    if (!executor || atomic_load(&s_uplink_executor) != executor || !atomic_load(&s_uplink_executor_enabled) ||
        !executor->is_current() || !atomic_load(&s_ready) || atomic_load(&s_stop)) {
        return XIAOTAI_MEDIA_UPLINK_IDLE;
    }
    if (!xiaotai_mutex_lock(&s_lock, 0)) return XIAOTAI_MEDIA_UPLINK_CONTENDED;
    media_audio_workers_t *const worker = s_audio;
    if (atomic_load(&s_stop) || !atomic_load(&s_uplink_executor_enabled) || !media_audio_uplink_claim(worker)) {
        unlock();
        return XIAOTAI_MEDIA_UPLINK_IDLE;
    }
    const media_uplink_view_t view = {
        .sink = executor->sink, .generation = s_control.config.generation, .epoch = s_uplink_epoch,
        .packet_ms = s_control.config.uplink.packet_ms,
        .eligible = s_control.phase == MEDIA_SESSION_ACTIVE && !s_control.start_pending && !s_mic_muted &&
                    s_control.config.sink == executor->sink,
        .gate = s_control.uplink_open, .tail = s_tail_deadline_us != 0,
        .tail_end_us = s_tail_end_us, .tail_deadline_us = s_tail_deadline_us,
    };
    unlock();
    return media_audio_uplink_finish(worker, &view) ? XIAOTAI_MEDIA_UPLINK_PENDING : XIAOTAI_MEDIA_UPLINK_IDLE;
}

static bool session_power(void)
{
    return !s_diagnostic_wait && (s_control.phase == MEDIA_SESSION_ACTIVE || s_control.phase == MEDIA_SESSION_DRAINING ||
                                  s_control.phase == MEDIA_SESSION_DRAINED);
}

void media_session_wake_playout(void)
{
    if (atomic_load(&s_ready)) xiaotai_signal_give(&s_wake);
}

static bool current_playout(uint32_t generation, uint32_t epoch)
{
    lock();
    const bool current = s_output_epoch == epoch && !s_speaker_muted &&
                         !s_diagnostic_wait &&
                         s_control.config.generation == generation &&
                         (s_control.phase == MEDIA_SESSION_ACTIVE || s_control.phase == MEDIA_SESSION_DRAINING ||
                          s_control.phase == MEDIA_SESSION_DRAINED);
    unlock();
    return current;
}

static void invalidate_uplink(void)
{
    ++s_uplink_epoch;
    s_tail_end_us = s_tail_deadline_us = 0;
}

void media_runtime_capture_view(media_capture_view_t *view)
{
    lock();
    *view = (media_capture_view_t){
        .session = s_control.config, .uplink_epoch = s_uplink_epoch,
        .active = session_power() && !s_control.start_pending,
        .gate = s_control.uplink_open, .muted = s_mic_muted, .preroll = s_control.preroll_bound,
        .tail_end_us = s_tail_end_us, .tail_deadline_us = s_tail_deadline_us,
        .mic_epoch = s_mic_epoch, .mic_cutover_us = s_mic_cutover_us,
        .volume = s_volume_applied, .gain_db = s_gain, .gain_applied_db = s_gain_applied,
    };
    unlock();
}

bool media_runtime_uplink_allowed(uint32_t generation, uint32_t epoch, int64_t captured, bool *tail)
{
    lock();
    const bool matches = generation && generation == s_control.config.generation && epoch == s_uplink_epoch &&
                         !s_mic_muted && s_control.phase == MEDIA_SESSION_ACTIVE && !s_control.start_pending;
    *tail = matches && !s_control.uplink_open && s_tail_deadline_us &&
            captured <= s_tail_end_us && xiaotai_time_us() <= s_tail_deadline_us;
    const bool allowed = matches && (s_control.uplink_open || *tail);
    unlock();
    return allowed;
}

void media_runtime_input_failed(void)
{
    media_record_end(0, XIAOTAI_FAIL);
    media_preroll_discard(media_preroll_current(), true);
    lock();
    const uint32_t generation = s_control.config.generation;
    if (generation && s_control.phase != MEDIA_SESSION_FAILED) {
        s_control.phase = MEDIA_SESSION_FAILED;
        s_control.uplink_open = s_control.start_pending = false;
        invalidate_uplink();
        media_rx_flush(false, s_speaker_muted);
        ++s_output_epoch;
        media_session_events_record(&s_events, generation, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL);
    }
    atomic_store(&s_open_failure, true);
    unlock();
    media_session_wake_playout();
}

void media_runtime_gain_applied(int8_t gain, xiaotai_err_t error)
{
    lock();
    if (error == XIAOTAI_OK) s_gain_applied = gain;
    else s_control_error[2] = s_power_error = error;
    unlock();
    if (error != XIAOTAI_OK) media_session_wake_playout();
}

static void far_end(const int16_t *mono, size_t frames, uint32_t rate)
{
    media_audio_far_end(s_audio, mono, frames, rate);
}

static void set_speaker_gate_locked(bool muted)
{
    if (s_speaker_muted == muted) return;
    s_speaker_muted = muted;
    const bool receives = s_control.config.downlink.rate_hz &&
                          (s_control.phase == MEDIA_SESSION_ARMED || s_control.phase == MEDIA_SESSION_ACTIVE);
    media_rx_flush(receives, muted);
    ++s_output_epoch;
    s_had_armed_audio = false;
    if (muted && s_clips) media_clip_player_cancel(s_clips, 0, XIAOTAI_ERR_INVALID_STATE);
    if (!muted) s_output_free = true;
}

typedef struct {
    uint8_t volume;
    bool muted, set_volume, set_mute;
} output_controls_t;

static output_controls_t take_controls_locked(bool output_opened)
{
    if (!output_opened) return (output_controls_t){0};
    const output_controls_t controls = {
        .volume = s_volume, .muted = s_mute_after_accept ? s_mute_target : s_speaker_muted,
        .set_volume = s_volume_dirty, .set_mute = s_mute_dirty,
    };
    s_volume_dirty = s_mute_dirty = false;
    return controls;
}

static void apply_controls(output_controls_t controls)
{
    if (!controls.set_volume && !controls.set_mute) return;
    const xiaotai_err_t change_volume = controls.set_volume && s_output->set_volume ?
                                       s_output->set_volume(s_output->self, controls.volume) : XIAOTAI_OK;
    const xiaotai_err_t change_mute = controls.set_mute && s_output->set_muted ?
                                     s_output->set_muted(s_output->self, controls.muted) : XIAOTAI_OK;
    lock();
    if (controls.set_volume) {
        if (change_volume == XIAOTAI_OK) s_volume_applied = controls.volume;
        else s_control_error[0] = s_power_error = change_volume;
    }
    if (controls.set_mute) {
        if (change_mute == XIAOTAI_OK) {
            s_mute_applied = controls.muted;
            if (s_mute_after_accept) set_speaker_gate_locked(controls.muted);
        } else {
            s_control_error[1] = s_power_error = change_mute;
            if (s_mute_after_accept && !s_mute_dirty) s_mute_target = s_mute_applied;
        }
    }
    unlock();
}

static bool post_control_errors(void)
{
    static const char *const names[] = {"volume", "mute", "gain", "power"};
    bool retry = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        lock();
        const xiaotai_err_t error = s_control_error[i];
        s_control_error[i] = XIAOTAI_OK;
        unlock();
        if (error == XIAOTAI_OK) continue;
        xiaotai_args_t args = {.value = error};
        memcpy(args.text, names[i], strlen(names[i]) + 1U);
        if (xiaotai_event_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, &args, NULL, 0) == XIAOTAI_ERR_NO_MEM) {
            lock();
            if (s_control_error[i] == XIAOTAI_OK) s_control_error[i] = error;
            unlock();
            retry = true;
        }
    }
    return retry;
}

static uint32_t mix_playout(int16_t *mono, size_t frames)
{
    const uint32_t id = s_clips ? media_clip_player_id(s_clips) : 0;
    return id && media_clip_player_over_session(s_clips, id) ? media_clip_player_mix(s_clips, id, mono, frames) : 0;
}

static void mixed_written(uint32_t token, xiaotai_err_t status)
{
    media_clip_player_mix_written(s_clips, token, status);
}

static uint32_t next_pass(power_state_t *power, bool output_opened)
{
    lock();
    const output_controls_t controls = take_controls_locked(output_opened);
    const bool apply_before_plan = s_mute_after_accept && controls.set_mute;
    if (apply_before_plan) {
        /* Plan power and playout from the gate that the codec actually accepted. */
        unlock();
        apply_controls(controls);
        lock();
    }
    if (s_diagnostic_wait && !media_diag_audio_active()) {
        s_diagnostic_wait = false;
        if (s_control.phase == MEDIA_SESSION_ACTIVE) {
            s_had_armed_audio = s_ring && xiaotai_downlink_count(s_ring) != 0;
            media_rx_activate();
        }
    }
    const bool waiting_diagnostic = s_diagnostic_wait;
    const xiaotai_media_session_t session = s_control.config;
    const uint32_t generation = session.generation;
    const uint32_t epoch = s_output_epoch;
    const media_session_phase_t phase = s_control.phase;
    const bool wants_session = session_power();
    const uint32_t clip_id = s_clips ? media_clip_player_id(s_clips) : 0;
    const bool mixing = wants_session && clip_id && media_clip_player_over_session(s_clips, clip_id);
    const bool muted = s_speaker_muted;
    const bool wants_power = output_opened && !muted && ((wants_session && session.downlink.rate_hz) || clip_id != 0);
    const bool armed_audio = s_had_armed_audio;
    unlock();

    if (!apply_before_plan) apply_controls(controls);
    xiaotai_err_t result = XIAOTAI_OK;
    const bool power_change = wants_power != power->powered;
    const bool already_refused = power_change && power->refused != XIAOTAI_OK &&
                                 power->wanted == wants_power && power->epoch == epoch && power->clip_id == clip_id;
    if (already_refused) {
        result = power->refused;
    } else if (power_change) {
        if (s_output && s_output->set_enabled) result = s_output->set_enabled(s_output->self, wants_power);
        if (result == XIAOTAI_OK) power->powered = wants_power;
        power->refused = result;
        power->wanted = wants_power;
        power->epoch = epoch;
        power->clip_id = clip_id;
    } else {
        power->refused = XIAOTAI_OK;
    }

    lock();
    if (power_change && !already_refused && result != XIAOTAI_OK) s_control_error[3] = s_power_error = result;
    if (wants_session && media_session_control_start_result(&s_control, generation, result)) {
        if (result != XIAOTAI_OK) media_rx_flush(false, muted);
        const xiaotai_media_event_t event = result == XIAOTAI_OK ? XIAOTAI_MEDIA_EVENT_STARTED :
                                                                 XIAOTAI_MEDIA_EVENT_FAILED;
        media_session_events_record(&s_events, generation, event, result);
    }
    unlock();

    media_playout_reset(s_playout, generation, epoch, session.latency);
    bool empty = (!session.downlink.rate_hz || muted || !s_playout) && !media_rx_writers();
    uint32_t playout_wait = XIAOTAI_WAIT_FOREVER;
    if (wants_session && result == XIAOTAI_OK && power->powered && (session.downlink.rate_hz || mixing) &&
        (phase == MEDIA_SESSION_ACTIVE || phase == MEDIA_SESSION_DRAINING || (phase == MEDIA_SESSION_DRAINED && mixing)) &&
        current_playout(generation, epoch)) {
        playout_wait = media_playout_pass(s_playout, &(media_playout_request_t){
            .generation = generation, .epoch = epoch, .session = &session, .armed_audio = armed_audio,
            .draining = phase == MEDIA_SESSION_DRAINING, .current = current_playout, .mixing = mixing,
            .capture_clock = s_topology == MEDIA_TOPOLOGY_FOLDED,
            .mix = mix_playout, .mixed_written = mixed_written, .far_end = far_end,
        }, &empty);
    }

    lock();
    if (s_control.phase == MEDIA_SESSION_DRAINING && s_control.config.generation == generation) {
        xiaotai_err_t drained;
        if (media_session_control_drain_result(&s_control, generation, empty, xiaotai_time_us(), &drained)) {
            if (drained != XIAOTAI_OK) {
                media_rx_flush(false, muted);
                ++s_output_epoch;
            }
            media_session_events_record(&s_events, generation, XIAOTAI_MEDIA_EVENT_DRAINED, drained);
        }
    }
    unlock();

    const uint32_t clip_wait = s_clips ? media_clip_player_step(s_clips,
                                wants_session && (!mixing || result == XIAOTAI_OK) ? 0 : clip_id, result) :
                                         XIAOTAI_WAIT_FOREVER;

    lock();
    const uint32_t next_clip = s_clips ? media_clip_player_id(s_clips) : 0;
    if (clip_id && !next_clip) s_output_free = true;
    const bool session_active = session_power();
    const bool power_request_changed = (output_opened && !s_speaker_muted &&
        ((session_active && s_control.config.downlink.rate_hz) || next_clip != 0)) != wants_power;
    const bool draining = s_control.phase == MEDIA_SESSION_DRAINING;
    const bool free_output = s_output_free && !power->powered && !session_active && !next_clip && !media_record_active();
    if (free_output) s_output_free = false;
    unlock();

    media_session_events_post(&s_events);
    const bool retry_error = post_control_errors();
    const bool retry_record = media_record_service();
    const bool retry_preroll = media_preroll_service();
    bool retry_free = false;
    if (free_output && xiaotai_event_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_FREE, NULL, NULL, 0) != XIAOTAI_OK) {
        lock();
        retry_free = !session_power() && (!s_clips || !media_clip_player_id(s_clips)) && !media_record_active();
        if (retry_free) s_output_free = true;
        unlock();
    }
    if (power_request_changed) return 0;
    const uint32_t wait = clip_wait < playout_wait ? clip_wait : playout_wait;
    if ((waiting_diagnostic || draining || retry_free || retry_error || retry_record || retry_preroll ||
         media_session_events_pending(&s_events)) && wait > COMPLETION_RETRY_MS) {
        return COMPLETION_RETRY_MS;
    }
    if (media_preroll_token(media_preroll_current()) && wait > 20U) return 20U;
    return wait;
}

static xiaotai_err_t parts_open(bool *output_opened, bool *input_opened)
{
    xiaotai_err_t err = XIAOTAI_OK;
    if (s_output) {
        err = s_output->open(s_output->self);
        *output_opened = err == XIAOTAI_OK;
        if (*output_opened) {
            lock();
            s_volume_dirty = s_mute_dirty = true;
            unlock();
        }
    }
    if (err == XIAOTAI_OK && s_input) {
        err = s_input->open(s_input->self);
        *input_opened = err == XIAOTAI_OK;
    }
    return err;
}

static xiaotai_err_t parts_close(bool *output_opened, bool *input_opened)
{
    xiaotai_err_t err = XIAOTAI_OK;
    if (*input_opened) {
        err = s_input->close(s_input->self);
        *input_opened = false;
    }
    if (*output_opened) {
        const xiaotai_err_t speaker = s_output->close(s_output->self);
        if (err == XIAOTAI_OK) err = speaker;
        *output_opened = false;
    }
    return err;
}

static bool audio_requested(void)
{
    lock();
    const bool active = s_control.config.generation || (s_clips && media_clip_player_id(s_clips)) ||
                        media_record_active() || media_preroll_token(media_preroll_current());
    unlock();
    if (active) return true;
    for (size_t i = 0; ; ++i) {
        bool enabled;
        if (!media_tap_at(i, &enabled)) return false;
        if (enabled) return true;
    }
}

void media_session_startup_complete(xiaotai_err_t error)
{
    atomic_store(&s_startup_error, error);
    atomic_store(&s_startup_complete, true);
}

void media_session_startup_reset(void)
{
    atomic_store(&s_startup_complete, false);
    s_initial_controls = (initial_controls_t){0};
}

static bool retry_startup_notice(void)
{
    if (atomic_load(&s_stop)) return false;
    const xiaotai_err_t err = xiaotai_event_emit(XIAOTAI_EVENT_MEDIA_STARTUP, NULL, NULL, 0);
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_NO_MEM && err != XIAOTAI_ERR_INVALID_STATE)
        XIAOTAI_LOGE(TAG, "post audio startup: %d", err);
    return err == XIAOTAI_ERR_NO_MEM;
}

static void worker(void *ctx)
{
    (void)ctx;
    bool output_opened = false, input_opened = false, idle_closed = false;
    power_state_t power = {0};
    int64_t idle_since = 0;
    const int32_t idle_ms = xiaotai_media_param("power.idle_close_ms", 0);
    xiaotai_err_t err = parts_open(&output_opened, &input_opened);
    bool startup_notice = true;
    if (err == XIAOTAI_OK) {
        atomic_store(&s_ready, true);
        media_audio_workers_start(s_audio);
        media_session_startup_complete(XIAOTAI_OK);
        while (!atomic_load(&s_stop)) {
            if (startup_notice) startup_notice = retry_startup_notice();
            uint32_t idle_wait = XIAOTAI_WAIT_FOREVER;
            if (idle_ms > 0) {
                const bool demand = audio_requested();
                if (demand) {
                    idle_since = 0;
                    if (idle_closed) {
                        err = parts_open(&output_opened, &input_opened);
                        if (err != XIAOTAI_OK) {
                            XIAOTAI_LOGE(TAG, "audio parts failed to reopen: %d", err);
                            media_runtime_input_failed();
                            (void)next_pass(&power, false);
                            break;
                        }
                        idle_closed = false;
                    }
                    media_audio_workers_pause(s_audio, false);
                } else if (!idle_closed) {
                    const int64_t now = xiaotai_time_us();
                    if (!idle_since) idle_since = now;
                    const int64_t remaining = (int64_t)idle_ms * 1000 - (now - idle_since);
                    if (remaining > 0) idle_wait = (uint32_t)((remaining + 999) / 1000);
                    else {
                        media_audio_workers_pause(s_audio, true);
                        if ((power.powered && power.refused == XIAOTAI_OK) || !media_audio_workers_parked(s_audio)) idle_wait = 1;
                        else if (power.powered) idle_wait = XIAOTAI_WAIT_FOREVER;
                        else {
                            err = parts_close(&output_opened, &input_opened);
                            if (err != XIAOTAI_OK) {
                                XIAOTAI_LOGE(TAG, "audio parts failed to idle close: %d", err);
                                media_runtime_input_failed();
                                (void)next_pass(&power, false);
                                break;
                            }
                            idle_closed = true;
                        }
                    }
                }
            }
            const bool capture_clock = input_opened && media_audio_folded_pass(s_audio);
            const uint32_t audio_wait = next_pass(&power, output_opened);
            uint32_t wait = audio_wait < idle_wait ? audio_wait : idle_wait;
            if (startup_notice && wait > COMPLETION_RETRY_MS) wait = COMPLETION_RETRY_MS;
            if (!capture_clock && !atomic_load(&s_stop)) (void)xiaotai_signal_take(&s_wake, wait);
        }
    } else {
        atomic_store(&s_open_failure, true);
        media_session_startup_complete(err);
        XIAOTAI_LOGE(TAG, "audio parts failed to open: %d", err);
    }
    media_audio_workers_stop(s_audio);
    const xiaotai_err_t closed = parts_close(&output_opened, &input_opened);
    if (closed != XIAOTAI_OK) XIAOTAI_LOGE(TAG, "audio parts failed to close: %d", closed);
    while (startup_notice && retry_startup_notice())
        (void)xiaotai_signal_take(&s_wake, COMPLETION_RETRY_MS);
    atomic_store(&s_ready, false);
    xiaotai_signal_give(&s_stopped);
    xiaotai_task_exit();
}

xiaotai_err_t media_session_runtime_start(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                         xiaotai_downlink_t *downlink, media_topology_t topology)
{
    atomic_store(&s_open_failure, false);
    s_input = input && input->open && input->close ? input : NULL;
    s_output = output && output->open && output->close ? output : NULL;
    s_topology = topology;
    media_diag_input(s_input);
    if (!s_input && !s_output) {
        media_session_startup_complete(XIAOTAI_ERR_NOT_SUPPORTED);
        return XIAOTAI_OK;
    }
    if (!atomic_load(&s_initialized)) {
        const xiaotai_err_t err = xiaotai_mutex_init(&s_lock);
        if (err != XIAOTAI_OK) return err;
        media_session_events_init(&s_events);
        media_rx_init();
        media_record_init();
        atomic_store(&s_initialized, true);
    }
    s_ring = downlink;
    s_preserve_diagnostic = xiaotai_media_param("diagnostics.keep_until_done", 0) != 0;
    /* Compatibility policy: a rejected codec request must not gate audio.
     * The default retains immediate software gating on request. */
    s_mute_after_accept = xiaotai_media_param("output.mute_after_accept", 0) != 0;
    const int8_t part_gain = s_input ? s_input->info.gain_db : 0;
    atomic_store(&s_stop, false);
    const int32_t volume = xiaotai_media_param("output.volume_percent", 70);
    /* Release the capture tasks with the privacy gate already in place. */
    lock();
    s_gain_applied = part_gain;
    s_gain = s_initial_controls.gain_set ? s_initial_controls.gain : part_gain;
    s_volume = s_initial_controls.volume_set ? s_initial_controls.volume :
               (uint8_t)(volume < 0 ? 0 : volume > 100 ? 100 : volume);
    s_mute_target = s_initial_controls.speaker_muted;
    s_speaker_muted = !s_mute_after_accept && s_mute_target;
    s_mic_muted = s_initial_controls.mic_muted;
    unlock();
    xiaotai_err_t err = s_output && s_output->write ? media_clip_player_create(s_output, &s_clips) : XIAOTAI_OK;
    if (err != XIAOTAI_OK) return err;
    s_mixer = s_clips && xiaotai_media_param("playout.mixer", xiaotai_mem_has_external() ? 1 : 0) != 0;
    if (s_clips) media_clip_player_enable_mixer(s_clips, s_mixer);
    if (s_clips) media_clip_player_capture_clock(s_clips, s_topology == MEDIA_TOPOLOGY_FOLDED);
    if (s_output && s_output->write) {
        err = media_playout_create(s_output, downlink, media_prepared_downlink_ms(), &s_playout);
        if (err != XIAOTAI_OK) goto clip_fail;
    }
    const bool delegated = atomic_load(&s_uplink_executor_enabled) && media_prepared_capture_safe();
    err = media_audio_workers_create(s_input, s_topology, delegated, &s_audio);
    if (err != XIAOTAI_OK) goto clip_fail;
    if (media_audio_uplink_delegated(s_audio)) XIAOTAI_LOGD(TAG, "uplink executor: transport worker");
    uint32_t decoder_stack = s_clips ? media_clip_player_decoder_stack_bytes(s_clips) : 0;
    const uint32_t downlink_stack = media_max_decoder_stack();
    if (downlink_stack > decoder_stack) decoder_stack = downlink_stack;
    const bool folded = s_topology == MEDIA_TOPOLOGY_FOLDED;
    const uint64_t minimum_stack = folded ? media_audio_folded_stack(s_audio, decoder_stack) : UINT64_C(6144) + decoder_stack;
    if (!minimum_stack || minimum_stack > UINT32_MAX) {
        err = XIAOTAI_ERR_INVALID_SIZE;
        goto clip_fail;
    }
    uint32_t stack = (uint32_t)minimum_stack;
    const int32_t tuned_stack = xiaotai_media_param(folded ? "task.capture.stack_bytes" : "task.playout.stack_bytes", 0);
    if (tuned_stack > 0 && (uint32_t)tuned_stack > stack) stack = (uint32_t)tuned_stack;
    err = xiaotai_signal_init(&s_wake);
    if (err != XIAOTAI_OK) goto clip_fail;
    err = xiaotai_signal_init(&s_stopped);
    if (err == XIAOTAI_OK) {
        const int32_t priority = folded ? xiaotai_media_param("task.capture.priority", 9) :
                                          xiaotai_media_param("task.playout.priority", 8);
        const xiaotai_task_config_t config = {
            .name = folded ? "xt_capture" : "xt_playout", .entry = worker,
            .stack_bytes = stack, .stack = XIAOTAI_MEM_BULK, .priority = priority > 0 ? (unsigned)priority : 1U,
            .core = folded ? xiaotai_media_param("task.capture.core", 1) :
                             xiaotai_media_param("task.playout.core", XIAOTAI_CORE_ANY),
        };
        xiaotai_task_t task;
        err = xiaotai_task_create(&config, &task);
        if (err == XIAOTAI_OK) {
            s_worker = true;
            return XIAOTAI_OK;
        }
        xiaotai_signal_deinit(&s_stopped);
    }
    xiaotai_signal_deinit(&s_wake);
clip_fail:
    media_audio_workers_destroy(s_audio);
    s_audio = NULL;
    media_playout_destroy(s_playout);
    s_playout = NULL;
    if (s_clips) media_clip_player_destroy(s_clips);
    s_clips = NULL;
    return err;
}

bool media_session_runtime_failed(void) { return atomic_load(&s_open_failure); }

void media_session_runtime_stop(void)
{
    if (!s_worker) return;
    media_rx_close();
    while (media_rx_writers()) xiaotai_delay_ms(1);
    atomic_store(&s_stop, true);
    xiaotai_signal_give(&s_wake);
    (void)xiaotai_signal_take(&s_stopped, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_deinit(&s_wake);
    xiaotai_signal_deinit(&s_stopped);
    if (s_clips) media_clip_player_destroy(s_clips);
    s_clips = NULL;
    lock();
    media_audio_workers_t *audio = s_audio;
    s_audio = NULL;
    media_playout_destroy(s_playout);
    s_playout = NULL;
    media_session_events_bind(&s_events, NULL);
    s_control = (media_session_control_t){0};
    s_output_free = false;
    s_power_error = XIAOTAI_OK;
    s_speaker_muted = s_mute_applied = s_mute_target = false;
    s_mic_muted = false;
    s_diagnostic_wait = false;
    s_mic_epoch = 0;
    s_mic_cutover_us = 0;
    s_volume = s_volume_applied = 0;
    s_volume_dirty = s_mute_dirty = false;
    for (size_t i = 0; i < sizeof(s_control_error) / sizeof(*s_control_error); ++i) s_control_error[i] = XIAOTAI_OK;
    unlock();
    media_audio_workers_destroy(audio);
    media_record_reset();
    media_preroll_dispatch_reset();
    s_worker = false;
}

xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *session)
{
    const xiaotai_err_t valid = media_session_validate(session);
    if (valid != XIAOTAI_OK) return valid;
    if (!atomic_load(&s_ready) || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (session->uplink.rate_hz) {
        lock();
        const bool delegated = media_audio_uplink_delegated(s_audio);
        const xiaotai_media_uplink_executor_t *executor = atomic_load(&s_uplink_executor);
        const bool matches = executor && session->sink == executor->sink;
        unlock();
        if (delegated && !matches) return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    media_preroll_t *history = media_preroll_current();
    const bool bind = session->preroll_token && !(session->flags & XIAOTAI_MEDIA_HOLD);
    if (session->preroll_token) {
        lock();
        const bool possible = s_control.phase == MEDIA_SESSION_IDLE && session->generation != s_control.previous && !s_mic_muted;
        unlock();
        if (!possible) return XIAOTAI_ERR_INVALID_STATE;
        /* History permits a bounded wait on its own lock. It must never hold
         * up the realtime snapshot lock or an unrelated receive callback. */
        const int64_t now = xiaotai_time_us();
        const xiaotai_err_t permission = bind ? media_preroll_bind(history, session->preroll_token, session->generation, now) :
                                               media_preroll_validate(history, session->preroll_token, now);
        if (permission != XIAOTAI_OK) return XIAOTAI_ERR_INVALID_STATE;
    }
    lock();
    const bool permitted = !session->preroll_token || media_preroll_token(history) == session->preroll_token;
    const xiaotai_err_t err = media_session_control_open(&s_control, session, permitted, s_mic_muted);
    if (err != XIAOTAI_OK && bind) {
        media_preroll_close_binding(history, session->generation);
    }
    if (err == XIAOTAI_OK) {
        invalidate_uplink();
        media_audio_tx_reset(s_audio, session->generation);
        s_ptt_open = s_ptt_close = 0;
        s_diagnostic_wait = s_preserve_diagnostic && media_diag_audio_active();
        if (!s_diagnostic_wait) media_record_end(0, XIAOTAI_ERR_INVALID_STATE);
        if (!session->preroll_token) media_preroll_discard(history, false);
        media_session_events_bind(&s_events, session);
        media_trace_session_open(session->generation, session->flags);
        media_rx_open(s_ring, session, !session_power(), s_speaker_muted);
        ++s_output_epoch;
        s_had_armed_audio = false;
        if (s_clips && session_power()) media_clip_player_cancel_plain(s_clips);
        s_output_free = false;
    }
    unlock();
    if (err == XIAOTAI_OK) xiaotai_signal_give(&s_wake);
    return err;
}

xiaotai_err_t xiaotai_media_activate(uint32_t generation)
{
    if (!atomic_load(&s_ready) || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    media_preroll_t *history = media_preroll_current();
    const uint32_t token = s_control.config.generation == generation && s_control.phase == MEDIA_SESSION_ARMED ?
                           s_control.config.preroll_token : 0;
    bool permission = true;
    if (token) {
        const bool unmuted = !s_mic_muted;
        unlock();
        permission = unmuted && media_preroll_bind(history, token, generation, xiaotai_time_us()) == XIAOTAI_OK;
        lock();
    }
    const bool token_current = !token || media_preroll_token(history) == token;
    const xiaotai_err_t err = media_session_control_activate(&s_control, generation, permission && token_current, s_mic_muted);
    if (err != XIAOTAI_OK && token && permission) media_preroll_close_binding(history, generation);
    if (err == XIAOTAI_OK) {
        if (!s_diagnostic_wait) {
            media_rx_activate();
            s_had_armed_audio = s_ring && xiaotai_downlink_count(s_ring) != 0;
            if (s_clips) media_clip_player_cancel_plain(s_clips);
        }
    }
    unlock();
    if (err == XIAOTAI_OK) xiaotai_signal_give(&s_wake);
    return err;
}

xiaotai_err_t xiaotai_media_drain(uint32_t generation, uint32_t timeout_ms)
{
    if (!atomic_load(&s_ready) || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    const xiaotai_err_t err = media_session_control_drain(&s_control, generation, timeout_ms, xiaotai_time_us());
    if (err == XIAOTAI_OK) { media_rx_drain(); invalidate_uplink(); }
    unlock();
    if (err == XIAOTAI_OK) xiaotai_signal_give(&s_wake);
    return err;
}

xiaotai_err_t xiaotai_media_flush(uint32_t generation)
{
    if (!atomic_load(&s_ready) || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    const bool matches = media_session_control_matches(&s_control, generation);
    if (matches) {
        const bool receives = s_control.config.downlink.rate_hz &&
                              (s_control.phase == MEDIA_SESSION_ARMED || s_control.phase == MEDIA_SESSION_ACTIVE);
        media_rx_flush(receives, s_speaker_muted);
        ++s_output_epoch;
        s_had_armed_audio = false;
    }
    unlock();
    if (matches) xiaotai_signal_give(&s_wake);
    return matches ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

void xiaotai_media_close(uint32_t generation)
{
    if (!atomic_load(&s_ready) || !xiaotai_on_owner()) return;
    lock();
    const bool closed = media_session_control_close(&s_control, generation);
    if (closed) {
        invalidate_uplink();
        s_diagnostic_wait = false;
        media_preroll_close_binding(media_preroll_current(), generation);
        media_session_events_bind(&s_events, NULL);
        media_rx_close();
        ++s_output_epoch;
        if (s_clips) media_clip_player_cancel_generation(s_clips, generation);
        s_output_free = true;
    }
    unlock();
    if (closed) xiaotai_signal_give(&s_wake);
}

xiaotai_err_t media_session_play(const xiaotai_media_play_t *request, uint32_t *id)
{
    if (!request || !request->clip_count || request->clip_count > XIAOTAI_MEDIA_PLAY_CLIPS_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_clips || ((request->flags & XIAOTAI_PLAY_OVER_SESSION) && !s_mixer)) return XIAOTAI_ERR_NOT_SUPPORTED;

    media_clip_request_t prepared;
    xiaotai_err_t err = media_clip_request_prepare(s_clips, request, &prepared);
    if (err != XIAOTAI_OK) return err;

    lock();
    if (prepared.generation && !media_session_control_matches(&s_control, prepared.generation)) {
        err = XIAOTAI_ERR_INVALID_STATE;
    } else if (s_speaker_muted) {
        err = XIAOTAI_ERR_INVALID_STATE;
    } else if (s_diagnostic_wait || (session_power() && !(prepared.flags & XIAOTAI_PLAY_OVER_SESSION)) || media_record_active()) {
        err = XIAOTAI_ERR_BUSY;
    } else {
        err = media_clip_player_submit(s_clips, &prepared, id);
    }
    unlock();
    if (err == XIAOTAI_OK) xiaotai_signal_give(&s_wake);
    return err;
}

xiaotai_err_t media_session_play_stream(const xiaotai_media_stream_t *request, uint32_t *id)
{
    if (!request || !id) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_clips || ((request->flags & XIAOTAI_PLAY_OVER_SESSION) && !s_mixer)) return XIAOTAI_ERR_NOT_SUPPORTED;
    media_clip_request_t prepared;
    xiaotai_err_t err = media_clip_stream_prepare(s_clips, request, &prepared);
    if (err != XIAOTAI_OK) return err;
    lock();
    if (prepared.generation && !media_session_control_matches(&s_control, prepared.generation)) {
        err = XIAOTAI_ERR_INVALID_STATE;
    } else if (s_speaker_muted) {
        err = XIAOTAI_ERR_INVALID_STATE;
    } else if (s_diagnostic_wait || (session_power() && !(prepared.flags & XIAOTAI_PLAY_OVER_SESSION)) || media_record_active()) {
        err = XIAOTAI_ERR_BUSY;
    } else {
        err = media_clip_player_submit(s_clips, &prepared, id);
    }
    unlock();
    if (err != XIAOTAI_OK) media_clip_request_discard(&prepared);
    else media_session_wake_playout();
    return err;
}

media_stream_buffer_t *media_session_find_stream(uint32_t id)
{
    if (!atomic_load(&s_ready)) return NULL;
    lock();
    media_stream_buffer_t *stream = s_clips ? media_clip_player_find_stream(s_clips, id) : NULL;
    unlock();
    return stream;
}

xiaotai_err_t media_session_record_target(const xiaotai_media_record_t *request, uint8_t *alaw, uint32_t *id)
{
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    if (!info.capture_pcm) return XIAOTAI_ERR_NOT_SUPPORTED;
    lock();
    media_preroll_t *history = media_preroll_current();
    xiaotai_err_t err;
    if (s_mic_muted || media_preroll_token(history)) err = XIAOTAI_ERR_INVALID_STATE;
    else if (s_control.config.generation || (s_clips && media_clip_player_id(s_clips))) err = XIAOTAI_ERR_BUSY;
    else {
        err = alaw ? media_record_begin_alaw(alaw, request->samples, request->done, request->ctx,
                                             info.capture_rate_hz, xiaotai_time_us(), id) :
                     media_record_begin(request, info.capture_rate_hz, xiaotai_time_us(), id);
        if (err == XIAOTAI_OK) media_preroll_discard(history, false);
    }
    unlock();
    if (err == XIAOTAI_OK) media_session_wake_playout();
    return err;
}

void media_session_record_stop(uint32_t id)
{
    if (!atomic_load(&s_ready)) return;
    media_record_end(id, XIAOTAI_ERR_INVALID_STATE);
    media_session_wake_playout();
}

void media_session_record_output_free(void)
{
    if (!atomic_load(&s_ready)) return;
    lock();
    s_output_free = true;
    unlock();
    media_session_wake_playout();
}

void media_session_preroll_lost(uint32_t generation)
{
    lock();
    if (s_control.config.generation == generation) {
        s_control.preroll_bound = false;
        media_session_events_record(&s_events, generation, XIAOTAI_MEDIA_EVENT_PREROLL_LOST, XIAOTAI_ERR_INVALID_STATE);
    }
    unlock();
    media_session_wake_playout();
}

void media_session_stop_play(uint32_t id)
{
    if (!atomic_load(&s_ready) || !s_clips) return;
    lock();
    const uint32_t active = media_clip_player_id(s_clips);
    if (active && (!id || id == active)) {
        media_clip_player_cancel(s_clips, active, XIAOTAI_ERR_INVALID_STATE);
        s_output_free = true;
    }
    unlock();
    xiaotai_signal_give(&s_wake);
}

void xiaotai_media_status(xiaotai_media_status_t *status)
{
    if (!status) return;
    *status = (xiaotai_media_status_t){0};
    status->startup_complete = atomic_load(&s_startup_complete);
    if (status->startup_complete) status->startup_error = atomic_load(&s_startup_error);
    if (!atomic_load(&s_initialized)) return;
    lock();
    status->ready = atomic_load(&s_ready) && !atomic_load(&s_open_failure);
    status->generation = s_control.config.generation;
    status->active = session_power();
    status->draining = s_control.phase == MEDIA_SESSION_DRAINING;
    status->uplink_open = s_control.uplink_open;
    status->mic_muted = s_mic_muted;
    status->output_error = s_power_error;
    status->volume_percent = s_volume;
    status->volume_applied = s_volume_applied;
    status->speaker_muted = s_speaker_muted;
    status->speaker_muted_applied = s_mute_applied;
    status->input_gain_db = s_gain_applied;
    status->playing = s_clips ? media_clip_player_id(s_clips) : 0;
    media_preroll_status(media_preroll_current(), status);
    media_record_status(status);
    media_audio_control_stats(s_audio, status);
    unlock();
}

void xiaotai_media_stats(xiaotai_media_stats_t *stats)
{
    if (!stats) return;
    *stats = (xiaotai_media_stats_t){0};
    if (!atomic_load(&s_initialized)) return;
    lock();
    stats->stale_calls = s_control.stale_calls;
    stats->owner_post_retries = media_session_events_retries(&s_events);
    media_rx_stats(stats);
    media_playout_stats(s_playout, stats);
    media_preroll_stats(media_preroll_current(), stats);
    media_record_stats(stats);
    media_audio_stats(s_audio, stats);
    stats->ptt_opens = s_ptt_open;
    stats->ptt_closes = s_ptt_close;
    stats->rx_dropped = stats->rx_stale + stats->rx_invalid + stats->rx_blocked + stats->rx_overflow +
                        stats->rx_decode_failed + stats->rx_flushed;
    unlock();
}

xiaotai_err_t xiaotai_media_set_mic_muted(bool muted)
{
    if (media_stage_initial_controls()) {
        if (!xiaotai_hal_audio_input(0)) return XIAOTAI_ERR_NOT_SUPPORTED;
        s_initial_controls.mic_muted = muted;
        return XIAOTAI_OK;
    }
    if (!atomic_load(&s_ready) || !s_input) return XIAOTAI_ERR_NOT_SUPPORTED;
    lock();
    if (s_mic_muted != muted) {
        invalidate_uplink();
        ++s_mic_epoch;
        s_mic_cutover_us = xiaotai_time_us();
        media_preroll_mic_transition(media_preroll_current(), s_mic_epoch, muted);
    }
    s_mic_muted = muted;
    if (muted) {
        s_control.uplink_open = false;
        media_record_end(0, XIAOTAI_ERR_INVALID_STATE);
    } else if (s_control.phase == MEDIA_SESSION_ACTIVE && s_control.config.duplex && s_control.config.uplink.rate_hz) {
        s_control.uplink_open = true;
    }
    unlock();
    media_session_wake_playout();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_set_input_gain(int8_t gain_db)
{
    const bool initial = media_stage_initial_controls();
    if (!initial && !atomic_load(&s_ready)) return XIAOTAI_ERR_NOT_SUPPORTED;
    const xiaotai_audio_input_t *input = initial ? xiaotai_hal_audio_input(0) : s_input;
    if (!input || !input->set_gain_db) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (gain_db < input->info.gain_min_db || gain_db > input->info.gain_max_db) return XIAOTAI_ERR_INVALID_ARG;
    const int step = input->info.gain_step_db;
    const int minimum = input->info.gain_min_db;
    if (step > 0) gain_db = (int8_t)(minimum + (gain_db - minimum) / step * step);
    if (initial) {
        s_initial_controls.gain = gain_db;
        s_initial_controls.gain_set = true;
        return XIAOTAI_OK;
    }
    lock();
    s_gain = gain_db;
    unlock();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_uplink_gate(uint32_t generation, bool open, uint32_t tail_ms)
{
    if (!atomic_load(&s_ready)) return open ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
    lock();
    if (!media_session_control_matches(&s_control, generation) || s_control.phase != MEDIA_SESSION_ACTIVE ||
        (open && (s_mic_muted || !s_control.config.uplink.rate_hz))) {
        unlock();
        return open ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
    }
    if (s_control.uplink_open != open) {
        s_control.uplink_open = open;
        if (open) { ++s_ptt_open; invalidate_uplink(); }
        else {
            ++s_ptt_close;
            if (tail_ms) {
                s_tail_end_us = xiaotai_time_us();
                s_tail_deadline_us = s_tail_end_us + (int64_t)tail_ms * 1000;
            } else invalidate_uplink();
        }
    } else if (!open && !tail_ms) invalidate_uplink();
    unlock();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_uplink_tone(uint32_t generation, uint32_t duration_ms)
{
    if (!duration_ms || duration_ms > XIAOTAI_MEDIA_TONE_MS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    xiaotai_err_t err = XIAOTAI_OK;
    media_tx_codec_t codec;
    if (!media_session_control_matches(&s_control, generation) || s_control.phase != MEDIA_SESSION_ACTIVE) err = XIAOTAI_ERR_INVALID_STATE;
    else if (!s_audio || !media_tx_codec(&s_control.config.uplink, &codec) || !codec.encoder) err = XIAOTAI_ERR_NOT_SUPPORTED;
    else if (!s_control.uplink_open || s_mic_muted) err = XIAOTAI_ERR_INVALID_STATE;
    else media_audio_tone(s_audio, generation, duration_ms);
    unlock();
    return err;
}

xiaotai_err_t xiaotai_media_set_volume(uint8_t volume)
{
    if (volume > 100) return XIAOTAI_ERR_INVALID_ARG;
    if (media_stage_initial_controls()) {
        const xiaotai_audio_output_t *output = xiaotai_hal_audio_output(0);
        if (!output || !output->set_volume) return XIAOTAI_ERR_NOT_SUPPORTED;
        s_initial_controls.volume = volume;
        s_initial_controls.volume_set = true;
        return XIAOTAI_OK;
    }
    if (!atomic_load(&s_ready) || !s_output || !s_output->set_volume) return XIAOTAI_ERR_NOT_SUPPORTED;
    lock();
    s_volume = volume;
    s_volume_dirty = true;
    unlock();
    media_session_wake_playout();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_set_speaker_muted(bool muted)
{
    if (media_stage_initial_controls()) {
        if (!xiaotai_hal_audio_output(0)) return XIAOTAI_ERR_NOT_SUPPORTED;
        s_initial_controls.speaker_muted = muted;
        return XIAOTAI_OK;
    }
    if (!atomic_load(&s_ready) || !s_output) return XIAOTAI_ERR_NOT_SUPPORTED;
    lock();
    if ((s_mute_after_accept ? s_mute_target : s_speaker_muted) != muted) {
        s_mute_dirty = true;
        if (s_mute_after_accept) s_mute_target = muted;
        else set_speaker_gate_locked(muted);
    }
    unlock();
    media_session_wake_playout();
    return XIAOTAI_OK;
}
