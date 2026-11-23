// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/config.h"

/* The optional UI and the application's view storage share this setting.
 * The UI Kconfig owns the option; the fallback does not require that module. */
#define XIAOTAI_UI_OWNER_FRAME_WORKSPACE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_UI_OWNER_FRAME_WORKSPACE, 0)
