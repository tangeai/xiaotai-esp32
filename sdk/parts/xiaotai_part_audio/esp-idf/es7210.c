// SPDX-License-Identifier: MIT
#include "codec_native.h"
#include "es7210_adc.h"
#include "sdkconfig.h"
#include "xiaotai/port/mem.h"

#if CONFIG_CODEC_ES7210_SUPPORT
typedef struct {
    xt_native_codec_t native;
    uint16_t mclk_multiple;
} es7210_t;

static xiaotai_err_t make(xt_native_codec_t *c)
{
    es7210_codec_cfg_t cfg = {.ctrl_if = &c->control, .mic_selected = c->part.gain_inputs,
        .mclk_src = ES7210_MCLK_FROM_PAD, .mclk_div = ((es7210_t *)c)->mclk_multiple};
    c->driver = es7210_codec_new(&cfg);
    return xt_native_codec_result(c, c->driver ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM);
}
#endif

xiaotai_err_t xiaotai_es7210_create(const xiaotai_es7210_config_t *p, xiaotai_audio_codec_t **codec)
{
    if (codec) *codec = NULL;
    if (!codec || !p || !p->bus || !p->inputs || (p->inputs & ~0x0fU) || p->address > 0x7f)
        return XIAOTAI_ERR_INVALID_ARG;
#if CONFIG_CODEC_ES7210_SUPPORT
    es7210_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_err_t err = xt_native_codec_init(&c->native, p->bus,
        p->address ? p->address : ES7210_CODEC_DEFAULT_ADDR >> 1, XT_NATIVE_IN, p->inputs, 0);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(c);
        return err;
    }
    static const uint8_t registers[] = {0x00, 0x01, 0x02, 0x08, 0x11, 0x12, 0x43, 0x44, 0x45, 0x46};
    c->native.name = "ES7210";
    c->native.make = make;
    c->native.registers = registers;
    c->native.register_count = sizeof(registers);
    c->mclk_multiple = p->mclk_multiple;
    *codec = &c->native.part;
    return XIAOTAI_OK;
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
