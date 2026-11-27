// SPDX-License-Identifier: MIT
#include "call_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/modules/contacts.h"
#ifdef XIAOTAI_CALL_HAS_SETTINGS
#include "xiaotai/modules/settings.h"
#endif
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#define SOURCE "call"
#define BURST_MS 200U
#define EARLY_LINK_MS 10000U
#define REPAIR_MS 15000U
#define RELEASE_SLOTS_MAX 8U
#define CANCEL_HOLD_MS 10000U

static const xiaotai_media_format_t audio = XIAOTAI_MEDIA_FORMAT_G711A_8K;
static const xiaotai_media_latency_t fallback_latency = XIAOTAI_MEDIA_LATENCY_RELAYED;
static const xiaotai_link_media_t link_audio = {
    .send = true, .send_stream = 10, .peer_streams = XIAOTAI_LINK_PEER_STREAM_REFUSE,
    .send_format = {XIAOTAI_LINK_CODEC_G711A, 8000, 1},
    .receive = true, .receive_stream = 10, .receive_request = true,
    .receive_formats = {{XIAOTAI_LINK_CODEC_G711A, 8000, 1}},
};

static struct {
    xiaotai_call_config_t config;
    xiaotai_call_status_t status;
    call_diagnostic_counters_t diag;
    xiaotai_link_media_t video;
    int contact_kind;
    bool initialized, settings, opened, answered, media, link_dead;
    uint32_t binding, release_binding, activity, listener[2], armed, timer, closing;
    uint32_t hold_listener, hold_timer, hold_generation;
    int64_t deadline_us, hold_deadline_us;
    char room[CALL_ROOM_SIZE];
    char early_answer_room[CALL_ROOM_SIZE];
    struct {
        bool attempted;
        call_repair_step_t step;
        char room[CALL_ROOM_SIZE];
    } repair;
} s_call;

/* Changes even when another call starts and ends before a queued dial runs. */
static _Atomic uint32_t s_dial_revision;

const xiaotai_call_config_t *call_config(void) { return &s_call.config; }

static void copy_text(char *out, size_t size, const char *text)
{
    size_t count = strlen(text);
    if (count >= size) count = size - 1U;
    memcpy(out, text, count);
    out[count] = '\0';
}

static bool default_latency(const xiaotai_media_latency_t *p)
{
    return !p->start_ms && !p->base_ms && !p->min_ms && !p->max_ms && !p->surplus_ms &&
           !p->settle_s && !p->slow_permille && !p->steady_jitter_ms && !p->steady_peak_ms && !p->step_down_ms;
}

xiaotai_err_t xiaotai_call_configure(const xiaotai_call_config_t *config)
{
    if (s_call.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (config && (config->pending_releases > RELEASE_SLOTS_MAX ||
                   config->ring_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS ||
                   config->answer_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS ||
                   config->connect_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS)) return XIAOTAI_ERR_INVALID_ARG;
    if (config && config->incoming_busy_during) {
        for (size_t i = 0; config->incoming_busy_during[i]; ++i) {
            if (i >= XIAOTAI_CALL_BUSY_DURING_MAX || !config->incoming_busy_during[i][0]) {
                return XIAOTAI_ERR_INVALID_ARG;
            }
        }
    }
    s_call.config = config ? *config : (xiaotai_call_config_t){0};
    return XIAOTAI_OK;
}

static void emit(const char *event, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) copy_text(args.text, sizeof(args.text), text);
    if (xiaotai_event_emit(event, &args, &s_call.status, sizeof(s_call.status)) != XIAOTAI_OK) {
        xiaotai_eventlog_warn(SOURCE, "event.queue_full", s_call.status.session, value);
    }
}

static void state_changed(void)
{
    emit(XIAOTAI_EVENT_CALL_STATE, s_call.status.state, NULL);
}

void xiaotai_call_status(xiaotai_call_status_t *status)
{
    if (status) *status = s_call.status;
}

bool call_signaling_current(uint32_t session, uint32_t link)
{
    return s_call.status.state == XIAOTAI_CALL_ACTIVE && s_call.status.session == session &&
           s_call.status.link == link && s_call.binding == s_call.release_binding;
}

void call_diagnostic_counters(call_diagnostic_counters_t *out) { *out = s_call.diag; }

void call_profile_fallback(void) { ++s_call.diag.profile_fallbacks; }

void call_profile_parts_ignored(size_t count) { s_call.diag.profile_parts_ignored += (uint32_t)count; }

static bool setting_muted(void)
{
#ifdef XIAOTAI_CALL_HAS_SETTINGS
    int32_t value = 0;
    if (s_call.settings && xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &value) == XIAOTAI_OK) {
        return value != 0;
    }
#endif
    return false;
}

static void cancel_timer(void)
{
    if (s_call.timer) xiaotai_timer_cancel(s_call.timer);
    s_call.timer = 0;
    s_call.deadline_us = 0;
}

static void close_link(void)
{
    const uint32_t link = s_call.status.link ? s_call.status.link : s_call.closing;
    s_call.status.link = s_call.closing = 0;
    s_call.opened = s_call.link_dead = false;
    if (link) xiaotai_link_disconnect(link);
    if (s_call.activity) xiaotai_activity_end(s_call.activity);
    s_call.activity = 0;
    s_call.status.releasing = false;
    atomic_fetch_add(&s_dial_revision, 1U);
}

static void close_after_delay(void *context)
{
    if (!s_call.status.releasing || s_call.closing != (uint32_t)(uintptr_t)context) return;
    s_call.timer = 0;
    close_link();
    state_changed();
}

static void disarm(void)
{
    if (s_call.armed) (void)xiaotai_link_listen_set(s_call.armed, XIAOTAI_LINK_LISTEN_OFF);
    s_call.armed = 0;
}

static xiaotai_err_t arm_listener(uint32_t listener)
{
    if (!listener) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (s_call.armed == listener) return XIAOTAI_OK;
    disarm();
    const xiaotai_err_t err = xiaotai_link_listen_set(listener, XIAOTAI_LINK_LISTEN_ONCE);
    if (err == XIAOTAI_OK) s_call.armed = listener;
    return err;
}

static xiaotai_err_t arm_outgoing(void)
{
    const uint32_t listener = s_call.listener[(s_call.status.flags & XIAOTAI_CALL_VIDEO) != 0];
    return arm_listener(listener);
}

static void clear_hold(void)
{
    if (s_call.hold_timer) xiaotai_timer_cancel(s_call.hold_timer);
    if (s_call.hold_listener) {
        const xiaotai_err_t err = xiaotai_link_listener_block_inbound(s_call.hold_listener, false);
        if (err != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "release.unblock", 0, err);
    }
    s_call.hold_timer = s_call.hold_listener = s_call.hold_generation = 0;
    s_call.hold_deadline_us = 0;
}

static void hold_expired(void *context)
{
    if (s_call.hold_generation != (uint32_t)(uintptr_t)context) return;
    s_call.hold_timer = 0;
    const int64_t left = s_call.hold_deadline_us - xiaotai_time_us();
    if (left > 0) {
        /* timers_run() freed this callback's slot before calling us, and no
         * other owner job runs here. Renewal therefore needs no second slot. */
        const uint32_t delay = (uint32_t)((left + 999) / 1000);
        const xiaotai_err_t err = xiaotai_timer_start(delay, hold_expired, context, &s_call.hold_timer);
        if (err != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "release.hold", 0, err);
        return; // Never withdraw an accepted hold when scheduling fails.
    }
    clear_hold(); // A new call's ordinary ONCE is already armed behind the block.
}

uint32_t xiaotai_call_answer_hold_ms(void)
{
    if (!s_call.hold_listener) return 0;
    const int64_t left = s_call.hold_deadline_us - xiaotai_time_us();
    return left > 0 ? (uint32_t)((left + 999) / 1000) : 1U;
}

xiaotai_err_t call_hold_prepare(bool video, bool *prepared)
{
    *prepared = false;
    if (s_call.hold_listener) return s_call.hold_timer ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
    const uint32_t listener = s_call.listener[video];
    xiaotai_err_t err = xiaotai_link_listener_block_inbound(listener, true);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_warn(SOURCE, "release.hold", 0, err);
        return err;
    }
    s_call.hold_listener = listener;
    s_call.hold_generation = xiaotai_generation_next();
    s_call.hold_deadline_us = xiaotai_time_us() + (int64_t)CANCEL_HOLD_MS * 1000;
    err = xiaotai_timer_start(CANCEL_HOLD_MS, hold_expired,
                            (void *)(uintptr_t)s_call.hold_generation, &s_call.hold_timer);
    if (err != XIAOTAI_OK) {
        clear_hold();
        xiaotai_eventlog_warn(SOURCE, "release.hold", 0, err);
        return err;
    }
    *prepared = true;
    return XIAOTAI_OK;
}

void call_hold_after_submit(bool prepared, bool accepted)
{
    if (!accepted) {
        if (prepared) clear_hold();
        return;
    }
    if (!s_call.hold_listener) return;
    s_call.hold_deadline_us = xiaotai_time_us() + (int64_t)CANCEL_HOLD_MS * 1000;
}

static void release_binding(uint32_t binding)
{
    if (binding == s_call.release_binding) return;
    call_signaling_reset();
    disarm();
    clear_hold();
    call_release_clear(binding);
    s_call.release_binding = binding;
}

/* How a call ended, in words; ends past PEER_CANCEL that are not the user's
 * doing are warnings with the error behind them. */
static void log_ended(uint32_t session, unsigned end, xiaotai_err_t error, int64_t talked_us)
{
    static const char *const why[] = {
        [XIAOTAI_CALL_END_NONE] = "ended",
        [XIAOTAI_CALL_END_LOCAL] = "hung up here",
        [XIAOTAI_CALL_END_PEER_HANGUP] = "the peer hung up",
        [XIAOTAI_CALL_END_PEER_REJECT] = "the peer rejected the call",
        [XIAOTAI_CALL_END_PEER_CANCEL] = "the caller gave up",
        [XIAOTAI_CALL_END_NO_ANSWER] = "nobody answered in time",
        [XIAOTAI_CALL_END_MISSED] = "missed: neither accepted nor rejected in time",
        [XIAOTAI_CALL_END_BUSY] = "busy: this device or the peer is in another call",
        [XIAOTAI_CALL_END_FAILED] = "the server refused or did not answer the request",
        [XIAOTAI_CALL_END_NETWORK] = "the link could not be made or broke",
        [XIAOTAI_CALL_END_NO_MEMORY] = "out of memory",
        [XIAOTAI_CALL_END_AUDIO] = "the audio session could not start or failed",
        [XIAOTAI_CALL_END_PREEMPTED] = "a higher-priority activity began",
        [XIAOTAI_CALL_END_SYSTEM] = "the system was quiesced or the binding ended",
    };
    const char *text = end < sizeof(why) / sizeof(why[0]) ? why[end] : "unknown end";
    const unsigned seconds = (unsigned)(talked_us / 1000000);
    if (end >= XIAOTAI_CALL_END_BUSY && end != XIAOTAI_CALL_END_PREEMPTED && end != XIAOTAI_CALL_END_SYSTEM) {
        XIAOTAI_LOGW(SOURCE, "ended gen=%" PRIu32 ": %s (error 0x%x)", session, text, (unsigned)error);
    } else if (talked_us > 0) {
        XIAOTAI_LOGI(SOURCE, "ended gen=%" PRIu32 ": %s after %u s", session, text, seconds);
    } else {
        XIAOTAI_LOGI(SOURCE, "ended gen=%" PRIu32 ": %s", session, text);
    }
}

static void finish(xiaotai_call_end_t end, xiaotai_err_t error, bool delay, const char *reason, bool release)
{
    const uint32_t session = s_call.status.session;
    if (!session) return;
    const xiaotai_call_state_t phase = s_call.status.state;
    cancel_timer();
    const bool retire = call_signaling_end(session);
    const bool muted = setting_muted();
    const xiaotai_err_t mute_err = xiaotai_media_set_mic_muted(muted);
    if (mute_err != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "mute.restore", session, mute_err);
    s_call.status.muted = muted;
    if (s_call.activity) xiaotai_activity_set_generation(s_call.activity, 0);
    if (s_call.media) xiaotai_media_close(s_call.status.link);
    s_call.media = false;
    if (s_call.opened) {
        const xiaotai_err_t sent = xiaotai_link_command(s_call.status.link, CALL_COMMAND_END, NULL, 0);
        if (sent != XIAOTAI_OK) {
            xiaotai_eventlog_warn(SOURCE, "hangup.send", session, sent);
            if (end == XIAOTAI_CALL_END_LOCAL && error == XIAOTAI_OK) error = sent;
        }
    }
    disarm();

    call_release_entry_t *slot = call_release_find(session);
    /* The peer's own answer; a refused dial has no slot here any more. */
    if (slot && (end == XIAOTAI_CALL_END_PEER_REJECT || end == XIAOTAI_CALL_END_BUSY)) slot->declined = true;
    if (!release || s_call.repair.step != CALL_REPAIR_NONE || (retire && end == XIAOTAI_CALL_END_PEER_HANGUP)) {
        if (slot) *slot = (call_release_entry_t){0};
    } else if (s_call.config.strict_failed_accept_room_id && phase == XIAOTAI_CALL_CONNECTING &&
               !s_call.status.outgoing && reason && !strcmp(reason, "p2p_error") &&
               !call_http_room_id_valid(s_call.room)) {
        xiaotai_eventlog_warn(SOURCE, "accept.release.room", session, XIAOTAI_ERR_INVALID_ARG);
    } else {
        call_release_end(session, s_call.binding, phase, s_call.room, reason,
                         (s_call.status.flags & XIAOTAI_CALL_VIDEO) != 0, retire);
    }
    s_call.room[0] = s_call.early_answer_room[0] = '\0';
    s_call.repair.step = CALL_REPAIR_NONE;
    const int64_t talked_us = s_call.status.active_since_us ? xiaotai_time_us() - s_call.status.active_since_us : 0;
    s_call.status.state = XIAOTAI_CALL_IDLE;
    s_call.status.session = 0;
    s_call.status.active_since_us = 0;
    s_call.status.end = end;
    s_call.status.error = error;
    s_call.closing = s_call.link_dead ? 0 : s_call.status.link;
    s_call.status.link = 0;
    s_call.status.releasing = delay && s_call.opened;
    if (s_call.status.releasing &&
        xiaotai_timer_start(XIAOTAI_CALL_RELEASE_DELAY_MS, close_after_delay,
                            (void *)(uintptr_t)s_call.closing, &s_call.timer) != XIAOTAI_OK) {
        xiaotai_eventlog_warn(SOURCE, "release.timer", session, XIAOTAI_ERR_NO_MEM);
        s_call.status.releasing = false;
    }
    call_release_counts();
    xiaotai_eventlog_record(SOURCE, "ended", session, end);
    log_ended(session, (unsigned)end, error, talked_us);
    emit(XIAOTAI_EVENT_CALL_ENDED, end, NULL);
    state_changed();
    if (!s_call.status.releasing) close_link();
    if (retire) call_signaling_drain(session);
}

void call_finish(xiaotai_call_end_t end, xiaotai_err_t error, bool delay, const char *reason)
{
    finish(error == XIAOTAI_ERR_NO_MEM ? XIAOTAI_CALL_END_NO_MEMORY : end, error, delay, reason, true);
}

static void deadline_passed(void *context)
{
    if (!s_call.status.session || s_call.status.session != (uint32_t)(uintptr_t)context) return;
    s_call.timer = 0;
    xiaotai_call_end_t end = XIAOTAI_CALL_END_NETWORK;
    if (s_call.status.state == XIAOTAI_CALL_INCOMING) end = XIAOTAI_CALL_END_MISSED;
    if (s_call.status.state == XIAOTAI_CALL_OUTGOING) end = XIAOTAI_CALL_END_NO_ANSWER;
    call_finish(end, XIAOTAI_ERR_TIMEOUT, true, s_call.status.state == XIAOTAI_CALL_CONNECTING ? "p2p_error" : NULL);
}

static xiaotai_err_t set_deadline(uint32_t timeout)
{
    cancel_timer();
    const xiaotai_err_t err = xiaotai_timer_start(timeout, deadline_passed,
                                      (void *)(uintptr_t)s_call.status.session, &s_call.timer);
    if (err == XIAOTAI_OK) s_call.deadline_us = xiaotai_time_us() + (int64_t)timeout * 1000;
    return err;
}

static void revoked(uint32_t id, xiaotai_revoke_t reason, void *context)
{
    (void)context;
    if (id != s_call.activity) return;
    s_call.activity = 0;  // The framework already ended the attachments.
    if (s_call.status.releasing) {
        cancel_timer();
        close_link();
        state_changed();
    } else {
        call_finish(reason == XIAOTAI_REVOKE_PREEMPTED ? XIAOTAI_CALL_END_PREEMPTED : XIAOTAI_CALL_END_SYSTEM,
                    XIAOTAI_OK, false, NULL);
    }
}

static xiaotai_err_t begin(xiaotai_call_state_t phase, const char *peer, const char *name, uint32_t flags,
                           uint32_t expected_binding)
{
    if (s_call.status.state != XIAOTAI_CALL_IDLE || s_call.status.releasing) return XIAOTAI_ERR_BUSY;
    static const xiaotai_activity_t activity = {
        .name = XIAOTAI_CALL_ACTIVITY, .revoked = revoked,
        .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT |
                     XIAOTAI_RESOURCE_LINK | XIAOTAI_RESOURCE_SCREEN,
    };
    const xiaotai_err_t err = xiaotai_activity_begin(&activity, &s_call.activity);
    if (err != XIAOTAI_OK) return err;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (expected_binding && platform.generation != expected_binding) {
        xiaotai_activity_end(s_call.activity);
        s_call.activity = 0;
        return XIAOTAI_ERR_PLATFORM_STALE;
    }
    release_binding(platform.generation);
    s_call.binding = platform.generation;
    s_call.status.state = phase;
    s_call.status.session = xiaotai_generation_next();
    s_call.status.outgoing = phase == XIAOTAI_CALL_OUTGOING;
    s_call.status.flags = flags;
    s_call.status.end = XIAOTAI_CALL_END_NONE;
    s_call.status.error = XIAOTAI_OK;
    s_call.status.muted = setting_muted();
    s_call.answered = false;
    s_call.repair.attempted = false;
    s_call.repair.step = CALL_REPAIR_NONE;
    s_call.repair.room[0] = '\0';
    copy_text(s_call.status.peer_id, sizeof(s_call.status.peer_id), peer);
    copy_text(s_call.status.peer_name, sizeof(s_call.status.peer_name), name);
    atomic_fetch_add(&s_dial_revision, 1U);
    return XIAOTAI_OK;
}

bool call_parse(const char *text, size_t size, xiaotai_json_view_t *root)
{
    size_t count;
    *root = (xiaotai_json_view_t){0};
    return text && s_call.initialized && xiaotai_json_view_parse(text, size, &count, root) == XIAOTAI_OK &&
           count <= CALL_JSON_TOKENS && root->type == XIAOTAI_JSON_OBJECT;
}

bool call_binding_current(uint32_t generation)
{
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.generation == generation) return true;
    ++s_call.status.stale_calls;
    return false;
}

bool call_foreground_owns_room(uint32_t except_session, const char *room)
{
    return s_call.status.session && s_call.status.session != except_session && room[0] &&
           (!strcmp(s_call.room, room) || (s_call.repair.step != CALL_REPAIR_NONE &&
                                          !strcmp(s_call.repair.room, room)));
}

static void media_event(uint32_t generation, xiaotai_media_event_t event, xiaotai_err_t error, void *context)
{
    (void)context;
    if (event == XIAOTAI_MEDIA_EVENT_FAILED && s_call.media && generation == s_call.status.link) {
        call_finish(XIAOTAI_CALL_END_AUDIO, error, false, s_call.status.outgoing ? NULL : "p2p_error");
    }
}

static void activate(void)
{
    if (!s_call.opened || (s_call.status.outgoing &&
        (!s_call.answered || !s_call.room[0] || s_call.status.state != XIAOTAI_CALL_OUTGOING))) return;
    const uint32_t link = s_call.status.link;
    const xiaotai_media_session_t session = {
        .generation = link, .uplink = audio, .downlink = audio, .downlink_burst_ms = BURST_MS,
        .duplex = true, .sink = xiaotai_link_audio_sink(), .latency = s_call.config.latency,
        .flags = s_call.config.uplink_noise_gate ? XIAOTAI_MEDIA_GATE_IDLE_NOISE : 0, .event = media_event,
    };
    xiaotai_err_t err = xiaotai_media_open(&session);
    const char *failure = s_call.status.outgoing ? NULL : "p2p_error";
    if (err != XIAOTAI_OK) {
        call_finish(XIAOTAI_CALL_END_AUDIO, err, false, failure);
        return;
    }
    s_call.media = true;
    err = xiaotai_link_receive(link, XIAOTAI_LINK_AUDIO, NULL, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_link_permit(link, XIAOTAI_LINK_AUDIO, true);
    if (err != XIAOTAI_OK) {
        call_finish(XIAOTAI_CALL_END_NETWORK, err, false, failure);
        return;
    }
    cancel_timer();
    s_call.status.state = XIAOTAI_CALL_ACTIVE;
    s_call.status.active_since_us = xiaotai_time_us();
    ++s_call.status.answered;
    xiaotai_activity_set_generation(s_call.activity, link);
    if (s_call.config.pause_platform_signaling_while_active) call_signaling_start(s_call.status.session, link);
    xiaotai_eventlog_note(SOURCE, "connected", s_call.status.session, (int32_t)link);
    emit(XIAOTAI_EVENT_CALL_CONNECTED, 0, NULL);
    state_changed();
}

static void opened(void *context, uint32_t generation)
{
    const bool inbound = context != NULL;
    if (inbound) {
        const bool video = (uintptr_t)context == 2U;
        if (s_call.hold_listener) {
            /* A candidate admitted just before the block may already be
             * queued for the owner. New inbound never reaches this callback. */
            if (s_call.armed == s_call.listener[video]) s_call.armed = 0;
            ++s_call.status.stale_calls;
            xiaotai_eventlog_debug(SOURCE, "release.late_link", generation, 0);
            xiaotai_link_disconnect(generation);
            if (s_call.status.state == XIAOTAI_CALL_OUTGOING && !s_call.status.link) {
                const xiaotai_err_t err = arm_outgoing();
                if (err != XIAOTAI_OK) call_finish(XIAOTAI_CALL_END_FAILED, err, false, NULL);
            }
            return;
        }
        if (s_call.status.state != XIAOTAI_CALL_OUTGOING || s_call.status.link ||
            video != ((s_call.status.flags & XIAOTAI_CALL_VIDEO) != 0)) {
            ++s_call.status.stale_calls;
            xiaotai_link_disconnect(generation);
            return;
        }
        // ONCE has already disarmed itself.
        s_call.armed = 0;
        if (s_call.repair.step != CALL_REPAIR_NONE) {
            xiaotai_link_disconnect(generation);
            const xiaotai_err_t err = arm_outgoing();
            if (err != XIAOTAI_OK) call_finish(XIAOTAI_CALL_END_FAILED, err, false, NULL);
            return;
        }
        s_call.status.link = generation;
    } else if (s_call.status.state != XIAOTAI_CALL_CONNECTING || s_call.status.link != generation) {
        ++s_call.status.stale_calls;
        xiaotai_link_disconnect(generation);
        return;
    }
    s_call.opened = true;
    if (inbound) {
        if (!s_call.answered) {
            const xiaotai_err_t err = set_deadline(EARLY_LINK_MS);
            if (err != XIAOTAI_OK) {
                call_finish(XIAOTAI_CALL_END_NETWORK, err, false, NULL);
                return;
            }
        }
    } else {
        const xiaotai_err_t err = call_send_answer(generation, s_call.room);
        if (err != XIAOTAI_OK) {
            call_finish(XIAOTAI_CALL_END_NETWORK, err, false, "p2p_error");
            return;
        }
    }
    activate();
    if (s_call.opened && s_call.status.state != XIAOTAI_CALL_ACTIVE) state_changed();
}

static void closed(void *context, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)context;
    if (s_call.status.releasing && s_call.closing == generation) {
        cancel_timer();
        s_call.closing = 0;
        close_link();
        state_changed();
        return;
    }
    if (s_call.status.link != generation || !generation) {
        ++s_call.status.stale_calls;
        return;
    }
    s_call.opened = false;
    // Keep the generation through media teardown, but do not disconnect a dead link.
    s_call.link_dead = true;
    if (detail) xiaotai_eventlog_warn(SOURCE, "link.detail", s_call.status.session, detail);
    const bool peer = reason == XIAOTAI_LINK_END_PEER;
    const xiaotai_err_t error = peer ? XIAOTAI_OK : reason == XIAOTAI_LINK_END_NO_MEMORY ? XIAOTAI_ERR_NO_MEM :
                               reason == XIAOTAI_LINK_END_TIMEOUT ? XIAOTAI_ERR_TIMEOUT :
                               reason == XIAOTAI_LINK_END_PEER_FAULT ? detail : XIAOTAI_FAIL;
    finish(peer ? XIAOTAI_CALL_END_PEER_HANGUP : XIAOTAI_CALL_END_NETWORK, error, false,
           s_call.status.state == XIAOTAI_CALL_CONNECTING ? "p2p_error" : NULL, true);
}

static bool current(uint32_t session);

static bool message_word(uint32_t word)
{
    return word >= XIAOTAI_CALL_MESSAGE_WORD(0) && word < XIAOTAI_CALL_MESSAGE_WORD(XIAOTAI_CALL_MESSAGE_TYPES);
}

static void deliver_message(const xiaotai_link_command_t *message)
{
    const uint8_t type = (uint8_t)(message->word - XIAOTAI_CALL_MESSAGE_WORD(0));
    if (message->dropped || message->size > XIAOTAI_CALL_MESSAGE_MAX || s_call.status.state != XIAOTAI_CALL_ACTIVE) {
        xiaotai_eventlog_warn(SOURCE, "message.drop", s_call.status.session, type);
        return;
    }
    xiaotai_call_message_t data = {.session = s_call.status.session, .type = type, .size = (uint8_t)message->size};
    if (message->size) memcpy(data.data, message->data, message->size);
    const xiaotai_args_t args = {.value = (int32_t)s_call.status.session};
    const xiaotai_err_t err = xiaotai_event_emit(XIAOTAI_EVENT_CALL_MESSAGE, &args, &data, sizeof(data));
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "message.queue", s_call.status.session, err);
}

xiaotai_err_t xiaotai_call_send_message(uint32_t session, uint8_t type, const void *data, size_t size)
{
    if (type >= XIAOTAI_CALL_MESSAGE_TYPES || size > XIAOTAI_CALL_MESSAGE_MAX || (size && !data))
        return XIAOTAI_ERR_INVALID_ARG;
    if (!current(session) || s_call.status.state != XIAOTAI_CALL_ACTIVE || !s_call.opened)
        return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = xiaotai_link_command(s_call.status.link, XIAOTAI_CALL_MESSAGE_WORD(type), data, size);
    if (err != XIAOTAI_OK) xiaotai_eventlog_warn(SOURCE, "message.send", session, err);
    return err;
}

static void command(void *context, uint32_t generation, const xiaotai_link_command_t *message)
{
    (void)context;
    if (!message) return;
    if (!s_call.opened || !generation || generation != s_call.status.link) {
        ++s_call.status.stale_calls;
        return;
    }
    if (message_word(message->word)) {
        deliver_message(message);
        return;
    }
    if (message->dropped ||(message->word != CALL_COMMAND_END && message->word != CALL_COMMAND_ANSWER)) return;
    xiaotai_json_view_t doc;
    if (message->size && !call_parse((const char *)message->data, message->size, &doc)) return;
    if (message->word == CALL_COMMAND_END) {
        call_finish(XIAOTAI_CALL_END_PEER_HANGUP, XIAOTAI_OK, false, NULL);
        return;
    }
    if (s_call.status.state != XIAOTAI_CALL_OUTGOING) return;
    char room[CALL_ROOM_SIZE] = "";
    xiaotai_json_view_t item;
    if (message->size && xiaotai_json_view_get(&doc, "room_id", &item) && !call_string(&item, room, sizeof(room)))
        return;
    if (room[0] && s_call.room[0] && strcmp(room, s_call.room)) {
        ++s_call.status.stale_calls;
        return;
    }
    if (!s_call.room[0]) copy_text(s_call.early_answer_room, sizeof(s_call.early_answer_room), room);
    s_call.answered = true;
    activate();
}

static xiaotai_link_handlers_t handlers(uintptr_t listener)
{
    return (xiaotai_link_handlers_t){.opened = opened, .closed = closed, .command = command, .ctx = (void *)listener};
}

static xiaotai_link_profile_t profile(bool video)
{
    xiaotai_link_profile_t result = {
        .audio = link_audio, .video = video ? s_call.video : (xiaotai_link_media_t){0},
        .command_max_bytes = CALL_COMMAND_BYTES, .command_pool_bytes = CALL_COMMAND_SHARE,
        .track_peer_retirement = s_call.config.pause_platform_signaling_while_active,
    };
    result.audio.accept_peer_stream = s_call.config.accept_peer_audio_stream;
    return result;
}

static bool call_online(void)
{
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    return platform.phase == XIAOTAI_PLATFORM_ONLINE && xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE;
}

static void submit_repair(uint32_t session, call_repair_step_t step, xiaotai_call_state_t phase)
{
    s_call.repair.step = step;
    const xiaotai_err_t err = call_submit_repair(session, step, s_call.repair.room, phase);
    if (err != XIAOTAI_OK) call_finish(XIAOTAI_CALL_END_FAILED, err, false, NULL);
}

static void discard_early_outgoing(void)
{
    const uint32_t early = s_call.status.link;
    s_call.status.link = 0;
    s_call.answered = s_call.opened = s_call.link_dead = false;
    s_call.early_answer_room[0] = '\0';
    if (early) xiaotai_link_disconnect(early);
}

static void start_repair(uint32_t session)
{
    s_call.repair.attempted = true;
    s_call.repair.step = CALL_REPAIR_QUERY;
    /* The conflict proves this attempt did not create a room. Any early link
     * or answer can only refer to the old room and must not survive a redial. */
    discard_early_outgoing();
    xiaotai_err_t err = arm_outgoing();
    if (err == XIAOTAI_OK) err = set_deadline(REPAIR_MS);
    if (err == XIAOTAI_OK) submit_repair(session, CALL_REPAIR_QUERY, XIAOTAI_CALL_IDLE);
    else call_finish(XIAOTAI_CALL_END_FAILED, err, false, NULL);
}

void call_repair_reply(uint32_t session, call_repair_step_t step, const xiaotai_platform_reply_t *reply)
{
    if (!call_binding_current(reply->generation)) return;
    call_release_entry_t *slot = call_release_find(session);
    if (!slot || slot->binding != reply->generation || session != s_call.status.session ||
        s_call.status.state != XIAOTAI_CALL_OUTGOING || s_call.repair.step != step) {
        ++s_call.status.stale_calls;
        return;
    }
    if (xiaotai_time_us() >= s_call.deadline_us) {
        call_finish(XIAOTAI_CALL_END_NO_ANSWER, XIAOTAI_ERR_TIMEOUT, false, NULL);
        return;
    }
    if (reply->result == XIAOTAI_ERR_PLATFORM_STALE) {
        call_finish(XIAOTAI_CALL_END_NETWORK, reply->result, false, NULL);
        return;
    }
    if (step == CALL_REPAIR_RELEASE) {
        /* Even a lost POST response still needs an authoritative GET. */
        submit_repair(session, CALL_REPAIR_VERIFY, XIAOTAI_CALL_IDLE);
        return;
    }
    xiaotai_call_state_t phase;
    xiaotai_err_t err = call_http_repair_action(reply, step == CALL_REPAIR_VERIFY, s_call.repair.room, &phase);
    if (err != XIAOTAI_OK) {
        call_finish(err == XIAOTAI_ERR_BUSY ? XIAOTAI_CALL_END_BUSY : XIAOTAI_CALL_END_FAILED, err, false, NULL);
        return;
    }
    if (phase != XIAOTAI_CALL_IDLE) {
        submit_repair(session, CALL_REPAIR_RELEASE, phase);
        return;
    }
    s_call.repair.step = CALL_REPAIR_NONE;
    slot->redial = true;
    slot->awaiting_dial = true;
    err = arm_outgoing();
    if (err == XIAOTAI_OK) err = set_deadline(s_call.config.answer_timeout_ms);
    if (err == XIAOTAI_OK) {
        err = call_submit_dial(session, s_call.status.peer_id, (s_call.status.flags & XIAOTAI_CALL_VIDEO) != 0, true, 0);
    }
    if (err != XIAOTAI_OK) finish(XIAOTAI_CALL_END_FAILED, err, false, NULL, false);
}

void call_dial_reply(uint32_t session, bool redial, const xiaotai_platform_reply_t *reply)
{
    if (!call_binding_current(reply->generation)) return;
    call_release_entry_t *slot = call_release_find(session);
    if (!slot || !slot->awaiting_dial || slot->redial != redial || slot->binding != reply->generation) {
        ++s_call.status.stale_calls;
        return;
    }
    slot->awaiting_dial = false;
    call_http_payload_t payload;
    const int32_t code = call_http_code(reply, &payload);
    const bool definitive = reply->http_status == 401 || code == 40000 || code == 40201 ||
                            code == 40202 || code == 40205;
    const bool got_room = code == XIAOTAI_PLATFORM_CODE_OK && call_http_room(&payload, slot->room);
    if (session != s_call.status.session || s_call.status.state != XIAOTAI_CALL_OUTGOING) {
        ++s_call.status.stale_calls;
        call_release_retired_dial(slot, definitive, got_room && call_http_room_id_valid(slot->room));
        return;
    }
    if (!got_room) {
        const xiaotai_err_t error = reply->result == XIAOTAI_OK ? CALL_INVALID_RESPONSE : reply->result;
        if (code == 40202 && !s_call.repair.attempted && call_http_room(&payload, s_call.repair.room) &&
            call_http_room_id_valid(s_call.repair.room)) {
            start_repair(session);
            return;
        }
        // Definitive refusals created no room. An uncertain failure remains owned until reconciliation.
        if (definitive) {
            *slot = (call_release_entry_t){0};
            discard_early_outgoing();
        }
        call_finish(code == 40202 ? XIAOTAI_CALL_END_BUSY : XIAOTAI_CALL_END_FAILED, error, false, NULL);
        return;
    }
    copy_text(s_call.room, sizeof(s_call.room), slot->room);
    if (s_call.early_answer_room[0] && strcmp(s_call.early_answer_room, s_call.room)) {
        s_call.answered = false;
        ++s_call.status.stale_calls;
    }
    s_call.early_answer_room[0] = '\0';
    xiaotai_err_t err = !s_call.opened ? arm_outgoing() : XIAOTAI_OK;
    if (err == XIAOTAI_OK) err = set_deadline(s_call.config.answer_timeout_ms);
    if (err != XIAOTAI_OK) call_finish(XIAOTAI_CALL_END_FAILED, err, false, NULL);
    else activate();
}

void call_accept_reply(uint32_t session, const xiaotai_platform_reply_t *reply)
{
    if (!call_binding_current(reply->generation)) return;
    if (session != s_call.status.session || s_call.status.state != XIAOTAI_CALL_CONNECTING || s_call.status.link) {
        ++s_call.status.stale_calls;
        return;
    }
    bool denied;
    char *token;
    xiaotai_err_t err = call_http_accept_token(reply, s_call.status.peer_id, &denied, &token);
    if (err != XIAOTAI_OK) {
        finish(err == XIAOTAI_ERR_NO_MEM ? XIAOTAI_CALL_END_NO_MEMORY : XIAOTAI_CALL_END_FAILED,
               err, false, "p2p_error", !denied);
        return;
    }
    const int64_t left = s_call.deadline_us - xiaotai_time_us();
    if (left <= 0) err = XIAOTAI_ERR_TIMEOUT;
    else {
        const xiaotai_link_request_t request = {
            .kind = XIAOTAI_LINK_P2P, .peer = s_call.status.peer_id, .token = token,
            .timeout_ms = (uint32_t)((left + 999) / 1000),
            .profile = profile((s_call.status.flags & XIAOTAI_CALL_VIDEO) != 0),
        };
        const xiaotai_link_handlers_t link = handlers(0);
        err = xiaotai_link_connect(&request, &link, &s_call.status.link);
    }
    call_http_free_token(token);
    if (err != XIAOTAI_OK) call_finish(XIAOTAI_CALL_END_NETWORK, err, false, "p2p_error");
    else state_changed();
}

static uint32_t contact_flags(void)
{
    return s_call.config.require_fresh_contacts ? 0 : XIAOTAI_CONTACTS_STALE_OK;
}

static xiaotai_err_t dial_contact(const xiaotai_contact_t *contact, uint32_t flags, uint32_t *session,
                                  uint32_t expected_binding)
{
    if (!call_online()) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    if (s_call.status.state != XIAOTAI_CALL_IDLE || s_call.status.releasing) return XIAOTAI_ERR_BUSY;
    const bool video = (flags & XIAOTAI_CALL_VIDEO) != 0;
    const uint32_t listener = s_call.listener[video ? 1 : 0];
    if (!listener) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (expected_binding && platform.generation != expected_binding) return XIAOTAI_ERR_PLATFORM_STALE;
    release_binding(platform.generation);
    call_release_entry_t *slot = call_release_slot();
    if (!slot) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = begin(XIAOTAI_CALL_OUTGOING, contact->id, contact->name, flags, expected_binding);
    if (err != XIAOTAI_OK) return err;
    const uint32_t generation = s_call.status.session;
    *slot = (call_release_entry_t){.session = generation, .binding = s_call.binding, .phase = XIAOTAI_CALL_OUTGOING,
                                   .awaiting_dial = true, .video = video};
    err = arm_outgoing();
    if (err == XIAOTAI_OK) err = set_deadline(s_call.config.answer_timeout_ms);
    if (err == XIAOTAI_OK) err = call_submit_dial(generation, contact->id, video, false, expected_binding);
    if (err != XIAOTAI_OK) {
        finish(err == XIAOTAI_ERR_NO_MEM ? XIAOTAI_CALL_END_NO_MEMORY : XIAOTAI_CALL_END_FAILED,
               err, false, NULL, false);
        return err;
    }
    if (session) *session = generation;
    ++s_call.status.dialled;
    xiaotai_eventlog_note(SOURCE, "outgoing", generation, (int32_t)flags);
    state_changed();
    return XIAOTAI_OK;
}

static xiaotai_err_t find_dial_contact(const char *device_id, bool local, xiaotai_contact_t *contact)
{
    xiaotai_err_t err = XIAOTAI_ERR_NOT_SUPPORTED;
    if (local)
        err = xiaotai_contacts_find_unique_online_prefix(device_id, s_call.contact_kind, contact_flags(), contact);
    if (err == XIAOTAI_ERR_NOT_SUPPORTED)
        err = xiaotai_contacts_find(device_id, s_call.contact_kind, contact_flags(), contact);
    return err;
}

static xiaotai_err_t dial_by_id(const char *device_id, uint32_t flags, uint32_t *session, bool local_at)
{
    if (session) *session = 0;
    if (!device_id || !device_id[0] || strlen(device_id) >= XIAOTAI_CALL_PEER_ID_MAX ||
        (flags & ~XIAOTAI_CALL_VIDEO)) return XIAOTAI_ERR_INVALID_ARG;
    if (!s_call.initialized) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_contact_t contact;
    const xiaotai_err_t err = find_dial_contact(device_id, local_at, &contact);
    return err == XIAOTAI_OK ? dial_contact(&contact, flags, session, 0) : err;
}

xiaotai_err_t xiaotai_call_dial(const char *device_id, uint32_t flags, uint32_t *session)
{
    return dial_by_id(device_id, flags, session, false);
}

xiaotai_err_t call_dial_device_at(const char *device_id)
{
    return dial_by_id(device_id, 0, NULL, true);
}

xiaotai_err_t xiaotai_call_dial_contact(size_t index, uint32_t flags, uint32_t *session)
{
    if (session) *session = 0;
    if (flags & ~XIAOTAI_CALL_VIDEO) return XIAOTAI_ERR_INVALID_ARG;
    if (!s_call.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (index > UINT8_MAX) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_contact_t contact;
    const xiaotai_err_t err = xiaotai_contacts_at((uint8_t)index, contact_flags(), &contact);
    if (err != XIAOTAI_OK) return err;
    return contact.kind == s_call.contact_kind ? dial_contact(&contact, flags, session, 0) : XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t call_index_owner(uint8_t index, bool *owned)
{
    xiaotai_contact_t contact;
    /* A stale page still identifies the route, but dial_contact applies the
     * selected product's freshness rule before starting the call. */
    const xiaotai_err_t err = xiaotai_contacts_at(index, XIAOTAI_CONTACTS_STALE_OK, &contact);
    if (err == XIAOTAI_OK) *owned = contact.kind == s_call.contact_kind;
    return err;
}

static bool current(uint32_t session)
{
    if (session && session != s_call.status.session) {
        ++s_call.status.stale_calls;
        return false;
    }
    return s_call.status.session != 0;
}

xiaotai_err_t xiaotai_call_accept(uint32_t session)
{
    if (!current(session) || s_call.status.state != XIAOTAI_CALL_INCOMING) return XIAOTAI_ERR_INVALID_STATE;
    s_call.status.state = XIAOTAI_CALL_CONNECTING;
    state_changed();
    xiaotai_err_t err = set_deadline(s_call.config.connect_timeout_ms);
    if (err == XIAOTAI_OK) err = call_submit_accept(s_call.status.session, s_call.room, s_call.status.peer_id);
    if (err != XIAOTAI_OK) call_finish(XIAOTAI_CALL_END_FAILED, err, false, "p2p_error");
    return err;
}

xiaotai_err_t xiaotai_call_reject(uint32_t session)
{
    if (!current(session) || s_call.status.state != XIAOTAI_CALL_INCOMING) return XIAOTAI_ERR_INVALID_STATE;
    call_finish(XIAOTAI_CALL_END_LOCAL, XIAOTAI_OK, true, NULL);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_call_hangup(uint32_t session)
{
    if (!current(session)) return XIAOTAI_ERR_INVALID_STATE;
    call_finish(XIAOTAI_CALL_END_LOCAL, XIAOTAI_OK, true, NULL);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_call_set_muted(uint32_t session, bool muted)
{
    if (!current(session)) return XIAOTAI_ERR_INVALID_STATE;
    muted = muted || setting_muted();
    const xiaotai_err_t err = xiaotai_media_set_mic_muted(muted);
    if (err == XIAOTAI_OK && s_call.status.muted != muted) {
        s_call.status.muted = muted;
        state_changed();
    }
    return err;
}

xiaotai_err_t xiaotai_call_set_saved_mute(uint32_t session, bool muted)
{
    if (!xiaotai_on_owner() || !session || !current(session) || s_call.status.state != XIAOTAI_CALL_ACTIVE)
        return XIAOTAI_ERR_INVALID_STATE;
#ifdef XIAOTAI_CALL_HAS_SETTINGS
    if (s_call.settings) {
        const xiaotai_err_t err = xiaotai_settings_set_mic_mute(muted);
        if (err == XIAOTAI_OK && s_call.status.muted != muted) {
            s_call.status.muted = muted;
            state_changed();
        }
        return err;
    }
#endif
    (void)muted;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static bool incoming_busy(void)
{
    const char *name = NULL;
    if (!s_call.config.incoming_busy_during || !xiaotai_activity_current(&name) || !name) return false;
    for (size_t i = 0; s_call.config.incoming_busy_during[i]; ++i) {
        if (!strcmp(name, s_call.config.incoming_busy_during[i])) return true;
    }
    return false;
}

void call_incoming(const char *room, const char *peer, const char *name, bool video, bool unsupported)
{
    if (s_call.status.session && s_call.binding && !call_binding_current(s_call.binding)) return;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    release_binding(platform.generation);
    if (s_call.status.session && s_call.room[0] && !strcmp(s_call.room, room)) return;
    if (call_release_finished_room(room)) return;
    if (unsupported) {
        ++s_call.status.refused_media;
        call_release_end(0, platform.generation, XIAOTAI_CALL_INCOMING, room, "unsupported_media", video, false);
        return;
    }
    /* The caller by the name this device's owner gave it, else by the name the
     * server sent, else by its id. */
    xiaotai_contact_t contact;
    const bool named = xiaotai_contacts_find(peer, s_call.contact_kind, XIAOTAI_CONTACTS_STALE_OK, &contact) == XIAOTAI_OK &&
                       contact.name[0];
    /* Nothing here to show or send a picture with: the caller is still heard. */
    const uint32_t flags = !video ? 0 : s_call.listener[1] ? XIAOTAI_CALL_VIDEO : XIAOTAI_CALL_VIDEO_OFFERED;
    if (incoming_busy() || begin(XIAOTAI_CALL_INCOMING, peer, named ? contact.name : name[0] ? name : peer,
                                 flags, 0) != XIAOTAI_OK) {
        ++s_call.status.refused_busy;
        call_release_end(0, platform.generation, XIAOTAI_CALL_INCOMING, room, "busy", video, false);
        return;
    }
    copy_text(s_call.room, sizeof(s_call.room), room);
    const xiaotai_err_t err = set_deadline(s_call.config.ring_timeout_ms);
    if (err != XIAOTAI_OK) {
        call_finish(XIAOTAI_CALL_END_FAILED, err, false, NULL);
        return;
    }
    ++s_call.status.announced;
    xiaotai_eventlog_note(SOURCE, "incoming", s_call.status.session, (int32_t)s_call.status.flags);
    emit(XIAOTAI_EVENT_CALL_INCOMING, 0, peer);
    state_changed();
}

static bool matches_room(const char *room)
{
    if (s_call.status.session && s_call.room[0] && !strcmp(s_call.room, room)) {
        return call_binding_current(s_call.binding);
    }
    ++s_call.status.stale_calls;
    return false;
}

void call_cloud_answer(const char *room, const char *callee)
{
    if (!matches_room(room) || s_call.status.state != XIAOTAI_CALL_OUTGOING) return;
    s_call.answered = true;
    if (callee[0]) copy_text(s_call.status.peer_id, sizeof(s_call.status.peer_id), callee);
    const xiaotai_err_t err = set_deadline(s_call.config.answer_timeout_ms);
    if (err != XIAOTAI_OK) call_finish(XIAOTAI_CALL_END_FAILED, err, false, NULL);
    else {
        activate();
        if (!s_call.opened) state_changed();
    }
}

void call_cloud_end(const char *room, bool rejected, const char *reason)
{
    if (!matches_room(room)) return;
    const bool busy = rejected && !strcmp(reason, "busy");
    const xiaotai_call_end_t end = busy ? XIAOTAI_CALL_END_BUSY : rejected ? XIAOTAI_CALL_END_PEER_REJECT :
                                  s_call.status.state == XIAOTAI_CALL_INCOMING ?
                                      XIAOTAI_CALL_END_PEER_CANCEL : XIAOTAI_CALL_END_PEER_HANGUP;
    const xiaotai_err_t error = rejected && !strcmp(reason, "unsupported_media") ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_OK;
    /* The caller has already cancelled the ringing room. Do not send a decline. */
    finish(end, error, false, NULL, end != XIAOTAI_CALL_END_PEER_CANCEL);
}

static void offline(const xiaotai_event_t *event, void *context)
{
    (void)context;
    if (!s_call.initialized) return;
    if (strcmp(event->name, XIAOTAI_EVENT_PLATFORM_OFFLINE) ||
        event->args.value == XIAOTAI_PLATFORM_OFFLINE_BINDING) {
        release_binding(0);
        finish(XIAOTAI_CALL_END_SYSTEM, XIAOTAI_OK, false, NULL, false);
        if (s_call.status.releasing) {
            cancel_timer();
            close_link();
            state_changed();
        }
    } else if (s_call.status.state != XIAOTAI_CALL_ACTIVE || s_call.config.end_on_platform_offline) {
        call_finish(XIAOTAI_CALL_END_NETWORK, XIAOTAI_ERR_PLATFORM_OFFLINE, false, NULL);
    }
}

static void online(const xiaotai_event_t *event, void *context)
{
    (void)event;
    (void)context;
    if (!s_call.initialized) return;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.phase != XIAOTAI_PLATFORM_ONLINE) return;
    if (s_call.status.session && s_call.binding != platform.generation) {
        finish(XIAOTAI_CALL_END_SYSTEM, XIAOTAI_OK, false, NULL, false);
    }
    release_binding(platform.generation);
    call_release_online(platform.generation);
}

static void activity_changed(const xiaotai_event_t *event, void *context)
{
    (void)context;
    if (s_call.initialized && event->args.value == 0) call_release_activity();
}

#ifdef XIAOTAI_CALL_HAS_SETTINGS
static void settings_changed(const xiaotai_event_t *event, void *context)
{
    (void)context;
    const bool muted = event->args.value != 0;
    if (s_call.status.session && !strcmp(event->args.text, XIAOTAI_SETTING_AUDIO_MIC_MUTED) &&
        s_call.status.muted != muted) {
        s_call.status.muted = muted;
        state_changed();
    }
}
#endif

static xiaotai_err_t prepare_dial_intent(const xiaotai_args_t *args, xiaotai_intent_stamp_t *stamp, void *context)
{
    (void)context;
    if (!args->text[0] || !memchr(args->text, '\0', sizeof(args->text)) ||
        strlen(args->text) >= XIAOTAI_CALL_PEER_ID_MAX || ((uint32_t)args->value & ~XIAOTAI_CALL_VIDEO))
        return XIAOTAI_ERR_INVALID_ARG;
    const uint32_t revision = atomic_load(&s_dial_revision);
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (!platform.generation || platform.phase != XIAOTAI_PLATFORM_ONLINE ||
        xiaotai_transport_state() != XIAOTAI_TRANSPORT_ONLINE) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    const uint32_t binding = platform.generation;
    xiaotai_contact_t contact;
    const xiaotai_err_t err = find_dial_contact(args->text, true, &contact);
    if (err != XIAOTAI_OK) return err;
    xiaotai_platform_status(&platform);
    if (platform.generation != binding) return XIAOTAI_ERR_PLATFORM_STALE;
    if (atomic_load(&s_dial_revision) != revision) return XIAOTAI_ERR_INVALID_STATE;
    *stamp = (xiaotai_intent_stamp_t){.scope = binding, .revision = revision};
    return XIAOTAI_OK;
}

static void dispatch_dial_intent(const xiaotai_args_t *args, const xiaotai_intent_stamp_t *stamp, void *context)
{
    (void)context;
    if (atomic_load(&s_dial_revision) != stamp->revision || !call_binding_current(stamp->scope)) return;
    xiaotai_contact_t contact;
    if (find_dial_contact(args->text, true, &contact) == XIAOTAI_OK)
        (void)dial_contact(&contact, (uint32_t)args->value, NULL, stamp->scope);
}

typedef enum {ACCEPT, REJECT, HANGUP, MUTE, UNMUTE, TOGGLE} intent_t;

static void intent(const xiaotai_args_t *args, void *context)
{
    const uint32_t generation = (uint32_t)args->value;
    switch ((intent_t)(uintptr_t)context) {
    case ACCEPT: (void)xiaotai_call_accept(generation); break;
    case REJECT: (void)xiaotai_call_reject(generation); break;
    case HANGUP: (void)xiaotai_call_hangup(generation); break;
    case MUTE: (void)xiaotai_call_set_muted(generation, true); break;
    case UNMUTE: (void)xiaotai_call_set_muted(generation, false); break;
    case TOGGLE: (void)xiaotai_call_set_muted(generation, !s_call.status.muted); break;
    }
}

xiaotai_err_t xiaotai_module_call_init(void)
{
    if (s_call.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_call.config.ring_timeout_ms) s_call.config.ring_timeout_ms = 45000;
    if (!s_call.config.answer_timeout_ms) s_call.config.answer_timeout_ms = 30000;
    if (!s_call.config.connect_timeout_ms) s_call.config.connect_timeout_ms = 30000;
    if (!s_call.config.pending_releases) s_call.config.pending_releases = 4;
    if (default_latency(&s_call.config.latency)) s_call.config.latency = fallback_latency;
    xiaotai_err_t err = call_release_init(&s_call.status, s_call.config.pending_releases);
    static const xiaotai_contact_kind_def_t kind = {
        .name = XIAOTAI_CALL_CONTACT_KIND, .alias = "DEV", .server_type = "device", .presence = true, .friendships = true,
    };
    static const xiaotai_platform_requirement_t platform = {
        .request_body_bytes = 512, .response_bytes = CALL_RESPONSE_BYTES, .signal_bytes = 1024, .concurrent_requests = 3,
    };
    static const xiaotai_transport_requirement_t transport = {.links = 1, .command_pool_bytes = CALL_COMMAND_SHARE};
    const xiaotai_media_requirement_t media = {
        .uplink = audio, .downlink = audio, .downlink_burst_ms = BURST_MS, .packet_ms = 20, .latency = s_call.config.latency,
    };
    if (err == XIAOTAI_OK) err = xiaotai_contacts_kind_register(&kind, &s_call.contact_kind);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require(&platform);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require_service(XIAOTAI_PLATFORM_SERVICE_CALL);
    if (err == XIAOTAI_OK) err = xiaotai_transport_require(&transport);
    if (err == XIAOTAI_OK) err = xiaotai_media_require(&media);
    if (err == XIAOTAI_OK) err = call_signals_register();
    if (err == XIAOTAI_OK) err = call_profile_register(&s_call.config);
    if (err == XIAOTAI_OK) err = call_at_register();
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_PLATFORM_OFFLINE, offline, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, offline, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST("binding.needed", offline, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_PLATFORM_ONLINE, online, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ACTIVITY, activity_changed, NULL);
#ifdef XIAOTAI_CALL_HAS_SETTINGS
    s_call.settings = xiaotai_module_present("settings");
    if (s_call.settings && err == XIAOTAI_OK) {
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_SETTINGS_CHANGED, settings_changed, NULL);
    }
#endif
    static const xiaotai_intent_prepared_t dial = {.prepare = prepare_dial_intent, .dispatch = dispatch_dial_intent};
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_prepared(XIAOTAI_INTENT_CALL_DIAL, &dial, NULL, 0);
    static const char *const names[] = {XIAOTAI_INTENT_CALL_ACCEPT, XIAOTAI_INTENT_CALL_REJECT,
        XIAOTAI_INTENT_CALL_HANGUP, XIAOTAI_INTENT_CALL_MIC_OFF, XIAOTAI_INTENT_CALL_MIC_ON, XIAOTAI_INTENT_CALL_MIC_TOGGLE};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && err == XIAOTAI_OK; ++i) {
        err = xiaotai_intent_handle(names[i], intent, (void *)(uintptr_t)i);
    }
    if (err != XIAOTAI_OK) {
        call_release_reset();
    } else s_call.initialized = true;
    return err;
}

void xiaotai_module_call_start(void)
{
    if (!s_call.initialized || s_call.listener[0]) return;
    const void *part = NULL;
    const size_t count = xiaotai_activity_contributions(XIAOTAI_CALL_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                                                       XIAOTAI_CALL_SCENE, &part, 1);
    if (part) s_call.video = *(const xiaotai_link_media_t *)part;
    if (count > 1U) {
        s_call.diag.video_parts_ignored += (uint32_t)(count - 1U);
        xiaotai_eventlog_warn(SOURCE, "video.extra", 0, (int32_t)(count - 1U));
    }
    for (size_t i = 0; i < (part ? 2U : 1U); ++i) {
        const xiaotai_link_listener_t listener = {
            .name = i ? "call.video" : "call", .profile = profile(i != 0), .handlers = handlers(i + 1U),
        };
        const xiaotai_err_t err = xiaotai_link_listen(&listener, &s_call.listener[i]);
        if (err != XIAOTAI_OK) {
            s_call.listener[i] = 0;
            ++s_call.diag.listen_failures;
            xiaotai_eventlog_warn(SOURCE, "listen.refused", 0, err);
        }
    }
}

void xiaotai_call_reset(void)
{
    call_signaling_reset();
    finish(XIAOTAI_CALL_END_SYSTEM, XIAOTAI_OK, false, NULL, false);
    cancel_timer();
    disarm();
    clear_hold();
    close_link();
    call_release_reset();
    memset(&s_call, 0, sizeof(s_call));
}
