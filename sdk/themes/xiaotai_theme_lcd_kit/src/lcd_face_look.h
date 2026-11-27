// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

/* How each expression looks: one number for each thing a face can do. Lengths
 * are in design units, 240 x 124 of them to the canvas, whatever the display.
 * An expression is never a picture: going from one to another is every number
 * on its way to its new value, so there is no cut between any two. */

typedef enum {
    /* eyes: both, then left and right where they may differ */
    LOOK_EYE_W, LOOK_EYE_H,
    LOOK_EYE_ROUND,             /* 0 .. 1 of the largest corner */
    LOOK_EYE_SOFT,              /* 1: one smooth curve, 0: a bar with round ends */
    LOOK_EYE_BEND,              /* above 0: an arc whose ends hang (a smile) */
    LOOK_EYE_SHEAR,             /* inner ends lower */
    LOOK_EYE_Y, LOOK_EYE_GAP,   /* from the middle of the face to the middle of an eye */
    LOOK_GROW_L, LOOK_GROW_R,   /* one eye larger */
    LOOK_UPPER_L, LOOK_UPPER_R, /* 0 .. 1: how far the upper lid is down */
    LOOK_UPPER_TILT,            /* inner ends lower: cross; higher: sad */
    LOOK_UPPER_ARCH,
    LOOK_LOWER_L, LOOK_LOWER_R, /* 0 .. 1: how far the lower lid is up */
    LOOK_LOWER_ARCH,
    LOOK_IRIS,                  /* size, 1 as designed, 0 none */
    LOOK_PUPIL,                 /* of the iris */
    LOOK_HEARTS,                /* 0 .. 1: in place of the irises */
    LOOK_CROSS,                 /* 0 .. 1: both look at the nose */
    LOOK_WINK,                  /* 0 .. 1: the right one winks now and then */
    LOOK_WET,                   /* 0 .. 1: brimming */
    LOOK_GAZE_X, LOOK_GAZE_Y,
    /* brows */
    LOOK_BROW, LOOK_BROW_Y, LOOK_BROW_TILT, LOOK_BROW_BEND, LOOK_BROW_RAISE_L, LOOK_BROW_RAISE_R,
    /* mouth */
    LOOK_MOUTH_X, LOOK_MOUTH_Y, LOOK_MOUTH_W, LOOK_MOUTH_H, LOOK_MOUTH_BEND, LOOK_MOUTH_SHEAR,
    LOOK_MOUTH_FLAT,            /* 0 .. 1: a flat upper lip (an open smile) */
    LOOK_MOUTH_OPEN,            /* 0 .. 1: its inside shows */
    LOOK_MOUTH_WAVE, LOOK_MOUTH_PUCKER,
    LOOK_TONGUE, LOOK_TONGUE_X, LOOK_TEETH,
    /* skin */
    LOOK_CHEEK, LOOK_HATCH, LOOK_LEAN,
    LOOK_RED, LOOK_GREEN, LOOK_BLUE,
    /* things around it, 0 .. 1 */
    LOOK_TEARS, LOOK_SWEAT, LOOK_HEART, LOOK_KISS, LOOK_NOTES, LOOK_ZZZ, LOOK_QUESTION, LOOK_EXCLAIM, LOOK_SPARKLE,
    LOOK_VEIN, LOOK_GLASSES, LOOK_THOUGHT,
    LOOK_COUNT
} look_channel_t;

typedef struct {
    const char *name;
    float value[LOOK_COUNT];
} look_t;

enum { LOOK_NEUTRAL, LOOK_SADNESS = 4, LOOK_THINKING = 11, LOOK_LISTENING = 21 };

/* The expressions in the order of lcd_face_manner(): the 23 of the catalogue, then
 * ringing, dialing, offline and error. */
extern const look_t lcd_face_looks[];
/* How quickly each number gets to its new value, in swings a second. */
float lcd_face_look_pace(look_channel_t channel);
