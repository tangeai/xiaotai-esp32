// SPDX-License-Identifier: MIT
/* theme_character_actor: the character on a page (src/chr_actor.c) as the kit drives it: shown and hidden,
 * on a page that comes and goes, through a dialog, a sleep and a waking, and on the page that shows only an
 * expression. Nothing is drawn here: the display is LVGL's alone, and what she is doing is read where the
 * actor itself reads it, after every step. The clock is the test's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "chr.h"
#include "chr_director.h"
#include "xiaotai/port/sys.h"
#include "xiaotai/theme_character.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai/ui_theme.h"
#include "xiaotai/view.h"

static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

enum { W = 1024, H = 600, FRAME = 24 };
static const xiaotai_theme_lcd_actor_t *const actor = &xiaotai_theme_character_actor;
static const xiaotai_display_desc_t panel = {.width = W, .height = H, .pixel = XIAOTAI_PIXEL_RGB565};
static lv_display_t *display;
static lv_obj_t *widget;
static void *state;
static uint32_t now_ms = 10000;
static chr_info_t she;
static unsigned returns;
static bool return_shows;

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return (int64_t)now_ms * 1000; }
const xiaotai_display_desc_t *__wrap_xiaotai_ui_host_display(const xiaotai_ui_host_t *host);
const xiaotai_display_desc_t *__wrap_xiaotai_ui_host_display(const xiaotai_ui_host_t *host) { (void)host; return &panel; }
void *__wrap_xiaotai_ui_host_root(const xiaotai_ui_host_t *host);
void *__wrap_xiaotai_ui_host_root(const xiaotai_ui_host_t *host) { (void)host; return display; }
xiaotai_err_t __wrap_xiaotai_ui_host_painter(const xiaotai_ui_host_t *host, void *painted, xiaotai_ui_paint_fn_t paint, void *ctx);
xiaotai_err_t __wrap_xiaotai_ui_host_painter(const xiaotai_ui_host_t *host, void *painted, xiaotai_ui_paint_fn_t paint, void *ctx)
{
    (void)host;
    (void)ctx;
    return painted && paint ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_ARG;
}
void __real_chr_inspect(const chr_t *c, chr_info_t *out);
void __wrap_chr_inspect(const chr_t *c, chr_info_t *out);
void __wrap_chr_inspect(const chr_t *c, chr_info_t *out)
{
    __real_chr_inspect(c, out);
    she = *out;
}
void __real_chr_return(chr_t *c, bool show);
void __wrap_chr_return(chr_t *c, bool show);
void __wrap_chr_return(chr_t *c, bool show)
{
    ++returns;
    return_shows = show;
    __real_chr_return(c, show);
}

/* Some time with her on the screen, or behind something. */
static void on(unsigned ms)
{
    for (unsigned t = 0; t < ms; t += FRAME) {
        now_ms += FRAME;
        (void)actor->tick(state, now_ms, true);
    }
}

static void hidden(unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 100) {
        now_ms += 100;
        (void)actor->tick(state, now_ms, false);
    }
}

static void page(bool raised)
{
    const xiaotai_theme_lcd_stage_t stage = {.width = W, .height = H, .raised = raised};
    widget = actor->build(state, lv_screen_active(), &stage);
    CHECK(widget != NULL);
}

static void leave(void)
{
    actor->detach(state);
    lv_obj_delete(widget);
    widget = NULL;
}

static void view(const char *emotion, uint8_t phase)
{
    actor->set(state, &(xiaotai_view_t){.kind = XIAOTAI_VIEW_HOME, .id = "home", .emotion = emotion, .phase = phase}, now_ms);
}

static unsigned late(void)
{
    char line[512] = "";
    actor->report(state, line, sizeof(line));
    const char *at = strstr(line, "late=");
    return at ? (unsigned)strtoul(at + 5, NULL, 10) : 999U;
}

/* The first page she is on: she straightens up and waves, whatever else has happened before it. */
static void test_first(void)
{
    chr_director_handle("system.started", 0, "");
    chr_director_cue(CHR_CUE_NOD, 0);
    now_ms += 60000;
    view("neutral", XIAOTAI_TALK_IDLE);
    page(false);
    float most = 0.0f;
    bool waved = false;
    for (int i = 0; i < 100; ++i) {
        on(FRAME);
        if (i < 40) CHECK(she.cue == CHR_CUE_GREET);
        most = she.pose[P_BY] > most ? she.pose[P_BY] : most;
        waved |= she.gesture == CHR_GESTURE_WAVE;
    }
    CHECK(waved && most > 0.5f);
    leave();
}

/* Away for more than a second she is as the page says when she is back; in a moment nothing has changed. */
static void test_away(void)
{
    view("neutral", XIAOTAI_TALK_IDLE);
    page(false);
    on(1000);
    /* A talk on her page, and the settings over it, in which the talk ends. */
    view("happy", XIAOTAI_TALK_SPEAKING);
    on(2000);
    CHECK(she.phase == CHR_PHASE_SPEAKING && she.mood == chr_mood_find("happy"));
    leave();
    now_ms += 3600000;
    view("neutral", XIAOTAI_TALK_IDLE);
    returns = 0;
    page(false);
    on(FRAME);
    CHECK(returns == 1 && !return_shows && she.phase == CHR_PHASE_IDLE && she.mood == CHR_MOOD_NEUTRAL && !she.parting && she.cue == CHR_CUE_NONE);
    for (int i = 0; i < 150; ++i) {
        on(FRAME);
        CHECK(she.cue != CHR_CUE_BYE && !she.parting && she.mouth_open == 0);
    }
    CHECK(returns == 1);
    /* A dialog over her, shorter and longer than a moment. */
    view("neutral", XIAOTAI_TALK_LISTENING);
    on(2000);
    hidden(700);
    on(FRAME);
    CHECK(returns == 1);
    hidden(1500);
    on(FRAME);
    CHECK(returns == 2 && she.phase == CHR_PHASE_LISTENING);
    /* A talk that ends on her page has its goodbye. */
    view("neutral", XIAOTAI_TALK_IDLE);
    bool waved = false;
    for (int i = 0; i < 100; ++i) {
        on(FRAME);
        waved |= she.cue == CHR_CUE_BYE;
    }
    CHECK(waved && returns == 2);
    /* A reaction waits for her for a moment, not for an hour. */
    chr_director_cue(CHR_CUE_NOD, 0);
    hidden(2000);
    on(FRAME);
    CHECK(returns == 3 && she.cue == CHR_CUE_NOD);
    on(3000);
    hidden(1500);
    chr_director_cue(CHR_CUE_NOD, 0);
    hidden(8000);
    on(FRAME);
    CHECK(she.cue == CHR_CUE_NONE);
    leave();
}

/* A talk over the clock, which has the conversation on it and not her; the service is lost and found again
 * meanwhile. Then her page, at once or after a while: nothing of it is played. */
static void test_elsewhere(void)
{
    for (int later = 0; later < 2; ++later) {
        view("neutral", XIAOTAI_TALK_IDLE);
        page(false);
        on(2000);
        leave();
        hidden(5000);
        static const struct { int32_t state; uint8_t phase; } steps[] = {
            {CHR_DIRECTOR_TALK_CONNECTING, XIAOTAI_TALK_IDLE}, {2, XIAOTAI_TALK_LISTENING}, {3, XIAOTAI_TALK_THINKING},
            {4, XIAOTAI_TALK_SPEAKING}, {CHR_DIRECTOR_TALK_ENDING, XIAOTAI_TALK_IDLE}, {0, XIAOTAI_TALK_IDLE},
        };
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
            chr_director_handle("ai_talk.state", steps[i].state, "");
            if (steps[i].state == 4) {
                chr_director_handle("platform.offline", 0, "");
                hidden(1000);
                chr_director_handle("platform.online", 0, "");
            }
            hidden(steps[i].state ? 1500 : later ? 20000 : 0);
        }
        view("neutral", XIAOTAI_TALK_IDLE);
        page(false);
        for (int i = 0; i < 100; ++i) {
            on(FRAME);
            CHECK(she.cue == CHR_CUE_NONE && she.gesture == CHR_GESTURE_NONE && she.mouth_open == 0 && !she.parting &&
                  she.particles == 0 && she.mood == CHR_MOOD_NEUTRAL);
        }
        leave();
    }
}

/* A screen that sleeps is not ticked. Woken after a real sleep she stretches; unless something has happened,
 * or the screen woke for another page. */
static void test_sleep(void)
{
    view("neutral", XIAOTAI_TALK_IDLE);
    page(false);
    on(2000);
    const unsigned before = late();
    actor->sleep(state, true);
    now_ms += 300000;
    actor->sleep(state, false);
    on(FRAME);
    CHECK(she.cue == CHR_CUE_STRETCH && late() == before);
    on(4000);
    CHECK(she.cue == CHR_CUE_NONE && !she.dozing && she.mood == CHR_MOOD_NEUTRAL);
    /* She leaves what she was doing: it is over. */
    chr_director_cue(CHR_CUE_DANCE, 0);
    on(1500);
    CHECK(she.cue == CHR_CUE_DANCE);
    actor->sleep(state, true);
    now_ms += 7200000;
    actor->sleep(state, false);
    on(FRAME);
    CHECK(she.cue == CHR_CUE_STRETCH && she.gesture != CHR_GESTURE_DANCE && she.shown == 0);
    on(4000);
    /* An alarm rings into the dark, and is stopped: she nods. */
    actor->sleep(state, true);
    now_ms += 300000;
    leave();
    actor->sleep(state, false);
    now_ms += 2000;
    chr_director_cue(CHR_CUE_NOD, 0);
    page(false);
    on(FRAME);
    CHECK(she.cue == CHR_CUE_NOD);
    on(4000);
    CHECK(she.cue == CHR_CUE_NONE);
    /* The screen woke for another page, some time before hers. */
    actor->sleep(state, true);
    now_ms += 300000;
    leave();
    actor->sleep(state, false);
    now_ms += 20000;
    page(false);
    on(FRAME);
    CHECK(she.cue == CHR_CUE_NONE);
    on(500);
    CHECK(she.cue == CHR_CUE_NONE);
    /* Nor when that was so long ago that the difference of the two times has turned round. */
    actor->sleep(state, true);
    now_ms += 300000;
    leave();
    actor->sleep(state, false);
    now_ms += 0x90000000U;
    page(false);
    on(FRAME);
    CHECK(she.cue == CHR_CUE_NONE);
    /* Dark for a moment is no sleep. */
    actor->sleep(state, true);
    now_ms += 800;
    actor->sleep(state, false);
    on(FRAME);
    CHECK(she.cue == CHR_CUE_NONE);
    leave();
}

/* The page that shows an expression shows that, whatever the device is in; the bench shows what it asks. */
static void test_preview(void)
{
    view("happy", XIAOTAI_TALK_IDLE);
    chr_director_set(CHR_OFFLINE | CHR_MUTED, true);
    page(false);
    on(1000);
    CHECK(she.conditions == (CHR_OFFLINE | CHR_MUTED));
    leave();
    /* From a list of them, on which she is not: she makes a show of the one that is chosen. */
    now_ms += 5000;
    view("angry", XIAOTAI_TALK_IDLE);
    page(true);
    on(600);
    CHECK(return_shows && she.conditions == 0 && she.mood == chr_mood_find("angry") && she.gesture == CHR_GESTURE_SULK);
    const xiaotai_theme_lcd_stage_t stage = {.width = W, .height = H};
    actor->place(state, &stage);
    on(FRAME);
    CHECK(she.conditions == (CHR_OFFLINE | CHR_MUTED));
    chr_director_set(~0U, false);
    leave();
}

/* Behind a dialog for six seconds of her answer (nothing of it is heard here: it is taken as begun a second after its
 * first sentence came). When she is back the sentences heard meanwhile are over: the last of them shows, not each in
 * turn, and the next shows at its time. What was told of a talk that ended while she was behind it is not hers. */
static void test_hidden_answer(void)
{
    const size_t comfort = chr_mood_find("comfort"), serious = chr_mood_find("serious"), happy = chr_mood_find("happy"),
                 gentle = chr_mood_find("gentle");
    view("neutral", XIAOTAI_TALK_IDLE);
    page(false);
    on(1000);
    chr_director_handle("ai_talk.state", 3, "");
    view("neutral", XIAOTAI_TALK_THINKING);
    on(1000);
    chr_director_handle("ai_talk.state", 4, "");
    view("neutral", XIAOTAI_TALK_SPEAKING);
    chr_director_handle("companion.sentence", 0, "comfort");
    chr_director_handle("companion.sentence", 2000, "serious");
    const uint32_t began = now_ms;     /* taken at the next tick, before its step: as of this one */
    on(1500);
    CHECK(she.mood == comfort);
    hidden(3000);
    chr_director_handle("companion.sentence", 4000, "happy");
    chr_director_handle("companion.sentence", 9000, "gentle");
    hidden(3000);
    bool flipped = false;
    for (int i = 0; i < 3; ++i) {
        on(FRAME);
        flipped |= she.mood == serious;
    }
    CHECK(she.mood == happy && !flipped);
    uint32_t gentle_at = 0;
    while (!gentle_at && now_ms - began < 12000) {
        on(FRAME);
        if (she.mood == gentle) gentle_at = now_ms - began;
    }
    CHECK(gentle_at >= 8900 && gentle_at <= 8900 + 2U * FRAME);
    /* The talk ends while she is behind something, and another begins. */
    hidden(500);
    chr_director_handle("companion.sentence", 0, "serious");
    chr_director_handle("ai_talk.state", 0, "");
    chr_director_handle("ai_talk.state", 2, "");
    view("neutral", XIAOTAI_TALK_LISTENING);
    hidden(2000);
    for (int i = 0; i < 100; ++i) {
        on(FRAME);
        CHECK(she.mood != serious);
    }
    chr_director_handle("ai_talk.state", 0, "");
    view("neutral", XIAOTAI_TALK_IDLE);
    on(3000);
    leave();
}

int main(void)
{
    lv_init();
    display = lv_display_create(W, H);
    static uint8_t buffer[W * 2 * 20];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    CHECK(actor->match(&panel) && actor->start((const xiaotai_ui_host_t *)&panel, &state) == XIAOTAI_OK && state);
    if (!state) return EXIT_FAILURE;
    test_first();
    test_away();
    test_elsewhere();
    test_sleep();
    test_preview();
    test_hidden_answer();
    actor->stop(state);
    lv_display_delete(display);
    lv_deinit();
    if (failures) {
        fprintf(stderr, "FAIL: theme_character_actor: %d checks failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS: theme_character_actor\n");
    return EXIT_SUCCESS;
}
