// SPDX-License-Identifier: MIT
/* mod_settings_register: which settings the module registers for which parts
 * (the gain range and default from the input part), their names, storage
 * keys, kinds, ranges and defaults, the product configuration, stored values
 * (loaded, out of range ignored without erasing, nothing written at start),
 * the intents that exist, and the diagnostics section. */
#include <string.h>
#include "settings_harness.h"

typedef struct {
    const char *name, *key;
    xiaotai_setting_kind_t kind;
    int32_t min, max, default_value;
} expected_t;

static int check_registry(const expected_t *expected, size_t count)
{
    CHECK(xiaotai_setting_count() == count);
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_setting_t *s = xiaotai_setting_at(i);
        CHECK(s != NULL);
        if (strcmp(s->name, expected[i].name) != 0 || strcmp(s->key, expected[i].key) != 0) {
            fprintf(stderr, "setting %zu is %s/%s, expected %s/%s\n", i, s->name, s->key, expected[i].name,
                    expected[i].key);
            return 1;
        }
        CHECK(s->kind == expected[i].kind);
        CHECK(s->min == expected[i].min && s->max == expected[i].max);
        CHECK(s->default_value == expected[i].default_value);
    }
    return 0;
}

static const fake_board_t FULL = {.output = true, .input = true, .display = true,
                                  .gain_db = 24, .gain_min_db = 0, .gain_max_db = 36};

static int test_full_board(void)
{
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    const expected_t expected[] = {
        {"audio.volume", "volume", XIAOTAI_SETTING_INT, 0, 100, 70},
        {"audio.speaker_muted", "spk_mute", XIAOTAI_SETTING_BOOL, 0, 1, 0},
        {"audio.mic_muted", "mic_mute", XIAOTAI_SETTING_BOOL, 0, 1, 0},
        {"audio.mic_gain", "mic_gain", XIAOTAI_SETTING_INT, 0, 36, 24},
        {"guidance.enabled", "guidance", XIAOTAI_SETTING_BOOL, 0, 1, 1},
        {"screen.sleep", "sleep", XIAOTAI_SETTING_INT, 0, 86400, 300},
    };
    if (check_registry(expected, 6)) return 1;
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.has_speaker && status.has_microphone && status.has_mic_gain && status.has_screen);
    CHECK(status.volume == 70 && !status.speaker_muted && !status.mic_muted && status.guidance);
    CHECK(status.mic_gain_db == 24 && status.mic_gain_min_db == 0 && status.mic_gain_max_db == 36);
    CHECK(status.screen_sleep_s == 300 && !status.previewing && status.apply_error == XIAOTAI_OK);
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_UP));
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_DOWN));
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_SET));
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE));
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_MIC_MUTE));
    /* Nothing was stored, so nothing is written: defaults stay defaults. */
    CHECK(harness_writes() == 0);
    CHECK(harness_stored("volume") == -999);
    char text[1024];
    owner_diag(text, sizeof(text));
    CHECK(strstr(text, "volume=70 speaker_muted=0 mic_muted=0 mic_gain=24 gain_min=0 gain_max=36 guidance=1 "
                       "sleep=300 previewing=0 preview=- apply_error=0 save_pending=0 revision=") == text);
    CHECK(strstr(text, "\n+SETTINGS-STATS: changes=0 applied=4 refused=0 steps=0 steps_at_limit=0 previews=0 "
                       "not_saved=0 output_errors=0 late_kept=0 take_back_deferred=0\n") != NULL);
    /* Configuration is refused once the module runs. */
    const xiaotai_settings_config_t late = {.volume_default = 40};
    CHECK(xiaotai_settings_configure(&late) == XIAOTAI_ERR_INVALID_STATE);
    harness_stop();
    return 0;
}

static int test_fixed_gain_without_display(void)
{
    const fake_board_t board = {.output = true, .input = true, .gain_db = 6, .gain_min_db = 6, .gain_max_db = 6};
    fake_board_set(&board);
    CHECK(harness_start(NULL) == 0);
    const expected_t expected[] = {
        {"audio.volume", "volume", XIAOTAI_SETTING_INT, 0, 100, 70},
        {"audio.speaker_muted", "spk_mute", XIAOTAI_SETTING_BOOL, 0, 1, 0},
        {"audio.mic_muted", "mic_mute", XIAOTAI_SETTING_BOOL, 0, 1, 0},
        {"guidance.enabled", "guidance", XIAOTAI_SETTING_BOOL, 0, 1, 1},
    };
    if (check_registry(expected, 4)) return 1;
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.has_speaker && status.has_microphone && !status.has_mic_gain && !status.has_screen);
    CHECK(status.mic_gain_db == 0 && status.screen_sleep_s == 0);
    char text[1024];
    owner_diag(text, sizeof(text));
    CHECK(strstr(text, "mic_gain=- gain_min=- gain_max=- guidance=1 sleep=- ") != NULL);
    harness_stop();
    return 0;
}

static int test_output_only(void)
{
    const fake_board_t board = {.output = true};
    fake_board_set(&board);
    CHECK(harness_start(NULL) == 0);
    const expected_t expected[] = {
        {"audio.volume", "volume", XIAOTAI_SETTING_INT, 0, 100, 70},
        {"audio.speaker_muted", "spk_mute", XIAOTAI_SETTING_BOOL, 0, 1, 0},
        {"guidance.enabled", "guidance", XIAOTAI_SETTING_BOOL, 0, 1, 1},
    };
    if (check_registry(expected, 3)) return 1;
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_UP));
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_MIC_MUTE));
    harness_stop();
    return 0;
}

static int test_input_only(void)
{
    const fake_board_t board = {.input = true, .gain_db = 9, .gain_min_db = -12, .gain_max_db = 12};
    fake_board_set(&board);
    CHECK(harness_start(NULL) == 0);
    const expected_t expected[] = {
        {"audio.mic_muted", "mic_mute", XIAOTAI_SETTING_BOOL, 0, 1, 0},
        {"audio.mic_gain", "mic_gain", XIAOTAI_SETTING_INT, -12, 12, 9},
    };
    if (check_registry(expected, 2)) return 1;
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_UP));
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE));
    CHECK(xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_MIC_MUTE));
    /* No output: the volume shortcuts are not supported, the guidance switch
     * does not exist. */
    CHECK(owner_step(1) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(owner_preview(50) == XIAOTAI_ERR_NOT_SUPPORTED);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(!status.has_speaker && status.has_microphone && status.has_mic_gain && !status.guidance);
    CHECK(status.mic_gain_min_db == -12 && status.mic_gain_max_db == 12 && status.mic_gain_db == 9);
    harness_stop();
    return 0;
}

static int test_display_only(void)
{
    const fake_board_t board = {.display = true};
    fake_board_set(&board);
    CHECK(harness_start(NULL) == 0);
    const expected_t expected[] = {{"screen.sleep", "sleep", XIAOTAI_SETTING_INT, 0, 86400, 300}};
    if (check_registry(expected, 1)) return 1;
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_VOLUME_UP));
    CHECK(!xiaotai_intent_exists(XIAOTAI_INTENT_AUDIO_MIC_MUTE));
    CHECK(fake_media_call_count() == 0);
    harness_stop();

    /* Nothing at all: no setting, the module still starts. */
    fake_board_set(NULL);
    CHECK(harness_start(NULL) == 0);
    CHECK(xiaotai_setting_count() == 0);
    harness_stop();
    return 0;
}

static int test_gain_default_clamped(void)
{
    fake_board_t board = {.input = true, .gain_db = 40, .gain_min_db = 0, .gain_max_db = 36};
    fake_board_set(&board);
    CHECK(harness_start(NULL) == 0);
    CHECK(xiaotai_setting_at(1) && xiaotai_setting_at(1)->default_value == 36);
    harness_stop();
    board.gain_db = -5;
    fake_board_set(&board);
    CHECK(harness_start(NULL) == 0);
    CHECK(xiaotai_setting_at(1) && xiaotai_setting_at(1)->default_value == 0);
    harness_stop();
    return 0;
}

static int test_configuration(void)
{
    /* Range checks, each refused without changing the configuration. */
    const xiaotai_settings_config_t loud = {.volume_default = 101};
    const xiaotai_settings_config_t big_step = {.volume_step = 51};
    const xiaotai_settings_config_t long_sleep = {.screen_sleep_default_s = 86401};
    CHECK(xiaotai_settings_configure(&loud) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_settings_configure(&big_step) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_settings_configure(&long_sleep) == XIAOTAI_ERR_INVALID_ARG);
    const xiaotai_settings_config_t edges = {.volume_default = 100, .volume_step = 50, .screen_sleep_default_s = 86400};
    CHECK(xiaotai_settings_configure(&edges) == XIAOTAI_OK);
    xiaotai_settings_reset();

    /* Default 40, never sleeping, guidance off by default. */
    fake_board_set(&FULL);
    const xiaotai_settings_config_t quiet = {.volume_default = 40, .screen_sleep_default_s = -1,
                                             .guidance_default_off = true};
    CHECK(harness_start(&quiet) == 0);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.volume == 40 && status.screen_sleep_s == 0 && !status.guidance);
    CHECK(xiaotai_setting_at(0)->default_value == 40);
    CHECK(xiaotai_setting_at(4)->default_value == 0);
    CHECK(xiaotai_setting_at(5)->default_value == 0);
    harness_stop();

    /* NULL restores every default. */
    CHECK(xiaotai_settings_configure(&quiet) == XIAOTAI_OK);
    CHECK(xiaotai_settings_configure(NULL) == XIAOTAI_OK);
    fake_board_set(&FULL);
    CHECK(harness_start(NULL) == 0);
    xiaotai_settings_status(&status);
    CHECK(status.volume == 70 && status.screen_sleep_s == 300 && status.guidance);
    harness_stop();
    return 0;
}

static int test_stored_values(void)
{
    fake_board_set(&FULL);
    CHECK(harness_preset("volume", 55));
    CHECK(harness_preset("spk_mute", 1));
    CHECK(harness_preset("mic_mute", 1));
    CHECK(harness_preset("mic_gain", 30));
    CHECK(harness_preset("guidance", 0));
    CHECK(harness_preset("sleep", 600));
    harness_clear_writes();
    CHECK(harness_start(NULL) == 0);
    xiaotai_settings_status_t status;
    xiaotai_settings_status(&status);
    CHECK(status.volume == 55 && status.speaker_muted && status.mic_muted && status.mic_gain_db == 30);
    CHECK(!status.guidance && status.screen_sleep_s == 600);
    CHECK(harness_writes() == 0);
    harness_stop();

    /* Out of range: the default is used and the stored value is not erased. */
    fake_board_set(&FULL);
    CHECK(harness_preset("volume", 101));
    CHECK(harness_preset("mic_gain", 37));
    CHECK(harness_preset("sleep", 90000));
    harness_clear_writes();
    CHECK(harness_start(NULL) == 0);
    xiaotai_settings_status(&status);
    CHECK(status.volume == 70 && status.mic_gain_db == 24 && status.screen_sleep_s == 300);
    CHECK(harness_writes() == 0);
    CHECK(harness_stored("volume") == 101 && harness_stored("mic_gain") == 37 && harness_stored("sleep") == 90000);
    harness_stop();
    return 0;
}

int main(void)
{
    if (test_full_board()) return 1;
    if (test_fixed_gain_without_display()) return 1;
    if (test_output_only()) return 1;
    if (test_input_only()) return 1;
    if (test_display_only()) return 1;
    if (test_gain_default_clamped()) return 1;
    if (test_configuration()) return 1;
    if (test_stored_values()) return 1;
    puts("PASS: settings registered by parts, keys, ranges, defaults, configuration, stored values, diagnostics");
    return 0;
}
