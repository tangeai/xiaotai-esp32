// SPDX-License-Identifier: MIT
// The audio device without sound hardware: silence in, nothing out, both at
// the pace of the sample clock; what it refuses.
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "xiaotai/parts/audio.h"
#include "xiaotai/port/os.h"

int main(void)
{
    const xiaotai_audio_input_t *in = NULL;
    const xiaotai_audio_output_t *out = NULL;
    xiaotai_audio_silent_config_t config = {0};
    assert(xiaotai_audio_silent_create(&config, &in, &out) == XIAOTAI_ERR_INVALID_ARG);   /* no direction */
    config.capture_channels = 1;
    assert(xiaotai_audio_silent_create(&config, &in, &out) == XIAOTAI_ERR_INVALID_ARG);   /* no group */
    config.group = 1;
    config.playback_channels = 3;
    assert(xiaotai_audio_silent_create(&config, &in, &out) == XIAOTAI_ERR_INVALID_ARG);
    config.playback_channels = 1;
    assert(xiaotai_audio_silent_create(&config, &in, &out) == XIAOTAI_OK && in && out);

    assert(in->desc.encoding == XIAOTAI_CODEC_PCM16 && in->desc.rate_hz == 16000 && in->desc.channels == 1);
    assert(in->desc.roles[0] == XIAOTAI_CHANNEL_MIC && in->info.group == 1 && in->info.block_frames == 320);
    assert(out->desc.channels == 1 && out->desc.roles[0] == XIAOTAI_CHANNEL_MONO);
    assert(out->info.flags & XIAOTAI_AUDIO_OUTPUT_PACED);

    int16_t block[320];
    size_t size = 0;
    assert(in->read(in->self, block, sizeof(block), &size) == XIAOTAI_ERR_INVALID_STATE);
    assert(out->write(out->self, block, sizeof(block)) == XIAOTAI_ERR_INVALID_STATE);

    /* Ten 20 ms blocks take about 200 ms each way, and the input is silent. */
    assert(in->open(in->self) == XIAOTAI_OK && out->open(out->self) == XIAOTAI_OK);
    int64_t began = xiaotai_time_us();
    for (int i = 0; i < 10; ++i) {
        memset(block, 0x55, sizeof(block));
        assert(in->read(in->self, block, sizeof(block), &size) == XIAOTAI_OK && size == sizeof(block));
        for (size_t j = 0; j < 320; ++j) assert(block[j] == 0);
    }
    int64_t took = xiaotai_time_us() - began;
    assert(took >= 170000 && took < 600000);
    began = xiaotai_time_us();
    for (int i = 0; i < 10; ++i) assert(out->write(out->self, block, sizeof(block)) == XIAOTAI_OK);
    took = xiaotai_time_us() - began;
    assert(took >= 170000 && took < 600000);

    /* Half frames, a gain other than 0 dB and volume above 100 % are refused; the rest is accepted. */
    assert(in->read(in->self, block, 3, &size) == XIAOTAI_ERR_INVALID_ARG);
    assert(in->set_gain_db(in->self, 0) == XIAOTAI_OK && in->set_gain_db(in->self, 6) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->set_volume(out->self, 100) == XIAOTAI_OK && out->set_volume(out->self, 101) == XIAOTAI_ERR_INVALID_ARG);
    assert(out->set_muted(out->self, true) == XIAOTAI_OK && out->set_enabled(out->self, true) == XIAOTAI_OK);
    assert(out->flush_frames(out->self) == 320);
    xiaotai_audio_stats_t stats;
    in->stats(in->self, &stats);
    assert(stats.errors == 0);
    assert(in->close(in->self) == XIAOTAI_OK && out->close(out->self) == XIAOTAI_OK);
    assert(out->flush_frames(out->self) == 0);
    return 0;
}
