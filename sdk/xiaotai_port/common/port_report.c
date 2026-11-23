// SPDX-License-Identifier: MIT
#include "port_report.h"

#include <inttypes.h>
#include <stdio.h>

void port_report_cache(xiaotai_port_write_fn_t write, void *ctx, const port_cache_report_t *record)
{
    char line[176];
    snprintf(line, sizeof(line), "cache_t_us=%" PRIu64 " cache_read_us=%" PRIu64
             " cache_data=%" PRIu32 " cache_psram_miss=%" PRIu32 " cache_flash_miss=%" PRIu32,
             record->timestamp_us, record->read_us, record->data, record->psram_miss, record->flash_miss);
    write(ctx, line);
    snprintf(line, sizeof(line), "cache_instruction=%" PRIu32 " cache_instruction_miss=%" PRIu32
             " cache_flags=%08" PRIx32 " cache_data_filter=%08" PRIx32 "-%08" PRIx32
             " cache_instruction_filter=%08" PRIx32 "-%08" PRIx32, record->instruction,
             record->instruction_miss, record->flags, record->data_start, record->data_end,
             record->instruction_start, record->instruction_end);
    write(ctx, line);
}

void port_report_usb(xiaotai_port_write_fn_t write, void *ctx, uint64_t timestamp_us,
                     uint32_t disconnects, uint32_t history, uint32_t since_sof, uint32_t missing_ticks)
{
    char line[152];
    snprintf(line, sizeof(line), "usb_disconnects=%" PRIu32 " usb_sof_history=%08" PRIx32
             " usb_since_sof_us=%" PRIu32 " usb_missing_ticks=%" PRIu32 " usb_t_us=%" PRIu64,
             disconnects, history, since_sof, missing_ticks, timestamp_us);
    write(ctx, line);
}

void port_report_series(xiaotai_port_write_fn_t write, void *ctx, const char *name,
                        const uint32_t *values, size_t count)
{
    char line[200]; /* Key, 16 ten-digit samples, commas, and terminator. */
    int n = snprintf(line, sizeof(line), "%s=", name);
    if (n < 0 || (size_t)n >= sizeof(line)) return;
    size_t cursor = (size_t)n;
    for (size_t i = 0; i < count; ++i) {
        n = snprintf(line + cursor, sizeof(line) - cursor, "%s%" PRIu32, i ? "," : "", values[i]);
        if (n < 0 || (size_t)n >= sizeof(line) - cursor) return;
        cursor += (size_t)n;
    }
    write(ctx, line);
}
