#include "tirtc_session_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_parser.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"

#include "audio_alaw_codec.h"
#include "app_log_policy.h"
#include "app_task_affinity.h"
#include "media_governor.h"
#include "media_tuning.h"
#include "system_time.h"
#include "tirtc_connect.h"
#include "tirtc_session_options.h"
#include "tiRTC.h"

static const char *TAG = "tirtc_session";
static const char *TIRTC_SDK_LOG_TAG = "tirtc_sdk";

#define TIRTC_SESSION_OBSERVER_MAX 4
#define TIRTC_SESSION_RX_LOG_INTERVAL_MS 1000U
#define TIRTC_SESSION_TX_LOG_INTERVAL_MS 10000U
#define TIRTC_SESSION_MESSAGE_PREVIEW_BYTES 8U
#define TIRTC_SESSION_MESSAGE_PREVIEW_TEXT_LEN ((TIRTC_SESSION_MESSAGE_PREVIEW_BYTES * 3U) + 1U)
#define TIRTC_SESSION_REMOTE_AUDIO_MAX_PAYLOAD 8192U
#define TIRTC_SESSION_BAD_REMOTE_AUDIO_LOG_INTERVAL_MS 5000U
#define TIRTC_SESSION_MEDIA_SEND_ERROR_LOG_INTERVAL_MS 1000U
#define TIRTC_SESSION_REMOTE_KEY_FRAME_RETRY_MS 2000U
#define TIRTC_SESSION_REMOTE_VIDEO_FIRST_PACKET_RETRY_US (1000ULL * 1000ULL)
#define TIRTC_SESSION_SHA256_HEX_LEN 65U
#define TIRTC_SESSION_IPC_AUDIO_SAMPLE_RATE_HZ 8000U
#define TIRTC_SESSION_IPC_AUDIO_STACK_ALAW_BYTES 512U
#define TIRTC_SESSION_CLIENT_ID_CONFLICT_RETRY_DELAY_US (60ULL * 1000ULL * 1000ULL)

enum {
    TIRTC_SESSION_VIDEO_TX_NOT_READY = INT_MIN + 1,
    TIRTC_SESSION_VIDEO_TX_WAIT_KEY_FRAME = INT_MIN + 2,
    TIRTC_SESSION_VIDEO_TX_THROTTLED = INT_MIN + 3,
};

typedef enum {
    TIRTC_SESSION_SEND_MEDIA_AUDIO = 0,
    TIRTC_SESSION_SEND_MEDIA_VIDEO,
} tirtc_session_send_media_t;

#if defined(TIRTC_VERSION_MAJOR) && defined(TIRTC_VERSION_MINOR) && \
    ((TIRTC_VERSION_MAJOR > 2) || (TIRTC_VERSION_MAJOR == 2 && TIRTC_VERSION_MINOR >= 2))
#define TIRTC_SESSION_HAS_CLIENT_ID_OPTION 1
#else
#define TIRTC_SESSION_HAS_CLIENT_ID_OPTION 0
#endif

#if defined(TIRTC_VERSION_MAJOR) && defined(TIRTC_VERSION_MINOR) && \
    ((TIRTC_VERSION_MAJOR > 2) || (TIRTC_VERSION_MAJOR == 2 && TIRTC_VERSION_MINOR >= 3))
#define TIRTC_SESSION_HAS_TGMP_BITRATE_CONTROL 1
#else
#define TIRTC_SESSION_HAS_TGMP_BITRATE_CONTROL 0
#endif

#define TIRTC_SESSION_BITRATE_EVENT_MIN_INTERVAL_US APP_MEDIA_TGMP_EVENT_MIN_INTERVAL_US
#define TIRTC_SESSION_BITRATE_EVENT_FAST_STEP_BPS   APP_MEDIA_TGMP_EVENT_FAST_STEP_BPS

#ifndef CONFIG_APP_RTC_WAIT_VIDEO_SUBSCRIBE_BEFORE_CAPTURE
#define CONFIG_APP_RTC_WAIT_VIDEO_SUBSCRIBE_BEFORE_CAPTURE 0
#endif

#ifndef CONFIG_APP_TIRTC_QUERY_SEND_BUFFER_USED
#define CONFIG_APP_TIRTC_QUERY_SEND_BUFFER_USED 1
#endif

#ifndef CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
#define CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS 0
#endif
#ifndef CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
#define CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG 0
#endif

static bool tirtc_session_sha256_hex(const char *input, char *out_hex, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32] = {0};

    if (input == NULL || out_hex == NULL || out_size < TIRTC_SESSION_SHA256_HEX_LEN) {
        return false;
    }

    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char *)input,
                   strlen(input),
                   digest) != 0) {
        return false;
    }

    for (size_t i = 0; i < sizeof(digest); ++i) {
        out_hex[i * 2U] = hex[(digest[i] >> 4) & 0x0F];
        out_hex[i * 2U + 1U] = hex[digest[i] & 0x0F];
    }
    out_hex[sizeof(digest) * 2U] = '\0';
    return true;
}

typedef enum {
    TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED = 0,
    TIRTC_SESSION_AUDIO_TX_GATE_CALL,
    TIRTC_SESSION_AUDIO_TX_GATE_TEST,
} tirtc_session_audio_tx_gate_t;

typedef enum {
    TIRTC_SESSION_MEDIA_PROFILE_AV = 0,
    TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO,
} tirtc_session_media_profile_t;

typedef struct {
    uint32_t generation;
    tirtc_conn_t expected_conn;
    tirtc_session_audio_format_t format;
    uint64_t pts_us;
    uint8_t buffer_slot;
    uint8_t *data;
    size_t data_len;
    tirtc_session_audio_tx_gate_t gate;
} tirtc_session_local_audio_packet_t;

typedef struct {
    uint32_t generation;
    tirtc_conn_t expected_conn;
    uint16_t width;
    uint16_t height;
    uint64_t pts_us;       /* Media clock carried into TiRTC. */
    uint64_t queued_at_us; /* Local queue-admission clock. */
    TIRTCFRAMEINFO frame_info;
    uint8_t buffer_slot;
    uint8_t *data;
    size_t data_len;
    uint8_t media;
    uint8_t flags;
    bool has_frame_info;
    bool test_frame;
    bool external_frame;
    uint8_t external_stream_id;
} tirtc_session_local_video_packet_t;

typedef struct {
    bool enabled;
    bool sdk_started;
    bool sdk_initialized;
    bool start_in_progress;
    bool stop_in_progress;
    tirtc_conn_t active_conn;
    tirtc_conn_t closing_conn;
} tirtc_session_runtime_snapshot_t;

typedef enum {
    TIRTC_SESSION_CONN_ACCEPT_REJECTED = 0,
    TIRTC_SESSION_CONN_ACCEPTED,
    TIRTC_SESSION_CONN_ACCEPT_STALE_CLOSING,
} tirtc_session_conn_accept_result_t;

typedef struct {
    TIRTCCONNECTCALLBACK cb;
    void *user_data;
    char *service_desc;
    char *token;
    uint32_t attempt_id;
} tirtc_session_whip_request_t;

typedef struct {
    uint32_t magic;
    uint32_t generation;
    uint64_t accepted_at_us;
} tirtc_session_conn_user_data_t;

typedef struct {
    tirtc_session_observer_t observer;
    void *ctx;
    bool used;
} tirtc_session_observer_slot_t;

static QueueHandle_t s_event_queue;
static QueueHandle_t s_local_video_tx_queue;
static QueueHandle_t s_local_video_tx_free_queue;
static QueueHandle_t s_local_audio_tx_queue;
static QueueHandle_t s_local_audio_tx_free_queue;
static SemaphoreHandle_t s_tirtc_api_mutex;
static TaskHandle_t s_worker_task;
static TaskHandle_t s_local_video_tx_task;
static TaskHandle_t s_local_audio_tx_task;
static tirtc_session_config_t s_config;
static tirtc_session_media_ops_t s_media_ops;
static void *s_media_ctx;
static tirtc_session_hooks_t s_hooks;
static void *s_hooks_ctx;
static tirtc_session_control_ops_t s_control_ops;
static void *s_control_ctx;
static tirtc_session_mode_t s_session_mode;
static tirtc_session_state_t s_state = TIRTC_SESSION_STATE_STOPPED;
static tirtc_conn_t s_active_conn;
static uint64_t s_active_conn_accepted_at_us;
static tirtc_session_conn_user_data_t s_active_conn_user_data;
static bool s_state_error_override;
static tirtc_conn_t s_closing_conn;
static uint32_t s_whip_connect_attempt_counter;
static uint32_t s_whip_connect_active_attempt;
static bool s_initialized;
static bool s_sdk_initialized;
static bool s_sdk_prepare_in_progress;
static bool s_sdk_started;
static bool s_sdk_stop_notified = true;
static uint32_t s_sdk_generation;
static uint32_t s_pending_stop_generation;
static bool s_platform_initialized;
static bool s_network_connected;
static bool s_start_in_progress;
static bool s_restart_runtime_requested;
static bool s_restart_runtime_full_requested;
static bool s_stop_in_progress;
static uint64_t s_next_start_allowed_us;
static bool s_force_wall_clock_sync_requested;
static bool s_local_video_send_enabled;
static bool s_local_audio_send_enabled;
static esp_timer_handle_t s_time_message_initial_timer;
static esp_timer_handle_t s_time_message_periodic_timer;
static esp_timer_handle_t s_media_bootstrap_timer;
static esp_timer_handle_t s_remote_video_first_packet_timer;
static esp_timer_handle_t s_disconnect_watchdog_timer;
static esp_timer_handle_t s_deferred_full_reset_timer;
static esp_timer_handle_t s_deferred_start_after_full_reset_timer;
static bool s_deferred_full_reset_pending;
static bool s_deferred_start_after_full_reset_pending;
static uint64_t s_deferred_full_reset_due_at_us;
static uint64_t s_deferred_start_after_full_reset_due_at_us;
static bool s_peer_wants_video;
static bool s_peer_wants_audio;
static bool s_peer_video_control_seen;
static bool s_peer_audio_control_seen;
static bool s_peer_video_subscription_active;
static uint64_t s_local_video_first_requested_at_us;
static bool s_builtin_capture_enabled;
static bool s_builtin_video_capture_enabled;
static tirtc_session_media_profile_t s_media_profile = TIRTC_SESSION_MEDIA_PROFILE_AV;
static uint8_t s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
static uint8_t s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
static tirtc_conn_t s_external_video_conn;
static uint8_t s_external_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
static bool s_external_video_active;
static bool s_local_video_publish_forced;
static bool s_local_audio_publish_forced;
static bool s_test_video_publish_forced;
static bool s_test_audio_publish_forced;
static bool s_next_connection_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
static bool s_active_conn_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
static bool s_active_conn_supports_tgmp_bitrate;
static bool s_next_connection_defer_media;
static bool s_active_conn_defer_media;
static bool s_call_media_deferred;
static bool s_call_active;
static bool s_incoming_call_pending;
static uint32_t s_pending_call_cmdw;
static bool s_remote_video_requested;
static bool s_remote_audio_requested;
static bool s_remote_video_receive_enabled;
static bool s_media_bootstrap_pending;
static uint64_t s_remote_video_first_request_at_us;
static uint64_t s_remote_video_first_submit_attempt_us;
static uint8_t s_remote_video_request_attempts;
static bool s_remote_video_first_packet_retry_armed;
static bool s_remote_video_first_packet_retry_due;
static bool s_remote_video_first_packet_logged;
static uint64_t s_remote_video_last_packet_us;
static uint64_t s_remote_video_last_submit_us;
static uint64_t s_remote_video_last_recovery_us;
static uint64_t s_remote_video_last_liveness_log_us;
static uint32_t s_remote_video_callback_frames;
static uint32_t s_remote_video_submit_failures;
static uint32_t s_remote_video_callback_gap_window_max_us;
static bool s_remote_video_rx_stalled;
static bool s_remote_audio_first_packet_logged;
static bool s_remote_message_first_packet_logged;
static bool s_local_video_first_packet_logged;
static bool s_local_audio_first_packet_logged;
static bool s_local_h264_key_frame_queued;
static bool s_local_h264_key_frame_published;
static bool s_local_h264_recovery_pending;
static uint32_t s_local_h264_recovery_count;
static bool s_closing_conn_was_sdk_started;
static char s_started_device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
static char s_started_credential_hash[TIRTC_SESSION_SHA256_HEX_LEN];
static uint32_t s_started_secret_len;
static uint32_t s_sys_started_callback_count;
static uint64_t s_test_video_retry_after_us;
static uint64_t s_test_audio_retry_after_us;
static uint8_t s_local_rgb[3] = {0x2E, 0x8F, 0x6B};
static tirtc_session_peer_state_t s_last_peer_state;
static bool s_video_bitrate_params_enabled;
static tirtc_session_video_bitrate_params_t s_video_bitrate_params;
static tirtc_conn_t s_last_video_bitrate_event_conn;
static uint32_t s_last_video_bitrate_event_bps;
static uint64_t s_last_video_bitrate_event_us;
static tirtc_session_stats_t s_stats = {
    .local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID,
    .local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID,
    .send_buffer_limit = TIRTC_SESSION_MAX_SEND_BUFFER,
};
static tirtc_session_observer_slot_t s_observers[TIRTC_SESSION_OBSERVER_MAX];
static uint8_t *s_local_video_tx_buffers[TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE];
static size_t s_local_video_tx_buffer_capacities[TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE];
static uint8_t *s_local_audio_tx_buffers[TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE];
static portMUX_TYPE s_rtc_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_observer_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_local_video_tx_generation;
static uint32_t s_local_audio_tx_generation;
static TickType_t s_last_local_audio_queue_fail_log_tick;
static TickType_t s_last_send_buffer_log_tick;
static TickType_t s_last_send_buffer_query_tick;
static tirtc_conn_t s_last_send_buffer_query_conn;
static uint64_t s_local_video_last_enqueue_us;
static uint64_t s_local_video_last_dequeue_us;
static uint64_t s_local_video_last_send_attempt_us;
static uint64_t s_local_video_last_send_success_us;
static uint64_t s_local_video_tx_stall_started_us;
static uint64_t s_local_video_tx_last_stall_log_us;
static bool s_local_video_tx_stalled;
static TickType_t s_last_local_video_tx_issue_log_tick;
static TickType_t s_last_local_video_tx_pool_log_tick;
static TickType_t s_last_remote_audio_rx_log_tick;
static TickType_t s_last_local_audio_tx_log_tick;
static TickType_t s_last_bad_remote_audio_log_tick;
static TickType_t s_last_media_send_error_log_tick;
static TickType_t s_last_remote_message_rx_log_tick;
static TickType_t s_last_remote_key_frame_request_tick;
static uint32_t s_local_audio_tx_window_frames;
static size_t s_local_audio_tx_window_payload_bytes;
static uint32_t s_local_audio_tx_window_peak_percent;
static uint32_t s_remote_audio_rx_window_frames;
static size_t s_remote_audio_rx_window_payload_bytes;
static size_t s_remote_audio_rx_window_playback_bytes;
static uint32_t s_remote_message_rx_window_frames;
static size_t s_remote_message_rx_window_bytes;


static void tirtc_session_worker_task(void *ctx);
static void tirtc_session_local_video_tx_task(void *ctx);
static void tirtc_session_local_audio_tx_task(void *ctx);
static void tirtc_session_init_stats(void);
static esp_err_t tirtc_session_create_queue(QueueHandle_t *queue,
                                            UBaseType_t length,
                                            UBaseType_t item_size,
                                            uint32_t caps);
static esp_err_t tirtc_session_create_timer(const char *name,
                                           esp_timer_cb_t callback,
                                           esp_timer_handle_t *handle);
static esp_err_t tirtc_session_create_task(TaskFunction_t task_entry,
                                           const char *name,
                                           uint32_t stack_size,
                                           UBaseType_t priority,
                                           TaskHandle_t *handle);
#if TIRTC_SESSION_WORKER_STACK_INTERNAL
static esp_err_t tirtc_session_create_internal_task(TaskFunction_t task_entry,
                                                    const char *name,
                                                    uint32_t stack_size,
                                                    UBaseType_t priority,
                                                    TaskHandle_t *handle);
#endif
static esp_err_t tirtc_session_create_event_resources(void);
static esp_err_t tirtc_session_create_local_video_tx_resources(void);
static esp_err_t tirtc_session_create_local_audio_tx_resources(void);
static esp_err_t tirtc_session_create_timers(void);
static esp_err_t tirtc_session_create_tasks(void);
static esp_err_t tirtc_session_create_runtime_resources(void);
static void tirtc_session_configure_runtime_callbacks(void);
static void tirtc_session_get_runtime_snapshot(tirtc_session_runtime_snapshot_t *snapshot);
static bool tirtc_session_is_ready_for_new_connection_locked(void);
static uint32_t tirtc_session_begin_whip_connect_attempt(tirtc_conn_t *active_conn,
                                                         tirtc_conn_t *closing_conn);
static void tirtc_session_finish_whip_connect_attempt(uint32_t attempt_id);
static void tirtc_session_update_pool_stats_unlocked(tirtc_session_stats_t *stats);
static void tirtc_session_update_queue_stats(tirtc_session_stats_t *stats);
static void tirtc_session_bind_connection_user_data(tirtc_conn_t conn);
static void tirtc_session_log_connection_user_data(const char *phase, tirtc_conn_t conn);
static void tirtc_session_log_connection_close_snapshot(const char *phase,
                                                        tirtc_conn_t conn,
                                                        int error);
static bool tirtc_session_should_log_media_send_error(void);
static void tirtc_session_note_transient_send_error(const char *media_name,
                                                    tirtc_conn_t conn,
                                                    uint8_t stream_id,
                                                    uint32_t payload_len,
                                                    int error);
static bool tirtc_session_check_send_buffer(tirtc_conn_t conn,
                                            tirtc_session_send_media_t media,
                                            const char *media_name,
                                            bool can_drop);
static int tirtc_session_disconnect_with_sdk_lock(tirtc_conn_t conn);
static tirtc_session_conn_accept_result_t tirtc_session_accept_connection(
    tirtc_conn_t conn,
    bool require_connect_mode,
    bool supports_tgmp_bitrate);
static esp_err_t tirtc_session_request_remote_key_frame(tirtc_conn_t conn, uint8_t stream_id, const char *reason);
static bool tirtc_session_take_remote_key_frame_retry_slot(void);
static esp_err_t tirtc_session_request_remote_video(tirtc_conn_t conn);
static bool tirtc_session_should_retry_media_request_after_invalid_handle(tirtc_conn_t conn, const char *operation);
static void tirtc_session_set_last_event_locked(const char *event_text);
static void tirtc_session_mark_local_video_requested_locked(void);
static void tirtc_session_reset_call_state_locked(void);
static bool tirtc_session_should_defer_audio_for_local_video_locked(void);
static bool tirtc_session_should_prepare_playback_after_bootstrap(void);
static void tirtc_session_build_local_peer_state_locked(tirtc_session_peer_state_t *state);
static tirtc_session_state_t tirtc_session_compute_state_locked(void);
static void tirtc_session_sync_stats_locked(void);
static void tirtc_session_return_to_listen_mode(void);
static void tirtc_session_free_event_payload(tirtc_session_event_t *event);
static void tirtc_session_handle_remote_message(const tirtc_session_event_t *event);
static const char *tirtc_session_media_name(uint8_t media);
static void tirtc_session_format_payload_head(const uint8_t *data, size_t data_len, char *out, size_t out_len);
static void tirtc_session_free_local_video_packet(tirtc_session_local_video_packet_t *packet);
static void tirtc_session_free_local_audio_packet(tirtc_session_local_audio_packet_t *packet);
static void tirtc_session_flush_local_video_tx_queue(void);
static void tirtc_session_flush_local_audio_tx_queue(void);
static void tirtc_session_recover_local_h264_stream(const char *reason,
                                                     bool network_backpressure);
static void tirtc_session_trim_local_audio_tx_queue(UBaseType_t max_packets);
static esp_err_t tirtc_session_acquire_local_video_buffer_slot(uint8_t *slot_out);
static void tirtc_session_release_local_video_buffer_slot(uint8_t slot);
static esp_err_t tirtc_session_acquire_local_audio_buffer_slot(uint8_t *slot_out);
static void tirtc_session_release_local_audio_buffer_slot(uint8_t slot);
static esp_err_t tirtc_session_ensure_local_video_buffer_capacity(uint8_t slot, size_t required_size);
static bool tirtc_session_enqueue_event(const tirtc_session_event_t *event, TickType_t wait_ticks);
static bool tirtc_session_enqueue_start_if_ready(void);
static bool tirtc_session_enqueue_disconnect_request(tirtc_conn_t conn,
                                                     bool complete_shutdown,
                                                     bool was_sdk_started);
static void tirtc_session_copy_config_snapshot(tirtc_session_config_t *config);
static bool tirtc_session_extract_start_identity(const tirtc_session_config_t *config,
                                                 char *device_id,
                                                 size_t device_id_size,
                                                 char *secret_key,
                                                 size_t secret_key_size);
static const char *tirtc_session_start_error_name(int error);
static void tirtc_session_clear_start_in_progress(void);
static esp_err_t tirtc_session_prepare_sdk_with_lock(void);
static esp_err_t tirtc_session_start_sdk_from_worker(void);
static void tirtc_session_handle_disconnect_request(const tirtc_session_event_t *event);
static bool tirtc_session_complete_connection_shutdown(tirtc_conn_t hconn, bool was_sdk_started);
static void tirtc_session_release_remote_media(void);
static bool tirtc_session_begin_connection_shutdown(tirtc_conn_t hconn,
                                                   int error,
                                                   bool *was_sdk_started,
                                                   bool *newly_detached_out);
static bool tirtc_session_enqueue_teardown_event(const tirtc_session_event_t *event);
static void tirtc_session_stop_disconnect_watchdog(void);
static bool tirtc_session_schedule_disconnect_watchdog(const char *reason, uint64_t delay_us);
static bool tirtc_session_maybe_force_local_video_publish_locked(void);
static bool tirtc_session_maybe_force_local_audio_publish_locked(void);
static void tirtc_session_sync_test_media_publish_locked(bool test_video_active, bool test_audio_active);
static uint8_t tirtc_session_get_effective_local_video_stream_id_locked(void);
static uint8_t tirtc_session_get_effective_local_audio_stream_id_locked(void);
static uint8_t tirtc_session_normalize_local_video_stream_id(uint8_t stream_id);
static uint8_t tirtc_session_normalize_local_audio_stream_id(uint8_t stream_id);
bool tirtc_session_should_reset_after_send_error(int error);
static bool tirtc_session_is_test_media_window_open_locked(uint64_t now_us, uint64_t retry_after_us);
static bool tirtc_session_is_media_bootstrap_ready_locked(void);
static bool tirtc_session_media_profile_allows_remote_video_locked(void);
static void tirtc_session_local_audio_cb(const uint8_t *data,
                                        size_t data_len,
                                        const tirtc_session_audio_format_t *format,
                                        void *ctx);
#if TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
static uint32_t tirtc_session_get_unix_time_s(void);
#endif
static esp_err_t tirtc_session_send_time_stream_message(void);
static void tirtc_session_time_message_initial_timer_cb(void *arg);
static void tirtc_session_time_message_periodic_timer_cb(void *arg);
static void tirtc_session_media_bootstrap_timer_cb(void *arg);
static void tirtc_session_remote_video_first_packet_timer_cb(void *arg);
static void tirtc_session_disconnect_watchdog_timer_cb(void *arg);
static void tirtc_session_deferred_full_reset_timer_cb(void *arg);
static void tirtc_session_deferred_start_after_full_reset_timer_cb(void *arg);
static void tirtc_session_configure_sdk_logs(bool announce);
static void tirtc_session_sdk_log_cb(const char *log, uint32_t length);
static void tirtc_session_on_peer_connect_result(int error, tirtc_conn_t hconn, void *user_data);
static void tirtc_session_on_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data);
static void tirtc_session_on_event(int event, const void *data, int len);
static void tirtc_session_on_conn_accepted(tirtc_conn_t hconn);
static void tirtc_session_on_conn_error(tirtc_conn_t hconn, int error);
static void tirtc_session_on_disconnected(tirtc_conn_t hconn);
static void tirtc_session_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data);
static void tirtc_session_on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data);
static void tirtc_session_on_message(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data);
static void tirtc_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len);
static int tirtc_session_on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id);
static void tirtc_session_on_unsubscribe_video(tirtc_conn_t hconn, uint8_t stream_id);
static int tirtc_session_on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id);
static void tirtc_session_on_unsubscribe_audio(tirtc_conn_t hconn, uint8_t stream_id);
static void tirtc_session_on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id);
#if TIRTC_SESSION_HAS_TGMP_BITRATE_CONTROL
static void tirtc_session_on_video_bitrate_required(tirtc_conn_t hconn,
                                                    uint8_t stream_id,
                                                    uint32_t target_bitrate_bps);
#endif
static bool tirtc_session_notify_command(tirtc_conn_t conn, uint32_t cmdw, const void *data, uint32_t data_len);
static void tirtc_session_notify_connection_error(tirtc_conn_t conn, int error);
static void tirtc_session_notify_disconnected(tirtc_conn_t conn);
static void tirtc_session_notify_video_bitrate_required(tirtc_conn_t conn,
                                                        uint8_t stream_id,
                                                        uint32_t target_bitrate_bps);
static void tirtc_session_on_external_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data);
static bool tirtc_session_should_tolerate_invalid_handle(tirtc_conn_t conn, uint64_t *conn_age_us);
bool tirtc_session_should_retry_message_stream_after_invalid_handle(tirtc_conn_t conn, const char *operation);
static void tirtc_session_retry_remote_media_request_after_delay(bool retry_video,
                                                                bool retry_audio,
                                                                const char *reason,
                                                                uint64_t delay_us);
extern int SA_platInit(void);

static const TIRTCCALLBACKS s_tirtc_callbacks = {
    .on_event = tirtc_session_on_event,
    .on_conn_accepted = tirtc_session_on_conn_accepted,
    .on_conn_error = tirtc_session_on_conn_error,
    .on_disconnected = tirtc_session_on_disconnected,
    .on_audio = tirtc_session_on_audio,
    .on_video = tirtc_session_on_video,
    .on_message = tirtc_session_on_message,
    .on_command = tirtc_session_on_command,
    .on_request_key_frame = tirtc_session_on_request_key_frame,
    .on_subscribe_video = tirtc_session_on_subscribe_video,
    .on_unsubscribe_video = tirtc_session_on_unsubscribe_video,
    .on_subscribe_audio = tirtc_session_on_subscribe_audio,
    .on_unsubscribe_audio = tirtc_session_on_unsubscribe_audio,
#if TIRTC_SESSION_HAS_TGMP_BITRATE_CONTROL
    .on_update_bitrate = tirtc_session_on_video_bitrate_required,
#endif
};

static bool tirtc_session_has_media_bridge(void)
{
    return s_media_ops.init != NULL ||
           s_media_ops.set_capture_frame_cb != NULL ||
           s_media_ops.set_capture_enabled != NULL ||
           s_media_ops.set_video_capture_enabled != NULL ||
           s_media_ops.request_video_key_frame != NULL ||
           s_media_ops.request_video_stream_start_key_frame != NULL ||
           s_media_ops.prepare_playback_path != NULL ||
           s_media_ops.submit_remote_audio != NULL ||
           s_media_ops.submit_remote_video != NULL ||
           s_media_ops.remote_video_requires_key_frame != NULL ||
           s_media_ops.flush != NULL;
}

static size_t tirtc_session_copy_observers(tirtc_session_observer_slot_t *out, size_t out_count)
{
    size_t count = 0;

    if (out == NULL || out_count == 0U) {
        return 0;
    }

    taskENTER_CRITICAL(&s_observer_lock);
    for (size_t index = 0; index < TIRTC_SESSION_OBSERVER_MAX && count < out_count; ++index) {
        if (s_observers[index].used) {
            out[count++] = s_observers[index];
        }
    }
    taskEXIT_CRITICAL(&s_observer_lock);

    return count;
}

static bool tirtc_session_notify_command(tirtc_conn_t conn, uint32_t cmdw, const void *data, uint32_t data_len)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_command != NULL &&
            observers[index].observer.on_command(conn, cmdw, data, data_len, observers[index].ctx)) {
            return true;
        }
    }

    return false;
}

static bool tirtc_session_notify_message(tirtc_conn_t conn,
                                         uint8_t media,
                                         uint8_t stream_id,
                                         uint8_t flags,
                                         const void *data,
                                         uint32_t data_len)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_message != NULL &&
            observers[index].observer.on_message(conn,
                                                 media,
                                                 stream_id,
                                                 flags,
                                                 data,
                                                 data_len,
                                                 observers[index].ctx)) {
            return true;
        }
    }

    return false;
}

static void tirtc_session_notify_connection_error(tirtc_conn_t conn, int error)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_connection_error != NULL) {
            observers[index].observer.on_connection_error(conn, error, observers[index].ctx);
        }
    }
}

void tirtc_session_notify_connection_accepted(tirtc_conn_t conn)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_connection_accepted != NULL) {
            observers[index].observer.on_connection_accepted(conn, observers[index].ctx);
        }
    }
}

static void tirtc_session_notify_disconnected(tirtc_conn_t conn)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_disconnected != NULL) {
            observers[index].observer.on_disconnected(conn, observers[index].ctx);
        }
    }
}

static void tirtc_session_notify_video_bitrate_required(tirtc_conn_t conn,
                                                        uint8_t stream_id,
                                                        uint32_t target_bitrate_bps)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_video_bitrate_required != NULL) {
            observers[index].observer.on_video_bitrate_required(conn,
                                                                stream_id,
                                                                target_bitrate_bps,
                                                                observers[index].ctx);
        }
    }
}

static void tirtc_session_notify_call_active(bool active)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_call_active != NULL) {
            observers[index].observer.on_call_active(active, observers[index].ctx);
        }
    }
}

static void tirtc_session_notify_start_error(int error, const char *device_id, const char *client_id)
{
    tirtc_session_observer_slot_t observers[TIRTC_SESSION_OBSERVER_MAX] = {0};
    size_t count = tirtc_session_copy_observers(observers, TIRTC_SESSION_OBSERVER_MAX);

    for (size_t index = 0; index < count; ++index) {
        if (observers[index].observer.on_start_error != NULL) {
            observers[index].observer.on_start_error(error,
                                                     device_id != NULL ? device_id : "",
                                                     client_id != NULL ? client_id : "",
                                                     observers[index].ctx);
        }
    }
}

static esp_err_t tirtc_session_media_init(void)
{
    if (s_media_ops.init == NULL) {
        return ESP_OK;
    }

    return s_media_ops.init(s_media_ctx);
}

static void tirtc_session_media_set_capture_cb(tirtc_session_capture_frame_cb_t cb, void *cb_ctx)
{
    if (s_media_ops.set_capture_frame_cb != NULL) {
        s_media_ops.set_capture_frame_cb(cb, cb_ctx, s_media_ctx);
    }
}

static esp_err_t tirtc_session_media_set_capture_enabled(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_media_ops.set_capture_enabled != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media capture bridge not configured");

    return s_media_ops.set_capture_enabled(enabled, s_media_ctx);
}

static esp_err_t tirtc_session_media_set_video_capture_enabled(bool enabled)
{
    if (s_media_ops.set_video_capture_enabled == NULL) {
        return enabled ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
    }

    return s_media_ops.set_video_capture_enabled(enabled, s_media_ctx);
}

static void tirtc_session_media_request_video_key_frame(void)
{
    if (s_media_ops.request_video_key_frame != NULL) {
        s_media_ops.request_video_key_frame(s_media_ctx);
    }
}

static void tirtc_session_media_request_video_stream_start_key_frame(void)
{
    if (s_media_ops.request_video_stream_start_key_frame != NULL) {
        s_media_ops.request_video_stream_start_key_frame(s_media_ctx);
        return;
    }
    tirtc_session_media_request_video_key_frame();
}

static esp_err_t tirtc_session_media_prepare_playback_path(void)
{
    ESP_RETURN_ON_FALSE(s_media_ops.prepare_playback_path != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media playback bridge not configured");

    return s_media_ops.prepare_playback_path(s_media_ctx);
}

static esp_err_t tirtc_session_media_submit_remote_audio(uint8_t media,
                                                         uint8_t flags,
                                                         const uint8_t *data,
                                                         size_t data_len,
                                                         uint32_t pts,
                                                         size_t *playback_data_len)
{
    ESP_RETURN_ON_FALSE(s_media_ops.submit_remote_audio != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media audio sink not configured");

    return s_media_ops.submit_remote_audio(media,
                                           flags,
                                           data,
                                           data_len,
                                           pts,
                                           playback_data_len,
                                           s_media_ctx);
}

static esp_err_t tirtc_session_media_submit_remote_video(uint8_t media,
                                                         uint8_t flags,
                                                         const uint8_t *data,
                                                         size_t data_len,
                                                         uint32_t pts)
{
    ESP_RETURN_ON_FALSE(s_media_ops.submit_remote_video != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media video sink not configured");

    return s_media_ops.submit_remote_video(media, flags, data, data_len, pts, s_media_ctx);
}

static bool tirtc_session_media_remote_video_requires_key_frame(void)
{
    if (s_media_ops.remote_video_requires_key_frame == NULL) {
        return true;
    }
    return s_media_ops.remote_video_requires_key_frame(s_media_ctx);
}

static void tirtc_session_media_flush(void)
{
    if (s_media_ops.flush != NULL) {
        s_media_ops.flush(s_media_ctx);
    }
}

void tirtc_session_flush_remote_media(void)
{
    tirtc_session_media_flush();
}

static uint8_t *tirtc_session_alloc_video_tx_buffer(size_t size)
{
    return app_memory_alloc_psram(size);
}

static char *tirtc_session_strdup_psram(const char *value)
{
    size_t len = 0;
    char *copy = NULL;

    if (value == NULL) {
        return NULL;
    }

    len = strlen(value) + 1U;
    copy = app_memory_alloc_psram(len);
    if (copy != NULL) {
        memcpy(copy, value, len);
    }
    return copy;
}

static void tirtc_session_free_whip_request(tirtc_session_whip_request_t *request)
{
    if (request == NULL) {
        return;
    }

    free(request->service_desc);
    free(request->token);
    free(request);
}

static tirtc_session_whip_request_t *tirtc_session_alloc_whip_request(const char *service_desc,
                                                                      const char *token,
                                                                      TIRTCCONNECTCALLBACK cb,
                                                                      void *user_data)
{
    tirtc_session_whip_request_t *request = app_memory_calloc_psram(1, sizeof(*request));
    if (request == NULL) {
        return NULL;
    }

    request->service_desc = tirtc_session_strdup_psram(service_desc);
    request->token = tirtc_session_strdup_psram(token);
    if (request->service_desc == NULL || request->token == NULL) {
        tirtc_session_free_whip_request(request);
        return NULL;
    }

    request->cb = cb;
    request->user_data = user_data;
    return request;
}

static void tirtc_session_init_stats(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_stats.local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_stats.send_buffer_limit = TIRTC_SESSION_MAX_SEND_BUFFER;
    tirtc_session_set_last_event_locked("init");
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static esp_err_t tirtc_session_create_queue(QueueHandle_t *queue,
                                            UBaseType_t length,
                                            UBaseType_t item_size,
                                            uint32_t caps)
{
    ESP_RETURN_ON_FALSE(queue != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid queue target");

    if (*queue == NULL) {
        *queue = xQueueCreateWithCaps(length, item_size, caps);
    }

    return *queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t tirtc_session_create_timer(const char *name,
                                           esp_timer_cb_t callback,
                                           esp_timer_handle_t *handle)
{
    esp_timer_create_args_t timer_args = {
        .callback = callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = name,
        .skip_unhandled_events = true,
    };

    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid timer target");

    if (*handle != NULL) {
        return ESP_OK;
    }

    return esp_timer_create(&timer_args, handle);
}

static esp_err_t tirtc_session_create_task_on_core(TaskFunction_t task_entry,
                                                   const char *name,
                                                   uint32_t stack_size,
                                                   UBaseType_t priority,
                                                   TaskHandle_t *handle,
                                                   BaseType_t core_id)
{
    BaseType_t task_ok = pdPASS;

    ESP_RETURN_ON_FALSE(task_entry != NULL && handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid task args");

    if (*handle != NULL) {
        return ESP_OK;
    }

    task_ok = xTaskCreatePinnedToCoreWithCaps(task_entry,
                                              name,
                                              stack_size,
                                              NULL,
                                              priority,
                                              handle,
                                              core_id,
                                              APP_TASK_STACK_CAPS_BACKGROUND);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t tirtc_session_create_task(TaskFunction_t task_entry,
                                           const char *name,
                                           uint32_t stack_size,
                                           UBaseType_t priority,
                                           TaskHandle_t *handle)
{
    return tirtc_session_create_task_on_core(task_entry,
                                             name,
                                             stack_size,
                                             priority,
                                             handle,
                                             APP_TASK_CORE_RTC);
}

#if TIRTC_SESSION_WORKER_STACK_INTERNAL
static esp_err_t tirtc_session_create_internal_task(TaskFunction_t task_entry,
                                                    const char *name,
                                                    uint32_t stack_size,
                                                    UBaseType_t priority,
                                                    TaskHandle_t *handle)
{
    BaseType_t task_ok = pdPASS;

    ESP_RETURN_ON_FALSE(task_entry != NULL && handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid task args");

    if (*handle != NULL) {
        return ESP_OK;
    }

    task_ok = xTaskCreatePinnedToCoreWithCaps(task_entry,
                                              name,
                                              stack_size,
                                              NULL,
                                              priority,
                                              handle,
                                              APP_TASK_CORE_RTC,
                                              APP_TASK_STACK_CAPS_INTERNAL);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
#endif

static esp_err_t tirtc_session_create_event_resources(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_queue(&s_event_queue,
                                                   TIRTC_SESSION_EVENT_QUEUE_LEN,
                                                   sizeof(tirtc_session_event_t),
                                                   APP_QUEUE_CAPS_CONTROL),
                        TAG,
                        "rtc event queue alloc failed");
    return ESP_OK;
}

static esp_err_t tirtc_session_create_sdk_api_lock(void)
{
    if (s_tirtc_api_mutex == NULL) {
        s_tirtc_api_mutex = xSemaphoreCreateRecursiveMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        ESP_RETURN_ON_FALSE(s_tirtc_api_mutex != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "rtc sdk api mutex alloc failed");
    }
    return ESP_OK;
}

static esp_err_t tirtc_session_prealloc_local_video_tx_pool(void)
{
    size_t total_capacity = 0;
    const size_t target_size = TIRTC_SESSION_VIDEO_TX_PREALLOC_BYTES;

    if (target_size == 0U) {
        return ESP_OK;
    }

    for (uint8_t slot = 0; slot < TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE; ++slot) {
        if (s_local_video_tx_buffer_capacities[slot] >= target_size &&
            s_local_video_tx_buffers[slot] != NULL) {
            total_capacity += s_local_video_tx_buffer_capacities[slot];
            continue;
        }

        uint8_t *buffer = tirtc_session_alloc_video_tx_buffer(target_size);
        ESP_RETURN_ON_FALSE(buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "local video tx prealloc failed: slot=%u size=%u psram_free=%u psram_largest=%u internal_largest=%u",
                            (unsigned)slot,
                            (unsigned)target_size,
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        free(s_local_video_tx_buffers[slot]);
        s_local_video_tx_buffers[slot] = buffer;
        s_local_video_tx_buffer_capacities[slot] = target_size;
        total_capacity += target_size;
    }

    ESP_LOGI(TAG,
             "local video tx pool preallocated: slots=%u slot_size=%u total=%u psram_free=%u psram_largest=%u",
             (unsigned)TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE,
             (unsigned)target_size,
             (unsigned)total_capacity,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

static esp_err_t tirtc_session_create_local_video_tx_resources(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_queue(&s_local_video_tx_queue,
                                                   TIRTC_SESSION_VIDEO_TX_QUEUE_LEN,
                                                   sizeof(tirtc_session_local_video_packet_t),
                                                   APP_QUEUE_CAPS_BACKGROUND),
                        TAG,
                        "rtc local video queue alloc failed");

    if (s_local_video_tx_free_queue == NULL) {
        s_local_video_tx_free_queue = xQueueCreateWithCaps(TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE,
                                                           sizeof(uint8_t),
                                                           APP_QUEUE_CAPS_CONTROL);
        ESP_RETURN_ON_FALSE(s_local_video_tx_free_queue != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "rtc local video free queue alloc failed");

        for (uint8_t slot = 0; slot < TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE; ++slot) {
            BaseType_t queued = xQueueSend(s_local_video_tx_free_queue, &slot, 0);
            ESP_RETURN_ON_FALSE(queued == pdTRUE,
                                ESP_ERR_NO_MEM,
                                TAG,
                                "rtc local video free queue fill failed");
        }
    }

    return ESP_OK;
}

static esp_err_t tirtc_session_prealloc_local_audio_tx_pool(void)
{
    size_t total_capacity = 0;

    for (uint8_t slot = 0; slot < TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE; ++slot) {
        if (s_local_audio_tx_buffers[slot] == NULL) {
            s_local_audio_tx_buffers[slot] = app_memory_alloc_psram(TIRTC_SESSION_AUDIO_TX_BUFFER_BYTES);
        }
        if (s_local_audio_tx_buffers[slot] == NULL) {
            for (uint8_t cleanup = 0; cleanup < TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE; ++cleanup) {
                free(s_local_audio_tx_buffers[cleanup]);
                s_local_audio_tx_buffers[cleanup] = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        total_capacity += TIRTC_SESSION_AUDIO_TX_BUFFER_BYTES;
    }

    ESP_LOGI(TAG,
             "local audio tx pool preallocated: slots=%u slot_size=%u total=%u",
             (unsigned)TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE,
             (unsigned)TIRTC_SESSION_AUDIO_TX_BUFFER_BYTES,
             (unsigned)total_capacity);
    return ESP_OK;
}

esp_err_t tirtc_session_prewarm_media_pools(void)
{
    /*
     * These are payload-only pools. Reserve them from PSRAM before network,
     * TLS, UI and business tasks start so media startup never has to compete
     * for multi-megabyte blocks at connection time.
     */
    ESP_RETURN_ON_ERROR(tirtc_session_prealloc_local_video_tx_pool(),
                        TAG,
                        "rtc video PSRAM pool prewarm failed");
    ESP_RETURN_ON_ERROR(tirtc_session_prealloc_local_audio_tx_pool(),
                        TAG,
                        "rtc audio PSRAM pool prewarm failed");
    return ESP_OK;
}

static esp_err_t tirtc_session_create_local_audio_tx_resources(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_queue(&s_local_audio_tx_queue,
                                                   TIRTC_SESSION_AUDIO_TX_QUEUE_LEN,
                                                   sizeof(tirtc_session_local_audio_packet_t),
                                                   APP_QUEUE_CAPS_CONTROL),
                        TAG,
                        "rtc local audio queue alloc failed");

    if (s_local_audio_tx_free_queue == NULL) {
        s_local_audio_tx_free_queue = xQueueCreateWithCaps(TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE,
                                                           sizeof(uint8_t),
                                                           APP_QUEUE_CAPS_CONTROL);
        ESP_RETURN_ON_FALSE(s_local_audio_tx_free_queue != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "rtc local audio free queue alloc failed");

        for (uint8_t slot = 0; slot < TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE; ++slot) {
            ESP_RETURN_ON_FALSE(xQueueSend(s_local_audio_tx_free_queue, &slot, 0) == pdTRUE,
                                ESP_ERR_NO_MEM,
                                TAG,
                                "rtc local audio free queue fill failed");
        }
    }
    return ESP_OK;
}

static esp_err_t tirtc_session_create_timers(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_time_once",
                                                  tirtc_session_time_message_initial_timer_cb,
                                                  &s_time_message_initial_timer),
                        TAG,
                        "rtc time initial timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_time_periodic",
                                                  tirtc_session_time_message_periodic_timer_cb,
                                                  &s_time_message_periodic_timer),
                        TAG,
                        "rtc time periodic timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_media_boot",
                                                   tirtc_session_media_bootstrap_timer_cb,
                                                   &s_media_bootstrap_timer),
                        TAG,
                        "rtc media bootstrap timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer(
                            "rtc_video_first",
                            tirtc_session_remote_video_first_packet_timer_cb,
                            &s_remote_video_first_packet_timer),
                        TAG,
                        "rtc remote video first-packet timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_disc_watch",
                                                  tirtc_session_disconnect_watchdog_timer_cb,
                                                  &s_disconnect_watchdog_timer),
                        TAG,
                        "rtc disconnect watchdog timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_full_reset",
                                                  tirtc_session_deferred_full_reset_timer_cb,
                                                  &s_deferred_full_reset_timer),
                        TAG,
                        "rtc deferred full reset timer create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timer("rtc_full_reset_start",
                                                  tirtc_session_deferred_start_after_full_reset_timer_cb,
                                                  &s_deferred_start_after_full_reset_timer),
                        TAG,
                        "rtc deferred full reset start timer create failed");
    return ESP_OK;
}

static esp_err_t tirtc_session_create_tasks(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_task_on_core(
                            tirtc_session_local_video_tx_task,
                            "rtc_video_tx",
                            TIRTC_SESSION_VIDEO_TX_TASK_STACK,
                            TIRTC_SESSION_VIDEO_TX_TASK_PRIORITY,
                            &s_local_video_tx_task,
                            APP_TASK_CORE_RTC_VIDEO_TX),
                        TAG,
                        "rtc local video task create failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_task(tirtc_session_local_audio_tx_task,
                                                 "rtc_audio_tx",
                                                 TIRTC_SESSION_AUDIO_TX_TASK_STACK,
                                                 TIRTC_SESSION_AUDIO_TX_TASK_PRIORITY,
                                                 &s_local_audio_tx_task),
                        TAG,
                        "rtc local audio task create failed");
#if TIRTC_SESSION_WORKER_STACK_INTERNAL
    ESP_RETURN_ON_ERROR(tirtc_session_create_internal_task(tirtc_session_worker_task,
                                                          "rtc_worker",
                                                          TIRTC_SESSION_WORKER_TASK_STACK,
                                                          TIRTC_SESSION_WORKER_TASK_PRIORITY,
                                                          &s_worker_task),
                        TAG,
                        "rtc worker task create failed");
#else
    ESP_RETURN_ON_ERROR(tirtc_session_create_task(tirtc_session_worker_task,
                                                 "rtc_worker",
                                                 TIRTC_SESSION_WORKER_TASK_STACK,
                                                 TIRTC_SESSION_WORKER_TASK_PRIORITY,
                                                 &s_worker_task),
                        TAG,
                        "rtc worker task create failed");
#endif
    return ESP_OK;
}

static esp_err_t tirtc_session_create_runtime_resources(void)
{
    ESP_RETURN_ON_ERROR(tirtc_session_create_event_resources(), TAG, "rtc event resources init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_sdk_api_lock(), TAG, "rtc sdk api lock init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_prewarm_media_pools(),
                        TAG,
                        "rtc PSRAM media pools init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_local_video_tx_resources(),
                        TAG,
                        "rtc local video resources init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_local_audio_tx_resources(),
                        TAG,
                        "rtc local audio resources init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_timers(), TAG, "rtc timers init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_media_init(), TAG, "rtc media bridge init failed");
    ESP_RETURN_ON_ERROR(tirtc_session_create_tasks(), TAG, "rtc worker init failed");
    return ESP_OK;
}

bool tirtc_session_take_sdk_api_lock(TickType_t wait_ticks)
{
    return s_tirtc_api_mutex != NULL && xSemaphoreTakeRecursive(s_tirtc_api_mutex, wait_ticks) == pdTRUE;
}

void tirtc_session_give_sdk_api_lock(void)
{
    if (s_tirtc_api_mutex != NULL) {
        xSemaphoreGiveRecursive(s_tirtc_api_mutex);
    }
}

static int tirtc_session_disconnect_with_sdk_lock(tirtc_conn_t conn)
{
    int ret = TIRTC_E_BUSY;

    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        ret = TiRtcDisconnect(conn);
        tirtc_session_give_sdk_api_lock();
    } else {
        ESP_LOGW(TAG, "rtc sdk api lock unavailable for disconnect");
    }

    return ret;
}

static void tirtc_session_configure_runtime_callbacks(void)
{
    tirtc_session_media_set_capture_cb(tirtc_session_local_audio_cb, NULL);
}

static void tirtc_session_get_runtime_snapshot(tirtc_session_runtime_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    taskENTER_CRITICAL(&s_rtc_lock);
    snapshot->enabled = s_config.enabled;
    snapshot->sdk_started = s_sdk_started;
    snapshot->sdk_initialized = s_sdk_initialized;
    snapshot->start_in_progress = s_start_in_progress;
    snapshot->stop_in_progress = s_stop_in_progress;
    snapshot->active_conn = s_active_conn;
    snapshot->closing_conn = s_closing_conn;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

bool tirtc_session_try_get_active_conn(tirtc_conn_t *conn)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL;
    if (ready && conn != NULL) {
        *conn = s_active_conn;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

bool tirtc_session_is_connection_usable(tirtc_conn_t conn)
{
    bool usable = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    usable = conn != NULL && conn == s_active_conn && s_sdk_started && !s_start_in_progress &&
             !s_stop_in_progress && s_closing_conn == NULL;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return usable;
}

static bool tirtc_session_is_ready_for_new_connection_locked(void)
{
    return s_network_connected && s_sdk_initialized && s_sdk_started &&
           !s_sdk_prepare_in_progress && !s_start_in_progress &&
           !s_stop_in_progress && s_whip_connect_active_attempt == 0U &&
           s_active_conn == NULL &&
           s_closing_conn == NULL;
}

static uint32_t tirtc_session_begin_whip_connect_attempt(tirtc_conn_t *active_conn,
                                                         tirtc_conn_t *closing_conn)
{
    uint32_t attempt_id = 0U;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (tirtc_session_is_ready_for_new_connection_locked()) {
        s_whip_connect_attempt_counter++;
        if (s_whip_connect_attempt_counter == 0U) {
            s_whip_connect_attempt_counter++;
        }
        attempt_id = s_whip_connect_attempt_counter;
        s_whip_connect_active_attempt = attempt_id;
    }
    if (active_conn != NULL) {
        *active_conn = s_active_conn;
    }
    if (closing_conn != NULL) {
        *closing_conn = s_closing_conn;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return attempt_id;
}

static void tirtc_session_finish_whip_connect_attempt(uint32_t attempt_id)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (attempt_id != 0U && s_whip_connect_active_attempt == attempt_id) {
        s_whip_connect_active_attempt = 0U;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

bool tirtc_session_is_ready_for_new_connection(void)
{
    bool ready = false;

    if (!s_initialized) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = tirtc_session_is_ready_for_new_connection_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_connection_tracked(tirtc_conn_t conn)
{
    bool tracked = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    tracked = conn != NULL && (conn == s_active_conn || conn == s_closing_conn);
    taskEXIT_CRITICAL(&s_rtc_lock);
    return tracked;
}

static void tirtc_session_bind_connection_user_data(tirtc_conn_t conn)
{
    uint32_t generation = 0;
    uint64_t accepted_at_us = 0;

    if (conn == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn) {
        s_active_conn_user_data.magic = TIRTC_SESSION_CONN_USER_MAGIC;
        s_active_conn_user_data.generation++;
        s_active_conn_user_data.accepted_at_us = s_active_conn_accepted_at_us;
        generation = s_active_conn_user_data.generation;
        accepted_at_us = s_active_conn_user_data.accepted_at_us;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (generation == 0U) {
        return;
    }

    int ret = TiRtcConnSetUserData(conn, &s_active_conn_user_data);
    if (ret < 0) {
        tirtc_session_set_last_error(ret);
        ESP_LOGW(TAG, "bind rtc connection user data failed: %s", TiRtcGetErrorStr(ret));
        return;
    }

    ESP_LOGD(TAG,
             "rtc connection user data bound: hconn=%p gen=%lu accepted_at_us=%llu",
             conn,
             (unsigned long)generation,
             (unsigned long long)accepted_at_us);
}

static void tirtc_session_log_connection_user_data(const char *phase, tirtc_conn_t conn)
{
#if !CONFIG_APP_VERBOSE_RUNTIME_LOGS
    (void)phase;
    (void)conn;
    return;
#else
    const char *safe_phase = phase != NULL ? phase : "connection";
    void *user_data = NULL;

    if (conn == NULL) {
        return;
    }

    user_data = TiRtcConnGetUserData(conn);
    if (user_data == NULL) {
        ESP_LOGD(TAG, "rtc %s: hconn=%p user_data=NULL", safe_phase, conn);
        return;
    }

    const tirtc_session_conn_user_data_t *conn_data = (const tirtc_session_conn_user_data_t *)user_data;
    if (conn_data->magic != TIRTC_SESSION_CONN_USER_MAGIC) {
        ESP_LOGD(TAG, "rtc %s: hconn=%p user_data=%p", safe_phase, conn, user_data);
        return;
    }

    uint64_t now_us = esp_timer_get_time();
    uint64_t age_ms = now_us >= conn_data->accepted_at_us
                          ? (now_us - conn_data->accepted_at_us) / 1000ULL
                          : 0ULL;
    APP_LOG_DETAIL(TAG,
                   "rtc %s: hconn=%p gen=%lu age_ms=%llu",
                   safe_phase,
                   conn,
                   (unsigned long)conn_data->generation,
                   (unsigned long long)age_ms);
#endif
}

static size_t tirtc_session_cached_send_buffer_used(void)
{
    size_t used = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    used = s_stats.send_buffer_used;
    taskEXIT_CRITICAL(&s_rtc_lock);

    return used;
}

static bool tirtc_session_query_send_buffer_used(tirtc_conn_t conn, size_t *used_out)
{
    size_t used = tirtc_session_cached_send_buffer_used();
    uint32_t query_conn_generation = 0U;

    if (used_out == NULL) {
        return false;
    }

#if CONFIG_APP_TIRTC_QUERY_SEND_BUFFER_USED
    if (conn != NULL && tirtc_session_is_connection_usable(conn)) {
        const TickType_t now_tick = xTaskGetTickCount();
        bool query_due = false;

        taskENTER_CRITICAL(&s_rtc_lock);
        if (s_last_send_buffer_query_conn != conn ||
            s_last_send_buffer_query_tick == 0 ||
            now_tick - s_last_send_buffer_query_tick >=
                pdMS_TO_TICKS(TIRTC_SESSION_SEND_BUFFER_QUERY_PERIOD_MS)) {
            s_last_send_buffer_query_conn = conn;
            s_last_send_buffer_query_tick = now_tick;
            query_conn_generation = s_active_conn_user_data.generation;
            query_due = true;
        }
        taskEXIT_CRITICAL(&s_rtc_lock);

        if (query_due) {
            used = TiRtcGetSendBufferUsed(conn);
            bool query_current = false;
            taskENTER_CRITICAL(&s_rtc_lock);
            /* A query runs without s_rtc_lock because the SDK may block. A
             * disconnect/reconnect can therefore supersede it while it is in
             * flight. Never publish an old connection's pressure into the
             * new call, where it could suppress the first media frames. */
            if (s_last_send_buffer_query_conn == conn &&
                s_active_conn == conn &&
                s_active_conn_user_data.generation == query_conn_generation) {
                s_stats.send_buffer_used = used;
                query_current = true;
            } else {
                used = s_stats.send_buffer_used;
            }
            taskEXIT_CRITICAL(&s_rtc_lock);
            *used_out = used;
            return query_current;
        }
    }
#else
    (void)conn;
#endif

    *used_out = used;
    return false;
}

static void tirtc_session_refresh_send_buffer_used(void)
{
#if CONFIG_APP_TIRTC_QUERY_SEND_BUFFER_USED
    tirtc_conn_t conn = NULL;
    size_t used = 0U;

    /*
     * TiRtcGetSendBufferUsed() is a connection-management API; unlike the
     * media-send APIs, its public contract does not promise concurrent use.
     * Keep it on the low-priority worker and take the common SDK lock without
     * waiting. A busy media path therefore wins, while the query can never race
     * a send or connection teardown.
     */
    if (tirtc_session_try_get_active_conn(&conn) &&
        tirtc_session_take_sdk_api_lock(0)) {
        (void)tirtc_session_query_send_buffer_used(conn, &used);
        tirtc_session_give_sdk_api_lock();
    }
#endif
}

static uint64_t tirtc_session_event_age_ms(uint64_t now_us, uint64_t event_us)
{
    return event_us != 0U && now_us > event_us ? (now_us - event_us) / 1000ULL : 0ULL;
}

static void tirtc_session_monitor_local_video_tx_liveness(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    bool expected = false;
    bool capture_enabled = false;
    bool stalled = false;
    bool log_stall = false;
    bool log_recovery = false;
    uint64_t last_enqueue_us = 0U;
    uint64_t last_dequeue_us = 0U;
    uint64_t last_attempt_us = 0U;
    uint64_t last_success_us = 0U;
    uint64_t stall_started_us = 0U;
    uint32_t tx_frames = 0U;
    uint32_t tx_failures = 0U;
    size_t send_buffer_used = 0U;

    taskENTER_CRITICAL(&s_rtc_lock);
    expected = s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
               s_closing_conn == NULL && s_active_conn != NULL && s_call_active &&
               s_local_video_send_enabled &&
               (s_peer_wants_video || s_local_video_publish_forced) &&
               s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    capture_enabled = s_builtin_video_capture_enabled || s_external_video_active;
    last_enqueue_us = s_local_video_last_enqueue_us;
    last_dequeue_us = s_local_video_last_dequeue_us;
    last_attempt_us = s_local_video_last_send_attempt_us;
    last_success_us = s_local_video_last_send_success_us;
    tx_frames = s_stats.tx_video_frames;
    tx_failures = s_stats.tx_failures;
    send_buffer_used = s_stats.send_buffer_used;

    if (!expected || last_success_us == 0U) {
        s_local_video_tx_stalled = false;
        s_local_video_tx_stall_started_us = 0U;
        s_local_video_tx_last_stall_log_us = 0U;
    } else if (now_us - last_success_us >= TIRTC_SESSION_VIDEO_TX_LIVENESS_TIMEOUT_US) {
        if (!s_local_video_tx_stalled) {
            s_local_video_tx_stalled = true;
            s_local_video_tx_stall_started_us = last_success_us;
            log_stall = true;
        } else if (s_local_video_tx_last_stall_log_us == 0U ||
                   now_us - s_local_video_tx_last_stall_log_us >=
                       TIRTC_SESSION_VIDEO_TX_LIVENESS_LOG_INTERVAL_US) {
            log_stall = true;
        }
        if (log_stall) {
            s_local_video_tx_last_stall_log_us = now_us;
        }
    } else if (s_local_video_tx_stalled) {
        stalled = true;
        stall_started_us = s_local_video_tx_stall_started_us;
        s_local_video_tx_stalled = false;
        s_local_video_tx_stall_started_us = 0U;
        s_local_video_tx_last_stall_log_us = 0U;
        log_recovery = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_stall) {
        const uint32_t queue_len = s_local_video_tx_queue != NULL ?
                                   (uint32_t)uxQueueMessagesWaiting(s_local_video_tx_queue) : 0U;
        const uint32_t free_slots = s_local_video_tx_free_queue != NULL ?
                                    (uint32_t)uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0U;
        const uint64_t enqueue_age_ms = tirtc_session_event_age_ms(now_us, last_enqueue_us);
        const uint64_t dequeue_age_ms = tirtc_session_event_age_ms(now_us, last_dequeue_us);
        const uint64_t attempt_age_ms = tirtc_session_event_age_ms(now_us, last_attempt_us);
        const uint64_t success_age_ms = tirtc_session_event_age_ms(now_us, last_success_us);
        const char *stage = "sdk_send";

        if (last_attempt_us != 0U &&
            last_attempt_us > last_success_us &&
            last_attempt_us >= last_dequeue_us &&
            now_us - last_attempt_us >= TIRTC_SESSION_VIDEO_TX_LIVENESS_TIMEOUT_US) {
            stage = "sdk_call";
        } else if (queue_len > 0U &&
            (last_dequeue_us == 0U ||
             now_us - last_dequeue_us >= TIRTC_SESSION_VIDEO_TX_LIVENESS_TIMEOUT_US)) {
            stage = "tx_task";
        } else if (last_enqueue_us == 0U ||
                   now_us - last_enqueue_us >= TIRTC_SESSION_VIDEO_TX_LIVENESS_TIMEOUT_US) {
            stage = "source";
        } else if (last_attempt_us == 0U ||
                   now_us - last_attempt_us >= TIRTC_SESSION_VIDEO_TX_LIVENESS_TIMEOUT_US) {
            stage = "sdk_lock";
        }

        ESP_LOGW(TAG,
                 "video tx liveness: stage=%s age_ms=enq:%llu,deq:%llu,api:%llu,ok:%llu q=%lu free=%lu capture=%d frames=%lu fail=%lu sdk_buf=%u",
                 stage,
                 (unsigned long long)enqueue_age_ms,
                 (unsigned long long)dequeue_age_ms,
                 (unsigned long long)attempt_age_ms,
                 (unsigned long long)success_age_ms,
                 (unsigned long)queue_len,
                 (unsigned long)free_slots,
                 capture_enabled ? 1 : 0,
                 (unsigned long)tx_frames,
                 (unsigned long)tx_failures,
                 (unsigned)send_buffer_used);
    } else if (log_recovery && stalled) {
        ESP_LOGI(TAG,
                 "video tx resumed: stalled_ms=%llu frames=%lu",
                 (unsigned long long)tirtc_session_event_age_ms(now_us, stall_started_us),
                 (unsigned long)tx_frames);
    }
}

static void tirtc_session_monitor_remote_video_rx_liveness(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    tirtc_conn_t conn = NULL;
    bool expected = false;
    bool packet_stale = false;
    bool submit_stale = false;
    bool log_stall = false;
    bool log_recovery = false;
    bool repair_subscription = false;
    bool request_key_frame = false;
    bool callback_seen = false;
    uint64_t first_request_us = 0U;
    uint64_t first_submit_attempt_us = 0U;
    uint64_t last_packet_us = 0U;
    uint64_t last_submit_us = 0U;
    uint32_t callback_frames = 0U;
    uint32_t submit_failures = 0U;

    taskENTER_CRITICAL(&s_rtc_lock);
    expected = s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
               s_closing_conn == NULL && s_active_conn != NULL && s_call_active &&
               s_remote_video_requested &&
               tirtc_session_media_profile_allows_remote_video_locked();
    conn = s_active_conn;
    first_request_us = s_remote_video_first_request_at_us;
    first_submit_attempt_us = s_remote_video_first_submit_attempt_us;
    last_packet_us = s_remote_video_last_packet_us;
    last_submit_us = s_remote_video_last_submit_us;
    callback_frames = s_remote_video_callback_frames;
    submit_failures = s_remote_video_submit_failures;
    callback_seen = callback_frames > 0U && last_packet_us != 0U;

    if (!expected) {
        s_remote_video_rx_stalled = false;
        s_remote_video_last_liveness_log_us = 0U;
    } else {
        if (!callback_seen) {
            packet_stale = first_request_us != 0U && now_us >= first_request_us &&
                           now_us - first_request_us >=
                                TIRTC_SESSION_VIDEO_RX_LIVENESS_TIMEOUT_US;
        } else {
            packet_stale = now_us >= last_packet_us &&
                           now_us - last_packet_us >=
                               TIRTC_SESSION_VIDEO_RX_LIVENESS_TIMEOUT_US;
        }
        submit_stale = callback_seen && !packet_stale &&
                       ((last_submit_us == 0U && first_submit_attempt_us != 0U &&
                         now_us >= first_submit_attempt_us &&
                         now_us - first_submit_attempt_us >=
                             TIRTC_SESSION_VIDEO_RX_LIVENESS_TIMEOUT_US) ||
                        (last_submit_us != 0U && now_us >= last_submit_us &&
                         now_us - last_submit_us >=
                             TIRTC_SESSION_VIDEO_RX_LIVENESS_TIMEOUT_US));
        if (packet_stale || submit_stale) {
            if (!s_remote_video_rx_stalled) {
                s_remote_video_rx_stalled = true;
                log_stall = true;
            } else if (s_remote_video_last_liveness_log_us == 0U ||
                       now_us - s_remote_video_last_liveness_log_us >=
                           TIRTC_SESSION_VIDEO_RX_LIVENESS_LOG_INTERVAL_US) {
                log_stall = true;
            }
            if (log_stall) {
                s_remote_video_last_liveness_log_us = now_us;
            }
            if (s_remote_video_last_recovery_us == 0U ||
                now_us - s_remote_video_last_recovery_us >=
                    TIRTC_SESSION_VIDEO_RX_RECOVERY_INTERVAL_US) {
                s_remote_video_last_recovery_us = now_us;
                repair_subscription = packet_stale;
                request_key_frame = submit_stale;
            }
        } else if (s_remote_video_rx_stalled) {
            s_remote_video_rx_stalled = false;
            s_remote_video_last_liveness_log_us = 0U;
            log_recovery = true;
        }
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_stall) {
        const char *stage = !callback_seen ? "startup" :
                            (packet_stale ? "transport" : "renderer");
        const uint64_t packet_reference_us = last_packet_us != 0U ?
                                             last_packet_us : first_request_us;
        const uint64_t submit_reference_us = last_submit_us != 0U ?
                                             last_submit_us : first_submit_attempt_us;
        ESP_LOGW(TAG,
                 "VRX stall stage=%s age=%llu/%llums cb=%lu ok=%lu fail=%lu repair=%d",
                 stage,
                 (unsigned long long)tirtc_session_event_age_ms(now_us, packet_reference_us),
                 (unsigned long long)tirtc_session_event_age_ms(now_us, submit_reference_us),
                 (unsigned long)callback_frames,
                 (unsigned long)(callback_frames - submit_failures),
                 (unsigned long)submit_failures,
                 (repair_subscription || request_key_frame) ? 1 : 0);
    } else if (log_recovery) {
        ESP_LOGI(TAG,
                 "VRX resumed cb=%lu ok=%lu",
                 (unsigned long)callback_frames,
                 (unsigned long)(callback_frames - submit_failures));
    }

    if (repair_subscription && conn != NULL) {
        /* Reassert the subscription and application-level video request on the
         * existing connection. This also requests a fresh H264 key frame, but
         * deliberately leaves connection ownership and audio untouched. */
        (void)tirtc_session_request_remote_video(conn);
    } else if (request_key_frame && conn != NULL &&
               tirtc_session_take_remote_key_frame_retry_slot()) {
        (void)tirtc_session_request_remote_key_frame(
            conn,
            TIRTC_SESSION_REMOTE_VIDEO_STREAM_ID,
            "remote renderer liveness");
    }
}

static void tirtc_session_log_connection_close_snapshot(const char *phase,
                                                        tirtc_conn_t conn,
                                                        int error)
{
#if !CONFIG_APP_VERBOSE_RUNTIME_LOGS
    (void)phase;
    (void)conn;
    if (error == TIRTC_E_CONN_REMOTECLOSE) {
        ESP_LOGI(TAG, "rtc connection closed: reason=remote_close code=%d", error);
    } else {
        ESP_LOGW(TAG, "rtc connection closed: reason=sdk_error code=%d", error);
    }
    return;
#else
    const char *safe_phase = phase != NULL ? phase : "close";
    tirtc_session_stats_t stats = {0};
    bool active_match = false;
    bool closing_match = false;
    bool peer_wants_video = false;
    bool peer_wants_audio = false;
    bool peer_video_control_seen = false;
    bool peer_audio_control_seen = false;
    bool local_video_publish_forced = false;
    bool local_audio_publish_forced = false;
    bool start_in_progress = false;
    bool stop_in_progress = false;
    uint64_t accepted_at_us = 0;
    uint64_t age_ms = 0;
    size_t send_buffer_used = 0;

    tirtc_session_get_stats(&stats);

    taskENTER_CRITICAL(&s_rtc_lock);
    active_match = conn != NULL && conn == s_active_conn;
    closing_match = conn != NULL && conn == s_closing_conn;
    accepted_at_us = active_match ? s_active_conn_accepted_at_us : 0U;
    peer_wants_video = s_peer_wants_video;
    peer_wants_audio = s_peer_wants_audio;
    peer_video_control_seen = s_peer_video_control_seen;
    peer_audio_control_seen = s_peer_audio_control_seen;
    local_video_publish_forced = s_local_video_publish_forced;
    local_audio_publish_forced = s_local_audio_publish_forced;
    start_in_progress = s_start_in_progress;
    stop_in_progress = s_stop_in_progress;
    taskEXIT_CRITICAL(&s_rtc_lock);

    send_buffer_used = tirtc_session_cached_send_buffer_used();
    if (accepted_at_us != 0U) {
        uint64_t now_us = esp_timer_get_time();
        age_ms = now_us >= accepted_at_us ? (now_us - accepted_at_us) / 1000ULL : 0ULL;
    }

    bool remote_close_info = error == TIRTC_E_CONN_REMOTECLOSE &&
                             (age_ms >= TIRTC_SESSION_REMOTE_CLOSE_WARN_AGE_MS ||
                              closing_match ||
                              !active_match);

    ESP_LOG_LEVEL_LOCAL(remote_close_info ? ESP_LOG_INFO : ESP_LOG_WARN,
                        TAG,
                        "rtc close snapshot: phase=%s hconn=%p err=%s(%d) active=%d closing=%d age_ms=%llu "
                        "state=%u sdk=%d start=%d stop=%d call=%d last_event=%s last_error=%s(%d) "
                        "send[v=%d a=%d forced_v=%d forced_a=%d] peer[v=%d/%d a=%d/%d] stream[v=%u a=%u] "
                        "tx[attempt=%lu fail=%lu v=%lu/%uKB a=%lu/%uKB] rx[v=%lu a=%lu] "
                        "q[v=%u free=%u a=%u] sdk_buf=%u/%u heap[int_largest=%u dma_largest=%u psram_largest=%u]",
                        safe_phase,
                        conn,
                        error != 0 ? TiRtcGetErrorStr(error) : "OK",
                        error,
                        active_match ? 1 : 0,
                        closing_match ? 1 : 0,
                        (unsigned long long)age_ms,
                        (unsigned)stats.state,
                        stats.sdk_started ? 1 : 0,
                        start_in_progress ? 1 : 0,
                        stop_in_progress ? 1 : 0,
                        stats.call_active ? 1 : 0,
                        stats.last_event,
                        stats.last_error != 0 ? TiRtcGetErrorStr(stats.last_error) : "OK",
                        stats.last_error,
                        stats.local_video_send_enabled ? 1 : 0,
                        stats.local_audio_send_enabled ? 1 : 0,
                        local_video_publish_forced ? 1 : 0,
                        local_audio_publish_forced ? 1 : 0,
                        peer_wants_video ? 1 : 0,
                        peer_video_control_seen ? 1 : 0,
                        peer_wants_audio ? 1 : 0,
                        peer_audio_control_seen ? 1 : 0,
                        (unsigned)stats.local_video_stream_id,
                        (unsigned)stats.local_audio_stream_id,
                        (unsigned long)stats.tx_attempts,
                        (unsigned long)stats.tx_failures,
                        (unsigned long)stats.tx_video_frames,
                        (unsigned)(stats.tx_video_bytes / 1024U),
                        (unsigned long)stats.tx_audio_frames,
                        (unsigned)(stats.tx_audio_bytes / 1024U),
                        (unsigned long)stats.rx_video_frames,
                        (unsigned long)stats.rx_audio_frames,
                        (unsigned)stats.local_video_tx_queue_len,
                        (unsigned)stats.local_video_tx_free_slots,
                        (unsigned)stats.local_audio_tx_queue_len,
                        (unsigned)send_buffer_used,
                        (unsigned)TIRTC_SESSION_MAX_SEND_BUFFER,
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
}

static bool tirtc_session_should_log_media_send_error(void)
{
    TickType_t now_tick = xTaskGetTickCount();
    bool should_log = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_last_media_send_error_log_tick == 0 ||
        now_tick - s_last_media_send_error_log_tick >=
            pdMS_TO_TICKS(TIRTC_SESSION_MEDIA_SEND_ERROR_LOG_INTERVAL_MS)) {
        s_last_media_send_error_log_tick = now_tick;
        should_log = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    return should_log;
}

static void tirtc_session_note_transient_send_error(const char *media_name,
                                                    tirtc_conn_t conn,
                                                    uint8_t stream_id,
                                                    uint32_t payload_len,
                                                    int error)
{
    const char *safe_media_name = media_name != NULL ? media_name : "media";
    size_t send_buffer_used = 0;

    tirtc_session_note_event("media tx transient");
    media_governor_note_network_backpressure();

    if (!tirtc_session_should_log_media_send_error()) {
        return;
    }

    send_buffer_used = tirtc_session_cached_send_buffer_used();

    ESP_LOGW(TAG,
             "%s send transient error: err=%s(%d) stream=%u payload=%u conn=%p q=%u free=%u sdk_buf=%u/%u",
             safe_media_name,
             TiRtcGetErrorStr(error),
             error,
             (unsigned)stream_id,
             (unsigned)payload_len,
             conn,
             (unsigned)(s_local_video_tx_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_queue) : 0),
             (unsigned)(s_local_video_tx_free_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0),
             (unsigned)send_buffer_used,
             (unsigned)TIRTC_SESSION_MAX_SEND_BUFFER);
}

static bool tirtc_session_check_send_buffer(tirtc_conn_t conn,
                                            tirtc_session_send_media_t media,
                                            const char *media_name,
                                            bool can_drop)
{
    size_t used = 0;
    size_t warn_level = 0;
    size_t video_throttle_level = 0;
    size_t drop_level = 0;
    bool should_log = false;
    TickType_t now_tick = 0;
    const char *safe_media_name = media_name != NULL ? media_name : "media";

    if (conn == NULL) {
        return false;
    }

    if (TIRTC_SESSION_MAX_SEND_BUFFER == 0U) {
        return true;
    }

    /* Refreshed by the low-priority RTC worker. A sampled value still leaves
     * far more headroom than the guard's throttle-to-drop margin, while keeping
     * lower-layer queue traversal out of the audio/video realtime paths. */
    used = tirtc_session_cached_send_buffer_used();
    warn_level = (size_t)(((uint64_t)TIRTC_SESSION_MAX_SEND_BUFFER * TIRTC_SESSION_SEND_BUFFER_WARN_PCT) / 100ULL);
    video_throttle_level =
        (size_t)(((uint64_t)TIRTC_SESSION_MAX_SEND_BUFFER * TIRTC_SESSION_SEND_BUFFER_VIDEO_THROTTLE_PCT) / 100ULL);
    drop_level = (size_t)(((uint64_t)TIRTC_SESSION_MAX_SEND_BUFFER * TIRTC_SESSION_SEND_BUFFER_DROP_PCT) / 100ULL);

    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.send_buffer_used = used;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (used >= warn_level && warn_level > 0U) {
        now_tick = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_rtc_lock);
        if (s_last_send_buffer_log_tick == 0 ||
            now_tick - s_last_send_buffer_log_tick >= pdMS_TO_TICKS(TIRTC_SESSION_SEND_BUFFER_LOG_PERIOD_MS)) {
            s_last_send_buffer_log_tick = now_tick;
            should_log = true;
        }
        taskEXIT_CRITICAL(&s_rtc_lock);

        if (should_log) {
            ESP_LOGW(TAG,
                     "rtc send buffer high: checked_by=%s used=%u limit=%lu warn=%u video_throttle=%u drop=%u queue=%u free=%u",
                     safe_media_name,
                     (unsigned)used,
                     (unsigned long)TIRTC_SESSION_MAX_SEND_BUFFER,
                     (unsigned)warn_level,
                     (unsigned)video_throttle_level,
                     (unsigned)drop_level,
                     (unsigned)(s_local_video_tx_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_queue) : 0),
                     (unsigned)(s_local_video_tx_free_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0));
        }
    }

    /*
     * The SDK owns connection liveness. This guard only bounds media latency:
     * drop delta video first, preserve a recovery key frame, then stop all
     * media at the hard waterline until the transport drains.
     */
    bool throttle_video = media == TIRTC_SESSION_SEND_MEDIA_VIDEO &&
                          can_drop &&
                          used >= video_throttle_level &&
                          video_throttle_level > 0U;
    bool hard_drop = used >= drop_level && drop_level > 0U;
    if (throttle_video || hard_drop) {
        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_failures++;
        tirtc_session_set_last_event_locked(media == TIRTC_SESSION_SEND_MEDIA_VIDEO ?
                                             "video backpressure" :
                                             "audio backpressure");
        taskEXIT_CRITICAL(&s_rtc_lock);
        media_governor_note_network_backpressure();
        ESP_LOGD(TAG,
                 "rtc %s frame throttled by send buffer guard: send_buffer=%u/%lu throttle=%u drop=%u",
                 safe_media_name,
                 (unsigned)used,
                 (unsigned long)TIRTC_SESSION_MAX_SEND_BUFFER,
                 (unsigned)video_throttle_level,
                 (unsigned)drop_level);
        return false;
    }

    return true;
}

static bool tirtc_session_should_log_local_video_tx_issue(void)
{
    bool should_log = false;
    TickType_t now_tick = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_last_local_video_tx_issue_log_tick == 0 ||
        now_tick - s_last_local_video_tx_issue_log_tick >=
            pdMS_TO_TICKS(TIRTC_SESSION_VIDEO_TX_ISSUE_LOG_PERIOD_MS)) {
        s_last_local_video_tx_issue_log_tick = now_tick;
        should_log = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    return should_log;
}

static esp_err_t tirtc_session_request_remote_key_frame(tirtc_conn_t conn, uint8_t stream_id, const char *reason)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcRequestKeyFrame(conn, stream_id);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret >= 0) {
        tirtc_session_note_event("key frame req");
        ESP_LOGD(TAG,
                 "remote key frame requested: stream=%u reason=%s",
                 (unsigned)stream_id,
                 reason != NULL ? reason : "unspecified");
        return ESP_OK;
    }

    if (ret == TIRTC_E_INVALID_HANDLE &&
        tirtc_session_should_retry_media_request_after_invalid_handle(conn, "request remote key frame")) {
        return ESP_ERR_INVALID_STATE;
    }

    tirtc_session_set_last_error(ret);
    if (ret == TIRTC_E_INVALID_HANDLE) {
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        ESP_LOGW(TAG, "request remote key frame failed: %s", TiRtcGetErrorStr(ret));
    }
    return ESP_FAIL;
}

static bool tirtc_session_take_remote_key_frame_retry_slot(void)
{
    TickType_t now = xTaskGetTickCount();
    bool allow = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_last_remote_key_frame_request_tick == 0 ||
        now - s_last_remote_key_frame_request_tick >=
            pdMS_TO_TICKS(TIRTC_SESSION_REMOTE_KEY_FRAME_RETRY_MS)) {
        s_last_remote_key_frame_request_tick = now;
        allow = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return allow;
}

static void tirtc_session_cancel_media_bootstrap(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_media_bootstrap_pending = false;
    s_remote_video_first_packet_retry_armed = false;
    s_remote_video_first_packet_retry_due = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (s_media_bootstrap_timer != NULL) {
        (void)esp_timer_stop(s_media_bootstrap_timer);
    }
    if (s_remote_video_first_packet_timer != NULL) {
        (void)esp_timer_stop(s_remote_video_first_packet_timer);
    }
}

static void tirtc_session_stop_disconnect_watchdog(void)
{
    if (s_disconnect_watchdog_timer != NULL) {
        (void)esp_timer_stop(s_disconnect_watchdog_timer);
    }
}

static void tirtc_session_reset_call_state_locked(void)
{
    s_call_active = false;
    s_incoming_call_pending = false;
    s_pending_call_cmdw = 0;
    s_remote_video_requested = false;
    s_remote_audio_requested = false;
    s_remote_video_receive_enabled = false;
    s_media_bootstrap_pending = false;
    s_remote_video_first_request_at_us = 0U;
    s_remote_video_first_submit_attempt_us = 0U;
    s_remote_video_request_attempts = 0U;
    s_remote_video_first_packet_retry_armed = false;
    s_remote_video_first_packet_retry_due = false;
    s_remote_video_first_packet_logged = false;
    s_remote_video_last_packet_us = 0U;
    s_remote_video_last_submit_us = 0U;
    s_remote_video_last_recovery_us = 0U;
    s_remote_video_last_liveness_log_us = 0U;
    s_remote_video_callback_frames = 0U;
    s_remote_video_submit_failures = 0U;
    s_remote_video_callback_gap_window_max_us = 0U;
    s_remote_video_rx_stalled = false;
    s_remote_audio_first_packet_logged = false;
    s_remote_message_first_packet_logged = false;
    s_local_video_first_packet_logged = false;
    s_local_h264_key_frame_queued = false;
    s_local_h264_key_frame_published = false;
    s_local_h264_recovery_pending = false;
    s_local_h264_recovery_count = 0;
    s_local_video_last_enqueue_us = 0U;
    s_local_video_last_dequeue_us = 0U;
    s_local_video_last_send_attempt_us = 0U;
    s_local_video_last_send_success_us = 0U;
    s_local_video_tx_stall_started_us = 0U;
    s_local_video_tx_last_stall_log_us = 0U;
    s_local_video_tx_stalled = false;
    s_external_video_conn = NULL;
    s_external_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_external_video_active = false;
    s_last_remote_audio_rx_log_tick = 0;
    s_last_remote_message_rx_log_tick = 0;
    s_last_remote_key_frame_request_tick = 0;
    s_remote_audio_rx_window_frames = 0;
    s_remote_audio_rx_window_payload_bytes = 0;
    s_remote_audio_rx_window_playback_bytes = 0;
    s_remote_message_rx_window_frames = 0;
    s_remote_message_rx_window_bytes = 0;
    s_local_audio_first_packet_logged = false;
    s_test_video_retry_after_us = 0U;
    s_test_audio_retry_after_us = 0U;
    s_peer_wants_video = false;
    s_peer_wants_audio = false;
    s_peer_video_control_seen = false;
    s_peer_audio_control_seen = false;
    s_peer_video_subscription_active = false;
    s_local_video_first_requested_at_us = 0U;
    s_builtin_capture_enabled = false;
    s_media_profile = TIRTC_SESSION_MEDIA_PROFILE_AV;
    s_next_connection_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
    s_active_conn_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
    s_next_connection_defer_media = false;
    s_active_conn_defer_media = false;
    s_call_media_deferred = false;
    s_local_video_publish_forced = false;
    s_local_audio_publish_forced = false;
    s_test_video_publish_forced = false;
    s_test_audio_publish_forced = false;
    memset(&s_last_peer_state, 0, sizeof(s_last_peer_state));
}

static bool tirtc_session_is_media_bootstrap_ready_locked(void)
{
    return s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
           s_closing_conn == NULL && s_call_active;
}

static bool tirtc_session_media_profile_allows_remote_video_locked(void)
{
    /*
     * The media profile selects microphone ownership. It must not suppress
     * encoded-video downlink for an external-audio full-duplex session such as
     * WeChat.
     */
    return s_remote_video_receive_enabled;
}

static bool tirtc_session_media_profile_uses_builtin_capture_locked(void)
{
    return s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO;
}

#if TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
static uint32_t tirtc_session_get_unix_time_s(void)
{
    time_t now = 0;

    time(&now);
    if (now < 0) {
        return 0;
    }

    return (uint32_t)now;
}
#endif

static void tirtc_session_on_peer_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    (void)user_data;

    tirtc_session_mode_t mode = TIRTC_SESSION_MODE_LISTEN;
    tirtc_session_state_t state = TIRTC_SESSION_STATE_STOPPED;
    bool sdk_started = false;
    bool start_in_progress = false;
    bool stop_in_progress = false;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    taskENTER_CRITICAL(&s_rtc_lock);
    mode = s_session_mode;
    state = s_state;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    stop_in_progress = s_stop_in_progress;
    active_conn = s_active_conn;
    closing_conn = s_closing_conn;
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG,
             "rtc peer connect result: error=%d %s hconn=%p mode=%u state=%u sdk_started=%d start=%d stop=%d active=%p closing=%p",
             error,
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             hconn,
             (unsigned)mode,
             (unsigned)state,
             sdk_started ? 1 : 0,
             start_in_progress ? 1 : 0,
             stop_in_progress ? 1 : 0,
             active_conn,
             closing_conn);

    if (error != 0) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(error);
        tirtc_session_note_event("peer connect fail");
        ESP_LOGW(TAG, "rtc peer connect failed: %s (%d)", TiRtcGetErrorStr(error), error);
        if (error == TIRTC_E_TIMEOUTED) {
            ESP_LOGW(TAG, "rtc peer connect timed out; resetting TiRTC runtime");
            (void)tirtc_session_request_runtime_restart("peer connect timeout");
        }
        return;
    }

    if (hconn == NULL) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(TIRTC_E_INVALID_PARAMETER);
        tirtc_session_note_event("peer conn empty");
        ESP_LOGW(TAG, "rtc peer connect returned empty handle");
        return;
    }

    tirtc_session_conn_accept_result_t accept_result =
        tirtc_session_accept_connection(hconn, true, true);
    if (accept_result != TIRTC_SESSION_CONN_ACCEPTED) {
        if (accept_result == TIRTC_SESSION_CONN_ACCEPT_STALE_CLOSING) {
            APP_LOG_DETAIL(TAG,
                           "rtc peer connect result ignored: hconn=%p already closing",
                           hconn);
            return;
        }
        tirtc_session_note_event("peer conn reject");
        ESP_LOGW(TAG, "rtc peer connection rejected: hconn=%p", hconn);
        (void)tirtc_session_disconnect_with_sdk_lock(hconn);
        return;
    }

    tirtc_session_bind_connection_user_data(hconn);

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
        .payload.conn = {
            .conn = hconn,
            .error = 0,
        },
    };

    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("peer accept inline");
        ESP_LOGW(TAG, "rtc event queue full: peer connection handled inline");
        tirtc_session_handle_runtime_event(&rtc_event);
    }
}

static void tirtc_session_on_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    tirtc_session_whip_request_t *request = (tirtc_session_whip_request_t *)user_data;
    bool accepted = false;

    ESP_LOGI(TAG,
             "WHIP connect callback: error=%s (%d) hconn=%p",
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             error,
             hconn);

    if (error == 0 && hconn != NULL) {
        tirtc_session_conn_accept_result_t accept_result =
            tirtc_session_accept_connection(hconn, false, false);
        if (accept_result == TIRTC_SESSION_CONN_ACCEPTED) {
            accepted = true;
            bool auto_media = tirtc_session_connection_auto_media_enabled(hconn);
            tirtc_session_bind_connection_user_data(hconn);
            if (!auto_media) {
                ESP_LOGI(TAG, "WHIP connection uses external media owner: hconn=%p", hconn);
            }

            tirtc_session_event_t rtc_event = {
                .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
                .payload.conn = {
                    .conn = hconn,
                    .error = 0,
                },
            };
            if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
                tirtc_session_note_event("whip accept inline");
                ESP_LOGW(TAG, "rtc event queue full: WHIP connection handled inline");
                tirtc_session_handle_runtime_event(&rtc_event);
            }
        } else {
            bool stale_closing =
                accept_result == TIRTC_SESSION_CONN_ACCEPT_STALE_CLOSING;
            if (!stale_closing) {
                ESP_LOGW(TAG, "WHIP connection rejected by runtime: hconn=%p", hconn);
                (void)tirtc_session_disconnect_with_sdk_lock(hconn);
            } else {
                APP_LOG_DETAIL(TAG,
                               "WHIP result ignored: hconn=%p already closing",
                               hconn);
            }
            error = TIRTC_E_BUSY;
            hconn = NULL;
        }
    }

    if (!accepted) {
        tirtc_session_set_next_connection_auto_media(TIRTC_SESSION_DEFAULT_AUTO_MEDIA);
    }

    if (request != NULL && request->cb != NULL) {
        request->cb(error, hconn, request->user_data);
    }
    tirtc_session_finish_whip_connect_attempt(request != NULL ? request->attempt_id : 0U);
    tirtc_session_free_whip_request(request);
}

static void tirtc_session_on_external_whip_connect_result(int error, tirtc_conn_t hconn, void *user_data)
{
    tirtc_session_whip_request_t *request = (tirtc_session_whip_request_t *)user_data;

    ESP_LOGI(TAG,
             "WHIP external callback: error=%s (%d) hconn=%p",
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             error,
             hconn);

    if (request != NULL && request->cb != NULL) {
        request->cb(error, hconn, request->user_data);
    }
    tirtc_session_finish_whip_connect_attempt(request != NULL ? request->attempt_id : 0U);
    tirtc_session_free_whip_request(request);
}

esp_err_t tirtc_session_start_configured_peer_connect(void)
{
    tirtc_session_config_t config = {0};
    bool network_connected = false;
    bool sdk_initialized = false;
    bool sdk_started = false;
    bool sdk_prepare_in_progress = false;
    bool start_in_progress = false;
    bool stop_in_progress = false;
    bool connect_in_progress = false;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    connect_in_progress = tirtc_connect_is_connecting();
    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_session_mode != TIRTC_SESSION_MODE_CONNECT) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }
    network_connected = s_network_connected;
    sdk_initialized = s_sdk_initialized;
    sdk_started = s_sdk_started;
    sdk_prepare_in_progress = s_sdk_prepare_in_progress;
    start_in_progress = s_start_in_progress;
    stop_in_progress = s_stop_in_progress;
    active_conn = s_active_conn;
    closing_conn = s_closing_conn;
    if (!s_network_connected || !s_sdk_initialized || !s_sdk_started || s_sdk_prepare_in_progress ||
        s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc peer connect preflight rejected: net=%d init=%d started=%d prep=%d start=%d stop=%d active=%p closing=%p connecting=%d",
                 network_connected ? 1 : 0,
                 sdk_initialized ? 1 : 0,
                 sdk_started ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0,
                 start_in_progress ? 1 : 0,
                 stop_in_progress ? 1 : 0,
                 active_conn,
                 closing_conn,
                 connect_in_progress ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (connect_in_progress) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG, "rtc peer connect preflight rejected: active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config.remote_device_id[0] == '\0') {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("remote id empty");
        ESP_LOGE(TAG, "rtc peer id is empty");
        return ESP_ERR_INVALID_ARG;
    }
    config = s_config;
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG,
             "rtc peer connect preflight ok: local_id_len=%u remote_id_len=%u state=%u",
             (unsigned)strlen(config.device_id),
             (unsigned)strlen(config.remote_device_id),
             (unsigned)tirtc_session_get_state());

    esp_err_t ret = tirtc_connect_start(&config, tirtc_session_on_peer_connect_result, NULL);
    if (ret != ESP_OK) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(ret);
        tirtc_session_note_event("peer connect fail");
        return ret;
    }

    tirtc_session_note_event("peer connect");
    ESP_LOGI(TAG,
             "rtc peer connect task started: remote_id_len=%u",
             (unsigned)strlen(config.remote_device_id));
    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ESP_OK;
}

static bool tirtc_session_should_tolerate_invalid_handle(tirtc_conn_t conn, uint64_t *conn_age_us)
{
    uint64_t accepted_at_us = 0;
    uint64_t now_us = 0;
    uint64_t age_us = 0;
    bool tolerate = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL && conn == s_active_conn && s_closing_conn == NULL && s_sdk_started &&
        !s_start_in_progress && !s_stop_in_progress) {
        accepted_at_us = s_active_conn_accepted_at_us;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (accepted_at_us != 0U) {
        now_us = esp_timer_get_time();
        age_us = now_us >= accepted_at_us ? now_us - accepted_at_us : 0U;
        tolerate = age_us <= TIRTC_SESSION_INVALID_HANDLE_GRACE_US;
    }

    if (conn_age_us != NULL) {
        *conn_age_us = age_us;
    }

    return tolerate;
}

static bool tirtc_session_should_retry_media_request_after_invalid_handle(tirtc_conn_t conn,
                                                                         const char *operation)
{
    uint64_t conn_age_us = 0;

    if (!tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
        return false;
    }

    tirtc_session_note_event("media req wait");
    ESP_LOGD(TAG,
             "%s got INVALID_HANDLE %llu us after accept; keep connection and retry media bootstrap",
             operation != NULL ? operation : "media request",
             (unsigned long long)conn_age_us);
    return true;
}

bool tirtc_session_should_retry_message_stream_after_invalid_handle(tirtc_conn_t conn, const char *operation)
{
    uint64_t conn_age_us = 0;

    if (!tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
        return false;
    }

    tirtc_session_note_event("message wait");
    ESP_LOGD(TAG,
             "%s got INVALID_HANDLE %llu us after accept; keep connection and retry time message",
             operation != NULL ? operation : "message stream",
             (unsigned long long)conn_age_us);
    return true;
}

bool tirtc_session_is_test_video_active(void)
{
    return s_hooks.is_test_video_active != NULL && s_hooks.is_test_video_active(s_hooks_ctx);
}

bool tirtc_session_is_test_audio_active(void)
{
    return s_hooks.is_test_audio_active != NULL && s_hooks.is_test_audio_active(s_hooks_ctx);
}

bool tirtc_session_is_test_media_active(void)
{
    return tirtc_session_is_test_video_active() || tirtc_session_is_test_audio_active();
}

void tirtc_session_request_test_audio_restart(void)
{
    if (s_hooks.request_test_audio_restart != NULL) {
        s_hooks.request_test_audio_restart(s_hooks_ctx);
    }
}

static bool tirtc_session_is_media_request_command(uint32_t cmdw)
{
    uint16_t cmd_id = (uint16_t)(cmdw & 0x7FFFU);

    return cmd_id == TIRTC_SESSION_CMD_REQ_VIDEO || cmd_id == TIRTC_SESSION_CMD_REQ_AUDIO;
}

static esp_err_t tirtc_session_send_time_stream_message(void)
{
#if !TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!tirtc_session_try_get_active_conn(NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t unix_time_s = tirtc_session_get_unix_time_s();
    uint8_t payload[sizeof(unix_time_s)] = {
        (uint8_t)(unix_time_s & 0xFFU),
        (uint8_t)((unix_time_s >> 8) & 0xFFU),
        (uint8_t)((unix_time_s >> 16) & 0xFFU),
        (uint8_t)((unix_time_s >> 24) & 0xFFU),
    };

    return tirtc_session_send_stream_message(payload, sizeof(payload));
#endif
}

static void tirtc_session_time_message_initial_timer_cb(void *arg)
{
    (void)arg;

    esp_err_t send_ret = tirtc_session_send_time_stream_message();

    if (send_ret == ESP_OK && s_time_message_periodic_timer != NULL && tirtc_session_try_get_active_conn(NULL)) {
        (void)esp_timer_start_periodic(s_time_message_periodic_timer,
                                       TIRTC_SESSION_TIME_MESSAGE_PERIOD_US);
    } else if (send_ret == ESP_ERR_INVALID_STATE && s_time_message_initial_timer != NULL &&
               tirtc_session_try_get_active_conn(NULL)) {
        (void)esp_timer_start_once(s_time_message_initial_timer,
                                   TIRTC_SESSION_TIME_MESSAGE_RETRY_DELAY_US);
    }
}

static void tirtc_session_time_message_periodic_timer_cb(void *arg)
{
    (void)arg;

    (void)tirtc_session_send_time_stream_message();
}

static void tirtc_session_media_bootstrap_timer_cb(void *arg)
{
    (void)arg;

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_MEDIA_BOOTSTRAP,
    };

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("media bootstrap drop");
        ESP_LOGW(TAG, "rtc event queue full: media bootstrap dropped");
    }
}

static void tirtc_session_remote_video_first_packet_timer_cb(void *arg)
{
    bool retry = false;

    (void)arg;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_remote_video_first_packet_retry_armed) {
        s_remote_video_first_packet_retry_armed = false;
        if (s_remote_video_requested &&
            !s_remote_video_first_packet_logged &&
            s_remote_video_callback_frames == 0U &&
            s_remote_video_request_attempts == 1U &&
            tirtc_session_is_media_bootstrap_ready_locked() &&
            tirtc_session_media_profile_allows_remote_video_locked()) {
            s_remote_video_first_packet_retry_due = true;
            s_media_bootstrap_pending = true;
            retry = true;
        }
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!retry) {
        return;
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_MEDIA_BOOTSTRAP,
    };
    if (!tirtc_session_enqueue_event(&rtc_event,
                                      TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("video retry drop");
        ESP_LOGW(TAG, "rtc event queue full: remote video retry dropped");
    }
}

static void tirtc_session_disconnect_watchdog_timer_cb(void *arg)
{
    tirtc_session_event_t rtc_event = {0};
    tirtc_conn_t closing_conn = NULL;
    bool was_sdk_started = false;

    (void)arg;

    taskENTER_CRITICAL(&s_rtc_lock);
    closing_conn = s_closing_conn;
    was_sdk_started = s_closing_conn_was_sdk_started;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (closing_conn == NULL) {
        return;
    }

    /*
     * Owners must see the same terminal event whether it came from the SDK or
     * from the watchdog. Otherwise they retain a stale connection handle even
     * though the protocol layer has already returned to READY.
     */
    rtc_event.type = TIRTC_SESSION_EVENT_DISCONNECTED;
    rtc_event.payload.conn.conn = closing_conn;
    rtc_event.payload.conn.error = 0;
    tirtc_session_note_event("disconnect timeout");
    ESP_LOGW(TAG,
             "rtc disconnect timeout: hconn=%p, completing teardown through worker",
             closing_conn);
    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        ESP_LOGE(TAG,
                 "rtc disconnect timeout event dropped: hconn=%p, force completing teardown",
                 closing_conn);
        (void)tirtc_session_complete_connection_shutdown(closing_conn, was_sdk_started);
    }
}

static void tirtc_session_deferred_full_reset_timer_cb(void *arg)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_DEFERRED_FULL_RESET,
    };

    (void)arg;

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("full reset drop");
        ESP_LOGW(TAG, "rtc event queue full: deferred full reset dropped");
    }
}

static void tirtc_session_deferred_start_after_full_reset_timer_cb(void *arg)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_DEFERRED_START_AFTER_FULL_RESET,
    };

    (void)arg;

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("full start drop");
        ESP_LOGW(TAG, "rtc event queue full: deferred start after full reset dropped");
    }
}

static void tirtc_session_cancel_deferred_full_reset(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_full_reset_pending = false;
    s_deferred_full_reset_due_at_us = 0U;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (s_deferred_full_reset_timer != NULL) {
        (void)esp_timer_stop(s_deferred_full_reset_timer);
    }
}

static void tirtc_session_cancel_deferred_start_after_full_reset(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_start_after_full_reset_pending = false;
    s_deferred_start_after_full_reset_due_at_us = 0U;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (s_deferred_start_after_full_reset_timer != NULL) {
        (void)esp_timer_stop(s_deferred_start_after_full_reset_timer);
    }
}

bool tirtc_session_schedule_deferred_full_reset(void)
{
    uint64_t due_at_us = esp_timer_get_time() + TIRTC_SESSION_DEFERRED_FULL_RESET_DELAY_US;

    if (s_deferred_full_reset_timer == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_full_reset_pending = true;
    s_deferred_full_reset_due_at_us = due_at_us;
    taskEXIT_CRITICAL(&s_rtc_lock);

    (void)esp_timer_stop(s_deferred_full_reset_timer);
    if (esp_timer_start_once(s_deferred_full_reset_timer, TIRTC_SESSION_DEFERRED_FULL_RESET_DELAY_US) != ESP_OK) {
        tirtc_session_cancel_deferred_full_reset();
        return false;
    }

    tirtc_session_note_event("rtc full reset wait");
    ESP_LOGD(TAG,
             "rtc full reset deferred: delay_us=%llu",
             (unsigned long long)TIRTC_SESSION_DEFERRED_FULL_RESET_DELAY_US);
    return true;
}

static bool tirtc_session_schedule_deferred_start_after_delay(uint64_t delay_us, const char *reason)
{
    uint64_t due_at_us = esp_timer_get_time() + delay_us;

    if (s_deferred_start_after_full_reset_timer == NULL) {
        return false;
    }
    if (delay_us == 0) {
        delay_us = 1000U;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_deferred_start_after_full_reset_pending = true;
    s_deferred_start_after_full_reset_due_at_us = due_at_us;
    taskEXIT_CRITICAL(&s_rtc_lock);

    (void)esp_timer_stop(s_deferred_start_after_full_reset_timer);
    if (esp_timer_start_once(s_deferred_start_after_full_reset_timer, delay_us) != ESP_OK) {
        tirtc_session_cancel_deferred_start_after_full_reset();
        return false;
    }

    tirtc_session_note_event(reason != NULL ? reason : "rtc start wait");
    ESP_LOGD(TAG,
             "rtc start deferred: reason=%s delay_us=%llu",
             reason != NULL ? reason : "start",
             (unsigned long long)delay_us);
    return true;
}

bool tirtc_session_schedule_deferred_start_after_full_reset(void)
{
    return tirtc_session_schedule_deferred_start_after_delay(TIRTC_SESSION_RESTART_AFTER_FULL_RESET_DELAY_US,
                                                            "rtc full start wait");
}

void tirtc_session_handle_deferred_full_reset(void)
{
    bool sdk_initialized = false;
    bool sdk_started = false;
    bool can_reset = false;
    bool should_restart = false;
    bool wait_stop_notice = false;
    int stop_ret = 0;

    tirtc_session_cancel_deferred_full_reset();

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("full reset deferred");
        ESP_LOGW(TAG, "rtc full reset deferred: connection still closing/active");
        (void)tirtc_session_schedule_deferred_full_reset();
        return;
    }

    sdk_initialized = s_sdk_initialized;
    sdk_started = s_sdk_started;
    can_reset = s_sdk_initialized || s_sdk_started || s_start_in_progress || s_sdk_prepare_in_progress;
    should_restart = s_config.enabled && s_network_connected;
    if (can_reset) {
        s_sdk_generation++;
        if (s_sdk_generation == 0U) {
            s_sdk_generation = 1U;
        }
        s_pending_stop_generation = s_sdk_generation;
        s_sdk_stop_notified = false;
        s_sdk_prepare_in_progress = false;
        s_start_in_progress = false;
        s_stop_in_progress = true;
        s_sdk_started = false;
        s_next_start_allowed_us = 0U;
        s_session_mode = TIRTC_SESSION_MODE_LISTEN;
        s_next_connection_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
        s_active_conn_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
        s_started_device_id[0] = '\0';
        s_started_credential_hash[0] = '\0';
        s_started_secret_len = 0;
        tirtc_session_reset_call_state_locked();
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!can_reset) {
        tirtc_session_note_event("rtc full reset idle");
        if (should_restart) {
            (void)tirtc_session_schedule_deferred_start_after_full_reset();
        }
        return;
    }

    tirtc_connect_cancel();
    tirtc_session_stop_time_stream_messages();
    tirtc_session_cancel_media_bootstrap();
    tirtc_session_flush_local_video_tx_queue();
    tirtc_session_flush_local_audio_tx_queue();
    tirtc_session_media_flush();

    ESP_LOGI(TAG,
             "rtc full reset begin: sdk_initialized=%d sdk_started=%d restart=%d",
             sdk_initialized ? 1 : 0,
             sdk_started ? 1 : 0,
             should_restart ? 1 : 0);

    if (sdk_started) {
        if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
            stop_ret = TiRtcStop();
            tirtc_session_give_sdk_api_lock();
        } else {
            stop_ret = TIRTC_E_BUSY;
        }
        if (stop_ret < 0) {
            tirtc_session_set_last_error(stop_ret);
            ESP_LOGW(TAG, "TiRtcStop during full reset failed: %s (%d)", TiRtcGetErrorStr(stop_ret), stop_ret);
        } else {
            uint32_t waited_ms = 0;
            wait_stop_notice = true;
            while (waited_ms < TIRTC_SESSION_STOP_WAIT_MS) {
                bool notified = false;

                taskENTER_CRITICAL(&s_rtc_lock);
                notified = s_sdk_stop_notified;
                taskEXIT_CRITICAL(&s_rtc_lock);
                if (notified) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                waited_ms += 20U;
            }
            if (waited_ms >= TIRTC_SESSION_STOP_WAIT_MS) {
                ESP_LOGW(TAG, "TiRtcStop notice wait timed out during full reset");
            }
        }
    }

    if (sdk_initialized) {
        if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
            TiRtcUninit();
            tirtc_session_give_sdk_api_lock();
        } else {
            ESP_LOGW(TAG, "TiRtcUninit skipped: sdk api lock busy");
        }
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_initialized = false;
    s_sdk_started = false;
    s_sdk_prepare_in_progress = false;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_session_mode = TIRTC_SESSION_MODE_LISTEN;
    s_pending_stop_generation = 0U;
    s_sdk_stop_notified = true;
    s_active_conn = NULL;
    s_active_conn_accepted_at_us = 0U;
    s_active_conn_supports_tgmp_bitrate = false;
    s_closing_conn = NULL;
    s_closing_conn_was_sdk_started = false;
    tirtc_session_reset_call_state_locked();
    s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_note_event("rtc full reset done");
    ESP_LOGI(TAG,
             "rtc full reset done: stop_notice=%d restart=%d",
             wait_stop_notice ? 1 : 0,
             should_restart ? 1 : 0);
    if (should_restart) {
        (void)tirtc_session_schedule_deferred_start_after_full_reset();
    }
}

void tirtc_session_handle_deferred_start_after_full_reset(void)
{
    tirtc_session_cancel_deferred_start_after_full_reset();
    tirtc_session_note_event("rtc prepare now");
    (void)tirtc_session_prepare_sdk();
}

static bool tirtc_session_schedule_disconnect_watchdog(const char *reason, uint64_t delay_us)
{
    tirtc_conn_t closing_conn = NULL;

    taskENTER_CRITICAL(&s_rtc_lock);
    closing_conn = s_closing_conn;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (closing_conn == NULL || s_disconnect_watchdog_timer == NULL) {
        return false;
    }

    (void)esp_timer_stop(s_disconnect_watchdog_timer);
    (void)esp_timer_start_once(s_disconnect_watchdog_timer, delay_us);
    ESP_LOGD(TAG,
             "schedule disconnect watchdog reason=%s hconn=%p delay_us=%llu",
             reason != NULL ? reason : "unspecified",
             closing_conn,
             (unsigned long long)delay_us);
    return true;
}

static bool tirtc_session_schedule_media_bootstrap_timer(const char *reason, uint64_t delay_us)
{
    bool should_start = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    should_start = tirtc_session_is_media_bootstrap_ready_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_start || s_media_bootstrap_timer == NULL) {
        return false;
    }

    (void)esp_timer_stop(s_media_bootstrap_timer);
    (void)esp_timer_start_once(s_media_bootstrap_timer, delay_us);
    tirtc_session_note_event(reason != NULL ? reason : "media bootstrap");
    ESP_LOGD(TAG,
             "rtc media bootstrap scheduled: reason=%s delay_us=%llu",
             reason != NULL ? reason : "unspecified",
             (unsigned long long)delay_us);
    return true;
}

static void tirtc_session_schedule_remote_video_first_packet_retry(
    uint8_t request_attempt)
{
    bool should_start = false;

    if (request_attempt != 1U || s_remote_video_first_packet_timer == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    should_start = s_remote_video_requested &&
                   !s_remote_video_first_packet_logged &&
                   s_remote_video_callback_frames == 0U &&
                   s_remote_video_request_attempts == 1U &&
                   tirtc_session_is_media_bootstrap_ready_locked();
    s_remote_video_first_packet_retry_armed = should_start;
    s_remote_video_first_packet_retry_due = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_start) {
        return;
    }

    (void)esp_timer_stop(s_remote_video_first_packet_timer);
    esp_err_t ret = esp_timer_start_once(
        s_remote_video_first_packet_timer,
        TIRTC_SESSION_REMOTE_VIDEO_FIRST_PACKET_RETRY_US);
    if (ret == ESP_OK) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_remote_video_first_packet_retry_armed = false;
    taskEXIT_CRITICAL(&s_rtc_lock);
    ESP_LOGW(TAG,
             "remote video first-packet retry timer failed: %s",
             esp_err_to_name(ret));
}

void tirtc_session_schedule_media_bootstrap(const char *reason)
{
    bool should_start = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (tirtc_session_is_media_bootstrap_ready_locked()) {
        s_media_bootstrap_pending = true;
        should_start = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_start || s_media_bootstrap_timer == NULL) {
        return;
    }

    (void)tirtc_session_schedule_media_bootstrap_timer(reason, TIRTC_SESSION_MEDIA_BOOTSTRAP_INITIAL_DELAY_US);
}

void tirtc_session_retry_remote_media_request(bool retry_video, bool retry_audio, const char *reason)
{
    tirtc_session_retry_remote_media_request_after_delay(retry_video,
                                                        retry_audio,
                                                        reason,
                                                        TIRTC_SESSION_MEDIA_BOOTSTRAP_RETRY_DELAY_US);
}

static bool tirtc_session_should_defer_audio_for_local_video_locked(void)
{
#if TIRTC_SESSION_VIDEO_FIRST_DEFER_AUDIO
    media_governor_rtc_policy_t policy = {0};
    media_governor_get_rtc_policy(&policy);
    return s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO &&
           policy.defer_audio_for_local_video &&
           s_call_active &&
           s_local_video_send_enabled &&
           s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
#else
    return false;
#endif
}

static bool tirtc_session_should_prepare_playback_after_bootstrap(void)
{
    media_governor_rtc_policy_t policy = {0};
    media_governor_get_rtc_policy(&policy);
    return policy.prepare_playback_while_video_first;
}

static void tirtc_session_retry_remote_media_request_after_delay(bool retry_video,
                                                                bool retry_audio,
                                                                const char *reason,
                                                                uint64_t delay_us)
{
    bool should_start = false;

    if (!retry_video && !retry_audio) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (tirtc_session_is_media_bootstrap_ready_locked()) {
        if (retry_video) {
            s_remote_video_requested = false;
        }
        if (retry_audio) {
            s_remote_audio_requested = false;
        }
        s_media_bootstrap_pending = true;
        should_start = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_start || s_media_bootstrap_timer == NULL) {
        return;
    }

    (void)tirtc_session_schedule_media_bootstrap_timer(reason, delay_us);
}

void tirtc_session_run_media_bootstrap(void)
{
    bool should_run = false;
    bool retry_remote_video = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_media_bootstrap_pending && tirtc_session_is_media_bootstrap_ready_locked()) {
        should_run = true;
        if (s_remote_video_first_packet_retry_due) {
            retry_remote_video = s_remote_video_requested &&
                                 !s_remote_video_first_packet_logged &&
                                 s_remote_video_callback_frames == 0U &&
                                 s_remote_video_request_attempts == 1U &&
                                 tirtc_session_media_profile_allows_remote_video_locked();
            if (retry_remote_video) {
                s_remote_video_requested = false;
            }
            s_remote_video_first_packet_retry_due = false;
        }
    }
    s_media_bootstrap_pending = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!should_run) {
        return;
    }

    if (retry_remote_video) {
        ESP_LOGI(TAG, "remote video first packet timeout: retry subscribe once");
    }
    tirtc_session_note_event("media bootstrap");
    tirtc_session_apply_local_media_policy();
    tirtc_session_request_remote_media();
    if (!tirtc_session_should_prepare_playback_after_bootstrap()) {
        APP_LOG_DETAIL(TAG, "rtc playback path deferred until first remote audio packet");
        return;
    }
    if (tirtc_session_media_prepare_playback_path() != ESP_OK) {
        ESP_LOGW(TAG, "rtc media bootstrap failed: playback path not ready");
    }
}

void tirtc_session_start_time_stream_messages(void)
{
#if !TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES
    tirtc_session_stop_time_stream_messages();
#else
    if (s_time_message_initial_timer == NULL || s_time_message_periodic_timer == NULL) {
        return;
    }

    (void)esp_timer_stop(s_time_message_initial_timer);
    (void)esp_timer_stop(s_time_message_periodic_timer);
    (void)esp_timer_start_once(s_time_message_initial_timer,
                               TIRTC_SESSION_TIME_MESSAGE_INITIAL_DELAY_US);
#endif
}

void tirtc_session_stop_time_stream_messages(void)
{
    if (s_time_message_initial_timer != NULL) {
        (void)esp_timer_stop(s_time_message_initial_timer);
    }
    if (s_time_message_periodic_timer != NULL) {
        (void)esp_timer_stop(s_time_message_periodic_timer);
    }
}

static bool tirtc_session_maybe_force_local_video_publish_locked(void)
{
    if ((!s_peer_wants_video && s_peer_video_control_seen) ||
        !s_local_video_send_enabled || s_active_conn == NULL || !s_sdk_started ||
        s_start_in_progress || s_stop_in_progress || s_closing_conn != NULL || !s_call_active ||
        s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return false;
    }

    s_local_video_stream_id = TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
    s_local_video_publish_forced = true;
    s_test_video_publish_forced = false;
    tirtc_session_sync_stats_locked();
    return true;
}

static bool tirtc_session_maybe_force_local_audio_publish_locked(void)
{
    if ((!s_peer_wants_audio && s_peer_audio_control_seen) ||
        !s_local_audio_send_enabled || s_active_conn == NULL || !s_sdk_started ||
        s_start_in_progress || s_stop_in_progress || s_closing_conn != NULL || !s_call_active ||
        s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return false;
    }

    s_local_audio_stream_id = TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
    s_local_audio_publish_forced = true;
    s_test_audio_publish_forced = false;
    tirtc_session_sync_stats_locked();
    return true;
}

static void tirtc_session_sync_test_media_publish_locked(bool test_video_active, bool test_audio_active)
{
    bool can_force_publish = s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
                             s_closing_conn == NULL && s_active_conn != NULL && s_call_active;
    bool changed = false;

    if (can_force_publish && test_video_active && s_local_video_send_enabled &&
        s_local_video_stream_id == TIRTC_SESSION_INVALID_STREAM_ID) {
        s_local_video_stream_id = TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
        s_local_video_publish_forced = true;
        s_test_video_publish_forced = true;
        changed = true;
    } else if (s_test_video_publish_forced &&
               (!can_force_publish || !test_video_active || !s_local_video_send_enabled)) {
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_test_video_publish_forced = false;
        changed = true;
    }

    if (can_force_publish && test_audio_active && s_local_audio_send_enabled &&
        s_local_audio_stream_id == TIRTC_SESSION_INVALID_STREAM_ID) {
        s_local_audio_stream_id = TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
        s_local_audio_publish_forced = true;
        s_test_audio_publish_forced = true;
        changed = true;
    } else if (s_test_audio_publish_forced &&
               (!can_force_publish || !test_audio_active || !s_local_audio_send_enabled)) {
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_publish_forced = false;
        s_test_audio_publish_forced = false;
        changed = true;
    }

    if (changed) {
        tirtc_session_sync_stats_locked();
    }
}

static uint8_t tirtc_session_get_effective_local_video_stream_id_locked(void)
{
    if (s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return tirtc_session_normalize_local_video_stream_id(s_local_video_stream_id);
    }

    return TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
}

static uint8_t tirtc_session_get_effective_local_audio_stream_id_locked(void)
{
    if (s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID) {
        return tirtc_session_normalize_local_audio_stream_id(s_local_audio_stream_id);
    }

    return TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
}

static uint8_t tirtc_session_normalize_local_video_stream_id(uint8_t stream_id)
{
    return stream_id == TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID ? stream_id : TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID;
}

static uint8_t tirtc_session_normalize_local_audio_stream_id(uint8_t stream_id)
{
    return stream_id == TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID ? stream_id : TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID;
}

bool tirtc_session_should_reset_after_send_error(int error)
{
    switch (error) {
    case TIRTC_E_INVALID_HANDLE:
    case TIRTC_E_CONN_TIMEOUTCLOSE:
    case TIRTC_E_CONN_REMOTECLOSE:
    case TIRTC_E_CONN_OTHER_ERROR:
    case TIRTC_E_INTERNAL_ERROR:
        return true;
    default:
        return false;
    }
}

static bool tirtc_session_is_test_media_window_open_locked(uint64_t now_us, uint64_t retry_after_us)
{
    uint64_t accepted_at_us = s_active_conn_accepted_at_us;
    uint64_t earliest_send_us = 0U;

    if (accepted_at_us == 0U) {
        return false;
    }

    earliest_send_us = accepted_at_us + TIRTC_SESSION_TEST_MEDIA_WARMUP_US;
    if (retry_after_us > earliest_send_us) {
        earliest_send_us = retry_after_us;
    }

    return now_us >= earliest_send_us;
}

static void tirtc_session_build_local_peer_state_locked(tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->valid = true;
    state->call_active = s_call_active;
    state->local_video_send_enabled = s_local_video_send_enabled;
    state->local_audio_send_enabled = s_local_audio_send_enabled;
    state->video_stream_active = s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    state->audio_stream_active = s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    memcpy(state->rgb, s_local_rgb, sizeof(state->rgb));
}

void tirtc_session_get_local_peer_state(tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_build_local_peer_state_locked(state);
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_last_peer_state(const tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_last_peer_state = *state;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_local_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_rgb[0] = red;
    s_local_rgb[1] = green;
    s_local_rgb[2] = blue;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_peer_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_last_peer_state.valid = true;
    s_last_peer_state.rgb[0] = red;
    s_last_peer_state.rgb[1] = green;
    s_last_peer_state.rgb[2] = blue;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_peer_video_requested(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;
    bool active = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    active = s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
             s_closing_conn == NULL && s_call_active;
    if (active) {
        s_peer_wants_video = enabled;
        s_peer_video_control_seen = true;
        if (enabled) {
            tirtc_session_mark_local_video_requested_locked();
        }
    }
    if (active && !enabled && s_local_video_publish_forced) {
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_test_video_publish_forced = false;
        tirtc_session_sync_stats_locked();
        cleared_forced_publish = true;
    } else if (active && enabled) {
        forced_publish = tirtc_session_maybe_force_local_video_publish_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local video fallback publish forced: peer request arrived before subscribe callback stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local video fallback publish cleared by peer request");
    }

    tirtc_session_apply_local_media_policy();
}

void tirtc_session_set_peer_audio_requested(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;
    bool active = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    active = s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
             s_closing_conn == NULL && s_call_active;
    if (active) {
        s_peer_wants_audio = enabled;
        s_peer_audio_control_seen = true;
    }
    if (active && !enabled && s_local_audio_publish_forced) {
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_publish_forced = false;
        s_test_audio_publish_forced = false;
        tirtc_session_sync_stats_locked();
        cleared_forced_publish = true;
    } else if (active && enabled) {
        forced_publish = tirtc_session_maybe_force_local_audio_publish_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local audio fallback publish forced: peer request arrived before subscribe callback stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local audio fallback publish cleared by peer request");
    }

    tirtc_session_apply_local_media_policy();
}

void tirtc_session_get_pending_call(tirtc_conn_t *conn, uint32_t *pending_cmdw)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL) {
        *conn = (s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
                 s_closing_conn == NULL) ? s_active_conn : NULL;
    }
    if (pending_cmdw != NULL) {
        *pending_cmdw = s_pending_call_cmdw;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_mark_incoming_call(uint32_t pending_cmdw)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        s_incoming_call_pending = true;
        s_pending_call_cmdw = pending_cmdw;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_complete_call_response_internal(bool accepted, bool defer_media)
{
    bool should_bootstrap = false;
    bool was_call_active = false;
    bool was_media_deferred = false;
    bool media_active_changed = false;
    bool media_active = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    was_call_active = s_call_active;
    was_media_deferred = s_call_media_deferred;
    s_call_active = accepted;
    s_call_media_deferred = accepted && defer_media;
    media_active = s_call_active && !s_call_media_deferred;
    media_active_changed = (was_call_active && !was_media_deferred) != media_active;
    s_incoming_call_pending = false;
    s_pending_call_cmdw = 0;
    tirtc_session_sync_stats_locked();
    should_bootstrap = accepted && !defer_media && (!was_call_active || was_media_deferred) &&
                       s_active_conn != NULL && s_sdk_started &&
                       !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL;
    taskEXIT_CRITICAL(&s_rtc_lock);

    /*
     * on_call_active owns application media resources. A deferred device call
     * has an accepted transport but must not start capture/render until the
     * room confirmation command promotes it to an active media session.
     */
    if (media_active_changed) {
        tirtc_session_notify_call_active(media_active);
    }
    if (should_bootstrap && !tirtc_session_is_test_media_active()) {
        tirtc_session_schedule_media_bootstrap(was_media_deferred ?
                                               "deferred media allowed" :
                                               "call accepted");
    }
}

void tirtc_session_complete_call_response(bool accepted)
{
    tirtc_session_complete_call_response_internal(accepted, false);
}

void tirtc_session_complete_call_response_without_media(bool accepted)
{
    tirtc_session_complete_call_response_internal(accepted, true);
}

esp_err_t tirtc_session_activate_deferred_media(bool enable_video, bool enable_audio)
{
    tirtc_conn_t conn = NULL;

    if (!tirtc_session_try_get_active_conn(&conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    /*
     * The accepted-event dispatcher notifies device-call observers before it
     * finishes its own deferred-media branch. A caller may therefore complete
     * the room handshake synchronously from that observer. Clear the
     * connection-scoped defer flag as part of the same promotion; otherwise
     * the dispatcher sees the stale flag afterwards and demotes an already
     * active call back to deferred media.
     */
    if (conn == s_active_conn) {
        s_active_conn_defer_media = false;
    }
    s_remote_video_receive_enabled = enable_video;
    taskEXIT_CRITICAL(&s_rtc_lock);

    (void)tirtc_session_set_local_video_send_enabled(enable_video);
    (void)tirtc_session_set_local_audio_send_enabled(enable_audio);
    tirtc_session_complete_call_response(true);
    tirtc_session_apply_local_media_policy();
    return ESP_OK;
}

static void tirtc_session_free_event_payload(tirtc_session_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TIRTC_SESSION_EVENT_REMOTE_MESSAGE:
        free(event->payload.message.data);
        event->payload.message.data = NULL;
        event->payload.message.data_len = 0;
        break;
    case TIRTC_SESSION_EVENT_REMOTE_COMMAND:
        free(event->payload.command.data);
        event->payload.command.data = NULL;
        event->payload.command.data_len = 0;
        break;
    default:
        break;
    }
}

static const char *tirtc_session_media_name(uint8_t media)
{
    switch (media) {
    case TIRTC_MEDIA_MESSAGE:
        return "message";
    case TIRTC_AUDIO_PCM:
        return "pcm";
    case TIRTC_AUDIO_AAC:
        return "aac";
    case TIRTC_VIDEO_JPEG:
        return "jpeg";
    case TIRTC_VIDEO_H264:
        return "h264";
    case TIRTC_VIDEO_H265:
        return "h265";
    default:
        break;
    }
    if (TIRTC_IS_AUDIO(media)) {
        return "audio";
    }
    if (TIRTC_IS_VIDEO(media)) {
        return "video";
    }
    return "unknown";
}

static void tirtc_session_format_payload_head(const uint8_t *data, size_t data_len, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (data == NULL || data_len == 0) {
        return;
    }

    size_t preview_len = data_len < TIRTC_SESSION_MESSAGE_PREVIEW_BYTES ? data_len :
                                                                          TIRTC_SESSION_MESSAGE_PREVIEW_BYTES;
    size_t offset = 0;
    for (size_t i = 0; i < preview_len && offset < out_len; ++i) {
        int written = snprintf(out + offset,
                               out_len - offset,
                               "%s%02X",
                               i == 0 ? "" : " ",
                               data[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= out_len - offset) {
            out[out_len - 1] = '\0';
            break;
        }
        offset += (size_t)written;
    }
}

static void tirtc_session_handle_remote_message(const tirtc_session_event_t *event)
{
    if (event == NULL) {
        return;
    }

    const TickType_t now_tick = xTaskGetTickCount();
    const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_RX_LOG_INTERVAL_MS);
    bool log_packet = false;
    bool log_first_packet = false;
    uint32_t window_frames = 0;
    size_t window_bytes = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_remote_message_first_packet_logged) {
        s_remote_message_first_packet_logged = true;
        s_last_remote_message_rx_log_tick = now_tick;
        log_first_packet = true;
    }
    s_stats.rx_message_frames++;
    s_stats.rx_message_bytes += event->payload.message.data_len;
    s_remote_message_rx_window_frames++;
    s_remote_message_rx_window_bytes += event->payload.message.data_len;
    tirtc_session_set_last_event_locked("message rx");
    if (!log_first_packet &&
        (s_last_remote_message_rx_log_tick == 0 ||
         now_tick - s_last_remote_message_rx_log_tick >= log_interval_ticks)) {
        log_packet = true;
        window_frames = s_remote_message_rx_window_frames;
        window_bytes = s_remote_message_rx_window_bytes;
        s_remote_message_rx_window_frames = 0;
        s_remote_message_rx_window_bytes = 0;
        s_last_remote_message_rx_log_tick = now_tick;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_first_packet || log_packet) {
        char head[TIRTC_SESSION_MESSAGE_PREVIEW_TEXT_LEN] = {0};
        tirtc_session_format_payload_head(event->payload.message.data,
                                          event->payload.message.data_len,
                                          head,
                                          sizeof(head));
        ESP_LOGI(TAG,
                 "%s stream=%u media=%u(%s) flags=%u ts=%lu payload=%u%s%s",
                 log_first_packet ? "remote message first packet" : "remote message rx",
                 (unsigned)event->payload.message.stream_id,
                 (unsigned)event->payload.message.media,
                 tirtc_session_media_name(event->payload.message.media),
                 (unsigned)event->payload.message.flags,
                 (unsigned long)event->payload.message.ts,
                 (unsigned)event->payload.message.data_len,
                 head[0] != '\0' ? " head=" : "",
                 head);
        if (log_packet) {
            ESP_LOGI(TAG,
                     "remote message window frames=%lu bytes=%u",
                     (unsigned long)window_frames,
                     (unsigned)window_bytes);
        }
    }
}

static void tirtc_session_sync_stats_locked(void)
{
    s_state = tirtc_session_compute_state_locked();
    s_stats.enabled = s_config.enabled;
    s_stats.sdk_initialized = s_sdk_initialized;
    s_stats.sdk_started = s_sdk_started;
    s_stats.active_connection = (s_active_conn != NULL);
    s_stats.call_active = s_call_active;
    s_stats.incoming_call_pending = s_incoming_call_pending;
    s_stats.local_video_send_enabled = s_local_video_send_enabled;
    s_stats.local_audio_send_enabled = s_local_audio_send_enabled;
    s_stats.session_mode = s_session_mode;
    s_stats.state = s_state;
    s_stats.local_video_stream_id = s_local_video_stream_id;
    s_stats.local_audio_stream_id = s_local_audio_stream_id;
}

static void tirtc_session_update_pool_stats_unlocked(tirtc_session_stats_t *stats)
{
    size_t total_capacity = 0;
    size_t largest_slot = 0;

    if (stats == NULL) {
        return;
    }

    for (size_t index = 0; index < TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE; ++index) {
        total_capacity += s_local_video_tx_buffer_capacities[index];
        if (s_local_video_tx_buffer_capacities[index] > largest_slot) {
            largest_slot = s_local_video_tx_buffer_capacities[index];
        }
    }

    stats->local_video_tx_pool_capacity = total_capacity;
    stats->local_video_tx_largest_slot = largest_slot;
    stats->local_audio_tx_pool_capacity = 0U;
    for (size_t index = 0; index < TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE; ++index) {
        if (s_local_audio_tx_buffers[index] != NULL) {
            stats->local_audio_tx_pool_capacity += TIRTC_SESSION_AUDIO_TX_BUFFER_BYTES;
        }
    }
}

static void tirtc_session_update_queue_stats(tirtc_session_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    stats->local_video_tx_queue_len =
        s_local_video_tx_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_local_video_tx_queue) : 0U;
    stats->local_video_tx_free_slots =
        s_local_video_tx_free_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0U;
    stats->local_audio_tx_queue_len =
        s_local_audio_tx_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_local_audio_tx_queue) : 0U;
    stats->local_audio_tx_free_slots =
        s_local_audio_tx_free_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_local_audio_tx_free_queue) : 0U;
}

static void tirtc_session_return_to_listen_mode(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_session_mode = TIRTC_SESSION_MODE_LISTEN;
    s_next_connection_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
    s_next_connection_defer_media = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_set_last_event_locked(const char *event_text)
{
    strlcpy(s_stats.last_event,
            event_text != NULL ? event_text : "idle",
            sizeof(s_stats.last_event));
}

static void tirtc_session_mark_local_video_requested_locked(void)
{
    if (s_local_video_first_requested_at_us == 0U) {
        s_local_video_first_requested_at_us = esp_timer_get_time();
    }
}

static tirtc_session_state_t tirtc_session_compute_state_locked(void)
{
    if (s_state_error_override) {
        return TIRTC_SESSION_STATE_ERROR;
    }

    if (s_stop_in_progress || s_closing_conn != NULL) {
        return TIRTC_SESSION_STATE_DISCONNECTING;
    }

    if (s_media_bootstrap_pending && s_active_conn != NULL) {
        return TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING;
    }

    if (s_active_conn != NULL) {
        return TIRTC_SESSION_STATE_CONNECTED;
    }

    if (s_sdk_prepare_in_progress || s_start_in_progress || tirtc_connect_is_connecting()) {
        return TIRTC_SESSION_STATE_STARTING;
    }

    if (s_sdk_started) {
        return TIRTC_SESSION_STATE_READY;
    }

    return TIRTC_SESSION_STATE_STOPPED;
}

void tirtc_session_note_event(const char *event_text)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_set_last_event_locked(event_text);
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_last_error(int error)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.last_error = error;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_update_state(tirtc_session_state_t state)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_state_error_override = (state == TIRTC_SESSION_STATE_ERROR);
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_mark_sdk_started(void)
{
    bool notify_started = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    notify_started = !s_sdk_started || s_start_in_progress;
    if (notify_started) {
        s_sdk_generation++;
        if (s_sdk_generation == 0U) {
            s_sdk_generation = 1U;
        }
    }
    s_sdk_started = true;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_next_start_allowed_us = 0U;
    s_state_error_override = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (notify_started) {
        tirtc_connect_on_tirtc_started();
    }
}

bool tirtc_session_mark_sdk_stopped(uint32_t generation)
{
    bool accepted = false;

    tirtc_connect_cancel();

    taskENTER_CRITICAL(&s_rtc_lock);
    accepted = generation != 0U && generation == s_sdk_generation;
    if (!accepted) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "ignore stale rtc stopped event: event_generation=%lu current_generation=%lu",
                 (unsigned long)generation,
                 (unsigned long)s_sdk_generation);
        return false;
    }
    s_active_conn = NULL;
    s_active_conn_accepted_at_us = 0U;
    s_active_conn_supports_tgmp_bitrate = false;
    s_closing_conn = NULL;
    s_closing_conn_was_sdk_started = false;
    s_whip_connect_active_attempt = 0U;
    s_sdk_prepare_in_progress = false;
    s_sdk_started = false;
    s_pending_stop_generation = 0U;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_next_start_allowed_us = 0U;
    s_state_error_override = false;
    s_started_device_id[0] = '\0';
    s_started_credential_hash[0] = '\0';
    s_started_secret_len = 0;
    tirtc_session_reset_call_state_locked();
    s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    return true;
}

void tirtc_session_mark_sdk_network_offline(void)
{
    tirtc_connect_cancel();

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_started = false;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_whip_connect_active_attempt = 0U;
    s_next_start_allowed_us = 0U;
    s_state_error_override = false;
    s_next_connection_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
    s_active_conn_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
    s_next_connection_defer_media = false;
    s_active_conn_defer_media = false;
    s_started_device_id[0] = '\0';
    s_started_credential_hash[0] = '\0';
    s_started_secret_len = 0;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

bool tirtc_session_request_runtime_restart(const char *reason)
{
    bool wait_for_disconnect = false;
    bool request_disconnect = false;
    bool request_full_reset = false;
    bool was_sdk_started = false;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_config.enabled &&
        (s_session_mode == TIRTC_SESSION_MODE_LISTEN || s_session_mode == TIRTC_SESSION_MODE_CONNECT) &&
        s_network_connected) {
        active_conn = s_active_conn;
        closing_conn = s_closing_conn;
        if (active_conn != NULL && s_sdk_started && !s_stop_in_progress && s_closing_conn == NULL) {
            request_disconnect = true;
        } else if (closing_conn != NULL) {
            wait_for_disconnect = true;
        } else if (s_sdk_initialized || s_sdk_started || s_start_in_progress || s_sdk_prepare_in_progress) {
            request_full_reset = true;
        }
        s_restart_runtime_requested = request_disconnect || wait_for_disconnect;
        s_restart_runtime_full_requested = request_disconnect || wait_for_disconnect;
        s_force_wall_clock_sync_requested = false;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (request_disconnect) {
        tirtc_session_note_event(reason != NULL ? reason : "restart requested");
        ESP_LOGI(TAG,
                 "rtc restart requested: reason=%s action=disconnect",
                 reason != NULL ? reason : "unspecified");
        if (!tirtc_session_begin_connection_shutdown(active_conn, 0, &was_sdk_started, NULL)) {
            ESP_LOGW(TAG, "restart disconnect tracking failed hconn=%p", active_conn);
            return false;
        }

        if (!tirtc_session_enqueue_disconnect_request(active_conn, true, was_sdk_started)) {
            tirtc_session_note_event("restart disconnect drop");
            ESP_LOGW(TAG, "request disconnect for restart dropped: hconn=%p", active_conn);
            tirtc_session_complete_connection_shutdown(active_conn, was_sdk_started);
        } else {
            tirtc_session_note_event("disconnect req");
            (void)tirtc_session_schedule_disconnect_watchdog(reason, TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
        }
        return true;
    }

    if (wait_for_disconnect) {
        tirtc_session_note_event(reason != NULL ? reason : "restart requested");
        ESP_LOGD(TAG,
                 "rtc restart waiting for disconnect: reason=%s",
                 reason != NULL ? reason : "unspecified");
        (void)tirtc_session_schedule_disconnect_watchdog(reason, TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
        return true;
    }

    if (request_full_reset) {
        tirtc_session_note_event(reason != NULL ? reason : "restart requested");
        ESP_LOGI(TAG,
                 "rtc restart requested: reason=%s action=full-reset",
                 reason != NULL ? reason : "unspecified");
        if (!tirtc_session_schedule_deferred_full_reset()) {
            ESP_LOGW(TAG, "rtc full reset schedule failed");
            return false;
        }
        return true;
    }

    tirtc_session_note_event(reason != NULL ? reason : "restart ignored");
    ESP_LOGI(TAG,
             "rtc runtime restart collapsed to connect/disconnect lifecycle: reason=%s",
             reason != NULL ? reason : "unspecified");
    return true;
}

bool tirtc_session_consume_runtime_restart_request(bool *full_reset_requested)
{
    bool requested = false;
    bool full_reset = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    requested = s_restart_runtime_requested;
    full_reset = s_restart_runtime_full_requested;
    s_restart_runtime_requested = false;
    s_restart_runtime_full_requested = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (full_reset_requested != NULL) {
        *full_reset_requested = full_reset;
    }

    return requested;
}

void tirtc_session_mark_access_hijacking_detected(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.access_hijacking_detected = true;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static tirtc_session_conn_accept_result_t tirtc_session_accept_connection(
    tirtc_conn_t conn,
    bool require_connect_mode,
    bool supports_tgmp_bitrate)
{
    tirtc_session_conn_accept_result_t result = TIRTC_SESSION_CONN_ACCEPT_REJECTED;
    bool newly_accepted = false;
    bool auto_media = true;
    bool defer_media = false;
    bool stop_in_progress = false;
    bool sdk_started = false;
    bool start_in_progress = false;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    taskENTER_CRITICAL(&s_rtc_lock);
    stop_in_progress = s_stop_in_progress;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    active_conn = s_active_conn;
    closing_conn = s_closing_conn;
    if (conn != NULL && conn == s_closing_conn) {
        result = TIRTC_SESSION_CONN_ACCEPT_STALE_CLOSING;
    } else if (conn == NULL || s_stop_in_progress || s_closing_conn != NULL ||
               !s_sdk_started || s_start_in_progress ||
               (require_connect_mode && s_session_mode != TIRTC_SESSION_MODE_CONNECT)) {
        result = TIRTC_SESSION_CONN_ACCEPT_REJECTED;
    } else if (s_active_conn == NULL) {
        s_active_conn = conn;
        s_active_conn_accepted_at_us = esp_timer_get_time();
        s_active_conn_supports_tgmp_bitrate = supports_tgmp_bitrate;
        /*
         * H264 stream-start state is connection-scoped. The cumulative media
         * counters intentionally survive a call, but they must never make a
         * new peer accept a delta frame before its first IDR.
         */
        s_local_video_first_packet_logged = false;
        s_local_h264_key_frame_queued = false;
        s_local_h264_key_frame_published = false;
        s_local_h264_recovery_pending = false;
        s_local_h264_recovery_count = 0;
        s_peer_video_subscription_active = false;
        s_local_video_last_enqueue_us = 0U;
        s_local_video_last_dequeue_us = 0U;
        s_local_video_last_send_attempt_us = 0U;
        s_local_video_last_send_success_us = 0U;
        s_local_video_tx_stall_started_us = 0U;
        s_local_video_tx_last_stall_log_us = 0U;
        s_local_video_tx_stalled = false;
        s_remote_video_last_packet_us = 0U;
        s_remote_video_first_submit_attempt_us = 0U;
        s_remote_video_last_submit_us = 0U;
        s_remote_video_last_recovery_us = 0U;
        s_remote_video_last_liveness_log_us = 0U;
        s_remote_video_callback_frames = 0U;
        s_remote_video_submit_failures = 0U;
        s_remote_video_callback_gap_window_max_us = 0U;
        s_remote_video_rx_stalled = false;
        /* Send-buffer telemetry is connection-scoped; never carry a cached
         * pressure value into the first media decision of the next call. */
        s_last_send_buffer_query_conn = NULL;
        s_last_send_buffer_query_tick = 0;
        s_stats.send_buffer_used = 0;
        s_active_conn_auto_media = s_next_connection_auto_media;
        s_active_conn_defer_media = s_next_connection_defer_media;
        s_next_connection_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
        s_next_connection_defer_media = false;
        auto_media = s_active_conn_auto_media;
        defer_media = s_active_conn_defer_media;
        result = TIRTC_SESSION_CONN_ACCEPTED;
        newly_accepted = true;
    } else if (s_active_conn == conn) {
        auto_media = s_active_conn_auto_media;
        defer_media = s_active_conn_defer_media;
        result = TIRTC_SESSION_CONN_ACCEPTED;
    }
    if (result == TIRTC_SESSION_CONN_ACCEPTED) {
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (result == TIRTC_SESSION_CONN_ACCEPT_REJECTED) {
        ESP_LOGW(TAG,
                 "rtc connection rejected: hconn=%p active=%p closing=%p sdk_started=%d start=%d stop=%d",
                 conn,
                 active_conn,
                 closing_conn,
                 sdk_started,
                 start_in_progress,
                 stop_in_progress);
    } else if (result == TIRTC_SESSION_CONN_ACCEPT_STALE_CLOSING) {
        APP_LOG_DETAIL(TAG,
                       "rtc connection accept ignored: hconn=%p already closing",
                       conn);
    } else if (newly_accepted) {
        APP_LOG_DETAIL(TAG,
                       "track accepted connection hconn=%p accepted_at_us=%llu auto_media=%d defer_media=%d",
                       conn,
                       (unsigned long long)s_active_conn_accepted_at_us,
                       auto_media ? 1 : 0,
                       defer_media ? 1 : 0);
    }

    return result;
}

void tirtc_session_set_next_connection_auto_media(bool enabled)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_next_connection_auto_media = enabled;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_next_connection_defer_media(bool enabled)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_next_connection_defer_media = enabled;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

bool tirtc_session_connection_auto_media_enabled(tirtc_conn_t conn)
{
    bool enabled = true;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL && conn == s_active_conn) {
        enabled = s_active_conn_auto_media;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return enabled;
}

bool tirtc_session_connection_media_deferred(tirtc_conn_t conn)
{
    bool deferred = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn != NULL && conn == s_active_conn) {
        deferred = s_active_conn_defer_media;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return deferred;
}

esp_err_t tirtc_session_track_external_connection(tirtc_conn_t conn, bool auto_media)
{
    if (conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tirtc_session_set_next_connection_auto_media(auto_media);
    if (tirtc_session_accept_connection(conn, false, false) != TIRTC_SESSION_CONN_ACCEPTED) {
        tirtc_session_set_next_connection_auto_media(TIRTC_SESSION_DEFAULT_AUTO_MEDIA);
        return ESP_ERR_INVALID_STATE;
    }
    tirtc_session_set_next_connection_auto_media(TIRTC_SESSION_DEFAULT_AUTO_MEDIA);
    tirtc_session_bind_connection_user_data(conn);

    if (!auto_media) {
        APP_LOG_DETAIL(TAG, "external connection uses external media owner: hconn=%p", conn);
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
        .payload.conn = {
            .conn = conn,
            .error = 0,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("external accept inline");
        ESP_LOGW(TAG, "rtc event queue full: external connection handled inline");
        tirtc_session_handle_runtime_event(&rtc_event);
    }

    return ESP_OK;
}

void tirtc_session_update_local_video_subscription(tirtc_conn_t conn, uint8_t stream_id, bool subscribed)
{
    uint8_t normalized_stream_id = tirtc_session_normalize_local_video_stream_id(stream_id);
    bool request_key_frame = false;
    bool stream_start_key_frame = false;
    bool flush_video_queue = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        if (subscribed) {
            /* A key-frame request can arrive before the SDK's subscribe
             * callback and also sets s_peer_wants_video. Keep the actual
             * subscription edge separate so that this callback always starts
             * the new decoder epoch with a fresh SPS/PPS/IDR. */
            bool new_subscription = !s_peer_video_subscription_active ||
                                    s_local_video_stream_id != normalized_stream_id;
            s_peer_video_subscription_active = true;
            s_local_video_stream_id = normalized_stream_id;
            s_peer_wants_video = true;
            s_peer_video_control_seen = true;
            tirtc_session_mark_local_video_requested_locked();
            s_local_video_publish_forced = false;
            s_test_video_publish_forced = false;
            if (new_subscription) {
                s_local_h264_key_frame_queued = false;
                s_local_h264_key_frame_published = false;
                s_local_h264_recovery_pending = false;
                flush_video_queue = true;
                stream_start_key_frame = true;
            }
            tirtc_session_sync_stats_locked();
            request_key_frame = true;
        } else {
            if (s_local_video_stream_id == normalized_stream_id) {
                s_peer_video_subscription_active = false;
                s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
                s_peer_wants_video = false;
                s_peer_video_control_seen = true;
                s_local_video_publish_forced = false;
                s_test_video_publish_forced = false;
                s_local_h264_key_frame_queued = false;
                s_local_h264_key_frame_published = false;
                s_local_h264_recovery_pending = false;
                flush_video_queue = true;
                tirtc_session_sync_stats_locked();
            }
        }
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (flush_video_queue) {
        tirtc_session_flush_local_video_tx_queue();
    }
    if (stream_start_key_frame) {
        tirtc_session_media_request_video_stream_start_key_frame();
    } else if (request_key_frame) {
        tirtc_session_media_request_video_key_frame();
    }
}

void tirtc_session_update_local_audio_subscription(tirtc_conn_t conn, uint8_t stream_id, bool subscribed)
{
    uint8_t normalized_stream_id = tirtc_session_normalize_local_audio_stream_id(stream_id);

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        if (subscribed) {
            s_local_audio_stream_id = normalized_stream_id;
            s_peer_wants_audio = true;
            s_peer_audio_control_seen = true;
            s_local_audio_publish_forced = false;
            s_test_audio_publish_forced = false;
            tirtc_session_sync_stats_locked();
        } else if (s_local_audio_stream_id == normalized_stream_id) {
            s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
            s_peer_wants_audio = false;
            s_peer_audio_control_seen = true;
            s_local_audio_publish_forced = false;
            s_test_audio_publish_forced = false;
            tirtc_session_sync_stats_locked();
        }
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static bool tirtc_session_begin_connection_shutdown(tirtc_conn_t hconn,
                                                   int error,
                                                   bool *was_sdk_started,
                                                   bool *newly_detached_out)
{
    bool tracked = false;
    bool newly_detached = false;
    bool sdk_started = false;
    bool call_media_was_active = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    sdk_started = s_sdk_started;
    if (hconn != NULL && hconn == s_active_conn) {
        call_media_was_active = s_call_active && !s_call_media_deferred;
        s_active_conn = NULL;
        s_active_conn_accepted_at_us = 0U;
        s_active_conn_supports_tgmp_bitrate = false;
        s_active_conn_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
        s_active_conn_defer_media = false;
        s_closing_conn = hconn;
        s_closing_conn_was_sdk_started = sdk_started;
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_local_audio_publish_forced = false;
        s_test_video_publish_forced = false;
        s_test_audio_publish_forced = false;
        tirtc_session_reset_call_state_locked();
        if (error != 0) {
            s_stats.last_error = error;
        }
        tirtc_session_sync_stats_locked();
        tracked = true;
        newly_detached = true;
    } else if (hconn != NULL && hconn == s_closing_conn) {
        if (error != 0) {
            s_stats.last_error = error;
        }
        tirtc_session_sync_stats_locked();
        tracked = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (was_sdk_started != NULL) {
        *was_sdk_started = sdk_started;
    }
    if (newly_detached_out != NULL) {
        *newly_detached_out = newly_detached;
    }

    if (tracked) {
        tirtc_session_cancel_media_bootstrap();
    }

    if (newly_detached) {
        tirtc_session_stop_time_stream_messages();
        tirtc_session_apply_local_media_policy();
        tirtc_session_flush_local_video_tx_queue();
        tirtc_session_flush_local_audio_tx_queue();
        tirtc_session_media_flush();
    }
    if (newly_detached && call_media_was_active) {
        tirtc_session_notify_call_active(false);
    }

    return tracked;
}

static bool tirtc_session_complete_connection_shutdown(tirtc_conn_t hconn, bool was_sdk_started)
{
    bool completed = false;
    bool restart_pending = false;

    tirtc_session_stop_disconnect_watchdog();

    taskENTER_CRITICAL(&s_rtc_lock);
    if (hconn != NULL && hconn == s_closing_conn) {
        s_closing_conn = NULL;
        s_closing_conn_was_sdk_started = false;
        completed = true;
    }
    if (completed && !was_sdk_started) {
        s_stop_in_progress = false;
    }
    if (completed) {
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!completed) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    restart_pending = s_restart_runtime_requested || s_restart_runtime_full_requested;
    taskEXIT_CRITICAL(&s_rtc_lock);

    /* Connection teardown returns to waiting; only explicit runtime restart crosses into SDK restart. */
    if (was_sdk_started && restart_pending &&
        tirtc_session_request_runtime_restart("restart after explicit disconnect")) {
        ESP_LOGI(TAG, "rtc restart resumed after disconnect");
        return true;
    }

    tirtc_session_return_to_listen_mode();

    ESP_LOGI(TAG,
             "rtc disconnected: next_state=%s",
             was_sdk_started ? "READY" : "STOPPED");
    return true;
}

static bool tirtc_session_is_ready_to_send_video(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && s_local_video_send_enabled &&
            (s_peer_wants_video || s_local_video_publish_forced) &&
            s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_video_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_external_video(tirtc_conn_t expected_conn,
                                                          uint8_t expected_stream_id,
                                                          tirtc_conn_t *conn,
                                                          uint8_t *stream_id)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = expected_conn != NULL &&
            expected_stream_id != TIRTC_SESSION_INVALID_STREAM_ID &&
            s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
            s_closing_conn == NULL &&
            s_active_conn == expected_conn &&
            s_external_video_active &&
            s_external_video_conn == expected_conn &&
            s_external_video_stream_id == expected_stream_id;
    if (ready) {
        *conn = expected_conn;
        *stream_id = expected_stream_id;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_audio(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && s_local_audio_send_enabled &&
            !s_media_bootstrap_pending &&
            s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_audio_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_test_video(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;
    uint64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && s_local_video_send_enabled &&
            !s_media_bootstrap_pending &&
            tirtc_session_is_test_media_window_open_locked(now_us, s_test_video_retry_after_us);
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_video_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_test_audio(tirtc_conn_t *conn, uint8_t *stream_id)
{
    bool ready = false;
    uint64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && s_call_active && s_local_audio_send_enabled &&
            !s_media_bootstrap_pending &&
            tirtc_session_is_test_media_window_open_locked(now_us, 0U);
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_audio_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_call_audio(tirtc_conn_t expected_conn,
                                                      tirtc_conn_t *conn,
                                                      uint8_t *stream_id)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    ready = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
            s_active_conn != NULL && (expected_conn == NULL || expected_conn == s_active_conn) &&
            s_call_active && s_local_audio_send_enabled && !s_media_bootstrap_pending;
    if (ready) {
        *conn = s_active_conn;
        *stream_id = tirtc_session_get_effective_local_audio_stream_id_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ready;
}

static bool tirtc_session_is_ready_to_send_audio_with_gate(tirtc_session_audio_tx_gate_t gate,
                                                           tirtc_conn_t expected_conn,
                                                           tirtc_conn_t *conn,
                                                           uint8_t *stream_id)
{
    switch (gate) {
    case TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED:
        return tirtc_session_is_ready_to_send_audio(conn, stream_id);
    case TIRTC_SESSION_AUDIO_TX_GATE_CALL:
        return tirtc_session_is_ready_to_send_call_audio(expected_conn, conn, stream_id);
    case TIRTC_SESSION_AUDIO_TX_GATE_TEST:
        return tirtc_session_is_ready_to_send_test_audio(conn, stream_id);
    default:
        return false;
    }
}

static uint32_t tirtc_session_get_local_audio_tx_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    generation = s_local_audio_tx_generation;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return generation;
}

static uint32_t tirtc_session_get_local_video_tx_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    generation = s_local_video_tx_generation;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return generation;
}

static void tirtc_session_free_local_video_packet(tirtc_session_local_video_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    if (packet->buffer_slot != TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID) {
        tirtc_session_release_local_video_buffer_slot(packet->buffer_slot);
    } else {
        free(packet->data);
    }
    memset(packet, 0, sizeof(*packet));
    packet->buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
}

static void tirtc_session_free_local_audio_packet(tirtc_session_local_audio_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    if (packet->buffer_slot != TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID) {
        tirtc_session_release_local_audio_buffer_slot(packet->buffer_slot);
    } else {
        free(packet->data);
    }
    memset(packet, 0, sizeof(*packet));
    packet->buffer_slot = TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID;
}

static void tirtc_session_drop_oldest_local_video_packet(void)
{
    tirtc_session_local_video_packet_t stale = {0};

    if (s_local_video_tx_queue != NULL && xQueueReceive(s_local_video_tx_queue, &stale, 0) == pdTRUE) {
        tirtc_session_free_local_video_packet(&stale);
    }
}

static bool tirtc_session_local_video_stream_started(void)
{
    bool started = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    started = s_local_h264_key_frame_queued ||
              s_local_h264_key_frame_published;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return started;
}

static bool tirtc_session_local_h264_key_frame_published(void)
{
    bool published = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    published = s_local_h264_key_frame_published;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return published;
}

static void tirtc_session_mark_local_h264_key_frame_queued(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_h264_key_frame_queued = true;
    s_local_h264_recovery_pending = false;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_mark_local_h264_key_frame_published(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_h264_key_frame_queued = false;
    s_local_h264_key_frame_published = true;
    s_local_h264_recovery_pending = false;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_trim_local_video_tx_queue(UBaseType_t max_packets)
{
    while (s_local_video_tx_queue != NULL &&
           uxQueueMessagesWaiting(s_local_video_tx_queue) >= max_packets) {
        tirtc_session_drop_oldest_local_video_packet();
    }
}

static void tirtc_session_drop_oldest_local_audio_packet(void)
{
    tirtc_session_local_audio_packet_t stale = {
        .buffer_slot = TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID,
    };

    if (s_local_audio_tx_queue != NULL && xQueueReceive(s_local_audio_tx_queue, &stale, 0) == pdTRUE) {
        tirtc_session_free_local_audio_packet(&stale);
    }
}

static void tirtc_session_trim_local_audio_tx_queue(UBaseType_t max_packets)
{
    while (s_local_audio_tx_queue != NULL &&
           uxQueueMessagesWaiting(s_local_audio_tx_queue) >= max_packets) {
        tirtc_session_drop_oldest_local_audio_packet();
    }
}

static esp_err_t tirtc_session_acquire_local_video_buffer_slot(uint8_t *slot_out)
{
    uint8_t slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;

    if (slot_out == NULL || s_local_video_tx_free_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueReceive(s_local_video_tx_free_queue, &slot, 0) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }

    *slot_out = slot;
    return ESP_OK;
}

static void tirtc_session_release_local_video_buffer_slot(uint8_t slot)
{
    if (slot >= TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE || s_local_video_tx_free_queue == NULL) {
        return;
    }

    (void)xQueueSend(s_local_video_tx_free_queue, &slot, 0);
}

static esp_err_t tirtc_session_acquire_local_audio_buffer_slot(uint8_t *slot_out)
{
    uint8_t slot = TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID;

    if (slot_out == NULL || s_local_audio_tx_free_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueReceive(s_local_audio_tx_free_queue, &slot, 0) != pdTRUE) {
        return ESP_ERR_NOT_FOUND;
    }

    *slot_out = slot;
    return ESP_OK;
}

static void tirtc_session_release_local_audio_buffer_slot(uint8_t slot)
{
    if (slot >= TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE || s_local_audio_tx_free_queue == NULL) {
        return;
    }

    (void)xQueueSend(s_local_audio_tx_free_queue, &slot, 0);
}

static void tirtc_session_maybe_log_local_video_tx_pool(uint8_t slot,
                                                        size_t frame_size,
                                                        bool capacity_changed)
{
    TickType_t now_tick = xTaskGetTickCount();
    if (!capacity_changed) {
        return;
    }
    if (s_last_local_video_tx_pool_log_tick != 0 &&
        now_tick - s_last_local_video_tx_pool_log_tick < pdMS_TO_TICKS(5000)) {
        return;
    }

    size_t total_capacity = 0;
    size_t largest_slot = 0;
    for (size_t index = 0; index < TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE; ++index) {
        total_capacity += s_local_video_tx_buffer_capacities[index];
        if (s_local_video_tx_buffer_capacities[index] > largest_slot) {
            largest_slot = s_local_video_tx_buffer_capacities[index];
        }
    }

    UBaseType_t queued = s_local_video_tx_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_queue) : 0;
    UBaseType_t free_slots =
        s_local_video_tx_free_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0;
    s_last_local_video_tx_pool_log_tick = now_tick;
    ESP_LOGI(TAG,
             "local video tx pool: frame=%u slot=%u slot_cap=%u total_cap=%u largest_slot=%u queued=%u free=%u",
             (unsigned)frame_size,
             (unsigned)slot,
             (unsigned)(slot < TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE ?
                            s_local_video_tx_buffer_capacities[slot] : 0),
             (unsigned)total_capacity,
             (unsigned)largest_slot,
             (unsigned)queued,
             (unsigned)free_slots);
}

static esp_err_t tirtc_session_ensure_local_video_buffer_capacity(uint8_t slot, size_t required_size)
{
    uint8_t *new_buffer = NULL;
    size_t alloc_size = required_size;

    if (slot >= TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (required_size <= s_local_video_tx_buffer_capacities[slot]) {
        return ESP_OK;
    }

    if (alloc_size < TIRTC_SESSION_VIDEO_TX_PREALLOC_BYTES) {
        alloc_size = TIRTC_SESSION_VIDEO_TX_PREALLOC_BYTES;
    }
    if (TIRTC_SESSION_VIDEO_TX_ALLOC_ALIGN_BYTES > 0U) {
        alloc_size = (alloc_size + TIRTC_SESSION_VIDEO_TX_ALLOC_ALIGN_BYTES - 1U) &
                     ~(TIRTC_SESSION_VIDEO_TX_ALLOC_ALIGN_BYTES - 1U);
    }

    new_buffer = tirtc_session_alloc_video_tx_buffer(alloc_size);
    ESP_RETURN_ON_FALSE(new_buffer != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "local video pool alloc failed size=%u psram_largest=%u",
                        (unsigned)alloc_size,
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    free(s_local_video_tx_buffers[slot]);
    s_local_video_tx_buffers[slot] = new_buffer;
    s_local_video_tx_buffer_capacities[slot] = alloc_size;
    return ESP_OK;
}

static esp_err_t tirtc_session_enqueue_local_video_packet(const uint8_t *data,
                                                         size_t data_len,
                                                         uint16_t width,
                                                         uint16_t height,
                                                         uint64_t pts_us,
                                                         uint8_t media,
                                                         uint8_t flags,
                                                         const TIRTCFRAMEINFO *frame_info,
                                                         bool test_frame,
                                                         bool external_frame,
                                                         tirtc_conn_t expected_conn,
                                                         uint8_t external_stream_id)
{
    const uint8_t effective_media = frame_info != NULL ? frame_info->media : media;
    const uint8_t effective_flags = frame_info != NULL ? frame_info->flags : flags;
    const bool local_h264 = !test_frame && effective_media == TIRTC_VIDEO_H264;
    const bool h264_key_frame = local_h264 &&
                                (effective_flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0;
    bool video_stream_started = false;
    bool arm_initial_h264_key = false;
    tirtc_session_local_video_packet_t packet = {
        .generation = tirtc_session_get_local_video_tx_generation(),
        .expected_conn = expected_conn,
        .width = width,
        .height = height,
        .pts_us = pts_us,
        .buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID,
        .data_len = data_len,
        .media = media,
        .flags = flags,
        .has_frame_info = frame_info != NULL,
        .test_frame = test_frame,
        .external_frame = external_frame,
        .external_stream_id = external_stream_id,
    };
    uint8_t buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
    esp_err_t slot_ret = ESP_OK;
    size_t old_slot_capacity = 0;

    if (test_frame && external_frame) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_local_video_tx_queue == NULL || s_local_video_tx_free_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!test_frame) {
        video_stream_started = tirtc_session_local_video_stream_started();
    }

    if (local_h264 && !video_stream_started) {
        if (!h264_key_frame) {
            return ESP_ERR_INVALID_STATE;
        }
        tirtc_session_flush_local_video_tx_queue();
        packet.generation = tirtc_session_get_local_video_tx_generation();
        arm_initial_h264_key = true;
    }

    if (!test_frame && !local_h264) {
        tirtc_session_trim_local_video_tx_queue(TIRTC_SESSION_VIDEO_TX_TARGET_BACKLOG);
    }

    slot_ret = tirtc_session_acquire_local_video_buffer_slot(&buffer_slot);
    if (slot_ret != ESP_OK) {
        if (local_h264) {
            tirtc_session_recover_local_h264_stream("tx pool exhausted", true);
            return slot_ret;
        }
        tirtc_session_drop_oldest_local_video_packet();
        slot_ret = tirtc_session_acquire_local_video_buffer_slot(&buffer_slot);
        if (slot_ret != ESP_OK) {
            return slot_ret;
        }
    }

    old_slot_capacity = s_local_video_tx_buffer_capacities[buffer_slot];
    slot_ret = tirtc_session_ensure_local_video_buffer_capacity(buffer_slot, data_len);
    if (slot_ret != ESP_OK) {
        tirtc_session_release_local_video_buffer_slot(buffer_slot);
        if (local_h264) {
            tirtc_session_recover_local_h264_stream("tx slot resize failed", false);
        }
        return slot_ret;
    }

    packet.buffer_slot = buffer_slot;
    packet.data = s_local_video_tx_buffers[buffer_slot];
    if (frame_info != NULL) {
        packet.frame_info = *frame_info;
    }
    memcpy(packet.data, data, data_len);
    tirtc_session_maybe_log_local_video_tx_pool(buffer_slot,
                                                data_len,
                                                s_local_video_tx_buffer_capacities[buffer_slot] != old_slot_capacity);

    if (arm_initial_h264_key) {
        /*
         * Publish the queued state before xQueueSend. The TX task can run as
         * soon as the item enters the queue and must not race a later flag
         * update that would put the stream back into WAIT_KEY_FRAME.
         */
        tirtc_session_mark_local_h264_key_frame_queued();
    }
    packet.queued_at_us = (uint64_t)esp_timer_get_time();
    if (xQueueSend(s_local_video_tx_queue, &packet, 0) == pdTRUE) {
        if (!test_frame) {
            taskENTER_CRITICAL(&s_rtc_lock);
            s_local_video_last_enqueue_us = packet.queued_at_us;
            taskEXIT_CRITICAL(&s_rtc_lock);
        }
        memset(&packet, 0, sizeof(packet));
        packet.buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
        return ESP_OK;
    }

    if (local_h264) {
        tirtc_session_free_local_video_packet(&packet);
        tirtc_session_recover_local_h264_stream("tx queue full", true);
        return ESP_ERR_TIMEOUT;
    }

    tirtc_session_drop_oldest_local_video_packet();
    packet.queued_at_us = (uint64_t)esp_timer_get_time();
    if (xQueueSend(s_local_video_tx_queue, &packet, 0) == pdTRUE) {
        if (!test_frame) {
            taskENTER_CRITICAL(&s_rtc_lock);
            s_local_video_last_enqueue_us = packet.queued_at_us;
            taskEXIT_CRITICAL(&s_rtc_lock);
        }
        memset(&packet, 0, sizeof(packet));
        packet.buffer_slot = TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID;
        return ESP_OK;
    }

    tirtc_session_free_local_video_packet(&packet);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t tirtc_session_enqueue_local_audio_packet(const uint8_t *data,
                                                         size_t data_len,
                                                         const tirtc_session_audio_format_t *format,
                                                         uint64_t pts_us,
                                                         tirtc_session_audio_tx_gate_t gate,
                                                         tirtc_conn_t expected_conn)
{
    tirtc_session_local_audio_packet_t packet = {
        .generation = tirtc_session_get_local_audio_tx_generation(),
        .expected_conn = expected_conn,
        .pts_us = pts_us,
        .buffer_slot = TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID,
        .data_len = data_len,
        .gate = gate,
    };

    ESP_RETURN_ON_FALSE(format != NULL, ESP_ERR_INVALID_ARG, TAG, "local audio format required");
    packet.format = *format;

    if (s_local_audio_tx_queue == NULL || s_local_audio_tx_free_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(data_len <= TIRTC_SESSION_AUDIO_TX_BUFFER_BYTES,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "local audio packet too large: size=%u max=%u",
                        (unsigned)data_len,
                        (unsigned)TIRTC_SESSION_AUDIO_TX_BUFFER_BYTES);

    tirtc_session_trim_local_audio_tx_queue(TIRTC_SESSION_AUDIO_TX_TARGET_BACKLOG);

    esp_err_t slot_ret = tirtc_session_acquire_local_audio_buffer_slot(&packet.buffer_slot);
    if (slot_ret != ESP_OK) {
        tirtc_session_drop_oldest_local_audio_packet();
        slot_ret = tirtc_session_acquire_local_audio_buffer_slot(&packet.buffer_slot);
    }
    ESP_RETURN_ON_ERROR(slot_ret, TAG, "local audio tx pool exhausted");

    packet.data = s_local_audio_tx_buffers[packet.buffer_slot];
    memcpy(packet.data, data, data_len);

    if (xQueueSend(s_local_audio_tx_queue, &packet, 0) == pdTRUE) {
        memset(&packet, 0, sizeof(packet));
        return ESP_OK;
    }

    tirtc_session_drop_oldest_local_audio_packet();
    if (xQueueSend(s_local_audio_tx_queue, &packet, 0) == pdTRUE) {
        memset(&packet, 0, sizeof(packet));
        return ESP_OK;
    }

    tirtc_session_free_local_audio_packet(&packet);
    return ESP_ERR_TIMEOUT;
}

static void tirtc_session_flush_local_video_tx_queue(void)
{
    tirtc_session_local_video_packet_t packet = {0};

    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_video_tx_generation++;
    taskEXIT_CRITICAL(&s_rtc_lock);

    while (s_local_video_tx_queue != NULL && xQueueReceive(s_local_video_tx_queue, &packet, 0) == pdTRUE) {
        tirtc_session_free_local_video_packet(&packet);
    }
}

static void tirtc_session_recover_local_h264_stream(const char *reason,
                                                     bool network_backpressure)
{
    bool start_recovery = false;
    uint32_t recovery_count = 0;

    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_h264_key_frame_queued = false;
    s_local_h264_key_frame_published = false;
    if (!s_local_h264_recovery_pending) {
        s_local_h264_recovery_pending = true;
        s_local_h264_recovery_count++;
        recovery_count = s_local_h264_recovery_count;
        start_recovery = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!start_recovery) {
        return;
    }

    /*
     * Once any encoded H264 frame is lost, later P-frames may reference data
     * the decoder never received. Drop the remaining delta chain and resume
     * only from a freshly requested IDR.
     */
    tirtc_session_flush_local_video_tx_queue();
    tirtc_session_note_event("h264 recovery");
    if (network_backpressure) {
        media_governor_note_network_backpressure();
    }
    APP_LOG_DETAIL(TAG,
                   "local H264 continuity recovery: reason=%s count=%lu backpressure=%d",
                   reason != NULL ? reason : "unspecified",
                   (unsigned long)recovery_count,
                   network_backpressure ? 1 : 0);
    /* A broken local reference chain is a stream restart, not a duplicate PLI.
     * Request an immediate IDR instead of waiting for the normal 2 s debounce
     * or the next GOP boundary. */
    tirtc_session_media_request_video_stream_start_key_frame();
}

static void tirtc_session_flush_local_audio_tx_queue(void)
{
    tirtc_session_local_audio_packet_t packet = {
        .buffer_slot = TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID,
    };

    taskENTER_CRITICAL(&s_rtc_lock);
    s_local_audio_tx_generation++;
    s_local_audio_tx_window_frames = 0;
    s_local_audio_tx_window_payload_bytes = 0;
    s_local_audio_tx_window_peak_percent = 0;
    s_last_local_audio_tx_log_tick = 0;
    taskEXIT_CRITICAL(&s_rtc_lock);

    while (s_local_audio_tx_queue != NULL && xQueueReceive(s_local_audio_tx_queue, &packet, 0) == pdTRUE) {
        tirtc_session_free_local_audio_packet(&packet);
    }
}

static bool tirtc_session_enqueue_event(const tirtc_session_event_t *event, TickType_t wait_ticks)
{
    if (s_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueSend(s_event_queue, event, wait_ticks) == pdTRUE;
}

static bool tirtc_session_enqueue_start_if_ready(void)
{
    tirtc_session_event_t event = {
        .type = TIRTC_SESSION_EVENT_START_IF_READY,
    };

    return tirtc_session_enqueue_event(&event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS);
}

static bool tirtc_session_enqueue_teardown_event(const tirtc_session_event_t *event)
{
    return tirtc_session_enqueue_event(event, TIRTC_SESSION_TEARDOWN_EVENT_WAIT_TICKS);
}

static bool tirtc_session_enqueue_disconnect_request(tirtc_conn_t conn,
                                                     bool complete_shutdown,
                                                     bool was_sdk_started)
{
    tirtc_session_event_t event = {
        .type = TIRTC_SESSION_EVENT_DISCONNECT_REQUEST,
    };

    event.payload.disconnect.conn = conn;
    event.payload.disconnect.complete_shutdown = complete_shutdown;
    event.payload.disconnect.was_sdk_started = was_sdk_started;

    return tirtc_session_enqueue_teardown_event(&event);
}

static void tirtc_session_copy_config_snapshot(tirtc_session_config_t *config)
{
    if (config == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    *config = s_config;
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static bool tirtc_session_config_differs(const tirtc_session_config_t *left,
                                         const tirtc_session_config_t *right)
{
    if (left == NULL || right == NULL) {
        return true;
    }

    return left->enabled != right->enabled ||
           left->default_session_mode != right->default_session_mode ||
           strcmp(left->service_endpoint, right->service_endpoint) != 0 ||
           strcmp(left->device_id, right->device_id) != 0 ||
           strcmp(left->client_id, right->client_id) != 0 ||
           strcmp(left->device_license, right->device_license) != 0 ||
           strcmp(left->device_secret_key, right->device_secret_key) != 0 ||
           strcmp(left->remote_device_id, right->remote_device_id) != 0 ||
           strcmp(left->remote_device_secret_key, right->remote_device_secret_key) != 0 ||
           strcmp(left->token_access_id, right->token_access_id) != 0 ||
           strcmp(left->token_secret_key, right->token_secret_key) != 0 ||
           strcmp(left->token_subject, right->token_subject) != 0 ||
           left->token_ttl_seconds != right->token_ttl_seconds;
}

static bool tirtc_session_extract_start_identity(const tirtc_session_config_t *config,
                                                 char *device_id,
                                                 size_t device_id_size,
                                                 char *secret_key,
                                                 size_t secret_key_size)
{
    if (config == NULL || device_id == NULL || secret_key == NULL ||
        device_id_size == 0 || secret_key_size == 0) {
        return false;
    }

    device_id[0] = '\0';
    secret_key[0] = '\0';
    strlcpy(device_id, config->device_id, device_id_size);
    strlcpy(secret_key, config->device_secret_key, secret_key_size);

    if (config->device_license[0] != '\0') {
        const char *comma = strchr(config->device_license, ',');
        if (comma != NULL) {
            if (device_id[0] == '\0') {
                size_t id_len = (size_t)(comma - config->device_license);
                if (id_len >= device_id_size) {
                    id_len = device_id_size - 1U;
                }
                memcpy(device_id, config->device_license, id_len);
                device_id[id_len] = '\0';
            }
            if (secret_key[0] == '\0') {
                strlcpy(secret_key, comma + 1, secret_key_size);
            }
        } else if (device_id[0] == '\0') {
            strlcpy(device_id, config->device_license, device_id_size);
        }
    }

    return device_id[0] != '\0' && secret_key[0] != '\0';
}

static void tirtc_session_clear_start_in_progress(void)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    s_start_in_progress = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static void tirtc_session_handle_disconnect_request(const tirtc_session_event_t *event)
{
    tirtc_conn_t conn = NULL;
    bool complete_shutdown = false;
    bool was_sdk_started = false;

    if (event == NULL) {
        return;
    }

    conn = event->payload.disconnect.conn;
    complete_shutdown = event->payload.disconnect.complete_shutdown;
    was_sdk_started = event->payload.disconnect.was_sdk_started;
    if (conn == NULL) {
        return;
    }

    int ret = tirtc_session_disconnect_with_sdk_lock(conn);
    if (ret < 0) {
        tirtc_session_set_last_error(ret);
        ESP_LOGW(TAG,
                 "rtc async disconnect failed: hconn=%p err=%s",
                 conn,
                 TiRtcGetErrorStr(ret));
        if (complete_shutdown) {
            tirtc_session_complete_connection_shutdown(conn, was_sdk_started);
        }
        return;
    }

    if (complete_shutdown) {
        tirtc_session_note_event("disconnect req");
        (void)tirtc_session_schedule_disconnect_watchdog("manual disconnect",
                                                         TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
    }
}

static esp_err_t tirtc_session_copy_payload(const void *data, size_t data_len, uint8_t **out_copy)
{
    if (out_copy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_copy = NULL;
    if (data == NULL || data_len == 0) {
        return ESP_OK;
    }

    uint8_t *copy = app_memory_alloc_psram(data_len);
    ESP_RETURN_ON_FALSE(copy != NULL, ESP_ERR_NO_MEM, TAG, "payload copy alloc failed");
    memcpy(copy, data, data_len);
    *out_copy = copy;
    return ESP_OK;
}

static bool tirtc_session_service_endpoint_is_valid(const tirtc_session_config_t *config)
{
    size_t length = strnlen(config->service_endpoint, sizeof(config->service_endpoint));
    if (length == 0) {
        return !config->enabled;
    }
    if (length >= sizeof(config->service_endpoint) ||
        strncmp(config->service_endpoint, "https://", 8) != 0) {
        return false;
    }

    // Keep TLS authentication in the SDK; never rewrite or downgrade the URL.
    struct http_parser_url url;
    http_parser_url_init(&url);
    return http_parser_parse_url(config->service_endpoint, length, 0, &url) == 0 &&
           (url.field_set & (1U << UF_HOST)) != 0 &&
           url.field_data[UF_HOST].len != 0 &&
           (url.field_set & ((1U << UF_USERINFO) | (1U << UF_FRAGMENT))) == 0 &&
           ((url.field_set & (1U << UF_PORT)) == 0 || url.port != 0);
}

static esp_err_t tirtc_session_ensure_platform_ready(void)
{
    int ret;

    if (s_platform_initialized) {
        return ESP_OK;
    }

    ret = SA_platInit();
    if (ret == 0) {
        ESP_LOGE(TAG, "rtc platform init failed: ret=%d", ret);
        return ESP_FAIL;
    }

    s_platform_initialized = true;
    return ESP_OK;
}

static esp_err_t tirtc_session_prepare_sdk_with_lock(void)
{
    int option_ret = 0;
    uint32_t max_send_buffer = TIRTC_SESSION_MAX_SEND_BUFFER;
    int connect_cache_enabled = TIRTC_SESSION_CONNECT_CACHE_ENABLE;

    if (s_sdk_initialized) {
        return ESP_OK;
    }

    tirtc_session_configure_sdk_logs(true);

    option_ret = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                                &max_send_buffer,
                                (uint32_t)sizeof(max_send_buffer));
    if (option_ret != 0) {
        tirtc_session_set_last_error(option_ret);
        tirtc_session_note_event("set send buffer failed");
        ESP_LOGE(TAG,
                 "TiRtcSetOption(MAX_SEND_BUFFER) failed: %s (%d)",
                 TiRtcGetErrorStr(option_ret),
                 option_ret);
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    APP_LOG_DETAIL(TAG,
                   "rtc sdk send buffer configured: %u bytes",
                   (unsigned)max_send_buffer);

    option_ret = TiRtcSetOption(TIRTC_OPT_CONNECT_CACHE,
                                &connect_cache_enabled,
                                (uint32_t)sizeof(connect_cache_enabled));
    if (option_ret != 0) {
        tirtc_session_set_last_error(option_ret);
        tirtc_session_note_event("set connect cache failed");
        ESP_LOGE(TAG,
                 "TiRtcSetOption(CONNECT_CACHE) failed: %s (%d)",
                 TiRtcGetErrorStr(option_ret),
                 option_ret);
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    APP_LOG_DETAIL(TAG,
                   "rtc sdk connect cache configured: enabled=%d",
                   connect_cache_enabled);

    if (s_config.service_endpoint[0] != '\0') {
        option_ret = TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT,
                                    s_config.service_endpoint,
                                    (uint32_t)(strlen(s_config.service_endpoint) + 1U));
        if (option_ret != 0) {
            tirtc_session_set_last_error(option_ret);
            tirtc_session_note_event("set endpoint failed");
            ESP_LOGE(TAG, "TiRtcSetOption(SERVICE_ENDPOINT) failed: %s (%d)", TiRtcGetErrorStr(option_ret), option_ret);
            tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
            return ESP_FAIL;
        }
    }

    APP_LOG_DETAIL(TAG, "rtc sdk init stage: TiRtcInit begin");
    int init_ret = TiRtcInit();
    if (init_ret < 0) {
        tirtc_session_set_last_error(init_ret);
        tirtc_session_note_event("init failed");
        ESP_LOGE(TAG, "TiRtcInit failed: %s", TiRtcGetErrorStr(init_ret));
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    const char *sdk_version = TiRtcGetVersion();
    const char *sdk_build_info = TiRtcGetBuildInfo();
    ESP_LOGI(TAG,
             "rtc sdk loaded: version=%s",
             sdk_version != NULL ? sdk_version : "unknown");
    APP_LOG_DETAIL(TAG,
                   "rtc sdk build: %s",
                   sdk_build_info != NULL ? sdk_build_info : "unknown");
    APP_LOG_DETAIL(TAG, "rtc sdk init stage: TiRtcInit done");

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_initialized = true;
    s_sdk_started = false;
    s_start_in_progress = false;
    s_stop_in_progress = false;
    s_sdk_stop_notified = true;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    ESP_LOGI(TAG, "rtc sdk init ready: TiRtcInit complete, listen start can be queued");
    return ESP_OK;
}

static const char *tirtc_session_start_error_name(int error)
{
    if (error == TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT) {
        return "TIRTC_SERVICE_CLIENT_ID_CONFLICT";
    }
    return TiRtcGetErrorStr(error);
}

static esp_err_t tirtc_session_start_sdk_from_worker(void)
{
    tirtc_session_config_t config = {0};
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN] = {0};
    char secret_key[TIRTC_SESSION_SECRET_KEY_MAX_LEN] = {0};
    char client_id[TIRTC_SESSION_CLIENT_ID_MAX_LEN] = {0};
    char credential_material[TIRTC_SESSION_DEVICE_LICENSE_MAX_LEN] = {0};
    bool already_started = false;
    bool can_start = false;
    int option_ret = 0;
    int start_ret = 0;
    uint32_t sys_started_cb_before = 0;
    uint32_t sys_started_cb_after = 0;
    int64_t start_begin_us = 0;
    int64_t start_elapsed_ms = 0;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");

    tirtc_session_copy_config_snapshot(&config);
    if (!config.enabled) {
        tirtc_session_note_event("rtc disabled");
        return ESP_OK;
    }

    if (!tirtc_session_extract_start_identity(&config,
                                              device_id,
                                              sizeof(device_id),
                                              secret_key,
                                              sizeof(secret_key))) {
        tirtc_session_note_event("start identity empty");
        ESP_LOGE(TAG, "rtc listen start failed: device_id/device_secret_key is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (config.client_id[0] == '\0') {
        tirtc_session_note_event("start client id empty");
        ESP_LOGE(TAG, "rtc listen start failed: client_id is empty");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(client_id, config.client_id, sizeof(client_id));

    taskENTER_CRITICAL(&s_rtc_lock);
    already_started = s_network_connected && s_sdk_initialized && s_sdk_started &&
                      !s_start_in_progress && !s_stop_in_progress;
    can_start = s_network_connected && s_sdk_initialized && !s_sdk_started &&
                !s_sdk_prepare_in_progress && !s_start_in_progress && !s_stop_in_progress &&
                s_active_conn == NULL && s_closing_conn == NULL;
    if (can_start) {
        s_start_in_progress = true;
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (already_started) {
        return ESP_OK;
    }
    if (!can_start) {
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(credential_material,
                           sizeof(credential_material),
                           "%s,%s",
                           device_id,
                           secret_key);
    if (written < 0 || written >= (int)sizeof(credential_material)) {
        tirtc_session_clear_start_in_progress();
        tirtc_session_note_event("start credential too long");
        ESP_LOGE(TAG, "rtc listen start failed: device credential buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }

    char credential_hash[TIRTC_SESSION_SHA256_HEX_LEN] = {0};
    if (!tirtc_session_sha256_hex(credential_material, credential_hash, sizeof(credential_hash))) {
        strlcpy(credential_hash, "hash-failed", sizeof(credential_hash));
    }

    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        tirtc_session_clear_start_in_progress();
        tirtc_session_note_event("sdk lock failed");
        return ESP_FAIL;
    }

    APP_LOG_DETAIL(TAG,
                   "rtc listen start stage: TiRtcStart begin endpoint=%s start_arg=device_id device_id=%s client_id=%s secret_len=%u credential_hash=%.16s",
                   config.service_endpoint,
                   device_id,
                   client_id,
                   (unsigned)strlen(secret_key),
                   credential_hash);
    option_ret = TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                                secret_key,
                                (uint32_t)strlen(secret_key) + 1U);
    if (option_ret != 0) {
        tirtc_session_give_sdk_api_lock();
        tirtc_session_clear_start_in_progress();
        tirtc_session_set_last_error(option_ret);
        tirtc_session_note_event("set secret failed");
        ESP_LOGE(TAG,
                 "TiRtcSetOption(DEVICE_SECRET_KEY) failed: %s (%d)",
                 TiRtcGetErrorStr(option_ret),
                 option_ret);
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    APP_LOG_DETAIL(TAG,
                   "rtc listen start option set: device_secret_key length=%u",
                   (unsigned)strlen(secret_key));

#if TIRTC_SESSION_HAS_CLIENT_ID_OPTION
    option_ret = TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
                                client_id,
                                (uint32_t)strlen(client_id) + 1U);
    if (option_ret != 0) {
        tirtc_session_give_sdk_api_lock();
        tirtc_session_clear_start_in_progress();
        tirtc_session_set_last_error(option_ret);
        tirtc_session_note_event("set client id failed");
        ESP_LOGE(TAG,
                 "TiRtcSetOption(CLIENT_ID) failed: %s (%d)",
                 TiRtcGetErrorStr(option_ret),
                 option_ret);
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        return ESP_FAIL;
    }
    APP_LOG_DETAIL(TAG,
                   "rtc listen start option set: client_id_len=%u",
                   (unsigned)strlen(client_id));
#else
    APP_LOG_DETAIL(TAG, "rtc listen start: SDK has no CLIENT_ID option, using device_id only");
#endif

    taskENTER_CRITICAL(&s_rtc_lock);
    sys_started_cb_before = s_sys_started_callback_count;
    taskEXIT_CRITICAL(&s_rtc_lock);

    start_begin_us = esp_timer_get_time();
    start_ret = TiRtcStart(device_id, &s_tirtc_callbacks);
    start_elapsed_ms = (esp_timer_get_time() - start_begin_us) / 1000;
    taskENTER_CRITICAL(&s_rtc_lock);
    sys_started_cb_after = s_sys_started_callback_count;
    taskEXIT_CRITICAL(&s_rtc_lock);

    APP_LOG_DETAIL(TAG,
                   "rtc listen start stage: TiRtcStart returned ret=%d elapsed_ms=%lld sys_started_cb=%d cb_before=%lu cb_after=%lu",
                   start_ret,
                   (long long)start_elapsed_ms,
                   sys_started_cb_after != sys_started_cb_before ? 1 : 0,
                   (unsigned long)sys_started_cb_before,
                   (unsigned long)sys_started_cb_after);

    tirtc_session_give_sdk_api_lock();

    if (start_ret != 0) {
        /*
         * A client_id is the stable physical identity. Never replace it with
         * device_id to mask 40305; let the platform mapping be repaired and
         * retry the same identity from a clean SDK lifecycle.
         */
        uint64_t retry_delay_us = start_ret == TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT ?
                                  TIRTC_SESSION_CLIENT_ID_CONFLICT_RETRY_DELAY_US :
                                  TIRTC_SESSION_START_RETRY_DELAY_US;
        tirtc_session_clear_start_in_progress();
        tirtc_session_set_last_error(start_ret);
        tirtc_session_note_event("start failed");
        taskENTER_CRITICAL(&s_rtc_lock);
        s_next_start_allowed_us = esp_timer_get_time() + retry_delay_us;
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGE(TAG,
                 "TiRtcStart failed: %s (%d) device_id=%s client_id=%s retry_in_ms=%llu",
                 tirtc_session_start_error_name(start_ret),
                 start_ret,
                 device_id,
                 client_id,
                 (unsigned long long)(retry_delay_us / 1000ULL));
        tirtc_session_update_state(TIRTC_SESSION_STATE_ERROR);
        tirtc_session_notify_start_error(start_ret, device_id, client_id);
        if (!tirtc_session_schedule_deferred_start_after_delay(retry_delay_us, "rtc start retry")) {
            ESP_LOGW(TAG, "rtc listen start retry schedule failed");
        }
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    strlcpy(s_started_device_id, device_id, sizeof(s_started_device_id));
    strlcpy(s_started_credential_hash, credential_hash, sizeof(s_started_credential_hash));
    s_started_secret_len = (uint32_t)strlen(secret_key);
    s_next_start_allowed_us = 0U;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (sys_started_cb_after == sys_started_cb_before) {
        ESP_LOGW(TAG,
                 "rtc listen start returned OK but SYS_STARTED callback was not observed; using return-path fallback");
    }
    tirtc_session_mark_sdk_started();
    tirtc_session_note_event("rtc started");
    tirtc_session_note_event("listen start req");
    ESP_LOGI(TAG, "rtc listen ready: client_id=%s", client_id);
    return ESP_OK;
}

esp_err_t tirtc_session_send_command_raw(tirtc_conn_t conn, uint32_t cmdw, const void *data, size_t data_len)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        ESP_LOGW(TAG, "rtc command skipped: inactive connection cmd=0x%08lx", (unsigned long)cmdw);
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            ESP_LOGW(TAG, "rtc command skipped after state changed: cmd=0x%08lx", (unsigned long)cmdw);
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSendCommand(conn, cmdw, data, (uint32_t)data_len);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret >= 0) {
        APP_LOG_DETAIL(TAG,
                       "rtc command sent: hconn=%p cmd=0x%08lx len=%lu ret=%d",
                       conn,
                       (unsigned long)cmdw,
                       (unsigned long)data_len,
                       ret);
        return ESP_OK;
    }

    if (ret == TIRTC_E_INVALID_HANDLE) {
        if (tirtc_session_is_media_request_command(cmdw) &&
            tirtc_session_should_retry_media_request_after_invalid_handle(conn, "send media request command")) {
            ESP_LOGD(TAG,
                     "send command 0x%08lx delayed for media retry after early INVALID_HANDLE",
                     (unsigned long)cmdw);
            return ESP_FAIL;
        }

        tirtc_session_set_last_error(ret);
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        tirtc_session_set_last_error(ret);
    }
    ESP_LOGW(TAG, "rtc command send failed: cmd=0x%08lx err=%s", (unsigned long)cmdw, TiRtcGetErrorStr(ret));
    return ESP_FAIL;
}

esp_err_t tirtc_session_subscribe_audio(tirtc_conn_t conn, uint8_t stream_id)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSubscribeAudio(conn, stream_id);
        tirtc_session_give_sdk_api_lock();
    }

    if (ret >= 0) {
        return ESP_OK;
    }
    if (ret == TIRTC_E_INVALID_HANDLE) {
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        tirtc_session_set_last_error(ret);
    }
    ESP_LOGW(TAG, "subscribe audio failed stream=%u err=%s", stream_id, TiRtcGetErrorStr(ret));
    return ESP_FAIL;
}

esp_err_t tirtc_session_unsubscribe_audio(tirtc_conn_t conn, uint8_t stream_id)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcUnsubscribeAudio(conn, stream_id);
        tirtc_session_give_sdk_api_lock();
    }

    if (ret >= 0) {
        return ESP_OK;
    }
    if (ret == TIRTC_E_INVALID_HANDLE) {
        tirtc_session_handle_connection_loss(conn, ret);
    } else {
        tirtc_session_set_last_error(ret);
    }
    ESP_LOGW(TAG, "unsubscribe audio failed stream=%u err=%s", stream_id, TiRtcGetErrorStr(ret));
    return ESP_FAIL;
}

static esp_err_t tirtc_session_request_remote_audio(tirtc_conn_t conn)
{
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSubscribeAudio(conn, TIRTC_SESSION_REMOTE_AUDIO_STREAM_ID);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret < 0) {
        if (ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_retry_media_request_after_invalid_handle(conn, "subscribe remote audio")) {
            return ESP_FAIL;
        }

        if (ret == TIRTC_E_INVALID_HANDLE) {
            tirtc_session_handle_connection_loss(conn, ret);
        } else {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "request remote audio failed: %s", TiRtcGetErrorStr(ret));
        }
        return ESP_FAIL;
    }

    esp_err_t cmd_ret = tirtc_session_send_media_toggle_request(TIRTC_SESSION_CMD_REQ_AUDIO, true);
    if (cmd_ret != ESP_OK) {
        ESP_LOGW(TAG, "remote audio request command failed: %s", esp_err_to_name(cmd_ret));
        return ESP_FAIL;
    }

    APP_LOG_DETAIL(TAG, "remote audio requested: stream=%u", (unsigned)TIRTC_SESSION_REMOTE_AUDIO_STREAM_ID);

    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        s_remote_audio_requested = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_note_event("remote audio req");
    return ESP_OK;
}

static esp_err_t tirtc_session_request_remote_video(tirtc_conn_t conn)
{
    uint64_t accepted_at_us = 0U;
    uint64_t requested_at_us = 0U;
    uint8_t request_attempt = 0U;

    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        ret = TiRtcSubscribeVideo(conn, TIRTC_SESSION_REMOTE_VIDEO_STREAM_ID);
        tirtc_session_give_sdk_api_lock();
    }
    if (ret < 0) {
        if (ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_retry_media_request_after_invalid_handle(conn, "subscribe remote video")) {
            return ESP_FAIL;
        }

        if (ret == TIRTC_E_INVALID_HANDLE) {
            tirtc_session_handle_connection_loss(conn, ret);
        } else {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "request remote video failed: %s", TiRtcGetErrorStr(ret));
        }
        return ESP_FAIL;
    }

    esp_err_t cmd_ret = tirtc_session_send_media_toggle_request(TIRTC_SESSION_CMD_REQ_VIDEO, true);
    if (cmd_ret != ESP_OK) {
        ESP_LOGW(TAG, "remote video request command failed: %s", esp_err_to_name(cmd_ret));
        return ESP_FAIL;
    }

    if (tirtc_session_media_remote_video_requires_key_frame()) {
        (void)tirtc_session_request_remote_key_frame(conn,
                                                    TIRTC_SESSION_REMOTE_VIDEO_STREAM_ID,
                                                    "remote video requested");
    }

    requested_at_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_rtc_lock);
    if (conn == s_active_conn && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        if (s_remote_video_request_attempts == 0U) {
            s_remote_video_first_request_at_us = requested_at_us;
        }
        if (s_remote_video_request_attempts < UINT8_MAX) {
            s_remote_video_request_attempts++;
        }
        s_remote_video_requested = true;
        accepted_at_us = s_active_conn_accepted_at_us;
        request_attempt = s_remote_video_request_attempts;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_schedule_remote_video_first_packet_retry(request_attempt);
    ESP_LOGI(TAG,
             "remote video subscribe accepted: stream=%u attempt=%u accepted_ms=%llu",
             (unsigned)TIRTC_SESSION_REMOTE_VIDEO_STREAM_ID,
             (unsigned)request_attempt,
             accepted_at_us != 0U && requested_at_us >= accepted_at_us ?
                 (unsigned long long)((requested_at_us - accepted_at_us) / 1000ULL) :
                 0ULL);
    tirtc_session_note_event("remote video req");
    return ESP_OK;
}

void tirtc_session_request_remote_media(void)
{
    tirtc_conn_t conn = NULL;
    bool request_video = false;
    bool request_audio = false;
    bool retry_video = false;
    bool retry_audio = false;
    bool defer_audio = false;

    if (tirtc_session_is_test_media_active()) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
        s_active_conn != NULL && s_call_active) {
        conn = s_active_conn;
        request_video = tirtc_session_media_profile_allows_remote_video_locked() &&
                        !s_remote_video_requested;
        defer_audio = tirtc_session_should_defer_audio_for_local_video_locked();
        /* External audio owners, such as WeChat VoIP, use their own call signaling. */
        request_audio = s_media_profile != TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO &&
                        !s_remote_audio_requested &&
                        !defer_audio;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (conn == NULL) {
        return;
    }

    if (request_video) {
        if (tirtc_session_request_remote_video(conn) != ESP_OK) {
            retry_video = true;
        } else if (request_audio) {
            tirtc_session_retry_remote_media_request_after_delay(false,
                                                                true,
                                                                "audio after video req",
                                                                TIRTC_SESSION_MEDIA_AUDIO_FOLLOWUP_DELAY_US);
        }
    } else if (request_audio && tirtc_session_request_remote_audio(conn) != ESP_OK) {
        retry_audio = true;
    }

    if (retry_video || retry_audio) {
        tirtc_session_retry_remote_media_request(retry_video, retry_audio, "media req retry");
    }
}

static void tirtc_session_release_remote_media(void)
{
    tirtc_conn_t conn = NULL;
    bool release_video = false;
    bool release_audio = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_active_conn != NULL && s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
        s_closing_conn == NULL) {
        conn = s_active_conn;
        release_video = s_remote_video_requested;
        release_audio = s_remote_audio_requested;
    }
    s_remote_video_requested = false;
    s_remote_audio_requested = false;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (conn == NULL) {
        return;
    }
    if ((release_video || release_audio) && !tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        ESP_LOGW(TAG, "rtc sdk api lock unavailable while releasing remote media");
        return;
    }
    if (release_video) {
        int ret = TiRtcUnsubscribeVideo(conn, TIRTC_SESSION_REMOTE_VIDEO_STREAM_ID);
        if (ret < 0) {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "release remote video failed: %s", TiRtcGetErrorStr(ret));
        }
    }
    if (release_audio) {
        int ret = TiRtcUnsubscribeAudio(conn, TIRTC_SESSION_REMOTE_AUDIO_STREAM_ID);
        if (ret < 0) {
            tirtc_session_set_last_error(ret);
            ESP_LOGW(TAG, "release remote audio failed: %s", TiRtcGetErrorStr(ret));
        }
    }
    if (release_video || release_audio) {
        tirtc_session_give_sdk_api_lock();
    }
}

static uint32_t tirtc_session_pcm_level_percent(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len < sizeof(int16_t)) {
        return 0;
    }

    const int16_t *samples = (const int16_t *)data;
    size_t sample_count = data_len / sizeof(int16_t);
    uint32_t peak = 0;

    for (size_t index = 0; index < sample_count; ++index) {
        int32_t sample = samples[index];
        uint32_t abs_value = (uint32_t)(sample < 0 ? -sample : sample);
        if (abs_value > peak) {
            peak = abs_value;
        }
    }

    uint32_t level = (peak * 100U) / 32767U;
    return level > 100U ? 100U : level;
}

static bool tirtc_session_audio_format_to_pcm_flags(const tirtc_session_audio_format_t *format, uint8_t *flags)
{
    if (format == NULL || flags == NULL || format->bits_per_sample != 16U) {
        return false;
    }

    if (format->sample_rate_hz == 8000U && format->channels == 1U) {
        *flags = TIRTC_AUDIOSAMPLE_8K16B1C;
        return true;
    }
    if (format->sample_rate_hz == 16000U && format->channels == 1U) {
        *flags = TIRTC_AUDIOSAMPLE_16K16B1C;
        return true;
    }
    if (format->sample_rate_hz == 8000U && format->channels == 2U) {
        *flags = TIRTC_AUDIOSAMPLE_8K16B2C;
        return true;
    }
    if (format->sample_rate_hz == 16000U && format->channels == 2U) {
        *flags = TIRTC_AUDIOSAMPLE_16K16B2C;
        return true;
    }

    return false;
}

static esp_err_t tirtc_session_encode_ipc_audio_alaw(const uint8_t *data,
                                                     size_t data_len,
                                                     const tirtc_session_audio_format_t *format,
                                                     audio_alaw_stream_encoder_t *encoder,
                                                     uint8_t *encoded_data,
                                                     size_t encoded_capacity,
                                                     size_t *encoded_len)
{
    if (data == NULL || data_len == 0U || format == NULL || encoded_data == NULL || encoded_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *encoded_len = 0;

    if (format->bits_per_sample != 16U || format->channels != 1U ||
        format->sample_rate_hz != (TIRTC_SESSION_IPC_AUDIO_SAMPLE_RATE_HZ * 2U) || (data_len & 0x3U) != 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return audio_alaw_stream_encode_16k_mono_to_8k(encoder,
                                                   data,
                                                   data_len,
                                                   encoded_data,
                                                   encoded_capacity,
                                                   encoded_len);
}

static void tirtc_session_send_local_audio_packet(const uint8_t *data,
                                                 size_t data_len,
                                                 const tirtc_session_audio_format_t *format,
                                                 uint64_t pts_us,
                                                 tirtc_session_audio_tx_gate_t gate,
                                                 tirtc_conn_t expected_conn,
                                                 audio_alaw_stream_encoder_t *alaw_encoder)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    uint8_t media = TIRTC_AUDIO_PCM;
    uint8_t flags = 0;
    uint8_t alaw_stack[TIRTC_SESSION_IPC_AUDIO_STACK_ALAW_BYTES];
    const uint8_t *send_data = data;
    size_t send_data_len = data_len;
    uint32_t input_level = 0;
    const bool encode_as_ipc_alaw = gate == TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED;
    if (data == NULL || data_len == 0U || data_len > (size_t)UINT32_MAX) {
        return;
    }

    input_level = tirtc_session_pcm_level_percent(data, data_len);
    if (!tirtc_session_is_ready_to_send_audio_with_gate(gate, expected_conn, &conn, &stream_id)) {
        return;
    }

    if (encode_as_ipc_alaw) {
        esp_err_t encode_ret =
            tirtc_session_encode_ipc_audio_alaw(data,
                                                data_len,
                                                format,
                                                alaw_encoder,
                                                alaw_stack,
                                                sizeof(alaw_stack),
                                                &send_data_len);
        if (encode_ret != ESP_OK) {
            if (format != NULL) {
                ESP_LOGW(TAG,
                         "unsupported ipc audio format: %lu Hz %u bit %u ch len=%u ret=%s",
                         (unsigned long)format->sample_rate_hz,
                         format->bits_per_sample,
                         format->channels,
                         (unsigned)data_len,
                         esp_err_to_name(encode_ret));
            } else {
                ESP_LOGW(TAG, "missing ipc audio format");
            }
            return;
        }
        send_data = alaw_stack;
        media = TIRTC_AUDIO_ALAW;
        flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    } else if (!tirtc_session_audio_format_to_pcm_flags(format, &flags)) {
        if (format != NULL) {
            ESP_LOGW(TAG,
                     "unsupported local pcm format: %lu Hz %u bit %u ch",
                     (unsigned long)format->sample_rate_hz,
                     format->bits_per_sample,
                     format->channels);
        } else {
            ESP_LOGW(TAG, "missing local pcm format");
        }
        return;
    }

    TIRTCFRAMEINFO frame_info = {
        .stream_id = stream_id,
        .media = media,
        .flags = flags,
        .reserved = 0,
        .ts = (uint32_t)(pts_us / 1000ULL),
        .length = (uint32_t)send_data_len,
    };

    int send_ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_ready_to_send_audio_with_gate(gate, expected_conn, &conn, &stream_id)) {
            tirtc_session_give_sdk_api_lock();
            return;
        }
        frame_info.stream_id = stream_id;
        if (!tirtc_session_check_send_buffer(conn,
                                             TIRTC_SESSION_SEND_MEDIA_AUDIO,
                                             "audio",
                                             false)) {
            tirtc_session_give_sdk_api_lock();
            return;
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_attempts++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        send_ret = TiRtcSendAudioStream(conn, &frame_info, send_data);
        tirtc_session_give_sdk_api_lock();
    }
    if (send_ret >= 0) {
        bool log_first_packet = false;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
        bool log_window = false;
#endif
        bool peer_wants_audio = false;
        bool forced_publish = false;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
        uint32_t window_frames = 0;
        size_t window_payload_bytes = 0;
        uint32_t window_peak_percent = 0;
        const TickType_t now_tick = xTaskGetTickCount();
        const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_TX_LOG_INTERVAL_MS);
#endif

        taskENTER_CRITICAL(&s_rtc_lock);
        if (!s_local_audio_first_packet_logged) {
            s_local_audio_first_packet_logged = true;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            s_last_local_audio_tx_log_tick = now_tick;
#endif
            log_first_packet = true;
        }
        s_stats.tx_audio_frames++;
        s_stats.tx_audio_bytes += send_data_len;
        peer_wants_audio = s_peer_wants_audio;
        forced_publish = s_local_audio_publish_forced;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
        s_local_audio_tx_window_frames++;
        s_local_audio_tx_window_payload_bytes += send_data_len;
        if (input_level > s_local_audio_tx_window_peak_percent) {
            s_local_audio_tx_window_peak_percent = input_level;
        }
        if (!log_first_packet &&
            (s_last_local_audio_tx_log_tick == 0 || now_tick - s_last_local_audio_tx_log_tick >= log_interval_ticks)) {
            log_window = true;
            window_frames = s_local_audio_tx_window_frames;
            window_payload_bytes = s_local_audio_tx_window_payload_bytes;
            window_peak_percent = s_local_audio_tx_window_peak_percent;
            s_local_audio_tx_window_frames = 0;
            s_local_audio_tx_window_payload_bytes = 0;
            s_local_audio_tx_window_peak_percent = 0;
            s_last_local_audio_tx_log_tick = now_tick;
        }
#endif
        tirtc_session_set_last_event_locked("audio tx");
        taskEXIT_CRITICAL(&s_rtc_lock);
        if (log_first_packet) {
            ESP_LOGI(TAG,
                     "local audio first packet stream=%u media=%u flags=%u payload=%u input=%lu peer_audio=%d forced=%d",
                     stream_id,
                     media,
                     flags,
                     (unsigned)send_data_len,
                     (unsigned long)input_level,
                     peer_wants_audio,
                     forced_publish);
        }
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
        else if (log_window) {
            ESP_LOGI(TAG,
                     "local audio tx frames=%lu payload=%u peak=%lu peer_audio=%d forced=%d",
                     (unsigned long)window_frames,
                     (unsigned)window_payload_bytes,
                     (unsigned long)window_peak_percent,
                     peer_wants_audio,
                     forced_publish);
        }
#endif
    } else {
        uint64_t conn_age_us = 0;

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_failures++;
        taskEXIT_CRITICAL(&s_rtc_lock);
        if (send_ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
            tirtc_session_note_event("audio tx wait");
            ESP_LOGD(TAG,
                     "send audio got INVALID_HANDLE %llu us after accept; keep connection and retry next packet",
                     (unsigned long long)conn_age_us);
            return;
        }
        if (tirtc_session_should_reset_after_send_error(send_ret)) {
            tirtc_session_set_last_error(send_ret);
            tirtc_session_note_event("audio tx error");
            tirtc_session_handle_connection_loss(conn, send_ret);
            ESP_LOG_LEVEL_LOCAL(send_ret == TIRTC_E_CONN_REMOTECLOSE ? ESP_LOG_DEBUG : ESP_LOG_WARN,
                                TAG,
                                "send audio failed: %s (%d)",
                                TiRtcGetErrorStr(send_ret),
                                send_ret);
        } else {
            tirtc_session_note_transient_send_error("audio",
                                                    conn,
                                                    stream_id,
                                                    (uint32_t)send_data_len,
                                                    send_ret);
        }
    }
}

static int tirtc_session_send_local_video_packet(const uint8_t *data,
                                                size_t data_len,
                                                uint16_t width,
                                                uint16_t height,
                                                uint64_t pts_us,
                                                uint8_t media,
                                                uint8_t flags,
                                                const TIRTCFRAMEINFO *input_frame_info,
                                                uint32_t generation,
                                                bool test_frame,
                                                bool external_frame,
                                                tirtc_conn_t expected_conn,
                                                uint8_t external_stream_id)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    uint64_t now_ms = pts_us / 1000ULL;
    TIRTCFRAMEINFO frame_info = {0};

    if (input_frame_info != NULL) {
        frame_info = *input_frame_info;
        frame_info.length = (uint32_t)data_len;
    } else {
        frame_info.media = media;
        frame_info.flags = flags;
        frame_info.reserved = 0;
        frame_info.ts = (uint32_t)now_ms;
        frame_info.length = (uint32_t)data_len;
    }

    bool video_stream_started = false;
    if (test_frame) {
        taskENTER_CRITICAL(&s_rtc_lock);
        video_stream_started = s_stats.tx_video_frames > 0U;
        taskEXIT_CRITICAL(&s_rtc_lock);
    } else {
        video_stream_started = tirtc_session_local_h264_key_frame_published();
    }

    bool key_frame = (frame_info.flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0;
    bool can_drop_frame = video_stream_started &&
                          (frame_info.media == TIRTC_VIDEO_JPEG ||
                           !key_frame);
    int send_ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(test_frame ? TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS :
                                                      TIRTC_SESSION_VIDEO_TX_SDK_API_LOCK_WAIT_TICKS)) {
        if (generation != tirtc_session_get_local_video_tx_generation()) {
            tirtc_session_give_sdk_api_lock();
            return TIRTC_SESSION_VIDEO_TX_NOT_READY;
        }
        bool ready = false;
        if (test_frame) {
            ready = tirtc_session_is_ready_to_send_test_video(&conn, &stream_id);
        } else if (external_frame) {
            ready = tirtc_session_is_ready_to_send_external_video(expected_conn,
                                                                  external_stream_id,
                                                                  &conn,
                                                                  &stream_id);
        } else {
            ready = tirtc_session_is_ready_to_send_video(&conn, &stream_id);
        }
        if (!ready) {
            send_ret = TIRTC_SESSION_VIDEO_TX_NOT_READY;
            if (!test_frame && tirtc_session_should_log_local_video_tx_issue()) {
                bool sdk_started = false;
                bool start_in_progress = false;
                bool stop_in_progress = false;
                bool closing = false;
                bool active_conn = false;
                bool call_active = false;
                bool video_enabled = false;
                bool peer_wants_video = false;
                bool forced_publish = false;
                uint8_t local_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

                taskENTER_CRITICAL(&s_rtc_lock);
                sdk_started = s_sdk_started;
                start_in_progress = s_start_in_progress;
                stop_in_progress = s_stop_in_progress;
                closing = s_closing_conn != NULL;
                active_conn = s_active_conn != NULL;
                call_active = s_call_active;
                video_enabled = s_local_video_send_enabled;
                peer_wants_video = s_peer_wants_video;
                forced_publish = s_local_video_publish_forced;
                local_stream_id = s_local_video_stream_id;
                taskEXIT_CRITICAL(&s_rtc_lock);

                ESP_LOGW(TAG,
                         "%svideo tx not ready: sdk=%d start=%d stop=%d closing=%d conn=%d call=%d send=%d peer_video=%d forced=%d stream=%u payload=%u",
                         external_frame ? "external " : "local ",
                         sdk_started,
                         start_in_progress,
                         stop_in_progress,
                         closing,
                         active_conn,
                         call_active,
                         video_enabled,
                         peer_wants_video,
                         forced_publish,
                         (unsigned)(external_frame ? external_stream_id : local_stream_id),
                         (unsigned)data_len);
            }
            tirtc_session_give_sdk_api_lock();
            return send_ret;
        }
        frame_info.stream_id = stream_id;
        if (!test_frame &&
            frame_info.media == TIRTC_VIDEO_H264 &&
            !video_stream_started &&
            !key_frame) {
            send_ret = TIRTC_SESSION_VIDEO_TX_WAIT_KEY_FRAME;
            if (tirtc_session_should_log_local_video_tx_issue()) {
                ESP_LOGW(TAG,
                         "local video waits for first H264 key frame: payload=%u stream=%u",
                         (unsigned)data_len,
                         (unsigned)stream_id);
            }
            tirtc_session_give_sdk_api_lock();
            return send_ret;
        }
        if (!tirtc_session_check_send_buffer(conn,
                                             TIRTC_SESSION_SEND_MEDIA_VIDEO,
                                             test_frame ? "test video" : "video",
                                             can_drop_frame)) {
            send_ret = TIRTC_SESSION_VIDEO_TX_THROTTLED;
            tirtc_session_give_sdk_api_lock();
            return send_ret;
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_attempts++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        if (!test_frame) {
            taskENTER_CRITICAL(&s_rtc_lock);
            s_local_video_last_send_attempt_us = (uint64_t)esp_timer_get_time();
            taskEXIT_CRITICAL(&s_rtc_lock);
        }
        send_ret = TiRtcSendVideoStream(conn, &frame_info, data);
        tirtc_session_give_sdk_api_lock();
    } else if (!test_frame && tirtc_session_should_log_local_video_tx_issue()) {
        ESP_LOGW(TAG,
                 "local video tx sdk lock busy: payload=%u media=%u flags=%u",
                 (unsigned)data_len,
                 (unsigned)media,
                 (unsigned)flags);
    }
    if (send_ret >= 0) {
        bool log_first_packet = false;
        bool peer_wants_video = false;
        bool forced_publish = false;
        uint64_t accepted_at_us = 0U;
        uint64_t first_requested_at_us = 0U;

        if (!test_frame &&
            frame_info.media == TIRTC_VIDEO_H264 &&
            key_frame) {
            tirtc_session_mark_local_h264_key_frame_published();
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        if (!s_local_video_first_packet_logged) {
            s_local_video_first_packet_logged = true;
            log_first_packet = true;
        }
        s_stats.tx_video_frames++;
        s_stats.tx_video_bytes += data_len;
        if (!test_frame) {
            s_local_video_last_send_success_us = (uint64_t)esp_timer_get_time();
        }
        peer_wants_video = s_peer_wants_video;
        forced_publish = s_local_video_publish_forced;
        accepted_at_us = s_active_conn_accepted_at_us;
        first_requested_at_us = s_local_video_first_requested_at_us;
        tirtc_session_set_last_event_locked(test_frame ? "test video tx" :
                                            (external_frame ? "external video tx" : "video tx"));
        taskEXIT_CRITICAL(&s_rtc_lock);

        if (log_first_packet) {
            uint64_t now_us = esp_timer_get_time();
            ESP_LOGI(TAG,
                     "%svideo first tx stream=%u media=%u(%s) flags=%u payload=%u size=%ux%u peer_video=%d forced=%d accepted_ms=%llu request_ms=%llu",
                     test_frame ? "test " : (external_frame ? "external " : "local "),
                     (unsigned)stream_id,
                     (unsigned)frame_info.media,
                     tirtc_session_media_name(frame_info.media),
                     (unsigned)frame_info.flags,
                     (unsigned)data_len,
                     (unsigned)width,
                     (unsigned)height,
                     peer_wants_video,
                     forced_publish,
                     accepted_at_us != 0U && now_us >= accepted_at_us ?
                         (unsigned long long)((now_us - accepted_at_us) / 1000ULL) :
                         0ULL,
                     first_requested_at_us != 0U && now_us >= first_requested_at_us ?
                         (unsigned long long)((now_us - first_requested_at_us) / 1000ULL) :
                         0ULL);
        }
    } else {
        if (send_ret == TIRTC_SESSION_VIDEO_TX_NOT_READY ||
            send_ret == TIRTC_SESSION_VIDEO_TX_WAIT_KEY_FRAME ||
            send_ret == TIRTC_SESSION_VIDEO_TX_THROTTLED) {
            return send_ret;
        }
        uint64_t conn_age_us = 0;

        if (send_ret == TIRTC_E_INVALID_HANDLE &&
            tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
            if (test_frame) {
                uint64_t retry_after_us = esp_timer_get_time() + TIRTC_SESSION_TEST_MEDIA_RETRY_DELAY_US;

                taskENTER_CRITICAL(&s_rtc_lock);
                if (retry_after_us > s_test_video_retry_after_us) {
                    s_test_video_retry_after_us = retry_after_us;
                }
                taskEXIT_CRITICAL(&s_rtc_lock);
            }
            tirtc_session_note_event(test_frame ? "test video tx wait" : "video tx wait");
            ESP_LOGD(TAG,
                     "send %svideo got INVALID_HANDLE %llu us after accept; keep connection and retry next packet",
                     test_frame ? "test " : "",
                     (unsigned long long)conn_age_us);
            return TIRTC_SESSION_VIDEO_TX_NOT_READY;
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_failures++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        if (send_ret == TIRTC_E_BUSY) {
            media_governor_note_network_backpressure();
            if (!test_frame && tirtc_session_should_log_local_video_tx_issue()) {
                size_t send_buffer_used = 0;
                bool peer_wants_video = false;
                bool forced_publish = false;

                taskENTER_CRITICAL(&s_rtc_lock);
                send_buffer_used = s_stats.send_buffer_used;
                peer_wants_video = s_peer_wants_video;
                forced_publish = s_local_video_publish_forced;
                taskEXIT_CRITICAL(&s_rtc_lock);

                ESP_LOGW(TAG,
                         "local video tx busy: payload=%u media=%u flags=%u stream=%u conn=%p peer_video=%d forced=%d q=%u free=%u sdk_buf=%u/%u",
                         (unsigned)data_len,
                         (unsigned)media,
                         (unsigned)flags,
                         (unsigned)stream_id,
                         conn,
                         peer_wants_video,
                         forced_publish,
                         (unsigned)(s_local_video_tx_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_queue) : 0),
                         (unsigned)(s_local_video_tx_free_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0),
                         (unsigned)send_buffer_used,
                         (unsigned)TIRTC_SESSION_MAX_SEND_BUFFER);
            }
            return send_ret;
        }

        if (tirtc_session_should_reset_after_send_error(send_ret)) {
            tirtc_session_set_last_error(send_ret);
            tirtc_session_note_event(test_frame ? "test video tx error" : "video tx error");
            tirtc_session_handle_connection_loss(conn, send_ret);
            ESP_LOG_LEVEL_LOCAL(send_ret == TIRTC_E_CONN_REMOTECLOSE ? ESP_LOG_DEBUG : ESP_LOG_WARN,
                                TAG,
                                "%svideo send failed: err=%s (%d)",
                                test_frame ? "test " : "",
                                TiRtcGetErrorStr(send_ret),
                                send_ret);
        } else {
            tirtc_session_note_transient_send_error(test_frame ? "test video" : "video",
                                                    conn,
                                                    stream_id,
                                                    (uint32_t)data_len,
                                                    send_ret);
        }
    }
    return send_ret;
}

esp_err_t tirtc_session_send_test_video_frame(const TIRTCFRAMEINFO *frame_info, const uint8_t *data)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (frame_info == NULL || data == NULL || frame_info->length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_test_video(&conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_video_packet(data,
                                                   frame_info->length,
                                                   0,
                                                   0,
                                                   (uint64_t)frame_info->ts * 1000ULL,
                                                   frame_info->media,
                                                   frame_info->flags,
                                                   frame_info,
                                                   true,
                                                   false,
                                                   NULL,
                                                   TIRTC_SESSION_INVALID_STREAM_ID);
}

esp_err_t tirtc_session_send_local_video_frame(const uint8_t *data,
                                               size_t data_len,
                                               uint16_t width,
                                               uint16_t height,
                                               uint64_t pts_us,
                                               uint8_t media,
                                               uint8_t flags)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (data == NULL || data_len == 0U || !TIRTC_IS_VIDEO(media)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_video(&conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)conn;
    (void)stream_id;

    return tirtc_session_enqueue_local_video_packet(data,
                                                   data_len,
                                                   width,
                                                   height,
                                                   pts_us,
                                                   media,
                                                   flags,
                                                   NULL,
                                                   false,
                                                   false,
                                                   NULL,
                                                   TIRTC_SESSION_INVALID_STREAM_ID);
}

esp_err_t tirtc_session_set_external_video_active(tirtc_conn_t conn,
                                                  uint8_t stream_id,
                                                  bool active)
{
    bool changed = false;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(conn != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "external video connection is null");
    ESP_RETURN_ON_FALSE(stream_id != TIRTC_SESSION_INVALID_STREAM_ID,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "external video stream is invalid");
    if (active) {
        ESP_RETURN_ON_FALSE(tirtc_session_is_connection_usable(conn),
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "external video connection is inactive");
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (active) {
        if (s_external_video_active &&
            (s_external_video_conn != conn ||
             s_external_video_stream_id != stream_id)) {
            ret = ESP_ERR_INVALID_STATE;
        } else if (!s_external_video_active) {
            s_external_video_conn = conn;
            s_external_video_stream_id = stream_id;
            s_external_video_active = true;
            changed = true;
        }
    } else if (s_external_video_active &&
               s_external_video_conn == conn &&
               s_external_video_stream_id == stream_id) {
        s_external_video_conn = NULL;
        s_external_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_external_video_active = false;
        changed = true;
    }
    if (changed) {
        s_local_video_first_packet_logged = false;
        s_local_h264_key_frame_queued = false;
        s_local_h264_key_frame_published = false;
        s_local_h264_recovery_pending = false;
        s_local_h264_recovery_count = 0;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (changed) {
        tirtc_session_flush_local_video_tx_queue();
        tirtc_session_note_event(active ? "external video on" : "external video off");
    }
    return ret;
}

esp_err_t tirtc_session_send_external_video_frame(tirtc_conn_t conn,
                                                  uint8_t stream_id,
                                                  const uint8_t *data,
                                                  size_t data_len,
                                                  uint16_t width,
                                                  uint16_t height,
                                                  uint64_t pts_us,
                                                  uint8_t media,
                                                  uint8_t flags)
{
    tirtc_conn_t active_conn = NULL;
    uint8_t active_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (conn == NULL || data == NULL || data_len == 0U || !TIRTC_IS_VIDEO(media)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_external_video(conn,
                                                       stream_id,
                                                       &active_conn,
                                                       &active_stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_video_packet(data,
                                                   data_len,
                                                   width,
                                                   height,
                                                   pts_us,
                                                   media,
                                                   flags,
                                                   NULL,
                                                   false,
                                                   true,
                                                   active_conn,
                                                   active_stream_id);
}

esp_err_t tirtc_session_send_test_audio_pcm_frame(const uint8_t *data,
                                                  size_t data_len,
                                                  const tirtc_session_audio_format_t *format,
                                                  uint64_t pts_us)
{
    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (data == NULL || data_len == 0U || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_test_audio(&conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_audio_packet(data,
                                                   data_len,
                                                   format,
                                                   pts_us,
                                                   TIRTC_SESSION_AUDIO_TX_GATE_TEST,
                                                   NULL);
}

esp_err_t tirtc_session_send_audio_frame(tirtc_conn_t conn, const TIRTCFRAMEINFO *frame_info, const void *data)
{
    TIRTCFRAMEINFO send_info = {0};
    int send_ret = TIRTC_E_BUSY;

    if (conn == NULL || frame_info == NULL || data == NULL || frame_info->length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }

    send_info = *frame_info;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        if (!tirtc_session_is_connection_usable(conn)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_INVALID_STATE;
        }
        if (!tirtc_session_check_send_buffer(conn,
                                             TIRTC_SESSION_SEND_MEDIA_AUDIO,
                                             "audio",
                                             true)) {
            tirtc_session_give_sdk_api_lock();
            return ESP_ERR_TIMEOUT;
        }

        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_attempts++;
        taskEXIT_CRITICAL(&s_rtc_lock);

        send_ret = TiRtcSendAudioStream(conn, &send_info, data);
        tirtc_session_give_sdk_api_lock();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    if (send_ret >= 0) {
        taskENTER_CRITICAL(&s_rtc_lock);
        s_stats.tx_audio_frames++;
        s_stats.tx_audio_bytes += frame_info->length;
        tirtc_session_set_last_event_locked("audio tx");
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_stats.tx_failures++;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (send_ret == TIRTC_E_BUSY) {
        tirtc_session_note_event("audio tx busy");
        return ESP_ERR_TIMEOUT;
    }
    if (send_ret == TIRTC_E_INVALID_HANDLE) {
        uint64_t conn_age_us = 0;

        if (tirtc_session_should_tolerate_invalid_handle(conn, &conn_age_us)) {
            tirtc_session_note_event("audio tx wait");
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (tirtc_session_should_reset_after_send_error(send_ret)) {
        tirtc_session_set_last_error(send_ret);
        tirtc_session_note_event("audio tx error");
        tirtc_session_handle_connection_loss(conn, send_ret);
        ESP_LOG_LEVEL_LOCAL(send_ret == TIRTC_E_CONN_REMOTECLOSE ? ESP_LOG_DEBUG : ESP_LOG_WARN,
                            TAG,
                            "send audio frame failed hconn=%p stream=%u len=%u ts=%lu err=%s (%d)",
                            conn,
                            send_info.stream_id,
                            (unsigned)send_info.length,
                            (unsigned long)send_info.ts,
                            TiRtcGetErrorStr(send_ret),
                            send_ret);
        return ESP_FAIL;
    }

    tirtc_session_note_transient_send_error("audio frame",
                                            conn,
                                            send_info.stream_id,
                                            send_info.length,
                                            send_ret);
    return ESP_ERR_TIMEOUT;
}

esp_err_t tirtc_session_send_captured_audio_frame(tirtc_conn_t conn,
                                                  const uint8_t *data,
                                                  size_t data_len,
                                                  const tirtc_session_audio_format_t *format,
                                                  uint64_t pts_us)
{
    tirtc_conn_t active_conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;

    if (conn == NULL || data == NULL || data_len == 0U || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tirtc_session_is_ready_to_send_call_audio(conn, &active_conn, &stream_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tirtc_session_enqueue_local_audio_packet(data,
                                                   data_len,
                                                   format,
                                                   pts_us,
                                                   TIRTC_SESSION_AUDIO_TX_GATE_CALL,
                                                   conn);
}

static void tirtc_session_local_audio_tx_task(void *ctx)
{
    (void)ctx;
    tirtc_session_local_audio_packet_t packet = {
        .buffer_slot = TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID,
    };
    audio_alaw_stream_encoder_t alaw_encoder = {0};
    uint32_t alaw_encoder_generation = UINT32_MAX;

    while (xQueueReceive(s_local_audio_tx_queue, &packet, portMAX_DELAY) == pdTRUE) {
        uint64_t now_us = esp_timer_get_time();
        uint32_t current_generation = tirtc_session_get_local_audio_tx_generation();
        bool packet_is_fresh = packet.pts_us == 0 || now_us <= packet.pts_us ||
                               now_us - packet.pts_us <= TIRTC_SESSION_AUDIO_TX_MAX_AGE_US;

        if (packet.generation == current_generation && packet_is_fresh) {
            if (alaw_encoder_generation != packet.generation) {
                audio_alaw_stream_encoder_reset(&alaw_encoder);
                alaw_encoder_generation = packet.generation;
            }
            tirtc_session_send_local_audio_packet(packet.data,
                                                 packet.data_len,
                                                 &packet.format,
                                                 packet.pts_us != 0 ? packet.pts_us : now_us,
                                                 packet.gate,
                                                 packet.expected_conn,
                                                 &alaw_encoder);
        } else {
            uint32_t age_ms = 0;
            if (packet.pts_us != 0 && now_us > packet.pts_us) {
                age_ms = (uint32_t)((now_us - packet.pts_us) / 1000ULL);
            }
            ESP_LOGD(TAG,
                     "queued audio dropped: len=%u pts=%lu gen=%lu current=%lu fresh=%d age_ms=%lu",
                     (unsigned)packet.data_len,
                     (unsigned long)(packet.pts_us / 1000ULL),
                     (unsigned long)packet.generation,
                     (unsigned long)current_generation,
                     packet_is_fresh,
                     (unsigned long)age_ms);
        }
        tirtc_session_free_local_audio_packet(&packet);
    }
}

static void tirtc_session_local_video_tx_task(void *ctx)
{
    (void)ctx;
    tirtc_session_local_video_packet_t packet = {0};
    uint64_t window_start_us = esp_timer_get_time();
    uint32_t sent_count = 0;
    uint32_t fail_count = 0;
    uint32_t busy_count = 0;
    uint32_t defer_count = 0;
    uint32_t throttle_count = 0;
    uint32_t stale_drop_count = 0;
    uint32_t total_payload_bytes = 0;
    uint32_t max_payload_bytes = 0;
    uint64_t send_us_total = 0;
    uint64_t max_send_us = 0;
    uint64_t queue_age_us_total = 0;
    uint64_t max_queue_age_us = 0;
    size_t max_send_buffer_used = 0U;
    uint64_t last_stall_log_us = 0;
    uint32_t window_generation = 0;
    bool window_generation_valid = false;
    uint32_t rx_callback_window_start = 0U;
    uint32_t rx_submit_failure_window_start = 0U;

    while (xQueueReceive(s_local_video_tx_queue, &packet, portMAX_DELAY) == pdTRUE) {
        uint64_t now_us = esp_timer_get_time();
        if (!packet.test_frame) {
            taskENTER_CRITICAL(&s_rtc_lock);
            s_local_video_last_dequeue_us = now_us;
            taskEXIT_CRITICAL(&s_rtc_lock);
        }
        uint32_t current_generation = tirtc_session_get_local_video_tx_generation();
        uint8_t packet_media = packet.has_frame_info ? packet.frame_info.media : packet.media;
        bool local_h264_packet = !packet.test_frame && packet_media == TIRTC_VIDEO_H264;
        bool recover_h264 = false;
        bool recovery_is_backpressure = false;
        const char *recovery_reason = NULL;
        uint64_t queue_age_us = packet.queued_at_us != 0U && now_us > packet.queued_at_us ?
                                    now_us - packet.queued_at_us : 0U;
        uint64_t media_age_us = packet.pts_us != 0U && now_us > packet.pts_us ?
                                    now_us - packet.pts_us : 0U;
        bool packet_is_fresh = packet.queued_at_us == 0U ||
                               queue_age_us <= TIRTC_SESSION_VIDEO_TX_MAX_AGE_US;

        if (!window_generation_valid || window_generation != current_generation) {
            window_generation = current_generation;
            window_generation_valid = true;
            window_start_us = now_us;
            sent_count = 0;
            fail_count = 0;
            busy_count = 0;
            defer_count = 0;
            throttle_count = 0;
            stale_drop_count = 0;
            total_payload_bytes = 0;
            max_payload_bytes = 0;
            send_us_total = 0;
            max_send_us = 0;
            queue_age_us_total = 0;
            max_queue_age_us = 0;
            max_send_buffer_used = 0U;
            taskENTER_CRITICAL(&s_rtc_lock);
            rx_callback_window_start = s_remote_video_callback_frames;
            rx_submit_failure_window_start = s_remote_video_submit_failures;
            s_remote_video_callback_gap_window_max_us = 0U;
            taskEXIT_CRITICAL(&s_rtc_lock);
        }

        if (packet.generation == current_generation && packet_is_fresh) {
            uint64_t send_start_us = esp_timer_get_time();
            int send_ret = tirtc_session_send_local_video_packet(packet.data,
                                                                 packet.data_len,
                                                                 packet.width,
                                                                 packet.height,
                                                                 packet.pts_us != 0 ? packet.pts_us : now_us,
                                                                 packet.media,
                                                                 packet.flags,
                                                                 packet.has_frame_info ? &packet.frame_info : NULL,
                                                                 packet.generation,
                                                                 packet.test_frame,
                                                                 packet.external_frame,
                                                                 packet.expected_conn,
                                                                 packet.external_stream_id);
            uint64_t send_us = esp_timer_get_time() - send_start_us;
            if (send_us >= TIRTC_SESSION_VIDEO_TX_STALL_US) {
                uint64_t stall_now_us = esp_timer_get_time();
                if (last_stall_log_us == 0U ||
                    stall_now_us - last_stall_log_us >=
                        TIRTC_SESSION_VIDEO_TX_STALL_LOG_INTERVAL_US) {
                    uint8_t packet_flags = packet.has_frame_info ?
                                           packet.frame_info.flags : packet.flags;
                    size_t send_buffer_used = 0;

                    taskENTER_CRITICAL(&s_rtc_lock);
                    send_buffer_used = s_stats.send_buffer_used;
                    taskEXIT_CRITICAL(&s_rtc_lock);
                    ESP_LOGW(TAG,
                             "video tx stall: send=%lluus ret=%d key=%d bytes=%u qage=%lluus q=%u free=%u sdk=%u",
                             (unsigned long long)send_us,
                             send_ret,
                             (packet_flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0,
                             (unsigned)packet.data_len,
                             (unsigned long long)queue_age_us,
                             (unsigned)(s_local_video_tx_queue != NULL ?
                                        uxQueueMessagesWaiting(s_local_video_tx_queue) : 0),
                             (unsigned)(s_local_video_tx_free_queue != NULL ?
                                        uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0),
                             (unsigned)send_buffer_used);
                    last_stall_log_us = stall_now_us;
                }
            }
            if (send_ret >= 0) {
                sent_count++;
                total_payload_bytes += packet.data_len;
                if (packet.data_len > max_payload_bytes) {
                    max_payload_bytes = packet.data_len;
                }
            } else {
                if (local_h264_packet) {
                    recover_h264 = true;
                    if (send_ret == TIRTC_SESSION_VIDEO_TX_THROTTLED ||
                        send_ret == TIRTC_E_BUSY) {
                        recovery_is_backpressure = true;
                        recovery_reason = send_ret == TIRTC_SESSION_VIDEO_TX_THROTTLED ?
                                          "SDK send buffer throttled" :
                                          "SDK video send busy";
                    } else if (send_ret == TIRTC_SESSION_VIDEO_TX_NOT_READY ||
                               send_ret == TIRTC_SESSION_VIDEO_TX_WAIT_KEY_FRAME) {
                        recovery_reason = "SDK send deferred";
                    } else {
                        recovery_reason = "SDK video send failed";
                    }
                }
                if (send_ret == TIRTC_SESSION_VIDEO_TX_NOT_READY ||
                    send_ret == TIRTC_SESSION_VIDEO_TX_WAIT_KEY_FRAME) {
                    defer_count++;
                } else if (send_ret == TIRTC_SESSION_VIDEO_TX_THROTTLED) {
                    throttle_count++;
                } else {
                    fail_count++;
                    if (send_ret == TIRTC_E_BUSY) {
                        busy_count++;
                    }
                }
            }
            send_us_total += send_us;
            if (send_us > max_send_us) {
                max_send_us = send_us;
            }
            queue_age_us_total += queue_age_us;
            if (queue_age_us > max_queue_age_us) {
                max_queue_age_us = queue_age_us;
            }
        } else {
            uint8_t packet_flags = packet.has_frame_info ?
                                   packet.frame_info.flags : packet.flags;
            stale_drop_count++;
            if (local_h264_packet && packet.generation == current_generation) {
                recover_h264 = true;
                recovery_is_backpressure = true;
                recovery_reason = "queued H264 frame stale";
            }
            if (!packet.test_frame && tirtc_session_should_log_local_video_tx_issue()) {
                size_t send_buffer_used = 0;
                taskENTER_CRITICAL(&s_rtc_lock);
                send_buffer_used = s_stats.send_buffer_used;
                taskEXIT_CRITICAL(&s_rtc_lock);
                ESP_LOGW(TAG,
                         "video tx stale: qage=%llums mage=%llums key=%d bytes=%u gen=%lu/%lu "
                         "send_max=%llums q=%u free=%u sdk=%u",
                         (unsigned long long)(queue_age_us / 1000ULL),
                         (unsigned long long)(media_age_us / 1000ULL),
                         (packet_flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0,
                         (unsigned)packet.data_len,
                         (unsigned long)packet.generation,
                         (unsigned long)current_generation,
                         (unsigned long long)(max_send_us / 1000ULL),
                         (unsigned)(s_local_video_tx_queue != NULL ?
                                    uxQueueMessagesWaiting(s_local_video_tx_queue) : 0),
                         (unsigned)(s_local_video_tx_free_queue != NULL ?
                                    uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0),
                         (unsigned)send_buffer_used);
            } else {
                ESP_LOGD(TAG,
                         "queued video dropped: len=%u gen=%lu/%lu fresh=%d qage=%llums mage=%llums",
                         (unsigned)packet.data_len,
                         (unsigned long)packet.generation,
                         (unsigned long)current_generation,
                         packet_is_fresh,
                         (unsigned long long)(queue_age_us / 1000ULL),
                         (unsigned long long)(media_age_us / 1000ULL));
            }
        }
        tirtc_session_free_local_video_packet(&packet);
        if (recover_h264) {
            tirtc_session_recover_local_h264_stream(recovery_reason,
                                                     recovery_is_backpressure);
        }

        size_t cached_send_buffer_used = tirtc_session_cached_send_buffer_used();
        if (cached_send_buffer_used > max_send_buffer_used) {
            max_send_buffer_used = cached_send_buffer_used;
        }

        uint64_t stats_now_us = esp_timer_get_time();
        if (stats_now_us - window_start_us >= ((uint64_t)TIRTC_SESSION_TX_LOG_INTERVAL_MS * 1000ULL)) {
            uint32_t elapsed_ms = (uint32_t)((stats_now_us - window_start_us) / 1000ULL);
            if (elapsed_ms == 0U) {
                elapsed_ms = 1U;
            }
            uint32_t fps_x10 = (uint32_t)(((uint64_t)sent_count * 10000ULL) / elapsed_ms);
            uint32_t bitrate_kbps = (uint32_t)(((uint64_t)total_payload_bytes * 8ULL) / elapsed_ms);
            uint32_t avg_payload = sent_count > 0U ? total_payload_bytes / sent_count : 0U;
            uint32_t observed_count = sent_count + fail_count + defer_count + throttle_count;
            uint32_t avg_send_us = observed_count > 0U ?
                                   (uint32_t)(send_us_total / observed_count) :
                                   0U;
            uint32_t avg_queue_age_ms = observed_count > 0U ?
                                        (uint32_t)(queue_age_us_total / observed_count / 1000ULL) :
                                        0U;
            size_t send_buffer_used = 0;
            uint32_t rx_callback_frames = 0U;
            uint32_t rx_submit_failures = 0U;
            uint32_t rx_callback_gap_max_us = 0U;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            uint32_t stat_tx_failures = 0;
#endif
            taskENTER_CRITICAL(&s_rtc_lock);
            send_buffer_used = s_stats.send_buffer_used;
            rx_callback_frames = s_remote_video_callback_frames;
            rx_submit_failures = s_remote_video_submit_failures;
            rx_callback_gap_max_us = s_remote_video_callback_gap_window_max_us;
            s_remote_video_callback_gap_window_max_us = 0U;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            stat_tx_failures = s_stats.tx_failures;
#endif
            taskEXIT_CRITICAL(&s_rtc_lock);
            uint32_t rx_callback_delta =
                rx_callback_frames >= rx_callback_window_start ?
                    rx_callback_frames - rx_callback_window_start :
                    rx_callback_frames;
            uint32_t rx_submit_failure_delta =
                rx_submit_failures >= rx_submit_failure_window_start ?
                    rx_submit_failures - rx_submit_failure_window_start :
                    rx_submit_failures;
            uint32_t rx_submit_ok_delta = rx_callback_delta >= rx_submit_failure_delta ?
                                          rx_callback_delta - rx_submit_failure_delta : 0U;
#if !CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS && !CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
            (void)fps_x10;
            (void)bitrate_kbps;
            (void)avg_payload;
            (void)avg_send_us;
            (void)avg_queue_age_ms;
            (void)send_buffer_used;
            (void)rx_callback_gap_max_us;
            (void)rx_submit_ok_delta;
#endif
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            ESP_LOGI(TAG,
                     "local video tx stats: sent=%lu fps=%lu.%lu bitrate=%lukbps fail=%lu busy=%lu defer=%lu throttle=%lu stale=%lu payload[avg/max]=%lu/%lu send_us[avg/max]=%lu/%llu queue_age_ms[avg/max]=%lu/%llu q=%u free=%u sdk_buf=%u/%u peak=%u total_fail=%lu rx[cb/ok/fail/gap_ms]=%lu/%lu/%lu/%lu hwm=%u",
                     (unsigned long)sent_count,
                     (unsigned long)(fps_x10 / 10U),
                     (unsigned long)(fps_x10 % 10U),
                     (unsigned long)bitrate_kbps,
                     (unsigned long)fail_count,
                     (unsigned long)busy_count,
                     (unsigned long)defer_count,
                     (unsigned long)throttle_count,
                     (unsigned long)stale_drop_count,
                     (unsigned long)avg_payload,
                     (unsigned long)max_payload_bytes,
                     (unsigned long)avg_send_us,
                     (unsigned long long)max_send_us,
                     (unsigned long)avg_queue_age_ms,
                     (unsigned long long)(max_queue_age_us / 1000ULL),
                     (unsigned)(s_local_video_tx_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_queue) : 0),
                     (unsigned)(s_local_video_tx_free_queue != NULL ? uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0),
                     (unsigned)send_buffer_used,
                     (unsigned)TIRTC_SESSION_MAX_SEND_BUFFER,
                     (unsigned)max_send_buffer_used,
                     (unsigned long)stat_tx_failures,
                     (unsigned long)rx_callback_delta,
                     (unsigned long)rx_submit_ok_delta,
                     (unsigned long)rx_submit_failure_delta,
                     (unsigned long)(rx_callback_gap_max_us / 1000U),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
#elif CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
            ESP_LOGI(TAG,
                     "VTX f=%lu.%lu br=%luk ok=%lu e/b/d/t/s=%lu/%lu/%lu/%lu/%lu "
                     "p=%lu/%lu api=%lu/%lluus age=%lu/%llums q=%u/%u sb=%u/%u "
                     "rx=%lu/%lu/%lu/%lums",
                     (unsigned long)(fps_x10 / 10U),
                     (unsigned long)(fps_x10 % 10U),
                     (unsigned long)bitrate_kbps,
                     (unsigned long)sent_count,
                     (unsigned long)fail_count,
                     (unsigned long)busy_count,
                     (unsigned long)defer_count,
                     (unsigned long)throttle_count,
                     (unsigned long)stale_drop_count,
                     (unsigned long)avg_payload,
                     (unsigned long)max_payload_bytes,
                     (unsigned long)avg_send_us,
                     (unsigned long long)max_send_us,
                     (unsigned long)avg_queue_age_ms,
                     (unsigned long long)(max_queue_age_us / 1000ULL),
                     (unsigned)(s_local_video_tx_queue != NULL ?
                                uxQueueMessagesWaiting(s_local_video_tx_queue) : 0),
                     (unsigned)(s_local_video_tx_free_queue != NULL ?
                                uxQueueMessagesWaiting(s_local_video_tx_free_queue) : 0),
                     (unsigned)send_buffer_used,
                     (unsigned)max_send_buffer_used,
                     (unsigned long)rx_callback_delta,
                     (unsigned long)rx_submit_ok_delta,
                     (unsigned long)rx_submit_failure_delta,
                     (unsigned long)(rx_callback_gap_max_us / 1000U));
#endif
            rx_callback_window_start = rx_callback_frames;
            rx_submit_failure_window_start = rx_submit_failures;
            window_start_us = stats_now_us;
            sent_count = 0;
            fail_count = 0;
            busy_count = 0;
            defer_count = 0;
            throttle_count = 0;
            stale_drop_count = 0;
            total_payload_bytes = 0;
            max_payload_bytes = 0;
            send_us_total = 0;
            max_send_us = 0;
            queue_age_us_total = 0;
            max_queue_age_us = 0;
            max_send_buffer_used = 0U;
        }
    }
}

static void tirtc_session_local_audio_cb(const uint8_t *data,
                                        size_t data_len,
                                        const tirtc_session_audio_format_t *format,
                                        void *ctx)
{
    (void)ctx;

    tirtc_conn_t conn = NULL;
    uint8_t stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    if (!tirtc_session_is_ready_to_send_audio(&conn, &stream_id)) {
        return;
    }

    esp_err_t enqueue_ret = tirtc_session_enqueue_local_audio_packet(data,
                                                                    data_len,
                                                                    format,
                                                                    esp_timer_get_time(),
                                                                    TIRTC_SESSION_AUDIO_TX_GATE_SUBSCRIBED,
                                                                    NULL);
    if (enqueue_ret != ESP_OK) {
        tirtc_session_note_event("audio tx drop");
        TickType_t now = xTaskGetTickCount();
        if (s_last_local_audio_queue_fail_log_tick == 0 ||
            now - s_last_local_audio_queue_fail_log_tick >= pdMS_TO_TICKS(1000)) {
            s_last_local_audio_queue_fail_log_tick = now;
            ESP_LOGW(TAG, "local audio queue pressure: %s", esp_err_to_name(enqueue_ret));
        }
    }
}

static void tirtc_session_on_event(int event, const void *data, int len)
{
    (void)data;
    (void)len;

    tirtc_session_event_t rtc_event = {0};

    switch (event) {
    case TIRTC_EVENT_SYS_STARTED:
        taskENTER_CRITICAL(&s_rtc_lock);
        s_sys_started_callback_count++;
        taskEXIT_CRITICAL(&s_rtc_lock);
        rtc_event.type = TIRTC_SESSION_EVENT_SYS_STARTED;
        break;
    case TIRTC_EVENT_SYS_STOPPED:
        taskENTER_CRITICAL(&s_rtc_lock);
        s_sdk_stop_notified = true;
        rtc_event.payload.system.generation =
            s_pending_stop_generation != 0U ? s_pending_stop_generation : s_sdk_generation;
        taskEXIT_CRITICAL(&s_rtc_lock);
        rtc_event.type = TIRTC_SESSION_EVENT_SYS_STOPPED;
        break;
    case TIRTC_EVENT_ACCESS_HIJACKING:
        rtc_event.type = TIRTC_SESSION_EVENT_ACCESS_HIJACKING;
        break;
    default:
        ESP_LOGW(TAG, "rtc system event ignored: event=%d", event);
        return;
    }

    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("sys event drop");
        ESP_LOGW(TAG, "rtc event queue full: system event dropped event=%d", event);
    }
}

static void tirtc_session_on_conn_accepted(tirtc_conn_t hconn)
{
    tirtc_session_conn_accept_result_t accept_result =
        tirtc_session_accept_connection(hconn, false, true);
    bool accepted = accept_result == TIRTC_SESSION_CONN_ACCEPTED;
    tirtc_session_mode_t mode = TIRTC_SESSION_MODE_LISTEN;
    tirtc_session_state_t state = TIRTC_SESSION_STATE_STOPPED;
    bool sdk_started = false;
    bool start_in_progress = false;
    bool stop_in_progress = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    mode = s_session_mode;
    state = s_state;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    stop_in_progress = s_stop_in_progress;
    taskEXIT_CRITICAL(&s_rtc_lock);

    APP_LOG_DETAIL(TAG,
                   "rtc conn accepted callback: hconn=%p accepted=%d mode=%u state=%u sdk_started=%d start=%d stop=%d",
                   hconn,
                   accepted,
                   (unsigned)mode,
                   (unsigned)state,
                   sdk_started ? 1 : 0,
                   start_in_progress ? 1 : 0,
                   stop_in_progress ? 1 : 0);

    if (!accepted) {
        if (accept_result == TIRTC_SESSION_CONN_ACCEPT_STALE_CLOSING) {
            APP_LOG_DETAIL(TAG,
                           "rtc conn accepted callback ignored: hconn=%p already closing",
                           hconn);
            return;
        }
        ESP_LOGW(TAG, "rtc connection rejected: hconn=%p", hconn);
        (void)tirtc_session_disconnect_with_sdk_lock(hconn);
        return;
    }

    tirtc_session_bind_connection_user_data(hconn);

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ACCEPTED,
        .payload.conn = {
            .conn = hconn,
            .error = 0,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("conn accept inline");
        ESP_LOGW(TAG, "rtc event queue full: connection accept handled inline");
        tirtc_session_handle_runtime_event(&rtc_event);
    }
}

static void tirtc_session_on_conn_error(tirtc_conn_t hconn, int error)
{
    tirtc_session_log_connection_user_data("connection error", hconn);
    APP_LOG_DETAIL(TAG,
                   "rtc connection error callback: hconn=%p code=%d detail=%s",
                   hconn,
                   error,
                   TiRtcGetErrorStr(error));
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_CONN_ERROR,
        .payload.conn = {
            .conn = hconn,
            .error = error,
        },
    };
    if (!tirtc_session_enqueue_teardown_event(&rtc_event)) {
        tirtc_session_note_event("conn err inline");
        ESP_LOGE(TAG, "rtc teardown queue failed after connection error: err=%d", error);
        tirtc_session_handle_connection_loss(hconn, error);
    }
}

static void tirtc_session_on_disconnected(tirtc_conn_t hconn)
{
    tirtc_session_log_connection_user_data("disconnected callback", hconn);
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_DISCONNECTED,
        .payload.conn = {
            .conn = hconn,
            .error = 0,
        },
    };
    if (!tirtc_session_enqueue_teardown_event(&rtc_event)) {
        tirtc_session_note_event("disconnect inline");
        ESP_LOGE(TAG, "rtc teardown queue failed after disconnect");
        tirtc_session_handle_connection_loss(hconn, 0);
    }
}

static void tirtc_session_log_bad_remote_audio_frame(tirtc_conn_t hconn,
                                                    const TIRTCFRAMEINFO *frame_info)
{
    TickType_t now_tick = xTaskGetTickCount();
    const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_BAD_REMOTE_AUDIO_LOG_INTERVAL_MS);

    if (s_last_bad_remote_audio_log_tick != 0 &&
        now_tick - s_last_bad_remote_audio_log_tick < log_interval_ticks) {
        return;
    }
    s_last_bad_remote_audio_log_tick = now_tick;

    ESP_LOGW(TAG,
             "remote audio dropped: invalid length hconn=%p stream=%u media=%u(%s) flags=%u len=%lu max=%u",
             hconn,
             (unsigned)frame_info->stream_id,
             (unsigned)frame_info->media,
             tirtc_session_media_name(frame_info->media),
             (unsigned)frame_info->flags,
             (unsigned long)frame_info->length,
             TIRTC_SESSION_REMOTE_AUDIO_MAX_PAYLOAD);
}

static void tirtc_session_on_audio(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data)
{
    tirtc_conn_t active_conn = NULL;
    size_t playback_data_len = 0;
    bool log_first_packet = false;
    bool log_window = false;
    uint32_t window_frames = 0;
    size_t window_payload_bytes = 0;
    size_t window_playback_bytes = 0;

    if (!tirtc_session_try_get_active_conn(&active_conn) || hconn != active_conn) {
        return;
    }

    if (frame_info == NULL || data == NULL || frame_info->length == 0) {
        return;
    }

    if (frame_info->length > TIRTC_SESSION_REMOTE_AUDIO_MAX_PAYLOAD) {
        tirtc_session_log_bad_remote_audio_frame(hconn, frame_info);
        tirtc_session_note_event("bad audio len");
        return;
    }

    if (tirtc_session_media_submit_remote_audio(frame_info->media,
                                                frame_info->flags,
                                                (const uint8_t *)data,
                                                frame_info->length,
                                                frame_info->ts,
                                                &playback_data_len) != ESP_OK) {
        tirtc_session_note_event("remote audio drop");
        return;
    }

    const TickType_t now_tick = xTaskGetTickCount();
    const TickType_t log_interval_ticks = pdMS_TO_TICKS(TIRTC_SESSION_RX_LOG_INTERVAL_MS);

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_remote_audio_first_packet_logged) {
        s_remote_audio_first_packet_logged = true;
        s_last_remote_audio_rx_log_tick = now_tick;
        log_first_packet = true;
    }
    s_stats.rx_audio_frames++;
    s_stats.rx_audio_bytes += playback_data_len;
    s_remote_audio_rx_window_frames++;
    s_remote_audio_rx_window_payload_bytes += frame_info->length;
    s_remote_audio_rx_window_playback_bytes += playback_data_len;
    tirtc_session_set_last_event_locked("audio rx");
    if (!log_first_packet &&
        (s_last_remote_audio_rx_log_tick == 0 || now_tick - s_last_remote_audio_rx_log_tick >= log_interval_ticks)) {
        log_window = true;
        window_frames = s_remote_audio_rx_window_frames;
        window_payload_bytes = s_remote_audio_rx_window_payload_bytes;
        window_playback_bytes = s_remote_audio_rx_window_playback_bytes;
        s_remote_audio_rx_window_frames = 0;
        s_remote_audio_rx_window_payload_bytes = 0;
        s_remote_audio_rx_window_playback_bytes = 0;
        s_last_remote_audio_rx_log_tick = now_tick;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_first_packet) {
        ESP_LOGI(TAG,
                 "remote audio first packet stream=%u media=%u(%s) flags=%u ts=%lu payload=%u playback=%u",
                 (unsigned)frame_info->stream_id,
                 (unsigned)frame_info->media,
                 tirtc_session_media_name(frame_info->media),
                 (unsigned)frame_info->flags,
                 (unsigned long)frame_info->ts,
                 (unsigned)frame_info->length,
                 (unsigned)playback_data_len);
    } else if (log_window) {
        ESP_LOGD(TAG,
                 "remote audio rx frames=%lu payload=%u playback=%u media=%u(%s) flags=%u",
                 (unsigned long)window_frames,
                 (unsigned)window_payload_bytes,
                 (unsigned)window_playback_bytes,
                 (unsigned)frame_info->media,
                 tirtc_session_media_name(frame_info->media),
                 (unsigned)frame_info->flags);
    }
}

static void tirtc_session_on_video(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data)
{
    tirtc_conn_t active_conn = NULL;
    bool log_first_packet = false;
    uint64_t received_at_us = 0U;
    uint64_t accepted_at_us = 0U;
    uint64_t first_request_at_us = 0U;
    uint8_t request_attempts = 0U;

    if (!tirtc_session_try_get_active_conn(&active_conn) || hconn != active_conn) {
        return;
    }

    if (frame_info == NULL || data == NULL || frame_info->length == 0) {
        return;
    }

    if (frame_info->media != TIRTC_VIDEO_H264 && frame_info->media != TIRTC_VIDEO_JPEG) {
        tirtc_session_note_event("bad remote video");
        ESP_LOGW(TAG, "unsupported remote video media: %u", frame_info->media);
        return;
    }

    received_at_us = (uint64_t)esp_timer_get_time();
    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_remote_video_last_packet_us != 0U &&
        received_at_us > s_remote_video_last_packet_us) {
        uint64_t gap_us = received_at_us - s_remote_video_last_packet_us;
        uint32_t bounded_gap_us = gap_us > UINT32_MAX ? UINT32_MAX : (uint32_t)gap_us;
        if (bounded_gap_us > s_remote_video_callback_gap_window_max_us) {
            s_remote_video_callback_gap_window_max_us = bounded_gap_us;
        }
    }
    s_remote_video_last_packet_us = received_at_us;
    if (s_remote_video_first_submit_attempt_us == 0U) {
        s_remote_video_first_submit_attempt_us = received_at_us;
    }
    if (s_remote_video_callback_frames < UINT32_MAX) {
        s_remote_video_callback_frames++;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    esp_err_t submit_ret = tirtc_session_media_submit_remote_video(frame_info->media,
                                                                   frame_info->flags,
                                                                   (const uint8_t *)data,
                                                                   frame_info->length,
                                                                   frame_info->ts);
    if (submit_ret != ESP_OK) {
        taskENTER_CRITICAL(&s_rtc_lock);
        if (s_remote_video_submit_failures < UINT32_MAX) {
            s_remote_video_submit_failures++;
        }
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("remote video drop");
        if (frame_info->media == TIRTC_VIDEO_H264 &&
            submit_ret != ESP_ERR_INVALID_STATE &&
            submit_ret != ESP_ERR_NOT_SUPPORTED &&
            tirtc_session_take_remote_key_frame_retry_slot()) {
            (void)tirtc_session_request_remote_key_frame(hconn,
                                                        frame_info->stream_id,
                                                        "H264 renderer resync");
        }
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_remote_video_first_packet_logged) {
        s_remote_video_first_packet_logged = true;
        s_remote_video_first_packet_retry_armed = false;
        s_remote_video_first_packet_retry_due = false;
        log_first_packet = true;
    }
    accepted_at_us = s_active_conn_accepted_at_us;
    first_request_at_us = s_remote_video_first_request_at_us;
    request_attempts = s_remote_video_request_attempts;
    s_remote_video_last_submit_us = received_at_us;
    s_stats.rx_video_frames++;
    s_stats.rx_video_bytes += frame_info->length;
    tirtc_session_set_last_event_locked("video rx");
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (log_first_packet) {
        ESP_LOGI(TAG,
                 "remote video first packet media=%u(%s) flags=%u payload=%u "
                 "accepted_ms=%llu subscribe_wait_ms=%llu attempts=%u",
                 frame_info->media,
                 tirtc_session_media_name(frame_info->media),
                 frame_info->flags,
                 (unsigned)frame_info->length,
                 accepted_at_us != 0U ?
                     (unsigned long long)((esp_timer_get_time() - accepted_at_us) / 1000ULL) :
                     0ULL,
                 first_request_at_us != 0U ?
                     (unsigned long long)((esp_timer_get_time() - first_request_at_us) / 1000ULL) :
                     0ULL,
                 (unsigned)request_attempts);
        tirtc_session_retry_remote_media_request_after_delay(false,
                                                            true,
                                                            "audio after first video",
                                                            TIRTC_SESSION_MEDIA_AUDIO_FOLLOWUP_DELAY_US);
    }
}

static void tirtc_session_on_message(tirtc_conn_t hconn, const TIRTCFRAMEINFO *frame_info, void *data)
{
    tirtc_conn_t active_conn = NULL;

    if (!tirtc_session_try_get_active_conn(&active_conn) || hconn != active_conn) {
        return;
    }

    if (frame_info == NULL) {
        return;
    }

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_REMOTE_MESSAGE,
        .payload.message = {
            .conn = hconn,
            .media = frame_info->media,
            .stream_id = frame_info->stream_id,
            .flags = frame_info->flags,
            .ts = frame_info->ts,
            .data_len = frame_info->length,
        },
    };

    if (tirtc_session_copy_payload(data, frame_info->length, &rtc_event.payload.message.data) != ESP_OK ||
        !tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("remote msg drop");
        tirtc_session_free_event_payload(&rtc_event);
    }
}

static void tirtc_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    uint16_t cmd = (uint16_t)(cmdw & ~TIRTC_SESSION_CMD_RESP_BIT);
    bool control_cmd = cmd == TIRTC_SESSION_CMD_CALL ||
                       cmd == TIRTC_SESSION_CMD_HANGUP ||
                       cmd == TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED ||
                       cmd == TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP;

    APP_LOG_DETAIL(TAG,
                   "rtc command callback: hconn=%p cmd=0x%04x resp=%d len=%lu",
                   hconn,
                   (unsigned)cmd,
                   (cmdw & TIRTC_SESSION_CMD_RESP_BIT) != 0,
                   (unsigned long)len);

    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_REMOTE_COMMAND,
        .payload.command = {
            .conn = hconn,
            .cmdw = cmdw,
            .data_len = len,
        },
    };

    if (tirtc_session_copy_payload(data, len, &rtc_event.payload.command.data) != ESP_OK) {
        tirtc_session_note_event("remote cmd drop");
        ESP_LOGW(TAG, "remote command dropped: payload alloc failed cmd=0x%08lx", (unsigned long)cmdw);
        tirtc_session_free_event_payload(&rtc_event);
        return;
    }

    if (!tirtc_session_enqueue_event(&rtc_event, control_cmd ? 0 : TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        if (control_cmd) {
            tirtc_session_note_event("remote cmd inline");
            ESP_LOGW(TAG, "rtc event queue full: handling control command inline cmd=0x%08lx", (unsigned long)cmdw);
            tirtc_session_handle_remote_command(&rtc_event);
        } else {
            tirtc_session_note_event("remote cmd drop");
            ESP_LOGW(TAG, "rtc event queue full: remote command dropped cmd=0x%08lx", (unsigned long)cmdw);
        }
        tirtc_session_free_event_payload(&rtc_event);
    }
}

static int tirtc_session_on_subscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_SUBSCRIBE_VIDEO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("sub video drop");
        ESP_LOGW(TAG, "rtc event queue full: subscribe video dropped stream=%u", stream_id);
    }
    return 0;
}

static void tirtc_session_on_unsubscribe_video(tirtc_conn_t hconn, uint8_t stream_id)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_UNSUBSCRIBE_VIDEO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("unsub video drop");
        ESP_LOGW(TAG, "rtc event queue full: unsubscribe video dropped stream=%u", stream_id);
    }
}

static int tirtc_session_on_subscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_SUBSCRIBE_AUDIO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("sub audio drop");
        ESP_LOGW(TAG, "rtc event queue full: subscribe audio dropped stream=%u", stream_id);
    }
    return 0;
}

static void tirtc_session_on_unsubscribe_audio(tirtc_conn_t hconn, uint8_t stream_id)
{
    tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_UNSUBSCRIBE_AUDIO,
        .payload.subscribe = {
            .conn = hconn,
            .stream_id = stream_id,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("unsub audio drop");
        ESP_LOGW(TAG, "rtc event queue full: unsubscribe audio dropped stream=%u", stream_id);
    }
}

#if TIRTC_SESSION_HAS_TGMP_BITRATE_CONTROL
static void tirtc_session_on_video_bitrate_required(tirtc_conn_t hconn,
                                                    uint8_t stream_id,
                                                    uint32_t target_bitrate_bps)
{
    if (hconn == NULL || target_bitrate_bps == 0U) {
        return;
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    bool post_event = true;

    taskENTER_CRITICAL(&s_rtc_lock);
    if (hconn == s_last_video_bitrate_event_conn &&
        s_last_video_bitrate_event_us != 0U &&
        now_us >= s_last_video_bitrate_event_us &&
        now_us - s_last_video_bitrate_event_us <
            TIRTC_SESSION_BITRATE_EVENT_MIN_INTERVAL_US) {
        const uint32_t delta_bps =
            target_bitrate_bps >= s_last_video_bitrate_event_bps ?
                target_bitrate_bps - s_last_video_bitrate_event_bps :
                s_last_video_bitrate_event_bps - target_bitrate_bps;
        post_event = delta_bps >= TIRTC_SESSION_BITRATE_EVENT_FAST_STEP_BPS;
    }
    if (post_event) {
        s_last_video_bitrate_event_conn = hconn;
        s_last_video_bitrate_event_bps = target_bitrate_bps;
        s_last_video_bitrate_event_us = now_us;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!post_event) {
        return;
    }

    const tirtc_session_event_t rtc_event = {
        .type = TIRTC_SESSION_EVENT_VIDEO_BITRATE_REQUIRED,
        .payload.video_bitrate = {
            .conn = hconn,
            .stream_id = stream_id,
            .target_bitrate_bps = target_bitrate_bps,
        },
    };
    if (!tirtc_session_enqueue_event(&rtc_event, 0)) {
        tirtc_session_note_event("bitrate event drop");
        APP_LOG_DETAIL(TAG,
                       "rtc event queue full: video bitrate target dropped stream=%u target=%ukbps",
                       (unsigned)stream_id,
                       (unsigned)(target_bitrate_bps / 1000U));
    }
}
#endif

static void tirtc_session_sdk_log_cb(const char *log, uint32_t length)
{
    if (log == NULL || length == 0) {
        return;
    }

    uint32_t offset = 0;
    while (offset < length) {
        uint32_t raw_chunk_len = length - offset;
        if (raw_chunk_len > TIRTC_SESSION_SDK_LOG_CHUNK_LEN) {
            raw_chunk_len = TIRTC_SESSION_SDK_LOG_CHUNK_LEN;
        }

        uint32_t chunk_len = raw_chunk_len;
        while (chunk_len > 0) {
            char last = log[offset + chunk_len - 1];
            if (last != '\n' && last != '\r') {
                break;
            }
            --chunk_len;
        }

        if (chunk_len > 0) {
            char chunk[TIRTC_SESSION_SDK_LOG_CHUNK_LEN + 1];
            memcpy(chunk, log + offset, chunk_len);
            chunk[chunk_len] = '\0';
            ESP_LOGI(TIRTC_SDK_LOG_TAG, "%s", chunk);
        }

        offset += raw_chunk_len;
    }
}

static void tirtc_session_configure_sdk_logs(bool announce)
{
    TiRtcLogSetCallback(tirtc_session_sdk_log_cb);
    TiRtcLogSetLevel(TIRTC_SESSION_SDK_LOG_LEVEL);

    if (announce) {
        APP_LOG_DETAIL(TAG,
                       "tirtc logs enabled: sdk_callback=esp_log sdk_level=%d",
                       TIRTC_SESSION_SDK_LOG_LEVEL);
    }
}

static void tirtc_session_on_request_key_frame(tirtc_conn_t hconn, uint8_t stream_id)
{
    bool active = false;
    bool external_video = false;
    bool forced_publish = false;
    bool apply_policy = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    active = hconn != NULL && hconn == s_active_conn && s_sdk_started && !s_start_in_progress &&
             !s_stop_in_progress && s_closing_conn == NULL && s_call_active;
    external_video = hconn != NULL &&
                     hconn == s_active_conn &&
                     s_sdk_started && !s_start_in_progress &&
                     !s_stop_in_progress && s_closing_conn == NULL &&
                     s_external_video_active &&
                     s_external_video_conn == hconn &&
                     s_external_video_stream_id == stream_id;
    if (active && stream_id == TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID) {
        s_peer_wants_video = true;
        s_peer_video_control_seen = true;
        tirtc_session_mark_local_video_requested_locked();
        forced_publish = tirtc_session_maybe_force_local_video_publish_locked();
        apply_policy = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!active && !external_video) {
        ESP_LOGD(TAG, "ignore key frame request for inactive connection hconn=%p stream=%u", hconn, stream_id);
        return;
    }

    if (!external_video && stream_id != TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID) {
        ESP_LOGD(TAG, "ignore key frame request for unsupported stream=%u", stream_id);
        return;
    }

    tirtc_session_note_event("key frame asked");
    APP_LOG_DETAIL(TAG,
                   "KF req: src=%s stream=%u force=%d",
                   external_video ? "ext" : "local",
                   (unsigned)stream_id,
                   forced_publish);

    tirtc_session_media_request_video_key_frame();
    if (apply_policy) {
        tirtc_session_apply_local_media_policy();
    }
}

void tirtc_session_apply_hangup_local_state(void)
{
    bool call_media_was_active = false;

    tirtc_session_release_remote_media();

    taskENTER_CRITICAL(&s_rtc_lock);
    call_media_was_active = s_call_active && !s_call_media_deferred;
    s_call_active = false;
    s_call_media_deferred = false;
    s_incoming_call_pending = false;
    s_pending_call_cmdw = 0;
    s_media_bootstrap_pending = false;
    s_peer_video_subscription_active = false;
    s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    s_local_h264_key_frame_queued = false;
    s_local_h264_key_frame_published = false;
    s_local_h264_recovery_pending = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_cancel_media_bootstrap();

    tirtc_session_apply_local_media_policy();
    tirtc_session_flush_local_video_tx_queue();
    tirtc_session_flush_local_audio_tx_queue();
    tirtc_session_media_flush();

    tirtc_session_note_event("hangup local");
    if (call_media_was_active) {
        tirtc_session_notify_call_active(false);
    }
}

esp_err_t tirtc_session_set_external_media_call_active(tirtc_conn_t conn,
                                                       bool active,
                                                       bool local_video_enabled,
                                                       bool remote_video_enabled)
{
    tirtc_conn_t active_conn = NULL;

    ESP_RETURN_ON_FALSE(conn != NULL, ESP_ERR_INVALID_ARG, TAG, "external call connection is null");
    ESP_RETURN_ON_FALSE(tirtc_session_try_get_active_conn(&active_conn) && active_conn == conn,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "external call connection is inactive");

    if (active) {
        taskENTER_CRITICAL(&s_rtc_lock);
        /* External audio owns microphone capture; video remains independently
         * configurable in both directions. */
        s_media_profile = TIRTC_SESSION_MEDIA_PROFILE_EXTERNAL_AUDIO;
        s_remote_video_receive_enabled = remote_video_enabled;
        tirtc_session_sync_stats_locked();
        taskEXIT_CRITICAL(&s_rtc_lock);

        (void)tirtc_session_set_local_video_send_enabled(local_video_enabled);
        (void)tirtc_session_set_local_audio_send_enabled(true);
        tirtc_session_complete_call_response(true);
        tirtc_session_apply_local_media_policy();
        tirtc_session_request_remote_media();
    } else {
        tirtc_session_apply_hangup_local_state();
    }
    return ESP_OK;
}

esp_err_t tirtc_session_set_external_audio_call_active(tirtc_conn_t conn, bool active)
{
    bool local_video_enabled = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    local_video_enabled = s_local_video_send_enabled;
    taskEXIT_CRITICAL(&s_rtc_lock);

    return tirtc_session_set_external_media_call_active(conn,
                                                        active,
                                                        local_video_enabled,
                                                        false);
}

void tirtc_session_apply_local_media_policy(void)
{
    bool enable_audio = false;
    bool enable_video = false;
    bool audio_publish_forced = false;
    bool video_publish_forced = false;
    bool builtin_capture_allowed = false;
    bool capture_changed = false;
    bool video_capture_changed = false;
    bool sdk_started = false;
    bool active_conn_present = false;
    bool call_active = false;
    bool video_send_enabled = false;
    bool audio_send_enabled = false;
    bool media_bootstrap_pending = false;
    bool peer_wants_audio = false;
    bool peer_wants_video = false;
    bool video_capture_peer_ready = false;
    bool audio_deferred_for_video = false;
    uint8_t local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    uint8_t local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
    bool test_video_active = tirtc_session_is_test_video_active();
    bool test_audio_active = tirtc_session_is_test_audio_active();
    bool test_media_active = test_video_active || test_audio_active;

    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_sync_test_media_publish_locked(test_video_active, test_audio_active);
    builtin_capture_allowed = tirtc_session_media_profile_uses_builtin_capture_locked();
    video_publish_forced = tirtc_session_maybe_force_local_video_publish_locked();
    audio_publish_forced = tirtc_session_maybe_force_local_audio_publish_locked();
    peer_wants_audio = s_peer_wants_audio;
    peer_wants_video = s_peer_wants_video;
    video_capture_peer_ready = s_peer_wants_video;
#if !CONFIG_APP_RTC_WAIT_VIDEO_SUBSCRIBE_BEFORE_CAPTURE
    video_capture_peer_ready = video_capture_peer_ready || s_local_video_publish_forced;
#endif
    local_audio_stream_id = s_local_audio_stream_id;
    local_video_stream_id = s_local_video_stream_id;
    sdk_started = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL;
    active_conn_present = s_active_conn != NULL;
    call_active = s_call_active;
    video_send_enabled = s_local_video_send_enabled;
    audio_send_enabled = s_local_audio_send_enabled;
    media_bootstrap_pending = s_media_bootstrap_pending;
    enable_audio = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
                   s_active_conn != NULL && s_call_active && s_local_audio_send_enabled &&
                   builtin_capture_allowed &&
                   !s_media_bootstrap_pending &&
                   s_local_audio_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    enable_video = s_sdk_started && !s_start_in_progress && !s_stop_in_progress && s_closing_conn == NULL &&
                   s_active_conn != NULL && s_call_active && s_local_video_send_enabled &&
                   video_capture_peer_ready &&
                   s_local_video_stream_id != TIRTC_SESSION_INVALID_STREAM_ID;
    audio_deferred_for_video = enable_video && tirtc_session_should_defer_audio_for_local_video_locked();
    if (audio_deferred_for_video) {
        enable_audio = false;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (test_media_active) {
        enable_audio = false;
        enable_video = false;
    }

    if (video_publish_forced) {
        APP_LOG_DETAIL(TAG,
                       "local video publish fallback: stream=%u peer_video=%d",
                       (unsigned)TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID,
                       peer_wants_video);
    }

    if (audio_publish_forced) {
        APP_LOG_DETAIL(TAG,
                       "local audio publish fallback: stream=%u peer_audio=%d",
                       (unsigned)TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID,
                       peer_wants_audio);
    }

    if (audio_deferred_for_video) {
        APP_LOG_DETAIL(TAG,
                       "rtc microphone capture deferred: video-first profile stream=%u",
                       (unsigned)local_audio_stream_id);
    }

    esp_err_t video_ret = tirtc_session_media_set_video_capture_enabled(enable_video);
    if (video_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "video capture policy failed: enable=%d peer_video=%d stream=%u ret=%s",
                 enable_video,
                 peer_wants_video,
                 (unsigned)local_video_stream_id,
                 esp_err_to_name(video_ret));
    }

    esp_err_t capture_ret = tirtc_session_media_set_capture_enabled(enable_audio);
    if (capture_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "audio capture policy failed: enable=%d peer_audio=%d stream=%u builtin=%d ret=%s",
                 enable_audio,
                 peer_wants_audio,
                 (unsigned)local_audio_stream_id,
                 builtin_capture_allowed,
                 esp_err_to_name(capture_ret));
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (capture_ret == ESP_OK && s_builtin_capture_enabled != enable_audio) {
        s_builtin_capture_enabled = enable_audio;
        capture_changed = true;
    }
    if (video_ret == ESP_OK && s_builtin_video_capture_enabled != enable_video) {
        s_builtin_video_capture_enabled = enable_video;
        video_capture_changed = true;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (video_capture_changed) {
        APP_LOG_DETAIL(TAG,
                       "rtc camera capture %s: stream=%u peer_video=%d peer_ready=%d send=%d call=%d conn=%d sdk=%d bootstrap=%d",
                       enable_video ? "enabled" : "disabled",
                       (unsigned)local_video_stream_id,
                       peer_wants_video,
                       video_capture_peer_ready,
                       video_send_enabled,
                       call_active,
                       active_conn_present,
                       sdk_started,
                       media_bootstrap_pending);
    }

    if (capture_changed) {
        APP_LOG_DETAIL(TAG,
                       "rtc microphone capture %s: stream=%u peer_audio=%d send=%d call=%d conn=%d sdk=%d bootstrap=%d owner=%s",
                       enable_audio ? "enabled" : "disabled",
                       (unsigned)local_audio_stream_id,
                       peer_wants_audio,
                       audio_send_enabled,
                       call_active,
                       active_conn_present,
                       sdk_started,
                       media_bootstrap_pending,
                       builtin_capture_allowed ? "tirtc" : "external");
    }
}

void tirtc_session_refresh_media_policy(void)
{
    tirtc_session_apply_local_media_policy();
}

void tirtc_session_handle_connection_loss(tirtc_conn_t hconn, int error)
{
    bool tracked = false;
    bool was_sdk_started = false;
    bool newly_detached = false;
    bool wait_for_disconnect = false;

    if (error != 0) {
        tirtc_session_log_connection_close_snapshot("connection error", hconn, error);
    }

    tracked = tirtc_session_begin_connection_shutdown(hconn,
                                                     error,
                                                     &was_sdk_started,
                                                     &newly_detached);
    if (!tracked) {
        ESP_LOGD(TAG,
                 "ignore connection loss for inactive hconn=%p error=%d",
                 hconn,
                 error);
        return;
    }

    if (error != 0) {
        tirtc_session_set_last_error(error);
        tirtc_session_note_event("conn error");
        APP_LOG_DETAIL(TAG, "rtc connection teardown: code=%d", error);

        if (newly_detached) {
            if (error == TIRTC_E_CONN_REMOTECLOSE) {
                int disconnect_ret = tirtc_session_disconnect_with_sdk_lock(hconn);

                if (disconnect_ret >= 0) {
                    /*
                     * The peer has already closed its side. Ask the SDK to
                     * release the handle and keep closing_conn until the SDK
                     * confirms DISCONNECTED. Advertising READY before that
                     * confirmation lets a new WHIP enter the SDK while the old
                     * connection is still being destroyed.
                     */
                    ESP_LOGI(TAG,
                             "remote close cleanup requested: hconn=%p ret=%d, waiting for disconnected",
                             hconn,
                             disconnect_ret);
                    wait_for_disconnect = true;
                } else {
                    ESP_LOGW(TAG,
                             "disconnect after remote close failed hconn=%p ret=%s; completing teardown inline",
                             hconn,
                             TiRtcGetErrorStr(disconnect_ret));
                }
            } else if (error == TIRTC_E_INVALID_HANDLE) {
                int disconnect_ret = tirtc_session_disconnect_with_sdk_lock(hconn);

                if (disconnect_ret >= 0) {
                    ESP_LOGD(TAG,
                             "request disconnect after invalid handle hconn=%p ret=%d",
                             hconn,
                             disconnect_ret);
                    wait_for_disconnect = true;
                } else {
                    ESP_LOGW(TAG,
                             "disconnect after invalid handle failed hconn=%p ret=%s; completing teardown inline",
                             hconn,
                             TiRtcGetErrorStr(disconnect_ret));
                }
            } else {
                int disconnect_ret = tirtc_session_disconnect_with_sdk_lock(hconn);
                if (disconnect_ret >= 0) {
                    wait_for_disconnect = true;
                } else {
                    tirtc_session_set_last_error(disconnect_ret);
                    ESP_LOGW(TAG,
                             "request disconnect during error handling failed: %s",
                             TiRtcGetErrorStr(disconnect_ret));
                }
            }
        } else {
            wait_for_disconnect = true;
        }

        if (wait_for_disconnect) {
            (void)tirtc_session_schedule_disconnect_watchdog("connection loss", TIRTC_SESSION_DISCONNECT_TIMEOUT_US);
            return;
        }
    } else {
        tirtc_session_note_event("disconnect done");
        APP_LOG_DETAIL(TAG, "rtc disconnected callback complete: hconn=%p", hconn);
    }

    tirtc_session_complete_connection_shutdown(hconn, was_sdk_started);
}

static void tirtc_session_run_worker_maintenance(uint64_t now_us)
{
    bool run_deferred_full_reset = false;
    bool run_deferred_start = false;

    tirtc_session_refresh_send_buffer_used();
    tirtc_session_monitor_local_video_tx_liveness();
    tirtc_session_monitor_remote_video_rx_liveness();

    taskENTER_CRITICAL(&s_rtc_lock);
    run_deferred_full_reset = s_deferred_full_reset_pending &&
                              s_deferred_full_reset_due_at_us != 0U &&
                              now_us >= s_deferred_full_reset_due_at_us;
    run_deferred_start = s_deferred_start_after_full_reset_pending &&
                         s_deferred_start_after_full_reset_due_at_us != 0U &&
                         now_us >= s_deferred_start_after_full_reset_due_at_us;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (run_deferred_full_reset) {
        tirtc_session_handle_deferred_full_reset();
    }
    if (run_deferred_start) {
        tirtc_session_handle_deferred_start_after_full_reset();
    }
}

static void tirtc_session_run_worker_maintenance_if_due(uint64_t *last_run_us)
{
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    if (*last_run_us != 0U && now_us >= *last_run_us &&
        now_us - *last_run_us <
            (uint64_t)TIRTC_SESSION_WORKER_POLL_MS * 1000ULL) {
        return;
    }

    *last_run_us = now_us;
    tirtc_session_run_worker_maintenance(now_us);
}

static void tirtc_session_worker_task(void *ctx)
{
    (void)ctx;
    tirtc_session_event_t event = {0};
    uint64_t last_maintenance_us = 0U;

    while (true) {
        BaseType_t event_received = xQueueReceive(
            s_event_queue,
            &event,
            pdMS_TO_TICKS(TIRTC_SESSION_WORKER_POLL_MS));
        if (event_received != pdTRUE) {
            tirtc_session_run_worker_maintenance_if_due(&last_maintenance_us);
            continue;
        }

        switch (event.type) {
        case TIRTC_SESSION_EVENT_START_IF_READY:
            (void)tirtc_session_start_sdk_from_worker();
            break;
        case TIRTC_SESSION_EVENT_REMOTE_MESSAGE:
            if (!tirtc_session_is_connection_usable(event.payload.message.conn)) {
                tirtc_session_note_event("stale remote msg");
                APP_LOG_DETAIL(TAG,
                               "remote message ignored: inactive hconn=%p",
                               event.payload.message.conn);
                break;
            }
            (void)tirtc_session_notify_message(event.payload.message.conn,
                                               event.payload.message.media,
                                               event.payload.message.stream_id,
                                               event.payload.message.flags,
                                               event.payload.message.data,
                                               (uint32_t)event.payload.message.data_len);
            tirtc_session_handle_remote_message(&event);
            break;
        case TIRTC_SESSION_EVENT_REMOTE_COMMAND:
            if (!tirtc_session_is_connection_usable(event.payload.command.conn)) {
                tirtc_session_note_event("stale remote cmd");
                APP_LOG_DETAIL(TAG,
                               "remote command ignored before observers: inactive hconn=%p cmdw=0x%08lx",
                               event.payload.command.conn,
                               (unsigned long)event.payload.command.cmdw);
                break;
            }
            if (!tirtc_session_notify_command(event.payload.command.conn,
                                              event.payload.command.cmdw,
                                              event.payload.command.data,
                                              (uint32_t)event.payload.command.data_len)) {
                tirtc_session_handle_remote_command(&event);
            }
            break;
        case TIRTC_SESSION_EVENT_VIDEO_BITRATE_REQUIRED:
            if (tirtc_session_is_connection_usable(event.payload.video_bitrate.conn)) {
                tirtc_session_notify_video_bitrate_required(
                    event.payload.video_bitrate.conn,
                    event.payload.video_bitrate.stream_id,
                    event.payload.video_bitrate.target_bitrate_bps);
            } else {
                APP_LOG_DETAIL(TAG,
                               "video bitrate target ignored: inactive hconn=%p stream=%u target=%ukbps",
                               event.payload.video_bitrate.conn,
                               (unsigned)event.payload.video_bitrate.stream_id,
                               (unsigned)(event.payload.video_bitrate.target_bitrate_bps / 1000U));
            }
            break;
        case TIRTC_SESSION_EVENT_CONN_ERROR:
            if (tirtc_session_is_connection_tracked(event.payload.conn.conn)) {
                tirtc_session_notify_connection_error(event.payload.conn.conn, event.payload.conn.error);
            } else {
                APP_LOG_DETAIL(TAG,
                               "connection error ignored before observers: stale hconn=%p error=%d",
                               event.payload.conn.conn,
                               event.payload.conn.error);
            }
            tirtc_session_handle_runtime_event(&event);
            break;
        case TIRTC_SESSION_EVENT_DISCONNECTED:
            if (tirtc_session_is_connection_tracked(event.payload.conn.conn)) {
                tirtc_session_notify_disconnected(event.payload.conn.conn);
            } else {
                APP_LOG_DETAIL(TAG,
                               "disconnect ignored before observers: stale hconn=%p",
                               event.payload.conn.conn);
            }
            tirtc_session_handle_runtime_event(&event);
            break;
        case TIRTC_SESSION_EVENT_DISCONNECT_REQUEST:
            tirtc_session_handle_disconnect_request(&event);
            break;
        default:
            tirtc_session_handle_runtime_event(&event);
            break;
        }

        tirtc_session_free_event_payload(&event);
        tirtc_session_run_worker_maintenance_if_due(&last_maintenance_us);
    }
}

esp_err_t tirtc_session_configure(const tirtc_session_config_t *config)
{
    tirtc_session_config_t validated_config;
    bool config_changed = false;
    bool needs_full_reset = false;
    bool sdk_initialized = false;
    bool sdk_started = false;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    validated_config = *config;
    if (!tirtc_session_service_endpoint_is_valid(&validated_config)) {
        ESP_LOGE(TAG, "rtc service endpoint rejected: explicit HTTPS URL with a host is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (tirtc_connect_is_connecting()) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }
    sdk_initialized = s_sdk_initialized;
    sdk_started = s_sdk_started;
    config_changed = tirtc_session_config_differs(&s_config, &validated_config);
    if (config_changed) {
        s_config = validated_config;
        s_session_mode = validated_config.default_session_mode;
        needs_full_reset = sdk_initialized || sdk_started;
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (needs_full_reset) {
        ESP_LOGI(TAG,
                 "rtc config changed after SDK init: schedule full reset enabled=%d device_id_len=%u client_id_len=%u endpoint=%s",
                 validated_config.enabled ? 1 : 0,
                 (unsigned)strlen(validated_config.device_id),
                 (unsigned)strlen(validated_config.client_id),
                 validated_config.service_endpoint);
        if (!tirtc_session_schedule_deferred_full_reset()) {
            ESP_LOGW(TAG, "rtc full reset schedule failed after config update");
            return ESP_FAIL;
        }
    } else if (config_changed) {
        ESP_LOGI(TAG,
                 "rtc config updated before SDK start: enabled=%d device_id_len=%u client_id_len=%u endpoint=%s",
                 validated_config.enabled ? 1 : 0,
                 (unsigned)strlen(validated_config.device_id),
                 (unsigned)strlen(validated_config.client_id),
                 validated_config.service_endpoint);
    }

    return ESP_OK;
}

esp_err_t tirtc_session_set_media_bridge(const tirtc_session_media_ops_t *ops, void *ctx)
{
    if (ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_initialized || s_sdk_started || s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_media_ops = *ops;
    s_media_ctx = ctx;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return ESP_OK;
}

void tirtc_session_set_hooks(const tirtc_session_hooks_t *hooks, void *ctx)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (hooks != NULL) {
        s_hooks = *hooks;
        s_hooks_ctx = ctx;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
        s_hooks_ctx = NULL;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

void tirtc_session_set_control_ops(const tirtc_session_control_ops_t *ops, void *ctx)
{
    taskENTER_CRITICAL(&s_rtc_lock);
    if (ops != NULL) {
        s_control_ops = *ops;
        s_control_ctx = ctx;
    } else {
        memset(&s_control_ops, 0, sizeof(s_control_ops));
        s_control_ctx = NULL;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
}

static bool tirtc_session_copy_control_ops(tirtc_session_control_ops_t *ops, void **ctx)
{
    bool configured = false;

    if (ops == NULL || ctx == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    *ops = s_control_ops;
    *ctx = s_control_ctx;
    configured = ops->set_speaker_volume != NULL || ops->set_door_open != NULL;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return configured;
}

esp_err_t tirtc_session_apply_remote_volume_command(uint8_t percent)
{
    tirtc_session_control_ops_t ops = {0};
    void *ctx = NULL;

    if (!tirtc_session_copy_control_ops(&ops, &ctx) || ops.set_speaker_volume == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ops.set_speaker_volume(percent, ctx);
}

esp_err_t tirtc_session_apply_remote_door_command(bool open)
{
    tirtc_session_control_ops_t ops = {0};
    void *ctx = NULL;

    if (!tirtc_session_copy_control_ops(&ops, &ctx) || ops.set_door_open == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ops.set_door_open(open, ctx);
}

esp_err_t tirtc_session_apply_video_bitrate_params(tirtc_conn_t conn)
{
    tirtc_session_video_bitrate_params_t params = {0};
    bool enabled = false;
    bool connection_supports_tgmp = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    enabled = s_video_bitrate_params_enabled;
    params = s_video_bitrate_params;
    connection_supports_tgmp = conn != NULL &&
                               conn == s_active_conn &&
                               s_active_conn_supports_tgmp_bitrate;
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!enabled) {
        return ESP_OK;
    }
    if (!connection_supports_tgmp) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#if !TIRTC_SESSION_HAS_TGMP_BITRATE_CONTROL
    (void)conn;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!tirtc_session_is_connection_usable(conn)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        return ESP_ERR_TIMEOUT;
    }

    int sdk_ret = TiRtcConnSetVideoBitrateParams(conn,
                                                 params.stream_id,
                                                 params.min_bitrate_bps,
                                                 params.max_bitrate_bps,
                                                 params.start_bitrate_bps);
    tirtc_session_give_sdk_api_lock();
    if (sdk_ret == TIRTC_E_INVALID_PARAMETER) {
        /* The SDK exposes the TGMP-only API through the common connection
         * handle and reports INVALID_PARAMETER when the negotiated transport
         * is not TGTRP. The range has already passed the strict local
         * min < start < max validation, so remember this as a per-connection
         * capability result instead of treating an IPC/KCP session as a bad
         * application configuration. */
        taskENTER_CRITICAL(&s_rtc_lock);
        if (conn == s_active_conn) {
            s_active_conn_supports_tgmp_bitrate = false;
        }
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGI(TAG,
                 "TGMP bitrate control unavailable for active transport: stream=%u",
                 (unsigned)params.stream_id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sdk_ret != 0) {
        tirtc_session_set_last_error(sdk_ret);
        ESP_LOGW(TAG,
                 "TGMP video bitrate params rejected: stream=%u min=%uk max=%uk start=%uk ret=%d %s",
                 (unsigned)params.stream_id,
                 (unsigned)(params.min_bitrate_bps / 1000U),
                 (unsigned)(params.max_bitrate_bps / 1000U),
                 (unsigned)(params.start_bitrate_bps / 1000U),
                 sdk_ret,
                 TiRtcGetErrorStr(sdk_ret));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "TGMP video bitrate control ready: stream=%u min=%uk max=%uk start=%uk",
             (unsigned)params.stream_id,
             (unsigned)(params.min_bitrate_bps / 1000U),
             (unsigned)(params.max_bitrate_bps / 1000U),
             (unsigned)(params.start_bitrate_bps / 1000U));
    return ESP_OK;
#endif
}

esp_err_t tirtc_session_set_video_bitrate_params(
    const tirtc_session_video_bitrate_params_t *params)
{
    tirtc_conn_t active_conn = NULL;

    if (params != NULL &&
        (params->min_bitrate_bps == 0U ||
         params->min_bitrate_bps >= params->start_bitrate_bps ||
         params->start_bitrate_bps >= params->max_bitrate_bps)) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (params != NULL) {
        s_video_bitrate_params = *params;
        s_video_bitrate_params_enabled = true;
    } else {
        memset(&s_video_bitrate_params, 0, sizeof(s_video_bitrate_params));
        s_video_bitrate_params_enabled = false;
    }
    if (s_active_conn != NULL && s_sdk_started && !s_start_in_progress &&
        !s_stop_in_progress && s_closing_conn == NULL) {
        active_conn = s_active_conn;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (params == NULL || active_conn == NULL) {
        return ESP_OK;
    }
    return tirtc_session_apply_video_bitrate_params(active_conn);
}

esp_err_t tirtc_session_register_observer(const tirtc_session_observer_t *observer, void *ctx)
{
    if (observer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_observer_lock);
    for (size_t index = 0; index < TIRTC_SESSION_OBSERVER_MAX; ++index) {
        if (s_observers[index].used &&
            s_observers[index].observer.on_command == observer->on_command &&
            s_observers[index].observer.on_message == observer->on_message &&
            s_observers[index].observer.on_call_active == observer->on_call_active &&
            s_observers[index].observer.on_connection_accepted ==
                observer->on_connection_accepted &&
            s_observers[index].observer.on_connection_error == observer->on_connection_error &&
            s_observers[index].observer.on_disconnected == observer->on_disconnected &&
            s_observers[index].observer.on_start_error == observer->on_start_error &&
            s_observers[index].observer.on_video_bitrate_required ==
                observer->on_video_bitrate_required &&
            s_observers[index].ctx == ctx) {
            taskEXIT_CRITICAL(&s_observer_lock);
            return ESP_OK;
        }
    }
    for (size_t index = 0; index < TIRTC_SESSION_OBSERVER_MAX; ++index) {
        if (!s_observers[index].used) {
            s_observers[index].observer = *observer;
            s_observers[index].ctx = ctx;
            s_observers[index].used = true;
            taskEXIT_CRITICAL(&s_observer_lock);
            return ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_observer_lock);

    return ESP_ERR_NO_MEM;
}

esp_err_t tirtc_session_init(const tirtc_session_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(tirtc_session_has_media_bridge(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc media bridge not configured");
    ESP_RETURN_ON_ERROR(tirtc_session_configure(config), TAG, "configure rtc failed");
    tirtc_session_init_stats();
    ESP_RETURN_ON_ERROR(tirtc_session_create_runtime_resources(), TAG, "rtc runtime init failed");
    tirtc_session_configure_runtime_callbacks();

    s_initialized = true;
    return ESP_OK;
}

esp_err_t tirtc_session_prepare_sdk(void)
{
    bool network_connected = false;
    bool prepare_in_progress = false;
    bool request_start = false;
    uint64_t next_start_allowed_us = 0U;
    uint64_t now_us = 0U;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");

    if (!s_config.enabled) {
        tirtc_session_note_event("rtc disabled");
        return ESP_OK;
    }
    if (!system_time_has_valid_time()) {
        tirtc_session_note_event("waiting time");
        ESP_LOGI(TAG, "rtc sdk init waits for valid system time");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(s_config.service_endpoint) == 0) {
        tirtc_session_note_event("waiting endpoint");
        ESP_LOGI(TAG, "rtc sdk init waits for endpoint");
        return ESP_ERR_INVALID_STATE;
    }

    now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_rtc_lock);
    network_connected = s_network_connected;
    if (s_sdk_initialized) {
        next_start_allowed_us = s_next_start_allowed_us;
        if (network_connected && !s_sdk_started && !s_start_in_progress && !s_stop_in_progress &&
            (next_start_allowed_us == 0U || now_us >= next_start_allowed_us)) {
            request_start = true;
        }
        bool sdk_ready = network_connected && !s_stop_in_progress &&
                         (s_sdk_started || s_start_in_progress || request_start);
        taskEXIT_CRITICAL(&s_rtc_lock);
        if (network_connected && !request_start && !sdk_ready &&
            next_start_allowed_us != 0U && now_us < next_start_allowed_us) {
            tirtc_session_note_event("start backoff");
            ESP_LOGD(TAG,
                     "rtc listen start delayed after previous failure: retry_in_ms=%llu",
                     (unsigned long long)((next_start_allowed_us - now_us + 999ULL) / 1000ULL));
        }
        if (request_start && !tirtc_session_enqueue_start_if_ready()) {
            tirtc_session_note_event("start evt drop");
            ESP_LOGW(TAG, "rtc listen start event dropped");
            return ESP_ERR_TIMEOUT;
        }
        return sdk_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    prepare_in_progress = s_sdk_prepare_in_progress;
    if (!prepare_in_progress &&
        (s_sdk_started || s_start_in_progress || s_stop_in_progress ||
         s_active_conn != NULL || s_closing_conn != NULL)) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!prepare_in_progress) {
        s_sdk_prepare_in_progress = true;
        tirtc_session_sync_stats_locked();
    }
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (!network_connected) {
        if (!prepare_in_progress) {
            taskENTER_CRITICAL(&s_rtc_lock);
            s_sdk_prepare_in_progress = false;
            tirtc_session_sync_stats_locked();
            taskEXIT_CRITICAL(&s_rtc_lock);
        }
        tirtc_session_note_event("waiting network");
        return ESP_ERR_INVALID_STATE;
    }

    if (prepare_in_progress) {
        return ESP_OK;
    }

    ret = tirtc_session_ensure_platform_ready();
    if (ret == ESP_OK) {
        if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
            tirtc_session_note_event("sdk lock failed");
            ret = ESP_FAIL;
        } else {
            ret = tirtc_session_prepare_sdk_with_lock();
            tirtc_session_give_sdk_api_lock();
        }
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_sdk_prepare_in_progress = false;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (ret != ESP_OK) {
        return ret;
    }

    if (!tirtc_session_enqueue_start_if_ready()) {
        tirtc_session_note_event("start evt drop");
        ESP_LOGW(TAG, "rtc listen start event dropped after init");
        return ESP_ERR_TIMEOUT;
    }

    tirtc_session_note_event("sdk initialized");
    return ESP_OK;
}

esp_err_t tirtc_session_start_if_ready(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");
    return tirtc_session_prepare_sdk();
}

int tirtc_session_whip_connect(const char *service_desc,
                               const char *token,
                               TIRTCCONNECTCALLBACK cb,
                               void *user_data)
{
    tirtc_session_whip_request_t *request = NULL;
    size_t service_desc_len = 0;
    size_t token_len = 0;
    int64_t sdk_call_start_us = 0;
    int ret = TIRTC_E_BUSY;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    if (service_desc == NULL || service_desc[0] == '\0' ||
        token == NULL || token[0] == '\0' || cb == NULL) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    service_desc_len = strlen(service_desc);
    token_len = strlen(token);
    if (!s_initialized) {
        return TIRTC_E_BUSY;
    }
    request = tirtc_session_alloc_whip_request(service_desc, token, cb, user_data);
    if (request == NULL) {
        return TIRTC_E_LACK_OF_RESOURCE;
    }

    request->attempt_id =
        tirtc_session_begin_whip_connect_attempt(&active_conn, &closing_conn);
    if (request->attempt_id == 0U) {
        ESP_LOGW(TAG,
                 "WHIP submit rejected before SDK call: active=%p closing=%p",
                 active_conn,
                 closing_conn);
        tirtc_session_free_whip_request(request);
        return TIRTC_E_BUSY;
    }

    ESP_LOGI(TAG,
             "WHIP submit begin: service_desc_len=%u token_len=%u",
             (unsigned)service_desc_len,
             (unsigned)token_len);
    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        tirtc_session_finish_whip_connect_attempt(request->attempt_id);
        tirtc_session_free_whip_request(request);
        tirtc_session_set_next_connection_auto_media(TIRTC_SESSION_DEFAULT_AUTO_MEDIA);
        return TIRTC_E_BUSY;
    }
    sdk_call_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "WHIP SDK call entered");
    ret = TiRtcWhipConnect(request->service_desc,
                           request->token,
                           tirtc_session_on_whip_connect_result,
                           request);
    tirtc_session_give_sdk_api_lock();
    ESP_LOGI(TAG,
             "WHIP submit returned: ret=%d elapsed=%ums",
             ret,
             (unsigned)((esp_timer_get_time() - sdk_call_start_us) / 1000LL));

    if (ret != 0) {
        tirtc_session_finish_whip_connect_attempt(request->attempt_id);
        ESP_LOGE(TAG,
                 "WHIP submit rejected: ret=%d %s service_desc_len=%u token_len=%u",
                 ret,
                 TiRtcGetErrorStr(ret),
                 (unsigned)service_desc_len,
                 (unsigned)token_len);
        tirtc_session_free_whip_request(request);
        tirtc_session_set_next_connection_auto_media(TIRTC_SESSION_DEFAULT_AUTO_MEDIA);
    }
    return ret;
}

int tirtc_session_whip_connect_external(const char *service_desc,
                                        const char *token,
                                        TIRTCCONNECTCALLBACK cb,
                                        void *user_data)
{
    tirtc_session_whip_request_t *request = NULL;
    size_t service_desc_len = 0;
    size_t token_len = 0;
    int64_t sdk_call_start_us = 0;
    int ret = TIRTC_E_BUSY;
    tirtc_conn_t active_conn = NULL;
    tirtc_conn_t closing_conn = NULL;

    if (service_desc == NULL || service_desc[0] == '\0' ||
        token == NULL || token[0] == '\0' || cb == NULL) {
        return TIRTC_E_INVALID_PARAMETER;
    }
    service_desc_len = strlen(service_desc);
    token_len = strlen(token);
    if (!s_initialized) {
        return TIRTC_E_BUSY;
    }

    request = tirtc_session_alloc_whip_request(service_desc, token, cb, user_data);
    if (request == NULL) {
        return TIRTC_E_LACK_OF_RESOURCE;
    }

    request->attempt_id =
        tirtc_session_begin_whip_connect_attempt(&active_conn, &closing_conn);
    if (request->attempt_id == 0U) {
        ESP_LOGW(TAG,
                 "WHIP external submit rejected before SDK call: active=%p closing=%p",
                 active_conn,
                 closing_conn);
        tirtc_session_free_whip_request(request);
        return TIRTC_E_BUSY;
    }

    ESP_LOGI(TAG,
             "WHIP external submit begin: service_desc_len=%u token_len=%u",
             (unsigned)service_desc_len,
             (unsigned)token_len);

    if (!tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        tirtc_session_finish_whip_connect_attempt(request->attempt_id);
        tirtc_session_free_whip_request(request);
        return TIRTC_E_BUSY;
    }
    sdk_call_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "WHIP external SDK call entered");
    ret = TiRtcWhipConnect(request->service_desc,
                           request->token,
                           tirtc_session_on_external_whip_connect_result,
                           request);
    tirtc_session_give_sdk_api_lock();
    ESP_LOGI(TAG,
             "WHIP external submit returned: ret=%d elapsed=%ums",
             ret,
             (unsigned)((esp_timer_get_time() - sdk_call_start_us) / 1000LL));

    if (ret != 0) {
        tirtc_session_finish_whip_connect_attempt(request->attempt_id);
        ESP_LOGE(TAG,
                 "WHIP external submit rejected: ret=%d %s service_desc_len=%u token_len=%u",
                 ret,
                 TiRtcGetErrorStr(ret),
                 (unsigned)service_desc_len,
                 (unsigned)token_len);
        tirtc_session_free_whip_request(request);
    }
    return ret;
}

int tirtc_session_service_request(const char *path,
                                  const char *json_body,
                                  const char *token,
                                  TIRTCSERVICEREQUESTCALLBACK cb,
                                  void *user_data)
{
    int ret = TIRTC_E_BUSY;

    if (path == NULL || path[0] == '\0') {
        return TIRTC_E_INVALID_PARAMETER;
    }
    if (!s_initialized) {
        return TIRTC_E_NOT_INITIALIZED;
    }

    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        ret = TiRtcServiceRequest(path, json_body, token, cb, user_data);
        tirtc_session_give_sdk_api_lock();
    }

    return ret;
}

esp_err_t tirtc_session_connect_peer(const char *remote_device_id,
                                     const char *remote_device_secret_key)
{
    bool sdk_started = false;
    bool start_in_progress = false;
    bool connect_in_progress = false;
    tirtc_session_event_t event = {0};

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "rtc event queue not ready");
    ESP_RETURN_ON_FALSE(remote_device_id != NULL && remote_device_id[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "remote device id is empty");
    ESP_RETURN_ON_FALSE(strlen(remote_device_id) < sizeof(s_config.remote_device_id),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "remote device id is too long");
    if (remote_device_secret_key != NULL && remote_device_secret_key[0] != '\0') {
        ESP_RETURN_ON_FALSE(strlen(remote_device_secret_key) < sizeof(s_config.remote_device_secret_key),
                            ESP_ERR_INVALID_SIZE,
                            TAG,
                            "remote device secret is too long");
    }
    connect_in_progress = tirtc_connect_is_connecting();
    if (connect_in_progress) {
        ESP_LOGW(TAG, "rtc peer connect rejected: active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(tirtc_session_prepare_sdk(), TAG, "prepare rtc sdk failed");

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_network_connected || !s_sdk_initialized || s_sdk_prepare_in_progress ||
        s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        bool network_connected = s_network_connected;
        bool sdk_initialized = s_sdk_initialized;
        bool sdk_prepare_in_progress = s_sdk_prepare_in_progress;
        bool stop_in_progress = s_stop_in_progress;
        tirtc_conn_t active_conn = s_active_conn;
        tirtc_conn_t closing_conn = s_closing_conn;
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc peer connect rejected: net=%d init=%d prep=%d stop=%d active=%p closing=%p connecting=%d",
                 network_connected ? 1 : 0,
                 sdk_initialized ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0,
                 stop_in_progress ? 1 : 0,
                 active_conn,
                 closing_conn,
                 connect_in_progress ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sdk_started && !s_start_in_progress) {
        bool sdk_initialized = s_sdk_initialized;
        bool sdk_prepare_in_progress = s_sdk_prepare_in_progress;
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc peer connect rejected: sdk not started init=%d prep=%d",
                 sdk_initialized ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config.device_id[0] != '\0' && strcmp(remote_device_id, s_config.device_id) == 0) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("self call blocked");
        ESP_LOGW(TAG, "rtc peer connect rejected: remote_id is local device_id");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_config.remote_device_id, remote_device_id, sizeof(s_config.remote_device_id));
    if (remote_device_secret_key != NULL && remote_device_secret_key[0] != '\0') {
        strlcpy(s_config.remote_device_secret_key,
                remote_device_secret_key,
                sizeof(s_config.remote_device_secret_key));
    }
    if (s_config.remote_device_secret_key[0] == '\0') {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_ARG;
    }

    s_session_mode = TIRTC_SESSION_MODE_CONNECT;
    s_next_connection_auto_media = TIRTC_SESSION_DEFAULT_AUTO_MEDIA;
    s_next_connection_defer_media = false;
    sdk_started = s_sdk_started;
    start_in_progress = s_start_in_progress;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    event.type = sdk_started ? TIRTC_SESSION_EVENT_CONNECT_PEER : TIRTC_SESSION_EVENT_START_IF_READY;
    if (!tirtc_session_enqueue_event(&event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_note_event("peer connect evt drop");
        return ESP_FAIL;
    }

    tirtc_session_note_event(sdk_started ? "peer connect req" : "peer wait start");
    ESP_LOGI(TAG,
             "rtc peer connect requested: remote_id_len=%u sdk_started=%d start_in_progress=%d",
             (unsigned)strlen(remote_device_id),
             sdk_started,
             start_in_progress);
    return ESP_OK;
}

esp_err_t tirtc_session_connect_peer_with_token(const char *remote_device_id,
                                                const char *connect_token)
{
    bool connect_in_progress = false;
    tirtc_session_config_t config = {0};

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "rtc event queue not ready");
    ESP_RETURN_ON_FALSE(remote_device_id != NULL && remote_device_id[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "remote device id is empty");
    ESP_RETURN_ON_FALSE(connect_token != NULL && connect_token[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "connect token is empty");
    ESP_RETURN_ON_FALSE(strlen(remote_device_id) < sizeof(s_config.remote_device_id),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "remote device id is too long");
    ESP_RETURN_ON_FALSE(strlen(connect_token) < TIRTC_CONNECT_TOKEN_MAX_LEN,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "connect token is too long");

    connect_in_progress = tirtc_connect_is_connecting();
    if (connect_in_progress) {
        ESP_LOGW(TAG, "rtc token connect rejected: active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(tirtc_session_prepare_sdk(), TAG, "prepare rtc sdk failed");

    taskENTER_CRITICAL(&s_rtc_lock);
    if (!s_network_connected || !s_sdk_initialized || !s_sdk_started ||
        s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        bool network_connected = s_network_connected;
        bool sdk_initialized = s_sdk_initialized;
        bool sdk_started = s_sdk_started;
        bool sdk_prepare_in_progress = s_sdk_prepare_in_progress;
        bool start_in_progress = s_start_in_progress;
        bool stop_in_progress = s_stop_in_progress;
        tirtc_conn_t active_conn = s_active_conn;
        tirtc_conn_t closing_conn = s_closing_conn;
        taskEXIT_CRITICAL(&s_rtc_lock);
        ESP_LOGW(TAG,
                 "rtc token connect rejected: net=%d init=%d started=%d prep=%d start=%d stop=%d active=%p closing=%p",
                 network_connected ? 1 : 0,
                 sdk_initialized ? 1 : 0,
                 sdk_started ? 1 : 0,
                 sdk_prepare_in_progress ? 1 : 0,
                 start_in_progress ? 1 : 0,
                 stop_in_progress ? 1 : 0,
                 active_conn,
                 closing_conn);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config.device_id[0] != '\0' && strcmp(remote_device_id, s_config.device_id) == 0) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        tirtc_session_note_event("self call blocked");
        ESP_LOGW(TAG, "rtc token connect rejected: remote_id is local device_id");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_config.remote_device_id, remote_device_id, sizeof(s_config.remote_device_id));
    s_session_mode = TIRTC_SESSION_MODE_CONNECT;
    s_next_connection_auto_media = true;
    config = s_config;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    esp_err_t ret = tirtc_connect_start_with_token(config.remote_device_id,
                                                   connect_token,
                                                   tirtc_session_on_peer_connect_result,
                                                   NULL);
    if (ret != ESP_OK) {
        tirtc_session_return_to_listen_mode();
        tirtc_session_set_last_error(ret);
        tirtc_session_note_event("token connect fail");
        return ret;
    }

    tirtc_session_note_event("token connect");
    ESP_LOGI(TAG,
             "rtc token peer connect task started: remote_id_len=%u token_len=%u",
             (unsigned)strlen(config.remote_device_id),
             (unsigned)strlen(connect_token));
    return ESP_OK;
}

esp_err_t tirtc_session_restart(void)
{
    tirtc_session_runtime_snapshot_t snapshot = {0};
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");

    tirtc_session_get_runtime_snapshot(&snapshot);

    if (!snapshot.enabled) {
        return ESP_OK;
    }

    if (snapshot.active_conn != NULL || snapshot.closing_conn != NULL) {
        tirtc_session_note_event("manual restart");
        ret = tirtc_session_disconnect();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
    }

    return tirtc_session_prepare_sdk();
}

esp_err_t tirtc_session_stop(void)
{
    tirtc_conn_t conn = NULL;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");
    if (!tirtc_session_try_get_active_conn(&conn)) {
        tirtc_session_note_event("disconnect idle");
        return ESP_OK;
    }
    return tirtc_session_disconnect();
}

esp_err_t tirtc_session_disconnect(void)
{
    tirtc_conn_t conn = NULL;
    bool was_sdk_started = false;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "rtc not initialized");

    if (!tirtc_session_try_get_active_conn(&conn)) {
        if (tirtc_connect_abort_attempt()) {
            /*
             * Room cancellation can arrive before TiRtcConnect produces a
             * handle. Invalidate that generation now so the next call is not
             * rejected as busy; a late successful callback releases itself.
             */
            tirtc_session_return_to_listen_mode();
            tirtc_session_note_event("connect aborted");
            ESP_LOGI(TAG, "rtc active connect aborted before handle creation");
        } else {
            tirtc_session_note_event("disconnect idle");
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(tirtc_session_begin_connection_shutdown(conn, 0, &was_sdk_started, NULL),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "rtc connection shutdown not tracked");

    if (!tirtc_session_enqueue_disconnect_request(conn, true, was_sdk_started)) {
        tirtc_session_note_event("disconnect drop");
        ESP_LOGW(TAG, "rtc disconnect request dropped: hconn=%p", conn);
        tirtc_session_complete_connection_shutdown(conn, was_sdk_started);
        return ESP_ERR_TIMEOUT;
    }

    tirtc_session_note_event("disconnect req");
    return ESP_OK;
}

int tirtc_session_disconnect_connection(tirtc_conn_t conn)
{
    bool was_sdk_started = false;
    bool newly_detached = false;

    if (conn == NULL) {
        return TIRTC_E_INVALID_PARAMETER;
    }

    if (tirtc_session_begin_connection_shutdown(conn,
                                                0,
                                                &was_sdk_started,
                                                &newly_detached)) {
        if (!newly_detached) {
            APP_LOG_DETAIL(TAG,
                           "rtc disconnect already pending: hconn=%p",
                           conn);
            return 0;
        }
        if (!tirtc_session_enqueue_disconnect_request(conn, true, was_sdk_started)) {
            tirtc_session_complete_connection_shutdown(conn, was_sdk_started);
            return TIRTC_E_BUSY;
        }
        tirtc_session_note_event("disconnect req");
        return 0;
    }

    if (!tirtc_session_enqueue_disconnect_request(conn, false, false)) {
        return TIRTC_E_BUSY;
    }
    return 0;
}

bool tirtc_session_get_last_peer_state(tirtc_session_peer_state_t *state)
{
    if (state == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    *state = s_last_peer_state;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return state->valid;
}

void tirtc_session_on_network_state_changed(const tirtc_session_network_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    s_network_connected = state->connected;
    taskEXIT_CRITICAL(&s_rtc_lock);

    tirtc_session_event_t event = {
        .type = TIRTC_SESSION_EVENT_NETWORK_CHANGED,
        .payload.network = {
            .state = *state,
        },
    };
    if (!tirtc_session_enqueue_event(&event, TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS)) {
        tirtc_session_note_event("network evt drop");
        ESP_LOGW(TAG, "rtc event queue full: network change dropped connected=%d", state->connected);
    }
}

esp_err_t tirtc_session_set_local_video_send_enabled(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;
    bool flush_video_queue = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    flush_video_queue = !enabled && s_local_video_send_enabled;
    s_local_video_send_enabled = enabled;
    if (!enabled && s_local_video_publish_forced) {
        s_local_video_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_video_publish_forced = false;
        s_test_video_publish_forced = false;
        cleared_forced_publish = true;
    } else if (enabled) {
        forced_publish = tirtc_session_maybe_force_local_video_publish_locked();
    }
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local video fallback publish forced: send enabled after peer request stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_VIDEO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local video fallback publish cleared");
    }

    if (flush_video_queue) {
        tirtc_session_flush_local_video_tx_queue();
    }
    tirtc_session_note_event(enabled ? "video send on" : "video send off");
    tirtc_session_apply_local_media_policy();
    return ESP_OK;
}

esp_err_t tirtc_session_set_local_audio_send_enabled(bool enabled)
{
    bool forced_publish = false;
    bool cleared_forced_publish = false;
    bool flush_audio_queue = false;

    taskENTER_CRITICAL(&s_rtc_lock);
    flush_audio_queue = !enabled && s_local_audio_send_enabled;
    s_local_audio_send_enabled = enabled;
    if (!enabled && s_local_audio_publish_forced) {
        s_local_audio_stream_id = TIRTC_SESSION_INVALID_STREAM_ID;
        s_local_audio_publish_forced = false;
        s_test_audio_publish_forced = false;
        cleared_forced_publish = true;
    } else if (enabled) {
        forced_publish = tirtc_session_maybe_force_local_audio_publish_locked();
    }
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);

    if (forced_publish) {
        ESP_LOGD(TAG,
                 "local audio fallback publish forced: send enabled after peer request stream=%u",
                 (unsigned)TIRTC_SESSION_LOCAL_AUDIO_STREAM_ID);
    } else if (cleared_forced_publish) {
        ESP_LOGD(TAG, "local audio fallback publish cleared");
    }

    if (flush_audio_queue) {
        tirtc_session_flush_local_audio_tx_queue();
    }
    tirtc_session_note_event(enabled ? "audio send on" : "audio send off");
    tirtc_session_apply_local_media_policy();
    return ESP_OK;
}

esp_err_t tirtc_session_set_session_mode(tirtc_session_mode_t session_mode)
{
    if (session_mode != TIRTC_SESSION_MODE_LISTEN &&
        session_mode != TIRTC_SESSION_MODE_CONNECT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tirtc_connect_is_connecting()) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    if (s_sdk_started || s_sdk_prepare_in_progress || s_start_in_progress || s_stop_in_progress ||
        s_active_conn != NULL || s_closing_conn != NULL) {
        taskEXIT_CRITICAL(&s_rtc_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_session_mode = session_mode;
    tirtc_session_sync_stats_locked();
    taskEXIT_CRITICAL(&s_rtc_lock);
    tirtc_session_note_event(session_mode == TIRTC_SESSION_MODE_LISTEN ? "mode listen" : "mode connect");
    return ESP_OK;
}

tirtc_session_mode_t tirtc_session_get_session_mode(void)
{
    tirtc_session_mode_t session_mode = TIRTC_SESSION_MODE_LISTEN;

    taskENTER_CRITICAL(&s_rtc_lock);
    session_mode = s_session_mode;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return session_mode;
}

tirtc_session_state_t tirtc_session_get_state(void)
{
    tirtc_session_state_t state = TIRTC_SESSION_STATE_STOPPED;

    taskENTER_CRITICAL(&s_rtc_lock);
    state = s_state;
    taskEXIT_CRITICAL(&s_rtc_lock);
    return state;
}

void tirtc_session_get_config(tirtc_session_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = s_config;
}

bool tirtc_session_get_started_auth_debug(char *device_id,
                                          size_t device_id_size,
                                          char *credential_hash,
                                          size_t credential_hash_size,
                                          uint32_t *secret_len)
{
    bool available = false;

    if ((device_id == NULL || device_id_size == 0) &&
        (credential_hash == NULL || credential_hash_size == 0) &&
        secret_len == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    available = s_started_device_id[0] != '\0' && s_started_credential_hash[0] != '\0';
    if (device_id != NULL && device_id_size > 0) {
        strlcpy(device_id, s_started_device_id, device_id_size);
    }
    if (credential_hash != NULL && credential_hash_size > 0) {
        strlcpy(credential_hash, s_started_credential_hash, credential_hash_size);
    }
    if (secret_len != NULL) {
        *secret_len = s_started_secret_len;
    }
    taskEXIT_CRITICAL(&s_rtc_lock);
    return available;
}

void tirtc_session_get_stats(tirtc_session_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_rtc_lock);
    tirtc_session_sync_stats_locked();
    tirtc_session_update_pool_stats_unlocked(&s_stats);
    *stats = s_stats;
    taskEXIT_CRITICAL(&s_rtc_lock);
    tirtc_session_update_queue_stats(stats);
}
