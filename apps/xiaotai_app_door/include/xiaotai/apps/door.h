// SPDX-License-Identifier: MIT
#pragma once
/* Application "door": the indoor station of a video door phone.
 *
 * What the application is: the door home screen (time, the state of the door
 * station outside, "look" and "open"), choosing which device contact is the
 * door station, the page of a call from that station (answer, mute, volume,
 * open, hang up) and the confirmation before a door is opened. It draws
 * nothing itself: generic views and five custom templates (below), which it
 * implements for the displays it supports and which fall back to generic
 * drawing elsewhere (xiaotai/view.h).
 *
 * Modules. Required: call, contacts, provisioning, binding, settings and the
 * engines ui, feedback and sounds. Optional, used when the product selects
 * them: video_call (the station's picture in a call), stream (the "stop"
 * control while somebody watches remotely), stream_video (the viewer's
 * picture behind the door home screen during a web stream), lock (a door
 * relay on this board). The application installs no video source: the video
 * call and stream video modules install the UI system's built-in source for
 * their session's generation (xiaotai_ui_video_show) and clear it
 * (xiaotai_ui_video_clear). While "video_call.remote" or "stream_video.remote"
 * reports a picture (args.value 1) the application marks the view that shows
 * it (the door call page, the door home screen behind a web stream)
 * XIAOTAI_VIEW_VIDEO, and removes the mark at 0 or when the session ends; a
 * format no decoder here handles is reported once per session generation
 * ("视频格式不支持"). Without the lock module "open" is confirmed and then answered with
 * "no lock is configured, nothing was sent": the application never drives an
 * output it was not given and never reports a door as opened when it is not.
 *
 * Door calls. A call is a door call when it is a device call (never a WeChat
 * call) whose peer id equals the stored door station (setting "door.peer").
 * The decision is made when the call is announced or dialled and kept for
 * that call; it is made again only when the call's session changes. Door
 * calls are presented by this application on the door call page and end on
 * the door home screen, without a result page.
 *
 * Several applications (xiaotai/framework.h, "Which application presents an
 * event"). The application never names another one, in code or in its
 * configuration:
 *   - its manifest lists the call, video call, web stream and stream video
 *     events in "handles", and xiaotai_app_door_claim() accepts the call and
 *     video call events of door calls and the presentation of active web
 *     streams (shown behind the door home screen). Admission changes and
 *     refused viewers do not take the screen by themselves. Other calls,
 *     a conversation with the assistant and setup guidance are presented
 *     by whichever application of the product lists it, or by the foreground
 *     application when none does;
 *   - when the door call or the stream it was brought forward for ends, it
 *     gives the screen back with xiaotai_app_return();
 *   - the home pager leaving the door home screen is xiaotai_app_next() and
 *     xiaotai_app_previous(); the application entered that way shows its first
 *     or its last home screen by the direction the framework reports
 *     (xiaotai_app_entry() in its foreground(true));
 *   - the two hidden entries of the door home screen post the framework's
 *     shared intents XIAOTAI_INTENT_SYSTEM_MENU (a long press on the title)
 *     and XIAOTAI_INTENT_SYSTEM_NETWORK (a tap on the network indicator),
 *     handled by the application of the product that
 *     offers a menu and a network page; a control whose intent nothing handles
 *     (xiaotai_intent_exists) is not shown.
 * Without another application the door application is the foreground one and
 * so receives what nobody else presents: it shows other calls and setup
 * guidance itself with generic views.
 *
 * Tags as in xiaotai/framework.h: [glue], [init], [owner], [any]. Every
 * handler of this application runs on the framework owner task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

/* Product configuration. Every field: NULL or 0 selects the stated default,
 * so a zeroed struct is valid. Strings are not copied (static storage). */
typedef struct {
    /* Title of the door home screen; NULL: "小钛可视门铃". */
    const char *title;
    /* Name of the door shown where the station has no name; NULL: "入户门".
     * The templates' fonts are subsets: the build derives their characters
     * from the templates, the application's texts and these two strings;
     * text outside the subset (contact names) is drawn with the UI system's
     * full font. */
    const char *door_name;
    /* Lock opened by "open" (index of the lock module); default 0. */
    uint8_t lock;
    /* While this application presents setup guidance (no other application
     * of the product presents the binding events): speak the verification
     * code once when it becomes visible (XIAOTAI_EVENT_BINDING_CODE ->
     * xiaotai_binding_announce(), when no feedback sound is pending). How it
     * sounds is the binding module's configuration. Default false: only the
     * "replay" control of the code page speaks it. */
    bool announce_on_code;
    /* Explicit contact-row confirmation saves even the selected peer.
     * Ordinary framework writes and role selection keep their defaults. */
    bool save_peer_on_reselect;
} xiaotai_app_door_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; the struct is
 * copied. NULL or never called: all defaults. XIAOTAI_ERR_INVALID_STATE after
 * the application's init. */
xiaotai_err_t xiaotai_app_door_configure(const xiaotai_app_door_config_t *config);

/* [owner] Whether this application presents the event (xiaotai_application_t
 * .claim, named in the manifest by "claims"): the call and video call events
 * of a door call (classified as above, from the call module's status and the
 * stored door station, read from memory) and the active web stream's
 * presentation. Changes nothing. */
bool xiaotai_app_door_claim(const xiaotai_event_t *event);

/* ------------------------------------------------------- pages and templates */

/* Page ids (xiaotai_view_t.id, diagnostics). Appended, never renamed. */
#define XIAOTAI_APP_DOOR_PAGE_HOME "door.home"          /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_HOME */
#define XIAOTAI_APP_DOOR_PAGE_DEVICES "door.devices"    /* CUSTOM ..._TEMPLATE_DEVICES (LIST): device contacts, the chosen one checked */
#define XIAOTAI_APP_DOOR_PAGE_MONITOR "door.monitor"    /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_MONITOR */
#define XIAOTAI_APP_DOOR_PAGE_CALL "door.call"          /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_CALL */
/* Overlay ids; all three are CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_DIALOG. */
#define XIAOTAI_APP_DOOR_OVERLAY_OPEN "door.open"       /* NOTICE: confirm opening the door */
#define XIAOTAI_APP_DOOR_OVERLAY_RESULT "door.result"   /* NOTICE: the door was opened, or why not */
#define XIAOTAI_APP_DOOR_OVERLAY_VOLUME "door.volume"   /* FORM: one live slider, 0..100 in the settings module's volume step */

/* Custom templates (xiaotai_view_t.custom). The application registers an
 * implementation for the displays it supports (xiaotai_ui_template_register);
 * on any other display the view is drawn as NOTICE from its generic members,
 * which therefore always carry the whole content:
 *   title, subtitle, status, text   the words
 *   indicators                      the network mark (with its link's intent)
 *   actions                         every control, identified by its intent
 *                                   (a template lays controls out by intent
 *                                   name and ignores intents it does not know)
 *   timer_s, timer_mode             the call time
 * custom_data is one of the structs below and tells the template what the
 * words cannot: which state colours and which picture apply. */
#define XIAOTAI_APP_DOOR_TEMPLATE_HOME "door.home"
#define XIAOTAI_APP_DOOR_TEMPLATE_MONITOR "door.monitor"
#define XIAOTAI_APP_DOOR_TEMPLATE_CALL "door.call"
/* The device list (generic form: LIST) and the three overlays, confirm,
 * result and volume slider (generic form: NOTICE, NOTICE, FORM), in the door
 * style: native coordinates and the door fonts on wide colour displays. */
#define XIAOTAI_APP_DOOR_TEMPLATE_DEVICES "door.devices"
#define XIAOTAI_APP_DOOR_TEMPLATE_DIALOG "door.dialog"

/* What the door home screen says about the device and the door station.
 * Appended, never renumbered. The first that applies, in this order. */
typedef enum {
    XIAOTAI_APP_DOOR_STREAMING = 0,   /* somebody watches remotely */
    XIAOTAI_APP_DOOR_NO_NETWORK,
    XIAOTAI_APP_DOOR_NOT_READY,       /* online services have not answered yet */
    XIAOTAI_APP_DOOR_NO_STATION,      /* no door station chosen */
    XIAOTAI_APP_DOOR_STATION_REMOVED, /* the chosen station is no longer a contact */
    XIAOTAI_APP_DOOR_STATION_OFFLINE,
    XIAOTAI_APP_DOOR_STATION_ONLINE,
    /* MONITOR only: the device is busy with a session, and the reason shown
     * when everything else is in order (no viewing service is connected). */
    XIAOTAI_APP_DOOR_BUSY,
    XIAOTAI_APP_DOOR_NO_SERVICE,
} xiaotai_app_door_state_t;

/* custom_data of XIAOTAI_APP_DOOR_TEMPLATE_HOME and _MONITOR. */
typedef struct {
    uint8_t state;           /* xiaotai_app_door_state_t */
} xiaotai_app_door_home_view_t;

typedef enum {
    XIAOTAI_APP_DOOR_CALL_RINGING = 0,   /* the station rings: accept or reject */
    XIAOTAI_APP_DOOR_CALL_CONNECTING,
    XIAOTAI_APP_DOOR_CALL_ACTIVE,
} xiaotai_app_door_call_phase_t;

/* custom_data of XIAOTAI_APP_DOOR_TEMPLATE_CALL. The view carries
 * XIAOTAI_VIEW_VIDEO while the call has video. */
typedef struct {
    uint8_t phase;           /* xiaotai_app_door_call_phase_t */
    bool video;              /* the call carries video */
    bool muted;              /* the microphone, as the call module reports it */
} xiaotai_app_door_call_view_t;

/* custom_data of the door.dialog result view. */
typedef struct {
    bool opened;
} xiaotai_app_door_result_view_t;

/* ----------------------------------------------------------------- intents */

/* Intents this application handles. Controls that act on a session carry
 * the session the view showed, so a tap meant for an ended call or stream
 * cannot act on the next one, and a refusal is shown as a toast.
 *
 *   door.call       args.text "accept", "reject" or "hangup"; args.value: the
 *                   call module's session shown on the door call page
 *                   (uint32_t carried in the int32_t). Calls
 *                   xiaotai_call_accept / _reject / _hangup with it.
 *   door.stream_stop  args.value: the stream generation captured when the
 *                   stop control was shown (stream.started); calls
 *                   xiaotai_stream_stop with it, so a stream that began in
 *                   between is not stopped.
 *
 *   door.home       args.value 0: the door home screen (brings the
 *                   application to the foreground); 1: the next screen of the
 *                   home pager (xiaotai_app_next()); -1: the previous one
 *                   (xiaotai_app_previous()); with no other application the
 *                   pager has one screen and 1 / -1 do nothing. Ignored while
 *                   an overlay shows, a call is presented or setup guidance
 *                   owns the screen.
 *   door.back       leave the page for the door home screen
 *   door.devices    show the list of device contacts and refresh it
 *   door.select     args.value: index in the current list, or the item's
 *                   opaque value from a rendered list. Stale item values
 *                   are ignored. Store the chosen door station and go home.
 *   door.monitor    show the monitor page
 *   door.open       ask for confirmation; door.open.confirm args.value 1
 *                   opens (or says why not), 0 dismisses
 *   door.volume     show the volume slider (door call, connected);
 *                   door.volume.set args.value: percent, with args.text
 *                   XIAOTAI_ARG_LIVE while the slider is dragged. Release
 *                   commits the most recent successful preview, if any.
 *   door.mute       toggle the microphone of the door call, from the state
 *                   the call module reports
 *   door.role       args.value 0: viewer, 1: sender (stored only) */
#define XIAOTAI_INTENT_APP_DOOR_CALL "door.call"
#define XIAOTAI_INTENT_APP_DOOR_STREAM_STOP "door.stream_stop"
#define XIAOTAI_INTENT_APP_DOOR_HOME "door.home"
#define XIAOTAI_INTENT_APP_DOOR_BACK "door.back"
#define XIAOTAI_INTENT_APP_DOOR_DEVICES "door.devices"
#define XIAOTAI_INTENT_APP_DOOR_SELECT "door.select"
#define XIAOTAI_INTENT_APP_DOOR_MONITOR "door.monitor"
#define XIAOTAI_INTENT_APP_DOOR_OPEN "door.open"
#define XIAOTAI_INTENT_APP_DOOR_OPEN_CONFIRM "door.open.confirm"
#define XIAOTAI_INTENT_APP_DOOR_VOLUME "door.volume"
#define XIAOTAI_INTENT_APP_DOOR_VOLUME_SET "door.volume.set"
#define XIAOTAI_INTENT_APP_DOOR_MUTE "door.mute"
#define XIAOTAI_INTENT_APP_DOOR_ROLE "door.role"

/* Two pager gestures closer together than this are one. */
#define XIAOTAI_APP_DOOR_PAGER_GUARD_MS 350U

/* ------------------------------------------------------------------ events */

/* The page changed. args.text: the page id, "" when the application left the
 * foreground. */
#define XIAOTAI_EVENT_APP_DOOR_PAGE "door.page"
/* "Open" was confirmed. args.value: XIAOTAI_OK when a lock was released,
 * XIAOTAI_ERR_NOT_FOUND no station is chosen, XIAOTAI_ERR_INVALID_STATE the
 * chosen station changed since the question was asked,
 * XIAOTAI_ERR_NOT_SUPPORTED no lock is configured, otherwise the lock
 * module's result. Nothing was sent or driven unless it is XIAOTAI_OK. */
#define XIAOTAI_EVENT_APP_DOOR_OPENED "door.opened"

/* ---------------------------------------------------------------- settings */

/* Settings this application registers (xiaotai/framework.h); storage keys
 * for a product's preference binding table (xiaotai/preferences.h).
 *
 * The door station: text, the device id of a device contact, at most 64
 * characters; "" when none is chosen. */
#define XIAOTAI_SETTING_APP_DOOR_PEER "door.peer"
#define XIAOTAI_SETTING_KEY_APP_DOOR_PEER "door_peer"
/* Role of this device: bool, 0 viewer, 1 sender; default 0. Stored and
 * reported only. */
#define XIAOTAI_SETTING_APP_DOOR_SENDER "door.sender"
#define XIAOTAI_SETTING_KEY_APP_DOOR_SENDER "door_sender"

/* ---------------------------------------------------------------- feedback */

/* States of the application's feedback table (xiaotai/feedback.h): a ring
 * while the door station calls and a ring-back while this device calls it.
 * Set only for calls this application presents. */
#define XIAOTAI_APP_DOOR_STATE_INCOMING "door.incoming"
#define XIAOTAI_APP_DOOR_STATE_OUTGOING "door.outgoing"

/* ------------------------------------------------------------- diagnostics */

typedef struct {
    bool foreground;
    bool presenting_call;       /* a door call owns the screen */
    bool overlay;               /* a confirmation, a result or the volume slider shows */
    bool station_selected;
    bool sender;
    bool lock_configured;       /* the lock module is part of the product and has the configured lock */
    bool save_pending;          /* a changed setting has not reached storage yet */
    uint8_t state;              /* xiaotai_app_door_state_t of the door home screen */
    char page[16];              /* id of the page shown, "" in the background */
    uint32_t door_calls;        /* calls classified as door calls since boot */
    uint32_t yielded;           /* call events claim() left to other applications */
    uint32_t opens;             /* confirmed "open" requests */
    uint32_t opens_done;        /* of which released a lock */
} xiaotai_app_door_status_t;

/* [owner] */
void xiaotai_app_door_status(xiaotai_app_door_status_t *status);

/* Commands: AT+DOOR (the status line +DOOR: control= accepted= page= role=
 * peer_selected= save_pending= awake= setup= lock_configured=) and its verbs
 * HOME, NEXT, PREVIOUS, VIEWER, SENDER, DEVICES, BACK. A verb other than the
 * bare query is accepted only while no overlay shows, no setup guidance owns
 * the screen and no session runs. VIEWER and SENDER store the role (manifest
 * access "guarded"); the bare query reads ("read") and the other verbs only
 * move between pages ("action"). Diagnostics section "app.door". Event-log
 * source "door". */

/* ---------------------------------------------------------------- lifecycle */

/* [owner] Move within the foreground home pager, or on to another app. */
xiaotai_err_t xiaotai_app_door_page_step(int direction);

/* Application lifecycle (called by the framework through the generated
 * tables). init: registers the settings, intents, command, diagnostics
 * provider, feedback table and custom templates, and subscribes to the
 * events of the modules that are present. start: reads the stored door
 * station. foreground(true): shows the door home screen, or the door call
 * when one is running. claim: above. */
xiaotai_err_t xiaotai_app_door_init(void);
void xiaotai_app_door_start(void);
void xiaotai_app_door_foreground(bool foreground);
/* Host tests: forget configuration and state. */
void xiaotai_app_door_reset(void);

#ifdef __cplusplus
}
#endif
