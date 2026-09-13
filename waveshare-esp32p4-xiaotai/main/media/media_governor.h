#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The OV5647 CSI driver exposes 800x640 as its smallest native landscape
 * sensor mode. RTC output sizes are scaler targets, so the P4 media layer must
 * crop/scale this native frame before encoding a 480x320 call.
 */
#define MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH  800U
#define MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT 640U

typedef enum {
    MEDIA_GOVERNOR_PROFILE_IDLE = 0,
    MEDIA_GOVERNOR_PROFILE_QR_SCAN,
    MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW,
    MEDIA_GOVERNOR_PROFILE_RTC_AUDIO,
    MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE,
    MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW,
    MEDIA_GOVERNOR_PROFILE_AI_CHAT,
} media_governor_profile_t;

typedef enum {
    MEDIA_GOVERNOR_WEAK_NETWORK_OFF = 0,
    MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY,
    MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY,
} media_governor_weak_network_mode_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint32_t bitrate_bps;
    media_governor_weak_network_mode_t weak_network_mode;
    uint8_t weak_network_level;
    uint8_t h264_min_qp;
    uint8_t h264_max_qp;
} media_governor_video_config_t;

typedef struct {
    uint16_t capture_width;
    uint16_t capture_height;
    uint8_t capture_fps;
    uint8_t rtc_video_fps;
    uint16_t rtc_width;
    uint16_t rtc_height;
    uint32_t h264_bitrate_bps;
    uint8_t h264_min_qp;
    uint8_t h264_max_qp;
    size_t h264_output_buffer_bytes;
    size_t h264_max_delta_payload_bytes;
    size_t dma_free_min_bytes;
    size_t dma_largest_min_bytes;
} media_governor_camera_policy_t;

typedef struct {
    bool defer_audio_for_local_video;
    bool prepare_playback_while_video_first;
} media_governor_rtc_policy_t;

typedef struct {
    bool active;
    bool backpressure_active;
    uint32_t backpressure_events;
    uint8_t target_fps;
    uint32_t camera_fps_x10;
    uint32_t tx_fps_x10;
    uint32_t tx_failures;
    uint32_t tx_queue_depth;
    size_t send_buffer_used;
    size_t send_buffer_limit;
    int8_t wifi_rssi;
} media_governor_network_sample_t;

typedef struct {
    uint32_t min_bitrate_bps;
    uint32_t max_bitrate_bps;
    uint32_t start_bitrate_bps;
} media_governor_transport_bitrate_range_t;

esp_err_t media_governor_init(void);
void media_governor_set_profile(media_governor_profile_t profile);
media_governor_profile_t media_governor_get_profile(void);
esp_err_t media_governor_set_rtc_video_config(const media_governor_video_config_t *config);
void media_governor_build_device_call_video_config(media_governor_video_config_t *config);
void media_governor_build_wechat_video_config(media_governor_video_config_t *config);
void media_governor_build_camera_policy(const media_governor_video_config_t *config,
                                        media_governor_camera_policy_t *policy);
esp_err_t media_governor_apply_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level);
/* Only video profiles with a non-OFF adaptation mode participate. */
bool media_governor_auto_adaptation_enabled(void);
esp_err_t media_governor_apply_auto_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level);
bool media_governor_update_auto_adaptation(const media_governor_network_sample_t *sample);
void media_governor_get_transport_bitrate_range(
    media_governor_transport_bitrate_range_t *range);
esp_err_t media_governor_apply_transport_bitrate_target(uint32_t target_bitrate_bps,
                                                        bool *changed);
/* Advances only the slow recovery side; congestion targets are applied immediately. */
esp_err_t media_governor_step_transport_adaptation(bool *changed);
esp_err_t media_governor_reset_transport_adaptation(bool restore_base,
                                                    bool *changed);
bool media_governor_transport_adaptation_active(void);
void media_governor_get_rtc_video_config(media_governor_video_config_t *config);
void media_governor_get_camera_policy(media_governor_camera_policy_t *policy);
void media_governor_get_rtc_av_camera_policy(media_governor_camera_policy_t *policy);
void media_governor_get_rtc_policy(media_governor_rtc_policy_t *policy);
void media_governor_note_network_backpressure(void);
uint32_t media_governor_get_network_backpressure_count(void);
bool media_governor_is_network_backpressured(void);
const char *media_governor_profile_name(media_governor_profile_t profile);
