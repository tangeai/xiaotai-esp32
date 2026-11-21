// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the platform core (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). */
#include "xiaotai/config.h"

/* Zero keeps the signaling port's legacy paired stack budget. */
#ifdef CONFIG_XIAOTAI_PLATFORM_SIGNALING_EVENT_STACK_BYTES
#define XIAOTAI_PLATFORM_SIGNALING_EVENT_STACK_BYTES CONFIG_XIAOTAI_PLATFORM_SIGNALING_EVENT_STACK_BYTES
#else
#define XIAOTAI_PLATFORM_SIGNALING_EVENT_STACK_BYTES 0U
#endif

/* The development command AT+PLATFORM RECONNECT [<1..50>] (xiaotai/platform.h,
 * engine lifecycle). Its handler is compiled only with this option, from its
 * own source file, with the symbol prefix xiaotai_platform_at_dev_, so a
 * build with the option off contains none of it (checked after linking from
 * the manifest's "option" and "symbol"). Off by default on every platform:
 * production firmware cannot drop and rebuild its signaling on request. */
#define XIAOTAI_PLATFORM_RECONNECT_COMMAND XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_PLATFORM_RECONNECT_COMMAND, 0)
