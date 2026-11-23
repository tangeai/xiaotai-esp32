// SPDX-License-Identifier: MIT
/* mod_settings_no_media: a product without the media engine (audio_none.c).
 * Whatever audio parts the board has, only screen.sleep is registered, no
 * audio intent exists, the audio shortcuts answer XIAOTAI_ERR_NOT_SUPPORTED,
 * and the initial event covers screen.sleep alone. */
#include <string.h>
#include "settings_harness.h"

int main(void)
{
    const fake_board_t full = {.output = true, .input = true, .display = true,
                               .gain_db = 24, .gain_min_db = 0, .gain_max_db = 36};
    fake_board_set(&full);
    CHECK(harness_start(NULL) == 0);
    CHECK(xiaotai_setting_count() == 1);
    CHECK(strcmp(xiaotai_setting_at(0)->name, XIAOTAI_SETTING_SCREEN_SLEEP) == 0);
    CHECK(strcmp(xiaotai_setting_at(0)->key, XIAOTAI_SETTING_KEY_SCREEN_SLEEP) == 0);
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_UP));
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_DOWN));
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_SET));
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE));
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_MIC_MUTE));
    CHECK(owner_step(1) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(owner_preview(50) == XIAOTAI_ERR_NOT_SUPPORTED);
    owner_restore();
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(!status.has_speaker && !status.has_microphone && !status.has_mic_gain && status.has_screen);
    CHECK(status.volume == 0 && !status.guidance && status.screen_sleep_s == 300 && !status.previewing);
    CHECK(probe_total() == 2);
    CHECK(strcmp(probe_at(0)->name, XIAOTAI_EVENT_STARTED) == 0);
    CHECK(strcmp(probe_at(1)->name, XIAOTAI_EVENT_SETTINGS_CHANGED) == 0);
    CHECK(strcmp(probe_at(1)->text, "screen.sleep") == 0 && probe_at(1)->value == 300);
    CHECK(owner_set("screen.sleep", 1800) == XIAOTAI_OK);
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_CHANGED, "screen.sleep") == 1800);
    CHECK(harness_stored("sleep") == 1800);
    char text[1024];
    owner_diag(text, sizeof(text));
    CHECK(strstr(text, "volume=- speaker_muted=- mic_muted=- mic_gain=- gain_min=- gain_max=- guidance=- "
                       "sleep=1800 ") == text);
    CHECK(strstr(text, "applied=0 ") != NULL);
    harness_stop();

    /* No display either: nothing to register, the module still starts. */
    const fake_board_t audio_only = {.output = true, .input = true};
    fake_board_set(&audio_only);
    CHECK(harness_start(NULL) == 0);
    CHECK(xiaotai_setting_count() == 0);
    CHECK(probe_total() == 1);
    harness_stop();
    puts("PASS: without the media engine only screen.sleep exists and the audio shortcuts are not supported");
    return 0;
}
