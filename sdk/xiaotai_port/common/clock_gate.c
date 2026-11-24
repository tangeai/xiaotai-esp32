// SPDX-License-Identifier: MIT
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "clock_internal.h"
#include "xiaotai/port/os.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VALID_AFTER_MS (XIAOTAI_CLOCK_VALID_AFTER_S * 1000LL)
#define RESPONSE 1U

static xiaotai_mutex_t s_lock;
static xiaotai_events_t s_changed;
static clock_servers_t s_servers;
static char s_zone[XIAOTAI_CLOCK_NAME_MAX];
static xiaotai_clock_stats_t s_stats;
static bool s_ready, s_confirmed, s_trust, s_probe, s_probing, s_starting;
static uint32_t s_failures;

static void lock(void) { (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&s_lock); }
static bool synced_locked(void) { return (s_confirmed || s_trust) && clock_wall_ms() > VALID_AFTER_MS; }

static xiaotai_err_t timezone_locked(const char *zone)
{
    if (setenv("TZ", zone[0] ? zone : "UTC0", 1) != 0) return XIAOTAI_FAIL;
    tzset();
    memcpy(s_zone, zone, strlen(zone) + 1);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_clock_init(const xiaotai_clock_config_t *config)
{
    if (s_ready) return XIAOTAI_ERR_INVALID_STATE;
    if (!config || !config->servers || !config->server_count || config->server_count > XIAOTAI_CLOCK_SERVERS_MAX)
        return XIAOTAI_ERR_INVALID_ARG;
    const char *zone = config->timezone ? config->timezone : "";
    if (strlen(zone) >= sizeof(s_zone)) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < config->server_count; ++i) {
        const char *name = config->servers[i];
        if (!name || !*name || strlen(name) >= XIAOTAI_CLOCK_NAME_MAX) return XIAOTAI_ERR_INVALID_ARG;
    }
    xiaotai_err_t err = xiaotai_mutex_init(&s_lock);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_events_init(&s_changed);
    if (err != XIAOTAI_OK) {
        xiaotai_mutex_deinit(&s_lock);
        return err;
    }
    err = timezone_locked(zone);
    if (err != XIAOTAI_OK) {
        xiaotai_events_deinit(&s_changed);
        xiaotai_mutex_deinit(&s_lock);
        return err;
    }
    s_servers.count = config->server_count;
    s_servers.keep_running = config->keep_running;
    for (size_t i = 0; i < s_servers.count; ++i) strcpy(s_servers.servers[i], config->servers[i]);
    s_probe = config->probe;
    s_trust = config->trust_kept_time;
    s_ready = true;
    return XIAOTAI_OK;
}

static void synced_at(int64_t before, int64_t after)
{
    s_confirmed = true;
    ++s_stats.syncs;
    s_stats.last_sync_uptime_us = xiaotai_time_us();
    /* Compare before subtracting: a caller cannot overflow the statistic. */
    if (after >= before) {
        const uint64_t delta = (uint64_t)after - (uint64_t)before;
        s_stats.last_step_ms = delta > INT32_MAX ? INT32_MAX : (int32_t)delta;
    } else {
        const uint64_t delta = (uint64_t)before - (uint64_t)after;
        s_stats.last_step_ms = delta > INT32_MAX ? INT32_MIN : -(int32_t)delta;
    }
    xiaotai_events_set(&s_changed, RESPONSE);
}

static void failed_locked(void)
{
    ++s_failures;
    xiaotai_events_set(&s_changed, RESPONSE);
}

void clock_service_started(bool ok)
{
    lock();
    s_starting = false;
    s_stats.running = ok;
    if (!ok) failed_locked();
    unlock();
}

bool clock_service_answer(int64_t before_ms, int64_t after_ms)
{
    lock();
    const bool valid = after_ms > VALID_AFTER_MS && clock_wall_ms() > VALID_AFTER_MS;
    if (valid) synced_at(before_ms, after_ms);
    else {
        ++s_stats.invalid;
        failed_locked();
    }
    const bool stop = valid && !s_servers.keep_running;
    unlock();
    return stop;
}

void clock_service_stopped(void)
{
    lock();
    s_stats.running = false;
    unlock();
}

static bool start_locked(bool release_after_start)
{
    if (s_stats.running || s_starting) return true;
    s_starting = clock_service_start(&s_servers, release_after_start);
    if (s_starting) ++s_stats.starts;
    else failed_locked();
    return s_starting;
}

bool xiaotai_clock_synced(void)
{
    if (!s_ready) return false;
    clock_service_poll();
    lock();
    const bool synced = synced_locked();
    unlock();
    return synced;
}

xiaotai_err_t xiaotai_clock_sync(uint32_t timeout_ms)
{
    if (!s_ready) return XIAOTAI_ERR_INVALID_STATE;
    /* A zero wait is a snapshot; in particular it must not run a probe. */
    if (timeout_ms) {
        clock_service_poll();
        lock();
    } else if (!xiaotai_mutex_lock(&s_lock, 0)) {
        return XIAOTAI_ERR_TIMEOUT;
    }
    if (synced_locked()) {
        unlock();
        return XIAOTAI_OK;
    }
    const uint32_t failures = s_failures;
    if (timeout_ms && s_probe && !s_probing && !s_starting && !s_stats.running) {
        s_probing = true;
        unlock();
        int64_t sample = 0;
        uint32_t invalid = 0;
        bool found = clock_parallel_probe(&s_servers, &sample, &invalid);
        lock();
        s_probing = false;
        s_stats.invalid += invalid;
        if (found && !synced_locked()) {
            const int64_t before = clock_wall_ms();
            if (sample > VALID_AFTER_MS && clock_wall_set(sample)) synced_at(before, sample);
            else ++s_stats.invalid;
        }
        (void)start_locked(synced_locked() && !s_servers.keep_running);
    } else if (!s_probing) {
        (void)start_locked(false);
    }
    const int64_t budget = (int64_t)timeout_ms * 1000;
    const int64_t started = xiaotai_time_us();
    const int64_t deadline = started > INT64_MAX - budget ? INT64_MAX : started + budget;
    for (;;) {
        if (synced_locked()) {
            if (timeout_ms && s_stats.last_sync_uptime_us > deadline) {
                ++s_stats.timeouts;
                clock_service_wait_timed_out();
                unlock();
                return XIAOTAI_ERR_TIMEOUT;
            }
            break;
        }
        if (failures != s_failures) {
            unlock();
            return XIAOTAI_FAIL;
        }
        const int64_t left = deadline - xiaotai_time_us();
        if (left <= 0 || !timeout_ms) {
            if (timeout_ms) {
                ++s_stats.timeouts;
                clock_service_wait_timed_out();
            }
            unlock();
            return XIAOTAI_ERR_TIMEOUT;
        }
        xiaotai_events_clear(&s_changed, RESPONSE);
        unlock();
        clock_service_poll();
        /* Poll margin also lets another waiter observe an answer after a peer cleared the bit. */
        const uint32_t wait_ms = left > 20000 ? 20U : (uint32_t)((left + 999) / 1000);
        (void)xiaotai_events_wait(&s_changed, RESPONSE, false, false, wait_ms);
        lock();
    }
    unlock();
    return XIAOTAI_OK;
}

uint32_t xiaotai_clock_sync_budget_ms(void)
{
    return s_ready ? clock_service_budget(s_servers.count) : 0;
}

int64_t xiaotai_clock_unix_ms(void) { return clock_wall_ms(); }

xiaotai_err_t xiaotai_clock_set_unix_ms(int64_t unix_ms)
{
    if (unix_ms <= VALID_AFTER_MS) return XIAOTAI_ERR_INVALID_ARG;
    if (!s_ready) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    int64_t before = clock_wall_ms();
    bool ok = clock_wall_set(unix_ms);
    if (ok) synced_at(before, unix_ms);
    unlock();
    return ok ? XIAOTAI_OK : XIAOTAI_FAIL;
}

xiaotai_err_t xiaotai_clock_set_timezone(const char *timezone)
{
    const char *zone = timezone ? timezone : "";
    if (strlen(zone) >= sizeof(s_zone)) return XIAOTAI_ERR_INVALID_ARG;
    if (!s_ready) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    xiaotai_err_t err = timezone_locked(zone);
    unlock();
    return err;
}

xiaotai_err_t xiaotai_clock_timezone(char *out, size_t capacity)
{
    if (!out || !capacity) return XIAOTAI_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!s_ready) return XIAOTAI_ERR_INVALID_STATE;
    lock();
    const size_t size = strlen(s_zone) + 1;
    if (size <= capacity) memcpy(out, s_zone, size);
    unlock();
    return size <= capacity ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE;
}

static int days_in_year(int year)
{
    return 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

xiaotai_err_t xiaotai_clock_local(int64_t unix_ms, xiaotai_clock_fields_t *fields)
{
    if (!fields || unix_ms < 0) return XIAOTAI_ERR_INVALID_ARG;
    if (!s_ready) return XIAOTAI_ERR_INVALID_STATE;
    const int64_t seconds = unix_ms / 1000;
    const time_t instant = (time_t)seconds;
    if ((int64_t)instant != seconds) return XIAOTAI_ERR_INVALID_ARG;
    struct tm local, utc;
    lock();
    const bool ok = localtime_r(&instant, &local) && gmtime_r(&instant, &utc);
    unlock();
    if (!ok || local.tm_year > INT16_MAX - 1900 || local.tm_year < 70)
        return XIAOTAI_ERR_INVALID_ARG;
    int days = local.tm_yday - utc.tm_yday;
    if (local.tm_year > utc.tm_year) days += days_in_year(utc.tm_year + 1900);
    if (local.tm_year < utc.tm_year) days -= days_in_year(local.tm_year + 1900);
    *fields = (xiaotai_clock_fields_t){
        .year = (int16_t)(local.tm_year + 1900), .month = (uint8_t)(local.tm_mon + 1),
        .day = (uint8_t)local.tm_mday, .hour = (uint8_t)local.tm_hour, .minute = (uint8_t)local.tm_min,
        .second = (uint8_t)local.tm_sec, .weekday = (uint8_t)local.tm_wday,
        .utc_offset_s = days * 86400 + (local.tm_hour - utc.tm_hour) * 3600 +
                        (local.tm_min - utc.tm_min) * 60 + local.tm_sec - utc.tm_sec,
    };
    return XIAOTAI_OK;
}

void xiaotai_clock_stats(xiaotai_clock_stats_t *stats)
{
    if (!stats) return;
    if (!s_ready) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    clock_service_poll();
    lock();
    *stats = s_stats;
    unlock();
}

#ifdef XIAOTAI_CLOCK_TEST_CONTROLS
/* Called only by the host fixture after clock users have stopped. */
void clock_fixture_reset(void)
{
    if (!s_ready) return;
    xiaotai_events_deinit(&s_changed);
    xiaotai_mutex_deinit(&s_lock);
    s_ready = s_confirmed = s_trust = s_probe = s_probing = s_starting = false;
    s_failures = 0;
    s_stats = (xiaotai_clock_stats_t){0};
}
#endif
