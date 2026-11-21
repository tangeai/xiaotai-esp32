// SPDX-License-Identifier: MIT
#include "clock_internal.h"

#include <limits.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"

enum { VERSION = 1, RECORD_BYTES = 30, PREFIX_BYTES = 2, LABEL_OFFSET = 6 };

static xiaotai_err_t invalid_table(void)
{
    ++clock_state.stats.load_invalid;
    return XIAOTAI_OK;
}

xiaotai_err_t clock_store_load(void)
{
    uint8_t record[CLOCK_ALARM_BYTES];
    size_t bytes = sizeof(record);
    xiaotai_err_t err = xiaotai_kv_get_blob_strict(CLOCK_ALARM_SPACE, CLOCK_ALARM_KEY, record, &bytes,
                                                XIAOTAI_KV_WAIT_MAX_MS);
    if (err == XIAOTAI_ERR_NOT_FOUND) return XIAOTAI_OK;
    if (err == XIAOTAI_ERR_INVALID_SIZE || err == XIAOTAI_ERR_INVALID_ARG) return invalid_table();
    if (err != XIAOTAI_OK) return err;
    if (bytes < PREFIX_BYTES || record[0] != VERSION || record[1] > XIAOTAI_SCHEDULE_MAX ||
        bytes != PREFIX_BYTES + (size_t)record[1] * RECORD_BYTES) return invalid_table();

    unsigned max_id = 0;
    uint8_t seen[(UINT8_MAX + 1U) / 8U] = {0};
    for (size_t i = 0; i < record[1]; ++i) {
        const uint8_t *s = record + PREFIX_BYTES + i * RECORD_BYTES;
        clock_schedule_row_t *row = &clock_state.rows[i];
        const uint8_t mask = (uint8_t)(1U << (s[0] % 8U));
        if (!s[0] || (seen[s[0] / 8U] & mask) || (s[1] & ~1U) || s[2] > 23U || s[3] > 59U ||
            s[4] > XIAOTAI_SCHEDULE_EVERY_DAY || s[5] || !memchr(s + LABEL_OFFSET, 0, sizeof(row->entry.label)) ||
            !clock_label_copy(row->entry.label, (const char *)s + LABEL_OFFSET)) return invalid_table();
        seen[s[0] / 8U] |= mask;
        if (s[0] > max_id) max_id = s[0];
        row->entry.id = s[0];
        row->entry.kind = XIAOTAI_SCHEDULE_ALARM;
        row->entry.enabled = (s[1] & 1U) != 0;
        row->entry.hour = s[2];
        row->entry.minute = s[3];
        row->entry.weekdays = s[4];
        row->fired_minute = INT64_MIN;
    }
    clock_state.count = record[1];
    clock_state.next_id = max_id + 1U;
    return XIAOTAI_OK;
}

void clock_store_save(void)
{
    uint8_t record[CLOCK_ALARM_BYTES] = {VERSION, 0};
    for (size_t i = 0; i < clock_state.count; ++i) {
        const xiaotai_schedule_t *entry = &clock_state.rows[i].entry;
        if (entry->kind != XIAOTAI_SCHEDULE_ALARM) continue;
        uint8_t *s = record + PREFIX_BYTES + (size_t)record[1]++ * RECORD_BYTES;
        s[0] = entry->id;
        s[1] = entry->enabled ? 1U : 0U;
        s[2] = entry->hour;
        s[3] = entry->minute;
        s[4] = entry->weekdays;
        memcpy(s + LABEL_OFFSET, entry->label, sizeof(entry->label));
    }
    const xiaotai_err_t err = xiaotai_kv_set_async(CLOCK_ALARM_SPACE, CLOCK_ALARM_KEY, record,
                                                 PREFIX_BYTES + (size_t)record[1] * RECORD_BYTES);
    /* set_async coalesces queued writes and only acknowledges submission;
     * the KV port reports a later flash write failure in its own counters. */
    clock_lock();
    if (err == XIAOTAI_OK) ++clock_state.stats.stored;
    else ++clock_state.stats.store_failures;
    clock_unlock();
    if (err != XIAOTAI_OK) xiaotai_eventlog_error("clock", "store_enqueue", 0, err);
}
