// SPDX-License-Identifier: MIT
/* media_loudness: the levels left for whoever draws along with sound. The
 * speaker's level is what is being heard, not what was last written: each
 * block comes out when its moment has come and goes when the next one's has,
 * a speaker that runs dry falls silent, and what was filed long ago never
 * comes back. */
#include <stdint.h>
#include <stdio.h>
#include "loudness.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

static uint16_t heard_at(int64_t ms)
{
    xiaotai_media_loudness_t now;
    media_loudness_read(&now, ms * 1000);
    return now.playout;
}

static void write_at(int64_t ms, int64_t heard, uint32_t peak, int64_t lasts)
{
    media_loudness_playout(peak, heard * 1000, lasts * 1000, ms * 1000);
}

int main(void)
{
    xiaotai_media_loudness_t now;
    xiaotai_media_loudness(NULL);

    /* Nothing written: silence. */
    CHECK(heard_at(0) == 0 && heard_at(5000) == 0);

    /* The microphone's level holds for a quarter of a second. */
    media_loudness_capture(700, 1000 * 1000);
    media_loudness_read(&now, 1250 * 1000);
    CHECK(now.capture == 700);
    media_loudness_read(&now, 1251 * 1000);
    CHECK(now.capture == 0);
    media_loudness_capture(99999, 1300 * 1000);
    media_loudness_read(&now, 1300 * 1000);
    CHECK(now.capture == 32768);

    /* Every fourth sample counts, whichever its sign. */
    int16_t pcm[16] = {0};
    pcm[8] = -1234; pcm[12] = 1000; pcm[13] = 30000;
    CHECK(media_loudness_peak(pcm, 16) == 1234 && media_loudness_peak(pcm, 8) == 0 && media_loudness_peak(pcm, 0) == 0);
    pcm[0] = INT16_MIN;
    CHECK(media_loudness_peak(pcm, 16) == 32768);

    /* A driver that holds 90 ms: blocks of 10 ms written ahead come out in their order, each at its time. */
    int64_t t = 10000;
    for (int i = 0; i < 9; ++i) write_at(t, t + 10 * i, 1000U + 100U * (unsigned)i, 10);
    for (int i = 0; i < 9; ++i) CHECK(heard_at(t + 10 * i) == 1000U + 100U * (unsigned)i && heard_at(t + 10 * i + 9) == 1000U + 100U * (unsigned)i);
    /* From there on one block is written as one is heard, for longer than the ring. */
    for (int i = 9; i < 200; ++i) {
        const int64_t wrote = t + 10 * (i - 8);
        write_at(wrote, t + 10 * i, 2000U + (unsigned)i, 10);
        if (i >= 17) CHECK(heard_at(wrote) == 2000U + (unsigned)(i - 8));
    }
    t += 10 * 200;
    /* It runs dry: the last block is heard to its end, a moment late at most, then silence. */
    CHECK(heard_at(t - 1) == 2199 && heard_at(t + 10) == 2199);
    CHECK(heard_at(t + 45) == 0 && heard_at(t + 1000) == 0);
    /* Never again, at whichever of the cells' 16-bit times the clock comes past. */
    for (int64_t later = t + 50; later < t + 200000; later += 7) CHECK(heard_at(later) == 0);

    /* Nor when something is written again just as the old cells' times come round. */
    t += 400000;
    for (int i = 0; i < 9; ++i) write_at(t, t + 10 * i, 9000, 10);
    write_at(t + 65536 - 20, t + 65536 + 500, 1, 10);
    for (int64_t later = t + 65536 - 20; later < t + 65536 + 400; ++later) CHECK(heard_at(later) == 0);
    CHECK(heard_at(t + 65536 + 505) == 1);

    /* Ahead: what a driver that holds 90 ms has been given is known before it is heard; beyond that, nothing. */
    t += 400000;
    write_at(t, t + 90, 7000, 10);
    CHECK(heard_at(t) == 0 && heard_at(t + 90) == 7000);
    CHECK(heard_at(t + 120) == 7000 && heard_at(t + 160) == 0);
    /* Known or not: beyond what is filed nothing is known, which is not a pause; a block filed silent is one. */
    uint16_t level = 1;
    CHECK(!media_loudness_playout_at(&level, t * 1000) && level == 0);
    CHECK(media_loudness_playout_at(&level, (t + 95) * 1000) && level == 7000);
    CHECK(!media_loudness_playout_at(&level, (t + 160) * 1000) && level == 0);
    write_at(t + 10, t + 100, 0, 10);
    level = 1;
    CHECK(media_loudness_playout_at(&level, (t + 105) * 1000) && level == 0);

    /* A block that is a little late still follows: the level before it holds meanwhile. */
    t += 400000;
    write_at(t, t + 60, 5000, 20);
    CHECK(heard_at(t + 59) == 0);             /* not heard yet */
    CHECK(heard_at(t + 60) == 5000 && heard_at(t + 85) == 5000);
    write_at(t + 90, t + 95, 300, 20);
    CHECK(heard_at(t + 94) == 5000 && heard_at(t + 95) == 300);

    /* A reader whose clock was taken just before the last block was filed still hears the one before it. */
    t += 400000;
    for (int i = 0; i < 5; ++i) write_at(t + 20 * i, t + 20 * i, 9000, 20);
    CHECK(heard_at(t + 79) == 9000 && heard_at(t + 60) == 9000);
    media_loudness_capture(600, (t + 500) * 1000);
    media_loudness_read(&now, (t + 498) * 1000);
    CHECK(now.capture == 600);
    /* A month of silence later nothing is left of either. */
    media_loudness_read(&now, (t + INT64_C(2592000000)) * 1000);
    CHECK(now.capture == 0 && now.playout == 0);

    /* A driver that tells nothing about what it holds: heard as it is written. */
    t += 400000;
    write_at(t + 20, t, 4000, 20);
    CHECK(heard_at(t + 20) == 4000);
    write_at(t + 40, t + 20, 0, 20);
    CHECK(heard_at(t + 40) == 0);

    /* Times past the 16-bit and the 32-bit wrap of the millisecond. */
    t = (INT64_C(1) << 32) - 30;
    for (int i = 0; i < 9; ++i) write_at(t, t + 10 * i, 1000U + 100U * (unsigned)i, 10);
    for (int i = 0; i < 9; ++i) CHECK(heard_at(t + 10 * i + 4) == 1000U + 100U * (unsigned)i);
    CHECK(heard_at(t + 200) == 0);
    printf("PASS: media_loudness\n");
    return 0;
}
