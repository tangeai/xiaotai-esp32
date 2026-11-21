// SPDX-License-Identifier: MIT
#include "interaction_internal.h"

#include <stdatomic.h>

static atomic_bool s_running;
#if XIAOTAI_INTERACTION_TASK
static xiaotai_task_t s_task;
static xiaotai_signal_t s_exited;
#else
static uint32_t s_timer, s_due;
#endif

static uint32_t now_ms(void) { return (uint32_t)(xiaotai_time_us() / 1000); }

uint32_t interaction_next_due(uint32_t due, uint32_t now)
{
    return (now - due > XIAOTAI_INPUT_POLL_MS ? now : due) + XIAOTAI_INPUT_POLL_MS;
}

static uint32_t wait_ms(uint32_t due, uint32_t now)
{
    const uint32_t delay = due - now;
    return delay <= INT32_MAX ? delay : 0;
}

void interaction_poll(uint32_t now)
{
    if (!interaction_lock()) return;
    interaction_flush_rejections();
    xiaotai_interaction_stats_t *stats = &xt_input.stats;
    if (stats->polls) {
        const uint32_t gap = now - xt_input.last_poll;
        if (gap > stats->max_gap_ms) stats->max_gap_ms = gap;
        if (gap > XIAOTAI_INTERACTION_LATE_MS) ++stats->late_polls;
    }
    xt_input.last_poll = now;
    ++stats->polls;
    const size_t count = xt_input.input_count;
    interaction_unlock();

    for (size_t i = 0; i < count; ++i) {
        input_state_t *state = &xt_input.inputs[i];
        const xiaotai_input_t *part = state->part;
        uint32_t events = part->poll(part->self) & part->desc.events;
        bool data_error = false;
        char text[XIAOTAI_INPUT_TEXT_MAX + 1U] = {0};
        int32_t level = 0;
        if ((events & XIAOTAI_INPUT_DATA) &&
            (!part->read || part->read(part->self, text, XIAOTAI_INPUT_TEXT_MAX) != XIAOTAI_OK ||
             text[XIAOTAI_INPUT_TEXT_MAX - 1U])) {
            events &= ~XIAOTAI_INPUT_DATA;
            data_error = true;
        }
        if ((events & XIAOTAI_INPUT_LEVEL) &&
            (!part->read_level || part->read_level(part->self, &level) != XIAOTAI_OK ||
             level < part->desc.level_min || level > part->desc.level_max)) {
            events &= ~XIAOTAI_INPUT_LEVEL;
            data_error = true;
        }
        /* Poll and read HAL outside the lock; only the single chosen executor calls them. */
        if (!interaction_lock()) return;
        if (data_error) ++stats->data_errors;
        interaction_recognize(state, now, events, text, level);
        interaction_unlock();
    }
}

static xiaotai_err_t queue_script(const char *role, uint8_t clicks, uint32_t hold_ms, bool profile_hold)
{
    if (!role || !*role || clicks > 8U || (!clicks && !hold_ms && !profile_hold)) return XIAOTAI_ERR_INVALID_ARG;
    if (!interaction_lock()) return XIAOTAI_ERR_NOT_FOUND;
    input_state_t *state = interaction_find(role);
    xiaotai_err_t result;
    if (!state) result = XIAOTAI_ERR_NOT_FOUND;
    else if (state->part->desc.kind == XIAOTAI_INPUT_SLIDER) result = XIAOTAI_ERR_NOT_SUPPORTED;
    else if (clicks && state->part->desc.kind == XIAOTAI_INPUT_CONTACT) result = XIAOTAI_ERR_INVALID_ARG;
    else if (state->real_down || state->down || state->clicks || state->script.active)
        result = XIAOTAI_ERR_INVALID_STATE;
    else {
        if (profile_hold) hold_ms = interaction_timing(role).long_ms + 200U;
        state->script = (input_script_t){.active = true, .remaining = clicks ? clicks : 1U,
                                         .hold_ms = clicks ? 100U : hold_ms};
        ++xt_input.stats.injected;
        result = XIAOTAI_OK;
    }
    interaction_unlock();
    return result;
}

xiaotai_err_t xiaotai_interaction_inject(const char *role, uint8_t clicks, uint32_t hold_ms)
{
    return queue_script(role, clicks, hold_ms, false);
}

xiaotai_err_t interaction_inject_hold(const char *role)
{
    return queue_script(role, 0, 0, true);
}

#if XIAOTAI_INTERACTION_TASK
static void run(void *ctx)
{
    (void)ctx;
    uint32_t due = now_ms();
    while (atomic_load(&s_running)) {
        const uint32_t now = now_ms(), delay = wait_ms(due, now);
        if (delay) {
            (void)xiaotai_task_wait_notify(delay);
            continue;
        }
        interaction_poll(now);
        due = interaction_next_due(due, now);
    }
    xiaotai_signal_give(&s_exited);
    xiaotai_task_exit();
}

xiaotai_err_t interaction_poll_start(void)
{
    xiaotai_err_t result = xiaotai_signal_init(&s_exited);
    if (result != XIAOTAI_OK) return result;
    const xiaotai_task_config_t task = {.name = "xt_input", .entry = run, .core = XIAOTAI_CORE_ANY,
        .stack_bytes = XIAOTAI_INTERACTION_TASK_STACK_BYTES, .stack = XIAOTAI_MEM_BULK,
        .priority = XIAOTAI_INTERACTION_TASK_PRIORITY};
    atomic_store(&s_running, true);
    result = xiaotai_task_create(&task, &s_task);
    if (result != XIAOTAI_OK) {
        atomic_store(&s_running, false);
        xiaotai_signal_deinit(&s_exited);
    }
    return result;
}

void interaction_poll_stop(void)
{
    if (!atomic_exchange(&s_running, false)) return;
    (void)xiaotai_signal_take(&s_exited, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_deinit(&s_exited);
}
#else
static void tick(void *ctx)
{
    (void)ctx;
    if (!atomic_load(&s_running)) return;
    const uint32_t now = now_ms();
    interaction_poll(now);
    s_due = interaction_next_due(s_due, now);
    const xiaotai_err_t err = xiaotai_timer_start(wait_ms(s_due, now_ms()), tick, NULL, &s_timer);
    if (err != XIAOTAI_OK) {
        atomic_store(&s_running, false);
        if (interaction_lock()) {
            xt_input.start_error = err;
            interaction_unlock();
        }
    }
}

xiaotai_err_t interaction_poll_start(void)
{
    s_due = now_ms();
    const xiaotai_err_t result = xiaotai_timer_start(0, tick, NULL, &s_timer);
    atomic_store(&s_running, result == XIAOTAI_OK);
    return result;
}

static void cancel_on_owner(void *ctx)
{
    xiaotai_timer_cancel(s_timer);
    s_timer = 0;
    xiaotai_signal_give(ctx);
}

void interaction_poll_stop(void)
{
    (void)atomic_exchange(&s_running, false);
    if (xiaotai_on_owner()) {
        xiaotai_timer_cancel(s_timer);
        s_timer = 0;
    } else {
        xiaotai_signal_t done;
        if (xiaotai_signal_init(&done) != XIAOTAI_OK) return;
        xiaotai_err_t result;
        do {
            result = xiaotai_run(cancel_on_owner, &done);
            if (result == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
        } while (result == XIAOTAI_ERR_NO_MEM);
        if (result == XIAOTAI_OK) (void)xiaotai_signal_take(&done, XIAOTAI_WAIT_FOREVER);
        xiaotai_signal_deinit(&done);
    }
}
#endif
