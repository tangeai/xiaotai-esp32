// SPDX-License-Identifier: MIT
#include "xiaotai/modules/settings.h"

#include "xiaotai/settings_confirmation.h"

xiaotai_err_t xiaotai_settings_confirm_text(const char *name, const char *value)
{
    return fw_settings_confirm_text(name, value);
}

xiaotai_err_t xiaotai_settings_confirm_int_selection(const char *name, int32_t value)
{
    return fw_settings_confirm_int_selection(name, value);
}
