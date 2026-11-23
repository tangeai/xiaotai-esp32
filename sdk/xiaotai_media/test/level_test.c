// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_voice_reliability.py::test_wake_diagnostics
/* Diagnostic measurements (src/level.h): the integer square root, block
 * levels of mono and interleaved PCM, and the playback meter's peak,
 * near-full-scale count and adjacent step across diagnostic windows. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "level.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int check_isqrt(void)
{
    for (uint32_t r = 0; r <= 32768U; ++r) CHECK(xiaotai_media_isqrt(r * r) == r);
    for (uint32_t r = 0; r < 65536; r += 37) {
        CHECK(xiaotai_media_isqrt(r * r) == r);
        if (r) CHECK(xiaotai_media_isqrt(r * r - 1U) == r - 1U);
        CHECK(xiaotai_media_isqrt(r * r + r) == r); /* below (r + 1)^2 */
    }
    CHECK(xiaotai_media_isqrt(0) == 0 && xiaotai_media_isqrt(1) == 1 && xiaotai_media_isqrt(3) == 1);
    CHECK(xiaotai_media_isqrt(UINT32_MAX) == 65535U);
    CHECK(xiaotai_media_isqrt(1U << 30) == 32768U); /* the mean square of full scale */
    return 0;
}

/* PCM arithmetic from the legacy S3 wake-diagnostics oracle. */
static int check_legacy_measure(void)
{
    const int16_t silence[4] = {0};
    const int16_t dc[4] = {-1000, -1000, -1000, -1000};
    int16_t extrema[4] = {INT16_MIN, INT16_MAX, INT16_MIN, INT16_MAX}, before[4];
    memcpy(before, extrema, sizeof(before));
    xiaotai_media_level_t level = xiaotai_media_level_measure(NULL, 4, 1);
    CHECK(level.rms == 0 && level.peak == 0 && level.dc == 0);
    level = xiaotai_media_level_measure(silence, 4, 1);
    CHECK(level.rms == 0 && level.peak == 0);
    level = xiaotai_media_level_measure(dc, 4, 1);
    CHECK(level.rms == 1000U && level.peak == 1000U && level.dc == -1000);
    level = xiaotai_media_level_measure(extrema, 4, 1);
    CHECK(level.rms == 32767U && level.peak == 32768U && level.dc == 0);
    CHECK(!memcmp(before, extrema, sizeof(before)));
    const int16_t mixed[] = {3, 4};
    CHECK(xiaotai_media_level_measure(mixed, 2, 1).rms == 3);
    return 0;
}

static int check_measure(void)
{
    const int16_t small[] = {3, -4};
    xiaotai_media_level_t level = xiaotai_media_level_measure(small, 2, 1);
    CHECK(level.rms == 3 && level.peak == 4 && level.dc == 0); /* sqrt(12.5) down, -0.5 toward zero */

    int16_t full[64];
    for (int i = 0; i < 64; ++i) full[i] = INT16_MIN;
    level = xiaotai_media_level_measure(full, 64, 1);
    CHECK(level.rms == 32768U && level.peak == 32768U && level.dc == -32768);

    int16_t sine[1600];
    for (int i = 0; i < 1600; ++i) sine[i] = (int16_t)lround(10000 * sin(2 * M_PI * 1000 * i / 16000.0) + 500);
    level = xiaotai_media_level_measure(sine, 1600, 1);
    /* RMS of a 10000 sine on a 500 offset: sqrt(10000^2 / 2 + 500^2) = 7088.7. */
    CHECK(level.rms >= 7087 && level.rms <= 7089);
    CHECK(level.peak == 10500 && level.dc == 500);

    /* One channel of an interleaved block: stride and start pick it. */
    int16_t stereo[200];
    for (int i = 0; i < 100; ++i) {
        stereo[2 * i] = (int16_t)(i % 2 ? 1000 : -1000);
        stereo[2 * i + 1] = (int16_t)(i == 50 ? -20000 : 7);
    }
    level = xiaotai_media_level_measure(stereo, 100, 2);
    CHECK(level.rms == 1000 && level.peak == 1000 && level.dc == 0);
    level = xiaotai_media_level_measure(stereo + 1, 100, 2);
    CHECK(level.peak == 20000 && level.dc == (7 * 99 - 20000) / 100);

    level = xiaotai_media_level_measure(NULL, 10, 1);
    CHECK(level.rms == 0 && level.peak == 0 && level.dc == 0);
    level = xiaotai_media_level_measure(small, 0, 1);
    CHECK(level.rms == 0 && level.peak == 0 && level.dc == 0);
    return 0;
}

static int check_meter(void)
{
    xiaotai_media_meter_t meter;
    memset(&meter, 0, sizeof(meter));
    const int16_t first[] = {0, 100, 31999, -32000, 32767};
    xiaotai_media_meter_add(&meter, first, 5, 1);
    CHECK(meter.samples == 5 && meter.peak == 32767 && meter.near_full == 2);
    CHECK(meter.step_max == 64767U); /* 32767 - (-32000) */

    /* A new window keeps adjacency: the first step is against the last sample. */
    xiaotai_media_meter_clear_window(&meter);
    CHECK(meter.samples == 0 && meter.peak == 0 && meter.near_full == 0 && meter.step_max == 0);
    const int16_t second[] = {-200};
    xiaotai_media_meter_add(&meter, second, 1, 1);
    CHECK(meter.samples == 1 && meter.peak == 200 && meter.step_max == 32967U);

    /* A new stream forgets it. */
    xiaotai_media_meter_reset(&meter);
    xiaotai_media_meter_add(&meter, second, 1, 1);
    CHECK(meter.samples == 1 && meter.step_max == 0 && meter.peak == 200);

    /* Stride: one channel of an interleaved block. */
    xiaotai_media_meter_reset(&meter);
    const int16_t interleaved[] = {10, INT16_MIN, 20, INT16_MIN, 40, INT16_MIN};
    xiaotai_media_meter_add(&meter, interleaved, 3, 2);
    CHECK(meter.samples == 3 && meter.peak == 40 && meter.near_full == 0 && meter.step_max == 20);
    xiaotai_media_meter_reset(&meter);
    xiaotai_media_meter_add(&meter, interleaved + 1, 3, 2);
    CHECK(meter.peak == 32768U && meter.near_full == 3 && meter.step_max == 0);
    return 0;
}

int main(void)
{
    if (check_isqrt() || check_legacy_measure() || check_measure() || check_meter()) return 1;
    printf("PASS: media_level\n");
    return 0;
}
