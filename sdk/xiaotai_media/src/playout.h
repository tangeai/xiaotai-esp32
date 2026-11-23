// SPDX-License-Identifier: MIT
#pragma once

#include "internal.h"

typedef struct media_playout media_playout_t;

typedef struct {
    uint32_t generation, epoch;
    const xiaotai_media_session_t *session;
    bool armed_audio, draining, mixing, capture_clock;
    bool (*current)(uint32_t generation, uint32_t epoch);
    uint32_t (*mix)(int16_t *mono, size_t frames);
    void (*mixed_written)(uint32_t token, xiaotai_err_t status);
    void (*far_end)(const int16_t *mono, size_t frames, uint32_t rate);
} media_playout_request_t;

xiaotai_err_t media_playout_create(const xiaotai_audio_output_t *output, xiaotai_downlink_t *ring,
                                  uint32_t downlink_ms, media_playout_t **player);
void media_playout_destroy(media_playout_t *player);
void media_playout_reset(media_playout_t *player, uint32_t generation, uint32_t epoch,
                         xiaotai_media_latency_t latency);
uint32_t media_playout_pass(media_playout_t *player, const media_playout_request_t *request, bool *drained);
void media_playout_stats(media_playout_t *player, xiaotai_media_stats_t *stats);
