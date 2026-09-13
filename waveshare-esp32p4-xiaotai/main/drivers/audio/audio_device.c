#include "audio_device.h"

#include "audio.h"
#include "audio_echo_cancel.h"

esp_err_t audio_device_prepare(void)
{
    return audio_prepare();
}

void audio_device_release(void)
{
    audio_release();
}

void audio_device_get_stats(audio_stats_t *stats)
{
    audio_get_stats(stats);
}

esp_err_t audio_device_prepare_echo_cancel(void)
{
    return audio_echo_cancel_prepare();
}

esp_err_t audio_device_set_echo_cancel_active(bool active)
{
    return audio_echo_cancel_set_active(active);
}

const audio_format_t *microphone_get_format(void)
{
    return audio_get_format();
}

esp_err_t microphone_prepare_capture_path(void)
{
    return audio_prepare_input_path();
}

void microphone_set_frame_cb(audio_capture_frame_cb_t cb, void *ctx)
{
    audio_set_capture_frame_cb(cb, ctx);
}

esp_err_t microphone_register_observer(audio_capture_frame_cb_t cb, void *ctx)
{
    return audio_register_capture_observer(cb, ctx);
}

void microphone_unregister_observer(audio_capture_frame_cb_t cb, void *ctx)
{
    audio_unregister_capture_observer(cb, ctx);
}

esp_err_t microphone_set_observer_enabled(audio_capture_frame_cb_t cb, void *ctx, bool enabled)
{
    return audio_set_capture_observer_enabled(cb, ctx, enabled);
}

esp_err_t microphone_set_enabled(bool enabled)
{
    return audio_set_capture_enabled(enabled);
}

esp_err_t microphone_set_gain_percent(uint8_t percent)
{
    return audio_set_capture_gain_percent(percent);
}

esp_err_t microphone_set_processing_config(const audio_capture_processing_config_t *config)
{
    return audio_set_capture_processing_config(config);
}

const audio_format_t *speaker_get_playback_format(void)
{
    return audio_get_playback_format();
}

esp_err_t speaker_set_volume_percent(uint8_t percent)
{
    return audio_set_speaker_volume(percent);
}

esp_err_t speaker_prepare_playback_path(void)
{
    return audio_prepare_playback_path();
}

esp_err_t speaker_play_pcm_frame(const uint8_t *data, size_t data_len, const audio_format_t *format)
{
    return audio_play_pcm_frame_with_format(data, data_len, format);
}

esp_err_t speaker_render_pcm(const uint8_t *data,
                             size_t data_len,
                             const audio_format_t *format,
                             int16_t **output_data,
                             size_t *output_bytes,
                             uint32_t *output_level)
{
    return audio_render_playback_pcm(data, data_len, format, output_data, output_bytes, output_level);
}

esp_err_t speaker_write_rendered_pcm(int16_t *data, size_t data_len, uint32_t output_level)
{
    return audio_write_rendered_playback(data, data_len, output_level);
}

void speaker_get_last_playback_timing(audio_playback_timing_t *timing)
{
    audio_get_last_playback_timing(timing);
}

void speaker_stop_playback(void)
{
    audio_stop_playback();
}

esp_err_t speaker_play_test_tone(uint32_t tone_hz, uint32_t duration_ms)
{
    return audio_play_test_tone(tone_hz, duration_ms);
}
