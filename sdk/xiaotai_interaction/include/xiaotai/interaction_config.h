// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the interaction engine (xiaotai/config.h explains
 * the mechanism; the options themselves are in the component's Kconfig).
 * Gesture timing is not an option: it belongs to the application's
 * interaction profile. The poll period is XIAOTAI_INPUT_POLL_MS of the part
 * interface (xiaotai/hal.h). Where a Kconfig default depends on external RAM,
 * this header holds the value without it, which the host uses. */
#include "xiaotai/config.h"

/* Where the inputs are polled, when the engine polls at all (a profile and
 * an input part exist):
 *   1  the engine's poll task (below), above the network tasks, so a key is
 *      sampled on time while a TLS handshake runs.
 *   0  no task and no stack: an owner-task timer (xiaotai_timer_start) runs
 *      the same poll step every XIAOTAI_INPUT_POLL_MS on an absolute
 *      schedule (the next one is due a period after the previous due time,
 *      not after the step ran; a step that runs more than one period behind
 *      its due time restarts the schedule from now and is not repeated).
 *      Lateness is counted as with the task, by the gap between two polls:
 *      late_polls only when it exceeds XIAOTAI_INTERACTION_LATE_MS
 *      (xiaotai/interaction.h), so a restart after a 20 ms delay is not a
 *      late poll. The step
 *      is an owner-task handler and uses at most
 *      XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES with the part calls it makes.
 *      The cost: polls run at the owner task's priority and wait behind the
 *      handler that runs, so a gesture's edges are timed as late as the owner
 *      is busy (late_polls and max_gap_ms show it); this is how a product
 *      without external RAM polls keys in its main loop or a timer today.
 * Kconfig default: 1 with external RAM, 0 without (the internal RAM ledger of
 * the boards without it). */
#define XIAOTAI_INTERACTION_TASK XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_INTERACTION_TASK, 0)

/* Priority of the poll task. A key must be sampled on time while the network
 * stack is busy (a TLS handshake), so it runs above the network tasks; one
 * poll is short and bounded by the parts. */
#ifdef CONFIG_XIAOTAI_INTERACTION_TASK_PRIORITY
#define XIAOTAI_INTERACTION_TASK_PRIORITY CONFIG_XIAOTAI_INTERACTION_TASK_PRIORITY
#else
#define XIAOTAI_INTERACTION_TASK_PRIORITY 5
#endif

/* Stack of the poll task in bytes. XIAOTAI_MEM_BULK (in external RAM where
 * the board has it): the task only calls the input parts' poll()/read(),
 * runs the recognizer and posts, and none of that touches flash or NVS, so
 * nothing on it runs with the cache disabled. Its lock and per-input state
 * stay in internal RAM. */
#ifdef CONFIG_XIAOTAI_INTERACTION_TASK_STACK_BYTES
#define XIAOTAI_INTERACTION_TASK_STACK_BYTES CONFIG_XIAOTAI_INTERACTION_TASK_STACK_BYTES
#else
#define XIAOTAI_INTERACTION_TASK_STACK_BYTES 3072
#endif

/* Registry capacities: profiles registered, layers pushed at once, and input
 * parts polled. The profile and layer tables are allocated from
 * XIAOTAI_MEM_BULK at the engine's init; per-input state stays in internal
 * RAM. Registering beyond PROFILES_MAX is XIAOTAI_ERR_NO_MEM in the
 * registering init(), so the start fails loudly; a product whose applications
 * bring more raises the option. */
#ifdef CONFIG_XIAOTAI_INTERACTION_PROFILES_MAX
#define XIAOTAI_INTERACTION_PROFILES_MAX CONFIG_XIAOTAI_INTERACTION_PROFILES_MAX
#else
#define XIAOTAI_INTERACTION_PROFILES_MAX 8
#endif
#ifdef CONFIG_XIAOTAI_INTERACTION_LAYERS_MAX
#define XIAOTAI_INTERACTION_LAYERS_MAX CONFIG_XIAOTAI_INTERACTION_LAYERS_MAX
#else
#define XIAOTAI_INTERACTION_LAYERS_MAX 4
#endif
#ifdef CONFIG_XIAOTAI_INTERACTION_INPUTS_MAX
#define XIAOTAI_INTERACTION_INPUTS_MAX CONFIG_XIAOTAI_INTERACTION_INPUTS_MAX
#else
#define XIAOTAI_INTERACTION_INPUTS_MAX 8
#endif
