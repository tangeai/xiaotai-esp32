// SPDX-License-Identifier: MIT
/* Fake hardware abstraction for the settings module's tests: which audio
 * output, audio input (with its gain range) and display the board has. The
 * module only asks whether part 0 exists and reads the input's gain facts
 * and the output's volume ceiling. */
#include <string.h>
#include "settings_harness.h"
#include "xiaotai/preferences.h"

static fake_board_t s_board;
static xiaotai_audio_input_t s_input;
static xiaotai_audio_output_t s_output;
static xiaotai_display_t s_display;

void fake_board_set(const fake_board_t *board)
{
    memset(&s_board, 0, sizeof(s_board));
    if (board) s_board = *board;
    memset(&s_input, 0, sizeof(s_input));
    s_input.info.gain_db = s_board.gain_db;
    s_input.info.gain_min_db = s_board.gain_min_db;
    s_input.info.gain_max_db = s_board.gain_max_db;
    memset(&s_output, 0, sizeof(s_output));
    s_output.info.volume_max_percent = s_board.volume_max_percent;
    memset(&s_display, 0, sizeof(s_display));
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    return index == 0 && s_board.input ? &s_input : NULL;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    return index == 0 && s_board.output ? &s_output : NULL;
}

const xiaotai_display_t *xiaotai_hal_display(size_t index)
{
    return index == 0 && s_board.display ? &s_display : NULL;
}

/* These fixtures use scalar settings and do not have a packed record. */
#ifndef SETTINGS_TEST_REAL_PREFERENCES
static bool s_pending;
void fake_preferences_set_pending(bool pending) { s_pending = pending; }
bool xiaotai_preferences_pending(void) { return s_pending; }
xiaotai_err_t xiaotai_preferences_packed_load_error(void) { return XIAOTAI_OK; }
/* Keys the module keeps through the preferences service directly (the
 * volume ceiling): the same store the framework keeps settings in here. */
xiaotai_err_t xiaotai_preferences_load_int(const char *key, int32_t fallback, int32_t *value)
{
    int32_t stored = 0;
    size_t size = sizeof(stored);
    *value = fallback;
    if (xiaotai_kv_get("xt_settings", key, &stored, &size, 1000) != XIAOTAI_OK || size != sizeof(stored)) {
        return XIAOTAI_ERR_NOT_FOUND;
    }
    *value = stored;
    return XIAOTAI_OK;
}
xiaotai_err_t xiaotai_preferences_store_int(const char *key, int32_t value)
{
    return xiaotai_kv_set("xt_settings", key, &value, sizeof(value), 1000);
}
#endif
