// SPDX-License-Identifier: MIT
#include "feedback_internal.h"

#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"

#define ALL_GROUPS ((uint8_t)((1U << XIAOTAI_FEEDBACK_GROUPS_MAX) - 1U))

typedef struct {
    xiaotai_feedback_done_fn_t done;
    void *ctx;
    uint32_t id;
    xiaotai_err_t result;
} reply_t;

typedef struct {
    reply_t entries[3];
    size_t count;
} replies_t;

static struct {
    const xiaotai_feedback_player_t *player;
    fb_prompt_t playing, waiting;
    const xiaotai_feedback_state_t *loop, *blocked_loop;
    xiaotai_err_t loop_error;
    uint32_t token, timer, last_busy;
    bool wait_busy;
    bool custom_player;
    bool submitting;
    /* Other tasks see only this published view and completion mailbox. */
    fb_sound_info_t view;
    uint32_t published_token;
    bool result_pending, idle_pending;
    xiaotai_err_t pending_result;
} sound;

#define COUNT(field) do { fb_lock(); ++g_feedback.stats.field; fb_unlock(); } while (0)

static void reply_later(replies_t *replies, const fb_prompt_t *prompt, xiaotai_err_t result)
{
    if (prompt->done) replies->entries[replies->count++] = (reply_t){prompt->done, prompt->ctx, prompt->id, result};
}

static void deliver(const replies_t *replies, uint32_t synchronous_id)
{
    for (size_t i = 0; i < replies->count; ++i) {
        const reply_t *reply = &replies->entries[i];
        if (reply->id != synchronous_id || reply->result != XIAOTAI_ERR_INVALID_STATE) {
            reply->done(reply->result, reply->ctx);
        }
    }
}

static void publish(void)
{
    fb_lock();
    sound.view.playing = sound.playing.effect ? sound.playing.effect->name : NULL;
    sound.view.waiting = sound.waiting.effect ? sound.waiting.effect->name : NULL;
    sound.view.loop = sound.loop ? sound.loop->name : NULL;
    if (sound.token != sound.published_token) {
        if (sound.result_pending) ++g_feedback.stats.stale_tokens;
        sound.result_pending = false;
        sound.published_token = sound.token;
    }
    fb_unlock();
}

static bool group_enabled(uint8_t group)
{
    return !group || (group <= XIAOTAI_FEEDBACK_GROUPS_MAX && (sound.view.groups & (1U << (group - 1U))));
}

static const xiaotai_feedback_state_t *loop_winner(void)
{
    const xiaotai_feedback_state_t *winner = NULL;
    if (!sound.player || !sound.player->play) return NULL;
    fb_lock();
    for (size_t i = 0; i < g_feedback.state_count; ++i) {
        if (!fb_state_active(i)) continue;
        const xiaotai_feedback_state_t *row = fb_state_row(i);
        if (row->sound.name && *row->sound.name && group_enabled(row->sound.group) &&
            (!winner || row->priority > winner->priority)) winner = row;
    }
    fb_unlock();
    return winner;
}

static void stop_current(replies_t *replies)
{
    const uint32_t token = sound.token;
    if (!token) return;
    if (sound.playing.effect) {
        reply_later(replies, &sound.playing, XIAOTAI_ERR_INVALID_STATE);
        COUNT(cancelled);
    }
    sound.playing = (fb_prompt_t){0};
    sound.loop = NULL;
    sound.token = 0;
    publish();
    if (sound.player->stop) sound.player->stop(sound.player->ctx, token);
}

static void clear_waiting(replies_t *replies, xiaotai_err_t result)
{
    reply_later(replies, &sound.waiting, result);
    sound.waiting = (fb_prompt_t){0};
    sound.wait_busy = false;
}

static void tick(void *ctx);

static xiaotai_err_t play(const char *const *names, uint8_t count, uint16_t repeat, uint16_t gap,
                          uint32_t generation)
{
    if (!sound.player || !sound.player->play) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (sound.custom_player && !sound.timer) {
        /* Reserve delivery capacity before a custom player can keep the
         * token and later report completion from outside the owner. */
        const xiaotai_err_t err = xiaotai_timer_start(XIAOTAI_FEEDBACK_RETRY_MS, tick, NULL, &sound.timer);
        if (err != XIAOTAI_OK) return err;
    }
    const uint32_t token = xiaotai_generation_next();
    const xiaotai_feedback_play_t request = {names, count, repeat, gap, generation, token};
    /* A custom player can complete on a different task as soon as play
     * returns. Publish its token before giving control to the player. */
    sound.token = token;
    sound.submitting = true;
    publish();
    const xiaotai_err_t result = sound.player->play(sound.player->ctx, &request);
    sound.submitting = false;
    if (result != XIAOTAI_OK) {
        sound.token = 0;
        publish();
    }
    return result;
}

static xiaotai_err_t arm(uint32_t now, replies_t *replies)
{
    if (sound.timer) xiaotai_timer_cancel(sound.timer);
    sound.timer = 0;
    uint32_t delay = XIAOTAI_TIMER_DELAY_MAX_MS;
    bool needed = false;
    if (sound.waiting.effect) {
        if (sound.waiting.valid_ms) {
            const uint32_t elapsed = now - sound.waiting.requested_ms;
            delay = elapsed < sound.waiting.valid_ms ? sound.waiting.valid_ms - elapsed : 0;
            if (delay > XIAOTAI_TIMER_DELAY_MAX_MS) delay = XIAOTAI_TIMER_DELAY_MAX_MS;
            needed = true;
        }
        if (sound.wait_busy) {
            const uint32_t elapsed = now - sound.last_busy;
            const uint32_t retry = elapsed < XIAOTAI_FEEDBACK_RETRY_MS ? XIAOTAI_FEEDBACK_RETRY_MS - elapsed : 0;
            if (retry < delay) delay = retry;
            needed = true;
        }
    }
    /* The built-in player completes on the owner; custom players may have to
     * publish through a full queue. This tick drains that bounded mailbox. */
    if (sound.custom_player && (sound.token || (sound.blocked_loop && sound.loop_error == XIAOTAI_ERR_BUSY))) {
        if (delay > XIAOTAI_FEEDBACK_RETRY_MS) delay = XIAOTAI_FEEDBACK_RETRY_MS;
        needed = true;
    }
    if (needed) {
        const xiaotai_err_t err = xiaotai_timer_start(delay, tick, NULL, &sound.timer);
        if (err != XIAOTAI_OK) {
            if (sound.waiting.effect) {
                clear_waiting(replies, err);
                xiaotai_eventlog_warn("feedback", "sound.timer.failed", 0, err);
                return err;
            }
            xiaotai_eventlog_warn("feedback", "sound.timer.failed", 0, err);
        }
    }
    return XIAOTAI_OK;
}

static bool expire_waiting(replies_t *replies, uint32_t now)
{
    if (sound.waiting.effect && sound.waiting.valid_ms && now - sound.waiting.requested_ms >= sound.waiting.valid_ms) {
        COUNT(dropped_expired);
        clear_waiting(replies, XIAOTAI_ERR_TIMEOUT);
        return true;
    }
    return false;
}

static xiaotai_err_t advance(replies_t *replies, bool idle)
{
    const uint32_t now = fb_now();
    xiaotai_err_t result = expire_waiting(replies, now) ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
    if (sound.waiting.effect && !sound.playing.effect &&
        (!sound.wait_busy || idle || now - sound.last_busy >= XIAOTAI_FEEDBACK_RETRY_MS)) {
        if (sound.loop) stop_current(replies);
        const uint32_t generation = sound.waiting.generation;
        result = play(sound.waiting.names, sound.waiting.count, 0, 0, generation);
        if (result == XIAOTAI_OK) {
            sound.playing = sound.waiting;
            sound.waiting = (fb_prompt_t){0};
            sound.wait_busy = false;
        } else if (result == XIAOTAI_ERR_BUSY && (sound.waiting.effect->sound.flags & XIAOTAI_FEEDBACK_WAIT)) {
            sound.last_busy = now;
            sound.wait_busy = true;
            result = XIAOTAI_OK;
        } else {
            if (result == XIAOTAI_ERR_BUSY) {
                COUNT(dropped_busy);
                result = XIAOTAI_ERR_INVALID_STATE;
            } else if (result == XIAOTAI_ERR_INVALID_STATE && generation &&
                       (sound.custom_player || fb_default_player_stale(sound.player, generation))) {
                COUNT(dropped_stale);
            } else {
                COUNT(player_errors);
            }
            clear_waiting(replies, result);
        }
    }
    const xiaotai_feedback_state_t *winner = loop_winner();
    if (winner != sound.blocked_loop || (idle && (sound.loop_error == XIAOTAI_ERR_BUSY ||
                                                  sound.loop_error == XIAOTAI_ERR_INVALID_STATE))) sound.blocked_loop = NULL;
    if (sound.loop && winner != sound.loop) stop_current(replies);
    if (!sound.token && !sound.waiting.effect && winner && winner != sound.blocked_loop) {
        const char *name = winner->sound.name;
        sound.loop_error = play(&name, 1, XIAOTAI_FEEDBACK_FOREVER, winner->sound.gap_ms, 0);
        if (sound.loop_error == XIAOTAI_OK) {
            sound.loop = winner;
            COUNT(loops);
        } else {
            sound.blocked_loop = winner;
            if (sound.loop_error != XIAOTAI_ERR_BUSY) COUNT(player_errors);
        }
    }
    const xiaotai_err_t timer_error = arm(now, replies);
    publish();
    return result == XIAOTAI_OK ? timer_error : result;
}

static void process_result(void *ctx)
{
    (void)ctx;
    replies_t replies = {0};
    fb_lock();
    const bool pending = sound.result_pending;
    const xiaotai_err_t result = sound.pending_result;
    sound.result_pending = false;
    fb_unlock();
    if (!pending) return;
    if (sound.playing.effect) {
        if (result == XIAOTAI_OK) COUNT(played);
        else if (result == XIAOTAI_ERR_INVALID_STATE) COUNT(cancelled);
        else COUNT(player_errors);
        reply_later(&replies, &sound.playing, result);
    } else if (sound.loop && result != XIAOTAI_OK) {
        sound.blocked_loop = sound.loop;
        sound.loop_error = result;
        if (result != XIAOTAI_ERR_INVALID_STATE) COUNT(player_errors);
    }
    sound.playing = (fb_prompt_t){0};
    sound.loop = NULL;
    sound.token = 0;
    (void)advance(&replies, false);
    deliver(&replies, 0);
}

static void player_idle(void *ctx);

static bool take_idle(void)
{
    fb_lock();
    const bool idle = sound.idle_pending;
    sound.idle_pending = false;
    fb_unlock();
    return idle;
}

static void tick(void *ctx)
{
    (void)ctx;
    sound.timer = 0;
    process_result(NULL);
    if (take_idle()) player_idle(NULL);
    else fb_sound_state_changed();
}

void fb_sound_state_changed(void)
{
    fb_lock();
    const bool started = g_feedback.started;
    fb_unlock();
    if (!started) return;
    replies_t replies = {0};
    (void)advance(&replies, false);
    deliver(&replies, 0);
}

xiaotai_err_t fb_sound_fire(const fb_prompt_t *prompt, bool force, bool queued)
{
    replies_t replies = {0};
    const bool before_expired = expire_waiting(&replies, fb_now());
    xiaotai_err_t result = XIAOTAI_OK;
    bool advanced = false;
    if (!prompt->count) {
        reply_later(&replies, prompt, XIAOTAI_OK);
    } else if (!force && !group_enabled(prompt->effect->sound.group)) {
        COUNT(dropped_group);
        reply_later(&replies, prompt, result = XIAOTAI_ERR_INVALID_STATE);
    } else if ((sound.playing.effect && sound.playing.effect->sound.rank > prompt->effect->sound.rank) ||
               (sound.waiting.effect && sound.waiting.effect->sound.rank > prompt->effect->sound.rank)) {
        COUNT(dropped_rank);
        reply_later(&replies, prompt, result = XIAOTAI_ERR_INVALID_STATE);
    } else if (prompt->valid_ms && fb_now() - prompt->requested_ms >= prompt->valid_ms) {
        COUNT(dropped_expired);
        reply_later(&replies, prompt, result = XIAOTAI_ERR_TIMEOUT);
    } else if (!sound.player || !sound.player->play) {
        reply_later(&replies, prompt, result = XIAOTAI_ERR_NOT_SUPPORTED);
    } else {
        if (sound.waiting.effect) {
            COUNT(replaced);
            clear_waiting(&replies, XIAOTAI_ERR_INVALID_STATE);
        }
        if (sound.playing.effect && (prompt->effect->sound.flags & XIAOTAI_FEEDBACK_INTERRUPT)) stop_current(&replies);
        sound.waiting = *prompt;
        result = advance(&replies, false);
        advanced = true;
        /* A waiting request can fail while the timer is being armed. */
        for (size_t i = 0; i < replies.count; ++i) {
            if (replies.entries[i].id == prompt->id) result = replies.entries[i].result;
        }
    }
    if (before_expired && !advanced) (void)advance(&replies, false);
    deliver(&replies, queued ? 0U : prompt->id);
    return !queued && prompt->done && result != XIAOTAI_ERR_INVALID_STATE ? XIAOTAI_OK : result;
}

void xiaotai_feedback_player_done(uint32_t token, xiaotai_err_t result)
{
    if (!g_feedback.ready) return;
    fb_lock();
    const bool current = token && token == sound.published_token && !sound.result_pending;
    if (current) {
        sound.result_pending = true;
        sound.pending_result = result;
    } else {
        ++g_feedback.stats.stale_tokens;
    }
    fb_unlock();
    if (!current) return;
    if (xiaotai_on_owner() && !sound.submitting) process_result(NULL);
    else (void)xiaotai_run(process_result, NULL); /* custom-player timer also drains the mailbox */
}

static void player_idle(void *ctx)
{
    (void)ctx;
    replies_t replies = {0};
    (void)advance(&replies, true);
    deliver(&replies, 0);
}

static void queued_idle(void *ctx)
{
    if (take_idle()) player_idle(ctx);
}

void xiaotai_feedback_player_idle(void)
{
    if (!g_feedback.ready) return;
    if (xiaotai_on_owner()) player_idle(NULL);
    else {
        fb_lock();
        const bool pending = sound.idle_pending;
        sound.idle_pending = true;
        fb_unlock();
        if (!pending) (void)xiaotai_run(queued_idle, NULL);
    }
}

static bool matches(const fb_prompt_t *p, const char *effect, uint8_t group, bool by_group)
{
    return p->effect && (by_group ? p->effect->sound.group == group : !effect || !strcmp(p->effect->name, effect));
}

static void cancel(const char *effect, uint8_t group, bool by_group, bool stop, bool stop_loop)
{
    replies_t replies = {0};
    if (matches(&sound.waiting, effect, group, by_group)) {
        COUNT(cancelled);
        clear_waiting(&replies, XIAOTAI_ERR_INVALID_STATE);
    }
    if ((stop && matches(&sound.playing, effect, group, by_group)) ||
        (stop_loop && sound.loop && sound.loop->sound.group == group)) stop_current(&replies);
    (void)advance(&replies, false);
    deliver(&replies, 0);
}

static void cancel_waiting(void *ctx) { cancel(ctx, 0, false, false, false); }
static void cancel_playing(void *ctx) { cancel(ctx, 0, false, true, false); }
static void cancel_group(void *ctx)
{
    const uintptr_t request = (uintptr_t)ctx;
    cancel(NULL, (uint8_t)(request >> 1U), true, request & 1U, false);
}

static xiaotai_err_t on_owner(xiaotai_timer_fn_t fn, void *ctx)
{
    if (!g_feedback.ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!xiaotai_on_owner()) return xiaotai_run(fn, ctx);
    fn(ctx);
    return XIAOTAI_OK;
}

static xiaotai_err_t submit_cancel(xiaotai_timer_fn_t fn, void *ctx)
{
    if (!g_feedback.ready) return XIAOTAI_ERR_INVALID_STATE;
    const bool owner = xiaotai_on_owner();
    fb_lock();
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_STATE;
    if (g_feedback.started) err = fn && !owner ? xiaotai_run(fn, ctx) : XIAOTAI_OK;
    fb_unlock();
    if (err == XIAOTAI_OK && owner && fn) fn(ctx);
    return err;
}

xiaotai_err_t xiaotai_feedback_cancel(const char *effect, bool stop)
{
    if (effect && !*effect) return XIAOTAI_ERR_INVALID_ARG;
    const char *registered = effect ? fb_effect_name(effect) : NULL;
    return submit_cancel(effect && !registered ? NULL : stop ? cancel_playing : cancel_waiting, (void *)registered);
}

xiaotai_err_t xiaotai_feedback_cancel_group(uint8_t group, bool stop)
{
    if (group > XIAOTAI_FEEDBACK_GROUPS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    return submit_cancel(cancel_group, (void *)(((uintptr_t)group << 1U) | stop));
}

static void enable_group(void *ctx)
{
    const uintptr_t request = (uintptr_t)ctx;
    const uint8_t group = (uint8_t)(request >> 1U);
    const uint8_t bit = (uint8_t)(1U << (group - 1U));
    const bool enabled = (request & 1U) != 0;
    fb_lock();
    const bool changed = enabled != ((sound.view.groups & bit) != 0);
    if (enabled) sound.view.groups |= bit;
    else sound.view.groups &= (uint8_t)~bit;
    fb_unlock();
    if (!changed) return;
    if (enabled) fb_sound_state_changed();
    else cancel(NULL, group, true, true, true);
}

xiaotai_err_t xiaotai_feedback_group_enable(uint8_t group, bool enabled)
{
    if (!group || group > XIAOTAI_FEEDBACK_GROUPS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    return on_owner(enable_group, (void *)(((uintptr_t)group << 1U) | enabled));
}

bool xiaotai_feedback_group_enabled(uint8_t group)
{
    if (!g_feedback.ready) return false;
    fb_lock();
    const bool enabled = group_enabled(group);
    fb_unlock();
    return enabled;
}

void fb_sound_info(fb_sound_info_t *out)
{
    *out = (fb_sound_info_t){0};
    if (!g_feedback.ready) return;
    fb_lock();
    *out = sound.view;
    fb_unlock();
}

bool xiaotai_feedback_sound_pending(void)
{
    fb_sound_info_t info;
    fb_sound_info(&info);
    return info.playing || info.waiting;
}

void xiaotai_feedback_set_player(const xiaotai_feedback_player_t *player)
{
    if (!g_feedback.started) {
        sound.player = player;
        sound.custom_player = player != NULL;
    }
}

void fb_sound_init(void) { sound.view.groups = ALL_GROUPS; }

void fb_sound_install_default(const xiaotai_feedback_player_t *player)
{
    sound.player = player;
    sound.custom_player = false;
}

void fb_sound_reset(void)
{
    /* Framework and player callbacks must be quiesced before host reset. */
    if (xiaotai_on_owner()) {
        replies_t replies = {0};
        xiaotai_timer_cancel(sound.timer);
        if (sound.waiting.effect) clear_waiting(&replies, XIAOTAI_ERR_INVALID_STATE);
        stop_current(&replies);
        deliver(&replies, 0);
    }
    memset(&sound, 0, sizeof(sound));
    fb_default_player_reset();
}
