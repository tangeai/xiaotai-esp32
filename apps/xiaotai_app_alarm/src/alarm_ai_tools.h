// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/port/err.h"

/* [init] Registers the application's tools of the AI conversation
 * (alarm_ai_tools.c) and allocates their one buffer. Compiled when the
 * product selects the ai_talk module. */
xiaotai_err_t alarm_ai_tools_init(void);
/* Host tests: free the buffer. */
void alarm_ai_tools_reset(void);
