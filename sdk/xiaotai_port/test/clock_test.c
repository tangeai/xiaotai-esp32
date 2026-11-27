// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_clock_fallback.py
#include "xiaotai/port/clock.h"
#include "xiaotai/port/os.h"
#include "xiaotai_clock_host.h"
#include "xiaotai_os_host.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define BOUNDARY (XIAOTAI_CLOCK_VALID_AFTER_S * 1000)
#define EPOCH 1800000000000LL
static const char *const servers[] = {"ntp.aliyun.com", "pool.ntp.org"};
static const xiaotai_clock_config_t defaults = {.servers = servers, .server_count = 2};

static xiaotai_clock_stats_t stats(void)
{
    xiaotai_clock_stats_t out;
    xiaotai_clock_stats(&out);
    return out;
}

static void boot(xiaotai_clock_config_t cfg, int64_t time)
{
    xiaotai_clock_host_reset();
    xiaotai_clock_host_set_wall_ms(time);
    assert(xiaotai_clock_init(&cfg) == XIAOTAI_OK);
    assert(xiaotai_clock_sync_budget_ms() == 50000);
    assert(!stats().starts);
}

static void gate_and_config(void)
{
    char out[XIAOTAI_CLOCK_NAME_MAX] = "dirty";
    assert(xiaotai_clock_sync(0) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_clock_sync_budget_ms() == 0);
    assert(xiaotai_clock_timezone(out, sizeof(out)) == XIAOTAI_ERR_INVALID_STATE && !out[0]);
    assert(xiaotai_clock_init(NULL) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_clock_config_t cfg = defaults;
    cfg.server_count = 0;
    assert(xiaotai_clock_init(&cfg) == XIAOTAI_ERR_INVALID_ARG);
    cfg = defaults;
    cfg.trust_kept_time = true;
    char zone[] = "CST-8";
    cfg.timezone = zone;
    boot(cfg, EPOCH);
    zone[0] = 'X';
    assert(xiaotai_clock_init(&cfg) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_clock_synced() && xiaotai_clock_sync(0) == XIAOTAI_OK && !stats().starts);
    assert(xiaotai_clock_timezone(out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, "CST-8"));
    assert(xiaotai_clock_timezone(out, 5) == XIAOTAI_ERR_INVALID_SIZE && !out[0]);
    xiaotai_clock_fields_t fields;
    assert(xiaotai_clock_local(1735675200000LL, &fields) == XIAOTAI_OK);
    assert(fields.year == 2025 && fields.month == 1 && fields.day == 1 &&
           fields.hour == 4 && fields.minute == 0 && fields.utc_offset_s == 28800);
    assert(xiaotai_clock_set_timezone(NULL) == XIAOTAI_OK);
    assert(xiaotai_clock_timezone(out, sizeof(out)) == XIAOTAI_OK && !out[0]);
    assert(xiaotai_clock_local(0, &fields) == XIAOTAI_OK && fields.year == 1970 && fields.utc_offset_s == 0);
    assert(xiaotai_clock_local(INT64_MAX, &fields) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_clock_set_timezone("EST5EDT,M3.2.0/2,M11.1.0/2") == XIAOTAI_OK);
    assert(xiaotai_clock_local(1735689600000LL, &fields) == XIAOTAI_OK &&
           fields.year == 2024 && fields.month == 12 && fields.day == 31 && fields.utc_offset_s == -18000);
    assert(xiaotai_clock_local(1751328000000LL, &fields) == XIAOTAI_OK && fields.utc_offset_s == -14400);

    boot(cfg, BOUNDARY);
    assert(!xiaotai_clock_synced());
    assert(xiaotai_clock_set_unix_ms(BOUNDARY) == XIAOTAI_ERR_INVALID_ARG);
    boot(defaults, EPOCH);
    assert(!xiaotai_clock_synced());
    assert(xiaotai_clock_set_unix_ms(EPOCH + 5) == XIAOTAI_OK);
    assert(xiaotai_clock_synced() && xiaotai_clock_unix_ms() == EPOCH + 5);
    assert(stats().last_step_ms == 5);
    xiaotai_clock_host_advance_ms(3);
    assert(xiaotai_clock_unix_ms() == EPOCH + 8);
    xiaotai_clock_host_set_wall_ms(BOUNDARY);
    assert(!xiaotai_clock_synced());
}

static void fallback_and_errors(void)
{
    const int64_t now = xiaotai_time_us();
    xiaotai_os_host_advance_us(5000000);
    assert(xiaotai_time_us() >= now + 5000000);
    boot(defaults, 0);
    xiaotai_clock_host_hold(true);
    for (unsigned i = 0; i < 3; ++i) assert(xiaotai_clock_sync(0) == XIAOTAI_ERR_TIMEOUT);
    xiaotai_clock_stats_t st = stats();
    assert(st.starts == 1 && st.running && !st.timeouts && !st.invalid && !st.syncs);
    for (unsigned i = 0; i < 3; ++i) assert(xiaotai_clock_sync(1) == XIAOTAI_ERR_TIMEOUT);
    st = stats();
    assert(st.starts == 1 && st.running && st.timeouts == 3);
    xiaotai_clock_host_answer(EPOCH, 5000);
    xiaotai_os_host_advance_us(5000001);
    assert(xiaotai_clock_sync(1000) == XIAOTAI_OK);
    st = stats();
    assert(st.starts == 1 && !st.running && st.timeouts == 3 && st.syncs == 1 && st.last_step_ms == INT32_MAX);
    assert(!xiaotai_clock_host_probes());

    boot(defaults, 0);
    xiaotai_clock_host_timeout_next(2);
    xiaotai_clock_host_answer(EPOCH, 0);
    assert(xiaotai_clock_sync(1) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_clock_sync(1) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_clock_sync(1000) == XIAOTAI_OK);
    assert(stats().timeouts == 2 && stats().starts == 1);

    boot(defaults, EPOCH);
    xiaotai_clock_host_fail_starts(1);
    assert(xiaotai_clock_sync(1000) == XIAOTAI_FAIL && !xiaotai_clock_synced());
    assert(!stats().running);
    xiaotai_clock_host_answer(BOUNDARY, 0);
    assert(xiaotai_clock_sync(1000) == XIAOTAI_FAIL && !xiaotai_clock_synced());
    assert(stats().starts == 2 && stats().invalid == 1);
    xiaotai_clock_host_answer(EPOCH, 0);
    assert(xiaotai_clock_sync(1000) == XIAOTAI_OK && stats().starts == 2);
}

static void probes(void)
{
    xiaotai_clock_config_t cfg = defaults;
    cfg.probe = true;
    boot(cfg, 0);
    xiaotai_clock_host_probe_result(true, EPOCH, 0);
    assert(xiaotai_clock_sync(1000) == XIAOTAI_OK);
    assert(xiaotai_clock_host_probes() == 1 && xiaotai_clock_unix_ms() == EPOCH);
    xiaotai_clock_stats_t st = stats();
    assert(st.starts == 1 && st.syncs == 1 && !st.running);

    cfg.keep_running = true;
    boot(cfg, 0);
    xiaotai_clock_host_probe_result(false, 0, 2);
    xiaotai_clock_host_answer(EPOCH, 0);
    assert(xiaotai_clock_sync(1000) == XIAOTAI_OK);
    st = stats();
    assert(xiaotai_clock_host_probes() == 1 && st.starts == 1 && st.syncs == 1 && st.invalid == 2 && st.running);

    boot(cfg, 0);
    xiaotai_clock_host_hold(true);
    xiaotai_clock_host_probe_result(true, EPOCH, 0);
    assert(xiaotai_clock_sync(0) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_clock_sync(1) == XIAOTAI_ERR_TIMEOUT);
    assert(!xiaotai_clock_host_probes() && stats().starts == 1);
}

static atomic_bool finished;
static atomic_bool hold_tz, inside_tz;
int __real_setenv(const char *name, const char *value, int overwrite);
int __wrap_setenv(const char *name, const char *value, int overwrite)
{
    if (atomic_load(&hold_tz)) {
        atomic_store(&inside_tz, true);
        while (atomic_load(&hold_tz)) xiaotai_delay_ms(1);
    }
    return __real_setenv(name, value, overwrite);
}

static void *set_timezone(void *unused)
{
    (void)unused;
    assert(xiaotai_clock_set_timezone("CST-8") == XIAOTAI_OK);
    return NULL;
}

static void poll_during_timezone_update(void)
{
    pthread_t worker;
    boot(defaults, 0);
    atomic_store(&hold_tz, true);
    assert(!pthread_create(&worker, NULL, set_timezone, NULL));
    const int64_t limit = xiaotai_time_us() + 1000000;
    while (!atomic_load(&inside_tz) && xiaotai_time_us() < limit) xiaotai_delay_ms(1);
    assert(atomic_load(&inside_tz));
    assert(xiaotai_clock_sync(0) == XIAOTAI_ERR_TIMEOUT);
    atomic_store(&hold_tz, false);
    assert(!pthread_join(worker, NULL));
    assert(stats().starts == 0 && stats().timeouts == 0);
}

static void *wait_sync(void *unused)
{
    (void)unused;
    assert(xiaotai_clock_sync(4000) == XIAOTAI_OK);
    atomic_store(&finished, true);
    return NULL;
}

static void timezone_during_wait(void)
{
    pthread_t worker;
    boot(defaults, 0);
    xiaotai_clock_host_hold(true);
    assert(!pthread_create(&worker, NULL, wait_sync, NULL));
    int64_t limit = xiaotai_time_us() + 500000;
    while (!stats().running && xiaotai_time_us() < limit) xiaotai_delay_ms(1);
    assert(stats().running && !atomic_load(&finished));
    assert(xiaotai_clock_set_timezone("CST-8") == XIAOTAI_OK);
    assert(xiaotai_clock_sync(0) == XIAOTAI_ERR_TIMEOUT && !atomic_load(&finished));
    xiaotai_clock_host_answer(EPOCH, 0);
    assert(!pthread_join(worker, NULL) && atomic_load(&finished));
    assert(stats().starts == 1 && !stats().timeouts);
}

int main(void)
{
    gate_and_config();
    fallback_and_errors();
    probes();
    poll_during_timezone_update();
    timezone_during_wait();
    xiaotai_clock_host_reset();
    return 0;
}
