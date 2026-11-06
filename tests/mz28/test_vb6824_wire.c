#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vb6824_wire.h"

int main(void)
{
    uint8_t frame[327];
    uint8_t pcm[320] = {0};
    size_t length = vb6824_wire_encode(0x2081, pcm, sizeof(pcm), frame,
                                       sizeof(frame));
    assert(length == sizeof(frame));
    assert(frame[0] == 0x55 && frame[1] == 0xAA);
    assert(frame[2] == 0x01 && frame[3] == 0x40);
    assert(frame[4] == 0x20 && frame[5] == 0x81);

    uint16_t command = 0;
    size_t payload = 0, total = 0;
    assert(vb6824_wire_inspect(frame, length, 512, &command, &payload,
                               &total) == VB6824_WIRE_COMPLETE);
    assert(command == 0x2081 && payload == 320 && total == length);
    assert(vb6824_wire_inspect(frame, length - 1, 512, NULL, NULL, NULL) ==
           VB6824_WIRE_NEED_MORE);

    frame[length - 1] ^= 1;
    assert(vb6824_wire_inspect(frame, length, 512, NULL, NULL, NULL) ==
           VB6824_WIRE_BAD_CHECKSUM);
    frame[length - 1] ^= 1;
    frame[0] = 0xAA;
    assert(vb6824_wire_inspect(frame, length, 512, NULL, NULL, NULL) ==
           VB6824_WIRE_INVALID);

    puts("VB6824 wire framing tests passed");
    return 0;
}
