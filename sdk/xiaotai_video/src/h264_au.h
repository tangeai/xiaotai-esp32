// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool annex_b, valid, sps, pps, idr, constrained_baseline, idr_with_headers;
    uint8_t profile, constraints, level;
    uint64_t sps_hash, pps_hash;
} video_h264_au_t;

typedef struct {
    bool waiting_key, unsupported, sps_ready, pps_ready;
    uint64_t sps_hash, pps_hash;
} video_h264_chain_t;

typedef enum {
    VIDEO_H264_ADMIT = 0,
    VIDEO_H264_BAD_FORMAT,
    VIDEO_H264_WAIT_KEY,
    VIDEO_H264_UNSUPPORTED,
} video_h264_admit_t;

typedef struct {
    video_h264_admit_t status;
    bool key, reference_changed, recovered;
} video_h264_admission_t;

/* One bounded Annex-B access unit. Never allocates or reads past size. */
video_h264_au_t video_h264_inspect(const uint8_t *data, size_t size);

/* Owner-task reference gate. Header-only AUs can feed the decoder while
 * waiting; picture AUs need an established chain or a key with both sets.
 * An unsupported stream can only recover on a compatible SPS, when enabled. */
void video_h264_chain_reset(video_h264_chain_t *chain);
video_h264_admission_t video_h264_admit(video_h264_chain_t *chain, const video_h264_au_t *au,
                                        bool link_key, bool baseline_only, bool recover_profile,
                                        bool preserve_parameter_chain);
