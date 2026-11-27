// SPDX-License-Identifier: MIT
#include "recognition_compact.h"

#include <string.h>

uint8_t rc_alaw(int16_t sample)
{
    static const int ends[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int pcm = sample >> 3;
    uint8_t mask = 0xD5;
    if (pcm < 0) {
        mask = 0x55;
        pcm = -pcm - 1;
    }
    int segment = 0;
    while (segment < 8 && pcm > ends[segment]) ++segment;
    if (segment >= 8) return (uint8_t)(0x7F ^ mask);
    const uint8_t value = (uint8_t)((segment << 4) | ((segment < 2 ? pcm >> 1 : pcm >> segment) & 0x0F));
    return value ^ mask;
}

size_t rc_compact_capacity(size_t wav_bytes) { return wav_bytes / 2U + RC_COMPACT_HEAD; }

bool rc_compact_started(const rc_compact_t *compact) { return compact->in_data; }

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* 1: the samples begin at *offset of the head; 0: more of the head is needed; -1: no PCM16 WAV. */
static int find_samples(rc_compact_t *c, size_t *offset)
{
    const uint8_t *h = c->head;
    if (c->head_size < 12U) return 0;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    bool format = false;
    for (size_t at = 12U; at + 8U <= c->head_size;) {
        const uint32_t length = le32(h + at + 4U);
        if (!memcmp(h + at, "fmt ", 4)) {
            if (length < 16U) return -1;
            if (at + 8U + 16U > c->head_size) return 0;
            const uint8_t *f = h + at + 8U;
            c->channels = (uint8_t)le16(f + 2);
            c->rate = le32(f + 4);
            if (le16(f) != 1U || le16(f + 14) != 16U || !c->channels || c->channels > 2U || !c->rate) return -1;
            format = true;
        } else if (!memcmp(h + at, "data", 4)) {
            if (!format) return -1;
            *offset = at + 8U;
            c->left = length;
            return 1;
        }
        at += 8U + (size_t)length + (length & 1U);
        if (at >= RC_COMPACT_WAV_HEAD_MAX) return -1;
    }
    return c->head_size >= RC_COMPACT_WAV_HEAD_MAX ? -1 : 0;
}

static xiaotai_err_t samples(rc_compact_t *c, const uint8_t *data, size_t size, uint8_t *out, size_t capacity, size_t *stored)
{
    if (size > c->left) size = c->left;     /* what follows the data chunk is not sound */
    c->left -= size;
    for (size_t i = 0; i < size; ++i) {
        if (!c->has_odd) {
            c->odd = data[i];
            c->has_odd = true;
            continue;
        }
        c->has_odd = false;
        if (*stored >= capacity) return XIAOTAI_ERR_INVALID_SIZE;
        out[(*stored)++] = rc_alaw((int16_t)(uint16_t)(c->odd | data[i] << 8));
    }
    return XIAOTAI_OK;
}

xiaotai_err_t rc_compact_feed(rc_compact_t *c, const uint8_t *data, size_t size, uint8_t *out, size_t capacity,
                              size_t *stored)
{
    if (!c || (!data && size) || !out || !stored) return XIAOTAI_ERR_INVALID_ARG;
    if (c->in_data) return samples(c, data, size, out, capacity, stored);
    const size_t room = RC_COMPACT_WAV_HEAD_MAX - c->head_size, take = size < room ? size : room;
    memcpy(c->head + c->head_size, data, take);
    c->head_size += take;
    size_t offset = 0;
    const int found = find_samples(c, &offset);
    if (found < 0) return XIAOTAI_FAIL;
    if (!found) return take == size ? XIAOTAI_OK : XIAOTAI_FAIL;
    if (capacity < RC_COMPACT_HEAD || *stored) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(out, RC_COMPACT_MAGIC, 4);
    for (unsigned i = 0; i < 4U; ++i) out[4 + i] = (uint8_t)(c->rate >> (8U * i));
    out[8] = c->channels;
    out[9] = out[10] = out[11] = 0;
    *stored = RC_COMPACT_HEAD;
    c->in_data = true;
    /* The head held the first samples, the rest of this piece follows them. */
    const xiaotai_err_t error = samples(c, c->head + offset, c->head_size - offset, out, capacity, stored);
    return error == XIAOTAI_OK ? samples(c, data + take, size - take, out, capacity, stored) : error;
}
