/*
 * SPDX-FileCopyrightText: 2023 Lierda Science & Technology Group Co., Ltd.
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 5x7 glyphs derived from xiaotai-lierda tirtc_lite/ssd1306_display.c.
 * Rendering below targets the ESP-IDF SSD1306 page-major 1-bit framebuffer.
 */
#include "xmini_canvas.h"

#include <stddef.h>
#include <string.h>

#include "xmini_cn_font.inc"

static const uint8_t digits[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
};
static const uint8_t letters[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x7f,0x20,0x18,0x20,0x7f}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43},
};

static const uint8_t *glyph(char ch)
{
    static const uint8_t blank[5] = {0};
    static const uint8_t plus[5] = {0x08,0x08,0x3e,0x08,0x08};
    static const uint8_t minus[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t arrow[5] = {0x00,0x41,0x22,0x14,0x08};
    static const uint8_t colon[5] = {0x00,0x36,0x36,0x00,0x00};
    static const uint8_t question[5] = {0x02,0x01,0x51,0x09,0x06};
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'];
    if (ch == '+') return plus;
    if (ch == '-') return minus;
    if (ch == '>') return arrow;
    if (ch == ':') return colon;
    return ch == ' ' ? blank : question;
}

static void pixel(uint8_t *frame, int x, int y)
{
    if (x >= 0 && x < 128 && y >= 0 && y < 64) {
        frame[(y / 8) * 128 + x] |= (uint8_t)(1U << (y % 8));
    }
}

void xmini_canvas_text(uint8_t *frame, int x, int y, const char *text, unsigned scale)
{
    if (!frame || !text || scale == 0 || scale > 3) return;
    for (; *text && x < 128; ++text, x += 6 * (int)scale) {
        const uint8_t *g = glyph(*text);
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if (!(g[col] & (1U << row))) continue;
                for (unsigned dx = 0; dx < scale; ++dx) {
                    for (unsigned dy = 0; dy < scale; ++dy) {
                        pixel(frame, x + col * (int)scale + (int)dx,
                               y + row * (int)scale + (int)dy);
                    }
                }
            }
        }
    }
}

void xmini_canvas_center(uint8_t *frame, int y, const char *text, unsigned scale)
{
    if (!text || scale == 0 || scale > 3) return;
    size_t count = strlen(text);
    if (count > 21) count = 21;
    while (scale > 1 && count * 6 * scale > 128) --scale;
    int width = (int)(count * 6 * scale);
    xmini_canvas_text(frame, width < 128 ? (128 - width) / 2 : 0, y, text, scale);
}

void xmini_canvas_header(uint8_t *frame, const char *text)
{
    xmini_canvas_center(frame, 3, text, 1);
    for (int x = 0; x < 128; ++x) pixel(frame, x, 14);
}

static uint16_t next_codepoint(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    uint16_t code = '?';
    unsigned count = 1;
    if (*p < 128) code = *p;
    else if (*p >= 0xc2 && *p <= 0xdf && p[1] && (p[1] & 0xc0) == 0x80) {
        code = (uint16_t)((p[0] & 31) << 6 | (p[1] & 63));
        count = 2;
    } else if (*p >= 0xe0 && *p <= 0xef && p[1] && p[2] &&
               (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        uint16_t candidate = (uint16_t)((p[0] & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63));
        if (candidate >= 0x800 && (candidate < 0xd800 || candidate > 0xdfff)) {
            code = candidate;
            count = 3;
        }
    }
    *cursor += count;
    return code;
}

unsigned xmini_canvas_utf8_width(const char *text)
{
    unsigned width = 0;
    if (!text) return 0;
    while (*text) width += next_codepoint(&text) < 128 ? 8 : 12;
    return width;
}

void xmini_canvas_utf8(uint8_t *frame, int x, int y, const char *text)
{
    if (!frame || !text) return;
    while (*text && x < 128) {
        uint16_t code = next_codepoint(&text);
        size_t low = 0, high = sizeof(s_cn_glyphs) / sizeof(s_cn_glyphs[0]);
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (s_cn_glyphs[middle].code < code) low = middle + 1;
            else high = middle;
        }
        unsigned width = code < 128 ? 8 : 12;
        if (low < sizeof(s_cn_glyphs) / sizeof(s_cn_glyphs[0]) && s_cn_glyphs[low].code == code) {
            for (int row = 0; row < 12; ++row)
                for (unsigned col = 0; col < width; ++col)
                    if (s_cn_glyphs[low].rows[row] & (1U << col)) pixel(frame, x + (int)col, y + row);
        } else {
            xmini_canvas_text(frame, x, y + 2, "?", 1);
        }
        x += (int)width;
    }
}

void xmini_canvas_utf8_center(uint8_t *frame, int y, const char *text)
{
    unsigned width = xmini_canvas_utf8_width(text);
    xmini_canvas_utf8(frame, width < 128 ? (128 - (int)width) / 2 : 0, y, text);
}

void xmini_canvas_utf8_window(uint8_t *frame, int x, int y, const char *text, unsigned width, uint32_t tick)
{
    if (!text || width > 128) return;
    unsigned total = xmini_canvas_utf8_width(text);
    unsigned skip = total > width ? (tick / 4) % (total + 32) : 0;
    const char *start = text;
    unsigned passed = 0;
    while (*start && passed < skip) passed += next_codepoint(&start) < 128 ? 8 : 12;
    char visible[97];
    size_t bytes = 0;
    unsigned used = 0;
    while (*start) {
        const char *next = start;
        unsigned character_width = next_codepoint(&next) < 128 ? 8 : 12;
        size_t length = (size_t)(next - start);
        if (used + character_width > width || bytes + length >= sizeof(visible)) break;
        memcpy(visible + bytes, start, length);
        bytes += length;
        used += character_width;
        start = next;
    }
    visible[bytes] = 0;
    xmini_canvas_utf8(frame, x, y, visible);
}
