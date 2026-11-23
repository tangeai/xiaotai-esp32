// SPDX-License-Identifier: MIT
#pragma once
/* What a theme and a custom template implement, and what the UI system gives
 * them. A theme draws every generic view kind for one class of display; it is
 * chosen by the display descriptor, so a new display needs at most a new
 * theme. A custom template draws one application-specific view
 * (XIAOTAI_VIEW_CUSTOM) for the displays it supports.
 *
 * Division of work
 * - UI core (portable): copies views, diffs them, keeps focus, edit cursors,
 *   timers and the wall clock, applies the element states, posts intents,
 *   owns sleep. Themes hold no application state and decide nothing.
 * - Backend (per port): opens the display part and brings up the drawing
 *   library, honouring the display descriptor (render mode, byte swap, DMA
 *   buffers; with XIAOTAI_DISPLAY_EVEN_AREAS every presented area is widened
 *   to start on an even column and end on an odd one). Two backends exist; a
 *   theme names the one it draws with:
 *     LVGL   the port attaches LVGL to the panel and the pointer. The theme
 *            and templates build LVGL widgets; LVGL is portable C, so they
 *            compile for the host too.
 *     FRAME  the theme renders a whole frame in the display's pixel format
 *            and hands it over; nothing but this header is needed (small
 *            mono displays on chips without external RAM).
 * - Theme: turns a scene into widgets or pixels and reports which element the
 *   user operated. It never posts intents itself.
 *
 * All callbacks run on the display's UI task, one at a time (report() is the
 * one exception, see there); for a FRAME display without a task
 * (XIAOTAI_UI_TASK 0) that is the owner task, inside owner-task handlers.
 * They must not block, use the network or storage, or call
 * xiaotai_view_show().
 *
 * Slow panels: render() is a request to draw, not a promise that the panel
 * refreshes. The core keeps one pending view per layer and ORs the change
 * bits, so while a slow present() blocks the UI task, later views coalesce
 * into one render. The FRAME backend never sends a frame equal to the last
 * one and LVGL flushes only invalidated areas, so a theme that draws times
 * at minute granularity turns the once-a-second XIAOTAI_CHANGED_TIMER into
 * no panel traffic at all. A panel whose refresh is slow or whose partial
 * refresh is limited carries XIAOTAI_DISPLAY_SLOW_REFRESH in its descriptor
 * (xiaotai/hal.h). match() reads it like any other descriptor fact: a theme
 * written for such panels scores them, and a theme that animates, scrolls or
 * redraws every second scores them 0, so it is never chosen for one (the
 * themes of this SDK do; their headers say so). The build applies the same
 * rule to the manifests' display rules. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/port/err.h"
#include "xiaotai/ui.h"
#include "xiaotai/view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOTAI_UI_BACKEND_LVGL = 0,
    XIAOTAI_UI_BACKEND_FRAME,
} xiaotai_ui_backend_t;

/* One-bit frames (FRAME backend, captures) are width * height / 8 bytes in
 * the layout of the display's pixel format (xiaotai/hal.h); 1 is lit:
 *   XIAOTAI_PIXEL_MONO1_VLSB  pixel (x, y) is bit (y % 8) of byte
 *                             (y / 8) * width + x; height is a multiple of 8
 *   XIAOTAI_PIXEL_MONO1       pixel (x, y) is bit 7 - (x % 8) of byte
 *                             y * (width / 8) + x / 8; width is a multiple of 8
 * A FRAME theme renders in the layout of the display it was started on. */
#define XIAOTAI_UI_MONO_FRAME_BYTES(width, height) ((size_t)(width) * (size_t)(height) / 8U)

/* ------------------------------------------------------------------- scene */

/* What changed in a layer since the theme last rendered it. */
enum {
    XIAOTAI_CHANGED_IDENTITY = 1u << 0,   /* a new view (or none): rebuild the layer */
    XIAOTAI_CHANGED_TITLE = 1u << 1,      /* title, subtitle or status */
    XIAOTAI_CHANGED_TEXT = 1u << 2,       /* text or speaker */
    XIAOTAI_CHANGED_EMOTION = 1u << 3,    /* emotion, phase or level */
    XIAOTAI_CHANGED_ITEMS = 1u << 4,
    XIAOTAI_CHANGED_ACTIONS = 1u << 5,
    XIAOTAI_CHANGED_FIELDS = 1u << 6,     /* fields or the edit cursor */
    XIAOTAI_CHANGED_INDICATORS = 1u << 7,
    XIAOTAI_CHANGED_TIMER = 1u << 8,      /* timer_s below moved (once a second) */
    XIAOTAI_CHANGED_PROGRESS = 1u << 9,
    XIAOTAI_CHANGED_FOCUS = 1u << 10,
    XIAOTAI_CHANGED_IMAGE = 1u << 11,
    XIAOTAI_CHANGED_CUSTOM = 1u << 12,    /* custom_data bytes */
    XIAOTAI_CHANGED_FLAGS = 1u << 13,
};

typedef enum {
    XIAOTAI_UI_FOCUS_NONE = 0,   /* pointer use, or nothing to focus */
    XIAOTAI_UI_FOCUS_ITEM,
    XIAOTAI_UI_FOCUS_FIELD,
} xiaotai_ui_focus_t;

/* One layer as the theme sees it. `view` is the core's copy: its pointers
 * stay valid and unchanged until the next render() of this display. */
typedef struct {
    bool present;                 /* false: the layer shows nothing; view is zeroed */
    uint32_t changed;             /* XIAOTAI_CHANGED_* */
    xiaotai_view_t view;
    int32_t timer_s;              /* the running timer; view.timer_s is the value at show */
    bool timer_expired;           /* REMAINING reached 0 */
    uint8_t focus_kind;           /* xiaotai_ui_focus_t */
    uint16_t focus;               /* index of the focused item or field */
    uint8_t cursor;               /* DIGITS field in focus: the digit being edited */
} xiaotai_ui_layer_t;

typedef struct {
    bool valid;                   /* false until the clock was set (draw "--:--") */
    uint16_t year;
    uint8_t month, day;           /* 1..12, 1..31 */
    uint8_t weekday;              /* 0 Sunday .. 6 */
    uint8_t hour, minute, second;
} xiaotai_ui_clock_t;

typedef struct {
    xiaotai_ui_layer_t page;
    xiaotai_ui_layer_t overlay;
    xiaotai_ui_layer_t toast;
    xiaotai_ui_clock_t clock;     /* local wall time */
    bool clock_changed;           /* the minute (or validity) changed since the last render */
    bool video;                   /* a video source is installed for this display */
    uint32_t now_ms;              /* monotonic, wraps */
} xiaotai_ui_scene_t;

/* -------------------------------------------------------------------- host */

/* The services of one display, given to its theme at start and to templates. */
typedef struct xiaotai_ui_host xiaotai_ui_host_t;

typedef enum {
    XIAOTAI_UI_INPUT_TAP = 0,
    XIAOTAI_UI_INPUT_PRESS,       /* pointer went down on the element */
    XIAOTAI_UI_INPUT_RELEASE,     /* pointer released or lost */
    XIAOTAI_UI_INPUT_LONG_PRESS,
    XIAOTAI_UI_INPUT_SWIPE_LEFT,  /* layer-wide gestures: index is ignored */
    XIAOTAI_UI_INPUT_SWIPE_RIGHT,
    XIAOTAI_UI_INPUT_SWIPE_UP,
    XIAOTAI_UI_INPUT_SWIPE_DOWN,
    XIAOTAI_UI_INPUT_CHANGE,      /* a field's value while the control is still held (value/text below) */
    XIAOTAI_UI_INPUT_COMMIT,      /* a field's final value */
    XIAOTAI_UI_INPUT_HOLD_REFRESH, /* driver still observes the captured control held */
    XIAOTAI_UI_INPUT_CONFIRM_CHOICE, /* user explicitly picked the current option in an open list */
} xiaotai_ui_input_t;

const xiaotai_display_desc_t *xiaotai_ui_host_display(const xiaotai_ui_host_t *host);   /* [ui] */
uint8_t xiaotai_ui_host_index(const xiaotai_ui_host_t *host);                           /* [ui] display index */
/* [ui] The backend's drawing root. LVGL: the lv_display_t * of this display
 * (LVGL calls need no lock on the UI task). FRAME: NULL. */
void *xiaotai_ui_host_root(const xiaotai_ui_host_t *host);

/* [ui] The user operated an element of a layer. The core checks the element
 * against the current scene (index, state, trigger), posts the intent with
 * the arguments view.h defines, drives HOLD repeats, moves focus and cursors,
 * restarts the sleep timeout and counts what it ignores. `value` is used for
 * CHANGE, COMMIT and CONFIRM_CHOICE; `text` may hold a DIGITS field's commit.
 * Reporting an element that no longer exists is safe. */
void xiaotai_ui_host_input(const xiaotai_ui_host_t *host, xiaotai_view_layer_t layer,
                           xiaotai_ui_element_t element, uint16_t index, xiaotai_ui_input_t input,
                           int32_t value, const char *text);

/* [ui] For custom templates whose controls are not elements of the view: post
 * an intent directly. Same gates as above except the element checks. */
void xiaotai_ui_host_post(const xiaotai_ui_host_t *host, const char *intent, int32_t value, const char *text);

/* [ui] Ask for render() soon although no view changed (an animation step, a
 * finished transition). Coalesced. */
void xiaotai_ui_host_invalidate(const xiaotai_ui_host_t *host);

/* [ui] FRAME backend: hand over one whole frame in the display's pixel format
 * and layout (one-bit: XIAOTAI_UI_MONO_FRAME_BYTES). The frame is borrowed for
 * the call; the core keeps its own copy of the last frame sent (for the
 * comparison and for captures).
 * An unchanged frame is not sent again. After a failure the display is marked
 * faulted and further frames are not sent until xiaotai_ui_refresh(); the
 * failure is returned once and counted. XIAOTAI_ERR_NOT_SUPPORTED on the LVGL
 * backend. */
xiaotai_err_t xiaotai_ui_host_present(const xiaotai_ui_host_t *host, const uint8_t *frame, size_t size);

/* [ui] LVGL backend: let `paint` draw the pixels of one LVGL image widget
 * (`widget` is its lv_obj_t *) straight into the frame being rendered, instead
 * of LVGL blending a source image: a procedural face, the video region. paint
 * returns false to fall back to LVGL's own drawing of the widget's source (it
 * must, when the target is not plain RGB565 or the widget is transformed; the
 * backend already falls back in those cases without calling it). One painter
 * per widget; NULL removes it; removing the widget removes it.
 * XIAOTAI_ERR_NOT_SUPPORTED on the FRAME backend, XIAOTAI_ERR_NO_MEM above
 * XIAOTAI_UI_PAINTERS_MAX (xiaotai/ui_config.h). */
typedef bool (*xiaotai_ui_paint_fn_t)(void *ctx, const xiaotai_ui_surface_t *target,
                                      int16_t widget_x, int16_t widget_y);
xiaotai_err_t xiaotai_ui_host_painter(const xiaotai_ui_host_t *host, void *widget,
                                      xiaotai_ui_paint_fn_t paint, void *ctx);
/* [ui] Like host_painter. If supplied, prepare_fallback refreshes the image
 * source on the display task just before a software draw (including capture).
 * It must not change the widget, invalidate anything, or retain a draw task. */
typedef void (*xiaotai_ui_paint_prepare_fn_t)(void *ctx);
xiaotai_err_t xiaotai_ui_host_painter_ex(const xiaotai_ui_host_t *host, void *widget,
                                       xiaotai_ui_paint_fn_t paint,
                                       xiaotai_ui_paint_prepare_fn_t prepare_fallback, void *ctx);

/* [ui] LVGL backend: attach the display's video source to an image widget the
 * theme placed as the video region; the backend polls the source and paints
 * through it. NULL widget detaches. Themes with a video region call it when
 * they build a view with XIAOTAI_VIEW_VIDEO. */
xiaotai_err_t xiaotai_ui_host_video_region(const xiaotai_ui_host_t *host, void *widget);

/* ------------------------------------------------------------------- theme */

typedef struct xiaotai_theme {
    /* "lcd320"; at most 15 characters. A theme component registers its theme
     * under its manifest name without "theme_" (theme_lcd320 -> "lcd320"),
     * so a product's configured name can be checked at build time. */
    const char *name;
    xiaotai_ui_backend_t backend;
    /* Score for a display: 0 cannot draw it, higher is better (exact
     * resolution 100, scaled 50..99). Pure; may be called on any task. */
    int (*match)(const xiaotai_display_desc_t *display);
    /* Once per display it serves, after the backend is up. Build what lives
     * for the whole run; *state is handed back to the callbacks below, so one
     * theme can serve several displays. An error leaves the display without a
     * theme. */
    xiaotai_err_t (*start)(const xiaotai_ui_host_t *host, void **state);
    /* Draw the scene. Called when a layer changed, the clock or a timer moved,
     * the theme asked (invalidate) and after wake. Layers whose `changed` is 0
     * need no work. When memory runs out it draws less but must leave the
     * display consistent; it reports nothing. */
    void (*render)(void *state, const xiaotai_ui_scene_t *scene);
    /* Optional. One animation step; returns the delay in ms until it wants
     * the next call, 0 for none until the next render. Not called while the
     * screen sleeps. */
    uint32_t (*tick)(void *state, uint32_t now_ms);
    /* Optional. The screen went to sleep or woke: pause and resume timers. */
    void (*sleep)(void *state, bool asleep);
    /* Optional. Visible controls for diagnostics, top layer first; returns
     * how many exist. The theme fills where each control is, its layer and
     * which element of the view it is (element, index); for a control that is
     * no element (XIAOTAI_UI_ELEMENT_OTHER) it also fills state and text. The
     * core fills state, text and intent of the elements from the scene. */
    size_t (*controls)(void *state, xiaotai_ui_control_t *out, size_t capacity);
    /* Optional. Release everything (host tests). */
    void (*stop)(void *state);
    /* Bytes of UI task stack the theme needs for its deepest callback. A
     * FRAME theme that asks for at most XIAOTAI_UI_OWNER_THEME_STACK_BYTES
     * can be run without a UI task (XIAOTAI_UI_TASK 0). */
    uint32_t stack_bytes;

    /* Members below default to "absent" when the structure is zeroed. */

    /* Optional. The theme's own counters for the "display" diagnostics
     * provider (the face: frames late or slow, direct paints and their
     * pixels, canvas fallbacks), written as "key=value" words separated by
     * spaces, without a line end, at most capacity bytes with the terminator.
     * Unlike the callbacks above it runs on the owner task while the UI task
     * keeps drawing: it reads only word-sized counters the theme updates
     * with plain stores, never other theme state, and it does not block. */
    void (*report)(void *state, char *line, size_t capacity);
    /* Opt in to wall-clock updates on HOME while it is not covered by an
     * overlay. CLOCK views use the same clock regardless of this option. */
    bool clock_on_home;
} xiaotai_theme_t;

/* Theme stack allowance where a display has no UI task (XIAOTAI_UI_TASK 0):
 * an owner-task handler may use XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES (3072)
 * with everything it calls, and the core's own frames around the theme's
 * callbacks (timer step, scene, present) take up to 1024 of them. A theme
 * that states more gets a UI task. Host tests measure the whole step against
 * the 3072 bytes. */
#define XIAOTAI_UI_OWNER_THEME_STACK_BYTES 2048U

/* [init] Add a theme to the registry. The structure is kept by pointer.
 * XIAOTAI_ERR_INVALID_STATE: the name is taken or the UI already started;
 * XIAOTAI_ERR_NO_MEM: more than XIAOTAI_UI_THEMES_MAX (xiaotai/ui_config.h). */
xiaotai_err_t xiaotai_ui_theme_register(const xiaotai_theme_t *theme);
size_t xiaotai_ui_theme_count(void);                              /* [any] */
const xiaotai_theme_t *xiaotai_ui_theme_at(size_t index);         /* [any] NULL past the end */
/* [any] The registered theme with the highest score for a display; on a tie
 * the one registered first. NULL when none scores above 0. */
const xiaotai_theme_t *xiaotai_ui_theme_select(const xiaotai_display_desc_t *display);

/* --------------------------------------------------------- custom templates */

/* Where a template draws. LVGL: `root` is the lv_obj_t * container the theme
 * made for the layer's content, in native display pixels and never scaled
 * (the template creates children; the container is deleted with the view).
 * FRAME: `root` is the frame buffer the theme will present, in the display's
 * layout. A template serves the PAGE and the OVERLAY layer alike: a custom
 * view shown as an overlay gets the overlay's container. */
typedef struct {
    xiaotai_ui_backend_t backend;
    void *root;
    uint16_t width, height;       /* of the area in display pixels */
} xiaotai_ui_canvas_t;

/* An application-specific view for the displays it supports. Several
 * templates may carry the same name (one per display class); the highest
 * score for the display wins. */
typedef struct xiaotai_ui_template {
    const char *name;             /* what xiaotai_view_t.custom names: "door.home" */
    xiaotai_ui_backend_t backend;
    int (*match)(const xiaotai_display_desc_t *display);   /* 0: not for this display */
    /* The view appeared (layer->changed has IDENTITY): build it. *state is
     * handed back to the other callbacks. An error draws the view as NOTICE. */
    xiaotai_err_t (*open)(const xiaotai_ui_host_t *host, const xiaotai_ui_canvas_t *canvas,
                          const xiaotai_ui_layer_t *layer, void **state);
    /* The same view changed (layer->changed), the clock moved, or the canvas
     * was cleared (FRAME: every frame). */
    void (*update)(void *state, const xiaotai_ui_canvas_t *canvas, const xiaotai_ui_layer_t *layer,
                   const xiaotai_ui_scene_t *scene);
    /* The view is gone. LVGL widgets under canvas->root are deleted by the
     * theme afterwards. */
    void (*close)(void *state);
    /* Optional. Draw a toast in the template's own style while its view is
     * the page; return false to let the theme draw it. text NULL hides it. */
    bool (*toast)(void *state, const char *text);
    /* Optional. Controls for diagnostics, filled as xiaotai_theme_t.controls. */
    size_t (*controls)(void *state, xiaotai_ui_control_t *out, size_t capacity);
} xiaotai_ui_template_t;

/* [init] XIAOTAI_ERR_NO_MEM above XIAOTAI_UI_TEMPLATES_MAX
 * (xiaotai/ui_config.h). Kept by pointer. */
xiaotai_err_t xiaotai_ui_template_register(const xiaotai_ui_template_t *template_);
/* [ui] For themes: the template to use for a custom view on this host's
 * display, or NULL (draw NOTICE). */
const xiaotai_ui_template_t *xiaotai_ui_template_find(const xiaotai_ui_host_t *host, const char *name);

#ifdef __cplusplus
}
#endif
