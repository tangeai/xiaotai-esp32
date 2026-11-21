// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

void companion_preferences_prepare(bool seed, uint8_t raw_volume);
xiaotai_err_t companion_preferences_install(void);
void companion_preferences_reset(void);
bool companion_preferences_pending(void);
typedef struct { uint32_t stores, write_failures; } companion_preferences_stats_t;
companion_preferences_stats_t companion_preferences_stats(void);
void companion_preferences_fill(unsigned count);
xiaotai_err_t companion_preferences_flush(void);
