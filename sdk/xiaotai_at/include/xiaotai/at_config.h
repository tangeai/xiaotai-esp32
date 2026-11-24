// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the console engine (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig).
 *
 * Two kinds, both depending on what a product selects and keeps, never on the
 * board:
 *   - the capacities of the engine's registration tables: how many byte
 *     sources one console serves, how many local commands and how many verb
 *     tables one build registers;
 *   - which of the engine's own guarded spellings exist (AT+RESTART,
 *     AT+RESTART FORCE, AT+SET <name> <value>, AT+INTENT), so each product's
 *     console offers no more than it does today. All are on by default.
 * Buffer sizes, task shape, the command gate and the timeouts are run-time
 * configuration (xiaotai_at_config_t in xiaotai/at.h), set per product.
 *
 * Each table is allocated from XIAOTAI_MEM_BULK when it is first written
 * (registration may come before xiaotai_at_open()) and freed by
 * xiaotai_at_reset(). Registering beyond a capacity is XIAOTAI_ERR_NO_MEM in
 * the registering call, so the start fails loudly instead of losing a
 * command; a product whose components bring more raises the option, and no
 * header changes.
 *
 * A guarded spelling whose option is off is not registered: AT+HELP does not
 * list it and the line is answered as if it never existed (AT+RESTART and
 * AT+INTENT: an unknown command; AT+RESTART FORCE: an unknown verb;
 * AT+SET <name> <value>: an argument count the read form does not take, the
 * setting unchanged). Its handlers
 * live in a source compiled only with the option and carry the prefix the
 * manifest names ("commands": option and symbol), so a build with the option
 * off can be checked after linking to contain none of them. */
#include "xiaotai/config.h"

/* Most byte sources one console serves (xiaotai_at_attach): the platform
 * console plus two more (a second UART, a debug socket). */
#ifdef CONFIG_XIAOTAI_AT_SOURCES_MAX
#define XIAOTAI_AT_SOURCES_MAX CONFIG_XIAOTAI_AT_SOURCES_MAX
#else
#define XIAOTAI_AT_SOURCES_MAX 3
#endif

/* Most local commands (xiaotai_at_local_register), the engine's own nine
 * included: AT, AT+HELP, AT+VERSION, AT+CLI, AT+LOG, AT+MEM, AT+CONSOLE,
 * AT+RESTART and AT+PORT. */
#ifdef CONFIG_XIAOTAI_AT_LOCAL_MAX
#define XIAOTAI_AT_LOCAL_MAX CONFIG_XIAOTAI_AT_LOCAL_MAX
#else
#define XIAOTAI_AT_LOCAL_MAX 24
#endif

/* Most verb tables one build registers (xiaotai_at_verbs_register), over
 * every command shared that way: each component adds one table per command
 * it contributes to. */
#ifdef CONFIG_XIAOTAI_AT_VERB_SETS_MAX
#define XIAOTAI_AT_VERB_SETS_MAX CONFIG_XIAOTAI_AT_VERB_SETS_MAX
#else
#define XIAOTAI_AT_VERB_SETS_MAX 48
#endif

/* The local command AT+RESTART: an orderly restart through the framework, or
 * a plain one while the framework is not running (xiaotai/at.h). Off, the
 * console cannot restart the device. Handlers prefixed
 * xiaotai_at_guarded_restart_. */
#define XIAOTAI_AT_RESTART_COMMAND XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AT_RESTART_COMMAND, 1)

/* AT+RESTART FORCE: restart 200 ms after OK without asking anyone. Exists only
 * together with AT+RESTART (Kconfig: depends on it), so its value is both
 * options. Handlers prefixed xiaotai_at_guarded_force_. */
#define XIAOTAI_AT_RESTART_FORCE \
    (XIAOTAI_AT_RESTART_COMMAND && XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AT_RESTART_FORCE, 1))

/* The write form AT+SET <name> <value>; the read form AT+SET [<name>] is
 * always there. Handlers prefixed xiaotai_at_guarded_set_. */
#define XIAOTAI_AT_SETTING_WRITE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AT_SETTING_WRITE, 1)

/* AT+INTENT <intent> [<value> [<text-hex>]]. Handlers prefixed
 * xiaotai_at_guarded_intent_. */
#define XIAOTAI_AT_INTENT_COMMAND XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_AT_INTENT_COMMAND, 1)
