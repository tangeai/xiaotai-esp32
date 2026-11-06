#include "video_timestamp_overlay.h"

#define TIMESTAMP_LENGTH 12U
#define TIMESTAMP_MARGIN 8U
#define TIMESTAMP_PADDING 4U

static const uint8_t s_glyphs[][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
    {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
    {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
    {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e},
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e},
    {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c},
    {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00},
};

static unsigned timestamp_glyph(char character)
{
    if (character == ':') return 10U;
    if (character == '.') return 11U;
    if (character == '-') return 12U;
    return (unsigned)(character - '0');
}

bool video_timestamp_overlay_draw(uint8_t *data, size_t length,
                                  uint16_t width, uint16_t height,
                                  const char *timestamp)
{
    if (data == NULL || timestamp == NULL || width == 0U || height == 0U ||
        (width & 1U) != 0U || (height & 1U) != 0U) {
        return false;
    }
    size_t stride = (size_t)width * 3U / 2U;
    if (height > SIZE_MAX / stride || length < stride * height) return false;
    for (unsigned i = 0U; i < TIMESTAMP_LENGTH; ++i) {
        char c = timestamp[i];
        if (i == 2U || i == 5U) {
            if (c != ':') return false;
        } else if (i == 8U) {
            if (c != '.') return false;
        } else if ((c < '0' || c > '9') && c != '-') {
            return false;
        }
    }
    if (timestamp[TIMESTAMP_LENGTH] != '\0') return false;

    unsigned scale = width >= 720U ? 3U : 2U;
    unsigned right = (TIMESTAMP_MARGIN + (TIMESTAMP_LENGTH * 6U - 1U) * scale +
                      2U * TIMESTAMP_PADDING + 1U) & ~1U;
    unsigned bottom = (TIMESTAMP_MARGIN + 7U * scale +
                       2U * TIMESTAMP_PADDING + 1U) & ~1U;
    if (right > width || bottom > height) return false;

    /* Every three-byte group contains U/V followed by two luma samples.
     * Align the panel to chroma pairs so no pixels outside it change colour. */
    for (unsigned y = TIMESTAMP_MARGIN; y < bottom; ++y) {
        uint8_t *row = data + y * stride;
        for (unsigned x = TIMESTAMP_MARGIN; x < right; x += 2U) {
            size_t offset = (size_t)x / 2U * 3U;
            row[offset] = 128U;
            row[offset + 1U] = 16U;
            row[offset + 2U] = 16U;
        }
    }
    for (unsigned i = 0U; i < TIMESTAMP_LENGTH; ++i) {
        const uint8_t *glyph = s_glyphs[timestamp_glyph(timestamp[i])];
        unsigned origin_x = TIMESTAMP_MARGIN + TIMESTAMP_PADDING + i * 6U * scale;
        for (unsigned y = 0U; y < 7U * scale; ++y) {
            uint8_t *row = data +
                (TIMESTAMP_MARGIN + TIMESTAMP_PADDING + y) * stride;
            for (unsigned x = 0U; x < 5U * scale; ++x) {
                if ((glyph[y / scale] & (0x10U >> (x / scale))) != 0U) {
                    unsigned pixel_x = origin_x + x;
                    row[(size_t)pixel_x / 2U * 3U + 1U + (pixel_x & 1U)] = 235U;
                }
            }
        }
    }
    return true;
}
