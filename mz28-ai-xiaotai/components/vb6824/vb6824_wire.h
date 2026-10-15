#pragma once

#include <stddef.h>
#include <stdint.h>

#define VB6824_WIRE_OVERHEAD 7U
#define VB6824_WIRE_HEADER_0 0x55U
#define VB6824_WIRE_HEADER_1 0xAAU

typedef enum {
    VB6824_WIRE_BAD_CHECKSUM = -2,
    VB6824_WIRE_INVALID = -1,
    VB6824_WIRE_NEED_MORE = 0,
    VB6824_WIRE_COMPLETE = 1,
} vb6824_wire_result_t;

size_t vb6824_wire_encode(uint16_t command, const void *payload,
                          size_t payload_length, uint8_t *frame,
                          size_t frame_capacity);
vb6824_wire_result_t vb6824_wire_inspect(const uint8_t *frame,
                                         size_t available,
                                         size_t max_payload,
                                         uint16_t *command,
                                         size_t *payload_length,
                                         size_t *frame_length);
