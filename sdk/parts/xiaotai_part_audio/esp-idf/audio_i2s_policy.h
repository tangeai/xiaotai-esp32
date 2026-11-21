// SPDX-License-Identifier: MIT
#pragma once
#include "sdkconfig.h"

#if (defined(CONFIG_I2S_ISR_IRAM_SAFE) && CONFIG_I2S_ISR_IRAM_SAFE) || \
    (defined(CONFIG_I2S_ISR_CACHE_SAFE) && CONFIG_I2S_ISR_CACHE_SAFE)
#error "Audio capture ring accesses BULK memory in the receive ISR; I2S cache-safe interrupts are unsupported"
#endif
