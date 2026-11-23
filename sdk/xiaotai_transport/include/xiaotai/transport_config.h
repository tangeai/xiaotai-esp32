// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the transport engine (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). */
#include "xiaotai/config.h"

/* Allocation trace of the RTC stack and the command AT+MEMTRACE. The trace
 * keeps XIAOTAI_TRANSPORT_MEMTRACE_SLOTS blocks the stack holds (address,
 * size, caller) and its first XIAOTAI_TRANSPORT_MEMTRACE_FAILURES failed
 * allocations (size, caller, free internal memory, largest internal block);
 * blocks beyond the slots are counted as lost. Off by default on every
 * platform: production firmware never wraps the stack's allocator. */
#define XIAOTAI_TRANSPORT_MEMTRACE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_TRANSPORT_MEMTRACE, 0)

/* Fixed sizes of the trace (the values the current diagnostic build uses). */
#define XIAOTAI_TRANSPORT_MEMTRACE_SLOTS 192U
#define XIAOTAI_TRANSPORT_MEMTRACE_FAILURES 8U

/* Listeners a build may register (XIAOTAI_LINK_LISTENERS_MAX in
 * xiaotai/link.h), 1..16. The table, about 100 bytes per entry, is one BULK
 * block taken at the first registration of a normal boot (none in a
 * provisioning boot). The default covers the inbound modules of every current
 * product (device call 2, web stream 1); a product that selects more raises
 * it. */
#ifdef CONFIG_XIAOTAI_TRANSPORT_LISTENERS
#define XIAOTAI_TRANSPORT_LISTENERS CONFIG_XIAOTAI_TRANSPORT_LISTENERS
#else
#define XIAOTAI_TRANSPORT_LISTENERS 4
#endif
#if XIAOTAI_TRANSPORT_LISTENERS < 1 || XIAOTAI_TRANSPORT_LISTENERS > 16
#error "XIAOTAI_TRANSPORT_LISTENERS must be 1..16"
#endif
