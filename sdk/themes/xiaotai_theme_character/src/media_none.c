// SPDX-License-Identifier: MIT
/* Without the media engine there is no loudness to follow: the character
 * moves by the conversation phase and the voice level of the view alone. */
#include "chr_director.h"

bool chr_director_loudness(uint16_t *microphone, uint16_t *speaker)
{
    *microphone = 0;
    *speaker = 0;
    return false;
}

bool chr_director_speaker_ahead(uint32_t ahead_ms, uint16_t *speaker)
{
    (void)ahead_ms;
    *speaker = 0;
    return false;
}
