// SPDX-License-Identifier: MIT
#include <assert.h>
#include "xiaotai/media.h"
#include "xiaotai_mem_host.h"
#include "g711.h"
#include "registry.h"

static xiaotai_audio_input_t source;
static xiaotai_audio_output_t speaker;
static unsigned processor_opens, processor_closes, codec_opens, codec_closes;
static xiaotai_err_t codec_result = XIAOTAI_OK;
static bool external_memory;

bool __wrap_xiaotai_mem_has_external(void);
bool __wrap_xiaotai_mem_has_external(void) { return external_memory; }

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index)
{
    return index || !source.desc.rate_hz ? NULL : &source;
}
const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    return index || !speaker.desc.rate_hz ? NULL : &speaker;
}

static int score(const xiaotai_audio_desc_t *input, const xiaotai_audio_desc_t *output)
{
    (void)output;
    return input->encoding == XIAOTAI_CODEC_PCM16 || input->encoding == XIAOTAI_CODEC_OPUS ? 1 : 0;
}
static xiaotai_err_t processor_open(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                    xiaotai_processor_info_t *info, void **state)
{
    (void)output;
    const bool pcm = input->desc.encoding == XIAOTAI_CODEC_PCM16;
    *info = (xiaotai_processor_info_t){.rate_hz = input->desc.rate_hz, .feed_frames = pcm ? 320 : 0,
        .fetch_max = pcm ? 320 : 40, .flags = pcm ? 0U : XIAOTAI_PROCESSOR_ENCODED};
    *state = &source;
    ++processor_opens;
    return XIAOTAI_OK;
}
static void processor_close(void *state) { assert(state == &source); ++processor_closes; }
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
static const xiaotai_audio_processor_t processor = {.name = "processor", .match = score, .open = processor_open,
    .close = processor_close, .feed = feed, .fetch = fetch};

static int decoder_score(const xiaotai_media_format_t *format)
{
    return format->encoding == XIAOTAI_CODEC_OPUS && format->rate_hz == 16000 && format->packet_ms == 20 ? 1 : 0;
}
static xiaotai_err_t decoder_open(const xiaotai_media_format_t *format, void **state)
{
    assert(decoder_score(format));
    ++codec_opens;
    if (codec_result == XIAOTAI_OK) *state = &codec_opens;
    return codec_result;
}
static void decoder_close(void *state) { assert(state == &codec_opens); ++codec_closes; }

int main(void)
{
    const xiaotai_media_format_t alaw = XIAOTAI_MEDIA_FORMAT_G711A_8K, opus = XIAOTAI_MEDIA_FORMAT_OPUS_16K;
    xiaotai_media_format_t bad_receive = alaw, bad_send = alaw, undeclared = alaw, too_big = alaw;
    bad_receive.rate_hz = 4000; /* decoder exists but cannot convert 4 kHz to 16 kHz */
    bad_send.rate_hz = 48000;   /* uplink cannot convert 16 kHz to 48 kHz */
    undeclared.packet_ms = 10;
    too_big.packet_ms = 120;
    too_big.rate_hz = 16000;    /* 1920-byte nominal sample packet */
    speaker.desc = (xiaotai_audio_desc_t){.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1};
    source.desc = speaker.desc;
    const xiaotai_media_requirement_t main_need = {.uplink = {XIAOTAI_CODEC_G711A, 8000, 1, 0},
        .downlink = {XIAOTAI_CODEC_G711A, 8000, 1, 0}, .packet_ms = 20, .downlink_burst_ms = 1000};
    const xiaotai_media_requirement_t unavailable = {.uplink = bad_send, .downlink = bad_receive,
        .downlink_burst_ms = 6000};
    const xiaotai_media_requirement_t no_decoder = {.downlink = opus, .max_packet_bytes = 640,
        .downlink_burst_ms = 5000};
    const xiaotai_media_requirement_t oversize = {.downlink = too_big, .downlink_burst_ms = 5000};

    assert(xiaotai_media_processor_register(&processor) == XIAOTAI_OK);
    assert(xiaotai_media_require(&main_need) == XIAOTAI_OK);
    assert(xiaotai_media_require(&unavailable) == XIAOTAI_OK);
    assert(xiaotai_media_require(&no_decoder) == XIAOTAI_OK);
    assert(xiaotai_media_require(&oversize) == XIAOTAI_OK);
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    assert(!info.capture && !info.playback && !info.processor);
    assert(!xiaotai_media_can_send(&alaw) && !xiaotai_media_can_receive(&alaw));
    xiaotai_engine_media_start();
    assert(xiaotai_media_can_send(&alaw) && xiaotai_media_can_receive(&alaw));
    assert(!xiaotai_media_can_send(&undeclared) && !xiaotai_media_can_receive(&undeclared));
    assert(!xiaotai_media_can_send(&bad_send) && !xiaotai_media_can_receive(&bad_receive));
    assert(!xiaotai_media_can_receive(&too_big) && !xiaotai_media_can_receive(&opus));
    assert(!xiaotai_media_can_receive(NULL) && !xiaotai_media_can_send(NULL));
    xiaotai_media_info(&info);
    assert(info.downlink_ms == 1200 && xiaotai_downlink_capacity(xiaotai_media_downlink()) == 10980);
    assert(xiaotai_media_require(&main_need) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_media_reset();
    assert(processor_opens == 1 && processor_closes == 1);

    /* An output descriptor cannot be invented on a new input-only board. */
    speaker.desc.rate_hz = 0;
    assert(xiaotai_media_require(&main_need) == XIAOTAI_OK);
    xiaotai_engine_media_start();
    assert(!xiaotai_media_downlink() && !xiaotai_media_can_receive(&alaw));
    xiaotai_media_info(&info);
    assert(info.capture && !info.capture_pcm && !info.playback && !info.downlink_ms);
    xiaotai_media_reset();

    /* Opus on the wire matches an encoded part even without an Opus encoder.
     * Same encoding at a different packet duration is not pass-through. */
    source.desc = (xiaotai_audio_desc_t){.encoding = XIAOTAI_CODEC_OPUS, .rate_hz = 16000,
        .channels = 1, .frame_ms = 20};
    xiaotai_media_format_t repacketized = opus;
    repacketized.packet_ms = 40;
    const xiaotai_media_requirement_t direct = {.uplink = opus};
    assert(xiaotai_media_processor_register(&processor) == XIAOTAI_OK);
    assert(xiaotai_media_require(&direct) == XIAOTAI_OK);
    assert(xiaotai_media_require(&(xiaotai_media_requirement_t){.uplink = repacketized}) == XIAOTAI_OK);
    assert(xiaotai_media_require(&main_need) == XIAOTAI_OK);
    xiaotai_engine_media_start();
    assert(xiaotai_media_can_send(&opus) && !xiaotai_media_can_send(&alaw) && !xiaotai_media_can_send(&repacketized));
    xiaotai_media_info(&info);
    assert(info.capture && !info.capture_pcm && info.capture_encoding == XIAOTAI_CODEC_OPUS);
    xiaotai_media_reset();

    static xiaotai_audio_decoder_t decoder;
    decoder = xiaotai_media_g711a_decoder; /* feed is not exercised by M2 */
    decoder.encoding = XIAOTAI_CODEC_OPUS;
    decoder.match = decoder_score;
    decoder.open = decoder_open;
    decoder.close = decoder_close;
    speaker.desc.rate_hz = 16000;
    for (size_t pass = 0; pass < 3; ++pass) {
        assert(xiaotai_media_processor_register(&processor) == XIAOTAI_OK);
        assert(xiaotai_media_decoder_register(&decoder) == XIAOTAI_OK);
        assert(xiaotai_media_require(&main_need) == XIAOTAI_OK);
        assert(xiaotai_media_require(&no_decoder) == XIAOTAI_OK);
        assert(xiaotai_media_require(&no_decoder) == XIAOTAI_OK);
        codec_result = pass == 1 ? XIAOTAI_ERR_NOT_SUPPORTED : XIAOTAI_OK;
        if (pass == 2) xiaotai_mem_host_fail_nth(2); /* ring memory after the control object */
        unsigned old_opens = codec_opens, old_closes = codec_closes;
        xiaotai_engine_media_start();
        /* One source decoder plus one session decoder, never one per module. */
        assert(codec_opens == old_opens + 2);
        assert(xiaotai_media_can_send(&alaw) == (pass == 0));
        assert(xiaotai_media_can_receive(&opus) == (pass == 0));
        assert(xiaotai_media_can_receive(&alaw) == (pass != 2));
        if (pass == 2) assert(!xiaotai_media_downlink() && codec_closes == old_closes + 2);
        xiaotai_media_reset();
        assert(codec_closes == old_closes + (pass == 1 ? 0 : 2));
        assert(xiaotai_mem_host_outstanding() == 0);
    }
    source.desc = speaker.desc;
    xiaotai_media_requirement_t history_need = main_need;
    history_need.preroll_ms = 20000;
    for (size_t pass = 0; pass < 3; ++pass) {
        external_memory = pass != 0;
        assert(xiaotai_media_processor_register(&processor) == XIAOTAI_OK);
        assert(xiaotai_media_require(&history_need) == XIAOTAI_OK);
        if (pass == 2) xiaotai_mem_host_fail_nth(3); /* pre-roll after the two downlink allocations */
        xiaotai_engine_media_start();
        xiaotai_media_info(&info);
        assert(info.preroll_ms == (pass == 1 ? 20000U : 0U));
        assert(xiaotai_media_can_send(&alaw) && xiaotai_media_can_receive(&alaw));
        assert(xiaotai_downlink_capacity(xiaotai_media_downlink()) == 10980);
        xiaotai_media_reset();
        assert(xiaotai_mem_host_outstanding() == 0);
    }
    assert(processor_opens == processor_closes);
    return 0;
}
