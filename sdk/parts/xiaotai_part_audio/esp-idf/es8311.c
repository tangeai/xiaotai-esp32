// SPDX-License-Identifier: MIT
#include "codec_native.h"
#include "es8311_codec.h"
#include "sdkconfig.h"
#include "xiaotai/port/mem.h"

#if CONFIG_CODEC_ES8311_SUPPORT
typedef struct {
    xt_native_codec_t native;
    xiaotai_es8311_config_t config;
} es8311_t;

static xiaotai_err_t make(xt_native_codec_t *c)
{
    const xiaotai_es8311_config_t *p = &((es8311_t *)c)->config;
    es8311_codec_cfg_t cfg = {
        .ctrl_if = &c->control, .pa_pin = -1,
        .no_dac_ref = !(p->reference || p->route_dac_to_unused_slot),
        .codec_mode = p->capture && p->playback ? ESP_CODEC_DEV_WORK_MODE_BOTH :
            p->capture ? ESP_CODEC_DEV_WORK_MODE_ADC : ESP_CODEC_DEV_WORK_MODE_DAC,
        .use_mclk = p->mclk_multiple != 0, .mclk_div = p->mclk_multiple,
        .hw_gain = {.pa_voltage = (p->amplifier_mv ? p->amplifier_mv : 5000U) / 1000.0f,
                    .codec_dac_voltage = (p->dac_mv ? p->dac_mv : 3300U) / 1000.0f},
    };
    c->driver = es8311_codec_new(&cfg);
    return xt_native_codec_result(c, c->driver ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM);
}
#endif

xiaotai_err_t xiaotai_es8311_create(const xiaotai_es8311_config_t *p, xiaotai_audio_codec_t **codec)
{
    if (codec) *codec = NULL;
    if (!codec || !p || !p->bus || (!p->capture && !p->playback) || p->address > 0x7f ||
        (!p->capture && (p->reference || p->route_dac_to_unused_slot)))
        return XIAOTAI_ERR_INVALID_ARG;
#if CONFIG_CODEC_ES8311_SUPPORT
    es8311_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) return XIAOTAI_ERR_NO_MEM;
    const uint8_t caps = (p->capture ? XT_NATIVE_IN : 0) | (p->playback ? XT_NATIVE_OUT : 0);
    const xiaotai_err_t err = xt_native_codec_init(&c->native, p->bus,
        p->address ? p->address : ES8311_CODEC_DEFAULT_ADDR >> 1, caps, p->capture ? 1 : 0, p->i2c_hz);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(c);
        return err;
    }
    static const uint8_t registers[] = {0x00, 0x01, 0x06, 0x09, 0x0a, 0x16, 0x31, 0x32, 0x44};
    c->native.name = "ES8311";
    c->native.make = make;
    c->native.registers = registers;
    c->native.register_count = sizeof(registers);
    c->config = *p;
    *codec = &c->native.part;
    return XIAOTAI_OK;
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
