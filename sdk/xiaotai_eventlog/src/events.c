// SPDX-License-Identifier: MIT
/* The event ring: initialization, recording, reading and statistics.
 *
 * Replaces the per-family copies of the runtime's diagnostic ring (eight
 * 44-byte entries, the label copied into each). Here a slot keeps the
 * caller's static source and label pointers, so recording is a handful of
 * stores inside one critical section and a slot is 28 bytes on 32-bit
 * targets. Sequences let readers see what was overwritten between two reads
 * instead of shifting the array on every event. */
#include <string.h>

#include "internal.h"
#include "xiaotai/port/mem.h"

eventlog_state_t eventlog_state;

/* NULL, empty and longer than max characters are refused, never cut: a
 * truncated label would name a different event. Bounded scan, so an
 * unterminated pointer is never read past max + 1 bytes. */
static bool text_acceptable(const char *text, size_t max)
{
    if (!text || text[0] == '\0') return false;
    for (size_t i = 1; i <= max; ++i) {
        if (text[i] == '\0') return true;
    }
    return false;
}

void eventlog_config_resolve(const xiaotai_eventlog_config_t *in, bool has_external, xiaotai_eventlog_config_t *out)
{
    xiaotai_eventlog_config_t config;
    memset(&config, 0, sizeof(config));
    if (in) config = *in;
    if (config.capacity == 0) {
        config.capacity = (uint16_t)(has_external ? EVENTLOG_CAPACITY_EXTERNAL : EVENTLOG_CAPACITY_INTERNAL);
    }
    /* Without external RAM a reader task's stack would come out of internal
     * RAM, so by default the console's service loop runs the reader instead. */
    if (config.reader_stack_bytes == 0) {
        config.reader_stack_bytes = has_external ? EVENTLOG_READER_STACK_EXTERNAL : XIAOTAI_EVENTLOG_NO_READER;
    }
    if (config.reader_priority == 0) config.reader_priority = (uint8_t)EVENTLOG_READER_PRIORITY;
    if (config.reader_period_ms == 0) config.reader_period_ms = (uint16_t)EVENTLOG_READER_PERIOD_MS;
    *out = config;
}

xiaotai_err_t xiaotai_eventlog_init(const xiaotai_eventlog_config_t *config)
{
    eventlog_state_t *s = &eventlog_state;
    if (atomic_load_explicit(&s->ready, memory_order_acquire)) return XIAOTAI_OK;
    if (config && config->capacity > EVENTLOG_CAPACITY_MAX) return XIAOTAI_ERR_INVALID_ARG;

    xiaotai_eventlog_config_t resolved;
    eventlog_config_resolve(config, xiaotai_mem_has_external(), &resolved);

    /* Everything long-lived and touched only by tasks goes to BULK (PSRAM
     * where the board has it). */
    xiaotai_eventlog_entry_t *ring = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, resolved.capacity, sizeof(*ring));
    eventlog_sink_t *sinks = ring ? xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_EVENTLOG_SINKS_MAX, sizeof(*sinks))
                                  : NULL;
    xiaotai_eventlog_topic_t **topics =
        sinks ? xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_EVENTLOG_TOPICS_MAX, sizeof(*topics)) : NULL;
    if (!topics) {
        xiaotai_mem_free(sinks);
        xiaotai_mem_free(ring);
        return XIAOTAI_ERR_NO_MEM;
    }

    /* The lock lives as long as the program: initialized once, never torn
     * down (the port has no deinit for critical sections). */
    if (!s->lock_initialized) {
        xiaotai_critical_init(&s->lock);
        s->lock_initialized = true;
    }
    s->config = resolved;
    s->ring = ring;
    s->capacity = resolved.capacity;
    s->latest = 0;
    s->kept = 0;
    s->emitted = 0;
    s->wraps = 0;
    s->sinks = sinks;
    s->sink_count = 0;
    s->topics = topics;
    atomic_store_explicit(&s->topic_count, 0, memory_order_relaxed);
    atomic_store_explicit(&s->ready, true, memory_order_release);
    return XIAOTAI_OK;
}

void xiaotai_eventlog_emit(const xiaotai_eventlog_event_t *event)
{
    eventlog_state_t *s = &eventlog_state;
    if (!atomic_load_explicit(&s->ready, memory_order_acquire)) {
        atomic_fetch_add_explicit(&s->dropped_early, 1U, memory_order_relaxed);
        return;
    }
    if (!event || !text_acceptable(event->source, XIAOTAI_EVENTLOG_SOURCE_MAX) ||
        !text_acceptable(event->label, XIAOTAI_EVENTLOG_LABEL_MAX)) {
        atomic_fetch_add_explicit(&s->rejected, 1U, memory_order_relaxed);
        return;
    }

    /* Only fixed-size stores and the clock inside the section (the existing
     * rings did the same: no I/O, no allocation). The time is read inside so
     * at_ms never decreases along the sequence. */
    xiaotai_critical_enter(&s->lock);
    uint32_t sequence = s->latest + 1U;
    if (sequence == 0U) {
        /* 2^32 events: restart at 1 with an empty ring, since older entries
         * would otherwise compare as newer. A reader whose cursor is above
         * latest knows the sequence restarted. */
        sequence = 1U;
        s->kept = 0;
        ++s->wraps;
    }
    xiaotai_eventlog_entry_t *slot = &s->ring[(sequence - 1U) % s->capacity];
    slot->sequence = sequence;
    slot->at_ms = (uint32_t)(xiaotai_time_us() / 1000);
    slot->generation = event->generation;
    slot->value = event->value;
    slot->source = event->source;
    slot->label = event->label;
    slot->level = event->level;
    slot->kind = event->kind;
    s->latest = sequence;
    if (s->kept < s->capacity) ++s->kept;
    ++s->emitted;
    xiaotai_critical_exit(&s->lock);
}

uint32_t xiaotai_eventlog_latest(void)
{
    eventlog_state_t *s = &eventlog_state;
    if (!atomic_load_explicit(&s->ready, memory_order_acquire)) return 0;
    xiaotai_critical_enter(&s->lock);
    const uint32_t latest = s->latest;
    xiaotai_critical_exit(&s->lock);
    return latest;
}

size_t xiaotai_eventlog_read(uint32_t after, xiaotai_eventlog_entry_t *entries, size_t capacity)
{
    eventlog_state_t *s = &eventlog_state;
    if (!entries || capacity == 0 || !atomic_load_explicit(&s->ready, memory_order_acquire)) return 0;
    /* 64-bit so after = UINT32_MAX cannot wrap to "from the oldest". */
    uint64_t next = (uint64_t)after + 1U;
    size_t count = 0;
    while (count < capacity) {
        /* One entry per critical section: a slow reader delays a producer by
         * one 28-byte copy at most, and the slot cannot change mid-copy. */
        xiaotai_critical_enter(&s->lock);
        const uint32_t latest = s->latest;
        if (next > latest) {
            xiaotai_critical_exit(&s->lock);
            break;
        }
        const uint32_t oldest = latest - s->kept + 1U;
        if (next < oldest) next = oldest;
        entries[count] = s->ring[(uint32_t)(next - 1U) % s->capacity];
        xiaotai_critical_exit(&s->lock);
        ++count;
        ++next;
    }
    return count;
}

void xiaotai_eventlog_stats(xiaotai_eventlog_stats_t *stats)
{
    if (!stats) return;
    eventlog_state_t *s = &eventlog_state;
    memset(stats, 0, sizeof(*stats));
    if (atomic_load_explicit(&s->ready, memory_order_acquire)) {
        xiaotai_critical_enter(&s->lock);
        stats->emitted = s->emitted;
        xiaotai_critical_exit(&s->lock);
        stats->capacity = s->capacity;
        stats->topics = (uint32_t)atomic_load_explicit(&s->topic_count, memory_order_acquire);
    }
    stats->dropped_early = (uint32_t)atomic_load_explicit(&s->dropped_early, memory_order_relaxed);
    stats->rejected = (uint32_t)atomic_load_explicit(&s->rejected, memory_order_relaxed);
    stats->sink_missed = (uint32_t)atomic_load_explicit(&s->sink_missed, memory_order_relaxed);
    stats->reports = (uint32_t)atomic_load_explicit(&s->reports, memory_order_relaxed);
    stats->flushes = (uint32_t)atomic_load_explicit(&s->flushes, memory_order_relaxed);
}

void eventlog_events_reset(void)
{
    eventlog_state_t *s = &eventlog_state;
    atomic_store_explicit(&s->ready, false, memory_order_release);
    xiaotai_mem_free(s->topics);
    xiaotai_mem_free(s->sinks);
    xiaotai_mem_free(s->ring);
    memset(&s->config, 0, sizeof(s->config));
    s->ring = NULL;
    s->capacity = 0;
    s->latest = 0;
    s->kept = 0;
    s->emitted = 0;
    s->wraps = 0;
    s->sinks = NULL;
    s->sink_count = 0;
    s->topics = NULL;
    atomic_store_explicit(&s->topic_count, 0, memory_order_relaxed);
    atomic_store_explicit(&s->dropped_early, 0U, memory_order_relaxed);
    atomic_store_explicit(&s->rejected, 0U, memory_order_relaxed);
    atomic_store_explicit(&s->sink_missed, 0U, memory_order_relaxed);
    atomic_store_explicit(&s->reports, 0U, memory_order_relaxed);
    atomic_store_explicit(&s->flushes, 0U, memory_order_relaxed);
}
