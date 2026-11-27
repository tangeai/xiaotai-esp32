// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"

static atomic_bool s_follow;
static bool s_attached;

static void entry_line(const xiaotai_eventlog_entry_t *entry, char out[192])
{
    static const char levels[] = "IWEDR";
    static const char *const kinds[] = {"n", "err", "link", "http", "ms"};
    const char level = entry->level < sizeof(levels) - 1 ? levels[entry->level] : '?';
    char unknown[4];
    (void)snprintf(unknown, sizeof(unknown), "%u", (unsigned)entry->kind);
    const char *kind = entry->kind < sizeof(kinds) / sizeof(*kinds) ? kinds[entry->kind] : unknown;
    (void)snprintf(out, 192, "+LOG: seq=%" PRIu32 " ms=%" PRIu32 " level=%c src=%s label=%s gen=%" PRIu32
                   " kind=%s value=%" PRId32, entry->sequence, entry->at_ms, level, entry->source, entry->label,
                   entry->generation, kind, entry->value);
}

static void on_entry(const xiaotai_eventlog_entry_t *entry, void *ctx)
{
    (void)ctx;
    if (!atomic_load(&s_follow)) return;
    char out[192];
    entry_line(entry, out);
    (void)xiaotai_at_notify(out);
}

static xiaotai_err_t history(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    uint32_t after = 0;
    if (argc && !xiaotai_at_parse_u32(argv[0], 0, UINT32_MAX, &after))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "SEQUENCE");
    const uint32_t ceiling = xiaotai_eventlog_latest();
    xiaotai_eventlog_entry_t entry;
    while (after < ceiling && xiaotai_eventlog_read(after, &entry, 1) && entry.sequence <= ceiling) {
        char out[192];
        entry_line(&entry, out);
        xiaotai_at_printf(reply, "%s", out);
        after = entry.sequence;
    }
    xiaotai_at_printf(reply, "+LOG-END: latest=%" PRIu32, ceiling);
    return XIAOTAI_OK;
}

static xiaotai_err_t follow(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    bool enabled;
    if (!xiaotai_at_parse_flag(argv[0], &enabled))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "VALUE");
    /* Drain pre-existing entries under the previous selection. */
    xiaotai_eventlog_flush();
    atomic_store(&s_follow, enabled);
    return XIAOTAI_OK;
}

static xiaotai_err_t topics(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    const uint32_t now = (uint32_t)(xiaotai_time_us() / 1000);
    for (size_t i = 0; i < xiaotai_eventlog_topic_count(); ++i) {
        xiaotai_eventlog_topic_snapshot_t sample;
        if (!xiaotai_eventlog_topic_read(i, &sample)) continue;
        char out[XIAOTAI_AT_OUT_LINE_MAX + 1];
        size_t used = (size_t)snprintf(out, sizeof(out), "+TOPIC: src=%s name=%s posts=%" PRIu32
                                      " missed=%" PRIu32 " age_ms=%" PRIu32, sample.desc->source, sample.desc->name,
                                      sample.posts, sample.missed, sample.posts ? now - sample.at_ms : 0);
        for (size_t f = 0; f < sample.desc->count && used < sizeof(out); ++f)
            used += (size_t)snprintf(out + used, sizeof(out) - used, " %s=%" PRIu32,
                                     sample.desc->fields[f], sample.values[f]);
        if (used >= sizeof(out)) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_SIZE, "OUTPUT_TOO_LONG");
        xiaotai_at_printf(reply, "%s", out);
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t stats(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_eventlog_stats_t st;
    xiaotai_eventlog_stats(&st);
    xiaotai_at_printf(reply, "+LOG-STATS: emitted=%" PRIu32 " dropped_early=%" PRIu32 " rejected=%" PRIu32
                      " sink_missed=%" PRIu32 " capacity=%" PRIu32 " topics=%" PRIu32 " reports=%" PRIu32
                      " flushes=%" PRIu32, st.emitted, st.dropped_early, st.rejected, st.sink_missed,
                      st.capacity, st.topics, st.reports, st.flushes);
    return XIAOTAI_OK;
}

xiaotai_err_t at_log_init(bool enabled)
{
    static const xiaotai_at_verb_t commands[] = {
        {.max_args = 1, .run = history, .usage = "[<after>]"},
        {.word = "FOLLOW", .min_args = 1, .max_args = 1, .run = follow, .usage = "0|1"},
        {.word = "TOPICS", .run = topics},
        {.word = "STATS", .run = stats},
    };
    atomic_store(&s_follow, enabled);
    return at_local_verbs_register("AT+LOG", commands, sizeof(commands) / sizeof(*commands));
}

xiaotai_err_t at_log_attach(void)
{
    if (s_attached) return XIAOTAI_OK;
    const xiaotai_err_t err = xiaotai_eventlog_sink_register(on_entry, NULL);
    if (err == XIAOTAI_OK) s_attached = true;
    return err;
}

void at_log_stop(void)
{
    atomic_store(&s_follow, false);
    s_attached = false;
}
