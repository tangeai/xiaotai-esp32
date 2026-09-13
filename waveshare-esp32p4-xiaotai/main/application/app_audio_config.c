#include "app_audio_config.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "platform_nvs_async.h"
#include "platform_storage.h"

static const char *TAG = "app_audio_config";

#define APP_AUDIO_NVS_NAMESPACE        "audio_cfg"
#define APP_AUDIO_NVS_KEY_SPEAKER_VOL  "spk_vol"
#define APP_AUDIO_NVS_KEY_CAPTURE_GAIN "cap_gain"
#define APP_AUDIO_DEFAULT_SPEAKER_PERCENT      70
#define APP_AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT 80

static uint8_t app_audio_clamp_percent(uint8_t percent)
{
    return percent > 100U ? 100U : percent;
}

static void app_audio_config_fill_defaults(app_audio_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->speaker_volume_percent = APP_AUDIO_DEFAULT_SPEAKER_PERCENT;
    config->capture_gain_percent = APP_AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT;
}

static esp_err_t app_audio_config_load_u8(nvs_handle_t nvs_handle, const char *key, uint8_t *value)
{
    uint8_t saved_value = 0;
    esp_err_t ret = nvs_get_u8(nvs_handle, key, &saved_value);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    *value = app_audio_clamp_percent(saved_value);
    return ESP_OK;
}

static esp_err_t app_audio_config_save_u8(const char *key, uint8_t value)
{
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio config key");
    return platform_nvs_async_set_u8(APP_AUDIO_NVS_NAMESPACE,
                                     key,
                                     app_audio_clamp_percent(value));
}

esp_err_t app_audio_config_load(app_audio_config_t *config)
{
    nvs_handle_t nvs_handle = 0;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio config");
    app_audio_config_fill_defaults(config);

    ESP_RETURN_ON_ERROR(platform_storage_init(), TAG, "nvs init failed");

    esp_err_t ret = nvs_open(APP_AUDIO_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = app_audio_config_load_u8(nvs_handle,
                                   APP_AUDIO_NVS_KEY_SPEAKER_VOL,
                                   &config->speaker_volume_percent);
    if (ret == ESP_OK) {
        ret = app_audio_config_load_u8(nvs_handle,
                                       APP_AUDIO_NVS_KEY_CAPTURE_GAIN,
                                       &config->capture_gain_percent);
    }
    nvs_close(nvs_handle);
    if (ret == ESP_OK && config->speaker_volume_percent == 0U) {
        ESP_LOGI(TAG,
                 "stored speaker volume is 0, restore default %u for audible remote playback",
                 APP_AUDIO_DEFAULT_SPEAKER_PERCENT);
        config->speaker_volume_percent = APP_AUDIO_DEFAULT_SPEAKER_PERCENT;
    }
    return ret;
}

esp_err_t app_audio_config_save_speaker_volume(uint8_t percent)
{
    return app_audio_config_save_u8(APP_AUDIO_NVS_KEY_SPEAKER_VOL, percent);
}

esp_err_t app_audio_config_save_capture_gain(uint8_t percent)
{
    return app_audio_config_save_u8(APP_AUDIO_NVS_KEY_CAPTURE_GAIN, percent);
}
