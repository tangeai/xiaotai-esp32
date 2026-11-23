// SPDX-License-Identifier: MIT
#include "internal.h"

static atomic_flag s_flushing = ATOMIC_FLAG_INIT;
static xiaotai_signal_t s_stop, s_stopped;
static xiaotai_task_t s_reader;
static bool s_running;

xiaotai_err_t xiaotai_eventlog_sink_register(xiaotai_eventlog_sink_fn_t sink, void *ctx)
{
    if (!sink) return XIAOTAI_ERR_INVALID_ARG;
    eventlog_state_t *s = &eventlog_state;
    if (!atomic_load_explicit(&s->ready, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_critical_enter(&s->lock);
    if (s->sink_count == XIAOTAI_EVENTLOG_SINKS_MAX) {
        xiaotai_critical_exit(&s->lock);
        return XIAOTAI_ERR_NO_MEM;
    }
    s->sinks[s->sink_count++] = (eventlog_sink_t){.fn = sink, .ctx = ctx,
                                                  .after = eventlog_ordinal(s) - s->kept};
    xiaotai_critical_exit(&s->lock);
    return XIAOTAI_OK;
}

static void deliver(eventlog_sink_t *sink, uint64_t ceiling)
{
    eventlog_state_t *s = &eventlog_state;
    while (sink->after < ceiling) {
        xiaotai_eventlog_entry_t entry;
        uint64_t next = sink->after + 1U;
        uint32_t missed = 0;
        bool have_entry = false;
        xiaotai_critical_enter(&s->lock);
        const uint64_t latest = eventlog_ordinal(s);
        const uint64_t oldest = latest - s->kept + 1U;
        if (next < oldest) {
            const uint64_t skipped = oldest - 1U < ceiling ? oldest - 1U : ceiling;
            missed = (uint32_t)(skipped - sink->after);
            sink->after = skipped;
            next = skipped + 1U;
        }
        if (next <= ceiling) {
            const uint32_t sequence = (uint32_t)(next - (latest - s->latest));
            entry = s->ring[(sequence - 1U) % s->capacity];
            sink->after = next;
            have_entry = true;
        }
        xiaotai_critical_exit(&s->lock);
        if (missed) atomic_fetch_add_explicit(&s->sink_missed, missed, memory_order_relaxed);
        if (have_entry) sink->fn(&entry, sink->ctx);
    }
}

void xiaotai_eventlog_flush(void)
{
    eventlog_state_t *s = &eventlog_state;
    if (!atomic_load_explicit(&s->ready, memory_order_acquire) ||
        atomic_flag_test_and_set_explicit(&s_flushing, memory_order_acquire)) return;
    xiaotai_critical_enter(&s->lock);
    const size_t count = s->sink_count;
    const uint64_t ceiling = eventlog_ordinal(s);
    xiaotai_critical_exit(&s->lock);
    /* Limit each pass to events already present at its start. A busy
     * producer (or a sink emitting a new event) cannot starve topic reports. */
    for (size_t i = 0; i < count; ++i) deliver(&s->sinks[i], ceiling);
    eventlog_topics_flush((uint32_t)(xiaotai_time_us() / 1000));
    atomic_fetch_add_explicit(&s->flushes, 1U, memory_order_relaxed);
    atomic_flag_clear_explicit(&s_flushing, memory_order_release);
}

static void reader(void *arg)
{
    (void)arg;
    do {
        xiaotai_eventlog_flush();
    } while (!xiaotai_signal_take(&s_stop, eventlog_state.config.reader_period_ms));
    xiaotai_signal_give(&s_stopped);
    xiaotai_task_exit();
}

xiaotai_err_t eventlog_reader_start(void)
{
    const xiaotai_eventlog_config_t *config = &eventlog_state.config;
    if (!atomic_load_explicit(&eventlog_state.ready, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    if (s_running || config->reader_stack_bytes == XIAOTAI_EVENTLOG_NO_READER) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_signal_init(&s_stop);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_signal_init(&s_stopped);
    if (err == XIAOTAI_OK) {
        const xiaotai_task_config_t task = {
            .name = "eventlog", .entry = reader, .stack_bytes = config->reader_stack_bytes,
            .stack = XIAOTAI_MEM_BULK, .priority = config->reader_priority, .core = XIAOTAI_CORE_ANY,
        };
        err = xiaotai_task_create(&task, &s_reader);
        if (err == XIAOTAI_OK) {
            s_running = true;
        } else {
            xiaotai_signal_deinit(&s_stopped);
        }
    }
    if (err != XIAOTAI_OK) xiaotai_signal_deinit(&s_stop);
    return err;
}

void eventlog_reader_stop(void)
{
    if (!s_running) return;
    xiaotai_signal_give(&s_stop);
    (void)xiaotai_signal_take(&s_stopped, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_deinit(&s_stopped);
    xiaotai_signal_deinit(&s_stop);
    s_reader = (xiaotai_task_t){0};
    s_running = false;
}
