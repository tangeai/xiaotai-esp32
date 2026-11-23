// SPDX-License-Identifier: MIT
#include "audio_io_fake.h"
#include <assert.h>

enum { FRAMES = 160, DESCRIPTORS = 4 };
static bool amp_high;
static xiaotai_audio_codec_t codec;
static xt_audio_fake_record_t events[128];

static xiaotai_err_t set_amp(void *ctx, bool high)
{
    (void)ctx;
    amp_high = high;
    return xt_audio_fake_call(FAKE_AMP, high);
}

static xiaotai_audio_device_config_t config(int port)
{
    return (xiaotai_audio_device_config_t){
        .i2s = {.port = port, .mclk_pin = 8, .bclk_pin = 7, .ws_pin = 5, .din_pin = 6, .dout_pin = 4,
                .rate_hz = 8000, .rx_slots = 2, .tx_slots = 2, .dma_descriptors = DESCRIPTORS, .dma_frames = FRAMES},
        .group = 1, .capture_codec = &codec, .capture_channels = 1,
        .capture_roles = {XIAOTAI_CHANNEL_MIC}, .input_gain_db = {24},
        .mic_inputs = 1, .gain_max_db = 36, .gain_step_db = 6,
        .playback_codec = &codec, .playback_channels = 1, .max_write_frames = FRAMES,
        .amplifier = {.set = set_amp}, .amplifier_pad_frames = FRAMES,
    };
}

static void boot_and_repeat(void)
{
    const xiaotai_audio_input_t *in = NULL;
    const xiaotai_audio_output_t *out = NULL;
    xiaotai_audio_device_config_t cfg = config(0);
    cfg.verify_at_create = true;
    xt_audio_fake_clear();
    assert(xiaotai_audio_device_create(&cfg, &in, &out) == XIAOTAI_OK);
    size_t n = xt_audio_fake_log(events, 128);
    assert(n >= 3 && events[1].call == FAKE_START && !amp_high);
    assert(events[n - 1].call == FAKE_PAUSE && "boot verification discarded its DMA reservation");
    xiaotai_audio_device_stats_t stats;
    xiaotai_audio_device_stats(out, &stats);
    assert(!stats.running && !stats.latched && stats.opens == 1);

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        int16_t pcm[FRAMES] = {1234};
        assert(out->open(out->self) == XIAOTAI_OK && in->open(in->self) == XIAOTAI_OK);
        assert(out->set_enabled(out->self, true) == XIAOTAI_OK && amp_high);
        assert(out->write(out->self, pcm, sizeof(pcm)) == XIAOTAI_OK);
        assert(out->flush_frames(out->self) == (DESCRIPTORS + 1U) * FRAMES);
        /* The middle session is canceled: audio_device must not add a
         * hardware tail to the media owner's existing drain policy. */
        if (attempt != 1) {
            pcm[0] = 0;
            for (unsigned i = 0; i < DESCRIPTORS + 1U; ++i)
                assert(out->write(out->self, pcm, sizeof(pcm)) == XIAOTAI_OK);
        }
        assert(in->close(in->self) == XIAOTAI_OK && out->close(out->self) == XIAOTAI_OK);
        assert(!amp_high && out->flush_frames(out->self) == 0);
        xiaotai_audio_device_stats(out, &stats);
        assert(!stats.running && !stats.latched && stats.opens == 2U + attempt);
    }
    n = xt_audio_fake_log(events, 128);
    unsigned allocated = 0, paused = 0, resumed = 0, deleted = 0, writes = 0, rises = 0;
    bool rail = false;
    for (size_t i = 0; i < n; ++i) {
        const xt_audio_fake_record_t e = events[i];
        if (e.call == FAKE_AMP) {
            rail = e.value != 0;
            if (rail) {
                ++rises;
                assert(i && events[i - 1U].call == FAKE_WRITE && events[i - 1U].value == FRAMES * 2);
            }
        }
        if (e.call == FAKE_START) ++allocated;
        if (e.call == FAKE_STOP) ++deleted;
        if (e.call == FAKE_WRITE) ++writes;
        if (e.call == FAKE_PAUSE || e.call == FAKE_RESUME ||
            e.call == FAKE_CLOSE_INPUT || e.call == FAKE_CLOSE_OUTPUT) assert(!rail);
        if (e.call == FAKE_PAUSE) {
            ++paused;
            assert(i && events[i - 1].call == FAKE_CLOSE_OUTPUT);
        }
        if (e.call == FAKE_RESUME) ++resumed;
    }
    assert(allocated == 1 && deleted == 0 && paused == 4 && resumed == 3);
    assert(rises == 3U && writes == 2U * (DESCRIPTORS + 1U) + 3U * 2U);
}

static void invalid_resume_is_retired(void)
{
    const xiaotai_audio_input_t *in = NULL;
    const xiaotai_audio_output_t *out = NULL;
    const xiaotai_audio_device_config_t cfg = config(1);
    /* The capability alone reserves DMA even without a board verification flag. */
    assert(xiaotai_audio_device_create(&cfg, &in, &out) == XIAOTAI_OK);
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_RESUME, XIAOTAI_ERR_TIMEOUT, false);
    assert(out->open(out->self) == XIAOTAI_ERR_TIMEOUT && !amp_high);
    assert(in->open(in->self) == XIAOTAI_ERR_INVALID_STATE);
    assert(xt_audio_fake_log(events, 128) == 2);
    assert(events[0].call == FAKE_RESUME && events[1].call == FAKE_STOP);
    xiaotai_audio_device_stats_t stats;
    xiaotai_audio_device_stats(out, &stats);
    assert(stats.latched && !stats.running && stats.open_failures == 1);
}

static void failed_pause_keeps_unsafe_handles(void)
{
    const xiaotai_audio_output_t *out = NULL;
    const xiaotai_audio_device_config_t cfg = config(3);
    assert(xiaotai_audio_device_create(&cfg, NULL, &out) == XIAOTAI_OK);
    assert(out->open(out->self) == XIAOTAI_OK && out->set_enabled(out->self, true) == XIAOTAI_OK);
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_PAUSE, XIAOTAI_ERR_TIMEOUT, false);
    assert(out->close(out->self) == XIAOTAI_ERR_TIMEOUT && !amp_high);
    assert(out->open(out->self) == XIAOTAI_ERR_INVALID_STATE);
    const size_t n = xt_audio_fake_log(events, 128);
    assert(n >= 3 && events[n - 1].call == FAKE_PAUSE);
    for (size_t i = 0; i < n; ++i) assert(events[i].call != FAKE_STOP);
}

static void failed_boot_reclaims_safe_handles(void)
{
    const xiaotai_audio_input_t *in = NULL;
    const xiaotai_audio_output_t *out = NULL;
    const xiaotai_audio_device_config_t cfg = config(2);
    xt_audio_fake_clear();
    xt_audio_fake_fail(FAKE_START, XIAOTAI_ERR_NO_MEM, true);
    assert(xiaotai_audio_device_create(&cfg, &in, &out) == XIAOTAI_ERR_NO_MEM && !in && !out);
    assert(xt_audio_fake_log(events, 128) == 3);
    assert(events[1].call == FAKE_START && events[2].call == FAKE_STOP);
    assert(xiaotai_audio_device_create(&cfg, &in, &out) == XIAOTAI_OK && !amp_high);
}

int main(void)
{
    xt_audio_fake_codec(&codec, 1);
    boot_and_repeat();
    invalid_resume_is_retired();
    failed_pause_keeps_unsafe_handles();
    failed_boot_reclaims_safe_handles();
    return 0;
}
