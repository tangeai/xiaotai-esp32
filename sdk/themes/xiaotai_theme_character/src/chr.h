// SPDX-License-Identifier: MIT
#pragma once
/* The character itself, without any drawing library or operating system: a
 * pose (a vector of numbers), what moves the pose (mood, conversation phase,
 * device conditions, one-shot reactions, touch, idle life), and the pictures
 * and shapes the pose turns into. One instance belongs to one task: every
 * function here is called on the display's UI task. The adapter
 * (chr_actor.c) carries events from other tasks across.
 *
 * The figure comes from a sprite pack (tools/character/bake.py), in one of
 * two makes. A flat figure is one picture of the whole character, patches for
 * her eyes and mouth, and other whole pictures for her poses; her head moves
 * by sliding and lifting the rows above her shoulders. A layered figure is a
 * stack of sprites cut from a puppet: hair, face, one picture per eye state
 * and mouth shape, body, and whatever else she has: arms, ears, a tail, a
 * head that turns. Either way the pose chooses the pictures and moves them;
 * marks such as hearts or the sound waves beside the ear are drawn as shapes.
 * Who she is comes from the pack alone: it carries the pictures, which parts
 * exist and where they are, so another pack is another character.
 *
 * Units: positions are pixels of the stage (the widget the character lives
 * in), time is milliseconds. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chr_raster.h"

/* Bench marks, for measuring when she does what against what is heard: one short line per event, "@<ms> <TAG> ...", on
 * the device's millisecond clock (chr_mark_ms, which the actor sets before each step). A bench build sets CHR_MARKS to
 * 1; otherwise they are nothing at all. */
#ifndef CHR_MARKS
#define CHR_MARKS 0
#endif
#if CHR_MARKS
#include <stdio.h>
extern uint32_t chr_mark_ms;
#define CHR_MARK(fmt, ...) printf("@%u " fmt "\n", (unsigned)chr_mark_ms, ##__VA_ARGS__)
#else
#define CHR_MARK(fmt, ...) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Pose channels. Springs move each one towards its target. */
enum {
    P_BX = 0, P_BY,           /* the whole figure, pixels from its place */
    P_HY,                     /* the head sinks (+) or lifts (-), pixels */
    P_LEAN,                   /* the head leans sideways by this many pixels at its top */
    P_GX, P_GY,               /* gaze, -1..1 */
    P_EOL, P_EOR,             /* eye openness left and right: 1 open, 0 shut (left is the viewer's right) */
    P_ARC,                    /* above 0.45 the eyes close in a smile, below -0.45 they rest shut */
    P_SLANT,                  /* brows: -1 sad .. +1 angry */
    P_BROW,                   /* brows raised, pixels */
    P_MC, P_MO,               /* mouth: -1 pout .. 1 smile; 0 shut .. 1 wide open */
    P_BLUSH,
    P_EAR_L, P_EAR_R,         /* ears: 0 upright, +1 flopped outwards, -1 folded inwards */
    P_WAG,                    /* tail: 0 still .. 1.5 wagging hard */
    P_GREY,                   /* 0..1 washed out */
    P_ARM_L, P_ARM_R,         /* arms: 0 hanging .. 1 raised (left is the viewer's right) */
    P_YAW,                    /* the head turns: -1 to the viewer's left .. 1 to the right */
    P_COUNT
};

/* The moods are the emotion names every colour theme accepts
 * (xiaotai/theme_lcd.h), in the order of chr_mood_name(), and after them the
 * moods of a conversation that other themes draw as one of those. */
enum { CHR_MOOD_NEUTRAL = 0, CHR_MOOD_NAMES = 23, CHR_MOOD_COUNT = 28 };

typedef enum {
    CHR_PHASE_IDLE = 0,
    CHR_PHASE_LISTENING,
    CHR_PHASE_THINKING,
    CHR_PHASE_SPEAKING,
} chr_phase_t;

/* Conditions hold until they are cleared. */
enum {
    CHR_OFFLINE = 1u << 0,     /* no network */
    CHR_CONNECTING = 1u << 1,  /* network or service on its way up */
    CHR_RINGING = 1u << 2,     /* a call wants an answer */
    CHR_IN_CALL = 1u << 3,
    CHR_ALARM = 1u << 4,       /* an alarm rings */
    CHR_MUTED = 1u << 5,       /* the microphone is off */
    CHR_PARTING = 1u << 6,     /* a talk is over, but its last words are still on their way */
    CHR_STARTING = 1u << 7,    /* a talk is being connected */
};

/* What a sprite pack brings: optional parts, and whether the figure is one flat picture
 * (CHR_FLAT) with whole-figure pictures for some gestures (CHR_HAS_POSES). */
enum { CHR_HAS_EARS = 1u << 0, CHR_HAS_TAIL = 1u << 1, CHR_HAS_ARMS = 1u << 2, CHR_HAS_YAW = 1u << 3,
       CHR_FLAT = 1u << 4, CHR_HAS_POSES = 1u << 5 };

/* What her arms and her whole body say. The figure shows a gesture with what it has: a flat figure
 * with the pose picture of that gesture, a layered one with its arms and the turn of its head; a
 * figure with neither keeps to its face. */
typedef enum {
    CHR_GESTURE_NONE = 0,
    CHR_GESTURE_WAVE, CHR_GESTURE_CHEER, CHR_GESTURE_THINK, CHR_GESTURE_SHY, CHR_GESTURE_SULK,
    CHR_GESTURE_STRETCH, CHR_GESTURE_SLEEP, CHR_GESTURE_HIGHFIVE, CHR_GESTURE_SHRUG,
    CHR_GESTURE_DANCE,
    /* Pictures a flat figure may bring; a layered one shows these with its face alone. */
    CHR_GESTURE_LAUGH, CHR_GESTURE_NOD, CHR_GESTURE_TILT, CHR_GESTURE_LISTEN, CHR_GESTURE_SURPRISE,
    CHR_GESTURE_HEART, CHR_GESTURE_PET,
    CHR_GESTURE_COUNT
} chr_gesture_t;

/* Reactions play once. The first group comes from the device, the second
 * from touch, the third from idle life; all can be requested by name. Some
 * are not played in a talk (chr_cue()). */
typedef enum {
    CHR_CUE_NONE = 0,
    CHR_CUE_GREET, CHR_CUE_WAKE, CHR_CUE_STARTLE, CHR_CUE_BYE, CHR_CUE_VOLUME, CHR_CUE_ERROR,
    CHR_CUE_NOD, CHR_CUE_STRETCH, CHR_CUE_CHEER, CHR_CUE_SHIVER,
    CHR_CUE_POKE, CHR_CUE_DIZZY, CHR_CUE_POUT, CHR_CUE_FLICK, CHR_CUE_TAIL, CHR_CUE_SWAY,
    CHR_CUE_JUMP, CHR_CUE_DUCK, CHR_CUE_LOOK,
    CHR_CUE_HOP /* a flat figure smiles instead */, CHR_CUE_HUM, CHR_CUE_YAWN, CHR_CUE_EARS, CHR_CUE_WAG, CHR_CUE_TWITCH,
    /* Added later, so that the numbers before them stay: two of touch, three of idle life. */
    CHR_CUE_OFFER,       /* a hand held out to the finger that touched her arm */
    CHR_CUE_HIGHFIVE,    /* that hand met */
    CHR_CUE_WAVE,        /* left alone for long: she looks for you and waves */
    CHR_CUE_GLANCE,      /* a look round to one side */
    CHR_CUE_NOD_OFF,     /* drowsy: the head sinks, then jerks up */
    CHR_CUE_DANCE,       /* a few bars of a dance, when she is in high spirits or is asked */
    CHR_CUE_COUNT
} chr_cue_t;

/* What touch is doing to the character right now. */
typedef enum {
    CHR_TOUCH_NONE = 0,
    CHR_TOUCH_WATCH,     /* a finger is down somewhere: the eyes follow it */
    CHR_TOUCH_PET,       /* stroking the head */
    CHR_TOUCH_SQUISH,    /* pressing and holding the face or body */
    CHR_TOUCH_PULL,      /* holding an ear */
    CHR_TOUCH_HOLD,      /* holding her hand: a finger resting on her arm */
} chr_touch_t;

typedef struct { int16_t x, y, width, height; } chr_rect_t;
enum { CHR_DAMAGE_MAX = 10, CHR_ITEMS_MAX = 160, CHR_PARTICLES_MAX = 48 };

typedef struct chr chr_t;

/* Bytes of one instance (the caller allocates it) and of the scratch row the
 * rasterizer needs for a stage of this width. */
size_t chr_size(void);
size_t chr_scratch_entries(uint16_t stage_width);
/* `pack` is the sprite pack; it is used in place and must outlive the
 * instance. False when it is not a pack this code draws. */
bool chr_init(chr_t *c, uint16_t stage_width, uint16_t stage_height, uint32_t seed, const void *pack, size_t pack_size);
/* The same for a pack that has been opened, whose pictures may come from a shelf (chr_shelf.h). */
struct chr_pack;
bool chr_init_from(chr_t *c, uint16_t stage_width, uint16_t stage_height, uint32_t seed, const struct chr_pack *pack);
/* Where the character stands: raised for a standalone preview of an emotion. */
void chr_place(chr_t *c, bool raised);

const char *chr_mood_name(size_t index);          /* NULL past the end */
size_t chr_mood_find(const char *name);           /* unknown names and NULL: neutral */
const char *chr_cue_name(chr_cue_t cue);          /* NULL for NONE and past the end */
chr_cue_t chr_cue_find(const char *name);

/* The mood the conversation gives her. While a talk runs she changes into it between two sentences;
 * a blink covers the change. It is hers for a while: two seconds after she has finished speaking, and
 * never long when it is a low one, she returns to her own mood of the moment (see chr_mind.c). */
void chr_set_mood(chr_t *c, size_t mood);
/* While held, the given mood stays for as long as it is given (the bench looks at one). */
void chr_hold(chr_t *c, bool held);
void chr_set_phase(chr_t *c, chr_phase_t phase);
/* She is on the screen again after a while; the page's mood, phase and conditions have been set. What was under
 * way when she went, or would come of those changes, is over (a reaction, a pose, a touch, a talk with its
 * goodbye): she is as she should be at once. Only on a page that is about a mood does she `show` it. */
void chr_return(chr_t *c, bool show);
/* The screen has come on after a sleep: someone is there, and she is awake. She stretches, if she is to and no
 * talk runs. */
void chr_woken(chr_t *c, bool stretch);
void chr_set_level(chr_t *c, uint8_t level);                      /* coarse voice activity 0..3 */
/* What the microphone hears and what is heard from the speaker, amplitudes 0..32768: her mouth follows
 * the speaker. Without any call she has nothing to follow, and moves it on her own while she speaks. */
void chr_set_envelope(chr_t *c, uint16_t microphone, uint16_t speaker);
/* There is nothing to tell the loudness by (any more): the mouth moves by itself while she speaks. */
void chr_no_envelope(chr_t *c);
/* The speaker's loudness as it will be heard CHR_AHEAD_MS from now, with each envelope: she begins to change a moment
 * before a sound is heard, and shuts her mouth as a pause comes. `known` false: nothing is written that far ahead (the
 * driver holds less, or nothing is played), which is no pause; she goes by what is heard. */
enum { CHR_AHEAD_MS = 100 };
void chr_set_ahead(chr_t *c, uint16_t speaker, bool known);
/* A sentence of her answer: the mood its words bring, and where it begins in the answer's voice (ms after its first
 * sound; -1: not said). In a talk her face and the little effect of a sentence go with what is heard: a moment
 * (CHR_LEAD_MS) before the sentence is. CHR_SENTENCE_NOW: a face the AI asked for, at once; it stands over her
 * sentences until CHR_SENTENCE_OWN, after which each shows its own again (the face of the one being heard at once, or,
 * when none of the answer is yet, `mood`). */
enum { CHR_SENTENCE_OWN = -3, CHR_SENTENCE_NOW = -2, CHR_LEAD_MS = 100, CHR_SENTENCES = 6 };
void chr_sentence(chr_t *c, int32_t begin_ms, size_t mood);
/* Time she was not stepped for (behind a dialog, on another page, the screen asleep): her clock goes on, as what is
 * heard did. */
void chr_pass(chr_t *c, uint32_t ms);
void chr_set_conditions(chr_t *c, uint32_t conditions);
void chr_cue(chr_t *c, chr_cue_t cue, int32_t value);
/* The pointer as read this frame. Returns true once the character has taken
 * the touch for itself (stroking, holding, an ear): the control under the
 * finger must not fire. */
bool chr_pointer(chr_t *c, bool down, int16_t x, int16_t y);

/* Advances by dt_ms and rebuilds what is drawn. Returns how many rectangles
 * of the stage changed since the previous step (0: nothing to redraw). */
size_t chr_step(chr_t *c, uint32_t dt_ms, chr_rect_t damage[CHR_DAMAGE_MAX]);
/* Draws what the last step built into the part of the stage `target` covers. */
uint32_t chr_paint(const chr_t *c, const chr_target_t *target);

/* The area a plain tap on the character covers: the middle of her, from the brows down. */
chr_rect_t chr_body_box(const chr_t *c);
/* The optional parts this character has (CHR_HAS_*). */
uint32_t chr_features(const chr_t *c);

/* For tests and diagnostics. */
typedef struct {
    float pose[P_COUNT];        /* as drawn by the last step */
    float target[P_COUNT];
    uint8_t mood, phase, cue, touch;
    uint8_t eye_left, eye_right, gaze, mouth_form, mouth_open, ear_left, ear_right;   /* the pictures chosen */
    uint8_t arm_left, arm_right;    /* 0 hanging .. 3 raised */
    uint8_t turn;                   /* 0 to the viewer's left, 1 facing, 2 to the right */
    uint8_t features;               /* CHR_HAS_*, CHR_FLAT */
    uint8_t gesture;                /* chr_gesture_t */
    uint8_t given;                  /* the mood last given; `mood` is the one she shows */
    bool dozing;
    float energy, closeness;        /* 0..1, the two slow variables */
    uint8_t shown;                  /* the pose picture shown: 0 none (the base figure), else the gesture's */
    uint8_t frame;                  /* which picture of that pose's clip, counted from its first */
    bool changing;                  /* between two pictures: dipping, or springing back up */
    uint32_t conditions;
    uint16_t items, particles;
    float mouth, listen;        /* the followed speaker and microphone levels, 0..1 */
    float silent;               /* how long nothing has been heard from the speaker, ms */
    bool parting;               /* a talk is over, and she is still heard */
    uint16_t shut_ms;           /* how long after the last sound her mouth last shut */
    uint16_t wait_ms, lag_ms;   /* the longest she waited for a pose's first picture, and for a later one past its time */
    /* A flat figure as drawn by the last step, pixels from where she stands at rest: all of her to the
     * right, her feet down, the top of her head down. */
    int16_t moved_x, moved_feet, moved_top;
    uint32_t steps, quiet_steps;
} chr_info_t;
void chr_inspect(const chr_t *c, chr_info_t *out);

#ifdef __cplusplus
}
#endif
