// SPDX-License-Identifier: MIT
#include "mono_canvas.h"

#include <limits.h>

enum {
    WIDTH = XIAOTAI_THEME_MONO128_WIDTH,
    HEIGHT = XIAOTAI_THEME_MONO128_HEIGHT,
    GLYPH_ROWS = 12,
    WINDOW_BYTES = 96,
    SCROLL_GAP = 32,
};

#include "mono_font_cn.inc"
static const struct { uint16_t code, rows[GLYPH_ROWS]; } s_pinyin_glyphs[] = {
#include "mono_font_pinyin.inc"
};

_Static_assert(sizeof(s_cn_glyphs) / sizeof(s_cn_glyphs[0]) == 21087, "CJK glyphs");
_Static_assert(sizeof(s_pinyin_glyphs) / sizeof(s_pinyin_glyphs[0]) == 50, "pinyin glyphs");

static void dot(const mono_canvas_t *canvas, int x, int y, mono_ink_t ink)
{
    if ((unsigned)x >= WIDTH || (unsigned)y >= HEIGHT) return;
    unsigned offset, bit;
    if (canvas->pixel == XIAOTAI_PIXEL_MONO1_VLSB) {
        offset = (unsigned)(y / 8) * WIDTH + (unsigned)x;
        bit = (unsigned)y % 8;
    } else {
        offset = (unsigned)y * (WIDTH / 8) + (unsigned)(x / 8);
        bit = 7U - (unsigned)x % 8;
    }
    uint8_t *byte = &canvas->frame[offset];
    const uint8_t mask = (uint8_t)(1U << bit);
    if (ink == MONO_SET) *byte |= mask;
    else if (ink == MONO_CLEAR) *byte &= (uint8_t)~mask;
    else *byte ^= mask;
}

void mono_rect(const mono_canvas_t *canvas, int x, int y, int width,
               int height, mono_ink_t ink)
{
    /* Clip the rectangle, including hostile negative origins and large
     * extents, before looping over its intersection with the frame. */
    const int64_t right = (int64_t)x + width, bottom = (int64_t)y + height;
    const int first_x = x < 0 ? 0 : x, first_y = y < 0 ? 0 : y;
    if (right <= first_x || bottom <= first_y || first_x >= WIDTH || first_y >= HEIGHT) return;
    const int last_x = right > WIDTH ? WIDTH : (int)right;
    const int last_y = bottom > HEIGHT ? HEIGHT : (int)bottom;
    for (int row = first_y; row < last_y; ++row)
        for (int col = first_x; col < last_x; ++col) dot(canvas, col, row, ink);
}

static void text5(const mono_canvas_t *canvas, int x, int y, const char *text,
                  size_t count, unsigned scale)
{
    if (!text || scale < 1 || scale > 3 || y <= -21 || y >= HEIGHT) return;
    const int step = (int)scale;
    for (size_t i = 0; i < count && text[i] && x < WIDTH; ++i, x += 6 * step) {
        if (x <= -15) continue;
        const uint8_t *columns = mono_glyph5(text[i]);
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (columns[col] & (1U << row))
                    mono_rect(canvas, x + col * step, y + row * step, step, step, MONO_SET);
    }
}

void mono_text5(const mono_canvas_t *canvas, int x, int y, const char *text, unsigned scale)
{
    text5(canvas, x, y, text, SIZE_MAX, scale);
}

void mono_center5(const mono_canvas_t *canvas, int y, const char *text, unsigned scale)
{
    if (!text || scale < 1 || scale > 3) return;
    size_t count = 0;
    while (count < 21 && text[count]) ++count;
    while (count * 6U * scale > WIDTH) --scale;
    text5(canvas, (WIDTH - (int)(count * 6U * scale)) / 2, y, text, count, scale);
}

/* When there is no valid 1/2/3-byte encoding, consume only the first byte.
 * Old labels deliberately show one fallback per byte of a non-BMP glyph. */
static uint16_t next_glyph(const char **text)
{
    const uint8_t *bytes = (const uint8_t *)*text;
    const unsigned lead = bytes[0];
    ++*text;
    if (lead < 0x80U) return (uint16_t)lead;
    if (lead < 0xc2U || lead > 0xefU || (bytes[1] & 0xc0U) != 0x80U) return '?';
    if (lead < 0xe0U) {
        ++*text;
        return (uint16_t)(((lead & 0x1fU) << 6) | (bytes[1] & 0x3fU));
    }
    if ((bytes[2] & 0xc0U) != 0x80U) return '?';
    const unsigned code = ((lead & 0x0fU) << 12) |
                          ((bytes[1] & 0x3fU) << 6) | (bytes[2] & 0x3fU);
    if (code < 0x800U || (code >= 0xd800U && code <= 0xdfffU)) return '?';
    *text += 2;
    return (uint16_t)code;
}

static unsigned glyph_width(uint16_t code) { return code < 0x80U ? 8U : 12U; }

static unsigned span_width(const char *text, const char *end)
{
    unsigned width = 0;
    if (!text) return 0;
    while (text != end && *text) {
        const unsigned advance = glyph_width(next_glyph(&text));
        if (width > UINT_MAX - SCROLL_GAP - advance) return UINT_MAX - SCROLL_GAP;
        width += advance;
    }
    return width;
}

unsigned mono_utf8_width(const char *text) { return span_width(text, NULL); }

static const uint16_t *glyph12(uint16_t code)
{
    size_t first = 0, last = sizeof(s_cn_glyphs) / sizeof(s_cn_glyphs[0]);
    while (first < last) {
        const size_t mid = first + (last - first) / 2;
        if (s_cn_glyphs[mid].code == code) return s_cn_glyphs[mid].rows;
        if (s_cn_glyphs[mid].code < code) first = mid + 1;
        else last = mid;
    }
    first = 0;
    last = sizeof(s_pinyin_glyphs) / sizeof(s_pinyin_glyphs[0]);
    while (first < last) {
        const size_t mid = first + (last - first) / 2;
        if (s_pinyin_glyphs[mid].code == code) return s_pinyin_glyphs[mid].rows;
        if (s_pinyin_glyphs[mid].code < code) first = mid + 1;
        else last = mid;
    }
    return NULL;
}

static void draw_glyph(const mono_canvas_t *canvas, uint16_t code, int x, int y)
{
    if (x <= -12 || x >= WIDTH || y <= -12 || y >= HEIGHT) return;
    const uint16_t *rows = glyph12(code);
    if (!rows) {
        mono_text5(canvas, x, y + 2, "?", 1);
        return;
    }
    for (int row = 0; row < GLYPH_ROWS; ++row)
        for (unsigned col = 0; col < glyph_width(code); ++col)
            if (rows[row] & (1U << col)) dot(canvas, x + (int)col, y + row, MONO_SET);
}

static unsigned window(const mono_canvas_t *canvas, int x, int y, const char *text, const char *end,
                       unsigned total, unsigned width, uint32_t tick)
{
    if (!text || width > WIDTH) return 0;
    const unsigned skip = (total > width) ? (tick / 4U) % (total + SCROLL_GAP) : 0;
    for (unsigned gone = 0; text != end && *text && gone < skip;)
        gone += glyph_width(next_glyph(&text));

    /* Bound the source to the old 96-byte visible span without copying a
     * scratch string onto the frame owner's stack. */
    const char *start = text;
    unsigned used = 0;
    while (text != end && *text) {
        const uint16_t code = next_glyph(&text);
        const unsigned advance = glyph_width(code);
        if (used + advance > width || (size_t)(text - start) > WINDOW_BYTES) break;
        if (x < WIDTH) draw_glyph(canvas, code, x + (int)used, y);
        used += advance;
    }
    return used;
}

unsigned mono_window(const mono_canvas_t *canvas, int x, int y, const char *text,
                     unsigned width, uint32_t tick)
{
    return window(canvas, x, y, text, NULL, span_width(text, NULL), width, tick);
}

void mono_center(const mono_canvas_t *canvas, int y, const char *text)
{
    const unsigned total = span_width(text, NULL);
    const unsigned width = total < WIDTH ? total : WIDTH;
    (void)window(canvas, (WIDTH - (int)width) / 2, y, text, NULL, total, width, 0);
}

const char *mono_center_line(const mono_canvas_t *canvas, int y, const char *text,
                             uint32_t tick, bool *scrolling)
{
    if (!text) return NULL;
    const char *end = text;
    while (*end && *end != '\n') ++end;
    const unsigned total = span_width(text, end);
    const unsigned width = total < WIDTH ? total : WIDTH;
    if (scrolling && total > WIDTH) *scrolling = true;
    (void)window(canvas, (WIDTH - (int)width) / 2, y, text, end, total, width, tick);
    return *end ? end + 1 : NULL;
}

bool mono_image(const mono_canvas_t *canvas, int y, int height, const xiaotai_ui_image_t *image)
{
    if (!image || image->format != XIAOTAI_IMAGE_MONO1 || !image->data || !image->width ||
        image->width > WIDTH || !image->height || image->height > height) return false;
    const size_t stride = (image->width + 7U) / 8U;
    if (image->size < stride * image->height) return false;
    const int x = (WIDTH - image->width) / 2;
    for (uint16_t row = 0; row < image->height; ++row) {
        const uint8_t *pixels = image->data + row * stride;
        for (uint16_t col = 0; col < image->width; ++col)
            if (pixels[col / 8] & (0x80U >> (col % 8))) dot(canvas, x + col, y + row, MONO_SET);
    }
    return true;
}
