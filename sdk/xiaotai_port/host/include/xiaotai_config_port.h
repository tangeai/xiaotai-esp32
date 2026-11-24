// SPDX-License-Identifier: MIT
#pragma once
/* xiaotai/config.h on the host: there is no Kconfig, every option takes the
 * default written in its configuration header unless the compiler command line
 * defines the CONFIG_XIAOTAI_* symbol. */

#define XIAOTAI_CONFIG_KCONFIG 0
#define XIAOTAI_CONFIG_UNSET(default_value) default_value
