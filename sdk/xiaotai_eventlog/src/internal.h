// SPDX-License-Identifier: MIT
#pragma once
/* State shared by the event log's sources: events.c (the ring, init, stats)
 * and the topic, reader and engine sources that build on it. Nothing here is
 * part of the contract (xiaotai/eventlog.h).
 *
 * Ownership: one static instance, eventlog_state. Fields written only by
 * xiaotai_eventlog_init() and xiaotai_eventlog_reset() are published by the
 * release store of `ready` and read after its acquire load; init and reset run
 * on one task ([glue, init]; reset is for host tests) and never concurrently
 * with anything else that touches those fields. The ring cursor (latest, kept,
 * emitted) is read and written only inside `lock`. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"

/* Largest ring xiaotai_eventlog_init() accepts (xiaotai/eventlog.h). */
#define EVENTLOG_CAPACITY_MAX 1024U
/* Defaults of xiaotai_eventlog_config_t, chosen by external RAM. */
#define EVENTLOG_CAPACITY_EXTERNAL 64U
#define EVENTLOG_CAPACITY_INTERNAL 16U
/* Keep at least 1 KiB above the measured reader workload. */
#define EVENTLOG_READER_STACK_EXTERNAL 4096U
#define EVENTLOG_READER_PRIORITY 1U
#define EVENTLOG_READER_PERIOD_MS 100U

/* One registered sink. `after` is the last sequence handed to it: the reader
 * delivers from there, and a sink registered late starts at the oldest event
 * kept at registration. */
typedef struct {
    xiaotai_eventlog_sink_fn_t fn;
    void *ctx;
    uint64_t after;    /* ordinals include laps around the public 32-bit sequence */
} eventlog_sink_t;

typedef struct {
    /* True from a successful init until reset. Emitting, reading and stats
     * test it first, so nothing below is touched before init. */
    atomic_bool ready;
    /* The product's configuration with every default filled in. */
    xiaotai_eventlog_config_t config;

    /* Guards the ring cursor and each slot copy; held for one entry only. */
    xiaotai_critical_t lock;
    bool lock_initialized;

    /* capacity slots of XIAOTAI_MEM_BULK; the event with sequence s is at
     * ring[(s - 1) % capacity]. */
    xiaotai_eventlog_entry_t *ring;
    uint32_t capacity;
    uint32_t latest;   /* newest sequence, 0 before the first event */
    uint32_t kept;     /* events in the ring: oldest = latest - kept + 1 */
    uint32_t emitted;  /* events recorded since init (does not restart on wrap) */
    uint32_t wraps;

    /* Registration tables, XIAOTAI_MEM_BULK, sized by the build options
     * (xiaotai/eventlog_config.h); written only while components initialize. */
    eventlog_sink_t *sinks;            /* XIAOTAI_EVENTLOG_SINKS_MAX entries */
    size_t sink_count;
    xiaotai_eventlog_topic_t **topics; /* XIAOTAI_EVENTLOG_TOPICS_MAX entries */
    atomic_size_t topic_count;

    /* Counters of xiaotai_eventlog_stats(). */
    atomic_uint_least32_t dropped_early;
    atomic_uint_least32_t rejected;
    atomic_uint_least32_t sink_missed;
    atomic_uint_least32_t reports;
    atomic_uint_least32_t flushes;
} eventlog_state_t;

extern eventlog_state_t eventlog_state;

/* Fills every zero field of `in` (NULL: all) with its default for a device
 * with or without external RAM. Does not check the capacity limit. */
void eventlog_config_resolve(const xiaotai_eventlog_config_t *in, bool has_external, xiaotai_eventlog_config_t *out);

/* Frees the ring and both tables, forgets the configuration and zeroes every
 * counter; afterwards the log behaves as before its first init. The caller
 * has stopped the reader and freed the topics' mailboxes first. */
void eventlog_events_reset(void);

/* Requires the ring lock when concurrent producers can emit. */
static inline uint64_t eventlog_ordinal(const eventlog_state_t *s)
{
    return (uint64_t)s->wraps * UINT32_MAX + s->latest;
}

void eventlog_topics_flush(uint32_t now_ms);
void eventlog_topics_reset(void);
xiaotai_err_t eventlog_reader_start(void);
void eventlog_reader_stop(void);
