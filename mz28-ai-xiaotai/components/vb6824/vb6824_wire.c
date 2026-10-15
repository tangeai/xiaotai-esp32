#include "vb6824_wire.h"

#include <string.h>

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint8_t checksum(const uint8_t *data, size_t length)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = (uint8_t)(sum + data[i]);
    }
    return sum;
}

size_t vb6824_wire_encode(uint16_t command, const void *payload,
                          size_t payload_length, uint8_t *frame,
                          size_t frame_capacity)
{
    size_t length = payload_length + VB6824_WIRE_OVERHEAD;
    if (frame == NULL || payload_length > UINT16_MAX ||
        length > frame_capacity || (payload_length > 0 && payload == NULL)) {
        return 0;
    }
    frame[0] = VB6824_WIRE_HEADER_0;
    frame[1] = VB6824_WIRE_HEADER_1;
    frame[2] = (uint8_t)(payload_length >> 8);
    frame[3] = (uint8_t)payload_length;
    frame[4] = (uint8_t)(command >> 8);
    frame[5] = (uint8_t)command;
    if (payload_length > 0) {
        memcpy(&frame[6], payload, payload_length);
    }
    frame[length - 1] = checksum(frame, length - 1);
    return length;
}

vb6824_wire_result_t vb6824_wire_inspect(const uint8_t *frame,
                                         size_t available,
                                         size_t max_payload,
                                         uint16_t *command,
                                         size_t *payload_length,
                                         size_t *frame_length)
{
    if (frame == NULL || available < 2 ||
        frame[0] != VB6824_WIRE_HEADER_0 ||
        frame[1] != VB6824_WIRE_HEADER_1) {
        return VB6824_WIRE_INVALID;
    }
    if (available < VB6824_WIRE_OVERHEAD) {
        return VB6824_WIRE_NEED_MORE;
    }
    size_t payload = read_be16(&frame[2]);
    if (payload > max_payload) {
        return VB6824_WIRE_INVALID;
    }
    size_t total = payload + VB6824_WIRE_OVERHEAD;
    if (available < total) {
        return VB6824_WIRE_NEED_MORE;
    }
    if (checksum(frame, total - 1) != frame[total - 1]) {
        return VB6824_WIRE_BAD_CHECKSUM;
    }
    if (command != NULL) {
        *command = read_be16(&frame[4]);
    }
    if (payload_length != NULL) {
        *payload_length = payload;
    }
    if (frame_length != NULL) {
        *frame_length = total;
    }
    return VB6824_WIRE_COMPLETE;
}
