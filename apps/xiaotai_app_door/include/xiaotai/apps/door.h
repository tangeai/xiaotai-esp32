// SPDX-License-Identifier: MIT
#pragma once
/* Application "door": the stations of a video door phone, by the stored mode
 * (setting "door.sender"): the indoor station, and two door stations, the
 * doorbell of one household and the access machine of a building or a company
 * (a keypad for the room). Each mode has its own home screen and
 * controls; none shows a function of another. A device nobody configured
 * asks for its mode first (page "door.mode"); later it is changed on the
 * settings page, after a question and never while a session runs. A device
 * without a camera cannot be put at the door (the page says so).
 *
 * Indoor station (role viewer, the default): looks at the door at any time
 * (page "door.monitor"), answers a call from the door, talks, opens, switches
 * between up to XIAOTAI_APP_DOOR_PEERS_MAX door stations and keeps the list of
 * recent door calls (memory only). Door station (role sender): the home
 * screen asks the visitor to call (door.ring); the chosen indoor stations are
 * called one after the other until one answers; a chosen indoor station may
 * look at any time, which the screen says while it lasts; the access machine
 * also calls the contacts whose names carry the room a visitor typed; an "open" request
 * in a call is carried out here (the lock module, or open_on_screen) and
 * shown as "门已打开". It draws
 * nothing itself: generic views and six custom templates (below), which it
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
 * call) whose peer id equals one of the stored counterparts (settings
 * "door.peer" to "door.peer4").
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
    /* Title of the door home screen; NULL: what the device is, "小钛可视门铃",
     * "小钛门铃室外机", "小钛门禁室内机" or "小钛门禁机". */
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
    /* Door station (role sender) without the lock module: accept an "open"
     * request of the indoor station and show "门已打开" on the screen,
     * saying that no lock is connected, without driving any output (a
     * demonstration). Default false: the request is answered
     * XIAOTAI_ERR_NOT_SUPPORTED and the indoor station says so. With the
     * lock module the lock is released and this has no effect. */
    bool open_on_screen;
    /* Every mode a page of its own. Paging forward from the application
     * before comes to the access machine's home page, then the access
     * system's indoor station, the doorbell (the door station) and the
     * doorbell's indoor station, then the application after; paging back
     * runs the other way. The page that stays in front for
     * XIAOTAI_APP_DOOR_MODE_SETTLE_MS, or is used (any page beyond its home,
     * a call), becomes the stored mode: the one the device answers in, also
     * while another application is in front. Pages only paged through leave
     * it as it was; another way in (a switch, a call) shows the stored mode's
     * page. Each page keeps its own counterparts, the four lists in a record
     * of the door's own: the counterpart settings are taken once, as the
     * stored mode's, and then left alone. No choice page, no mode row in the
     * settings. Default false: one mode, chosen on the screen. */
    bool mode_pages;
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

/* Chosen counterparts a device stores: the door stations of an indoor
 * station, the indoor stations of a door station. The first one is the one
 * looked at from the home screen, or called first. */
#define XIAOTAI_APP_DOOR_PEERS_MAX 4U

/* Looking (door.monitor). The indoor station places a device call to the
 * chosen door station with its microphone muted and its camera off; the door
 * station answers it by itself, because the caller is one of its chosen
 * indoor stations (so both sides chose each other, and the server connects
 * device contacts only). A call from any other device is never answered by
 * itself. A look ends by itself after XIAOTAI_APP_DOOR_LOOK_MS unless the
 * indoor station talks. */
#define XIAOTAI_APP_DOOR_LOOK_MS 120000U

/* Page ids (xiaotai_view_t.id, diagnostics). Appended, never renamed. */
#define XIAOTAI_APP_DOOR_PAGE_HOME "door.home"          /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_HOME */
#define XIAOTAI_APP_DOOR_PAGE_DEVICES "door.devices"    /* CUSTOM ..._TEMPLATE_DEVICES (LIST): device contacts, the chosen one checked */
#define XIAOTAI_APP_DOOR_PAGE_KEYPAD "door.keypad"      /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_KEYPAD (FORM): the access machine's home */
#define XIAOTAI_APP_DOOR_PAGE_MONITOR "door.monitor"    /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_MONITOR */
#define XIAOTAI_APP_DOOR_PAGE_CALL "door.call"          /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_CALL */
#define XIAOTAI_APP_DOOR_PAGE_LOOKED "door.looked"      /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_LOOKED (CALL): somebody looks at this door */
#define XIAOTAI_APP_DOOR_PAGE_DIRECTORY "door.directory" /* CUSTOM ..._TEMPLATE_DEVICES (LIST): the door station's directory */
#define XIAOTAI_APP_DOOR_PAGE_MODE "door.mode"          /* CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_MODE: the choice of mode */
#define XIAOTAI_APP_DOOR_PAGE_SETTINGS "door.settings"  /* CUSTOM ..._TEMPLATE_DEVICES (LIST) */
#define XIAOTAI_APP_DOOR_PAGE_LOG "door.log"            /* CUSTOM ..._TEMPLATE_DEVICES (LIST): recent door calls */
/* Overlay ids; CUSTOM XIAOTAI_APP_DOOR_TEMPLATE_DIALOG, the result CUSTOM
 * XIAOTAI_APP_DOOR_TEMPLATE_OPENED (both generic NOTICE). */
#define XIAOTAI_APP_DOOR_OVERLAY_ASK "door.ask"         /* NOTICE: confirm a change of mode, or removing a device */
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
/* The choice of mode (generic form: NOTICE with two actions). */
#define XIAOTAI_APP_DOOR_TEMPLATE_MODE "door.mode"
/* The door station while an indoor station looks, and what became of opening
 * the door, on both stations: they fill the display, whatever its size, on
 * every colour display from 240 x 200. */
#define XIAOTAI_APP_DOOR_TEMPLATE_LOOKED "door.looked"
#define XIAOTAI_APP_DOOR_TEMPLATE_OPENED "door.opened"
/* The access machine's home screen, on the same displays: the twelve keys are
 * the view's items in reading order. */
#define XIAOTAI_APP_DOOR_TEMPLATE_KEYPAD "door.keypad"
/* The doorbell's home screen on those displays that have no home template of
 * their own (smaller than the wide templates' home): custom_data
 * xiaotai_app_door_home_view_t; the primary action is the big button, the
 * other labelled ones stand quietly beside it. */
#define XIAOTAI_APP_DOOR_TEMPLATE_BELL "door.bell"

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
    XIAOTAI_APP_DOOR_NO_SERVICE,   /* no longer reported */
} xiaotai_app_door_state_t;

/* custom_data of XIAOTAI_APP_DOOR_TEMPLATE_HOME, _MONITOR and _MODE. */
typedef struct {
    uint8_t state;           /* xiaotai_app_door_state_t */
    bool station;            /* the door station's picture and layout */
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

typedef enum {
    XIAOTAI_APP_DOOR_LOOKED_ONLY = 0,    /* an indoor station looks, nobody talks */
    XIAOTAI_APP_DOOR_LOOKED_RINGING,     /* the visitor calls it meanwhile */
    XIAOTAI_APP_DOOR_LOOKED_TALKING,
} xiaotai_app_door_looked_t;

/* custom_data of XIAOTAI_APP_DOOR_TEMPLATE_LOOKED. */
typedef struct {
    uint8_t state;           /* xiaotai_app_door_looked_t */
    bool camera;             /* this device is seen, not only heard */
} xiaotai_app_door_looked_view_t;

/* custom_data of XIAOTAI_APP_DOOR_TEMPLATE_KEYPAD. */
typedef struct {
    char typed[16];          /* digits of the room, "" at first */
    bool ready;              /* a call can be made */
} xiaotai_app_door_keypad_view_t;

/* custom_data of XIAOTAI_APP_DOOR_TEMPLATE_OPENED. */
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
 *   door.monitor    indoor station: look at the first door station and show
 *                   the monitor page; on that page: look again
 *   door.look.talk, door.look.next, door.look.end, door.look.ignore
 *                   indoor station, on the monitor page: talk, or only look
 *                   again; the next door station; stop looking; the visitor's
 *                   call is not answered. args.value: the call's session (0
 *                   with next and end when none runs)
 *   door.mode       args.value 0: show the choice of mode; 1: indoor station,
 *                   2: doorbell, 3: access machine. Stored at once on a device
 *                   that was not configured yet, after a question otherwise
 *   door.key.digit, door.key.clear, door.key.call
 *                   access machine, on its home screen: a digit of the room
 *                   (args.value; args.text with all of them from a form), none
 *                   again, call that room. Untouched for 15 s the digits go
 *   door.settings   show the settings page; door.remove args.value: the slot
 *                   of a counterpart, removed after a question; door.ask
 *                   args.value: the question's token to say yes, 0 to say no
 *   door.log        indoor station: show the recent door calls
 *   door.open       ask for confirmation; door.open.confirm args.value 1
 *                   opens (or says why not), 0 dismisses. Indoor station
 *                   only. With the lock module it releases this device's
 *                   lock; without it, in a connected door call, it asks the
 *                   door station (call message XIAOTAI_APP_DOOR_MESSAGE_OPEN),
 *                   shows "正在开门…" and the station's answer, or after 4 s
 *                   that the station did not answer
 *   door.ring       door station: call the chosen indoor stations one after
 *                   the other (a video call when the product has video
 *                   calls). While an indoor station looks, tell it that the
 *                   visitor calls (message RING) instead. Ignored on the
 *                   indoor station, while an overlay shows or another session
 *                   runs
 *   door.volume     show the volume slider (door call, connected);
 *                   door.volume.set args.value: percent, with args.text
 *                   XIAOTAI_ARG_LIVE while the slider is dragged. Release
 *                   commits the most recent successful preview, if any.
 *   door.mute       toggle the microphone of the door call, from the state
 *                   the call module reports
 *   door.role       args.value a mode (XIAOTAI_APP_DOOR_INDOOR and so on);
 *                   stored, and the home screen follows it */
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
#define XIAOTAI_INTENT_APP_DOOR_RING "door.ring"
#define XIAOTAI_INTENT_APP_DOOR_KEY "door.key"          /* the main key: bell, answer, hang up; args.value 1: reject */
#define XIAOTAI_INTENT_APP_DOOR_LOOK_TALK "door.look.talk"
#define XIAOTAI_INTENT_APP_DOOR_LOOK_NEXT "door.look.next"
#define XIAOTAI_INTENT_APP_DOOR_LOOK_END "door.look.end"
#define XIAOTAI_INTENT_APP_DOOR_LOOK_IGNORE "door.look.ignore"
#define XIAOTAI_INTENT_APP_DOOR_MODE "door.mode"
#define XIAOTAI_INTENT_APP_DOOR_SETTINGS "door.settings"
#define XIAOTAI_INTENT_APP_DOOR_REMOVE "door.remove"
#define XIAOTAI_INTENT_APP_DOOR_ASK "door.ask"
#define XIAOTAI_INTENT_APP_DOOR_LOG "door.log"
#define XIAOTAI_INTENT_APP_DOOR_KEY_DIGIT "door.key.digit"
#define XIAOTAI_INTENT_APP_DOOR_KEY_CLEAR "door.key.clear"
#define XIAOTAI_INTENT_APP_DOOR_KEY_CALL "door.key.call"
/* Door station: the directory (WeChat users first, then devices), a call
 * from it (args.value: the row's value), and hanging up a WeChat call the
 * station dialled (args.value: its session). */
#define XIAOTAI_INTENT_APP_DOOR_DIRECTORY "door.directory"
#define XIAOTAI_INTENT_APP_DOOR_DIAL "door.dial"
#define XIAOTAI_INTENT_APP_DOOR_WECHAT_HANGUP "door.wechat.hangup"

/* Call messages between the two stations of a door call
 * (xiaotai_call_send_message, XIAOTAI_EVENT_CALL_MESSAGE). Only messages of
 * the call classified as the door call count.
 *   OPEN         indoor -> door station, no payload: open the door
 *   OPEN_RESULT  door station -> indoor, 4 bytes: the xiaotai_err_t of the
 *                attempt, little endian (XIAOTAI_OK opened;
 *                XIAOTAI_ERR_NOT_SUPPORTED no lock and no open_on_screen;
 *                XIAOTAI_ERR_NOT_ALLOWED the asking device is not chosen here
 *                (an access machine also obeys the device it called itself);
 *                otherwise the lock module's result)
 *   LOOK         indoor -> door station, 1 byte: 0 only looks, 1 talks. Sent
 *                when a look connects and whenever it changes
 *   RING         door station -> indoor, 1 byte: 1 the visitor calls the
 *                indoor station that is looking, 0 gave up
 *   HELLO        door station -> indoor, 1 byte: its mode. Sent when a call
 *                the door station placed to one of its indoor stations is
 *                answered. The one message that counts in a call not
 *                classified as a door call: an indoor station that has not
 *                chosen this door station adds it (while its list has room),
 *                so the next call from it is a door call
 * A station that does not know a type ignores it: the visitor's call then ends
 * unanswered after XIAOTAI_APP_DOOR_RING_MS.
 *
 * The first contact pairs the side that was not set up. A doorbell (BELL) that
 * has no indoor station chosen takes the first device that looks at it (LOOK)
 * as its indoor station; an indoor station learns a door station from HELLO. */
#define XIAOTAI_APP_DOOR_MESSAGE_OPEN 1U
#define XIAOTAI_APP_DOOR_MESSAGE_OPEN_RESULT 2U
#define XIAOTAI_APP_DOOR_MESSAGE_LOOK 3U
#define XIAOTAI_APP_DOOR_MESSAGE_RING 4U
#define XIAOTAI_APP_DOOR_MESSAGE_HELLO 5U
#define XIAOTAI_APP_DOOR_RING_MS 30000U

/* Two pager gestures closer together than this are one. */
#define XIAOTAI_APP_DOOR_PAGER_GUARD_MS 350U
/* mode_pages: how long a page stays in front before it is the stored mode. */
#define XIAOTAI_APP_DOOR_MODE_SETTLE_MS 2000U

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
 * The first counterpart: text, the device id of a device contact, at most 64
 * characters; "" when none is chosen. */
#define XIAOTAI_SETTING_APP_DOOR_PEER "door.peer"
#define XIAOTAI_SETTING_KEY_APP_DOOR_PEER "door_peer"
/* The other counterparts, in the order they are called or switched to. */
#define XIAOTAI_SETTING_APP_DOOR_PEER2 "door.peer2"
#define XIAOTAI_SETTING_KEY_APP_DOOR_PEER2 "door_peer2"
#define XIAOTAI_SETTING_APP_DOOR_PEER3 "door.peer3"
#define XIAOTAI_SETTING_KEY_APP_DOOR_PEER3 "door_peer3"
#define XIAOTAI_SETTING_APP_DOOR_PEER4 "door.peer4"
#define XIAOTAI_SETTING_KEY_APP_DOOR_PEER4 "door_peer4"
/* The mode was chosen on the screen: bool, default 0. A device that is a door
 * station, or which has a counterpart, counts as configured without it. */
#define XIAOTAI_SETTING_APP_DOOR_CHOSEN "door.chosen"
#define XIAOTAI_SETTING_KEY_APP_DOOR_CHOSEN "door_chosen"
/* Mode of this device: one of the three, default 0. */
typedef enum {
    XIAOTAI_APP_DOOR_INDOOR = 0,    /* indoor station of an access system (门禁室内机) */
    XIAOTAI_APP_DOOR_BELL,          /* door station of one household (门铃室外机) */
    XIAOTAI_APP_DOOR_ACCESS,        /* door station of a building or a company: rooms (门禁机) */
    /* Indoor station of a doorbell (可视门铃): it rings and answers, and looks at
     * (or listens to) the door; no opening, no other door stations, no list of
     * visits. Its door station is usually a device without a screen. */
    XIAOTAI_APP_DOOR_BELL_INDOOR,
} xiaotai_app_door_mode_t;
#define XIAOTAI_SETTING_APP_DOOR_SENDER "door.sender"
#define XIAOTAI_SETTING_KEY_APP_DOOR_SENDER "door_sender"

/* ------------------------------------------------------------------ sounds */

/* Played at the door when it opens, over the call that asked for it (a
 * product without a mixer, media parameter playout.mixer 0: outside a call
 * only). The application registers it; a later pack with the same name
 * replaces it (xiaotai/sounds.h). */
#define XIAOTAI_APP_DOOR_SOUND_OPENED "door.opened"
/* A key of the access machine; not heard while anything else plays. */
#define XIAOTAI_APP_DOOR_SOUND_KEY "door.key"

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
    bool sender;                /* a door station */
    uint8_t mode;               /* xiaotai_app_door_mode_t */
    bool lock_configured;       /* the lock module is part of the product and has the configured lock */
    bool save_pending;          /* a changed setting has not reached storage yet */
    uint8_t state;              /* xiaotai_app_door_state_t of the door home screen */
    char page[16];              /* id of the page shown, "" in the background */
    uint32_t door_calls;        /* calls classified as door calls since boot */
    uint32_t yielded;           /* call events claim() left to other applications */
    uint32_t opens;             /* confirmed "open" requests */
    uint32_t opens_done;        /* of which released a lock, here or at the door station */
    uint32_t opens_received;    /* door station: "open" requests of the indoor station */
    uint32_t looks;             /* indoor station: looks begun; door station: looks answered */
    uint32_t missed;            /* indoor station: door calls nobody answered since the list was opened */
    uint8_t peers;              /* counterparts chosen */
} xiaotai_app_door_status_t;

/* [owner] */
void xiaotai_app_door_status(xiaotai_app_door_status_t *status);

/* Commands: AT+DOOR (the status line +DOOR: control= accepted= page= role=
 * peer_selected= save_pending= awake= setup= lock_configured=) and its verbs
 * HOME, NEXT, PREVIOUS, VIEWER, SENDER, ACCESS, DEVICES, BACK, DIAL <room>. A
 * verb other than the bare query is accepted only while no overlay shows, no
 * setup guidance owns the screen and no session runs. VIEWER, SENDER and
 * ACCESS store the mode (role=viewer, sender, access; manifest access
 * "guarded"); DIAL calls a room as the keypad does; the bare query reads
 * ("read") and the other verbs only move between pages ("action").
 * Diagnostics section "app.door". Event-log source "door". */

/* ---------------------------------------------------------------- lifecycle */

/* [owner] Move within the foreground home pager, or on to another app. */
xiaotai_err_t xiaotai_app_door_page_step(int direction);

/* Application lifecycle (called by the framework through the generated
 * tables). init: registers the settings, intents, command, diagnostics
 * provider, feedback table and custom templates, and subscribes to the
 * events of the modules that are present. start: reads the stored role and
 * counterparts. foreground(true): shows the door home screen, or the door call
 * when one is running. claim: above. */
xiaotai_err_t xiaotai_app_door_init(void);
void xiaotai_app_door_start(void);
void xiaotai_app_door_foreground(bool foreground);
/* Host tests: forget configuration and state. */
void xiaotai_app_door_reset(void);

#ifdef __cplusplus
}
#endif
