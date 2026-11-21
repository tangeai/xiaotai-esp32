// SPDX-License-Identifier: MIT
#pragma once
#include "codec.h"

typedef enum {
    FAKE_DESCRIBE, FAKE_START, FAKE_STOP, FAKE_DROP_INPUT, FAKE_READ, FAKE_WRITE,
    FAKE_OPEN_OUTPUT, FAKE_OPEN_INPUT, FAKE_CLOSE_OUTPUT, FAKE_CLOSE_INPUT,
    FAKE_CHECK_CLOCK, FAKE_GAIN, FAKE_VOLUME, FAKE_MUTE, FAKE_AMP, FAKE_PAUSE, FAKE_RESUME, FAKE_CALLS
} xt_audio_fake_call_t;

typedef struct { xt_audio_fake_call_t call; int32_t value; } xt_audio_fake_record_t;

void xt_audio_fake_clear(void);
void xt_audio_fake_fail(xt_audio_fake_call_t call, xiaotai_err_t err, bool partial);
xiaotai_err_t xt_audio_fake_call(xt_audio_fake_call_t call, int32_t value);
size_t xt_audio_fake_log(xt_audio_fake_record_t *out, size_t capacity);
void xt_audio_fake_codec(xiaotai_audio_codec_t *codec, uint8_t inputs);
void xt_audio_fake_overflow(int port);
void xt_audio_fake_receive(int port, const void *dma, size_t bytes);
void xt_audio_fake_short_io(bool read, bool write);
size_t xt_audio_fake_last_write(void *out, size_t capacity);
