// SPDX-License-Identifier: MIT
#pragma once
#include "codec.h"
#include "audio_codec_ctrl_if.h"
#include "audio_codec_if.h"
#include "driver/i2c_master.h"

typedef struct xt_native_codec xt_native_codec_t;
struct xt_native_codec {
    xiaotai_audio_codec_t part;
    audio_codec_ctrl_if_t control;
    xiaotai_i2c_bus_t *bus;
    i2c_master_dev_handle_t device;
    const audio_codec_if_t *driver;
    xiaotai_err_t (*make)(xt_native_codec_t *codec);
    xiaotai_err_t (*clock)(xt_native_codec_t *codec, xt_audio_io_t *io);
    /* Optional: mutes the DAC output when the vendor driver's mute() does not
     * (it must leave capture running). NULL: the vendor mute() is used. */
    xiaotai_err_t (*mute_output)(xt_native_codec_t *codec, bool muted);
    const char *name;
    const uint8_t *registers;
    size_t register_count;
    xt_audio_io_t *owner;
    uint32_t rate_hz;
    xiaotai_err_t io_error;
    uint8_t address, bus_id, directions, opened;
    bool wrote_register, rollback, unknown_state;
};

enum { XT_NATIVE_IN = 1U << XT_AUDIO_INPUT, XT_NATIVE_OUT = 1U << XT_AUDIO_OUTPUT };

extern const xt_audio_codec_ops_t xt_native_codec_ops;

xiaotai_err_t xt_native_codec_init(xt_native_codec_t *codec, xiaotai_i2c_bus_t *bus, uint8_t address,
                                   uint8_t directions, uint8_t gain_inputs, uint32_t i2c_hz);
xiaotai_err_t xt_native_codec_result(xt_native_codec_t *codec, int result);
uint8_t xt_audio_io_wire_slots(const xt_audio_io_t *io, xt_audio_direction_t direction);
uint16_t xt_audio_io_mclk(const xt_audio_io_t *io);
unsigned xt_audio_io_bit_clocks(const xt_audio_io_t *io);
