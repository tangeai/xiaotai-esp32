#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_echo_cancel.h"
#include "app_log_policy.h"
#include "app_task_affinity.h"
#include "hardware_board.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "audio";

#define AUDIO_INPUT_LEVEL_DISPLAY_SCALE  10U
#define AUDIO_CAPTURE_AUTO_GAIN_TARGET_PEAK 8192U
#define AUDIO_CAPTURE_AUTO_GAIN_NOISE_FLOOR_PEAK 192U
#define AUDIO_CAPTURE_UPLOAD_GAIN_MAX_Q8 384U
#define AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT 200U
#define AUDIO_CAPTURE_AUTO_GAIN_LIMIT_PERCENT 800U
#define AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8   256U
#define AUDIO_CAPTURE_AUTO_GAIN_ATTACK_DIV 4U
#define AUDIO_CAPTURE_HIGH_PASS_ALPHA_Q15 31506
#define AUDIO_MAX_CAPTURE_GAIN_DB        30.0f
#define AUDIO_CAPTURE_LEVEL_LOG_INTERVAL_MS 15000U
#define AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10 (-960)
#define AUDIO_SPEAKER_POWER_SETTLE_MS     5U
#define AUDIO_CAPTURE_PRIMARY_CHANNEL HARDWARE_BOARD_AUDIO_ADC_PRIMARY_CHANNEL
#define AUDIO_CAPTURE_REFERENCE_CHANNEL HARDWARE_BOARD_AUDIO_ADC_REFERENCE_CHANNEL
#define AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT \
    ((uint8_t)(((HARDWARE_BOARD_AUDIO_DEFAULT_ADC_GAIN_DB) * 100.0f / AUDIO_MAX_CAPTURE_GAIN_DB) + 0.5f))
/*
 * ESP-IDF uses the same DMA sizing for both sides of this duplex I2S channel.
 * Keep it conservative so TiRTC/WebRTC internal RAM pressure does not prevent
 * the microphone path from coming up when the speaker path is also prepared.
 */
#define AUDIO_I2S_DMA_DESC_NUM           6
#define AUDIO_I2S_DMA_FRAME_NUM          64
#define AUDIO_PLAYBACK_SAMPLE_RATE_HZ    HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ
#define AUDIO_PLAYBACK_OUTPUT_CHANNELS   HARDWARE_BOARD_AUDIO_CHANNELS
#define AUDIO_CAPTURE_TASK_STACK         (6 * 1024)
#define AUDIO_CAPTURE_TASK_PRIORITY      APP_TASK_PRIORITY_AUDIO_CAPTURE
#define AUDIO_CAPTURE_TASK_CORE          APP_TASK_CORE_AUDIO
#define AUDIO_CAPTURE_OBSERVER_MAX       4
#define AUDIO_CAPTURE_TASK_STOP_WAIT_MS  300
/*
 * AI Chat declares PCM/16 kHz/1 ch to the cloud. Keep the local capture stream
 * at the native board rate so ASR receives real 16 kHz audio instead of an
 * 8 kHz stream expanded back to 16 kHz later.
 */
#define AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ
#define AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ     HARDWARE_BOARD_AUDIO_SAMPLE_RATE_HZ
#define AUDIO_CAPTURE_HW_INPUT_CHANNELS     HARDWARE_BOARD_AUDIO_ADC_CHANNELS
#define AUDIO_CAPTURE_OUTPUT_CHANNELS    1
#define AUDIO_CAPTURE_DOWNSAMPLE_RATIO \
    (AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ / AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ)

#if (AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ % AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ) != 0
#error "audio capture upload sample rate must evenly divide hardware sample rate"
#endif

#if AUDIO_CAPTURE_PRIMARY_CHANNEL >= AUDIO_CAPTURE_HW_INPUT_CHANNELS
#error "audio capture primary channel must be within hardware input channels"
#endif

#if AUDIO_CAPTURE_REFERENCE_CHANNEL >= AUDIO_CAPTURE_HW_INPUT_CHANNELS
#error "audio capture reference channel must be within hardware input channels"
#endif

#if AUDIO_CAPTURE_REFERENCE_CHANNEL == AUDIO_CAPTURE_PRIMARY_CHANNEL
#error "audio capture reference channel must differ from the microphone channel"
#endif

static const audio_format_t s_capture_format = {
    .sample_rate_hz = AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ,
    .channels = AUDIO_CAPTURE_OUTPUT_CHANNELS,
    .bits_per_sample = HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
};

static const audio_format_t s_playback_format = {
    .sample_rate_hz = AUDIO_PLAYBACK_SAMPLE_RATE_HZ,
    .channels = AUDIO_PLAYBACK_OUTPUT_CHANNELS,
    .bits_per_sample = HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
};

/*
 * esp_codec_dev's default 0..100 curve is linear from -50 dB to 0 dB, so the
 * product default of 70 maps to -15 dB and wastes most of the small onboard
 * speaker's usable range. Keep 0 as a hard mute and preserve the 0 dB ceiling,
 * but use a board-specific perceptual control curve in the normal UI range.
 */
static esp_codec_dev_vol_map_t s_p4_speaker_volume_map[] = {
    { .vol = 0,   .db_value = -50.0f },
    { .vol = 30,  .db_value = -24.0f },
    { .vol = 50,  .db_value = -12.0f },
    { .vol = 70,  .db_value = -6.0f },
    { .vol = 85,  .db_value = -2.0f },
    { .vol = 100, .db_value = 0.0f },
};

typedef struct {
    audio_capture_frame_cb_t cb;
    void *ctx;
    bool enabled;
} audio_capture_observer_t;

static audio_capture_frame_cb_t s_capture_cb;
static void *s_capture_cb_ctx;
static bool s_capture_primary_enabled;
static audio_capture_observer_t s_capture_observers[AUDIO_CAPTURE_OBSERVER_MAX];
static TaskHandle_t s_capture_task;
static TaskHandle_t s_tone_task;
static bool s_capture_task_stop_requested;
static bool s_audio_ready;
static bool s_audio_output_ready;
static bool s_audio_input_ready;
static bool s_audio_preparing;
static bool s_audio_output_preparing;
static bool s_audio_input_preparing;
static bool s_speaker_path_enabled;
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_playback_mutex;
static audio_stats_t s_audio_stats = {
    .speaker_volume_percent = HARDWARE_BOARD_AUDIO_DEFAULT_VOLUME,
    .capture_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .capture_codec_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .capture_upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .capture_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .capture_effective_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .far_end_upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .far_end_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED,
};
static esp_err_t s_audio_output_prepare_last_err = ESP_OK;
static esp_err_t s_audio_input_prepare_last_err = ESP_OK;
static TickType_t s_audio_output_prepare_retry_after_ticks;
static TickType_t s_audio_input_prepare_retry_after_ticks;
static uint8_t s_speaker_volume_percent = HARDWARE_BOARD_AUDIO_DEFAULT_VOLUME;
static audio_capture_processing_config_t s_capture_processing_config = {
    .send_volume_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .codec_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .far_end_gain_guard_enabled = false,
    .far_end_upload_gain_percent = AUDIO_DEFAULT_CAPTURE_GAIN_PERCENT,
    .far_end_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
    .echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED,
    .high_pass_filter_enabled = false,
};
static bool s_playback_muted_logged;
static bool s_playback_write_logged;
static TickType_t s_last_playback_write_log_tick;

static esp_codec_dev_handle_t s_play_dev_handle;
static esp_codec_dev_handle_t s_record_dev_handle;
static esp_codec_dev_handle_t s_shared_es8311_dev_handle;
static const audio_codec_if_t *s_play_codec_if;
static const audio_codec_ctrl_if_t *s_play_ctrl_if;
static const audio_codec_gpio_if_t *s_play_gpio_if;
static const audio_codec_if_t *s_record_codec_if;
static const audio_codec_ctrl_if_t *s_record_ctrl_if;
static const audio_codec_gpio_if_t *s_record_gpio_if;
static const audio_codec_if_t *s_shared_es8311_codec_if;
static const audio_codec_ctrl_if_t *s_shared_es8311_ctrl_if;
static const audio_codec_gpio_if_t *s_shared_es8311_gpio_if;
static i2s_chan_handle_t s_i2s_tx_chan;
static i2s_chan_handle_t s_i2s_rx_chan;
static const audio_codec_data_if_t *s_i2s_data_if;

static uint8_t *s_playback_scratch;
static size_t s_playback_scratch_size;
static int16_t *s_capture_raw_buffer;
static int16_t *s_capture_mono_buffer;
static int16_t *s_capture_reference_buffer;
static audio_playback_timing_t s_last_playback_timing;
static bool s_playback_path_ready_logged;

static esp_codec_dev_handle_t audio_new_speaker(void);
static esp_codec_dev_handle_t audio_new_microphone(void);
static void audio_capture_task(void *ctx);

static esp_err_t audio_ensure_playback_mutex(void)
{
    if (s_playback_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    if (s_playback_mutex == NULL) {
        s_playback_mutex = mutex;
        mutex = NULL;
    }
    taskEXIT_CRITICAL(&s_audio_lock);

    if (mutex != NULL) {
        vSemaphoreDeleteWithCaps(mutex);
    }
    return ESP_OK;
}

static esp_err_t audio_take_playback_mutex(TickType_t wait_ticks)
{
    ESP_RETURN_ON_ERROR(audio_ensure_playback_mutex(), TAG, "create playback mutex failed");
    return xSemaphoreTakeRecursive(s_playback_mutex, wait_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void audio_give_playback_mutex(void)
{
    if (s_playback_mutex != NULL) {
        xSemaphoreGiveRecursive(s_playback_mutex);
    }
}

static uint32_t audio_capture_peak_to_meter_percent(uint32_t peak)
{
    uint32_t meter_percent = (peak * 100U * AUDIO_INPUT_LEVEL_DISPLAY_SCALE) / 32767U;

    return meter_percent > 100U ? 100U : meter_percent;
}

static int16_t audio_clip_i16(int32_t sample)
{
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return (int16_t)sample;
}

static uint32_t audio_abs_i16(int16_t sample)
{
    return sample == INT16_MIN ? 32768U : (uint32_t)abs(sample);
}

#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
static int audio_dbfs_x10_from_ratio(double ratio)
{
    if (ratio <= 0.0) {
        return AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10;
    }

    double db = 20.0 * log10(ratio);
    int value = (int)(db * 10.0 + (db >= 0.0 ? 0.5 : -0.5));
    return value < AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10 ? AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10 : value;
}

static int audio_peak_dbfs_x10(uint32_t peak)
{
    if (peak == 0U) {
        return AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10;
    }
    if (peak > 32767U) {
        peak = 32767U;
    }
    return audio_dbfs_x10_from_ratio((double)peak / 32767.0);
}

static int audio_rms_dbfs_x10(uint64_t square_sum, uint32_t sample_count)
{
    if (square_sum == 0U || sample_count == 0U) {
        return AUDIO_CAPTURE_LEVEL_DBFS_FLOOR_X10;
    }

    double rms = sqrt((double)square_sum / (double)sample_count);
    return audio_dbfs_x10_from_ratio(rms / 32767.0);
}

static void audio_format_dbfs_x10(int dbfs_x10, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    int abs_value = dbfs_x10 < 0 ? -dbfs_x10 : dbfs_x10;
    snprintf(buffer,
             buffer_size,
             "%s%d.%01d",
             dbfs_x10 < 0 ? "-" : "",
             abs_value / 10,
             abs_value % 10);
}
#endif

static audio_capture_processing_config_t audio_get_capture_processing_config_locked(void)
{
    return s_capture_processing_config;
}

static uint8_t audio_get_speaker_volume_percent_locked(void)
{
    return s_speaker_volume_percent > 100U ? 100U : s_speaker_volume_percent;
}

static uint8_t audio_clamp_percent(uint8_t percent)
{
    return percent > 100U ? 100U : percent;
}

static uint16_t audio_clamp_auto_gain_percent(uint16_t percent)
{
    if (percent < 100U) {
        return 100U;
    }
    return percent > AUDIO_CAPTURE_AUTO_GAIN_LIMIT_PERCENT ?
           AUDIO_CAPTURE_AUTO_GAIN_LIMIT_PERCENT : percent;
}

static audio_capture_processing_config_t audio_capture_make_default_processing_config(
    uint8_t percent)
{
    percent = audio_clamp_percent(percent);
    return (audio_capture_processing_config_t) {
        .send_volume_percent = percent,
        .codec_gain_percent = percent,
        .upload_gain_percent = percent,
        .auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
        .far_end_gain_guard_enabled = false,
        .far_end_upload_gain_percent = percent,
        .far_end_auto_gain_max_percent = AUDIO_CAPTURE_AUTO_GAIN_DEFAULT_MAX_PERCENT,
        .echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED,
        .high_pass_filter_enabled = false,
    };
}

static audio_capture_processing_config_t audio_capture_sanitize_processing_config(
    const audio_capture_processing_config_t *config)
{
    audio_capture_processing_config_t sanitized = *config;
    sanitized.send_volume_percent = audio_clamp_percent(sanitized.send_volume_percent);
    sanitized.codec_gain_percent = audio_clamp_percent(sanitized.codec_gain_percent);
    sanitized.upload_gain_percent = audio_clamp_percent(sanitized.upload_gain_percent);
    sanitized.auto_gain_max_percent =
        audio_clamp_auto_gain_percent(sanitized.auto_gain_max_percent);
    sanitized.far_end_upload_gain_percent =
        audio_clamp_percent(sanitized.far_end_upload_gain_percent);
    sanitized.far_end_auto_gain_max_percent =
        audio_clamp_auto_gain_percent(sanitized.far_end_auto_gain_max_percent);
    if (sanitized.echo_suppression != AUDIO_ECHO_SUPPRESSION_STRONG) {
        sanitized.echo_suppression = AUDIO_ECHO_SUPPRESSION_BALANCED;
    }
    return sanitized;
}

static uint32_t audio_capture_base_gain_q8(uint8_t upload_gain_percent)
{
    if (upload_gain_percent == 0U) {
        return 0U;
    }
    return ((uint32_t)upload_gain_percent * AUDIO_CAPTURE_UPLOAD_GAIN_MAX_Q8 + 99U) / 100U;
}

static uint32_t audio_capture_auto_gain_target_q8(uint32_t pre_peak,
                                                   uint32_t base_gain_q8,
                                                   uint16_t max_percent)
{
    if (pre_peak < AUDIO_CAPTURE_AUTO_GAIN_NOISE_FLOOR_PEAK || base_gain_q8 == 0U) {
        return AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }

    uint32_t base_peak = (uint32_t)(((uint64_t)pre_peak * base_gain_q8) / 256ULL);
    if (base_peak == 0U || base_peak >= AUDIO_CAPTURE_AUTO_GAIN_TARGET_PEAK) {
        return AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }

    uint32_t target_q8 =
        (uint32_t)(((uint64_t)AUDIO_CAPTURE_AUTO_GAIN_TARGET_PEAK * 256ULL) / base_peak);
    if (target_q8 < AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8) {
        target_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }
    uint32_t max_q8 = ((uint32_t)audio_clamp_auto_gain_percent(max_percent) * 256U) / 100U;
    if (target_q8 > max_q8) {
        target_q8 = max_q8;
    }
    return target_q8;
}

static uint32_t audio_capture_smooth_auto_gain_q8(uint32_t current_q8, uint32_t target_q8)
{
    if (current_q8 < AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8) {
        current_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    }
    if (target_q8 <= current_q8) {
        return target_q8;
    }

    uint32_t delta = target_q8 - current_q8;
    uint32_t step = delta / AUDIO_CAPTURE_AUTO_GAIN_ATTACK_DIV;
    if (step == 0U) {
        step = 1U;
    }
    return current_q8 + step;
}

static int16_t audio_apply_capture_upload_gain(int32_t sample, uint32_t base_gain_q8, uint32_t auto_gain_q8)
{
    if (base_gain_q8 == 0U || auto_gain_q8 == 0U) {
        return 0;
    }

    int64_t amplified = (int64_t)sample * (int64_t)base_gain_q8 * (int64_t)auto_gain_q8;
    amplified /= (256LL * 256LL);
    return audio_clip_i16((int32_t)amplified);
}

static int16_t audio_capture_high_pass_filter(int16_t sample,
                                              int32_t *previous_input,
                                              int32_t *previous_output)
{
    int32_t input = sample;
    int64_t filtered = (int64_t)AUDIO_CAPTURE_HIGH_PASS_ALPHA_Q15 *
                       (*previous_output + input - *previous_input);
    filtered >>= 15;
    *previous_input = input;
    *previous_output = (int32_t)filtered;
    return audio_clip_i16((int32_t)filtered);
}

static void audio_mute_playback_path_no_mutex(void)
{
    if (!s_speaker_path_enabled) {
        return;
    }
    (void)esp_codec_dev_set_out_mute(s_play_dev_handle, true);
    (void)hardware_board_set_audio_power(false);
    s_speaker_path_enabled = false;
    s_playback_path_ready_logged = false;
}

static float audio_capture_gain_percent_to_db(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return ((float)percent * AUDIO_MAX_CAPTURE_GAIN_DB) / 100.0f;
}

static void audio_log_heap(const char *stage)
{
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGD(TAG,
             "%s heap dma_free=%u dma_largest=%u internal_free=%u psram_free=%u",
             stage != NULL ? stage : "audio",
             (unsigned)dma_free,
             (unsigned)dma_largest,
             (unsigned)internal_free,
             (unsigned)psram_free);
}

static void audio_update_ready_state(void)
{
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_ready = s_audio_output_ready || s_audio_input_ready;
    s_audio_stats.ready = s_audio_ready;
    if (!s_audio_input_ready) {
        s_audio_stats.capture_enabled = false;
        s_audio_stats.input_level = 0;
    }
    if (!s_audio_output_ready) {
        s_audio_stats.speaker_enabled = false;
        s_audio_stats.output_level = 0;
    }
    taskEXIT_CRITICAL(&s_audio_lock);
}

static bool audio_capture_has_active_consumer_locked(void)
{
    if (s_capture_primary_enabled && s_capture_cb != NULL) {
        return true;
    }

    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].enabled && s_capture_observers[index].cb != NULL) {
            return true;
        }
    }
    return false;
}

static esp_err_t audio_ensure_scratch(size_t required_size)
{
    if (required_size <= s_playback_scratch_size) {
        return ESP_OK;
    }

    uint8_t *new_buffer = app_memory_alloc_psram(required_size);
    if (new_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_playback_scratch != NULL && s_playback_scratch_size > 0) {
        memcpy(new_buffer, s_playback_scratch, s_playback_scratch_size);
        free(s_playback_scratch);
    }

    s_playback_scratch = new_buffer;
    s_playback_scratch_size = required_size;
    return ESP_OK;
}

static void *audio_calloc_psram(size_t count, size_t size)
{
    return app_memory_calloc_psram(count, size);
}

static esp_err_t audio_ensure_capture_buffers(void)
{
    const size_t samples_per_frame = AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ / 50;
    const size_t raw_samples_per_frame = samples_per_frame * AUDIO_CAPTURE_DOWNSAMPLE_RATIO *
                                         AUDIO_CAPTURE_HW_INPUT_CHANNELS;
    int16_t *raw_buffer = s_capture_raw_buffer;
    int16_t *mono_buffer = s_capture_mono_buffer;
    int16_t *reference_buffer = s_capture_reference_buffer;

    if (raw_buffer == NULL) {
        raw_buffer = audio_calloc_psram(raw_samples_per_frame, sizeof(int16_t));
        if (raw_buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (mono_buffer == NULL) {
        mono_buffer = audio_calloc_psram(samples_per_frame, sizeof(int16_t));
        if (mono_buffer == NULL) {
            if (s_capture_raw_buffer == NULL) {
                free(raw_buffer);
            }
            return ESP_ERR_NO_MEM;
        }
    }

    if (reference_buffer == NULL) {
        reference_buffer = audio_calloc_psram(samples_per_frame, sizeof(int16_t));
        if (reference_buffer == NULL) {
            if (s_capture_raw_buffer == NULL) {
                free(raw_buffer);
            }
            if (s_capture_mono_buffer == NULL) {
                free(mono_buffer);
            }
            return ESP_ERR_NO_MEM;
        }
    }

    s_capture_raw_buffer = raw_buffer;
    s_capture_mono_buffer = mono_buffer;
    s_capture_reference_buffer = reference_buffer;
    return ESP_OK;
}

static void audio_release_capture_buffers(void)
{
    free(s_capture_raw_buffer);
    free(s_capture_mono_buffer);
    free(s_capture_reference_buffer);
    s_capture_raw_buffer = NULL;
    s_capture_mono_buffer = NULL;
    s_capture_reference_buffer = NULL;
}

static void audio_delete_codec_interfaces(const audio_codec_if_t **codec_if,
                                          const audio_codec_ctrl_if_t **ctrl_if,
                                          const audio_codec_gpio_if_t **gpio_if)
{
    if (codec_if != NULL && *codec_if != NULL) {
        (void)audio_codec_delete_codec_if(*codec_if);
        *codec_if = NULL;
    }
    if (ctrl_if != NULL && *ctrl_if != NULL) {
        (void)audio_codec_delete_ctrl_if(*ctrl_if);
        *ctrl_if = NULL;
    }
    if (gpio_if != NULL && *gpio_if != NULL) {
        (void)audio_codec_delete_gpio_if(*gpio_if);
        *gpio_if = NULL;
    }
}

static void audio_release_codec_instance(esp_codec_dev_handle_t *dev_handle,
                                         const audio_codec_if_t **codec_if,
                                         const audio_codec_ctrl_if_t **ctrl_if,
                                         const audio_codec_gpio_if_t **gpio_if)
{
    if (dev_handle != NULL && *dev_handle != NULL) {
        esp_codec_dev_close(*dev_handle);
        esp_codec_dev_delete(*dev_handle);
        *dev_handle = NULL;
    }

    audio_delete_codec_interfaces(codec_if, ctrl_if, gpio_if);
}

static void audio_release_shared_es8311_device(void)
{
    esp_codec_dev_handle_t shared_handle = s_shared_es8311_dev_handle;

    if (shared_handle != NULL) {
        esp_codec_dev_close(shared_handle);
        esp_codec_dev_delete(shared_handle);
    }
    if (s_play_dev_handle == shared_handle) {
        s_play_dev_handle = NULL;
    }
    if (s_record_dev_handle == shared_handle) {
        s_record_dev_handle = NULL;
    }
    s_shared_es8311_dev_handle = NULL;

    audio_delete_codec_interfaces(&s_shared_es8311_codec_if,
                                  &s_shared_es8311_ctrl_if,
                                  &s_shared_es8311_gpio_if);
}

static void audio_release_speaker_device(void)
{
    if (s_play_dev_handle == NULL) {
        return;
    }

    if (s_play_dev_handle == s_shared_es8311_dev_handle) {
        if (s_audio_input_ready) {
            return;
        }
        audio_release_shared_es8311_device();
        return;
    }

    audio_release_codec_instance(&s_play_dev_handle,
                                 &s_play_codec_if,
                                 &s_play_ctrl_if,
                                 &s_play_gpio_if);
}

static void audio_release_microphone_device(void)
{
    if (s_record_dev_handle == NULL) {
        return;
    }

    if (s_record_dev_handle == s_shared_es8311_dev_handle) {
        if (s_audio_output_ready) {
            return;
        }
        audio_release_shared_es8311_device();
        return;
    }

    audio_release_codec_instance(&s_record_dev_handle,
                                 &s_record_codec_if,
                                 &s_record_ctrl_if,
                                 &s_record_gpio_if);
}

static void audio_release_i2s_channel(i2s_chan_handle_t *channel)
{
    if (channel == NULL || *channel == NULL) {
        return;
    }

    i2s_chan_info_t chan_info = {0};
    if (i2s_channel_get_info(*channel, &chan_info) == ESP_OK && chan_info.is_enabled) {
        (void)i2s_channel_disable(*channel);
    }
    (void)i2s_del_channel(*channel);
    *channel = NULL;
}

static void audio_release_i2s_bus(void)
{
    if (s_i2s_data_if != NULL) {
        (void)audio_codec_delete_data_if(s_i2s_data_if);
        s_i2s_data_if = NULL;
    }
    audio_release_i2s_channel(&s_i2s_rx_chan);
    audio_release_i2s_channel(&s_i2s_tx_chan);
}

static void audio_cleanup_output_prepare_failure(void)
{
    s_audio_output_ready = false;
    s_speaker_path_enabled = false;
    audio_release_speaker_device();
    if (!s_audio_input_ready) {
        audio_release_i2s_bus();
    }
    audio_update_ready_state();
}

static void audio_cleanup_input_prepare_failure(void)
{
    s_audio_input_ready = false;
    s_capture_primary_enabled = false;
    audio_release_microphone_device();
    if (!s_audio_output_ready) {
        audio_release_i2s_bus();
    }
    audio_release_capture_buffers();
    audio_update_ready_state();
}

static esp_err_t audio_open_speaker(uint32_t sample_rate_hz, uint32_t bits_per_sample, uint32_t channels)
{
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate_hz,
        .bits_per_sample = bits_per_sample,
        .channel = channels,
    };

    ESP_RETURN_ON_FALSE(s_play_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker codec handle missing");
    return esp_codec_dev_open(s_play_dev_handle, &sample_info);
}

static esp_err_t audio_open_microphone(uint32_t sample_rate_hz, uint32_t bits_per_sample, uint32_t channels)
{
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate_hz,
        .bits_per_sample = bits_per_sample,
        .channel = channels,
    };

    uint8_t codec_gain_percent = 0U;
    taskENTER_CRITICAL(&s_audio_lock);
    codec_gain_percent = s_capture_processing_config.codec_gain_percent;
    taskEXIT_CRITICAL(&s_audio_lock);

    ESP_RETURN_ON_FALSE(s_record_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "mic codec handle missing");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_record_dev_handle, &sample_info), TAG, "open mic codec failed");
    return esp_codec_dev_set_in_gain(s_record_dev_handle,
                                     audio_capture_gain_percent_to_db(codec_gain_percent));
}

static esp_err_t audio_bus_init(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_i2s_data_if != NULL && s_i2s_tx_chan != NULL && s_i2s_rx_chan != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(HARDWARE_BOARD_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_I2S_DMA_FRAME_NUM;

    if (s_i2s_tx_chan == NULL && s_i2s_rx_chan == NULL) {
        audio_log_heap("i2s before duplex new_channel");
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan),
                            TAG,
                            "create i2s duplex channel failed");
    } else if (s_i2s_tx_chan == NULL || s_i2s_rx_chan == NULL) {
        audio_release_i2s_bus();
        return ESP_ERR_INVALID_STATE;
    }

    i2s_std_slot_config_t tx_slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                            AUDIO_PLAYBACK_OUTPUT_CHANNELS > 1 ?
                                                I2S_SLOT_MODE_STEREO :
                                                I2S_SLOT_MODE_MONO);
    if (AUDIO_PLAYBACK_OUTPUT_CHANNELS <= 1) {
        tx_slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_PLAYBACK_SAMPLE_RATE_HZ),
        .slot_cfg = tx_slot_cfg,
        .gpio_cfg = {
            .mclk = HARDWARE_BOARD_AUDIO_MCLK,
            .bclk = HARDWARE_BOARD_AUDIO_BCLK,
            .ws = HARDWARE_BOARD_AUDIO_LRCK,
            .dout = HARDWARE_BOARD_AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };

    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            AUDIO_CAPTURE_HW_INPUT_CHANNELS > 1 ?
                I2S_SLOT_MODE_STEREO :
                I2S_SLOT_MODE_MONO);
    slot_cfg.slot_mask = AUDIO_CAPTURE_HW_INPUT_CHANNELS > 1 ?
                             I2S_STD_SLOT_BOTH :
                             I2S_STD_SLOT_LEFT;

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = HARDWARE_BOARD_AUDIO_MCLK,
            .bclk = HARDWARE_BOARD_AUDIO_BCLK,
            .ws = HARDWARE_BOARD_AUDIO_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din = HARDWARE_BOARD_AUDIO_DIN,
        },
    };

    i2s_tdm_config_t rx_tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                                            I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = HARDWARE_BOARD_AUDIO_MCLK,
            .bclk = HARDWARE_BOARD_AUDIO_BCLK,
            .ws = HARDWARE_BOARD_AUDIO_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din = HARDWARE_BOARD_AUDIO_DIN,
        },
    };

    if (s_i2s_data_if == NULL) {
        esp_err_t ret = i2s_channel_init_std_mode(s_i2s_tx_chan, &tx_std_cfg);
        if (ret != ESP_OK) {
            audio_log_heap("i2s tx init failed");
            audio_release_i2s_bus();
            return ret;
        }
        ret = audio_config != NULL && audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES7210 ?
                  i2s_channel_init_tdm_mode(s_i2s_rx_chan, &rx_tdm_cfg) :
                  i2s_channel_init_std_mode(s_i2s_rx_chan, &rx_std_cfg);
        if (ret != ESP_OK) {
            audio_log_heap("i2s rx init failed");
            audio_release_i2s_bus();
            return ret;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = HARDWARE_BOARD_AUDIO_I2S_PORT,
            .rx_handle = s_i2s_rx_chan,
            .tx_handle = s_i2s_tx_chan,
        };
        s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    }

    return s_i2s_data_if == NULL ? ESP_FAIL : ESP_OK;
}

static esp_err_t audio_do_prepare_output(void)
{
    esp_err_t ret = hardware_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_play_dev_handle == NULL) {
        s_play_dev_handle = audio_new_speaker();
    }
    if (s_play_dev_handle == NULL) {
        return ESP_FAIL;
    }

    ret = audio_open_speaker(s_playback_format.sample_rate_hz,
                                       s_playback_format.bits_per_sample,
                                       s_playback_format.channels);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_codec_dev_vol_curve_t volume_curve = {
        .count = sizeof(s_p4_speaker_volume_map) /
                 sizeof(s_p4_speaker_volume_map[0]),
        .vol_map = s_p4_speaker_volume_map,
    };
    ret = esp_codec_dev_set_vol_curve(s_play_dev_handle, &volume_curve);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_codec_dev_set_out_vol(s_play_dev_handle, s_speaker_volume_percent);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_codec_dev_set_out_mute(s_play_dev_handle, true);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = hardware_board_set_audio_power(false);
    if (ret != ESP_OK) {
        return ret;
    }
    s_speaker_path_enabled = false;

    return ESP_OK;
}

static esp_err_t audio_do_prepare_input(void)
{
    esp_err_t ret = hardware_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_record_dev_handle == NULL) {
        s_record_dev_handle = audio_new_microphone();
    }
    if (s_record_dev_handle == NULL) {
        return ESP_FAIL;
    }

    ret = audio_open_microphone(AUDIO_CAPTURE_HW_SAMPLE_RATE_HZ,
                                         HARDWARE_BOARD_AUDIO_BITS_PER_SAMPLE,
                                         AUDIO_CAPTURE_HW_INPUT_CHANNELS);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_ensure_capture_buffers();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_capture_task == NULL) {
        BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(audio_capture_task,
                                                             "audio_capture",
                                                             AUDIO_CAPTURE_TASK_STACK,
                                                             NULL,
                                                             AUDIO_CAPTURE_TASK_PRIORITY,
                                                             &s_capture_task,
                                                             AUDIO_CAPTURE_TASK_CORE,
                                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (task_ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static bool audio_uses_shared_es8311_codec(const hardware_audio_config_t *audio_config)
{
    return audio_config != NULL &&
           audio_config->speaker_codec == HARDWARE_AUDIO_CODEC_ES8311 &&
           audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES8311 &&
           audio_config->speaker_codec_i2c_addr == audio_config->microphone_codec_i2c_addr;
}

static const audio_codec_if_t *audio_create_es8311_codec_if(const hardware_audio_config_t *audio_config,
                                                            uint8_t i2c_addr,
                                                            esp_codec_dec_work_mode_t codec_mode,
                                                            const audio_codec_ctrl_if_t **ctrl_if_slot,
                                                            const audio_codec_gpio_if_t **gpio_if_slot)
{
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        return NULL;
    }
    if (gpio_if_slot != NULL) {
        *gpio_if_slot = gpio_if;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = hardware_board_get_i2c_config()->port,
        .addr = i2c_addr,
        .bus_handle = hardware_board_get_i2c_bus_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (i2c_ctrl_if == NULL) {
        return NULL;
    }
    if (ctrl_if_slot != NULL) {
        *ctrl_if_slot = i2c_ctrl_if;
    }

    esp_codec_dev_hw_gain_t hw_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = codec_mode,
        .pa_pin = audio_config->pa_gpio,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hw_gain,
        .no_dac_ref = false,
    };

    return es8311_codec_new(&codec_cfg);
}

static esp_codec_dev_handle_t audio_new_shared_es8311_device(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_shared_es8311_dev_handle != NULL) {
        s_play_dev_handle = s_shared_es8311_dev_handle;
        s_record_dev_handle = s_shared_es8311_dev_handle;
        return s_shared_es8311_dev_handle;
    }

    if (s_i2s_data_if == NULL) {
        return NULL;
    }

    s_shared_es8311_codec_if = audio_create_es8311_codec_if(audio_config,
                                                            audio_config->speaker_codec_i2c_addr,
                                                            ESP_CODEC_DEV_WORK_MODE_BOTH,
                                                            &s_shared_es8311_ctrl_if,
                                                            &s_shared_es8311_gpio_if);
    if (s_shared_es8311_codec_if == NULL) {
        audio_release_shared_es8311_device();
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_shared_es8311_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_shared_es8311_dev_handle = esp_codec_dev_new(&dev_cfg);
    if (s_shared_es8311_dev_handle == NULL) {
        audio_release_shared_es8311_device();
        return NULL;
    }

    s_play_dev_handle = s_shared_es8311_dev_handle;
    s_record_dev_handle = s_shared_es8311_dev_handle;
    return s_shared_es8311_dev_handle;
}

static esp_codec_dev_handle_t audio_new_speaker(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_i2s_data_if == NULL) {
        return NULL;
    }
    if (audio_uses_shared_es8311_codec(audio_config)) {
        return audio_new_shared_es8311_device();
    }
    if (audio_config->speaker_codec != HARDWARE_AUDIO_CODEC_ES8311) {
        ESP_LOGE(TAG, "unsupported speaker codec: %d", (int)audio_config->speaker_codec);
        return NULL;
    }

    s_play_codec_if = audio_create_es8311_codec_if(audio_config,
                                                   audio_config->speaker_codec_i2c_addr,
                                                   ESP_CODEC_DEV_WORK_MODE_DAC,
                                                   &s_play_ctrl_if,
                                                   &s_play_gpio_if);
    if (s_play_codec_if == NULL) {
        audio_release_codec_instance(&s_play_dev_handle,
                                     &s_play_codec_if,
                                     &s_play_ctrl_if,
                                     &s_play_gpio_if);
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_play_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_play_dev_handle = esp_codec_dev_new(&dev_cfg);
    if (s_play_dev_handle == NULL) {
        audio_release_codec_instance(&s_play_dev_handle,
                                     &s_play_codec_if,
                                     &s_play_ctrl_if,
                                     &s_play_gpio_if);
    }
    return s_play_dev_handle;
}

static esp_codec_dev_handle_t audio_new_microphone(void)
{
    const hardware_audio_config_t *audio_config = hardware_board_get_audio_config();

    if (s_i2s_data_if == NULL) {
        return NULL;
    }
    if (audio_uses_shared_es8311_codec(audio_config)) {
        return audio_new_shared_es8311_device();
    }

    if (audio_config->microphone_codec == HARDWARE_AUDIO_CODEC_ES8311) {
        s_record_codec_if = audio_create_es8311_codec_if(audio_config,
                                                         audio_config->microphone_codec_i2c_addr,
                                                         ESP_CODEC_DEV_WORK_MODE_ADC,
                                                         &s_record_ctrl_if,
                                                         &s_record_gpio_if);
        if (s_record_codec_if == NULL) {
            audio_release_codec_instance(&s_record_dev_handle,
                                         &s_record_codec_if,
                                         &s_record_ctrl_if,
                                         &s_record_gpio_if);
            return NULL;
        }

        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = s_record_codec_if,
            .data_if = s_i2s_data_if,
        };
        s_record_dev_handle = esp_codec_dev_new(&dev_cfg);
        if (s_record_dev_handle == NULL) {
            audio_release_codec_instance(&s_record_dev_handle,
                                         &s_record_codec_if,
                                         &s_record_ctrl_if,
                                         &s_record_gpio_if);
        }
        return s_record_dev_handle;
    }

    if (audio_config->microphone_codec != HARDWARE_AUDIO_CODEC_ES7210) {
        ESP_LOGE(TAG, "unsupported microphone codec: %d", (int)audio_config->microphone_codec);
        return NULL;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = hardware_board_get_i2c_config()->port,
        .addr = audio_config->microphone_codec_i2c_addr,
        .bus_handle = hardware_board_get_i2c_bus_handle(),
    };
    s_record_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_record_ctrl_if == NULL) {
        return NULL;
    }

    es7210_codec_cfg_t codec_cfg = {
        .ctrl_if = s_record_ctrl_if,
        .mic_selected = audio_config->microphone_select_mask,
    };
    s_record_codec_if = es7210_codec_new(&codec_cfg);
    if (s_record_codec_if == NULL) {
        audio_release_codec_instance(&s_record_dev_handle,
                                     &s_record_codec_if,
                                     &s_record_ctrl_if,
                                     &s_record_gpio_if);
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_record_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_record_dev_handle = esp_codec_dev_new(&dev_cfg);
    if (s_record_dev_handle == NULL) {
        audio_release_codec_instance(&s_record_dev_handle,
                                     &s_record_codec_if,
                                     &s_record_ctrl_if,
                                     &s_record_gpio_if);
    }
    return s_record_dev_handle;
}

static void audio_capture_task(void *ctx)
{
    (void)ctx;
    const size_t samples_per_frame = AUDIO_CAPTURE_UPLOAD_SAMPLE_RATE_HZ / 50;
    const size_t raw_samples_per_frame = samples_per_frame * AUDIO_CAPTURE_DOWNSAMPLE_RATIO *
                                         AUDIO_CAPTURE_HW_INPUT_CHANNELS;
    const size_t raw_frame_bytes = raw_samples_per_frame * sizeof(int16_t);
    int16_t *raw_buffer = s_capture_raw_buffer;
    int16_t *mono_buffer = s_capture_mono_buffer;
    int16_t *reference_buffer = s_capture_reference_buffer;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
    TickType_t last_level_log_tick = 0;
    uint32_t log_raw_channel_peak[AUDIO_CAPTURE_HW_INPUT_CHANNELS] = {0};
    uint32_t log_pre_gain_peak = 0;
    uint32_t log_post_gain_peak = 0;
    uint64_t log_pre_gain_square_sum = 0;
    uint64_t log_post_gain_square_sum = 0;
    uint32_t log_echo_ref_peak = 0;
    uint32_t log_echo_out_peak = 0;
    uint32_t log_echo_active_frames = 0;
    uint32_t log_echo_suppress_percent = 0;
    uint64_t log_echo_process_us = 0;
    uint32_t log_echo_process_max_us = 0;
    uint32_t log_echo_process_calls = 0;
    uint32_t log_sample_count = 0;
    uint32_t log_frame_count = 0;
    uint32_t log_auto_gain_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
#endif
    uint32_t auto_gain_q8 = AUDIO_CAPTURE_AUTO_GAIN_MIN_Q8;
    int32_t high_pass_previous_input = 0;
    int32_t high_pass_previous_output = 0;

    if (raw_buffer == NULL || mono_buffer == NULL || reference_buffer == NULL) {
        ESP_LOGE(TAG, "audio capture buffers alloc failed");
        taskENTER_CRITICAL(&s_audio_lock);
        s_capture_task = NULL;
        s_audio_input_ready = false;
        s_capture_primary_enabled = false;
        taskEXIT_CRITICAL(&s_audio_lock);
        audio_update_ready_state();
        vTaskDeleteWithCaps(NULL);
        return;
    }

    while (true) {
        bool stop_requested = false;
        bool capture_primary_enabled = false;
        bool capture_enabled = false;
        audio_capture_frame_cb_t capture_cb = NULL;
        void *capture_cb_ctx = NULL;
        audio_capture_observer_t observers[AUDIO_CAPTURE_OBSERVER_MAX] = {0};
        size_t observer_count = 0;

        taskENTER_CRITICAL(&s_audio_lock);
        stop_requested = s_capture_task_stop_requested;
        if (stop_requested) {
            s_capture_task = NULL;
            s_capture_task_stop_requested = false;
            s_audio_input_ready = false;
            s_capture_primary_enabled = false;
            s_audio_stats.capture_enabled = false;
            s_audio_stats.input_level = 0;
            taskEXIT_CRITICAL(&s_audio_lock);
            audio_update_ready_state();
            vTaskDeleteWithCaps(NULL);
            return;
        }
        capture_primary_enabled = s_capture_primary_enabled && s_capture_cb != NULL;
        capture_cb = s_capture_cb;
        capture_cb_ctx = s_capture_cb_ctx;
        for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
            if (s_capture_observers[index].enabled && s_capture_observers[index].cb != NULL) {
                observers[observer_count++] = s_capture_observers[index];
            }
        }
        capture_enabled = capture_primary_enabled || observer_count > 0;
        if (!capture_enabled) {
            s_audio_stats.input_level = 0;
        }
        taskEXIT_CRITICAL(&s_audio_lock);

        if (!capture_enabled) {
            high_pass_previous_input = 0;
            high_pass_previous_output = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        esp_err_t ret = esp_codec_dev_read(s_record_dev_handle, raw_buffer, raw_frame_bytes);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio capture read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t peak = 0;
        audio_capture_processing_config_t processing_config = {0};
        uint32_t pre_frame_peak = 0;
        uint32_t base_gain_q8 = 0;
        uint8_t effective_upload_gain_percent = 0;
        uint16_t effective_auto_gain_max_percent = 100U;
        audio_echo_cancel_metrics_t echo_metrics = {0};

        taskENTER_CRITICAL(&s_audio_lock);
        processing_config = audio_get_capture_processing_config_locked();
        taskEXIT_CRITICAL(&s_audio_lock);

        for (size_t frame_index = 0; frame_index < samples_per_frame; ++frame_index) {
            int32_t primary_sum = 0;
            size_t primary_sample_count = 0;
            int32_t reference_sum = 0;
            size_t reference_sample_count = 0;
            for (size_t downsample_index = 0; downsample_index < AUDIO_CAPTURE_DOWNSAMPLE_RATIO; ++downsample_index) {
                size_t raw_base_index = (frame_index * AUDIO_CAPTURE_DOWNSAMPLE_RATIO + downsample_index) *
                                        AUDIO_CAPTURE_HW_INPUT_CHANNELS;
                for (size_t channel_index = 0; channel_index < AUDIO_CAPTURE_HW_INPUT_CHANNELS; ++channel_index) {
                    int16_t raw_sample = raw_buffer[raw_base_index + channel_index];
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
                    uint32_t raw_abs = audio_abs_i16(raw_sample);
                    if (raw_abs > log_raw_channel_peak[channel_index]) {
                        log_raw_channel_peak[channel_index] = raw_abs;
                    }
#endif
                    if (channel_index == AUDIO_CAPTURE_PRIMARY_CHANNEL) {
                        primary_sum += raw_sample;
                        primary_sample_count++;
                    }
                    if (channel_index == AUDIO_CAPTURE_REFERENCE_CHANNEL) {
                        reference_sum += raw_sample;
                        reference_sample_count++;
                    }
                }
            }
            if (primary_sample_count > 0) {
                primary_sum /= (int32_t)primary_sample_count;
            }
            if (reference_sample_count > 0) {
                reference_sum /= (int32_t)reference_sample_count;
            }

            mono_buffer[frame_index] = (int16_t)primary_sum;
            reference_buffer[frame_index] = (int16_t)reference_sum;
        }

        audio_echo_cancel_process_capture_with_reference(mono_buffer,
                                                         reference_buffer,
                                                         samples_per_frame,
                                                         &echo_metrics);
        effective_upload_gain_percent = processing_config.upload_gain_percent;
        effective_auto_gain_max_percent = processing_config.auto_gain_max_percent;
        if (processing_config.far_end_gain_guard_enabled &&
            echo_metrics.reference_active && !echo_metrics.near_end_detected) {
            effective_upload_gain_percent = processing_config.far_end_upload_gain_percent;
            effective_auto_gain_max_percent =
                processing_config.far_end_auto_gain_max_percent;
        }
        base_gain_q8 = audio_capture_base_gain_q8(effective_upload_gain_percent);
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
        if (echo_metrics.active) {
            log_echo_active_frames++;
            if (echo_metrics.ref_peak > log_echo_ref_peak) {
                log_echo_ref_peak = echo_metrics.ref_peak;
            }
            if (echo_metrics.out_peak > log_echo_out_peak) {
                log_echo_out_peak = echo_metrics.out_peak;
            }
            log_echo_suppress_percent = echo_metrics.suppress_percent;
            if (echo_metrics.process_us > 0U) {
                log_echo_process_us += echo_metrics.process_us;
                log_echo_process_calls++;
                if (echo_metrics.process_us > log_echo_process_max_us) {
                    log_echo_process_max_us = echo_metrics.process_us;
                }
            }
        }
#endif

        if (!processing_config.high_pass_filter_enabled) {
            high_pass_previous_input = 0;
            high_pass_previous_output = 0;
        }
        for (size_t frame_index = 0; frame_index < samples_per_frame; ++frame_index) {
            if (processing_config.high_pass_filter_enabled) {
                mono_buffer[frame_index] =
                    audio_capture_high_pass_filter(mono_buffer[frame_index],
                                                   &high_pass_previous_input,
                                                   &high_pass_previous_output);
            }
            uint32_t pre_abs_value = audio_abs_i16(mono_buffer[frame_index]);
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            if (pre_abs_value > log_pre_gain_peak) {
                log_pre_gain_peak = pre_abs_value;
            }
#endif
            if (pre_abs_value > pre_frame_peak) {
                pre_frame_peak = pre_abs_value;
            }
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            log_pre_gain_square_sum += (uint64_t)((int64_t)mono_buffer[frame_index] *
                                                  (int64_t)mono_buffer[frame_index]);
#endif
        }

        uint32_t target_auto_gain_q8 =
            audio_capture_auto_gain_target_q8(pre_frame_peak,
                                              base_gain_q8,
                                              effective_auto_gain_max_percent);
        auto_gain_q8 = audio_capture_smooth_auto_gain_q8(auto_gain_q8, target_auto_gain_q8);
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
        log_auto_gain_q8 = auto_gain_q8;
#endif

        for (size_t frame_index = 0; frame_index < samples_per_frame; ++frame_index) {
            mono_buffer[frame_index] =
                audio_apply_capture_upload_gain(mono_buffer[frame_index], base_gain_q8, auto_gain_q8);
            uint32_t post_abs_value = audio_abs_i16(mono_buffer[frame_index]);
            if (post_abs_value > peak) {
                peak = post_abs_value;
            }
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            if (post_abs_value > log_post_gain_peak) {
                log_post_gain_peak = post_abs_value;
            }
            log_post_gain_square_sum += (uint64_t)((int64_t)mono_buffer[frame_index] *
                                                   (int64_t)mono_buffer[frame_index]);
            log_sample_count++;
#endif
        }

        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_stats.capture_frames++;
        s_audio_stats.input_level = audio_capture_peak_to_meter_percent(peak);
        s_audio_stats.capture_effective_auto_gain_max_percent =
            effective_auto_gain_max_percent;
        s_audio_stats.aec_near_end_detected = echo_metrics.near_end_detected;
        s_audio_stats.aec_ref_peak = echo_metrics.ref_peak;
        s_audio_stats.aec_mic_peak = echo_metrics.mic_peak;
        s_audio_stats.aec_out_peak = echo_metrics.out_peak;
        s_audio_stats.aec_suppress_percent = echo_metrics.suppress_percent;
        taskEXIT_CRITICAL(&s_audio_lock);

        if (capture_primary_enabled) {
            capture_cb((const uint8_t *)mono_buffer,
                       samples_per_frame * sizeof(int16_t),
                       &s_capture_format,
                       capture_cb_ctx);
        }
        for (size_t index = 0; index < observer_count; ++index) {
            observers[index].cb((const uint8_t *)mono_buffer,
                                samples_per_frame * sizeof(int16_t),
                                &s_capture_format,
                                observers[index].ctx);
        }

#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
        log_frame_count++;
        TickType_t now_tick = xTaskGetTickCount();
        if (last_level_log_tick == 0 ||
            now_tick - last_level_log_tick >= pdMS_TO_TICKS(AUDIO_CAPTURE_LEVEL_LOG_INTERVAL_MS)) {
            char pre_peak_db[16] = {0};
            char pre_rms_db[16] = {0};
            char post_peak_db[16] = {0};
            char post_rms_db[16] = {0};
            char ch0_peak_db[16] = {0};
            char ch1_peak_db[16] = {0};
            int codec_gain_x10 =
                (int)(audio_capture_gain_percent_to_db(processing_config.codec_gain_percent) *
                      10.0f + 0.5f);
            uint32_t sw_gain_x10 = (base_gain_q8 * 10U) / 256U;
            uint32_t auto_gain_x10 = (log_auto_gain_q8 * 10U) / 256U;

            audio_format_dbfs_x10(audio_peak_dbfs_x10(log_pre_gain_peak),
                                  pre_peak_db,
                                  sizeof(pre_peak_db));
            audio_format_dbfs_x10(audio_rms_dbfs_x10(log_pre_gain_square_sum, log_sample_count),
                                  pre_rms_db,
                                  sizeof(pre_rms_db));
            audio_format_dbfs_x10(audio_peak_dbfs_x10(log_post_gain_peak),
                                  post_peak_db,
                                  sizeof(post_peak_db));
            audio_format_dbfs_x10(audio_rms_dbfs_x10(log_post_gain_square_sum, log_sample_count),
                                  post_rms_db,
                                  sizeof(post_rms_db));
            audio_format_dbfs_x10(audio_peak_dbfs_x10(log_raw_channel_peak[0]),
                                  ch0_peak_db,
                                  sizeof(ch0_peak_db));
            audio_format_dbfs_x10(audio_peak_dbfs_x10(
                                      AUDIO_CAPTURE_HW_INPUT_CHANNELS > 1U ? log_raw_channel_peak[1] : 0U),
                                  ch1_peak_db,
                                  sizeof(ch1_peak_db));

            ESP_LOGI(TAG,
                     "mic capture level: frames=%lu gain=%u codec_gain=%d.%01ddB sw_gain=%lu.%lux auto_gain=%lu.%lux effective=%u/%u near=%d primary_ch=%u ch0_peak=%sdBFS ch1_peak=%sdBFS aec_frames=%lu aec_ref=%lu aec_out=%lu aec_suppress=%lu%% aec_process_avg=%luus aec_process_max=%luus pre_peak=%sdBFS pre_rms=%sdBFS post_peak=%sdBFS post_rms=%sdBFS meter=%lu",
                     (unsigned long)log_frame_count,
                     (unsigned)processing_config.send_volume_percent,
                     codec_gain_x10 / 10,
                     codec_gain_x10 % 10,
                     (unsigned long)(sw_gain_x10 / 10U),
                     (unsigned long)(sw_gain_x10 % 10U),
                     (unsigned long)(auto_gain_x10 / 10U),
                     (unsigned long)(auto_gain_x10 % 10U),
                     (unsigned)effective_upload_gain_percent,
                     (unsigned)effective_auto_gain_max_percent,
                     echo_metrics.near_end_detected ? 1 : 0,
                     (unsigned)AUDIO_CAPTURE_PRIMARY_CHANNEL,
                     ch0_peak_db,
                     ch1_peak_db,
                     (unsigned long)log_echo_active_frames,
                     (unsigned long)log_echo_ref_peak,
                     (unsigned long)log_echo_out_peak,
                     (unsigned long)log_echo_suppress_percent,
                     (unsigned long)(log_echo_process_calls > 0U ?
                                         log_echo_process_us / log_echo_process_calls : 0U),
                     (unsigned long)log_echo_process_max_us,
                     pre_peak_db,
                     pre_rms_db,
                     post_peak_db,
                     post_rms_db,
                     (unsigned long)audio_capture_peak_to_meter_percent(log_post_gain_peak));

            memset(log_raw_channel_peak, 0, sizeof(log_raw_channel_peak));
            log_pre_gain_peak = 0;
            log_post_gain_peak = 0;
            log_pre_gain_square_sum = 0;
            log_post_gain_square_sum = 0;
            log_echo_ref_peak = 0;
            log_echo_out_peak = 0;
            log_echo_active_frames = 0;
            log_echo_suppress_percent = 0;
            log_echo_process_us = 0;
            log_echo_process_max_us = 0;
            log_echo_process_calls = 0;
            log_sample_count = 0;
            log_frame_count = 0;
            last_level_log_tick = now_tick;
        }
#endif
    }
}

static void audio_tone_task(void *ctx)
{
    uint32_t tone_hz = ((uint32_t *)ctx)[0];
    uint32_t duration_ms = ((uint32_t *)ctx)[1];
    free(ctx);

    size_t frame_count = ((size_t)s_playback_format.sample_rate_hz * duration_ms) / 1000U;
    size_t sample_count = frame_count * s_playback_format.channels;
    int16_t *tone_buffer = audio_calloc_psram(sample_count, sizeof(int16_t));
    if (tone_buffer == NULL) {
        s_tone_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    for (size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        float phase = (6.2831853f * (float)tone_hz * (float)frame_index) /
                      (float)s_playback_format.sample_rate_hz;
        int16_t sample = (int16_t)(sinf(phase) * 14000.0f);
        for (uint8_t channel = 0; channel < s_playback_format.channels; ++channel) {
            tone_buffer[frame_index * s_playback_format.channels + channel] = sample;
        }
    }

    audio_format_t tone_format = s_playback_format;
    audio_play_pcm_frame_with_format((const uint8_t *)tone_buffer,
                                              sample_count * sizeof(int16_t),
                                              &tone_format);
    audio_stop_playback();

    free(tone_buffer);
    s_tone_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t audio_prepare_output(void)
{
    const TickType_t retry_delay = pdMS_TO_TICKS(200);

    while (true) {
        TickType_t now = xTaskGetTickCount();

        taskENTER_CRITICAL(&s_audio_lock);
        if (s_audio_output_ready) {
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
        if (!s_audio_preparing) {
            if (s_audio_output_prepare_last_err != ESP_OK && now < s_audio_output_prepare_retry_after_ticks) {
                esp_err_t last_err = s_audio_output_prepare_last_err;
                taskEXIT_CRITICAL(&s_audio_lock);
                return last_err;
            }
            s_audio_preparing = true;
            s_audio_output_preparing = true;
            taskEXIT_CRITICAL(&s_audio_lock);
            break;
        }
        taskEXIT_CRITICAL(&s_audio_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t ret = audio_do_prepare_output();
    if (ret == ESP_OK) {
        s_audio_output_ready = true;
        audio_update_ready_state();
        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_output_prepare_last_err = ESP_OK;
        s_audio_output_prepare_retry_after_ticks = 0;
        s_audio_output_preparing = false;
        s_audio_preparing = false;
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_OK;
    }

    audio_cleanup_output_prepare_failure();
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_output_prepare_last_err = ret;
    s_audio_output_prepare_retry_after_ticks = xTaskGetTickCount() + retry_delay;
    s_audio_output_preparing = false;
    s_audio_preparing = false;
    taskEXIT_CRITICAL(&s_audio_lock);
    return ret;
}

static esp_err_t audio_prepare_input(void)
{
    const TickType_t retry_delay = pdMS_TO_TICKS(200);

    while (true) {
        TickType_t now = xTaskGetTickCount();

        taskENTER_CRITICAL(&s_audio_lock);
        if (s_audio_input_ready) {
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
        if (!s_audio_preparing) {
            if (s_audio_input_prepare_last_err != ESP_OK && now < s_audio_input_prepare_retry_after_ticks) {
                esp_err_t last_err = s_audio_input_prepare_last_err;
                taskEXIT_CRITICAL(&s_audio_lock);
                return last_err;
            }
            s_audio_preparing = true;
            s_audio_input_preparing = true;
            taskEXIT_CRITICAL(&s_audio_lock);
            break;
        }
        taskEXIT_CRITICAL(&s_audio_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t ret = audio_do_prepare_input();
    if (ret == ESP_OK) {
        s_audio_input_ready = true;
        audio_update_ready_state();
        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_input_prepare_last_err = ESP_OK;
        s_audio_input_prepare_retry_after_ticks = 0;
        s_audio_input_preparing = false;
        s_audio_preparing = false;
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_OK;
    }

    audio_cleanup_input_prepare_failure();
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_input_prepare_last_err = ret;
    s_audio_input_prepare_retry_after_ticks = xTaskGetTickCount() + retry_delay;
    s_audio_input_preparing = false;
    s_audio_preparing = false;
    taskEXIT_CRITICAL(&s_audio_lock);
    return ret;
}

esp_err_t audio_prepare(void)
{
    esp_err_t input_ret = audio_prepare_input();
    esp_err_t output_ret = audio_prepare_output();

    if (input_ret == ESP_OK && output_ret == ESP_OK) {
        return ESP_OK;
    }

    if (input_ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker output not ready, microphone capture remains available: %s",
                 esp_err_to_name(output_ret));
        return ESP_OK;
    }

    if (output_ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "microphone capture not ready, speaker output remains available: %s",
                 esp_err_to_name(input_ret));
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "audio prepare failed: microphone=%s speaker=%s",
             esp_err_to_name(input_ret),
             esp_err_to_name(output_ret));
    return input_ret;
}

esp_err_t audio_prepare_input_path(void)
{
    return audio_prepare_input();
}

void audio_release(void)
{
    bool retained_input = false;
    bool retained_output = false;
    TaskHandle_t capture_task = NULL;

    audio_log_heap("audio suspend begin");
    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_cb = NULL;
    s_capture_cb_ctx = NULL;
    s_capture_primary_enabled = false;
    s_capture_task_stop_requested = false;
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        s_capture_observers[index].enabled = false;
    }
    s_audio_stats.capture_enabled = false;
    s_audio_stats.input_level = 0;
    retained_input = s_audio_input_ready;
    retained_output = s_audio_output_ready;
    capture_task = s_capture_task;
    taskEXIT_CRITICAL(&s_audio_lock);

    audio_stop_playback();
    /*
     * Keep the ESP-SR AEC allocation warm across app switches. Recreating it
     * on every call adds first-speech latency and makes a long-running media
     * system repeatedly allocate the same realtime working set.
     */
    esp_err_t aec_ret = audio_echo_cancel_set_active(false);
    if (aec_ret != ESP_OK) {
        ESP_LOGW(TAG, "suspend AEC failed: %s", esp_err_to_name(aec_ret));
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_input_prepare_last_err = ESP_OK;
    s_audio_input_prepare_retry_after_ticks = 0;
    s_audio_output_prepare_last_err = ESP_OK;
    s_audio_output_prepare_retry_after_ticks = 0;
    s_audio_preparing = false;
    s_audio_output_preparing = false;
    s_audio_input_preparing = false;
    s_speaker_path_enabled = false;
    memset(&s_last_playback_timing, 0, sizeof(s_last_playback_timing));
    taskEXIT_CRITICAL(&s_audio_lock);

    audio_update_ready_state();
    ESP_LOGI(TAG,
             "audio suspend done: retained_input=%u retained_output=%u capture_task=%p",
             retained_input ? 1U : 0U,
             retained_output ? 1U : 0U,
             capture_task);
    audio_log_heap("audio suspend done");
}

const audio_format_t *audio_get_format(void)
{
    return &s_capture_format;
}

const audio_format_t *audio_get_playback_format(void)
{
    return &s_playback_format;
}

void audio_set_capture_frame_cb(audio_capture_frame_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_cb = cb;
    s_capture_cb_ctx = ctx;
    s_audio_stats.capture_enabled = s_audio_input_ready &&
                                    audio_capture_has_active_consumer_locked();
    taskEXIT_CRITICAL(&s_audio_lock);
}

esp_err_t audio_register_capture_observer(audio_capture_frame_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "capture observer callback is null");

    taskENTER_CRITICAL(&s_audio_lock);
    int free_slot = -1;
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].cb == cb && s_capture_observers[index].ctx == ctx) {
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
        if (free_slot < 0 && s_capture_observers[index].cb == NULL) {
            free_slot = (int)index;
        }
    }
    if (free_slot < 0) {
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_ERR_NO_MEM;
    }
    s_capture_observers[free_slot] = (audio_capture_observer_t){
        .cb = cb,
        .ctx = ctx,
        .enabled = false,
    };
    s_audio_stats.capture_enabled = s_audio_input_ready &&
                                    audio_capture_has_active_consumer_locked();
    taskEXIT_CRITICAL(&s_audio_lock);
    return ESP_OK;
}

void audio_unregister_capture_observer(audio_capture_frame_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_audio_lock);
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].cb == cb && s_capture_observers[index].ctx == ctx) {
            memset(&s_capture_observers[index], 0, sizeof(s_capture_observers[index]));
            break;
        }
    }
    s_audio_stats.capture_enabled = s_audio_input_ready &&
                                    audio_capture_has_active_consumer_locked();
    if (!s_audio_stats.capture_enabled) {
        s_audio_stats.input_level = 0;
    }
    taskEXIT_CRITICAL(&s_audio_lock);
}

esp_err_t audio_set_capture_observer_enabled(audio_capture_frame_cb_t cb, void *ctx, bool enabled)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "capture observer callback is null");

    if (enabled && !s_audio_input_ready) {
        ESP_RETURN_ON_ERROR(audio_prepare_input(), TAG, "audio input prepare failed");
    }

    taskENTER_CRITICAL(&s_audio_lock);
    for (size_t index = 0; index < AUDIO_CAPTURE_OBSERVER_MAX; ++index) {
        if (s_capture_observers[index].cb == cb && s_capture_observers[index].ctx == ctx) {
            s_capture_observers[index].enabled = enabled;
            s_audio_stats.capture_enabled = s_audio_input_ready &&
                                            audio_capture_has_active_consumer_locked();
            if (!s_audio_stats.capture_enabled) {
                s_audio_stats.input_level = 0;
            }
            taskEXIT_CRITICAL(&s_audio_lock);
            return ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_audio_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_set_capture_enabled(bool enabled)
{
    if (!s_audio_input_ready && enabled) {
        ESP_RETURN_ON_ERROR(audio_prepare_input(), TAG, "audio input prepare failed");
    }

    if (!s_audio_input_ready) {
        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_stats.capture_enabled = false;
        s_audio_stats.input_level = 0;
        taskEXIT_CRITICAL(&s_audio_lock);
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_primary_enabled = enabled;
    s_audio_stats.capture_enabled = audio_capture_has_active_consumer_locked();
    if (!s_audio_stats.capture_enabled) {
        s_audio_stats.input_level = 0;
    }
    taskEXIT_CRITICAL(&s_audio_lock);
    return ESP_OK;
}

esp_err_t audio_set_speaker_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_speaker_volume_percent = percent;
    s_audio_stats.speaker_volume_percent = percent;
    if (percent > 0U) {
        s_playback_muted_logged = false;
    }
    taskEXIT_CRITICAL(&s_audio_lock);

    if (!s_audio_output_ready) {
        return ESP_OK;
    }
    return esp_codec_dev_set_out_vol(s_play_dev_handle, percent);
}

esp_err_t audio_set_capture_gain_percent(uint8_t percent)
{
    audio_capture_processing_config_t config =
        audio_capture_make_default_processing_config(percent);
    return audio_set_capture_processing_config(&config);
}

esp_err_t audio_set_capture_processing_config(
    const audio_capture_processing_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "capture processing config is required");
    audio_capture_processing_config_t sanitized =
        audio_capture_sanitize_processing_config(config);

    taskENTER_CRITICAL(&s_audio_lock);
    s_capture_processing_config = sanitized;
    s_audio_stats.capture_gain_percent = sanitized.send_volume_percent;
    s_audio_stats.capture_codec_gain_percent = sanitized.codec_gain_percent;
    s_audio_stats.capture_upload_gain_percent = sanitized.upload_gain_percent;
    s_audio_stats.capture_auto_gain_max_percent = sanitized.auto_gain_max_percent;
    s_audio_stats.capture_effective_auto_gain_max_percent =
        sanitized.auto_gain_max_percent;
    s_audio_stats.far_end_gain_guard_enabled = sanitized.far_end_gain_guard_enabled;
    s_audio_stats.far_end_upload_gain_percent = sanitized.far_end_upload_gain_percent;
    s_audio_stats.far_end_auto_gain_max_percent = sanitized.far_end_auto_gain_max_percent;
    s_audio_stats.echo_suppression = sanitized.echo_suppression;
    s_audio_stats.capture_high_pass_filter_enabled =
        sanitized.high_pass_filter_enabled;
    taskEXIT_CRITICAL(&s_audio_lock);

    audio_echo_cancel_set_suppression(sanitized.echo_suppression);
    int codec_gain_x10 =
        (int)(audio_capture_gain_percent_to_db(sanitized.codec_gain_percent) * 10.0f + 0.5f);
    APP_LOG_DETAIL(TAG,
                   "capture profile: send=%u codec=%u(%d.%01ddB) upload=%u auto_max=%u far_guard=%d/%u/%u aec=%s hpf=%d",
                   (unsigned)sanitized.send_volume_percent,
                   (unsigned)sanitized.codec_gain_percent,
                   codec_gain_x10 / 10,
                   codec_gain_x10 % 10,
                   (unsigned)sanitized.upload_gain_percent,
                   (unsigned)sanitized.auto_gain_max_percent,
                   sanitized.far_end_gain_guard_enabled ? 1 : 0,
                   (unsigned)sanitized.far_end_upload_gain_percent,
                   (unsigned)sanitized.far_end_auto_gain_max_percent,
                   sanitized.echo_suppression == AUDIO_ECHO_SUPPRESSION_STRONG ?
                       "strong" : "balanced",
                   sanitized.high_pass_filter_enabled ? 1 : 0);

    if (!s_audio_input_ready) {
        return ESP_OK;
    }
    return esp_codec_dev_set_in_gain(s_record_dev_handle,
                                      audio_capture_gain_percent_to_db(
                                          sanitized.codec_gain_percent));
}

esp_err_t audio_prepare_playback_path(void)
{
    esp_err_t ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_prepare_output();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "audio output prepare failed: %s", esp_err_to_name(ret));
        audio_give_playback_mutex();
        return ret;
    }

    if (!s_speaker_path_enabled) {
        ret = esp_codec_dev_set_out_mute(s_play_dev_handle, false);
        if (ret != ESP_OK) {
            audio_give_playback_mutex();
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_SPEAKER_POWER_SETTLE_MS));
        ret = hardware_board_set_audio_power(true);
        if (ret != ESP_OK) {
            (void)esp_codec_dev_set_out_mute(s_play_dev_handle, true);
            ESP_LOGW(TAG, "speaker power on failed: %s", esp_err_to_name(ret));
            audio_give_playback_mutex();
            return ret;
        }
        s_speaker_path_enabled = true;
        s_playback_path_ready_logged = false;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_stats.speaker_enabled = true;
    taskEXIT_CRITICAL(&s_audio_lock);
    if (!s_playback_path_ready_logged) {
        s_playback_path_ready_logged = true;
        ESP_LOGI(TAG,
                 "speaker playback path ready: rate=%luHz channels=%u volume=%u",
                 (unsigned long)s_playback_format.sample_rate_hz,
                 (unsigned)s_playback_format.channels,
                 (unsigned)s_speaker_volume_percent);
    }
    audio_give_playback_mutex();
    return ESP_OK;
}

esp_err_t audio_play_pcm_frame_with_format(const uint8_t *data,
                                                     size_t data_len,
                                                     const audio_format_t *format)
{
    int16_t *output_samples = NULL;
    size_t output_bytes = 0;
    uint32_t output_level = 0;
    esp_err_t ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_render_playback_pcm(data,
                                    data_len,
                                    format,
                                    &output_samples,
                                    &output_bytes,
                                    &output_level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "render playback pcm failed: %s", esp_err_to_name(ret));
        audio_give_playback_mutex();
        return ret;
    }

    ret = audio_write_rendered_playback(output_samples, output_bytes, output_level);
    audio_give_playback_mutex();
    return ret;
}

esp_err_t audio_render_playback_pcm(const uint8_t *data,
                                              size_t data_len,
                                              const audio_format_t *format,
                                              int16_t **output_data,
                                              size_t *output_bytes,
                                              uint32_t *output_level)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(data != NULL && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid pcm input");
    ESP_RETURN_ON_FALSE(output_data != NULL && output_bytes != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid pcm output");
    *output_data = NULL;
    *output_bytes = 0;

    ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    if (format->bits_per_sample != 16) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "only 16-bit pcm is supported");
        goto out;
    }
    if (format->channels != 1 && format->channels != 2) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "unsupported channel count");
        goto out;
    }
    if (format->sample_rate_hz != 8000 && format->sample_rate_hz != 16000) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "unsupported sample rate");
        goto out;
    }

    size_t input_frames = data_len / (sizeof(int16_t) * format->channels);
    if (input_frames == 0 || data_len != input_frames * sizeof(int16_t) * format->channels) {
        ret = ESP_ERR_INVALID_SIZE;
        ESP_LOGW(TAG, "invalid playback pcm length: bytes=%u ch=%u", (unsigned)data_len, format->channels);
        goto out;
    }
    if (s_playback_format.sample_rate_hz < format->sample_rate_hz) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "playback downsample is not supported");
        goto out;
    }
    if ((s_playback_format.sample_rate_hz % format->sample_rate_hz) != 0) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "playback sample rate must be an integer multiple of input");
        goto out;
    }
    size_t upsample_ratio = s_playback_format.sample_rate_hz / format->sample_rate_hz;
    size_t output_frames = input_frames * upsample_ratio;
    size_t rendered_bytes = output_frames * s_playback_format.channels * sizeof(int16_t);
    ret = audio_ensure_scratch(rendered_bytes);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "playback scratch alloc failed: %s", esp_err_to_name(ret));
        goto out;
    }

    const int16_t *input_samples = (const int16_t *)data;
    int16_t *output_samples = (int16_t *)s_playback_scratch;
    size_t out_index = 0;
    uint32_t playback_peak = 0;
    for (size_t frame_index = 0; frame_index < input_frames; ++frame_index) {
        int16_t left = input_samples[frame_index * format->channels];
        int16_t right = format->channels == 2 ?
                            input_samples[frame_index * format->channels + 1] : left;
        int32_t mono = ((int32_t)left + (int32_t)right) / 2;
        size_t next_frame = frame_index + 1U < input_frames ? frame_index + 1U : frame_index;
        int16_t next_left = input_samples[next_frame * format->channels];
        int16_t next_right = format->channels == 2 ?
                                 input_samples[next_frame * format->channels + 1] : next_left;
        int32_t next_mono = ((int32_t)next_left + (int32_t)next_right) / 2;
        for (size_t repeat_index = 0; repeat_index < upsample_ratio; ++repeat_index) {
            int32_t interpolated = mono;
            if (upsample_ratio > 1U) {
                interpolated += ((next_mono - mono) * (int32_t)repeat_index) /
                                (int32_t)upsample_ratio;
            }
            int16_t rendered = audio_clip_i16(interpolated);
            uint32_t rendered_abs = audio_abs_i16(rendered);
            if (rendered_abs > playback_peak) {
                playback_peak = rendered_abs;
            }
            for (uint8_t channel = 0; channel < s_playback_format.channels; ++channel) {
                output_samples[out_index++] = rendered;
            }
        }
    }

    *output_data = output_samples;
    *output_bytes = rendered_bytes;
    if (output_level != NULL) {
        *output_level = (playback_peak * 100U) / 32767U;
    }

out:
    audio_give_playback_mutex();
    return ret;
}

esp_err_t audio_write_rendered_playback(int16_t *data,
                                                   size_t data_len,
                                                   uint32_t output_level)
{
    int64_t prepare_start_us = 0;
    int64_t write_start_us = 0;
    uint32_t prepare_ms = 0;
    uint32_t write_ms = 0;
    esp_err_t ret = ESP_OK;
    bool playback_path_ready = false;

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid rendered pcm input");

    ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t speaker_volume_percent = 0;
    bool log_muted_playback = false;
    taskENTER_CRITICAL(&s_audio_lock);
    speaker_volume_percent = audio_get_speaker_volume_percent_locked();
    if (speaker_volume_percent == 0U && !s_playback_muted_logged) {
        s_playback_muted_logged = true;
        log_muted_playback = true;
    }
    taskEXIT_CRITICAL(&s_audio_lock);

    if (speaker_volume_percent == 0U) {
        if (log_muted_playback) {
            ESP_LOGW(TAG, "speaker volume is 0: remote playback muted");
        }
        audio_mute_playback_path_no_mutex();

        taskENTER_CRITICAL(&s_audio_lock);
        s_audio_stats.speaker_enabled = false;
        s_audio_stats.output_level = 0;
        memset(&s_last_playback_timing, 0, sizeof(s_last_playback_timing));
        s_last_playback_timing.data_bytes = (uint32_t)data_len;
        taskEXIT_CRITICAL(&s_audio_lock);
        goto out;
    }

    if (!s_audio_output_ready) {
        ret = audio_prepare_output();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio output prepare failed: %s", esp_err_to_name(ret));
            goto out;
        }
    }

    taskENTER_CRITICAL(&s_audio_lock);
    playback_path_ready = s_speaker_path_enabled;
    taskEXIT_CRITICAL(&s_audio_lock);
    if (!playback_path_ready) {
        prepare_start_us = esp_timer_get_time();
        ret = audio_prepare_playback_path();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "prepare playback path failed: %s", esp_err_to_name(ret));
            goto out;
        }
        prepare_ms = (uint32_t)((esp_timer_get_time() - prepare_start_us) / 1000ULL);
    }

    output_level = (uint32_t)(((uint64_t)(output_level > 100U ? 100U : output_level) *
                               speaker_volume_percent) /
                              100U);

    /*
     * Queue the far-end reference before handing the same PCM to I2S. Feeding
     * it after a blocking DMA write makes the reference trail the acoustic
     * output and prevents the fixed-delay AEC path from converging reliably.
     */
    audio_echo_cancel_feed_playback(data,
                                    data_len / sizeof(int16_t),
                                    s_playback_format.channels);

    write_start_us = esp_timer_get_time();
    ret = esp_codec_dev_write(s_play_dev_handle, (void *)data, (int)data_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "speaker write failed: %s", esp_err_to_name(ret));
        goto out;
    }
    write_ms = (uint32_t)((esp_timer_get_time() - write_start_us) / 1000ULL);

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_stats.speaker_enabled = true;
    s_audio_stats.output_level = output_level > 100U ? 100U : output_level;
    s_last_playback_timing.prepare_ms = prepare_ms;
    s_last_playback_timing.write_ms = write_ms;
    s_last_playback_timing.data_bytes = (uint32_t)data_len;
    taskEXIT_CRITICAL(&s_audio_lock);

    TickType_t now = xTaskGetTickCount();
    if (!s_playback_write_logged) {
        s_playback_write_logged = true;
        s_last_playback_write_log_tick = now;
        ESP_LOGI(TAG,
                 "speaker write committed: bytes=%u level=%u volume=%u prepare_ms=%lu write_ms=%lu path_enabled=%d output_ready=%d",
                 (unsigned)data_len,
                 (unsigned)output_level,
                 (unsigned)speaker_volume_percent,
                 (unsigned long)prepare_ms,
                 (unsigned long)write_ms,
                 s_speaker_path_enabled,
                 s_audio_output_ready);
    } else if (s_last_playback_write_log_tick == 0 ||
               now - s_last_playback_write_log_tick >= pdMS_TO_TICKS(1000)) {
        s_last_playback_write_log_tick = now;
        ESP_LOGD(TAG,
                 "speaker write steady: bytes=%u level=%u volume=%u prepare_ms=%lu write_ms=%lu path_enabled=%d output_ready=%d",
                 (unsigned)data_len,
                 (unsigned)output_level,
                 (unsigned)speaker_volume_percent,
                 (unsigned long)prepare_ms,
                 (unsigned long)write_ms,
                 s_speaker_path_enabled,
                 s_audio_output_ready);
    }
out:
    audio_give_playback_mutex();
    return ret;
}

void audio_get_last_playback_timing(audio_playback_timing_t *timing)
{
    if (timing == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    *timing = s_last_playback_timing;
    taskEXIT_CRITICAL(&s_audio_lock);
}

void audio_stop_playback(void)
{
    esp_err_t ret = audio_take_playback_mutex(portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "audio stop playback skipped: %s", esp_err_to_name(ret));
        return;
    }

    if (!s_audio_output_ready) {
        audio_give_playback_mutex();
        return;
    }

    if (s_speaker_path_enabled) {
        esp_codec_dev_set_out_mute(s_play_dev_handle, true);
        hardware_board_set_audio_power(false);
        s_speaker_path_enabled = false;
        s_playback_path_ready_logged = false;
    }

    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_stats.speaker_enabled = false;
    s_audio_stats.output_level = 0;
    taskEXIT_CRITICAL(&s_audio_lock);
    s_playback_write_logged = false;
    s_last_playback_write_log_tick = 0;
    audio_give_playback_mutex();
}

esp_err_t audio_play_test_tone(uint32_t tone_hz, uint32_t duration_ms)
{
    if (!s_audio_output_ready) {
        ESP_RETURN_ON_ERROR(audio_prepare_output(), TAG, "audio output prepare failed");
    }
    if (s_tone_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t *args = audio_calloc_psram(2, sizeof(uint32_t));
    if (args == NULL) {
        return ESP_ERR_NO_MEM;
    }
    args[0] = tone_hz;
    args[1] = duration_ms;

    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(audio_tone_task,
                                                         "audio_tone",
                                                         4 * 1024,
                                                         args,
                                                         5,
                                                         &s_tone_task,
                                                         APP_TASK_CORE_AUDIO,
                                                         APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_ok != pdPASS) {
        free(args);
        s_tone_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_get_stats(audio_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    audio_echo_cancel_status_t aec = {0};
    taskENTER_CRITICAL(&s_audio_lock);
    *stats = s_audio_stats;
    taskEXIT_CRITICAL(&s_audio_lock);

    audio_echo_cancel_get_status(&aec);
    stats->aec_active = aec.active;
    stats->aec_reference_active = aec.reference_active;
    stats->aec_process_frames = aec.process_frames;
    stats->aec_process_us_total = aec.process_us_total;
    stats->aec_process_us_max = aec.process_us_max;
}
