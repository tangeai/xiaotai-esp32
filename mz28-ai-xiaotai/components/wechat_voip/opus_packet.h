#pragma once
#include <stddef.h>
#include <stdint.h>

#define OPUS_PACKET_MAX_BYTES 1275
/* Validate RFC 6716 packet framing, not the compressed audio content.
 * Duration uses Opus's fixed 48 kHz clock, independent of decoded sample rate. */
unsigned opus_packet_samples(const uint8_t *data, size_t length);
