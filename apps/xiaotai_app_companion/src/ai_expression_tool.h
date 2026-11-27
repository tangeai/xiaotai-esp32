// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/port/err.h"

/* [init] Registers the conversation's expression tool "show_expression"
 * (ai_expression_tool.c). Called where the device shows a face; compiled when
 * the product selects the ai_talk module. */
xiaotai_err_t companion_ai_expression_tool_init(void);
