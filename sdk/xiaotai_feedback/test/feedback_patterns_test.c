// SPDX-License-Identifier: MIT
#include "feedback_internal.h"
#include <assert.h>
#include <string.h>

/* Goldened from the old 7-lamp renderer: RGB after undoing the old GRB
 * wire order, with the physical brightness at 255. M2 double-click uses
 * the old time in the fixture and shifts the new oscillator 70 ms. */
static const struct {
    xiaotai_light_pattern_t pattern;
    uint16_t phase_shift_ms;
    uint32_t at[6];
    const char *RGB[6];
} legacy[] = {
    { { .kind = XIAOTAI_LIGHT_CHASE, .color = {80, 210, 255},
        .level_min = 0, .level_max = 0, .period_ms = 90, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 50, 180 } },
      0, {0, 89, 90, 179, 360, 361},
      {"50d2ff000000000000000000000000000b27001d6a", "50d2ff000000000000000000000000000b27001d6a",
       "001d6a50d2ff000000000000000000000000000b27", "001d6a50d2ff000000000000000000000000000b27",
       "000000000000000b27001d6a50d2ff000000000000", "000000000000000b27001d6a50d2ff000000000000" } },
    { { .kind = XIAOTAI_LIGHT_BREATHE, .color = {25, 115, 255},
        .level_min = 45, .level_max = 220, .period_ms = 1200, .count = 0,
        .accent_mode = 1, .accent_ms = 180, .accent_level = 0, .accent = {230, 250, 255 } },
      0, {179, 180, 599, 600, 601, 1199},
      {"e6faff0a2c610a2c610a2c610a2c610a2c610a2c61", "0a2c61e6faff0a2c610a2c610a2c610a2c610a2c61",
       "1563db1563db1563dbe6faff1563db1563db1563db", "1663dc1663dc1663dce6faff1663dc1663dc1663dc",
       "1563db1563db1563dbe6faff1563db1563db1563db", "04142d04142d04142d04142d04142d04142de6faff" } },
    { { .kind = XIAOTAI_LIGHT_BREATHE, .color = {255, 118, 8},
        .level_min = 35, .level_max = 210, .period_ms = 1800, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {0, 399, 899, 900, 901, 1799},
      {"231001231001231001231001231001231001231001", "703404703404703404703404703404703404703404",
       "d16107d16107d16107d16107d16107d16107d16107", "d26107d26107d26107d26107d26107d26107d26107",
       "d16107d16107d16107d16107d16107d16107d16107", "231001231001231001231001231001231001231001" } },
    { { .kind = XIAOTAI_LIGHT_CHASE, .color = {50, 125, 255},
        .level_min = 0, .level_max = 0, .period_ms = 140, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {20, 30, 170 } },
      0, {0, 139, 140, 141, 279, 280},
      {"327dff0000000000000000000000000406250c1264", "327dff0000000000000000000000000406250c1264",
       "0c1264327dff000000000000000000000000040625", "0c1264327dff000000000000000000000000040625",
       "0c1264327dff000000000000000000000000040625", "0406250c1264327dff000000000000000000000000" } },
    { { .kind = XIAOTAI_LIGHT_BREATHE, .color = {0, 210, 155},
        .level_min = 28, .level_max = 90, .period_ms = 2600, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {29, 1299, 1300, 1301, 2449, 2599},
      {"001812001812001812001812001812001812001812", "004936004936004936004936004936004936004936",
       "004a37004a37004a37004a37004a37004a37004a37", "004936004936004936004936004936004936004936",
       "001d15001d15001d15001d15001d15001d15001d15", "001711001711001711001711001711001711001711" } },
    { { .kind = XIAOTAI_LIGHT_CHASE, .color = {0, 235, 255},
        .level_min = 0, .level_max = 0, .period_ms = 75, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {20, 80, 255 } },
      0, {0, 74, 75, 76, 151, 199},
      {"00ebff0000000000000000000000000411370c2f96", "00ebff0000000000000000000000000411370c2f96",
       "0c2f9600ebff000000000000000000000000041137", "0c2f9600ebff000000000000000000000000041137",
       "0411370c2f9600ebff000000000000000000000000", "0411370c2f9600ebff000000000000000000000000" } },
    { { .kind = XIAOTAI_LIGHT_ALTERNATE, .color = {20, 255, 92},
        .level_min = 0, .level_max = 255, .period_ms = 220, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {0, 219, 220, 221, 439, 440},
      {"14ff5c00000014ff5c00000014ff5c00000014ff5c", "14ff5c00000014ff5c00000014ff5c00000014ff5c",
       "00000014ff5c00000014ff5c00000014ff5c000000", "00000014ff5c00000014ff5c00000014ff5c000000",
       "00000014ff5c00000014ff5c00000014ff5c000000", "14ff5c00000014ff5c00000014ff5c00000014ff5c" } },
    { { .kind = XIAOTAI_LIGHT_CHASE, .color = {255, 215, 35},
        .level_min = 0, .level_max = 0, .period_ms = 95, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 170, 255 } },
      0, {0, 94, 95, 96, 189, 190},
      {"ffd723000000000000000000000000002537006496", "ffd723000000000000000000000000002537006496",
       "006496ffd723000000000000000000000000002537", "006496ffd723000000000000000000000000002537",
       "006496ffd723000000000000000000000000002537", "002537006496ffd723000000000000000000000000" } },
    { { .kind = XIAOTAI_LIGHT_BREATHE, .color = {28, 255, 74},
        .level_min = 70, .level_max = 175, .period_ms = 1400, .count = 0,
        .accent_mode = 2, .accent_ms = 0, .accent_level = 210, .accent = {180, 255, 215 } },
      0, {0, 561, 699, 700, 701, 1399},
      {"08461408461408461494d2b1084614084614084614", "119a2d119a2d119a2d94d2b1119a2d119a2d119a2d",
       "13ae3213ae3213ae3294d2b113ae3213ae3213ae32", "13af3313af3313af3394d2b113af3313af3313af33",
       "13ae3213ae3213ae3294d2b113ae3213ae3213ae32", "08461408461408461494d2b1084614084614084614" } },
    { { .kind = XIAOTAI_LIGHT_BLINK, .color = {255, 48, 22},
        .level_min = 28, .level_max = 170, .period_ms = 110, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {0, 109, 110, 111, 219, 220},
      {"1c05021c05021c05021c05021c05021c05021c0502", "1c05021c05021c05021c05021c05021c05021c0502",
       "aa200faa200faa200faa200faa200faa200faa200f", "aa200faa200faa200faa200faa200faa200faa200f",
       "aa200faa200faa200faa200faa200faa200faa200f", "1c05021c05021c05021c05021c05021c05021c0502" } },
    { { .kind = XIAOTAI_LIGHT_FLASHES, .color = {255, 18, 12},
        .level_min = 0, .level_max = 255, .period_ms = 1300, .count = 2,
        .accent_mode = 0, .accent_ms = 120, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {0, 119, 120, 240, 359, 360},
      {"ff120cff120cff120cff120cff120cff120cff120c", "ff120cff120cff120cff120cff120cff120cff120c",
       "000000000000000000000000000000000000000000", "ff120cff120cff120cff120cff120cff120cff120c",
       "ff120cff120cff120cff120cff120cff120cff120c", "000000000000000000000000000000000000000000" } },
    { { .kind = XIAOTAI_LIGHT_SOLID, .color = {110, 245, 255},
        .level_min = 0, .level_max = 210, .period_ms = 0, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {0, 39, 40, 69, 75, 95},
      {"5bcad25bcad25bcad25bcad25bcad25bcad25bcad2", "5bcad25bcad25bcad25bcad25bcad25bcad25bcad2",
       "5bcad25bcad25bcad25bcad25bcad25bcad25bcad2", "5bcad25bcad25bcad25bcad25bcad25bcad25bcad2",
       "5bcad25bcad25bcad25bcad25bcad25bcad25bcad2", "5bcad25bcad25bcad25bcad25bcad25bcad25bcad2" } },
    { { .kind = XIAOTAI_LIGHT_ALTERNATE, .color = {255, 45, 20},
        .level_min = 0, .level_max = 255, .period_ms = 70, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      70, {0, 69, 70, 71, 139, 140},
      {"000000ff2d14000000ff2d14000000ff2d14000000", "000000ff2d14000000ff2d14000000ff2d14000000",
       "ff2d14000000ff2d14000000ff2d14000000ff2d14", "ff2d14000000ff2d14000000ff2d14000000ff2d14",
       "ff2d14000000ff2d14000000ff2d14000000ff2d14", "000000ff2d14000000ff2d14000000ff2d14000000" } },
    { { .kind = XIAOTAI_LIGHT_SOLID, .color = {210, 255, 225},
        .level_min = 0, .level_max = 255, .period_ms = 0, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {0, 39, 40, 95, 111, 199},
      {"d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1", "d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1",
       "d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1", "d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1",
       "d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1", "d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1d2ffe1" } },
    { { .kind = XIAOTAI_LIGHT_BLINK, .color = {255, 138, 0},
        .level_min = 55, .level_max = 255, .period_ms = 80, .count = 0,
        .accent_mode = 0, .accent_ms = 0, .accent_level = 0, .accent = {0, 0, 0 } },
      0, {0, 79, 80, 81, 159, 160},
      {"371e00371e00371e00371e00371e00371e00371e00", "371e00371e00371e00371e00371e00371e00371e00",
       "ff8a00ff8a00ff8a00ff8a00ff8a00ff8a00ff8a00", "ff8a00ff8a00ff8a00ff8a00ff8a00ff8a00ff8a00",
       "ff8a00ff8a00ff8a00ff8a00ff8a00ff8a00ff8a00", "371e00371e00371e00371e00371e00371e00371e00" } },
};

static unsigned nibble(char c) { return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10); }

int main(void)
{
    for (size_t row = 0; row < sizeof(legacy) / sizeof(legacy[0]); ++row) {
        for (size_t n = 0; n < 6U; ++n) {
            uint8_t expected[21], actual[21];
            for (size_t i = 0; i < sizeof(expected); ++i) {
                expected[i] = 16U * nibble(legacy[row].RGB[n][2U * i]) + nibble(legacy[row].RGB[n][2U * i + 1U]);
            }
            fb_pattern_rgb(&legacy[row].pattern, legacy[row].at[n] + legacy[row].phase_shift_ms, actual, 7U);
            assert(!memcmp(expected, actual, sizeof(actual)));
        }
    }
    const xiaotai_light_pattern_t undefined = {.kind = XIAOTAI_LIGHT_FLASHES, .level_max = 255, .color = {1, 20, 30}};
    uint8_t out[] = {255, 255, 255};
    fb_pattern_rgb(&undefined, UINT32_MAX, out, 1U);
    assert(!out[0] && !out[1] && !out[2]);
    return 0;
}
