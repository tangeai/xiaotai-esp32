// SPDX-License-Identifier: MIT
#pragma once
/* The colour theme kit: the one implementation of the generic view kinds for
 * RGB565 displays drawn with LVGL, shared by every colour theme. Pages are
 * laid out on a logical 320x240 canvas; the kit scales positions and sizes to
 * the display it was started on (horizontally by width / 320, vertically by
 * height / 240; 1:1 on a 320x240 display). Fonts, alignment offsets and
 * extended click areas are in display pixels and are not scaled. The emotion
 * takes as many pixels as the vertical factor gives it and is drawn in them:
 * 240x124 of a 320x240 display, 320x165 of 480x320, 600x310 of 1024x600.
 * Custom templates get their container in display pixels, unscaled.
 *
 * A theme built on the kit is a registration (name, match, stack) whose
 * callbacks forward to the functions below, plus a variant that says what it
 * adds. xiaotai_theme_lcd320 is the kit with no additions; larger displays
 * add a background, the clock on HOME and a video region.
 *
 * The kit is a component of its own (xiaotai_theme_lcd_kit, lifecycle unit
 * "theme_lcd_kit") that every colour theme depends on, so no theme drags
 * another theme into a product. It registers no theme and its manifest has
 * no display rule: it takes no part in the build-time display check. It
 * alone carries the LVGL configuration the colour products share
 * (sdkconfig.defaults: only values every colour product has today; what
 * differs per product stays in the product's configuration).
 *
 * Emotions: the procedural face accepts the names below; anything else is
 * drawn as "neutral". Only the set of names is part of this contract, not
 * their order: an application that stores an emotion keeps its own index
 * table (xiaotai/view.h), since applications do not include themes.
 *   neutral happy laughing funny sad angry crying loving embarrassed
 *   surprised shocked thinking winking cool relaxed delicious kissy confident
 *   sleepy silly confused listening ambient
 * Aliases: calm=neutral excited=laughing curious=confused proud=confident
 * moved=loving speech=listening gentle=relaxed comfort=loving
 * serious=confident.
 *
 * LVGL objects cross this interface as void * (lv_obj_t *), so the header
 * needs no drawing library. Tags as in xiaotai/ui.h: every function is [ui]
 * (the display's UI task) except xiaotai_theme_lcd_report(), which is [owner]
 * as xiaotai_theme_t.report is, xiaotai_theme_lcd_emotion_name(), which is
 * [any], and the lifecycle init, which the framework calls. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_THEME_LCD_LOGICAL_WIDTH 320U
#define XIAOTAI_THEME_LCD_LOGICAL_HEIGHT 240U
/* UI task stack a theme built on the kit asks for (xiaotai_theme_t.stack_bytes). */
#define XIAOTAI_THEME_LCD_STACK_BYTES 16384U
/* Number of emotion names above (aliases not counted). */
#define XIAOTAI_THEME_LCD_EMOTION_COUNT 23U

/* xiaotai_theme_lcd_variant_t.features */
enum {
    /* HOME shows the wall clock beside the status. */
    XIAOTAI_THEME_LCD_HOME_CLOCK = 1u << 0,
    /* The emotion is rasterized straight into the frame being rendered, and
     * what changed of it is sent to the panel without a refresh where the
     * display allows (xiaotai_ui_host_paint()). Otherwise, and whenever LVGL
     * transforms or masks the widget, it is drawn into its canvas and blended
     * by LVGL. */
    XIAOTAI_THEME_LCD_DIRECT_EMOTION = 1u << 1,
    /* Use the shared procedural expression renderer on HOME and TALK. */
    XIAOTAI_THEME_LCD_EMOTION = 1u << 2,
};

typedef struct {
    uint32_t features;        /* XIAOTAI_THEME_LCD_* */
    /* Optional. A generic page was built for a new identity. Paint its
     * background under this full-display container (lv_obj_t *), behind
     * both video and controls. Its children are deleted with the page. */
    void (*page_built)(void *self, const xiaotai_ui_host_t *host, void *background, const xiaotai_ui_layer_t *layer);
    /* Optional. Create a display-sized image (lv_obj_t *) under the
     * persistent root. The kit registers it hidden before showing pages,
     * then places it behind controls only for views showing video. A
     * supplied callback returning NULL fails theme startup. */
    void *(*video_region)(void *self, const xiaotai_ui_host_t *host, void *root);
    /* Optional. Release variant resources after its LVGL widgets are gone. */
    void (*stop)(void *self);
    void *self;
    /* Optional flat RGB888 color behind the expression, after page_built.
     * Return false to use the kit background (also used when an optional
     * page background could not be built). */
    bool (*emotion_background)(void *self, const xiaotai_view_t *view, uint32_t *rgb);
} xiaotai_theme_lcd_variant_t;

/* [ui] The implementations of xiaotai_theme_t's callbacks, called where the
 * UI system calls those (xiaotai/ui_theme.h: start, render, tick, sleep,
 * controls and stop on the UI task). `variant` is kept by pointer for the
 * life of the state (NULL: no additions).
 * XIAOTAI_ERR_NOT_SUPPORTED: the display is not RGB565 or is smaller than the
 * logical canvas; XIAOTAI_ERR_NO_MEM: the theme or its video image could
 * not be created. On failure the caller retains any variant resources. */
xiaotai_err_t xiaotai_theme_lcd_start(const xiaotai_ui_host_t *host, const xiaotai_theme_lcd_variant_t *variant,
                                      void **state);
void xiaotai_theme_lcd_render(void *state, const xiaotai_ui_scene_t *scene);
uint32_t xiaotai_theme_lcd_tick(void *state, uint32_t now_ms);
void xiaotai_theme_lcd_sleep(void *state, bool asleep);
size_t xiaotai_theme_lcd_controls(void *state, xiaotai_ui_control_t *out, size_t capacity);
void xiaotai_theme_lcd_stop(void *state);
/* [owner, see xiaotai_theme_t.report] The face counters: face_late= (frame
 * gaps over 80 ms) face_slow= (frames computed in over 8 ms) direct=
 * direct_pixels= (direct paints and the pixels they wrote) fallback=
 * (paints that fell back to the canvas). */
void xiaotai_theme_lcd_report(void *state, char *line, size_t capacity);

/* [any] The accepted emotion names, for tests and diagnostic listings; NULL
 * past the end. The order is not a storage format and may change. */
const char *xiaotai_theme_lcd_emotion_name(size_t index);

/* ------------------------------------------------------------------ actors */

/* An actor draws the emotion of HOME and TALK in place of the kit's built-in
 * face, on the displays it scores. It is a component of its own that
 * registers itself (a full character for large displays is one); the kit
 * knows only this interface, so a product gets an actor by selecting its
 * unit and every other product is built without it. A theme takes part by
 * having XIAOTAI_THEME_LCD_EMOTION in its variant, as for the face.
 *
 * The actor owns one widget, as large as the page, under the page's
 * controls; it takes no pointer events (the tap on the emotion stays the
 * kit's: hit() says where it lands). All callbacks are [ui]. */
typedef struct {
    uint16_t width, height;   /* the page in display pixels */
    bool raised;              /* XIAOTAI_VIEW_EMOTION_PREVIEW: stand higher, the page shows only the emotion */
} xiaotai_theme_lcd_stage_t;

typedef struct xiaotai_theme_lcd_actor {
    const char *name;
    /* Score for a display: 0 is not for it; the highest score wins, the
     * built-in face draws when none scores. Pure. */
    int (*match)(const xiaotai_display_desc_t *display);
    /* Once per display it serves. An error leaves the built-in face. */
    xiaotai_err_t (*start)(const xiaotai_ui_host_t *host, void **state);
    void (*stop)(void *state);
    /* A HOME or TALK page was built: create the widget (lv_obj_t *) under
     * parent. NULL: the page shows no emotion. The kit deletes the widget
     * with the page after detach(). */
    void *(*build)(void *state, void *parent, const xiaotai_theme_lcd_stage_t *stage);
    void (*place)(void *state, const xiaotai_theme_lcd_stage_t *stage);
    void (*detach)(void *state);
    /* The view's emotion, phase or level changed. */
    void (*set)(void *state, const xiaotai_view_t *view, uint32_t now_ms);
    /* One animation step; returns the delay until the next. `shown` is false
     * while the page has no widget of the actor's (after detach()) or an
     * overlay covers it (the widget is hidden meanwhile). A sleeping display
     * is not ticked: see sleep(). */
    uint32_t (*tick)(void *state, uint32_t now_ms, bool shown);
    /* Optional. The screen went to sleep or woke. */
    void (*sleep)(void *state, bool asleep);
    /* The area a tap on the emotion covers, in display pixels. */
    xiaotai_ui_rect_t (*hit)(void *state);
    /* Optional. As xiaotai_theme_t.report. */
    void (*report)(void *state, char *line, size_t capacity);
} xiaotai_theme_lcd_actor_t;

enum { XIAOTAI_THEME_LCD_ACTORS_MAX = 4 };
/* [init] Kept by pointer. XIAOTAI_ERR_NO_MEM above XIAOTAI_THEME_LCD_ACTORS_MAX. */
xiaotai_err_t xiaotai_theme_lcd_actor_register(const xiaotai_theme_lcd_actor_t *actor);
/* [any] The actor for a display, or NULL. */
const xiaotai_theme_lcd_actor_t *xiaotai_theme_lcd_actor_select(const xiaotai_display_desc_t *display);
/* Host tests: forget every registration. */
void xiaotai_theme_lcd_actor_reset(void);

/* Lifecycle unit "theme_lcd_kit" (theme.json). The manifest format gives
 * every unit an init; the kit has nothing to set up (its state lives in the
 * themes that start it), so this returns XIAOTAI_OK without allocating or
 * registering anything. */
xiaotai_err_t xiaotai_engine_theme_lcd_kit_init(void);

#ifdef __cplusplus
}
#endif
