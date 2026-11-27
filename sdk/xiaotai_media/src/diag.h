// SPDX-License-Identifier: MIT
#pragma once

#include "level.h"
#include "xiaotai/at_command.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"

#define MEDIA_DIAG_CAPTURE_FRAMES 32U

typedef struct {
    uint16_t rms, rms_max, peak;
    int16_t dc;
} media_diag_level_window_t;

xiaotai_err_t media_diag_register(void);
void media_diag_detach(void);
xiaotai_err_t media_commands_register(void);
void media_diag_audio(const xiaotai_reply_t *reply);
void media_diag_input(const xiaotai_audio_input_t *input);
void media_diag_capture(const xiaotai_audio_input_t *input, const int16_t *pcm, size_t frames);
/* Windowed capture levels, in this order: microphone, reference, monitor
 * (raw, by role), clean (after the capture effects), processed (the
 * processor's output before the capture effects). */
enum { MEDIA_DIAG_LEVEL_MIC, MEDIA_DIAG_LEVEL_REFERENCE, MEDIA_DIAG_LEVEL_MONITOR, MEDIA_DIAG_LEVEL_CLEAN,
       MEDIA_DIAG_LEVEL_PROCESSED, MEDIA_DIAG_LEVELS };
void media_diag_capture_raw_levels(const xiaotai_media_level_t levels[3]);
void media_diag_capture_clean_level(const xiaotai_media_level_t *level);
void media_diag_capture_processed_level(const xiaotai_media_level_t *level);
bool media_diag_capture_levels_snapshot(media_diag_level_window_t levels[MEDIA_DIAG_LEVELS]);
void media_diag_capture_window(const xiaotai_media_stats_t *stats, const int32_t dc[4]);
void media_diag_playout_window(const xiaotai_media_stats_t *stats, uint32_t peak, uint32_t near_full,
                                uint32_t step_max, uint32_t resample_clips);
void media_diag_uplink_window(const xiaotai_media_stats_t *stats);
bool media_diag_audio_active(void);
void media_diag_diagnostic_begin(bool recording, uint32_t id);
void media_diag_diagnostic_end(bool recording, uint32_t id);

xiaotai_err_t media_at_test_register(void);
void media_at_test_detach(void);
xiaotai_err_t media_at_sound_register(void);
void media_at_sound_detach(void);
