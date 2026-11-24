// SPDX-License-Identifier: MIT
#pragma once
/* Shared pieces of the UI system's host tests: the check macro, a fake board
 * whose display parts a test sets (ui_fake_board.cc), and fake themes and
 * templates whose scores and results a test sets (ui_fake_theme.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "xiaotai/hal.h"
#include "xiaotai/ui.h"
#include "xiaotai/ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#define RUN(test)                                              \
    do {                                                       \
        if (test() != 0) {                                     \
            fprintf(stderr, "FAIL: %s\n", #test);              \
            return 1;                                          \
        }                                                      \
    } while (0)

/* ------------------------------------------------------------ fake board */

#define UI_FAKE_DISPLAYS_MAX 4U

/* The board's displays from now on (the board reads the table on every
 * query, so a test may change it after xiaotai_hal_init()). Counters and
 * injected failures are cleared. */
void ui_fake_board_set_displays(const xiaotai_display_desc_t *descs, size_t count);
/* open() of display `index` returns `err` from now on. */
void ui_fake_display_fail_open(size_t index, xiaotai_err_t err);
uint32_t ui_fake_display_opens(size_t index);
const xiaotai_display_t *ui_fake_display(size_t index);

xiaotai_display_desc_t ui_fake_desc(uint16_t width, uint16_t height, xiaotai_pixel_t pixel);

/* ----------------------------------------------------------- fake themes */

#define UI_FAKE_THEMES_MAX 10U

/* How a fake theme scores a display: `exact` for exactly width x height in
 * `pixel`, `other` for any other display in `pixel`, 0 for other pixel
 * formats and for displays with XIAOTAI_DISPLAY_SLOW_REFRESH. */
typedef struct {
    const char *name;
    xiaotai_ui_backend_t backend;
    uint16_t width, height;
    xiaotai_pixel_t pixel;
    int exact;
    int other;
    xiaotai_err_t start_result;
} ui_fake_theme_config_t;

/* The theme structure of fake `index` configured as given; register it with
 * xiaotai_ui_theme_register(). Counters are cleared. */
const xiaotai_theme_t *ui_fake_theme(size_t index, const ui_fake_theme_config_t *config);
uint32_t ui_fake_theme_starts(size_t index);
uint32_t ui_fake_theme_stops(size_t index);
/* Host the fake theme was last started with for display `display`, NULL when
 * it was not started there. */
const xiaotai_ui_host_t *ui_fake_theme_host(size_t index, uint8_t display);

/* -------------------------------------------------------- fake templates */

#define UI_FAKE_TEMPLATES_MAX 20U

/* A template named `name` for `backend` scoring `exact` on displays
 * `width` wide and `other` on any other. */
const xiaotai_ui_template_t *ui_fake_template(size_t index, const char *name, xiaotai_ui_backend_t backend,
                                              uint16_t width, int exact, int other);

#ifdef __cplusplus
}
#endif
