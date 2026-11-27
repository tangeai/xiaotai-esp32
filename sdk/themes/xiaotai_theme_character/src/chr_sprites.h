// SPDX-License-Identifier: MIT
#pragma once
/* Slots: the pictures the character asks a sprite pack for. The numbers are
 * the pack format's (tools/character/bake.py writes the same ones) and only
 * ever grow at the end. A pack fills the slots of the parts its character
 * has; an empty slot is a part she does not have (no tail, no ears, no arms
 * of her own, a head that does not turn), and the code that moves that part
 * does nothing. */
enum {
    SL_TAIL = 0,
    SL_EAR_L = 1,             /* 3 pictures: folded inwards, upright, flopped outwards */
    SL_EAR_R = 4,
    SL_BODY = 7,
    SL_ARM_L = 8,             /* CHR_ARM_STATES pictures: hanging .. raised */
    SL_ARM_R = 12,
    SL_HEAD = 16,             /* CHR_YAWS blocks of H_SLOTS: facing the viewer, turned left, turned right */
};
enum { CHR_ARM_STATES = 4, CHR_YAWS = 3 };
/* Inside one block of the head. */
enum {
    H_HAIR_BACK = 0, H_FACE, H_HAIR_FRONT, H_BROW_L, H_BROW_R,
    H_MOUTH = 5,              /* 15: openness 0..4 times form (pout, plain, smile) */
    H_EYE_L = 20,             /* 9: open and half open at three gazes, then nearly shut, smiling shut, shut */
    H_EYE_R = 29,
    H_SLOTS = 38
};
/* After the head blocks: pictures of a flat figure (one base picture in SL_BODY, patches over its
 * face in the first head block, and these). */
enum {
    SL_EYE_L_WIDE = SL_HEAD + CHR_YAWS * H_SLOTS, SL_EYE_R_WIDE,
    SL_EYE_L_UP, SL_EYE_R_UP, /* looking up: no longer drawn (the whites under the eyes show), nor baked */
    SL_BROWS,                 /* 3: raised, cross, sad (both brows in one picture) */
    SL_FACE = SL_BROWS + 3,   /* 3 whole-face pictures: puffed cheeks, crying, (free) */
    SL_POSE = SL_FACE + 3,    /* CHR_POSES whole-figure pictures, in the order of chr_gesture_t after NONE */
};
enum { CHR_POSES = 24 };
/* Added later: open eyes with another look, and the brows of someone who comforts or is serious. */
enum {
    SL_EYE_L_SOFT = SL_POSE + CHR_POSES, SL_EYE_R_SOFT, SL_EYE_L_BRIGHT, SL_EYE_R_BRIGHT,
    SL_BROWS_COMFORT, SL_BROWS_SERIOUS,
    /* The eyes and the closed mouth that go with those brows. */
    SL_EYE_L_COMFORT, SL_EYE_R_COMFORT, SL_EYE_L_SERIOUS, SL_EYE_R_SERIOUS, SL_MOUTH_COMFORT, SL_MOUTH_SERIOUS,
    /* Eyes lowered to one side, the lids a little down: thinking, or waiting. */
    SL_EYE_L_PONDER, SL_EYE_R_PONDER,
    /* Eyes that smile half open (a glad face, and the way to one that smiles with shut eyes), eyes cast down (shy,
     * praised, a little cross) and eyes with a line of tears on the lower lid (moved, hurt); a smile that is higher
     * on one side, and a mouth whose corners are a little down. */
    SL_EYE_L_GLAD, SL_EYE_R_GLAD, SL_EYE_L_DOWN, SL_EYE_R_DOWN, SL_EYE_L_TEARY, SL_EYE_R_TEARY,
    SL_MOUTH_SMIRK, SL_MOUTH_FROWN,
    /* Pictures of effects: CHR_FX_FRAMES slots for each kind of chr_fx_t, the frames of an animation or variants of
     * one thing. A kind without a picture is drawn as shapes, or not at all. */
    SL_FX,
};
enum { CHR_FX_KINDS = 23, CHR_FX_FRAMES = 12, SL_COUNT = SL_FX + CHR_FX_KINDS * CHR_FX_FRAMES };
_Static_assert(SL_EYE_L_WIDE == 130 && SL_POSE == 140 && SL_EYE_L_SOFT == 164 && SL_EYE_L_PONDER == 176 && SL_EYE_L_GLAD == 178 &&
               SL_FX == 186 && SL_COUNT == 462, "slot numbers are the pack format's");
