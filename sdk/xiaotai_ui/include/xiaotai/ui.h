// SPDX-License-Identifier: MIT
#pragma once
/* UI system: takes the views applications show (xiaotai/view.h), keeps a copy
 * per display and layer, and has the theme that best matches the display draw
 * them. It also turns pointer and navigation-key input on the view into the
 * intents the view names, owns the screen's sleep timeout, and offers the
 * diagnostics the test bench drives (inspect, capture, injected pointer).
 *
 * This header is what applications, the product glue and other engines use.
 * Themes and custom templates implement xiaotai/ui_theme.h.
 *
 * Threading: one UI task per display draws, reads the pointer and runs theme
 * code; nothing else touches a theme or the display. Where the build option
 * XIAOTAI_UI_TASK is 0 (the default without external RAM), a display drawn
 * by the FRAME backend has no task: owner-task timers (xiaotai_timer_start)
 * do that work, one bounded step per timer, and "the UI task" of that display
 * is the owner task (xiaotai/ui_config.h has the rule and its cost). A
 * function tagged [any, not the UI task] waits for the UI task, so it is not
 * called on the owner task for such a display and returns
 * XIAOTAI_ERR_INVALID_STATE there instead of waiting for itself.
 * Functions here are safe from the tasks stated; none of them blocks on
 * drawing unless it says so.
 * Tags: [glue], [init], [owner], [any] as in xiaotai/framework.h; [ui] the
 * display's UI task (the owner task where the display has none). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/port/err.h"
#include "xiaotai/ui_config.h"
#include "xiaotai/view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- lifecycle */

/* Engine "ui" (engine.json; the video engine is an optional dependency, see
 * xiaotai_ui_video_show). init installs the view sink and registers the
 * navigation intents, the commands AT+UI (state, visible actions, timing,
 * injected tap, swipe and pointer, sleep, redraw) and AT+SCREEN (capture), and
 * the diagnostics providers "ui" and "display"; it follows
 * XIAOTAI_EVENT_ACTIVITY for the capture admission below. The owner task
 * never waits for a UI task: AT+UI ACTIONS and AT+SCREEN hand over a stream
 * (xiaotai/at_command.h) whose next() runs on the console task, calls
 * xiaotai_ui_controls() or xiaotai_ui_capture() there and writes the picture
 * with xiaotai_at_blob_write(). AT+UI SLEEP is
 * admitted under the conditions of the automatic sleep (the page carries
 * XIAOTAI_VIEW_MAY_SLEEP, no overlay shows, no pointer is down; a sleeping
 * screen answers OK), otherwise ERROR: INVALID_STATE: which pages may go dark
 * is the application's decision, also for the test bench. start (after every engine,
 * module and application initialized, so every theme, template and image is
 * registered) opens each
 * display part, picks its theme and starts its UI task (or, see
 * XIAOTAI_UI_TASK, arms its owner-task timer). A display no theme
 * matches is left closed and counted; views for it are dropped. Views shown
 * between init and start are kept and drawn at start. */
xiaotai_err_t xiaotai_engine_ui_init(void);
void xiaotai_engine_ui_start(void);

typedef struct {
    /* Theme to use for display 0 by name (a theme's name is its manifest
     * name without "theme_": "lcd320"), so the build can check a product's
     * value against the selected themes and the board's display. NULL: the
     * best match. A name that at start is not registered, or whose theme
     * scores 0 for display 0, is refused: display 0 gets the best match so it
     * does not stay dark, and the refusal is recorded in the event log and
     * reported (xiaotai_ui_info_t.theme_refused, AT+UI). Borrowed until start. */
    const char *theme;
    /* Screen idle timeout; 0: never sleeps on its own. Changed later with
     * xiaotai_ui_set_sleep_timeout(). */
    uint32_t sleep_timeout_ms;
    /* Bytes of copied strings and arrays kept for the PAGE and for the OVERLAY
     * of a display (XIAOTAI_MEM_BULK, taken when the layer is first used and
     * kept); 0: 8192 with external RAM, 1536 without. A view that does not fit
     * is truncated and counted. The TOAST keeps XIAOTAI_UI_TOAST_BYTES. */
    uint32_t layer_bytes;

    /* Members below default to the behaviour described when zeroed. */

    /* What a touch that begins while the screen sleeps does. false: it only
     * wakes the screen; until it is lifted no control receives anything
     * (press, tap, hold, swipe). true: it wakes the screen and is then
     * handled as any touch, so it may operate the control under it. A
     * navigation intent while asleep only wakes, either way. */
    bool dark_touch_operates;
    /* Milliseconds after a swipe was recognised on a layer during which a tap
     * on that layer's emotion (the PRIMARY action without label and icon that
     * pointer themes fire from the emotion on HOME and TALK) is ignored and
     * counted in ignored, so the finger that just swiped does not also start
     * the action on the next home page. The window belongs to the layer, so
     * it also covers the page the swipe's intent showed next. Other controls
     * are not affected. 0: no such window. This is the product's choice; the
     * core always keeps one swipe per touch and at least 350 ms between two
     * swipes. */
    uint32_t swipe_tap_guard_ms;
    /* Optional rejection text for touch actions marked TAP_LOCAL_REJECTION.
     * On a full owner queue the display shows this directly, instead of
     * enqueueing a rejection event. NULL uses the ordinary event for all
     * actions. Text is borrowed until reset. */
    const char *local_rejection_toast;
} xiaotai_ui_config_t;

#define XIAOTAI_UI_TOAST_BYTES 192U

/* [glue, init] Product configuration; without it the defaults above apply.
 * XIAOTAI_ERR_INVALID_ARG: a theme name longer than 15 characters (no theme
 * can carry it); XIAOTAI_ERR_INVALID_STATE once the UI started. Whether the
 * named theme exists is known only at start (themes register in their init). */
xiaotai_err_t xiaotai_ui_configure(const xiaotai_ui_config_t *config);

typedef enum {
    XIAOTAI_UI_HOLD_BEGIN = 0,
    XIAOTAI_UI_HOLD_END,
    XIAOTAI_UI_HOLD_REFRESH,
} xiaotai_ui_hold_phase_t;

typedef xiaotai_err_t (*xiaotai_ui_local_hold_fn_t)(void *ctx, uint32_t ticket,
                                                   int32_t value, xiaotai_ui_hold_phase_t phase);

/* [init] Optionally bind one display-0 HOLD intent after UI init, before start.
 * The intent name and ctx are borrowed through UI shutdown. The binding is
 * synchronous on the UI task, without the engine state mutex: the callback
 * must not reenter the renderer or wait for the owner. A successful BEGIN
 * gets one END with the same nonzero ticket and value on release, cancellation or
 * shutdown; a refused BEGIN gets no END. Neither edge enters the intent queue,
 * and the binding does not repeat. All other HOLD inputs post intents as usual.
 * A headless UI returns XIAOTAI_ERR_NOT_SUPPORTED. */
xiaotai_err_t xiaotai_ui_local_hold_register(const char *intent, xiaotai_ui_local_hold_fn_t fn, void *ctx);

/* Host tests: stop the UI tasks, close displays, forget every registration. */
void xiaotai_ui_reset(void);

/* --------------------------------------------------- intents and events */

/* Navigation intents the UI system handles, for displays operated by keys.
 * An application's interaction profile maps gestures to them
 * ("main" click -> ui.next, double click -> ui.confirm, long press -> ui.back).
 * They act on the top layer of the display given by args.value:
 *   next / previous  move the focus through items or fields (wrapping); on
 *                    DIGITS change the digit; with nothing to step through,
 *                    fire the AUX action
 *   confirm          choose the focused item, operate the focused field, or
 *                    fire the PRIMARY action when nothing has the focus
 *   back             fire the BACK action
 * A key press while the screen sleeps only wakes it. */
#define XIAOTAI_INTENT_UI_NEXT "ui.next"
#define XIAOTAI_INTENT_UI_PREVIOUS "ui.previous"
#define XIAOTAI_INTENT_UI_CONFIRM "ui.confirm"
#define XIAOTAI_INTENT_UI_BACK "ui.back"

/* A person touched the screen or used a navigation intent. args.value: the
 * display. Emitted at most once per 250 ms. Applications use it for their own
 * idle rules (idle expressions); the screen timeout is restarted internally. */
#define XIAOTAI_EVENT_UI_INTERACTION "ui.interaction"
/* The screen went to sleep (args.value 1) or woke (0); args.text: the display
 * index as decimal text. */
#define XIAOTAI_EVENT_UI_SLEEP "ui.sleep"
/* An element named an intent nothing handles, or the post was refused.
 * args.text: the intent name. args.value: 0 for immediate delivery, or an
 * opaque scope ticket for a deferred notice. Before showing a notice, consumers
 * should use xiaotai_ui_rejection_current() on the framework owner. */
#define XIAOTAI_EVENT_UI_REJECTED "ui.rejected"
bool xiaotai_ui_rejection_current(const xiaotai_args_t *args);

/* ------------------------------------------------------------ screen power */

/* The screen sleeps (backlight off, theme animation paused, views still
 * accepted) when: a timeout is set, the page carries XIAOTAI_VIEW_MAY_SLEEP,
 * no overlay shows, and no interaction happened for the timeout. It wakes on
 * xiaotai_ui_activity(), xiaotai_ui_wake(), and on a touch or navigation
 * intent; that first key only wakes and reaches no control, and so does that
 * first touch unless the product set xiaotai_ui_config_t.dark_touch_operates. */

xiaotai_err_t xiaotai_ui_set_sleep_timeout(uint32_t timeout_ms);   /* [any] 0: never */
/* [any] Something the user would call activity happened outside the UI (a
 * voice command, a call ended): restart the timeout and wake every display. */
void xiaotai_ui_activity(void);
/* [any] Sleep now / wake now. The display owner performs the operation;
 * a caller on another task waits for that step and may already have a frame
 * in flight. A request not picked up in time returns XIAOTAI_ERR_TIMEOUT;
 * a second pending request returns XIAOTAI_ERR_BUSY. XIAOTAI_ERR_NOT_FOUND:
 * no such display;
 * the part's error when the backlight refused, in which case the state is
 * unchanged and the next request tries again. Sleeping is refused with
 * XIAOTAI_ERR_INVALID_STATE while a pointer is down. */
xiaotai_err_t xiaotai_ui_sleep(uint8_t display);
xiaotai_err_t xiaotai_ui_wake(uint8_t display);
bool xiaotai_ui_awake(uint8_t display);                            /* [any] */
/* [any] Backlight level while awake, 1..100; kept across sleep. Same
 * display-owner completion rule as xiaotai_ui_sleep()/wake(). */
xiaotai_err_t xiaotai_ui_set_brightness(uint8_t display, uint8_t percent);

/* ------------------------------------------------------------------ images */

typedef enum {
    XIAOTAI_IMAGE_MONO1 = 0,   /* 1 bit per pixel, rows top to bottom, most significant bit left,
                                  each row padded to a byte; 1 is ink (whatever the layout of the
                                  display's own frames) */
    XIAOTAI_IMAGE_RGB565,      /* little-endian, rows top to bottom */
    XIAOTAI_IMAGE_RGB565A8,    /* RGB565 plane followed by an 8-bit alpha plane */
} xiaotai_image_format_t;

typedef struct {
    const char *name;          /* what xiaotai_view_t.image and icons refer to */
    uint16_t width, height;
    xiaotai_image_format_t format;
    const uint8_t *data;       /* borrowed for the life of the program (read-only data) */
    size_t size;
} xiaotai_ui_image_t;

/* [init] Pictures an application brings (a mini-program code, a shortcut
 * icon). The structure and its data are kept by pointer. A theme draws an
 * image when its format suits the display and skips it otherwise.
 * XIAOTAI_ERR_INVALID_STATE: the name is taken; XIAOTAI_ERR_NO_MEM: more than
 * XIAOTAI_UI_IMAGES_MAX (xiaotai/ui_config.h). */
xiaotai_err_t xiaotai_ui_image_register(const xiaotai_ui_image_t *image);
const xiaotai_ui_image_t *xiaotai_ui_image_find(const char *name);   /* [any] NULL when absent */

/* ------------------------------------------------------------------- video */

/* A rectangle in display pixels: the display's logical frame, as the theme
 * draws (not the panel's native frame of xiaotai_rect_t). The one logical
 * display rectangle of xiaotai/hal.h, under the name the UI system has always
 * used; the video engine's xiaotai_video_area_t is the same type, so a video
 * source passes rectangles between the two without converting. */
typedef xiaotai_display_area_t xiaotai_ui_rect_t;

/* A drawing target inside the display's frame: `pixels` addresses the pixel
 * at (x, y) of the display; rows are `stride` bytes apart. Valid only during
 * the callback that receives it. */
typedef struct {
    uint8_t *pixels;
    uint16_t x, y, width, height;   /* the part of the display this call may write */
    uint32_t stride;
    xiaotai_pixel_t pixel;
    bool swap_bytes;                /* RGB565 words are stored byte-swapped */
} xiaotai_ui_surface_t;

/* What draws behind the controls of a view with XIAOTAI_VIEW_VIDEO (the far
 * end of a video call). Both functions run on the UI task and must neither
 * block nor allocate.
 *
 * One refresh is one pending() followed, when it returned true, by every
 * paint() of that refresh, all before the next pending(). A source may
 * therefore pin one frame in pending() and paint the strips of a partial
 * refresh from it without tearing. After xiaotai_ui_video_clear() returned,
 * neither function is called again, so the owner of the source may then
 * release the frame it pinned. */
typedef struct {
    /* True when a frame newer than the last painted one waits. Polled every
     * XIAOTAI_UI_VIDEO_POLL_MS while such a view shows (the pointer is read
     * first, so video never delays touch). */
    bool (*pending)(void *ctx);
    /* Write the pinned frame into `target`, placed in `area` (the video
     * region of the view, in display pixels; themes with a video region lay
     * it out over the whole display). The source copies rows or repeats
     * pixels by a whole factor, centred in the area; it never resamples, and
     * what the frame does not cover keeps the theme's background. May be
     * called several times per refresh with different targets (partial
     * refresh). Returning false leaves the theme's placeholder. */
    bool (*paint)(void *ctx, const xiaotai_ui_surface_t *target,
                  uint16_t area_x, uint16_t area_y, uint16_t area_width, uint16_t area_height);
    void *ctx;

    /* Members below default to "absent" when the structure is zeroed. */

    /* Optional. Called after pending() returned true, before the paints of
     * that refresh: where paint() will place the pinned frame inside the
     * area, in display pixels. The backend then invalidates only the union of
     * this rectangle and the one of the previous refresh (a letterboxed
     * picture on a large panel), instead of the whole area. Returning false,
     * or no function, invalidates the whole area. */
    bool (*placement)(void *ctx, uint16_t area_x, uint16_t area_y, uint16_t area_width,
                      uint16_t area_height, xiaotai_ui_rect_t *picture);
} xiaotai_ui_video_source_t;

#define XIAOTAI_UI_VIDEO_POLL_MS 10U

/* [owner] Install the video source of a display for one session generation
 * (0 is invalid): the generation of the session the picture belongs to (a
 * video call module passes its call's link generation). The structure is
 * copied. A source of an older generation than the installed one is refused
 * with XIAOTAI_ERR_INVALID_STATE and counted. XIAOTAI_ERR_NOT_SUPPORTED: the
 * display's theme has no video region. */
xiaotai_err_t xiaotai_ui_video_set(uint8_t display, const xiaotai_ui_video_source_t *source, uint32_t generation);
/* [owner] Remove the source if `generation` still owns it; stale calls are
 * dropped and counted. Returns after the UI task stopped calling it. */
void xiaotai_ui_video_clear(uint8_t display, uint32_t generation);

/* [owner] Install the built-in source that shows the video engine's peer
 * picture (xiaotai/video.h), with the rules of xiaotai_ui_video_set(), so
 * the modules that show remote video (calls, web streams) install no adapter
 * of their own. The UI task takes frames with xiaotai_video_frame_acquire()
 * (pending), paints them with xiaotai_video_frame_paint() (paint, converting
 * the surface) and reports where the picture lands with
 * xiaotai_video_frame_placement() (placement, so only the old and new picture
 * rectangles are refreshed); the video engine does the centring arithmetic
 * for both, so placement and paint never differ by a pixel.
 * xiaotai_ui_video_clear(display, generation) removes it like any source;
 * the UI system then releases the last pinned frame
 * (xiaotai_video_frame_release() on the owner task, after the UI task
 * stopped). XIAOTAI_ERR_NOT_SUPPORTED: the product has no video engine (the
 * UI system is built without it) or the display's theme has no video region;
 * other results as xiaotai_ui_video_set(). */
xiaotai_err_t xiaotai_ui_video_show(uint8_t display, uint32_t generation);

/* ------------------------------------------------------------- diagnostics */

typedef struct {
    bool ready;                 /* display open and a theme running */
    bool awake;
    bool present_fault;         /* the display refused a frame; cleared by xiaotai_ui_refresh() */
    uint16_t width, height;
    xiaotai_pixel_t pixel;
    char theme[16];
    char page[32];              /* id of the page, or its kind name */
    char overlay[32];           /* "" when none */
    uint8_t page_kind;          /* xiaotai_view_kind_t */
    uint16_t focus;             /* focused item or field + 1; 0: none */
    uint32_t shown;             /* views accepted (new identities) */
    uint32_t updated;           /* views accepted as updates */
    uint32_t dropped;           /* views for a display without a theme, or while not started */
    uint32_t truncated;         /* strings or arrays cut to the limits */
    uint32_t posted;            /* intents posted from input */
    uint32_t rejected;          /* intents nothing handled */
    uint32_t ignored;           /* input on disabled, busy or stale elements; wake-only touches */
    uint32_t frames;            /* frames drawn */
    uint32_t slow_frames;       /* frames over XIAOTAI_UI_SLOW_FRAME_MS */
    uint32_t max_frame_ms;
    uint32_t present_errors;
    uint32_t video_stale;       /* video calls dropped for a stale generation */
    uint32_t stack_free;        /* least free stack of the UI task, bytes; 0 when served by
                                   owner-task timers (the framework section reports that stack) */

    /* Members below were appended; older readers ignore them. */
    bool theme_refused;         /* display 0: the configured theme was unknown or scored 0 */
    bool owner_timer;           /* served by owner-task timers, without a UI task (XIAOTAI_UI_TASK) */
    uint32_t late_steps;        /* owner-timer form: periodic steps (the once-a-second timer or
                                   clock, the pointer read) that ran more than one period behind
                                   their due time and restarted their schedule from now, without
                                   catching up; 0 with a UI task */
} xiaotai_ui_info_t;

#define XIAOTAI_UI_SLOW_FRAME_MS 20U

/* [any] XIAOTAI_ERR_NOT_FOUND: no such display. Counters are read without
 * stopping the UI task and may be one frame old. */
xiaotai_err_t xiaotai_ui_inspect(uint8_t display, xiaotai_ui_info_t *out);

/* What of a view a control stands for. */
typedef enum {
    XIAOTAI_UI_ELEMENT_ITEM = 0,
    XIAOTAI_UI_ELEMENT_ACTION,
    XIAOTAI_UI_ELEMENT_FIELD,
    XIAOTAI_UI_ELEMENT_INDICATOR,
    XIAOTAI_UI_ELEMENT_OTHER,   /* no element of the view: a keypad key, a template's own widget */
} xiaotai_ui_element_t;

/* One operable or readable control as the theme laid it out. */
typedef struct {
    int16_t x, y;
    uint16_t width, height;
    uint8_t layer;              /* xiaotai_view_layer_t */
    uint8_t element;            /* xiaotai_ui_element_t */
    uint16_t index;             /* of the element in its array; 0 for OTHER */
    uint8_t state;              /* XIAOTAI_STATE_* */
    char text[48];              /* label, or the icon name */
    char intent[32];            /* what operating it posts, cut to fit; "" when nothing */
} xiaotai_ui_control_t;

/* [any, not the UI task] Visible controls, top layer first. Returns how many
 * exist; at most `capacity` are written. Waits for the UI task up to
 * timeout_ms; XIAOTAI_ERR_TIMEOUT leaves *count 0. XIAOTAI_ERR_NOT_SUPPORTED:
 * the theme has no control list. */
xiaotai_err_t xiaotai_ui_controls(uint8_t display, xiaotai_ui_control_t *out, size_t capacity,
                                  size_t *count, uint32_t timeout_ms);

/* [any] Injected pointer for test benches; it takes the same path as the real
 * pointer (sleep gate included) and wins over it while active.
 * tap: 120 ms press. swipe: duration 100..2000 ms. pointer: one raw sample; a
 * press without a following sample is released after 1000 ms.
 * XIAOTAI_ERR_NOT_SUPPORTED: the display has no pointer;
 * XIAOTAI_ERR_INVALID_ARG: outside the display; XIAOTAI_ERR_INVALID_STATE: a
 * real touch is down or an earlier injection still runs. */
xiaotai_err_t xiaotai_ui_tap(uint8_t display, int16_t x, int16_t y);
xiaotai_err_t xiaotai_ui_swipe(uint8_t display, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                               uint32_t duration_ms);
xiaotai_err_t xiaotai_ui_pointer(uint8_t display, int16_t x, int16_t y, bool pressed);

typedef struct {
    uint16_t width, height;     /* logical, as the theme draws */
    /* RGB565: little-endian words, rows packed, whatever the panel's byte
     * order. One-bit displays: the display's own format (XIAOTAI_PIXEL_MONO1
     * or XIAOTAI_PIXEL_MONO1_VLSB) and frame layout. */
    xiaotai_pixel_t pixel;
    uint8_t *pixels;            /* XIAOTAI_MEM_BULK; release with xiaotai_ui_capture_free() */
    size_t size;
} xiaotai_ui_capture_t;

/* [any, not the UI task] Render the current scene once more into memory (not
 * a read-back of the panel). Blocks up to timeout_ms; a capture that finishes
 * after the timeout is discarded by the UI task and never touches *out.
 * Admitted only while no foreground activity is current: a capture renders a
 * whole extra frame and takes a frame of XIAOTAI_MEM_BULK, which must not
 * compete with a session. The condition is checked when the request is
 * submitted and again on the UI task right before rendering (an activity may
 * begin in between), both times from the flag the UI system keeps from
 * XIAOTAI_EVENT_ACTIVITY (so the console task can submit); both refusals
 * return XIAOTAI_ERR_BUSY and leave *out empty. XIAOTAI_ERR_NOT_SUPPORTED: the backend cannot capture;
 * XIAOTAI_ERR_INVALID_STATE: another capture is in progress. */
xiaotai_err_t xiaotai_ui_capture(uint8_t display, xiaotai_ui_capture_t *out, uint32_t timeout_ms);
void xiaotai_ui_capture_free(xiaotai_ui_capture_t *capture);   /* [any] */

/* [any] Draw and send the whole frame again; clears present_fault. */
xiaotai_err_t xiaotai_ui_refresh(uint8_t display);

#ifdef __cplusplus
}
#endif
