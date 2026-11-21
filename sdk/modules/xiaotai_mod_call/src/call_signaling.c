// SPDX-License-Identifier: MIT
#include "call_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

enum { POLL_MS = 80, ADMISSION_MS = 3000, RETIRE_WARN_MS = 2000, LATE_POLL_MS = 250 };
typedef enum { SIGNAL_IDLE, SIGNAL_ADMIT, SIGNAL_HELD, SIGNAL_DRAIN } signal_phase_t;

/* Call, its timers, and the platform completion are owned by the framework. */
static struct {
    uint32_t session, link, timer, hold;
    int64_t deadline;
    signal_phase_t phase;
    bool overdue;
} s_signal;

static void poll(void *ctx);

static void reset(void)
{
    if (s_signal.timer) xiaotai_timer_cancel(s_signal.timer);
    if (s_signal.hold) {
        xiaotai_link_unwatch_release(s_signal.link);
        xiaotai_platform_signaling_release_bounded(s_signal.hold, (void *)(uintptr_t)s_signal.session);
    }
    s_signal.session = s_signal.link = s_signal.timer = s_signal.hold = 0;
    s_signal.deadline = 0;
    s_signal.phase = SIGNAL_IDLE;
    s_signal.overdue = false;
}

void call_signaling_reset(void) { reset(); }

static void suspended(xiaotai_err_t result, void *ctx)
{
    const uint32_t session = (uint32_t)(uintptr_t)ctx;
    if (session != s_signal.session || !s_signal.hold) return;
    if (result == XIAOTAI_OK) xiaotai_eventlog_note("call", "signaling.paused", session, 0);
    else {
        xiaotai_eventlog_warn("call", "signaling.pause", session, result);
        if (s_signal.phase == SIGNAL_HELD) reset();
    }
}

static void schedule(uint32_t delay)
{
    const xiaotai_err_t err = xiaotai_timer_start(delay, poll, (void *)(uintptr_t)s_signal.session,
                                                &s_signal.timer);
    if (err == XIAOTAI_OK) return;
    xiaotai_eventlog_warn("call", "signaling.timer", s_signal.session, err);
    if (s_signal.phase == SIGNAL_HELD) return;
    if (s_signal.phase == SIGNAL_DRAIN) {
        /* Loss of observation cannot certify retirement. The platform keeps
         * its own bounded lease; do not restart it here or send the POST. */
        call_release_retirement(s_signal.session, false);
        xiaotai_link_unwatch_release(s_signal.link);
        s_signal.hold = 0;
    }
    reset();
}

static bool audio_flowing(uint32_t session, uint32_t link)
{
    if (!call_signaling_current(session, link)) return false;
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    if (media.generation != link || !media.active) return false;
    xiaotai_link_stats_t stats;
    if (!xiaotai_link_stats(link, &stats) || stats.state != XIAOTAI_LINK_STATE_OPEN ||
        !stats.audio.sent || !stats.audio.received) return false;
    xiaotai_media_stats_t count;
    xiaotai_media_stats(&count);
    return count.pcm_written != 0;
}

static void admit(uint32_t session)
{
    xiaotai_err_t err = XIAOTAI_ERR_TIMEOUT;
    if (xiaotai_time_us() < s_signal.deadline) {
        err = XIAOTAI_ERR_BUSY;
    }
    if (err == XIAOTAI_ERR_BUSY && audio_flowing(session, s_signal.link)) {
        err = xiaotai_link_watch_release(s_signal.link);
        if (err == XIAOTAI_OK) {
            uint32_t hold = 0;
            err = xiaotai_platform_signaling_hold_bounded(60000U, suspended, (void *)(uintptr_t)session, &hold);
            if (err == XIAOTAI_OK) {
                s_signal.hold = hold;
                s_signal.phase = SIGNAL_HELD;
                schedule(LATE_POLL_MS);
                return;
            }
            xiaotai_link_unwatch_release(s_signal.link);
        }
    }
    if (err == XIAOTAI_ERR_BUSY && xiaotai_time_us() < s_signal.deadline) {
        schedule(POLL_MS);
        return;
    }
    xiaotai_eventlog_note("call", "signaling.skipped", session, err);
    reset();
}

static void drain(uint32_t session)
{
    if (!xiaotai_platform_signaling_bounded_hold_active(s_signal.hold, (void *)(uintptr_t)session)) {
        xiaotai_eventlog_warn("call", "signaling.release_expired", session, XIAOTAI_ERR_TIMEOUT);
        reset();
        call_release_retirement(session, false);
        return;
    }
    const xiaotai_link_release_state_t state = xiaotai_link_release_state(s_signal.link);
    if (state == XIAOTAI_LINK_RELEASE_COMPLETE || state == XIAOTAI_LINK_RELEASE_ABORTED) {
        xiaotai_eventlog_note("call", "signaling.retired", session, state);
        reset();
        call_release_retirement(session, true);
        return;
    }
    if (!s_signal.overdue && xiaotai_time_us() >= s_signal.deadline) {
        s_signal.overdue = true;
        xiaotai_eventlog_warn("call", "signaling.release_timeout", session, XIAOTAI_ERR_TIMEOUT);
    }
    schedule(s_signal.overdue ? LATE_POLL_MS : POLL_MS);
}

static void poll(void *ctx)
{
    const uint32_t session = (uint32_t)(uintptr_t)ctx;
    if (!session || s_signal.session != session) return;
    s_signal.timer = 0;
    if (s_signal.phase == SIGNAL_ADMIT) admit(session);
    else if (s_signal.phase == SIGNAL_HELD) {
        if (xiaotai_platform_signaling_bounded_hold_active(s_signal.hold, (void *)(uintptr_t)session)) {
            schedule(LATE_POLL_MS);
        } else {
            xiaotai_eventlog_warn("call", "signaling.lease_expired", session, XIAOTAI_ERR_TIMEOUT);
        }
    } else if (s_signal.phase == SIGNAL_DRAIN) drain(session);
}

void call_signaling_start(uint32_t session, uint32_t link)
{
    if (!session || !link || s_signal.phase != SIGNAL_IDLE) return;
    s_signal.session = session;
    s_signal.link = link;
    s_signal.phase = SIGNAL_ADMIT;
    s_signal.deadline = xiaotai_time_us() + ADMISSION_MS * INT64_C(1000);
    schedule(POLL_MS);
}

bool call_signaling_end(uint32_t session)
{
    if (session != s_signal.session) return false;
    if (s_signal.phase != SIGNAL_HELD) {
        reset();
        return false;
    }
    if (s_signal.timer) xiaotai_timer_cancel(s_signal.timer);
    s_signal.timer = 0;
    s_signal.phase = SIGNAL_DRAIN;
    return true;
}

void call_signaling_drain(uint32_t session)
{
    if (s_signal.session != session || s_signal.phase != SIGNAL_DRAIN) return;
    s_signal.deadline = xiaotai_time_us() + RETIRE_WARN_MS * INT64_C(1000);
    drain(session);
}
