// SPDX-License-Identifier: MIT
#include "xiaotai/apps/companion.h"
#include "xiaotai/theme_lcd.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const size_t app_count = XIAOTAI_APP_COMPANION_EMOTIONS;
    const size_t theme_count = XIAOTAI_THEME_LCD_EMOTION_COUNT;
    if (app_count != theme_count || xiaotai_app_companion_emotion(app_count) ||
        xiaotai_theme_lcd_emotion_name(theme_count)) {
        fprintf(stderr, "emotion catalog sizes or end markers disagree: app=%zu theme=%zu\n", app_count, theme_count);
        return 1;
    }

    const char *theme_names[XIAOTAI_THEME_LCD_EMOTION_COUNT];
    bool seen[XIAOTAI_THEME_LCD_EMOTION_COUNT] = {0};
    for (size_t i = 0; i < theme_count; ++i) {
        theme_names[i] = xiaotai_theme_lcd_emotion_name(i);
        if (!theme_names[i] || !theme_names[i][0]) {
            fprintf(stderr, "theme emotion %zu is empty\n", i);
            return 1;
        }
    }
    /* Only the application owns the stored index order. The theme exports a set. */
    for (size_t i = 0; i < app_count; ++i) {
        const char *name = xiaotai_app_companion_emotion(i);
        if (!name || !name[0]) {
            fprintf(stderr, "companion emotion %zu is empty\n", i);
            return 1;
        }
        size_t j = 0;
        while (j < theme_count && strcmp(name, theme_names[j]) != 0) ++j;
        if (j == theme_count || seen[j]) {
            fprintf(stderr, "companion emotion %zu (%s) has no unique match in the theme catalog\n", i, name);
            return 1;
        }
        seen[j] = true;
    }
    return 0;
}
