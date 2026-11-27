// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/media.h"
#include "xiaotai/platform.h"

typedef struct {
    uint32_t id;
    xiaotai_media_play_fn_t done;
    void *ctx;
} fake_play_t;

extern struct fake_audio {
    bool playback, other_urgent;
    xiaotai_err_t refuse, write_refuse, finish_refuse;
    unsigned clips, streams, stops, finishes, writes;
    uint32_t stopped_id;
    xiaotai_media_play_t clip;
    xiaotai_media_stream_t stream;
    fake_play_t play;
    uint8_t bytes[4096];
    size_t size;
} fake_audio;

extern struct fake_tts {
    xiaotai_err_t refuse;
    unsigned submissions, cancels;
    bool pending;
    xiaotai_platform_transfer_t req;
    xiaotai_platform_stream_t stream;
    char path[64], bearer[64];
} fake_tts;

void fake_audio_reset(void);
void fake_audio_done(fake_play_t play, xiaotai_err_t result);
xiaotai_err_t fake_tts_head(int status, const char *type, int64_t length, bool repeated);
xiaotai_err_t fake_tts_write(const uint8_t *data, size_t size);
void fake_tts_done(xiaotai_err_t result, int status);
