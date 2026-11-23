// SPDX-License-Identifier: MIT
#pragma once

#include "internal.h"
#include "xiaotai/media_uplink.h"

#define MEDIA_COMPACT_CAPTURE_STACK_KEY "task.capture.compact_stack"
#define MEDIA_UPLINK_MAX_QUEUE_AGE_KEY "uplink.max_queue_age_ms"

typedef struct {
    xiaotai_media_session_t session;
    uint32_t uplink_epoch, mic_epoch;
    int64_t tail_end_us, tail_deadline_us, mic_cutover_us;
    uint8_t volume;
    int8_t gain_db, gain_applied_db;
    bool active, gate, muted, preroll;
} media_capture_view_t;

typedef struct {
    const xiaotai_audio_encoder_t *encoder;
    void *state;
} media_tx_codec_t;

bool media_tx_codec(const xiaotai_media_format_t *format, media_tx_codec_t *out);
uint32_t media_max_encoder_stack(void);
uint32_t media_uplink_sink_stack(void);
bool media_prepared_uplink(void);
bool media_prepared_uplink_needs_resample(uint32_t input_rate_hz);
bool media_prepared_capture_safe(void);
size_t media_max_uplink_packet(void);
size_t media_max_uplink_samples(void);
const xiaotai_audio_decoder_t *media_capture_decoder(void **state);
const xiaotai_audio_processor_t *media_capture_prepared(void **state, xiaotai_processor_info_t *info);

typedef struct media_audio_workers media_audio_workers_t;

xiaotai_err_t media_audio_workers_create(const xiaotai_audio_input_t *input, media_topology_t topology,
                                         bool delegated, media_audio_workers_t **out);
uint64_t media_audio_folded_stack(const media_audio_workers_t *workers, uint32_t decoder_stack);
/* True while the single capture/playout task is clocked by an input read. */
bool media_audio_folded_pass(media_audio_workers_t *workers);
void media_audio_workers_start(media_audio_workers_t *workers);
void media_audio_workers_pause(media_audio_workers_t *workers, bool pause);
bool media_audio_workers_parked(const media_audio_workers_t *workers);
void media_audio_workers_stop(media_audio_workers_t *workers);
void media_audio_workers_destroy(media_audio_workers_t *workers);
void media_audio_far_end(media_audio_workers_t *workers, const int16_t *pcm, size_t frames, uint32_t rate);
void media_audio_stats(media_audio_workers_t *workers, xiaotai_media_stats_t *stats);
void media_audio_tx_reset(media_audio_workers_t *workers, uint32_t generation);
void media_audio_control_stats(media_audio_workers_t *workers, xiaotai_media_status_t *status);
void media_audio_tone(media_audio_workers_t *workers, uint32_t generation, uint32_t ms);
bool media_audio_uplink_delegated(const media_audio_workers_t *workers);

/* Dequeue/lease under the runtime lock. Send and release outside that lock. */
typedef struct {
    const xiaotai_media_sink_t *sink;
    uint32_t generation, epoch;
    uint16_t packet_ms;
    bool eligible, gate, tail;
    int64_t tail_end_us, tail_deadline_us;
} media_uplink_view_t;
bool media_audio_uplink_claim(media_audio_workers_t *workers);
bool media_audio_uplink_finish(media_audio_workers_t *workers, const media_uplink_view_t *view);
void media_runtime_uplink_notify(void);
bool media_runtime_uplink_is_executor(void);

/* Short runtime-lock snapshots; called from capture/uplink owners only. */
void media_runtime_capture_view(media_capture_view_t *view);
bool media_runtime_uplink_allowed(uint32_t generation, uint32_t epoch, int64_t frame_end_us, bool *tail);
void media_runtime_input_failed(void);
void media_runtime_gain_applied(int8_t gain, xiaotai_err_t error);
