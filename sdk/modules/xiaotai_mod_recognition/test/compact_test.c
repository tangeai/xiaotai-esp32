// SPDX-License-Identifier: MIT
/* Speech kept as A-law: a WAV with a chunk before its samples and a chunk
 * after them, fed in uneven pieces, gives the head and one A-law byte for
 * every sample and nothing else; what is no PCM16 WAV is refused. */
#include "recognition_compact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static size_t put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); return 4; }
static size_t put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return 2; }
static int16_t sample(size_t n) { return (int16_t)(uint16_t)(n * 2654435761U >> 7); }

static size_t wav(uint8_t *out, size_t samples, uint16_t bits)
{
    size_t at = 0;
    memcpy(out + at, "RIFF", 4); at += 4;
    at += put32(out + at, 0);
    memcpy(out + at, "WAVE", 4); at += 4;
    memcpy(out + at, "fmt ", 4); at += 4;
    at += put32(out + at, 16);
    at += put16(out + at, 1);
    at += put16(out + at, 1);
    at += put32(out + at, 16000);
    at += put32(out + at, 32000);
    at += put16(out + at, 2);
    at += put16(out + at, bits);
    memcpy(out + at, "LIST", 4); at += 4;       /* something before the samples, odd length */
    at += put32(out + at, 5);
    memcpy(out + at, "abcde\0", 6); at += 6;
    memcpy(out + at, "data", 4); at += 4;
    at += put32(out + at, (uint32_t)(samples * 2));
    for (size_t i = 0; i < samples; ++i) at += put16(out + at, (uint16_t)sample(i));
    memcpy(out + at, "id3 ", 4); at += 4;       /* and something after them */
    at += put32(out + at, 4);
    memcpy(out + at, "zzzz", 4); at += 4;
    return at;
}

int main(void)
{
    /* The encoder agrees with G.711. */
    CHECK(rc_alaw(0) == 0xD5 && rc_alaw(-1) == 0x55 && rc_alaw(32767) == 0xAA && rc_alaw(-32768) == 0x2A);

    enum { SAMPLES = 9001 };
    static uint8_t file[SAMPLES * 2 + 200], out[SAMPLES + 64];
    const size_t size = wav(file, SAMPLES, 16);
    for (size_t piece = 1; piece <= 700; piece += 233) {
        rc_compact_t compact = {0};
        size_t stored = 0;
        const size_t capacity = rc_compact_capacity(size);
        CHECK(capacity <= sizeof(out));
        for (size_t at = 0; at < size; at += piece) {
            const size_t n = size - at < piece ? size - at : piece;
            CHECK(rc_compact_feed(&compact, file + at, n, out, capacity, &stored) == XIAOTAI_OK);
        }
        CHECK(rc_compact_started(&compact) && stored == RC_COMPACT_HEAD + SAMPLES);
        CHECK(!memcmp(out, RC_COMPACT_MAGIC, 4) && out[4] == 0x80 && out[5] == 0x3E && out[8] == 1);
        for (size_t i = 0; i < SAMPLES; ++i) CHECK(out[RC_COMPACT_HEAD + i] == rc_alaw(sample(i)));
    }

    /* In place: what lies in the buffer becomes A-law at its front, the rest follows into it. */
    {
        static uint8_t buffer[SAMPLES * 2 + 200];
        const size_t first = 5000;
        memcpy(buffer, file, first);
        rc_compact_t compact = {0};
        size_t stored = 0;
        const size_t room = SAMPLES + 64;       /* the buffer's size: the WAV so far fills part of it */
        CHECK(first <= room);
        CHECK(rc_compact_feed(&compact, buffer, first, buffer, room, &stored) == XIAOTAI_OK);
        CHECK(rc_compact_feed(&compact, file + first, size - first, buffer, room, &stored) == XIAOTAI_OK);
        CHECK(stored == RC_COMPACT_HEAD + SAMPLES && !memcmp(buffer, RC_COMPACT_MAGIC, 4));
        for (size_t i = 0; i < SAMPLES; ++i) CHECK(buffer[RC_COMPACT_HEAD + i] == rc_alaw(sample(i)));
    }

    /* Not 16 bits, not a WAV, no room: refused. */
    rc_compact_t compact = {0};
    size_t stored = 0;
    const size_t eight = wav(file, 100, 8);
    CHECK(rc_compact_feed(&compact, file, eight, out, sizeof(out), &stored) == XIAOTAI_FAIL);
    compact = (rc_compact_t){0};
    stored = 0;
    CHECK(rc_compact_feed(&compact, (const uint8_t *)"<html>not found</html>", 22, out, sizeof(out), &stored) == XIAOTAI_FAIL);
    compact = (rc_compact_t){0};
    stored = 0;
    const size_t small = wav(file, 100, 16);
    CHECK(rc_compact_feed(&compact, file, small, out, RC_COMPACT_HEAD + 10, &stored) == XIAOTAI_ERR_INVALID_SIZE);
    printf("compact ok\n");
    return 0;
}
