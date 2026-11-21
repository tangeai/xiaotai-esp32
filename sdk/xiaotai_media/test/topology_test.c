// SPDX-License-Identifier: MIT
#include <assert.h>

#include "topology.h"

static xiaotai_err_t read_block(void *part, void *data, size_t capacity, size_t *n)
{
    (void)part;
    (void)data;
    (void)capacity;
    (void)n;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

static xiaotai_err_t write_block(void *part, void *data, size_t bytes)
{
    (void)part;
    (void)data;
    (void)bytes;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

int main(void)
{
    const xiaotai_audio_input_t input = {.desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1},
        .info = {.group = 3}, .read = read_block};
    const xiaotai_audio_output_t output = {.desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 16000, .channels = 1},
        .info = {.group = 3, .max_write_frames = 320}, .write = write_block};
    const xiaotai_processor_info_t sync = {.rate_hz = 16000, .feed_frames = 160, .fetch_max = 320};
    assert(media_topology_can_fold_parts(&input, &output, false));
    assert(media_topology_select(&input, &output, &sync, false) == MEDIA_TOPOLOGY_FOLDED);
    assert(media_topology_select(&input, &output, &sync, true) == MEDIA_TOPOLOGY_SPLIT);

    xiaotai_processor_info_t changed = sync;
    changed.flags = XIAOTAI_PROCESSOR_ASYNC;
    assert(media_topology_select(&input, &output, &changed, false) == MEDIA_TOPOLOGY_SPLIT);
    changed.flags = XIAOTAI_PROCESSOR_ENCODED;
    xiaotai_audio_input_t encoded = input;
    encoded.desc.encoding = XIAOTAI_CODEC_OPUS;
    assert(media_topology_select(&encoded, &output, &changed, false) == MEDIA_TOPOLOGY_ENCODED);
    assert(media_topology_select(&encoded, &output, &changed, true) == MEDIA_TOPOLOGY_ENCODED);
    assert(media_topology_select(&encoded, NULL, &changed, false) == MEDIA_TOPOLOGY_ENCODED);

    xiaotai_audio_output_t other = output;
    other.info.group = 4;
    assert(media_topology_select(&input, &other, &sync, false) == MEDIA_TOPOLOGY_SPLIT);
    xiaotai_audio_input_t ungrouped = input;
    ungrouped.info.group = other.info.group = 0;
    assert(media_topology_select(&ungrouped, &other, &sync, false) == MEDIA_TOPOLOGY_SPLIT);

    other = output;
    other.info.max_write_frames = 160; /* static speaker burst 10 ms versus mic's 20 ms */
    assert(media_output_block_frames(&other) == 160);
    assert(media_topology_select(&input, &other, &sync, false) == MEDIA_TOPOLOGY_SPLIT);
    other.info.max_write_frames = 640; /* driver can take more; media still caps at 20 ms */
    assert(media_output_block_frames(&other) == 320);
    assert(media_topology_select(&input, &other, &sync, false) == MEDIA_TOPOLOGY_FOLDED);
    assert(media_topology_select(&input, &other, NULL, false) == MEDIA_TOPOLOGY_SPLIT);
    other.write = NULL;
    assert(media_topology_select(&input, &other, &sync, false) == MEDIA_TOPOLOGY_SPLIT);
}
