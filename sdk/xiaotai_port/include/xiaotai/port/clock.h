// SPDX-License-Identifier: MIT
#pragma once
/* Clock port: wall-clock time and its synchronization gate.
 *
 * Monotonic time is xiaotai_time_us() (xiaotai/port/os.h); this port is the
 * calendar clock. Signed requests, token lifetimes and TLS need a real date, so
 * callers pass the gate first: the clock counts as synchronized only when a
 * synchronization succeeded during this boot AND the time is later than
 * XIAOTAI_CLOCK_VALID_AFTER_S. A plausible time kept across a reset does not
 * pass, because nothing proves it is still right, unless the product's
 * configuration keeps the older rule of its deployed firmware
 * (trust_kept_time).
 *
 * The port owns how time is obtained (network time servers on ESP-IDF, the
 * operating system on the host) and whatever tasks, sockets and timers that
 * takes. It needs an IP link but does not watch one: the caller synchronizes
 * after the link is up and repeats until it succeeds. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2023-11-14T22:13:20Z. A clock at or before it has never been set. */
#define XIAOTAI_CLOCK_VALID_AFTER_S 1700000000LL
#define XIAOTAI_CLOCK_SERVERS_MAX 4U
/* Longest host name of a time server and longest time zone rule, with terminator. */
#define XIAOTAI_CLOCK_NAME_MAX 64U

typedef struct {
    /* Time servers by host name, in order of preference: 1..XIAOTAI_CLOCK_SERVERS_MAX
     * entries. The array and the strings are copied by xiaotai_clock_init. */
    const char *const *servers;
    size_t server_count;
    /* POSIX time zone rule ("CST-8"); NULL or "" is UTC. Copied. */
    const char *timezone;
    /* true: keep the time service running after the first success so the clock
     * follows drift. false: release its sockets and timers after each success
     * (boards with little memory); a later xiaotai_clock_sync starts it again. */
    bool keep_running;
    /* true: a synchronization with a timeout first asks every server at once
     * (one short UDP exchange each, on the caller's stack, at most 3000 ms)
     * and only then waits for the time service. false: it waits for the time
     * service alone. Products keep the way their deployed firmware got the
     * time. */
    bool probe;
    /* true: a time later than XIAOTAI_CLOCK_VALID_AFTER_S that the clock kept
     * across a reset passes the gate without a synchronization in this boot,
     * as a product's deployed firmware accepted it. false (the rule above):
     * only a synchronization in this boot opens the gate. */
    bool trust_kept_time;
} xiaotai_clock_config_t;

/* [init] Once, before any other clock call. Does not touch the network.
 * XIAOTAI_ERR_INVALID_ARG: no server, too many, or a name of
 * XIAOTAI_CLOCK_NAME_MAX bytes or more. XIAOTAI_ERR_INVALID_STATE: called twice. */
xiaotai_err_t xiaotai_clock_init(const xiaotai_clock_config_t *config);

/* [any] The gate: synchronized during this boot (or, with trust_kept_time,
 * a kept time) and later than XIAOTAI_CLOCK_VALID_AFTER_S. Reads memory and
 * the clock only. */
bool xiaotai_clock_synced(void);

/* [any task that may block; never the framework owner task with a non-zero
 * timeout, never a real-time task] Passes the gate or waits for it.
 *
 * XIAOTAI_OK at once when already synchronized. Otherwise starts the time
 * service if it is not running and waits up to timeout_ms. A timeout leaves the
 * service running: the next call keeps waiting for the same attempt instead of
 * starting over, so short timeouts never cut a slow server rotation short.
 *
 * timeout_ms 0 never blocks ([any], the owner task included): no probe is
 * sent on the caller's stack; it only starts the service when it is not
 * running and reports the present state. It may be called at any time and
 * repeatedly, also before an IP link exists: the service then keeps trying on
 * its own and nothing else changes (the gate, the clock and the counters
 * other than starts stay as they were), and a later call simply continues.
 *
 * XIAOTAI_ERR_TIMEOUT: not synchronized yet (still trying).
 * XIAOTAI_ERR_INVALID_STATE: xiaotai_clock_init was not called.
 * XIAOTAI_FAIL: the service could not start, or it reported success while the
 * time is still not later than XIAOTAI_CLOCK_VALID_AFTER_S. */
xiaotai_err_t xiaotai_clock_sync(uint32_t timeout_ms);

/* [any, after xiaotai_clock_init] What the port suggests as timeout for a first
 * synchronization: long enough for the service to give up on every configured
 * server once. 0 before xiaotai_clock_init. */
uint32_t xiaotai_clock_sync_budget_ms(void);

/* [any] Milliseconds since 1970-01-01T00:00:00Z. Meaningful only when
 * xiaotai_clock_synced(); before that it is whatever the clock holds. */
int64_t xiaotai_clock_unix_ms(void);

/* [any task that may block] Sets the clock from another trusted source (a
 * cellular modem, a test) and opens the gate. XIAOTAI_ERR_INVALID_ARG when
 * unix_ms is not later than XIAOTAI_CLOCK_VALID_AFTER_S. */
xiaotai_err_t xiaotai_clock_set_unix_ms(int64_t unix_ms);

/* [any, the owner task included] Replaces the time zone rule given at init.
 * Applies to later xiaotai_clock_local calls. NULL or "" is UTC. Never waits
 * for a synchronization: the port's state lock is only ever held for a copy
 * or a conversion, never across a wait. XIAOTAI_ERR_INVALID_ARG: a rule of
 * XIAOTAI_CLOCK_NAME_MAX bytes or more. */
xiaotai_err_t xiaotai_clock_set_timezone(const char *timezone);

/* [any, the owner task included] Copies the time zone rule in use (the one
 * given at init or by the latest xiaotai_clock_set_timezone), "" for UTC,
 * terminated. Same lock rule as xiaotai_clock_set_timezone: never waits for a
 * synchronization. A capacity of XIAOTAI_CLOCK_NAME_MAX always fits.
 * XIAOTAI_ERR_INVALID_SIZE: the rule does not fit (out is ""),
 * XIAOTAI_ERR_INVALID_ARG: no out or capacity 0,
 * XIAOTAI_ERR_INVALID_STATE: xiaotai_clock_init was not called (out is ""). */
xiaotai_err_t xiaotai_clock_timezone(char *out, size_t capacity);

typedef struct {
    int16_t year;          /* 1970.. */
    uint8_t month;         /* 1..12 */
    uint8_t day;           /* 1..31 */
    uint8_t hour;          /* 0..23 */
    uint8_t minute;        /* 0..59 */
    uint8_t second;        /* 0..60 */
    uint8_t weekday;       /* 0 Sunday .. 6 Saturday */
    int32_t utc_offset_s;  /* local minus UTC at that instant */
} xiaotai_clock_fields_t;

/* [any] Calendar fields of an instant in the configured time zone.
 * XIAOTAI_ERR_INVALID_ARG for an instant the platform cannot represent. */
xiaotai_err_t xiaotai_clock_local(int64_t unix_ms, xiaotai_clock_fields_t *fields);

typedef struct {
    uint32_t syncs;            /* successful synchronizations this boot */
    uint32_t timeouts;         /* xiaotai_clock_sync calls that ended in XIAOTAI_ERR_TIMEOUT */
    uint32_t invalid;          /* answers rejected (bad packet, implausible time, servers disagreeing) */
    uint32_t starts;           /* times the time service was started */
    int64_t last_sync_uptime_us; /* xiaotai_time_us() at the last success; 0: never */
    int32_t last_step_ms;      /* clock correction applied by the last success, saturated */
    bool running;              /* the time service holds resources now */
} xiaotai_clock_stats_t;

/* [any] */
void xiaotai_clock_stats(xiaotai_clock_stats_t *stats);

#ifdef __cplusplus
}
#endif
