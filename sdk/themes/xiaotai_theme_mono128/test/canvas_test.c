// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_ui.c::ui
// Replaces legacy: tests/xmini/test_guide.c::guide
#include "mono_canvas.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

enum { BYTES = XIAOTAI_THEME_MONO128_FRAME_BYTES };

static void layouts(void)
{
    uint8_t vertical[BYTES] = {0}, horizontal[BYTES] = {0};
    const mono_canvas_t v = {vertical, XIAOTAI_PIXEL_MONO1_VLSB};
    const mono_canvas_t h = {horizontal, XIAOTAI_PIXEL_MONO1};
    mono_rect(&v, 17, 9, 1, 1, MONO_SET);
    mono_rect(&h, 17, 9, 1, 1, MONO_SET);
    for (size_t i = 0; i < BYTES; ++i) {
        assert(vertical[i] == (i == 145 ? 0x02 : 0));
        assert(horizontal[i] == (i == 146 ? 0x40 : 0));
    }
    mono_rect(&v, 17, 9, 1, 1, MONO_INVERT);
    mono_rect(&h, 17, 9, 1, 1, MONO_CLEAR);
    for (size_t i = 0; i < BYTES; ++i) assert(!vertical[i] && !horizontal[i]);
}

static void clipped(xiaotai_pixel_t pixel)
{
    struct { uint8_t before[16], data[BYTES], after[16]; } protected;
    memset(&protected, 0xa5, sizeof(protected));
    memset(protected.data, 0, BYTES);
    const mono_canvas_t canvas = {protected.data, pixel};
    mono_rect(&canvas, -5, -7, INT_MAX, INT_MAX, MONO_SET);
    for (size_t i = 0; i < BYTES; ++i) assert(protected.data[i] == 0xff);
    mono_rect(&canvas, INT_MAX, INT_MIN, INT_MAX, INT_MAX, MONO_CLEAR);
    for (size_t i = 0; i < BYTES; ++i) assert(protected.data[i] == 0xff);
    mono_rect(&canvas, -5, -7, INT_MAX, INT_MAX, MONO_INVERT);
    for (size_t i = 0; i < BYTES; ++i) assert(!protected.data[i]);
    mono_text5(&canvas, -5, -3, "XIAOTAI", 3);
    mono_text5(&canvas, 120, 62, "END", 3);
    (void)mono_window(&canvas, 127, 63, "X", 128, 100);
    (void)mono_window(&canvas, INT_MAX, INT_MAX, "outside", 128, 0);
    (void)mono_window(&canvas, -6, -6, "绑定成功", 128, 0);
    (void)mono_window(&canvas, 127, 63, "设备", 128, 0);
    (void)mono_window(&canvas, 0, 0, "\xe4\xb8", 128, 0);
    for (size_t i = 0; i < sizeof(protected.before); ++i)
        assert(protected.before[i] == 0xa5 && protected.after[i] == 0xa5);
}

static void glyphs_and_windows(void)
{
    uint8_t a[BYTES] = {0}, b[BYTES] = {0}, zero[BYTES] = {0};
    const mono_canvas_t ca = {a, XIAOTAI_PIXEL_MONO1_VLSB}, cb = {b, XIAOTAI_PIXEL_MONO1_VLSB};
    const char chinese[] = "\xe4\xb8\xad"; /* U+4E2D */
    const char pinyin[] = "\xc7\x96";         /* U+01D6, in the supplement */
    assert(mono_utf8_width("AB") == 16 && mono_utf8_width(chinese) == 12);
    assert(mono_utf8_width(pinyin) == 12);
    assert(mono_utf8_width("\xf0\x9f\x98\x80") == 32);
    assert(mono_utf8_width("\xe4\xb8") == 16 && mono_utf8_width("\xe0\x80\x80") == 24);
    assert(mono_utf8_width("\xed\xa0\x80") == 24 && mono_utf8_width(NULL) == 0);
    assert(mono_utf8_width("微信搜索小钛") == 72);
    assert(mono_utf8_width("XiaoTai-CA88") <= 128);
    assert(mono_utf8_width("正在连接无线网络") <= 128);
    assert(mono_window(&ca, 0, 0, pinyin, 12, 0) == 12 && memcmp(a, zero, BYTES));
    assert(mono_window(&cb, 0, 0, chinese, 12, 0) == 12 && memcmp(b, zero, BYTES));
    assert(memcmp(a, b, BYTES));

    memset(a, 0, BYTES);
    memset(b, 0, BYTES);
    /* A one-pixel scroll skips the whole first (three-byte) glyph. */
    const char mixed[] = "\xe4\xb8\xad" "AB";
    assert(mono_window(&ca, 0, 0, mixed, 16, 4) == 16);
    assert(mono_window(&cb, 0, 0, "AB", 16, 0) == 16 && !memcmp(a, b, BYTES));
    memset(a, 0, BYTES);
    assert(mono_window(&ca, 0, 0, mixed, 16, 3) == 12 && memcmp(a, b, BYTES));
    memset(a, 0, BYTES);
    assert(mono_window(&ca, 0, 0, mixed, 16, 28U * 4U) == 0 && !memcmp(a, zero, BYTES));
    assert(mono_window(&ca, 0, 0, mixed, 16, 60U * 4U) == 12 && memcmp(a, zero, BYTES));

    memset(a, 0, BYTES);
    memset(b, 0, BYTES);
    mono_center5(&ca, 4, "0123456789", 3);
    mono_text5(&cb, 4, 4, "0123456789", 2);
    assert(!memcmp(a, b, BYTES) && memcmp(a, zero, BYTES));
    memset(a, 0, BYTES);
    memset(b, 0, BYTES);
    mono_center5(&ca, 0, "ABCDEFGHIJKLMNOPQRSTUZ", 3);
    mono_text5(&cb, 1, 0, "ABCDEFGHIJKLMNOPQRSTU", 1);
    assert(!memcmp(a, b, BYTES));
}

static void legacy_pinyin(void)
{
    static const uint16_t codes[] = {
        0x00c0, 0x00c1, 0x00c8, 0x00c9, 0x00cc, 0x00cd, 0x00d2, 0x00d3, 0x00d9, 0x00da,
        0x00dc, 0x00e0, 0x00e1, 0x00e8, 0x00e9, 0x00ec, 0x00ed, 0x00f2, 0x00f3, 0x00f9,
        0x00fa, 0x00fc, 0x0100, 0x0101, 0x0112, 0x0113, 0x011a, 0x011b, 0x012a, 0x012b,
        0x014c, 0x014d, 0x016a, 0x016b, 0x01cd, 0x01ce, 0x01cf, 0x01d0, 0x01d1, 0x01d2,
        0x01d3, 0x01d4, 0x01d5, 0x01d6, 0x01d7, 0x01d8, 0x01d9, 0x01da, 0x01db, 0x01dc,
    };
    const uint8_t blank[BYTES] = {0};
    uint8_t fallback[BYTES] = {0}, previous[BYTES] = {0}, frame[BYTES] = {0};
    const mono_canvas_t unknown = {fallback, XIAOTAI_PIXEL_MONO1_VLSB};
    const mono_canvas_t canvas = {frame, XIAOTAI_PIXEL_MONO1_VLSB};
    assert(mono_window(&unknown, 0, 0, "\xe2\x98\x83", 128, 0) == 12);
    assert(memcmp(fallback, blank, BYTES));
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        const uint16_t code = codes[i];
        const char text[] = {(char)(0xc0 | (code >> 6)), (char)(0x80 | (code & 0x3f)), 0};
        memset(frame, 0, BYTES);
        assert(mono_utf8_width(text) == 12);
        assert(mono_window(&canvas, 0, 0, text, 128, 0) == 12);
        assert(memcmp(frame, blank, BYTES) && memcmp(frame, fallback, BYTES) && memcmp(frame, previous, BYTES));
        memcpy(previous, frame, BYTES);
    }
}

int main(void)
{
    layouts();
    clipped(XIAOTAI_PIXEL_MONO1);
    clipped(XIAOTAI_PIXEL_MONO1_VLSB);
    glyphs_and_windows();
    legacy_pinyin();
    return 0;
}
