#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "scan_preview.h"
#include "wifi.h"

#define DISPLAY_WIFI_SCAN_MAX       10
#define DISPLAY_DEVICE_UUID_MAX_LEN 37
#define DISPLAY_TEST_STATUS_MAX     96
#define DISPLAY_AI_CHAT_CAPTION_MAX 256
#define DISPLAY_AI_CHAT_MESSAGE_MAX 100
#define DISPLAY_OTA_URL_MAX         256
#define DISPLAY_OTA_MESSAGE_MAX     96
#define DISPLAY_OTA_VERSION_MAX     32
#define DISPLAY_TIRTC_CONFIG_TEXT_MAX 128
#define DISPLAY_TIRTC_CONFIG_TOKEN_SUBJECT_MAX 64
#define DISPLAY_BINDING_CODE_MAX_LEN 8
#define DISPLAY_BINDING_MESSAGE_MAX_LEN 96
#define DISPLAY_CALL_CONTACT_MAX   8
#define DISPLAY_CALL_CONTACT_DEVICE_ID_LENGTH 12
#define DISPLAY_CALL_CONTACT_DEVICE_ID_MAX 64
#define DISPLAY_CALL_CONTACT_REMARK_MAX 64
#define DISPLAY_CALL_CONTACT_CREATED_AT_MAX 48
#define DISPLAY_CALL_ROOM_ID_MAX 96
#define DISPLAY_CALL_MESSAGE_MAX 96
#define DISPLAY_WECHAT_CONTACT_MAX  4
#define DISPLAY_WECHAT_OPEN_ID_MAX  96
#define DISPLAY_WECHAT_REMARK_MAX_CHARS 64
#define DISPLAY_WECHAT_REMARK_MAX ((DISPLAY_WECHAT_REMARK_MAX_CHARS * 4) + 1)

typedef enum {
    DISPLAY_OTA_STATE_IDLE = 0,
    DISPLAY_OTA_STATE_CHECKING,
    DISPLAY_OTA_STATE_DOWNLOADING,
    DISPLAY_OTA_STATE_VERIFYING,
    DISPLAY_OTA_STATE_READY_TO_REBOOT,
    DISPLAY_OTA_STATE_FAILED,
} display_ota_state_t;

typedef enum {
    DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID = 0,
    DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET,
    DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT,
    DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID,
    DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET,
    DISPLAY_TIRTC_CONFIG_FIELD_COUNT,
} display_tirtc_config_field_t;

typedef enum {
    DISPLAY_TIRTC_SERVER_ENV_TEST = 0,
    DISPLAY_TIRTC_SERVER_ENV_PRE,
    DISPLAY_TIRTC_SERVER_ENV_PROD,
} display_tirtc_server_env_t;

typedef enum {
    DISPLAY_WECHAT_CALL_STATE_IDLE = 0,
    DISPLAY_WECHAT_CALL_STATE_INCOMING,
    DISPLAY_WECHAT_CALL_STATE_CALLING,
    DISPLAY_WECHAT_CALL_STATE_CONNECTING,
    DISPLAY_WECHAT_CALL_STATE_IN_CALL,
    DISPLAY_WECHAT_CALL_STATE_CLOSING,
} display_wechat_call_state_t;

typedef enum {
    DISPLAY_CALL_TYPE_AUDIO = 0,
    DISPLAY_CALL_TYPE_VIDEO,
} display_call_type_t;

typedef enum {
    DISPLAY_CALL_STATE_IDLE = 0,
    DISPLAY_CALL_STATE_OUTGOING,
    DISPLAY_CALL_STATE_INCOMING,
    DISPLAY_CALL_STATE_CONNECTING,
    DISPLAY_CALL_STATE_IN_CALL,
    DISPLAY_CALL_STATE_ERROR,
} display_call_state_t;

typedef enum {
    DISPLAY_DEVICE_BINDING_STATE_DISABLED = 0,
    DISPLAY_DEVICE_BINDING_STATE_IDLE,
    DISPLAY_DEVICE_BINDING_STATE_REPORTING,
    DISPLAY_DEVICE_BINDING_STATE_WAITING_USER,
    DISPLAY_DEVICE_BINDING_STATE_BOUND,
    DISPLAY_DEVICE_BINDING_STATE_ERROR,
} display_device_binding_state_t;

typedef enum {
    DISPLAY_APP_HOME = 0,
    DISPLAY_APP_DEVICE,
    DISPLAY_APP_CALL,
    DISPLAY_APP_WECHAT,
    DISPLAY_APP_AI_CHAT,
    DISPLAY_APP_SYSTEM,
} display_app_id_t;

typedef enum {
    DISPLAY_MEMORY_HEALTH_NORMAL = 0,
    DISPLAY_MEMORY_HEALTH_WARNING,
    DISPLAY_MEMORY_HEALTH_CRITICAL,
} display_memory_health_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
    uint8_t channel;
} display_wifi_scan_result_t;

typedef struct {
    char open_id[DISPLAY_WECHAT_OPEN_ID_MAX];
    char remark[DISPLAY_WECHAT_REMARK_MAX];
} display_wechat_contact_t;

typedef struct {
    char device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
    char remark[DISPLAY_CALL_CONTACT_REMARK_MAX];
    bool online;
    bool deletable;
} display_call_contact_t;

typedef struct {
    char device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
    char created_at[DISPLAY_CALL_CONTACT_CREATED_AT_MAX];
} display_call_pending_contact_t;

typedef struct {
    uint8_t caption_type;
    int64_t utterance_id;
    char text[DISPLAY_AI_CHAT_CAPTION_MAX];
} display_ai_chat_message_t;

typedef struct {
    bool network_connected;
    int8_t network_rssi;
    char network_ip_addr[16];
    char network_ssid[33];
    char saved_network_ssid[33];
    char saved_network_password[WIFI_PASSWORD_MAX_LEN + 1];
    bool network_connect_failed;
    bool wifi_scan_in_progress;
    uint16_t wifi_scan_count;
    display_wifi_scan_result_t wifi_scan_results[DISPLAY_WIFI_SCAN_MAX];
    bool ping_running;
    bool ping_valid;
    uint32_t ping_transmitted;
    uint32_t ping_received;
    uint32_t ping_latency_avg_ms;
    uint32_t ping_jitter_ms;
    uint32_t ping_loss_percent;
    bool test_running;
    char test_status[DISPLAY_TEST_STATUS_MAX];
    display_ota_state_t ota_state;
    bool ota_running;
    uint8_t ota_progress_percent;
    size_t ota_bytes_read;
    size_t ota_total_size;
    int ota_last_error;
    char ota_current_version[DISPLAY_OTA_VERSION_MAX];
    char ota_target_version[DISPLAY_OTA_VERSION_MAX];
    char ota_url[DISPLAY_OTA_URL_MAX];
    char ota_message[DISPLAY_OTA_MESSAGE_MAX];

    char device_uuid[DISPLAY_DEVICE_UUID_MAX_LEN];
    uint8_t cpu_usage_percent;
    bool device_door_open;
    size_t memory_internal_free;
    size_t memory_internal_largest;
    size_t memory_dma_largest;
    size_t memory_psram_largest;
    display_memory_health_t memory_health;

    bool rtc_connected;
    bool rtc_call_active;
    bool rtc_incoming_call_pending;
    bool rtc_video_enabled;
    bool rtc_audio_enabled;
    bool rtc_local_audio_send_enabled;
    uint8_t rtc_state;
    uint8_t rtc_tx_fail_percent;
    uint32_t rtc_tx_video_frames;
    uint32_t rtc_rx_video_frames;
    uint32_t rtc_tx_audio_frames;
    uint32_t rtc_rx_audio_frames;
    uint16_t rtc_tx_video_fps;
    uint16_t rtc_rx_video_fps;
    uint16_t rtc_tx_audio_fps;
    uint16_t rtc_rx_audio_fps;
    uint16_t rtc_tx_video_width;
    uint16_t rtc_tx_video_height;
    uint8_t rtc_tx_video_target_fps;
    uint32_t rtc_tx_video_configured_bitrate_kbps;
    uint32_t rtc_tx_video_measured_fps_x10;
    uint32_t rtc_tx_video_measured_bitrate_kbps;
    uint32_t rtc_tx_video_transport_bitrate_kbps;
    uint32_t rtc_rx_video_transport_bitrate_kbps;
    char tirtc_device_id[DISPLAY_TIRTC_CONFIG_TEXT_MAX];
    char tirtc_device_secret[DISPLAY_TIRTC_CONFIG_TEXT_MAX];
    char tirtc_token_subject[DISPLAY_TIRTC_CONFIG_TOKEN_SUBJECT_MAX];
    char tirtc_access_key_id[DISPLAY_TIRTC_CONFIG_TEXT_MAX];
    char tirtc_access_key_secret[DISPLAY_TIRTC_CONFIG_TEXT_MAX];
    char tirtc_access_url[DISPLAY_TIRTC_CONFIG_TEXT_MAX];
    char tirtc_server_api[DISPLAY_TIRTC_CONFIG_TEXT_MAX];
    display_tirtc_server_env_t tirtc_server_env;
    display_device_binding_state_t binding_state;
    bool binding_running;
    char binding_code[DISPLAY_BINDING_CODE_MAX_LEN];
    char binding_message[DISPLAY_BINDING_MESSAGE_MAX_LEN];
    display_call_state_t call_state;
    display_call_type_t call_type;
    bool call_contacts_ready;
    bool call_contacts_refreshing;
    int call_last_error;
    int call_contacts_last_error;
    char call_peer_device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
    char call_room_id[DISPLAY_CALL_ROOM_ID_MAX];
    char call_message[DISPLAY_CALL_MESSAGE_MAX];
    uint8_t call_contact_count;
    display_call_contact_t call_contacts[DISPLAY_CALL_CONTACT_MAX];
    uint8_t call_pending_contact_count;
    display_call_pending_contact_t call_pending_contacts[DISPLAY_CALL_CONTACT_MAX];
    uint8_t wechat_contact_count;
    bool wechat_contacts_ready;
    bool wechat_contacts_server_synced;
    int wechat_contacts_last_error;
    bool wechat_incoming_call_pending;
    display_wechat_call_state_t wechat_call_state;
    display_wechat_contact_t wechat_contacts[DISPLAY_WECHAT_CONTACT_MAX];

    bool audio_ready;
    bool audio_speaker_enabled;
    uint32_t audio_input_level;
    uint32_t audio_output_level;
    uint8_t audio_speaker_volume_percent;
    uint8_t audio_capture_gain_percent;

    uint8_t ai_chat_state;
    bool ai_chat_active;
    bool ai_chat_listening;
    bool ai_chat_cloud_speaking;
    bool ai_chat_video_active;
    uint32_t ai_chat_tx_audio_frames;
    uint32_t ai_chat_tx_video_frames;
    uint32_t ai_chat_tx_video_failures;
    uint32_t ai_chat_rx_commands;
    int ai_chat_last_error;
    char ai_chat_asr_caption[DISPLAY_AI_CHAT_CAPTION_MAX];
    char ai_chat_tts_caption[DISPLAY_AI_CHAT_CAPTION_MAX];
    uint8_t ai_chat_message_count;
    display_ai_chat_message_t ai_chat_messages[DISPLAY_AI_CHAT_MESSAGE_MAX];
    char ai_chat_status[DISPLAY_TEST_STATUS_MAX];
    uint8_t ai_chat_avatar;
} display_status_t;

typedef void (*display_snapshot_cb_t)(display_status_t *status, void *ctx);

typedef esp_err_t (*display_wifi_connect_cb_t)(const char *ssid, const char *password, void *ctx);
typedef esp_err_t (*display_uuid_update_cb_t)(const char *uuid, void *ctx);
typedef esp_err_t (*display_simple_action_cb_t)(void *ctx);
typedef esp_err_t (*display_toggle_action_cb_t)(bool enabled, void *ctx);
typedef esp_err_t (*display_percent_cb_t)(uint8_t percent, void *ctx);
typedef esp_err_t (*display_ai_avatar_cb_t)(uint8_t avatar, void *ctx);
typedef esp_err_t (*display_call_action_cb_t)(void *ctx);
typedef esp_err_t (*display_call_contact_cb_t)(const char *device_id,
                                               display_call_type_t call_type,
                                               void *ctx);
typedef esp_err_t (*display_call_contact_add_cb_t)(const char *device_id, void *ctx);
typedef esp_err_t (*display_call_contact_respond_cb_t)(const char *device_id,
                                                       bool accept,
                                                       void *ctx);
typedef esp_err_t (*display_call_contact_remark_cb_t)(const char *device_id,
                                                      const char *remark,
                                                      void *ctx);
typedef scan_preview_cb_t display_scan_preview_cb_t;
typedef void (*display_contact_scan_result_cb_t)(esp_err_t result,
                                                 const char *device_id,
                                                 const char *raw_payload,
                                                 void *ctx);
typedef esp_err_t (*display_contact_scan_start_cb_t)(display_scan_preview_cb_t preview_cb,
                                                     display_contact_scan_result_cb_t result_cb,
                                                     void *scan_ctx,
                                                     void *ctx);
typedef void (*display_tirtc_config_scan_result_cb_t)(esp_err_t result,
                                                      const char *device_id,
                                                      const char *device_secret,
                                                      const char *raw_payload,
                                                      void *ctx);
typedef esp_err_t (*display_tirtc_config_scan_start_cb_t)(display_scan_preview_cb_t preview_cb,
                                                         display_tirtc_config_scan_result_cb_t result_cb,
                                                         void *scan_ctx,
                                                         void *ctx);
typedef void (*display_wechat_contact_scan_result_cb_t)(esp_err_t result,
                                                        const char *open_id,
                                                        const char *raw_payload,
                                                        void *ctx);
typedef esp_err_t (*display_wechat_contact_scan_start_cb_t)(display_scan_preview_cb_t preview_cb,
                                                            display_wechat_contact_scan_result_cb_t result_cb,
                                                            void *scan_ctx,
                                                            void *ctx);
typedef esp_err_t (*display_wechat_action_cb_t)(void *ctx);
typedef esp_err_t (*display_wechat_contact_cb_t)(const char *open_id, void *ctx);
typedef esp_err_t (*display_wechat_contact_remark_cb_t)(const char *open_id,
                                                        const char *remark,
                                                        void *ctx);
typedef esp_err_t (*display_tirtc_config_update_cb_t)(display_tirtc_config_field_t field,
                                                      const char *value,
                                                      void *ctx);
typedef esp_err_t (*display_tirtc_server_env_cb_t)(display_tirtc_server_env_t env, void *ctx);
typedef esp_err_t (*display_app_lifecycle_cb_t)(display_app_id_t app_id, void *ctx);
typedef esp_err_t (*display_home_lifecycle_cb_t)(void *ctx);

typedef struct {
    display_wifi_connect_cb_t on_wifi_connect;
    display_uuid_update_cb_t on_set_device_uuid;
    display_simple_action_cb_t on_wifi_scan;
    display_simple_action_cb_t on_ping_test;
    display_simple_action_cb_t on_disconnect_rtc;
    display_simple_action_cb_t on_hangup_call;
    display_simple_action_cb_t on_start_ota;
    display_simple_action_cb_t on_restart_for_ota;
    display_toggle_action_cb_t on_set_local_video_enabled;
    display_toggle_action_cb_t on_set_local_audio_enabled;
    display_percent_cb_t on_set_speaker_volume;
    display_percent_cb_t on_set_capture_gain;
    display_call_contact_cb_t on_call_contact;
    display_call_contact_add_cb_t on_add_call_contact;
    display_call_contact_respond_cb_t on_respond_call_contact;
    display_call_contact_remark_cb_t on_update_call_contact_remark;
    display_call_contact_add_cb_t on_delete_call_contact;
    display_simple_action_cb_t on_refresh_call_contacts;
    display_simple_action_cb_t on_scan_contact;
    display_contact_scan_start_cb_t on_start_contact_scan;
    display_simple_action_cb_t on_stop_contact_scan;
    display_tirtc_config_scan_start_cb_t on_start_tirtc_config_scan;
    display_simple_action_cb_t on_stop_tirtc_config_scan;
    display_simple_action_cb_t on_start_device_binding;
    display_simple_action_cb_t on_reset_device_binding;
    display_wechat_contact_cb_t on_wechat_contact;
    display_wechat_contact_cb_t on_add_wechat_contact;
    display_wechat_contact_cb_t on_remove_wechat_contact;
    display_wechat_contact_remark_cb_t on_update_wechat_contact_remark;
    display_simple_action_cb_t on_scan_wechat_contact;
    display_wechat_contact_scan_start_cb_t on_start_wechat_contact_scan;
    display_simple_action_cb_t on_stop_wechat_contact_scan;
    display_wechat_action_cb_t on_wechat_hangup_call;
    display_wechat_action_cb_t on_wechat_accept_call;
    display_wechat_action_cb_t on_wechat_reject_call;
    display_call_action_cb_t on_accept_call;
    display_call_action_cb_t on_reject_call;
    display_simple_action_cb_t on_start_ai_chat;
    display_simple_action_cb_t on_close_ai_chat;
    display_simple_action_cb_t on_clear_ai_chat_messages;
    display_ai_avatar_cb_t on_set_ai_chat_avatar;
    display_tirtc_config_update_cb_t on_set_tirtc_config_field;
    display_tirtc_server_env_cb_t on_set_tirtc_server_env;
    display_app_lifecycle_cb_t on_enter_app;
    display_home_lifecycle_cb_t on_return_home;
    void *ctx;
} display_actions_t;

esp_err_t display_init(const display_actions_t *actions);
void display_set_snapshot_provider(display_snapshot_cb_t cb, void *ctx);
esp_err_t display_open_home_page_async(void);
esp_err_t display_open_device_page_async(void);
esp_err_t display_open_call_page_async(void);
esp_err_t display_open_call_active_page_async(void);
esp_err_t display_open_wechat_page_async(void);
esp_err_t display_open_wechat_active_page_async(void);
esp_err_t display_open_ai_chat_page_async(void);
esp_err_t display_open_system_page_async(void);
esp_err_t display_capture_bmp(uint8_t **bmp_data, size_t *bmp_size);
esp_err_t display_debug_tap(uint16_t x, uint16_t y);
esp_err_t display_debug_tap_async(uint16_t x, uint16_t y);
esp_err_t display_debug_scroll_async(uint16_t x, uint16_t y, int16_t dx, int16_t dy);
