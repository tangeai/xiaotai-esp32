#pragma once

#include "esp_err.h"
#include "rtc_transport.h"
#include "app.h"

esp_err_t app_build_rtc_transport_config(rtc_transport_config_t *config);
void app_get_rtc_config_snapshot(app_rtc_config_snapshot_t *snapshot);
esp_err_t app_set_rtc_device_credentials(const char *device_id, const char *device_secret);
esp_err_t app_clear_rtc_device_credentials(void);
esp_err_t app_set_rtc_config_field(app_rtc_config_field_t field, const char *value);
esp_err_t app_set_rtc_config_server_env(app_rtc_server_env_t env);
