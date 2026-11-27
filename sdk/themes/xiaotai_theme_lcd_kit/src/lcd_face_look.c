// SPDX-License-Identifier: MIT
#include "lcd_face_look.h"

#include "lcd_face.h"

/* The face at rest. An expression says only what it does differently. */
#define REST \
    [LOOK_EYE_W] = 64, [LOOK_EYE_H] = 46, [LOOK_EYE_ROUND] = 1, [LOOK_EYE_SOFT] = 1, [LOOK_EYE_Y] = 57, [LOOK_EYE_GAP] = 60, \
    [LOOK_IRIS] = 1, [LOOK_PUPIL] = 0.50f, [LOOK_BROW_Y] = 18, \
    [LOOK_MOUTH_Y] = 97, [LOOK_MOUTH_W] = 33, [LOOK_MOUTH_H] = 7.5f, [LOOK_MOUTH_BEND] = -4

#define TINT(rgb) [LOOK_RED] = (float)((rgb) >> 16 & 255), [LOOK_GREEN] = (float)((rgb) >> 8 & 255), [LOOK_BLUE] = (float)((rgb) & 255)
/* Eyes shut in a smile: thick arcs. */
#define SMILING(w, h, b) [LOOK_EYE_W] = (w), [LOOK_EYE_H] = (h), [LOOK_EYE_SOFT] = 0, [LOOK_EYE_BEND] = (b), [LOOK_IRIS] = 0, [LOOK_EYE_Y] = 58

/* Later entries of an initializer replace earlier ones: that is how a face differs from REST. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"

const look_t lcd_face_looks[] = {
    {"neutral", {REST, TINT(0xEAF6F2)}},
    {"happy", {REST, TINT(0xDDF8EC), SMILING(58, 12, 20), [LOOK_MOUTH_W] = 42, [LOOK_MOUTH_H] = 13, [LOOK_MOUTH_BEND] = -7,
        [LOOK_MOUTH_FLAT] = 0.55f, [LOOK_MOUTH_OPEN] = 0.55f, [LOOK_TONGUE] = 0.5f, [LOOK_CHEEK] = 0.55f}},
    {"laughing", {REST, TINT(0xDDF8EC), SMILING(57, 12, 24), [LOOK_EYE_Y] = 55, [LOOK_MOUTH_W] = 54, [LOOK_MOUTH_H] = 27,
        [LOOK_MOUTH_BEND] = -6, [LOOK_MOUTH_FLAT] = 0.75f, [LOOK_MOUTH_OPEN] = 1, [LOOK_TONGUE] = 1, [LOOK_TEETH] = 1,
        [LOOK_MOUTH_Y] = 96, [LOOK_CHEEK] = 0.75f}},
    {"funny", {REST, TINT(0xF6EEDC), [LOOK_GROW_L] = 0.06f, [LOOK_GROW_R] = -0.16f, [LOOK_UPPER_R] = 0.22f, [LOOK_LOWER_R] = 0.24f,
        [LOOK_LOWER_ARCH] = 5, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 20, [LOOK_BROW_RAISE_L] = 8, [LOOK_BROW_RAISE_R] = -3, [LOOK_BROW_BEND] = 5,
        [LOOK_GAZE_X] = 5, [LOOK_MOUTH_W] = 40, [LOOK_MOUTH_H] = 9, [LOOK_MOUTH_BEND] = -6, [LOOK_MOUTH_SHEAR] = -6,
        [LOOK_MOUTH_X] = 7, [LOOK_TONGUE] = 0, [LOOK_LEAN] = 2}},
    {"sad", {REST, TINT(0xD7E3F6), [LOOK_UPPER_L] = 0.14f, [LOOK_UPPER_R] = 0.14f, [LOOK_UPPER_TILT] = -11, [LOOK_IRIS] = 1.06f,
        [LOOK_LOWER_L] = 0.06f, [LOOK_LOWER_R] = 0.06f, [LOOK_WET] = 0.7f, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 21, [LOOK_BROW_TILT] = -8,
        [LOOK_BROW_BEND] = -2, [LOOK_GAZE_Y] = 5, [LOOK_MOUTH_W] = 28, [LOOK_MOUTH_BEND] = 8, [LOOK_MOUTH_Y] = 98}},
    {"angry", {REST, TINT(0xF6DCE2), [LOOK_UPPER_L] = 0.20f, [LOOK_UPPER_R] = 0.20f, [LOOK_UPPER_TILT] = 13, [LOOK_LOWER_L] = 0.10f,
        [LOOK_LOWER_R] = 0.10f, [LOOK_LOWER_ARCH] = 2, [LOOK_IRIS] = 0.80f, [LOOK_PUPIL] = 0.42f, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 27,
        [LOOK_BROW_TILT] = 11, [LOOK_BROW_BEND] = 1, [LOOK_MOUTH_W] = 30, [LOOK_MOUTH_H] = 9, [LOOK_MOUTH_BEND] = 9,
        [LOOK_VEIN] = 1, [LOOK_CHEEK] = 0.25f}},
    {"crying", {REST, TINT(0xDCE8F8), [LOOK_EYE_W] = 56, [LOOK_EYE_H] = 11, [LOOK_EYE_SOFT] = 0, [LOOK_EYE_BEND] = -13, [LOOK_IRIS] = 0,
        [LOOK_EYE_Y] = 54, [LOOK_EYE_SHEAR] = -5, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 30, [LOOK_BROW_TILT] = -9, [LOOK_MOUTH_W] = 40,
        [LOOK_MOUTH_H] = 20, [LOOK_MOUTH_BEND] = 7, [LOOK_MOUTH_OPEN] = 1, [LOOK_TONGUE] = 0.8f, [LOOK_TEARS] = 1, [LOOK_CHEEK] = 0.4f}},
    {"loving", {REST, TINT(0xFBE6EE), [LOOK_HEARTS] = 1, [LOOK_IRIS] = 1.12f, [LOOK_LOWER_L] = 0.05f, [LOOK_LOWER_R] = 0.05f,
        [LOOK_LOWER_ARCH] = 3, [LOOK_MOUTH_W] = 38, [LOOK_MOUTH_H] = 11, [LOOK_MOUTH_BEND] = -7, [LOOK_MOUTH_FLAT] = 0.4f,
        [LOOK_MOUTH_OPEN] = 0.5f, [LOOK_TONGUE] = 0.5f, [LOOK_CHEEK] = 0.8f, [LOOK_HEART] = 1}},
    {"embarrassed", {REST, TINT(0xF0E7F4), [LOOK_UPPER_L] = 0.16f, [LOOK_UPPER_R] = 0.16f, [LOOK_UPPER_TILT] = -5, [LOOK_LOWER_L] = 0.14f,
        [LOOK_LOWER_R] = 0.14f, [LOOK_LOWER_ARCH] = 4, [LOOK_GAZE_X] = -9, [LOOK_GAZE_Y] = 5, [LOOK_BROW] = 0.9f, [LOOK_BROW_Y] = 20,
        [LOOK_BROW_TILT] = -5, [LOOK_MOUTH_W] = 30, [LOOK_MOUTH_H] = 6, [LOOK_MOUTH_WAVE] = 3, [LOOK_MOUTH_BEND] = 0, [LOOK_CHEEK] = 1, [LOOK_HATCH] = 1}},
    {"surprised", {REST, TINT(0xF8F0DC), [LOOK_EYE_W] = 62, [LOOK_EYE_H] = 54, [LOOK_IRIS] = 0.82f, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 11,
        [LOOK_BROW_BEND] = 5, [LOOK_MOUTH_W] = 20, [LOOK_MOUTH_H] = 22, [LOOK_MOUTH_BEND] = 0, [LOOK_MOUTH_OPEN] = 1, [LOOK_MOUTH_Y] = 98,
        [LOOK_TONGUE] = 0.3f, [LOOK_EXCLAIM] = 1}},
    {"shocked", {REST, TINT(0xE4F0F8), [LOOK_EYE_W] = 62, [LOOK_EYE_H] = 57, [LOOK_IRIS] = 0.52f, [LOOK_PUPIL] = 0.60f, [LOOK_BROW] = 1,
        [LOOK_BROW_Y] = 9, [LOOK_BROW_TILT] = -5, [LOOK_BROW_BEND] = 3, [LOOK_MOUTH_W] = 30, [LOOK_MOUTH_H] = 24, [LOOK_MOUTH_BEND] = 3,
        [LOOK_MOUTH_OPEN] = 1, [LOOK_MOUTH_Y] = 99, [LOOK_SWEAT] = 1}},
    {"thinking", {REST, TINT(0xE0EFEA), [LOOK_UPPER_L] = 0.22f, [LOOK_UPPER_R] = 0.06f, [LOOK_LOWER_L] = 0.12f, [LOOK_LOWER_R] = 0.04f,
        [LOOK_GAZE_X] = 9, [LOOK_GAZE_Y] = -6, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 21, [LOOK_BROW_RAISE_L] = -4, [LOOK_BROW_RAISE_R] = 5,
        [LOOK_BROW_BEND] = 4, [LOOK_MOUTH_X] = -8, [LOOK_MOUTH_W] = 20, [LOOK_MOUTH_BEND] = 0, [LOOK_MOUTH_SHEAR] = -3, [LOOK_THOUGHT] = 1}},
    {"winking", {REST, TINT(0xDDF8EC), [LOOK_WINK] = 1, [LOOK_UPPER_L] = 0.02f, [LOOK_LOWER_L] = 0.08f, [LOOK_LOWER_ARCH] = 3, [LOOK_BROW] = 0.9f,
        [LOOK_BROW_Y] = 18, [LOOK_BROW_BEND] = 5, [LOOK_BROW_RAISE_L] = 4, [LOOK_BROW_RAISE_R] = -2, [LOOK_MOUTH_W] = 36, [LOOK_MOUTH_H] = 10,
        [LOOK_MOUTH_BEND] = -7, [LOOK_MOUTH_SHEAR] = -3, [LOOK_MOUTH_FLAT] = 0.4f, [LOOK_MOUTH_OPEN] = 0.4f, [LOOK_TONGUE] = 0.4f,
        [LOOK_CHEEK] = 0.5f, [LOOK_SPARKLE] = 1}},
    {"cool", {REST, TINT(0xD6ECF6), [LOOK_GLASSES] = 1, [LOOK_IRIS] = 0, [LOOK_EYE_H] = 34, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 27,
        [LOOK_BROW_BEND] = 3, [LOOK_BROW_RAISE_R] = 4, [LOOK_MOUTH_W] = 36, [LOOK_MOUTH_BEND] = -6, [LOOK_MOUTH_SHEAR] = -5, [LOOK_MOUTH_X] = 4}},
    {"relaxed", {REST, TINT(0xF2EBDC), [LOOK_UPPER_L] = 0.34f, [LOOK_UPPER_R] = 0.34f, [LOOK_UPPER_ARCH] = 3, [LOOK_LOWER_L] = 0.12f,
        [LOOK_LOWER_R] = 0.12f, [LOOK_LOWER_ARCH] = 4, [LOOK_MOUTH_W] = 36, [LOOK_MOUTH_BEND] = -6, [LOOK_CHEEK] = 0.35f, [LOOK_NOTES] = 1}},
    {"delicious", {REST, TINT(0xFBE6EC), SMILING(56, 12, 18), [LOOK_MOUTH_W] = 40, [LOOK_MOUTH_H] = 9, [LOOK_MOUTH_BEND] = -8,
        [LOOK_TONGUE] = -1, [LOOK_TONGUE_X] = 10, [LOOK_CHEEK] = 0.8f, [LOOK_SPARKLE] = 0.7f}},
    {"kissy", {REST, TINT(0xFBE6EC), SMILING(54, 11, 16), [LOOK_MOUTH_W] = 15, [LOOK_MOUTH_H] = 17, [LOOK_MOUTH_BEND] = 0,
        [LOOK_MOUTH_PUCKER] = 1, [LOOK_CHEEK] = 0.9f, [LOOK_KISS] = 1}},
    {"confident", {REST, TINT(0xE4F2EA), [LOOK_UPPER_L] = 0.26f, [LOOK_UPPER_R] = 0.26f, [LOOK_UPPER_TILT] = 4, [LOOK_LOWER_L] = 0.14f,
        [LOOK_LOWER_R] = 0.14f, [LOOK_LOWER_ARCH] = 4, [LOOK_GAZE_Y] = -2, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 23, [LOOK_BROW_TILT] = 5,
        [LOOK_BROW_BEND] = 3, [LOOK_MOUTH_W] = 38, [LOOK_MOUTH_BEND] = -7, [LOOK_MOUTH_SHEAR] = -4, [LOOK_MOUTH_X] = 3, [LOOK_SPARKLE] = 0.9f}},
    {"sleepy", {REST, TINT(0xD6DAEE), [LOOK_UPPER_L] = 0.56f, [LOOK_UPPER_R] = 0.56f, [LOOK_UPPER_ARCH] = 2, [LOOK_IRIS] = 0.94f,
        [LOOK_GAZE_Y] = 5, [LOOK_MOUTH_W] = 22, [LOOK_MOUTH_H] = 8, [LOOK_MOUTH_BEND] = -1, [LOOK_MOUTH_OPEN] = 0.2f, [LOOK_ZZZ] = 1}},
    {"silly", {REST, TINT(0xDDF8EC), [LOOK_GROW_L] = 0.06f, [LOOK_GROW_R] = -0.06f, [LOOK_CROSS] = 1, [LOOK_IRIS] = 0.86f,
        [LOOK_BROW] = 0.8f, [LOOK_BROW_Y] = 14, [LOOK_BROW_BEND] = 5, [LOOK_MOUTH_W] = 44, [LOOK_MOUTH_H] = 13, [LOOK_MOUTH_BEND] = -8,
        [LOOK_MOUTH_FLAT] = 0.6f, [LOOK_MOUTH_OPEN] = 0.8f, [LOOK_TONGUE] = -1, [LOOK_TONGUE_X] = -4, [LOOK_CHEEK] = 0.6f}},
    {"confused", {REST, TINT(0xE6F0EE), [LOOK_GROW_L] = 0.08f, [LOOK_GROW_R] = -0.10f, [LOOK_UPPER_R] = 0.20f, [LOOK_BROW] = 1,
        [LOOK_BROW_Y] = 18, [LOOK_BROW_RAISE_L] = 6, [LOOK_BROW_RAISE_R] = -4, [LOOK_BROW_TILT] = -2, [LOOK_BROW_BEND] = 4, [LOOK_GAZE_X] = -4,
        [LOOK_GAZE_Y] = -3, [LOOK_MOUTH_W] = 32, [LOOK_MOUTH_H] = 6, [LOOK_MOUTH_WAVE] = 3.4f, [LOOK_MOUTH_BEND] = 0, [LOOK_LEAN] = 2, [LOOK_QUESTION] = 1}},
    /* Wide awake and turned to the speaker: brows up, eyes larger, nothing beside the face. */
    {"listening", {REST, TINT(0xE4F2F8), [LOOK_EYE_W] = 66, [LOOK_EYE_H] = 51, [LOOK_IRIS] = 1.08f, [LOOK_PUPIL] = 0.56f, [LOOK_BROW] = 1,
        [LOOK_BROW_Y] = 14, [LOOK_BROW_BEND] = 6, [LOOK_MOUTH_W] = 22, [LOOK_MOUTH_H] = 7, [LOOK_MOUTH_BEND] = -3, [LOOK_MOUTH_Y] = 98}},
    {"ambient", {REST, TINT(0xDDEEE8), [LOOK_UPPER_L] = 0.20f, [LOOK_UPPER_R] = 0.20f, [LOOK_UPPER_ARCH] = 2, [LOOK_GAZE_Y] = -2,
        [LOOK_MOUTH_W] = 26, [LOOK_LEAN] = 1}},
    {"ringing", {REST, TINT(0xF8F0DC), [LOOK_EYE_W] = 64, [LOOK_EYE_H] = 51, [LOOK_BROW] = 1, [LOOK_BROW_Y] = 12, [LOOK_BROW_BEND] = 6,
        [LOOK_MOUTH_W] = 30, [LOOK_MOUTH_H] = 14, [LOOK_MOUTH_BEND] = -4, [LOOK_MOUTH_FLAT] = 0.4f, [LOOK_MOUTH_OPEN] = 0.9f,
        [LOOK_TONGUE] = 0.6f, [LOOK_CHEEK] = 0.4f}},
    {"dialing", {REST, TINT(0xE4F2F8), [LOOK_EYE_H] = 48, [LOOK_GAZE_Y] = -3, [LOOK_BROW] = 0.9f, [LOOK_BROW_Y] = 16, [LOOK_BROW_BEND] = 5,
        [LOOK_MOUTH_W] = 24, [LOOK_MOUTH_BEND] = -3}},
    {"offline", {REST, TINT(0xC8D4DC), [LOOK_UPPER_L] = 0.18f, [LOOK_UPPER_R] = 0.10f, [LOOK_UPPER_TILT] = -5, [LOOK_IRIS] = 0.90f,
        [LOOK_BROW] = 0.8f, [LOOK_BROW_Y] = 20, [LOOK_BROW_TILT] = -5, [LOOK_BROW_RAISE_L] = 2, [LOOK_MOUTH_W] = 28, [LOOK_MOUTH_H] = 6, [LOOK_MOUTH_WAVE] = 2.4f,
        [LOOK_MOUTH_BEND] = 2}},
    {"error", {REST, TINT(0xE0E4F0), [LOOK_UPPER_L] = 0.14f, [LOOK_UPPER_R] = 0.14f, [LOOK_UPPER_TILT] = -9, [LOOK_IRIS] = 0.86f,
        [LOOK_BROW] = 1, [LOOK_BROW_Y] = 19, [LOOK_BROW_TILT] = -8, [LOOK_MOUTH_W] = 28, [LOOK_MOUTH_BEND] = 6, [LOOK_SWEAT] = 1}},
};
#pragma GCC diagnostic pop

_Static_assert(sizeof(lcd_face_looks) / sizeof(lcd_face_looks[0]) == LCD_FACE_MANNERS, "a look for every manner");

float lcd_face_look_pace(look_channel_t c)
{
    if (c <= LOOK_LOWER_ARCH) return 3.6f;          /* eyes and lids lead */
    if (c <= LOOK_GAZE_Y) return 4.2f;
    if (c <= LOOK_BROW_RAISE_R) return 3.9f;        /* brows go with them */
    if (c <= LOOK_TEETH) return 2.7f;               /* the mouth follows */
    if (c <= LOOK_LEAN) return 2.0f;
    if (c <= LOOK_BLUE) return 1.2f;                /* a colour never jumps */
    return 2.0f;
}
