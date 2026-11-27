// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/hal.h"
#include "xiaotai/media_capture.h"

typedef enum { MEDIA_TOPOLOGY_SPLIT, MEDIA_TOPOLOGY_ENCODED, MEDIA_TOPOLOGY_FOLDED } media_topology_t;

size_t media_output_block_frames(const xiaotai_audio_output_t *output);
bool media_topology_can_fold_parts(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                   bool external_memory);
media_topology_t media_topology_select(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                        const xiaotai_processor_info_t *processor, bool external_memory);
