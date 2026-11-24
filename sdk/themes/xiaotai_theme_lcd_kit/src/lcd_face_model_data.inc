/* XiaoTai procedural expression poses and motion tracks. See ../assets/NOTICE.md. */

enum {
    FACE_TEARS = 21, FACE_THOUGHT, FACE_BLUSH, FACE_HEART,
    FACE_TONGUE, FACE_MOUTH_TILT,
    FACE_RED, FACE_GREEN, FACE_BLUE,
    FACE_ATTENTION, FACE_SPEECH,
    FACE_GLASSES, FACE_MUSIC, FACE_ZZZ, FACE_QUESTION, FACE_EXCLAIM,
    FACE_EMPHASIS, FACE_SPARKLE, FACE_LISTEN, FACE_WINK, FACE_KISS,
    FACE_TONGUE_X, FACE_GAZE_X, FACE_GAZE_Y, FACE_LEAN, FACE_WINK_LEFT,
    FACE_STYLE_B, FACE_FOLD_LEFT, FACE_FOLD_RIGHT, FACE_MOUTH_WAVE,
    FACE_MOUTH_PUCKER, FACE_MOUTH_PINK, FACE_MOUTH_TWO_TONE, FACE_STEAM,
    FACE_POSE_VALUES
};
enum { FACE_VARIANTS = 2, FACE_MOTION_STEPS = 4 };
typedef struct {
    const char *key;
    int16_t value[FACE_POSE_VALUES]; /* Q4 preserves half-pixel centres. */
    uint16_t transition_ms;
} face_pose_t;
/* Each part: centre x/y, width/height, radius, shear, quadratic bend. */
#define FP(x,y,w,h,r,s,b) ((x)*16+(w)*8),((y)*16+(h)*8),(w)*16,(h)*16,(r)*16,(s)*16,(b)*16
#define EL(w,h,r,s,b) FP(50-(w)/2,50-(h)/2,w,h,r,s,b)
#define ER(w,h,r,s,b) FP(170-(w)/2,50-(h)/2,w,h,r,s,b)
#define MP(w,h,r,b) FP(110-(w)/2,86-(h)/2,w,h,r,0,b)
#define FX(tear,think,blush,heart,tongue,tilt,c) \
    (tear)*16,(think)*16,(blush)*16,(heart)*16,(tongue)*16,(tilt)*16, \
    (((c)>>16)&255)*16,(((c)>>8)&255)*16,((c)&255)*16
/* 23-face family. Keep broad silhouettes even in resting squints.
 * Accent channels are independent of eyelids; hearts/glasses never blink flat. */
static const face_pose_t face_poses[] = {
    {"neutral", {EL(64,42,21,0,0), ER(64,42,21,0,0), MP(36,10,5,-1), FX(0,0,0,0,0,0,0xECF5F1)}, 520},
    {"happy", {EL(62,26,13,0,3), ER(62,26,13,0,3), MP(44,13,6,-3), FX(0,0,0,0,0,0,0xD7F6E9)}, 520},
    {"laughing", {EL(62,24,12,0,3), ER(62,24,12,0,3), MP(44,20,9,-1), FX(0,0,0,0,0,0,0xD7F6E9)}, 480},
    {"funny", {EL(64,36,18,0,0), ER(64,36,18,0,0), MP(40,12,6,-3), FX(0,0,0,0,0,-2,0xF5ECD8), [FACE_GAZE_X]=2*16}, 600},
    {"sad", {EL(62,30,14,-3,0), ER(62,30,14,3,0), MP(36,11,5,4), FX(0,0,0,0,0,0,0xD9E4F5)}, 720},
    {"angry", {EL(64,28,11,4,0), ER(64,28,11,-4,0), MP(36,10,5,0), FX(0,0,0,0,0,0,0xF4DDE6)}, 600},
    {"crying", {EL(62,28,14,-3,0), ER(62,28,14,3,0), MP(28,12,6,3), FX(256,0,0,0,0,0,0xD9E7F5)}, 720},
    {"loving", {EL(64,34,17,0,0), ER(64,34,17,0,0), MP(40,12,6,-3), FX(0,0,0,256,0,0,0xF5DCE8)}, 640},
    {"embarrassed", {EL(62,30,15,0,0), ER(62,30,15,0,0), MP(32,11,5,-2), FX(0,0,190,0,0,0,0xE5E7F4), [FACE_GAZE_Y]=16}, 680},
    {"surprised", {EL(64,46,22,0,0), ER(64,46,22,0,0), MP(24,16,8,0), FX(0,0,0,0,0,0,0xF6EEDB), [FACE_EXCLAIM]=256*16}, 440},
    {"shocked", {EL(64,48,23,0,0), ER(64,48,23,0,0), MP(28,18,8,0), FX(0,0,0,0,0,0,0xDFEFF4), [FACE_EMPHASIS]=256*16}, 440},
    {"thinking", {EL(64,30,13,0,0), ER(64,30,13,0,0), MP(36,10,5,0), FX(0,256,0,0,0,0,0xDCEFE8), [FACE_GAZE_Y]=-16}, 640},
    {"winking", {EL(64,42,21,0,0), ER(64,42,21,0,0), MP(40,12,6,-2), FX(0,0,0,0,0,0,0xD7F6E9), [FACE_WINK]=256*16, [FACE_SPARKLE]=200*16}, 440},
    {"cool", {EL(64,30,12,0,0), ER(64,30,12,0,0), MP(40,12,6,-2), FX(0,0,0,0,0,0,0xD1ECF5), [FACE_GLASSES]=256*16}, 640},
    {"relaxed", {EL(62,28,13,0,0), ER(62,28,13,0,0), MP(40,12,6,-2), FX(0,0,0,0,0,0,0xF1E9D8), [FACE_MUSIC]=256*16}, 800},
    {"delicious", {EL(62,26,13,0,3), ER(62,26,13,0,3), MP(40,12,6,-2), FX(0,0,0,0,220,0,0xF5DCE8), [FACE_TONGUE_X]=12*16}, 640},
    {"kissy", {EL(62,28,14,0,0), ER(62,28,14,0,0), MP(24,12,6,0), FX(0,0,0,0,0,0,0xF5DCE8), [FACE_KISS]=256*16}, 640},
    {"confident", {EL(64,32,14,0,0), ER(64,32,14,0,0), MP(40,12,6,-2), FX(0,0,0,0,0,-2,0xE3F1EA), [FACE_SPARKLE]=210*16, [FACE_GAZE_Y]=-16}, 600},
    {"sleepy", {EL(62,20,10,0,0), ER(62,20,10,0,0), MP(28,10,5,0), FX(0,0,0,0,0,0,0xDCDFF0), [FACE_ZZZ]=256*16}, 880},
    {"silly", {EL(62,26,13,0,3), ER(62,26,13,0,3), MP(40,12,6,-3), FX(0,0,0,0,256,0,0xD7F6E9)}, 600},
    {"confused", {EL(64,42,21,0,0), ER(64,42,21,0,0), MP(32,10,5,0), FX(0,0,0,0,0,0,0xE4F0EC), [FACE_QUESTION]=256*16, [FACE_GAZE_X]=2*16, [FACE_GAZE_Y]=-16}, 660},
    {"listening", {EL(64,42,21,0,0), ER(64,42,21,0,0), MP(32,10,5,0), FX(0,0,0,0,0,0,0xDFEFF4), [FACE_LISTEN]=256*16, [FACE_GAZE_X]=-2*16}, 520},
    {"ambient", {EL(62,28,14,-1,0), ER(62,28,14,1,0), MP(36,12,6,-2), FX(0,0,0,0,0,0,0xDCEEE8), [FACE_LEAN]=16, [FACE_GAZE_X]=16}, 820},
};
/* Approved B concept: broad beans/folds, shallow coloured lips and readable props. */
#define BL(w,h,r,s,b) FP(58-(w)/2,48-(h)/2,w,h,r,s,b)
#define FACE_B_RIGHT(w,h,r,s,b) FP(162-(w)/2,48-(h)/2,w,h,r,s,b)
#define BM(w,h,r,b) FP(110-(w)/2,83-(h)/2,w,h,r,0,b)
#define BX(tear,think,blush,heart,tongue,tilt,c) \
    FX(tear,think,blush,heart,tongue,tilt,c), [FACE_STYLE_B]=256*16
static const face_pose_t face_alternate_poses[] = {
    {"neutral", {BL(68,40,18,0,0), FACE_B_RIGHT(68,40,18,0,0), BM(48,12,6,0), BX(0,0,0,0,0,0,0xECF8F5)}, 560},
    {"happy", {BL(64,24,12,-1,9), FACE_B_RIGHT(64,26,13,0,9), BM(50,14,7,-4), BX(0,0,0,0,0,0,0xD5F8EE), [FACE_MOUTH_PINK]=256*16, [FACE_LEAN]=-16}, 560},
    {"laughing", {BL(62,38,13,0,0), FACE_B_RIGHT(62,36,13,0,0), BM(62,22,10,-2), BX(0,0,0,0,0,0,0xD9F8F4), [FACE_FOLD_LEFT]=256*16, [FACE_FOLD_RIGHT]=256*16, [FACE_MOUTH_TWO_TONE]=256*16, [FACE_LEAN]=-16}, 480},
    {"funny", {BL(66,42,20,0,0), FACE_B_RIGHT(62,26,13,0,9), FP(93,76,44,14,7,0,-4), BX(0,0,0,0,0,-4,0xECF7F4), [FACE_MOUTH_PINK]=128*16, [FACE_LEAN]=16}, 600},
    {"sad", {BL(64,34,15,-5,2), FACE_B_RIGHT(64,34,15,5,2), BM(40,11,5,5), BX(0,0,0,0,0,0,0xE0E7FA), [FACE_GAZE_Y]=2*16}, 760},
    {"angry", {BL(64,34,15,5,2), FACE_B_RIGHT(64,34,15,-5,2), BM(34,12,6,4), BX(0,0,0,0,0,0,0xF7DCF0), [FACE_STEAM]=256*16}, 640},
    {"crying", {BL(62,36,13,0,0), FACE_B_RIGHT(62,36,13,0,0), BM(44,11,5,0), BX(256,0,0,0,0,0,0xE1EAFB), [FACE_FOLD_LEFT]=256*16, [FACE_FOLD_RIGHT]=256*16, [FACE_MOUTH_WAVE]=4*16, [FACE_GAZE_Y]=16}, 760},
    {"loving", {BL(64,26,13,-1,9), FACE_B_RIGHT(64,26,13,1,9), BM(48,14,7,-4), BX(0,0,0,256,0,0,0xDAF7F2), [FACE_MOUTH_PINK]=256*16}, 680},
    {"embarrassed", {BL(64,28,14,-4,1), FACE_B_RIGHT(64,28,14,3,1), FP(97,76,36,14,7,0,-3), BX(0,0,230,0,0,-4,0xE5EAF8), [FACE_MOUTH_PINK]=192*16, [FACE_GAZE_Y]=2*16, [FACE_LEAN]=2*16}, 720},
    {"surprised", {BL(68,40,16,0,0), FACE_B_RIGHT(68,40,16,0,0), BM(32,18,5,0), BX(0,0,0,0,0,0,0xEAF8F4), [FACE_EXCLAIM]=256*16, [FACE_MOUTH_PINK]=160*16, [FACE_GAZE_Y]=-16}, 440},
    {"shocked", {BL(68,44,18,0,0), FACE_B_RIGHT(68,44,18,0,0), BM(46,11,5,0), BX(0,0,0,0,0,0,0xE6F2F9), [FACE_EMPHASIS]=256*16, [FACE_MOUTH_WAVE]=4*16, [FACE_LEAN]=2*16}, 480},
    {0}, /* thinking: first set only; preserve the menu index. */
    {"winking", {BL(66,44,21,0,0), FACE_B_RIGHT(64,28,14,0,9), BM(48,14,7,-4), BX(0,0,0,0,0,-3,0xDDF8F0), [FACE_WINK]=256*16, [FACE_MOUTH_PINK]=220*16}, 520},
    {"cool", {FP(26,26,64,28,14,0,1), FP(130,23,64,28,14,0,1), BM(44,14,7,-3), BX(0,0,0,0,0,-4,0xE4F7F4), [FACE_GLASSES]=256*16, [FACE_MOUTH_PINK]=96*16, [FACE_LEAN]=-16}, 680},
    {0}, /* relaxed: first set only; preserve the menu index. */
    {"delicious", {BL(64,26,13,0,10), FACE_B_RIGHT(64,26,13,0,10), BM(48,14,7,-4), BX(0,0,0,0,256,-2,0xD9F4F2), [FACE_TONGUE_X]=17*16, [FACE_MOUTH_PINK]=160*16}, 680},
    {"kissy", {BL(64,28,14,0,10), FACE_B_RIGHT(64,28,14,0,10), BM(24,24,10,0), BX(0,0,0,0,0,0,0xDEF5F2), [FACE_KISS]=256*16, [FACE_MOUTH_PUCKER]=256*16, [FACE_MOUTH_PINK]=256*16}, 680},
    {"confident", {BL(64,32,14,4,1), FACE_B_RIGHT(62,30,14,-4,1), FP(90,76,48,14,7,0,-3), BX(0,0,0,0,0,-5,0xE5F7F1), [FACE_SPARKLE]=256*16, [FACE_MOUTH_PINK]=96*16, [FACE_GAZE_Y]=-16, [FACE_LEAN]=-16}, 600},
    {"sleepy", {BL(64,26,13,0,0), FACE_B_RIGHT(64,26,13,1,0), BM(36,11,5,0), BX(0,0,0,0,0,0,0xE4E4F9), [FACE_ZZZ]=256*16, [FACE_GAZE_Y]=16, [FACE_LEAN]=3*16}, 920},
    {"silly", {BL(64,28,14,0,9), FACE_B_RIGHT(62,36,13,0,0), BM(46,14,7,-4), BX(0,0,0,0,256,3,0xD8F9EF), [FACE_FOLD_RIGHT]=256*16, [FACE_TONGUE_X]=8*16, [FACE_MOUTH_PINK]=96*16, [FACE_LEAN]=16}, 600},
    {"confused", {BL(66,42,20,0,0), FACE_B_RIGHT(66,42,20,0,0), BM(38,12,6,0), BX(0,0,0,0,0,-4,0xE6F6F1), [FACE_QUESTION]=256*16, [FACE_LEAN]=-16}, 680},
    {"listening", {BL(64,42,20,0,0), FACE_B_RIGHT(64,42,20,0,0), BM(36,11,5,0), BX(0,0,0,0,0,0,0xE1F4F8), [FACE_LISTEN]=256*16, [FACE_GAZE_X]=-2*16, [FACE_LEAN]=2*16}, 560},
    {"ambient", {BL(64,28,14,-3,1), FACE_B_RIGHT(64,28,14,3,1), FP(87,76,50,14,7,0,-4), BX(0,0,0,0,0,1,0xDBF5ED), [FACE_MOUTH_PINK]=128*16, [FACE_LEAN]=-2*16}, 880},
};
#undef BL
#undef FACE_B_RIGHT
#undef BM
#undef BX
_Static_assert(sizeof(face_alternate_poses) == sizeof(face_poses), "Face slots must preserve meaning indices");
#undef FP
#undef FX
#undef EL
#undef ER
#undef MP
enum {
    MOTION_LOOK_X, MOTION_LOOK_Y, MOTION_EYE_HEIGHT, MOTION_EYE_BEND,
    MOTION_MOUTH_WIDTH, MOTION_MOUTH_HEIGHT, MOTION_MOUTH_Y,
    MOTION_MOUTH_TILT, MOTION_LEAN, FACE_MOTION_VALUES
};
typedef struct {
    uint16_t at_ms;
    int8_t value[FACE_MOTION_VALUES]; /* Pixel offsets; interpolated in Q8. */
} face_keyframe_t;
typedef struct {
    uint16_t end_ms;
    face_keyframe_t frame[FACE_MOTION_STEPS];
} face_track_t;
typedef struct {
    const char *key;
    face_track_t variant[FACE_VARIANTS];
} face_sequence_t;
/* Each clip starts/ends at zero offset: anticipation, expression, recovery.
 * The selected clip plays once, then the existing quiet idle cues take over. */
#define MK(t,gx,gy,eh,eb,mw,mh,my,mt,lean) {t,{gx,gy,eh,eb,mw,mh,my,mt,lean}}
static const face_sequence_t face_sequences[] = {
    {"neutral", {
        {1400, {MK(240,1,0,1,0,0,0,0,0,0), MK(640,1,0,1,0,0,0,0,0,0), MK(920,0,0,0,0,0,0,0,0,0), MK(1120,0,0,0,0,0,0,0,0,0)}},
        {1640, {MK(320,-1,0,1,0,0,0,0,0,-1), MK(720,-1,0,1,0,0,0,0,0,-1), MK(1080,0,0,0,0,0,0,0,0,0), MK(1320,0,0,0,0,0,0,0,0,0)}},
    }},
    {"happy", {
        {1320, {MK(240,0,-1,1,-1,0,-1,0,0,0), MK(520,0,1,-1,0,1,1,0,0,0), MK(800,0,0,0,0,0,0,0,0,0), MK(1040,0,0,0,0,0,0,0,0,0)}},
        {1560, {MK(280,-1,0,1,-1,-1,0,0,0,-1), MK(640,0,1,-1,0,1,1,0,1,1), MK(960,1,0,0,0,0,0,0,0,1), MK(1240,0,0,0,0,0,0,0,0,0)}},
    }},
    {"laughing", {
        {1600, {MK(200,0,-1,2,-1,-2,-3,0,0,0), MK(520,0,1,-2,0,2,2,1,0,0), MK(820,0,-1,0,0,0,0,0,0,0), MK(1100,0,1,-1,0,1,1,0,0,0)}},
        {1880, {MK(240,0,-1,2,0,-3,-3,-1,0,-1), MK(600,-1,1,-2,0,2,2,1,-1,-2), MK(1000,1,-1,-1,0,1,1,0,1,2), MK(1400,0,1,0,0,0,0,0,0,0)}},
    }},
    {"funny", {
        {1640, {MK(240,1,0,1,0,-1,0,0,0,1), MK(600,1,0,0,0,1,1,0,-1,1), MK(960,-1,0,0,0,0,0,0,0,-1), MK(1280,0,0,0,0,0,0,0,0,0)}},
        {1800, {MK(280,-1,0,1,0,-1,0,0,0,-1), MK(680,-1,1,-1,1,1,1,0,1,-1), MK(1080,1,0,0,0,0,0,0,0,1), MK(1440,0,0,0,0,0,0,0,0,0)}},
    }},
    {"sad", {
        {2080, {MK(360,0,1,-1,0,0,0,1,0,0), MK(840,0,1,-1,0,0,0,1,0,0), MK(1280,0,1,0,0,0,0,0,0,0), MK(1640,0,0,0,0,0,0,0,0,0)}},
        {2320, {MK(400,-1,1,-1,0,0,0,1,0,1), MK(960,-1,1,-1,0,0,0,1,0,1), MK(1480,0,1,0,0,0,0,0,0,1), MK(1840,0,0,0,0,0,0,0,0,0)}},
    }},
    {"angry", {
        {1440, {MK(240,0,-1,-1,0,-1,0,0,0,0), MK(560,0,0,-2,0,-1,0,0,0,0), MK(880,0,0,-1,0,0,0,0,0,0), MK(1160,0,0,0,0,0,0,0,0,0)}},
        {1640, {MK(280,1,0,-1,0,-1,0,0,0,1), MK(640,1,-1,-2,0,0,0,0,0,1), MK(1000,0,0,-1,0,0,0,0,0,0), MK(1320,0,0,0,0,0,0,0,0,0)}},
    }},
    {"crying", {
        {2200, {MK(360,0,1,-1,0,0,0,1,0,0), MK(880,0,1,-1,0,0,0,1,0,0), MK(1360,0,1,0,0,0,0,0,0,0), MK(1800,0,0,0,0,0,0,0,0,0)}},
        {2440, {MK(400,0,1,-1,0,0,0,1,0,-1), MK(880,0,2,-2,0,-1,1,1,0,-1), MK(1400,0,0,0,0,0,0,0,0,0), MK(1960,0,1,-1,0,0,1,1,0,1)}},
    }},
    {"loving", {
        {1800, {MK(280,0,-1,1,0,-1,0,0,0,0), MK(680,0,1,-1,1,1,1,0,0,0), MK(1080,0,0,0,0,0,0,0,0,0), MK(1440,0,0,0,0,0,0,0,0,0)}},
        {2040, {MK(320,-1,0,1,0,-1,0,0,0,-1), MK(800,0,1,-1,1,1,1,0,1,1), MK(1280,0,0,0,0,0,0,0,0,1), MK(1640,0,0,0,0,0,0,0,0,0)}},
    }},
    {"embarrassed", {
        {1960, {MK(320,1,1,-1,0,-1,0,0,0,0), MK(800,1,1,-1,0,-1,0,0,0,0), MK(1240,0,1,0,0,0,0,0,0,0), MK(1600,0,0,0,0,0,0,0,0,0)}},
        {2240, {MK(360,-1,1,-1,0,-1,0,0,0,-1), MK(920,-1,1,-1,0,-1,0,0,0,-1), MK(1440,0,1,0,0,0,0,0,0,-1), MK(1840,0,0,0,0,0,0,0,0,0)}},
    }},
    {"surprised", {
        {1280, {MK(160,0,0,1,0,-1,-1,0,0,0), MK(400,0,-1,2,0,1,2,0,0,0), MK(720,0,0,1,0,0,1,0,0,0), MK(1000,0,0,0,0,0,0,0,0,0)}},
        {1520, {MK(200,1,0,1,0,-1,-1,0,0,1), MK(480,1,-1,2,0,1,2,0,0,1), MK(880,0,0,1,0,0,1,0,0,0), MK(1200,0,0,0,0,0,0,0,0,0)}},
    }},
    {"shocked", {
        {1520, {MK(160,0,0,1,0,-1,-1,0,0,0), MK(440,0,-1,2,0,1,2,0,0,0), MK(880,0,0,1,0,0,1,0,0,0), MK(1200,0,0,0,0,0,0,0,0,0)}},
        {1760, {MK(200,-1,0,1,0,-1,-1,0,0,-1), MK(520,0,-1,2,0,2,2,0,0,0), MK(1040,0,0,1,0,1,1,0,0,0), MK(1400,0,0,0,0,0,0,0,0,0)}},
    }},
    {"thinking", {
        {2240, {MK(320,1,-1,0,0,-1,0,0,0,0), MK(880,1,-1,0,0,-1,0,0,0,0), MK(1440,0,-1,0,0,0,0,0,0,0), MK(1840,0,0,0,0,0,0,0,0,0)}},
    }},
    {"winking", {
        {1440, {MK(240,1,0,0,0,-1,0,0,0,0), MK(600,1,0,0,0,1,1,0,0,1), MK(920,0,0,0,0,0,0,0,0,0), MK(1160,0,0,0,0,0,0,0,0,0)}},
        {1640, {MK(240,1,0,0,0,-1,0,0,0,0), MK(600,1,0,0,0,2,1,0,-1,-1), MK(1040,0,0,0,0,0,0,0,0,0), MK(1320,0,0,0,0,0,0,0,0,0)}},
    }},
    {"cool", {
        {1720, {MK(240,0,-1,0,0,-1,0,0,0,0), MK(680,0,0,0,0,1,0,0,-1,0), MK(1080,0,0,0,0,0,0,0,0,0), MK(1400,0,0,0,0,0,0,0,0,0)}},
        {2000, {MK(280,-1,0,0,0,-1,0,0,0,-1), MK(800,-1,0,0,0,1,0,0,-1,1), MK(1280,0,-1,0,0,0,0,0,0,1), MK(1640,0,0,0,0,0,0,0,0,0)}},
    }},
    {"relaxed", {
        {2240, {MK(400,0,1,-1,0,0,0,0,0,0), MK(960,0,0,0,0,1,1,-1,0,0), MK(1520,0,1,-1,0,0,0,0,0,0), MK(1880,0,0,0,0,0,0,0,0,0)}},
    }},
    {"delicious", {
        {1600, {MK(240,0,-1,1,-1,-1,0,0,0,0), MK(640,0,1,-1,0,1,1,0,0,0), MK(1000,0,0,0,0,0,0,0,0,0), MK(1280,0,0,0,0,0,0,0,0,0)}},
        {1880, {MK(280,-1,0,1,-1,-1,0,0,0,-1), MK(760,-1,1,-1,0,1,1,0,1,-1), MK(1200,0,0,0,0,0,0,0,0,0), MK(1520,0,0,0,0,0,0,0,0,0)}},
    }},
    {"kissy", {
        {1760, {MK(280,0,0,-1,0,-2,0,0,0,0), MK(680,0,0,-1,0,-2,1,0,0,0), MK(1080,0,0,0,0,0,0,0,0,0), MK(1400,0,0,0,0,0,0,0,0,0)}},
        {2040, {MK(320,1,0,-1,0,-2,0,0,0,1), MK(800,1,0,-1,0,-2,1,0,0,1), MK(1280,0,0,0,0,0,0,0,0,0), MK(1640,0,0,0,0,0,0,0,0,0)}},
    }},
    {"confident", {
        {1640, {MK(240,0,-1,1,0,-1,0,0,0,0), MK(640,0,-1,1,0,1,1,0,-1,0), MK(1000,0,0,0,0,0,0,0,0,0), MK(1320,0,0,0,0,0,0,0,0,0)}},
        {1920, {MK(280,1,-1,1,0,-1,0,0,0,1), MK(760,1,-1,1,0,1,1,0,1,1), MK(1200,0,0,0,0,0,0,0,0,0), MK(1560,0,0,0,0,0,0,0,0,0)}},
    }},
    {"sleepy", {
        {2600, {MK(480,0,1,0,0,0,0,0,0,0), MK(1120,0,2,0,0,0,0,1,0,0), MK(1720,0,1,0,0,0,0,0,0,0), MK(2160,0,0,0,0,0,0,0,0,0)}},
        {2880, {MK(520,0,1,-1,0,0,0,0,0,1), MK(1280,0,2,-2,0,0,0,1,0,1), MK(1920,0,1,-1,0,0,0,0,0,1), MK(2400,0,0,0,0,0,0,0,0,0)}},
    }},
    {"silly", {
        {1640, {MK(240,1,0,1,-1,-1,0,0,0,1), MK(600,0,1,-1,0,1,1,0,0,0), MK(1000,-1,0,0,0,0,0,0,0,-1), MK(1320,0,0,0,0,0,0,0,0,0)}},
        {1880, {MK(280,-1,0,1,-1,-1,0,0,0,-1), MK(720,-1,1,-1,0,1,1,0,1,-2), MK(1160,1,0,0,0,0,0,0,0,1), MK(1520,0,0,0,0,0,0,0,0,0)}},
    }},
    {"confused", {
        {1880, {MK(280,1,0,1,0,-1,0,0,0,1), MK(720,-1,0,1,0,0,0,0,0,-1), MK(1160,0,-1,0,0,0,0,0,0,0), MK(1520,0,0,0,0,0,0,0,0,0)}},
        {2160, {MK(320,-1,0,1,0,-1,0,0,0,-1), MK(840,1,0,1,0,0,0,0,0,1), MK(1320,0,-1,0,0,0,0,0,0,1), MK(1760,0,0,0,0,0,0,0,0,0)}},
    }},
    {"listening", {
        {1920, {MK(280,-1,0,1,0,0,0,0,0,0), MK(760,-1,0,2,0,0,0,0,0,-1), MK(1200,-1,0,1,0,0,0,0,0,-1), MK(1560,0,0,0,0,0,0,0,0,0)}},
        {2240, {MK(320,-1,0,1,0,0,0,0,0,1), MK(880,-1,0,2,0,0,0,0,0,1), MK(1400,-1,0,1,0,0,0,0,0,1), MK(1840,0,0,0,0,0,0,0,0,0)}},
    }},
    {"ambient", {
        {2480, {MK(480,1,0,0,0,0,0,0,0,1), MK(1120,1,0,-1,0,0,0,0,0,1), MK(1680,0,0,0,0,0,0,0,0,0), MK(2080,0,0,0,0,0,0,0,0,0)}},
        {2800, {MK(520,-1,1,0,0,0,0,0,0,-1), MK(1280,-1,1,-1,0,0,0,0,0,-1), MK(1920,0,0,0,0,0,0,0,0,0), MK(2320,0,0,0,0,0,0,0,0,0)}},
    }},
};
#undef MK
_Static_assert(sizeof(face_sequences) / sizeof(face_sequences[0]) ==
               sizeof(face_poses) / sizeof(face_poses[0]), "Every face needs a motion entry");
