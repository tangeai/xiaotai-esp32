// SPDX-License-Identifier: MIT
#include <string.h>

#include "lcd_face.h"
#include "xiaotai/theme_lcd.h"

size_t lcd_face_resolve(const char *name)
{
    if (!name) return 0;
    static const struct { const char *from, *to; } aliases[] = {
        {"calm", "neutral"}, {"excited", "laughing"}, {"curious", "confused"},
        {"proud", "confident"}, {"moved", "loving"}, {"speech", "listening"},
        /* What an AI service's words come to: a theme may have faces of its own for these. */
        {"gentle", "relaxed"}, {"comfort", "loving"}, {"serious", "confident"},
        {"ringing", "surprised"}, {"dialing", "listening"}, {"offline", "confused"}, {"error", "sad"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); ++i) {
        if (strcmp(aliases[i].from, name) == 0) {
            name = aliases[i].to;
            break;
        }
    }
    for (size_t i = 0; i < XIAOTAI_THEME_LCD_EMOTION_COUNT; ++i) {
        if (strcmp(xiaotai_theme_lcd_emotion_name(i), name) == 0) return i;
    }
    return 0; /* The authored fallback pose is slot zero. */
}

size_t lcd_face_manner(const char *name)
{
    static const char *const states[] = {"ringing", "dialing", "offline", "error"};
    for (size_t i = 0; name && i < sizeof(states) / sizeof(*states); ++i)
        if (!strcmp(name, states[i])) return LCD_FACE_RINGING + i;
    return lcd_face_resolve(name);
}
