// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media.h"

typedef struct {
    xiaotai_media_info_t info;
    xiaotai_media_status_t status;
    xiaotai_media_record_t record;
    xiaotai_media_play_t play;
    xiaotai_err_t record_error, play_error;
    uint32_t serial;
    unsigned records, plays, record_stops, play_stops;
} companion_audio_fixture_t;

extern companion_audio_fixture_t audio_fixture;
void companion_fixture_record_done(xiaotai_err_t result, uint32_t samples);
void companion_fixture_play_done(xiaotai_err_t result);
