// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the runtime framework (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). A
 * product sets them through its build options; host tests pass their own
 * values in xiaotai_framework_config_t instead. */
#include "xiaotai/config.h"

/* In this mode the caller's task hosts the same framework owner loop. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_OWNER_ON_MAIN
#define XIAOTAI_FRAMEWORK_OWNER_ON_MAIN CONFIG_XIAOTAI_FRAMEWORK_OWNER_ON_MAIN
#else
#define XIAOTAI_FRAMEWORK_OWNER_ON_MAIN 0
#endif

/* Stack of the owner task in bytes, allocated as XIAOTAI_MEM_BULK (external
 * RAM where the board has it: the owner task never runs with the cache
 * disabled, flash and NVS work goes through the storage port's worker).
 * Kconfig default: 24576 with external RAM (the deepest owner path of the
 * former session owners needed more than 13 KiB), 6144 without (the internal
 * RAM ledger of the boards without external RAM). This header holds the
 * value without external RAM, which the host uses. Every handler gets
 * XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES of it (xiaotai/framework.h). */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_OWNER_STACK_BYTES
#define XIAOTAI_FRAMEWORK_OWNER_STACK_BYTES CONFIG_XIAOTAI_FRAMEWORK_OWNER_STACK_BYTES
#else
#define XIAOTAI_FRAMEWORK_OWNER_STACK_BYTES 6144
#endif

/* Priority of the owner task: below every audio, video and network task, which
 * hand work to it and never wait for it. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_OWNER_PRIORITY
#define XIAOTAI_FRAMEWORK_OWNER_PRIORITY CONFIG_XIAOTAI_FRAMEWORK_OWNER_PRIORITY
#else
#define XIAOTAI_FRAMEWORK_OWNER_PRIORITY 5
#endif

/* Messages the owner's queue holds (intents, events, xiaotai_run jobs; about
 * 100 bytes each on 32-bit targets, BULK memory). A full queue is
 * XIAOTAI_ERR_NO_MEM for the poster and counted. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_QUEUE_DEPTH
#define XIAOTAI_FRAMEWORK_QUEUE_DEPTH CONFIG_XIAOTAI_FRAMEWORK_QUEUE_DEPTH
#else
#define XIAOTAI_FRAMEWORK_QUEUE_DEPTH 32
#endif

/* Between XIAOTAI_EVENT_RESTARTING and the restart: time for a last prompt to
 * be heard and a last line to leave the console. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_RESTART_DELAY_MS
#define XIAOTAI_FRAMEWORK_RESTART_DELAY_MS CONFIG_XIAOTAI_FRAMEWORK_RESTART_DELAY_MS
#else
#define XIAOTAI_FRAMEWORK_RESTART_DELAY_MS 1000
#endif

/* Stack of the short task that calls xiaotai_restart() at the end of the
 * orderly restart, in internal RAM (xiaotai/framework.h, step 4). It exists
 * only for that moment. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_RESTART_STACK_BYTES
#define XIAOTAI_FRAMEWORK_RESTART_STACK_BYTES CONFIG_XIAOTAI_FRAMEWORK_RESTART_STACK_BYTES
#else
#define XIAOTAI_FRAMEWORK_RESTART_STACK_BYTES 4096
#endif

/* How long a restart waits for the quiesce guards to agree before it
 * quiesces without them. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_RESTART_GUARD_WAIT_MS
#define XIAOTAI_FRAMEWORK_RESTART_GUARD_WAIT_MS CONFIG_XIAOTAI_FRAMEWORK_RESTART_GUARD_WAIT_MS
#else
#define XIAOTAI_FRAMEWORK_RESTART_GUARD_WAIT_MS 10000
#endif

/* Registry capacities. Each entry is a few pointers; the registries live in
 * BULK memory, allocated once at start. The product selector sizes the intent
 * and subscriber registries from manifest declarations. Registering beyond a
 * capacity fails init with XIAOTAI_ERR_NO_MEM instead of losing a registration. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_MODULES
#define XIAOTAI_FRAMEWORK_MAX_MODULES CONFIG_XIAOTAI_FRAMEWORK_MAX_MODULES
#else
#define XIAOTAI_FRAMEWORK_MAX_MODULES 48
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_APPLICATIONS
#define XIAOTAI_FRAMEWORK_MAX_APPLICATIONS CONFIG_XIAOTAI_FRAMEWORK_MAX_APPLICATIONS
#else
#define XIAOTAI_FRAMEWORK_MAX_APPLICATIONS 8
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_INTENTS
#define XIAOTAI_FRAMEWORK_MAX_INTENTS CONFIG_XIAOTAI_FRAMEWORK_MAX_INTENTS
#else
#define XIAOTAI_FRAMEWORK_MAX_INTENTS 96
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS
#define XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS CONFIG_XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS
#else
#define XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS 160
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
#define XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS CONFIG_XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
#else
#define XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS 0
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_ATTACHMENTS
#define XIAOTAI_FRAMEWORK_MAX_ATTACHMENTS CONFIG_XIAOTAI_FRAMEWORK_MAX_ATTACHMENTS
#else
#define XIAOTAI_FRAMEWORK_MAX_ATTACHMENTS 8
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_SETTINGS
#define XIAOTAI_FRAMEWORK_MAX_SETTINGS CONFIG_XIAOTAI_FRAMEWORK_MAX_SETTINGS
#else
#define XIAOTAI_FRAMEWORK_MAX_SETTINGS 48
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_COMMANDS
#define XIAOTAI_FRAMEWORK_MAX_COMMANDS CONFIG_XIAOTAI_FRAMEWORK_MAX_COMMANDS
#else
#define XIAOTAI_FRAMEWORK_MAX_COMMANDS 64
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_DIAGNOSTICS
#define XIAOTAI_FRAMEWORK_MAX_DIAGNOSTICS CONFIG_XIAOTAI_FRAMEWORK_MAX_DIAGNOSTICS
#else
#define XIAOTAI_FRAMEWORK_MAX_DIAGNOSTICS 64
#endif
/* Owner-task timers in use at once. Counted in the default: the preferences
 * service keeps one slot permanently (its coalescing write timer; with no
 * free slot its writes are postponed, never lost), and every module or
 * application timer that may be pending at the same time. */
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_TIMERS
#define XIAOTAI_FRAMEWORK_MAX_TIMERS CONFIG_XIAOTAI_FRAMEWORK_MAX_TIMERS
#else
#define XIAOTAI_FRAMEWORK_MAX_TIMERS 24
#endif
#ifdef CONFIG_XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS
#define XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS CONFIG_XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS
#else
#define XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS 8
#endif
