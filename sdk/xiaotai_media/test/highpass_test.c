// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>
#include "portable_audio.h"
#include "xiaotai/media.h"

int main(void)
{
    const xiaotai_processor_info_t input = {.rate_hz = 16000};
    xiaotai_effect_info_t info;
    void *one, *split;
    assert(media_highpass.open(&input, &info, &one) == XIAOTAI_ERR_NOT_SUPPORTED);
    static const xiaotai_media_param_t parameters[] = {{"capture.highpass_hz", 100}};
    assert(xiaotai_media_tune(parameters, 1) == XIAOTAI_OK);
    assert(media_highpass.open(&input, &info, &one) == XIAOTAI_OK);
    assert(info.delay_samples == 0 && info.stack_bytes != 0);
    assert(media_highpass.open(&input, &info, &split) == XIAOTAI_OK);
    int16_t full[] = {3000, 3000, 3000, 4000, 4000, 4000, 3000, 3000, -32768, 32767, 32767};
    int16_t blocks[sizeof(full) / sizeof(full[0])];
    memcpy(blocks, full, sizeof(full));
    assert(media_highpass.process(one, full, sizeof(full) / sizeof(full[0])) == XIAOTAI_OK);
    assert(media_highpass.process(split, blocks, 5) == XIAOTAI_OK);
    assert(media_highpass.process(split, blocks + 5, sizeof(blocks) / sizeof(blocks[0]) - 5) == XIAOTAI_OK);
    assert(memcmp(full, blocks, sizeof(full)) == 0);
    assert(full[0] == 0 && full[2] == 0 && full[3] == 961 && full[4] == 924 && full[5] == 888);
    assert(full[8] == -32768 && full[9] > 28000);
    media_highpass.reset(split);
    int16_t dc[] = {-1200, -1200};
    assert(media_highpass.process(split, dc, 2) == XIAOTAI_OK && dc[0] == 0 && dc[1] == 0);
    media_highpass.close(split);
    media_highpass.close(one);
    const xiaotai_processor_info_t encoded = {.flags = XIAOTAI_PROCESSOR_ENCODED, .rate_hz = 16000};
    assert(media_highpass.open(&encoded, &info, &one) == XIAOTAI_ERR_NOT_SUPPORTED);
    return 0;
}
