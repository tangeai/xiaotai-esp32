// SPDX-License-Identifier: MIT
#include "lvgl.h"

#include <assert.h>
#include <stdio.h>

LV_FONT_DECLARE(ui_font_cn_18);
LV_FONT_DECLARE(ui_font_cn_18_supplement);

static bool has_ink(lv_font_glyph_dsc_t *glyph)
{
    lv_draw_buf_t *buf = lv_draw_buf_create(glyph->box_w, glyph->box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    const lv_draw_buf_t *bitmap = buf ? lv_font_get_glyph_bitmap(glyph, buf) : NULL;
    bool ink = false;
    if (bitmap) {
        for (unsigned y = 0; y < glyph->box_h && !ink; ++y) {
            const uint8_t *row = bitmap->data + y * bitmap->header.stride;
            for (unsigned x = 0; x < glyph->box_w; ++x) ink |= row[x] != 0;
        }
    }
    if (buf) lv_draw_buf_destroy(buf);
    return ink;
}

int main(void)
{
    lv_init();
    assert(ui_font_cn_18.line_height == 20 && ui_font_cn_18.base_line == 3);
    assert(ui_font_cn_18_supplement.line_height == 20 && ui_font_cn_18_supplement.base_line == 3);
    assert(ui_font_cn_18.fallback == &ui_font_cn_18_supplement);
    unsigned base = 0, supplement = 0;
    for (uint32_t cp = 0x20; cp <= 0xffff; ++cp) {
        lv_font_glyph_dsc_t glyph = {0};
        bool found = lv_font_get_glyph_dsc(&ui_font_cn_18, &glyph, cp, 0) && !glyph.is_placeholder;
        if (cp == 0x2252) assert(!found);
        if (!found) continue;
        if (glyph.resolved_font == &ui_font_cn_18) {
            assert(glyph.format == LV_FONT_GLYPH_FORMAT_A2);
            ++base;
        } else {
            assert(glyph.resolved_font == &ui_font_cn_18_supplement && glyph.format == LV_FONT_GLYPH_FORMAT_A4);
            ++supplement;
        }
        assert(glyph.ofs_y >= -3 && glyph.ofs_y + glyph.box_h <= 17);
        if (cp == 0x20 || cp == 0x3000) {
            assert(glyph.box_w == 0 && glyph.box_h == 0);
        } else if (!glyph.box_w || !glyph.box_h || !has_ink(&glyph)) {
            fprintf(stderr, "empty LCD font glyph U+%04X\n", (unsigned)cp);
            return 1;
        }
    }
    assert(base == 21337 && supplement == 131);
    lv_deinit();
    printf("LCD font: %u A2 primary and %u A4 supplemental glyphs\n", base, supplement);
    return 0;
}
