// SPDX-License-Identifier: MIT
/* The character as an actor of the colour theme kit (xiaotai/theme_lcd.h): one
 * image widget as large as the page, painted straight into the frame being
 * rendered. Each tick it reads what the device and the finger are doing,
 * steps the character and invalidates what changed. */
#include "xiaotai/theme_character.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "chr.h"
#include "chr_director.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"
#include "xiaotai/theme_lcd.h"
#include "xiaotai/ui_config.h"

/* A step is due a little before the next refresh of a 40 Hz panel: a light frame is then shown on
 * the very next one, a heavy frame on the one after. */
enum { FRAME_MS = 24, DOZE_MS = 3000, AWAY_MS = 1000 };
/* Painting is shared with a second core when the part to redraw has at least this many rows and
 * pixels: below that, waking the other core costs more than it saves. It is cut into bands of
 * BAND_ROWS rows. */
enum { SHARE_ROWS = 48, SHARE_PIXELS = 12000, BAND_ROWS = 16, HELPER_STACK = 4096 };

/* What must be redrawn is cut into bands of rows, and the UI task and a helper on the other core
 * each take the next band that nobody has taken, until none is left. Whoever has time paints: the
 * other core also handles sound and the wake word, which run before the helper, so on a busy
 * moment the UI task simply paints most bands itself and never waits for more than the band the
 * helper is in. (Handing the helper a fixed half was measured: the frame then waits for a helper
 * that gets little of its core, and painting took twice as long as on one core.)
 *
 * Bands never share a row, the list they draw from does not change while a frame is painted, and
 * each painter has its own scratch row. */
enum { JOB_NONE = 0, JOB_POSTED, JOB_TAKEN };

typedef struct {
    xiaotai_task_t task;
    xiaotai_signal_t wake, done;
    int16_t *scratch;
    bool running, quit;
    /* One frame's job: written by the UI task before it is posted. */
    const chr_t *chr;
    chr_target_t stage;
    int bands;
    atomic_int state, next;
    atomic_uint pixels, helped;
} helper_t;

typedef struct {
    const xiaotai_ui_host_t *host;
    lv_display_t *display;
    chr_t *chr;
    int16_t *scratch;
    lv_image_dsc_t source;       /* a blank picture as large as the page: the painter draws instead */
    lv_obj_t *widget;
    uint16_t width, height;
    uint32_t last_ms, slept_ms, woke_ms;
    size_t mood;
    uint8_t phase, level;
    bool shown, pressed, asleep, woke, raised, stepped;
    helper_t helper;
    /* Counters for diagnostics: one writer (the UI task), plain stores. */
    atomic_uint frames, quiet, late, step_max_us, paint_max_us, pixels, paints, shared, paint_total_us,
                mouth, hear, shut, shut_max, wait, lag;
    atomic_int moved[6];         /* least and most since the last reading: sideways, feet, top */
} actor_t;

static void count(atomic_uint *counter, unsigned value) { atomic_store_explicit(counter, value, memory_order_relaxed); }
static unsigned get(const atomic_uint *counter) { return atomic_load_explicit(counter, memory_order_relaxed); }

static int match(const xiaotai_display_desc_t *display)
{
    /* The sprites are baked for a landscape page about 600 pixels high. */
    if (!display || display->pixel != XIAOTAI_PIXEL_RGB565 || display->width < 800 || display->height < 560 ||
        display->height > 720 || display->width <= display->height || (display->flags & XIAOTAI_DISPLAY_SLOW_REFRESH))
        return 0;
    return 80;
}

/* Paints bands until none is left; returns how many this caller painted. */
static unsigned paint_bands(helper_t *h, int16_t *scratch)
{
    unsigned painted = 0;
    for (;;) {
        const int band = atomic_fetch_add(&h->next, 1);
        if (band >= h->bands) return painted;
        chr_target_t part = h->stage;
        part.y0 = (int16_t)(h->stage.y0 + band * BAND_ROWS);
        if (band + 1 < h->bands) part.y1 = (int16_t)(part.y0 + BAND_ROWS - 1);
        part.pixels = h->stage.pixels + (size_t)(band * BAND_ROWS) * h->stage.stride;
        part.scratch = scratch;
        atomic_fetch_add(&h->pixels, chr_paint(h->chr, &part));
        ++painted;
    }
}

static void help(void *arg)
{
    helper_t *h = arg;
    for (;;) {
        (void)xiaotai_signal_take(&h->wake, XIAOTAI_WAIT_FOREVER);
        if (h->quit) break;
        /* A job the UI task has finished alone meanwhile is no longer posted. */
        int posted = JOB_POSTED;
        if (!atomic_compare_exchange_strong(&h->state, &posted, JOB_TAKEN)) continue;
        atomic_fetch_add(&h->helped, paint_bands(h, h->scratch));
        xiaotai_signal_give(&h->done);
    }
    xiaotai_signal_give(&h->done);
    xiaotai_task_exit();
}

/* Starts the helper where there is a second core. Without one, or when it cannot start, the UI
 * task paints everything, as on a single core. */
static void helper_start(actor_t *a)
{
    helper_t *h = &a->helper;
    xiaotai_firmware_t firmware;
    xiaotai_firmware(&firmware);
    if (firmware.cores < 2U) return;
    h->scratch = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, chr_scratch_entries(a->width), sizeof(*h->scratch));
    if (!h->scratch || xiaotai_signal_init(&h->wake) != XIAOTAI_OK) return;
    if (xiaotai_signal_init(&h->done) != XIAOTAI_OK) {
        xiaotai_signal_deinit(&h->wake);
        return;
    }
    /* The core the UI task is not on; either, when the UI task may run on both. */
    const int ui = XIAOTAI_UI_TASK_CORE;
    const xiaotai_task_config_t config = {.name = "chr_paint", .entry = help, .arg = h, .stack_bytes = HELPER_STACK,
        .stack = XIAOTAI_MEM_INTERNAL, .priority = XIAOTAI_UI_TASK_PRIORITY, .core = ui == 0 ? 1 : ui == 1 ? 0 : XIAOTAI_CORE_ANY};
    if (xiaotai_task_create(&config, &h->task) != XIAOTAI_OK) {
        xiaotai_signal_deinit(&h->wake);
        xiaotai_signal_deinit(&h->done);
        return;
    }
    h->running = true;
}

static void helper_stop(actor_t *a)
{
    helper_t *h = &a->helper;
    if (h->running) {
        h->quit = true;
        xiaotai_signal_give(&h->wake);
        (void)xiaotai_signal_take(&h->done, XIAOTAI_WAIT_FOREVER);
        xiaotai_signal_deinit(&h->wake);
        xiaotai_signal_deinit(&h->done);
        h->running = false;
    }
    xiaotai_mem_free(h->scratch);
    h->scratch = NULL;
}

static void release(actor_t *a)
{
    helper_stop(a);
    if (a->source.data) lv_image_cache_drop(&a->source);
    xiaotai_mem_free((void *)a->source.data);
    xiaotai_mem_free(a->scratch);
    xiaotai_mem_free(a->chr);
    xiaotai_mem_free(a);
    chr_pack_carried_close();
}

static xiaotai_err_t start(const xiaotai_ui_host_t *host, void **state)
{
    if (!host || !state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    const xiaotai_display_desc_t *desc = xiaotai_ui_host_display(host);
    if (!match(desc)) return XIAOTAI_ERR_NOT_SUPPORTED;
    actor_t *a = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*a));
    if (!a) return XIAOTAI_ERR_NO_MEM;
    a->host = host;
    a->display = xiaotai_ui_host_root(host);
    a->width = desc->width;
    a->height = desc->height;
    const uint32_t stride = lv_draw_buf_width_to_stride(desc->width, LV_COLOR_FORMAT_A1);
    const size_t bytes = (size_t)stride * desc->height;
    a->chr = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, chr_size());
    a->scratch = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, chr_scratch_entries(desc->width), sizeof(*a->scratch));
    const uint8_t *blank = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, bytes);
    a->source = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A1, .w = desc->width,
        .h = desc->height, .stride = stride}, .data_size = (uint32_t)bytes, .data = blank};
    const struct chr_pack *pack = chr_pack_carried();
    if (!a->chr || !a->scratch || !blank || stride > UINT16_MAX) {
        release(a);
        return XIAOTAI_ERR_NO_MEM;
    }
    if (!chr_init_from(a->chr, desc->width, desc->height, xiaotai_random(), pack)) {
        release(a);
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    helper_start(a);
    *state = a;
    return XIAOTAI_OK;
}

static void stop(void *state)
{
    if (state) release(state);
}

static bool paint(void *ctx, const xiaotai_ui_surface_t *target, int16_t widget_x, int16_t widget_y)
{
    actor_t *a = ctx;
    if (!target || !target->pixels || target->pixel != XIAOTAI_PIXEL_RGB565 || !target->width || !target->height ||
        target->width > a->width) return false;
    const int64_t began = xiaotai_time_us();
    const chr_target_t stage = {.pixels = target->pixels, .stride = target->stride,
        .x0 = (int16_t)(target->x - widget_x), .y0 = (int16_t)(target->y - widget_y),
        .x1 = (int16_t)(target->x - widget_x + target->width - 1), .y1 = (int16_t)(target->y - widget_y + target->height - 1),
        .swap = target->swap_bytes, .scratch = a->scratch, .lines = a->width};
    uint32_t pixels;
    helper_t *h = &a->helper;
    const int rows = stage.y1 - stage.y0 + 1;
    if (h->running && chr_director_share() && rows >= SHARE_ROWS &&
        (uint32_t)rows * target->width >= SHARE_PIXELS) {
        h->chr = a->chr;
        h->stage = stage;
        h->bands = (rows + BAND_ROWS - 1) / BAND_ROWS;
        atomic_store(&h->next, 0);
        atomic_store(&h->pixels, 0U);
        atomic_store(&h->state, JOB_POSTED);
        xiaotai_signal_give(&h->wake);
        (void)paint_bands(h, a->scratch);
        /* The helper either never came (the job is withdrawn) or is in its last band. */
        int posted = JOB_POSTED;
        if (!atomic_compare_exchange_strong(&h->state, &posted, JOB_NONE)) {
            (void)xiaotai_signal_take(&h->done, XIAOTAI_WAIT_FOREVER);
            atomic_store(&h->state, JOB_NONE);
        }
        pixels = atomic_load(&h->pixels);
        count(&a->shared, get(&a->shared) + 1U);
    } else {
        pixels = chr_paint(a->chr, &stage);
    }
    const unsigned spent = (unsigned)(xiaotai_time_us() - began);
    if (spent > get(&a->paint_max_us)) count(&a->paint_max_us, spent);
    count(&a->pixels, pixels);
    count(&a->paints, get(&a->paints) + 1U);
    count(&a->paint_total_us, get(&a->paint_total_us) + spent);
    return true;
}


static void place(void *state, const xiaotai_theme_lcd_stage_t *stage)
{
    actor_t *a = state;
    if (a->raised == stage->raised) return;
    a->raised = stage->raised;
    chr_place(a->chr, stage->raised);
    if (a->widget) lv_obj_invalidate(a->widget);
}

static void *build(void *state, void *parent, const xiaotai_theme_lcd_stage_t *stage)
{
    actor_t *a = state;
    if (stage->width != a->width || stage->height != a->height) return NULL;
    lv_obj_t *widget = lv_image_create(parent);
    if (!widget) return NULL;
    lv_image_set_src(widget, &a->source);
    lv_image_set_inner_align(widget, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_set_pos(widget, 0, 0);
    lv_obj_set_size(widget, a->width, a->height);
    lv_obj_remove_flag(widget, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (xiaotai_ui_host_painter(a->host, widget, paint, a) != XIAOTAI_OK) {
        lv_obj_delete(widget);
        return NULL;
    }
    a->widget = widget;
    a->shown = false;
    a->raised = stage->raised;
    chr_place(a->chr, stage->raised);
    return widget;
}

static void detach(void *state)
{
    actor_t *a = state;
    a->widget = NULL;
    a->shown = false;
}

static void set(void *state, const xiaotai_view_t *view, uint32_t now_ms)
{
    (void)now_ms;
    actor_t *a = state;
    a->mood = chr_mood_find(view->emotion);
    a->phase = view->phase <= XIAOTAI_TALK_SPEAKING ? view->phase : XIAOTAI_TALK_IDLE;
    a->level = view->level;
}

static lv_indev_t *pointer_of(const lv_display_t *display)
{
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && lv_indev_get_display(indev) == display) return indev;
    return NULL;
}

static void touch(actor_t *a)
{
    lv_indev_t *indev = pointer_of(a->display);
    if (!indev) return;
    const bool down = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t point = {0, 0};
    lv_indev_get_point(indev, &point);
    lv_area_t area = {0, 0, 0, 0};
    lv_obj_get_coords(a->widget, &area);
    const bool taken = chr_pointer(a->chr, down, (int16_t)(point.x - area.x1), (int16_t)(point.y - area.y1));
    if (!down) a->pressed = false;
    /* A press that the character took for itself: lift it from the control under the finger. */
    else if (taken && !a->pressed) {
        a->pressed = true;
        lv_indev_wait_release(indev);
    }
}

/* For the bench, which shows a speaking face without a talk: something like speech, with pauses. */
static uint16_t bench_voice(uint32_t now_ms)
{
    const uint32_t t = now_ms % 2600U;
    if (t >= 2000U || (t >= 900U && t < 1150U)) return 0;
    const float syllable = sinf((float)t * 0.0145f);
    return (uint16_t)(1500.0f + 10500.0f * syllable * syllable);
}

static void reach(atomic_int *least, atomic_int *most, int value)
{
    if (value < atomic_load_explicit(least, memory_order_relaxed)) atomic_store_explicit(least, value, memory_order_relaxed);
    if (value > atomic_load_explicit(most, memory_order_relaxed)) atomic_store_explicit(most, value, memory_order_relaxed);
}

static uint32_t tick(void *state, uint32_t now_ms, bool shown)
{
    actor_t *a = state;
    if (!a->widget || !shown) {
        if (a->shown) (void)chr_pointer(a->chr, false, 0, 0);
        a->shown = false;
        return 0;
    }
    const bool away = !a->shown && now_ms - a->last_ms > AWAY_MS;
    const uint32_t gap = a->shown ? now_ms - a->last_ms : FRAME_MS;
    if (a->shown && gap + 2U < FRAME_MS) return FRAME_MS - gap;
    if (a->shown && gap > 80U) count(&a->late, get(&a->late) + 1U);
    /* The time she was not on the screen (behind a dialog, on another page, the screen asleep) is hers as well, though
     * she does not move through it: her answer is timed by what is heard, which went on. */
    const uint32_t passed = !a->shown && a->stepped && now_ms - a->last_ms > FRAME_MS ? now_ms - a->last_ms - FRAME_MS : 0U;
    a->stepped = true;
    const int64_t began = xiaotai_time_us();
#if CHR_MARKS
    chr_mark_ms = (uint32_t)(began / 1000);
#endif
    if (passed) chr_pass(a->chr, passed);
    a->shown = true;
    a->last_ms = now_ms;

    uint32_t held = 0;
    size_t mood = a->mood;
    uint8_t phase = a->phase;
    uint32_t conditions = chr_director_conditions();
    const bool bench = chr_director_shown(now_ms, &held);
    if (bench) {
        mood = held & 0xFFU;
        phase = (uint8_t)(held >> 8 & 3U);
        conditions = held >> 16;
    }
    /* A preview shows the expression, whatever the device is in. */
    if (a->raised && !bench) conditions = 0;
    chr_hold(a->chr, bench);      /* what the bench shows stays for as long as it asked */
    chr_set_mood(a->chr, mood);
    chr_set_phase(a->chr, (chr_phase_t)phase);
    chr_set_level(a->chr, a->level);
    chr_set_conditions(a->chr, conditions);
    uint16_t microphone = 0, speaker = 0;
    const bool sounded = chr_director_loudness(&microphone, &speaker), voiced = bench && phase == CHR_PHASE_SPEAKING;
    if (voiced) speaker = bench_voice(now_ms);
    if (sounded || voiced) {
        chr_set_envelope(a->chr, microphone, speaker);
        uint16_t ahead = speaker;
        const bool known = voiced || chr_director_speaker_ahead(CHR_AHEAD_MS, &ahead);
        chr_set_ahead(a->chr, ahead, known);
    } else chr_no_envelope(a->chr);
    int32_t begin = 0;
    size_t told = 0;
    while (chr_director_take_sentence(&begin, &told)) chr_sentence(a->chr, begin, told);
    if (away) chr_return(a->chr, a->raised && !bench);
    int32_t value = 0;
    const uint32_t cues = chr_director_take_cues(&value, now_ms);
    if (a->woke) {
        /* She stretches when the screen comes on for her page, not some time later, and not if there is
         * something to react to (below). */
        a->woke = false;
        const int32_t ago = (int32_t)(now_ms - a->woke_ms);
        chr_woken(a->chr, ago > -DOZE_MS && ago < DOZE_MS);
    }
    for (unsigned cue = 1; cues && cue < CHR_CUE_COUNT; ++cue)
        if (cues & 1U << cue) chr_cue(a->chr, (chr_cue_t)cue, cue == CHR_CUE_VOLUME ? value : 0);
    touch(a);

    chr_rect_t damage[CHR_DAMAGE_MAX];
    const size_t rects = chr_step(a->chr, gap, damage);
    touch(a);   /* what the step took is hers at once, before the finger has gone far enough to turn the page */
    lv_area_t origin = {0, 0, 0, 0};
    lv_obj_get_coords(a->widget, &origin);
    for (size_t i = 0; i < rects; ++i) {
        const lv_area_t area = {origin.x1 + damage[i].x, origin.y1 + damage[i].y,
                                origin.x1 + damage[i].x + damage[i].width - 1, origin.y1 + damage[i].y + damage[i].height - 1};
        lv_obj_invalidate_area(a->widget, &area);
    }
    count(&a->frames, get(&a->frames) + 1U);
    if (!rects) count(&a->quiet, get(&a->quiet) + 1U);
    chr_info_t info;
    chr_inspect(a->chr, &info);
    count(&a->mouth, (unsigned)(info.mouth * 100.0f));
    count(&a->hear, (unsigned)(info.listen * 100.0f));
    if (info.shut_ms != get(&a->shut)) {
        count(&a->shut, info.shut_ms);
        if (info.shut_ms > get(&a->shut_max)) count(&a->shut_max, info.shut_ms);
    }
    count(&a->wait, info.wait_ms);
    count(&a->lag, info.lag_ms);
    reach(&a->moved[0], &a->moved[1], info.moved_x);
    reach(&a->moved[2], &a->moved[3], info.moved_feet);
    reach(&a->moved[4], &a->moved[5], info.moved_top);
    const unsigned spent = (unsigned)(xiaotai_time_us() - began);
    if (spent > get(&a->step_max_us)) count(&a->step_max_us, spent);
    return FRAME_MS;
}

static void sleep_changed(void *state, bool asleep)
{
    actor_t *a = state;
    const uint32_t now = (uint32_t)(xiaotai_time_us() / 1000);
    if (asleep && !a->asleep) a->slept_ms = now;
    /* Woken after a real sleep: tick() has her stretch. */
    if (!asleep && a->asleep && now - a->slept_ms > DOZE_MS) {
        a->woke = true;
        a->woke_ms = now;
    }
    /* A sleeping screen is not drawn, nor ticked: the first step after it is not a late one. */
    if (asleep) a->shown = false;
    a->asleep = asleep;
}

static xiaotai_ui_rect_t hit(void *state)
{
    const actor_t *a = state;
    const chr_rect_t box = chr_body_box(a->chr);
    return (xiaotai_ui_rect_t){.x = (uint16_t)(box.x < 0 ? 0 : box.x), .y = (uint16_t)(box.y < 0 ? 0 : box.y),
                               .width = (uint16_t)box.width, .height = (uint16_t)box.height};
}

static void report(void *state, char *line, size_t capacity)
{
    actor_t *a = state;
    if (!line || !capacity) return;
    /* Totals since start, so that two readings give rates: steps, steps that drew nothing, late steps,
     * paint calls, those offered to the second core, the bands it painted, time spent painting. Then
     * the worst step and paint, the pixels of the last paint, and how far the mouth and the listening
     * are open (0..100). How long after the last sound the mouth shut, the last time and at most. The
     * longest she waited for a pose's first picture, and for a later one past its time. Then how far
     * she has been from her place since the last reading, in pixels: sideways, her feet, the top of
     * her head. */
    int moved[6];
    for (unsigned i = 0; i < 6U; ++i) moved[i] = atomic_exchange_explicit(&a->moved[i], 0, memory_order_relaxed);
    (void)snprintf(line, capacity, "actor=character frames=%u quiet=%u late=%u paints=%u shared=%u helped=%u "
                   "paint_ms=%u step_max_us=%u paint_max_us=%u pixels=%u mouth=%u hear=%u shut_ms=%u/%u "
                   "wait_ms=%u/%u moved=%d:%d,%d:%d,%d:%d", get(&a->frames),
                   get(&a->quiet), get(&a->late), get(&a->paints), get(&a->shared), get(&a->helper.helped),
                   get(&a->paint_total_us) / 1000U, get(&a->step_max_us), get(&a->paint_max_us), get(&a->pixels),
                   get(&a->mouth), get(&a->hear), get(&a->shut), get(&a->shut_max), get(&a->wait), get(&a->lag),
                   moved[0], moved[1], moved[2], moved[3], moved[4], moved[5]);
    const size_t used = strlen(line);
    chr_pack_carried_report(line + used, capacity - used);
}

const xiaotai_theme_lcd_actor_t xiaotai_theme_character_actor = {
    .name = "character", .match = match, .start = start, .stop = stop, .build = build, .place = place,
    .detach = detach, .set = set, .tick = tick, .sleep = sleep_changed, .hit = hit, .report = report,
};
