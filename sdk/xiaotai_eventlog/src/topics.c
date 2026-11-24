// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

#define FIELD_MAX 31U
#define TOPIC_LINE_MAX (XIAOTAI_EVENTLOG_LABEL_MAX + XIAOTAI_EVENTLOG_TOPIC_VALUES * (FIELD_MAX + 12U) + 64U)

typedef struct {
    _Atomic uint32_t sequence;
    _Atomic uint32_t missed;
} gate_t;

struct xiaotai_eventlog_topic {
    const xiaotai_eventlog_topic_desc_t *desc;
    gate_t *gate;
    _Atomic uint32_t posts, at_ms;
    uint32_t reported_sequence, reported_missed, reported_at;
    bool reported;
    _Atomic uint32_t values[];
};

static bool token(const char *text, size_t max, bool punctuation)
{
    if (!text || !*text) return false;
    for (size_t i = 0; i <= max; ++i) {
        const char c = text[i];
        if (!c) return true;
        if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '_' &&
            (!punctuation || (c != '.' && c != '-'))) return false;
    }
    return false;
}

static bool valid(const xiaotai_eventlog_topic_desc_t *desc)
{
    if (!desc || !desc->fields || !desc->count || desc->count > XIAOTAI_EVENTLOG_TOPIC_VALUES ||
        (unsigned)desc->level > XIAOTAI_EVENTLOG_DEBUG ||
        !token(desc->source, XIAOTAI_EVENTLOG_SOURCE_MAX, false) ||
        !token(desc->name, XIAOTAI_EVENTLOG_LABEL_MAX, true)) return false;
    for (size_t i = 0; i < desc->count; ++i) {
        const char *field = desc->fields[i];
        if (!token(field, FIELD_MAX, false) || strcmp(field, "posts") == 0 ||
            strcmp(field, "missed") == 0 || strcmp(field, "age_ms") == 0) return false;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(desc->fields[j], field) == 0) return false;
        }
    }
    return true;
}

xiaotai_err_t xiaotai_eventlog_topic_register(const xiaotai_eventlog_topic_desc_t *desc,
                                              xiaotai_eventlog_topic_t **topic)
{
    if (!topic) return XIAOTAI_ERR_INVALID_ARG;
    *topic = NULL;
    if (!valid(desc)) return XIAOTAI_ERR_INVALID_ARG;
    eventlog_state_t *s = &eventlog_state;
    if (!atomic_load_explicit(&s->ready, memory_order_acquire)) return XIAOTAI_ERR_INVALID_STATE;
    const size_t count = atomic_load_explicit(&s->topic_count, memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_eventlog_topic_desc_t *other = s->topics[i]->desc;
        if (strcmp(desc->name, other->name) == 0 && strcmp(desc->source, other->source) == 0) {
            return XIAOTAI_ERR_INVALID_STATE;
        }
    }
    if (count >= XIAOTAI_EVENTLOG_TOPICS_MAX) return XIAOTAI_ERR_NO_MEM;
    /* Producers and readers only access the fields declared by this topic. */
    const size_t values = xiaotai_mem_has_external() ? XIAOTAI_EVENTLOG_TOPIC_VALUES : desc->count;
    const size_t alignment = _Alignof(xiaotai_eventlog_topic_t);
    const size_t bytes = sizeof(xiaotai_eventlog_topic_t) + values * sizeof(_Atomic uint32_t);
    xiaotai_eventlog_topic_t *mailbox =
        xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, (bytes + alignment - 1U) / alignment * alignment);
    if (!mailbox) return XIAOTAI_ERR_NO_MEM;
    /* Xtensa hardware RMW cannot target PSRAM. Only the two RMW words
     * need internal RAM; the values are accessed with loads/stores. */
    mailbox->gate = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, sizeof(*mailbox->gate));
    if (!mailbox->gate) {
        xiaotai_mem_free(mailbox);
        return XIAOTAI_ERR_NO_MEM;
    }
    atomic_init(&mailbox->gate->sequence, 0);
    atomic_init(&mailbox->gate->missed, 0);
    atomic_init(&mailbox->posts, 0);
    atomic_init(&mailbox->at_ms, 0);
    for (size_t i = 0; i < values; ++i) atomic_init(&mailbox->values[i], 0);
    mailbox->desc = desc;
    s->topics[count] = mailbox;
    atomic_store_explicit(&s->topic_count, count + 1, memory_order_release);
    *topic = mailbox;
    return XIAOTAI_OK;
}

void xiaotai_eventlog_post(xiaotai_eventlog_topic_t *topic, const uint32_t *values)
{
    if (!topic || !values) return;
    gate_t *gate = topic->gate;
    uint32_t seq = atomic_load_explicit(&gate->sequence, memory_order_relaxed);
    if ((seq & 1U) || !atomic_compare_exchange_strong_explicit(&gate->sequence, &seq, seq + 1U,
                                                               memory_order_acquire, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&gate->missed, 1U, memory_order_relaxed);
        return;
    }
    /* Paired with the reader's trailing acquire fence: seeing any payload
     * store from a new writer forces its final sequence load past this CAS. */
    atomic_thread_fence(memory_order_release);
    uint32_t posts = atomic_load_explicit(&topic->posts, memory_order_relaxed);
    atomic_store_explicit(&topic->posts, posts + 1U, memory_order_relaxed);
    atomic_store_explicit(&topic->at_ms, (uint32_t)(xiaotai_time_us() / 1000), memory_order_relaxed);
    for (size_t i = 0; i < topic->desc->count; ++i) {
        atomic_store_explicit(&topic->values[i], values[i], memory_order_relaxed);
    }
    atomic_store_explicit(&gate->sequence, seq + 2U, memory_order_release);
}

size_t xiaotai_eventlog_topic_count(void)
{
    eventlog_state_t *s = &eventlog_state;
    if (!atomic_load_explicit(&s->ready, memory_order_acquire)) return 0;
    return atomic_load_explicit(&s->topic_count, memory_order_acquire);
}

static bool snapshot(const xiaotai_eventlog_topic_t *topic, xiaotai_eventlog_topic_snapshot_t *out,
                     uint32_t *sequence)
{
    const gate_t *gate = topic->gate;
    const uint32_t seq = atomic_load_explicit(&gate->sequence, memory_order_acquire);
    if (seq & 1U) return false;
    xiaotai_eventlog_topic_snapshot_t sample = {.desc = topic->desc};
    sample.posts = atomic_load_explicit(&topic->posts, memory_order_relaxed);
    sample.missed = atomic_load_explicit(&gate->missed, memory_order_relaxed);
    sample.at_ms = atomic_load_explicit(&topic->at_ms, memory_order_relaxed);
    for (size_t i = 0; i < topic->desc->count; ++i) {
        sample.values[i] = atomic_load_explicit(&topic->values[i], memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_acquire);
    if (seq != atomic_load_explicit(&gate->sequence, memory_order_relaxed)) return false;
    *out = sample;
    if (sequence) *sequence = seq;
    return true;
}

bool xiaotai_eventlog_topic_read(size_t index, xiaotai_eventlog_topic_snapshot_t *out)
{
    return out && index < xiaotai_eventlog_topic_count() && snapshot(eventlog_state.topics[index], out, NULL);
}

static void report(const xiaotai_eventlog_topic_snapshot_t *sample, uint32_t now_ms)
{
    const xiaotai_eventlog_topic_desc_t *desc = sample->desc;
    char line[TOPIC_LINE_MAX];
    size_t used = strlen(desc->name);
    memcpy(line, desc->name, used);
    for (size_t i = 0; i < desc->count; ++i) {
        used += (size_t)snprintf(line + used, sizeof(line) - used, " %s=%" PRIu32,
                                 desc->fields[i], sample->values[i]);
    }
    (void)snprintf(line + used, sizeof(line) - used, " posts=%" PRIu32 " missed=%" PRIu32 " age_ms=%" PRIu32,
                   sample->posts, sample->missed, now_ms - sample->at_ms);
    switch (desc->level) {
    case XIAOTAI_EVENTLOG_ERROR: XIAOTAI_LOGE(desc->source, "%s", line); break;
    case XIAOTAI_EVENTLOG_WARN: XIAOTAI_LOGW(desc->source, "%s", line); break;
    default: XIAOTAI_LOGD(desc->source, "%s", line); break;
    }
}

/* Statistics are debug lines (xiaotai/eventlog.h); a report nobody would see
 * is not taken, so a quiet log costs no snapshot and no formatting. */
static bool wanted(const xiaotai_eventlog_topic_desc_t *desc)
{
    const uint8_t level = desc->level == XIAOTAI_EVENTLOG_ERROR ? XIAOTAI_LOG_LEVEL_ERROR
                        : desc->level == XIAOTAI_EVENTLOG_WARN ? XIAOTAI_LOG_LEVEL_WARN : XIAOTAI_LOG_LEVEL_DEBUG;
    return xiaotai_log_enabled(level, desc->source);
}

void eventlog_topics_flush(uint32_t now_ms)
{
    const size_t count = xiaotai_eventlog_topic_count();
    for (size_t i = 0; i < count; ++i) {
        xiaotai_eventlog_topic_t *topic = eventlog_state.topics[i];
        const uint32_t interval = topic->desc->interval_ms ? topic->desc->interval_ms : XIAOTAI_EVENTLOG_TOPIC_INTERVAL_MS;
        if (topic->reported && now_ms - topic->reported_at < interval) continue;
        if (!wanted(topic->desc)) continue;
        xiaotai_eventlog_topic_snapshot_t sample;
        uint32_t seq;
        if (!snapshot(topic, &sample, &seq)) continue;
        if (seq == topic->reported_sequence && sample.missed == topic->reported_missed) continue;
        /* A post can occur after the pass' interval clock was sampled. */
        const uint32_t at = (uint32_t)(xiaotai_time_us() / 1000);
        report(&sample, at);
        topic->reported_sequence = seq;
        topic->reported_missed = sample.missed;
        topic->reported_at = at;
        topic->reported = true;
        atomic_fetch_add_explicit(&eventlog_state.reports, 1U, memory_order_relaxed);
    }
}

void eventlog_topics_reset(void)
{
    const size_t count = xiaotai_eventlog_topic_count();
    for (size_t i = 0; i < count; ++i) {
        xiaotai_eventlog_topic_t *topic = eventlog_state.topics[i];
        xiaotai_mem_free(topic->gate);
        xiaotai_mem_free(topic);
    }
}
