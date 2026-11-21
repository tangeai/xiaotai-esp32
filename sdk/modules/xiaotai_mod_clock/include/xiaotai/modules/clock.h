// SPDX-License-Identifier: MIT
#pragma once
/* Clock module: the time of day for applications, and things that happen at a
 * time.
 *
 * Two parts:
 *   - the wall clock: whether the time is known, what it is in the device's
 *     time zone, an event once a minute for a clock face, and the time zone as
 *     a setting;
 *   - schedules: alarms (a time of day, once or on chosen weekdays, kept
 *     across restarts) and countdowns (a duration, gone at restart). When one
 *     is due the module emits an event; what then rings, shows or starts is
 *     the application's business.
 *
 * Time comes from the clock port (xiaotai/port/clock.h). Who configures and
 * synchronizes it is decided once, at init, by xiaotai_module_present
 * ("platform"), never by a product flag:
 *   - with the platform core, that engine initializes the port from its own
 *     configuration (time zone, time servers) before any module and
 *     synchronizes it after the network came up; this module only watches
 *     the gate, and its own timezone and time_servers must not be given;
 *   - without it, this module initializes the port from timezone and
 *     time_servers and asks it to synchronize every sync_retry_s until the
 *     gate opens.
 * So time is configured in exactly one place in every product.
 *
 * Names. The prefix xiaotai_clock_ belongs to the clock port, so this
 * module's functions are xiaotai_wallclock_* and xiaotai_schedule_*; its
 * manifest name, events, intents, settings and command use "clock".
 *
 * Manifest name "clock". Depends on the engines eventlog and at only. No
 * hardware need.
 *
 * Task tags as in xiaotai/framework.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/clock.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- wall clock */

typedef struct {
    /* The port's gate (xiaotai_clock_synced()). While false, unix_ms and local
     * hold whatever the clock holds and must not be shown as the time. */
    bool synced;
    int64_t unix_ms;
    xiaotai_clock_fields_t local;   /* in the configured time zone */
} xiaotai_wallclock_t;

/* [any] The present time. Returns out->synced. */
bool xiaotai_wallclock_now(xiaotai_wallclock_t *out);

/* Setting: the user's POSIX time zone rule, text. Registered only with
 * timezone_setting (xiaotai_clock_module_config_t). Its default is "", which
 * means the rule the port was initialized with (the platform core's timezone,
 * or this module's own without it): nothing is stored and nothing replaces
 * that rule until the user picks one. A non-empty value replaces the rule at
 * init and at once on every change, and is kept across restarts; going back
 * to "" restores the initial rule. */
#define XIAOTAI_SETTING_CLOCK_TIMEZONE "clock.timezone"

/* ----------------------------------------------------------------- schedules */

/* Entries the module holds, alarms and countdowns together. A constant, not a
 * build option: it bounds the stored alarm table, which every firmware must
 * read the same way. */
#define XIAOTAI_SCHEDULE_MAX 16U
/* Label bytes without the terminator (UTF-8, cut at a character boundary). */
#define XIAOTAI_SCHEDULE_LABEL_MAX 23U
/* Longest countdown: 24 hours. */
#define XIAOTAI_SCHEDULE_COUNTDOWN_MAX_S 86400U

/* A further kind is one more value. Each kind reads only the fields named for
 * it in xiaotai_schedule_t; the others are ignored when given and 0 when
 * read back. */
typedef enum {
    /* Due when the local time of day reaches hour:minute. Needs a synchronized
     * clock: while it is not, alarms do not fire and are not made up for later. */
    XIAOTAI_SCHEDULE_ALARM = 0,
    /* Due `seconds` after it was added, on the monotonic clock: works without
     * a synchronized clock, is not kept across a restart, and removes itself
     * when it fired. */
    XIAOTAI_SCHEDULE_COUNTDOWN,
} xiaotai_schedule_kind_t;

/* Weekday bits of an alarm: bit 0 Sunday .. bit 6 Saturday. */
#define XIAOTAI_SCHEDULE_EVERY_DAY 0x7FU

typedef struct {
    /* 1..255, unique among the present entries and never reused while the
     * device runs; assigned by xiaotai_schedule_add (the value passed in is
     * ignored there). */
    uint8_t id;
    xiaotai_schedule_kind_t kind;
    /* A disabled alarm is kept and never fires. Countdowns are always enabled. */
    bool enabled;
    /* ALARM. */
    uint8_t hour;         /* 0..23 */
    uint8_t minute;       /* 0..59 */
    /* ALARM: days it repeats on; 0: once, at the next occurrence of
     * hour:minute, after which the module disables it. */
    uint8_t weekdays;
    /* COUNTDOWN: the duration, 1..XIAOTAI_SCHEDULE_COUNTDOWN_MAX_S. */
    uint32_t seconds;
    char label[XIAOTAI_SCHEDULE_LABEL_MAX + 1U];
} xiaotai_schedule_t;

typedef struct {
    xiaotai_schedule_t entry;
    /* Seconds until it is due next; UINT32_MAX when that is not known: a
     * disabled alarm, or an alarm while the clock is not synchronized. */
    uint32_t due_in_s;
} xiaotai_schedule_info_t;

/* [owner] Add an entry; *id (may be NULL) receives its id.
 * XIAOTAI_ERR_INVALID_ARG  a field out of range, weekday bits above 0x7F, a
 *                          label that is not terminated within its size
 * XIAOTAI_ERR_NO_MEM       XIAOTAI_SCHEDULE_MAX entries exist */
xiaotai_err_t xiaotai_schedule_add(const xiaotai_schedule_t *entry, uint8_t *id);

/* [owner] Replace the entry with entry->id (same kind). A countdown starts
 * over with the new duration. XIAOTAI_ERR_NOT_FOUND; XIAOTAI_ERR_INVALID_ARG
 * as above or for another kind. */
xiaotai_err_t xiaotai_schedule_update(const xiaotai_schedule_t *entry);

/* [owner] XIAOTAI_ERR_NOT_FOUND for an unknown id. */
xiaotai_err_t xiaotai_schedule_remove(uint8_t id);

/* [owner] Enable or disable an alarm. XIAOTAI_ERR_NOT_FOUND;
 * XIAOTAI_ERR_NOT_SUPPORTED for a countdown. */
xiaotai_err_t xiaotai_schedule_enable(uint8_t id, bool enabled);

/* [any] Copies the entries, alarms first, each group in the order added;
 * returns how many (at most capacity). */
size_t xiaotai_schedule_list(xiaotai_schedule_info_t *entries, size_t capacity);

/* [any] XIAOTAI_ERR_NOT_FOUND for an unknown id. */
xiaotai_err_t xiaotai_schedule_get(uint8_t id, xiaotai_schedule_info_t *info);

/* [any] The entry that is due soonest. XIAOTAI_ERR_NOT_FOUND when nothing is
 * scheduled with a known due time. */
xiaotai_err_t xiaotai_schedule_next(xiaotai_schedule_info_t *info);

/* ------------------------------------------------------------- configuration */

/* Product configuration. Every field: 0 / NULL selects the default. */
typedef struct {
    /* Offer the time zone as the setting XIAOTAI_SETTING_CLOCK_TIMEZONE. */
    bool timezone_setting;
    /* The three fields below apply only to a product without the platform
     * core. With it they must stay NULL / 0: time is configured in the
     * platform core's section (xiaotai_platform_config_t), and init refuses a
     * product that gives them here too (the selection script's check refuses
     * it before the build). */
    /* POSIX time zone rule the port starts with; NULL: "CST-8". Static. */
    const char *timezone;
    /* Time servers by host name, in order of preference, at most
     * XIAOTAI_CLOCK_SERVERS_MAX; NULL / 0: the two public servers the
     * platform core uses by default. Static storage. */
    const char *const *time_servers;
    size_t time_server_count;
    /* Seconds between synchronization requests until the gate opens; default
     * 5. Ignored with the platform core. */
    uint16_t sync_retry_s;
} xiaotai_clock_module_config_t;

/* [glue] Before xiaotai_module_clock_init(), from the product glue; copied (strings
 * are not). NULL restores the defaults. XIAOTAI_ERR_INVALID_ARG: more than
 * XIAOTAI_CLOCK_SERVERS_MAX servers, a count without servers, a server name or
 * a time zone of XIAOTAI_CLOCK_NAME_MAX bytes or more.
 * XIAOTAI_ERR_INVALID_STATE once initialized. Whether the platform core is in
 * the build is not known yet here; init checks it. */
xiaotai_err_t xiaotai_clock_module_configure(const xiaotai_clock_module_config_t *config);

/* -------------------------------------------------------------------- events */

/* The clock passed the gate for the first time in this boot. No arguments. */
#define XIAOTAI_EVENT_CLOCK_SYNCED "clock.synced"
/* A local minute began (and once right after XIAOTAI_EVENT_CLOCK_SYNCED).
 * args.value: minutes since local midnight; data: a copy of
 * xiaotai_wallclock_t. A step of the clock (a later synchronization, a time
 * zone change) emits it again at once. */
#define XIAOTAI_EVENT_CLOCK_MINUTE "clock.minute"
/* An entry is due. args.value: its id; args.text: its label; data: a copy of
 * xiaotai_schedule_t as it was when it fired. Emitted once per occurrence; an
 * alarm whose minute was reached while the owner task was busy still fires as
 * long as that minute has not ended. */
#define XIAOTAI_EVENT_CLOCK_DUE "clock.due"
/* The set of entries changed (added, updated, removed, enabled, disabled,
 * a one-time alarm fired, a countdown ended). args.value: entries now. */
#define XIAOTAI_EVENT_CLOCK_SCHEDULES "clock.schedules"

/* ------------------------------------------------------------------- intents */

/* Add a countdown. args.value: seconds; args.text: the label. A refusal is
 * only counted. */
#define XIAOTAI_INTENT_CLOCK_COUNTDOWN "clock.countdown"
/* xiaotai_schedule_remove(args.value). args.value 0 removes every countdown
 * ("stop the timer"). */
#define XIAOTAI_INTENT_CLOCK_REMOVE "clock.remove"
/* xiaotai_schedule_enable(args.value, args.text is not "0"). */
#define XIAOTAI_INTENT_CLOCK_ENABLE "clock.enable"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t sync_requests;     /* without the platform core: times the port was asked */
    uint32_t minutes;           /* minute events emitted */
    uint32_t steps;             /* clock steps noticed (a jump of more than 2 s against the monotonic clock) */
    uint32_t fired_alarms, fired_countdowns;
    uint32_t missed_alarms;     /* alarm minutes that passed while the device ran unsynchronized or was busy */
    uint32_t added, removed, refused;
    uint32_t stored, store_failures;
    uint32_t load_invalid;      /* a stored alarm table or entry ignored as malformed */
} xiaotai_clock_module_stats_t;

/* [any] */
void xiaotai_clock_module_stats(xiaotai_clock_module_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* Called by the framework through the generated tables. init: without the
 * platform core, initializes the clock port (unless something already did:
 * host tests, firmware without generated glue); with it, returns
 * XIAOTAI_ERR_INVALID_ARG when timezone or time_servers were given (the start
 * fails rather than run with a second, ignored time configuration). Then
 * loads the stored alarms, registers the setting (when configured), the
 * intents, the command AT+CLOCK and the diagnostics provider "clock":
 *   AT+CLOCK         +CLOCK: synced=<0|1> unix_ms=<n>
 *                    local=<YYYY-MM-DDThh:mm:ss> weekday=<0..6> offset_s=<n>;
 *                    per entry +CLOCK-ITEM: id=<n> kind=<alarm|countdown>
 *                    enabled=<0|1> time=<hh:mm|-> weekdays=<n> due_in_s=<n|->
 *                    label_hex=<hex|->
 *   AT+CLOCK ALARM <hh:mm> <0..127> [<label-hex>]   xiaotai_schedule_add
 *   AT+CLOCK COUNTDOWN <seconds> [<label-hex>]      xiaotai_schedule_add
 *   AT+CLOCK REMOVE <id>                            xiaotai_schedule_remove
 *   AT+CLOCK ENABLE <id> <0|1>                      xiaotai_schedule_enable
 * start:
 * arms the first timer; without the platform core the first
 * xiaotai_clock_sync(0) is asked at once, network or not (the port keeps
 * trying on its own). */
xiaotai_err_t xiaotai_module_clock_init(void);
void xiaotai_module_clock_start(void);

/* Host tests: forget entries, configuration and counters. */
void xiaotai_clock_module_reset(void);

#ifdef __cplusplus
}
#endif
