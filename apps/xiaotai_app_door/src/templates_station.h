// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

xiaotai_err_t door_station_pages_register(void);
/* Whether a colour display of this size shows them. */
bool door_station_pages_cover(uint16_t width, uint16_t height);
