// SPDX-License-Identifier: MIT
#pragma once
/* Build configuration of the colour theme kit. On ESP-IDF the values come from
 * the component's Kconfig; elsewhere define the CONFIG_ names on the compiler
 * command line. */
#include "xiaotai/config.h"

/* The product carries the 26 px text face, the 28 px icons and the 96 px
 * digits, and draws displays of 800 x 480 and more with them. */
#define XIAOTAI_THEME_LCD_LARGE_TEXT XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_THEME_LCD_LARGE_TEXT, 0)
