// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

xiaotai_err_t door_templates_register(void);
/* The displays the wide templates draw for (an RGB565 panel of at least this size). */
bool door_templates_cover(uint16_t width, uint16_t height);
