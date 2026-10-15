#include "opus_packet.h"

static int frame_size(const uint8_t **data, size_t *left)
{
    if (!*left)
        return -1;
    unsigned size = *(*data)++;
    --*left;
    if (size >= 252) {
        if (!*left)
            return -1;
        size += 4U * *(*data)++;
        --*left;
    }
    return (int)size;
}

unsigned opus_packet_samples(const uint8_t *data, size_t length)
{
    if (!data || !length || length > OPUS_PACKET_MAX_BYTES)
        return 0;
    unsigned toc = *data++, code = toc & 3U, count = code ? 2 : 1;
    size_t left = length - 1;
    unsigned samples;
    if (toc & 128)
        samples = 120U << ((toc >> 3) & 3);
    else if ((toc & 96) == 96)
        samples = (toc & 8) ? 960 : 480;
    else {
        unsigned index = (toc >> 3) & 3;
        samples = index == 3 ? 2880 : (480U << index);
    }
    unsigned vbr = code == 2;
    if (code == 3) {
        if (!left)
            return 0;
        unsigned header = *data++;
        --left;
        count = header & 63;
        vbr = header & 128;
        if (!count || count * samples > 5760)
            return 0;
        if (header & 64) {
            size_t padding = 0;
            unsigned byte;
            do {
                if (!left)
                    return 0;
                byte = *data++;
                --left;
                padding += byte == 255 ? 254 : byte;
            } while (byte == 255);
            if (padding > left)
                return 0;
            left -= padding;
        }
    }
    if (count * samples > 5760)
        return 0;
    if (vbr) {
        size_t total = 0;
        for (unsigned i = 1; i < count; ++i) {
            int size = frame_size(&data, &left);
            if (size < 0 || size > 1275)
                return 0;
            total += (unsigned)size;
        }
        if (total > left || left - total > 1275)
            return 0;
    } else if (left % count || left / count > 1275)
        return 0;
    return count * samples;
}
