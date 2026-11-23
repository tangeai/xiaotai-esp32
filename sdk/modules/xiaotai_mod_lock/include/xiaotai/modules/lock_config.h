// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the lock module (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). */
#include "xiaotai/config.h"

/* Register AT+LOCK OPEN [<index> [<ms>]] and AT+LOCK CLOSE [<index>]. Off by
 * default: with it anyone who reaches the console can release the door. The
 * status form of AT+LOCK is always registered. The two handlers live in a
 * source compiled only with this option and are named xiaotai_lock_at_open_*,
 * so a build with the option off can be checked to contain neither (manifest
 * "commands": option and symbol). */
#define XIAOTAI_LOCK_AT_OPEN XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_LOCK_AT_OPEN, 0)

/* Locks the module manages; further door relays of a board are ignored and
 * counted (ignored_relays). The table is allocated from XIAOTAI_MEM_BULK at
 * init, one entry per lock; a board with more door relays raises the option,
 * and no header changes. */
#ifdef CONFIG_XIAOTAI_LOCK_MAX
#define XIAOTAI_LOCK_MAX CONFIG_XIAOTAI_LOCK_MAX
#else
#define XIAOTAI_LOCK_MAX 4
#endif

#if XIAOTAI_LOCK_MAX < 1 || XIAOTAI_LOCK_MAX > 255
#error "XIAOTAI_LOCK_MAX must be 1..255: a lock's index is a uint8_t."
#endif
