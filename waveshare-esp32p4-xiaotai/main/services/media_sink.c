#include "media_sink.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "audio_playout_controller.h"
#include "audio_device.h"
#include "platform/app_log_policy.h"
#include "platform/app_task_affinity.h"

static const char *TAG = "media_sink";

#define MEDIA_SINK_AUDIO_QUEUE_LEN 32
#define MEDIA_SINK_AUDIO_TASK_STACK (5 * 1024)
/* Speaker playout is deadline-driven: one late 20ms packet is audible and
 * cannot be recovered by a deeper queue. Keep it ahead of the decode caller;
 * the TinyH264 helper shares this priority so neither can starve the other. */
#define MEDIA_SINK_AUDIO_TASK_PRIORITY APP_TASK_PRIORITY_AUDIO_PLAYBACK
#define MEDIA_SINK_AUDIO_TASK_CORE APP_TASK_CORE_AUDIO
#define MEDIA_SINK_AUDIO_PLAY_CHUNK_MS 20
#define MEDIA_SINK_AUDIO_PCM_BUFFER_MS 1600
#define MEDIA_SINK_AUDIO_RESAMPLE_INPUT_MS 22
#define MEDIA_SINK_AUDIO_SLOW_PLAY_US 25000
#define MEDIA_SINK_AUDIO_CONTROL_WINDOW_MS 1000
#define MEDIA_SINK_AUDIO_WARNING_INTERVAL_MS 10000
#define MEDIA_SINK_AUDIO_UNDERFLOW_WARNING_COUNT 10
#define MEDIA_SINK_AUDIO_TRIM_KEEP_MARGIN_MS 80
#define MEDIA_SINK_AUDIO_CALL_TRIM_KEEP_MARGIN_MS 200

typedef struct {
    uint32_t backlog_trim_threshold;
    uint32_t backlog_target_packets;
    uint8_t drain_burst_max;
} media_sink_audio_tuning_t;

typedef struct {
    uint32_t generation;
    audio_format_t format;
    uint8_t *data;
    size_t data_len;
    uint32_t source_timestamp_ms;
    uint32_t arrival_ms;
} media_sink_audio_packet_t;

static QueueHandle_t s_audio_queue;
static TaskHandle_t s_audio_task;
static bool s_initialized;
static bool s_remote_audio_playback_started_logged;
static bool s_remote_audio_enqueue_logged;
static bool s_remote_audio_render_logged;
static bool s_remote_audio_buffering_logged;
static portMUX_TYPE s_sink_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_generation;
static TickType_t s_last_audio_trim_log_tick;
static TickType_t s_last_audio_enqueue_log_tick;
static TickType_t s_last_audio_render_log_tick;
static TickType_t s_last_audio_slow_log_tick;
static TickType_t s_last_audio_write_drop_log_tick;
static TickType_t s_last_audio_rate_log_tick;
static TickType_t s_audio_warning_window_tick;
static TickType_t s_last_audio_warning_tick;
static uint32_t s_audio_rx_packets_in_window;
static uint32_t s_audio_rx_ms_in_window;
static uint32_t s_audio_play_ok_packets_in_window;
static uint32_t s_audio_play_ok_ms_in_window;
static uint32_t s_audio_play_drop_packets_in_window;
static uint32_t s_audio_play_drop_ms_in_window;
static uint32_t s_audio_trim_drop_packets_in_window;
static uint32_t s_audio_trim_drop_ms_in_window;
static uint32_t s_audio_underflows_in_window;
static uint32_t s_audio_accelerated_chunks_in_window;
static uint32_t s_audio_expanded_chunks_in_window;
static uint32_t s_audio_warning_rx_packets;
static uint32_t s_audio_warning_rx_ms;
static uint32_t s_audio_warning_play_packets;
static uint32_t s_audio_warning_play_ms;
static uint32_t s_audio_warning_write_drop_packets;
static uint32_t s_audio_warning_write_drop_ms;
static uint32_t s_audio_warning_trim_drop_packets;
static uint32_t s_audio_warning_trim_drop_ms;
static uint32_t s_audio_warning_underflows;
static uint32_t s_audio_warning_accelerated_chunks;
static uint32_t s_audio_warning_expanded_chunks;
static audio_playout_condition_t s_audio_last_condition =
    AUDIO_PLAYOUT_CONDITION_STARTUP;
static uint32_t s_audio_last_source_packet_ms;
static uint8_t *s_audio_pcm_buffer;
static size_t s_audio_pcm_buffer_size;
static size_t s_audio_pcm_read_offset;
static size_t s_audio_pcm_used_bytes;
static uint8_t *s_audio_pcm_chunk_buffer;
static size_t s_audio_pcm_chunk_buffer_size;
static uint8_t *s_audio_pcm_resample_buffer;
static size_t s_audio_pcm_resample_buffer_size;
static media_sink_audio_profile_t s_audio_profile = MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;
static audio_playout_controller_t s_audio_controller;

static const media_sink_audio_tuning_t s_audio_tunings[] = {
    [MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY] = {
        .backlog_trim_threshold = 12,
        .backlog_target_packets = 4,
        .drain_burst_max = 4,
    },
    [MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL] = {
        /* TGTRP recovery can deliver 16 or more 20 ms packets in one burst.
         * Move that burst into the PSRAM PCM ring instead of deleting half of
         * it from the bounded internal-RAM queue. Keep trimming only as the
         * final guard before the 32-slot queue is exhausted. */
        .backlog_trim_threshold = 30,
        .backlog_target_packets = 24,
        .drain_burst_max = 16,
    },
    [MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE] = {
        .backlog_trim_threshold = 24,
        .backlog_target_packets = 12,
        .drain_burst_max = 8,
    },
};

static void media_sink_audio_task(void *ctx);

static bool media_sink_audio_profile_valid(media_sink_audio_profile_t profile)
{
    return profile == MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY ||
           profile == MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL ||
           profile == MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE;
}

static const char *media_sink_audio_profile_name(media_sink_audio_profile_t profile)
{
    switch (profile) {
    case MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY:
        return "low_latency";
    case MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL:
        return "adaptive_call";
    case MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE:
        return "jitter_safe";
    default:
        return "unknown";
    }
}

static media_sink_audio_profile_t media_sink_audio_get_profile(void)
{
    media_sink_audio_profile_t profile = MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;

    taskENTER_CRITICAL(&s_sink_lock);
    profile = s_audio_profile;
    taskEXIT_CRITICAL(&s_sink_lock);

    return media_sink_audio_profile_valid(profile) ?
           profile : MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;
}

static media_sink_audio_tuning_t media_sink_audio_get_tuning(void)
{
    return s_audio_tunings[media_sink_audio_get_profile()];
}

static audio_playout_profile_t media_sink_audio_controller_profile(
    media_sink_audio_profile_t profile)
{
    switch (profile) {
    case MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL:
        return AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL;
    case MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE:
        return AUDIO_PLAYOUT_PROFILE_JITTER_SAFE;
    case MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY:
    default:
        return AUDIO_PLAYOUT_PROFILE_LOW_LATENCY;
    }
}

static uint32_t media_sink_audio_profile_base_delay_ms(media_sink_audio_profile_t profile)
{
    switch (profile) {
    case MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE:
        return 160U;
    case MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL:
        return 140U;
    case MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY:
    default:
        return 40U;
    }
}

static uint32_t media_sink_audio_profile_trim_keep_margin_ms(
    media_sink_audio_profile_t profile)
{
    return profile == MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL ?
           MEDIA_SINK_AUDIO_CALL_TRIM_KEEP_MARGIN_MS :
           MEDIA_SINK_AUDIO_TRIM_KEEP_MARGIN_MS;
}

static void media_sink_audio_get_controller_snapshot(audio_playout_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    audio_playout_controller_get_snapshot(&s_audio_controller, snapshot);
    taskEXIT_CRITICAL(&s_sink_lock);
}

static size_t media_sink_audio_frame_bytes(const audio_format_t *format)
{
    if (format == NULL || format->bits_per_sample == 0 || format->channels == 0) {
        return 0;
    }

    return ((size_t)format->bits_per_sample / 8U) * format->channels;
}

static size_t media_sink_audio_bytes_for_duration_ms(uint32_t duration_ms,
                                                              const audio_format_t *format)
{
    size_t frame_bytes = media_sink_audio_frame_bytes(format);

    if (frame_bytes == 0 || format == NULL || format->sample_rate_hz == 0) {
        return 0;
    }

    return (size_t)(((uint64_t)format->sample_rate_hz * duration_ms * frame_bytes) / 1000ULL);
}

static uint32_t media_sink_audio_duration_ms_for_bytes(size_t bytes,
                                                                 const audio_format_t *format)
{
    size_t frame_bytes = media_sink_audio_frame_bytes(format);
    size_t bytes_per_second = 0;

    if (frame_bytes == 0 || format == NULL || format->sample_rate_hz == 0) {
        return 0;
    }

    bytes_per_second = (size_t)format->sample_rate_hz * frame_bytes;
    if (bytes_per_second == 0) {
        return 0;
    }

    return (uint32_t)(((uint64_t)bytes * 1000ULL) / bytes_per_second);
}

static uint32_t media_sink_get_last_source_packet_ms(void)
{
    uint32_t source_packet_ms = 0;

    taskENTER_CRITICAL(&s_sink_lock);
    source_packet_ms = s_audio_last_source_packet_ms;
    taskEXIT_CRITICAL(&s_sink_lock);
    return source_packet_ms;
}

static void media_sink_audio_note_underflow(void)
{
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_underflows_in_window++;
    audio_playout_controller_note_underflow(&s_audio_controller);
    taskEXIT_CRITICAL(&s_sink_lock);
}

static uint8_t *media_sink_alloc_audio_buffer(size_t size)
{
    return app_memory_alloc_psram(size);
}

static esp_err_t media_sink_ensure_audio_playback_buffers(void)
{
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t pcm_buffer_size =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PCM_BUFFER_MS,
                                                        playback_format);
    const size_t chunk_buffer_size =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                                        playback_format);
    const size_t resample_buffer_size =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_RESAMPLE_INPUT_MS,
                                               playback_format);

    if (pcm_buffer_size == 0 || chunk_buffer_size == 0 || resample_buffer_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_audio_pcm_buffer == NULL) {
        s_audio_pcm_buffer = media_sink_alloc_audio_buffer(pcm_buffer_size);
        ESP_RETURN_ON_FALSE(s_audio_pcm_buffer != NULL, ESP_ERR_NO_MEM, TAG, "audio pcm buffer alloc failed");
        s_audio_pcm_buffer_size = pcm_buffer_size;
    }

    if (s_audio_pcm_chunk_buffer == NULL) {
        s_audio_pcm_chunk_buffer = media_sink_alloc_audio_buffer(chunk_buffer_size);
        ESP_RETURN_ON_FALSE(s_audio_pcm_chunk_buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "audio chunk buffer alloc failed");
        s_audio_pcm_chunk_buffer_size = chunk_buffer_size;
    }

    if (s_audio_pcm_resample_buffer == NULL) {
        s_audio_pcm_resample_buffer = media_sink_alloc_audio_buffer(resample_buffer_size);
        ESP_RETURN_ON_FALSE(s_audio_pcm_resample_buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "audio resample buffer alloc failed");
        s_audio_pcm_resample_buffer_size = resample_buffer_size;
    }

    return ESP_OK;
}

static void media_sink_reset_audio_pcm_buffer(void)
{
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_pcm_read_offset = 0;
    s_audio_pcm_used_bytes = 0;
    taskEXIT_CRITICAL(&s_sink_lock);
}

static size_t media_sink_get_audio_pcm_used_bytes(void)
{
    size_t used_bytes = 0;

    taskENTER_CRITICAL(&s_sink_lock);
    used_bytes = s_audio_pcm_used_bytes;
    taskEXIT_CRITICAL(&s_sink_lock);
    return used_bytes;
}

static void media_sink_note_trimmed_audio_ms(uint32_t dropped_ms, uint32_t buffered_ms)
{
    TickType_t now = xTaskGetTickCount();
    bool should_log = false;

    if (dropped_ms == 0) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_trim_drop_ms_in_window += dropped_ms;
    s_audio_trim_drop_packets_in_window +=
        (dropped_ms + MEDIA_SINK_AUDIO_PLAY_CHUNK_MS - 1U) / MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
    if (dropped_ms > MEDIA_SINK_AUDIO_PLAY_CHUNK_MS &&
        (s_last_audio_trim_log_tick == 0 ||
         now - s_last_audio_trim_log_tick >= pdMS_TO_TICKS(1000))) {
        s_last_audio_trim_log_tick = now;
        should_log = true;
    }
    taskEXIT_CRITICAL(&s_sink_lock);

    if (should_log) {
        APP_LOG_DETAIL(TAG,
                       "remote audio buffer trimmed: dropped_ms=%u buffered_ms=%u queued=%u",
                       (unsigned)dropped_ms,
                       (unsigned)buffered_ms,
                       (unsigned)uxQueueMessagesWaiting(s_audio_queue));
    }
}

static uint32_t media_sink_drop_audio_pcm_head(size_t drop_bytes)
{
    const audio_format_t *playback_format = speaker_get_playback_format();

    if (playback_format == NULL || s_audio_pcm_buffer_size == 0 || s_audio_pcm_used_bytes == 0 || drop_bytes == 0) {
        return 0;
    }

    if (drop_bytes > s_audio_pcm_used_bytes) {
        drop_bytes = s_audio_pcm_used_bytes;
    }

    s_audio_pcm_read_offset = (s_audio_pcm_read_offset + drop_bytes) % s_audio_pcm_buffer_size;
    s_audio_pcm_used_bytes -= drop_bytes;
    return media_sink_audio_duration_ms_for_bytes(drop_bytes, playback_format);
}

static esp_err_t media_sink_append_audio_pcm(const uint8_t *data,
                                             size_t data_len,
                                             uint32_t source_packet_ms)
{
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t frame_bytes = media_sink_audio_frame_bytes(playback_format);
    const uint8_t *write_data = data;
    uint32_t trimmed_ms = 0;
    uint32_t buffered_ms = 0;

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid audio pcm append");
    ESP_RETURN_ON_FALSE(frame_bytes > 0, ESP_ERR_INVALID_SIZE, TAG, "invalid playback frame size");
    ESP_RETURN_ON_ERROR(media_sink_ensure_audio_playback_buffers(), TAG, "audio pcm buffer prepare failed");

    taskENTER_CRITICAL(&s_sink_lock);

    if (data_len > s_audio_pcm_buffer_size) {
        size_t trim_bytes = data_len - s_audio_pcm_buffer_size;

        write_data += trim_bytes;
        data_len = s_audio_pcm_buffer_size;
        trimmed_ms += media_sink_audio_duration_ms_for_bytes(trim_bytes, playback_format);
    }

    if (data_len > (s_audio_pcm_buffer_size - s_audio_pcm_used_bytes)) {
        size_t drop_bytes = data_len - (s_audio_pcm_buffer_size - s_audio_pcm_used_bytes);
        if ((drop_bytes % frame_bytes) != 0U) {
            drop_bytes += frame_bytes - (drop_bytes % frame_bytes);
        }
        trimmed_ms += media_sink_drop_audio_pcm_head(drop_bytes);
    }

    size_t write_offset = (s_audio_pcm_read_offset + s_audio_pcm_used_bytes) % s_audio_pcm_buffer_size;
    size_t first_copy = s_audio_pcm_buffer_size - write_offset;
    if (first_copy > data_len) {
        first_copy = data_len;
    }
    memcpy(s_audio_pcm_buffer + write_offset, write_data, first_copy);
    if (data_len > first_copy) {
        memcpy(s_audio_pcm_buffer, write_data + first_copy, data_len - first_copy);
    }
    s_audio_pcm_used_bytes += data_len;
    if (source_packet_ms > 0U) {
        s_audio_last_source_packet_ms = source_packet_ms;
    }

    audio_playout_snapshot_t snapshot = {0};
    audio_playout_controller_get_snapshot(&s_audio_controller, &snapshot);
    uint32_t emergency_limit_ms = snapshot.emergency_limit_ms;
    if (emergency_limit_ms < source_packet_ms) {
        emergency_limit_ms = source_packet_ms;
    }
    size_t emergency_limit_bytes =
        media_sink_audio_bytes_for_duration_ms(emergency_limit_ms, playback_format);
    if (emergency_limit_bytes > 0U &&
        s_audio_pcm_used_bytes > emergency_limit_bytes) {
        uint32_t keep_ms = snapshot.target_delay_ms +
                           media_sink_audio_profile_trim_keep_margin_ms(
                               media_sink_audio_get_profile());
        if (keep_ms < source_packet_ms) {
            keep_ms = source_packet_ms;
        }
        if (keep_ms > emergency_limit_ms) {
            keep_ms = emergency_limit_ms;
        }
        size_t keep_bytes =
            media_sink_audio_bytes_for_duration_ms(keep_ms, playback_format);
        size_t drop_bytes = s_audio_pcm_used_bytes > keep_bytes ?
                            s_audio_pcm_used_bytes - keep_bytes : 0U;
        drop_bytes -= drop_bytes % frame_bytes;
        if (drop_bytes > 0U) {
            trimmed_ms += media_sink_drop_audio_pcm_head(drop_bytes);
        }
    }

    buffered_ms = media_sink_audio_duration_ms_for_bytes(s_audio_pcm_used_bytes, playback_format);

    taskEXIT_CRITICAL(&s_sink_lock);

    media_sink_note_trimmed_audio_ms(trimmed_ms, buffered_ms);
    return ESP_OK;
}

static esp_err_t media_sink_pop_audio_pcm_chunk(size_t output_bytes,
                                                int16_t rate_adjust_permille,
                                                uint8_t **chunk_data,
                                                size_t *consumed_bytes)
{
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t frame_bytes = media_sink_audio_frame_bytes(playback_format);
    size_t output_frames = 0;
    size_t input_frames = 0;
    size_t input_bytes = 0;

    ESP_RETURN_ON_FALSE(chunk_data != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio chunk output");
    ESP_RETURN_ON_ERROR(media_sink_ensure_audio_playback_buffers(), TAG, "audio chunk buffer prepare failed");
    ESP_RETURN_ON_FALSE(playback_format != NULL &&
                        playback_format->bits_per_sample == 16 &&
                        frame_bytes > 0,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "audio smooth rate requires 16-bit PCM");
    ESP_RETURN_ON_FALSE(output_bytes <= s_audio_pcm_chunk_buffer_size &&
                        (output_bytes % frame_bytes) == 0U,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "audio chunk too large");

    output_frames = output_bytes / frame_bytes;
    input_frames = output_frames;
    if (rate_adjust_permille != 0 && output_frames > 1U) {
        uint32_t abs_rate = rate_adjust_permille < 0 ?
                            (uint32_t)(-rate_adjust_permille) :
                            (uint32_t)rate_adjust_permille;
        size_t adjust_frames = (output_frames * abs_rate + 999U) / 1000U;
        if (adjust_frames == 0U) {
            adjust_frames = 1U;
        }
        if (rate_adjust_permille > 0) {
            input_frames += adjust_frames;
        } else if (input_frames > adjust_frames + 1U) {
            input_frames -= adjust_frames;
        }
    }
    input_bytes = input_frames * frame_bytes;
    ESP_RETURN_ON_FALSE(input_bytes <= s_audio_pcm_resample_buffer_size,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "audio resample input too large");

    taskENTER_CRITICAL(&s_sink_lock);
    if (input_bytes > s_audio_pcm_used_bytes && rate_adjust_permille > 0) {
        input_frames = output_frames;
        input_bytes = output_bytes;
    }
    if (input_bytes > s_audio_pcm_used_bytes) {
        taskEXIT_CRITICAL(&s_sink_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t first_copy = s_audio_pcm_buffer_size - s_audio_pcm_read_offset;
    if (first_copy > input_bytes) {
        first_copy = input_bytes;
    }
    memcpy(s_audio_pcm_resample_buffer,
           s_audio_pcm_buffer + s_audio_pcm_read_offset,
           first_copy);
    if (input_bytes > first_copy) {
        memcpy(s_audio_pcm_resample_buffer + first_copy,
               s_audio_pcm_buffer,
               input_bytes - first_copy);
    }

    s_audio_pcm_read_offset = (s_audio_pcm_read_offset + input_bytes) % s_audio_pcm_buffer_size;
    s_audio_pcm_used_bytes -= input_bytes;
    taskEXIT_CRITICAL(&s_sink_lock);

    if (input_frames == output_frames) {
        memcpy(s_audio_pcm_chunk_buffer, s_audio_pcm_resample_buffer, output_bytes);
    } else {
        /* The controller is limited to small speech-rate corrections, so a
         * linear resample is predictable and keeps this path allocation-free. */
        const int16_t *input = (const int16_t *)s_audio_pcm_resample_buffer;
        int16_t *output = (int16_t *)s_audio_pcm_chunk_buffer;
        const size_t channels = playback_format->channels;
        const size_t denominator = output_frames - 1U;

        for (size_t output_frame = 0; output_frame < output_frames; ++output_frame) {
            uint64_t position = (uint64_t)output_frame * (input_frames - 1U);
            size_t input_frame = (size_t)(position / denominator);
            size_t fraction = (size_t)(position % denominator);
            size_t next_frame = input_frame + 1U < input_frames ?
                                input_frame + 1U : input_frame;

            for (size_t channel = 0; channel < channels; ++channel) {
                int32_t first = input[input_frame * channels + channel];
                int32_t second = input[next_frame * channels + channel];
                int64_t mixed =
                    (int64_t)first * (int64_t)(denominator - fraction) +
                    (int64_t)second * (int64_t)fraction;
                mixed += mixed >= 0 ? (int64_t)(denominator / 2U) :
                                      -(int64_t)(denominator / 2U);
                output[output_frame * channels + channel] =
                    (int16_t)(mixed / (int64_t)denominator);
            }
        }
    }

    *chunk_data = s_audio_pcm_chunk_buffer;
    if (consumed_bytes != NULL) {
        *consumed_bytes = input_bytes;
    }
    return ESP_OK;
}

static uint32_t media_sink_audio_level_percent(const int16_t *samples, size_t data_len)
{
    uint32_t playback_peak = 0;
    size_t sample_count = data_len / sizeof(int16_t);

    if (samples == NULL || sample_count == 0) {
        return 0;
    }

    for (size_t index = 0; index < sample_count; ++index) {
        uint32_t abs_value = (uint32_t)abs(samples[index]);
        if (abs_value > playback_peak) {
            playback_peak = abs_value;
        }
    }

    return (playback_peak * 100U) / 32767U;
}

static uint32_t media_sink_get_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_sink_lock);
    generation = s_generation;
    taskEXIT_CRITICAL(&s_sink_lock);
    return generation;
}

static void media_sink_free_audio_packet(media_sink_audio_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    free(packet->data);
    memset(packet, 0, sizeof(*packet));
}

static uint32_t media_sink_audio_packet_duration_us(const media_sink_audio_packet_t *packet)
{
    if (packet == NULL || packet->format.bits_per_sample == 0 || packet->format.channels == 0 ||
        packet->format.sample_rate_hz == 0) {
        return 0;
    }

    size_t bytes_per_frame = ((size_t)packet->format.bits_per_sample / 8U) * packet->format.channels;
    if (bytes_per_frame == 0) {
        return 0;
    }

    size_t frame_count = packet->data_len / bytes_per_frame;
    if (frame_count == 0) {
        return 0;
    }

    return (uint32_t)(((uint64_t)frame_count * 1000000ULL) / packet->format.sample_rate_hz);
}

static void media_sink_maybe_log_audio_enqueue(const media_sink_audio_packet_t *packet)
{
    TickType_t now = 0;
    bool should_log = false;
    bool first_log = false;
    uint32_t packet_ms = 0;
    UBaseType_t queued_packets = 0;

    if (packet == NULL || s_audio_queue == NULL) {
        return;
    }

    now = xTaskGetTickCount();
    queued_packets = uxQueueMessagesWaiting(s_audio_queue);
    packet_ms = media_sink_audio_packet_duration_us(packet) / 1000U;
    if (!s_remote_audio_enqueue_logged) {
        s_remote_audio_enqueue_logged = true;
        first_log = true;
        should_log = true;
    } else if (s_last_audio_enqueue_log_tick == 0 ||
               now - s_last_audio_enqueue_log_tick >= pdMS_TO_TICKS(1000)) {
        should_log = true;
    }

    if (!should_log) {
        return;
    }

    s_last_audio_enqueue_log_tick = now;
    if (first_log) {
        ESP_LOGI(TAG,
                 "remote audio queued: packet_ms=%u bytes=%u format=%luHz/%ubit/%uch queued=%u buffered_ms=%u",
                 (unsigned)packet_ms,
                 (unsigned)packet->data_len,
                 (unsigned long)packet->format.sample_rate_hz,
                 packet->format.bits_per_sample,
                 packet->format.channels,
                 (unsigned)queued_packets,
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    } else {
        ESP_LOGD(TAG,
                 "remote audio queued: packet_ms=%u bytes=%u format=%luHz/%ubit/%uch queued=%u buffered_ms=%u",
                 (unsigned)packet_ms,
                 (unsigned)packet->data_len,
                 (unsigned long)packet->format.sample_rate_hz,
                 packet->format.bits_per_sample,
                 packet->format.channels,
                 (unsigned)queued_packets,
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    }
}

static void media_sink_drop_oldest_audio(void)
{
    media_sink_audio_packet_t stale = {0};

    if (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &stale, 0) == pdTRUE) {
        media_sink_free_audio_packet(&stale);
    }
}

static void media_sink_trim_audio_backlog(void)
{
    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();

    if (s_audio_queue == NULL) {
        return;
    }

    UBaseType_t queued_packets = uxQueueMessagesWaiting(s_audio_queue);
    if (queued_packets < tuning.backlog_trim_threshold) {
        return;
    }

    uint32_t dropped_packets = 0;
    uint32_t dropped_ms = 0;
    while (queued_packets > tuning.backlog_target_packets) {
        media_sink_audio_packet_t stale = {0};

        if (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &stale, 0) == pdTRUE) {
            dropped_ms += media_sink_audio_packet_duration_us(&stale) / 1000U;
            media_sink_free_audio_packet(&stale);
            dropped_packets++;
        }
        queued_packets = uxQueueMessagesWaiting(s_audio_queue);
    }

    TickType_t now = xTaskGetTickCount();
    bool should_log = false;
    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_trim_drop_packets_in_window += dropped_packets;
    s_audio_trim_drop_ms_in_window += dropped_ms;
    if (dropped_packets > 0 &&
        (s_last_audio_trim_log_tick == 0 || now - s_last_audio_trim_log_tick >= pdMS_TO_TICKS(1000))) {
        s_last_audio_trim_log_tick = now;
        should_log = true;
    }
    taskEXIT_CRITICAL(&s_sink_lock);

    if (should_log) {
        const audio_format_t *playback_format = speaker_get_playback_format();
        APP_LOG_DETAIL(TAG,
                       "remote audio backlog trimmed: dropped_packets=%u queued=%u buffered_ms=%u",
                       (unsigned)dropped_packets,
                       (unsigned)queued_packets,
                       (unsigned)media_sink_audio_duration_ms_for_bytes(
                           media_sink_get_audio_pcm_used_bytes(), playback_format));
    }
}

static void media_sink_reset_audio_warning_window(TickType_t now)
{
    s_audio_warning_window_tick = now;
    s_audio_warning_rx_packets = 0;
    s_audio_warning_rx_ms = 0;
    s_audio_warning_play_packets = 0;
    s_audio_warning_play_ms = 0;
    s_audio_warning_write_drop_packets = 0;
    s_audio_warning_write_drop_ms = 0;
    s_audio_warning_trim_drop_packets = 0;
    s_audio_warning_trim_drop_ms = 0;
    s_audio_warning_underflows = 0;
    s_audio_warning_accelerated_chunks = 0;
    s_audio_warning_expanded_chunks = 0;
}

static void media_sink_maybe_log_audio_rate(void)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t rx_packets = 0;
    uint32_t rx_ms = 0;
    uint32_t play_ok_packets = 0;
    uint32_t play_ok_ms = 0;
    uint32_t play_drop_packets = 0;
    uint32_t play_drop_ms = 0;
    uint32_t trim_drop_packets = 0;
    uint32_t trim_drop_ms = 0;
    uint32_t underflows = 0;
    uint32_t accelerated_chunks = 0;
    uint32_t expanded_chunks = 0;
    audio_playout_snapshot_t controller = {0};

    if (s_last_audio_rate_log_tick == 0) {
        s_last_audio_rate_log_tick = now;
        return;
    }
    if (now - s_last_audio_rate_log_tick <
        pdMS_TO_TICKS(MEDIA_SINK_AUDIO_CONTROL_WINDOW_MS)) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    rx_packets = s_audio_rx_packets_in_window;
    rx_ms = s_audio_rx_ms_in_window;
    play_ok_packets = s_audio_play_ok_packets_in_window;
    play_ok_ms = s_audio_play_ok_ms_in_window;
    play_drop_packets = s_audio_play_drop_packets_in_window;
    play_drop_ms = s_audio_play_drop_ms_in_window;
    trim_drop_packets = s_audio_trim_drop_packets_in_window;
    trim_drop_ms = s_audio_trim_drop_ms_in_window;
    underflows = s_audio_underflows_in_window;
    accelerated_chunks = s_audio_accelerated_chunks_in_window;
    expanded_chunks = s_audio_expanded_chunks_in_window;

    audio_playout_window_t window = {
        .received_ms = rx_ms,
        .played_ms = play_ok_ms,
        .underflows = underflows,
        .local_write_drop_ms = play_drop_ms,
        .hard_trim_ms = trim_drop_ms,
    };
    audio_playout_controller_update_window(&s_audio_controller, &window);
    audio_playout_controller_get_snapshot(&s_audio_controller, &controller);

    s_audio_rx_packets_in_window = 0;
    s_audio_rx_ms_in_window = 0;
    s_audio_play_ok_packets_in_window = 0;
    s_audio_play_ok_ms_in_window = 0;
    s_audio_play_drop_packets_in_window = 0;
    s_audio_play_drop_ms_in_window = 0;
    s_audio_trim_drop_packets_in_window = 0;
    s_audio_trim_drop_ms_in_window = 0;
    s_audio_underflows_in_window = 0;
    s_audio_accelerated_chunks_in_window = 0;
    s_audio_expanded_chunks_in_window = 0;
    taskEXIT_CRITICAL(&s_sink_lock);

    uint32_t buffered_ms = media_sink_audio_duration_ms_for_bytes(
        media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format());
    uint32_t queued_packets =
        s_audio_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_audio_queue) : 0U;

    if (s_audio_warning_window_tick == 0) {
        media_sink_reset_audio_warning_window(now);
    }
    s_audio_warning_rx_packets += rx_packets;
    s_audio_warning_rx_ms += rx_ms;
    s_audio_warning_play_packets += play_ok_packets;
    s_audio_warning_play_ms += play_ok_ms;
    s_audio_warning_write_drop_packets += play_drop_packets;
    s_audio_warning_write_drop_ms += play_drop_ms;
    s_audio_warning_trim_drop_packets += trim_drop_packets;
    s_audio_warning_trim_drop_ms += trim_drop_ms;
    s_audio_warning_underflows += underflows;
    s_audio_warning_accelerated_chunks += accelerated_chunks;
    s_audio_warning_expanded_chunks += expanded_chunks;

    bool warning_window_due =
        now - s_audio_warning_window_tick >=
        pdMS_TO_TICKS(MEDIA_SINK_AUDIO_WARNING_INTERVAL_MS);
    bool warning_allowed =
        s_last_audio_warning_tick == 0 ||
        now - s_last_audio_warning_tick >=
            pdMS_TO_TICKS(MEDIA_SINK_AUDIO_WARNING_INTERVAL_MS);
    /*
     * A single underflow while the initial prebuffer is filling can move the
     * controller into recovery without producing sustained audible damage.
     * Escalate recovery only through the accumulated window; local output
     * pressure remains an immediate warning.
     */
    bool condition_entered_pressure =
        controller.condition != s_audio_last_condition &&
        controller.condition == AUDIO_PLAYOUT_CONDITION_LOCAL_PRESSURE;
    bool accumulated_pressure =
        s_audio_warning_underflows >=
            MEDIA_SINK_AUDIO_UNDERFLOW_WARNING_COUNT ||
        s_audio_warning_write_drop_packets > 0U ||
        s_audio_warning_trim_drop_ms > MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
    bool immediate_pressure =
        play_drop_packets > 0U ||
        trim_drop_ms > MEDIA_SINK_AUDIO_PLAY_CHUNK_MS ||
        controller.condition == AUDIO_PLAYOUT_CONDITION_LOCAL_PRESSURE;
    bool should_warn =
        warning_allowed &&
        (immediate_pressure || condition_entered_pressure ||
         (warning_window_due && accumulated_pressure));
    s_audio_last_condition = controller.condition;

    if (should_warn) {
        ESP_LOGW(TAG,
                 "remote audio unstable: rx=%up/%ums play=%up/%ums underflow=%u "
                 "i2s_drop=%up/%ums trim=%up/%ums q=%u buf=%ums target=%ums "
                 "jitter=%ums mode=%s rate=+%u/-%u",
                 (unsigned)s_audio_warning_rx_packets,
                 (unsigned)s_audio_warning_rx_ms,
                 (unsigned)s_audio_warning_play_packets,
                 (unsigned)s_audio_warning_play_ms,
                 (unsigned)s_audio_warning_underflows,
                 (unsigned)s_audio_warning_write_drop_packets,
                 (unsigned)s_audio_warning_write_drop_ms,
                 (unsigned)s_audio_warning_trim_drop_packets,
                 (unsigned)s_audio_warning_trim_drop_ms,
                 (unsigned)queued_packets,
                 (unsigned)buffered_ms,
                 (unsigned)controller.target_delay_ms,
                 (unsigned)controller.jitter_ms,
                 audio_playout_condition_name(controller.condition),
                 (unsigned)s_audio_warning_accelerated_chunks,
                 (unsigned)s_audio_warning_expanded_chunks);
        s_last_audio_warning_tick = now;
        media_sink_reset_audio_warning_window(now);
    } else if (warning_window_due) {
        media_sink_reset_audio_warning_window(now);
    }

    if (trim_drop_packets > 0U) {
        ESP_LOGD(TAG,
                 "remote audio convergence: rx=%up/%ums played=%up/%ums hard_trim=%up/%ums buffered=%ums target=%ums mode=%s",
                 (unsigned)rx_packets,
                 (unsigned)rx_ms,
                 (unsigned)play_ok_packets,
                 (unsigned)play_ok_ms,
                 (unsigned)trim_drop_packets,
                 (unsigned)trim_drop_ms,
                 (unsigned)buffered_ms,
                 (unsigned)controller.target_delay_ms,
                 audio_playout_condition_name(controller.condition));
    } else if (rx_packets > 0U && play_ok_packets == 0U) {
        if (!s_remote_audio_buffering_logged) {
            s_remote_audio_buffering_logged = true;
            APP_LOG_DETAIL(
                TAG,
                "remote audio buffering: rx=%up/%ums queued=%u buffered=%ums prebuffer=%ums target=%ums jitter=%ums mode=%s",
                (unsigned)rx_packets,
                (unsigned)rx_ms,
                (unsigned)queued_packets,
                (unsigned)buffered_ms,
                (unsigned)controller.prebuffer_ms,
                (unsigned)controller.target_delay_ms,
                (unsigned)controller.jitter_ms,
                audio_playout_condition_name(controller.condition));
        }
    } else {
        if (play_ok_packets > 0U) {
            s_remote_audio_buffering_logged = false;
        }
        ESP_LOGD(TAG,
                 "remote audio steady: rx=%up/%ums played=%up/%ums queued=%u buffered=%ums target=%ums jitter=%ums mode=%s rate=+%u/-%u",
                 (unsigned)rx_packets,
                 (unsigned)rx_ms,
                 (unsigned)play_ok_packets,
                 (unsigned)play_ok_ms,
                 (unsigned)queued_packets,
                 (unsigned)buffered_ms,
                 (unsigned)controller.target_delay_ms,
                 (unsigned)controller.jitter_ms,
                 audio_playout_condition_name(controller.condition),
                 (unsigned)accelerated_chunks,
                 (unsigned)expanded_chunks);
    }

    s_last_audio_rate_log_tick = now;
}

static esp_err_t media_sink_queue_audio(media_sink_audio_packet_t *packet)
{
    if (packet == NULL || s_audio_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    media_sink_trim_audio_backlog();

    if (xQueueSend(s_audio_queue, packet, 0) == pdTRUE) {
        memset(packet, 0, sizeof(*packet));
        return ESP_OK;
    }

    ESP_LOGD(TAG, "remote audio queue full: dropped oldest packet");
    media_sink_drop_oldest_audio();
    if (xQueueSend(s_audio_queue, packet, 0) == pdTRUE) {
        memset(packet, 0, sizeof(*packet));
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

static void media_sink_buffer_audio_packet(media_sink_audio_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    if (packet->generation != media_sink_get_generation()) {
        media_sink_free_audio_packet(packet);
        return;
    }

    uint32_t packet_duration_us = media_sink_audio_packet_duration_us(packet);
    uint32_t packet_duration_ms = packet_duration_us / 1000U;
    int16_t *rendered_samples = NULL;
    size_t rendered_bytes = 0;
    uint32_t output_level = 0;
    taskENTER_CRITICAL(&s_sink_lock);
    audio_playout_controller_observe_packet(&s_audio_controller,
                                            packet->source_timestamp_ms,
                                            packet->arrival_ms,
                                            packet_duration_ms);
    taskEXIT_CRITICAL(&s_sink_lock);

    esp_err_t render_ret = speaker_render_pcm(packet->data,
                                                               packet->data_len,
                                                               &packet->format,
                                                              &rendered_samples,
                                                              &rendered_bytes,
                                                              &output_level);
    if (render_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "remote audio render failed: ret=%d rate=%lu bits=%u ch=%u bytes=%u",
                 render_ret,
                 (unsigned long)packet->format.sample_rate_hz,
                 packet->format.bits_per_sample,
                 packet->format.channels,
                 (unsigned)packet->data_len);
        media_sink_free_audio_packet(packet);
        return;
    }

    media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
    while (uxQueueMessagesWaiting(s_audio_queue) >= tuning.backlog_trim_threshold) {
        media_sink_trim_audio_backlog();
    }

    esp_err_t append_ret = media_sink_append_audio_pcm((const uint8_t *)rendered_samples,
                                                                rendered_bytes,
                                                                packet_duration_us / 1000U);
    TickType_t now = xTaskGetTickCount();
    if (!s_remote_audio_render_logged) {
        s_remote_audio_render_logged = true;
        s_last_audio_render_log_tick = now;
        ESP_LOGI(TAG,
                 "remote audio rendered: input_bytes=%u rendered_bytes=%u packet_ms=%lu level=%u queued=%u buffered_ms=%u",
                 (unsigned)packet->data_len,
                 (unsigned)rendered_bytes,
                 (unsigned long)(packet_duration_us / 1000U),
                 (unsigned)output_level,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    } else if (s_last_audio_render_log_tick == 0 ||
               now - s_last_audio_render_log_tick >= pdMS_TO_TICKS(1000)) {
        s_last_audio_render_log_tick = now;
        ESP_LOGD(TAG,
                 "remote audio render steady: input_bytes=%u rendered_bytes=%u packet_ms=%lu level=%u queued=%u buffered_ms=%u",
                 (unsigned)packet->data_len,
                 (unsigned)rendered_bytes,
                 (unsigned long)(packet_duration_us / 1000U),
                 (unsigned)output_level,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue),
                 (unsigned)media_sink_audio_duration_ms_for_bytes(
                     media_sink_get_audio_pcm_used_bytes(), speaker_get_playback_format()));
    }
    if (append_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "remote audio pcm append failed: ret=%d rendered_bytes=%u input_ms=%lu",
                 append_ret,
                 (unsigned)rendered_bytes,
                 (unsigned long)(packet_duration_us / 1000U));
    }

    media_sink_free_audio_packet(packet);
}

static void media_sink_audio_task(void *ctx)
{
    (void)ctx;
    const audio_format_t *playback_format = speaker_get_playback_format();
    const size_t play_chunk_bytes =
        media_sink_audio_bytes_for_duration_ms(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                                        playback_format);
    bool playback_started = false;
    media_sink_audio_packet_t packet = {0};
    TickType_t next_play_tick = 0;
    uint32_t playback_generation = 0;

    while (true) {
        TickType_t queue_wait = portMAX_DELAY;
        if (playback_started) {
            /* Wait for the next packet until the playout deadline. Checking an
             * empty queue immediately after each write creates false underflows. */
            TickType_t now_tick = xTaskGetTickCount();
            queue_wait = next_play_tick != 0 &&
                         (int32_t)(next_play_tick - now_tick) > 0 ?
                         next_play_tick - now_tick : 0;
        }

        BaseType_t received_packet =
            xQueueReceive(s_audio_queue, &packet, queue_wait);
        size_t buffered_bytes = 0;

        if (received_packet == pdTRUE) {
            media_sink_audio_tuning_t tuning = media_sink_audio_get_tuning();
            size_t drained_packets = 0;

            media_sink_buffer_audio_packet(&packet);
            while (drained_packets < tuning.drain_burst_max &&
                   xQueueReceive(s_audio_queue, &packet, 0) == pdTRUE) {
                media_sink_buffer_audio_packet(&packet);
                drained_packets++;
            }
        }

        buffered_bytes = media_sink_get_audio_pcm_used_bytes();
        uint32_t source_packet_ms = media_sink_get_last_source_packet_ms();
        uint32_t buffered_ms =
            media_sink_audio_duration_ms_for_bytes(buffered_bytes, playback_format);
        audio_playout_decision_t decision = {0};
        taskENTER_CRITICAL(&s_sink_lock);
        audio_playout_controller_decide(&s_audio_controller,
                                        buffered_ms,
                                        MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                        playback_started,
                                        &decision);
        taskEXIT_CRITICAL(&s_sink_lock);

        uint32_t prebuffer_ms = decision.prebuffer_ms;
        if (prebuffer_ms < source_packet_ms) {
            prebuffer_ms = source_packet_ms;
        }
        size_t prebuffer_bytes =
            media_sink_audio_bytes_for_duration_ms(prebuffer_ms, playback_format);
        if (prebuffer_bytes == 0U) {
            prebuffer_bytes = play_chunk_bytes;
        }

        if (!playback_started && buffered_bytes >= prebuffer_bytes) {
            playback_started = true;
            next_play_tick = xTaskGetTickCount();
            playback_generation = media_sink_get_generation();
            taskENTER_CRITICAL(&s_sink_lock);
            audio_playout_controller_note_playback_started(&s_audio_controller);
            taskEXIT_CRITICAL(&s_sink_lock);
        }

        if (playback_started) {
            TickType_t now_tick = xTaskGetTickCount();
            if (next_play_tick != 0 &&
                (int32_t)(next_play_tick - now_tick) > 0) {
                vTaskDelay(next_play_tick - now_tick);
            }
        }

        if (playback_started &&
            playback_generation != media_sink_get_generation()) {
            playback_started = false;
            next_play_tick = 0;
            media_sink_maybe_log_audio_rate();
            continue;
        }

        if (playback_started) {
            buffered_bytes = media_sink_get_audio_pcm_used_bytes();
            buffered_ms =
                media_sink_audio_duration_ms_for_bytes(buffered_bytes, playback_format);
            taskENTER_CRITICAL(&s_sink_lock);
            audio_playout_controller_decide(&s_audio_controller,
                                            buffered_ms,
                                            MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                                            true,
                                            &decision);
            taskEXIT_CRITICAL(&s_sink_lock);
        }

        if (playback_started && buffered_bytes < play_chunk_bytes) {
            playback_started = false;
            next_play_tick = 0;
            media_sink_audio_note_underflow();
        }

        if (playback_started && buffered_bytes >= play_chunk_bytes) {
            uint8_t *play_chunk = NULL;
            uint32_t buffered_ms_before_play = buffered_ms;
            uint32_t output_level = 0;
            int64_t play_start_us = 0;
            int64_t play_elapsed_us = 0;
            size_t consumed_bytes = 0;
            esp_err_t play_ret =
                media_sink_pop_audio_pcm_chunk(play_chunk_bytes,
                                               decision.rate_adjust_permille,
                                               &play_chunk,
                                               &consumed_bytes);

            if (play_ret != ESP_OK) {
                playback_started = false;
                next_play_tick = 0;
                continue;
            }

            taskENTER_CRITICAL(&s_sink_lock);
            if (consumed_bytes > play_chunk_bytes) {
                s_audio_accelerated_chunks_in_window++;
            } else if (consumed_bytes < play_chunk_bytes) {
                s_audio_expanded_chunks_in_window++;
            }
            taskEXIT_CRITICAL(&s_sink_lock);

            output_level = media_sink_audio_level_percent((const int16_t *)play_chunk, play_chunk_bytes);
            if (playback_generation != media_sink_get_generation()) {
                playback_started = false;
                next_play_tick = 0;
                media_sink_maybe_log_audio_rate();
                continue;
            }

            play_start_us = esp_timer_get_time();
            play_ret = speaker_write_rendered_pcm((int16_t *)play_chunk,
                                                              play_chunk_bytes,
                                                              output_level);
            play_elapsed_us = esp_timer_get_time() - play_start_us;

            audio_playback_timing_t playback_timing = {0};
            speaker_get_last_playback_timing(&playback_timing);
            if (play_ret == ESP_ERR_TIMEOUT) {
                taskENTER_CRITICAL(&s_sink_lock);
                s_audio_play_drop_packets_in_window++;
                s_audio_play_drop_ms_in_window += MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
                taskEXIT_CRITICAL(&s_sink_lock);
                TickType_t now = xTaskGetTickCount();
                if (s_last_audio_write_drop_log_tick == 0 ||
                    now - s_last_audio_write_drop_log_tick >= pdMS_TO_TICKS(1000)) {
                    s_last_audio_write_drop_log_tick = now;
                    APP_LOG_DETAIL(
                        TAG,
                        "remote audio playback drop: i2s_busy chunk_ms=%u buffered_ms=%u queued=%u",
                        MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                        (unsigned)buffered_ms_before_play,
                        (unsigned)uxQueueMessagesWaiting(s_audio_queue));
                }
            } else if (play_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "remote audio playback failed: ret=%d chunk_ms=%u buffered_ms=%u",
                         play_ret,
                         MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                         (unsigned)buffered_ms_before_play);
                playback_started = false;
                next_play_tick = 0;
            } else {
                if (!s_remote_audio_playback_started_logged) {
                    audio_playout_snapshot_t controller = {0};
                    media_sink_audio_get_controller_snapshot(&controller);
                    s_remote_audio_playback_started_logged = true;
                    ESP_LOGI(TAG,
                             "remote audio playback started: prebuffer=%ums target=%ums jitter=%ums mode=%s chunk=%ums buffered=%ums level=%u",
                             (unsigned)prebuffer_ms,
                             (unsigned)controller.target_delay_ms,
                             (unsigned)controller.jitter_ms,
                             audio_playout_condition_name(controller.condition),
                             MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                             (unsigned)buffered_ms_before_play,
                             (unsigned)output_level);
                }
                taskENTER_CRITICAL(&s_sink_lock);
                s_audio_play_ok_packets_in_window++;
                s_audio_play_ok_ms_in_window += MEDIA_SINK_AUDIO_PLAY_CHUNK_MS;
                taskEXIT_CRITICAL(&s_sink_lock);
            }

            if (play_ret == ESP_OK || play_ret == ESP_ERR_TIMEOUT) {
                const TickType_t play_period = pdMS_TO_TICKS(MEDIA_SINK_AUDIO_PLAY_CHUNK_MS);
                TickType_t now_tick = xTaskGetTickCount();
                if (next_play_tick == 0 ||
                    (int32_t)(now_tick - next_play_tick) > (int32_t)(play_period * 2U)) {
                    next_play_tick = now_tick + play_period;
                } else {
                    next_play_tick += play_period;
                }
            }

            if (play_ret == ESP_OK && play_elapsed_us > MEDIA_SINK_AUDIO_SLOW_PLAY_US) {
                TickType_t now = xTaskGetTickCount();
                if (s_last_audio_slow_log_tick == 0 || now - s_last_audio_slow_log_tick >= pdMS_TO_TICKS(1000)) {
                    s_last_audio_slow_log_tick = now;
                    ESP_LOGD(TAG,
                             "remote audio playback slow elapsed_ms=%lu prepare_ms=%lu write_ms=%lu bytes=%lu chunk_ms=%u buffered_ms=%u queued=%u",
                             (unsigned long)(play_elapsed_us / 1000ULL),
                             (unsigned long)playback_timing.prepare_ms,
                             (unsigned long)playback_timing.write_ms,
                             (unsigned long)playback_timing.data_bytes,
                             MEDIA_SINK_AUDIO_PLAY_CHUNK_MS,
                             (unsigned)buffered_ms_before_play,
                             (unsigned)uxQueueMessagesWaiting(s_audio_queue));
                }
            }

            media_sink_maybe_log_audio_rate();
            continue;
        }

        media_sink_maybe_log_audio_rate();
    }
}

void media_sink_set_audio_profile(media_sink_audio_profile_t profile)
{
    bool changed = false;

    if (!media_sink_audio_profile_valid(profile)) {
        profile = MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    changed = s_audio_profile != profile;
    s_audio_profile = profile;
    if (changed) {
        audio_playout_controller_set_profile(
            &s_audio_controller,
            media_sink_audio_controller_profile(profile));
        if (s_remote_audio_playback_started_logged) {
            audio_playout_controller_note_playback_started(&s_audio_controller);
        }
    }
    taskEXIT_CRITICAL(&s_sink_lock);

    if (changed) {
        ESP_LOGI(TAG, "remote audio profile: %s", media_sink_audio_profile_name(profile));
    }
}

esp_err_t media_sink_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    audio_playout_controller_init(
        &s_audio_controller,
        media_sink_audio_controller_profile(s_audio_profile));

    s_audio_queue = xQueueCreateWithCaps(MEDIA_SINK_AUDIO_QUEUE_LEN,
                                         sizeof(media_sink_audio_packet_t),
                                         APP_QUEUE_CAPS_CONTROL);
    ESP_RETURN_ON_FALSE(s_audio_queue != NULL, ESP_ERR_NO_MEM, TAG, "audio queue alloc failed");

    esp_err_t buffer_ret = media_sink_ensure_audio_playback_buffers();
    if (buffer_ret != ESP_OK) {
        vQueueDeleteWithCaps(s_audio_queue);
        s_audio_queue = NULL;
        return buffer_ret;
    }

    BaseType_t audio_ok = xTaskCreatePinnedToCoreWithCaps(media_sink_audio_task,
                                                          "media_audio_rx",
                                                          MEDIA_SINK_AUDIO_TASK_STACK,
                                                          NULL,
                                                          MEDIA_SINK_AUDIO_TASK_PRIORITY,
                                                          &s_audio_task,
                                                          MEDIA_SINK_AUDIO_TASK_CORE,
                                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (audio_ok != pdPASS) {
        vQueueDeleteWithCaps(s_audio_queue);
        s_audio_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "remote audio adaptive playout ready: profile=%s pcm_psram=%u resample_psram=%u",
             media_sink_audio_profile_name(s_audio_profile),
             (unsigned)s_audio_pcm_buffer_size,
             (unsigned)(s_audio_pcm_chunk_buffer_size +
                        s_audio_pcm_resample_buffer_size));
    return ESP_OK;
}

void media_sink_get_stats(media_sink_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->initialized = s_initialized;
    stats->audio_profile = media_sink_audio_get_profile();
    stats->audio_queue_capacity = MEDIA_SINK_AUDIO_QUEUE_LEN;
    stats->audio_queue_len = s_audio_queue != NULL ? (uint32_t)uxQueueMessagesWaiting(s_audio_queue) : 0U;

    const audio_format_t *playback_format = speaker_get_playback_format();
    audio_playout_snapshot_t controller = {0};
    taskENTER_CRITICAL(&s_sink_lock);
    stats->audio_pcm_used_bytes = s_audio_pcm_used_bytes;
    stats->audio_pcm_capacity = s_audio_pcm_buffer_size;
    audio_playout_controller_get_snapshot(&s_audio_controller, &controller);
    uint32_t base_delay_ms = media_sink_audio_profile_base_delay_ms(s_audio_profile);
    stats->audio_jitter_boost_ms = controller.target_delay_ms > base_delay_ms ?
                                   controller.target_delay_ms - base_delay_ms : 0U;
    stats->audio_target_delay_ms = controller.target_delay_ms;
    stats->audio_prebuffer_ms = controller.prebuffer_ms;
    stats->audio_arrival_jitter_ms = controller.jitter_ms;
    stats->audio_peak_timing_error_ms = controller.peak_timing_error_ms;
    stats->audio_condition = (uint8_t)controller.condition;
    stats->audio_buffered_ms = media_sink_audio_duration_ms_for_bytes(s_audio_pcm_used_bytes, playback_format);
    taskEXIT_CRITICAL(&s_sink_lock);
}

static esp_err_t media_sink_submit_audio_packet(media_sink_audio_packet_t *packet)
{
    esp_err_t ret = media_sink_queue_audio(packet);
    if (ret != ESP_OK) {
        media_sink_free_audio_packet(packet);
    }
    return ret;
}

esp_err_t media_sink_submit_remote_audio(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         uint32_t source_timestamp_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "media sink not initialized");
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0 && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio packet");

    media_sink_audio_packet_t packet = {
        .generation = media_sink_get_generation(),
        .format = *format,
        .data_len = data_len,
        .source_timestamp_ms = source_timestamp_ms,
        .arrival_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };
    packet.data = app_memory_alloc_psram(data_len);
    ESP_RETURN_ON_FALSE(packet.data != NULL, ESP_ERR_NO_MEM, TAG, "audio packet alloc failed");
    memcpy(packet.data, data, data_len);

    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_rx_packets_in_window++;
    s_audio_rx_ms_in_window += media_sink_audio_packet_duration_us(&packet) / 1000U;
    taskEXIT_CRITICAL(&s_sink_lock);
    media_sink_maybe_log_audio_enqueue(&packet);

    return media_sink_submit_audio_packet(&packet);
}

esp_err_t media_sink_submit_remote_audio_owned(uint8_t *data,
                                               size_t data_len,
                                               const audio_format_t *format,
                                               uint32_t source_timestamp_ms)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "media sink not initialized");
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0 && format != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid audio packet");

    media_sink_audio_packet_t packet = {
        .generation = media_sink_get_generation(),
        .format = *format,
        .data = data,
        .data_len = data_len,
        .source_timestamp_ms = source_timestamp_ms,
        .arrival_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };

    taskENTER_CRITICAL(&s_sink_lock);
    s_audio_rx_packets_in_window++;
    s_audio_rx_ms_in_window += media_sink_audio_packet_duration_us(&packet) / 1000U;
    taskEXIT_CRITICAL(&s_sink_lock);
    media_sink_maybe_log_audio_enqueue(&packet);

    return media_sink_submit_audio_packet(&packet);
}

void media_sink_flush(void)
{
    if (!s_initialized) {
        return;
    }

    taskENTER_CRITICAL(&s_sink_lock);
    s_generation++;
    s_remote_audio_playback_started_logged = false;
    s_audio_last_source_packet_ms = 0;
    s_audio_rx_packets_in_window = 0;
    s_audio_rx_ms_in_window = 0;
    s_audio_play_ok_packets_in_window = 0;
    s_audio_play_ok_ms_in_window = 0;
    s_audio_play_drop_packets_in_window = 0;
    s_audio_play_drop_ms_in_window = 0;
    s_audio_trim_drop_packets_in_window = 0;
    s_audio_trim_drop_ms_in_window = 0;
    s_audio_underflows_in_window = 0;
    s_audio_accelerated_chunks_in_window = 0;
    s_audio_expanded_chunks_in_window = 0;
    audio_playout_controller_reset(&s_audio_controller);
    taskEXIT_CRITICAL(&s_sink_lock);

    s_remote_audio_enqueue_logged = false;
    s_remote_audio_render_logged = false;
    s_remote_audio_buffering_logged = false;
    s_last_audio_trim_log_tick = 0;
    s_last_audio_enqueue_log_tick = 0;
    s_last_audio_render_log_tick = 0;
    s_last_audio_slow_log_tick = 0;
    s_last_audio_write_drop_log_tick = 0;
    s_last_audio_rate_log_tick = 0;
    s_last_audio_warning_tick = 0;
    s_audio_last_condition = AUDIO_PLAYOUT_CONDITION_STARTUP;
    media_sink_reset_audio_warning_window(0);

    speaker_stop_playback();
    media_sink_reset_audio_pcm_buffer();

    media_sink_audio_packet_t audio_packet = {0};
    while (s_audio_queue != NULL && xQueueReceive(s_audio_queue, &audio_packet, 0) == pdTRUE) {
        media_sink_free_audio_packet(&audio_packet);
    }
}
