// SPDX-License-Identifier: MIT
#include "codec_native.h"
#include "driver/i2c_master.h"
#include "soc/soc_caps.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { I2C_TIMEOUT_MS = 100, PRIVATE_BUS_ID_BASE = 16 };

/* The vendor's chip reference table keys by (bus, address). Its port number
 * is only an equality key for this custom control, so use the SDK bus's stable
 * identity without probing every hardware controller (and logging failures). */
static xiaotai_i2c_bus_t *buses[SOC_I2C_NUM];

static xt_native_codec_t *from_control(const audio_codec_ctrl_if_t *control)
{
    return (xt_native_codec_t *)((uint8_t *)control - offsetof(xt_native_codec_t, control));
}

static bool available(const audio_codec_ctrl_if_t *control) { return control != NULL; }

static int bus_result(xt_native_codec_t *c, xiaotai_err_t err, int vendor_error)
{
    if (err == XIAOTAI_OK) return ESP_CODEC_DEV_OK;
    if (c->io_error == XIAOTAI_OK) c->io_error = err;
    return vendor_error;
}

static int read_reg(const audio_codec_ctrl_if_t *control, int reg, int reg_len, void *data, int len)
{
    if (!data || reg_len != 1 || reg < 0 || reg > UINT8_MAX || len <= 0) return ESP_CODEC_DEV_INVALID_ARG;
    xt_native_codec_t *c = from_control(control);
    const uint8_t address = (uint8_t)reg;
    const xiaotai_err_t err = c->device ? i2c_master_transmit_receive(c->device, &address, 1, data, (size_t)len, I2C_TIMEOUT_MS) :
        xiaotai_i2c_read(c->bus, c->address, address, data, (size_t)len);
    return bus_result(c, err, ESP_CODEC_DEV_READ_FAIL);
}

static int write_reg(const audio_codec_ctrl_if_t *control, int reg, int reg_len, void *data, int len)
{
    if (!data || reg_len != 1 || reg < 0 || reg > UINT8_MAX || len <= 0) return ESP_CODEC_DEV_INVALID_ARG;
    xt_native_codec_t *c = from_control(control);
    xiaotai_err_t err;
    if (c->device) {
        uint8_t transaction[3] = {(uint8_t)reg};
        if ((size_t)len > sizeof(transaction) - 1U) return ESP_CODEC_DEV_INVALID_ARG;
        memcpy(transaction + 1, data, (size_t)len);
        err = i2c_master_transmit(c->device, transaction, 1U + (size_t)len, I2C_TIMEOUT_MS);
    } else err = xiaotai_i2c_write(c->bus, c->address, (uint8_t)reg, data, (size_t)len);
    if (err != XIAOTAI_ERR_NOT_FOUND && err != XIAOTAI_ERR_INVALID_ARG) c->wrote_register = true;
    return bus_result(c, err, ESP_CODEC_DEV_WRITE_FAIL);
}

static int info(const audio_codec_ctrl_if_t *control, audio_codec_ctrl_info_t *out)
{
    const xt_native_codec_t *c = from_control(control);
    *out = (audio_codec_ctrl_info_t){.type = AUDIO_CODEC_CTRL_I2C,
        .i2c = {.addr = c->address << 1, .port = c->bus_id}};
    return ESP_CODEC_DEV_OK;
}

xiaotai_err_t xt_native_codec_init(xt_native_codec_t *c, xiaotai_i2c_bus_t *bus, uint8_t address,
                                   uint8_t directions, uint8_t gain_inputs, uint32_t i2c_hz)
{
    const i2c_master_bus_handle_t handle = xiaotai_i2c_native(bus);
    if (!handle || !address || address > 0x7fU) return XIAOTAI_ERR_INVALID_ARG;
    size_t slot = 0;
    while (slot < SOC_I2C_NUM && buses[slot] && buses[slot] != bus) ++slot;
    if (slot == SOC_I2C_NUM) return XIAOTAI_ERR_INVALID_STATE;
    if (i2c_hz) {
        const i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address, .scl_speed_hz = i2c_hz};
        const esp_err_t err = i2c_master_bus_add_device(handle, &config, &c->device);
        if (err != ESP_OK) return err;
    }
    buses[slot] = bus;
    c->control = (audio_codec_ctrl_if_t){.is_open = available, .read_reg = read_reg,
                                         .write_reg = write_reg, .get_info = info};
    c->part = (xiaotai_audio_codec_t){.ops = &xt_native_codec_ops, .self = c, .gain_inputs = gain_inputs};
    c->bus = bus;
    c->address = address;
    c->bus_id = (uint8_t)(PRIVATE_BUS_ID_BASE + slot);
    c->directions = directions;
    return XIAOTAI_OK;
}

xiaotai_err_t xt_native_codec_result(xt_native_codec_t *c, int result)
{
    const xiaotai_err_t wire = c->io_error;
    c->io_error = XIAOTAI_OK;
    if (wire != XIAOTAI_OK) return wire;
    switch (result) {
        case ESP_CODEC_DEV_OK:
        case ESP_CODEC_DEV_INVALID_ARG:
        case ESP_CODEC_DEV_WRONG_STATE:
        case ESP_CODEC_DEV_NOT_SUPPORT:
        case ESP_CODEC_DEV_NOT_FOUND:
        case ESP_CODEC_DEV_TIMEOUT:
        case ESP_CODEC_DEV_NO_MEM:
            return result;
        default:
            return XIAOTAI_FAIL;
    }
}

static bool can_mute_output(const xt_native_codec_t *c)
{
    return c->mute_output || (c->driver && c->driver->mute);
}

static xiaotai_err_t mute_output(xt_native_codec_t *c, bool muted)
{
    if (c->mute_output) return c->mute_output(c, muted);
    return xt_native_codec_result(c, c->driver->mute(c->driver, muted));
}

static xiaotai_err_t open_codec(void *self, xt_audio_io_t *io, xt_audio_direction_t direction,
                              const xiaotai_audio_desc_t *desc, bool *opened)
{
    xt_native_codec_t *c = self;
    const uint8_t bit = 1U << direction;
    *opened = false;
    if (!(c->directions & bit) || desc->encoding != XIAOTAI_CODEC_PCM16) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (c->opened) {
        if (c->owner != io || c->rate_hz != desc->rate_hz || c->rollback) return XIAOTAI_ERR_INVALID_STATE;
        c->opened |= bit;
        *opened = true;
        return XIAOTAI_OK;
    }
    if (!c->driver) {
        c->wrote_register = false;
        const xiaotai_err_t err = c->make(c);
        if (err != XIAOTAI_OK) {
            /* The vendor frees its handle on a failed constructor. Treat a
             * partially programmed chip without a handle as unknown state. */
            if (c->driver || c->wrote_register) {
                c->opened = bit;
                c->rollback = true;
                c->unknown_state = !c->driver;
                *opened = true;
            }
            return err;
        }
    }
    const audio_codec_if_t *d = c->driver;
    c->opened = bit;
    c->owner = io;
    c->rate_hz = desc->rate_hz;
    c->rollback = true;
    *opened = true;
    esp_codec_dev_sample_info_t format = {.bits_per_sample = 16, .channel = xt_audio_io_wire_slots(io, direction),
        .sample_rate = desc->rate_hz, .mclk_multiple = xt_audio_io_mclk(io)};
    xiaotai_err_t err = XIAOTAI_OK;
    const bool output = (c->directions & XT_NATIVE_OUT) && can_mute_output(c);
    if (output) err = mute_output(c, true);
    if (err == XIAOTAI_OK) err = xt_native_codec_result(c, d->set_fs(d, &format));
    if (err == XIAOTAI_OK) err = xt_native_codec_result(c, d->enable(d, true));
    /* Some vendor chips restore an unmuted DAC when enabled. Keep the port
     * on silent DMA and reassert mute before the device can publish output. */
    if (err == XIAOTAI_OK && output) err = mute_output(c, true);
    if (err == XIAOTAI_OK) c->rollback = false;
    return err;
}

static xiaotai_err_t close_codec(void *self, xt_audio_direction_t direction)
{
    xt_native_codec_t *c = self;
    const uint8_t bit = 1U << direction;
    if (!(c->opened & bit)) return XIAOTAI_OK;
    if (c->opened & ~bit) {
        c->opened &= (uint8_t)~bit;
        return XIAOTAI_OK;
    }
    if (c->unknown_state) return XIAOTAI_ERR_INVALID_STATE;
    const audio_codec_if_t *d = c->driver;
    xiaotai_err_t err = xt_native_codec_result(c, d->enable(d, false));
    if (c->rollback && err == XIAOTAI_OK) {
        err = xt_native_codec_result(c, d->close(d));
        if (err == XIAOTAI_OK) {
            err = audio_codec_delete_codec_if(d);
            c->driver = NULL;
            if (err != XIAOTAI_OK) c->unknown_state = true;
        }
    }
    if (err == XIAOTAI_OK) {
        c->opened = 0;
        c->owner = NULL;
        c->rollback = false;
    }
    return err;
}

static xiaotai_err_t check_clock(void *self, xt_audio_io_t *io)
{
    xt_native_codec_t *c = self;
    return c->clock ? c->clock(c, io) : XIAOTAI_OK;
}

static xiaotai_err_t gain(void *self, uint8_t input, int8_t db)
{
    xt_native_codec_t *c = self;
    const audio_codec_if_t *d = c->driver;
    if (!d || !(c->opened & XT_NATIVE_IN)) return XIAOTAI_ERR_INVALID_STATE;
    if (input >= XIAOTAI_AUDIO_CHANNELS_MAX || !(c->part.gain_inputs & (1U << input))) return XIAOTAI_ERR_INVALID_ARG;
    if (d->set_mic_channel_gain) return xt_native_codec_result(c, d->set_mic_channel_gain(d, 1U << input, db));
    if (d->set_mic_gain) return xt_native_codec_result(c, d->set_mic_gain(d, db));
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t volume(void *self, uint8_t percent)
{
    xt_native_codec_t *c = self;
    const audio_codec_if_t *d = c->driver;
    if (!d || !(c->opened & XT_NATIVE_OUT)) return XIAOTAI_ERR_INVALID_STATE;
    /* esp_codec_dev 1.6.2 default law: 1..100 % maps to -49.5..0 dB. */
    const float db = percent ? ((float)percent - 100.0f) * 0.5f : -96.0f;
    return d->set_vol ? xt_native_codec_result(c, d->set_vol(d, db)) : XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t mute(void *self, bool muted)
{
    xt_native_codec_t *c = self;
    const audio_codec_if_t *d = c->driver;
    if (!d || !(c->opened & XT_NATIVE_OUT)) return XIAOTAI_ERR_INVALID_STATE;
    return can_mute_output(c) ? mute_output(c, muted) : XIAOTAI_ERR_NOT_SUPPORTED;
}

static void report(void *self, xiaotai_hal_write_fn_t write, void *ctx)
{
    xt_native_codec_t *c = self;
    char line[72];
    (void)snprintf(line, sizeof(line), "%s addr=%02x open=%u", c->name, c->address, c->opened);
    write(ctx, line);
    if (!c->driver || !c->driver->get_reg) return;
    for (size_t i = 0; i < c->register_count; ++i) {
        const uint8_t reg = c->registers[i];
        int value = 0;
        const xiaotai_err_t err = xt_native_codec_result(c, c->driver->get_reg(c->driver, reg, &value));
        if (err != XIAOTAI_OK) {
            (void)snprintf(line, sizeof(line), "%s reg=%02x error=0x%x", c->name, reg, (unsigned)err);
            write(ctx, line);
            break;
        }
        (void)snprintf(line, sizeof(line), "%s reg=%02x value=%04x", c->name, reg, (unsigned)value);
        write(ctx, line);
    }
}

const xt_audio_codec_ops_t xt_native_codec_ops = {.open = open_codec, .close = close_codec,
    .check_clock = check_clock, .gain = gain, .volume = volume, .mute = mute, .report = report, .volume_span_cdb = 4950};
