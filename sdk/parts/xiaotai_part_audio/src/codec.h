// SPDX-License-Identifier: MIT
#pragma once
#include "audio_io.h"

typedef enum { XT_AUDIO_INPUT, XT_AUDIO_OUTPUT } xt_audio_direction_t;

typedef struct {
    /* Report partial ownership the same way as port start. An output opens
     * muted. A shared chip handles per-direction opens/closes, including when
     * one underlying handle implements both directions. */
    xiaotai_err_t (*open)(void *self, xt_audio_io_t *io, xt_audio_direction_t direction,
                         const xiaotai_audio_desc_t *format, bool *opened);
    xiaotai_err_t (*close)(void *self, xt_audio_direction_t direction);
    /* Once per chip, after both directions' clocks and chips are running. */
    xiaotai_err_t (*check_clock)(void *self, xt_audio_io_t *io);
    xiaotai_err_t (*gain)(void *self, uint8_t input, int8_t db);
    xiaotai_err_t (*volume)(void *self, uint8_t percent);
    xiaotai_err_t (*mute)(void *self, bool muted);
    void (*report)(void *self, xiaotai_hal_write_fn_t write, void *ctx);
    uint16_t volume_span_cdb;
} xt_audio_codec_ops_t;

struct xiaotai_audio_codec {
    const xt_audio_codec_ops_t *ops;
    void *self;
    uint8_t gain_inputs; /* per-chip input bits initialized on every start */
};
