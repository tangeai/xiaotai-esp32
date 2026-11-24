// SPDX-License-Identifier: MIT
#pragma once

#include "../assets/fonts/coverage.h"
#include "lvgl.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

extern const lv_font_t door_font_small, door_font_button, door_font_body;
extern const lv_font_t door_font_dialog, door_font_title, door_font_clock;

static void door_wide_verify_fonts(void)
{
    static const struct {
        const char *name;
        const lv_font_t *font;
        const uint32_t *codepoints;
        size_t count;
    } subsets[] = {
#define DOOR_FONT_SUBSET(font, codepoints) {#font, &font, codepoints, sizeof(codepoints) / sizeof(*codepoints)}
        DOOR_FONT_SUBSET(door_font_small, door_text_codepoints),
        DOOR_FONT_SUBSET(door_font_button, door_text_codepoints),
        DOOR_FONT_SUBSET(door_font_body, door_text_codepoints),
        DOOR_FONT_SUBSET(door_font_dialog, door_text_codepoints),
        DOOR_FONT_SUBSET(door_font_title, door_text_codepoints),
        DOOR_FONT_SUBSET(door_font_clock, door_clock_codepoints),
#undef DOOR_FONT_SUBSET
    };
    for (size_t i = 0; i < sizeof(subsets) / sizeof(*subsets); ++i) {
        const lv_font_t *font = subsets[i].font;
        for (size_t n = 0; n < subsets[i].count; ++n) {
            uint32_t cp = subsets[i].codepoints[n];
            lv_font_glyph_dsc_t glyph = {0};
            bool found = lv_font_get_glyph_dsc(font, &glyph, cp, 0) &&
                         glyph.resolved_font == font && !glyph.is_placeholder &&
                         glyph.adv_w && glyph.format == LV_FONT_GLYPH_FORMAT_A4;
            if (found && cp != ' ') {
                lv_draw_buf_t *buf = glyph.box_w && glyph.box_h
                    ? lv_draw_buf_create(glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO)
                    : NULL;
                const lv_draw_buf_t *bitmap = buf ? lv_font_get_glyph_bitmap(&glyph, buf) : NULL;
                found = false;
                if (bitmap) {
                    for (size_t y = 0; !found && y < glyph.box_h; ++y) {
                        const uint8_t *row = bitmap->data + y * bitmap->header.stride;
                        for (size_t x = 0; !found && x < glyph.box_w; ++x) found = row[x] != 0;
                    }
                }
                if (buf) lv_draw_buf_destroy(buf);
            }
            if (!found) {
                fprintf(stderr, "%s: absent/empty glyph U+%04" PRIX32 "\n", subsets[i].name, cp);
                abort();
            }
        }
    }
}
