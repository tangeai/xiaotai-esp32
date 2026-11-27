// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "clock_internal.h"
#include "xiaotai_clock_host.h"
#include "xiaotai/port/os.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t s_host_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool fixed, adjusted, starting, running, stop_after_start, pumping, hold, answer_set, probe_ok;
    unsigned fail_starts, timeouts, probes;
    uint32_t delay_ms, probe_invalid;
    int64_t wall, wall_at_ms, due_us, answer, probe_result;
} s_host;

static int64_t sum(int64_t base, int64_t delta)
{
    if (delta > 0 && base > INT64_MAX - delta) return INT64_MAX;
    if (delta < 0 && base < INT64_MIN - delta) return INT64_MIN;
    return base + delta;
}

static int64_t wall_locked(void)
{
    if (s_host.fixed) return s_host.wall;
    if (s_host.adjusted) return sum(s_host.wall, xiaotai_time_us() / 1000 - s_host.wall_at_ms);
    struct timespec wall;
    return clock_gettime(CLOCK_REALTIME, &wall) == 0 ? (int64_t)wall.tv_sec * 1000 + wall.tv_nsec / 1000000 : 0;
}

static void set_wall_locked(int64_t unix_ms)
{
    s_host.adjusted = true;
    s_host.wall = unix_ms;
    s_host.wall_at_ms = xiaotai_time_us() / 1000;
}

int64_t clock_wall_ms(void)
{
    pthread_mutex_lock(&s_host_lock);
    const int64_t now = wall_locked();
    pthread_mutex_unlock(&s_host_lock);
    return now;
}

bool clock_wall_set(int64_t unix_ms)
{
    pthread_mutex_lock(&s_host_lock);
    set_wall_locked(unix_ms);
    pthread_mutex_unlock(&s_host_lock);
    return true;
}

uint32_t clock_service_budget(size_t servers) { return ((uint32_t)servers + 1U) * 15000U + 5000U; }

bool clock_service_start(const clock_servers_t *servers, bool release_after_start)
{
    (void)servers;
    pthread_mutex_lock(&s_host_lock);
    s_host.starting = true;
    s_host.stop_after_start = release_after_start;
    s_host.due_us = sum(xiaotai_time_us(), (int64_t)s_host.delay_ms * 1000);
    pthread_mutex_unlock(&s_host_lock);
    return true;
}

void clock_service_poll(void)
{
    pthread_mutex_lock(&s_host_lock);
    if (s_host.pumping) {
        pthread_mutex_unlock(&s_host_lock);
        return;
    }
    bool start = s_host.starting, stop = false, start_ok = true;
    bool answer = false;
    int64_t before = 0, after = 0;
    if (start) {
        s_host.starting = false;
        if (s_host.fail_starts) {
            --s_host.fail_starts;
            start_ok = false;
        }
        stop = s_host.stop_after_start && start_ok;
        s_host.running = start_ok && !stop;
    }
    if (s_host.running && !s_host.hold && !s_host.timeouts && xiaotai_time_us() >= s_host.due_us) {
        before = wall_locked();
        after = s_host.answer_set ? s_host.answer : before;
        if (s_host.answer_set) set_wall_locked(after);
        s_host.hold = true;
        answer = true;
    }
    s_host.pumping = start || answer;
    if (!s_host.pumping) {
        pthread_mutex_unlock(&s_host_lock);
        return;
    }
    pthread_mutex_unlock(&s_host_lock);
    if (start) clock_service_started(start_ok);
    if (answer) stop = clock_service_answer(before, after);
    if (stop) clock_service_stopped();
    pthread_mutex_lock(&s_host_lock);
    if (stop) s_host.running = false;
    s_host.pumping = false;
    pthread_mutex_unlock(&s_host_lock);
}

void clock_service_wait_timed_out(void)
{
    pthread_mutex_lock(&s_host_lock);
    if (s_host.timeouts) --s_host.timeouts;
    pthread_mutex_unlock(&s_host_lock);
}

bool clock_parallel_probe(const clock_servers_t *servers, int64_t *unix_ms, uint32_t *invalid)
{
    (void)servers;
    pthread_mutex_lock(&s_host_lock);
    ++s_host.probes;
    *unix_ms = s_host.probe_result;
    *invalid = s_host.probe_invalid;
    bool ok = s_host.probe_ok;
    pthread_mutex_unlock(&s_host_lock);
    return ok;
}

void xiaotai_clock_host_reset(void)
{
    clock_fixture_reset();
    /* No clock calls are in flight during fixture reset. */
    memset(&s_host, 0, sizeof(s_host));
}

void xiaotai_clock_host_set_wall_ms(int64_t unix_ms)
{
    pthread_mutex_lock(&s_host_lock);
    s_host.fixed = true;
    s_host.wall = unix_ms;
    pthread_mutex_unlock(&s_host_lock);
}

void xiaotai_clock_host_advance_ms(int64_t delta)
{
    pthread_mutex_lock(&s_host_lock);
    s_host.wall = sum(wall_locked(), delta);
    s_host.fixed = true;
    pthread_mutex_unlock(&s_host_lock);
}

void xiaotai_clock_host_answer(int64_t wall_ms, uint32_t delay_ms)
{
    pthread_mutex_lock(&s_host_lock);
    s_host.answer_set = true;
    s_host.answer = wall_ms;
    s_host.delay_ms = delay_ms;
    s_host.due_us = sum(xiaotai_time_us(), (int64_t)delay_ms * 1000);
    s_host.hold = false;
    pthread_mutex_unlock(&s_host_lock);
}

void xiaotai_clock_host_hold(bool hold)
{
    pthread_mutex_lock(&s_host_lock);
    s_host.hold = hold;
    pthread_mutex_unlock(&s_host_lock);
}

void xiaotai_clock_host_timeout_next(unsigned count)
{
    pthread_mutex_lock(&s_host_lock);
    s_host.timeouts = count;
    pthread_mutex_unlock(&s_host_lock);
}

void xiaotai_clock_host_fail_starts(unsigned count)
{
    pthread_mutex_lock(&s_host_lock);
    s_host.fail_starts = count;
    pthread_mutex_unlock(&s_host_lock);
}

void xiaotai_clock_host_probe_result(bool accepted, int64_t wall_ms, uint32_t invalid)
{
    pthread_mutex_lock(&s_host_lock);
    s_host.probe_ok = accepted;
    s_host.probe_result = wall_ms;
    s_host.probe_invalid = invalid;
    pthread_mutex_unlock(&s_host_lock);
}

unsigned xiaotai_clock_host_probes(void)
{
    pthread_mutex_lock(&s_host_lock);
    unsigned count = s_host.probes;
    pthread_mutex_unlock(&s_host_lock);
    return count;
}
