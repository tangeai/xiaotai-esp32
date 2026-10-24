// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/xmini_settings/xmini_settings.c"

static int64_t s_now;
static int s_init_error, s_set_error, s_commit_error;
static unsigned s_initializations, s_sets, s_commits;
static bool s_record_present, s_change_during_write, s_freeze_during_write;
static settings_record_t s_record, s_staged;

const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
int64_t esp_timer_get_time(void) { return s_now * 1000; }
esp_err_t nvs_flash_init(void) { ++s_initializations; return s_init_error; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "xmini_ui"));
    *handle = 1;
    return mode == NVS_READONLY && !s_record_present ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 1 && !strcmp(key, "audio") && *size == sizeof(s_record));
    memcpy(value, &s_record, sizeof(s_record));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle == 1 && !strcmp(key, "audio") && size == sizeof(s_record));
    ++s_sets;
    memcpy(&s_staged, value, sizeof(s_staged));
    if (s_freeze_during_write) {
        s_freeze_during_write = false;
        assert(xmini_settings_quiesce_begin() == ESP_OK);
        assert(xmini_settings_schedule(90, 36) == ESP_ERR_INVALID_STATE);
        assert(xmini_settings_schedule_guidance(false) == ESP_ERR_INVALID_STATE);
    }
    if (s_change_during_write) {
        s_change_during_write = false;
        assert(xmini_settings_schedule(80, 12) == ESP_OK);
    }
    return s_set_error;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++s_commits;
    if (!s_commit_error) { s_record = s_staged; s_record_present = true; }
    return s_commit_error;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

static void reset_runtime(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_loading = s_quiesced = false;
    s_now = 0;
    s_init_error = s_set_error = s_commit_error = 0;
    s_initializations = s_sets = s_commits = 0;
    s_change_during_write = s_freeze_during_write = false;
}

static void test_quiesce_inflight_write(void)
{
    reset_runtime(); s_record_present = false;
    int volume, gain;
    assert(xmini_settings_load(&volume, &gain) == ESP_OK);
    assert(xmini_settings_schedule(65, 24) == ESP_OK);
    s_freeze_during_write = true; s_now = 1000;
    xmini_settings_process();
    assert(s_commits == 1 && s_record.volume == 65 && xmini_settings_status().saved);
    assert(xmini_settings_status().quiesced);
    assert(xmini_settings_quiesce_begin() == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_schedule(65, 24) == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_status().volume == 65);
    xmini_settings_quiesce_end();
    assert(!xmini_settings_status().quiesced && xmini_settings_schedule(90, 36) == ESP_OK);
}

static void test_quiesce_explicit_retry(void)
{
    reset_runtime(); s_record_present = false;
    int volume, gain;
    assert(xmini_settings_quiesce_retry() == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_load(&volume, &gain) == ESP_OK);
    assert(xmini_settings_quiesce_retry() == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_schedule(65, 24) == ESP_OK);
    assert(xmini_settings_quiesce_begin() == ESP_OK);
    assert(xmini_settings_quiesce_retry() == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_status().attempt_pending);
    s_commit_error = ESP_FAIL; s_now = 1000; xmini_settings_process();
    assert(s_commits == 1 && xmini_settings_status().pending && !xmini_settings_status().attempt_pending);
    assert(xmini_settings_quiesce_retry() == ESP_OK);
    assert(xmini_settings_quiesce_retry() == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_status().quiesced && xmini_settings_status().attempt_pending);
    assert(xmini_settings_status().error == ESP_FAIL);
    assert(xmini_settings_schedule(90, 36) == ESP_ERR_INVALID_STATE);
    s_now = 1999; xmini_settings_process(); assert(s_commits == 1);
    s_commit_error = 0; s_now = 2000; xmini_settings_process();
    assert(s_commits == 2 && s_record.volume == 65 && s_record.gain_db == 24);
    assert(xmini_settings_status().saved && !xmini_settings_status().attempt_pending && !xmini_settings_status().error);
    assert(xmini_settings_quiesce_retry() == ESP_ERR_INVALID_STATE);
    xmini_settings_quiesce_end();
    assert(xmini_settings_quiesce_retry() == ESP_ERR_INVALID_STATE);
}

static void test_guidance_persistence(void)
{
    reset_runtime(); s_record_present = true;
    s_record = (settings_record_t){.version = 1, .volume = 67, .gain_db = 13};
    int volume, gain;
    assert(xmini_settings_load(&volume, &gain) == ESP_OK && volume == 67 && gain == 13);
    assert(xmini_settings_status().guidance_enabled && xmini_settings_status().saved && !s_sets);
    unsigned revision = xmini_settings_status().revision;
    assert(xmini_settings_schedule_guidance(true) == ESP_OK && xmini_settings_status().revision == revision);
    assert(xmini_settings_schedule_guidance(false) == ESP_OK);
    assert(xmini_settings_schedule(70, 15) == ESP_OK);
    s_now = 999; xmini_settings_process(); assert(!s_sets);
    s_now = 1000; xmini_settings_process();
    assert(s_commits == 1 && s_record.version == 2 && s_record.reserved == 0);
    assert(s_record.volume == 70 && s_record.gain_db == 15 && xmini_settings_status().saved);
    reset_runtime();
    assert(xmini_settings_load(&volume, &gain) == ESP_OK && volume == 70 && gain == 15);
    assert(!xmini_settings_status().guidance_enabled && xmini_settings_status().saved && !s_sets);
    assert(xmini_settings_quiesce_begin() == ESP_OK);
    assert(xmini_settings_schedule_guidance(true) == ESP_ERR_INVALID_STATE);
    assert(!xmini_settings_status().guidance_enabled && !xmini_settings_status().pending);
    xmini_settings_quiesce_end();
    assert(xmini_settings_schedule_guidance(true) == ESP_OK);
    s_commit_error = ESP_FAIL; s_now = 1000; xmini_settings_process();
    assert(s_commits == 1 && xmini_settings_status().pending && xmini_settings_status().error == ESP_FAIL);
    for (unsigned i = 0; i < 1000; ++i) { ++s_now; xmini_settings_process(); }
    assert(s_commits == 1 && s_record.reserved == 0);
    assert(xmini_settings_schedule_guidance(true) == ESP_OK);
    assert(xmini_settings_quiesce_begin() == ESP_OK);
    s_commit_error = 0; s_now += 1000; xmini_settings_process();
    assert(s_commits == 2 && s_record.reserved == 1 && xmini_settings_status().saved);
    assert(xmini_settings_status().guidance_enabled && !xmini_settings_status().error);
    reset_runtime();
    assert(xmini_settings_load(&volume, &gain) == ESP_OK && xmini_settings_status().guidance_enabled);
    reset_runtime(); s_record.reserved = 2;
    assert(xmini_settings_load(&volume, &gain) == ESP_ERR_INVALID_STATE && !s_sets);
    reset_runtime(); s_record.version = 1; s_record.reserved = 1;
    assert(xmini_settings_load(&volume, &gain) == ESP_ERR_INVALID_STATE && !s_sets);
    reset_runtime(); s_record_present = false;
    assert(xmini_settings_schedule_guidance(false) == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_load(&volume, &gain) == ESP_OK && xmini_settings_status().guidance_enabled);
    xmini_settings_state_t state = {0};
    xmini_settings_write_t write;
    assert(xmini_settings_state_init(&state, 40, 24));
    assert(xmini_settings_state_schedule_guidance(&state, false, 0));
    assert(xmini_settings_state_take(&state, 1000, &write) && !write.guidance_enabled);
    assert(xmini_settings_state_schedule_guidance(&state, true, 1001));
    assert(xmini_settings_state_finish(&state, write.generation, ESP_OK));
    assert(xmini_settings_state_pending(&state) && state.guidance_enabled && !state.durable_guidance_enabled);
    assert(!xmini_settings_state_take(&state, 2000, &write));
    assert(xmini_settings_state_take(&state, 2001, &write) && write.guidance_enabled);
    assert(xmini_settings_state_finish(&state, write.generation, ESP_OK) && !xmini_settings_state_pending(&state));
}

int main(void)
{
    int volume = -3, gain = -4;
    reset_runtime();
    assert(xmini_settings_schedule(40, 24) == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_quiesce_begin() == ESP_ERR_INVALID_STATE);
    s_init_error = ESP_ERR_NVS_NO_FREE_PAGES;
    assert(xmini_settings_load(&volume, &gain) == s_init_error);
    assert(volume == -3 && gain == -4 && !xmini_settings_status().loaded);
    assert(xmini_settings_status().error == s_init_error);
    assert(xmini_settings_quiesce_begin() == s_init_error);
    xmini_settings_process();
    assert(!s_sets && !s_commits && s_initializations == 1);

    reset_runtime();
    s_record_present = false;
    assert(xmini_settings_load(&volume, &gain) == ESP_OK);
    assert(volume == 40 && gain == 24 && xmini_settings_status().saved);
    assert(xmini_settings_load(&volume, &gain) == ESP_ERR_INVALID_STATE);
    assert(xmini_settings_schedule(-1, 24) == ESP_ERR_INVALID_ARG);
    assert(xmini_settings_schedule(101, 24) == ESP_ERR_INVALID_ARG);
    assert(xmini_settings_schedule(40, -1) == ESP_ERR_INVALID_ARG);
    assert(xmini_settings_schedule(40, 37) == ESP_ERR_INVALID_ARG);
    assert(xmini_settings_schedule(40, 24) == ESP_OK);
    s_now = 100000;
    xmini_settings_process();
    assert(!s_sets && !s_commits);

    s_now = 0;
    assert(xmini_settings_schedule(50, 30) == ESP_OK);
    s_now = 999;
    xmini_settings_process();
    assert(!s_sets);
    assert(xmini_settings_schedule(60, 36) == ESP_OK);
    s_now = 1998;
    xmini_settings_process();
    assert(!s_sets);
    s_now = 1999;
    xmini_settings_process();
    assert(s_sets == 1 && s_commits == 1 && s_record.volume == 60 && s_record.gain_db == 36);
    assert(xmini_settings_status().saved && !xmini_settings_status().pending);

    reset_runtime();
    assert(xmini_settings_load(&volume, &gain) == ESP_OK && volume == 60 && gain == 36);
    assert(xmini_settings_schedule(0, 0) == ESP_OK);
    s_set_error = ESP_FAIL;
    s_now = 1000;
    xmini_settings_process();
    assert(s_sets == 1 && !s_commits && xmini_settings_status().pending && !xmini_settings_status().saved);
    assert(xmini_settings_status().error == ESP_FAIL);
    for (int i = 0; i < 10000; ++i) { ++s_now; xmini_settings_process(); }
    assert(s_sets == 1);
    assert(xmini_settings_schedule(0, 0) == ESP_OK);
    s_set_error = 0;
    s_commit_error = ESP_FAIL;
    s_now += 1000;
    xmini_settings_process();
    assert(s_sets == 2 && s_commits == 1 && !xmini_settings_status().saved);
    for (int i = 0; i < 10000; ++i) { ++s_now; xmini_settings_process(); }
    assert(s_sets == 2);
    assert(xmini_settings_schedule(0, 0) == ESP_OK);
    s_commit_error = 0;
    s_now += 1000;
    xmini_settings_process();
    assert(xmini_settings_status().saved && xmini_settings_status().error == ESP_OK);

    assert(xmini_settings_schedule(70, 8) == ESP_OK);
    s_change_during_write = true;
    s_now += 1000;
    xmini_settings_process();
    assert(s_record.volume == 70 && xmini_settings_status().volume == 80 && xmini_settings_status().pending);
    s_now += 999;
    xmini_settings_process();
    assert(s_record.volume == 70);
    ++s_now;
    xmini_settings_process();
    assert(s_record.volume == 80 && s_record.gain_db == 12 && xmini_settings_status().saved);

    reset_runtime();
    s_record = (settings_record_t){.version = 3, .volume = 40, .gain_db = 24};
    assert(xmini_settings_load(&volume, &gain) == ESP_ERR_INVALID_STATE);
    assert(s_record.version == 3 && !s_sets && !xmini_settings_status().saved);

    xmini_settings_state_t state = {0};
    xmini_settings_write_t write;
    assert(xmini_settings_state_init(&state, 40, 24));
    assert(xmini_settings_state_schedule(&state, 100, 36, 0));
    assert(xmini_settings_state_schedule(&state, 40, 24, 500));
    assert(!xmini_settings_state_take(&state, 2000, &write));
    assert(!xmini_settings_state_pending(&state));
    assert(xmini_settings_state_schedule(&state, 100, 36, 3000));
    assert(xmini_settings_state_take(&state, 4000, &write));
    assert(!xmini_settings_state_finish(&state, write.generation + 1, 0));
    assert(state.writing);
    assert(xmini_settings_state_finish(&state, write.generation, 0));
    test_quiesce_inflight_write();
    test_quiesce_explicit_retry();
    test_guidance_persistence();
    puts("settings: real NVS adapter, defaults, bounds, debounce, failures, explicit retry, concurrent edits and reload passed");
    return 0;
}
