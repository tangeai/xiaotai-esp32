// SPDX-License-Identifier: MIT
/* Fake media engine for the settings module's tests: records every control
 * call (kind, value, whether it came from the owner task), answers with a
 * programmed result, and reports applied values as the real engine keeps
 * them (at the last success) in xiaotai_media_status. */
#include <string.h>
#include "settings_harness.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#ifdef SETTINGS_TEST_REAL_PREFERENCES
#include "xiaotai/preferences.h"
static uint32_t s_stores_at_volume;
uint32_t fake_media_stores_at_volume_request(void) { return s_stores_at_volume; }
#endif

static fake_media_call_t s_calls[FAKE_MEDIA_CALLS_MAX];
static size_t s_count;
static uint32_t s_off_owner;
static xiaotai_err_t s_fail[FAKE_MEDIA_KINDS];
static xiaotai_media_status_t s_status;
static int32_t s_mic_setting_at_request;

void fake_media_reset(void)
{
    memset(s_calls, 0, sizeof(s_calls));
    s_count = 0;
    s_off_owner = 0;
    memset(s_fail, 0, sizeof(s_fail));
    memset(&s_status, 0, sizeof(s_status));
}

void fake_media_clear_calls(void) { s_count = 0; }

size_t fake_media_call_count(void) { return s_count; }

const fake_media_call_t *fake_media_call(size_t index) { return index < s_count ? &s_calls[index] : NULL; }

size_t fake_media_count(fake_media_kind_t kind)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count && i < FAKE_MEDIA_CALLS_MAX; ++i) n += s_calls[i].kind == kind;
    return n;
}

uint32_t fake_media_off_owner(void) { return s_off_owner; }
int32_t fake_media_mic_setting_at_request(void) { return s_mic_setting_at_request; }

void fake_media_fail_next(fake_media_kind_t kind, xiaotai_err_t err) { s_fail[kind] = err; }

void fake_media_set_applied(uint8_t volume, bool speaker_muted, int8_t gain_db)
{
    s_status.volume_applied = volume;
    s_status.speaker_muted_applied = speaker_muted;
    s_status.input_gain_db = gain_db;
}

static xiaotai_err_t record(fake_media_kind_t kind, int32_t value)
{
    if (!xiaotai_on_owner()) ++s_off_owner;
    if (s_count < FAKE_MEDIA_CALLS_MAX) s_calls[s_count] = (fake_media_call_t){.kind = kind, .value = value};
    ++s_count;
    const xiaotai_err_t err = s_fail[kind];
    s_fail[kind] = XIAOTAI_OK;
    return err;
}

/* Requests are recorded at once; a test decides what the hardware applied
 * (fake_media_set_applied), as the engine's task would. */
xiaotai_err_t xiaotai_media_set_volume(uint8_t percent)
{
#ifdef SETTINGS_TEST_REAL_PREFERENCES
    xiaotai_preferences_stats_t stats;
    xiaotai_preferences_stats(&stats);
    s_stores_at_volume = stats.stores;
#endif
    const xiaotai_err_t err = record(FAKE_MEDIA_VOLUME, percent);
    if (err == XIAOTAI_OK) s_status.volume_percent = percent;
    return err;
}

xiaotai_err_t xiaotai_media_set_speaker_muted(bool muted)
{
    const xiaotai_err_t err = record(FAKE_MEDIA_SPEAKER_MUTED, muted);
    if (err == XIAOTAI_OK) s_status.speaker_muted = muted;
    return err;
}

xiaotai_err_t xiaotai_media_set_mic_muted(bool muted)
{
    s_mic_setting_at_request = setting(XIAOTAI_SETTING_AUDIO_MIC_MUTED);
    const xiaotai_err_t err = record(FAKE_MEDIA_MIC_MUTED, muted);
    if (err == XIAOTAI_OK) s_status.mic_muted = muted;
    return err;
}

xiaotai_err_t xiaotai_media_set_input_gain(int8_t gain_db) { return record(FAKE_MEDIA_GAIN, gain_db); }

void xiaotai_media_status(xiaotai_media_status_t *status) { *status = s_status; }
