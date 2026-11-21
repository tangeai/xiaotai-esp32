// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_product_volume_application
/* mod_settings_volume (and mod_settings_volume_unmute, built with
 * XIAOTAI_SETTINGS_VOLUME_UP_UNMUTES): volume steps (configured size,
 * clamping, nothing at a limit, refusals, quiesce), the speaker mute on a step
 * up as the option says, the preview (heard, never stored, no allocation,
 * restore, ended by the next change), and the door station's volume slider
 * ported from tests/lvgl/test_door_preferences.c: for every pair of old and
 * new positions 0..10, a drag during maintenance changes nothing, a drag is
 * heard at once without being stored, the release stores the value only when
 * it changed and reaches the hardware once; a hardware refusal of the drag
 * leaves the setting where it was. */
#include <string.h>
#include "settings_harness.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/settings_config.h"
#include "xiaotai_mem_host.h"

#define UNMUTES XIAOTAI_SETTINGS_VOLUME_UP_UNMUTES

static const fake_board_t FULL = {.output = true, .input = true, .display = true,
                                  .gain_db = 24, .gain_min_db = 0, .gain_max_db = 36};

static int test_steps(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    fake_media_clear_calls();
    CHECK(owner_step(1) == XIAOTAI_OK && setting("audio.volume") == 80);
    CHECK(owner_step(-2) == XIAOTAI_OK && setting("audio.volume") == 60);
    CHECK(owner_step(5) == XIAOTAI_OK && setting("audio.volume") == 100); /* 110 clamped */
    CHECK(fake_media_call_count() == 3 && fake_media_count(FAKE_MEDIA_VOLUME) == 3);
    CHECK(fake_media_call(2)->value == 100);
    CHECK(harness_stored("volume") == 100);
    /* At the top nothing changes and the result is still OK. */
    CHECK(owner_step(1) == XIAOTAI_OK && setting("audio.volume") == 100);
    CHECK(fake_media_call_count() == 3);
    CHECK(owner_step(-20) == XIAOTAI_OK && setting("audio.volume") == 0);
    CHECK(owner_step(-1) == XIAOTAI_OK && setting("audio.volume") == 0);
    CHECK(owner_step(0) == XIAOTAI_OK);
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.steps == 6 && stats.steps_at_limit == 2);
    CHECK(fake_media_count(FAKE_MEDIA_VOLUME) == 4);

    /* A refusal returns the engine's result and the setting keeps its value. */
    fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_ERR_INVALID_ARG);
    CHECK(owner_step(1) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(setting("audio.volume") == 0);
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.volume") == XIAOTAI_ERR_INVALID_ARG);

    /* During maintenance a step is refused like every setting change. */
    CHECK(owner_quiesce() == XIAOTAI_OK);
    fake_media_clear_calls();
    CHECK(owner_step(1) == XIAOTAI_ERR_BUSY);
    CHECK(setting("audio.volume") == 0 && fake_media_call_count() == 0);
    owner_release();
    harness_stop();

    /* The step size is configured; a step that passes the limit stops at it. */
    fake_board_set(&FULL);
    const xiaotai_settings_config_t config = {.volume_default = 40, .volume_step = 10};
    CHECK(harness_start(&config) == 0);
    CHECK(owner_set("audio.volume", 95) == XIAOTAI_OK);
    CHECK(owner_step(1) == XIAOTAI_OK && setting("audio.volume") == 100);
    CHECK(owner_set("audio.volume", 5) == XIAOTAI_OK);
    CHECK(owner_step(-1) == XIAOTAI_OK && setting("audio.volume") == 0);
    harness_stop();
    fake_board_set(&FULL);
    const xiaotai_settings_config_t coarse = {.volume_step = 25};
    CHECK(harness_start(&coarse) == 0);
    CHECK(owner_step(-1) == XIAOTAI_OK && setting("audio.volume") == 45);
    harness_stop();
    return 0;
}

static int test_step_and_mute(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    CHECK(owner_set("audio.speaker_muted", 1) == XIAOTAI_OK);
    fake_media_clear_calls();
    /* Down never unmutes. */
    CHECK(owner_step(-1) == XIAOTAI_OK && setting("audio.speaker_muted") == 1);
    CHECK(fake_media_count(FAKE_MEDIA_SPEAKER_MUTED) == 0);
    CHECK(owner_step(1) == XIAOTAI_OK && setting("audio.volume") == 70);
    if (UNMUTES) {
        CHECK(setting("audio.speaker_muted") == 0);
        CHECK(fake_media_count(FAKE_MEDIA_SPEAKER_MUTED) == 1);
        CHECK(fake_media_call(fake_media_call_count() - 1)->kind == FAKE_MEDIA_SPEAKER_MUTED);
        CHECK(fake_media_call(fake_media_call_count() - 1)->value == 0);
        CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.speaker_muted") == 0);
    } else {
        CHECK(setting("audio.speaker_muted") == 1);
        CHECK(fake_media_count(FAKE_MEDIA_SPEAKER_MUTED) == 0);
    }
    /* At the top nothing changes, the mute included. */
    CHECK(owner_set("audio.volume", 100) == XIAOTAI_OK);
    CHECK(owner_set("audio.speaker_muted", 1) == XIAOTAI_OK);
    CHECK(owner_step(1) == XIAOTAI_OK && setting("audio.speaker_muted") == 1);
    harness_stop();
    return 0;
}

static int test_preview(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    fake_media_clear_calls();
    probe_clear();
    harness_clear_writes();
    xiaotai_mem_host_clear();
    CHECK(owner_preview(30) == XIAOTAI_OK);
    /* Heard at once, the setting untouched, nothing stored, nothing allocated. */
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(fake_media_call_count() == 1 && fake_media_call(0)->kind == FAKE_MEDIA_VOLUME);
    CHECK(fake_media_call(0)->value == 30);
    CHECK(setting("audio.volume") == 70 && harness_writes() == 0 && probe_total() == 0);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.previewing && status.preview_volume == 30 && status.volume == 70);
    CHECK(owner_preview(35) == XIAOTAI_OK);
    xiaotai_settings_status(&status);
    CHECK(status.previewing && status.preview_volume == 35);
    char text[1024];
    owner_diag(text, sizeof(text));
    CHECK(strstr(text, "volume=70 ") == text && strstr(text, "previewing=1 preview=35 ") != NULL);

    /* Restore returns to the setting's value, once. */
    xiaotai_mem_host_clear();
    owner_restore();
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(fake_media_call_count() == 3 && fake_media_call(2)->value == 70);
    xiaotai_settings_status(&status);
    CHECK(!status.previewing && status.preview_volume == 0);
    owner_restore();
    CHECK(fake_media_call_count() == 3);

    /* Range. */
    CHECK(owner_preview(101) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(fake_media_call_count() == 3);

    /* The next change of the setting ends the preview and is applied. */
    CHECK(owner_preview(20) == XIAOTAI_OK);
    CHECK(owner_set("audio.volume", 40) == XIAOTAI_OK);
    xiaotai_settings_status(&status);
    CHECK(!status.previewing && status.volume == 40);
    CHECK(fake_media_call_count() == 5 && fake_media_call(4)->value == 40);
    CHECK(harness_writes() == 1 && harness_stored("volume") == 40);
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.previews == 3);
    harness_stop();
    return 0;
}

static int test_slider(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    unsigned combinations = 0;
    for (int32_t old = 0; old <= 10; ++old) {
        for (int32_t next = 0; next <= 10; ++next) {
            CHECK(owner_set("audio.volume", old * 10) == XIAOTAI_OK);
            fake_media_clear_calls();
            probe_clear();
            harness_clear_writes();

            /* Maintenance: the drag is refused, nothing is heard or stored. */
            CHECK(owner_quiesce() == XIAOTAI_OK);
            CHECK(owner_preview((uint8_t)(next * 10)) == XIAOTAI_ERR_BUSY);
            CHECK(owner_set("audio.volume", next * 10) == XIAOTAI_ERR_BUSY);
            owner_restore();
            CHECK(setting("audio.volume") == old * 10);
            CHECK(fake_media_call_count() == 0 && harness_writes() == 0);
            owner_release();

            /* The drag is heard at once and not stored. */
            CHECK(owner_preview((uint8_t)(next * 10)) == XIAOTAI_OK);
            CHECK(fake_media_call_count() == 1 && fake_media_call(0)->value == next * 10);
            CHECK(setting("audio.volume") == old * 10 && harness_writes() == 0);

            /* The release stores a changed value, and the hardware hears it once. */
            CHECK(owner_set("audio.volume", next * 10) == XIAOTAI_OK);
            owner_restore();
            CHECK(setting("audio.volume") == next * 10);
            CHECK(fake_media_call_count() == 1);
            CHECK(harness_writes() == (next != old ? 1U : 0U));
            CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.volume") == (next != old ? 1U : 0U));
            xiaotai_settings_status_t status;
            xiaotai_settings_status(&status);
            CHECK(!status.previewing);
            ++combinations;
        }
    }
    CHECK(combinations == 121);

    /* The hardware refuses the drag: the setting stays and nothing is stored. */
    CHECK(owner_set("audio.volume", 40) == XIAOTAI_OK);
    harness_clear_writes();
    probe_clear();
    fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_FAIL);
    CHECK(owner_preview(90) == XIAOTAI_FAIL);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(!status.previewing && status.volume == 40);
    CHECK(harness_writes() == 0 && probe_total() == 0);
    CHECK(harness_eventlog("preview.refused") == XIAOTAI_FAIL);
    harness_stop();
    return 0;
}

static int test_preview_then_refused_setting(bool strict)
{
    const xiaotai_settings_config_t config = {.volume_default = 40, .screen_sleep_default_s = -1,
                                               .volume_accept_before_save = strict};
    fake_board_set(&FULL);
    /* 67 is a legacy Xmini percentage, not a screen product's 10% step.
     * The harness starts at the decoded registry value, not legacy bytes. */
    CHECK(harness_preset("volume", 67));
    CHECK(harness_start(&config) == 0);
    fake_media_clear_calls();
    probe_clear();
    harness_clear_writes();
    CHECK(owner_preview(89) == XIAOTAI_OK && setting("audio.volume") == 67 && harness_writes() == 0);
    fake_media_set_applied(89, false, 24);
    fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_ERR_INVALID_STATE);
    CHECK(owner_set("audio.volume", 74) == XIAOTAI_OK && setting("audio.volume") == 67);
    owner_restore();
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    CHECK(media.volume_percent == 67 && fake_media_call_count() == 3 && fake_media_call(2)->value == 67);
    CHECK(harness_stored("volume") == 67 && (!strict || harness_writes() == 0));
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.volume") == XIAOTAI_ERR_INVALID_STATE);
    CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.volume") == 0);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(!status.previewing && status.apply_error == XIAOTAI_ERR_INVALID_STATE);

    /* A preview already at the base value needs no second engine write. */
    fake_media_clear_calls();
    CHECK(owner_preview(67) == XIAOTAI_OK);
    fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_ERR_INVALID_STATE);
    CHECK(owner_set("audio.volume", 74) == XIAOTAI_OK && setting("audio.volume") == 67);
    owner_restore();
    CHECK(fake_media_call_count() == 2);
    xiaotai_media_status(&media);
    CHECK(media.volume_percent == 67);
    if (strict) {
        CHECK(harness_stored("volume") == 67 && harness_writes() == 0);
        CHECK(owner_preview(75) == XIAOTAI_OK);
        CHECK(owner_set("audio.volume", 75) == XIAOTAI_OK);
        xiaotai_settings_status(&status);
        CHECK(!status.previewing && fake_media_call_count() == 3 && fake_media_call(2)->value == 75);
        CHECK(harness_stored("volume") == 75 && harness_writes() == 1);
        fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_ERR_TIMEOUT);
        CHECK(owner_step(1) == XIAOTAI_ERR_TIMEOUT && setting("audio.volume") == 75 && harness_writes() == 1);
    }
    harness_stop();
    return 0;
}

int main(void)
{
    if (test_steps()) return 1;
    if (test_step_and_mute()) return 1;
    if (test_preview()) return 1;
    if (test_slider()) return 1;
    if (test_preview_then_refused_setting(false) || test_preview_then_refused_setting(true)) return 1;
    printf("PASS: steps, limits, mute on step up %s, preview and restore, 121 slider combinations\n",
           UNMUTES ? "cleared" : "kept");
    return 0;
}
