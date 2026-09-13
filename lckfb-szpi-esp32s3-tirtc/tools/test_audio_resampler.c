#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "starter_audio_resampler.h"

static uint32_t average_abs(const int16_t *samples, size_t count)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t value = samples[i];
        sum += (uint32_t)(value < 0 ? -value : value);
    }
    return (uint32_t)(sum / count);
}

int main(void)
{
    enum { INPUT_SAMPLES = 640, OUTPUT_SAMPLES = INPUT_SAMPLES / 2 };
    int16_t dc[INPUT_SAMPLES];
    int16_t nyquist[INPUT_SAMPLES];
    int16_t one_shot[OUTPUT_SAMPLES];
    int16_t split[OUTPUT_SAMPLES];
    for (size_t i = 0; i < INPUT_SAMPLES; ++i) {
        dc[i] = 6000;
        nyquist[i] = (i & 1U) == 0U ? 12000 : -12000;
    }

    starter_audio_resampler_16k_to_8k_t resampler = {0};
    assert(starter_audio_resampler_16k_to_8k_process(&resampler, dc, INPUT_SAMPLES,
                                                      one_shot, OUTPUT_SAMPLES) == OUTPUT_SAMPLES);
    assert(average_abs(one_shot + 32, OUTPUT_SAMPLES - 32) > 5500U);

    starter_audio_resampler_16k_to_8k_reset(&resampler);
    assert(starter_audio_resampler_16k_to_8k_process(&resampler, nyquist, INPUT_SAMPLES,
                                                      one_shot, OUTPUT_SAMPLES) == OUTPUT_SAMPLES);
    assert(average_abs(one_shot + 32, OUTPUT_SAMPLES - 32) < 500U);

    starter_audio_resampler_16k_to_8k_reset(&resampler);
    assert(starter_audio_resampler_16k_to_8k_process(&resampler, dc, 320,
                                                      split, OUTPUT_SAMPLES) == 160U);
    assert(starter_audio_resampler_16k_to_8k_process(&resampler, dc + 320, 320,
                                                      split + 160, OUTPUT_SAMPLES - 160) == 160U);
    starter_audio_resampler_16k_to_8k_reset(&resampler);
    assert(starter_audio_resampler_16k_to_8k_process(&resampler, dc, INPUT_SAMPLES,
                                                      one_shot, OUTPUT_SAMPLES) == OUTPUT_SAMPLES);
    for (size_t i = 0; i < OUTPUT_SAMPLES; ++i) {
        assert(split[i] == one_shot[i]);
    }
    puts("PASS: 16k->8k resampler preserves speech band, rejects Nyquist aliases, and is block-continuous");
    return 0;
}
