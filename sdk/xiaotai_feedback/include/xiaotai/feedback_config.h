// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the feedback engine (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). Which
 * lights and vibrators exist is not an option: it comes from the board's
 * output parts. Where a Kconfig default depends on external RAM, this header
 * holds the value without it, which the host uses. */
#include "xiaotai/config.h"

/* Where light and vibration frames are produced, on boards where a table row
 * resolves to a light or vibrator (elsewhere neither exists):
 *   1  the engine's own task (below). An output write may block up to the
 *      20 ms the part interface allows without delaying anything else.
 *   0  an owner-task timer (xiaotai_timer_start) re-armed every
 *      XIAOTAI_FEEDBACK_FRAME_MS on an absolute schedule while something
 *      shows: no task and no stack, but each frame's writes run on the owner
 *      task and delay its handlers by as long as they take, so it suits fast
 *      parts (a GPIO or PWM lamp, a short LED strip). One frame step is an
 *      owner-task handler and uses at most
 *      XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES with the part writes it makes
 *      (the frame lives in the engine's state, not on the stack).
 * Kconfig default: 1 with external RAM, 0 without (the internal RAM ledger of
 * the boards without external RAM). Sound decisions and sound timers run on
 * the owner task either way. */
#define XIAOTAI_FEEDBACK_TASK XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_FEEDBACK_TASK, 0)

/* Priority of the engine's task (XIAOTAI_FEEDBACK_TASK). It renders one light
 * and vibration frame every XIAOTAI_FEEDBACK_FRAME_MS and writes it to the
 * output parts; a late frame only delays a pattern, so it runs below
 * everything that carries audio or the network. */
#ifdef CONFIG_XIAOTAI_FEEDBACK_TASK_PRIORITY
#define XIAOTAI_FEEDBACK_TASK_PRIORITY CONFIG_XIAOTAI_FEEDBACK_TASK_PRIORITY
#else
#define XIAOTAI_FEEDBACK_TASK_PRIORITY 2
#endif

/* Stack of the engine's task in bytes. XIAOTAI_MEM_BULK (in external RAM
 * where the board has it): the task only computes frames and calls the output
 * parts' set()/write(), which never touch flash or NVS, so nothing on it runs
 * with the cache disabled. Its lock and state stay in internal RAM. */
#ifdef CONFIG_XIAOTAI_FEEDBACK_TASK_STACK_BYTES
#define XIAOTAI_FEEDBACK_TASK_STACK_BYTES CONFIG_XIAOTAI_FEEDBACK_TASK_STACK_BYTES
#else
#define XIAOTAI_FEEDBACK_TASK_STACK_BYTES 2560
#endif

/* Registry capacities: tables registered, and states over all tables (each
 * state has one bit and one resolved part list). The table pointers and the
 * per-state data are allocated from XIAOTAI_MEM_BULK at the engine's init;
 * the state bits stay in internal RAM under the engine's lock. Registering
 * beyond a capacity is XIAOTAI_ERR_NO_MEM in the registering init(), so the
 * start fails loudly; an application with more rows raises the option. */
#ifdef CONFIG_XIAOTAI_FEEDBACK_TABLES_MAX
#define XIAOTAI_FEEDBACK_TABLES_MAX CONFIG_XIAOTAI_FEEDBACK_TABLES_MAX
#else
#define XIAOTAI_FEEDBACK_TABLES_MAX 4
#endif
#ifdef CONFIG_XIAOTAI_FEEDBACK_STATES_MAX
#define XIAOTAI_FEEDBACK_STATES_MAX CONFIG_XIAOTAI_FEEDBACK_STATES_MAX
#else
#define XIAOTAI_FEEDBACK_STATES_MAX 64
#endif
