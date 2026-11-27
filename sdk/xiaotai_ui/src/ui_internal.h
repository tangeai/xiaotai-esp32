// SPDX-License-Identifier: MIT
#pragma once
/* Internal state of the UI system, shared by the core (ui_core.c), the view
 * store (ui_store.c), the registries (ui_registry.c) and the parts that build
 * on them (scene, input, task, backends).
 *
 * View slots. Each display has one slot per layer. A slot holds two copies:
 * `staged`, written by the owner task in xiaotai_view_show(), and `current`,
 * what the theme draws. Both live in one XIAOTAI_MEM_BULK block. A show
 * overwrites the staged copy (depth 1: the newest view wins); the UI task
 * takes it with ui_layers_take(), which computes the change bits of the
 * staged copy against the current one, ORs in an identity change that
 * happened in between (A, then B, then A again still rebuilds), swaps the two
 * copies and applies expiry. Everything in a slot is guarded by the engine
 * lock; the copy itself runs under it and is bounded by the slot capacity.
 *
 * Pointer lifetime: after ui_layers_take() swapped a slot, the copy the theme
 * drew before is the next staged copy and may be overwritten by the owner at
 * once. The caller of ui_layers_take() therefore renders right after it, and
 * no theme code may read the previous scene in between (view pointers stay
 * valid "until the next render", xiaotai/ui_theme.h). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai/ui_theme.h"
#include "xiaotai/ui_owner_frame.h"
#include "xiaotai/view.h"
#include "ui_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_LAYER_COUNT 3U
#define UI_NAV_DEPTH 8U

/* Layer buffer defaults of xiaotai_ui_config_t.layer_bytes. */
#define UI_LAYER_BYTES_EXTERNAL 8192U
#define UI_LAYER_BYTES_INTERNAL 1536U

/* Longest theme name, without the terminator (xiaotai_theme_t.name). */
#define UI_THEME_NAME_MAX 15U

/* Every content bit of XIAOTAI_CHANGED_*: a new identity (or a layer that
 * appears or disappears) sets all of them with XIAOTAI_CHANGED_IDENTITY, so a
 * theme that tests single bits also redraws everything. */
#define UI_CHANGED_ALL                                                                                 \
    (XIAOTAI_CHANGED_IDENTITY | XIAOTAI_CHANGED_TITLE | XIAOTAI_CHANGED_TEXT | XIAOTAI_CHANGED_EMOTION | \
     XIAOTAI_CHANGED_ITEMS | XIAOTAI_CHANGED_ACTIONS | XIAOTAI_CHANGED_FIELDS |                          \
     XIAOTAI_CHANGED_INDICATORS | XIAOTAI_CHANGED_TIMER | XIAOTAI_CHANGED_PROGRESS |                     \
     XIAOTAI_CHANGED_FOCUS | XIAOTAI_CHANGED_IMAGE | XIAOTAI_CHANGED_CUSTOM | XIAOTAI_CHANGED_FLAGS)

/* ------------------------------------------------------------------- store */

/* One copied view: the structure, whose pointers lead into `arena`. */
typedef struct {
    xiaotai_view_t view;
    bool present;              /* false: the layer shows nothing; view is zeroed */
    bool has_deadline;         /* toast, or overlay with duration_ms */
    uint32_t deadline_ms;      /* removed when the clock reaches it (wrapping compare) */
    uint32_t shown_ms;
    uint8_t *arena;
} ui_copy_t;

/* Copy `view` for `layer` into `copy` (arena of `capacity` bytes), applying
 * the limits of xiaotai/view.h and, when the arena runs out, the retention
 * order: identity (custom, id), header (title, subtitle, status, emotion,
 * image), custom_data, actions, fields, indicators, item labels (with intent
 * and icon), item details, and the body text last, cut from its tail. The
 * first member that does not fit ends the copy: nothing after it is kept, so
 * a short member of a later group never takes room an earlier one lacked. A
 * TOAST copy keeps only id and text. Strings are cut at a UTF-8 character
 * boundary; names that select something (custom, id, emotion, image, intent,
 * icon) are kept whole or dropped, never cut, so a cut never posts another
 * intent. Returns the number of cuts (each string, array or custom_data
 * shortened or dropped counts once). The copy does not touch has_deadline. */
uint32_t ui_store_copy(ui_copy_t *copy, size_t capacity, const xiaotai_view_t *view, xiaotai_view_layer_t layer);

/* Same identity (kind, custom, id) of a source view, as the store would keep
 * it in a copy of `capacity` bytes, and an existing copy. The layer is the
 * slot's, so it is equal by construction. */
bool ui_store_same_identity(const xiaotai_view_t *source, size_t capacity, xiaotai_view_layer_t layer,
                            const xiaotai_view_t *copy);

/* XIAOTAI_CHANGED_* between two copies of the same identity: one bit per
 * group of members that differ by content (strings by bytes, arrays element
 * by element). Equal content gives 0, so a theme never sets the same text
 * again. */
uint32_t ui_store_diff(const xiaotai_view_t *before, const xiaotai_view_t *after);
bool ui_store_same_controls(const xiaotai_view_t *before, const xiaotai_view_t *after);

/* Length of the longest prefix of `text` (length `length`) that is at most
 * `limit` bytes and ends on a UTF-8 character boundary. */
size_t ui_store_utf8_prefix(const char *text, size_t length, size_t limit);

/* -------------------------------------------------------------------- slots */

typedef struct {
    uint8_t *block;            /* both arenas, XIAOTAI_MEM_BULK; NULL until first used */
    uint32_t capacity;         /* bytes of each arena */
    ui_copy_t copies[2];
    ui_copy_t *staged;
    ui_copy_t *current;
    bool pending;              /* staged is newer than current */
    bool identity_changed;     /* an identity change was accepted since the last take */
    bool shown;                /* a view (not a removal) reached the slot since the last take */
} ui_slot_t;

/* What ui_layers_take() reports per layer beyond the scene's members. */
typedef struct {
    bool shown;                /* a view reached the layer since the last take (a focus request
                                  of an unchanged view still applies) */
    uint32_t shown_ms;
    bool focus_changed;
    bool press_changed;
} ui_take_t;

/* --------------------------------------------------------------- displays */

typedef struct {
    uint32_t shown;
    uint32_t updated;
    uint32_t dropped;
    uint32_t truncated;
    uint32_t ignored;
    uint32_t posted, rejected;
} ui_counters_t;

typedef enum { UI_NAV_NEXT, UI_NAV_PREVIOUS, UI_NAV_CONFIRM, UI_NAV_BACK } ui_nav_t;

typedef struct {
    uint32_t timer_sample_ms;
    uint64_t timer_elapsed_ms;
    bool timer_source;
    char digits[XIAOTAI_VIEW_DIGITS_MAX + 1U];
} ui_scene_layer_t;

typedef struct {
    xiaotai_view_layer_t layer;
    xiaotai_ui_element_t element;
    uint16_t index;
    bool down, suppressed, swiped, long_fired, release_pending;
    bool hold_from_pointer;
    char hold_intent[XIAOTAI_ARG_TEXT_MAX];
    char *hold_name_long;
    int32_t hold_value;
    uint32_t hold_due_ms;
    uint32_t hold_ticket;
} ui_press_t;

struct xiaotai_ui_host {
    uint8_t index;
    const xiaotai_display_t *part;
    void *root;                /* LVGL: lv_display_t *; FRAME: NULL */
};

typedef enum {
    UI_DISPLAY_IDLE = 0,       /* initialized, not started: views are kept */
    UI_DISPLAY_READY,          /* part open and theme running */
    UI_DISPLAY_CLOSED,         /* no theme, or opening failed: views are dropped */
} ui_display_state_t;

typedef struct ui_power_request ui_power_request_t;
typedef struct ui_diag_request ui_diag_request_t;

typedef struct {
    xiaotai_task_t task;
    xiaotai_signal_t lifecycle;
    const char *start_stage;
    xiaotai_err_t start_result;
    bool driven, owner_timer, stop, stopped, kick_pending;
    uint32_t id, due, tick_due, pointer_due, periodic_due, periodic_source;
    uint32_t unpainted[UI_LAYER_COUNT];
    bool tick, periodic, rebased, busy;
} ui_driver_t;

typedef struct {
    ui_power_request_t *request;
    uint32_t active_at, retry_due;
    uint8_t brightness;
    bool awake, activity_pending, retry, pointer, suppressed, redraw, force_frame;
} ui_power_t;

typedef enum { UI_POINTER_IDLE, UI_POINTER_GESTURE, UI_POINTER_RAW } ui_pointer_mode_t;

typedef struct {
    xiaotai_touch_point_t point, start, end;
    uint32_t begun_at, duration;
    ui_pointer_mode_t mode;
    bool real_down, down, sample_pending, requested_down;
} ui_pointer_t;

#define UI_SAMPLES 16U
#define UI_CONTACTS (2U * UI_SAMPLES + 1U)

/* Where the time of a touch goes, for AT+UI PERF: how long reading the
 * controller took, how far apart the reads were, one UI step, the press until
 * its first frame, the release until its intent, the intent until the
 * application's next view, that view until it was painted. */
typedef enum {
    UI_SPAN_READ, UI_SPAN_POLL, UI_SPAN_STEP, UI_SPAN_PRESS, UI_SPAN_ACTION, UI_SPAN_APP, UI_SPAN_PAGE, UI_SPAN_COUNT
} ui_span_kind_t;

typedef struct { uint32_t count, max_us; uint64_t total_us; } ui_span_t;

typedef struct {
    ui_span_t span[UI_SPAN_COUNT];
    int64_t polled, pressed, released, posted, shown;   /* 0: not waiting for the next mark */
} ui_trace_t;

/* A display with touch has one, from open to stop. */
typedef struct {
    ui_touch_t filter;          /* owned by the UI step, like the queue */
    ui_contact_t queue[UI_CONTACTS];
    size_t queued;              /* what this step hands to the toolkit */
    bool contact, ended;        /* the last one was down; one ended in this step */
    ui_trace_t trace;           /* the rest is under the engine lock */
    ui_sample_t ring[UI_SAMPLES];
    size_t count;
    xiaotai_task_t task;
    xiaotai_signal_t done;
    uint32_t stack_free;
    bool sampling, stop;
} ui_touch_state_t;

typedef struct {
    uint8_t *last;
    uint8_t *native;              /* allocated only if the display needs software rotation */
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    uint8_t *owner_workspace;
    size_t owner_workspace_bytes;
#endif
#if XIAOTAI_UI_CAPTURE
    xiaotai_ui_capture_t *capture;  /* temporary render destination, display context only */
#endif
    size_t bytes;
    size_t native_bytes;
    xiaotai_rect_t area;
    bool valid, fault;
    uint32_t errors;
} ui_frame_t;

typedef struct {
    xiaotai_ui_host_t host;
    const xiaotai_display_t *part;
    ui_display_state_t state;
    ui_slot_t slots[UI_LAYER_COUNT];
    const xiaotai_theme_t *theme;
    void *theme_state;
    xiaotai_display_handles_t handles;
#if XIAOTAI_UI_LVGL
    xiaotai_err_t native_cleanup_error;
#endif
    bool theme_refused;
    ui_counters_t counters;    /* under the engine lock */
    uint8_t nav_queue[UI_NAV_DEPTH], nav_head, nav_count; /* under the engine lock */
    xiaotai_ui_scene_t scene;  /* owned by the UI step */
    ui_scene_layer_t scene_layer[UI_LAYER_COUNT];
    uint32_t scene_pending[UI_LAYER_COUNT];
    ui_press_t press;
    uint32_t swipe_at[2], activity_at;
    bool swipe_seen[2], activity_seen;
    uint32_t input_epoch;     /* bumped on page/focused-entry transitions, under lock */
    uint32_t rejection_scope, rendered_rejection_scope;
    struct {
        uint32_t ticket, scope, due, expiry;
        char intent[XIAOTAI_ARG_TEXT_MAX];
        bool pending;
    } rejected;              /* one notice per display, under the UI lock */
    uint32_t nav_epoch[UI_NAV_DEPTH];
    uint32_t clock_due_ms;
    bool clock_due;
    uint32_t periodic_source;  /* advances when a timer or clock source changes */
    ui_driver_t driver;
    ui_power_t power;
    ui_pointer_t pointer;
    ui_touch_state_t *touch;
    ui_frame_t frame;
    ui_diag_request_t *controls_request;
#if XIAOTAI_UI_CAPTURE
    ui_diag_request_t *capture_request;
#endif
    uint32_t frames, slow_frames, max_frame_ms, late_steps, stack_free;
    uint32_t video_stale;
#if XIAOTAI_UI_LVGL
    void *lvgl;
    void *drawing;
    void (*closed_video_release)(void); /* reset owner releases after driver joined */
#endif
    uint32_t flushes, flush_waits;
    uint64_t flush_pixels, flush_bytes, flush_us, render_ms;
    size_t controls_visible;
    uint16_t focus_visible;
} ui_display_t;

/* ------------------------------------------------------------------ engine */

typedef struct {
    bool initialized;
    bool started;
    bool lock_ready;
    xiaotai_mutex_t lock;
    uint32_t layer_bytes;
    uint32_t generation;
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    xiaotai_ui_owner_frame_fn owner_frame_provider;
#endif

    const xiaotai_theme_t **themes;
    size_t theme_count;
    const xiaotai_ui_template_t **templates;
    size_t template_count;
    const xiaotai_ui_image_t **images;
    size_t image_count;

    ui_display_t *displays;
    size_t display_count;
    uint32_t displays_closed;  /* displays left without a theme at start */
    uint32_t sleep_timeout_ms;
    struct {
        const char *intent;
        xiaotai_ui_local_hold_fn_t fn;
        void *ctx;
    } local_hold;
#if XIAOTAI_UI_CAPTURE
    ui_display_t *capture_display; /* one frame request across all displays */
#endif
} ui_engine_t;

#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
bool ui_render_scene(ui_display_t *display);
#else
static inline bool ui_render_scene(ui_display_t *display)
{
    display->theme->render(display->theme_state, &display->scene);
    return true;
}
#endif

/* The one engine instance and the product configuration it was given. */
extern ui_engine_t g_ui;
extern xiaotai_ui_config_t g_ui_config;
extern char g_ui_theme_name[UI_THEME_NAME_MAX + 1];

/* Monotonic milliseconds of the UI system, wrapping. Host tests replace the
 * source with ui_core_set_clock() so deadlines are checked at exact times. */
uint32_t ui_now_ms(void);
void ui_core_set_clock(uint32_t (*clock_ms)(void));

/* [owner] The view sink body: route a view to its display and layer at the
 * UI system's time. Installed with xiaotai_view_set_sink() at init. */
void ui_view_accept(const xiaotai_view_t *view);
void ui_local_rejection_toast(ui_display_t *display, const char *text, uint32_t now);

/* [ui] Take the slots of a display: fills present, changed and view of the
 * scene's three layers (the other scene members belong to the scene code),
 * reports per layer whether a view arrived, and removes a toast or an overlay
 * whose duration ran out at `now_ms`. Returns true when any layer changed. */
bool ui_layers_take(ui_display_t *display, uint32_t now_ms, xiaotai_ui_scene_t *scene,
                    ui_take_t take[UI_LAYER_COUNT]);

/* [ui] Earliest deadline of the display's toast or overlay (staged or
 * current), for the UI task's wait. False when none is set. */
bool ui_layers_next_due(ui_display_t *display, uint32_t *due_ms);

/* [ui] Consume staged layers, key mailbox and due scene work; render the
 * resulting display->scene before returning to theme input callbacks.
 * UI-3 owns the scheduling, wake gate and raw pointer polling. */
bool ui_scene_step(ui_display_t *display, uint32_t now_ms);
bool ui_scene_update(ui_display_t *display, uint32_t now_ms, bool read_pointer);
bool ui_scene_next_due(ui_display_t *display, uint32_t now_ms, uint32_t *due_ms);
bool ui_scene_periodic_due(ui_display_t *display, uint32_t now_ms, uint32_t *due_ms);
bool ui_scene_nonperiodic_due(ui_display_t *display, uint32_t now_ms, uint32_t *due_ms);
xiaotai_ui_layer_t *ui_scene_layer(ui_display_t *display, xiaotai_view_layer_t layer);
xiaotai_view_layer_t ui_scene_top(const ui_display_t *display);
void ui_scene_focus(ui_display_t *display, xiaotai_view_layer_t layer, uint8_t kind, uint16_t index);
bool ui_scene_focus_changed(const xiaotai_ui_layer_t *old, const xiaotai_view_t *next, bool identity);
bool ui_input_target_changed(const ui_press_t *press, const xiaotai_view_t *old, const xiaotai_view_t *next);
void ui_input_scene_changed(ui_display_t *display, const ui_take_t take[UI_LAYER_COUNT]);
void ui_input_step(ui_display_t *display, uint32_t now_ms);
void ui_input_cancel_press(ui_display_t *display);
void ui_input_end_local_hold(ui_display_t *display);
void ui_input_reset(ui_display_t *display);
void ui_input_ignored(ui_display_t *display);
xiaotai_err_t ui_input_register(void);

xiaotai_err_t ui_driver_start(ui_display_t *display);
void ui_driver_stop(ui_display_t *display);
xiaotai_err_t ui_driver_kick(ui_display_t *display);
/* Engine mutex held: queue a wake without dropping the publisher's lock. */
xiaotai_err_t ui_driver_post_locked(ui_display_t *display);
bool ui_driver_context(const ui_display_t *display);
/* Refuse an operation that would wait for the current display context. */
xiaotai_err_t ui_driver_wait_guard(const ui_display_t *display);
void ui_driver_step(ui_display_t *display, uint32_t now);
void ui_driver_manual(bool manual);   /* deterministic unit fixtures */
xiaotai_err_t ui_display_open(ui_display_t *display);
void ui_display_stop(ui_display_t *display);

void ui_power_init(ui_display_t *display, uint32_t now);
void ui_power_step(ui_display_t *display, uint32_t now);
void ui_power_shutdown(ui_display_t *display);
bool ui_power_next_due(ui_display_t *display, uint32_t now, uint32_t *due);
bool ui_power_navigation(ui_display_t *display, uint32_t now);
bool ui_power_touch(ui_display_t *display, xiaotai_ui_input_t input, uint32_t now);
void ui_power_activity(ui_display_t *display, uint32_t now);
xiaotai_err_t ui_power_sleep_guarded(uint8_t display);

/* The state of a display's touch, and the task that reads it (ui_pointer.c). */
xiaotai_err_t ui_pointer_open(ui_display_t *display);
void ui_pointer_close(ui_display_t *display);
void ui_pointer_start(ui_display_t *display);
void ui_pointer_stop(ui_display_t *display);
ui_sample_t ui_pointer_read(ui_display_t *display, int64_t at_us);
void ui_pointer_push(ui_display_t *display, const ui_sample_t *sample);
size_t ui_pointer_take(ui_display_t *display, ui_sample_t out[UI_SAMPLES]);

/* One logical pointer stream for real and injected samples, owned by the UI step. */
void ui_pointer_step(ui_display_t *display, uint32_t now, bool poll_real);
void ui_pointer_settle(ui_display_t *display);
bool ui_pointer_due(ui_display_t *display, uint32_t now, uint32_t *due);

xiaotai_err_t ui_frame_open(ui_display_t *display);
void ui_frame_close(ui_display_t *display);
void ui_frame_unlatch(ui_display_t *display);
#if XIAOTAI_UI_CAPTURE
xiaotai_err_t ui_frame_capture(ui_display_t *display, xiaotai_ui_capture_t *capture);
#endif

xiaotai_err_t ui_diag_init(void);
xiaotai_err_t ui_commands_init(void);
void ui_diag_step(ui_display_t *display);
void ui_diag_shutdown(ui_display_t *display);
bool ui_diag_pending(ui_display_t *display);
bool ui_diag_activity(void);
xiaotai_err_t ui_capture_named(uint8_t display, xiaotai_ui_capture_t *out, char view[32], uint32_t timeout_ms);
void ui_view_name(const xiaotai_view_t *view, char *out, size_t capacity);
void ui_count_flush(ui_display_t *display, uint32_t pixels, size_t bytes, uint64_t wait_us);
void ui_trace_span(ui_display_t *display, ui_span_kind_t kind, int64_t us);
void ui_trace_poll(ui_display_t *display, int64_t began, int64_t ended);
void ui_trace_contact(ui_display_t *display, bool down, int64_t at);
void ui_trace_posted(ui_display_t *display);
void ui_trace_shown(ui_display_t *display);
void ui_trace_painted(ui_display_t *display, bool scene);

/* Display by index, NULL past the end or before init. */
ui_display_t *ui_display_at(size_t index);

/* Registries (ui_registry.c): allocated at engine init, freed at reset. */
xiaotai_err_t ui_registry_init(void);
void ui_registry_free(void);
/* Registered theme by name, NULL when none. */
const xiaotai_theme_t *ui_registry_theme_named(const char *name);
/* match() of a theme, negative scores as 0. */
int ui_registry_score(const xiaotai_theme_t *theme, const xiaotai_display_desc_t *desc);

#if XIAOTAI_UI_LVGL
/* Backend attaches to the opened part. Enter/leave serialize calls into LVGL,
 * including callbacks; they are no-ops on the independent FRAME backend. */
xiaotai_err_t ui_lvgl_open(ui_display_t *display);
void ui_lvgl_enter(const ui_display_t *display);
void ui_lvgl_leave(const ui_display_t *display);
void ui_lvgl_close(ui_display_t *display);
void ui_lvgl_shutdown(void);
void ui_lvgl_step(ui_display_t *display, uint32_t now);
/* Something on the display moves by itself, or may any moment. */
bool ui_lvgl_busy(const ui_display_t *display);
void ui_lvgl_invalidate_screen(const ui_display_t *display);
/* The backend's hardware drawing: its counters as one line ("" without any),
 * and its switch (XIAOTAI_ERR_NOT_SUPPORTED without any). */
void ui_lvgl_report(char *words, size_t capacity);
xiaotai_err_t ui_lvgl_accelerate(bool on);
#if XIAOTAI_UI_CAPTURE
xiaotai_err_t ui_lvgl_capture(ui_display_t *display, xiaotai_ui_capture_t *capture);
#endif
#endif

/* The optional built-in presenter supplies an owner-side release callback. */
xiaotai_err_t ui_video_install(uint8_t display, const xiaotai_ui_video_source_t *source,
                              uint32_t generation, void (*release)(void));

#ifdef __cplusplus
}
#endif
