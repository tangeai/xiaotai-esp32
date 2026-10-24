// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int volume;
    int gain_db;
    bool guidance_enabled;
    bool loaded;
    bool saved;
    bool pending;
    bool quiesced;
    bool attempt_pending;
    esp_err_t error;
    unsigned revision;
} xmini_settings_status_t;

/* Missing storage uses firmware defaults volume=40/gain=24/guidance=on without writing.
 * Invalid storage and NVS initialization errors are returned without erase. */
esp_err_t xmini_settings_load(int *volume, int *gain_db);
/* Thread-safe, nonblocking. A change replaces the pending snapshot and waits
 * one second. Repeating a failed snapshot explicitly retries that write. */
esp_err_t xmini_settings_schedule(int volume, int gain_db);
/* Automatic spoken guidance only. Text, alarms, RTC and explicit replay stay on. */
esp_err_t xmini_settings_schedule_guidance(bool enabled);
/* One maintenance owner freezes new changes while process drains the already
 * accepted snapshot. Failures require an explicit owner retry or end/change. */
esp_err_t xmini_settings_quiesce_begin(void);
void xmini_settings_quiesce_end(void);
/* The quiesce owner may explicitly retry its failed, already accepted snapshot
 * without reopening admission. Never called automatically by process. */
esp_err_t xmini_settings_quiesce_retry(void);
/* Call only on the control task. NVS is never written by schedule/callbacks. */
void xmini_settings_process(void);
xmini_settings_status_t xmini_settings_status(void);
