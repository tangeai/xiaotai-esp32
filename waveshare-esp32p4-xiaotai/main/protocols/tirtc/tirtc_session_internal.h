#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "tirtc_session.h"
#include "tirtc_session_options.h"
#include "tiRTC.h"

#define TIRTC_SESSION_INVALID_STREAM_ID    0xFF
#define TIRTC_SESSION_EVENT_QUEUE_LEN      16
#define TIRTC_SESSION_MESSAGE_STREAM_ID    11
#define TIRTC_SESSION_REMOTE_VIDEO_STREAM_ID 11
#define TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID 10
#define TIRTC_SESSION_REMOTE_AUDIO_STREAM_ID 10
#define TIRTC_SESSION_CMD_RESP_BIT          0x8000U

#define TIRTC_SESSION_CMD_CALL           0x1101
#define TIRTC_SESSION_CMD_VOLUME         0x1102
#define TIRTC_SESSION_CMD_DOOR           0x1103
#define TIRTC_SESSION_CMD_HANGUP         0x1104
#define TIRTC_SESSION_CMD_REQ_VIDEO      0x1105
#define TIRTC_SESSION_CMD_REQ_AUDIO      0x1106
#define TIRTC_SESSION_CMD_SET_SEND_VIDEO 0x1107
#define TIRTC_SESSION_CMD_SET_SEND_AUDIO 0x1108
#define TIRTC_SESSION_CMD_RGB_LEGACY     0x1112
#define TIRTC_SESSION_CMD_STATE_LEGACY   0x1113
#define TIRTC_SESSION_CMD_TIME_QUERY     0x1F11
#define TIRTC_SESSION_CMD_CLOCK_SYNC     0x4001
#define TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED 0x2000
#define TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP    0x2001

typedef struct {
    uint8_t accepted;
} tirtc_session_call_reply_t;

typedef struct {
    uint8_t enabled;
} tirtc_session_toggle_payload_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} tirtc_session_rgb_payload_t;

typedef struct {
    uint8_t call_active;
    uint8_t local_video_send_enabled;
    uint8_t local_audio_send_enabled;
    uint8_t video_stream_active;
    uint8_t audio_stream_active;
    uint8_t rgb[3];
} tirtc_session_peer_state_payload_t;

typedef enum {
    TIRTC_SESSION_EVENT_START_IF_READY = 0,
    TIRTC_SESSION_EVENT_NETWORK_CHANGED,
    TIRTC_SESSION_EVENT_SYS_STARTED,
    TIRTC_SESSION_EVENT_SYS_STOPPED,
    TIRTC_SESSION_EVENT_CONNECT_PEER,
    TIRTC_SESSION_EVENT_DEFERRED_FULL_RESET,
    TIRTC_SESSION_EVENT_DEFERRED_START_AFTER_FULL_RESET,
    TIRTC_SESSION_EVENT_ACCESS_HIJACKING,
    TIRTC_SESSION_EVENT_CONN_ACCEPTED,
    TIRTC_SESSION_EVENT_MEDIA_BOOTSTRAP,
    TIRTC_SESSION_EVENT_CONN_ERROR,
    TIRTC_SESSION_EVENT_DISCONNECTED,
    TIRTC_SESSION_EVENT_SUBSCRIBE_VIDEO,
    TIRTC_SESSION_EVENT_UNSUBSCRIBE_VIDEO,
    TIRTC_SESSION_EVENT_SUBSCRIBE_AUDIO,
    TIRTC_SESSION_EVENT_UNSUBSCRIBE_AUDIO,
    TIRTC_SESSION_EVENT_REMOTE_MESSAGE,
    TIRTC_SESSION_EVENT_REMOTE_COMMAND,
    TIRTC_SESSION_EVENT_VIDEO_BITRATE_REQUIRED,
    TIRTC_SESSION_EVENT_DISCONNECT_REQUEST,
} tirtc_session_event_type_t;

typedef struct {
    tirtc_session_event_type_t type;
    union {
        struct {
            tirtc_session_network_state_t state;
        } network;
        struct {
            tirtc_conn_t conn;
            int error;
        } conn;
        struct {
            tirtc_conn_t conn;
            uint8_t stream_id;
        } subscribe;
        struct {
            tirtc_conn_t conn;
            uint8_t media;
            uint8_t stream_id;
            uint8_t flags;
            uint32_t ts;
            uint8_t *data;
            size_t data_len;
        } message;
        struct {
            tirtc_conn_t conn;
            uint32_t cmdw;
            uint8_t *data;
            size_t data_len;
        } command;
        struct {
            tirtc_conn_t conn;
            uint8_t stream_id;
            uint32_t target_bitrate_bps;
        } video_bitrate;
        struct {
            tirtc_conn_t conn;
            bool complete_shutdown;
            bool was_sdk_started;
        } disconnect;
        struct {
            uint32_t generation;
        } system;
    } payload;
} tirtc_session_event_t;

bool tirtc_session_try_get_active_conn(tirtc_conn_t *conn);
bool tirtc_session_is_connection_usable(tirtc_conn_t conn);
void tirtc_session_note_event(const char *event_text);
void tirtc_session_set_last_error(int error);
void tirtc_session_update_state(tirtc_session_state_t state);
void tirtc_session_mark_sdk_started(void);
bool tirtc_session_mark_sdk_stopped(uint32_t generation);
void tirtc_session_mark_sdk_network_offline(void);
esp_err_t tirtc_session_start_configured_peer_connect(void);
bool tirtc_session_request_runtime_restart(const char *reason);
bool tirtc_session_consume_runtime_restart_request(bool *full_reset_requested);
bool tirtc_session_schedule_deferred_full_reset(void);
void tirtc_session_handle_deferred_full_reset(void);
bool tirtc_session_schedule_deferred_start_after_full_reset(void);
void tirtc_session_handle_deferred_start_after_full_reset(void);
void tirtc_session_schedule_media_bootstrap(const char *reason);
void tirtc_session_retry_remote_media_request(bool retry_video, bool retry_audio, const char *reason);
void tirtc_session_run_media_bootstrap(void);
void tirtc_session_apply_local_media_policy(void);
void tirtc_session_mark_access_hijacking_detected(void);
bool tirtc_session_connection_auto_media_enabled(tirtc_conn_t conn);
bool tirtc_session_connection_media_deferred(tirtc_conn_t conn);
void tirtc_session_notify_connection_accepted(tirtc_conn_t conn);
void tirtc_session_start_time_stream_messages(void);
void tirtc_session_stop_time_stream_messages(void);
void tirtc_session_update_local_video_subscription(tirtc_conn_t conn, uint8_t stream_id, bool subscribed);
void tirtc_session_update_local_audio_subscription(tirtc_conn_t conn, uint8_t stream_id, bool subscribed);
void tirtc_session_handle_connection_loss(tirtc_conn_t hconn, int error);
void tirtc_session_apply_hangup_local_state(void);
void tirtc_session_request_remote_media(void);
void tirtc_session_get_pending_call(tirtc_conn_t *conn, uint32_t *pending_cmdw);
void tirtc_session_mark_incoming_call(uint32_t pending_cmdw);
void tirtc_session_complete_call_response(bool accepted);
void tirtc_session_complete_call_response_without_media(bool accepted);
void tirtc_session_set_local_rgb(uint8_t red, uint8_t green, uint8_t blue);
void tirtc_session_set_peer_rgb(uint8_t red, uint8_t green, uint8_t blue);
void tirtc_session_set_peer_video_requested(bool enabled);
void tirtc_session_set_peer_audio_requested(bool enabled);
void tirtc_session_get_local_peer_state(tirtc_session_peer_state_t *state);
void tirtc_session_set_last_peer_state(const tirtc_session_peer_state_t *state);
esp_err_t tirtc_session_apply_video_bitrate_params(tirtc_conn_t conn);
esp_err_t tirtc_session_apply_remote_volume_command(uint8_t percent);
esp_err_t tirtc_session_apply_remote_door_command(bool open);
esp_err_t tirtc_session_send_command_raw(tirtc_conn_t conn, uint32_t cmdw, const void *data, size_t data_len);
bool tirtc_session_should_reset_after_send_error(int error);
bool tirtc_session_should_retry_message_stream_after_invalid_handle(tirtc_conn_t conn, const char *operation);
bool tirtc_session_take_sdk_api_lock(TickType_t wait_ticks);
void tirtc_session_give_sdk_api_lock(void);
esp_err_t tirtc_session_send_media_toggle_request(uint16_t cmd, bool enabled);
esp_err_t tirtc_session_request_call(void);
void tirtc_session_handle_runtime_event(const tirtc_session_event_t *event);
void tirtc_session_handle_remote_command(const tirtc_session_event_t *event);
bool tirtc_session_is_test_video_active(void);
bool tirtc_session_is_test_audio_active(void);
bool tirtc_session_is_test_media_active(void);
void tirtc_session_request_test_audio_restart(void);
