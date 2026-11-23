// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the environment module (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). */
#include "xiaotai/config.h"

/* What a storage barrier before the write does when it fails with anything
 * but a timeout (a setting that could not be written, xiaotai_preferences_flush
 * in xiaotai/preferences.h):
 *   0  the switch ends with XIAOTAI_ENVIRONMENT_FAILED and everything is
 *      released at once (the default);
 *   1  the switch stops in XIAOTAI_ENVIRONMENT_ERROR_HELD with every gate
 *      still held, so nothing new begins until someone looks at the settings
 *      and cancels (xiaotai_environment_cancel releases).
 * Products keep the handling their deployed firmware has. A timeout is never
 * held: with a bounded wait it fails the switch, without one it is tried
 * again. */
#define XIAOTAI_ENVIRONMENT_HOLD_ON_SETTINGS_ERROR \
    XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_ENVIRONMENT_HOLD_ON_SETTINGS_ERROR, 0)
