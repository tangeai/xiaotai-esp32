// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/port/err.h"

/* Private edge to the settings module, not an application setter.
 * [owner] A user confirmed a value: use the configured store even when
 * the value is unchanged, but do not publish another change. */
xiaotai_err_t fw_settings_confirm_text(const char *name, const char *value);
xiaotai_err_t fw_settings_confirm_int_selection(const char *name, int32_t value);
