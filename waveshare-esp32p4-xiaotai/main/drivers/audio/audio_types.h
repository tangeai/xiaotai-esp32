#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} audio_format_t;

typedef void (*audio_capture_frame_cb_t)(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         void *ctx);

typedef enum {
    AUDIO_ECHO_SUPPRESSION_BALANCED = 0,
    AUDIO_ECHO_SUPPRESSION_STRONG,
} audio_echo_suppression_t;

typedef struct {
    uint8_t send_volume_percent;
    uint8_t codec_gain_percent;
    uint8_t upload_gain_percent;
    uint16_t auto_gain_max_percent;
    bool far_end_gain_guard_enabled;
    uint8_t far_end_upload_gain_percent;
    uint16_t far_end_auto_gain_max_percent;
    audio_echo_suppression_t echo_suppression;
    bool high_pass_filter_enabled;
} audio_capture_processing_config_t;

typedef struct {
    bool ready;
    bool capture_enabled;
    bool speaker_enabled;
    uint32_t capture_frames;
    uint32_t input_level;
    uint32_t output_level;
    uint8_t speaker_volume_percent;
    uint8_t capture_gain_percent;
    uint8_t capture_codec_gain_percent;
    uint8_t capture_upload_gain_percent;
    uint16_t capture_auto_gain_max_percent;
    uint16_t capture_effective_auto_gain_max_percent;
    bool far_end_gain_guard_enabled;
    uint8_t far_end_upload_gain_percent;
    uint16_t far_end_auto_gain_max_percent;
    audio_echo_suppression_t echo_suppression;
    bool capture_high_pass_filter_enabled;
    bool aec_active;
    bool aec_reference_active;
    bool aec_near_end_detected;
    uint32_t aec_ref_peak;
    uint32_t aec_mic_peak;
    uint32_t aec_out_peak;
    uint8_t aec_suppress_percent;
    uint32_t aec_process_frames;
    uint64_t aec_process_us_total;
    uint32_t aec_process_us_max;
} audio_stats_t;

typedef struct {
    uint32_t prepare_ms;
    uint32_t write_ms;
    uint32_t data_bytes;
} audio_playback_timing_t;
