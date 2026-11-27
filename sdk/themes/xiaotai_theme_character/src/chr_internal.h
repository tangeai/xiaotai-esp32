// SPDX-License-Identifier: MIT
#pragma once
#include "chr.h"

/* The figure in its own coordinates: pixels from the anchor (bottom centre),
 * x to the right, y down, so everything visible has a negative y. The values
 * come from the sprite pack, in this order (tools/character/bake.py measures
 * them on the baked sprites); marks and touch regions are laid out from them.
 * "Left" is the figure's left: the viewer's right. */
typedef struct {
    int16_t ears_top, hair_top, brow_y, eye_y, cheek_y, mouth_y, neck_y, ear_base_y;
    int16_t eye_lx, eye_rx, face_x, head_half;       /* head_half: half the head's width */
    int16_t tail_x0, tail_x1, tail_top;
    int16_t brow_lx, brow_rx;                        /* the middle of each brow */
    int16_t cheek_dx, blush_rx, blush_ry;            /* a cheek lies cheek_dx outside its eye */
    int16_t ear_lx0, ear_lx1, ear_rx0, ear_rx1;
    int16_t shoulder_half;                           /* half the width of the torso */
    int16_t arm_lx, arm_rx, arm_top;                 /* the hanging arms */
    int16_t hand_lx, hand_ly, hand_rx, hand_ry, hand_half;   /* the hands of the raised arms */
    int16_t yaw_shift;                               /* how far the face moves sideways in a turned head */
    int16_t shoulder_y;                              /* below this row a flat figure's head does not move it */
} chr_figure_t;
enum { CHR_FIGURE_VALUES = sizeof(chr_figure_t) / sizeof(int16_t) };

/* Effects that float around the character, in the order of the pack's pictures of them (chr_sprites.h). The first
 * eight are drawn as shapes when the pack has no picture of them; the others only as pictures. */
typedef enum {
    FX_HEART = 0, FX_ZED, FX_NOTE, FX_SPARK, FX_DROP, FX_QUESTION, FX_EXCLAIM, FX_PUFF,
    FX_HEART_SMALL, FX_SPARK_SKY, FX_GLINT, FX_RING_GOLD, FX_RING_SKY, FX_BURST, FX_PETAL, FX_ROSE, FX_RIBBON, FX_NOTE2,
    FX_ORB_GOLD, FX_ORB_PINK, FX_ORB_SKY, FX_ORB_LILAC, FX_AURA,
    FX_KINDS
} chr_fx_t;

typedef struct {
    uint8_t kind, live;
    uint8_t frame;            /* the variant of a picture that is not animated */
    bool animated;            /* a picture that steps through its frames over its life */
    float x, y, vx, vy;       /* figure coordinates; pixels a second */
    float fall;               /* pixels a second, gained every second */
    float age, life;          /* ms; a particle younger than 0 has not come yet */
    float size;               /* a shape: pixels; a picture: how much larger than it was baked */
    float sway;               /* pixels to either side, slowly */
    float phase, pop;         /* where its sway starts; a picture grows from (1 - pop) of its size in its first moment */
    float drag;               /* how much of its sideways speed it loses in a second */
    float fade_in, fade_out;  /* ms */
} chr_particle_t;

/* The parts of a flat figure's face that change by themselves. */
enum { PART_EYE_L = 0, PART_EYE_R, PART_BROWS, PART_MOUTH, PART_COUNT };
/* The lids of a flat figure's eye, from shut to smiling: they go from one rung to the next, never past one. */
enum { RUNG_SHUT = 0, RUNG_HALF, RUNG_OPEN, RUNG_GLAD, RUNG_SMILE };

/* Items are grouped so that a part which did not change is not redrawn. The parts that move most
 * often (the tail, each ear, the eyes and mouth) are groups of their own: a blink redraws a small
 * rectangle, not the head. */
enum { G_TAIL = 0, G_ARM_L, G_ARM_R, G_BODY, G_EAR_L, G_EAR_R, G_HEAD, G_FACE, G_FX,
       G_FX_OTHER /* marks on the other side of her head: not one rectangle with those, across her face */,
       /* Particles, by where they are: above or below her chin, on either side of her face; a glow behind her. A
        * few flying about on one side do not redraw a rectangle across all of her. */
       G_FX_UL, G_FX_UR, G_FX_DL, G_FX_DR, G_AURA, G_COUNT };

typedef struct {
    int16_t x0, y0, x1, y1;   /* inclusive; x1 < x0: empty */
    uint32_t signature;
} chr_group_t;

/* One thing to draw: a sprite or a shape. */
typedef struct {
    uint8_t sprite, group;
    union {
        chr_shape_t shape;
        chr_blit_t blit;
    } as;
} chr_item_t;

struct chr {
    /* Stage. */
    float width, height, anchor_x, anchor_y;
    bool raised;
    chr_pack_t pack;
    chr_figure_t fig;
    uint32_t features;              /* CHR_HAS_* */
    /* Inputs. */
    uint8_t mood, phase, level;
    uint32_t conditions;
    float env_mic, env_speaker;     /* 0..1; amplitude 0..32768 */
    bool sounded;                   /* the speaker's loudness is known */
    float mic_level, env_ahead;     /* the microphone's amplitude; the speaker's a moment ahead (CHR_AHEAD_MS) */
    bool ahead;                     /* what is about to be heard is known */
    /* Pose. */
    float p[P_COUNT], v[P_COUNT], target[P_COUNT], out[P_COUNT];
    float breath, drowsy;           /* where she is in a breath (radians), and how much like a sleeper's it is (0..1) */
    /* Followers of sound. */
    float mouth, listen;
    /* Her voice: the loudest of the last seconds, which the gate and the openness go by; since when
     * nothing has been heard (or, with nothing to hear, since she last spoke). The mouth goes in steps
     * (0 shut .. 3 wide), each for a moment at least. When a sentence began, and when it was cut off
     * (< 0: it was not); since when the microphone has heard nobody, and for how long it has been on. */
    float voice_peak, silent_ms, voice_step_ms, shut_ms, sentence_ms, cut_ms, unheard_ms, hearing_ms;
    uint8_t voice_step;
    bool word_shut;                 /* her mouth shut after words: it goes shut at once, not a step at a time */
    bool leant;                     /* listening for long without a word, she leans in */
    bool parting, parted;           /* the talk is over, but not what she has to say (parting_step()); it was the last */
    float parting_ms;
    /* Clocks, milliseconds. */
    double now;
    float blink_in, blink_age;      /* blink_age < 0: not blinking */
    bool blink_twice, blink_turned;             /* a blink has come to its shut lids in this step */
    float look_in, look_x, look_y;
    float idle_in;
    float emit_in[8];
    float phase_age, mood_age;
    float wag_phase, tail_sway, tail_in, step_ms;   /* the tail is moved a few times a second, not every frame */
    /* The running reaction. */
    uint8_t cue;
    float cue_age, cue_len;
    int32_t cue_value;
    float cue_dir;
    /* Touch. */
    bool down, taken;
    uint8_t touch, region;
    float touch_x, touch_y, start_x, start_y, travel, held_ms, release_ms, watch_x, watch_y;
    uint8_t pokes;
    float poke_ms, pet, squish, sulk_ms;
    /* Hair lags behind the head on a spring. */
    float swing, swing_v, head_x;
    bool head_known;
    /* The pictures in use, kept between frames so that they do not flicker at a threshold. */
    uint8_t gaze, eye[2], mouth_form, mouth_open, ear[2], arm[2], turn;
    /* The gesture she makes, and the pose picture a flat figure shows for it. A change of picture
     * takes a moment: her eyes close, she dips a little, the picture changes at the bottom of the dip,
     * and she comes back up to her height. */
    uint8_t gesture, pose, pose_next;
    float gesture_ms, pose_ms, change_ms;       /* change_ms < 0: no change under way */
    bool changed;                               /* the picture has changed in this change */
    uint8_t change_kind, change_style;          /* to a pose, out of one, or to the clip's next picture; CHR_STYLE_* */
    float from_x, from_y, from_lift, from_lids; /* how the change found her: it goes on from there */
    uint8_t wait_for, shunned;                  /* the pose that is waited for; one whose pictures did not come */
    float wait_ms, wait_max_ms, lag_max_ms;     /* for pictures that are being read */
    /* A pose that the pack plays with several pictures (a clip): which of them shows and for how
     * long it has, and whether she is on her way out of the pose. `picture` is the sprite shown
     * for the pose (CHR_NO_SPRITE: the base picture), `picture_under` the one it is a patch over
     * (CHR_NO_SPRITE: none), `frame_next` the picture of the clip that a change under way leads to. */
    chr_clip_t clip;
    bool clipped, leaving, entered;             /* entered: the clip's entry has played */
    uint16_t clip_at, picture, picture_under, frame_next;
    float frame_ms;
    bool crying;
    /* What lasts: how awake she is, how close she feels, and for how long she has been left alone.
     * The mood she was given, and since when; whether it was a talk's that has ended. The mood she was
     * given last while there was no talk, the one she wants to show, and how long ago she stopped
     * wanting the one of a talk that had ended. */
    float energy, closeness, alone_ms, lonely_in;
    uint8_t given, home, wanted, waves;
    float given_ms, after_ms;
    bool held, talked, dozing;
    bool spoken;                                /* she has spoken in the mood she was given, or was still heard when it came */
    /* A change of mood: how long it has been wanted, and the blink that covers it (< 0: none). */
    float mood_wait, swap_ms;
    bool mood_talk;                             /* the mood she shows came while a talk ran: her face only */
    bool snap;                                  /* behind shut lids: eyes and brows are the new ones at once */
    bool ponder;                                /* eyes lowered to one side: thinking, or waiting (compose()) */
    uint8_t eyes_look, brows_look;              /* other pictures of open eyes and of brows; LOOK_* */
    uint8_t eyes_full, mouth_look;              /* the eyes of the mood once it shows fully; the mouth it rests in */
    bool face_cut;                              /* the next face is shown at once, without fading */
    uint8_t eyes_shown;                         /* the eyes follow when the lids are shut */
    float smile_in;                             /* until a happy face next closes its eyes in a smile */
    int16_t moved_x, moved_feet, moved_top;     /* a flat figure as drawn, from where she is at rest */
    /* A flat figure's face changes one part at a time (chr_draw.c): the picture each part shows (eyes, brows, mouth;
     * CHR_NO_SPRITE: the base picture's own), the one it showed before and how long ago it changed (it fades from
     * one to the other); the rung each eye's lids are on (RUNG_*) and how long it has been there. */
    uint16_t part_now[PART_COUNT], part_was[PART_COUNT];
    float part_ms[PART_COUNT];
    uint8_t rung[2], aim[2];                    /* aim: the rung the lids are on their way to */
    float rung_ms[2];
    float mouth_lag;                            /* a change of mood reaches the mouth this much later */
    uint8_t mouth_drawn;                        /* how open the mouth was drawn (a word opens and shuts it at once) */
    uint8_t mouth_rest;                         /* the mouth she rests in as drawn: the mood's, once it has reached it */
    float mouth_ms;                             /* since the mouth last changed how open it is, outside words */
    float blush, blush_from;                    /* the blush as shown, and what it fades out from */
    /* How strongly the mood shows: lightly when it comes, fully once it has stayed (MOOD_*), and since when. On its
     * way out a full mood is shown lightly first. */
    uint8_t strength;
    float strength_ms;
    bool slow_blink;                            /* the blink under way is a slow one */
    bool easing;                                /* a full mood shows lightly on its way out */
    uint8_t round_poses;                        /* pose pictures she went into in this round of a talk */
    /* An answer, as heard: when its first sound is (< 0: not yet; on the clock `now`), whether the next sound to
     * begin will be it (`answer_armed`; where it began is not known then), whether it was only taken as begun for
     * want of a sound (`answer_guessed`); its sentences told and not yet shown (CHR_SENTENCES at most), the face the
     * last shown one brought (the talk's, while `telling`), the mood of that sentence's own words, the face the AI
     * asked for that stands over them (CHR_MOOD_COUNT: none), and whether the user has begun to speak since. How
     * long the speaker ahead has been quiet as far as is seen (`ahead_silent_ms`) and as is known, every frame of it
     * (`ahead_sure_ms`), and how long the microphone has heard the user. */
    double answer_at, voice_at, told_at;    /* voice_at: when she was last heard; told_at: her last sentence shown */
    double back_at, away_at;                /* her clock as she was last back on the screen, and when she had gone */
    bool answer_armed, answer_foreseen, answer_guessed, telling, user_turn;
    uint8_t told, sentence_mood, asked, sentence_count;
    struct { double came, onset; int32_t begin; uint8_t mood; } sentence[CHR_SENTENCES];
    float ahead_silent_ms, ahead_sure_ms, user_ms;
    /* Effects. How many frames the pack has of each picture; the dance's picture the effects saw last. */
    uint8_t fx_frames[FX_KINDS];
    uint16_t beat_seen;
    chr_particle_t particles[CHR_PARTICLES_MAX];
    /* What the last step built. */
    chr_item_t items[CHR_ITEMS_MAX];
    uint16_t item_count;
    chr_group_t groups[G_COUNT], drawn[G_COUNT];
    bool built;
    uint32_t random, steps, quiet_steps;
};

uint32_t chr_random(chr_t *c);
float chr_uniform(chr_t *c, float from, float to);
void chr_emit(chr_t *c, chr_fx_t kind, float x, float y, float vx, float vy, float size, float life);
/* A particle that the caller dresses further; it comes after `delay` ms and lasts `life` ms. Size: of a picture, how
 * much larger than it was baked. When all are taken it takes the place of the one nearest its end. */
chr_particle_t *chr_spark(chr_t *c, chr_fx_t kind, float x, float y, float delay, float life, float size);
/* Whether the pack has a picture of an effect. */
static inline bool chr_fx_drawn(const chr_t *c, chr_fx_t kind) { return c->fx_frames[kind] != 0; }
void chr_effects_step(chr_t *c, float dt);
/* chr_draw.c: turns c->out and the particles into items and groups. */
void chr_build(chr_t *c);
/* Where the head is drawn relative to its place in the figure. */
void chr_head_offset(const chr_t *c, float *x, float *y);
enum { LOOK_PLAIN = 0, LOOK_SOFT, LOOK_BRIGHT, LOOK_COMFORT, LOOK_SERIOUS, LOOK_GLAD, LOOK_DOWN, LOOK_TEARY,
       /* Of a mouth that rests (LOOK_COMFORT and LOOK_SERIOUS too); LOOK_SMILE: the base picture's own smile. */
       LOOK_SMIRK, LOOK_FROWN, LOOK_SMILE };
/* How far her lids are closed over what her pose says: by a blink, or to cover a change (0..1). */
float chr_lids(const chr_t *c);
/* How a flat figure gets from one picture to the next. Never by fading one into the other (two
 * faces would show): she dips by a hundredth of her height or less, the picture changes at the
 * bottom of the dip, and she comes back up to her height, not past it. Popped: into and out of most
 * gestures. Soft: slower and smaller, for a sulk, falling asleep. Beat: every step of a dance, which
 * alone springs back past her height and leaves the ground a little. */
enum { CHR_STYLE_POP = 0, CHR_STYLE_SOFT, CHR_STYLE_BEAT, CHR_STYLE_COUNT };
/* How she is drawn while a picture changes: how much wider and taller, and how far off the ground. */
void chr_change_shape(const chr_t *c, float *grow_x, float *grow_y, float *lift);
/* A flat figure moves as one large piece, so it moves far less than a figure's head alone would:
 * how many pixels its top sinks for one that a separate head would sink, how far it goes sideways
 * for one that a head would lean, and what pushes it around is this part of what pushes a layered
 * figure. Whatever adds up, it stays within a few pixels of its place. */
#define CHR_FLAT_GIVE 0.6f
#define CHR_FLAT_LEAN 0.12f
#define CHR_FLAT_PUSH 0.25f
#define CHR_FLAT_SIDE 2.0f            /* pixels sideways, at most ... */
#define CHR_FLAT_SIDE_DANCE 6.0f      /* ... and in a dance */
#define CHR_FLAT_UP 6.0f              /* pixels up or down, at most */
#define CHR_FLAT_TALL 8.0f            /* pixels shorter or taller, at most (breathing aside) */
/* Where a flat figure is, in pixels from its place: to the right, and down. */
float chr_flat_x(const chr_t *c);
float chr_flat_y(const chr_t *c);
/* How many pixels taller a flat figure is than at rest (without a change of picture). */
float chr_flat_tall(const chr_t *c);
/* Beside the head, on the figure's right (the viewer's left): where marks that belong to the head are drawn, away from
 * the controls a page keeps on its right. CHR_SIDE: which way is away from her head there. */
#define CHR_SIDE (-1.0f)
static inline float chr_side_x(const chr_t *c) { return (float)c->fig.face_x + CHR_SIDE * (float)(c->fig.head_half + 14); }
/* Nothing of hers is drawn above the top of her hair (a particle fades out on its way there): above it is the page's
 * title bar. */
static inline float chr_mark_top(const chr_t *c) { return (float)c->fig.hair_top + 16.0f; }
