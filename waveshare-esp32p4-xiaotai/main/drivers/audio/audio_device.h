#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_types.h"

esp_err_t audio_device_prepare(void);
void audio_device_release(void);
void audio_device_get_stats(audio_stats_t *stats);
esp_err_t audio_device_prepare_echo_cancel(void);
esp_err_t audio_device_set_echo_cancel_active(bool active);

const audio_format_t *microphone_get_format(void);
esp_err_t microphone_prepare_capture_path(void);
void microphone_set_frame_cb(audio_capture_frame_cb_t cb, void *ctx);
esp_err_t microphone_register_observer(audio_capture_frame_cb_t cb, void *ctx);
void microphone_unregister_observer(audio_capture_frame_cb_t cb, void *ctx);
esp_err_t microphone_set_observer_enabled(audio_capture_frame_cb_t cb, void *ctx, bool enabled);
esp_err_t microphone_set_enabled(bool enabled);
esp_err_t microphone_set_gain_percent(uint8_t percent);
esp_err_t microphone_set_processing_config(const audio_capture_processing_config_t *config);

const audio_format_t *speaker_get_playback_format(void);
esp_err_t speaker_set_volume_percent(uint8_t percent);
esp_err_t speaker_prepare_playback_path(void);
esp_err_t speaker_play_pcm_frame(const uint8_t *data, size_t data_len, const audio_format_t *format);
esp_err_t speaker_render_pcm(const uint8_t *data,
                             size_t data_len,
                             const audio_format_t *format,
                             int16_t **output_data,
                             size_t *output_bytes,
                             uint32_t *output_level);
esp_err_t speaker_write_rendered_pcm(int16_t *data, size_t data_len, uint32_t output_level);
void speaker_get_last_playback_timing(audio_playback_timing_t *timing);
void speaker_stop_playback(void);
esp_err_t speaker_play_test_tone(uint32_t tone_hz, uint32_t duration_ms);
