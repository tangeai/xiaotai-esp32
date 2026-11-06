// SPDX-License-Identifier: MIT
#include "xmini_settings.h"
#include "xmini_settings_state.h"
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

/* v1 reserved=0 implies guidance on; v2 stores the explicit 0/1 in that byte. */
typedef struct { uint8_t version, volume, gain_db, reserved; } settings_record_t;
static xmini_settings_state_t s_state;
static bool s_loading, s_quiesced;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t xmini_settings_load(int *volume, int *gain_db)
{
    if (!volume || !gain_db) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    bool busy = s_loading || s_state.loaded;
    if (!busy) s_loading = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy) return ESP_ERR_INVALID_STATE;

    esp_err_t err = nvs_flash_init();
    settings_record_t record = {.version = 1, .volume = 40, .gain_db = 24};
    if (err == ESP_OK) {
        nvs_handle_t handle;
        err = nvs_open("xmini_ui", NVS_READONLY, &handle);
        if (err == ESP_OK) {
            size_t size = sizeof(record);
            err = nvs_get_blob(handle, "audio", &record, &size);
            nvs_close(handle);
            if (err == ESP_OK && (size != sizeof(record) ||
                                  (record.version != 1 && record.version != 2) ||
                                  (record.version == 1 ? record.reserved != 0 : record.reserved > 1) ||
                                  !xmini_settings_values_valid(record.volume, record.gain_db)))
                err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            record = (settings_record_t){.version = 1, .volume = 40, .gain_db = 24};
            err = ESP_OK;
        }
    }
    portENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        xmini_settings_state_init(&s_state, record.volume, record.gain_db);
        s_state.guidance_enabled = s_state.durable_guidance_enabled = record.version == 1 || record.reserved != 0;
        *volume = record.volume;
        *gain_db = record.gain_db;
    } else {
        s_state.error = err;
        ++s_state.revision;
    }
    s_loading = false;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t xmini_settings_schedule(int volume, int gain_db)
{
    if (!xmini_settings_values_valid(volume, gain_db)) return ESP_ERR_INVALID_ARG;
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    bool accepted = !s_quiesced && xmini_settings_state_schedule(&s_state, volume, gain_db, now_ms);
    portEXIT_CRITICAL(&s_lock);
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t xmini_settings_schedule_guidance(bool enabled)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    bool accepted = !s_quiesced && xmini_settings_state_schedule_guidance(&s_state, enabled, now_ms);
    portEXIT_CRITICAL(&s_lock);
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t xmini_settings_quiesce_begin(void)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t error = s_quiesced || !s_state.loaded ?
        (s_state.error ? s_state.error : ESP_ERR_INVALID_STATE) : ESP_OK;
    if (!error) { s_quiesced = true; ++s_state.revision; }
    portEXIT_CRITICAL(&s_lock);
    return error;
}

void xmini_settings_quiesce_end(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_quiesced) { s_quiesced = false; ++s_state.revision; }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xmini_settings_quiesce_retry(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_quiesced && s_state.loaded && s_state.error && !s_state.writing &&
        s_state.attempted_generation == s_state.generation &&
        xmini_settings_state_schedule(&s_state, s_state.volume, s_state.gain_db, now_ms);
    portEXIT_CRITICAL(&s_lock);
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void xmini_settings_process(void)
{
    xmini_settings_write_t write;
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    bool pending = xmini_settings_state_take(&s_state, now_ms, &write);
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return;

    settings_record_t record = {.version = 2, .volume = (uint8_t)write.volume,
        .gain_db = (uint8_t)write.gain_db, .reserved = write.guidance_enabled ? 1 : 0};
    nvs_handle_t handle;
    esp_err_t err = nvs_open("xmini_ui", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "audio", &record, sizeof(record));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    portENTER_CRITICAL(&s_lock);
    xmini_settings_state_finish(&s_state, write.generation, err);
    portEXIT_CRITICAL(&s_lock);
    if (err != ESP_OK)
        ESP_LOGE("xmini_settings", "audio settings write failed: %s; explicit change/retry required", esp_err_to_name(err));
}

xmini_settings_status_t xmini_settings_status(void)
{
    portENTER_CRITICAL(&s_lock);
    bool pending = xmini_settings_state_pending(&s_state);
    xmini_settings_status_t status = {.volume = s_state.volume, .gain_db = s_state.gain_db,
        .guidance_enabled = s_state.guidance_enabled,
        .loaded = s_state.loaded, .saved = s_state.loaded && !pending, .pending = pending, .quiesced = s_quiesced,
        .attempt_pending = pending && (s_state.writing || s_state.attempted_generation != s_state.generation),
        .error = s_state.error, .revision = s_state.revision};
    portEXIT_CRITICAL(&s_lock);
    return status;
}
