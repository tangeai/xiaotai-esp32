// SPDX-License-Identifier: MIT
#include "platform_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/clock.h"

enum { HOLD_MAX_MS = 60000, HOLD_TOKEN_MARGIN_S = 2, HOLD_CLOCK_CHECK_MS = 1000 };

static platform_hold_t *find(uint32_t id)
{
    for (size_t i = 0; i < sizeof(platform.session.hold) / sizeof(*platform.session.hold); ++i)
        if (platform.session.hold[i].id == id) return &platform.session.hold[i];
    return NULL;
}

static bool bounded_context(void)
{
    return platform.session.enabled && platform.network_up && platform.status.authorized && !platform.gate &&
        !platform.active && !platform_binding_in_transition() && !platform.session.auth_pending &&
        platform.status.generation == xiaotai_identity_generation();
}

static uint32_t token_hold_ms(void)
{
    const platform_session_t *s = &platform.session;
    if (s->expiry <= 0 || !xiaotai_clock_synced()) return 0;
    const int64_t wall = xiaotai_clock_unix_ms();
    if (wall < 0) return 0;
    const int64_t edge = s->refresh_at > 0 && s->refresh_at < s->expiry ? s->refresh_at : s->expiry;
    const int64_t remaining = edge - wall / 1000;
    if (remaining <= HOLD_TOKEN_MARGIN_S) return 0;
    if (remaining > HOLD_MAX_MS / 1000 + HOLD_TOKEN_MARGIN_S + 1) return HOLD_MAX_MS;
    const int64_t ms = (remaining - HOLD_TOKEN_MARGIN_S) * 1000 - wall % 1000;
    return ms > HOLD_MAX_MS ? HOLD_MAX_MS : (uint32_t)ms;
}

static xiaotai_err_t take_hold(xiaotai_platform_done_fn_t suspended, void *ctx, uint32_t *hold, bool startup,
                              uint32_t max_ms)
{
    if (!hold) return XIAOTAI_ERR_INVALID_ARG;
    if (!platform.objects || atomic_load(&platform.stopped)) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    platform_lock();
    if (max_ms) {
        if (!atomic_load(&platform.running) || platform.status.phase != XIAOTAI_PLATFORM_ONLINE ||
            !platform.status.signaling || !platform.session.link_live) {
            platform_unlock();
            return XIAOTAI_ERR_PLATFORM_OFFLINE;
        }
        if (platform.status.held || !bounded_context()) {
            platform_unlock();
            return XIAOTAI_ERR_BUSY;
        }
        const uint32_t room = token_hold_ms();
        if (!room) {
            platform_unlock();
            return XIAOTAI_ERR_BUSY;
        }
        if (room < max_ms) max_ms = room;
    }
    platform_hold_t *slot = find(0);
    if (!slot) { platform_unlock(); return XIAOTAI_ERR_NO_MEM; }
    do { if (++platform.session.hold_next == 0) ++platform.session.hold_next; }
    while (find(platform.session.hold_next));
    *hold = platform.session.hold_next;
    *slot = (platform_hold_t){.id = *hold, .callback = suspended, .ctx = ctx, .startup = startup};
    if (max_ms) {
        platform.session.bounded_hold = *hold;
        platform.session.bounded_hold_until = xiaotai_time_us() + (int64_t)max_ms * 1000;
    }
    platform.status.held = true;
    platform_unlock();
    platform_wake();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_signaling_hold(xiaotai_platform_done_fn_t suspended, void *ctx, uint32_t *hold)
{
    return take_hold(suspended, ctx, hold, false, 0);
}

xiaotai_err_t xiaotai_platform_signaling_hold_bounded(uint32_t max_ms,
                                                      xiaotai_platform_done_fn_t suspended, void *ctx, uint32_t *hold)
{
    if (!max_ms || max_ms > HOLD_MAX_MS) return XIAOTAI_ERR_INVALID_ARG;
    return take_hold(suspended, ctx, hold, false, max_ms);
}

xiaotai_err_t xiaotai_platform_signaling_hold_startup(xiaotai_platform_done_fn_t suspended, void *ctx, uint32_t *hold)
{
    return take_hold(suspended, ctx, hold, true, 0);
}

bool platform_holds_allow_discovery(void)
{
    for (size_t i = 0; i < sizeof(platform.session.hold) / sizeof(*platform.session.hold); ++i) {
        const platform_hold_t *slot = &platform.session.hold[i];
        if (slot->id && !slot->startup) return false;
    }
    return true;
}

void platform_holds_retry_destroy(void)
{
    platform_session_t *s = &platform.session;
    /* Keep PAUSED/last_error visible until the retained handle is safely
     * destroyed and the regular resume path reports ONLINE. */
    if (s->destroy_pending && s->destroy_failed && s->resume_link && !platform_binding_in_transition())
        s->destroy_failed = false;
}

static void drop(uint32_t hold)
{
    platform_hold_t *slot = find(hold);
    if (slot) *slot = (platform_hold_t){0};
    if (hold == platform.session.bounded_hold) {
        platform.session.bounded_hold = 0;
        platform.session.bounded_hold_until = 0;
        platform_holds_retry_destroy();
    }
    bool held = false;
    for (size_t i = 0; i < sizeof(platform.session.hold) / sizeof(*platform.session.hold); ++i)
        held |= platform.session.hold[i].id != 0;
    if (!held && platform.status.held && platform.session.resume_link) platform.session.retry_at = 0;
    platform.status.held = held;
}

void xiaotai_platform_signaling_release(uint32_t hold)
{
    if (!hold || !platform.objects || atomic_load(&platform.stopped)) return;
    platform_lock();
    drop(hold);
    platform_unlock();
    platform_wake();
}

static bool scoped_bounded(uint32_t hold, const void *ctx, bool release)
{
    if (!hold || !ctx || !platform.objects || atomic_load(&platform.stopped)) return false;
    platform_lock();
    platform_hold_t *slot = hold == platform.session.bounded_hold ? find(hold) : NULL;
    const bool matches = slot && slot->ctx == ctx;
    if (matches && release) drop(hold);
    platform_unlock();
    if (matches && release) platform_wake();
    return matches;
}

bool xiaotai_platform_signaling_bounded_hold_active(uint32_t hold, const void *ctx)
{
    return scoped_bounded(hold, ctx, false);
}

void xiaotai_platform_signaling_release_bounded(uint32_t hold, const void *ctx)
{
    (void)scoped_bounded(hold, ctx, true);
}

void platform_holds_drop_bounded(void)
{
    if (platform.session.bounded_hold) drop(platform.session.bounded_hold);
}

uint32_t platform_holds_check(void)
{
    platform_session_t *s = &platform.session;
    if (!s->bounded_hold) return XIAOTAI_WAIT_FOREVER;
    const uint32_t room = bounded_context() ? token_hold_ms() : 0;
    uint32_t wait = platform_remaining_ms(s->bounded_hold_until);
    if (!room || !wait) {
        platform_holds_drop_bounded();
        return 0;
    }
    if (room < wait) {
        s->bounded_hold_until = xiaotai_time_us() + (int64_t)room * 1000;
        wait = room;
    }
    return wait < HOLD_CLOCK_CHECK_MS ? wait : HOLD_CLOCK_CHECK_MS;
}

void platform_holds_finish(xiaotai_err_t result)
{
    for (size_t i = 0; i < sizeof(platform.session.hold) / sizeof(*platform.session.hold); ++i) {
        platform_hold_t *slot = &platform.session.hold[i];
        if (!slot->id || slot->complete) continue;
        slot->result = result;
        slot->complete = true;
    }
}

static void deliver(void *ctx)
{
    xiaotai_platform_done_fn_t callback = NULL;
    void *arg = NULL;
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    platform_lock();
    const uint32_t hold = (uint32_t)(uintptr_t)ctx;
    const bool expired = hold == platform.session.bounded_hold && !platform_holds_check();
    platform_hold_t *slot = find(hold);
    if (slot && slot->posted && slot->complete) {
        callback = slot->callback;
        arg = slot->ctx;
        result = slot->result;
        slot->callback = NULL;
    }
    platform_unlock();
    if (expired) platform_wake();
    if (callback) callback(result, arg);
}

uint32_t platform_holds_step(void)
{
    uint32_t wait = XIAOTAI_WAIT_FOREVER;
    platform_lock();
    for (size_t i = 0; i < sizeof(platform.session.hold) / sizeof(*platform.session.hold); ++i) {
        platform_hold_t *slot = &platform.session.hold[i];
        if (!slot->complete || !slot->callback || slot->posted) continue;
        if (xiaotai_run(deliver, (void *)(uintptr_t)slot->id) == XIAOTAI_OK) slot->posted = true;
        else { wait = PLATFORM_OWNER_REPOST_MS; break; }
    }
    platform_unlock();
    return wait;
}
