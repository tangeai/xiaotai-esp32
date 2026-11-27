// SPDX-License-Identifier: MIT
#pragma once
/* Config port: build-time configuration for SDK code.
 *
 * SDK code never includes sdkconfig.h and never reads a CONFIG_* symbol outside
 * a configuration header. Each component that has options owns two files:
 *
 *   <component>/Kconfig                      the options, on ESP-IDF
 *   <component>/include/xiaotai/<name>_config.h
 *                                            one XIAOTAI_<NAME>_<OPTION> macro per option
 *
 * The configuration header includes this file and maps each Kconfig symbol
 * CONFIG_XIAOTAI_<NAME>_<OPTION> to the macro of the same name without the
 * CONFIG_ prefix, giving the value used where Kconfig does not exist (the
 * host). The default written in the header equals the Kconfig default; where
 * the Kconfig default depends on the build (external RAM or not), the header
 * holds the one the host uses and says so. SDK sources include the
 * configuration header and use only XIAOTAI_* macros.
 *
 *   value options (int, hex, string): always
 *       #ifdef CONFIG_XIAOTAI_NET_SCAN_MAX
 *       #define XIAOTAI_NET_SCAN_MAX CONFIG_XIAOTAI_NET_SCAN_MAX
 *       #else
 *       #define XIAOTAI_NET_SCAN_MAX 24
 *       #endif
 *   bool options:
 *       #define XIAOTAI_NET_PROVISION_PORTAL \
 *           XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_NET_PROVISION_PORTAL, 1)
 *
 * A new option is one more entry in its component's own two files; nothing in
 * this port changes. Options are compile-time constants: anything a user or
 * the platform may change at run time is a setting (xiaotai/framework.h), and
 * anything that differs per board is a part descriptor (xiaotai/hal.h).
 *
 * The linked port supplies xiaotai_config_port.h:
 *   ESP-IDF  includes sdkconfig.h; an unset bool option is absent there, so
 *            XIAOTAI_CONFIG_UNSET(default) is 0.
 *   host     defines nothing; XIAOTAI_CONFIG_UNSET(default) is the default. A
 *            host test overrides an option on the compiler command line
 *            (-DCONFIG_XIAOTAI_NET_SCAN_MAX=16, -DCONFIG_XIAOTAI_NET_PROVISION_PORTAL=0). */
#include "xiaotai_config_port.h"

/* 1 or 0 for a bool option: its value when the symbol is defined as 1 or 0,
 * otherwise what the platform means by "unset" (see above). A constant
 * expression, usable in #if and in code. */
#define XIAOTAI_CONFIG_BOOL(option, default_value) \
    XIAOTAI_CONFIG_PICK_(option, XIAOTAI_CONFIG_UNSET(default_value))

/* Implementation of XIAOTAI_CONFIG_BOOL: when `option` expands to 0 or 1, the
 * probe below turns into two arguments and shifts the value into second place;
 * any other token sequence leaves the fallback there. */
#define XIAOTAI_CONFIG_PROBE_0 ~,
#define XIAOTAI_CONFIG_PROBE_1 ~,
#define XIAOTAI_CONFIG_PICK_(value, fallback) XIAOTAI_CONFIG_PICK_1_(value, fallback)
#define XIAOTAI_CONFIG_PICK_1_(value, fallback) \
    XIAOTAI_CONFIG_PICK_2_(XIAOTAI_CONFIG_PROBE_##value value, fallback, ~)
#define XIAOTAI_CONFIG_PICK_2_(...) XIAOTAI_CONFIG_PICK_3_(__VA_ARGS__)
#define XIAOTAI_CONFIG_PICK_3_(first, second, ...) second
