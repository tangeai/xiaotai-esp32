#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_types.h"

typedef enum {
    AUDIO_ECHO_CANCEL_REFERENCE_NONE = 0,
    AUDIO_ECHO_CANCEL_REFERENCE_SOFTWARE,
    AUDIO_ECHO_CANCEL_REFERENCE_CODEC,
} audio_echo_cancel_reference_t;

typedef struct {
    bool active;
    bool reference_active;
    bool near_end_detected;
    bool warming_up;
    audio_echo_cancel_reference_t reference;
    uint32_t ref_peak;
    uint32_t mic_peak;
    uint32_t out_peak;
    uint8_t suppress_percent;
    uint16_t delay_samples;
    uint32_t process_us;
} audio_echo_cancel_metrics_t;

typedef struct {
    bool prepared;
    bool active;
    bool reference_active;
    audio_echo_cancel_reference_t reference;
    uint32_t process_frames;
    uint64_t process_us_total;
    uint32_t process_us_max;
} audio_echo_cancel_status_t;

esp_err_t audio_echo_cancel_prepare(void);
esp_err_t audio_echo_cancel_set_active(bool active);
void audio_echo_cancel_set_suppression(audio_echo_suppression_t suppression);
void audio_echo_cancel_get_status(audio_echo_cancel_status_t *status);
void audio_echo_cancel_feed_playback(const int16_t *samples,
                                     size_t sample_count,
                                     uint8_t channels);
void audio_echo_cancel_process_capture(int16_t *samples,
                                       size_t sample_count,
                                       audio_echo_cancel_metrics_t *metrics);
void audio_echo_cancel_process_capture_with_reference(
    int16_t *samples,
    const int16_t *codec_reference,
    size_t sample_count,
    audio_echo_cancel_metrics_t *metrics);
void audio_echo_cancel_reset(void);
void audio_echo_cancel_deinit(void);
