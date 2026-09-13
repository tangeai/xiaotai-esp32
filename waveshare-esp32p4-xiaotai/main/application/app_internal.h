#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app.h"
#include "rtc_transport.h"

typedef struct {
	bool video_enabled;
	bool audio_enabled;
} app_control_state_t;

app_control_state_t app_state_get_control(void);
app_id_t app_get_active_app(void);
bool app_is_door_open(void);
esp_err_t app_configure_tirtc(void);
esp_err_t app_acquire_call_session_resources(bool video);
void app_release_call_session_resources(void);
bool app_call_start_is_in_progress(void);
esp_err_t app_suspend_call_scan_resources(void);
esp_err_t app_resume_call_scan_resources(void);
void app_cancel_contact_scan_for_lifecycle(void);
esp_err_t app_cancel_pending_call_start_for_lifecycle(void);
void app_call_start_apply_snapshot(app_call_snapshot_t *snapshot);
esp_err_t app_suspend_wechat_scan_resources(void);
esp_err_t app_resume_wechat_scan_resources(void);
void app_cancel_wechat_contact_scan_for_lifecycle(void);
esp_err_t app_acquire_tirtc_config_scan_resources(void);
void app_release_tirtc_config_scan_resources(void);
void app_cancel_tirtc_config_scan_for_lifecycle(void);
bool app_state_is_call_active(void);
void app_state_set_video_enabled(bool enabled);
void app_state_set_audio_enabled(bool enabled);
void app_state_prepare_call_media(bool video, bool audio);
void app_state_reset_call_media_policy(void);
bool app_state_sync_call_media_defaults(bool call_active, app_control_state_t *control);
void app_reset_rtc_call_media_state(void);
void app_state_fill_rtc_frame_rates(app_rtc_snapshot_t *snapshot, const rtc_transport_stats_t *rtc);

void app_snapshot_get(app_snapshot_t *snapshot);
