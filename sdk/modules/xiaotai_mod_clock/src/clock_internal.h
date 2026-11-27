// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/framework.h"
#include "xiaotai/modules/clock.h"
#include "xiaotai/port/os.h"

#define CLOCK_ALARM_SPACE "xt_clock"
#define CLOCK_ALARM_KEY "alarms"
#define CLOCK_ALARM_BYTES (2U + 30U * XIAOTAI_SCHEDULE_MAX)
#define CLOCK_MINUTE_MS INT64_C(60000)

typedef struct {
    xiaotai_schedule_t entry;
    int64_t armed_ms;
    union {
        int64_t fired_minute; /* local civil minute high-water mark, incl. DST fold */
        int64_t deadline_ms;  /* monotonic countdown deadline */
    };
} clock_schedule_row_t;

extern struct clock_state {
    xiaotai_mutex_t lock;
    clock_schedule_row_t *rows;
    xiaotai_clock_module_stats_t stats;
    size_t count;
    unsigned next_id;
} clock_state;

void clock_lock(void);
void clock_unlock(void);
bool clock_initialized(void);
int64_t clock_local_ms(const xiaotai_wallclock_t *wall);
int64_t clock_minute_at(const xiaotai_wallclock_t *wall);
void clock_reschedule(void);
void clock_schedules_changed(bool persist);
void clock_refused(void);
bool clock_label_copy(char out[XIAOTAI_SCHEDULE_LABEL_MAX + 1U], const char *text);
void clock_schedule_tick(int64_t mono_ms, const xiaotai_wallclock_t *wall, int64_t missed_from_ms);
uint32_t clock_countdown_wait_ms(int64_t mono_ms, uint32_t limit);
xiaotai_err_t clock_schedule_intents(void);
xiaotai_err_t clock_store_load(void);
void clock_store_save(void);
xiaotai_err_t clock_at_register(void);
