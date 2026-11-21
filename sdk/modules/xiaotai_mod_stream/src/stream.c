// SPDX-License-Identifier: MIT
#include "stream_internal.h"

#include <stdatomic.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#if XIAOTAI_STREAM_HAS_CONTACTS
#include "xiaotai/modules/contacts.h"
#endif

#define SOURCE "stream"
#define COMMAND_MAX 256U
#define COMMAND_POOL 512U
#define BURST_MS 200U
#define RETRY_MS 1000U

static const xiaotai_media_format_t s_alaw = XIAOTAI_MEDIA_FORMAT_G711A_8K;
static const xiaotai_media_format_t s_opus = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
static const xiaotai_media_latency_t s_latency = XIAOTAI_MEDIA_LATENCY_LOW;

typedef struct {
    xiaotai_stream_config_t config;
    xiaotai_stream_status_t status;
    xiaotai_stream_stats_t stats;
    uint32_t listener, activity, binding, timer, next_hold;
    uint32_t holds[XIAOTAI_STREAM_HOLDS_MAX];
    bool initialized, started, listening, wait_idle, exchange_waiting, environment_gate_held;
    bool sends_audio, sends_video, media_open, ending, contact_confirmation;
} stream_t;

static stream_t s_stream;
/* Only snapshots are shared: owner callbacks may re-enter the module through
 * the activity attachments. Never call framework or engine code under this lock. */
static xiaotai_critical_t s_lock;
static atomic_bool s_lock_ready;
static xiaotai_stream_status_t s_status;
static xiaotai_stream_stats_t s_stats;

static void snapshot(void)
{
    if (!atomic_load_explicit(&s_lock_ready, memory_order_acquire)) return;
    xiaotai_critical_enter(&s_lock);
    s_status = s_stream.status;
    s_stats = s_stream.stats;
    xiaotai_critical_exit(&s_lock);
}

void xiaotai_stream_status(xiaotai_stream_status_t *status)
{
    if (!status) return;
    if (!atomic_load_explicit(&s_lock_ready, memory_order_acquire)) {
        *status = (xiaotai_stream_status_t){0};
        return;
    }
    xiaotai_critical_enter(&s_lock);
    *status = s_status;
    xiaotai_critical_exit(&s_lock);
}

void xiaotai_stream_stats(xiaotai_stream_stats_t *stats)
{
    if (!stats) return;
    if (!atomic_load_explicit(&s_lock_ready, memory_order_acquire)) {
        *stats = (xiaotai_stream_stats_t){0};
        return;
    }
    xiaotai_critical_enter(&s_lock);
    *stats = s_stats;
    xiaotai_critical_exit(&s_lock);
}

const xiaotai_stream_config_t *stream_config(void) { return &s_stream.config; }
bool stream_has_video(void) { return s_stream.status.video_declared; }

void stream_profile_overflow(void)
{
    ++s_stream.stats.profile_overflows;
    snapshot();
}

xiaotai_err_t xiaotai_stream_configure(const xiaotai_stream_config_t *config)
{
    if (s_stream.initialized) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_stream_config_t c = config ? *config : (xiaotai_stream_config_t){0};
    switch (c.platform_idle) {
    case XIAOTAI_STREAM_PLATFORM_IDLE_AUTO:
    case XIAOTAI_STREAM_PLATFORM_IDLE_WAIT:
    case XIAOTAI_STREAM_PLATFORM_IDLE_IGNORE: break;
    default: return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!c.up_audio_stream) c.up_audio_stream = XIAOTAI_STREAM_UP_AUDIO;
    if (!c.down_audio_stream) c.down_audio_stream = XIAOTAI_STREAM_DOWN_AUDIO;
    if (!c.up_video_stream) c.up_video_stream = XIAOTAI_STREAM_UP_VIDEO;
    if (!c.down_video_stream) c.down_video_stream = XIAOTAI_STREAM_DOWN_VIDEO;
    if (c.up_audio_stream > XIAOTAI_LINK_STREAM_MAX || c.down_audio_stream > XIAOTAI_LINK_STREAM_MAX ||
        c.up_video_stream > XIAOTAI_LINK_STREAM_MAX || c.down_video_stream > XIAOTAI_LINK_STREAM_MAX ||
        c.up_audio_stream == c.up_video_stream || c.down_audio_stream == c.down_video_stream) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    s_stream.config = c;
    return XIAOTAI_OK;
}

static void emit(const char *event, uint32_t generation, int32_t value, const void *data, size_t size)
{
    const xiaotai_args_t args = {.value = value};
    const xiaotai_err_t err = xiaotai_event_emit(event, &args, data, size);
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "event.queue_full", generation, err);
}

static uint32_t admission_reasons(const xiaotai_platform_status_t *platform)
{
    int32_t enabled = 1;
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_STREAM_ENABLED, &enabled);
    uint32_t reasons = 0;
    if (!enabled) reasons |= XIAOTAI_STREAM_CLOSED_DISABLED;
    if (s_stream.status.holds) reasons |= XIAOTAI_STREAM_CLOSED_HELD;
    if (xiaotai_quiesced()) reasons |= XIAOTAI_STREAM_CLOSED_QUIESCED;
    if (platform->phase != XIAOTAI_PLATFORM_ONLINE || platform->verifying ||
        xiaotai_transport_state() != XIAOTAI_TRANSPORT_ONLINE) reasons |= XIAOTAI_STREAM_CLOSED_OFFLINE;
    if (!s_stream.config.admit_before_profile && !platform->profile_ready) reasons |= XIAOTAI_STREAM_CLOSED_PROFILE;
    if (s_stream.wait_idle && platform->exchanges) reasons |= XIAOTAI_STREAM_CLOSED_PLATFORM;
    if (s_stream.config.wait_initial_contact_confirmation) {
        bool confirmed = false;
#if XIAOTAI_STREAM_HAS_CONTACTS
        confirmed = s_stream.contact_confirmation && xiaotai_contacts_initial_confirmation_done();
#endif
        if (!confirmed) reasons |= XIAOTAI_STREAM_CLOSED_CONTACTS;
    }
    return reasons;
}

static void retry(void *ctx)
{
    (void)ctx;
    s_stream.timer = 0;
    stream_admission_update();
}

static xiaotai_err_t apply_admission(bool exchange_pending)
{
    if (!s_stream.started) return XIAOTAI_OK;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    uint32_t reasons = admission_reasons(&platform);
    if (exchange_pending) reasons |= XIAOTAI_STREAM_CLOSED_PLATFORM;
    const bool previous = s_stream.listening;
    xiaotai_err_t result = XIAOTAI_OK;
    if ((reasons == 0) != previous && s_stream.listener) {
        const xiaotai_err_t err = xiaotai_link_listen_set(s_stream.listener, reasons == 0 ?
                                                        XIAOTAI_LINK_LISTEN_OPEN : XIAOTAI_LINK_LISTEN_OFF);
        if (err == XIAOTAI_OK) {
            s_stream.listening = reasons == 0;
        } else {
            result = err;
            ++s_stream.stats.listen_failures;
            reasons |= XIAOTAI_STREAM_CLOSED_MEMORY;
            xiaotai_eventlog_warn(SOURCE, "listen.set", 0, err);
            if (!s_stream.timer &&
                xiaotai_timer_start(RETRY_MS, retry, NULL, &s_stream.timer) != XIAOTAI_OK) {
                xiaotai_eventlog_warn(SOURCE, "listen.retry", 0, XIAOTAI_ERR_NO_MEM);
            }
        }
    }
    if (!s_stream.listener) reasons |= XIAOTAI_STREAM_CLOSED_MEMORY;
    if (!(reasons & XIAOTAI_STREAM_CLOSED_MEMORY) && s_stream.timer) {
        xiaotai_timer_cancel(s_stream.timer);
        s_stream.timer = 0;
    }
    const xiaotai_stream_phase_t idle = s_stream.listening && reasons == 0 ?
                                        XIAOTAI_STREAM_LISTENING : XIAOTAI_STREAM_CLOSED;
    if (!s_stream.status.generation) {
        s_stream.status.phase = idle;
        s_stream.status.closed = reasons;
    }
    snapshot();
    if (previous != s_stream.listening) emit(XIAOTAI_EVENT_STREAM_STATE, 0, idle, NULL, 0);
    return result;
}

void stream_admission_update(void) { (void)apply_admission(false); }

static bool admit_open(void *ctx)
{
    (void)ctx;
    /* Transport can invoke this under its slot lock; no status or bus calls. */
    return !xiaotai_platform_exchanges_busy();
}

static xiaotai_err_t environment_gate_acquire(const char *reason, void *ctx)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) || !s_stream.listener) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_link_listener_open_gate(s_stream.listener, true);
    if (err == XIAOTAI_OK) s_stream.environment_gate_held = true;
    return err;
}

static void environment_gate_release(const char *reason, void *ctx)
{
    (void)reason;
    (void)ctx;
    if (!s_stream.environment_gate_held) return;
    /* Recheck the current platform state before exposing the old listen mode. */
    stream_admission_update();
    s_stream.environment_gate_held = false;
    const xiaotai_err_t err = xiaotai_link_listener_open_gate(s_stream.listener, false);
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "environment.gate_release", s_stream.listener, err);
}

static const xiaotai_quiesce_gate_t environment_gate = {
    .acquire = environment_gate_acquire, .release = environment_gate_release,
};

static xiaotai_err_t exchange_preflight(void *ctx)
{
    (void)ctx;
    s_stream.exchange_waiting = false;
    const xiaotai_err_t err = apply_admission(true);
    if (err != XIAOTAI_OK) return err == XIAOTAI_ERR_BUSY ? XIAOTAI_ERR_INVALID_STATE : err;
    if (s_stream.listener && xiaotai_link_listener_retiring(s_stream.listener)) {
        s_stream.exchange_waiting = true;
        return XIAOTAI_ERR_BUSY;
    }
    return XIAOTAI_OK;
}

static void on_link_retirement(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    if (s_stream.exchange_waiting && !xiaotai_link_listener_retiring(s_stream.listener)) {
        s_stream.exchange_waiting = false;
        xiaotai_platform_exchange_preflight_resume();
    }
}

static void report_end(uint32_t generation, xiaotai_stream_end_t end, xiaotai_err_t error, bool started)
{
    switch (end) {
    case XIAOTAI_STREAM_END_PEER: ++s_stream.stats.ended_peer; break;
    case XIAOTAI_STREAM_END_LOCAL: ++s_stream.stats.ended_local; break;
    case XIAOTAI_STREAM_END_REVOKED: ++s_stream.stats.ended_revoked; break;
    case XIAOTAI_STREAM_END_BUSY: ++s_stream.stats.refused_busy; break;
    case XIAOTAI_STREAM_END_CLOSED: ++s_stream.stats.refused_closed; break;
    case XIAOTAI_STREAM_END_LINK: ++s_stream.stats.ended_link; break;
    case XIAOTAI_STREAM_END_MEDIA: ++s_stream.stats.ended_media; break;
    case XIAOTAI_STREAM_END_BINDING: ++s_stream.stats.ended_binding; break;
    }
    s_stream.status.last_end = end;
    s_stream.status.last_error = error;
    snapshot();
    xiaotai_eventlog_note(SOURCE, "ended", generation, end);
    if (error != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "end.error", generation, error);
    const xiaotai_stream_ended_t data = {.generation = generation, .end = end, .error = error, .started = started};
    emit(XIAOTAI_EVENT_STREAM_ENDED, generation, end, &data, sizeof(data));
}

static bool pause_for_local_stop(bool pause)
{
    const xiaotai_err_t err = xiaotai_link_listener_pause_open(s_stream.listener, pause);
    if (err == XIAOTAI_OK) return true;
    ++s_stream.stats.listen_failures;
    xiaotai_eventlog_warn(SOURCE, pause ? "listen.stop.pause" : "listen.stop.resume", s_stream.status.generation, err);
    return false;
}

static void finish(xiaotai_stream_end_t end, xiaotai_err_t error, bool link_closed)
{
    const uint32_t generation = s_stream.status.generation;
    if (!generation || s_stream.ending) return;
    s_stream.ending = true;
    const bool paused = end == XIAOTAI_STREAM_END_LOCAL && s_stream.config.pause_admission_on_local_stop &&
                        s_stream.listener && pause_for_local_stop(true);
    const bool started = s_stream.status.phase == XIAOTAI_STREAM_ACTIVE;
    if (s_stream.activity) {
        xiaotai_activity_set_generation(s_stream.activity, 0);
        xiaotai_activity_end(s_stream.activity);
        s_stream.activity = 0;
    }
    if (s_stream.media_open) {
        xiaotai_media_close(generation);
        s_stream.media_open = false;
    }
    if (!link_closed) xiaotai_link_disconnect(generation);
    s_stream.status.generation = 0;
    s_stream.status.peer_audio = false;
    s_stream.status.peer_video = false;
    s_stream.binding = 0;
    s_stream.ending = false;
    stream_admission_update();
    if (paused) (void)pause_for_local_stop(false);
    report_end(generation, end, error, started);
}

static void revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)ctx;
    if (id != s_stream.activity) return;
    s_stream.activity = 0;   /* The framework already ended the attachments. */
    xiaotai_eventlog_note(SOURCE, "revoked", s_stream.status.generation, reason);
    finish(XIAOTAI_STREAM_END_REVOKED, XIAOTAI_OK, false);
}

static void media_event(uint32_t generation, xiaotai_media_event_t event, xiaotai_err_t error, void *ctx)
{
    (void)ctx;
    if (generation == s_stream.status.generation && event == XIAOTAI_MEDIA_EVENT_FAILED) {
        finish(XIAOTAI_STREAM_END_MEDIA, error, false);
    }
}

static const char *preemption_label(const char *name)
{
    /* Event log retains static strings and its vocabulary is bounded. */
    static const struct { const char *name, *label; } labels[] = {
        {"ai", "preempts.ai"}, {"call", "preempts.call"}, {"room", "preempts.room"},
        {"recognition", "preempts.recognition"}, {"voip", "preempts.voip"},
    };
    for (size_t i = 0; name && i < sizeof(labels) / sizeof(labels[0]); ++i) {
        if (strcmp(name, labels[i].name) == 0) return labels[i].label;
    }
    return "preempts.other";
}

static void opened(void *ctx, uint32_t generation)
{
    (void)ctx;
    ++s_stream.stats.admitted;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (!s_stream.listening || admission_reasons(&platform)) {
        xiaotai_link_disconnect(generation);
        stream_admission_update();
        report_end(generation, XIAOTAI_STREAM_END_CLOSED, XIAOTAI_OK, false);
        return;
    }
    if (s_stream.status.generation) {
        xiaotai_link_disconnect(generation);
        report_end(generation, XIAOTAI_STREAM_END_BUSY, XIAOTAI_ERR_BUSY, false);
        return;
    }
    s_stream.status.phase = XIAOTAI_STREAM_STARTING;
    s_stream.status.generation = generation;
    s_stream.status.closed = 0;
    s_stream.binding = platform.generation;
    snapshot();  /* Attachments see STARTING and the link before begin(). */

    const char *previous = NULL;
    const uint32_t previous_id = xiaotai_activity_current(&previous);
    const char *label = previous_id ? preemption_label(previous) : NULL;
    xiaotai_activity_t activity = {
        .name = XIAOTAI_STREAM_ACTIVITY,
        .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT | XIAOTAI_RESOURCE_LINK,
        .revoked = revoked, .generation = generation,
    };
    if (s_stream.sends_video) activity.resources |= XIAOTAI_RESOURCE_CAMERA;
    xiaotai_err_t err = xiaotai_activity_begin(&activity, &s_stream.activity);
    if (err != XIAOTAI_OK) {
        finish(XIAOTAI_STREAM_END_BUSY, err, false);
        return;
    }
    if (previous_id) {
        ++s_stream.stats.preempted_others;
        xiaotai_eventlog_note(SOURCE, label, generation, 0);
    }
    const xiaotai_media_session_t session = {
        .generation = generation, .uplink = s_stream.sends_audio ? s_alaw : (xiaotai_media_format_t){0},
        .downlink = s_alaw, .downlink_burst_ms = BURST_MS,
        .duplex = true, .sink = xiaotai_link_audio_sink(), .latency = s_latency, .event = media_event,
    };
    err = xiaotai_media_open(&session);
    if (err != XIAOTAI_OK) {
        finish(XIAOTAI_STREAM_END_MEDIA, err, false);
        return;
    }
    s_stream.media_open = true;
    err = xiaotai_link_receive(generation, XIAOTAI_LINK_AUDIO, NULL, NULL);
    if (err != XIAOTAI_OK) {
        finish(XIAOTAI_STREAM_END_LINK, err, false);
        return;
    }
    s_stream.status.phase = XIAOTAI_STREAM_ACTIVE;
    ++s_stream.stats.started;
    snapshot();
    xiaotai_eventlog_note(SOURCE, "started", generation, 0);
    emit(XIAOTAI_EVENT_STREAM_STARTED, generation, (int32_t)generation, NULL, 0);
}

static void closed(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)ctx;
    if (generation != s_stream.status.generation || !generation) return;
    if (detail) xiaotai_eventlog_warn(SOURCE, "link.detail", generation, detail);
    const bool peer = reason == XIAOTAI_LINK_END_PEER || reason == XIAOTAI_LINK_END_PEER_FAULT;
    xiaotai_err_t error = XIAOTAI_FAIL;
    switch (reason) {
    case XIAOTAI_LINK_END_PEER:
    case XIAOTAI_LINK_END_PEER_FAULT: error = XIAOTAI_OK; break;
    case XIAOTAI_LINK_END_TIMEOUT: error = XIAOTAI_ERR_TIMEOUT; break;
    case XIAOTAI_LINK_END_NO_MEMORY:
    case XIAOTAI_LINK_END_OVERFLOW: error = XIAOTAI_ERR_NO_MEM; break;
    case XIAOTAI_LINK_END_STOPPED: error = XIAOTAI_ERR_INVALID_STATE; break;
    case XIAOTAI_LINK_END_FORMAT: error = XIAOTAI_ERR_NOT_SUPPORTED; break;
    default: break;
    }
    finish(peer ? XIAOTAI_STREAM_END_PEER : XIAOTAI_STREAM_END_LINK, error, true);
}

static void peer_subscribed(void *ctx, uint32_t generation, xiaotai_link_media_kind_t kind, uint8_t stream,
                            bool subscribed)
{
    (void)ctx;
    if (generation != s_stream.status.generation || !generation) return;
    if (kind == XIAOTAI_LINK_AUDIO && stream == s_stream.config.up_audio_stream) {
        s_stream.status.peer_audio = subscribed;
    } else if (kind == XIAOTAI_LINK_VIDEO && stream == s_stream.config.up_video_stream) {
        s_stream.status.peer_video = subscribed;
    }
    snapshot();
}

static void state_event(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    stream_admission_update();
}

static void platform_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (s_stream.status.generation &&
        (platform.generation != s_stream.binding || platform.verifying ||
         strcmp(event->name, XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED) == 0 ||
         (strcmp(event->name, XIAOTAI_EVENT_PLATFORM_OFFLINE) == 0 &&
          event->args.value == XIAOTAI_PLATFORM_OFFLINE_BINDING))) {
        finish(XIAOTAI_STREAM_END_BINDING, XIAOTAI_OK, false);
        return;
    }
    stream_admission_update();
}

static void stop_job(void *ctx)
{
    if ((uint32_t)(uintptr_t)ctx != s_stream.status.generation) {
        ++s_stream.stats.stale_stops;
        snapshot();
    } else {
        stream_stop_current();
    }
}

xiaotai_err_t xiaotai_stream_stop(uint32_t generation)
{
    return generation ? xiaotai_run(stop_job, (void *)(uintptr_t)generation) : XIAOTAI_ERR_INVALID_ARG;
}

void stream_stop_current(void) { finish(XIAOTAI_STREAM_END_LOCAL, XIAOTAI_OK, false); }

static bool hold_exists(uint32_t hold)
{
    for (size_t i = 0; i < XIAOTAI_STREAM_HOLDS_MAX; ++i) {
        if (s_stream.holds[i] == hold) return true;
    }
    return false;
}

xiaotai_err_t xiaotai_stream_hold(const char *reason, uint32_t *hold)
{
    if (!reason || !hold) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < XIAOTAI_STREAM_HOLDS_MAX; ++i) {
        if (s_stream.holds[i]) continue;
        do { ++s_stream.next_hold; } while (!s_stream.next_hold || hold_exists(s_stream.next_hold));
        *hold = s_stream.holds[i] = s_stream.next_hold;
        ++s_stream.status.holds;
        xiaotai_eventlog_note(SOURCE, "hold.taken", *hold, s_stream.status.holds);
        stream_admission_update();
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NO_MEM;
}

void xiaotai_stream_release(uint32_t hold)
{
    if (!hold) return;
    for (size_t i = 0; i < XIAOTAI_STREAM_HOLDS_MAX; ++i) {
        if (s_stream.holds[i] != hold) continue;
        s_stream.holds[i] = 0;
        --s_stream.status.holds;
        xiaotai_eventlog_note(SOURCE, "hold.released", hold, s_stream.status.holds);
        stream_admission_update();
        return;
    }
}

xiaotai_err_t xiaotai_module_stream_init(void)
{
    if (!s_stream.config.up_audio_stream) (void)xiaotai_stream_configure(NULL);
    s_stream.wait_idle = s_stream.config.platform_idle == XIAOTAI_STREAM_PLATFORM_IDLE_WAIT ||
                         (s_stream.config.platform_idle == XIAOTAI_STREAM_PLATFORM_IDLE_AUTO &&
                          !xiaotai_mem_has_external());
    s_stream.initialized = true;
    if (!atomic_load_explicit(&s_lock_ready, memory_order_relaxed)) {
        xiaotai_critical_init(&s_lock);
        atomic_store_explicit(&s_lock_ready, true, memory_order_release);
    }
    const xiaotai_transport_requirement_t transport = {.links = 1, .command_pool_bytes = COMMAND_POOL};
    xiaotai_media_requirement_t audio = {
        .uplink = s_alaw, .downlink = s_alaw, .downlink_burst_ms = BURST_MS, .packet_ms = 20,
        .max_packet_bytes = 640, .latency = s_latency,
    };
    xiaotai_err_t err = xiaotai_transport_require(&transport);
    if (err == XIAOTAI_OK) err = xiaotai_media_require(&audio);
    audio.uplink = (xiaotai_media_format_t){0};
    audio.downlink = s_opus;
    if (err == XIAOTAI_OK) err = xiaotai_media_require(&audio);
    if (err == XIAOTAI_OK) err = stream_profile_register();
    if (err == XIAOTAI_OK) err = stream_controls_register();
    if (err == XIAOTAI_OK && s_stream.config.preserve_inbound_on_environment)
        err = xiaotai_quiesce_gate_register(SOURCE, &environment_gate, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_TRANSPORT_STATE, state_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_QUIESCE, state_event, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ACTIVITY, state_event, NULL);
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_PLATFORM_ONLINE, platform_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, platform_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, platform_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_PROFILE, platform_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_PAUSED, platform_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_IDLE, platform_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_DISCOVERED, platform_event, NULL},
        {XIAOTAI_EVENT_PLATFORM_CLOCK, platform_event, NULL},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(events[0]); ++i) {
        err = xiaotai_event_subscribe_static(&events[i]);
    }
    if (err == XIAOTAI_OK && s_stream.wait_idle) {
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_TRANSPORT_LINKS, on_link_retirement, NULL);
        if (err == XIAOTAI_OK) err = xiaotai_platform_exchange_preflight_register(exchange_preflight, NULL);
    }
    if (err == XIAOTAI_OK && s_stream.config.wait_initial_contact_confirmation) {
        xiaotai_err_t contact_err = XIAOTAI_ERR_NOT_SUPPORTED;
#if XIAOTAI_STREAM_HAS_CONTACTS
        contact_err = xiaotai_contacts_require_initial_confirmation();
        if (contact_err == XIAOTAI_OK)
            err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_CONTACTS_PENDING, state_event, NULL);
#endif
        s_stream.contact_confirmation = contact_err == XIAOTAI_OK;
        if (contact_err != XIAOTAI_OK) xiaotai_eventlog_error(SOURCE, "contacts.unavailable", 0, contact_err);
    }
    return err;
}

static xiaotai_link_media_t video_contribution(void)
{
    const void *first = NULL;
    const size_t count = xiaotai_activity_contributions(XIAOTAI_STREAM_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                                                         XIAOTAI_STREAM_PROFILE_SCENE, &first, 1);
    const xiaotai_link_media_t *part = first;
    bool receives = false;
    for (size_t i = 0; part && i < XIAOTAI_LINK_RECEIVE_FORMATS; ++i) {
        if (part->receive_formats[i].codec != XIAOTAI_LINK_CODEC_NONE) receives = true;
    }
    const bool usable = part && (part->send || part->receive) && (!part->receive || receives);
    for (size_t i = usable ? 1 : 0; i < count; ++i) {
        ++s_stream.stats.extensions_ignored;
        xiaotai_eventlog_warn(SOURCE, "extension.ignored", 0, (int32_t)i);
    }
    if (!usable) return (xiaotai_link_media_t){0};
    xiaotai_link_media_t video = *part;
    video.send_stream = s_stream.config.up_video_stream;
    video.receive_stream = s_stream.config.down_video_stream;
    return video;
}

void xiaotai_module_stream_start(void)
{
    if (s_stream.started) return;
    s_stream.started = true;
    s_stream.sends_audio = xiaotai_media_can_send(&s_alaw);
    const xiaotai_link_media_t video = video_contribution();
    s_stream.status.video_declared = video.send || video.receive;
    s_stream.sends_video = video.send;
    const xiaotai_link_format_t g711 = {XIAOTAI_LINK_CODEC_G711A, 8000U, 1U};
    xiaotai_link_listener_t listener = {
        .name = SOURCE,
        .admit_open = s_stream.wait_idle ? admit_open : NULL,
        .handlers = {.opened = opened, .closed = closed, .peer_subscribed = peer_subscribed},
        .profile = {
            .audio = {
                .send = s_stream.sends_audio, .send_stream = s_stream.config.up_audio_stream,
                .send_format = g711, .receive = true, .receive_stream = s_stream.config.down_audio_stream,
                .receive_request = true, .receive_formats = {g711},
            },
            .video = video, .command_max_bytes = COMMAND_MAX, .command_pool_bytes = COMMAND_POOL,
        },
    };
    if (!s_stream.config.receive_pcma_only && xiaotai_media_can_receive(&s_opus)) {
        listener.profile.audio.receive_formats[1] = (xiaotai_link_format_t){XIAOTAI_LINK_CODEC_OPUS, 16000U, 1U};
    }
    const xiaotai_err_t err = xiaotai_link_listen(&listener, &s_stream.listener);
    if (err != XIAOTAI_OK) {
        ++s_stream.stats.listen_failures;
        xiaotai_eventlog_warn(SOURCE, "listen.register", 0, err);
    }
    stream_admission_update();
}

void xiaotai_stream_reset(void)
{
    s_stream = (stream_t){0};
    snapshot();
}
