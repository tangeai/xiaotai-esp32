#include "virtual_audio_source.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "app_memory_policy.h"

#define VIRTUAL_AUDIO_SAMPLE_RATE_HZ 8000U
#define VIRTUAL_AUDIO_CHANNELS       1U
#define VIRTUAL_AUDIO_BITS_PER_SAMPLE 16U
#define VIRTUAL_AUDIO_FRAME_MS       20U
#define VIRTUAL_AUDIO_TONE_HZ        440U
#define VIRTUAL_AUDIO_TONE_AMPLITUDE 6000
#define VIRTUAL_AUDIO_SAMPLES_PER_FRAME \
    ((VIRTUAL_AUDIO_SAMPLE_RATE_HZ * VIRTUAL_AUDIO_FRAME_MS) / 1000U)
#define VIRTUAL_AUDIO_PACKET_BYTES \
    (VIRTUAL_AUDIO_SAMPLES_PER_FRAME * VIRTUAL_AUDIO_CHANNELS * sizeof(int16_t))

static int virtual_audio_source_reserve(virtual_audio_source_t *source)
{
    uint8_t *new_buffer = NULL;

    if (source == NULL) {
        return DEVICE_VIDEO_ERR_INVALID_ARG;
    }
    if (source->packet_capacity >= VIRTUAL_AUDIO_PACKET_BYTES) {
        return DEVICE_VIDEO_OK;
    }

    new_buffer = (uint8_t *)app_memory_alloc_psram(VIRTUAL_AUDIO_PACKET_BYTES);
    if (new_buffer == NULL) {
        return DEVICE_VIDEO_ERR_IO;
    }
    if (source->packet_buffer != NULL) {
        memcpy(new_buffer, source->packet_buffer, source->packet_capacity);
        free(source->packet_buffer);
    }

    source->packet_buffer = new_buffer;
    source->packet_capacity = VIRTUAL_AUDIO_PACKET_BYTES;
    return DEVICE_VIDEO_OK;
}

static int16_t virtual_audio_source_next_sample(virtual_audio_source_t *source)
{
    uint32_t phase = source->phase;
    int32_t sample = 0;

    if (phase < (VIRTUAL_AUDIO_SAMPLE_RATE_HZ / 2U)) {
        sample = ((int32_t)phase * 4 * VIRTUAL_AUDIO_TONE_AMPLITUDE /
                  (int32_t)VIRTUAL_AUDIO_SAMPLE_RATE_HZ) -
                 VIRTUAL_AUDIO_TONE_AMPLITUDE;
    } else {
        sample = (3 * VIRTUAL_AUDIO_TONE_AMPLITUDE) -
                 ((int32_t)phase * 4 * VIRTUAL_AUDIO_TONE_AMPLITUDE /
                  (int32_t)VIRTUAL_AUDIO_SAMPLE_RATE_HZ);
    }

    source->phase += VIRTUAL_AUDIO_TONE_HZ;
    while (source->phase >= VIRTUAL_AUDIO_SAMPLE_RATE_HZ) {
        source->phase -= VIRTUAL_AUDIO_SAMPLE_RATE_HZ;
    }

    return (int16_t)sample;
}

int virtual_audio_source_open(virtual_audio_source_t *source)
{
    int rc = DEVICE_VIDEO_OK;

    if (source == NULL) {
        return DEVICE_VIDEO_ERR_INVALID_ARG;
    }

    memset(source, 0, sizeof(*source));
    source->format.sample_rate_hz = VIRTUAL_AUDIO_SAMPLE_RATE_HZ;
    source->format.channels = VIRTUAL_AUDIO_CHANNELS;
    source->format.bits_per_sample = VIRTUAL_AUDIO_BITS_PER_SAMPLE;

    rc = virtual_audio_source_reserve(source);
    if (rc != DEVICE_VIDEO_OK) {
        virtual_audio_source_close(source);
        return rc;
    }
    return DEVICE_VIDEO_OK;
}

void virtual_audio_source_reset(virtual_audio_source_t *source)
{
    if (source == NULL) {
        return;
    }

    source->packet_length = 0U;
    source->phase = 0U;
}

void virtual_audio_source_close(virtual_audio_source_t *source)
{
    if (source == NULL) {
        return;
    }

    free(source->packet_buffer);
    memset(source, 0, sizeof(*source));
}

int virtual_audio_source_next_packet(virtual_audio_source_t *source,
                                     const uint8_t **data_ptr,
                                     size_t *data_len,
                                     const tirtc_session_audio_format_t **format,
                                     uint32_t *duration_us)
{
    int16_t *samples = NULL;
    int rc = DEVICE_VIDEO_OK;

    if (source == NULL || data_ptr == NULL || data_len == NULL || duration_us == NULL) {
        return DEVICE_VIDEO_ERR_INVALID_ARG;
    }

    rc = virtual_audio_source_reserve(source);
    if (rc != DEVICE_VIDEO_OK) {
        return rc;
    }

    samples = (int16_t *)source->packet_buffer;
    for (size_t index = 0; index < VIRTUAL_AUDIO_SAMPLES_PER_FRAME; ++index) {
        samples[index] = virtual_audio_source_next_sample(source);
    }

    source->packet_length = VIRTUAL_AUDIO_PACKET_BYTES;
    *data_ptr = source->packet_buffer;
    *data_len = source->packet_length;
    if (format != NULL) {
        *format = &source->format;
    }
    *duration_us = VIRTUAL_AUDIO_FRAME_MS * 1000U;
    return DEVICE_VIDEO_OK;
}
