#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_video_profile.h"
#include "tirtc_session.h"

typedef struct {
    uint8_t *packet_buffer;
    size_t packet_length;
    size_t packet_capacity;
    tirtc_session_audio_format_t format;
    uint32_t phase;
} virtual_audio_source_t;

int virtual_audio_source_open(virtual_audio_source_t *source);
void virtual_audio_source_reset(virtual_audio_source_t *source);
void virtual_audio_source_close(virtual_audio_source_t *source);
int virtual_audio_source_next_packet(virtual_audio_source_t *source,
                                     const uint8_t **data_ptr,
                                     size_t *data_len,
                                     const tirtc_session_audio_format_t **format,
                                     uint32_t *duration_us);
