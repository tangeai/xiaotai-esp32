// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media.h"

typedef enum {
    MEDIA_PACER_ADAPTIVE = 0,
    MEDIA_PACER_TIMED = 1,
    MEDIA_PACER_PACKET_PREFILL = 2,
    MEDIA_PACER_FIXED = 3,
} media_pacer_policy_t;

typedef struct {
    xiaotai_media_latency_t profile;
    media_pacer_policy_t policy;
    uint32_t target_ms, jitter_q8, peak_ms, quiet_windows;
    uint32_t last_ts, last_duration_us, jitter_us, prefill_packets, required_prefill_packets;
    int64_t last_arrival_us, window_us;
    int64_t prefill_since_us, last_adjust_us, stable_since_us, started_us, last_written_us;
    uint8_t timed_run;
    int8_t rate_delta;
    bool have_arrival, started, pressure, have_prefill, have_written;
} media_pacer_t;

void media_pacer_open(media_pacer_t *pacer, xiaotai_media_latency_t profile, uint32_t capacity_ms,
                      media_pacer_policy_t policy);
void media_pacer_set_prefill_packets(media_pacer_t *pacer, int32_t packets);
void media_pacer_arrival(media_pacer_t *pacer, uint32_t timestamp_ms, int64_t received_us,
                         uint32_t duration_us, bool new_stream);
void media_pacer_tick(media_pacer_t *pacer, int64_t now_us, bool keeping_up, bool local_pressure);
bool media_pacer_start(media_pacer_t *pacer, uint32_t queued_ms, uint32_t queued_packets, int64_t now_us,
                       bool armed, bool draining);
void media_pacer_written(media_pacer_t *pacer, int64_t now_us);
bool media_pacer_on_empty(media_pacer_t *pacer, int64_t now_us);
uint32_t media_pacer_empty_wait(const media_pacer_t *pacer, int64_t now_us);
void media_pacer_underrun(media_pacer_t *pacer);
int media_pacer_speed(const media_pacer_t *pacer, uint32_t queued_ms, uint32_t output_ms, uint32_t block_ms);
int media_pacer_timed_delta(media_pacer_t *pacer, uint32_t queued_ms, int64_t now_us,
                            size_t available_samples, size_t block_samples);
bool media_pacer_short_tail(const media_pacer_t *pacer, int64_t now_us, int64_t first_pending_us,
                            int64_t last_arrival_us, uint32_t output_ms, uint32_t block_ms, uint32_t nominal_ms);
