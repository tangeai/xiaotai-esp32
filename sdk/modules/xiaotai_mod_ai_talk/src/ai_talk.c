// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "ai_talk_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/link.h"
#include "xiaotai/modules/ai_talk_config.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"
#include "xiaotai/transport.h"

#define SOURCE "ai_talk"
#define BURST_MS 1000U
/* The service said the conversation is over. Its last sentence may still be held
 * or on the way: the link stays until everything was heard and nothing has come
 * for TAIL_QUIET_MS. In the service's turn its answer may not even have begun to
 * come, so that is given TAIL_WAIT_MS. Sound that goes on coming is received for
 * TAIL_LIMIT_MS at most; playing out what is then held takes up to
 * XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS more. */
#define TAIL_WAIT_MS 1000U
#define TAIL_QUIET_MS 800U
#define TAIL_LIMIT_MS 8000U
/* The media engine reports the end of it. Should that report not come, the
 * conversation ends on its own after this. */
#define ENDING_LIMIT_MS (TAIL_LIMIT_MS + XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS + 1000U)

/* Most the service's speech takes; the downlink is sized by it. */
#define OPUS_MAX_BITRATE_BPS 64000U

/* One way a conversation may sound: for the engine, for the link, for the service.
 * What is sent and what is heard need not be the same. */
typedef struct {
    xiaotai_media_format_t up, down;
    xiaotai_link_format_t link_up, link_down;
    const ai_talk_audio_t *wire_up, *wire_down;
} format_t;

#define G711 XIAOTAI_MEDIA_FORMAT_G711A_8K
#define OPUS XIAOTAI_MEDIA_FORMAT_OPUS_16K
#define LINK_G711 {XIAOTAI_LINK_CODEC_G711A, 8000U, 1}
#define LINK_OPUS {XIAOTAI_LINK_CODEC_OPUS, 16000U, 1}

static const format_t FORMAT_G711 = {G711, G711, LINK_G711, LINK_G711, &ai_talk_audio_g711, &ai_talk_audio_g711};
static const format_t FORMAT_OPUS = {OPUS, OPUS, LINK_OPUS, LINK_OPUS, &ai_talk_audio_opus, &ai_talk_audio_opus};
/* Hears Opus, sends G.711: the encoder is what a slow chip cannot afford beside echo cancelling. */
static const format_t FORMAT_HEARD = {G711, OPUS, LINK_G711, LINK_OPUS, &ai_talk_audio_g711, &ai_talk_audio_opus};
static const xiaotai_media_latency_t DEFAULT_LATENCY = XIAOTAI_MEDIA_LATENCY_BURSTY;

typedef enum {STEP_NONE, STEP_TOKEN, STEP_LINK, STEP_HANDSHAKE} step_t;

static struct {
    xiaotai_ai_talk_config_t config;
    xiaotai_ai_talk_status_t status;
    const format_t *format;      /* of the running conversation */
    bool opus_refused;           /* the service turned Opus down since boot */
    ai_talk_diag_t diag;
    bool initialized;
    bool link_open;
    uint32_t activity;
    uint32_t timer;
    uint32_t media;
    uint32_t preroll;
    step_t step;
    char role[AI_TALK_ROLE_MAX];
    char request_id[AI_TALK_REQUEST_ID_MAX];
} s_talk;
static atomic_uint s_signaling_hold;

enum { SIGNAL_RESTORE_POLL_MS = 80, SIGNAL_RESTORE_WARN_MS = 2000, SIGNAL_RESTORE_LATE_POLL_MS = 250 };
typedef struct {
    uint32_t session, link, hold, timer;
    int64_t deadline;
    bool overdue;
} signaling_restore_t;
static signaling_restore_t s_restore;
static xiaotai_critical_t s_restore_lock;
static atomic_bool s_restore_initialized;

static signaling_restore_t restore_take(uint32_t session)
{
    signaling_restore_t watch = {0};
    if (!atomic_load(&s_restore_initialized)) return watch;
    xiaotai_critical_enter(&s_restore_lock);
    if (!session || session == s_restore.session) {
        watch = s_restore;
        s_restore = (signaling_restore_t){0};
    }
    xiaotai_critical_exit(&s_restore_lock);
    return watch;
}

static void restore_unwatch(signaling_restore_t watch)
{
    if (watch.timer) xiaotai_timer_cancel(watch.timer);
    if (watch.link) xiaotai_link_unwatch_release(watch.link);
}

static void restore_close(signaling_restore_t watch, uint32_t active_hold)
{
    restore_unwatch(watch);
    const uint32_t hold = watch.hold ? watch.hold : active_hold;
    if (hold && watch.session) {
        xiaotai_platform_signaling_release_bounded(hold, (void *)(uintptr_t)watch.session);
    } else if (hold) xiaotai_platform_signaling_release(hold);
}

static void release_signaling(void)
{
    const uint32_t hold = atomic_exchange(&s_signaling_hold, 0U);
    if (s_talk.config.resume_signaling_after_link_release) restore_close(restore_take(0), hold);
    else if (hold) xiaotai_platform_signaling_release(hold);
}

static void restore_poll(void *context)
{
    const uint32_t session = (uint32_t)(uintptr_t)context;
    xiaotai_critical_enter(&s_restore_lock);
    const signaling_restore_t watch = s_restore;
    if (watch.session == session) s_restore.timer = 0;
    xiaotai_critical_exit(&s_restore_lock);
    if (watch.session != session || !watch.hold) return;
    const xiaotai_link_release_state_t state = xiaotai_link_release_state(watch.link);
    if (!xiaotai_platform_signaling_bounded_hold_active(watch.hold, context)) {
        const signaling_restore_t retired = restore_take(session);
        if (retired.session && state != XIAOTAI_LINK_RELEASE_COMPLETE && state != XIAOTAI_LINK_RELEASE_ABORTED)
            xiaotai_eventlog_warn(SOURCE, "signaling.release_expired", session, XIAOTAI_ERR_INVALID_STATE);
        restore_unwatch(retired);
        return;
    }
    if (state == XIAOTAI_LINK_RELEASE_COMPLETE || state == XIAOTAI_LINK_RELEASE_ABORTED) {
        if (state == XIAOTAI_LINK_RELEASE_ABORTED)
            xiaotai_eventlog_debug(SOURCE, "signaling.release_by_stop", session, 0);
        restore_close(restore_take(session), 0);
        return;
    }
    const int64_t remaining = watch.deadline - xiaotai_time_us();
    if (remaining <= 0 && !watch.overdue) {
        xiaotai_eventlog_warn(SOURCE, "signaling.release_timeout", session, XIAOTAI_ERR_TIMEOUT);
        xiaotai_critical_enter(&s_restore_lock);
        if (s_restore.session == session) s_restore.overdue = true;
        xiaotai_critical_exit(&s_restore_lock);
    }
    const uint32_t remaining_ms = remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 0U;
    const uint32_t delay = remaining_ms == 0 ? SIGNAL_RESTORE_LATE_POLL_MS :
        remaining_ms < SIGNAL_RESTORE_POLL_MS ? remaining_ms : SIGNAL_RESTORE_POLL_MS;
    uint32_t timer = 0;
    const xiaotai_err_t err = xiaotai_timer_start(delay, restore_poll, context, &timer);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_warn(SOURCE, "signaling.release_timer", session, err);
        restore_unwatch(restore_take(session));
        /* Keep the original hold bound when the owner cannot observe completion. */
        return;
    }
    xiaotai_critical_enter(&s_restore_lock);
    const bool keep = s_restore.session == session;
    if (keep) s_restore.timer = timer;
    xiaotai_critical_exit(&s_restore_lock);
    if (!keep) xiaotai_timer_cancel(timer);
}

static void finish_signaling(uint32_t session)
{
    if (!s_talk.config.resume_signaling_after_link_release) {
        release_signaling();
        return;
    }
    const uint32_t hold = atomic_exchange(&s_signaling_hold, 0U);
    if (!hold) return;
    xiaotai_critical_enter(&s_restore_lock);
    const bool watched = s_restore.session == session;
    if (watched) {
        s_restore.hold = hold;
        s_restore.deadline = xiaotai_time_us() + SIGNAL_RESTORE_WARN_MS * INT64_C(1000);
    }
    xiaotai_critical_exit(&s_restore_lock);
    if (watched) restore_poll((void *)(uintptr_t)session);
    else xiaotai_eventlog_warn(SOURCE, "signaling.release_unwatched", session, XIAOTAI_ERR_INVALID_STATE);
}

static void signaling_suspended(xiaotai_err_t result, void *ctx)
{
    const uint32_t session = (uint32_t)(uintptr_t)ctx;
    if (s_talk.status.session != session || !atomic_load(&s_signaling_hold)) return;
    if (result == XIAOTAI_OK) {
        xiaotai_eventlog_debug(SOURCE, "signaling.paused", session, 0);
    } else {
        release_signaling();
        xiaotai_eventlog_error(SOURCE, "signaling.pause", session, result);
    }
}

static void pause_signaling(void)
{
    if (!s_talk.config.pause_platform_signaling_while_active) return;
    const uint32_t session = s_talk.status.session, link = s_talk.status.link;
    if (!session || !link || xiaotai_activity_generation(s_talk.activity) != link) return;
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    if (!media.active || media.generation != link) return;
    if (s_talk.config.resume_signaling_after_link_release) {
        const xiaotai_err_t watch = xiaotai_link_watch_release(link);
        if (watch != XIAOTAI_OK) {
            xiaotai_eventlog_warn(SOURCE, "signaling.release_watch", session, watch);
            return;
        }
        xiaotai_critical_enter(&s_restore_lock);
        s_restore = (signaling_restore_t){.session = session, .link = link};
        xiaotai_critical_exit(&s_restore_lock);
    }
    uint32_t hold = 0;
    const xiaotai_err_t result = xiaotai_platform_signaling_hold_bounded(
        60000U, signaling_suspended, (void *)(uintptr_t)session, &hold);
    if (result == XIAOTAI_OK) {
        atomic_store(&s_signaling_hold, hold);
    } else {
        if (s_talk.config.resume_signaling_after_link_release) restore_close(restore_take(session), 0);
        xiaotai_eventlog_debug(SOURCE, "signaling.skipped", session, result);
    }
}

static void publish_status(const char *event)
{
    xiaotai_ai_talk_status_t status;
    xiaotai_ai_talk_status(&status);
    const xiaotai_args_t args = {.value = status.phase};
    const xiaotai_err_t err = status.phase == XIAOTAI_AI_TALK_IDLE &&
        !strcmp(event, XIAOTAI_EVENT_AI_TALK_STATE) ?
        xiaotai_event_emit_owner_terminal(event, &args, &status, sizeof(status)) :
        xiaotai_event_emit(event, &args, &status, sizeof(status));
    if (err != XIAOTAI_OK) {
        ++s_talk.diag.events_lost;
    }
}

static void phase(xiaotai_ai_talk_phase_t next)
{
    static const char *const names[] = {"idle", "connecting", "listening", "thinking", "speaking", "ending"};
    if (s_talk.status.phase == next) return;
    XIAOTAI_LOGD(SOURCE, "phase %s gen=%" PRIu32, (size_t)next < sizeof(names) / sizeof(names[0]) ? names[next] : "?",
                 s_talk.status.session);
    s_talk.status.phase = next;
    publish_status(XIAOTAI_EVENT_AI_TALK_STATE);
}

static void cancel_deadline(void)
{
    if (s_talk.timer) xiaotai_timer_cancel(s_talk.timer);
    s_talk.timer = 0;
    s_talk.step = STEP_NONE;
}

static void disconnect(void)
{
    const uint32_t link = s_talk.status.link;
    s_talk.status.link = 0;
    s_talk.link_open = false;
    if (link) xiaotai_link_disconnect(link);
}

xiaotai_err_t ai_talk_send(const char *json, size_t length)
{
    if (!s_talk.link_open) return XIAOTAI_ERR_INVALID_STATE;
    return xiaotai_link_command(s_talk.status.link, AI_TALK_COMMAND, json, length);
}

/* When the running conversation began, for the one line that sums it up. */
static int64_t s_started_us;

/* How a conversation ended, in words, with how long it ran. */
static void log_ended(uint32_t session, xiaotai_ai_talk_end_t end, xiaotai_err_t error)
{
    static const char *const why[] = {
        [XIAOTAI_AI_TALK_END_NONE] = "ended",
        [XIAOTAI_AI_TALK_END_LOCAL] = "stopped here",
        [XIAOTAI_AI_TALK_END_REMOTE] = "the service ended it",
        [XIAOTAI_AI_TALK_END_PREEMPTED] = "a higher-priority activity began",
        [XIAOTAI_AI_TALK_END_SYSTEM] = "the system was quiesced or the binding ended",
        [XIAOTAI_AI_TALK_END_TIMEOUT] = "a step did not finish in time",
        [XIAOTAI_AI_TALK_END_REFUSED] = "the token or the session was refused",
        [XIAOTAI_AI_TALK_END_UNSUPPORTED] = "the service offered an audio format this device cannot use",
        [XIAOTAI_AI_TALK_END_NETWORK] = "the link could not be made or broke",
        [XIAOTAI_AI_TALK_END_NO_MEMORY] = "out of memory",
        [XIAOTAI_AI_TALK_END_AUDIO] = "the audio session failed",
        [XIAOTAI_AI_TALK_END_PROTOCOL] = "a malformed command, or a tool reply could not be sent",
    };
    const char *text = (size_t)end < sizeof(why) / sizeof(why[0]) && why[end] ? why[end] : "unknown end";
    const unsigned seconds = (unsigned)((xiaotai_time_us() - s_started_us) / 1000000);
    if (error == XIAOTAI_OK) XIAOTAI_LOGI(SOURCE, "ended gen=%" PRIu32 ": %s after %u s", session, text, seconds);
    else XIAOTAI_LOGE(SOURCE, "ended gen=%" PRIu32 ": %s (error 0x%x) after %u s", session, text, (unsigned)error, seconds);
}

static void finish(xiaotai_ai_talk_end_t end, xiaotai_err_t error, bool notify)
{
    if (!s_talk.status.session) return;
    const uint32_t session = s_talk.status.session;
    cancel_deadline();
    ai_talk_tools_end(session);
    /* Stop the camera attachment before releasing the link it uses. */
    if (s_talk.activity) xiaotai_activity_set_generation(s_talk.activity, 0);
    if (notify && s_talk.link_open && s_talk.status.phase != XIAOTAI_AI_TALK_ENDING) {
        (void)ai_talk_send(AI_TALK_END_SESSION, sizeof(AI_TALK_END_SESSION) - 1U);
    }
    if (s_talk.media) xiaotai_media_close(s_talk.media);
    s_talk.media = 0;
    disconnect();
    if (s_talk.preroll) xiaotai_media_preroll_release(s_talk.preroll);
    s_talk.preroll = 0;
    if (s_talk.activity) xiaotai_activity_end(s_talk.activity);
    s_talk.activity = 0;
    s_talk.role[0] = s_talk.request_id[0] = '\0';
    s_talk.status.session = 0;
    s_talk.status.end = end;
    s_talk.status.error = error;
    /* The ring keeps the same two events for tools; the log gets one sentence. */
    if (error == XIAOTAI_OK) {
        ++s_talk.status.completed;
        xiaotai_eventlog_record(SOURCE, "end", session, end);
    } else {
        ++s_talk.status.failed;
        xiaotai_eventlog_emit(&(xiaotai_eventlog_event_t){
            .source = SOURCE, .label = "end", .generation = session, .value = (int32_t)error,
            .level = (uint8_t)XIAOTAI_EVENTLOG_RECORD, .kind = (uint8_t)XIAOTAI_EVENTLOG_ERR});
    }
    log_ended(session, end, error);
    phase(XIAOTAI_AI_TALK_IDLE);
    finish_signaling(session);
}

static void fail(xiaotai_ai_talk_end_t end, xiaotai_err_t err)
{
    finish(err == XIAOTAI_ERR_NO_MEM ? XIAOTAI_AI_TALK_END_NO_MEMORY : end,
           err, XIAOTAI_AI_TALK_NOTIFY_FAILURES);
}

static void timed_out(void *context)
{
    if ((uint32_t)(uintptr_t)context != s_talk.status.session) return;
    s_talk.timer = 0;
    if (s_talk.status.phase == XIAOTAI_AI_TALK_ENDING) {
        /* The service ended it; only the engine's word that it was heard is missing. */
        xiaotai_eventlog_warn(SOURCE, "ending.timeout", s_talk.status.session, XIAOTAI_ERR_TIMEOUT);
        finish(XIAOTAI_AI_TALK_END_REMOTE, XIAOTAI_OK, false);
        return;
    }
    fail(XIAOTAI_AI_TALK_END_TIMEOUT, XIAOTAI_ERR_TIMEOUT);
}

static xiaotai_err_t deadline(step_t step, uint32_t delay)
{
    cancel_deadline();
    const xiaotai_err_t err = xiaotai_timer_start(delay, timed_out,
                    (void *)(uintptr_t)s_talk.status.session, &s_talk.timer);
    if (err == XIAOTAI_OK) s_talk.step = step;
    return err;
}

static void on_revoked(uint32_t id, xiaotai_revoke_t reason, void *context)
{
    (void)context;
    if (id != s_talk.activity) return;
    s_talk.activity = 0;
    finish(reason == XIAOTAI_REVOKE_SYSTEM ? XIAOTAI_AI_TALK_END_SYSTEM : XIAOTAI_AI_TALK_END_PREEMPTED,
           XIAOTAI_OK, true);
}

static void on_media(uint32_t generation, xiaotai_media_event_t event, xiaotai_err_t error, void *context)
{
    (void)context;
    if (!generation || generation != s_talk.media) return;
    if (event == XIAOTAI_MEDIA_EVENT_DRAINED && s_talk.status.phase == XIAOTAI_AI_TALK_ENDING) {
        if (error == XIAOTAI_OK) {
            finish(XIAOTAI_AI_TALK_END_REMOTE, XIAOTAI_OK, false);
        } else {
            fail(error == XIAOTAI_ERR_TIMEOUT ? XIAOTAI_AI_TALK_END_TIMEOUT : XIAOTAI_AI_TALK_END_AUDIO, error);
        }
    } else if (event == XIAOTAI_MEDIA_EVENT_FAILED) {
        fail(XIAOTAI_AI_TALK_END_AUDIO, error == XIAOTAI_OK ? XIAOTAI_FAIL : error);
    }
}

static void enter_ending(void)
{
    xiaotai_activity_set_generation(s_talk.activity, 0);
    ai_talk_tools_end(s_talk.status.session);
    /* With no timer free the engine's own limits still end it. */
    (void)deadline(STEP_NONE, ENDING_LIMIT_MS);
    phase(XIAOTAI_AI_TALK_ENDING);
}

static void remote_end(void)
{
    const xiaotai_media_tail_t tail = {
        .wait_ms = s_talk.status.phase == XIAOTAI_AI_TALK_LISTENING ? 0 : TAIL_WAIT_MS,
        .quiet_ms = TAIL_QUIET_MS, .limit_ms = TAIL_LIMIT_MS,
    };
    enter_ending();
    const xiaotai_err_t err = xiaotai_media_finish(s_talk.media, &tail);
    if (err != XIAOTAI_OK) fail(XIAOTAI_AI_TALK_END_AUDIO, err);
}

static void on_opened(void *context, uint32_t generation)
{
    (void)context;
    if (generation != s_talk.status.link || s_talk.step != STEP_LINK) return;
    s_talk.link_open = true;
    const xiaotai_media_session_t media = {
        .generation = generation, .uplink = s_talk.format->up, .downlink = s_talk.format->down,
        .downlink_burst_ms = BURST_MS, .duplex = true, .sink = xiaotai_link_audio_sink(),
        .latency = s_talk.config.latency,
        .flags = XIAOTAI_MEDIA_HOLD | (s_talk.config.trace_arrivals ? XIAOTAI_MEDIA_SESSION_TRACE_ARRIVALS : 0U),
        .preroll_token = s_talk.preroll, .event = on_media,
    };
    xiaotai_err_t err = xiaotai_media_open(&media);
    if (err != XIAOTAI_OK) {
        /* Invalid-state open consumes a rejected pre-roll only when the
         * engine releases it; releasing a stale token is harmless. */
        fail(XIAOTAI_AI_TALK_END_AUDIO, err);
        return;
    }
    s_talk.media = generation;
    err = xiaotai_link_receive(generation, XIAOTAI_LINK_AUDIO, NULL, NULL);
    if (err != XIAOTAI_OK) {
        fail(XIAOTAI_AI_TALK_END_NETWORK, err);
        return;
    }
    char device[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U];
    err = xiaotai_identity_device_id(device, sizeof(device));
    if (err != XIAOTAI_OK) {
        fail(XIAOTAI_AI_TALK_END_SYSTEM, err);
        return;
    }
    const uint32_t high = xiaotai_random(), low = xiaotai_random();
    snprintf(s_talk.request_id, sizeof(s_talk.request_id), "%08" PRIx32 "%08" PRIx32, high, low);
    char payload[AI_TALK_REQUEST_TEXT_MAX];
    size_t size = 0;
    err = ai_talk_session_request(payload, sizeof(payload), s_talk.request_id, device, s_talk.role,
                                  s_talk.format->wire_up, s_talk.format->wire_down, &size);
    if (err != XIAOTAI_OK) {
        fail(XIAOTAI_AI_TALK_END_PROTOCOL, err);
        return;
    }
    err = deadline(STEP_HANDSHAKE, s_talk.config.handshake_timeout_ms);
    if (err == XIAOTAI_OK) err = ai_talk_send(payload, size);
    if (err != XIAOTAI_OK) fail(XIAOTAI_AI_TALK_END_NETWORK, err);
    else publish_status(XIAOTAI_EVENT_AI_TALK_MEDIA);
}

static bool running(void)
{
    return s_talk.status.phase >= XIAOTAI_AI_TALK_LISTENING &&
           s_talk.status.phase <= XIAOTAI_AI_TALK_SPEAKING;
}

static void on_closed(void *context, uint32_t generation, xiaotai_link_end_t reason, int detail)
{
    (void)context;
    if (!generation || generation != s_talk.status.link) return;
    s_talk.status.link = 0;
    s_talk.link_open = false;
    if (s_talk.status.phase == XIAOTAI_AI_TALK_ENDING) {
        /* Nothing more can come; refused when the drain runs already. */
        (void)xiaotai_media_drain(s_talk.media, XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS);
        return;
    }
    if (running() && (reason == XIAOTAI_LINK_END_PEER || reason == XIAOTAI_LINK_END_PEER_FAULT)) {
        /* The service left without a word: what came is played to its end. */
        enter_ending();
        const xiaotai_err_t err = xiaotai_media_drain(s_talk.media, XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS);
        if (err != XIAOTAI_OK) fail(XIAOTAI_AI_TALK_END_AUDIO, err);
        return;
    }
    if (reason == XIAOTAI_LINK_END_TIMEOUT) {
        fail(XIAOTAI_AI_TALK_END_TIMEOUT, XIAOTAI_ERR_TIMEOUT);
    } else if (reason == XIAOTAI_LINK_END_NO_MEMORY) {
        fail(XIAOTAI_AI_TALK_END_NO_MEMORY, XIAOTAI_ERR_NO_MEM);
    } else {
        fail(XIAOTAI_AI_TALK_END_NETWORK, reason == XIAOTAI_LINK_END_PEER_FAULT ? detail : XIAOTAI_FAIL);
    }
}

static void handshake(const ai_talk_message_t *message)
{
    switch (ai_talk_handshake_check(message, s_talk.request_id, s_talk.format->wire_up, s_talk.format->wire_down)) {
    case AI_TALK_HANDSHAKE_NONE:
        ++s_talk.status.stale_commands;
        return;
    case AI_TALK_HANDSHAKE_REFUSED:
        if (s_talk.format != &FORMAT_G711) s_talk.opus_refused = true;
        fail(XIAOTAI_AI_TALK_END_REFUSED, XIAOTAI_FAIL);
        return;
    case AI_TALK_HANDSHAKE_UNSUPPORTED:
        if (s_talk.format != &FORMAT_G711) s_talk.opus_refused = true;
        fail(XIAOTAI_AI_TALK_END_UNSUPPORTED, XIAOTAI_ERR_NOT_SUPPORTED);
        return;
    case AI_TALK_HANDSHAKE_ACCEPTED:
        break;
    }
    xiaotai_err_t err = xiaotai_media_activate(s_talk.media);
    if (err != XIAOTAI_OK) {
        fail(XIAOTAI_AI_TALK_END_AUDIO, err);
        return;
    }
    /* The media session now owns the pre-roll, even if permitting fails. */
    s_talk.preroll = 0;
    err = xiaotai_link_permit(s_talk.status.link, XIAOTAI_LINK_AUDIO, true);
    if (err != XIAOTAI_OK) {
        fail(XIAOTAI_AI_TALK_END_NETWORK, err);
        return;
    }
    cancel_deadline();
    xiaotai_activity_set_generation(s_talk.activity, s_talk.status.link);
    phase(XIAOTAI_AI_TALK_LISTENING);
    pause_signaling();
}

static void on_command(void *context, uint32_t generation, const xiaotai_link_command_t *command)
{
    (void)context;
    const bool ending = s_talk.status.phase == XIAOTAI_AI_TALK_ENDING;
    if (!generation || generation != s_talk.status.link || !command ||
        !xiaotai_link_command_is(command->word, AI_TALK_COMMAND) ||
        (s_talk.step != STEP_HANDSHAKE && !running() && !ending)) {
        ++s_talk.status.stale_commands;
        return;
    }
    if (command->dropped || command->size > s_talk.config.command_max_bytes) {
        ++s_talk.diag.commands_dropped;
        if (XIAOTAI_AI_TALK_DROPPED_ENDS) fail(XIAOTAI_AI_TALK_END_PROTOCOL, XIAOTAI_ERR_INVALID_SIZE);
        return;
    }
    ai_talk_message_t message;
    ai_talk_message_parse((const char *)command->data, command->size, s_talk.diag.token_capacity, &message);
    if (message.kind == AI_TALK_MESSAGE_INVALID) {
        ++s_talk.status.stale_commands;
    } else if (s_talk.step == STEP_HANDSHAKE) {
        handshake(&message);
    } else if (ai_talk_message_is(&message, "caption")) {
        xiaotai_ai_talk_speaker_t speaker;
        bool final;
        const uint32_t previous = ai_talk_caption_revision();
        if (ai_talk_caption_apply(&message.params, &speaker, &final)) {
            const uint32_t current = ai_talk_caption_revision();
            if (previous != current) {
                const xiaotai_args_t args = {.value = current};
                if (xiaotai_event_emit(XIAOTAI_EVENT_AI_TALK_CAPTION, &args, NULL, 0) != XIAOTAI_OK) {
                    ++s_talk.diag.events_lost;
                }
            }
            if (!ending) phase(speaker == XIAOTAI_AI_TALK_SPEAKER_AI ? XIAOTAI_AI_TALK_SPEAKING :
                               final ? XIAOTAI_AI_TALK_THINKING : XIAOTAI_AI_TALK_LISTENING);
        }
    } else if (ending) {
        ++s_talk.status.stale_commands;   /* only the words of the last sentence still matter */
    } else if (ai_talk_message_is(&message, "round_start")) {
        phase(XIAOTAI_AI_TALK_SPEAKING);
    } else if (ai_talk_message_is(&message, "round_end")) {
        phase(XIAOTAI_AI_TALK_LISTENING);
    } else if (ai_talk_message_is(&message, "interrupt")) {
        (void)xiaotai_media_flush(s_talk.media);
        phase(XIAOTAI_AI_TALK_LISTENING);
    } else if (ai_talk_message_is(&message, "end_session")) {
        remote_end();
    } else if (message.kind == AI_TALK_MESSAGE_METHOD) {
        ai_talk_tools_dispatch(&message, ai_talk_message_is(&message, "device_action"));
    }
}

static xiaotai_err_t connect(const ai_talk_credentials_t *credentials)
{
    const bool subscribe = s_talk.config.request_downlink;
    xiaotai_link_request_t request = {
        .kind = XIAOTAI_LINK_WHIP, .peer = credentials->peer, .token = credentials->token,
        .timeout_ms = s_talk.config.connect_timeout_ms,
        .profile = {
            .audio = {
                .send = true, .send_stream = AI_TALK_STREAM, .send_format = s_talk.format->link_up,
                .receive = true, .receive_stream = subscribe ? AI_TALK_STREAM : XIAOTAI_LINK_STREAM_ANY,
                .receive_request = subscribe, .receive_formats = {s_talk.format->link_down},
            },
            .command_max_bytes = s_talk.config.command_max_bytes,
            .command_pool_bytes = s_talk.config.command_pool_bytes,
        },
    };
    const void *video = NULL;
    const size_t count = xiaotai_activity_contributions(XIAOTAI_AI_TALK_ACTIVITY, XIAOTAI_LINK_SLOT_VIDEO,
                                                       XIAOTAI_AI_TALK_SCENE, &video, 1);
    if (count && video) request.profile.video = *(const xiaotai_link_media_t *)video;
    if (count > 1U) s_talk.diag.video_extra += (uint32_t)(count - 1U);
    static const xiaotai_link_handlers_t handlers = {
        .opened = on_opened, .closed = on_closed, .command = on_command,
    };
    xiaotai_err_t err = deadline(STEP_LINK, request.timeout_ms);
    if (err == XIAOTAI_OK) err = xiaotai_link_connect(&request, &handlers, &s_talk.status.link);
    return err;
}

static void on_token(const xiaotai_platform_reply_t *reply, void *context)
{
    if (!s_talk.status.session || (uint32_t)(uintptr_t)context != s_talk.status.session ||
        s_talk.step != STEP_TOKEN) {
        ++s_talk.status.stale_calls;
        return;
    }
    if (reply->result != XIAOTAI_OK) {
        fail(reply->result == XIAOTAI_ERR_TIMEOUT ? XIAOTAI_AI_TALK_END_TIMEOUT : XIAOTAI_AI_TALK_END_NETWORK,
             reply->result);
        return;
    }
    ai_talk_credentials_t *credentials = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*credentials));
    if (!credentials) {
        fail(XIAOTAI_AI_TALK_END_NO_MEMORY, XIAOTAI_ERR_NO_MEM);
        return;
    }
    const xiaotai_err_t parsed = ai_talk_token_parse(reply->http_status, reply->body, reply->size,
                            XIAOTAI_AI_TALK_ROLE_REQUIRED, s_talk.diag.token_capacity, credentials);
    xiaotai_err_t err = parsed;
    if (parsed == XIAOTAI_OK) {
        memcpy(s_talk.role, credentials->role, strlen(credentials->role) + 1U);
        err = connect(credentials);
    }
    xiaotai_identity_wipe(credentials, sizeof(*credentials));
    xiaotai_mem_free(credentials);
    if (err != XIAOTAI_OK) fail(parsed != XIAOTAI_OK ? XIAOTAI_AI_TALK_END_REFUSED :
                                XIAOTAI_AI_TALK_END_NETWORK, err);
}

void xiaotai_ai_talk_status(xiaotai_ai_talk_status_t *status)
{
    if (!status) return;
    *status = s_talk.status;
    status->caption_revision = ai_talk_caption_revision();
}

bool ai_talk_current(uint32_t *session, uint32_t *link)
{
    if (!running()) return false;
    if (session) *session = s_talk.status.session;
    if (link) *link = s_talk.status.link;
    return true;
}

void ai_talk_count_tool(bool handled)
{
    ++s_talk.status.tool_calls;
    if (!handled) ++s_talk.status.tool_unhandled;
}

void ai_talk_count_stale_reply(void) { ++s_talk.status.stale_calls; }
void ai_talk_count_tool_reply_failure(void) { ++s_talk.diag.tool_reply_failures; }

void ai_talk_diag(ai_talk_diag_t *out)
{
    if (out) *out = s_talk.diag;
}

/* A farewell does not stand in the way of the next conversation. */
static bool occupied(void)
{
    return s_talk.status.session && s_talk.status.phase != XIAOTAI_AI_TALK_ENDING;
}

xiaotai_err_t xiaotai_ai_talk_start(const xiaotai_ai_talk_start_t *options, uint32_t *session)
{
    const uint32_t preroll = options ? options->preroll_token : 0;
    if (session) *session = 0;
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    /* Why a start is refused: the one thing a developer needs and the status
     * code alone does not say. */
    const char *why = "the module is not started";
    if (!xiaotai_on_owner()) return err; /* media release has the same owner contract */
    if (!s_talk.initialized) goto refuse;
    if (occupied()) {
        why = "a conversation is already running";
        goto refuse;
    }
    if (!s_talk.diag.token_capacity) {
        err = XIAOTAI_ERR_NO_MEM;
        why = "no memory was reserved for the token";
        goto refuse;
    }
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    if (media.mic_muted) {
        why = "the microphone is muted: unmute it first";
        goto refuse;
    }
    const format_t *const wanted = s_talk.config.opus_heard_only ? &FORMAT_HEARD : &FORMAT_OPUS;
    s_talk.format = s_talk.config.opus && !s_talk.opus_refused && xiaotai_media_can_send(&wanted->up) &&
                    xiaotai_media_can_receive(&wanted->down) ? wanted : &FORMAT_G711;
    if (!xiaotai_media_can_send(&s_talk.format->up) || !xiaotai_media_can_receive(&s_talk.format->down)) {
        why = "the audio engine cannot run the conversation format now";
        goto refuse;
    }
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.phase != XIAOTAI_PLATFORM_ONLINE || xiaotai_transport_state() != XIAOTAI_TRANSPORT_ONLINE ||
        (XIAOTAI_AI_TALK_START_NEEDS_PROFILE && !platform.profile_ready)) {
        err = XIAOTAI_ERR_PLATFORM_OFFLINE;
        why = "the platform or the transport is not online yet";
        goto refuse;
    }
    static const xiaotai_activity_t activity = {
        .name = XIAOTAI_AI_TALK_ACTIVITY, .revoked = on_revoked,
        .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT | XIAOTAI_RESOURCE_LINK,
    };
    /* Only a farewell is left of the one before. */
    if (s_talk.status.session) finish(XIAOTAI_AI_TALK_END_REMOTE, XIAOTAI_OK, false);
    err = xiaotai_activity_begin(&activity, &s_talk.activity);
    if (err != XIAOTAI_OK) {
        why = "another activity holds the audio or the link";
        goto refuse;
    }
    s_talk.status.session = xiaotai_generation_next();
    err = deadline(STEP_TOKEN, s_talk.config.token_timeout_ms);
    static const xiaotai_platform_request_t request = {
        .service = XIAOTAI_PLATFORM_SERVICE_AI, .method = XIAOTAI_PLATFORM_GET, .path = AI_TALK_TOKEN_PATH,
        .timeout_ms = AI_TALK_TOKEN_HTTP_TIMEOUT_MS, .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
        .flags = XIAOTAI_PLATFORM_URGENT | XIAOTAI_PLATFORM_RELEASE,
    };
    if (err == XIAOTAI_OK) {
        err = xiaotai_platform_request(&request, on_token, (void *)(uintptr_t)s_talk.status.session);
    }
    if (err != XIAOTAI_OK) {
        cancel_deadline();
        xiaotai_activity_end(s_talk.activity);
        s_talk.activity = s_talk.status.session = 0;
        why = "the token request could not be sent";
        goto refuse;
    }
    ai_talk_caption_begin(s_talk.status.session);
    s_talk.preroll = preroll;
    s_talk.status.preroll = preroll != 0;
    s_talk.status.origin = options ? options->origin : XIAOTAI_AI_TALK_ORIGIN_USER;
    s_talk.status.end = XIAOTAI_AI_TALK_END_NONE;
    s_talk.status.error = XIAOTAI_OK;
    ++s_talk.status.started;
    s_started_us = xiaotai_time_us();
    XIAOTAI_LOGI(SOURCE, "started gen=%" PRIu32 " (%s)", s_talk.status.session,
                 s_talk.status.origin == XIAOTAI_AI_TALK_ORIGIN_USER ? "by the user"
                 : s_talk.status.origin == XIAOTAI_AI_TALK_ORIGIN_WAKE ? "by the wake word" : "by another module");
    phase(XIAOTAI_AI_TALK_CONNECTING);
    if (session) *session = s_talk.status.session;
    return XIAOTAI_OK;

refuse:
    XIAOTAI_LOGW(SOURCE, "start refused: %s (error 0x%x)", why, (unsigned)err);
    if (preroll) xiaotai_media_preroll_release(preroll);
    return err;
}

static bool same_session(uint32_t session)
{
    if (!xiaotai_on_owner()) return false;
    if (!s_talk.status.session || (session && session != s_talk.status.session)) {
        ++s_talk.status.stale_calls;
        return false;
    }
    return true;
}

xiaotai_err_t xiaotai_ai_talk_stop(uint32_t session)
{
    if (!same_session(session)) return XIAOTAI_ERR_INVALID_STATE;
    finish(XIAOTAI_AI_TALK_END_LOCAL, XIAOTAI_OK, true);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_preroll_lost(uint32_t token)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_talk.status.session || !token || token != s_talk.preroll) {
        ++s_talk.status.stale_calls;
        return XIAOTAI_ERR_INVALID_STATE;
    }
    fail(XIAOTAI_AI_TALK_END_AUDIO, XIAOTAI_ERR_INVALID_STATE);
    return XIAOTAI_OK;
}

static xiaotai_err_t speech_command(uint32_t session, bool interrupt)
{
    if (!same_session(session) || !running()) return XIAOTAI_ERR_INVALID_STATE;
    if (interrupt) (void)xiaotai_media_flush(s_talk.media);
    const xiaotai_err_t err = interrupt ? ai_talk_send(AI_TALK_INTERRUPT, sizeof(AI_TALK_INTERRUPT) - 1U) :
                                         ai_talk_send(AI_TALK_SUBMIT, sizeof(AI_TALK_SUBMIT) - 1U);
    if (err != XIAOTAI_OK) {
        fail(XIAOTAI_AI_TALK_END_NETWORK, err);
    } else if (interrupt) {
        phase(XIAOTAI_AI_TALK_LISTENING);
    }
    return err;
}

xiaotai_err_t xiaotai_ai_talk_interrupt(uint32_t session) { return speech_command(session, true); }
xiaotai_err_t xiaotai_ai_talk_submit(uint32_t session) { return speech_command(session, false); }

static void on_start(const xiaotai_args_t *args, void *context)
{
    (void)args;
    (void)context;
    if (!occupied()) (void)xiaotai_ai_talk_start(NULL, NULL);
}

static void on_toggle(const xiaotai_args_t *args, void *context)
{
    const uint32_t session = args ? (uint32_t)args->value : 0;
    if (session || occupied()) {
        (void)xiaotai_ai_talk_stop(session);
    } else {
        on_start(NULL, context);
    }
}

static void on_stop(const xiaotai_args_t *args, void *context)
{
    (void)context;
    (void)xiaotai_ai_talk_stop(args ? (uint32_t)args->value : 0);
}

static void on_interrupt(const xiaotai_args_t *args, void *context)
{
    (void)context;
    (void)xiaotai_ai_talk_interrupt(args ? (uint32_t)args->value : 0);
}

static void on_submit(const xiaotai_args_t *args, void *context)
{
    (void)context;
    (void)xiaotai_ai_talk_submit(args ? (uint32_t)args->value : 0);
}

static void on_offline(const xiaotai_event_t *event, void *context)
{
    (void)context;
    if (s_talk.config.end_on_platform_offline) {
        /* A conversation the service has ended did not fail. */
        if (s_talk.status.phase == XIAOTAI_AI_TALK_ENDING) finish(XIAOTAI_AI_TALK_END_REMOTE, XIAOTAI_OK, false);
        else finish(XIAOTAI_AI_TALK_END_NETWORK, XIAOTAI_ERR_PLATFORM_OFFLINE, true);
    } else if (event->args.value == XIAOTAI_PLATFORM_OFFLINE_BINDING) {
        finish(XIAOTAI_AI_TALK_END_SYSTEM, XIAOTAI_OK, true);
    }
}

static void on_binding_needed(const xiaotai_event_t *event, void *context)
{
    (void)event;
    (void)context;
    finish(XIAOTAI_AI_TALK_END_SYSTEM, XIAOTAI_OK, true);
}

static void on_transport_state(const xiaotai_event_t *event, void *context)
{
    (void)context;
    if (event->args.value != XIAOTAI_TRANSPORT_ONLINE) {
        if (s_talk.status.phase == XIAOTAI_AI_TALK_ENDING) finish(XIAOTAI_AI_TALK_END_REMOTE, XIAOTAI_OK, false);
        else fail(XIAOTAI_AI_TALK_END_NETWORK, XIAOTAI_ERR_PLATFORM_OFFLINE);
    }
}

xiaotai_err_t xiaotai_ai_talk_configure(const xiaotai_ai_talk_config_t *config)
{
    if (s_talk.initialized) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_ai_talk_config_t zero = {0};
    const xiaotai_ai_talk_config_t *cfg = config ? config : &zero;
    if ((cfg->caption_bytes && cfg->caption_bytes < 64U) ||
        (cfg->resume_signaling_after_link_release && !cfg->pause_platform_signaling_while_active) ||
        cfg->command_max_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES ||
        cfg->command_pool_bytes > XIAOTAI_LINK_COMMAND_LIMIT_BYTES ||
        cfg->token_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS || cfg->connect_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS ||
        cfg->handshake_timeout_ms > XIAOTAI_TIMER_DELAY_MAX_MS) return XIAOTAI_ERR_INVALID_ARG;
    const uint32_t max_command = cfg->command_max_bytes ? cfg->command_max_bytes : XIAOTAI_LINK_COMMAND_DEFAULT_BYTES;
    if (cfg->command_pool_bytes && cfg->command_pool_bytes < max_command) return XIAOTAI_ERR_INVALID_ARG;
    s_talk.config = *cfg;
    return XIAOTAI_OK;
}

static void normalize_config(void)
{
    xiaotai_ai_talk_config_t *cfg = &s_talk.config;
    const bool external = xiaotai_mem_has_external();
    if (!cfg->token_timeout_ms) cfg->token_timeout_ms = 17000U;
    if (!cfg->connect_timeout_ms) cfg->connect_timeout_ms = 12000U;
    if (!cfg->handshake_timeout_ms) cfg->handshake_timeout_ms = 10000U;
    if (!cfg->caption_bytes) cfg->caption_bytes = external ? 4096U : 193U;
    if (!cfg->command_max_bytes) cfg->command_max_bytes = XIAOTAI_LINK_COMMAND_DEFAULT_BYTES;
    if (!cfg->command_pool_bytes) {
        const uint32_t minimum = external ? 8192U : 4096U;
        cfg->command_pool_bytes = minimum > cfg->command_max_bytes ? minimum : cfg->command_max_bytes;
    }
    const xiaotai_media_latency_t *p = &cfg->latency;
    if (!(p->start_ms || p->base_ms || p->min_ms || p->max_ms || p->surplus_ms || p->settle_s ||
          p->slow_permille || p->steady_jitter_ms || p->steady_peak_ms || p->step_down_ms)) {
        cfg->latency = DEFAULT_LATENCY;
    }
}

xiaotai_err_t xiaotai_module_ai_talk_init(void)
{
    if (s_talk.config.resume_signaling_after_link_release && !atomic_load(&s_restore_initialized)) {
        xiaotai_critical_init(&s_restore_lock);
        atomic_store(&s_restore_initialized, true);
    }
    s_talk.initialized = true;
    normalize_config();
    static const xiaotai_platform_requirement_t platform = {
        .response_bytes = AI_TALK_TOKEN_RESPONSE_BYTES, .concurrent_requests = 1,
    };
    const xiaotai_transport_requirement_t transport = {.links = 1, .command_pool_bytes = s_talk.config.command_pool_bytes};
    const xiaotai_media_requirement_t media = {
        .downlink_burst_ms = BURST_MS, .uplink = FORMAT_G711.up, .downlink = FORMAT_G711.down,
        .latency = s_talk.config.latency,
    };
    const xiaotai_media_requirement_t opus = {
        .downlink_burst_ms = BURST_MS, .max_bitrate_bps = OPUS_MAX_BITRATE_BPS,
        .uplink = s_talk.config.opus_heard_only ? FORMAT_HEARD.up : FORMAT_OPUS.up, .downlink = FORMAT_OPUS.down,
        .latency = s_talk.config.latency,
    };
    xiaotai_err_t err = xiaotai_platform_require(&platform);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require_service(XIAOTAI_PLATFORM_SERVICE_AI);
    if (err == XIAOTAI_OK) err = xiaotai_transport_require(&transport);
    if (err == XIAOTAI_OK) err = xiaotai_media_require(&media);
    if (err == XIAOTAI_OK && s_talk.config.opus) err = xiaotai_media_require(&opus);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_PLATFORM_OFFLINE, on_offline, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_BINDING_NEEDED, on_binding_needed, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_TRANSPORT_STATE, on_transport_state, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_TALK_START, on_start, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_TALK_TOGGLE, on_toggle, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_TALK_STOP, on_stop, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_TALK_INTERRUPT, on_interrupt, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_TALK_SUBMIT, on_submit, NULL);
    if (err == XIAOTAI_OK) err = ai_talk_at_init();
    return err;
}

void xiaotai_module_ai_talk_start(void)
{
    s_talk.diag.token_capacity = xiaotai_mem_has_external() ? 128U : 64U;
    ai_talk_caption_start(s_talk.config.caption_bytes);
    ai_talk_tools_start();
}

void xiaotai_ai_talk_reset(void)
{
    if (xiaotai_on_owner()) finish(XIAOTAI_AI_TALK_END_LOCAL, XIAOTAI_OK, false);
    if (s_talk.config.resume_signaling_after_link_release) {
        const uint32_t hold = atomic_exchange(&s_signaling_hold, 0U);
        const signaling_restore_t watch = restore_take(0);
        const xiaotai_link_release_state_t state = xiaotai_link_release_state(watch.link);
        if (state == XIAOTAI_LINK_RELEASE_COMPLETE || state == XIAOTAI_LINK_RELEASE_ABORTED) {
            restore_close(watch, hold);
        } else {
            restore_unwatch(watch);
            if (hold || watch.hold)
                xiaotai_eventlog_debug(SOURCE, "signaling.release_reset", watch.session, 0);
        }
    } else release_signaling();
    ai_talk_caption_reset();
    ai_talk_tools_reset();
    memset(&s_talk, 0, sizeof(s_talk));
}
