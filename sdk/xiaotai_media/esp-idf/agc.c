// SPDX-License-Identifier: MIT
#include <math.h>
#include <string.h>
#include "effects/frame_stream.h"
#include "esp_agc.h"
#include "xiaotai/media.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/mem.h"

enum { RATE = 16000, FRAME = 160, DSP_STACK_MIN = 6144, DSP_STACK_DEFAULT = 49152 };

typedef struct {
    void *vendor;
    xiaotai_media_frame_stream_t capture;
    int32_t attenuation_q15;
    int16_t storage[FRAME * 2];
} gain_t;

static void close_gain(void *opaque)
{
    gain_t *gain = opaque;
    if (gain->vendor) esp_agc_close(gain->vendor);
    xiaotai_mem_free(gain);
}

static xiaotai_err_t open_gain(const xiaotai_processor_info_t *processor, xiaotai_effect_info_t *info,
                               void **state, bool boost)
{
    if (!processor || !info || !state) return XIAOTAI_ERR_INVALID_ARG;
    const int32_t cdb = xiaotai_media_param(boost ? "uplink.boost_cdb" : "capture.agc_db", boost ? 600 : 9);
    if (!cdb || processor->rate_hz != RATE || (processor->flags & XIAOTAI_PROCESSOR_ENCODED)) {
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    if (cdb < 0 || cdb > (boost ? 9000 : 90)) return XIAOTAI_ERR_INVALID_ARG;
    /* Tune only the effect's reservation; the worker still adds its own and
       any AFE, codec or sink requirements. Untuned products keep 48 KiB. */
    const int32_t stack = xiaotai_media_param(boost ? "uplink.boost_stack_bytes" : "capture.agc_stack_bytes",
                                             DSP_STACK_DEFAULT);
    if (stack < DSP_STACK_MIN || stack > DSP_STACK_DEFAULT || stack % 16) return XIAOTAI_ERR_INVALID_ARG;
    gain_t *gain = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*gain));
    if (!gain) return XIAOTAI_ERR_NO_MEM;
    gain->vendor = esp_agc_open(boost ? AGC_MODE_3 : AGC_MODE_2, RATE);
    if (!gain->vendor) { close_gain(gain); return XIAOTAI_ERR_NO_MEM; }
    if (xiaotai_mem_has_external() && !xiaotai_mem_is_external(gain->vendor)) {
        close_gain(gain);
        return XIAOTAI_ERR_NO_MEM;
    }
    const int db = boost ? (cdb + 99) / 100 : cdb;
    set_agc_config(gain->vendor, db, 1, boost ? 3 : 6);
    if (boost) {
        /* The previous 9.5219 dB fixed profile was realized as a 10 dB
           limiter with pre-attenuation. Preserve its calibrated Q15 value. */
        gain->attenuation_q15 = cdb == 952 ? 31013 : (int32_t)(32768.0 * pow(10.0, (cdb - 100.0 * db) / 2000.0) + 0.5);
    } else {
        xiaotai_media_frame_stream_init(&gain->capture, gain->storage, FRAME);
    }
    *info = (xiaotai_effect_info_t){.delay_samples = boost ? 0 : FRAME, .stack_bytes = (uint32_t)stack};
    *state = gain;
    return XIAOTAI_OK;
}

static xiaotai_err_t open_capture(const xiaotai_processor_info_t *p, xiaotai_effect_info_t *info, void **state)
{
    return open_gain(p, info, state, false);
}

static xiaotai_err_t open_boost(const xiaotai_processor_info_t *p, xiaotai_effect_info_t *info, void **state)
{
    return open_gain(p, info, state, true);
}

static bool gain_block(void *ctx, int16_t *input, int16_t *output, size_t samples)
{
    gain_t *gain = ctx;
    return esp_agc_process(gain->vendor, input, output, (int)samples, RATE) >= ESP_AGC_SUCCESS;
}

static xiaotai_err_t capture_gain(void *ctx, int16_t *pcm, size_t samples)
{
    if (!ctx || (!pcm && samples)) return XIAOTAI_ERR_INVALID_ARG;
    gain_t *gain = ctx;
    return xiaotai_media_frame_stream_process(&gain->capture, pcm, pcm, samples, gain_block, gain) ? XIAOTAI_OK : XIAOTAI_FAIL;
}

static void reset_capture(void *ctx)
{
    gain_t *gain = ctx;
    /* The vendor has no allocation-free reset. Clear only held pre-gap PCM. */
    xiaotai_media_frame_stream_reset(&gain->capture);
}

static xiaotai_err_t boost_gain(void *ctx, int16_t *pcm, size_t samples)
{
    if (!ctx || (!pcm && samples)) return XIAOTAI_ERR_INVALID_ARG;
    if (samples % FRAME) return XIAOTAI_ERR_INVALID_SIZE;
    gain_t *gain = ctx;
    for (size_t offset = 0; offset < samples; offset += FRAME) {
        for (size_t i = 0; i < FRAME; ++i) {
            const int32_t scaled = (int32_t)pcm[offset + i] * gain->attenuation_q15;
            gain->storage[i] = (int16_t)((scaled + (scaled >= 0 ? 16384 : -16384)) / 32768);
        }
        if (!gain_block(gain, gain->storage, pcm + offset, FRAME)) return XIAOTAI_FAIL;
    }
    return XIAOTAI_OK;
}

const xiaotai_audio_effect_t media_agc = {.name = "agc", .stage = XIAOTAI_EFFECT_CAPTURE, .order = 20,
    .open = open_capture, .process = capture_gain, .reset = reset_capture, .close = close_gain};
const xiaotai_audio_effect_t media_boost = {.name = "boost", .stage = XIAOTAI_EFFECT_UPLINK, .order = 20,
    .open = open_boost, .process = boost_gain, .close = close_gain};
