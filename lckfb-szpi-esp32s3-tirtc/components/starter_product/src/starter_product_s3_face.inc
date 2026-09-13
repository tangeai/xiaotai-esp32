/* Product-owned procedural face: one eye/mouth family, expressive through motion.
 * Q8 poses -> timed transitions -> bounded gestures -> RGB565 pixels.
 * Mutable animation state/canvas are PSRAM. One LVGL timer uses the existing
 * fixed pool. No frame allocation, extra RTOS task or media/runtime changes. */
#define S3_FACE_PERIOD_MS 40U
#define S3_FACE_BOUNDS 16U
enum {
    FACE_TEARS = 21, FACE_THOUGHT, FACE_BLUSH, FACE_HEART,
    FACE_TONGUE, FACE_MOUTH_TILT,
    FACE_RED, FACE_GREEN, FACE_BLUE,
    FACE_ATTENTION, FACE_SPEECH, S3_FACE_POSE_VALUES
};
typedef enum { FACE_STILL, FACE_NOD, FACE_LAUGH, FACE_TILT, FACE_STARTLE } s3_face_gesture_t;
typedef struct {
    const char *key;
    int16_t value[S3_FACE_POSE_VALUES]; /* Q4 preserves half-pixel centres. */
    uint16_t transition_ms;
    s3_face_gesture_t gesture;
} s3_face_pose_t;
/* Each part: centre x/y, width/height, radius, shear, quadratic bend. */
#define FP(x,y,w,h,r,s,b) ((x)*16+(w)*8),((y)*16+(h)*8),(w)*16,(h)*16,(r)*16,(s)*16,(b)*16
#define EL(w,h,r,s,b) FP(50-(w)/2,50-(h)/2,w,h,r,s,b)
#define ER(w,h,r,s,b) FP(170-(w)/2,50-(h)/2,w,h,r,s,b)
#define MP(w,h,r,b) FP(110-(w)/2,91-(h)/2,w,h,r,0,b)
#define FX(tear,think,blush,heart,tongue,tilt,c) \
    (tear)*16,(think)*16,(blush)*16,(heart)*16,(tongue)*16,(tilt)*16, \
    (((c)>>16)&255)*16,(((c)>>8)&255)*16,((c)&255)*16
/* Small LCD: expression comes from solid silhouettes, not hairline ornaments. */
static const s3_face_pose_t s3_face_poses[] = {
    {"neutral", {EL(64,42,21,0,0), ER(64,42,21,0,0), MP(40,7,3,-2), FX(0,0,0,0,0,0,0xF4FCFF)}, 520, FACE_STILL},
    {"happy", {EL(62,14,7,0,15), ER(62,14,7,0,15), MP(48,9,4,-11), FX(0,0,0,0,0,0,0xD7F6E9)}, 500, FACE_NOD},
    {"laughing", {EL(58,12,6,0,20), ER(58,12,6,0,20), MP(42,22,11,-3), FX(0,0,0,0,0,0,0xD7F6E9)}, 460, FACE_LAUGH},
    {"funny", {EL(58,42,21,-5,0), ER(56,16,8,3,12), MP(44,9,4,-8), FX(0,0,0,0,0,-5,0xF5ECD8)}, 520, FACE_TILT},
    {"sad", {EL(56,24,12,-5,-4), ER(56,24,12,5,-4), MP(34,8,4,10), FX(0,0,0,0,0,0,0xD0E0F1)}, 700, FACE_STILL},
    {"angry", {EL(60,20,7,10,0), ER(60,20,7,-10,0), MP(32,8,4,4), FX(0,0,0,0,0,0,0xF3D3D1)}, 480, FACE_STILL},
    {"crying", {EL(52,10,5,-4,-12), ER(52,10,5,4,-12), MP(24,14,7,5), FX(240,0,0,0,0,0,0xD0E5F4)}, 700, FACE_STILL},
    {"loving", {EL(50,42,21,0,0), ER(50,42,21,0,0), MP(38,8,4,-8), FX(0,0,100,256,0,0,0xF5DCE8)}, 640, FACE_NOD},
    {"embarrassed", {EL(44,26,13,0,0), ER(44,26,13,0,0), MP(20,7,3,-3), FX(0,0,200,0,0,0,0xF1DFE9)}, 660, FACE_STILL},
    {"surprised", {EL(42,48,21,0,0), ER(42,48,21,0,0), MP(14,18,7,0), FX(0,0,0,0,0,0,0xF6EEDB)}, 400, FACE_STARTLE},
    {"shocked", {EL(64,54,25,0,0), ER(64,54,25,0,0), MP(22,24,11,0), FX(0,0,0,0,0,0,0xF2E5D4)}, 400, FACE_STARTLE},
    {"thinking", {EL(58,28,12,0,0), ER(54,19,8,-3,0), MP(26,7,3,0), FX(0,220,0,0,0,-3,0xE2E3F2)}, 600, FACE_TILT},
    {"winking", {EL(56,44,22,0,0), ER(58,9,4,0,12), MP(40,9,4,-8), FX(0,0,0,0,0,0,0xD7F6E9)}, 500, FACE_NOD},
    {"cool", {EL(66,22,5,0,0), ER(66,22,5,0,0), MP(32,7,3,-2), FX(0,0,0,0,0,-4,0xD1ECF5)}, 580, FACE_STILL},
    {"relaxed", {EL(54,9,4,0,-5), ER(54,9,4,0,-5), MP(34,7,3,-5), FX(0,0,0,0,0,0,0xDCEEE8)}, 800, FACE_STILL},
    {"delicious", {EL(54,11,5,0,10), ER(54,11,5,0,10), MP(36,9,4,-6), FX(0,0,0,0,220,0,0xF5E5D1)}, 580, FACE_NOD},
    {"kissy", {EL(50,10,5,0,7), ER(50,10,5,0,7), MP(12,10,5,0), FX(0,0,140,0,0,0,0xF5DCE8)}, 660, FACE_STILL},
    {"confident", {EL(62,36,16,-3,0), ER(62,36,16,3,0), MP(46,9,4,-10), FX(0,0,0,0,0,0,0xD7F1E8)}, 560, FACE_NOD},
    {"sleepy", {EL(58,8,4,0,2), ER(58,8,4,0,2), MP(14,12,6,0), FX(0,0,0,0,0,0,0xD5E1E8)}, 880, FACE_STILL},
    {"silly", {EL(44,46,22,2,0), ER(60,18,9,-4,2), MP(38,12,6,-4), FX(0,0,0,0,256,4,0xF5ECD8)}, 560, FACE_TILT},
    {"confused", {EL(44,44,22,0,0), ER(60,21,10,-3,0), MP(24,8,4,4), FX(0,0,0,0,0,4,0xE2E3F2)}, 600, FACE_TILT},
    {"listening", {EL(64,42,21,0,0), ER(64,42,21,0,0), MP(32,7,3,-1), FX(0,0,0,0,0,0,0xDFEFF4)}, 520, FACE_STILL},
    {"ambient", {EL(62,38,19,0,0), ER(62,38,19,0,0), MP(30,6,3,-1), FX(0,0,0,0,0,0,0xD2E1E6)}, 720, FACE_STILL},
    {"speech", {EL(62,40,20,0,0), ER(62,40,20,0,0), MP(36,8,4,-3), FX(0,0,0,0,0,0,0xD6F0E5)}, 580, FACE_STILL},
};
#undef FP
#undef FX
#undef EL
#undef ER
#undef MP
typedef struct { int16_t x0, y0, x1, y1; } s3_face_bounds_t;
typedef struct {
    lv_timer_t *timer;
    int32_t from[S3_FACE_POSE_VALUES], target[S3_FACE_POSE_VALUES], painted[S3_FACE_POSE_VALUES];
    s3_face_bounds_t bounds[S3_FACE_BOUNDS];
    s3_face_bounds_t damage[3];
    lv_color_t palette[17];
    int64_t transition_at, blink_at, gaze_at, gaze_next, last_frame_at, slow_log_at;
    int32_t gaze_from_x, gaze_from_y, gaze_to_x, gaze_to_y, activity;
    int32_t carry_nod, carry_tilt;
    uint32_t random, frames, late_frames, max_draw_us, colour;
    uint16_t transition_ms, pose;
    uint8_t bound_count, activity_target, damage_mask;
    starter_ai_ui_phase_t phase;
    s3_face_gesture_t gesture;
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
static void s3_face_gesture(int64_t now, int32_t *nod, int32_t *tilt)
{
    int64_t age = now - s3_face.transition_at;
    int32_t carry = 1024 - s3_face_ease(age, 320);
    *nod = s3_face.carry_nod * carry / 1024;
    *tilt = s3_face.carry_tilt * carry / 1024;
    if (age >= 800 && s3_face.gesture == FACE_LAUGH) {
        *nod += s3_face_wave(age - 800, 880) * 128 / 32768 * s3_face_ease(age - 800, 400) / 1024;
    }
    if (age <= 0 || age >= 800) return;
    int32_t envelope = s3_face_wave(age, 1600);
    switch (s3_face.gesture) {
    case FACE_NOD: *nod += envelope * 192 / 32768; break;
    case FACE_LAUGH: *nod += s3_face_wave(age, 400) * (int32_t)(800 - age) / 800 * 256 / 32768; break;
    case FACE_TILT: *tilt += envelope * 192 / 32768; break;
    case FACE_STARTLE: *nod -= envelope * 192 / 32768; break;
    default: break;
    }
    /* Rapidly replaced emotions must not accumulate unbounded head motion. */
    if (*nod > 512) *nod = 512;
    if (*nod < -512) *nod = -512;
    if (*tilt > 512) *tilt = 512;
    if (*tilt < -512) *tilt = -512;
}
static void s3_face_base(int64_t now, int32_t *pose)
{
    for (size_t i = 0; i < S3_FACE_POSE_VALUES; ++i) {
        /* Eyes lead; the mouth and secondary details follow by a few frames. */
        int delay = i >= 14 && i < 21 ? 70 : 0;
        int32_t t = s3_face_ease(now - s3_face.transition_at - delay, s3_face.transition_ms);
        pose[i] = s3_face_lerp(s3_face.from[i], s3_face.target[i], t);
    }
}
static void s3_face_set_target(const char *emotion, starter_ai_ui_phase_t phase, uint8_t activity)
{
    const char *key = emotion == NULL ? "neutral" : emotion;
    if (strcmp(key, "calm") == 0) key = "neutral";
    else if (strcmp(key, "excited") == 0) key = "laughing";
    else if (strcmp(key, "curious") == 0) key = "confused";
    else if (strcmp(key, "proud") == 0) key = "confident";
    else if (strcmp(key, "moved") == 0) key = "loving";
    uint16_t index = 0;
    for (size_t i = 0; i < sizeof(s3_face_poses) / sizeof(s3_face_poses[0]); ++i) {
        if (strcmp(key, s3_face_poses[i].key) == 0) { index = (uint16_t)i; break; }
    }
    int64_t now = monotonic_ms();
    bool phase_changed = s3_face.phase != phase;
    if (phase_changed) s3_face.gaze_next = now;
    s3_face.activity_target = activity > 3U ? 3U : activity;
    if (s3_face.valid && s3_face.pose == index && !phase_changed) return;
    bool mood_changed = !s3_face.valid || s3_face.pose != index;
    /* Retarget the current interpolated pose, never the previous endpoint.
     * New emotions replace this target; no stale-animation queue is built. */
    if (s3_face.valid) {
        s3_face_base(now, s3_face.from);
        s3_face_gesture(now, &s3_face.carry_nod, &s3_face.carry_tilt);
    }
    for (size_t i = 0; i < S3_FACE_POSE_VALUES; ++i) {
        s3_face.target[i] = s3_face_poses[index].value[i] * 16;
        /* Listening changes attention, not character identity. Keep a trace of
         * the current mood instead of swapping to an unrelated purple face. */
        if (phase == STARTER_AI_UI_LISTENING || phase == STARTER_AI_UI_THINKING)
            s3_face.target[i] = s3_face_lerp(s3_face_poses[0].value[i] * 16,
                                            s3_face.target[i], 256);
    }
    if (phase == STARTER_AI_UI_THINKING) s3_face.target[FACE_THOUGHT] = 140 * 256;
    s3_face.target[FACE_ATTENTION] = phase == STARTER_AI_UI_LISTENING ? 65536 : 0;
    s3_face.target[FACE_SPEECH] = phase == STARTER_AI_UI_SPEAKING ? 65536 : 0;
    if (!s3_face.valid) memcpy(s3_face.from, s3_face.target, sizeof(s3_face.from));
    s3_face.phase = phase;
    /* Phase/level updates must not replay a nod for every sentence boundary. */
    s3_face.gesture = mood_changed && phase != STARTER_AI_UI_LISTENING &&
                     phase != STARTER_AI_UI_THINKING ? s3_face_poses[index].gesture : FACE_STILL;
    s3_face.transition_at = now;
    s3_face.transition_ms = s3_face_poses[index].transition_ms;
    s3_face.pose = index;
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
static void s3_face_column(int x, int32_t top, int32_t bottom, int y0, int y1,
                           unsigned opacity, bool cutout)
{
    int a0 = top / 256, a1 = (bottom + 255) / 256;
    if (a0 < y0) a0 = y0;
    if (a1 > y1) a1 = y1;
    for (int y = a0; y <= a1; ++y) {
        int32_t a = top > y * 256 ? top : y * 256;
        int32_t b = bottom < (y + 1) * 256 ? bottom : (y + 1) * 256;
        if (b <= a) continue;
        unsigned coverage = ((unsigned)(b - a) * opacity + 2048U) / 4096U;
        if (coverage > 16U) coverage = 16U;
        lv_color_t colour = s3_face.palette[cutout ? 16U - coverage : coverage];
        lv_color_t *pixel = &s_face_canvas_buffer[y * FACE_CANVAS_W + x];
        /* Union coverage in RGB order, not the LCD's byte-swapped bus order.
         * Comparing swapped uint16 values can discard opaque fill while
         * accepting antialiased edges, turning solid faces into outlines. */
        uint16_t front = colour.full, back = pixel->full;
#if LV_COLOR_16_SWAP
        front = (uint16_t)((front >> 8) | (front << 8));
        back = (uint16_t)((back >> 8) | (back << 8));
#endif
        if (cutout || front > back) *pixel = colour;
    }
}
static void s3_face_shape_morph(const int32_t *p, unsigned opacity, unsigned heart)
{
    if (opacity == 0 || p[2] <= 0 || p[3] <= 0) return;
    if (opacity > 256) opacity = 256;
    int32_t rx = p[2] / 2, ry = p[3] / 2, radius = p[4];
    if (radius > rx) radius = rx;
    if (radius > ry) radius = ry;
    if (radius < 0) radius = 0;
    int x0 = (p[0] - rx) / 256 - 1, x1 = (p[0] + rx) / 256 + 1;
    int extent = (abs(p[5]) + abs(p[6])) / 256 + 2;
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
        int32_t top = centre - half, bottom = centre + half;
        if (heart) {
            /* Morph one opaque silhouette, rather than fading two overlapping
             * eyes. Cross-fading max-union coverage halves brightness midway. */
            int32_t ax = abs(nx), c = ax - 128;
            lv_sqrt_res_t root;
            lv_sqrt((uint32_t)(128 * 128 - c * c), &root, 0x8000);
            int32_t heart_top = centre - ry / 2 - ry * root.i / 256;
            int32_t heart_bottom = centre + ry - 3 * ry * ax / 512;
            top = s3_face_lerp(top, heart_top, (int32_t)heart * 4);
            bottom = s3_face_lerp(bottom, heart_bottom, (int32_t)heart * 4);
        }
        s3_face_column(x, top, bottom, y0, y1, opacity, false);
    }
}
static void s3_face_mark(int x, int y, int w, int h, int slope, int bend, unsigned opacity)
{
    int32_t p[7] = {x, y, w, h, h / 2, slope, bend};
    s3_face_shape_morph(p, opacity, 0);
}
static void s3_face_render(const int32_t *pose, int64_t now)
{
    uint32_t rgb = ((uint32_t)(pose[FACE_RED] / 256) << 16) |
                   ((uint32_t)(pose[FACE_GREEN] / 256) << 8) | (uint32_t)(pose[FACE_BLUE] / 256);
    if (!s3_face.painted_valid || rgb != s3_face.colour) {
        for (unsigned i = 0; i <= 16; ++i)
            s3_face.palette[i] = lv_color_mix(lv_color_hex(rgb), product_face_background_color(), (lv_opa_t)(i * 255U / 16U));
        s3_face.colour = rgb;
    }
    s3_face_clear();
    unsigned heart = (unsigned)pose[FACE_HEART] / 256;
    for (unsigned i = 0; i < 2; ++i)
        s3_face_shape_morph(pose + i * 7, 256, heart);
    s3_face_shape_morph(pose + 14, 256, 0);
    for (unsigned side = 0; side < 2; ++side) {
        const int32_t *eye = pose + side * 7;
        s3_face_mark(eye[0], eye[1] + 30 * 256, 16 * 256, 6 * 256,
                     0, 0, (unsigned)pose[FACE_BLUSH] / 256);
        int64_t fall = (now + side * 650) % 1600;
        unsigned fade = (unsigned)(fall < 250 ? fall : 1600 - fall);
        if (fade > 250) fade = 250;
        s3_face_mark(eye[0] + (side ? 19 : -19) * 256, 62 * 256 + (int32_t)fall * 23 * 256 / 1600,
                     6 * 256, 12 * 256, 0, 0, (unsigned)pose[FACE_TEARS] / 256 * fade / 250);
    }
    const int32_t *mouth = pose + 14;
    unsigned tongue = (unsigned)pose[FACE_TONGUE] / 256;
    /* Anchor the tongue to the moving lower lip, including its bend/tilt. */
    int32_t tx = 8 * 256, nx = tx * 512 / mouth[2];
    if (nx > 128) { nx = 128; tx = mouth[2] / 4; }
    int32_t lip = mouth[1] + mouth[3] / 2 + mouth[5] * nx / 256 +
                  mouth[6] * (nx * nx - 32768) / 65536;
    int32_t tongue_y = lip + 256;
    /* An opening mouth can already use most of the canvas height. Keep the
     * tongue joined to it without letting its rounded tip cross the canvas. */
    if (tongue_y > (FACE_CANVAS_H - 6) * 256) tongue_y = (FACE_CANVAS_H - 6) * 256;
    s3_face_mark(mouth[0] + tx, tongue_y,
                 10 * 256, 10 * 256, 0, 0, tongue);
    for (unsigned i = 0; i < 3; ++i) {
        int wave = s3_face_wave(now + i * 180, 1200);
        unsigned alpha = (unsigned)pose[FACE_THOUGHT] / 256 * (unsigned)(220 + wave * 35 / 32768) / 256;
        s3_face_mark((98 + (int)i * 12) * 256, 12 * 256 - wave * 2 * 256 / 32768,
                     6 * 256, 6 * 256, 0, 0, alpha);
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
    int32_t nod = 0, tilt = 0;
    s3_face_gesture(now, &nod, &tilt);
    int32_t breath = s3_face_wave(now, 4800) * 104 / 32768;
    for (unsigned i = 0; i < 3; ++i) {
        int32_t *part = pose + i * 7;
        part[0] += i == 2 ? gx / 3 : gx;
        part[1] += gy + breath + nod + (part[0] - 110 * 256) * tilt / (64 * 256);
        if (i < 2) {
            part[3] += s3_face.activity * pose[FACE_ATTENTION] / 65536;
            part[3] = 4 * 256 + (part[3] - 4 * 256) * blink / 1024;
            part[6] = part[6] * blink / 1024;
        }
    }
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
    s3_face.carry_nod = s3_face.carry_tilt = 0;
    s3_face_set_target("neutral", STARTER_AI_UI_IDLE, 0);
    if (s3_face.timer == NULL) {
        s3_face.timer = lv_timer_create(s3_face_frame, S3_FACE_PERIOD_MS, NULL);
        if (s3_face.timer == NULL) ESP_LOGE(TAG, "face animation timer allocation failed");
    }
    s3_face_visibility_changed(s_display_awake);
    s3_face_frame(NULL);
}
