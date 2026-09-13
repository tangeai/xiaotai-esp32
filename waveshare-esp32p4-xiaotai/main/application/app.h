#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "device.h"
#include "device_binding.h"
#include "device_online.h"
#include "network.h"
#include "ota.h"
#include "scan_preview.h"

#define APP_SSID_MAX_LEN         33
#define APP_IP_ADDR_MAX_LEN      16
#define APP_WIFI_SCAN_MAX        NETWORK_SCAN_RESULT_MAX
#define APP_TEST_STATUS_MAX      96
#define APP_AI_CHAT_CAPTION_MAX  256
#define APP_AI_CHAT_MESSAGE_MAX  100
#define APP_RTC_CONFIG_TEXT_MAX  128
#define APP_RTC_CONFIG_TOKEN_SUBJECT_MAX 64
#define APP_CALL_CONTACT_MAX     8
#define APP_CALL_CONTACT_DEVICE_ID_LENGTH 12
#define APP_CALL_CONTACT_DEVICE_ID_MAX 64
#define APP_CALL_CONTACT_REMARK_MAX 64
#define APP_CALL_CONTACT_CREATED_AT_MAX 48
#define APP_WECHAT_CONTACT_MAX   4
#define APP_WECHAT_OPEN_ID_MAX   96
#define APP_WECHAT_REMARK_MAX_CHARS 64
#define APP_WECHAT_REMARK_MAX    ((APP_WECHAT_REMARK_MAX_CHARS * 4) + 1)

typedef enum {
	APP_RTC_CONFIG_FIELD_DEVICE_ID = 0,
	APP_RTC_CONFIG_FIELD_DEVICE_SECRET,
	APP_RTC_CONFIG_FIELD_TOKEN_SUBJECT,
	APP_RTC_CONFIG_FIELD_ACCESS_KEY_ID,
	APP_RTC_CONFIG_FIELD_ACCESS_KEY_SECRET,
} app_rtc_config_field_t;

typedef enum {
	APP_RTC_SERVER_ENV_TEST = 0,
	APP_RTC_SERVER_ENV_PRE,
	APP_RTC_SERVER_ENV_PROD,
} app_rtc_server_env_t;

typedef enum {
	APP_ID_HOME = 0,
	APP_ID_DEVICE,
	APP_ID_CALL,
	APP_ID_WECHAT,
	APP_ID_AI_CHAT,
	APP_ID_SYSTEM,
} app_id_t;

typedef scan_preview_cb_t app_scan_preview_cb_t;
typedef void (*app_contact_scan_result_cb_t)(esp_err_t result,
					     const char *device_id,
					     const char *raw_payload,
					     void *ctx);
typedef void (*app_tirtc_config_scan_result_cb_t)(esp_err_t result,
						  const char *device_id,
						  const char *device_secret,
						  const char *raw_payload,
						  void *ctx);
typedef void (*app_wechat_contact_scan_result_cb_t)(esp_err_t result,
						    const char *open_id,
						    const char *raw_payload,
						    void *ctx);

typedef struct {
	char ssid[APP_SSID_MAX_LEN];
	int8_t rssi;
	bool secure;
	uint8_t channel;
} app_wifi_scan_result_t;

typedef struct {
	bool connected;
	int8_t rssi;
	char ip_addr[APP_IP_ADDR_MAX_LEN];
	char ssid[APP_SSID_MAX_LEN];
	char saved_ssid[APP_SSID_MAX_LEN];
	char saved_password[NETWORK_PASSWORD_MAX_LEN + 1];
	bool connect_failed;
	bool scan_in_progress;
	uint16_t scan_count;
	app_wifi_scan_result_t scan_results[APP_WIFI_SCAN_MAX];
	bool ping_running;
	bool ping_valid;
	uint32_t ping_transmitted;
	uint32_t ping_received;
	uint32_t ping_latency_avg_ms;
	uint32_t ping_jitter_ms;
	uint32_t ping_loss_percent;
} app_network_snapshot_t;

typedef struct {
	char uuid[DEVICE_UUID_MAX_LEN];
	uint8_t cpu_usage_percent;
	bool door_open;
} app_device_snapshot_t;

typedef struct {
	bool connected;
	bool call_active;
	bool incoming_call_pending;
	bool local_audio_send_enabled;
	uint8_t state;
	uint32_t tx_video_frames;
	uint32_t rx_video_frames;
	uint32_t tx_audio_frames;
	uint32_t rx_audio_frames;
	uint16_t tx_video_fps;
	uint16_t rx_video_fps;
	uint16_t tx_audio_fps;
	uint16_t rx_audio_fps;
	uint16_t tx_video_width;
	uint16_t tx_video_height;
	uint8_t tx_video_target_fps;
	uint32_t tx_video_configured_bitrate_kbps;
	uint32_t tx_video_measured_fps_x10;
	uint32_t tx_video_measured_bitrate_kbps;
	uint32_t tx_video_transport_bitrate_kbps;
	uint32_t rx_video_transport_bitrate_kbps;
} app_rtc_snapshot_t;

typedef enum {
	APP_RTC_VIDEO_ADAPT_OFF = 0,
	APP_RTC_VIDEO_ADAPT_FRAMERATE_PRIORITY,
	APP_RTC_VIDEO_ADAPT_RESOLUTION_PRIORITY,
} app_rtc_video_adaptation_mode_t;

typedef struct {
	uint16_t width;
	uint16_t height;
	uint8_t fps;
	uint32_t bitrate_bps;
	app_rtc_video_adaptation_mode_t adaptation_mode;
	uint8_t adaptation_level;
} app_rtc_video_config_t;

typedef struct {
	char device_id[APP_RTC_CONFIG_TEXT_MAX];
	char device_secret[APP_RTC_CONFIG_TEXT_MAX];
	char token_subject[APP_RTC_CONFIG_TOKEN_SUBJECT_MAX];
	char access_key_id[APP_RTC_CONFIG_TEXT_MAX];
	char access_key_secret[APP_RTC_CONFIG_TEXT_MAX];
	char access_url[APP_RTC_CONFIG_TEXT_MAX];
	char server_api[APP_RTC_CONFIG_TEXT_MAX];
	app_rtc_server_env_t server_env;
} app_rtc_config_snapshot_t;

typedef struct {
	bool ready;
	bool speaker_enabled;
	uint32_t input_level;
	uint32_t output_level;
	uint8_t speaker_volume_percent;
	uint8_t capture_gain_percent;
} app_audio_snapshot_t;

typedef struct {
	bool sender_running;
	bool sender_spiffs_ready;
	char sender_status[APP_TEST_STATUS_MAX];
} app_test_snapshot_t;

typedef struct {
	ota_state_t state;
	bool running;
	uint8_t progress_percent;
	size_t bytes_read;
	size_t total_size;
	esp_err_t last_error;
	char current_version[OTA_VERSION_MAX];
	char target_version[OTA_VERSION_MAX];
	char url[OTA_URL_MAX];
	char message[OTA_MESSAGE_MAX];
} app_ota_snapshot_t;

typedef struct {
	bool video_enabled;
	bool audio_enabled;
	bool effective_video_enabled;
	bool effective_audio_enabled;
} app_control_snapshot_t;

typedef struct {
	uint8_t caption_type;
	int64_t utterance_id;
	char text[APP_AI_CHAT_CAPTION_MAX];
} app_ai_chat_message_t;

typedef struct {
	uint8_t state;
	bool active;
	bool listening;
	bool cloud_speaking;
	bool video_active;
	uint32_t tx_audio_frames;
	uint32_t tx_video_frames;
	uint32_t tx_video_failures;
	uint32_t rx_commands;
	char asr_caption[APP_AI_CHAT_CAPTION_MAX];
	char tts_caption[APP_AI_CHAT_CAPTION_MAX];
	uint8_t message_count;
	app_ai_chat_message_t messages[APP_AI_CHAT_MESSAGE_MAX];
	char status[APP_TEST_STATUS_MAX];
	int last_error;
	uint8_t avatar;
} app_ai_chat_snapshot_t;

typedef struct {
	char device_id[APP_CALL_CONTACT_DEVICE_ID_MAX];
	char remark[APP_CALL_CONTACT_REMARK_MAX];
	bool online;
	bool deletable;
} app_call_contact_t;

typedef struct {
	char device_id[APP_CALL_CONTACT_DEVICE_ID_MAX];
	char created_at[APP_CALL_CONTACT_CREATED_AT_MAX];
} app_call_pending_contact_t;

typedef struct {
	bool ready;
	bool refreshing;
	uint8_t count;
	uint8_t pending_count;
	esp_err_t last_error;
	app_call_contact_t contacts[APP_CALL_CONTACT_MAX];
	app_call_pending_contact_t pending[APP_CALL_CONTACT_MAX];
} app_call_contacts_snapshot_t;

typedef enum {
	APP_CALL_TYPE_AUDIO = 0,
	APP_CALL_TYPE_VIDEO,
} app_call_type_t;

typedef enum {
	APP_CALL_STATE_IDLE = 0,
	APP_CALL_STATE_OUTGOING,
	APP_CALL_STATE_INCOMING,
	APP_CALL_STATE_CONNECTING,
	APP_CALL_STATE_IN_CALL,
	APP_CALL_STATE_ERROR,
} app_call_state_t;

typedef struct {
	app_call_state_t state;
	app_call_type_t type;
	bool pending_incoming;
	char peer_device_id[APP_CALL_CONTACT_DEVICE_ID_MAX];
	char room_id[96];
	char message[96];
	esp_err_t last_error;
} app_call_snapshot_t;

typedef struct {
	char open_id[APP_WECHAT_OPEN_ID_MAX];
	char remark[APP_WECHAT_REMARK_MAX];
} app_wechat_contact_t;

typedef enum {
	APP_WECHAT_CALL_STATE_IDLE = 0,
	APP_WECHAT_CALL_STATE_INCOMING,
	APP_WECHAT_CALL_STATE_CALLING,
	APP_WECHAT_CALL_STATE_CONNECTING,
	APP_WECHAT_CALL_STATE_IN_CALL,
	APP_WECHAT_CALL_STATE_CLOSING,
} app_wechat_call_state_t;

typedef struct {
	uint8_t count;
	bool contacts_ready;
	bool contacts_server_synced;
	esp_err_t contacts_last_error;
	bool incoming_call_pending;
	app_wechat_call_state_t call_state;
	app_wechat_contact_t contacts[APP_WECHAT_CONTACT_MAX];
} app_wechat_snapshot_t;

typedef struct {
	device_binding_state_t state;
	bool running;
	char mac[16];
	char code[DEVICE_BINDING_CODE_MAX_LEN];
	char device_id[DEVICE_BINDING_DEVICE_ID_MAX_LEN];
	esp_err_t last_error;
	char message[DEVICE_BINDING_MESSAGE_MAX_LEN];
} app_device_binding_snapshot_t;

typedef struct {
	device_online_state_t state;
	bool running;
	bool network_ready;
	bool bound;
	bool mqtt_connected;
	char device_id[DEVICE_ONLINE_DEVICE_ID_MAX_LEN];
	esp_err_t last_error;
	char message[DEVICE_ONLINE_MESSAGE_MAX_LEN];
} app_device_online_snapshot_t;

typedef struct {
	app_network_snapshot_t network;
	app_device_snapshot_t device;
	app_rtc_snapshot_t rtc;
	app_rtc_config_snapshot_t rtc_config;
	app_device_binding_snapshot_t binding;
	app_device_online_snapshot_t online;
	app_audio_snapshot_t audio;
	app_test_snapshot_t test;
	app_ota_snapshot_t ota;
	app_control_snapshot_t controls;
	app_ai_chat_snapshot_t ai_chat;
	app_call_snapshot_t call;
	app_call_contacts_snapshot_t call_contacts;
	app_wechat_snapshot_t wechat;
} app_snapshot_t;

esp_err_t app_init(void);
void app_run(void);
esp_err_t app_enter_app(app_id_t app_id);
esp_err_t app_return_home(void);
esp_err_t app_request_enter_app(app_id_t app_id);
esp_err_t app_request_return_home(void);
esp_err_t app_connect_wifi(const char *ssid, const char *password);
esp_err_t app_request_wifi_scan(void);
esp_err_t app_update_device_uuid(const char *uuid);
esp_err_t app_start_ping_test(void);

esp_err_t app_disconnect_rtc(void);
esp_err_t app_update_rtc_config_field(app_rtc_config_field_t field, const char *value);
esp_err_t app_update_rtc_device_credentials(const char *device_id, const char *device_secret);
esp_err_t app_request_update_rtc_device_credentials(const char *device_id, const char *device_secret);
esp_err_t app_start_device_binding(void);
esp_err_t app_reset_device_binding(void);
esp_err_t app_set_rtc_server_env(app_rtc_server_env_t env);

esp_err_t app_call_contact(const char *device_id, app_call_type_t call_type);
esp_err_t app_add_call_contact(const char *device_id);
esp_err_t app_respond_call_contact(const char *device_id, bool accept);
esp_err_t app_update_call_contact_remark(const char *device_id, const char *remark);
esp_err_t app_delete_call_contact(const char *device_id);
esp_err_t app_refresh_call_contacts(void);
void app_get_call_contacts(app_call_contacts_snapshot_t *snapshot);
esp_err_t app_scan_contact(void);
esp_err_t app_start_contact_scan(app_scan_preview_cb_t preview_cb,
				 app_contact_scan_result_cb_t result_cb,
				 void *ctx);
esp_err_t app_stop_contact_scan(void);
esp_err_t app_start_tirtc_config_scan(app_scan_preview_cb_t preview_cb,
				      app_tirtc_config_scan_result_cb_t result_cb,
				      void *ctx);
esp_err_t app_stop_tirtc_config_scan(void);
esp_err_t app_hangup_call(void);
esp_err_t app_hangup_call_async(void);
esp_err_t app_accept_call(void);
esp_err_t app_request_accept_call(void);
esp_err_t app_reject_call(void);

esp_err_t app_wechat_call_contact(const char *open_id);
esp_err_t app_wechat_call_contact_with_type(const char *open_id,
                                            app_call_type_t call_type);
esp_err_t app_wechat_add_contact(const char *open_id);
esp_err_t app_wechat_remove_contact(const char *open_id);
esp_err_t app_wechat_update_contact_remark(const char *open_id,
					   const char *remark);
esp_err_t app_scan_wechat_contact(void);
esp_err_t app_start_wechat_contact_scan(app_scan_preview_cb_t preview_cb,
					app_wechat_contact_scan_result_cb_t result_cb,
					void *ctx);
esp_err_t app_stop_wechat_contact_scan(void);
esp_err_t app_wechat_hangup_call(void);
esp_err_t app_wechat_accept_call(void);
esp_err_t app_wechat_reject_call(void);

esp_err_t app_start_ota(void);
void app_restart_for_ota(void);

esp_err_t app_open_ai_chat(void);
esp_err_t app_request_start_ai_chat(void);
esp_err_t app_close_ai_chat(void);
esp_err_t app_clear_ai_chat_messages(void);
esp_err_t app_handle_ai_chat_button(bool pressed);
esp_err_t app_set_ai_chat_avatar(uint8_t avatar);

esp_err_t app_set_speaker_volume(uint8_t percent);
esp_err_t app_set_capture_gain(uint8_t percent);
esp_err_t app_request_speaker_volume(uint8_t percent);
esp_err_t app_request_capture_gain(uint8_t percent);
esp_err_t app_set_local_video_enabled(bool enabled);
esp_err_t app_set_local_audio_enabled(bool enabled);
esp_err_t app_set_rtc_video_config(const app_rtc_video_config_t *config);
esp_err_t app_apply_rtc_weak_network_level(app_rtc_video_adaptation_mode_t mode, uint8_t level);
void app_get_rtc_video_config(app_rtc_video_config_t *config);

void app_on_boot_button_changed(bool pressed, void *ctx);
void app_get_snapshot(app_snapshot_t *snapshot);
esp_err_t app_apply_media_policy(void);
