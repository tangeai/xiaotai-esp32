// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>
#include "xiaotai/media.h"
#include "g711.h"
#include "registry.h"

static const xiaotai_audio_input_t input = {
    .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 2,
             .roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE}},
    .info = {.group = 1},
};
static const xiaotai_audio_output_t output = {
    .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1},
    .info = {.group = 1},
};
static unsigned opened, closed;

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index) { return index ? NULL : &input; }
const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index) { return index ? NULL : &output; }

static int score_reference(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    assert(in == &input.desc && out == &output.desc);
    return in->roles[1] == XIAOTAI_CHANNEL_REFERENCE ? 2 : 0;
}
static int score_generic(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    (void)out;
    return in && in->encoding == XIAOTAI_CODEC_PCM16 ? 1 : 0;
}
static xiaotai_err_t open_processor(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                                    xiaotai_processor_info_t *info, void **state)
{
    assert(in == &input && out == &output);
    *info = (xiaotai_processor_info_t){.rate_hz = 16000, .feed_frames = 320, .fetch_max = 320,
                                     .flags = XIAOTAI_PROCESSOR_CANCELS_ECHO};
    *state = &opened;
    ++opened;
    return XIAOTAI_OK;
}
static void close_processor(void *state) { assert(state == &opened); ++closed; }
static xiaotai_err_t feed(void *state, const void *data, size_t size, int64_t us, uint32_t lost)
{
    (void)state; (void)data; (void)size; (void)us; (void)lost;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
static xiaotai_err_t fetch(void *state, xiaotai_processor_frame_t *frame, uint32_t timeout)
{
    (void)state; (void)frame; (void)timeout;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
static void stats(void *state, xiaotai_processor_stats_t *value)
{
    assert(state == &opened);
    value->frames = 7;
}
static xiaotai_err_t effect_open(const xiaotai_processor_info_t *info, xiaotai_effect_info_t *out, void **state)
{
    (void)info; (void)out; (void)state;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
static xiaotai_err_t effect_process(void *state, int16_t *pcm, size_t samples)
{
    (void)state; (void)pcm; (void)samples;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
static void effect_close(void *state) { (void)state; }
static void tap_pcm(void *ctx, const int16_t *pcm, size_t n, uint32_t hz, int64_t us)
{
    (void)ctx; (void)pcm; (void)n; (void)hz; (void)us;
    assert(!"registry must not run the capture path");
}
static int prefer_codec(const xiaotai_media_format_t *format) { return format->rate_hz == 8000 ? 2 : 0; }

int main(void)
{
    static xiaotai_audio_processor_t generic = {.name = "generic", .match = score_generic, .open = open_processor,
        .close = close_processor, .feed = feed, .fetch = fetch, .stats = stats};
    static xiaotai_audio_processor_t first, tie;
    first = generic; first.name = "first"; first.match = score_reference;
    tie = first; tie.name = "tie";
    static xiaotai_audio_effect_t effect = {.name = "effect", .open = effect_open, .process = effect_process,
        .close = effect_close, .stage = XIAOTAI_EFFECT_UPLINK, .order = 1};
    static xiaotai_audio_effect_t high, low, also_low;
    high = low = also_low = effect;
    high.stage = low.stage = also_low.stage = XIAOTAI_EFFECT_CAPTURE;
    high.order = 9;
    const xiaotai_audio_effect_t invalid_stage = {.stage = (xiaotai_effect_stage_t)2, .name = "invalid",
        .open = effect_open, .process = effect_process, .close = effect_close};
    static const xiaotai_media_tap_t tap = {.name = "test", .pcm = tap_pcm};
    static xiaotai_audio_encoder_t better, same_score, other_codec;
    better = xiaotai_media_g711a_encoder; better.match = prefer_codec; better.name = "better";
    same_score = better; same_score.name = "tie";
    other_codec = better; other_codec.encoding = XIAOTAI_CODEC_PCM16;
    static xiaotai_audio_decoder_t better_decoder;
    better_decoder = xiaotai_media_g711a_decoder; better_decoder.match = prefer_codec;
    const xiaotai_media_format_t format = XIAOTAI_MEDIA_FORMAT_G711A_8K;
    assert(xiaotai_media_encoder_register(&other_codec) == XIAOTAI_OK);
    assert(media_select_encoder(&format) == &xiaotai_media_g711a_encoder);
    assert(xiaotai_media_encoder_register(&better) == XIAOTAI_OK);
    assert(xiaotai_media_encoder_register(&same_score) == XIAOTAI_OK);
    assert(media_select_encoder(&format) == &better);
    assert(media_select_decoder(&format) == &xiaotai_media_g711a_decoder);
    assert(xiaotai_media_decoder_register(&better_decoder) == XIAOTAI_OK);
    assert(media_select_decoder(&format) == &better_decoder);

    assert(xiaotai_media_processor_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_media_processor_register(&generic) == XIAOTAI_OK);
    assert(xiaotai_media_processor_register(&first) == XIAOTAI_OK);
    assert(xiaotai_media_processor_register(&tie) == XIAOTAI_OK);
    unsigned slots = 3;
    while (slots < 20 && xiaotai_media_processor_register(&generic) == XIAOTAI_OK) ++slots;
    assert(slots == 8 && xiaotai_media_processor_register(&generic) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_media_effect_register(&invalid_stage) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_media_effect_register(&effect) == XIAOTAI_OK);
    assert(xiaotai_media_effect_register(&high) == XIAOTAI_OK);
    assert(xiaotai_media_effect_register(&low) == XIAOTAI_OK);
    assert(xiaotai_media_effect_register(&also_low) == XIAOTAI_OK);
    assert(media_effect_at(0) == &low && media_effect_at(1) == &also_low &&
           media_effect_at(2) == &high && media_effect_at(3) == &effect && !media_effect_at(4));
    assert(xiaotai_media_tap_enable(&tap, true) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_media_tap_register(&tap) == XIAOTAI_OK);
    assert(xiaotai_media_tap_register(&tap) == XIAOTAI_OK);
    bool enabled = true;
    assert(media_tap_at(0, &enabled) == &tap && !enabled && !media_tap_at(1, NULL));
    assert(xiaotai_media_tap_enable(&tap, true) == XIAOTAI_OK);
    assert(media_tap_at(0, &enabled) == &tap && enabled);

    assert(xiaotai_media_processor() == NULL);
    xiaotai_engine_media_start();
    xiaotai_engine_media_start();
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    assert(info.capture && info.playback && info.capture_pcm && info.cancels_echo);
    assert(info.capture_rate_hz == 16000 && info.playback_rate_hz == 16000 && !strcmp(info.processor, "first"));
    assert(xiaotai_media_processor() == &first && opened == 1 && closed == 0);
    xiaotai_processor_stats_t current;
    xiaotai_media_processor_stats(&current);
    assert(current.frames == 7 && !current.feeds);
    assert(xiaotai_media_processor_register(&generic) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_media_effect_register(&effect) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_media_tap_register(&tap) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_media_tap_enable(&tap, false) == XIAOTAI_OK && media_tap_at(0, &enabled) == &tap && !enabled);
    xiaotai_media_reset();
    xiaotai_media_processor_stats(&current);
    assert(closed == 1 && !xiaotai_media_processor() && !current.frames);
    assert(!media_effect_at(0) && !media_tap_at(0, NULL));
    return 0;
}
