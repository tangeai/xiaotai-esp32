/* Host-only checks of the actual product rasterizer. No ESP-IDF or device I/O. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "misc/lv_math.h"
#include "misc/lv_color.h"
#include "misc/lv_area.h"

#define EXT_RAM_BSS_ATTR
#define FACE_CANVAS_W 220
#define FACE_CANVAS_H 108
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef struct { bool paused, ready; } lv_timer_t;
typedef struct { int unused; } lv_obj_t;
typedef enum {
    STARTER_AI_UI_IDLE, STARTER_AI_UI_LISTENING,
    STARTER_AI_UI_THINKING, STARTER_AI_UI_SPEAKING
} starter_ai_ui_phase_t;
enum { PAGE_HOME_FACE, PAGE_EMOJI_PREVIEW, PAGE_OTHER };
static lv_color_t canvas[FACE_CANVAS_W * FACE_CANVAS_H];
static lv_color_t previous[FACE_CANVAS_W * FACE_CANVAS_H];
static lv_color_t rendered[FACE_CANVAS_W * FACE_CANVAS_H];
static lv_color_t *s_face_canvas_buffer = canvas;
static lv_obj_t object, *s_face = &object;
static bool s_display_awake = true;
static int s_page = PAGE_HOME_FACE;
static int64_t host_ms = 1000;
static unsigned timer_count, invalidated_count;
static lv_area_t invalidated[3];
static int64_t monotonic_ms(void) { return host_ms; }
static int64_t esp_timer_get_time(void) { return host_ms * 1000; }
static lv_color_t product_face_background_color(void)
{ return lv_color_hex(0x141719); }
static void lv_obj_get_coords(lv_obj_t *o, lv_area_t *a)
{ (void)o; memset(a, 0, sizeof(*a)); }
static void lv_obj_invalidate_area(lv_obj_t *o, const lv_area_t *a)
{
    (void)o;
    assert(invalidated_count < 3);
    invalidated[invalidated_count++] = *a;
}
static void lv_timer_resume(lv_timer_t *t) { t->paused = false; }
static void lv_timer_pause(lv_timer_t *t) { t->paused = true; }
static void lv_timer_ready(lv_timer_t *t) { t->ready = true; }
static lv_timer_t *lv_timer_create(void (*fn)(lv_timer_t *), uint32_t ms, void *u)
{
    static lv_timer_t t;
    (void)fn; (void)u;
    assert(ms == 40);
    ++timer_count;
    return &t;
}

/* Match Xtensa's register macro and ensure the renderer leaves it intact. */
#define BR 4
#include "starter_product_s3_face.inc"
_Static_assert(BR == 4, "Face helpers must preserve Xtensa register macros");
#undef BR

static void frame(void)
{
    memcpy(previous, canvas, sizeof(canvas));
    invalidated_count = 0;
    host_ms += 40;
    s3_face_frame(NULL);
    assert(!s3_face.clear_all);
    assert(s3_face.bound_count <= S3_FACE_BOUNDS);
    for (unsigned i = 0; i < s3_face.bound_count; ++i) {
        const s3_face_bounds_t *b = &s3_face.bounds[i];
        assert(b->x0 >= 0 && b->y0 >= 0);
        assert(b->x0 <= b->x1 && b->y0 <= b->y1);
        assert(b->x1 < FACE_CANVAS_W && b->y1 < FACE_CANVAS_H);
    }
    for (int y = 0; y < FACE_CANVAS_H; ++y) {
        for (int x = 0; x < FACE_CANVAS_W; ++x) {
            if (previous[y * FACE_CANVAS_W + x].full == canvas[y * FACE_CANVAS_W + x].full) continue;
            int sx = x, sy = y;
            bool covered = false;
            for (unsigned i = 0; i < invalidated_count; ++i)
                if (sx >= invalidated[i].x1 && sx <= invalidated[i].x2 &&
                    sy >= invalidated[i].y1 && sy <= invalidated[i].y2) covered = true;
            assert(covered);
        }
    }
    /* Partial clears must produce the same pixels as a fresh full canvas. */
    memcpy(rendered, canvas, sizeof(canvas));
    s3_face.clear_all = true;
    s3_face_render(s3_face.painted, host_ms);
    if (memcmp(rendered, canvas, sizeof(canvas)) != 0) {
        for (unsigned i = 0; i < sizeof(canvas) / sizeof(canvas[0]); ++i) {
            if (rendered[i].full == canvas[i].full) continue;
            fprintf(stderr, "damage mismatch: key=%s variant=%u phase=%u age=%lld x=%u y=%u partial=%04x full=%04x\n",
                    s3_face_poses[s3_face.pose].key, s3_face.variant, (unsigned)s3_face.phase,
                    (long long)(host_ms - s3_face.transition_at), i % FACE_CANVAS_W, i / FACE_CANVAS_W,
                    rendered[i].full, canvas[i].full);
            break;
        }
    }
    assert(memcmp(rendered, canvas, sizeof(canvas)) == 0);
}

static void settle(const char *key, starter_ai_ui_phase_t phase)
{
    s3_face_set_target(key, phase, 1);
    for (unsigned i = 0; i < 30; ++i) frame();
}

static void select_variant(const char *key, unsigned variant)
{
    assert(variant < s3_face_variant_count(s3_face_pose_index(key)));
    settle(strcmp(key, "neutral") == 0 ? "happy" : "neutral", STARTER_AI_UI_IDLE);
    /* Exercise the real selector with deterministic xorshift inputs. */
    s3_face.random = variant ? 1U : 2U;
    s3_face_set_target(key, STARTER_AI_UI_IDLE, 0);
    assert(s3_face.variant == variant);
}

static void check_b_design(void)
{
    const s3_face_pose_t *b = s3_face_alternate_poses;
    assert(b[s3_face_pose_index("laughing")].value[FACE_FOLD_LEFT] == 256 * 16);
    assert(b[s3_face_pose_index("laughing")].value[FACE_MOUTH_TWO_TONE] == 256 * 16);
    assert(b[s3_face_pose_index("crying")].value[FACE_MOUTH_WAVE] == 4 * 16);
    assert(b[s3_face_pose_index("angry")].value[FACE_STEAM] == 256 * 16);
    assert(b[s3_face_pose_index("kissy")].value[FACE_MOUTH_PUCKER] == 256 * 16);
    assert(b[s3_face_pose_index("sleepy")].value[FACE_ZZZ] == 256 * 16);
    const int16_t *confused = b[s3_face_pose_index("confused")].value;
    assert(confused[2] == confused[9] && confused[3] == confused[10]);

    select_variant("laughing", 1);
    for (unsigned i = 0; i < 90; ++i) frame();
    int x = s3_face.painted[0] / 256, y = s3_face.painted[1] / 256;
    lv_color_t bg = product_face_background_color();
    /* The outer rounded notch must not turn into a ring or a thin straight lid. */
    assert(canvas[y * FACE_CANVAS_W + x - 21].full == bg.full);
    assert(canvas[(y - 12) * FACE_CANVAS_W + x - 21].full != bg.full);
    assert(canvas[y * FACE_CANVAS_W + x + 16].full != bg.full);

    select_variant("kissy", 1);
    for (unsigned i = 0; i < 90; ++i) frame();
    x = s3_face.painted[14] / 256;
    y = s3_face.painted[15] / 256;
    assert(canvas[y * FACE_CANVAS_W + x].full != bg.full);
    assert(canvas[y * FACE_CANVAS_W + x + 9].full == bg.full);
    assert(canvas[(y + 5) * FACE_CANVAS_W + x + 9].full != bg.full);

    select_variant("cool", 1);
    for (unsigned i = 0; i < 90; ++i) frame();
    x = s3_face.painted[0] / 256;
    y = (s3_face.painted[1] - s3_face.painted[3] / 2) / 256 + 4;
    assert(canvas[y * FACE_CANVAS_W + x].full != bg.full);
    assert(canvas[y * FACE_CANVAS_W + x].full != lv_color_hex(0x344350).full);

    s3_face.random = 1U;
    s3_face_set_target("laughing", STARTER_AI_UI_LISTENING, 2);
    assert(s3_face.variant == 1);
    assert(s3_face.target[FACE_STYLE_B] == 65536);
    assert(s3_face.target[FACE_FOLD_LEFT] == 0 && s3_face.target[FACE_FOLD_RIGHT] == 0);
    assert(s3_face.target[FACE_MOUTH_TWO_TONE] == 0 && s3_face.target[FACE_MOUTH_PUCKER] == 0);
    for (unsigned i = 0; i < 90; ++i) frame();
    for (unsigned eye = 0; eye < 2; ++eye) {
        x = s3_face.painted[eye * 7] / 256;
        y = s3_face.painted[eye * 7 + 1] / 256;
        assert(canvas[y * FACE_CANVAS_W + x].full != bg.full);
    }
}

static void check_first_set_only(void)
{
    static const char *keys[] = {"thinking", "relaxed"};
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
        uint16_t index = s3_face_pose_index(keys[k]);
        assert(s3_face_variant_count(index) == 1);
        assert(s3_face_alternate_poses[index].key == NULL);
        assert(s3_face_sequences[index].variant[1].end_ms == 0);
        assert(s3_face_pose_for_variant(index, 1) == &s3_face_poses[index]);
        for (uint32_t seed = 1; seed <= 8; ++seed) {
            select_variant("happy", 1);
            s3_face.random = seed;
            s3_face_set_target(keys[k], STARTER_AI_UI_IDLE, 0);
            assert(s3_face.variant == 0 && s3_face.random == seed);
            for (unsigned i = 0; i < S3_FACE_POSE_VALUES; ++i)
                assert(s3_face.target[i] == s3_face_poses[index].value[i] * 16);
            int64_t at = s3_face.motion_at;
            s3_face_set_target(keys[k], STARTER_AI_UI_IDLE, 3);
            s3_face_set_target(keys[k], STARTER_AI_UI_SPEAKING, 2);
            assert(s3_face.variant == 0 && s3_face.motion_at == at && s3_face.random == seed);
        }
    }
    assert(s3_face_poses[s3_face_pose_index("relaxed")].value[FACE_MUSIC] == 256 * 16);

    select_variant("cool", 1);
    s3_face.random = 1U; /* Would select B for a two-set expression. */
    s3_face_set_target("laughing", STARTER_AI_UI_THINKING, 1);
    assert(s3_face.motion_pose == s3_face_pose_index("thinking") && s3_face.variant == 0);
    assert(s3_face.random == 1U && s3_face.target[FACE_STYLE_B] == 0);
    int64_t at = s3_face.motion_at;
    s3_face_set_target("angry", STARTER_AI_UI_THINKING, 3);
    assert(s3_face.variant == 0 && s3_face.motion_at == at);
    assert(s3_face.target[FACE_STEAM] == 0 && s3_face.target[FACE_THOUGHT] == 65536);

    /* A-only moods can still accompany B listening without reading empty slots. */
    s3_face.random = 1U;
    s3_face_set_target("thinking", STARTER_AI_UI_LISTENING, 1);
    assert(s3_face.variant == 1 && s3_face.target[FACE_STYLE_B] == 65536);
    const s3_face_pose_t *listening = &s3_face_alternate_poses[s3_face_pose_index("listening")];
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
        s3_face_set_target(keys[k], STARTER_AI_UI_LISTENING, 1);
        const s3_face_pose_t *mood = &s3_face_poses[s3_face_pose_index(keys[k])];
        for (unsigned i = 0; i < FACE_TEARS; ++i)
            assert(s3_face.target[i] == s3_face_lerp(listening->value[i] * 16, mood->value[i] * 16, 256));
        assert(s3_face.variant == 1 && s3_face.target[FACE_LISTEN] == 65536);
        assert(s3_face.target[FACE_THOUGHT] == 0 && s3_face.target[FACE_MUSIC] == 0);
        for (unsigned frame_index = 0; frame_index < 30; ++frame_index) frame();
    }
}

static void check_blink_transition(void)
{
    static const char *destinations[] = {"laughing", "crying", "happy"};
    for (unsigned k = 0; k < sizeof(destinations) / sizeof(destinations[0]); ++k) {
        select_variant("neutral", k == 2 ? 1 : 0);
        for (unsigned i = 0; i < 90; ++i) frame();
        s3_face.random = 1;
        s3_face_set_target(destinations[k], STARTER_AI_UI_IDLE, 0);
        int64_t start = host_ms;
        int32_t previous_height[2] = {0, 0};
        /* Hold the natural blink closed while crossing every morph threshold. */
        for (unsigned age = 1; age <= 900; ++age) {
            host_ms = start + age;
            s3_face.blink_at = host_ms - 100;
            invalidated_count = 0;
            s3_face_frame(NULL);
            for (unsigned eye = 0; eye < 2; ++eye) {
                int32_t height = s3_face.painted[eye * 7 + 3];
                if (age > 1) {
                    int32_t step = abs(height - previous_height[eye]);
                    if (step > 256)
                        fprintf(stderr, "blink jump: %s age=%u eye=%u step_q8=%d\n",
                                destinations[k], age, eye, (int)step);
                    assert(step <= 256);
                }
                previous_height[eye] = height;
            }
        }
        s3_face.blink_at = host_ms + 2600;
    }
}

static void check_cue_reentry(void)
{
    static const struct { const char *key; unsigned channel; } cases[] = {
        {"winking", FACE_WINK}, {"cool", FACE_GLASSES}, {"crying", FACE_TEARS},
        {"loving", FACE_HEART}, {"kissy", FACE_KISS}, {"angry", FACE_STEAM},
        {"relaxed", FACE_MUSIC},
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        unsigned count = s3_face_variant_count(s3_face_pose_index(cases[k].key));
        for (unsigned v = 0; v < count; ++v) {
            const s3_face_pose_t *pose = s3_face_pose_for_variant(s3_face_pose_index(cases[k].key), v);
            if (!pose->value[cases[k].channel]) continue;
            select_variant(cases[k].key, v);
            for (unsigned i = 0; i < 90; ++i) frame();
            s3_face_set_target("neutral", STARTER_AI_UI_IDLE, 0);
            for (unsigned i = 0; i < 5; ++i) frame();
            s3_face.random = v ? 1 : 2;
            s3_face_set_target(cases[k].key, STARTER_AI_UI_IDLE, 0);
            assert(s3_face.from[cases[k].channel] > 0);
            int64_t *cue = &s3_face.cue_at[cases[k].channel - FACE_TEARS];
            if (*cue != host_ms)
                fprintf(stderr, "stale cue: %s variant=%u age_ms=%lld\n",
                        cases[k].key, v, (long long)(host_ms - *cue));
            assert(*cue == host_ms && s3_face.motion_at == host_ms);
            int64_t entry = host_ms;
            uint32_t random = s3_face.random;
            host_ms += 40;
            s3_face_set_target(cases[k].key, STARTER_AI_UI_IDLE, 3);
            s3_face_set_target(cases[k].key, STARTER_AI_UI_SPEAKING, 2);
            assert(*cue == entry && s3_face.motion_at == entry && s3_face.random == random);
            s3_face_set_target(cases[k].key, STARTER_AI_UI_IDLE, 0);
            if (cases[k].channel == FACE_WINK) {
                for (unsigned i = 0; i < 14; ++i) frame();
                assert(s3_face.painted[10] == (v ? 24 : 14) * 256);
            }
        }
    }
}

static void check_visibility(void)
{
    settle("ambient", STARTER_AI_UI_IDLE);
    lv_timer_t *timer = s3_face.timer;
    unsigned variants = s3_face.variant;
    for (unsigned cycle = 0; cycle < 12; ++cycle) {
        assert(s3_face.attached && !timer->paused);
        uint32_t frames = s3_face.frames;
        memcpy(previous, canvas, sizeof(canvas));
        s_display_awake = false;
        s3_face_visibility_changed(false);
        assert(timer->paused);
        invalidated_count = 0;
        host_ms += 8000;
        /* Still attached: this exercises the sleep guard independently. */
        s3_face_frame(NULL);
        assert(s3_face.frames == frames && invalidated_count == 0);
        assert(memcmp(previous, canvas, sizeof(canvas)) == 0);
        s_display_awake = true;
        timer->ready = false;
        s3_face_visibility_changed(true);
        assert(!timer->paused && timer->ready);
        frame();
        assert(s3_face.frames == frames + 1 && s3_face.variant == variants);
        assert(s3_face.timer == timer && timer_count == 1);
    }
    uint32_t frames = s3_face.frames;
    s_page = PAGE_OTHER;
    invalidated_count = 0;
    s3_face_frame(NULL);
    assert(s3_face.frames == frames && invalidated_count == 0);
    s_page = PAGE_HOME_FACE;
    s3_face_detach();
    assert(timer->paused);
    s3_face_frame(NULL);
    assert(s3_face.frames == frames);
    invalidated_count = 0;
    /* create_expression_face clears the reused canvas before attaching it. */
    for (size_t i = 0; i < sizeof(canvas) / sizeof(canvas[0]); ++i)
        canvas[i] = product_face_background_color();
    s3_face_attach();
    assert(s3_face.timer == timer && timer_count == 1 && !timer->paused);
    frame();
}

int main(void)
{
    const size_t count = sizeof(s3_face_poses) / sizeof(s3_face_poses[0]);
    assert(count == 23);
    assert(S3_FACE_VARIANTS == 2);
    unsigned enabled_variants = 0;
    for (size_t i = 0; i < count; ++i) {
        assert(strcmp(s3_face_poses[i].key, "speech") != 0);
        unsigned variants = s3_face_variant_count((uint16_t)i);
        bool first_only = strcmp(s3_face_poses[i].key, "thinking") == 0 ||
                          strcmp(s3_face_poses[i].key, "relaxed") == 0;
        assert(variants == (first_only ? 1U : 2U));
        enabled_variants += variants;
        if (variants > 1)
            assert(strcmp(s3_face_poses[i].key, s3_face_alternate_poses[i].key) == 0);
        else assert(s3_face_sequences[i].variant[1].end_ms == 0);
        assert(strcmp(s3_face_poses[i].key, s3_face_sequences[i].key) == 0);
        assert(memcmp(s3_face_poses[i].value, s3_face_alternate_poses[i].value,
                      sizeof(s3_face_poses[i].value)) != 0);
        for (unsigned v = 0; v < s3_face_variant_count((uint16_t)i); ++v) {
            const s3_face_pose_t *set = v ? s3_face_alternate_poses : s3_face_poses;
            for (unsigned eye = 0; eye < 2; ++eye) {
                const int16_t *p = set[i].value + eye * 7;
                assert(p[2] >= 62 * 16 && p[3] >= (v ? 24 : 20) * 16);
                assert(abs(p[5]) <= (v ? 5 : 4) * 16 && abs(p[6]) <= (v ? 10 : 3) * 16);
                assert(set[i].value[FACE_STYLE_B] == (v ? 256 * 16 : 0));
            }
            const s3_face_track_t *track = &s3_face_sequences[i].variant[v];
            uint16_t previous_ms = 0;
            for (unsigned k = 0; k < S3_FACE_MOTION_STEPS; ++k) {
                assert(track->frame[k].at_ms > previous_ms);
                previous_ms = track->frame[k].at_ms;
                for (unsigned j = 0; j < S3_FACE_MOTION_VALUES; ++j)
                    assert(abs(track->frame[k].value[j]) <= 3);
            }
            assert(track->end_ms > previous_ms && track->end_ms <= 3000);
        }
    }
    assert(enabled_variants == 44);
    for (size_t i = 0; i < sizeof(canvas) / sizeof(canvas[0]); ++i)
        canvas[i] = product_face_background_color();
    s3_face_attach();

    settle("speech", STARTER_AI_UI_IDLE);
    assert(strcmp(s3_face_poses[s3_face.pose].key, "listening") == 0);
    assert(s3_face.target[FACE_LISTEN] == 65536);
    for (unsigned v = 0; v < S3_FACE_VARIANTS; ++v) {
        select_variant("cool", v);
        for (unsigned i = 0; i < 30; ++i) frame();
        /* B lenses sit lower; sample inside each lens, away from the glint. */
        for (unsigned eye = 0; eye < 2; ++eye) {
            int x = s3_face.painted[eye * 7] / 256;
            int y = s3_face.painted[eye * 7 + 1] / 256 + (v ? 20 : 0) + 2;
            assert(canvas[y * FACE_CANVAS_W + x].full == lv_color_hex(0x344350).full);
        }
    }
    settle("cool", STARTER_AI_UI_LISTENING);
    assert(s3_face.target[FACE_GLASSES] == 0);
    assert(s3_face.target[FACE_LISTEN] == 65536);
    settle("relaxed", STARTER_AI_UI_THINKING);
    assert(s3_face.target[FACE_MUSIC] == 0 && s3_face.target[FACE_THOUGHT] == 65536);

    for (unsigned v = 0; v < S3_FACE_VARIANTS; ++v) {
        select_variant("winking", v);
        for (int i = 0; i < 15; ++i) frame();
        assert(s3_face.painted[10] == (v ? 24 : 14) * 256);
        assert(s3_face.painted[3] > 35 * 256);
        int64_t at = s3_face.transition_at, cue = s3_face.cue_at[FACE_WINK - FACE_TEARS];
        int64_t motion_at = s3_face.motion_at;
        uint32_t random = s3_face.random;
        s3_face_set_target("winking", STARTER_AI_UI_IDLE, 3);
        assert(s3_face.transition_at == at && s3_face.cue_at[FACE_WINK - FACE_TEARS] == cue);
        assert(s3_face.motion_at == motion_at && s3_face.variant == v && s3_face.random == random);
        for (int i = 0; i < 16; ++i) frame();
        assert(s3_face.painted[3] > 35 * 256);
        if (v) assert(s3_face.painted[10] >= 24 * 256 && s3_face.painted[10] <= 30 * 256);
        else assert(s3_face.painted[10] > 35 * 256);
    }

    select_variant("laughing", 1);
    for (unsigned i = 0; i < 15; ++i) frame();
    int64_t motion_at = s3_face.motion_at;
    uint32_t random = s3_face.random;
    int32_t before_phase[S3_FACE_POSE_VALUES], after_phase[S3_FACE_POSE_VALUES];
    s3_face_base(host_ms, before_phase);
    s3_face_set_target("excited", STARTER_AI_UI_SPEAKING, 2);
    s3_face_base(host_ms, after_phase);
    assert(memcmp(before_phase, after_phase, sizeof(before_phase)) == 0);
    assert(s3_face.variant == 1 && s3_face.motion_at == motion_at && s3_face.random == random);
    s3_face_set_target("laughing", STARTER_AI_UI_IDLE, 0);
    assert(s3_face.variant == 1 && s3_face.motion_at == motion_at && s3_face.random == random);

    s3_face_set_target("cool", STARTER_AI_UI_LISTENING, 1);
    assert(s3_face.motion_pose == s3_face_pose_index("listening"));
    motion_at = s3_face.motion_at;
    random = s3_face.random;
    unsigned variant = s3_face.variant;
    s3_face_set_target("angry", STARTER_AI_UI_LISTENING, 3);
    assert(s3_face.motion_at == motion_at && s3_face.variant == variant && s3_face.random == random);
    assert(s3_face.target[FACE_GLASSES] == 0 && s3_face.target[FACE_LISTEN] == 65536);
    s3_face_set_target("cool", STARTER_AI_UI_THINKING, 1);
    assert(s3_face.motion_pose == s3_face_pose_index("thinking"));
    assert(s3_face.target[FACE_GLASSES] == 0 && s3_face.target[FACE_THOUGHT] == 65536);

    s3_face_set_target("happy", STARTER_AI_UI_IDLE, 0);
    for (int i = 0; i < 5; ++i) frame();
    int32_t midpoint[S3_FACE_POSE_VALUES];
    s3_face_base(host_ms, midpoint);
    s3_face_set_target("sad", STARTER_AI_UI_IDLE, 0);
    assert(memcmp(s3_face.from, midpoint, sizeof(midpoint)) == 0);

    /* Exercise all 44 enabled choices, including recovery, later blinks and quiet cues. */
    unsigned selected_mask = 0;
    for (size_t i = 0; i < count; ++i) {
        for (unsigned v = 0; v < s3_face_variant_count((uint16_t)i); ++v) {
            select_variant(s3_face_poses[i].key, v);
            selected_mask |= 1U << s3_face.variant;
            int32_t offset[S3_FACE_MOTION_VALUES];
            s3_face_motion(host_ms, offset);
            for (unsigned j = 0; j < S3_FACE_MOTION_VALUES; ++j) assert(offset[j] == 0);
            for (int f = 0; f < 230; ++f) frame();
            assert(s3_face.variant == v);
            s3_face_motion(host_ms, offset);
            for (unsigned j = 0; j < S3_FACE_MOTION_VALUES; ++j) assert(offset[j] == 0);
            assert(canvas[86 * FACE_CANVAS_W + 110].full != product_face_background_color().full);
        }
    }
    assert(selected_mask == 3);
    check_b_design();
    check_first_set_only();
    check_blink_transition();
    check_cue_reentry();
    /* Rapid replacement stresses fading, layered accents and the bounds pool. */
    for (unsigned i = 0; i < 230; ++i) {
        s3_face_set_target(s3_face_poses[i % count].key, STARTER_AI_UI_IDLE, i % 4);
        frame();
    }
    for (unsigned phase = STARTER_AI_UI_LISTENING; phase <= STARTER_AI_UI_SPEAKING; ++phase)
        for (size_t i = 0; i < count; ++i) settle(s3_face_poses[i].key, (starter_ai_ui_phase_t)phase);
    settle("ambient", STARTER_AI_UI_IDLE);
    assert(s3_face.target[FACE_MUSIC] == 0 && s3_face.target[FACE_ZZZ] == 0);
    assert(s3_face.target[FACE_LISTEN] == 0 && s3_face.target[FACE_THOUGHT] == 0);

    check_visibility();
    puts("PASS: 44 face variants, A-only thinking/relaxed, B contours, phases, clips, damage, smooth blinks, cue reentry and sleep/wake (host only)");
    return 0;
}
