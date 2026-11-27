// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What keeps an expression alive: where it looks, how it blinks and breathes,
 * the small things it does when nothing happens, how it answers a finger, and
 * the mouth that follows the sound. It knows no pixels of a display: it takes
 * what the face senses and gives offsets for the parts of a face drawn on a
 * canvas of 240 x 124 units, the same for every display. */

/* A pointer on the canvas of the face; far outside it when it is elsewhere. */
typedef struct {
    bool down;
    int16_t x, y;
    bool hears;                 /* capture and playout are measured */
    uint16_t capture, playout;  /* sample amplitudes, full scale 32768 */
} lcd_face_senses_t;

/* What goes on slowly all the time: it breathes, it rocks. Its parts are to
 * move on together, and only when that would be seen. */
typedef struct { float rise, sway, tilt, squash; } lcd_face_drift_t;

/* What a frame of the face adds to the drawn pose. */
typedef struct {
    float gaze_x, gaze_y;       /* pixels; the eyes turn with it */
    float open[2];              /* 1: as drawn; 0: closed */
    float squash;               /* above 0: wider and lower, below 0: taller and narrower */
    float rise, sway;           /* the whole face, pixels; up is negative */
    float tilt;                 /* pixels an eye is lower for 64 to the side */
    float mouth_open;           /* 0 .. 1 */
    float mouth_wide;           /* 1: as drawn */
    lcd_face_drift_t slow;      /* on top of the above */
    float pupil;                /* 1: as designed */
    float listen;               /* 0 .. 1: the light in the eyes of one who listens */
    float brow;                 /* pixels the brows are up */
    float shade;                /* 0 .. 1: the eyes dimmed (dozing, offline) */
} lcd_face_frame_t;

enum { LCD_FACE_MOOD_NONE = -1 };

typedef struct {
    uint32_t random;
    int64_t now, mood_at, touched_at, calm_since;
    int mood;                   /* pose index the face was asked to show */
    uint8_t phase, level;
    /* looking */
    float gaze_x, gaze_y, aim_x, aim_y;
    int64_t look_next;
    bool follow;
    /* blinking */
    int64_t blink_at, blink_next;
    uint8_t blink_left;
    bool blink_slow;
    float lid[2], lid_to[2];
    /* springs: value and speed */
    float squash, squash_v, rise, rise_v, sway, sway_v, tilt, tilt_v;
    float squash_to, rise_to, sway_to, tilt_to;
    /* little acts and reactions */
    uint8_t act, last_act;
    int64_t act_at, act_next;
    int override_mood;
    int64_t override_until;
    /* finger */
    bool down, petting, pet_done;
    int16_t down_x, down_y, last_x, last_y;
    float path;
    int64_t down_at, poke_at;
    uint8_t pokes;
    /* sound */
    float mouth, wide, loud, loud_ref, heard, heard_ref, heard_floor, ring, brow, brow_v;
    int64_t sound_at, voice_at;
    bool nod_due;
    float pupil, shade;
} lcd_face_life_t;

void lcd_face_life_start(lcd_face_life_t *life, int64_t now_ms);
/* The face was asked for another expression or phase. */
void lcd_face_life_mood(lcd_face_life_t *life, int mood, uint8_t phase, uint8_t level, int64_t now_ms);
/* One frame. The result is the pose index to show instead of the one asked
 * for (a reaction, an act), or LCD_FACE_MOOD_NONE. */
int lcd_face_life_step(lcd_face_life_t *life, int64_t now_ms, const lcd_face_senses_t *senses, lcd_face_frame_t *out);
/* Whether the finger that is being lifted was stroking the face: that is no tap. */
bool lcd_face_life_stroked(const lcd_face_life_t *life);
