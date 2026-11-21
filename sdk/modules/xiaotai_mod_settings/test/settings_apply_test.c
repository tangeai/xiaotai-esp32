// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_runtime_wifi.c::runtime_wifi
/* mod_settings_apply (and mod_settings_apply_take_back, built with
 * XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL): the stored audio settings are
 * applied at start in a fixed order and announced once each after
 * system.started; every change is applied once; a value the engine refuses
 * at once is taken back without being applied again; late refusals
 * (media.output_error) are taken back to the engine's applied value or kept,
 * as the option says, never for values applied at start; "power" is only
 * counted; a volume failure during a preview only ends the preview; the
 * take-back waits for the end of a quiesce; not-saved notices, the pending
 * save in the status, and the intents. */
#include <string.h>
#include "settings_harness.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/settings_config.h"

#define TAKE_BACK XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL

static const fake_board_t FULL = {.output = true, .input = true, .display = true,
                                  .gain_db = 24, .gain_min_db = 0, .gain_max_db = 36};

static int check_call(size_t index, fake_media_kind_t kind, int32_t value)
{
    const fake_media_call_t *call = fake_media_call(index);
    if (!call || call->kind != kind || call->value != value) {
        fprintf(stderr, "media call %zu: expected kind %d value %d, got %s %d %d\n", index, (int)kind, (int)value,
                call ? "" : "none", call ? (int)call->kind : -1, call ? (int)call->value : -1);
        return 1;
    }
    return 0;
}

static int check_event(size_t index, const char *name, const char *text, int32_t value)
{
    const probe_event_t *event = probe_at(index);
    if (!event || strcmp(event->name, name) != 0 || (text && strcmp(event->text, text) != 0) ||
        (text && event->value != value)) {
        fprintf(stderr, "event %zu: expected %s %s=%d, got %s %s=%d\n", index, name, text ? text : "", value,
                event ? event->name : "none", event ? event->text : "", event ? event->value : 0);
        return 1;
    }
    return 0;
}

static int test_start(void)
{
    fake_board_set(&FULL);
    CHECK(harness_preset("volume", 60));
    CHECK(harness_preset("spk_mute", 1));
    CHECK(harness_preset("mic_mute", 1));
    CHECK(harness_preset("mic_gain", 30));
    CHECK(harness_start(NULL) == 0);
    /* Applied once each, in the order deployed firmware applies them. */
    CHECK(fake_media_call_count() == 4);
    if (check_call(0, FAKE_MEDIA_VOLUME, 60) || check_call(1, FAKE_MEDIA_SPEAKER_MUTED, 1) ||
        check_call(2, FAKE_MEDIA_MIC_MUTED, 1) || check_call(3, FAKE_MEDIA_GAIN, 30)) {
        return 1;
    }
    CHECK(fake_media_off_owner() == 0);
    /* system.started, then one settings.changed per setting, in registration order. */
    CHECK(probe_total() == 7);
    if (check_event(0, XIAOTAI_EVENT_STARTED, NULL, 0) ||
        check_event(1, XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.volume", 60) ||
        check_event(2, XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.speaker_muted", 1) ||
        check_event(3, XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.mic_muted", 1) ||
        check_event(4, XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.mic_gain", 30) ||
        check_event(5, XIAOTAI_EVENT_SETTINGS_CHANGED, "guidance.enabled", 1) ||
        check_event(6, XIAOTAI_EVENT_SETTINGS_CHANGED, "screen.sleep", 300)) {
        return 1;
    }
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.applied == 4 && stats.changes == 0 && stats.refused == 0);
    harness_stop();

    /* A failure while applying the stored values is recorded, never taken
     * back: the stored value stays the user's choice. */
    fake_board_set(&FULL);
    CHECK(harness_preset("volume", 60));
    fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_FAIL);
    harness_clear_writes();
    CHECK(harness_start(NULL) == 0);
    CHECK(setting("audio.volume") == 60);
    CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_REFUSED, NULL) == 0);
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.volume") == 60);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.apply_error == XIAOTAI_OK); /* the later applies succeeded */
    CHECK(harness_eventlog("start.apply") == XIAOTAI_FAIL);
    CHECK(harness_writes() == 0);
    harness_stop();
    return 0;
}

static int test_changes(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    fake_media_clear_calls();
    probe_clear();
    harness_clear_writes();

    CHECK(owner_set("audio.volume", 50) == XIAOTAI_OK);
    CHECK(fake_media_call_count() == 1 && check_call(0, FAKE_MEDIA_VOLUME, 50) == 0);
    CHECK(probe_total() == 1 && check_event(0, XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.volume", 50) == 0);
    CHECK(harness_stored("volume") == 50 && harness_writes() == 1);

    CHECK(owner_set("audio.speaker_muted", 1) == XIAOTAI_OK);
    CHECK(owner_set("audio.mic_muted", 1) == XIAOTAI_OK);
    CHECK(owner_set("audio.mic_gain", 12) == XIAOTAI_OK);
    CHECK(fake_media_call_count() == 4);
    if (check_call(1, FAKE_MEDIA_SPEAKER_MUTED, 1) || check_call(2, FAKE_MEDIA_MIC_MUTED, 1) ||
        check_call(3, FAKE_MEDIA_GAIN, 12)) {
        return 1;
    }
    /* Applied by the application: an event, no engine call. */
    CHECK(owner_set("guidance.enabled", 0) == XIAOTAI_OK);
    CHECK(owner_set("screen.sleep", 600) == XIAOTAI_OK);
    CHECK(fake_media_call_count() == 4);
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_CHANGED, "guidance.enabled") == 0);
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_CHANGED, "screen.sleep") == 600);
    CHECK(probe_total() == 6);

    /* The same value again changes nothing; a value out of range is refused. */
    CHECK(owner_set("audio.volume", 50) == XIAOTAI_OK);
    CHECK(owner_set("audio.volume", 101) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(owner_set("audio.mic_gain", 37) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(fake_media_call_count() == 4 && probe_total() == 6);
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.changes == 6 && stats.applied == 4 + 4 && stats.refused == 0);
    CHECK(fake_media_off_owner() == 0);
    harness_stop();
    return 0;
}

static int test_refused_at_once(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    fake_media_clear_calls();
    probe_clear();
    harness_clear_writes();
    fake_media_fail_next(FAKE_MEDIA_VOLUME, XIAOTAI_ERR_INVALID_ARG);
    CHECK(owner_set("audio.volume", 80) == XIAOTAI_OK);
    /* Back to the last accepted value, applied once (the refused one), never again. */
    CHECK(setting("audio.volume") == 70);
    CHECK(fake_media_call_count() == 1 && check_call(0, FAKE_MEDIA_VOLUME, 80) == 0);
    CHECK(probe_total() == 1);
    if (check_event(0, XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.volume", XIAOTAI_ERR_INVALID_ARG)) return 1;
    CHECK(harness_stored("volume") == 70);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.volume == 70 && status.apply_error == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.refused == 1 && stats.changes == 1);
    CHECK(harness_eventlog("refused") == XIAOTAI_ERR_INVALID_ARG);

    /* The next accepted change clears the error. */
    CHECK(owner_set("audio.volume", 60) == XIAOTAI_OK);
    xiaotai_settings_status(&status);
    CHECK(status.volume == 60 && status.apply_error == XIAOTAI_OK);

    /* Speaker mute and gain take the same path. */
    fake_media_fail_next(FAKE_MEDIA_SPEAKER_MUTED, XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(owner_set("audio.speaker_muted", 1) == XIAOTAI_OK);
    CHECK(setting("audio.speaker_muted") == 0);
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.speaker_muted") == XIAOTAI_ERR_NOT_SUPPORTED);
    fake_media_fail_next(FAKE_MEDIA_GAIN, XIAOTAI_ERR_INVALID_ARG);
    CHECK(owner_set("audio.mic_gain", 33) == XIAOTAI_OK);
    CHECK(setting("audio.mic_gain") == 24);
    xiaotai_settings_stats(&stats);
    CHECK(stats.refused == 3);
    harness_stop();
    return 0;
}

static int test_refused_later(void)
{
    fake_board_set(&FULL);
    const xiaotai_settings_config_t cfg = {.speaker_accept_before_save = true};
    CHECK(harness_start(&cfg) == 0);
    /* The user changes all three; the engine's task later fails each and
     * still applies the start values. */
    CHECK(owner_set("audio.volume", 50) == XIAOTAI_OK);
    CHECK(owner_set("audio.speaker_muted", 1) == XIAOTAI_OK);
    CHECK(owner_set("audio.mic_gain", 30) == XIAOTAI_OK);
    fake_media_set_applied(70, false, 24);
    fake_media_clear_calls();
    probe_clear();
    harness_clear_writes();

    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_FAIL, "volume");
    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_ERR_TIMEOUT, "mute");
    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_ERR_INVALID_ARG, "gain");
    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_FAIL, "power");
    CHECK(fake_media_call_count() == 0); /* a take-back is never sent to the engine */
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.apply_error == XIAOTAI_ERR_INVALID_ARG); /* the last of the three */
    char text[1024];
    owner_diag(text, sizeof(text));
    CHECK(strstr(text, "output_errors=4 ") != NULL);
    CHECK(harness_eventlog("output.failed") == XIAOTAI_FAIL);
    if (TAKE_BACK) {
        CHECK(setting("audio.volume") == 70 && setting("audio.speaker_muted") == 0 && setting("audio.mic_gain") == 24);
        CHECK(probe_total() == 3);
        if (check_event(0, XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.volume", XIAOTAI_FAIL) ||
            check_event(1, XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.speaker_muted", XIAOTAI_ERR_TIMEOUT) ||
            check_event(2, XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.mic_gain", XIAOTAI_ERR_INVALID_ARG)) {
            return 1;
        }
        CHECK(stats.refused == 3);
        CHECK(harness_stored("volume") == 70 && harness_stored("spk_mute") == 0 && harness_stored("mic_gain") == 24);
        CHECK(strstr(text, "late_kept=0 ") != NULL);
    } else {
        /* Kept: the stored preference stays what the user chose. */
        CHECK(setting("audio.volume") == 50 && setting("audio.speaker_muted") == 1 && setting("audio.mic_gain") == 30);
        CHECK(probe_total() == 0 && stats.refused == 0 && harness_writes() == 0);
        CHECK(strstr(text, "late_kept=3 ") != NULL);
        CHECK(harness_eventlog("late.kept") >= 0);
    }

    /* A failure whose applied value equals the setting has nothing to take back. */
    probe_clear();
    CHECK(owner_set("audio.volume", 40) == XIAOTAI_OK);
    fake_media_set_applied(40, false, 24);
    probe_clear();
    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_FAIL, "volume");
    CHECK(setting("audio.volume") == 40 && probe_total() == 0);
    harness_stop();

    /* Values applied at start are never taken back, whatever the option. */
    fake_board_set(&FULL);
    CHECK(harness_preset("volume", 60));
    CHECK(harness_start(NULL) == 0);
    fake_media_set_applied(0, false, 24);
    probe_clear();
    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_FAIL, "volume");
    CHECK(setting("audio.volume") == 60 && probe_total() == 0);
    owner_diag(text, sizeof(text));
    CHECK(strstr(text, "late_kept=1 ") != NULL);
    harness_stop();
    return 0;
}

static int test_refused_while_quiesced(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    CHECK(owner_set("audio.volume", 50) == XIAOTAI_OK);
    fake_media_set_applied(70, false, 24);
    probe_clear();
    CHECK(owner_quiesce() == XIAOTAI_OK);
    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_FAIL, "volume");
    /* The registry accepts nothing while quiesced. */
    CHECK(setting("audio.volume") == 50);
    char text[1024];
    owner_diag(text, sizeof(text));
    if (TAKE_BACK) {
        CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_REFUSED, "audio.volume") == 1);
        CHECK(strstr(text, "take_back_deferred=1") != NULL);
    } else {
        CHECK(probe_total() == 0 && strstr(text, "take_back_deferred=0") != NULL);
    }
    fake_media_clear_calls();
    owner_release();
    CHECK(setting("audio.volume") == (TAKE_BACK ? 70 : 50));
    CHECK(fake_media_call_count() == 0);
    CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.volume") == 0);
    harness_stop();
    return 0;
}

static int test_preview_failure(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    CHECK(owner_set("audio.volume", 50) == XIAOTAI_OK);
    CHECK(owner_preview(90) == XIAOTAI_OK);
    fake_media_set_applied(50, false, 24);
    probe_clear();
    harness_emit(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, XIAOTAI_FAIL, "volume");
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(!status.previewing && status.volume == 50);
    CHECK(probe_total() == 0);
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.refused == 0);
    CHECK(harness_eventlog("preview.refused") == XIAOTAI_FAIL);
    harness_stop();
    return 0;
}

static int test_notices(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    /* Only this module's settings count. */
    harness_emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, XIAOTAI_FAIL, "audio.volume");
    harness_emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, XIAOTAI_FAIL, "companion.ack_voice");
    harness_emit(XIAOTAI_EVENT_SETTING_NOT_SAVED, XIAOTAI_ERR_TIMEOUT, "screen.sleep");
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.not_saved == 2);
    CHECK(harness_eventlog("not_saved") == XIAOTAI_ERR_TIMEOUT);

    /* The pending save shows in the status, and the revision follows it. */
    xiaotai_settings_status_t before, after, again;
    xiaotai_settings_status(&before);
    CHECK(!before.save_pending);
    fake_preferences_set_pending(true);
    xiaotai_settings_status(&after);
    CHECK(after.save_pending && after.revision > before.revision);
    xiaotai_settings_status(&again);
    CHECK(again.save_pending && again.revision == after.revision);
    fake_preferences_set_pending(false);
    xiaotai_settings_status(&again);
    CHECK(!again.save_pending && again.revision > after.revision);
    /* Any change of a value moves the revision too. */
    xiaotai_settings_status(&before);
    CHECK(owner_set("screen.sleep", 60) == XIAOTAI_OK);
    xiaotai_settings_status(&after);
    CHECK(after.revision > before.revision && after.screen_sleep_s == 60);
    harness_stop();
    return 0;
}

typedef struct { bool muted; xiaotai_err_t result; } saved_mute_t;

static void set_saved_mute(void *ctx)
{
    saved_mute_t *request = ctx;
    request->result = xiaotai_settings_set_mic_mute(request->muted);
}

static int test_saved_mute(void)
{
    fake_board_set(&FULL);
    CHECK(harness_preset("mic_mute", 1));
    CHECK(harness_start(NULL) == 0);
    fake_media_clear_calls();
    harness_clear_writes();
    probe_clear();
    saved_mute_t request = {.muted = false};
    CHECK(xiaotai_settings_set_mic_mute(false) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(owner_quiesce() == XIAOTAI_OK);
    harness_on_owner(set_saved_mute, &request);
    CHECK(request.result == XIAOTAI_ERR_BUSY && fake_media_call_count() == 0 && harness_writes() == 0);
    owner_release();

    fake_media_fail_next(FAKE_MEDIA_MIC_MUTED, XIAOTAI_FAIL);
    harness_on_owner(set_saved_mute, &request);
    CHECK(request.result == XIAOTAI_FAIL && fake_media_call_count() == 1);
    CHECK(fake_media_mic_setting_at_request() == 1 && setting("audio.mic_muted") == 1);
    CHECK(harness_stored("mic_mute") == 1 && harness_writes() == 0);
    CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.mic_muted") == 0);

    harness_on_owner(set_saved_mute, &request);
    CHECK(request.result == XIAOTAI_OK && fake_media_call_count() == 2);
    CHECK(fake_media_mic_setting_at_request() == 1 && setting("audio.mic_muted") == 0);
    CHECK(harness_stored("mic_mute") == 0 && harness_writes() == 1);
    CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.mic_muted") == 1);
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.mic_muted") == 0);

    harness_on_owner(set_saved_mute, &request);
    CHECK(request.result == XIAOTAI_OK && fake_media_call_count() == 3 && harness_writes() == 1);
    CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.mic_muted") == 1);
    harness_stop();
    return 0;
}

static int test_intents(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_VOLUME_UP, 0) == XIAOTAI_OK);
    CHECK(setting("audio.volume") == 80);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_VOLUME_DOWN, 0) == XIAOTAI_OK);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_VOLUME_DOWN, 0) == XIAOTAI_OK);
    CHECK(setting("audio.volume") == 60);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_VOLUME_SET, 35) == XIAOTAI_OK);
    CHECK(setting("audio.volume") == 35);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_VOLUME_SET, 101) == XIAOTAI_OK);
    CHECK(setting("audio.volume") == 35);
    CHECK(harness_eventlog("intent.invalid") == 101);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE, 1) == XIAOTAI_OK);
    CHECK(setting("audio.speaker_muted") == 1);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE, -1) == XIAOTAI_OK);
    CHECK(setting("audio.speaker_muted") == 0);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE, 5) == XIAOTAI_OK);
    CHECK(setting("audio.speaker_muted") == 0);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_MIC_MUTE, -1) == XIAOTAI_OK);
    CHECK(setting("audio.mic_muted") == 1);
    CHECK(owner_intent(XIAOTAI_INTENT_AUDIO_MIC_MUTE, 0) == XIAOTAI_OK);
    CHECK(setting("audio.mic_muted") == 0);
    CHECK(fake_media_count(FAKE_MEDIA_MIC_MUTED) == 1 + 2); /* start + two changes */
    /* An open channel may post them (none is local-only). */
    const xiaotai_args_t args = {.value = 1};
    CHECK(xiaotai_intent_post_external(XIAOTAI_INTENT_AUDIO_MIC_MUTE, &args) == XIAOTAI_OK);
    CHECK(harness_settle());
    CHECK(setting("audio.mic_muted") == 1);
    harness_stop();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) return strcmp(argv[1], "saved_mute") == 0 ? test_saved_mute() : 1;
    if (test_start()) return 1;
    if (test_changes()) return 1;
    if (test_refused_at_once()) return 1;
    if (test_refused_later()) return 1;
    if (test_refused_while_quiesced()) return 1;
    if (test_preview_failure()) return 1;
    if (test_notices()) return 1;
    if (test_intents()) return 1;
    printf("PASS: start apply and initial events, changes, refusals at once and later (take back %s), "
           "quiesced take-back, preview failure, notices, intents\n",
           TAKE_BACK ? "on" : "off");
    return 0;
}
