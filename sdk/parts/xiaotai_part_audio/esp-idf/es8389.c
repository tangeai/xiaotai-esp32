// SPDX-License-Identifier: MIT
#include "codec_native.h"
#include "es8389_codec.h"
#include "sdkconfig.h"
#include "xiaotai/port/mem.h"

#if CONFIG_CODEC_ES8389_SUPPORT
typedef struct {
    xt_native_codec_t native;
    xiaotai_es8389_config_t config;
} es8389_t;

static xiaotai_err_t make(xt_native_codec_t *c)
{
    const xiaotai_es8389_config_t *p = &((es8389_t *)c)->config;
    es8389_codec_cfg_t cfg = {
        .ctrl_if = &c->control, .pa_pin = -1, .master_mode = false,
        .codec_mode = p->capture && p->playback ? ESP_CODEC_DEV_WORK_MODE_BOTH :
            p->capture ? ESP_CODEC_DEV_WORK_MODE_ADC : ESP_CODEC_DEV_WORK_MODE_DAC,
        /* The DAC loopback makes capture four slots: MIC1, reference, MIC2, unused. */
        .no_dac_ref = !p->reference,
        .use_mclk = p->mclk_multiple != 0, .mclk_div = p->mclk_multiple,
        .hw_gain = {.pa_voltage = (p->amplifier_mv ? p->amplifier_mv : 5000U) / 1000.0f,
                    .codec_dac_voltage = (p->dac_mv ? p->dac_mv : 3300U) / 1000.0f},
    };
    c->driver = es8389_codec_new(&cfg);
    return xt_native_codec_result(c, c->driver ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM);
}

/* esp_codec_dev 1.6.2 maps the DAC mute() to the ADC serial port (0x20), so
 * muting the speaker silenced the microphones and left the DAC playing. The
 * DAC serial port's mute bits are 0x40[1:0]; the vendor driver keeps them
 * across its format and clock updates. */
enum { ES8389_DAC_SERIAL_PORT = 0x40, ES8389_SERIAL_PORT_MUTE = 0x03 };

static xiaotai_err_t mute_output(xt_native_codec_t *c, bool muted)
{
    const audio_codec_if_t *d = c->driver;
    int value = 0;
    xiaotai_err_t err = xt_native_codec_result(c, d->get_reg(d, ES8389_DAC_SERIAL_PORT, &value));
    if (err != XIAOTAI_OK) return err;
    value = muted ? (value | ES8389_SERIAL_PORT_MUTE) : (value & ~ES8389_SERIAL_PORT_MUTE);
    return xt_native_codec_result(c, d->set_reg(d, ES8389_DAC_SERIAL_PORT, value & 0xff));
}
#endif

xiaotai_err_t xiaotai_es8389_create(const xiaotai_es8389_config_t *p, xiaotai_audio_codec_t **codec)
{
    if (codec) *codec = NULL;
    if (!codec || !p || !p->bus || (!p->capture && !p->playback) || p->address > 0x7f ||
        (p->inputs & ~0x03U) || (!p->capture && (p->reference || p->inputs)))
        return XIAOTAI_ERR_INVALID_ARG;
#if CONFIG_CODEC_ES8389_SUPPORT
    es8389_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) return XIAOTAI_ERR_NO_MEM;
    const uint8_t caps = (p->capture ? XT_NATIVE_IN : 0) | (p->playback ? XT_NATIVE_OUT : 0);
    const uint8_t inputs = p->capture ? (p->inputs ? p->inputs : 0x03U) : 0;
    const xiaotai_err_t err = xt_native_codec_init(&c->native, p->bus,
        p->address ? p->address : ES8389_CODEC_DEFAULT_ADDR >> 1, caps, inputs, p->i2c_hz);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(c);
        return err;
    }
    /* Reset, mode, clock source and manager, ADC/DAC serial ports (mute and
     * format), reference routing, DAC control and volume, analog power and
     * output stage, PGA, chip misc. */
    static const uint8_t registers[] = {0x00, 0x01, 0x02, 0x03, 0x10, 0x20, 0x23, 0x40, 0x44, 0x46, 0x47,
                                        0x49, 0x4d, 0x61, 0x64, 0x69, 0x72, 0x73, 0xf0};
    c->native.name = "ES8389";
    c->native.make = make;
    c->native.mute_output = mute_output;
    c->native.registers = registers;
    c->native.register_count = sizeof(registers);
    c->config = *p;
    *codec = &c->native.part;
    return XIAOTAI_OK;
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
