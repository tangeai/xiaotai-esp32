// SPDX-License-Identifier: MIT
/* Audio settings without the media engine in the build: no audio setting
 * (audio.*, guidance.enabled) and no audio intent is registered, whatever
 * audio parts the board has, and the audio shortcuts are not supported. Only
 * screen.sleep remains (settings.c). */
#include "settings_internal.h"

xiaotai_err_t xiaotai_settings_audio_register(void) { return XIAOTAI_OK; }

void xiaotai_settings_audio_prepare(void) {}

void xiaotai_settings_audio_reset(void) {}

xiaotai_err_t xiaotai_settings_volume_step(int steps)
{
    (void)steps;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

xiaotai_err_t xiaotai_settings_volume_preview(uint8_t percent)
{
    (void)percent;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

void xiaotai_settings_volume_restore(void) {}

xiaotai_err_t xiaotai_settings_set_mic_mute(bool muted)
{
    (void)muted;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
