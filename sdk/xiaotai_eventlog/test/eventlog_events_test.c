// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_stop_and_diagnostics
/* The event ring (work package EL-1): initialization, recording, reading and
 * statistics, against the real host port (tasks are threads, the clock is the
 * monotonic clock, allocations go through the counting host allocator).
 *
 * Ported: tests/regression/test_voice_reliability.py, "diagnostic ring:
 * bounded eviction, ordering, session attribution" (12 events into the old
 * runtime's 8-entry ring keep the newest 8, oldest first, with time, session
 * and label). That case drives the host clock at the original 100 ms steps. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

static int s_failures;
static int64_t s_fixture_now_us = -1;

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    return s_fixture_now_us < 0 ? __real_xiaotai_time_us() : s_fixture_now_us;
}

#define CHECK(condition, ...)                                         \
    do {                                                              \
        if (!(condition)) {                                           \
            ++s_failures;                                             \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

static xiaotai_eventlog_stats_t stats(void)
{
    xiaotai_eventlog_stats_t out;
    xiaotai_eventlog_stats(&out);
    return out;
}

static void fresh(uint16_t capacity)
{
    xiaotai_eventlog_reset();
    const xiaotai_eventlog_config_t config = {.capacity = capacity};
    const xiaotai_err_t err = xiaotai_eventlog_init(&config);
    CHECK(err == XIAOTAI_OK, "init(capacity %u) = 0x%x", (unsigned)capacity, (unsigned)err);
}

/* ------------------------------------------------------------ before init */

static void test_before_init(void)
{
    xiaotai_eventlog_reset();
    xiaotai_eventlog_note("call", "incoming", 3, 1);
    xiaotai_eventlog_error("call", "ended", 3, XIAOTAI_FAIL);
    xiaotai_eventlog_entry_t entry;
    CHECK(xiaotai_eventlog_latest() == 0, "latest before init %u", (unsigned)xiaotai_eventlog_latest());
    CHECK(xiaotai_eventlog_read(0, &entry, 1) == 0, "read before init returned entries");
    const xiaotai_eventlog_stats_t s = stats();
    CHECK(s.dropped_early == 2, "dropped_early %u", (unsigned)s.dropped_early);
    CHECK(s.emitted == 0 && s.rejected == 0 && s.capacity == 0, "emitted %u rejected %u capacity %u",
          (unsigned)s.emitted, (unsigned)s.rejected, (unsigned)s.capacity);
}

/* ------------------------------------------------- init: limits, memory, idempotence */

static void test_init(void)
{
    xiaotai_eventlog_reset();
    xiaotai_mem_host_clear();

    /* Above 1024 is refused before anything is allocated; 1024 itself is fine. */
    const xiaotai_eventlog_config_t too_big = {.capacity = 1025};
    CHECK(xiaotai_eventlog_init(&too_big) == XIAOTAI_ERR_INVALID_ARG, "capacity 1025 accepted");
    CHECK(xiaotai_mem_host_allocations() == 0, "refused init allocated %u", (unsigned)xiaotai_mem_host_allocations());
    xiaotai_eventlog_note("framework", "started", 0, 0);
    CHECK(stats().dropped_early == 1, "after a refused init events are still early");

    /* Each of the three allocations (ring, sink table, topic table) failing
     * in turn: NO_MEM, nothing left allocated, still not initialized. */
    for (uint32_t nth = 1; nth <= 3; ++nth) {
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(nth);
        const xiaotai_err_t err = xiaotai_eventlog_init(NULL);
        CHECK(err == XIAOTAI_ERR_NO_MEM, "allocation %u failing: init = 0x%x", (unsigned)nth, (unsigned)err);
        CHECK(xiaotai_mem_host_outstanding() == 0, "allocation %u failing leaks %lld", (unsigned)nth,
              (long long)xiaotai_mem_host_outstanding());
        CHECK(stats().capacity == 0, "allocation %u failing: initialized anyway", (unsigned)nth);
    }

    /* NULL: every default. The host has no external RAM: 16 events. Exactly
     * three port allocations, all still held. */
    xiaotai_mem_host_clear();
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK, "init(NULL) failed");
    CHECK(xiaotai_mem_host_allocations() == 3, "init allocated %u times", (unsigned)xiaotai_mem_host_allocations());
    CHECK(xiaotai_mem_host_outstanding() == 3, "init holds %lld blocks", (long long)xiaotai_mem_host_outstanding());
    CHECK(stats().capacity == 16, "default capacity without external RAM %u", (unsigned)stats().capacity);
    CHECK(eventlog_state.config.reader_stack_bytes == XIAOTAI_EVENTLOG_NO_READER,
          "no external RAM must mean no reader task by default, got %u",
          (unsigned)eventlog_state.config.reader_stack_bytes);

    /* A later init changes nothing, whatever it asks for, and allocates nothing. */
    xiaotai_eventlog_note("framework", "started", 0, 0);
    const xiaotai_eventlog_config_t other = {.capacity = 200, .reader_period_ms = 5};
    CHECK(xiaotai_eventlog_init(&other) == XIAOTAI_OK, "second init failed");
    CHECK(xiaotai_eventlog_init(&too_big) == XIAOTAI_OK, "second init with a bad capacity must still be OK");
    CHECK(stats().capacity == 16, "second init changed the capacity to %u", (unsigned)stats().capacity);
    CHECK(eventlog_state.config.reader_period_ms == EVENTLOG_READER_PERIOD_MS, "second init changed the period");
    CHECK(xiaotai_eventlog_latest() == 1, "second init lost the recorded event");
    CHECK(xiaotai_mem_host_allocations() == 3, "second init allocated");

    /* Reset releases all three blocks; init works again afterwards. */
    xiaotai_eventlog_reset();
    CHECK(xiaotai_mem_host_outstanding() == 0, "reset leaves %lld blocks", (long long)xiaotai_mem_host_outstanding());
    const xiaotai_eventlog_stats_t s = stats();
    CHECK(s.emitted == 0 && s.capacity == 0 && s.dropped_early == 0, "reset kept counters");
    const xiaotai_eventlog_config_t largest = {.capacity = 1024};
    CHECK(xiaotai_eventlog_init(&largest) == XIAOTAI_OK, "capacity 1024 refused");
    CHECK(stats().capacity == 1024, "capacity %u", (unsigned)stats().capacity);
    xiaotai_eventlog_reset();
}

static void test_defaults(void)
{
    xiaotai_eventlog_config_t out;
    eventlog_config_resolve(NULL, true, &out);
    CHECK(out.capacity == 64 && out.reader_stack_bytes == 4096 && out.reader_priority == 1 &&
              out.reader_period_ms == 100,
          "external defaults %u %u %u %u", (unsigned)out.capacity, (unsigned)out.reader_stack_bytes,
          (unsigned)out.reader_priority, (unsigned)out.reader_period_ms);
    eventlog_config_resolve(NULL, false, &out);
    CHECK(out.capacity == 16 && out.reader_stack_bytes == XIAOTAI_EVENTLOG_NO_READER && out.reader_priority == 1 &&
              out.reader_period_ms == 100,
          "internal defaults %u %u %u %u", (unsigned)out.capacity, (unsigned)out.reader_stack_bytes,
          (unsigned)out.reader_priority, (unsigned)out.reader_period_ms);
    /* Given values are kept, including an explicit "no reader" on a board
     * with external RAM. */
    const xiaotai_eventlog_config_t given = {
        .capacity = 40, .reader_stack_bytes = XIAOTAI_EVENTLOG_NO_READER, .reader_priority = 3, .reader_period_ms = 250};
    eventlog_config_resolve(&given, true, &out);
    CHECK(out.capacity == 40 && out.reader_stack_bytes == XIAOTAI_EVENTLOG_NO_READER && out.reader_priority == 3 &&
              out.reader_period_ms == 250,
          "given values changed");
    const xiaotai_eventlog_config_t stack = {.reader_stack_bytes = 3072};
    eventlog_config_resolve(&stack, false, &out);
    CHECK(out.reader_stack_bytes == 3072 && out.capacity == 16, "explicit stack without external RAM");
    eventlog_config_resolve(&stack, true, &out);
    CHECK(out.reader_stack_bytes == 3072 && out.capacity == 64, "explicit stack with external RAM");
}

/* ------------------------------------------- ported: eviction, order, attribution */

static void test_ported_ring(void)
{
    fresh(8);
    for (int i = 0; i < 12; ++i) {
        s_fixture_now_us = i * INT64_C(100000);
        xiaotai_eventlog_note("ai_talk", "calltool.received", 7, i);
    }
    s_fixture_now_us = -1;
    xiaotai_eventlog_entry_t entries[16];
    const size_t count = xiaotai_eventlog_read(0, entries, 16);
    CHECK(count == 8, "kept %zu of 12 in a ring of 8", count);
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_eventlog_entry_t *e = &entries[i];
        CHECK(e->value == (int32_t)i + 4, "entry %zu value %d", i, (int)e->value);
        CHECK(e->sequence == (uint32_t)i + 5U, "entry %zu sequence %u", i, (unsigned)e->sequence);
        CHECK(e->generation == 7, "entry %zu generation %u", i, (unsigned)e->generation);
        CHECK(e->at_ms == (i + 4U) * 100U, "entry %zu at %u", i, (unsigned)e->at_ms);
        CHECK(strcmp(e->source, "ai_talk") == 0 && strcmp(e->label, "calltool.received") == 0, "entry %zu text", i);
        CHECK(e->level == XIAOTAI_EVENTLOG_INFO && e->kind == XIAOTAI_EVENTLOG_NUMBER, "entry %zu level/kind", i);
    }
    const xiaotai_eventlog_stats_t s = stats();
    CHECK(s.emitted == 12 && xiaotai_eventlog_latest() == 12, "emitted %u latest %u", (unsigned)s.emitted,
          (unsigned)xiaotai_eventlog_latest());
}

/* -------------------------------------------- sequences and the four kinds of after */

static void check_run(const xiaotai_eventlog_entry_t *entries, size_t count, uint32_t first, size_t expected,
                      const char *what)
{
    CHECK(count == expected, "%s: %zu entries, expected %zu", what, count, expected);
    for (size_t i = 0; i < count && i < expected; ++i) {
        CHECK(entries[i].sequence == first + (uint32_t)i, "%s: entry %zu sequence %u, expected %u", what, i,
              (unsigned)entries[i].sequence, (unsigned)(first + i));
        /* value = sequence, as recorded below */
        CHECK(entries[i].value == (int32_t)entries[i].sequence, "%s: entry %zu holds %d", what, i,
              (int)entries[i].value);
    }
}

static void test_sequences(void)
{
    fresh(8);
    xiaotai_eventlog_entry_t entries[16];
    CHECK(xiaotai_eventlog_read(0, entries, 16) == 0, "empty ring returned entries");

    /* Sequences start at 1 and grow by one. */
    for (int32_t i = 1; i <= 5; ++i) xiaotai_eventlog_note("link", "online", 1, i);
    check_run(entries, xiaotai_eventlog_read(0, entries, 16), 1, 5, "first five");

    for (int32_t i = 6; i <= 12; ++i) xiaotai_eventlog_note("link", "online", 1, i);
    CHECK(xiaotai_eventlog_latest() == 12, "latest %u", (unsigned)xiaotai_eventlog_latest());

    /* after = 0: from the oldest kept (5), oldest first. */
    check_run(entries, xiaotai_eventlog_read(0, entries, 16), 5, 8, "after 0");
    /* after inside the ring: only the newer ones. */
    check_run(entries, xiaotai_eventlog_read(9, entries, 16), 10, 3, "after 9");
    /* after already overwritten: from the oldest kept; the gap 3..4 shows. */
    check_run(entries, xiaotai_eventlog_read(2, entries, 16), 5, 8, "after 2");
    /* after = latest, and beyond it: nothing. */
    CHECK(xiaotai_eventlog_read(12, entries, 16) == 0, "after latest returned entries");
    CHECK(xiaotai_eventlog_read(13, entries, 16) == 0, "after beyond latest returned entries");
    CHECK(xiaotai_eventlog_read(UINT32_MAX, entries, 16) == 0, "after UINT32_MAX returned entries");

    /* The newest N: after = latest - N. */
    check_run(entries, xiaotai_eventlog_read(xiaotai_eventlog_latest() - 3, entries, 16), 10, 3, "newest 3");
    /* A short buffer: the oldest first, then continue after the last one read. */
    check_run(entries, xiaotai_eventlog_read(0, entries, 3), 5, 3, "first page");
    check_run(entries, xiaotai_eventlog_read(entries[2].sequence, entries, 3), 8, 3, "second page");
    check_run(entries, xiaotai_eventlog_read(entries[2].sequence, entries, 3), 11, 2, "last page");
    /* No buffer, no room. */
    CHECK(xiaotai_eventlog_read(0, NULL, 4) == 0, "NULL buffer");
    CHECK(xiaotai_eventlog_read(0, entries, 0) == 0, "zero capacity");
}

/* ---------------------------------------------------- refused events, helpers */

static void test_rejected(void)
{
    fresh(8);
    char unterminated[40];
    memset(unterminated, 'a', sizeof(unterminated)); /* no terminator: must not be read past 32 bytes */
    xiaotai_eventlog_note(NULL, "incoming", 1, 0);
    xiaotai_eventlog_note("call", NULL, 1, 0);
    xiaotai_eventlog_note("", "incoming", 1, 0);
    xiaotai_eventlog_note("call", "", 1, 0);
    xiaotai_eventlog_note("abcdefghijklmnop", "incoming", 1, 0);                  /* 16 characters */
    xiaotai_eventlog_note("call", "abcdefghijklmnopqrstuvwxyz012345", 1, 0);      /* 32 characters */
    xiaotai_eventlog_note("call", unterminated, 1, 0);
    xiaotai_eventlog_emit(NULL);
    xiaotai_eventlog_stats_t s = stats();
    CHECK(s.rejected == 8, "rejected %u", (unsigned)s.rejected);
    CHECK(s.emitted == 0 && xiaotai_eventlog_latest() == 0, "a refused event was recorded");

    /* The longest accepted: 15 and 31 characters. */
    xiaotai_eventlog_note("abcdefghijklmno", "abcdefghijklmnopqrstuvwxyz01234", 1, 0);
    s = stats();
    CHECK(s.rejected == 8 && s.emitted == 1, "15/31 characters: rejected %u emitted %u", (unsigned)s.rejected,
          (unsigned)s.emitted);
}

static void test_helpers(void)
{
    fresh(8);
    static const char source[] = "media";
    static const char label[] = "pipeline.open";
    xiaotai_eventlog_note(source, label, 11, -5);
    xiaotai_eventlog_warn("media", "stale.dropped", 10, 3);
    xiaotai_eventlog_error("media", "pipeline.failed", 11, XIAOTAI_ERR_NO_MEM);
    const xiaotai_eventlog_event_t link = {.source = "link",
                                           .label = "closed",
                                           .generation = 4,
                                           .value = -1003,
                                           .level = XIAOTAI_EVENTLOG_ERROR,
                                           .kind = XIAOTAI_EVENTLOG_LINK};
    xiaotai_eventlog_emit(&link);
    /* Kinds unknown today are kept as given; readers print them as numbers. */
    const xiaotai_eventlog_event_t future = {
        .source = "ai_talk", .label = "ready", .generation = 2, .value = 812, .level = 2, .kind = 9};
    xiaotai_eventlog_emit(&future);

    xiaotai_eventlog_entry_t e[8];
    CHECK(xiaotai_eventlog_read(0, e, 8) == 5, "helpers recorded %zu", xiaotai_eventlog_read(0, e, 8));
    /* The ring keeps the caller's pointers, not copies. */
    CHECK(e[0].source == source && e[0].label == label, "pointers not kept");
    CHECK(e[0].level == XIAOTAI_EVENTLOG_INFO && e[0].kind == XIAOTAI_EVENTLOG_NUMBER && e[0].value == -5 &&
              e[0].generation == 11,
          "note");
    CHECK(e[1].level == XIAOTAI_EVENTLOG_WARN && e[1].kind == XIAOTAI_EVENTLOG_NUMBER && e[1].value == 3 &&
              e[1].generation == 10,
          "warn");
    CHECK(e[2].level == XIAOTAI_EVENTLOG_ERROR && e[2].kind == XIAOTAI_EVENTLOG_ERR &&
              e[2].value == XIAOTAI_ERR_NO_MEM && strcmp(e[2].label, "pipeline.failed") == 0,
          "error");
    CHECK(e[3].level == XIAOTAI_EVENTLOG_ERROR && e[3].kind == XIAOTAI_EVENTLOG_LINK && e[3].value == -1003 &&
              strcmp(e[3].source, "link") == 0,
          "emit with a link code");
    CHECK(e[4].level == 2 && e[4].kind == 9 && e[4].value == 812, "unknown kind not kept");
}

/* ---------------------------------------------- no allocation on the recording path */

static void test_no_allocation(void)
{
    fresh(16);
    xiaotai_mem_host_clear();
    xiaotai_eventlog_entry_t entries[16];
    for (int32_t i = 0; i < 100; ++i) {
        xiaotai_eventlog_warn("transport", "reserve_miss", 2, i);
        (void)xiaotai_eventlog_read(xiaotai_eventlog_latest() - 1U, entries, 16);
    }
    xiaotai_eventlog_note(NULL, "x", 0, 0);
    xiaotai_eventlog_stats_t s;
    xiaotai_eventlog_stats(&s);
    CHECK(xiaotai_mem_host_allocations() == 0, "emit/read/stats allocated %u times",
          (unsigned)xiaotai_mem_host_allocations());
}

/* ----------------------------------------------------- sequence restart at 2^32 */

static void test_wrap(void)
{
    fresh(4);
    for (int32_t i = 0; i < 4; ++i) xiaotai_eventlog_note("clock", "synced", 0, i);
    /* Only this test moves the cursor; nothing else runs. */
    eventlog_state.latest = UINT32_MAX - 1U;
    xiaotai_eventlog_note("clock", "synced", 0, 100);
    CHECK(xiaotai_eventlog_latest() == UINT32_MAX, "latest %u", (unsigned)xiaotai_eventlog_latest());
    xiaotai_eventlog_note("clock", "synced", 0, 101);
    xiaotai_eventlog_note("clock", "synced", 0, 102);
    CHECK(xiaotai_eventlog_latest() == 2, "after the wrap latest %u", (unsigned)xiaotai_eventlog_latest());
    xiaotai_eventlog_entry_t entries[8];
    const size_t count = xiaotai_eventlog_read(0, entries, 8);
    /* Only the events after the restart: older ones would compare as newer. */
    CHECK(count == 2, "after the wrap read %zu", count);
    if (count == 2) {
        CHECK(entries[0].sequence == 1 && entries[0].value == 101, "first after wrap %u/%d",
              (unsigned)entries[0].sequence, (int)entries[0].value);
        CHECK(entries[1].sequence == 2 && entries[1].value == 102, "second after wrap");
    }
    /* A reader still holding the old cursor gets nothing and sees latest < cursor. */
    CHECK(xiaotai_eventlog_read(UINT32_MAX - 1U, entries, 8) == 0, "stale cursor read entries");
    CHECK(stats().emitted == 7, "emitted %u", (unsigned)stats().emitted);
}

/* ------------------------------------------------------------- concurrency */

#define PRODUCERS 4

static const char *const s_labels[PRODUCERS] = {"task.0", "task.1", "task.2", "task.3"};

typedef struct {
    int index;
    int32_t count;
    xiaotai_signal_t done; /* one each: a binary signal does not count gives */
} producer_t;

static void producer(void *arg)
{
    producer_t *p = arg;
    for (int32_t i = 0; i < p->count; ++i) {
        /* value carries producer and position, generation the producer. */
        xiaotai_eventlog_note("test", s_labels[p->index], (uint32_t)p->index + 1U, p->index * 100000 + i);
    }
    xiaotai_signal_give(&p->done);
}

/* Runs PRODUCERS tasks of count events each and waits for all of them. */
static void run_producers(int32_t count)
{
    static producer_t producers[PRODUCERS];
    for (int i = 0; i < PRODUCERS; ++i) {
        producers[i].index = i;
        producers[i].count = count;
        CHECK(xiaotai_signal_init(&producers[i].done) == XIAOTAI_OK, "signal %d", i);
    }
    for (int i = 0; i < PRODUCERS; ++i) {
        const xiaotai_task_config_t config = {.name = "producer",
                                              .entry = producer,
                                              .arg = &producers[i],
                                              .stack_bytes = 4096,
                                              .stack = XIAOTAI_MEM_BULK,
                                              .priority = 5,
                                              .core = XIAOTAI_CORE_ANY};
        xiaotai_task_t task;
        CHECK(xiaotai_task_create(&config, &task) == XIAOTAI_OK, "producer %d not started", i);
    }
    for (int i = 0; i < PRODUCERS; ++i) {
        CHECK(xiaotai_signal_take(&producers[i].done, 30000), "producer %d did not finish", i);
        xiaotai_signal_deinit(&producers[i].done);
    }
}

/* An entry is whole: producer, label and value agree. */
static bool consistent(const xiaotai_eventlog_entry_t *e)
{
    if (e->generation < 1 || e->generation > PRODUCERS) return false;
    const int index = (int)e->generation - 1;
    return e->label == s_labels[index] && e->value / 100000 == index && strcmp(e->source, "test") == 0;
}

/* Every event of 4 x 256 kept in a ring of 1024: each recorded exactly once,
 * sequences 1..1024 without gaps, each producer's events in its own order. */
static void test_concurrent_all_kept(void)
{
    fresh(1024);
    run_producers(256);
    CHECK(xiaotai_eventlog_latest() == 1024, "latest %u", (unsigned)xiaotai_eventlog_latest());
    xiaotai_eventlog_entry_t *entries = calloc(1024, sizeof(*entries));
    const size_t count = xiaotai_eventlog_read(0, entries, 1024);
    CHECK(count == 1024, "read %zu", count);
    uint8_t seen[PRODUCERS][256];
    memset(seen, 0, sizeof(seen));
    int32_t last[PRODUCERS] = {-1, -1, -1, -1};
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_eventlog_entry_t *e = &entries[i];
        CHECK(e->sequence == i + 1, "entry %zu sequence %u", i, (unsigned)e->sequence);
        CHECK(consistent(e), "entry %zu torn", i);
        if (!consistent(e)) continue;
        const int index = (int)e->generation - 1;
        const int32_t position = e->value % 100000;
        CHECK(position >= 0 && position < 256, "position %d", (int)position);
        if (position < 0 || position >= 256) continue;
        ++seen[index][position];
        CHECK(position > last[index], "producer %d: %d after %d", index, (int)position, (int)last[index]);
        last[index] = position;
        if (i > 0) CHECK(e->at_ms - entries[i - 1].at_ms < 0x80000000U, "entry %zu earlier than its predecessor", i);
    }
    for (int p = 0; p < PRODUCERS; ++p) {
        for (int i = 0; i < 256; ++i) CHECK(seen[p][i] == 1, "producer %d event %d seen %u times", p, i, seen[p][i]);
    }
    free(entries);
}

typedef struct {
    atomic_bool stop;
    uint32_t received;
    uint32_t torn;
    uint32_t out_of_order;
    xiaotai_signal_t finished;
} follower_t;

/* Reads continuously while the producers run, as the event log's reader
 * will: never sees a torn entry or a sequence twice. */
static void follower(void *arg)
{
    follower_t *f = arg;
    xiaotai_eventlog_entry_t entries[32];
    uint32_t after = 0;
    for (;;) {
        const bool stopping = atomic_load(&f->stop);
        const size_t count = xiaotai_eventlog_read(after, entries, 32);
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].sequence <= after) ++f->out_of_order;
            if (!consistent(&entries[i])) ++f->torn;
            after = entries[i].sequence;
            ++f->received;
        }
        if (stopping && count == 0) break;
        if (count == 0) xiaotai_delay_ms(0);
    }
    xiaotai_signal_give(&f->finished);
}

/* 4 tasks x 1000 events into the default-sized ring with a reader running:
 * every emit got its own sequence (4000 in all), the reader saw whole entries
 * in strictly increasing order, and the ring holds the newest 16. */
static void test_concurrent_with_reader(void)
{
    xiaotai_eventlog_reset();
    CHECK(xiaotai_eventlog_init(NULL) == XIAOTAI_OK, "init");
    static follower_t f;
    atomic_store(&f.stop, false);
    f.received = f.torn = f.out_of_order = 0;
    CHECK(xiaotai_signal_init(&f.finished) == XIAOTAI_OK, "signal");
    const xiaotai_task_config_t config = {.name = "follower",
                                          .entry = follower,
                                          .arg = &f,
                                          .stack_bytes = 8192,
                                          .stack = XIAOTAI_MEM_BULK,
                                          .priority = 1,
                                          .core = XIAOTAI_CORE_ANY};
    xiaotai_task_t task;
    CHECK(xiaotai_task_create(&config, &task) == XIAOTAI_OK, "follower not started");
    run_producers(1000);
    atomic_store(&f.stop, true);
    CHECK(xiaotai_signal_take(&f.finished, 30000), "follower did not finish");
    xiaotai_signal_deinit(&f.finished);

    CHECK(xiaotai_eventlog_latest() == 4000, "latest %u", (unsigned)xiaotai_eventlog_latest());
    const xiaotai_eventlog_stats_t s = stats();
    CHECK(s.emitted == 4000 && s.rejected == 0 && s.dropped_early == 0, "emitted %u rejected %u", (unsigned)s.emitted,
          (unsigned)s.rejected);
    CHECK(f.torn == 0 && f.out_of_order == 0, "reader saw %u torn, %u out of order", (unsigned)f.torn,
          (unsigned)f.out_of_order);
    CHECK(f.received >= 16 && f.received <= 4000, "reader received %u", (unsigned)f.received);
    xiaotai_eventlog_entry_t entries[16];
    const size_t count = xiaotai_eventlog_read(0, entries, 16);
    CHECK(count == 16 && entries[0].sequence == 3985 && entries[15].sequence == 4000, "ring holds %zu from %u",
          count, count ? (unsigned)entries[0].sequence : 0U);
    for (size_t i = 0; i < count; ++i) CHECK(consistent(&entries[i]), "kept entry %zu torn", i);
}

int main(void)
{
    test_before_init();
    test_init();
    test_defaults();
    test_ported_ring();
    test_sequences();
    test_rejected();
    test_helpers();
    test_no_allocation();
    test_wrap();

    test_concurrent_all_kept();
    test_concurrent_with_reader();

    xiaotai_eventlog_reset();
    if (s_failures) {
        fprintf(stderr, "eventlog_events: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    printf("eventlog_events: ok\n");
    return EXIT_SUCCESS;
}
