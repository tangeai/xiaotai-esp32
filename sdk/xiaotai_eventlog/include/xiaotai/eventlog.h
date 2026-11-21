// SPDX-License-Identifier: MIT
#pragma once
/* Event log: the diagnostics store shared by every engine, module and
 * application. Two kinds of record, both kept in RAM only:
 *
 *   events   A ring of the most recent state transitions and failures (a call
 *            arrived, a link closed with a code, an activity began). Each is a
 *            fixed label, a number and the session generation it belongs to.
 *            Screens, the console (AT+LOG) and test benches read the ring; it
 *            replaces parsing log text to learn what a device did.
 *
 *   topics   One mailbox per real-time report (playout pacing, downlink
 *            admission, capture overflow). A real-time task posts a fixed set
 *            of numbers without waiting, allocating or printing; a reader
 *            prints the latest values at a low rate and counts what it skipped.
 *
 * Privacy: a label is a fixed word chosen at compile time. Never record user
 * text, names, identifiers, addresses, credentials or tokens, neither in a
 * label nor encoded in a value. Everything recorded here may be shown on a
 * screen and printed on the console.
 *
 * Threading: emitting and posting are safe from any task, never from an
 * interrupt handler. Reading, sinks and reports run on the reader: the event
 * log's own low-priority task when it has one, otherwise whichever task calls
 * xiaotai_eventlog_flush(). Function tags: [init] while engines, modules and
 * applications initialize; [any] any task; [rt] also real-time tasks (capture,
 * playout, transport callbacks); [reader] the task running the flush. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/eventlog_config.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- events */

/* Longest source and label, without the terminator. Longer ones are rejected
 * (counted in stats.rejected), never truncated: a cut label would be ambiguous. */
#define XIAOTAI_EVENTLOG_SOURCE_MAX 15U
#define XIAOTAI_EVENTLOG_LABEL_MAX 31U

typedef enum {
    XIAOTAI_EVENTLOG_INFO = 0,   /* an expected transition */
    XIAOTAI_EVENTLOG_WARN,       /* handled, but worth a look (a stale generation, a retry) */
    XIAOTAI_EVENTLOG_ERROR,      /* a session or an operation failed */
} xiaotai_eventlog_level_t;

/* What the value of an event means, so readers print it without knowing the
 * label. New kinds are added at the end; readers print unknown kinds as numbers. */
typedef enum {
    XIAOTAI_EVENTLOG_NUMBER = 0, /* a count, an index or a state number */
    XIAOTAI_EVENTLOG_ERR,        /* xiaotai_err_t */
    XIAOTAI_EVENTLOG_LINK,       /* result code of the transport (link) library */
    XIAOTAI_EVENTLOG_HTTP,       /* HTTP status */
    XIAOTAI_EVENTLOG_MS,         /* duration in milliseconds */
} xiaotai_eventlog_value_t;

/* What a caller records. source and label must have static storage duration
 * (string literals): the log keeps the pointers, not copies.
 *   source  short name of the recording component: "call", "media", "link";
 *           characters [a-z0-9_].
 *   label   what happened, in the component's own fixed vocabulary:
 *           "incoming", "connected", "closed", "stale.dropped"; characters
 *           [a-z0-9._-]. */
typedef struct {
    const char *source;
    const char *label;
    uint32_t generation;         /* session or link generation; 0: not tied to one */
    int32_t value;
    uint8_t level;               /* xiaotai_eventlog_level_t */
    uint8_t kind;                /* xiaotai_eventlog_value_t */
} xiaotai_eventlog_event_t;

/* What a reader gets. sequence starts at 1 and increases by one per recorded
 * event, so a gap between two reads means entries were overwritten in between.
 * source and label point at the recorder's static strings and stay valid. */
typedef struct {
    uint32_t sequence;
    uint32_t at_ms;              /* monotonic milliseconds since boot, wraps after 49 days */
    uint32_t generation;
    int32_t value;
    const char *source;
    const char *label;
    uint8_t level;
    uint8_t kind;
} xiaotai_eventlog_entry_t;

/* [any] [rt] Records one event: a few stores inside a critical section, no
 * allocation, no output, no waiting. Allowed on real-time tasks for
 * transitions (a pipeline opened, a link dropped), not for per-frame data:
 * that belongs in a topic. Before xiaotai_eventlog_init() and for a NULL or
 * over-long source or label the event is dropped and counted. */
void xiaotai_eventlog_emit(const xiaotai_eventlog_event_t *event);

/* [any] [rt] An informational event with a plain number. */
static inline void xiaotai_eventlog_note(const char *source, const char *label, uint32_t generation, int32_t value)
{
    xiaotai_eventlog_event_t event;
    event.source = source;
    event.label = label;
    event.generation = generation;
    event.value = value;
    event.level = (uint8_t)XIAOTAI_EVENTLOG_INFO;
    event.kind = (uint8_t)XIAOTAI_EVENTLOG_NUMBER;
    xiaotai_eventlog_emit(&event);
}

/* [any] [rt] A handled anomaly with a plain number (how many, which one). */
static inline void xiaotai_eventlog_warn(const char *source, const char *label, uint32_t generation, int32_t value)
{
    xiaotai_eventlog_event_t event;
    event.source = source;
    event.label = label;
    event.generation = generation;
    event.value = value;
    event.level = (uint8_t)XIAOTAI_EVENTLOG_WARN;
    event.kind = (uint8_t)XIAOTAI_EVENTLOG_NUMBER;
    xiaotai_eventlog_emit(&event);
}

/* [any] [rt] A failure with its xiaotai_err_t. Failures that carry another
 * code (link result, HTTP status) use xiaotai_eventlog_emit with that kind, so
 * one field never mixes code spaces. */
static inline void xiaotai_eventlog_error(const char *source, const char *label, uint32_t generation,
                                          xiaotai_err_t error)
{
    xiaotai_eventlog_event_t event;
    event.source = source;
    event.label = label;
    event.generation = generation;
    event.value = (int32_t)error;
    event.level = (uint8_t)XIAOTAI_EVENTLOG_ERROR;
    event.kind = (uint8_t)XIAOTAI_EVENTLOG_ERR;
    xiaotai_eventlog_emit(&event);
}

/* [any] Sequence of the newest event; 0 when nothing was recorded yet. */
uint32_t xiaotai_eventlog_latest(void);

/* [any] Copies the events newer than `after`, oldest first, into entries (at
 * most capacity) and returns how many. after = 0 reads from the oldest kept.
 * When `after` is older than the oldest kept event the copy starts at the
 * oldest; the caller sees the gap in the sequence numbers. The newest N events:
 * after = latest - N (or 0 when latest < N). Never blocks: entries is the
 * caller's memory and is filled inside one short critical section per entry. */
size_t xiaotai_eventlog_read(uint32_t after, xiaotai_eventlog_entry_t *entries, size_t capacity);

/* --------------------------------------------------------------------- sinks */

/* A sink sees every event once, in order, some time after it was recorded
 * (within one reader period), starting with the oldest event still kept when
 * it was registered, so events recorded during start-up reach a sink that
 * registers later. It may format and write (console lines, a file,
 * a future persistent store) but must return promptly: sinks run one after
 * another on the reader. An event overwritten before the reader got to it is
 * skipped and counted in stats.sink_missed; the sink sees the sequence gap.
 * entry is valid during the call only. */
typedef void (*xiaotai_eventlog_sink_fn_t)(const xiaotai_eventlog_entry_t *entry, void *ctx);

/* Most sinks one build may register: XIAOTAI_EVENTLOG_SINKS_MAX, a build
 * option (xiaotai/eventlog_config.h). */

/* [init] ctx is passed back unchanged and must outlive the event log.
 * XIAOTAI_ERR_INVALID_ARG for a NULL sink, XIAOTAI_ERR_NO_MEM when the table is
 * full, XIAOTAI_ERR_INVALID_STATE before xiaotai_eventlog_init(). */
xiaotai_err_t xiaotai_eventlog_sink_register(xiaotai_eventlog_sink_fn_t sink, void *ctx);

/* -------------------------------------------------------------------- topics */

/* Most numbers one topic carries. */
#define XIAOTAI_EVENTLOG_TOPIC_VALUES 17U
/* Most topics one build may register: XIAOTAI_EVENTLOG_TOPICS_MAX, a build
 * option (xiaotai/eventlog_config.h). */
/* Default minimum time between two reports of one topic. */
#define XIAOTAI_EVENTLOG_TOPIC_INTERVAL_MS 5000U

/* Describes one real-time report. The descriptor and every string in it must
 * have static storage duration. The reader prints a report as
 *   <name> <field0>=<value0> <field1>=<value1> … posts=<n> missed=<n> age_ms=<n>
 * through the port log, with source as the tag and at the given level. */
typedef struct {
    const char *source;               /* as for events */
    const char *name;                 /* "playout", "downlink.rejected"; characters [a-z0-9._-] */
    xiaotai_eventlog_level_t level;
    /* count distinct [a-z0-9_] names (1..31 chars), in the order posted;
     * posts, missed and age_ms are reserved for the reader. */
    const char *const *fields;
    uint8_t count;                    /* 1..XIAOTAI_EVENTLOG_TOPIC_VALUES */
    uint32_t interval_ms;             /* 0: XIAOTAI_EVENTLOG_TOPIC_INTERVAL_MS */
} xiaotai_eventlog_topic_desc_t;

/* Owned by the event log; valid until xiaotai_eventlog_reset(). */
typedef struct xiaotai_eventlog_topic xiaotai_eventlog_topic_t;

/* [init] Allocates the topic's mailbox (XIAOTAI_MEM_BULK) and returns its
 * handle in *topic. On any failure *topic is NULL, which xiaotai_eventlog_post
 * accepts and ignores, so producers need no "is it registered" branch.
 * XIAOTAI_ERR_INVALID_ARG for a bad descriptor, XIAOTAI_ERR_INVALID_STATE when
 * source and name are already registered or before xiaotai_eventlog_init(),
 * XIAOTAI_ERR_NO_MEM when the table or memory is exhausted. */
xiaotai_err_t xiaotai_eventlog_topic_register(const xiaotai_eventlog_topic_desc_t *desc,
                                              xiaotai_eventlog_topic_t **topic);

/* [rt] [any] Replaces the topic's values with desc->count numbers read from
 * values. One attempt, no waiting: when another task is posting to the same
 * topic at that instant the post is dropped and counted as missed. Not from an
 * interrupt handler. A newer post overwrites an unreported older one; the
 * report says how many posts it stands for. */
void xiaotai_eventlog_post(xiaotai_eventlog_topic_t *topic, const uint32_t *values);

/* The latest state of a topic, for diagnostics that print on demand. */
typedef struct {
    const xiaotai_eventlog_topic_desc_t *desc;
    uint32_t posts;                   /* accepted posts since boot */
    uint32_t missed;                  /* posts dropped because of a concurrent post */
    uint32_t at_ms;                   /* when the values were posted; meaningless while posts == 0 */
    uint32_t values[XIAOTAI_EVENTLOG_TOPIC_VALUES];
} xiaotai_eventlog_topic_snapshot_t;

/* [any] Number of registered topics; indexes are stable (registration order). */
size_t xiaotai_eventlog_topic_count(void);
/* [any] False when index is out of range or a producer was writing during the
 * copy (try again later); never waits for the producer. Not for real-time tasks. */
bool xiaotai_eventlog_topic_read(size_t index, xiaotai_eventlog_topic_snapshot_t *out);

/* -------------------------------------------------------------------- reader */

/* [any] Runs one reader pass on the calling task: hands new events to the
 * sinks, then prints every topic whose values changed and whose interval has
 * elapsed. Returns at once when another task is already inside a pass, so any
 * number of tasks may call it; passes never overlap. The event log's own task
 * calls it every reader period. A product without that task (config
 * reader_stack_bytes = XIAOTAI_EVENTLOG_NO_READER) calls it from a task it
 * already has; the console engine does so while it waits for input. Never call
 * it from a real-time task or from a sink. */
void xiaotai_eventlog_flush(void);

/* ----------------------------------------------------------------- lifecycle */

#define XIAOTAI_EVENTLOG_NO_READER UINT32_MAX

/* Every field: 0 selects the default. */
typedef struct {
    /* Events kept. Default 64 with external RAM, 16 without. At most 1024. */
    uint16_t capacity;
    /* Stack of the reader task (XIAOTAI_MEM_BULK). Default: 4096 with external
     * RAM; without it no task is created, as for XIAOTAI_EVENTLOG_NO_READER. */
    uint32_t reader_stack_bytes;
    /* RTOS priority of the reader task. Default 1 (just above idle). */
    uint8_t reader_priority;
    /* Time between two passes of the reader task. Default 100. */
    uint16_t reader_period_ms;
} xiaotai_eventlog_config_t;

/* [glue, init] The first start [glue] allows (xiaotai/framework.h). Allocates
 * the ring and the sink and topic tables (XIAOTAI_MEM_BULK); after it
 * returns XIAOTAI_OK, events are kept. config may be NULL for all defaults; it
 * is copied. The
 * product glue (xiaotai_selected_configure, xiaotai/selection.h) calls it with
 * the product's values before the board and the framework start, so start-up
 * failures are recorded; later calls return XIAOTAI_OK and change nothing. The
 * reader task is not created here but by xiaotai_engine_eventlog_start().
 * XIAOTAI_ERR_INVALID_ARG for a capacity above 1024, XIAOTAI_ERR_NO_MEM. */
xiaotai_err_t xiaotai_eventlog_init(const xiaotai_eventlog_config_t *config);

typedef struct {
    uint32_t emitted;            /* events recorded */
    uint32_t dropped_early;      /* events emitted before init */
    uint32_t rejected;           /* events with a NULL or over-long source or label */
    uint32_t sink_missed;        /* events overwritten before the sinks saw them */
    uint32_t capacity;           /* events the ring keeps */
    uint32_t topics;             /* registered topics */
    uint32_t reports;            /* topic reports printed */
    uint32_t flushes;            /* completed reader passes */
} xiaotai_eventlog_stats_t;

/* [any] */
void xiaotai_eventlog_stats(xiaotai_eventlog_stats_t *stats);

/* Engine lifecycle (called by the framework through the generated tables).
 * init: xiaotai_eventlog_init(NULL) unless already done; subscribes to the
 * framework's system events and records them (source "framework"); registers
 * the diagnostics provider "eventlog". start: creates the reader task when the
 * configuration asks for one. */
xiaotai_err_t xiaotai_engine_eventlog_init(void);
void xiaotai_engine_eventlog_start(void);

/* Host tests: stop the reader task, free the ring and every topic, forget the
 * sinks and the configuration. Topic handles become invalid. */
void xiaotai_eventlog_reset(void);

#ifdef __cplusplus
}
#endif
