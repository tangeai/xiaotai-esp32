// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the binding module (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). */
#include "xiaotai/config.h"

/* Register AT+BIND SET <device-id> <key-hex> [<client-id>] and AT+BIND CLEAR
 * (format and checks in xiaotai/modules/binding.h): they write and erase the
 * stored identity directly, without the server, and restart. For development
 * and bench firmware only; the key is never echoed. Their handlers live in a
 * source compiled only with this option and are named
 * xiaotai_binding_at_dev_*, so a build with the option off can be checked to
 * contain none of them (manifest "commands": option and symbol). */
#define XIAOTAI_BINDING_DEV_COMMANDS XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_BINDING_DEV_COMMANDS, 0)

/* Registry capacities. Both tables hold other modules' registrations, made in
 * their init(). Registering beyond a capacity is XIAOTAI_ERR_NO_MEM in the
 * registering init(), so the start fails instead of losing a veto or a step;
 * a product whose modules bring more raises the option, and no header
 * changes. */
/* Guards (xiaotai_binding_guard_register). */
#ifdef CONFIG_XIAOTAI_BINDING_GUARDS_MAX
#define XIAOTAI_BINDING_GUARDS_MAX CONFIG_XIAOTAI_BINDING_GUARDS_MAX
#else
#define XIAOTAI_BINDING_GUARDS_MAX 4
#endif
/* Preparation steps (xiaotai_binding_prepare_register). Each may take up to
 * XIAOTAI_BINDING_PREPARE_MS before the report, so every further step can
 * lengthen an attempt by that much. */
#ifdef CONFIG_XIAOTAI_BINDING_PREPARES_MAX
#define XIAOTAI_BINDING_PREPARES_MAX CONFIG_XIAOTAI_BINDING_PREPARES_MAX
#else
#define XIAOTAI_BINDING_PREPARES_MAX 2
#endif

#if XIAOTAI_BINDING_GUARDS_MAX < 1 || XIAOTAI_BINDING_PREPARES_MAX < 1
#error "The binding module's registry capacities must be at least 1."
#endif
