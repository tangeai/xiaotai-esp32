// SPDX-License-Identifier: MIT
/* mod_settings_volume_ceiling: a board whose output plays 100 % at a share of
 * its range (volume_max_percent) converts a volume stored for another ceiling
 * once, so the device keeps its loudness: rounded to the volume step, at least
 * one step, at most 100; then the ceiling key holds the board's ceiling and a
 * later start changes nothing. A device that never stored a volume writes
 * nothing at start; its first stored volume also writes the key. The
 * conversion is not a user change (no admission, applied once at start). A
 * board without a ceiling never reads or writes the key. */
#include <string.h>
#include "settings_harness.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/settings_config.h"

static const fake_board_t CEILING_70 = {.output = true, .input = true, .volume_max_percent = 70};

/* Starts with the stored volume (or none: -1) and ceiling key (or none: -1);
 * checks the volume afterwards and that the engine got it once. */
static int start_with(const fake_board_t *board, const xiaotai_settings_config_t *config, int32_t stored,
                      int32_t marker, int32_t expected)
{
    fake_board_set(board);
    if (stored >= 0) CHECK(harness_preset("volume", stored));
    if (marker >= 0) CHECK(harness_preset("vol_ceiling", marker));
    harness_clear_writes();
    CHECK(harness_start(config) == 0);
    CHECK(setting("audio.volume") == expected);
    CHECK(fake_media_count(FAKE_MEDIA_VOLUME) == 1);
    for (size_t i = 0; i < fake_media_call_count(); ++i) {
        if (fake_media_call(i)->kind == FAKE_MEDIA_VOLUME) CHECK(fake_media_call(i)->value == expected);
    }
    CHECK(probe_last_value(XIAOTAI_EVENT_SETTINGS_CHANGED, "audio.volume") == expected);
    CHECK(harness_settle());
    return 0;
}

static int test_converts_once(void)
{
    static const struct { int32_t stored, expected; } CASES[] = {
        {80, 100}, {100, 100}, {70, 100}, {60, 90}, {50, 70}, {30, 40}, {10, 10}, {1, 10}, {0, 0},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        const int32_t stored = CASES[i].stored, expected = CASES[i].expected;
        if (start_with(&CEILING_70, NULL, stored, -1, expected)) {
            fprintf(stderr, "stored %d\n", (int)stored);
            return 1;
        }
        CHECK(harness_stored("volume") == expected);
        CHECK(harness_stored("vol_ceiling") == 70);
        CHECK(harness_eventlog("vol.convert") == (expected != stored ? expected : -999));
        CHECK(harness_writes() == (expected != stored ? 2U : 1U));
        harness_stop();
    }

    /* The next start (key present) keeps what is stored and writes nothing. */
    if (start_with(&CEILING_70, NULL, 100, 70, 100)) return 1;
    CHECK(harness_writes() == 0 && harness_eventlog("vol.convert") == -999);
    harness_stop();
    return 0;
}

static int test_never_stored(void)
{
    /* Nothing stored: the default, and nothing written at start. */
    if (start_with(&CEILING_70, NULL, -1, -1, 70)) return 1;
    CHECK(harness_writes() == 0 && harness_stored("vol_ceiling") == -999);
    /* The first stored volume is this firmware's: the key goes with it. */
    CHECK(owner_set("audio.volume", 50) == XIAOTAI_OK && setting("audio.volume") == 50);
    CHECK(harness_stored("volume") == 50 && harness_stored("vol_ceiling") == 70);
    harness_clear_writes();
    CHECK(owner_set("audio.volume", 60) == XIAOTAI_OK && harness_stored("volume") == 60);
    CHECK(harness_writes() == 1);
    harness_stop();

    /* Restarted: 60 is kept. */
    if (start_with(&CEILING_70, NULL, 60, 70, 60)) return 1;
    harness_stop();
    return 0;
}

static int test_other_ceiling_and_step(void)
{
    /* Saved for a ceiling of 50, now 70: 60 * 50 / 70 = 42.9 -> 40. */
    if (start_with(&CEILING_70, NULL, 60, 50, 40)) return 1;
    CHECK(harness_stored("vol_ceiling") == 70);
    harness_stop();

    /* Rounded to the configured step: 33 / 0.7 = 47.1 -> 45. */
    const xiaotai_settings_config_t fives = {.volume_step = 5};
    if (start_with(&CEILING_70, &fives, 33, -1, 45)) return 1;
    harness_stop();
    return 0;
}

static int test_not_a_user_change(void)
{
    /* With synchronous acceptance the conversion is still applied once, at
     * start, and counts as no change. */
    const xiaotai_settings_config_t accept = {.volume_accept_before_save = true, .volume_default = 100};
    if (start_with(&CEILING_70, &accept, 80, -1, 100)) return 1;
    xiaotai_settings_stats_t stats;
    xiaotai_settings_stats(&stats);
    CHECK(stats.changes == 0 && stats.refused == 0);
    CHECK(probe_count(XIAOTAI_EVENT_SETTINGS_REFUSED, NULL) == 0);
    harness_stop();
    return 0;
}

static int test_no_ceiling(void)
{
    static const fake_board_t FULL = {.output = true, .input = true};
    static const fake_board_t HUNDRED = {.output = true, .input = true, .volume_max_percent = 100};
    const fake_board_t *const boards[] = {&FULL, &HUNDRED};
    for (size_t i = 0; i < 2; ++i) {
        if (start_with(boards[i], NULL, 80, -1, 80)) return 1;
        CHECK(owner_set("audio.volume", 50) == XIAOTAI_OK);
        CHECK(harness_stored("vol_ceiling") == -999 && harness_stored("volume") == 50);
        harness_stop();
    }
    return 0;
}

int main(void)
{
    if (test_converts_once()) return 1;
    if (test_never_stored()) return 1;
    if (test_other_ceiling_and_step()) return 1;
    if (test_not_a_user_change()) return 1;
    if (test_no_ceiling()) return 1;
    printf("PASS: stored volumes converted once for the ceiling, nothing written for a device that never stored one\n");
    return 0;
}
