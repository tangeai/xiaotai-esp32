#include "app_audio_policy.h"

#include "app_memory_policy.h"
#include "app_log_policy.h"
#include "audio_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "media_sink.h"

static const char *TAG = "app_audio_policy";

#define APP_AUDIO_SOURCE_MASK \
    (APP_AUDIO_SOURCE_RTC_MEDIA | APP_AUDIO_SOURCE_AI_CHAT_MEDIA)

static SemaphoreHandle_t s_policy_lock;
static uint32_t s_prepared_sources;
static uint32_t s_active_sources;
static bool s_adaptive_playout_enabled;
static bool s_aec_runtime_active;
static esp_err_t s_last_aec_apply_error = ESP_OK;

static bool app_audio_policy_source_valid(app_audio_source_t source)
{
    const uint32_t value = (uint32_t)source;

    return value != 0U && (value & (value - 1U)) == 0U &&
           (value & ~APP_AUDIO_SOURCE_MASK) == 0U;
}

static const char *app_audio_policy_source_name(app_audio_source_t source)
{
    switch (source) {
    case APP_AUDIO_SOURCE_RTC_MEDIA:
        return "rtc-media";
    case APP_AUDIO_SOURCE_AI_CHAT_MEDIA:
        return "ai-chat-media";
    default:
        return "unknown";
    }
}

static esp_err_t app_audio_policy_apply_locked(void)
{
    const bool adaptive_playout = (s_prepared_sources | s_active_sources) != 0U;
    if (adaptive_playout != s_adaptive_playout_enabled) {
        media_sink_set_audio_profile(
            adaptive_playout ?
                MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL :
                MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY);
        s_adaptive_playout_enabled = adaptive_playout;
    }

    const bool requested_aec_active = s_active_sources != 0U;
    const bool should_apply_aec =
        requested_aec_active != s_aec_runtime_active ||
        (requested_aec_active && s_last_aec_apply_error != ESP_OK);
    esp_err_t ret = ESP_OK;

    if (should_apply_aec) {
        ret = audio_device_set_echo_cancel_active(requested_aec_active);
        if (ret == ESP_OK) {
            s_aec_runtime_active = requested_aec_active;
        } else if (!requested_aec_active) {
            /* The driver stops new processing before waiting for in-flight users. */
            s_aec_runtime_active = false;
        }
        s_last_aec_apply_error = ret;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "apply realtime audio policy failed: requested_aec=%d active=0x%02lx ret=%s",
                 requested_aec_active ? 1 : 0,
                 (unsigned long)s_active_sources,
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t app_audio_policy_init(void)
{
    if (s_policy_lock != NULL) {
        return ESP_OK;
    }

    s_policy_lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    return s_policy_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t app_audio_policy_set_source_prepared(app_audio_source_t source, bool prepared)
{
    ESP_RETURN_ON_FALSE(app_audio_policy_source_valid(source),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid audio source");
    ESP_RETURN_ON_FALSE(s_policy_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "audio policy is not initialized");

    xSemaphoreTake(s_policy_lock, portMAX_DELAY);

    const uint32_t previous_sources = s_prepared_sources;
    if (prepared) {
        s_prepared_sources |= (uint32_t)source;
    } else {
        s_prepared_sources &= ~(uint32_t)source;
    }

    esp_err_t ret = app_audio_policy_apply_locked();
    if (previous_sources != s_prepared_sources) {
        APP_LOG_DETAIL(TAG,
                       "audio source %s %s: prepared=0x%02lx active=0x%02lx adaptive=%d aec=%d",
                       app_audio_policy_source_name(source),
                       prepared ? "prepared" : "released",
                       (unsigned long)s_prepared_sources,
                       (unsigned long)s_active_sources,
                       s_adaptive_playout_enabled ? 1 : 0,
                       s_aec_runtime_active ? 1 : 0);
    }

    xSemaphoreGive(s_policy_lock);
    return ret;
}

esp_err_t app_audio_policy_set_source_active(app_audio_source_t source, bool active)
{
    ESP_RETURN_ON_FALSE(app_audio_policy_source_valid(source),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid audio source");
    ESP_RETURN_ON_FALSE(s_policy_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "audio policy is not initialized");

    xSemaphoreTake(s_policy_lock, portMAX_DELAY);

    const uint32_t previous_sources = s_active_sources;
    if (active) {
        s_active_sources |= (uint32_t)source;
    } else {
        s_active_sources &= ~(uint32_t)source;
    }

    esp_err_t ret = app_audio_policy_apply_locked();
    if (previous_sources != s_active_sources) {
        APP_LOG_DETAIL(TAG,
                       "audio source %s %s: prepared=0x%02lx active=0x%02lx adaptive=%d aec=%d",
                       app_audio_policy_source_name(source),
                       active ? "active" : "idle",
                       (unsigned long)s_prepared_sources,
                       (unsigned long)s_active_sources,
                       s_adaptive_playout_enabled ? 1 : 0,
                       s_aec_runtime_active ? 1 : 0);
    }

    xSemaphoreGive(s_policy_lock);
    return ret;
}
