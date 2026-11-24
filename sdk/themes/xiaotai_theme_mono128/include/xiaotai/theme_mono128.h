// SPDX-License-Identifier: MIT
#pragma once
/* Monochrome theme for 128x64 one-bit displays without a pointer. It renders
 * whole frames itself (FRAME backend, XIAOTAI_UI_MONO_FRAME_BYTES(128, 64) =
 * 1024 bytes, in the frame layout of the display it serves) and needs no
 * drawing library, so it fits chips without external RAM. Views are operated
 * with the navigation intents of the UI system (xiaotai/ui.h): the focused
 * entry is drawn inverted, lists show three rows around the focus, long
 * focused text scrolls.
 *
 * Match: 100 for a 128x64 display whose pixel format is
 * XIAOTAI_PIXEL_MONO1_VLSB or XIAOTAI_PIXEL_MONO1; 0 otherwise, and 0 for a
 * display with XIAOTAI_DISPLAY_SLOW_REFRESH (the talk animation and text
 * scrolling redraw several times a second).
 *
 * Stack: it asks for XIAOTAI_UI_OWNER_THEME_STACK_BYTES, so on a board
 * without external RAM its display needs no UI task (XIAOTAI_UI_TASK 0):
 * the frame and glyph rows live in the theme's state, never on the stack.
 *
 * What it draws of a view: title and status in the header, items or fields as
 * rows, text, the code in large digits with the remaining seconds, the timer
 * beside the title, toasts as an inverted band at the bottom. TALK is the
 * exception to the header rule: its status ("connecting", "speak now") is
 * the centred state line of the body, above the talk animation drawn from
 * phase, and its text (captions) is not drawn, for want of room. HOME items
 * are drawn as the home menu. It ignores images that are not MONO1,
 * indicators it has no glyph for, and XIAOTAI_VIEW_VIDEO; actions are reached
 * through their role, and a view's hint line names them. */
#include "xiaotai/port/err.h"
#include "xiaotai/ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_THEME_MONO128_NAME "mono128"
#define XIAOTAI_THEME_MONO128_WIDTH 128U
#define XIAOTAI_THEME_MONO128_HEIGHT 64U
#define XIAOTAI_THEME_MONO128_FRAME_BYTES 1024U
#define XIAOTAI_THEME_MONO128_ROWS 3U   /* list rows visible at once */

extern const xiaotai_theme_t xiaotai_theme_mono128;

/* Lifecycle unit "theme_mono128" (theme.json): registers the theme with the
 * UI system. */
xiaotai_err_t xiaotai_engine_theme_mono128_init(void);

#ifdef __cplusplus
}
#endif
