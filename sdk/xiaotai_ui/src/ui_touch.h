// SPDX-License-Identifier: MIT
#pragma once
/* What a touch panel reports, turned into what a finger does: no toolkit, no
 * clock, no task, so it runs on the host with recorded or made-up traces.
 *
 *   jitter       a One Euro filter: a slow finger is smoothed, a fast one is
 *                followed closely. The first sample of a contact passes as it
 *                is, so a tap is neither late nor displaced.
 *   spikes       a jump no finger makes in one sample waits for the next one,
 *                which either confirms it (another finger) or shows a glitch.
 *   gaps         a lost sample while dragging does not end the drag; a tap
 *                that did not move ends with its first empty sample.
 *   prediction   a moving finger is drawn a few milliseconds ahead, by at most
 *                a few pixels, which makes up for what the filter lags.
 *   velocity     of the last tenth of a second, for what coasts on after the
 *                finger left.
 *
 * Distances are in units: 1/240 of the display's shorter side, so a setting
 * means the same movement of a finger on every size. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t at_us;
    int32_t x, y;               /* display pixels */
    bool down;
    bool failed;                /* the read failed: the contact, if any, is void */
} ui_sample_t;

typedef struct {
    int64_t at_us;              /* of the sample it comes from */
    int32_t x, y;
    bool down;
    float vx, vy;               /* px/s, of a contact that ends */
} ui_contact_t;

#define UI_TOUCH_HISTORY 16U

typedef struct {
    uint16_t width, height;
    float unit;                 /* pixels */
    bool down, moved, lifting, suspect;
    int64_t at_us, lift_us;
    float x, y;                 /* filtered */
    float vx, vy;               /* smoothed, px/s */
    float raw_x, raw_y, press_x, press_y, jump_x, jump_y;
    int32_t out_x, out_y;
    struct { int64_t at_us; float x, y; } history[UI_TOUCH_HISTORY];
    uint8_t used, head;
} ui_touch_t;

void ui_touch_init(ui_touch_t *t, uint16_t width, uint16_t height);
/* Forget the contact without reporting its end. */
void ui_touch_reset(ui_touch_t *t);
/* One sample in, up to two contacts out (a confirmed jump ends one contact
 * and starts another). Samples come in order, and go on for at least two
 * periods after the finger left. */
size_t ui_touch_feed(ui_touch_t *t, const ui_sample_t *sample, ui_contact_t out[2]);
