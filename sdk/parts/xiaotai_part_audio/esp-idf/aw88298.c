// SPDX-License-Identifier: MIT
#include "codec_native.h"
#include "aw88298_dac.h"
#include "sdkconfig.h"
#include "xiaotai/port/os.h"

#if CONFIG_CODEC_AW88298_SUPPORT
typedef struct {
    xt_native_codec_t native;
    xiaotai_line_t reset;
    uint8_t bit_clocks;
} aw88298_t;

static xiaotai_err_t make(xt_native_codec_t *c)
{
    const xiaotai_line_t *reset = &((aw88298_t *)c)->reset;
    if (reset->set) {
        xiaotai_err_t err = xiaotai_line_set(reset, true);
        if (err != XIAOTAI_OK) return err;
        xiaotai_delay_ms(10);
        err = xiaotai_line_set(reset, false);
        if (err != XIAOTAI_OK) return err;
        xiaotai_delay_ms(50);
    }
    aw88298_codec_cfg_t cfg = {.ctrl_if = &c->control, .reset_pin = -1};
    c->driver = aw88298_codec_new(&cfg);
    return xt_native_codec_result(c, c->driver ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM);
}

static xiaotai_err_t clock_check(xt_native_codec_t *c, xt_audio_io_t *io)
{
    enum { I2SCTRL = 0x06, I2SBCK_MASK = 0x30, I2SBCK_64FS = 0x20 };
    const unsigned bits = ((const aw88298_t *)c)->bit_clocks;
    if (!bits) return XIAOTAI_OK;
    if (bits != xt_audio_io_bit_clocks(io)) return XIAOTAI_FAIL;
    const audio_codec_if_t *d = c->driver;
    int value;
    if (xt_native_codec_result(c, d->get_reg(d, I2SCTRL, &value)) != XIAOTAI_OK) return XIAOTAI_FAIL;
    const int expected = (value & ~I2SBCK_MASK) | (bits == 64U ? I2SBCK_64FS : 0);
    if (xt_native_codec_result(c, d->set_reg(d, I2SCTRL, expected)) != XIAOTAI_OK) return XIAOTAI_FAIL;
    return xt_native_codec_result(c, d->get_reg(d, I2SCTRL, &value)) == XIAOTAI_OK && value == expected ?
        XIAOTAI_OK : XIAOTAI_FAIL;
}
#endif

xiaotai_err_t xiaotai_aw88298_create(const xiaotai_aw88298_config_t *p, xiaotai_audio_codec_t **codec)
{
    if (codec) *codec = NULL;
    if (!codec || !p || !p->bus || p->address > 0x7f || (p->reset.get && !p->reset.set)) return XIAOTAI_ERR_INVALID_ARG;
    if (p->bit_clocks_per_frame && p->bit_clocks_per_frame != 32 && p->bit_clocks_per_frame != 64)
        return XIAOTAI_ERR_NOT_SUPPORTED;
#if CONFIG_CODEC_AW88298_SUPPORT
    aw88298_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = xt_native_codec_init(&c->native, p->bus,
        p->address ? p->address : AW88298_CODEC_DEFAULT_ADDR >> 1, XT_NATIVE_OUT, 0, 0);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(c);
        return err;
    }
    static const uint8_t registers[] = {0x04, 0x05, 0x06, 0x0c};
    c->native.name = "AW88298";
    c->native.make = make;
    c->native.clock = clock_check;
    c->native.registers = registers;
    c->native.register_count = sizeof(registers);
    c->reset = p->reset;
    c->bit_clocks = p->bit_clocks_per_frame;
    *codec = &c->native.part;
    return XIAOTAI_OK;
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
