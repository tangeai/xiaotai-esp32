// SPDX-License-Identifier: MIT
/* With the media engine in the product: the mouth and the ears follow the
 * loudness the audio tasks publish. */
#include "chr_director.h"
#include "xiaotai/media.h"

bool chr_director_loudness(uint16_t *microphone, uint16_t *speaker)
{
    xiaotai_media_loudness_t loudness;
    xiaotai_media_loudness(&loudness);
    *microphone = loudness.capture;
    *speaker = loudness.playout;
    return true;
}

bool chr_director_speaker_ahead(uint32_t ahead_ms, uint16_t *speaker)
{
    xiaotai_media_loudness_t loudness;
    const bool known = xiaotai_media_loudness_ahead(&loudness, ahead_ms);
    *speaker = loudness.playout;
    return known;
}
