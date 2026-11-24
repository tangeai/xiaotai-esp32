// SPDX-License-Identifier: MIT
#pragma once
/* Views: applications describe what to show with a few generic templates; a
 * theme of the UI system draws them for the display at hand (320x240 colour,
 * 128x64 mono, 1024x600 door panel, ...). The same application thus runs on any
 * display, and a new display only needs a matching theme. Products without a
 * display have no UI system: views are dropped and feedback (lights, sounds)
 * presents the state.
 *
 * Model
 * - A display shows up to three layers: the PAGE, one OVERLAY (dialog) above it
 *   and one TOAST (a short notice) above both. xiaotai_view_show() replaces
 *   what its layer shows.
 * - Identity: a view is identified by (layer, kind, custom, id). Showing the
 *   same identity again is an UPDATE: focus, scroll position and edit state are
 *   kept and only what changed is redrawn, so showing on every state change is
 *   cheap and is the intended use (captions, timers, button states). A
 *   different identity is a NEW view: it is rebuilt and the focus is reset.
 * - A new PAGE identity removes the overlay (it belonged to the page it was
 *   shown on) and makes the UI system emit XIAOTAI_EVENT_VIEW_CHANGED. The
 *   toast survives it and runs out its own duration, so "submit, toast, then
 *   a page change driven by state" keeps the notice. A new OVERLAY identity
 *   removes the toast (a dialog covers it). A page the toast must not cover
 *   (a call) is shown after xiaotai_view_dismiss(XIAOTAI_LAYER_TOAST, NULL).
 * - A pointer press that began before a layer got a new identity belongs to
 *   what was shown then: it operates nothing until it is released.
 * - Everything the user can operate posts an intent by name; the application
 *   (or a module) handles it and shows the next state. Themes never decide.
 * - Time is drawn by the UI system: timers run from the value given at show,
 *   CLOCK reads the wall clock. Applications do not tick views.
 *
 * Ownership: every pointer in a view is borrowed for the duration of
 * xiaotai_view_show(); the UI system copies what it keeps. Strings are UTF-8,
 * NULL means absent. Strings and arrays above the limits below are truncated
 * (strings at a character boundary) and counted by the UI system.
 *
 * Extension: a new kind, flag, trigger or field kind is appended to its
 * enumeration; existing values and field meanings never change. Themes ignore
 * what they do not know and fall back to NOTICE for an unknown kind. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ limits */

#define XIAOTAI_VIEW_ITEMS_MAX 32U       /* items of one view */
#define XIAOTAI_VIEW_ACTIONS_MAX 8U
#define XIAOTAI_VIEW_FIELDS_MAX 8U
#define XIAOTAI_VIEW_INDICATORS_MAX 4U
#define XIAOTAI_VIEW_OPTIONS_MAX 8U      /* options of one CHOICE field */
#define XIAOTAI_VIEW_TEXT_MAX 2048U      /* bytes of one string, terminator included */
#define XIAOTAI_VIEW_CUSTOM_MAX 512U     /* bytes of custom_data */
#define XIAOTAI_VIEW_DIGITS_MAX 16U      /* digits of one DIGITS field */
#define XIAOTAI_TOAST_DEFAULT_MS 2400U
#define XIAOTAI_VIEW_HOLD_REPEAT_MS 250U /* repeat period of a held HOLD action */

/* ------------------------------------------------------------------- kinds */

typedef enum {
    XIAOTAI_VIEW_HOME = 0,  /* idle screen: title, status, emotion, text (caption or hint), actions.
                               items are the application's top-level entries: a theme without room
                               for an emotion shows them as the home menu, the others ignore them
                               and reach the menu through an action */
    XIAOTAI_VIEW_TALK,      /* conversation: emotion with phase and level, caption text */
    XIAOTAI_VIEW_CALL,      /* call: title (kind), subtitle (peer), text (state), timer, actions */
    XIAOTAI_VIEW_LIST,      /* selectable items (menu, contacts, members, results), actions */
    XIAOTAI_VIEW_CODE,      /* text is a verification code, or a QR payload with XIAOTAI_VIEW_QR;
                               or image; subtitle and items explain; timer counts down */
    XIAOTAI_VIEW_NOTICE,    /* title, text (may be long: it scrolls), items as "label: detail" rows, actions */
    XIAOTAI_VIEW_PROGRESS,  /* connecting, updating: title, text (stage), optional percent, actions */
    XIAOTAI_VIEW_CLOCK,     /* wall time and date drawn by the theme; text (next alarm), actions;
                               with XIAOTAI_VIEW_CALENDAR a clock to leave standing: themes with room add the
                               calendar, and items (a label, a detail, an icon; not selectable) say what comes
                               next. A theme that draws the items leaves text out */
    XIAOTAI_VIEW_CUSTOM,    /* application template named by custom; without an implementation for the
                               display it is drawn as NOTICE from the generic members */
    XIAOTAI_VIEW_FORM,      /* fields edited in place (settings, digit entry), actions */
} xiaotai_view_kind_t;

typedef enum {
    XIAOTAI_LAYER_PAGE = 0,
    XIAOTAI_LAYER_OVERLAY,  /* dialog above the page; pointer input reaches only the overlay, and a
                               tap outside its controls fires its BACK action when it has one */
    XIAOTAI_LAYER_TOAST,    /* text only, no input */
} xiaotai_view_layer_t;

/* xiaotai_view_t.flags */
enum {
    XIAOTAI_VIEW_DISMISS = 1u << 0,    /* remove what the layer shows; with id: only if it shows that id */
    XIAOTAI_VIEW_GRID = 1u << 1,       /* LIST: short labels laid out as a grid when the theme can */
    XIAOTAI_VIEW_VIDEO = 1u << 2,      /* the video source of the UI system is drawn behind the controls */
    XIAOTAI_VIEW_MAY_SLEEP = 1u << 3,  /* PAGE: the screen may sleep on its idle timeout while this shows */
    XIAOTAI_VIEW_QR = 1u << 4,         /* CODE: text is a payload to draw as a QR code */
    XIAOTAI_VIEW_PERCENT = 1u << 5,    /* PROGRESS: progress is valid (otherwise indeterminate) */
    XIAOTAI_VIEW_EMOTION_PREVIEW = 1u << 6, /* TALK: raise the expression for a standalone preview */
    XIAOTAI_VIEW_FOCUS_ON_ENTRY = 1u << 7, /* Apply focus only when the view identity changes. */
    XIAOTAI_VIEW_CALENDAR = 1u << 8,   /* CLOCK: the day, the month and what comes next beside the time */
};

/* state bits of items, actions and fields; 0: enabled and visible */
enum {
    XIAOTAI_STATE_DISABLED = 1u << 0,  /* shown greyed; does not post */
    XIAOTAI_STATE_BUSY = 1u << 1,      /* request in progress: does not post; themes may animate */
    XIAOTAI_STATE_HIDDEN = 1u << 2,
    XIAOTAI_STATE_CHECKED = 1u << 3,   /* current choice, active tab, selected entry */
    XIAOTAI_STATE_HOLD_FROM_POINTER = 1u << 4, /* local HOLD renews only on pointer events */
    XIAOTAI_STATE_TAP_LOCAL_REJECTION = 1u << 5, /* action can show a local toast when its tap cannot enqueue */
};

/* ----------------------------------------------------------- items, actions */

/* Intent arguments of an item or action: args.value = its value. args.text is
 * empty except for HOLD actions and fields (below). */
typedef struct {
    const char *label;
    const char *detail;
    const char *intent;   /* posted when chosen; NULL: not selectable */
    int32_t value;
    const char *icon;     /* named glyph, see XIAOTAI_ICON_*; NULL: none */
    uint8_t state;        /* XIAOTAI_STATE_* */
} xiaotai_view_item_t;

/* How a pointer (touch) fires an action. Displays without a pointer use role. */
typedef enum {
    XIAOTAI_TRIGGER_TAP = 0,
    /* Press-and-hold (push to talk): the intent is posted with args.text
     * XIAOTAI_ARG_PRESS at press and again every XIAOTAI_VIEW_HOLD_REPEAT_MS
     * while held (a heartbeat the handler may require), and once with
     * XIAOTAI_ARG_RELEASE at release, when the press is lost, when the action
     * becomes disabled or hidden, and when its view is replaced. */
    XIAOTAI_TRIGGER_HOLD,
    XIAOTAI_TRIGGER_LONG_PRESS,   /* posted once after a long press; a tap does nothing */
    XIAOTAI_TRIGGER_SWIPE_LEFT,   /* a swipe anywhere on the layer; a visible button also takes taps */
    XIAOTAI_TRIGGER_SWIPE_RIGHT,
    XIAOTAI_TRIGGER_SWIPE_UP,
    XIAOTAI_TRIGGER_SWIPE_DOWN,
} xiaotai_view_trigger_t;

#define XIAOTAI_ARG_PRESS "press"
#define XIAOTAI_ARG_RELEASE "release"
#define XIAOTAI_ARG_LIVE "live"
#define XIAOTAI_ARG_CHOICE_RECONFIRM "choice.reconfirm"

/* Which navigation key fires an action on displays operated by keys
 * (XIAOTAI_INTENT_UI_CONFIRM / _BACK / _NEXT of the UI system). Pointer themes
 * use the role for placement (BACK: the back affordance). At most one action
 * of each role per view; the first wins. */
typedef enum {
    XIAOTAI_ROLE_NONE = 0,
    /* Confirm, when no item or field has the focus (accept a call). Pointer
     * themes: on HOME and TALK a PRIMARY action without label and icon is
     * fired by a tap on the emotion. */
    XIAOTAI_ROLE_PRIMARY,
    XIAOTAI_ROLE_BACK,      /* back (leave, hang up, reject, dismiss) */
    XIAOTAI_ROLE_AUX,       /* next, when the view has nothing to step through (interrupt) */
} xiaotai_view_role_t;

typedef enum {
    XIAOTAI_STYLE_NORMAL = 0,
    XIAOTAI_STYLE_POSITIVE,     /* accept, confirm */
    XIAOTAI_STYLE_DESTRUCTIVE,  /* hang up, reject, leave */
    XIAOTAI_STYLE_QUIET,        /* secondary: a text link or small icon */
} xiaotai_view_style_t;

/* Icon names themes know (they also mark where a theme places the action:
 * "menu" in the corner of HOME, "phone" as the HOME shortcut). An unknown name
 * shows the label; an action with neither label nor known icon is invisible
 * and fires only by trigger or role. */
#define XIAOTAI_ICON_BACK "back"
#define XIAOTAI_ICON_MENU "menu"
#define XIAOTAI_ICON_NEXT "next"
#define XIAOTAI_ICON_PREVIOUS "previous"
#define XIAOTAI_ICON_PHONE "phone"
#define XIAOTAI_ICON_VIDEO "video"
#define XIAOTAI_ICON_ACCEPT "accept"
#define XIAOTAI_ICON_HANGUP "hangup"
#define XIAOTAI_ICON_MIC "mic"
#define XIAOTAI_ICON_MIC_OFF "mic_off"
#define XIAOTAI_ICON_CAMERA "camera"
#define XIAOTAI_ICON_CAMERA_OFF "camera_off"
#define XIAOTAI_ICON_VOLUME "volume"
#define XIAOTAI_ICON_REFRESH "refresh"
#define XIAOTAI_ICON_STOP "stop"
#define XIAOTAI_ICON_SETTINGS "settings"
#define XIAOTAI_ICON_WIFI "wifi"          /* indicator: level 0..3, < 0 disconnected */
#define XIAOTAI_ICON_LOCK "lock"
#define XIAOTAI_ICON_CONTACTS "contacts"  /* a person: the address book */
#define XIAOTAI_ICON_GROUP "group"        /* several people: a room */
#define XIAOTAI_ICON_FACE "face"          /* expressions */
#define XIAOTAI_ICON_SOUND "sound"        /* a waveform: listening for sounds */
#define XIAOTAI_ICON_PULSE "pulse"        /* running state, diagnostics */
#define XIAOTAI_ICON_INFO "info"
#define XIAOTAI_ICON_ALARM "alarm"
#define XIAOTAI_ICON_TIMER "timer"
#define XIAOTAI_ICON_CLOCK "clock"
#define XIAOTAI_ICON_DOOR "door"
#define XIAOTAI_ICON_KEY "key"            /* open the door */

typedef struct {
    const char *label;
    const char *intent;   /* posted when fired; NULL: shown but inert */
    int32_t value;
    const char *icon;
    uint8_t trigger;      /* xiaotai_view_trigger_t */
    uint8_t role;         /* xiaotai_view_role_t */
    uint8_t state;        /* XIAOTAI_STATE_* */
    uint8_t style;        /* xiaotai_view_style_t */
} xiaotai_view_action_t;

/* Small status marks (signal, muted microphone). With an intent they can be
 * tapped. */
typedef struct {
    const char *icon;
    int32_t level;        /* meaning by icon; "wifi": 0..3 bars, < 0 disconnected */
    const char *intent;
    int32_t value;
} xiaotai_view_indicator_t;

/* ------------------------------------------------------------------ fields */

typedef enum {
    XIAOTAI_FIELD_TEXT = 0,  /* read-only row: label and text */
    XIAOTAI_FIELD_SWITCH,    /* value 0 or 1 */
    XIAOTAI_FIELD_STEPPER,   /* value in min..max by step, with - and + */
    XIAOTAI_FIELD_SLIDER,    /* value in min..max; XIAOTAI_FIELD_LIVE posts while dragging */
    XIAOTAI_FIELD_CHOICE,    /* value indexes options */
    XIAOTAI_FIELD_DIGITS,    /* text holds decimal digits; max is the digit count */
} xiaotai_field_kind_t;

/* xiaotai_view_field_t.flags */
enum {
    XIAOTAI_FIELD_MASKED = 1u << 0,  /* DIGITS: only the digit being edited is readable */
    XIAOTAI_FIELD_LIVE = 1u << 1,    /* SLIDER: also post intermediate values, args.text XIAOTAI_ARG_LIVE */
    XIAOTAI_FIELD_ALLOW_EMPTY = 1u << 2, /* DIGITS: an empty value is also a valid commit */
    XIAOTAI_FIELD_CONFIRM_CURRENT = 1u << 3, /* CHOICE: an explicit reselect posts XIAOTAI_ARG_CHOICE_RECONFIRM */
};

/* A change posts the field's intent: args.value = the new value (SWITCH,
 * STEPPER, SLIDER, CHOICE), args.text = the digits (DIGITS, posted on every
 * edit). The field does not keep the new value by itself: the application
 * applies it and shows the view again with the value it accepted, so a refused
 * change snaps back. Until that update the theme may draw the value it sent.
 * Key navigation: confirm on a SWITCH toggles, on a CHOICE advances, on a
 * STEPPER steps up (wrapping to min), on DIGITS moves to the next digit while
 * next increments the digit; confirming the last digit of the last DIGITS
 * field fires the PRIMARY action. */
typedef struct {
    uint8_t kind;                  /* xiaotai_field_kind_t */
    uint8_t state;                 /* XIAOTAI_STATE_* */
    uint8_t flags;                 /* XIAOTAI_FIELD_* */
    const char *label;
    const char *intent;            /* NULL: read-only */
    int32_t value, min, max, step; /* step 0 means 1 */
    const char *text;              /* TEXT: the value; DIGITS: current digits; others: unit shown after the value */
    const char *const *options;    /* CHOICE: option_count labels */
    size_t option_count;
} xiaotai_view_field_t;

/* -------------------------------------------------------------------- view */

typedef enum {
    XIAOTAI_TIMER_NONE = 0,
    XIAOTAI_TIMER_ELAPSED,    /* counts up from timer_s (call duration) */
    XIAOTAI_TIMER_REMAINING,  /* counts down from timer_s and stops at 0 (code expiry) */
} xiaotai_view_timer_t;

/* Conversation phase shown with the emotion (TALK, HOME). */
typedef enum {
    XIAOTAI_TALK_IDLE = 0,
    XIAOTAI_TALK_LISTENING,
    XIAOTAI_TALK_THINKING,
    XIAOTAI_TALK_SPEAKING,
} xiaotai_talk_phase_t;

/* Whose words text holds (TALK, HOME); themes style them differently. */
typedef enum {
    XIAOTAI_SPEAKER_NONE = 0,   /* a hint or status, not speech */
    XIAOTAI_SPEAKER_ASSISTANT,
    XIAOTAI_SPEAKER_USER,
} xiaotai_speaker_t;

/* Emotion names every theme accepts; anything else is drawn as "neutral".
 * Applications that store an emotion keep their own stable index table. */
#define XIAOTAI_EMOTION_NEUTRAL "neutral"
#define XIAOTAI_EMOTION_LISTENING "listening"
#define XIAOTAI_EMOTION_THINKING "thinking"
#define XIAOTAI_EMOTION_AMBIENT "ambient"
#define XIAOTAI_EMOTION_SLEEPY "sleepy"

typedef struct {
    xiaotai_view_kind_t kind;
    const char *custom;                  /* template name when kind is CUSTOM */
    const char *title;
    const char *subtitle;
    const char *text;                    /* body, caption, code or state line */
    const char *emotion;                 /* named expression; themes choose the art */
    const xiaotai_view_item_t *items;
    size_t item_count;
    const xiaotai_view_action_t *actions;
    size_t action_count;
    int32_t timer_s;                     /* seconds at the moment of show; used when timer_mode is set */
    const void *custom_data;             /* for CUSTOM templates: custom_size bytes, copied */

    /* Members below default to "absent" when the structure is zeroed. */
    size_t custom_size;                  /* at most XIAOTAI_VIEW_CUSTOM_MAX */
    const char *id;                      /* tells views of one kind apart ("contacts", "settings"); NULL: none */
    const char *status;                  /* short state beside the title ("online", "saving") */
    const char *image;                   /* picture registered with the UI system (CODE, NOTICE, HOME) */
    const xiaotai_view_field_t *fields;  /* FORM */
    size_t field_count;
    const xiaotai_view_indicator_t *indicators;
    size_t indicator_count;
    uint32_t flags;                      /* XIAOTAI_VIEW_* */
    uint32_t duration_ms;                /* OVERLAY, TOAST: removed after this long; 0: never for an
                                            overlay, XIAOTAI_TOAST_DEFAULT_MS for a toast */
    uint16_t focus;                      /* 0: keep the focus (first entry for a new view);
                                            n: move it to item or field n - 1 */
    uint8_t layer;                       /* xiaotai_view_layer_t */
    uint8_t display;                     /* display part index; 0: the first display */
    uint8_t timer_mode;                  /* xiaotai_view_timer_t */
    uint8_t phase;                       /* xiaotai_talk_phase_t */
    uint8_t level;                       /* voice activity 0..3 driving the emotion */
    uint8_t speaker;                     /* xiaotai_speaker_t of text */
    uint8_t progress;                    /* PROGRESS with XIAOTAI_VIEW_PERCENT: 0..100 */
} xiaotai_view_t;

/* Emitted [by the UI task, queued to the owner] when a display's PAGE shows a
 * new identity, and when an update of any layer put another entry under the
 * focus (the focused item's or field's intent or value differs from before).
 * args.text: the page id (or "" without one); args.value: the display index.
 * Input in flight belongs to what the user saw before: the interaction engine
 * drops it on this event when its profile asks for that. Updates that leave
 * the focused entry alone (a caption, a timer, another row's detail) do not
 * emit it, so a refresh never eats a gesture. */
#define XIAOTAI_EVENT_VIEW_CHANGED "view.changed"

/* [owner] Show, update or dismiss a view. Applications call it from their
 * code (handlers, timers, foreground); engines and modules never do. Only the
 * foreground application draws: the framework knows which application's code
 * runs, and a call from a background application is dropped and counted
 * (view_refused= in the "framework" diagnostics, xiaotai/framework.h). Never
 * blocks on drawing: the UI system copies the view (bounded, under a short
 * lock) and draws on its own task. Without a UI system the call does nothing. */
void xiaotai_view_show(const xiaotai_view_t *view);

/* The sink runs on the caller's task, inside xiaotai_view_show(). */
typedef void (*xiaotai_view_sink_t)(const xiaotai_view_t *view, void *ctx);
/* [init] The UI system installs itself as the view sink in its engine init. */
void xiaotai_view_set_sink(xiaotai_view_sink_t sink, void *ctx);

/* [owner] A short notice above everything on the first display, without
 * changing the page. A new toast replaces the current one; a page change
 * keeps it, a new overlay removes it. duration_ms 0 uses
 * XIAOTAI_TOAST_DEFAULT_MS. */
static inline void xiaotai_view_toast(const char *text, uint32_t duration_ms)
{
    xiaotai_view_t view;
    memset(&view, 0, sizeof(view));
    view.kind = XIAOTAI_VIEW_NOTICE;
    view.layer = XIAOTAI_LAYER_TOAST;
    view.text = text;
    view.duration_ms = duration_ms;
    xiaotai_view_show(&view);
}

/* [owner] Remove what a layer of the first display shows. id NULL removes
 * whatever is there; otherwise only a view with that id (a dialog that was
 * already replaced stays). Dismissing the PAGE leaves the display blank. */
static inline void xiaotai_view_dismiss(xiaotai_view_layer_t layer, const char *id)
{
    xiaotai_view_t view;
    memset(&view, 0, sizeof(view));
    view.kind = XIAOTAI_VIEW_NOTICE;
    view.layer = (uint8_t)layer;
    view.id = id;
    view.flags = XIAOTAI_VIEW_DISMISS;
    xiaotai_view_show(&view);
}

#ifdef __cplusplus
}
#endif
