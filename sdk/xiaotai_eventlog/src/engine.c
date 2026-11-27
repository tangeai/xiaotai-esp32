// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/log.h"

/* One system-log line per event. The source is the tag and the label the
 * message, so the log threshold and per-tag filters apply as to any line. A
 * label and a number never carry user data (xiaotai/eventlog.h), so printing
 * them is as safe as recording them. */
static void system_log(const xiaotai_eventlog_entry_t *entry, void *ctx)
{
    (void)ctx;
    if (entry->level == XIAOTAI_EVENTLOG_RECORD) return;
    const uint8_t level = entry->level == XIAOTAI_EVENTLOG_ERROR ? XIAOTAI_LOG_LEVEL_ERROR
                        : entry->level == XIAOTAI_EVENTLOG_WARN ? XIAOTAI_LOG_LEVEL_WARN
                        : entry->level == XIAOTAI_EVENTLOG_DEBUG ? XIAOTAI_LOG_LEVEL_DEBUG : XIAOTAI_LOG_LEVEL_INFO;
    if (!xiaotai_log_enabled(level, entry->source)) return;
    char gen[20] = "", value[24] = "";
    if (entry->generation) (void)snprintf(gen, sizeof(gen), " gen=%" PRIu32, entry->generation);
    switch (entry->kind) {
    case XIAOTAI_EVENTLOG_ERR:
        (void)snprintf(value, sizeof(value), " error=0x%" PRIx32, (uint32_t)entry->value);
        break;
    case XIAOTAI_EVENTLOG_LINK: (void)snprintf(value, sizeof(value), " link=%" PRId32, entry->value); break;
    case XIAOTAI_EVENTLOG_HTTP: (void)snprintf(value, sizeof(value), " http=%" PRId32, entry->value); break;
    case XIAOTAI_EVENTLOG_MS: (void)snprintf(value, sizeof(value), " ms=%" PRId32, entry->value); break;
    default:
        if (entry->value) (void)snprintf(value, sizeof(value), " value=%" PRId32, entry->value);
        break;
    }
    if (entry->level == XIAOTAI_EVENTLOG_ERROR) XIAOTAI_LOGE(entry->source, "%s%s%s", entry->label, gen, value);
    else if (entry->level == XIAOTAI_EVENTLOG_WARN) XIAOTAI_LOGW(entry->source, "%s%s%s", entry->label, gen, value);
    else if (entry->level == XIAOTAI_EVENTLOG_DEBUG) XIAOTAI_LOGD(entry->source, "%s%s%s", entry->label, gen, value);
    else XIAOTAI_LOGI(entry->source, "%s%s%s", entry->label, gen, value);
}

static void system_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (strcmp(event->name, XIAOTAI_EVENT_ACTIVITY) == 0) {
        const uint32_t activity = (uint32_t)event->args.value;
        xiaotai_eventlog_record("framework", activity ? "activity.started" : "activity.ended", 0, event->args.value);
    } else if (strcmp(event->name, XIAOTAI_EVENT_STARTED) == 0) {
        xiaotai_eventlog_note("framework", "started", 0, 0);
    } else if (strcmp(event->name, XIAOTAI_EVENT_FOREGROUND) == 0) {
        xiaotai_eventlog_debug("framework", "foreground", 0, 0);
    } else if (strcmp(event->name, XIAOTAI_EVENT_QUIESCE) == 0) {
        xiaotai_eventlog_note("framework", event->args.value ? "quiesced" : "resumed", 0, 0);
    } else if (strcmp(event->name, XIAOTAI_EVENT_RESTARTING) == 0) {
        xiaotai_eventlog_note("framework", "restarting", 0, event->args.value);
    } else if (strcmp(event->name, XIAOTAI_EVENT_SETTING_NOT_SAVED) == 0) {
        xiaotai_eventlog_error("framework", "setting.not_saved", 0, (xiaotai_err_t)event->args.value);
    }
}

static void report(const xiaotai_reply_t *out)
{
    xiaotai_eventlog_stats_t s;
    char line[224];
    xiaotai_eventlog_stats(&s);
    (void)snprintf(line, sizeof(line),
                   "emitted=%" PRIu32 " dropped_early=%" PRIu32 " rejected=%" PRIu32 " sink_missed=%" PRIu32
                   " capacity=%" PRIu32 " topics=%" PRIu32 " reports=%" PRIu32 " flushes=%" PRIu32,
                   s.emitted, s.dropped_early, s.rejected, s.sink_missed, s.capacity, s.topics, s.reports, s.flushes);
    out->write(out->ctx, line);
}

xiaotai_err_t xiaotai_engine_eventlog_init(void)
{
    static const xiaotai_subscriber_descriptor_t subscriptions[] = {
        {XIAOTAI_EVENT_STARTED, system_event, NULL}, {XIAOTAI_EVENT_ACTIVITY, system_event, NULL},
        {XIAOTAI_EVENT_FOREGROUND, system_event, NULL}, {XIAOTAI_EVENT_QUIESCE, system_event, NULL},
        {XIAOTAI_EVENT_RESTARTING, system_event, NULL}, {XIAOTAI_EVENT_SETTING_NOT_SAVED, system_event, NULL},
    };
    xiaotai_err_t err = xiaotai_eventlog_init(NULL);
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(subscriptions) / sizeof(subscriptions[0]); ++i) {
        err = xiaotai_event_subscribe_static(&subscriptions[i]);
    }
    if (err == XIAOTAI_OK && !eventlog_state.config.no_system_log)
        err = xiaotai_eventlog_sink_register(system_log, NULL);
    return err == XIAOTAI_OK ? xiaotai_diag_register("eventlog", report) : err;
}

void xiaotai_engine_eventlog_start(void)
{
    const xiaotai_err_t err = eventlog_reader_start();
    if (err != XIAOTAI_OK) xiaotai_eventlog_error("eventlog", "reader.start_failed", 0, err);
}

void xiaotai_eventlog_reset(void)
{
    eventlog_reader_stop();
    eventlog_topics_reset();
    eventlog_events_reset();
}
