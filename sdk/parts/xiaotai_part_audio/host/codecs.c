// SPDX-License-Identifier: MIT
#include "audio_io_fake.h"
#include "xiaotai/port/mem.h"

typedef struct host_codec {
    xiaotai_audio_codec_t codec;
    struct host_codec *next;
} host_codec_t;

/* Board factories have boot lifetime, as on the target. Host execution uses
 * the same PCM device logic, but the codec bus/register traffic is simulated. */
static host_codec_t *codecs;

static xiaotai_err_t create(uint8_t gain_inputs, xiaotai_audio_codec_t **out)
{
    host_codec_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*c));
    if (!c) return XIAOTAI_ERR_NO_MEM;
    xt_audio_fake_codec(&c->codec, gain_inputs);
    c->next = codecs;
    codecs = c;
    *out = &c->codec;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_es8311_create(const xiaotai_es8311_config_t *p, xiaotai_audio_codec_t **out)
{
    if (out) *out = NULL;
    if (!out || !p || !p->bus || (!p->capture && !p->playback) || p->address > 0x7f ||
        (!p->capture && (p->reference || p->route_dac_to_unused_slot))) return XIAOTAI_ERR_INVALID_ARG;
    return create(p->capture ? 1 : 0, out);
}

xiaotai_err_t xiaotai_es8389_create(const xiaotai_es8389_config_t *p, xiaotai_audio_codec_t **out)
{
    if (out) *out = NULL;
    if (!out || !p || !p->bus || (!p->capture && !p->playback) || p->address > 0x7f ||
        (p->inputs & ~0x03U) || (!p->capture && (p->reference || p->inputs))) return XIAOTAI_ERR_INVALID_ARG;
    return create(p->capture ? (p->inputs ? p->inputs : 0x03U) : 0, out);
}

xiaotai_err_t xiaotai_es7210_create(const xiaotai_es7210_config_t *p, xiaotai_audio_codec_t **out)
{
    if (out) *out = NULL;
    if (!out || !p || !p->bus || !p->inputs ||
        (p->inputs & ~0x0fU) || p->address > 0x7f) return XIAOTAI_ERR_INVALID_ARG;
    return create(p->inputs, out);
}

xiaotai_err_t xiaotai_aw88298_create(const xiaotai_aw88298_config_t *p, xiaotai_audio_codec_t **out)
{
    if (out) *out = NULL;
    if (!out || !p || !p->bus || p->address > 0x7f ||
        (p->reset.get && !p->reset.set)) return XIAOTAI_ERR_INVALID_ARG;
    if (p->bit_clocks_per_frame && p->bit_clocks_per_frame != 32 && p->bit_clocks_per_frame != 64)
        return XIAOTAI_ERR_NOT_SUPPORTED;
    return create(0, out);
}
