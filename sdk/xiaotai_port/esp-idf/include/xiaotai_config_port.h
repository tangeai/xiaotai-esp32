// SPDX-License-Identifier: MIT
#pragma once
/* xiaotai/config.h on ESP-IDF: options come from Kconfig. A bool option that is
 * off has no symbol in sdkconfig.h, so "unset" means off whatever the default. */
#include "sdkconfig.h"

#define XIAOTAI_CONFIG_KCONFIG 1
#define XIAOTAI_CONFIG_UNSET(default_value) 0
