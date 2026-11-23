// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media_capture.h"

/* Capture owns feed(), the frontend worker owns feed_next(), and the clean
 * PCM owner calls fetch(). A successful vendor feed exposes one or two PCM
 * frames; metadata can become visible just before or just after the PCM. */
typedef struct media_frontend_pairing media_frontend_pairing_t;

typedef struct {
    int (*feed)(void *ctx, const int16_t *packed_pcm); /* >0 accepted, 0 ring full, <0 fatal */
    xiaotai_err_t (*fetch)(void *ctx, const int16_t **pcm, size_t *samples, uint32_t timeout_ms);
    void *ctx;
} media_frontend_ops_t;

typedef struct {
    const xiaotai_audio_desc_t *input;
    uint16_t feed_samples, fetch_samples;
    uint8_t microphones, input_slots, ring_frames;
    uint8_t stamp_wait_ms; /* at most 20; 0 with strict_feed matches the old immediate check */
    bool strict_feed;     /* publish stamps before feed; zero return permanently fails */
    bool count_monitor_clipping; /* include monitor lanes in mic statistics, never in AFE input */
} media_frontend_pairing_config_t;

/* The frontend itself belongs to its adapter; pairing close does not free it. */
xiaotai_err_t media_frontend_pairing_open(const media_frontend_pairing_config_t *config,
                                          const media_frontend_ops_t *ops, media_frontend_pairing_t **out);
void media_frontend_pairing_close(media_frontend_pairing_t *pairing);
xiaotai_err_t media_frontend_pairing_feed(media_frontend_pairing_t *pairing, const void *data, size_t bytes,
                                          int64_t captured_us, uint32_t lost_samples);
/* Process one slot. TIMEOUT means no slot; NO_MEM means the vendor ring dropped this slot. */
xiaotai_err_t media_frontend_pairing_feed_next(media_frontend_pairing_t *pairing, uint32_t timeout_ms);
xiaotai_err_t media_frontend_pairing_fetch(media_frontend_pairing_t *pairing, xiaotai_processor_frame_t *frame,
                                           uint32_t timeout_ms);
void media_frontend_pairing_stats(media_frontend_pairing_t *pairing, xiaotai_processor_stats_t *stats);

/* Match exactly the offered microphone count with one real reference. A
 * monitor is never promoted to a microphone. Pairs are routed MIC...REF. */
bool media_frontend_pairing_slots(const xiaotai_audio_desc_t *input, uint8_t microphones, uint8_t *slots);
