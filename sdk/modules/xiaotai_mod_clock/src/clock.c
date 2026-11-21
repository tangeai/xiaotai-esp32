// SPDX-License-Identifier: MIT
#include "clock_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/port/mem.h"

static const char *const clock_servers[] = {"ntp.aliyun.com", "pool.ntp.org"};
static const char clock_source[] = "clock";

struct clock_state clock_state;
static xiaotai_clock_module_config_t config;
static char initial_timezone[XIAOTAI_CLOCK_NAME_MAX];
static atomic_bool initialized;
static bool self_sync, started, seen_sync, last_synced;
static int64_t started_ms, last_mono_ms, last_wall_ms, last_minute, retry_at_ms, timer_at_ms;
static uint32_t timer_id;

void clock_lock(void) { (void)xiaotai_mutex_lock(&clock_state.lock, XIAOTAI_WAIT_FOREVER); }
void clock_unlock(void) { xiaotai_mutex_unlock(&clock_state.lock); }
bool clock_initialized(void) { return atomic_load(&initialized); }

bool xiaotai_wallclock_now(xiaotai_wallclock_t *out)
{
    if (!out) return false;
    const bool gate = xiaotai_clock_synced();
    *out = (xiaotai_wallclock_t){.unix_ms = xiaotai_clock_unix_ms()};
    if (xiaotai_clock_local(out->unix_ms, &out->local) == XIAOTAI_OK) {
        out->synced = gate && out->unix_ms > XIAOTAI_CLOCK_VALID_AFTER_S * INT64_C(1000) && xiaotai_clock_synced();
    }
    return out->synced;
}

int64_t clock_local_ms(const xiaotai_wallclock_t *wall)
{
    const int64_t offset = (int64_t)wall->local.utc_offset_s * 1000;
    return offset > 0 && wall->unix_ms > INT64_MAX - offset ? INT64_MAX :
           offset < 0 && wall->unix_ms < INT64_MIN - offset ? INT64_MIN : wall->unix_ms + offset;
}

int64_t clock_minute_at(const xiaotai_wallclock_t *wall)
{
    const int64_t local = clock_local_ms(wall);
    return local / CLOCK_MINUTE_MS - (local % CLOCK_MINUTE_MS < 0);
}

void xiaotai_clock_module_stats(xiaotai_clock_module_stats_t *out)
{
    if (!out) return;
    *out = (xiaotai_clock_module_stats_t){0};
    if (!atomic_load(&initialized)) return;
    clock_lock();
    *out = clock_state.stats;
    clock_unlock();
}

void clock_refused(void)
{
    clock_lock();
    ++clock_state.stats.refused;
    clock_unlock();
}

static void emit(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    const xiaotai_err_t err = xiaotai_event_emit(name, args, data, size);
    if (err != XIAOTAI_OK) xiaotai_eventlog_error(clock_source, "event", 0, err);
}

static void minute_event(int64_t key, const xiaotai_wallclock_t *wall)
{
    last_minute = key;
    clock_lock();
    ++clock_state.stats.minutes;
    clock_unlock();
    const xiaotai_args_t args = {.value = wall->local.hour * 60 + wall->local.minute};
    emit(XIAOTAI_EVENT_CLOCK_MINUTE, &args, wall, sizeof(*wall));
}

static void reschedule_at(int64_t mono_ms, const xiaotai_wallclock_t *wall);

static void tick(bool timezone_changed)
{
    if (!started) return;
    const int64_t mono_ms = xiaotai_time_us() / 1000;
    xiaotai_wallclock_t now;
    xiaotai_wallclock_now(&now);
    if (!now.synced && self_sync && mono_ms >= retry_at_ms) {
        clock_lock();
        ++clock_state.stats.sync_requests;
        clock_unlock();
        const xiaotai_err_t err = xiaotai_clock_sync(0);
        if (err != XIAOTAI_OK && err != XIAOTAI_ERR_TIMEOUT) xiaotai_eventlog_error(clock_source, "sync", 0, err);
        retry_at_ms = mono_ms + (int64_t)(config.sync_retry_s ? config.sync_retry_s : 5U) * 1000;
        xiaotai_wallclock_now(&now);
    }

    int64_t missed_from = now.unix_ms;
    if (now.synced) {
        bool stepped = timezone_changed;
        if (last_synced) {
            const int64_t elapsed = mono_ms - last_mono_ms;
            const int64_t drift = (now.unix_ms - last_wall_ms) - elapsed;
            stepped |= drift > 2000 || drift < -2000;
            if (!stepped) missed_from = last_wall_ms;
        } else if (!seen_sync && !timezone_changed) {
            /* The first trusted wall reading can date elapsed time from this
             * boot; never interpret an untrusted RTC value as prior history. */
            const int64_t elapsed = mono_ms - started_ms;
            if (elapsed > 0 && elapsed < now.unix_ms) missed_from = now.unix_ms - elapsed;
        }
        const int64_t minute = clock_minute_at(&now);
        if (stepped) {
            clock_lock();
            ++clock_state.stats.steps;
            clock_unlock();
        }
        if (!seen_sync) {
            seen_sync = true;
            emit(XIAOTAI_EVENT_CLOCK_SYNCED, NULL, NULL, 0);
        }
        if (!last_synced || stepped || last_minute != minute) minute_event(minute, &now);
    }
    last_synced = now.synced;
    if (timezone_changed && !now.synced) started_ms = mono_ms;
    last_mono_ms = mono_ms;
    last_wall_ms = now.unix_ms;
    clock_schedule_tick(mono_ms, &now, missed_from);
    reschedule_at(mono_ms, &now);
}

static void on_timer(void *unused)
{
    (void)unused;
    timer_id = 0;
    tick(false);
}

static void arm_timer(uint32_t wait_ms)
{
    int64_t target_ms = xiaotai_time_us() / 1000 + wait_ms;
    if (timer_id && timer_at_ms <= target_ms) return;
    xiaotai_timer_cancel(timer_id);
    timer_id = 0;
    const xiaotai_err_t err = xiaotai_timer_start(wait_ms, on_timer, NULL, &timer_id);
    if (err == XIAOTAI_OK) timer_at_ms = target_ms;
    else xiaotai_eventlog_error(clock_source, "timer", 0, err);
}

static void reschedule_at(int64_t mono_ms, const xiaotai_wallclock_t *wall)
{
    uint32_t wait_ms = 1000U;
    if (wall->synced) {
        int64_t phase = clock_local_ms(wall) % CLOCK_MINUTE_MS;
        if (phase < 0) phase += CLOCK_MINUTE_MS;
        wait_ms = (uint32_t)(CLOCK_MINUTE_MS - phase);
    } else if (self_sync && retry_at_ms > mono_ms && retry_at_ms - mono_ms < wait_ms) {
        wait_ms = (uint32_t)(retry_at_ms - mono_ms);
    }
    wait_ms = clock_countdown_wait_ms(mono_ms, wait_ms);
    if (wait_ms < 20U) wait_ms = 20U;
    arm_timer(wait_ms);
}

void clock_reschedule(void)
{
    if (started) arm_timer(20U);
}

static xiaotai_err_t apply_timezone(bool refresh)
{
    char selected[XIAOTAI_CLOCK_NAME_MAX], current[XIAOTAI_CLOCK_NAME_MAX];
    xiaotai_err_t err = xiaotai_setting_get_text(XIAOTAI_SETTING_CLOCK_TIMEZONE, selected, sizeof(selected));
    if (err != XIAOTAI_OK) return err;
    const char *rule = selected[0] ? selected : initial_timezone;
    err = xiaotai_clock_timezone(current, sizeof(current));
    if (err != XIAOTAI_OK || strcmp(rule, current) == 0) return err;
    err = xiaotai_clock_set_timezone(rule);
    if (err == XIAOTAI_OK && refresh) tick(true);
    return err;
}

static void on_timezone(const char *name, void *unused)
{
    (void)name;
    (void)unused;
    xiaotai_err_t err = apply_timezone(true);
    if (err != XIAOTAI_OK) xiaotai_eventlog_error(clock_source, "timezone", 0, err);
}

xiaotai_err_t xiaotai_clock_module_configure(const xiaotai_clock_module_config_t *given)
{
    if (atomic_load(&initialized)) return XIAOTAI_ERR_INVALID_STATE;
    if (!given) {
        config = (xiaotai_clock_module_config_t){0};
        return XIAOTAI_OK;
    }
    if ((given->timezone && strlen(given->timezone) >= XIAOTAI_CLOCK_NAME_MAX) ||
        given->time_server_count > XIAOTAI_CLOCK_SERVERS_MAX || (given->time_server_count && !given->time_servers)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < given->time_server_count; ++i) {
        const char *name = given->time_servers[i];
        if (!name || !*name || strlen(name) >= XIAOTAI_CLOCK_NAME_MAX) return XIAOTAI_ERR_INVALID_ARG;
    }
    config = *given;
    return XIAOTAI_OK;
}

static xiaotai_err_t clock_port_init(void)
{
    self_sync = !xiaotai_module_present("platform");
    if (!self_sync && (config.timezone || config.time_servers || config.time_server_count)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_err_t err = xiaotai_clock_timezone(initial_timezone, sizeof(initial_timezone));
    if (err != XIAOTAI_ERR_INVALID_STATE || !self_sync) return err;
    const xiaotai_clock_config_t port = {
        .timezone = config.timezone ? config.timezone : "CST-8",
        .servers = config.time_server_count ? config.time_servers : clock_servers,
        .server_count = config.time_server_count ? config.time_server_count : sizeof(clock_servers) / sizeof(clock_servers[0]),
        .keep_running = true,
    };
    err = xiaotai_clock_init(&port);
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_INVALID_STATE) return err;
    return xiaotai_clock_timezone(initial_timezone, sizeof(initial_timezone));
}

xiaotai_err_t xiaotai_module_clock_init(void)
{
    if (atomic_load(&initialized)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t err = clock_port_init();
    if (err != XIAOTAI_OK) return err;
    clock_state.rows = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_SCHEDULE_MAX, sizeof(*clock_state.rows));
    if (!clock_state.rows) return XIAOTAI_ERR_NO_MEM;
    clock_state.next_id = 1;
    err = xiaotai_mutex_init(&clock_state.lock);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(clock_state.rows);
        clock_state.rows = NULL;
        return err;
    }
    err = clock_store_load();
    if (err == XIAOTAI_OK && config.timezone_setting) {
        const xiaotai_setting_t timezone = {.name = XIAOTAI_SETTING_CLOCK_TIMEZONE, .key = "clock_tz",
            .kind = XIAOTAI_SETTING_TEXT, .default_text = "", .changed = on_timezone};
        err = xiaotai_setting_register_text(&timezone, XIAOTAI_CLOCK_NAME_MAX - 1U);
        if (err == XIAOTAI_OK) {
            const xiaotai_err_t applied = apply_timezone(false);
            if (applied != XIAOTAI_OK) xiaotai_eventlog_error(clock_source, "timezone", 0, applied);
        }
    }
    if (err == XIAOTAI_OK) err = clock_schedule_intents();
    if (err == XIAOTAI_OK) err = clock_at_register();
    if (err != XIAOTAI_OK) {
        xiaotai_mutex_deinit(&clock_state.lock);
        xiaotai_mem_free(clock_state.rows);
        memset(&clock_state, 0, sizeof(clock_state));
        return err;
    }
    atomic_store(&initialized, true);
    return XIAOTAI_OK;
}

void xiaotai_module_clock_start(void)
{
    if (!atomic_load(&initialized) || started) return;
    started = true;
    started_ms = xiaotai_time_us() / 1000;
    retry_at_ms = started_ms;
    tick(false);
}

void xiaotai_clock_module_reset(void)
{
    if (timer_id && xiaotai_on_owner()) xiaotai_timer_cancel(timer_id);
    timer_id = 0;
    if (atomic_exchange(&initialized, false)) xiaotai_mutex_deinit(&clock_state.lock);
    xiaotai_mem_free(clock_state.rows);
    memset(&clock_state, 0, sizeof(clock_state));
    config = (xiaotai_clock_module_config_t){0};
    seen_sync = self_sync = last_synced = started = false;
    initial_timezone[0] = '\0';
}
