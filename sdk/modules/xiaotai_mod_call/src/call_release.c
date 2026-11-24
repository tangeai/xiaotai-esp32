// SPDX-License-Identifier: MIT
#include "call_internal.h"

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include <string.h>

#define SOURCE "call"
#define RELEASE_ATTEMPTS 6U
#define RELEASE_POST_DENIALS 3U
#define RELEASE_REPLY_US 10000000LL
#define RELEASE_RETRY_US 1000000LL

static struct {
    xiaotai_call_status_t *status;
    call_release_entry_t *entries;
    size_t quota;
    uint32_t timer, timer_generation, binding;
} s_release;

xiaotai_err_t call_release_init(xiaotai_call_status_t *status, size_t count)
{
    /* Finished, unconfirmed ownership must not take over the dial quota.
     * Both registries are bounded; never evict a room we still own. */
    s_release.entries = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 2U * count, sizeof(*s_release.entries));
    if (!s_release.entries) return XIAOTAI_ERR_NO_MEM;
    s_release.quota = count;
    s_release.status = status;
    return XIAOTAI_OK;
}

void call_release_counts(void)
{
    if (!s_release.status) return;
    unsigned pending = 0;
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        const call_release_entry_t *entry = &s_release.entries[i];
        if (entry->session && entry->session != s_release.status->session &&
            entry->step != CALL_RELEASE_UNCONFIRMED && !entry->reconciling) ++pending;
    }
    s_release.status->releases_pending = pending;
}

call_release_entry_t *call_release_find(uint32_t session)
{
    if (!session) return NULL;
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        if (s_release.entries[i].session == session) return &s_release.entries[i];
    }
    return NULL;
}

call_release_entry_t *call_release_slot(void)
{
    size_t active = 0;
    call_release_entry_t *empty = NULL;
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        call_release_entry_t *entry = &s_release.entries[i];
        if (!entry->session) empty = entry;
        else if (entry->step != CALL_RELEASE_UNCONFIRMED && !entry->reconciling) ++active;
    }
    return active < s_release.quota ? empty : NULL;
}

bool call_release_finished_room(const char *room)
{
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        const call_release_entry_t *entry = &s_release.entries[i];
        if (entry->session && entry->step != CALL_RELEASE_RESERVED && !strcmp(entry->room, room)) return true;
    }
    return false;
}

static void cancel_timer(void)
{
    if (s_release.timer) xiaotai_timer_cancel(s_release.timer);
    s_release.timer = s_release.timer_generation = 0;
}

void call_release_clear(uint32_t binding)
{
    cancel_timer();
    if (s_release.entries) memset(s_release.entries, 0, 2U * s_release.quota * sizeof(*s_release.entries));
    s_release.binding = binding;
    call_release_counts();
}

void call_release_reset(void)
{
    call_release_clear(0);
    xiaotai_mem_free(s_release.entries);
    memset(&s_release, 0, sizeof(s_release));
}

static bool online(void)
{
    xiaotai_platform_status_t p;
    xiaotai_platform_status(&p);
    return p.phase == XIAOTAI_PLATFORM_ONLINE && p.generation == s_release.binding;
}

static bool signaling_ready(uint32_t binding)
{
    xiaotai_platform_status_t p;
    xiaotai_platform_status(&p);
    return p.phase == XIAOTAI_PLATFORM_ONLINE && p.generation == binding &&
           p.authorized && p.signaling && !p.held;
}

static bool ready(const call_release_entry_t *entry)
{
    return entry->session && entry->step != CALL_RELEASE_RESERVED && entry->step != CALL_RELEASE_UNCONFIRMED &&
           entry->step != CALL_RELEASE_AWAIT_RETIREMENT && !entry->deferred &&
           (!entry->post_once || signaling_ready(entry->binding));
}

static void timer_fired(void *context);

static void reschedule(void)
{
    cancel_timer();
    if (!online()) return;
    int64_t next = INT64_MAX;
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        const call_release_entry_t *entry = &s_release.entries[i];
        if (!ready(entry)) continue;
        if (entry->due_us < next) next = entry->due_us;
    }
    if (next == INT64_MAX) return;
    const int64_t left = next - xiaotai_time_us();
    const uint32_t delay = left > 0 ? (uint32_t)((left + 999) / 1000) : 1U;
    s_release.timer_generation = xiaotai_generation_next();
    const xiaotai_err_t err = xiaotai_timer_start(delay, timer_fired,
        (void *)(uintptr_t)s_release.timer_generation, &s_release.timer);
    if (err != XIAOTAI_OK) {
        s_release.timer_generation = 0;
        xiaotai_eventlog_warn(SOURCE, "release.timer", 0, err);
    }
}

static void unconfirmed(call_release_entry_t *entry)
{
    entry->step = CALL_RELEASE_UNCONFIRMED;
    entry->ticket = 0;
    entry->deferred = false;
    ++s_release.status->releases_unconfirmed;
    xiaotai_eventlog_warn(SOURCE, "release.unconfirmed", entry->session, entry->attempts);
}

static void next_action(call_release_entry_t *entry, call_release_step_t step, int64_t delay)
{
    entry->ticket = 0;
    entry->deferred = false;
    if (entry->attempts >= RELEASE_ATTEMPTS) unconfirmed(entry);
    else {
        entry->step = step;
        entry->due_us = xiaotai_time_us() + delay;
    }
}

static void defer(call_release_entry_t *entry)
{
    entry->ticket = 0;
    /* Queued work may be refused before an empty-activity event reaches us. */
    entry->deferred = xiaotai_activity_current(NULL) != 0;
    entry->due_us = xiaotai_time_us();
}

static bool background(const call_release_entry_t *entry)
{
    return entry->reconciling || entry->attempts != 0;
}

static void request_reply(const xiaotai_platform_reply_t *reply, void *context)
{
    const uint32_t ticket = (uint32_t)(uintptr_t)context;
    if (!ticket || !s_release.status || !call_binding_current(reply->generation)) return;
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        call_release_entry_t *entry = &s_release.entries[i];
        if (entry->ticket != ticket || entry->binding != reply->generation) continue;
        if (reply->result == XIAOTAI_ERR_BUSY && (entry->reconciling || entry->attempts > 1U)) {
            --entry->attempts;
            defer(entry);
        } else if (xiaotai_time_us() >= entry->due_us) {
            ++s_release.status->stale_calls;
            next_action(entry, CALL_RELEASE_QUERY, 0);
        } else if (entry->step == CALL_RELEASE_POST) {
            const int32_t code = call_http_code(reply, NULL);
            if (code != XIAOTAI_PLATFORM_CODE_OK) {
                xiaotai_eventlog_warn(SOURCE, "release.reply", entry->session,
                                     reply->result == XIAOTAI_OK ? code : reply->result);
            }
            /* HTTP acknowledgment, including room-not-found, is not proof. */
            next_action(entry, CALL_RELEASE_QUERY, 0);
        } else {
            xiaotai_call_state_t phase;
            const xiaotai_err_t err = call_http_release_action(reply, entry->room, &phase);
            if (err != XIAOTAI_OK) {
                xiaotai_eventlog_warn(SOURCE, "release.check", entry->session, err);
                next_action(entry, CALL_RELEASE_QUERY, RELEASE_RETRY_US);
            } else if (phase == XIAOTAI_CALL_IDLE) {
                xiaotai_eventlog_debug(SOURCE, "release.confirmed", entry->session, entry->attempts);
                *entry = (call_release_entry_t){0};
            } else if (!entry->post_once && !call_foreground_owns_room(entry->session, entry->room)) {
                entry->phase = phase;
                next_action(entry, CALL_RELEASE_POST, RELEASE_RETRY_US);
            } else {
                next_action(entry, CALL_RELEASE_QUERY, RELEASE_RETRY_US);
            }
        }
        call_release_counts();
        reschedule();
        return;
    }
    ++s_release.status->stale_calls;
}

static void submit(call_release_entry_t *entry, const char *reason)
{
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (platform.generation != entry->binding) {
        *entry = (call_release_entry_t){0};
        return;
    }
    if (entry->post_once && !signaling_ready(entry->binding)) return;
    const bool post = entry->step == CALL_RELEASE_POST;
    if (post && call_foreground_owns_room(entry->session, entry->room)) {
        next_action(entry, CALL_RELEASE_QUERY, RELEASE_RETRY_US);
        return;
    }
    const uint32_t flags = background(entry) ? XIAOTAI_PLATFORM_BACKGROUND : 0U;
    const uint32_t ticket = xiaotai_generation_next();
    const bool cancel = post && entry->phase == XIAOTAI_CALL_OUTGOING;
    if (post && entry->post_p2p_error) reason = "p2p_error";
    bool prepared = false;
    xiaotai_err_t err = cancel ? call_hold_prepare(entry->video, &prepared) : XIAOTAI_OK;
    if (err == XIAOTAI_OK) {
        err = post ? call_submit_release(entry->phase, entry->room, reason, ticket, flags, request_reply) :
                     call_submit_room_check(ticket, flags, request_reply);
    }
    if (cancel) call_hold_after_submit(prepared, err == XIAOTAI_OK);
    if (flags && err == XIAOTAI_ERR_BUSY) {
        defer(entry);
        return;
    }
    if (err == XIAOTAI_OK) {
        ++entry->attempts;
        entry->ticket = ticket;
        entry->deferred = false;
        entry->due_us = xiaotai_time_us() + RELEASE_REPLY_US;
        if (post && entry->post_once) xiaotai_eventlog_debug(SOURCE, "signaling.hangup", entry->session, 0);
        return;
    }
    xiaotai_eventlog_warn(SOURCE, "release.submit", entry->session, err);
    if (post && entry->post_once && (err == XIAOTAI_ERR_BUSY || err == XIAOTAI_ERR_PLATFORM_OFFLINE)) {
        /* Synchronous denial did not enqueue any HTTP request. Retry only
         * this admission, never a POST that reached the platform queue. */
        if (++entry->post_denials >= RELEASE_POST_DENIALS) unconfirmed(entry);
        else entry->due_us = xiaotai_time_us() + RELEASE_RETRY_US;
        return;
    }
    if (err != XIAOTAI_ERR_PLATFORM_OFFLINE) ++entry->attempts;
    next_action(entry, CALL_RELEASE_QUERY, RELEASE_RETRY_US);
}

static void work(void)
{
    const int64_t now = xiaotai_time_us();
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        call_release_entry_t *entry = &s_release.entries[i];
        if (!ready(entry) || entry->due_us > now) continue;
        if (entry->ticket || entry->step == CALL_RELEASE_DIAL_WAIT) {
            if (entry->ticket) xiaotai_eventlog_warn(SOURCE, "release.timeout", entry->session, entry->attempts);
            next_action(entry, CALL_RELEASE_QUERY, 0);
        }
        if (entry->step != CALL_RELEASE_UNCONFIRMED) submit(entry, NULL);
    }
    call_release_counts();
}

static void timer_fired(void *context)
{
    if (s_release.timer_generation != (uint32_t)(uintptr_t)context) return;
    s_release.timer = s_release.timer_generation = 0;
    if (online()) work();
    reschedule();
}

void call_release_activity(void)
{
    if (xiaotai_activity_current(NULL)) return;
    for (size_t i = 0; i < 2U * s_release.quota; ++i) s_release.entries[i].deferred = false;
    reschedule();
}

void call_release_online(uint32_t binding)
{
    if (s_release.binding != binding) {
        call_release_clear(binding);
        return;
    }
    for (size_t i = 0; i < 2U * s_release.quota; ++i) {
        call_release_entry_t *entry = &s_release.entries[i];
        if (entry->step != CALL_RELEASE_UNCONFIRMED) continue;
        entry->attempts = 0;
        entry->reconciling = true;
        next_action(entry, CALL_RELEASE_QUERY, 0);
    }
    reschedule();
}

static void untracked_reply(const xiaotai_platform_reply_t *reply, void *context)
{
    (void)reply;
    (void)context;
}

void call_release_end(uint32_t session, uint32_t binding, xiaotai_call_state_t phase,
                      const char *room, const char *reason, bool video, bool await_retirement)
{
    call_release_entry_t *entry = call_release_find(session);
    if (!room[0] && !entry) return;
    if (!session && call_release_finished_room(room)) return;
    if (!entry) entry = call_release_slot();
    if (entry && !entry->session) {
        *entry = (call_release_entry_t){.session = session ? session : xiaotai_generation_next(), .binding = binding};
    }
    if (!entry) {
        /* A remote incoming request can arrive despite the local quota.
         * Send its immediate rejection, but document that it cannot be confirmed. */
        const xiaotai_err_t err = await_retirement ? XIAOTAI_ERR_NO_MEM :
            call_submit_release(phase, room, reason, 0, 0, untracked_reply);
        ++s_release.status->releases_unconfirmed;
        xiaotai_eventlog_warn(SOURCE, "release.capacity", session, err);
        return;
    }
    entry->phase = phase;
    entry->video = video;
    if (room[0]) {
        memcpy(entry->room, room, strlen(room) + 1U);
        entry->post_once = await_retirement;
        entry->post_p2p_error = await_retirement && reason && !strcmp(reason, "p2p_error");
        entry->step = await_retirement ? CALL_RELEASE_AWAIT_RETIREMENT : CALL_RELEASE_POST;
        if (!await_retirement) submit(entry, reason);
    } else if (entry->awaiting_dial) {
        next_action(entry, CALL_RELEASE_DIAL_WAIT, RELEASE_REPLY_US);
    } else {
        next_action(entry, CALL_RELEASE_QUERY, RELEASE_RETRY_US);
    }
    reschedule();
}

void call_release_retirement(uint32_t session, bool confirmed)
{
    call_release_entry_t *entry = call_release_find(session);
    if (!entry || entry->step != CALL_RELEASE_AWAIT_RETIREMENT || entry->binding != s_release.binding) return;
    if (confirmed) next_action(entry, CALL_RELEASE_POST, 0);
    else unconfirmed(entry);
    call_release_counts();
    reschedule();
}

void call_release_retired_dial(call_release_entry_t *entry, bool definitive, bool got_room)
{
    if (definitive) *entry = (call_release_entry_t){0};
    else if (got_room) {
        entry->phase = XIAOTAI_CALL_OUTGOING;
        if (entry->step != CALL_RELEASE_UNCONFIRMED) {
            next_action(entry, CALL_RELEASE_POST, 0);
            if (entry->step == CALL_RELEASE_POST) submit(entry, NULL);
        }
    } else {
        entry->room[0] = '\0';
        if (entry->step == CALL_RELEASE_DIAL_WAIT) next_action(entry, CALL_RELEASE_QUERY, RELEASE_RETRY_US);
    }
    call_release_counts();
    reschedule();
}
