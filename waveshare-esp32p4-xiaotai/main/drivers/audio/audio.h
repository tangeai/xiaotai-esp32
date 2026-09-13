#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_types.h"

esp_err_t audio_prepare(void);
esp_err_t audio_prepare_input_path(void);
void audio_release(void);
const audio_format_t *audio_get_format(void);
const audio_format_t *audio_get_playback_format(void);
void audio_set_capture_frame_cb(audio_capture_frame_cb_t cb, void *ctx);
esp_err_t audio_register_capture_observer(audio_capture_frame_cb_t cb, void *ctx);
void audio_unregister_capture_observer(audio_capture_frame_cb_t cb, void *ctx);
esp_err_t audio_set_capture_observer_enabled(audio_capture_frame_cb_t cb,
                                             void *ctx,
                                             bool enabled);
esp_err_t audio_set_capture_enabled(bool enabled);
esp_err_t audio_set_speaker_volume(uint8_t percent);
esp_err_t audio_set_capture_gain_percent(uint8_t percent);
esp_err_t audio_set_capture_processing_config(const audio_capture_processing_config_t *config);
esp_err_t audio_prepare_playback_path(void);
esp_err_t audio_play_pcm_frame_with_format(const uint8_t *data,
                                                    size_t data_len,
                                                    const audio_format_t *format);
esp_err_t audio_render_playback_pcm(const uint8_t *data,
                                             size_t data_len,
                                             const audio_format_t *format,
                                             int16_t **output_data,
                                             size_t *output_bytes,
                                             uint32_t *output_level);
esp_err_t audio_write_rendered_playback(int16_t *data,
                                                  size_t data_len,
                                                  uint32_t output_level);
void audio_get_last_playback_timing(audio_playback_timing_t *timing);
void audio_stop_playback(void);
esp_err_t audio_play_test_tone(uint32_t tone_hz, uint32_t duration_ms);
void audio_get_stats(audio_stats_t *stats);
