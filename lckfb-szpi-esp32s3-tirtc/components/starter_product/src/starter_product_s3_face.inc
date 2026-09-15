/* Product-owned procedural faces: two wide-eye sets with distinct silhouettes.
 * Q8 poses -> timed transitions -> bounded gestures -> RGB565 pixels.
 * Mutable animation state/canvas are PSRAM. One LVGL timer uses the existing
 * fixed pool. No frame allocation, extra RTOS task or media/runtime changes. */
#define S3_FACE_PERIOD_MS 40U
/* Includes overlapping old/new ornaments during rapid expression changes. */
#define S3_FACE_BOUNDS 96U
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
    S3_FACE_POSE_VALUES
};
enum { S3_FACE_VARIANTS = 2, S3_FACE_MOTION_STEPS = 4 };
typedef struct {
    const char *key;
    int16_t value[S3_FACE_POSE_VALUES]; /* Q4 preserves half-pixel centres. */
    uint16_t transition_ms;
} s3_face_pose_t;
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
static const s3_face_pose_t s3_face_poses[] = {
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
#define S3_FACE_B_RIGHT(w,h,r,s,b) FP(162-(w)/2,48-(h)/2,w,h,r,s,b)
#define BM(w,h,r,b) FP(110-(w)/2,83-(h)/2,w,h,r,0,b)
#define BX(tear,think,blush,heart,tongue,tilt,c) \
    FX(tear,think,blush,heart,tongue,tilt,c), [FACE_STYLE_B]=256*16
static const s3_face_pose_t s3_face_alternate_poses[] = {
    {"neutral", {BL(68,40,18,0,0), S3_FACE_B_RIGHT(68,40,18,0,0), BM(48,12,6,0), BX(0,0,0,0,0,0,0xECF8F5)}, 560},
    {"happy", {BL(64,24,12,-1,9), S3_FACE_B_RIGHT(64,26,13,0,9), BM(50,14,7,-4), BX(0,0,0,0,0,0,0xD5F8EE), [FACE_MOUTH_PINK]=256*16, [FACE_LEAN]=-16}, 560},
    {"laughing", {BL(62,38,13,0,0), S3_FACE_B_RIGHT(62,36,13,0,0), BM(62,22,10,-2), BX(0,0,0,0,0,0,0xD9F8F4), [FACE_FOLD_LEFT]=256*16, [FACE_FOLD_RIGHT]=256*16, [FACE_MOUTH_TWO_TONE]=256*16, [FACE_LEAN]=-16}, 480},
    {"funny", {BL(66,42,20,0,0), S3_FACE_B_RIGHT(62,26,13,0,9), FP(93,76,44,14,7,0,-4), BX(0,0,0,0,0,-4,0xECF7F4), [FACE_MOUTH_PINK]=128*16, [FACE_LEAN]=16}, 600},
    {"sad", {BL(64,34,15,-5,2), S3_FACE_B_RIGHT(64,34,15,5,2), BM(40,11,5,5), BX(0,0,0,0,0,0,0xE0E7FA), [FACE_GAZE_Y]=2*16}, 760},
    {"angry", {BL(64,34,15,5,2), S3_FACE_B_RIGHT(64,34,15,-5,2), BM(34,12,6,4), BX(0,0,0,0,0,0,0xF7DCF0), [FACE_STEAM]=256*16}, 640},
    {"crying", {BL(62,36,13,0,0), S3_FACE_B_RIGHT(62,36,13,0,0), BM(44,11,5,0), BX(256,0,0,0,0,0,0xE1EAFB), [FACE_FOLD_LEFT]=256*16, [FACE_FOLD_RIGHT]=256*16, [FACE_MOUTH_WAVE]=4*16, [FACE_GAZE_Y]=16}, 760},
    {"loving", {BL(64,26,13,-1,9), S3_FACE_B_RIGHT(64,26,13,1,9), BM(48,14,7,-4), BX(0,0,0,256,0,0,0xDAF7F2), [FACE_MOUTH_PINK]=256*16}, 680},
    {"embarrassed", {BL(64,28,14,-4,1), S3_FACE_B_RIGHT(64,28,14,3,1), FP(97,76,36,14,7,0,-3), BX(0,0,230,0,0,-4,0xE5EAF8), [FACE_MOUTH_PINK]=192*16, [FACE_GAZE_Y]=2*16, [FACE_LEAN]=2*16}, 720},
    {"surprised", {BL(68,40,16,0,0), S3_FACE_B_RIGHT(68,40,16,0,0), BM(32,18,5,0), BX(0,0,0,0,0,0,0xEAF8F4), [FACE_EXCLAIM]=256*16, [FACE_MOUTH_PINK]=160*16, [FACE_GAZE_Y]=-16}, 440},
    {"shocked", {BL(68,44,18,0,0), S3_FACE_B_RIGHT(68,44,18,0,0), BM(46,11,5,0), BX(0,0,0,0,0,0,0xE6F2F9), [FACE_EMPHASIS]=256*16, [FACE_MOUTH_WAVE]=4*16, [FACE_LEAN]=2*16}, 480},
    {0}, /* thinking: first set only; preserve the menu index. */
    {"winking", {BL(66,44,21,0,0), S3_FACE_B_RIGHT(64,28,14,0,9), BM(48,14,7,-4), BX(0,0,0,0,0,-3,0xDDF8F0), [FACE_WINK]=256*16, [FACE_MOUTH_PINK]=220*16}, 520},
    {"cool", {FP(26,26,64,28,14,0,1), FP(130,23,64,28,14,0,1), BM(44,14,7,-3), BX(0,0,0,0,0,-4,0xE4F7F4), [FACE_GLASSES]=256*16, [FACE_MOUTH_PINK]=96*16, [FACE_LEAN]=-16}, 680},
    {0}, /* relaxed: first set only; preserve the menu index. */
    {"delicious", {BL(64,26,13,0,10), S3_FACE_B_RIGHT(64,26,13,0,10), BM(48,14,7,-4), BX(0,0,0,0,256,-2,0xD9F4F2), [FACE_TONGUE_X]=17*16, [FACE_MOUTH_PINK]=160*16}, 680},
    {"kissy", {BL(64,28,14,0,10), S3_FACE_B_RIGHT(64,28,14,0,10), BM(24,24,10,0), BX(0,0,0,0,0,0,0xDEF5F2), [FACE_KISS]=256*16, [FACE_MOUTH_PUCKER]=256*16, [FACE_MOUTH_PINK]=256*16}, 680},
    {"confident", {BL(64,32,14,4,1), S3_FACE_B_RIGHT(62,30,14,-4,1), FP(90,76,48,14,7,0,-3), BX(0,0,0,0,0,-5,0xE5F7F1), [FACE_SPARKLE]=256*16, [FACE_MOUTH_PINK]=96*16, [FACE_GAZE_Y]=-16, [FACE_LEAN]=-16}, 600},
    {"sleepy", {BL(64,26,13,0,0), S3_FACE_B_RIGHT(64,26,13,1,0), BM(36,11,5,0), BX(0,0,0,0,0,0,0xE4E4F9), [FACE_ZZZ]=256*16, [FACE_GAZE_Y]=16, [FACE_LEAN]=3*16}, 920},
    {"silly", {BL(64,28,14,0,9), S3_FACE_B_RIGHT(62,36,13,0,0), BM(46,14,7,-4), BX(0,0,0,0,256,3,0xD8F9EF), [FACE_FOLD_RIGHT]=256*16, [FACE_TONGUE_X]=8*16, [FACE_MOUTH_PINK]=96*16, [FACE_LEAN]=16}, 600},
    {"confused", {BL(66,42,20,0,0), S3_FACE_B_RIGHT(66,42,20,0,0), BM(38,12,6,0), BX(0,0,0,0,0,-4,0xE6F6F1), [FACE_QUESTION]=256*16, [FACE_LEAN]=-16}, 680},
    {"listening", {BL(64,42,20,0,0), S3_FACE_B_RIGHT(64,42,20,0,0), BM(36,11,5,0), BX(0,0,0,0,0,0,0xE1F4F8), [FACE_LISTEN]=256*16, [FACE_GAZE_X]=-2*16, [FACE_LEAN]=2*16}, 560},
    {"ambient", {BL(64,28,14,-3,1), S3_FACE_B_RIGHT(64,28,14,3,1), FP(87,76,50,14,7,0,-4), BX(0,0,0,0,0,1,0xDBF5ED), [FACE_MOUTH_PINK]=128*16, [FACE_LEAN]=-2*16}, 880},
};
#undef BL
#undef S3_FACE_B_RIGHT
#undef BM
#undef BX
_Static_assert(sizeof(s3_face_alternate_poses) == sizeof(s3_face_poses), "Face slots must preserve meaning indices");
#undef FP
#undef FX
#undef EL
#undef ER
#undef MP
enum {
    MOTION_LOOK_X, MOTION_LOOK_Y, MOTION_EYE_HEIGHT, MOTION_EYE_BEND,
    MOTION_MOUTH_WIDTH, MOTION_MOUTH_HEIGHT, MOTION_MOUTH_Y,
    MOTION_MOUTH_TILT, MOTION_LEAN, S3_FACE_MOTION_VALUES
};
typedef struct {
    uint16_t at_ms;
    int8_t value[S3_FACE_MOTION_VALUES]; /* Pixel offsets; interpolated in Q8. */
} s3_face_keyframe_t;
typedef struct {
    uint16_t end_ms;
    s3_face_keyframe_t frame[S3_FACE_MOTION_STEPS];
} s3_face_track_t;
typedef struct {
    const char *key;
    s3_face_track_t variant[S3_FACE_VARIANTS];
} s3_face_sequence_t;
/* Each clip starts/ends at zero offset: anticipation, expression, recovery.
 * The selected clip plays once, then the existing quiet idle cues take over. */
#define MK(t,gx,gy,eh,eb,mw,mh,my,mt,lean) {t,{gx,gy,eh,eb,mw,mh,my,mt,lean}}
static const s3_face_sequence_t s3_face_sequences[] = {
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
_Static_assert(sizeof(s3_face_sequences) / sizeof(s3_face_sequences[0]) ==
               sizeof(s3_face_poses) / sizeof(s3_face_poses[0]), "Every face needs a motion entry");
typedef struct { int16_t x0, y0, x1, y1; } s3_face_bounds_t;
typedef struct {
    lv_timer_t *timer;
    int32_t from[S3_FACE_POSE_VALUES], target[S3_FACE_POSE_VALUES], painted[S3_FACE_POSE_VALUES];
    s3_face_bounds_t bounds[S3_FACE_BOUNDS];
    s3_face_bounds_t damage[3];
    lv_color_t palette[17];
    int64_t cue_at[S3_FACE_POSE_VALUES - FACE_TEARS];
    int64_t transition_at, motion_at, blink_at, gaze_at, gaze_next, last_frame_at, slow_log_at;
    int32_t gaze_from_x, gaze_from_y, gaze_to_x, gaze_to_y, activity;
    uint32_t random, frames, late_frames, max_draw_us, colour;
    uint16_t transition_ms, pose, motion_pose;
    uint8_t bound_count, activity_target, damage_mask, variant;
    starter_ai_ui_phase_t phase;
    bool attached, valid, painted_valid, clear_all;
} s3_face_animation_t;
static EXT_RAM_BSS_ATTR s3_face_animation_t s3_face;

static uint32_t s3_face_random(void)
{
    uint32_t x = s3_face.random;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s3_face.random = x;
}
static int32_t s3_face_ease(int64_t elapsed, uint32_t duration)
{
    if (elapsed <= 0) return 0;
    if (elapsed >= duration) return 1024;
    return (int32_t)lv_bezier3((uint32_t)(elapsed * 1024 / duration), 0, 0, 1024, 1024);
}
static int32_t s3_face_lerp(int32_t a, int32_t b, int32_t t)
{
    return a + (b - a) * t / 1024;
}
static int32_t s3_face_wave(int64_t elapsed, uint32_t period)
{
    return lv_trigo_sin((int16_t)(elapsed % period * 360 / period));
}
static int32_t s3_face_pulse(int64_t age, uint32_t rise, uint32_t hold, uint32_t fall)
{
    if (age <= 0) return 0;
    if (age < rise) return s3_face_ease(age, rise);
    age -= rise;
    if (age < hold) return 1024;
    return 1024 - s3_face_ease(age - hold, fall);
}
static int64_t s3_face_cue_age(unsigned channel, int64_t now)
{
    int64_t age = now - s3_face.cue_at[channel - FACE_TEARS];
    return age > 0 ? age : 0;
}
static void s3_face_motion(int64_t now, int32_t *offset)
{
    memset(offset, 0, S3_FACE_MOTION_VALUES * sizeof(*offset));
    const s3_face_track_t *track = &s3_face_sequences[s3_face.motion_pose].variant[s3_face.variant];
    int64_t age = now - s3_face.motion_at;
    if (age <= 0 || age >= track->end_ms) return;
    const s3_face_keyframe_t *previous = NULL;
    for (unsigned step = 0; step <= S3_FACE_MOTION_STEPS; ++step) {
        const s3_face_keyframe_t *next = step < S3_FACE_MOTION_STEPS ? &track->frame[step] : NULL;
        uint16_t from_ms = previous ? previous->at_ms : 0;
        uint16_t to_ms = next ? next->at_ms : track->end_ms;
        if (age <= to_ms) {
            int32_t t = s3_face_ease(age - from_ms, to_ms - from_ms);
            for (unsigned i = 0; i < S3_FACE_MOTION_VALUES; ++i)
                offset[i] = s3_face_lerp(previous ? previous->value[i] * 256 : 0,
                                         next ? next->value[i] * 256 : 0, t);
            return;
        }
        previous = next;
    }
}
static void s3_face_base(int64_t now, int32_t *pose)
{
    int32_t motion[S3_FACE_MOTION_VALUES];
    s3_face_motion(now, motion);
    for (size_t i = 0; i < S3_FACE_POSE_VALUES; ++i) {
        int32_t offset = 0;
        switch (i) {
        case FACE_GAZE_X: offset = motion[MOTION_LOOK_X]; break;
        case FACE_GAZE_Y: offset = motion[MOTION_LOOK_Y]; break;
        case 3: case 10: offset = motion[MOTION_EYE_HEIGHT]; break;
        case 6: case 13: offset = motion[MOTION_EYE_BEND]; break;
        case 16: offset = motion[MOTION_MOUTH_WIDTH]; break;
        case 17: offset = motion[MOTION_MOUTH_HEIGHT]; break;
        case 15: offset = motion[MOTION_MOUTH_Y]; break;
        case FACE_MOUTH_TILT: offset = motion[MOTION_MOUTH_TILT]; break;
        case FACE_LEAN: offset = motion[MOTION_LEAN]; break;
        default: break;
        }
        int32_t target = s3_face.target[i] + offset;
        if ((i == 3 || i == 10) && target < 20 * 256) target = 20 * 256;
        if (i == 17 && target < 10 * 256) target = 10 * 256;
        /* Blend toward the animated endpoint. Retargeting captures this exact
         * pose, so changing phase never adds the old motion offset twice. */
        int delay = i >= 14 && i < 21 ? 70 : 0;
        int32_t t = s3_face_ease(now - s3_face.transition_at - delay, s3_face.transition_ms);
        pose[i] = s3_face_lerp(s3_face.from[i], target, t);
    }
}
static uint16_t s3_face_pose_index(const char *key)
{
    for (size_t i = 0; i < sizeof(s3_face_poses) / sizeof(s3_face_poses[0]); ++i)
        if (strcmp(key, s3_face_poses[i].key) == 0) return (uint16_t)i;
    return 0;
}
static unsigned s3_face_variant_count(uint16_t index)
{
    return s3_face_alternate_poses[index].key != NULL ? S3_FACE_VARIANTS : 1U;
}
static const s3_face_pose_t *s3_face_pose_for_variant(uint16_t index, unsigned variant)
{
    return variant && s3_face_variant_count(index) > 1U ?
           &s3_face_alternate_poses[index] : &s3_face_poses[index];
}
static void s3_face_set_target(const char *emotion, starter_ai_ui_phase_t phase, uint8_t activity)
{
    const char *key = emotion == NULL ? "neutral" : emotion;
    if (strcmp(key, "calm") == 0) key = "neutral";
    else if (strcmp(key, "excited") == 0) key = "laughing";
    else if (strcmp(key, "curious") == 0) key = "confused";
    else if (strcmp(key, "proud") == 0) key = "confident";
    else if (strcmp(key, "moved") == 0) key = "loving";
    /* Legacy input remains accepted, but is no longer a separate face. */
    else if (strcmp(key, "speech") == 0) key = "listening";
    uint16_t index = s3_face_pose_index(key);
    uint16_t display_index = phase == STARTER_AI_UI_LISTENING ? s3_face_pose_index("listening") :
                             phase == STARTER_AI_UI_THINKING ? s3_face_pose_index("thinking") : index;
    int64_t now = monotonic_ms();
    bool phase_changed = s3_face.phase != phase;
    if (phase_changed) s3_face.gaze_next = now;
    s3_face.activity_target = activity > 3U ? 3U : activity;
    if (s3_face.valid && s3_face.pose == index && !phase_changed) return;
    bool expression_changed = !s3_face.valid || s3_face.motion_pose != display_index;
    /* Capture the old set/clip before drawing a new random choice. */
    if (s3_face.valid) s3_face_base(now, s3_face.from);
    if (expression_changed) {
        unsigned count = s3_face_variant_count(display_index);
        s3_face.variant = count > 1U ? (uint8_t)(s3_face_random() % count) : 0;
        s3_face.motion_at = now;
    }
    const s3_face_pose_t *mood = s3_face_pose_for_variant(index, s3_face.variant);
    const s3_face_pose_t *display = s3_face_pose_for_variant(display_index, s3_face.variant);
    bool attending = phase == STARTER_AI_UI_LISTENING || phase == STARTER_AI_UI_THINKING;
    for (size_t i = 0; i < S3_FACE_POSE_VALUES; ++i) {
        s3_face.target[i] = mood->value[i] * 16;
        if (attending) {
            /* A clear phase face with a trace of the mood; no unrelated props. */
            bool silhouette = i < FACE_TEARS || (i >= FACE_RED && i <= FACE_BLUE);
            s3_face.target[i] = silhouette ? s3_face_lerp(display->value[i] * 16,
                                                          s3_face.target[i], 256) : display->value[i] * 16;
        }
    }
    if (attending) {
        s3_face.target[FACE_GAZE_X] = display->value[FACE_GAZE_X] * 16;
        s3_face.target[FACE_GAZE_Y] = display->value[FACE_GAZE_Y] * 16;
        s3_face.target[FACE_LEAN] = display->value[FACE_LEAN] * 16;
    }
    if (phase == STARTER_AI_UI_THINKING) s3_face.target[FACE_THOUGHT] = 65536;
    if (phase == STARTER_AI_UI_LISTENING) s3_face.target[FACE_LISTEN] = 65536;
    s3_face.target[FACE_ATTENTION] = phase == STARTER_AI_UI_LISTENING ? 65536 : 0;
    s3_face.target[FACE_SPEECH] = phase == STARTER_AI_UI_SPEAKING ? 65536 : 0;
    for (unsigned i = FACE_TEARS; i < S3_FACE_POSE_VALUES; ++i) {
        /* Entry cues follow the new expression, even if the old cue is fading.
         * Activity updates and phases that keep this face do not replay entry. */
        if (!s3_face.valid || (s3_face.target[i] != 0 &&
                              (expression_changed || s3_face.from[i] == 0)))
            s3_face.cue_at[i - FACE_TEARS] = now;
    }
    if (!s3_face.valid) memcpy(s3_face.from, s3_face.target, sizeof(s3_face.from));
    s3_face.phase = phase;
    s3_face.transition_at = now;
    s3_face.transition_ms = display->transition_ms;
    s3_face.pose = index;
    s3_face.motion_pose = display_index;
    s3_face.valid = true;
}
static void s3_face_damage(const s3_face_bounds_t *b)
{
    /* Three small LCD regions rather than one full-face transfer. Include
     * both old and new geometry so blinking/moving parts leave no trails. */
    int centre = (b->x0 + b->x1) / 2;
    unsigned group = centre < 90 ? 0U : centre > 130 ? 1U : 2U;
    s3_face_bounds_t *d = &s3_face.damage[group];
    if (!(s3_face.damage_mask & (1U << group))) {
        *d = *b;
        s3_face.damage_mask |= (uint8_t)(1U << group);
    } else {
        if (b->x0 < d->x0) d->x0 = b->x0;
        if (b->y0 < d->y0) d->y0 = b->y0;
        if (b->x1 > d->x1) d->x1 = b->x1;
        if (b->y1 > d->y1) d->y1 = b->y1;
    }
}
static void s3_face_clear(void)
{
    const lv_color_t background = product_face_background_color();
    s3_face.damage_mask = 0;
    if (s3_face.clear_all) {
        for (size_t i = 0; i < FACE_CANVAS_W * FACE_CANVAS_H; ++i) s_face_canvas_buffer[i] = background;
        const s3_face_bounds_t all = {0, 0, FACE_CANVAS_W - 1, FACE_CANVAS_H - 1};
        s3_face_damage(&all);
    } else {
        for (unsigned i = 0; i < s3_face.bound_count; ++i) {
            const s3_face_bounds_t *b = &s3_face.bounds[i];
            s3_face_damage(b);
            for (int y = b->y0; y <= b->y1; ++y)
                for (int x = b->x0; x <= b->x1; ++x) s_face_canvas_buffer[y * FACE_CANVAS_W + x] = background;
        }
    }
    s3_face.clear_all = false;
    s3_face.bound_count = 0;
}
static void s3_face_record_bounds(const s3_face_bounds_t *bounds)
{
    s3_face_damage(bounds);
    if (s3_face.bound_count < S3_FACE_BOUNDS)
        s3_face.bounds[s3_face.bound_count++] = *bounds;
    else s3_face.clear_all = true;
}
static void s3_face_ink(uint32_t rgb)
{
    if (s3_face.painted_valid && rgb == s3_face.colour) return;
    for (unsigned i = 0; i <= 16; ++i)
        s3_face.palette[i] = lv_color_mix(lv_color_hex(rgb), product_face_background_color(),
                                        (lv_opa_t)(i * 255U / 16U));
    s3_face.colour = rgb;
}
static void s3_face_pixel(int x, int y, unsigned coverage, bool overlay)
{
    if (coverage == 0) return;
    if (coverage > 16U) coverage = 16U;
    lv_color_t *pixel = &s_face_canvas_buffer[y * FACE_CANVAS_W + x];
    if (overlay) {
        /* Dark lenses and coloured accents need source-over, not bright union. */
        *pixel = lv_color_mix(s3_face.palette[16], *pixel, (lv_opa_t)(coverage * 255U / 16U));
        return;
    }
    lv_color_t colour = s3_face.palette[coverage];
    /* Union in RGB order, not the LCD's byte-swapped bus order. */
    uint16_t front = colour.full, back = pixel->full;
#if LV_COLOR_16_SWAP
    front = (uint16_t)((front >> 8) | (front << 8));
    back = (uint16_t)((back >> 8) | (back << 8));
#endif
    if (front > back) *pixel = colour;
}
static void s3_face_column(int x, int32_t top, int32_t bottom, int y0, int y1,
                           unsigned opacity, bool overlay)
{
    int a0 = top / 256, a1 = (bottom + 255) / 256;
    if (a0 < y0) a0 = y0;
    if (a1 > y1) a1 = y1;
    for (int y = a0; y <= a1; ++y) {
        int32_t a = top > y * 256 ? top : y * 256;
        int32_t b = bottom < (y + 1) * 256 ? bottom : (y + 1) * 256;
        if (b > a) s3_face_pixel(x, y, ((unsigned)(b - a) * opacity + 2048U) / 4096U, overlay);
    }
}
typedef enum { FACE_SHAPE_ROUND, FACE_SHAPE_HEART, FACE_SHAPE_SPARK, FACE_SHAPE_TEAR } s3_face_shape_t;
typedef struct {
    int32_t fold, wave, pucker; /* Q8 strength for folds/lips; Q8 pixels for wave. */
    bool lower_half;
} s3_face_profile_t;
static void s3_face_contour(const int32_t *p, unsigned opacity, s3_face_shape_t glyph,
                            bool overlay, const s3_face_profile_t *profile)
{
    if (opacity == 0 || p[2] <= 0 || p[3] <= 0) return;
    if (opacity > 256) opacity = 256;
    int32_t rx = p[2] / 2, ry = p[3] / 2, radius = p[4];
    if (radius > rx) radius = rx;
    if (radius > ry) radius = ry;
    if (radius < 0) radius = 0;
    int x0 = (p[0] - rx) / 256 - 1, x1 = (p[0] + rx) / 256 + 1;
    int extent = (abs(p[5]) + abs(p[6]) + (profile ? abs(profile->wave) : 0)) / 256 + 2;
    int y0 = (p[1] - ry) / 256 - extent, y1 = (p[1] + ry) / 256 + extent;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= FACE_CANVAS_W) x1 = FACE_CANVAS_W - 1;
    if (y1 >= FACE_CANVAS_H) y1 = FACE_CANVAS_H - 1;
    if (x0 > x1 || y0 > y1) return;
    const s3_face_bounds_t bounds = {x0, y0, x1, y1};
    s3_face_record_bounds(&bounds);
    for (int x = x0; x <= x1; ++x) {
        int32_t dx = x * 256 + 128 - p[0];
        if (abs(dx) > rx) continue;
        int32_t half = ry, corner = abs(dx) - (rx - radius);
        if (corner > 0 && radius > 0) {
            lv_sqrt_res_t root;
            /* Q4 before squaring: bounded integer rounded-rectangle raster. */
            int32_t r = radius / 16, c = corner / 16;
            lv_sqrt((uint32_t)(r * r - c * c), &root, 0x8000);
            half = ry - radius + root.i * 16;
        }
        int32_t nx = dx * 256 / rx;
        int32_t centre = p[1] + p[5] * nx / 256 + p[6] * (nx * nx - 32768) / 65536;
        int32_t gap = 0;
        if (profile) {
            int32_t q = 2 * abs(nx) - 256;
            centre += profile->wave * (q * q - 32768) / 65536;
            if (profile->fold) {
                /* Two thick, round-ended arms meeting at the inner corner. */
                int32_t stroke = ry * 7 / 16;
                if (stroke > rx) stroke = rx;
                int32_t run = rx - stroke, cap = stroke;
                int32_t edge = abs(dx) - run;
                if (edge > 0) {
                    lv_sqrt_res_t root;
                    int32_t r = stroke / 16, c = edge / 16;
                    lv_sqrt((uint32_t)(r * r - c * c), &root, 0x8000);
                    cap = root.i * 16;
                }
                int32_t along = run - (profile->fold > 0 ? dx : -dx);
                if (along < 0) along = 0;
                if (along > run * 2) along = run * 2;
                int32_t offset = run > 0 ? (ry - stroke) * along / (run * 2) : 0;
                int32_t folded_half = offset + cap;
                int32_t folded_gap = offset > cap ? offset - cap : 0;
                int32_t t = abs(profile->fold) / 64;
                half = s3_face_lerp(half, folded_half, t);
                gap = folded_gap * t / 1024;
            }
            if (profile->pucker) {
                lv_sqrt_res_t root;
                lv_sqrt((uint32_t)(65536 - nx * nx), &root, 0x8000);
                int32_t lobe = ry * root.i / 512, offset = ry * 7 / 16;
                int32_t t = profile->pucker / 64;
                half = s3_face_lerp(half, offset + lobe, t);
                gap = s3_face_lerp(gap, offset > lobe ? offset - lobe : 0, t);
            }
        }
        int32_t top = centre - half, bottom = centre + half;
        if (glyph == FACE_SHAPE_HEART) {
            int32_t ax = abs(nx), c = ax - 128;
            lv_sqrt_res_t root;
            lv_sqrt((uint32_t)(128 * 128 - c * c), &root, 0x8000);
            top = centre - ry / 2 - ry * root.i / 256;
            bottom = centre + ry - 3 * ry * ax / 512;
        } else if (glyph == FACE_SHAPE_SPARK) {
            int32_t tip = 256 - abs(nx);
            half = ry * tip * tip / 65536;
            top = centre - half; bottom = centre + half;
        } else if (glyph == FACE_SHAPE_TEAR) {
            lv_sqrt_res_t root;
            lv_sqrt((uint32_t)(65536 - nx * nx), &root, 0x8000);
            top = centre - ry + 4 * ry * abs(nx) / 768;
            bottom = centre + ry / 3 + 2 * ry * root.i / 768;
        }
        if (profile && profile->lower_half) {
            if (top < centre + gap) top = centre + gap;
        } else if (gap > 0) {
            s3_face_column(x, top, centre - gap, y0, y1, opacity, overlay);
            top = centre + gap;
        }
        s3_face_column(x, top, bottom, y0, y1, opacity, overlay);
    }
}
static void s3_face_shape(const int32_t *p, unsigned opacity, s3_face_shape_t glyph, bool overlay)
{
    s3_face_contour(p, opacity, glyph, overlay, NULL);
}
static uint32_t s3_face_tint(uint32_t rgb, uint32_t tint, unsigned amount)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned a = (rgb >> shift) & 255U, b = (tint >> shift) & 255U;
        result |= ((a * (256U - amount) + b * amount) / 256U) << shift;
    }
    return result;
}
static void s3_face_mark(int x, int y, int w, int h, int slope, int bend, unsigned opacity)
{
    int32_t p[7] = {x, y, w, h, h / 2, slope, bend};
    s3_face_shape(p, opacity, 0, true);
}
static void s3_face_heart(int x, int y, int size, unsigned opacity)
{
    const int32_t p[7] = {x, y, size, size, size / 2, 0, 0};
    s3_face_shape(p, opacity, FACE_SHAPE_HEART, true);
}
static void s3_face_line(int ax, int ay, int bx, int by, int width, unsigned opacity)
{
    if (opacity == 0) return;
    if (opacity > 256) opacity = 256;
    int radius = width / 2;
    s3_face_bounds_t b = {
        ((ax < bx ? ax : bx) - radius) / 256 - 1,
        ((ay < by ? ay : by) - radius) / 256 - 1,
        ((ax > bx ? ax : bx) + radius) / 256 + 1,
        ((ay > by ? ay : by) + radius) / 256 + 1,
    };
    if (b.x0 < 0) b.x0 = 0;
    if (b.y0 < 0) b.y0 = 0;
    if (b.x1 >= FACE_CANVAS_W) b.x1 = FACE_CANVAS_W - 1;
    if (b.y1 >= FACE_CANVAS_H) b.y1 = FACE_CANVAS_H - 1;
    if (b.x0 > b.x1 || b.y0 > b.y1) return;
    s3_face_record_bounds(&b);
    int dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy;
    int outer = radius + 128, inner = radius > 128 ? radius - 128 : 0;
    for (int y = b.y0; y <= b.y1; ++y) {
        for (int x = b.x0; x <= b.x1; ++x) {
            int px = x * 256 + 128 - ax, py = y * 256 + 128 - ay;
            int t = length2 ? (int)(((int64_t)px * dx + (int64_t)py * dy) * 256 / length2) : 0;
            if (t < 0) t = 0;
            if (t > 256) t = 256;
            px -= dx * t / 256; py -= dy * t / 256;
            int distance2 = px * px + py * py;
            if (distance2 >= outer * outer) continue;
            unsigned coverage = 256;
            if (distance2 > inner * inner) {
                lv_sqrt_res_t root;
                lv_sqrt((uint32_t)distance2, &root, 0x8000);
                coverage = (unsigned)(outer - root.i);
            }
            s3_face_pixel(x, y, (coverage * opacity + 2048U) / 4096U, true);
        }
    }
}
static void s3_face_path(const int8_t (*points)[2], unsigned count,
                          int x, int y, int scale, int width, unsigned opacity)
{
    for (unsigned i = 1; i < count; ++i)
        s3_face_line(x + points[i - 1][0] * scale, y + points[i - 1][1] * scale,
                     x + points[i][0] * scale, y + points[i][1] * scale, width, opacity);
}
static void s3_face_note(int x, int y, int scale, unsigned opacity)
{
    s3_face_mark(x - 2 * scale, y + 5 * scale, 6 * scale, 4 * scale, -scale, 0, opacity);
    s3_face_line(x, y + 4 * scale, x, y - 6 * scale, 2 * scale, opacity);
    s3_face_line(x, y - 6 * scale, x + 4 * scale, y - 4 * scale, 2 * scale, opacity);
}
static void s3_face_render(const int32_t *pose, int64_t now)
{
    uint32_t rgb = ((uint32_t)(pose[FACE_RED] / 256) << 16) |
                   ((uint32_t)(pose[FACE_GREEN] / 256) << 8) | (uint32_t)(pose[FACE_BLUE] / 256);
    s3_face_ink(rgb);
    s3_face_clear();
    unsigned style = (unsigned)pose[FACE_STYLE_B] / 256;
    unsigned glasses = (unsigned)pose[FACE_GLASSES] / 256;
    for (unsigned i = 0; i < 2; ++i) {
        const s3_face_profile_t eye = {
            .fold = i == 0 ? pose[FACE_FOLD_LEFT] : -pose[FACE_FOLD_RIGHT],
        };
        unsigned visible = 256 - glasses + glasses * style / 256;
        s3_face_contour(pose + i * 7, visible, FACE_SHAPE_ROUND, false, &eye);
    }
    const int32_t *mouth = pose + 14;
    const int32_t *right = pose + 7;
    s3_face_profile_t lips = {.wave = pose[FACE_MOUTH_WAVE], .pucker = pose[FACE_MOUTH_PUCKER]};
    s3_face_ink(s3_face_tint(rgb, 0xF8DDF1, (unsigned)pose[FACE_MOUTH_PINK] / 256));
    s3_face_contour(mouth, 256, FACE_SHAPE_ROUND, false, &lips);
    if (pose[FACE_MOUTH_TWO_TONE]) {
        lips.lower_half = true;
        s3_face_ink(0xF4C9E4);
        s3_face_contour(mouth, (unsigned)pose[FACE_MOUTH_TWO_TONE] / 256,
                        FACE_SHAPE_ROUND, true, &lips);
    }

    unsigned blush = (unsigned)pose[FACE_BLUSH] / 256;
    unsigned tongue = (unsigned)pose[FACE_TONGUE] / 256;
    unsigned heart = (unsigned)pose[FACE_HEART] / 256;
    unsigned kiss = (unsigned)pose[FACE_KISS] / 256;
    if (blush || tongue || heart || kiss) {
        s3_face_ink(0xF3BDD4);
        for (unsigned i = 0; i < 2; ++i) {
            int x = pose[i * 7], y = pose[i * 7 + 1] + 25 * 256;
            s3_face_mark(x, y, 13 * 256, 5 * 256, 0, 0, blush * (256 - style) / 256);
            for (int j = -1; j <= 1; j += 2)
                s3_face_line(x + j * 6 * 256 - 2 * 256, y + 3 * 256,
                             x + j * 6 * 256 + 2 * 256, y - 3 * 256,
                             5 * 256, blush * style / 256);
        }
        int32_t tx = pose[FACE_TONGUE_X], nx = tx * 512 / mouth[2];
        int32_t lip = mouth[1] + mouth[3] / 2 + mouth[5] * nx / 256 +
                      mouth[6] * (nx * nx - 32768) / 65536;
        s3_face_mark(mouth[0] + tx, lip + 256, 9 * 256 + (int)style * 3,
                     9 * 256 + (int)style * 5, (int)style, 0, tongue);
        int32_t lift = s3_face_pulse(s3_face_cue_age(FACE_HEART, now) - 450, 600, 200, 900) / 4;
        s3_face_heart(right[0] + 24 * 256 - (int)style * 44, 16 * 256 - lift,
                      14 * 256 + (int)style * 4, heart);
        s3_face_heart(right[0] - 44 * 256, 25 * 256 - lift / 2,
                      12 * 256, heart * style / 256);
        int32_t drift = s3_face_pulse(s3_face_cue_age(FACE_KISS, now) - 400, 800, 200, 1000) / 2;
        s3_face_heart(mouth[0] + 42 * 256 + drift * (256 + (int)style * 2) / 256,
                      mouth[1] - 256, 14 * 256 + (int)style * 6, kiss);
    }

    unsigned tears = (unsigned)pose[FACE_TEARS] / 256;
    if (tears) {
        s3_face_ink(0xA8DDF2);
        int32_t fall = s3_face_ease(s3_face_cue_age(FACE_TEARS, now) - 550, 1000) * 3 / 4;
        for (unsigned i = 0; i < 2; ++i) {
            const int32_t *eye = pose + i * 7;
            const int32_t drop[7] = {eye[0] + (i ? 1 : -1) * (17 * 256 + (int)style * 7),
                eye[1] + eye[3] / 2 + 8 * 256 + (int)style + fall,
                7 * 256 + (int)style * 3, 11 * 256 + (int)style * 5, 0, 0, 0};
            s3_face_shape(drop, i ? tears : tears * style / 256, FACE_SHAPE_TEAR, true);
        }
    }

    if (glasses) {
        int32_t settle = (1024 - s3_face_ease(s3_face_cue_age(FACE_GLASSES, now), 800)) / 2;
        settle = settle * (256 + (int)style * 7) / 256;
        int32_t low = (int)style * 20 - settle;
        s3_face_ink(0xA6CBE9);
        s3_face_mark((pose[0] + right[0]) / 2, (pose[1] + right[1]) / 2 - 12 * 256 + low,
                     60 * 256, 5 * 256, 0, 0, glasses);
        for (unsigned i = 0; i < 2; ++i) {
            const int32_t *eye = pose + i * 7;
            int32_t frame[7] = {eye[0], eye[1] + low, 68 * 256 + (int)style * 8,
                40 * 256 + (int)style * 4, 14 * 256 + (int)style * 4, -(int)style, 0};
            s3_face_ink(0xA6CBE9);
            s3_face_shape(frame, glasses, 0, true);
            frame[1] += 2 * 256; frame[2] -= 4 * 256; frame[3] -= 4 * 256;
            s3_face_ink(0x344350);
            s3_face_shape(frame, glasses, 0, true);
            s3_face_ink(0xB8D9F3);
            s3_face_line(eye[0] - 19 * 256, eye[1] + 2 * 256 + low,
                         eye[0] - 9 * 256, eye[1] - 8 * 256 + low, 3 * 256, glasses * 3 / 4);
        }
    }

    unsigned music = (unsigned)pose[FACE_MUSIC] / 256;
    if (music) {
        int32_t lift = s3_face_pulse(s3_face_cue_age(FACE_MUSIC, now) % 7200 - 1000, 700, 300, 1200) / 2;
        lift = lift * (256 + (int)style) / 256;
        s3_face_ink(s3_face_tint(0xF2E8CE, 0xC9F8EA, style));
        s3_face_note(right[0] + 12 * 256 - (int)style * 5, 15 * 256 + (int)style - lift,
                     256 + (int)style / 2, music);
        s3_face_ink(s3_face_tint(0xF2E8CE, 0xF0CAEA, style));
        s3_face_note(right[0] + 28 * 256, 23 * 256 - lift / 2,
                     256 + (int)style / 4, music * 4 / 5);
    }
    unsigned zzz = (unsigned)pose[FACE_ZZZ] / 256;
    if (zzz) {
        static const int8_t z_path[][2] = {{0,0},{6,0},{0,7},{6,7}};
        s3_face_ink(0xCAC6EB);
        int32_t lift = s3_face_pulse(s3_face_cue_age(FACE_ZZZ, now) % 8800 - 1200, 1400, 400, 1800) / 2;
        for (unsigned i = 0; i < 3; ++i) {
            int scale = 192 + (int)i * 48 + (int)style / 2;
            s3_face_path(z_path, 4, right[0] + ((int)i * 14 - 10) * 256,
                         (24 - (int)i * 8) * 256 - lift * (int)(i + 1) / 3,
                         scale, 2 * 256 + (int)style, zzz);
        }
    }

    unsigned question = (unsigned)pose[FACE_QUESTION] / 256;
    if (question) {
        static const int8_t question_path[][2] = {{0,0},{2,-3},{6,-3},{8,0},{7,3},{4,5},{4,7}};
        s3_face_ink(0xADD6EF);
        int x = right[0] + 32 * 256, y = 13 * 256;
        s3_face_path(question_path, 7, x, y, 256 + (int)style / 2, 3 * 256 + (int)style, question);
        s3_face_mark(x + 4 * 256 + (int)style * 2, y + 12 * 256 + (int)style * 5,
                     3 * 256 + (int)style, 3 * 256 + (int)style, 0, 0, question);
    }
    unsigned exclaim = (unsigned)pose[FACE_EXCLAIM] / 256;
    if (exclaim) {
        s3_face_ink(0xF4E5AE);
        s3_face_mark(right[0] + 37 * 256, 16 * 256, 4 * 256 + (int)style * 2, 10 * 256 + (int)style * 3, 0, 0, exclaim);
        s3_face_mark(right[0] + 37 * 256, 27 * 256, 4 * 256 + (int)style, 4 * 256 + (int)style, 0, 0, exclaim);
    }
    unsigned emphasis = (unsigned)pose[FACE_EMPHASIS] / 256;
    if (emphasis) {
        s3_face_ink(0xDDEAF7);
        s3_face_line(102 * 256, 12 * 256, 104 * 256, 19 * 256, 4 * 256, emphasis);
        s3_face_line(118 * 256, 12 * 256, 116 * 256, 19 * 256, 4 * 256, emphasis);
        s3_face_line(130 * 256, 20 * 256, 127 * 256, 24 * 256, 4 * 256, emphasis * style / 256);
    }
    unsigned sparkle = (unsigned)pose[FACE_SPARKLE] / 256;
    if (sparkle) {
        s3_face_ink(0xF4E7B6);
        int x = right[0] + 32 * 256, y = 15 * 256;
        const int32_t star[7] = {x, y, 13 * 256 + (int)style * 3, 15 * 256 + (int)style * 5, 0, 0, 0};
        s3_face_shape(star, sparkle, FACE_SHAPE_SPARK, true);
    }

    unsigned listen = (unsigned)pose[FACE_LISTEN] / 256;
    if (listen) {
        static const int8_t outer[][2] = {{5,-14},{2,-8},{1,0},{2,8},{5,14}};
        static const int8_t inner[][2] = {{11,-7},{9,-3},{9,3},{11,7}};
        int x = pose[0] - pose[2] / 2 - 14 * 256, y = pose[1];
        int wave = s3_face_wave(s3_face_cue_age(FACE_LISTEN, now), 2400);
        unsigned alpha = listen * (unsigned)(232 + wave * 24 / 32768) / 256;
        s3_face_ink(0xA8D4EF);
        s3_face_path(outer, 5, x, y, 256, 2 * 256 + (int)style * 2, alpha * 4 / 5);
        s3_face_path(inner, 4, x, y, 256, 2 * 256 + (int)style * 2, alpha);
    }
    unsigned thought = (unsigned)pose[FACE_THOUGHT] / 256;
    if (thought) {
        s3_face_ink(rgb);
        for (unsigned i = 0; i < 3; ++i) {
            int wave = s3_face_wave(s3_face_cue_age(FACE_THOUGHT, now) + i * 300, 2400);
            unsigned alpha = thought * (unsigned)(216 + wave * 32 / 32768) / 256;
            int size = (i == 1 ? 6 : 4) * 256 + (int)style * (int)(i + 1);
            s3_face_mark(right[0] + ((int)i - 1) * 12 * 256,
                         14 * 256 + (1 - (int)i) * (int)style * 4,
                         size, size, 0, 0, alpha);
        }
    }
    unsigned steam = (unsigned)pose[FACE_STEAM] / 256;
    if (steam) {
        int32_t lift = s3_face_pulse(s3_face_cue_age(FACE_STEAM, now) - 320, 400, 300, 1000);
        int x = right[0] + 29 * 256, y = 16 * 256 - lift / 2;
        s3_face_ink(0xF5DAF0);
        s3_face_mark(x, y, 15 * 256, 12 * 256, 0, 0, steam);
        s3_face_mark(x + 6 * 256, y - 6 * 256, 12 * 256, 12 * 256, 0, 0, steam);
        s3_face_mark(x + 11 * 256, y + 256, 12 * 256, 11 * 256, 0, 0, steam);
        s3_face_line(x - 2 * 256, y + 4 * 256, x - 5 * 256, y + 11 * 256, 4 * 256, steam);
    }
}
static void s3_face_frame(lv_timer_t *timer)
{
    (void)timer;
    if (!s3_face.attached || !s_display_awake || s_face == NULL ||
        (s_page != PAGE_HOME_FACE && s_page != PAGE_EMOJI_PREVIEW)) return;
    int64_t started = esp_timer_get_time(), now = started / 1000;
    int32_t pose[S3_FACE_POSE_VALUES];
    s3_face_base(now, pose);
    pose[19] += pose[FACE_MOUTH_TILT];
    int64_t dt = s3_face.last_frame_at ? now - s3_face.last_frame_at : S3_FACE_PERIOD_MS;
    if (dt > 80) ++s3_face.late_frames;
    if (dt > 160) dt = 160;
    if (dt < 0) dt = 0;
    s3_face.last_frame_at = now;
    s3_face.activity = s3_face_lerp(s3_face.activity, s3_face.activity_target * 256, (int32_t)dt * 1024 / 160);
    int32_t gaze_t = s3_face_ease(now - s3_face.gaze_at, 560);
    int32_t gx = s3_face_lerp(s3_face.gaze_from_x, s3_face.gaze_to_x, gaze_t);
    int32_t gy = s3_face_lerp(s3_face.gaze_from_y, s3_face.gaze_to_y, gaze_t);
    if (now >= s3_face.gaze_next) {
        s3_face.gaze_from_x = gx; s3_face.gaze_from_y = gy;
        s3_face.gaze_to_x = ((int)(s3_face_random() % 5U) - 2) * 256;
        s3_face.gaze_to_y = ((int)(s3_face_random() % 3U) - 1) * 256;
        if (s3_face.phase == STARTER_AI_UI_LISTENING || s3_face.phase == STARTER_AI_UI_SPEAKING)
            s3_face.gaze_to_x = s3_face.gaze_to_y = 0;
        else if (s3_face.phase == STARTER_AI_UI_THINKING) { s3_face.gaze_to_x = 2 * 256; s3_face.gaze_to_y = -256; }
        s3_face.gaze_at = now;
        s3_face.gaze_next = now + 6000 + s3_face_random() % 4000U;
    }
    int32_t blink = 1024;
    int64_t blink_ms = now - s3_face.blink_at;
    if (blink_ms >= 0 && blink_ms < 240) {
        if (blink_ms < 80) blink = 1024 - s3_face_ease(blink_ms, 80);
        else if (blink_ms < 120) blink = 0;
        else blink = s3_face_ease(blink_ms - 120, 120);
    } else if (blink_ms >= 240) s3_face.blink_at = now + 3000 + s3_face_random() % 3500U;
    gx += pose[FACE_GAZE_X]; gy += pose[FACE_GAZE_Y];
    /* Keep all looks inside the same canvas, including the side sound waves. */
    if (gx > 3 * 256) gx = 3 * 256;
    if (gx < -3 * 256) gx = -3 * 256;
    if (gy > 2 * 256) gy = 2 * 256;
    if (gy < -2 * 256) gy = -2 * 256;
    int32_t tilt = pose[FACE_LEAN];
    int32_t style = pose[FACE_STYLE_B] / 256;
    int32_t wink = s3_face_pulse(s3_face_cue_age(FACE_WINK, now) % 6800 - 420, 120, 160, 240);
    wink = wink * (pose[FACE_WINK] / 256) / 256;
    if (wink > 0) blink = 1024;
    int32_t breath = s3_face_wave(now, 4800) * 104 / 32768;
    for (unsigned i = 0; i < 3; ++i) {
        int32_t *part = pose + i * 7;
        part[0] += i == 2 ? gx / 3 : gx;
        part[1] += gy + breath + (part[0] - 110 * 256) * tilt / (64 * 256);
        if (i < 2) {
            part[3] += s3_face.activity * pose[FACE_ATTENTION] / (3 * 65536);
            /* Blend blink eligibility, not just blink time: morphing through a
             * folded eye or resting lid must not switch eye height in one frame. */
            int32_t fold = pose[i == 0 ? FACE_FOLD_LEFT : FACE_FOLD_RIGHT];
            int32_t openness = (part[3] - (24 * 256 + style * 8)) * 2;
            if (openness < 0) openness = 0;
            if (openness > 1024) openness = 1024;
            int32_t closure = (1024 - blink) * openness / 1024;
            closure = closure * (1024 - fold / 64) / 1024;
            part[3] = s3_face_lerp(part[3], 10 * 256 + style * 10, closure);
            part[6] = s3_face_lerp(part[6], 0, closure);
            int32_t left_wink = wink * (pose[FACE_WINK_LEFT] / 256) / 256;
            int32_t eye_wink = i == 0 ? left_wink : wink - left_wink;
            part[3] = s3_face_lerp(part[3], 14 * 256 + style * 10, eye_wink);
            part[6] = s3_face_lerp(part[6], 3 * 256 + style * 6, eye_wink);
        }
    }
    int32_t hum = s3_face_pulse(s3_face_cue_age(FACE_MUSIC, now) % 7200 - 1000, 700, 300, 1200);
    pose[15] -= hum * (pose[FACE_MUSIC] / 256) / (8 * 256) * (256 + style * 3) / 256;
    /* A state accent, not fabricated lip sync from the microphone meter. */
    pose[17] += (s3_face_wave(now, 1000) + 32768) * 192 / 65536 * pose[FACE_SPEECH] / 65536;
    s3_face_render(pose, now);
    memcpy(s3_face.painted, pose, sizeof(pose));
    s3_face.painted_valid = true;
    lv_area_t coords;
    lv_obj_get_coords(s_face, &coords);
    for (unsigned i = 0; i < 3; ++i) {
        if (!(s3_face.damage_mask & (1U << i))) continue;
        const s3_face_bounds_t *d = &s3_face.damage[i];
#if CONFIG_IDF_TARGET_ESP32P4
        /* Image zoom is separate from LVGL's object/style transform. Convert
         * the union of old/new pixels to screen damage before invalidating.
         * Pivot is (0,0), angle is zero (create_face). Round outwards and keep
         * the image sampler's edge pixels; raw bounds left hollow/trailing
         * eyes, while full-canvas invalidation stalled touch on every frame. */
        const int32_t zoom = lv_img_get_zoom(s_face);
        lv_area_t area = {
            coords.x1 + d->x0 * zoom / 256 - 2,
            coords.y1 + d->y0 * zoom / 256 - 2,
            coords.x1 + ((d->x1 + 1) * zoom + 255) / 256 + 2,
            coords.y1 + ((d->y1 + 1) * zoom + 255) / 256 + 2,
        };
#else
        lv_area_t area = {coords.x1 + d->x0, coords.y1 + d->y0,
                          coords.x1 + d->x1, coords.y1 + d->y1};
#endif
        lv_obj_invalidate_area(s_face, &area);
    }
    ++s3_face.frames;
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    if (elapsed > s3_face.max_draw_us) s3_face.max_draw_us = elapsed;
    if (elapsed > 8000U && now - s3_face.slow_log_at > 5000) {
        s3_face.slow_log_at = now;
        ESP_LOGW(TAG, "face draw slow: us=%u late=%u", (unsigned)elapsed, (unsigned)s3_face.late_frames);
    }
}
static void s3_face_visibility_changed(bool awake)
{
    if (s3_face.timer == NULL) return;
    if (awake && s3_face.attached) {
        s3_face.last_frame_at = 0;
        lv_timer_resume(s3_face.timer);
        lv_timer_ready(s3_face.timer);
    } else lv_timer_pause(s3_face.timer);
}
static void s3_face_detach(void)
{
    s3_face.attached = false;
    s3_face_visibility_changed(false);
}
static void s3_face_attach(void)
{
    int64_t now = monotonic_ms();
    s3_face.attached = true;
    s3_face.valid = s3_face.painted_valid = false;
    s3_face.bound_count = 0; s3_face.clear_all = false;
    s3_face.last_frame_at = 0;
    s3_face.random = (uint32_t)now | 1U;
    s3_face.gaze_from_x = s3_face.gaze_to_x = 0;
    s3_face.gaze_from_y = s3_face.gaze_to_y = 0;
    s3_face.gaze_at = now; s3_face.gaze_next = now + 6000;
    s3_face.blink_at = now + 2600;
    s3_face.activity = 0;
    s3_face_set_target("neutral", STARTER_AI_UI_IDLE, 0);
    if (s3_face.timer == NULL) {
        s3_face.timer = lv_timer_create(s3_face_frame, S3_FACE_PERIOD_MS, NULL);
        if (s3_face.timer == NULL) ESP_LOGE(TAG, "face animation timer allocation failed");
    }
    s3_face_visibility_changed(s_display_awake);
    s3_face_frame(NULL);
}
