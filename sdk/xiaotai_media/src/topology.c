// SPDX-License-Identifier: MIT
#include "topology.h"

size_t media_output_block_frames(const xiaotai_audio_output_t *output)
{
    if (!output) return 0;
    const size_t nominal = output->desc.rate_hz / 50U;
    return output->info.max_write_frames && output->info.max_write_frames < nominal ?
           output->info.max_write_frames : nominal;
}

bool media_topology_can_fold_parts(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                   bool external_memory)
{
    return !external_memory && input && output && input->read && output->write &&
           input->desc.encoding == XIAOTAI_CODEC_PCM16 && output->desc.encoding == XIAOTAI_CODEC_PCM16 &&
           input->info.group && input->info.group == output->info.group && input->desc.rate_hz &&
           media_output_block_frames(output);
}

media_topology_t media_topology_select(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                        const xiaotai_processor_info_t *processor, bool external_memory)
{
    if (input && processor && (processor->flags & XIAOTAI_PROCESSOR_ENCODED)) return MEDIA_TOPOLOGY_ENCODED;
    if (!processor || (processor->flags & XIAOTAI_PROCESSOR_ASYNC) || !processor->feed_frames ||
        !media_topology_can_fold_parts(input, output, external_memory)) return MEDIA_TOPOLOGY_SPLIT;
    /* Compare exact integer durations using the parts' rates, not the rate
     * of the clean PCM after processing. */
    const uint64_t capture = (uint64_t)processor->feed_frames * output->desc.rate_hz;
    const uint64_t playout = (uint64_t)media_output_block_frames(output) * input->desc.rate_hz;
    return capture == playout ? MEDIA_TOPOLOGY_FOLDED : MEDIA_TOPOLOGY_SPLIT;
}
