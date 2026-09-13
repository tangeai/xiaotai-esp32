#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_types.h"

typedef enum {
    /* Clean LAN or one-way monitoring: prioritize first-sound latency. */
    MEDIA_SINK_AUDIO_PROFILE_LOW_LATENCY = 0,
    /* Full-duplex calls and AI speech: adapt across normal mobile jitter. */
    MEDIA_SINK_AUDIO_PROFILE_ADAPTIVE_CALL,
    /* Explicit fallback for persistently bursty links. */
    MEDIA_SINK_AUDIO_PROFILE_JITTER_SAFE,
} media_sink_audio_profile_t;

typedef struct {
    bool initialized;
    media_sink_audio_profile_t audio_profile;
    uint32_t audio_queue_len;
    uint32_t audio_queue_capacity;
    uint32_t audio_buffered_ms;
    uint32_t audio_jitter_boost_ms;
    uint32_t audio_target_delay_ms;
    uint32_t audio_prebuffer_ms;
    uint32_t audio_arrival_jitter_ms;
    uint32_t audio_peak_timing_error_ms;
    uint8_t audio_condition;
    size_t audio_pcm_used_bytes;
    size_t audio_pcm_capacity;
} media_sink_stats_t;

esp_err_t media_sink_init(void);
void media_sink_set_audio_profile(media_sink_audio_profile_t profile);
esp_err_t media_sink_submit_remote_audio(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         uint32_t source_timestamp_ms);
esp_err_t media_sink_submit_remote_audio_owned(uint8_t *data,
                                               size_t data_len,
                                               const audio_format_t *format,
                                               uint32_t source_timestamp_ms);
void media_sink_flush(void);
void media_sink_get_stats(media_sink_stats_t *stats);
