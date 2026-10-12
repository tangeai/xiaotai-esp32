#ifndef STARTER_AUDIO_DMA_H
#define STARTER_AUDIO_DMA_H

#include <stdint.h>

/* One hardware contract for the paired I2S channels and playout estimate.
 * A frame includes all slots; its duration is independent of slot count. */
#define STARTER_AUDIO_DMA_SAMPLE_RATE_HZ 16000U
#define STARTER_AUDIO_DMA_DESCRIPTOR_COUNT 6U
#define STARTER_AUDIO_DMA_FRAME_COUNT 256U
#define STARTER_AUDIO_DMA_DURATION_US \
    ((uint32_t)(STARTER_AUDIO_DMA_DESCRIPTOR_COUNT * \
                (uint64_t)STARTER_AUDIO_DMA_FRAME_COUNT * 1000000U / \
                STARTER_AUDIO_DMA_SAMPLE_RATE_HZ))

#endif
