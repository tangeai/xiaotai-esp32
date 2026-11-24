// SPDX-License-Identifier: MIT
#include "ui_internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/port/mem.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define DIAG_WAIT_MS 2000U

static void lock(void) { (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&g_ui.lock); }

/* Status words have no spaces or unescaped non-ASCII bytes on the wire. */
static void token(char *out, size_t capacity, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)text; p && *p; ++p) {
        const bool safe = *p > 32U && *p <= 126U && *p != '%' && *p != '=';
        if (used + (safe ? 1U : 3U) >= capacity) break;
        if (safe) out[used++] = (char)*p;
        else {
            out[used++] = '%';
            out[used++] = hex[*p >> 4U];
            out[used++] = hex[*p & 15U];
        }
    }
    if (!used) out[used++] = '-';
    out[used] = '\0';
}

static xiaotai_err_t show(const xiaotai_reply_t *reply, uint8_t index)
{
    xiaotai_ui_info_t info;
    const xiaotai_err_t err = xiaotai_ui_inspect(index, &info);
    if (err != XIAOTAI_OK) return err;
    const ui_display_t *d = ui_display_at(index);
    lock();
    const size_t objects = d->controls_visible;
    unlock();
    char title[3U * sizeof(info.page) + 1U], theme[3U * sizeof(info.theme) + 1U], app[64];
    token(title, sizeof(title), info.page);
    token(theme, sizeof(theme), info.theme);
    token(app, sizeof(app), xiaotai_app_foreground());
    xiaotai_at_printf(reply, "+UI: ready=%u awake=%u display=%u width=%u height=%u theme=%s app=%s view=%s "
        "objects=%zu focus=%u theme_refused=%u shown=%" PRIu32 " updated=%" PRIu32 " dropped=%" PRIu32
        " truncated=%" PRIu32 " posted=%" PRIu32 " rejected=%" PRIu32 " ignored=%" PRIu32
        " owner_timer=%u late_steps=%" PRIu32 " stack_free=%" PRIu32,
        info.ready, info.awake, index, info.width, info.height, theme, app, title, objects,
        info.focus, info.theme_refused, info.shown, info.updated, info.dropped, info.truncated, info.posted, info.rejected,
        info.ignored, info.owner_timer, info.late_steps, info.stack_free);
    return XIAOTAI_OK;
}

static xiaotai_err_t perf(const xiaotai_reply_t *reply, uint8_t index)
{
    ui_display_t *d = ui_display_at(index);
    if (!d) return XIAOTAI_ERR_NOT_FOUND;
    lock();
    const uint32_t refresh = d->frames, flush = d->flushes, wait = d->flush_waits, errors = d->frame.errors;
    const uint32_t slow = d->slow_frames, longest = d->max_frame_ms;
    const uint64_t pixels = d->flush_pixels, ms = d->render_ms, bytes = d->flush_bytes, us = d->flush_us;
    const xiaotai_theme_t *theme = d->state == UI_DISPLAY_READY ? d->theme : NULL;
    void *state = d->theme_state;
    unlock();
    xiaotai_at_printf(reply, "+UI-PERF: refresh=%" PRIu32 " pixels=%" PRIu64 " ms=%" PRIu64 " flush=%" PRIu32
        " bytes=%" PRIu64 " wait=%" PRIu32 " us=%" PRIu64 " errors=%" PRIu32 " slow=%" PRIu32 " max_ms=%" PRIu32,
        refresh, pixels, ms, flush, bytes, wait, us, errors, slow, longest);
    if (theme && theme->report) {
        char words[192] = {0};
        theme->report(state, words, sizeof(words));
        words[sizeof(words) - 1U] = '\0';
        if (words[0] && !strpbrk(words, "\r\n")) xiaotai_at_printf(reply, "+UI-THEME: %s", words);
    }
    return XIAOTAI_OK;
}

static void report_ui(const xiaotai_reply_t *reply)
{
    for (size_t i = 0; i < g_ui.display_count; ++i) (void)show(reply, (uint8_t)i);
}

static void report_display(const xiaotai_reply_t *reply)
{
    for (size_t i = 0; i < g_ui.display_count; ++i) (void)perf(reply, (uint8_t)i);
}

static xiaotai_err_t state_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return show(reply, 0);
}

static xiaotai_err_t perf_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return perf(reply, 0);
}

static xiaotai_err_t local(const xiaotai_reply_t *reply)
{
    return xiaotai_at_reply_is_console(reply) ? XIAOTAI_OK : XIAOTAI_ERR_NOT_ALLOWED;
}

static xiaotai_err_t handover(const xiaotai_reply_t *reply, bool (*next)(void *, const xiaotai_reply_t *), void *ctx)
{
    const xiaotai_at_stream_t stream = {.next = next, .ctx = ctx};
    const xiaotai_err_t err = xiaotai_at_stream(reply, &stream);
    return err == XIAOTAI_ERR_NOT_SUPPORTED ? XIAOTAI_ERR_NOT_ALLOWED : err;
}

static bool actions_next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    size_t count = 0;
    xiaotai_err_t err = xiaotai_ui_controls(0, NULL, 0, &count, DIAG_WAIT_MS);
    xiaotai_ui_control_t *rows = NULL;
    for (unsigned attempt = 0; err == XIAOTAI_OK && count && attempt < 3U; ++attempt) {
        const size_t capacity = count;
        if (capacity > SIZE_MAX / sizeof(*rows)) { err = XIAOTAI_ERR_INVALID_SIZE; break; }
        rows = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, capacity * sizeof(*rows));
        if (!rows) { err = XIAOTAI_ERR_NO_MEM; break; }
        err = xiaotai_ui_controls(0, rows, capacity, &count, DIAG_WAIT_MS);
        if (err != XIAOTAI_OK || count <= capacity) break;
        xiaotai_mem_free(rows);
        rows = NULL;
    }
    if (err == XIAOTAI_OK && count && !rows) err = XIAOTAI_ERR_BUSY;
    if (err != XIAOTAI_OK) (void)xiaotai_at_fail(reply, err, NULL);
    else for (size_t i = 0; i < count && xiaotai_at_stream_result(reply) == XIAOTAI_OK; ++i) {
        const xiaotai_ui_control_t *r = &rows[i];
        char label[2U * sizeof(r->text) + 1U], intent[3U * sizeof(r->intent) + 1U];
        (void)xiaotai_at_text_encode(r->text, label, sizeof(label));
        token(intent, sizeof(intent), r->intent);
        xiaotai_at_printf(reply, "+UI-ACTION: x=%d y=%d w=%u h=%u enabled=%u intent=%s text_hex=%s",
            r->x, r->y, r->width, r->height,
            !(r->state & (XIAOTAI_STATE_DISABLED | XIAOTAI_STATE_BUSY | XIAOTAI_STATE_HIDDEN)), intent, label);
    }
    xiaotai_mem_free(rows);
    return false;
}

static xiaotai_err_t actions_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return handover(reply, actions_next, NULL);
}

static bool sleep_next(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    (void)xiaotai_at_fail(reply, ui_power_sleep_guarded(0), NULL);
    return false;
}

static xiaotai_err_t sleep_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return handover(reply, sleep_next, NULL);
}

static xiaotai_err_t redraw_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    const xiaotai_err_t err = local(reply);
    return err == XIAOTAI_OK ? xiaotai_ui_refresh(0) : err;
}

static bool coord(const char *const *argv, uint32_t *x, uint32_t *y)
{
    return xiaotai_at_parse_u32(argv[0], 0, INT16_MAX, x) && xiaotai_at_parse_u32(argv[1], 0, INT16_MAX, y);
}

static xiaotai_err_t tap_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    const xiaotai_err_t err = local(reply);
    uint32_t x, y;
    if (err != XIAOTAI_OK) return err;
    if (!coord(argv, &x, &y)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_ui_tap(0, (int16_t)x, (int16_t)y);
}

static xiaotai_err_t swipe_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    const xiaotai_err_t err = local(reply);
    uint32_t x0, y0, x1, y1, ms;
    if (err != XIAOTAI_OK) return err;
    if (!coord(argv, &x0, &y0) || !coord(argv + 2, &x1, &y1) ||
        !xiaotai_at_parse_u32(argv[4], 100, 2000, &ms)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_ui_swipe(0, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, ms);
}

static xiaotai_err_t pointer_verb(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    const xiaotai_err_t err = local(reply);
    uint32_t x, y;
    bool down;
    if (err != XIAOTAI_OK) return err;
    if (!coord(argv, &x, &y) || !xiaotai_at_parse_flag(argv[2], &down)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_ui_pointer(0, (int16_t)x, (int16_t)y, down);
}

#if XIAOTAI_UI_CAPTURE
static bool xiaotai_ui_at_screen_next(void *ctx, const xiaotai_reply_t *reply)
{
    const uint8_t display = (uint8_t)(uintptr_t)ctx;
    xiaotai_ui_capture_t frame;
    char view[32];
    xiaotai_err_t err = ui_capture_named(display, &frame, view, DIAG_WAIT_MS);
    if (err == XIAOTAI_OK) {
        char header[XIAOTAI_AT_BLOB_HEADER_MAX + 1U], word[3U * sizeof(view) + 1U];
        const char *format = frame.pixel == XIAOTAI_PIXEL_MONO1_VLSB ? "MONO1_VLSB" :
                             frame.pixel == XIAOTAI_PIXEL_MONO1 ? "MONO1" : "RGB565LE";
        token(word, sizeof(word), view);
        (void)snprintf(header, sizeof(header), "format=%s width=%u height=%u view=%s", format,
                       frame.width, frame.height, word);
        err = xiaotai_at_blob_write(reply, "SCREEN", header, frame.pixels, frame.size);
    }
    xiaotai_ui_capture_free(&frame);
    if (err != XIAOTAI_OK) (void)xiaotai_at_fail(reply, err, err == XIAOTAI_ERR_BUSY ? "ACTIVITY" : NULL);
    return false;
}

static xiaotai_err_t xiaotai_ui_at_screen_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    uint32_t display = 0;
    if (argc && !xiaotai_at_parse_u32(argv[0], 0, UINT8_MAX, &display)) return XIAOTAI_ERR_INVALID_ARG;
    if (ui_diag_activity()) return xiaotai_at_fail(reply, XIAOTAI_ERR_BUSY, "ACTIVITY");
    return handover(reply, xiaotai_ui_at_screen_next, (void *)(uintptr_t)display);
}
#endif

xiaotai_err_t ui_commands_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = state_verb}, {.word = "ACTIONS", .run = actions_verb}, {.word = "PERF", .run = perf_verb},
        {.word = "SLEEP", .run = sleep_verb}, {.word = "REDRAW", .run = redraw_verb},
        {.word = "TAP", .min_args = 2, .max_args = 2, .run = tap_verb, .usage = "<x> <y>"},
        {.word = "SWIPE", .min_args = 5, .max_args = 5, .run = swipe_verb, .usage = "<x> <y> <x2> <y2> <ms>"},
        {.word = "POINTER", .min_args = 3, .max_args = 3, .run = pointer_verb, .usage = "<x> <y> <0|1>"},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+UI", verbs, sizeof(verbs) / sizeof(verbs[0]));
#if XIAOTAI_UI_CAPTURE
    static const xiaotai_at_verb_t screen = {.max_args = 1, .run = xiaotai_ui_at_screen_run, .usage = "[<display>]"};
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+SCREEN", &screen, 1);
#endif
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("ui", report_ui);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("display", report_display);
    return err;
}
