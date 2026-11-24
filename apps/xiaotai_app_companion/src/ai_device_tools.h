// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/port/err.h"

/* [init] Registers the conversation's device tools the board allows
 * (ai_device_tools.c). Compiled when the product selects the ai_talk module. */
xiaotai_err_t companion_ai_device_tools_init(void);
