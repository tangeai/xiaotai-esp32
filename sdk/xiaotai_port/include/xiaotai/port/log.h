// SPDX-License-Identifier: MIT
#pragma once
/* The system log: one line per event, `<level> (<ms since boot>) <tag>: <text>`.
 * docs/日志规范.md is the rule book (what each level is for, how a line reads,
 * what must never be printed); this header is the mechanism.
 *
 * Two thresholds, both changed at run time (the console's AT+LOGLEVEL):
 *
 *   app     lines written with the macros below, by XiaoTai's own code. The
 *           default, info, is the story of what the device does: identity,
 *           network, platform, each session's start and end, every failure
 *           with its reason. Debug adds what only someone debugging needs:
 *           state transitions, protocol steps, timings, memory, media
 *           statistics. One module can be given a threshold of its own.
 *   system  lines of the platform and third-party components (ESP-IDF, drivers,
 *           codecs), which write with their own macros. The default is
 *           warning, so they stay out of the story unless something is wrong.
 *
 * Never log on real-time paths (capture, playout, transport callbacks): count
 * there and report through the event log's topics (xiaotai/eventlog.h). Never
 * print keys, tokens, passwords, device identifiers or what a user said.
 * The linked port supplies the output (ESP-IDF log, or stderr on the host). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/port_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Levels, numbered as the console names them. */
#define XIAOTAI_LOG_LEVEL_OFF 0U
#define XIAOTAI_LOG_LEVEL_ERROR 1U   /* something failed; the line says what and, where it helps, what to do */
#define XIAOTAI_LOG_LEVEL_WARN 2U    /* handled, but worth a look: a retry, a refusal, a fallback */
#define XIAOTAI_LOG_LEVEL_INFO 3U    /* the story: one line per step a developer would name */
#define XIAOTAI_LOG_LEVEL_DEBUG 4U   /* for whoever debugs the module: transitions, timings, statistics */

/* [any] True when a line of `level` tagged `tag` is printed now: the module's
 * own threshold when it has one, otherwise the app threshold. One atomic load
 * unless a module threshold is set. Use it to skip work that only prepares a
 * debug line (formatting a table, reading statistics). */
bool xiaotai_log_enabled(uint8_t level, const char *tag);

#include "xiaotai_log_port.h"

/* tag: the module's short name, [a-z0-9_.], at most 15 characters; the same
 * word the module uses as its event-log source. format: a string literal. */
#define XIAOTAI_LOG_AT(level, letter, tag, ...)                                                  \
    do {                                                                                         \
        if ((level) <= XIAOTAI_LOG_LEVEL_CEILING) XIAOTAI_LOG_PORT(level, letter, tag, __VA_ARGS__); \
    } while (0)
#define XIAOTAI_LOGE(tag, ...) XIAOTAI_LOG_AT(XIAOTAI_LOG_LEVEL_ERROR, "E", tag, __VA_ARGS__)
#define XIAOTAI_LOGW(tag, ...) XIAOTAI_LOG_AT(XIAOTAI_LOG_LEVEL_WARN, "W", tag, __VA_ARGS__)
#define XIAOTAI_LOGI(tag, ...) XIAOTAI_LOG_AT(XIAOTAI_LOG_LEVEL_INFO, "I", tag, __VA_ARGS__)
#define XIAOTAI_LOGD(tag, ...) XIAOTAI_LOG_AT(XIAOTAI_LOG_LEVEL_DEBUG, "D", tag, __VA_ARGS__)

/* ------------------------------------------------------------- app threshold */

/* [any] 0..XIAOTAI_LOG_LEVEL_DEBUG; XIAOTAI_ERR_INVALID_ARG above. A build
 * compiles lines only up to its ceiling (xiaotai_log_level_max(), the option
 * XIAOTAI_LOG_LEVEL_CEILING); a threshold above it prints nothing more. Not
 * saved by this call: every boot starts from info unless the console stored
 * another level (AT+LOGLEVEL SAVE). */
xiaotai_err_t xiaotai_log_set_level(uint8_t level);
uint8_t xiaotai_log_level(void);
uint8_t xiaotai_log_level_max(void);

/* ---------------------------------------------------------- module threshold */

/* Most modules with a threshold of their own at one time (static storage). */
#define XIAOTAI_LOG_MODULES_MAX 4U
#define XIAOTAI_LOG_MODULE_NAME_MAX 15U
/* Passed as level: the module follows the app threshold again. */
#define XIAOTAI_LOG_LEVEL_INHERIT 0xffU

/* [any; not a real-time task; one caller at a time: the console] Gives the
 * lines tagged `tag` a threshold of their own, above or below the app threshold: debug for one module while the
 * rest stays at info, or one talkative module held at warning.
 * XIAOTAI_ERR_INVALID_ARG for an empty or over-long tag or a level above debug,
 * XIAOTAI_ERR_NO_MEM when XIAOTAI_LOG_MODULES_MAX other modules have one.
 * Never saved. */
xiaotai_err_t xiaotai_log_set_module_level(const char *tag, uint8_t level);
/* [any] The index-th module threshold in force; false past the last. */
bool xiaotai_log_module_at(size_t index, const char **tag, uint8_t *level);

/* ---------------------------------------------------------- system threshold */

/* [any; not a real-time task] Threshold of the platform's and third-party
 * components' lines, 0..xiaotai_log_system_level_max() (what the build
 * compiled in); XIAOTAI_ERR_INVALID_ARG above. At warning and below, tags
 * known to warn about things that are normal here stay at error. */
xiaotai_err_t xiaotai_log_set_system_level(uint8_t level);
uint8_t xiaotai_log_system_level(void);
uint8_t xiaotai_log_system_level_max(void);

/* ------------------------------------------------------- kept across restarts */

/* [any; not a real-time task] Queues the app and system thresholds in force
 * for the storage worker (xiaotai/port/kv.h, namespace "xt_log"); the caller
 * never waits for flash. From the next start they apply as soon as storage
 * opens, so a start-up can be followed at debug level. Module thresholds are
 * not kept. The errors are those of xiaotai_kv_set_int_async. */
xiaotai_err_t xiaotai_log_save(void);
/* [any; not a real-time task] The next start uses the defaults again; the
 * thresholds in force now stay. */
xiaotai_err_t xiaotai_log_forget(void);
/* [any] The thresholds the store holds: as loaded at start, or as last queued. */
bool xiaotai_log_saved(uint8_t *app, uint8_t *system);
/* [init] Applies the stored thresholds; the framework calls it once storage
 * is open. Nothing stored, a read error or a value this build cannot apply
 * leaves the defaults. */
void xiaotai_log_load(void);

/* ------------------------------------------------------------------ lifecycle */

/* [glue] Once, before anything else of the SDK runs: applies the default
 * thresholds. xiaotai_start() calls it. */
void xiaotai_log_start(void);
/* [any task that may block] Right before the chip resets: drivers and
 * connections are torn down inside the reset, so what they report is expected.
 * At the default thresholds only XiaoTai's errors remain; a threshold raised
 * above its default (app debug, system info) is left alone, because whoever
 * raised it wants to see the teardown. xiaotai_restart() calls it. */
void xiaotai_log_quiet_for_restart(void);

#ifdef __cplusplus
}
#endif
