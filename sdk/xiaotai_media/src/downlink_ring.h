// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media_downlink.h"

#define MEDIA_DOWNLINK_CONTINUATION 128U

/* Before accepting packets, enable media tags and configure overflow.
 * Zero disables the corresponding packet count or duration limit. */
void media_downlink_tagged(xiaotai_downlink_t *ring, bool drop_oldest, uint32_t packet_limit, uint32_t limit_us);
uint32_t media_downlink_epoch(xiaotai_downlink_t *ring);
size_t media_downlink_packet_capacity(const xiaotai_downlink_t *ring);
bool media_downlink_push_tagged(xiaotai_downlink_t *ring, uint32_t epoch, uint32_t generation,
                                uint32_t stamp, uint32_t duration, const void *data, size_t size,
                                int64_t received, uint16_t stream, uint8_t format, uint16_t original_samples,
                                unsigned *evicted);
bool media_downlink_pop_tagged(xiaotai_downlink_t *ring, void *data, size_t capacity,
                               xiaotai_downlink_packet_t *packet, uint16_t *stream, uint8_t *format,
                               uint16_t *original_samples);
