#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "tirtc_session.h"

typedef tirtc_session_config_t rtc_transport_config_t;
typedef tirtc_session_network_state_t rtc_transport_network_state_t;
typedef tirtc_session_state_t rtc_transport_state_t;
typedef tirtc_session_stats_t rtc_transport_stats_t;
typedef tirtc_session_peer_state_t rtc_transport_peer_state_t;
typedef tirtc_session_media_ops_t rtc_transport_media_ops_t;
typedef tirtc_session_hooks_t rtc_transport_hooks_t;
typedef tirtc_session_control_ops_t rtc_transport_control_ops_t;
typedef tirtc_session_observer_t rtc_transport_observer_t;
typedef tirtc_session_video_bitrate_params_t rtc_transport_video_bitrate_params_t;

#define RTC_TRANSPORT_MODE_LISTEN     TIRTC_SESSION_MODE_LISTEN
#define RTC_TRANSPORT_MODE_CONNECT    TIRTC_SESSION_MODE_CONNECT
#define RTC_TRANSPORT_STATE_STOPPED   TIRTC_SESSION_STATE_STOPPED
#define RTC_TRANSPORT_STATE_STARTING  TIRTC_SESSION_STATE_STARTING
#define RTC_TRANSPORT_STATE_READY     TIRTC_SESSION_STATE_READY
#define RTC_TRANSPORT_STATE_CONNECTED TIRTC_SESSION_STATE_CONNECTED
#define RTC_TRANSPORT_STATE_MEDIA_BOOTSTRAPPING TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING
#define RTC_TRANSPORT_STATE_DISCONNECTING TIRTC_SESSION_STATE_DISCONNECTING
#define RTC_TRANSPORT_STATE_ERROR     TIRTC_SESSION_STATE_ERROR
#define RTC_TRANSPORT_LOCAL_VIDEO_STREAM_ID TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID

esp_err_t rtc_transport_init(const rtc_transport_config_t *config);
esp_err_t rtc_transport_prewarm_media_pools(void);
esp_err_t rtc_transport_configure(const rtc_transport_config_t *config);
esp_err_t rtc_transport_prepare_sdk(void);
esp_err_t rtc_transport_set_media_bridge(const rtc_transport_media_ops_t *ops, void *ctx);
void rtc_transport_set_hooks(const rtc_transport_hooks_t *hooks, void *ctx);
void rtc_transport_set_control_ops(const rtc_transport_control_ops_t *ops, void *ctx);
esp_err_t rtc_transport_register_observer(const rtc_transport_observer_t *observer, void *ctx);
esp_err_t rtc_transport_set_video_bitrate_params(
    const rtc_transport_video_bitrate_params_t *params);
esp_err_t rtc_transport_start_if_ready(void);
esp_err_t rtc_transport_connect_peer(const char *remote_device_id, const char *remote_device_secret_key);
esp_err_t rtc_transport_connect_peer_with_token(const char *remote_device_id, const char *connect_token);
esp_err_t rtc_transport_send_command(uint32_t cmdw, const void *data, size_t data_len);
void rtc_transport_set_next_connection_auto_media(bool enabled);
void rtc_transport_set_next_connection_defer_media(bool enabled);
esp_err_t rtc_transport_activate_deferred_media(bool enable_video, bool enable_audio);
esp_err_t rtc_transport_restart(void);
esp_err_t rtc_transport_stop(void);
esp_err_t rtc_transport_disconnect(void);
void rtc_transport_flush_remote_media(void);
esp_err_t rtc_transport_accept_incoming_call(void);
esp_err_t rtc_transport_reject_incoming_call(void);
esp_err_t rtc_transport_hangup(void);
void rtc_transport_on_network_state_changed(const rtc_transport_network_state_t *state);
esp_err_t rtc_transport_set_local_video_send_enabled(bool enabled);
esp_err_t rtc_transport_set_local_audio_send_enabled(bool enabled);
esp_err_t rtc_transport_query_peer_state(void);
bool rtc_transport_get_last_peer_state(rtc_transport_peer_state_t *state);
rtc_transport_state_t rtc_transport_get_state(void);
void rtc_transport_get_config(rtc_transport_config_t *config);
void rtc_transport_get_stats(rtc_transport_stats_t *stats);
void rtc_transport_refresh_media_policy(void);
