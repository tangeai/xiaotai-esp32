// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/port/sys.h"

typedef struct {
    uint64_t timestamp_us;
    uint64_t read_us;
    uint32_t data, psram_miss, flash_miss, instruction, instruction_miss;
    uint32_t flags, data_start, data_end, instruction_start, instruction_end;
} port_cache_report_t;

void port_report_cache(xiaotai_port_write_fn_t write, void *ctx, const port_cache_report_t *record);
void port_report_usb(xiaotai_port_write_fn_t write, void *ctx, uint64_t timestamp_us,
                     uint32_t disconnects, uint32_t history, uint32_t since_sof, uint32_t missing_ticks);
/* Each call prints one ordered list of at most 16 unsigned values. */
void port_report_series(xiaotai_port_write_fn_t write, void *ctx, const char *name,
                        const uint32_t *values, size_t count);
