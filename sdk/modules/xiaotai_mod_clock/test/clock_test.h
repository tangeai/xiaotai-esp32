// SPDX-License-Identifier: MIT
#pragma once

#include <assert.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/modules/clock.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_clock_host.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_os_host.h"

#define SUN_2026_01_04 INT64_C(1767484800000)

extern unsigned clock_init_calls, clock_server_count;
extern char clock_tz[XIAOTAI_CLOCK_NAME_MAX];
extern char clock_hosts[XIAOTAI_CLOCK_SERVERS_MAX][XIAOTAI_CLOCK_NAME_MAX];
extern struct seen_events {
    unsigned syncs, minutes, schedules, due;
    uint8_t ids[300];
    bool again;         /* a due event announced again, without a fired one */
    uint8_t fired_id;   /* announced by the fired event, its due event not seen yet */
    xiaotai_wallclock_t minute;
} seen;

void prepare(int64_t wall_ms);
xiaotai_err_t start(const xiaotai_clock_module_config_t *config, const char *platform_tz, bool initially_synced);
void stop(void);
void run(void (*fn)(void *), void *ctx);
void advance(int64_t mono_ms, int64_t wall_ms);
void drain(void);
uint8_t add(xiaotai_schedule_t entry);
void set_zone(const char *text);
xiaotai_clock_module_stats_t stats(void);
xiaotai_schedule_info_t get(uint8_t id);
