// SPDX-License-Identifier: MIT
#include <string.h>
#include "platform_internal.h"
#include "guest.h"
#include "signal_pool.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"

#define STACK_SAMPLE_MS 1000U

/* Do not recycle queued owner jobs across the host's engine reset. */
static uint32_t s_handoff_seq;

uint32_t platform_remaining_ms(int64_t deadline_us)
{
    const int64_t remaining = deadline_us - xiaotai_time_us();
    if (remaining <= 0) return 0;
    const int64_t ms = (remaining + 999) / 1000;
    return ms >= UINT32_MAX ? UINT32_MAX - 1U : (uint32_t)ms;
}

static bool background_busy(const platform_slot_t *slot)
{
    return (slot->flags & XIAOTAI_PLATFORM_BACKGROUND) && platform.config.defer_background_while_active &&
        platform_active_now();
}

static xiaotai_err_t slot_check(const platform_slot_t *slot)
{
    if (slot->cancelled) return XIAOTAI_ERR_PLATFORM_CANCELLED;
    if (platform_slot_stale(slot)) return XIAOTAI_ERR_PLATFORM_STALE;
    /* A hold postpones an unsent profile, without invalidating a reply from
     * HTTP that had already started before the hold. */
    if (platform_profile_hold_blocks(slot->profile_id)) return XIAOTAI_ERR_PLATFORM_GATED;
    if (!platform_remaining_ms(slot->deadline_us)) return XIAOTAI_ERR_TIMEOUT;
    if (background_busy(slot)) return XIAOTAI_ERR_BUSY;
    return XIAOTAI_OK;
}

static void owner_preflight(void *ctx)
{
    if (!platform.objects) return;
    platform_lock();
    if ((uint32_t)(uintptr_t)ctx != platform.handoff || atomic_load(&platform.stopped)) {
        platform_unlock();
        return;
    }
    platform.handoff_running = true;
    platform_slot_t *slot = platform.handoff_slot;
    const uint32_t observed = platform.resume_revision;
    xiaotai_err_t err = slot_check(slot);
    platform_unlock();
    bool deferred = false;
    if (err == XIAOTAI_OK && platform.preflight) {
        err = platform.preflight(platform.preflight_ctx);
        deferred = err == XIAOTAI_ERR_BUSY;
    }
    platform_lock();
    const xiaotai_err_t latest = slot_check(slot);
    if (latest != XIAOTAI_OK) {
        err = latest;
        deferred = false;
    }
    platform.preflight_result = err;
    platform.preflight_deferred = deferred;
    platform.preflight_resume_at = observed;
    platform.handoff_running = false;
    platform.handoff_done = true;
    platform_unlock();
    platform_wake();
}

static void result_hide_body(platform_result_t *result, xiaotai_err_t err)
{
    result->reply.result = result->transfer.result = err;
    result->transfer.receive_fault = XIAOTAI_PLATFORM_RECEIVE_FAULT_NONE;
    result->reply.body = NULL;
    result->reply.size = 0;
    result->reply.code = XIAOTAI_PLATFORM_NO_CODE;
}

static void owner_reply(void *ctx)
{
    if (!platform.objects) return;
    platform_lock();
    if ((uint32_t)(uintptr_t)ctx != platform.handoff || atomic_load(&platform.stopped)) {
        platform_unlock();
        return;
    }
    platform.handoff_running = true;
    platform_slot_t *slot = platform.handoff_slot;
    platform_result_t *result = platform.handoff_reply;
    if (result->reply.result != XIAOTAI_ERR_PLATFORM_CANCELLED && platform_slot_stale(slot)) {
        result_hide_body(result, XIAOTAI_ERR_PLATFORM_STALE);
    }
    const bool accepted = result->reply.result == XIAOTAI_OK;
    const int status = result->reply.http_status;
    const bool live = result->reply.result != XIAOTAI_ERR_PLATFORM_STALE && result->reply.result != XIAOTAI_ERR_PLATFORM_CANCELLED;
    void (*auth_failed)(void) = live && status == 401 && slot->auth == XIAOTAI_PLATFORM_AUTH_SESSION &&
        platform.config.auth_failure != XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE ?
        platform.reply_hooks.auth_failed : NULL;
    const bool lost = accepted && !slot->is_transfer && !slot->profile_id && status == 410 &&
        result->reply.code == XIAOTAI_PLATFORM_CODE_UNBOUND;
    void (*binding_lost)(uint32_t) = lost ? platform.reply_hooks.binding_lost : NULL;
    platform_unlock();
    if (binding_lost) binding_lost(slot->generation);
    if (auth_failed) auth_failed();
    if (slot->is_transfer) slot->handler.transfer.stream.done(slot->handler.transfer.stream.ctx, &result->transfer);
    else slot->handler.reply.fn(&result->reply, slot->handler.reply.ctx);
    platform_lock();
    platform.handoff_running = false;
    platform.handoff_done = true;
    platform_unlock();
    platform_wake();
}

static uint32_t handoff_begin(platform_slot_t *slot, platform_result_t *result)
{
    uint32_t token = ++s_handoff_seq;
    if (!token) token = ++s_handoff_seq;
    platform_lock();
    platform.handoff = token;
    platform.handoff_done = platform.handoff_running = false;
    platform.handoff_slot = slot;
    platform.handoff_reply = result;
    platform_unlock();
    return token;
}

/* Before a preflight starts, an expired/cancelled request may invalidate the
 * queued job while leaving its token harmless in the framework queue. An
 * executing callback still owns the slot. A reply must reach the owner; no
 * I/O, network retry or next request goes past the retained reply. */
static xiaotai_err_t handoff_wait(platform_slot_t *slot, platform_result_t *result)
{
    const uint32_t token = handoff_begin(slot, result);
    bool posted = false;
    xiaotai_err_t err = XIAOTAI_OK;
    for (;;) {
        platform_lock();
        if (platform.handoff_done) { platform_unlock(); break; }
        const bool entered = platform.handoff_running;
        if (!entered && atomic_load(&platform.stopped)) err = XIAOTAI_ERR_INVALID_STATE;
        if (!entered && !result && err == XIAOTAI_OK) err = slot_check(slot);
        if (err != XIAOTAI_OK) platform.handoff = 0;
        platform_unlock();
        if (err != XIAOTAI_OK) return err;
        if (!posted) {
            posted = xiaotai_run(result ? owner_reply : owner_preflight, (void *)(uintptr_t)token) == XIAOTAI_OK;
            if (!posted && result) {
                platform_lock();
                ++platform.stats.reply_post_retries;
                platform_unlock();
            }
        }
        uint32_t wait = posted ? XIAOTAI_WAIT_FOREVER : PLATFORM_OWNER_REPOST_MS;
        if (!result && !entered) {
            const uint32_t remaining = platform_remaining_ms(slot->deadline_us);
            if (remaining < wait) wait = remaining;
        }
        (void)xiaotai_signal_take(&platform.wake, wait);
    }
    platform_lock();
    platform.handoff = 0;
    platform_unlock();
    return err;
}

static xiaotai_err_t preflight(platform_slot_t *slot)
{
    platform_lock();
    xiaotai_err_t err = slot_check(slot);
    platform_unlock();
    if (err != XIAOTAI_OK || (!platform.preflight &&
        !(platform.config.defer_background_while_active && (slot->flags & XIAOTAI_PLATFORM_BACKGROUND)))) return err;
    for (;;) {
        err = handoff_wait(slot, NULL);
        if (err != XIAOTAI_OK) return err;
        platform_lock();
        const bool deferred = platform.preflight_deferred;
        const uint32_t then = platform.preflight_resume_at;
        err = platform.preflight_result;
        platform_unlock();
        if (!deferred) return err;
        for (;;) {
            platform_lock();
            err = slot_check(slot);
            const bool resumed = platform.resume_revision != then;
            platform_unlock();
            if (atomic_load(&platform.stopped)) return XIAOTAI_ERR_INVALID_STATE;
            if (err != XIAOTAI_OK) return err;
            if (resumed) break;
            (void)xiaotai_signal_take(&platform.wake, platform_remaining_ms(slot->deadline_us));
        }
    }
}

static void close_idle(bool force)
{
    platform_lock();
    if (force || platform.connection_close || (platform.gate && !platform.active) ||
        (platform.keep_until && !platform_remaining_ms(platform.keep_until)) ||
        (platform.client && !xiaotai_http_idle_connection(platform.client))) {
        xiaotai_http_t *client = platform.client;
        platform.client = NULL;
        platform.keep_until = 0;
        platform.connection_close = false;
        const uint32_t ticket = platform.gate;
        platform_unlock();
        xiaotai_http_destroy(client);
        platform_lock();
        if (ticket && ticket == platform.gate && !platform.active) platform.gate_closed = ticket;
    }
    platform_unlock();
}

static void run_slot(platform_slot_t *slot)
{
    platform_result_t result = {.reply = {.generation = slot->generation, .code = XIAOTAI_PLATFORM_NO_CODE},
                                .transfer = {.generation = slot->generation}};
    const xiaotai_err_t allowed = preflight(slot);
    if (allowed == XIAOTAI_OK) platform_http_run(slot, &result);
    else result_hide_body(&result, allowed);
    platform_lock();
    if (slot->cancelled) result_hide_body(&result, XIAOTAI_ERR_PLATFORM_CANCELLED);
    else if (platform_slot_stale(slot)) result_hide_body(&result, XIAOTAI_ERR_PLATFORM_STALE);
    slot->state = PLATFORM_REPLY;
    const bool close = platform.gate || platform.connection_close ||
        (!platform.config.idle_keep_ms && (!platform.queue_count || (slot->flags & XIAOTAI_PLATFORM_RELEASE)));
    platform_unlock();
    close_idle(close);
    (void)handoff_wait(slot, &result);
    platform_lock();
    if (!slot->is_transfer) ++platform.stats.replies;
    if (result.reply.result != XIAOTAI_OK) ++platform.stats.request_failures;
    if (result.reply.result == XIAOTAI_ERR_PLATFORM_STALE) ++platform.stats.requests_stale;
    if (result.reply.result == XIAOTAI_ERR_BUSY) ++platform.stats.requests_busy;
    if (result.reply.result == XIAOTAI_ERR_PLATFORM_CANCELLED) ++platform.stats.transfers_cancelled;
    if (result.temporary_body) {
        xiaotai_identity_wipe(result.temporary_body, result.temporary_capacity);
        xiaotai_mem_free(result.temporary_body);
    } else if (!slot->is_transfer && platform.response) {
        xiaotai_identity_wipe(platform.response, platform.response_limit + 1U);
    }
    platform_slot_release(slot);
    platform_unlock();
}

void platform_worker(void *ctx)
{
    (void)ctx;
    int64_t sample_at = 0;
    while (!atomic_load(&platform.stopped)) {
        platform_slot_t *slot = NULL;
        close_idle(false);
        platform_idle_flush();
        platform_events_flush();
        const uint32_t signaling_wait = platform_signaling_step();
        const uint32_t messages_wait = platform_signals_step();
        const uint32_t guest_wait = platform_guest_step();
        const uint32_t binding_wait = platform_binding_step();
        uint32_t wait = platform_session_step();
        const uint32_t profile_wait = platform_profile_step();
        if (signaling_wait < wait) wait = signaling_wait;
        if (messages_wait < wait) wait = messages_wait;
        if (guest_wait < wait) wait = guest_wait;
        if (binding_wait < wait) wait = binding_wait;
        if (profile_wait < wait) wait = profile_wait;
        const uint32_t hold_wait = platform_holds_step();
        if (hold_wait < wait) wait = hold_wait;
        platform_lock();
        /* Admission can race the session step. Recheck before dequeuing an
         * exchange that could prevent this worker from resuming signaling. */
        const uint32_t bounded_wait = platform_holds_check();
        if (bounded_wait < wait) wait = bounded_wait;
        const size_t next = bounded_wait ? platform_binding_queue_index() : SIZE_MAX;
        if (next != SIZE_MAX) {
            slot = &platform.slots[platform.pending[next]];
            --platform.queue_count;
            memmove(platform.pending + next, platform.pending + next + 1U, platform.queue_count - next);
            slot->state = PLATFORM_RUNNING;
        }
        if (platform.keep_until) {
            const uint32_t keep = platform_remaining_ms(platform.keep_until);
            if (keep < wait) wait = keep;
        }
        if ((platform.idle_pending || platform.event_pending) && wait > PLATFORM_OWNER_REPOST_MS)
            wait = PLATFORM_OWNER_REPOST_MS;
        platform_unlock();
        if (slot) run_slot(slot);
        const int64_t now = xiaotai_time_us();
        if (now >= sample_at || (!slot && wait > STACK_SAMPLE_MS)) {
            /* Sample this worker after its work without task enumeration or
             * an extra wake-up when it would otherwise sleep. */
            const uint32_t free_bytes = xiaotai_task_current_stack_free();
            platform_lock();
            platform.stats.worker_stack_free = free_bytes;
            platform_unlock();
            sample_at = now + (int64_t)STACK_SAMPLE_MS * 1000;
        }
        if (!slot) (void)xiaotai_signal_take(&platform.wake, wait);
    }
    platform_session_shutdown();
    if (platform_guest_shutdown() != XIAOTAI_OK) platform.session.shutdown_failed = true;
    platform_signaling_reset();
    close_idle(true);
    xiaotai_signal_give(&platform.exited);
    xiaotai_task_exit();
}
