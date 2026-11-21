// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the settings module (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig). Both
 * default to off, which is how most deployed products behave; a product that
 * behaves otherwise today turns the option on in its configuration. */
#include "xiaotai/config.h"

/* A late hardware refusal (XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR "volume", "mute"
 * or "gain") of a value the user changed since start takes the setting back
 * to the value the media engine still applies, and XIAOTAI_EVENT_SETTINGS_REFUSED
 * follows. Off: the setting keeps the refused value (the stored preference
 * stays what the user chose, as on devices whose controls are applied by an
 * audio task that only logs a failure); the refusal is counted and recorded
 * in the event log. A refusal the media engine returns at once is always
 * taken back, and a failure of the values applied at start never is. */
#define XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL \
    XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL, 0)

/* xiaotai_settings_volume_step() with a positive step that raises the volume
 * also clears the speaker mute. Off: the speaker mute is left as it is. */
#define XIAOTAI_SETTINGS_VOLUME_UP_UNMUTES \
    XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_SETTINGS_VOLUME_UP_UNMUTES, 0)
