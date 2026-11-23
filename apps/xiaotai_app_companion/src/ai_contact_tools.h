// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/port/err.h"
#include <stdint.h>

xiaotai_err_t companion_ai_contact_tools_init(void);
void companion_ai_contact_tools_observe(uint32_t session, uint32_t link);
void companion_ai_contact_tools_reset(void);
void companion_ai_talk_hint(uint32_t session, const char *text);
