// SPDX-License-Identifier: MIT
#include "platform_internal.h"
#include "signal_pool.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/log.h"

/* A queued fence has no borrowed platform pointer and may outlive reset. */
static atomic_uint owner_fence;
static uint32_t next_fence;

static void owner_finished(void *ctx)
{
    uint32_t ticket = (uint32_t)(uintptr_t)ctx;
    (void)atomic_compare_exchange_strong(&owner_fence, &ticket, 0);
}

static void finish_unbound(void)
{
    platform.binding = (platform_binding_t){0};
    platform.status.phase = XIAOTAI_PLATFORM_UNBOUND;
    platform.status.verifying = false;
    platform.status.last_error = XIAOTAI_OK;
    platform_event_publish(PLATFORM_EVENT_BINDING, 1);
}

static void report(bool confirmed, int32_t ack_id, uint32_t generation)
{
    if (!atomic_load(&platform.running) || atomic_load(&platform.stopped)) return;
    platform_lock();
    platform_binding_t *b = &platform.binding;
    const uint32_t current = xiaotai_identity_generation();
    const bool prior = b->phase != PLATFORM_BIND_NONE && generation == b->generation && current == b->revised_generation;
    if (!atomic_load(&platform.stopped) && platform.session.enabled && platform.status.discovered &&
        platform.status.phase != XIAOTAI_PLATFORM_UNBOUND && xiaotai_identity_state() == XIAOTAI_IDENTITY_VALID &&
        (!generation || generation == current || (confirmed && prior))) {
        ++platform.stats.unbound_reports;
        if (b->phase == PLATFORM_BIND_NONE) {
            const bool was_live = platform.status.authorized || platform.status.signaling || platform.session.link_live;
            *b = (platform_binding_t){.phase = was_live ? PLATFORM_BIND_QUIESCE : PLATFORM_BIND_CUT,
                .generation = current};
            platform.status.generation = b->revised_generation = xiaotai_identity_generation_advance();
            atomic_store(&platform.session.receive_generation, 0);
            platform.status.authorized = platform.status.signaling = platform.status.profile_ready = false;
            platform_profile_offline();
            platform_event_cancel_online();
            if (was_live) platform.status.phase = XIAOTAI_PLATFORM_CONNECTING;
            platform.status.verifying = !confirmed;
            platform.connection_close = true;
        }
        if (confirmed) {
            if (prior && (b->phase == PLATFORM_BIND_DRAIN || b->phase == PLATFORM_BIND_VERIFY)) ack_id = 0;
            if (b->phase == PLATFORM_BIND_VERIFY) {
                b->phase = PLATFORM_BIND_CUT;
                atomic_store(&platform.session.receive_generation, 0);
                platform.status.authorized = platform.status.signaling = platform.status.profile_ready = false;
                platform_profile_offline();
                platform.connection_close = true;
            }
            b->confirmed = true;
            platform.status.verifying = false;
            if (!b->ack_id && ack_id > 0 && b->phase != PLATFORM_BIND_DRAIN) b->ack_id = ack_id;
        }
        if (platform_binding_in_transition()) {
            if (platform.session.http) xiaotai_http_cancel(platform.session.http);
            else if (platform.io && platform.client) xiaotai_http_cancel(platform.client);
        }
    }
    platform_unlock();
    platform_wake();
}

void xiaotai_platform_report_unbound(bool confirmed) { report(confirmed, 0, 0); }
void platform_binding_request_lost(uint32_t generation) { report(true, 0, generation); }

void platform_binding_signal(bool confirmed, int32_t ack_id, uint32_t generation, void *ctx)
{
    (void)ctx;
    report(confirmed, ack_id, generation);
}

bool platform_binding_prior_generation(uint32_t generation)
{
    if (!atomic_load(&platform.running) || atomic_load(&platform.stopped)) return false;
    platform_lock();
    const platform_binding_t *b = &platform.binding;
    const bool prior = b->phase != PLATFORM_BIND_NONE && generation == b->generation &&
        xiaotai_identity_generation() == b->revised_generation;
    platform_unlock();
    return prior;
}

bool platform_binding_in_transition(void)
{
    return platform.binding.phase != PLATFORM_BIND_NONE && platform.binding.phase != PLATFORM_BIND_VERIFY;
}

void platform_binding_login_unbound(void)
{
    ++platform.stats.unbound_reports;
    if (platform.binding.phase == PLATFORM_BIND_NONE) platform.status.generation = xiaotai_identity_generation_advance();
    finish_unbound();
}

void platform_binding_online(void)
{
    if (platform.binding.phase == PLATFORM_BIND_VERIFY) {
        platform.binding = (platform_binding_t){0};
        platform.status.verifying = false;
    }
}

size_t platform_binding_queue_index(void)
{
    if (!platform.queue_count) return SIZE_MAX;
    if (!platform_binding_in_transition()) return 0;
    if (platform.binding.phase == PLATFORM_BIND_DRAIN) {
        const uint32_t generation = xiaotai_identity_generation();
        for (size_t i = 0; i < platform.queue_count; ++i)
            if (platform.slots[platform.pending[i]].generation != generation) return i;
    }
    return SIZE_MAX;
}

static bool owner_has_quiesced(platform_binding_t *b)
{
    if (!b->offline_posted) {
        if (!platform_event_binding_offline()) return false;
        b->offline_posted = true;
    }
    if (!b->fence_posted) {
        if (!b->fence) {
            if (++next_fence == 0) ++next_fence;
            b->fence = next_fence;
            atomic_store(&owner_fence, b->fence);
        }
        if (xiaotai_run(owner_finished, (void *)(uintptr_t)b->fence) != XIAOTAI_OK) {
            ++platform.stats.event_post_retries;
            return false;
        }
        b->fence_posted = true;
    }
    return atomic_load(&owner_fence) == 0;
}

uint32_t platform_binding_step(void)
{
    platform_lock();
    platform_binding_t *b = &platform.binding;
    platform_session_t *s = &platform.session;
    uint32_t wait = XIAOTAI_WAIT_FOREVER;
    if (b->phase == PLATFORM_BIND_QUIESCE) {
        if (owner_has_quiesced(b)) b->phase = PLATFORM_BIND_CUT;
        else wait = PLATFORM_OWNER_REPOST_MS;
    }
    if (b->phase == PLATFORM_BIND_WAIT_GATE && !platform.gate) {
        /* Releasing maintenance gives only the blocked unbind one more attempt. */
        b->phase = PLATFORM_BIND_CUT;
        s->destroy_failed = false;
    }
    if (b->phase == PLATFORM_BIND_CUT && !s->destroy_failed) {
        if (platform_signals_binding_pending()) {
            platform_unlock();
            return PLATFORM_OWNER_REPOST_MS;
        }
        if (b->ack_id > 0 && platform.config.unbind_puback_wait_ms &&
            !platform_signaling_unbind_published(b->ack_id)) {
            if (!b->ack_due) b->ack_due = xiaotai_time_us() + (int64_t)platform.config.unbind_puback_wait_ms * 1000;
            wait = platform_remaining_ms(b->ack_due);
            if (wait) { platform_unlock(); return wait; }
            XIAOTAI_LOGW("platform", "unbind ACK PUBACK not confirmed");
        }
        const bool owned = s->link_live || s->destroy_pending;
        const bool was_gated = platform.gate != 0;
        platform_unlock();
        const xiaotai_err_t err = owned ? s->link.destroy(s->link.ctx) : XIAOTAI_OK;
        platform_lock();
        if (err == XIAOTAI_OK) {
            platform_session_binding_clean();
            b->phase = PLATFORM_BIND_DRAIN;
        } else {
            s->destroy_failed = true;
            platform.status.phase = XIAOTAI_PLATFORM_PAUSED;
            platform.status.last_error = err;
            platform_event_publish(PLATFORM_EVENT_PAUSED, err);
            platform_holds_finish(err);
            wait = XIAOTAI_WAIT_FOREVER;
            if (b->confirmed && (was_gated || platform.gate)) {
                b->phase = PLATFORM_BIND_WAIT_GATE;
                /* Opening maintenance wakes the worker. If it opened during
                 * stop, the earlier wake may already have been consumed. */
                if (!platform.gate) wait = 0;
            }
        }
    }
    if (b->phase == PLATFORM_BIND_DRAIN) {
        if (platform_binding_queue_index() == SIZE_MAX) {
            const uint32_t current = xiaotai_identity_generation();
            if (current != b->revised_generation) {
                /* A separately saved identity must not inherit a verdict
                 * made for the released generation. */
                *b = (platform_binding_t){0};
                platform.status.generation = current;
                platform.status.phase = XIAOTAI_PLATFORM_STOPPED;
                platform.status.verifying = false;
                platform.status.last_error = XIAOTAI_OK;
            } else if (b->confirmed) finish_unbound();
            else {
                b->phase = PLATFORM_BIND_VERIFY;
                b->ack_id = 0;
                b->ack_due = 0;
                platform.status.phase = platform.network_up ? XIAOTAI_PLATFORM_LOGIN : XIAOTAI_PLATFORM_STOPPED;
            }
            platform_holds_finish(XIAOTAI_OK);
        }
        wait = 0;
    }
    platform_unlock();
    return wait;
}
