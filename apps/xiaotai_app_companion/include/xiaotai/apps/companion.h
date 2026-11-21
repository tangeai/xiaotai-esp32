// SPDX-License-Identifier: MIT
#pragma once
/* Application "companion": the companion intercom. One application for every
 * board that has a microphone and a speaker: a colour touch display, a small
 * one-bit display operated by one key, or no display at all.
 *
 * What the application is: the business of the product. Which page follows
 * which, what a key or a tap means in each situation, when setup guidance
 * takes the screen, when a ring, an acknowledgement or a spoken hint is heard,
 * which idle expression shows. It draws nothing (views, xiaotai/view.h), reads
 * no input part (interaction profile, xiaotai/interaction.h), drives no light
 * and plays no sound itself (feedback table, xiaotai/feedback.h), and talks to
 * no service: every capability is a base module, used through its intents,
 * events and API.
 *
 * Modules. Required: provisioning, binding, settings and the engines platform
 * (its events and phase: online, offline, stopped on a failure; the retry
 * itself goes through xiaotai_binding_retry()), media (the application reads
 * whether audio is ready), feedback, interaction and sounds. Everything else
 * is optional and taken when the product selects it:
 * ui (any display), contacts, ai_talk, wake, ai_vision, call, wechat,
 * video_call, room, stream, stream_video, recognition, environment. The code
 * that serves an optional module is a source file of its own that the build
 * compiles only when that module is selected; an unselected module leaves no
 * page, intent, setting, tool or byte.
 *
 * Sessions. A control that acts on a running session (a call, the
 * conversation, the room, a web stream) posts XIAOTAI_INTENT_APP_COMPANION_SESSION
 * with the session the view showed, never the module's own intent: the
 * application then calls the module with that session, so a tap meant for
 * an ended session cannot act on the next one, and every refusal reaches the
 * user (XIAOTAI_EVENT_APP_COMPANION_REFUSED). When the session kind, session
 * or peer changes, gestures still in progress are voided
 * (xiaotai_interaction_invalidate).
 *
 * Remote pictures. The application installs no video source. The video call
 * and stream video modules install the UI system's built-in source for their
 * session's generation (xiaotai_ui_video_show) and clear it
 * (xiaotai_ui_video_clear) when the picture or the session ends; the
 * application marks the view that shows the session (the call page, HOME or
 * the stream page) XIAOTAI_VIEW_VIDEO while "video_call.remote" or
 * "stream_video.remote" reports a picture (args.value 1) and removes the mark
 * at 0 or when the session ends. "video_call.unsupported" and
 * "stream_video.unsupported" show "视频格式不支持" on a new incompatibility.
 * Confirmed format recovery for that session dismisses only its own notice.
 *
 * AI tools. With ai_talk and contacts selected the application registers the
 * device tools of the AI service (xiaotai_ai_talk_tool_register): the call
 * tool under the action names "call", "call_contact", "call_device",
 * "device_call", "start_device_call", "call_wechat", "wechat_call",
 * "start_voip_call" and the notification methods "call_intent" and
 * "ai_call_intent", and the contact query "query_contact_status". Their
 * answers are the protocol the service already speaks.
 *
 * One application on different boards. Everything that differs between boards
 * is data chosen by what the board has, never by its name:
 *   - the theme of the UI system draws the same views for the display at hand;
 *   - a style (below) carries the interaction profile, the feedback table, the
 *     entries of the home and menu pages and a few policy bits; the first
 *     registered style whose hardware needs the board satisfies is used.
 * A new kind of board is one more style, registered by the product or added
 * here; nothing existing changes.
 *
 * Spoken verification code. When the code is heard is this application's
 * decision (xiaotai/modules/binding.h), set by the announce_* fields of the
 * configuration: spoken when it becomes visible, repeated at an interval
 * counted from the start or the end of the previous announcement, and, when
 * the product asks for it, only while the setting "guidance.enabled" is on
 * (turning it off stops an automatic announcement in progress). An automatic
 * announcement waits until no feedback sound is pending
 * (xiaotai_feedback_sound_pending(), checked again every
 * XIAOTAI_APP_COMPANION_ANNOUNCE_WAIT_MS) and is made only while this
 * application presents the binding; a refused one (BUSY) is tried again at
 * the next interval. Repeats stop when the code is no longer visible. The
 * "replay" control posts binding.announce and is never gated.
 *
 * Several applications (xiaotai/framework.h, "Which application presents an
 * event"). The application never names another one. Its manifest lists the
 * session, setup and binding events in "handles"; xiaotai_app_companion_claim()
 * accepts an event that starts something to present (an announced call, a
 * session leaving idle, setup guidance or a code becoming needed) or that
 * belongs to what it presents now, and nothing else, so a session's end does
 * not pull it forward again. Brought forward for an event, it gives the
 * screen back with xiaotai_app_return() when that is over. "Home" leads to
 * the product's home application (xiaotai_app_home()) when that is another
 * one; the home pager continues into the other applications at its ends
 * (xiaotai_app_next / _previous) and, entered by the pager, starts on its
 * first or last home screen by the direction the framework reports
 * (xiaotai_app_entry()). It handles the shared intents
 * XIAOTAI_INTENT_SYSTEM_MENU and XIAOTAI_INTENT_SYSTEM_NETWORK (its menu and
 * network pages), which another application's controls may post. It shows
 * views only while it is the foreground application (the framework drops
 * the others).
 *
 * Tags as in xiaotai/framework.h: [glue], [init], [owner], [any]. Every handler of
 * this application runs on the framework owner task. The names of the board's
 * network co-processor (xiaotai/hal.h, AT+COMPANION) are unrelated: everything
 * of this application is spelled xiaotai_app_companion_ / XIAOTAI_APP_COMPANION_. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/feedback.h"
#include "xiaotai/framework.h"
#include "xiaotai/interaction.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

typedef enum {
    XIAOTAI_COMPANION_AI_CONTACT_TOOLS_DISABLED = 0,
    XIAOTAI_COMPANION_AI_CONTACT_TOOLS_LEGACY,
    XIAOTAI_COMPANION_AI_CONTACT_TOOLS_STANDARD,
} xiaotai_companion_ai_contact_tools_t;

enum {
    XIAOTAI_COMPANION_BINDING_RETRY_DEFAULT = 0,
    XIAOTAI_COMPANION_BINDING_RETRY_HEADER,
    XIAOTAI_COMPANION_BINDING_RETRY_REFRESH,
};

/* Product configuration. Every field: NULL or 0 selects the stated default,
 * so a zeroed struct is valid. Strings are not copied (static storage). */
typedef struct {
    /* Style by name; NULL: the first registered style the board satisfies. */
    const char *style;
    /* Name shown on the home page and in setup guidance; NULL: "小钛". */
    const char *title;
    /* Address of the portal where the verification code is entered, shown
     * with the code on the binding page. NULL: the page shows the hint for
     * the mini program instead of an address. */
    const char *binding_url;
    /* Binding failure hint and retry control: generic, header icon, or
     * refresh-code button. Zero keeps the existing default presentation. */
    uint8_t binding_retry_control;
    /* Contacts shown per page of the contact list and of the list of pending
     * requests; default 8 and 4, at most XIAOTAI_VIEW_ITEMS_MAX - 1. */
    uint8_t contacts_per_page;
    uint8_t requests_per_page;
    /* Spoken verification code (see "Spoken verification code" above).
     * announce_on_code: speak it when it becomes visible; default false, only
     * the replay control speaks it. announce_repeat_s: then again every so
     * many seconds while it is visible; 0: once. announce_repeat_from_end:
     * count the interval from the end of the previous announcement
     * (XIAOTAI_EVENT_BINDING_ANNOUNCED); false: from its start.
     * announce_follows_guidance: automatic announcements only while
     * "guidance.enabled" is on (a product without that setting counts as on). */
    bool announce_on_code;
    uint16_t announce_repeat_s;
    bool announce_repeat_from_end;
    bool announce_follows_guidance;
    /* Only expose six ASCII digits in the binding guide; the owner still reads its full code. */
    bool binding_guide_six_digits;
    /* Where earlier firmware of the products differed, the product keeps its
     * own handling (the zero value is the first form named):
     * save_failure_on_home: a setting that was not saved (setting.not_saved)
     * is reported on the settings page and in a toast; true: in the status
     * text of HOME for XIAOTAI_APP_COMPANION_SAVE_FAILURE_MS instead.
     * save_failure_toast: with save_failure_on_home, also a toast at once,
     * on whatever page this application shows while it is in the foreground
     * (the application's text "设置保存失败，重启后可能恢复原设置"); without
     * save_failure_on_home it changes nothing.
     * room_leave_confirm: leaving the room acts at once; true: the confirm
     * overlay asks first.
     * room_keeps_talk: entering the room page ends a running conversation
     * on the first room-page entry; true: the room
     * page is exempt (the other function pages still end it).
     * hold_refuses_sessions: a held failed session (context "error") lets new
     * sessions in; true: while it is held the application keeps its own
     * activity XIAOTAI_APP_COMPANION_ACTIVITY, so sessions that do not
     * outrank it are refused (an incoming call is answered busy by its
     * module) until the user dismisses the failure. */
    bool save_failure_on_home;
    /* Reselecting the current screen timeout or reply voice saves it again. */
    bool save_choice_on_reselect;
    /* Ignore invalid local timeout/reply-voice choices instead of showing a refusal. */
    bool ignore_invalid_choice_values;
    /* Applying the current idle expression from its preview saves it again. */
    bool save_emotion_on_reapply;
    bool room_leave_confirm;
    bool room_keeps_talk;
    /* Key UI can cancel a reprovision begin before the network engine takes it. */
    bool provisioning_cancel_from_keys;
    /* Keep room pages and their read-only navigation during an environment switch. */
    bool room_navigation_during_environment;
    /* This touch control closes its matched room gate in the UI task. */
    bool room_local_hold;
    /* Ticketed room hold, renewed only by observed pointer input. */
    bool room_pointer_hold;
    /* Use camera-phase copy for live viewing instead of the fixed notice. */
    bool stream_camera_status;
    /* Built-in pointer style: handle the main key on the debounced press
     * rather than a click. Other inputs and styles keep their gestures. */
    bool main_button_on_press;
    /* Play the normal/failed cue when an owned room connection attempt ends. */
    bool room_end_feedback;
    /* Preserve the full Room key menu before assignment, with inert talk and leave. */
    bool room_full_unassigned_key_menu;
    const char *stream_active_text; /* NULL keeps the standard stream notice. */
    bool stream_show_elapsed;
    bool hold_refuses_sessions;
    bool save_failure_toast;
    /* Keep saved Wi-Fi while entering the setup portal. The default
     * confirmation requests explicit forgetting before a new setup. */
    bool keep_saved_wifi_on_reprovision;
    /* True: selecting a contact calls it in voice mode (original key UI).
     * Default false: open details for an explicit dial confirmation. */
    bool contacts_dial_on_select;
    /* True: number unnamed WeChat contacts in the selected list (key UI).
     * Default false: a generic WeChat contact label (pointer UI). */
    bool contacts_number_unnamed_wechat;
    /* Confirm reloads an authoritative empty WeChat or request list on page zero. */
    bool contacts_refresh_empty_on_confirm;
    /* Suppress a notification superseded by a newer value for the same key.
     * Default false preserves the legacy notification of every write error. */
    bool save_only_current_revision;
    /* Display the existing caption speaker as a local label. */
    bool ai_caption_speaker_prefix;
    /* Show elapsed active time on the dedicated AI conversation page. */
    bool ai_talk_show_elapsed;
    /* True: request the wake confirmation when AI is admitted. False:
     * wait for the opened media generation. Neither plays over a session. */
    bool wake_ack_on_admission;
    /* Select explicit tool wire policy; DISABLED is the default. */
    xiaotai_companion_ai_contact_tools_t ai_contact_tools;
    /* In the HOME network label, wait for RTC as well as the platform.
     * Default false keeps the label driven by the network and platform. */
    bool home_online_requires_transport;
} xiaotai_app_companion_config_t;

/* How long HOME shows a save failure with save_failure_on_home. */
#define XIAOTAI_APP_COMPANION_SAVE_FAILURE_MS 5000U

/* The foreground activity the application begins while it holds a failed
 * session with hold_refuses_sessions (manifest "activities", default
 * priority 1; the product's priority table may rank it). No device resource
 * is used; it only occupies the foreground. */
#define XIAOTAI_APP_COMPANION_ACTIVITY "companion_hold"

/* [glue] Before xiaotai_framework_start(), by the product glue; the struct is
 * copied. NULL or never called: all defaults. XIAOTAI_ERR_INVALID_ARG for a
 * page size above the limit or announce_repeat_s above 3600;
 * XIAOTAI_ERR_INVALID_STATE after the application's init. An unknown style
 * name is reported by init (XIAOTAI_ERR_NOT_FOUND), which stops the start. */
xiaotai_err_t xiaotai_app_companion_configure(const xiaotai_app_companion_config_t *config);

/* An automatic announcement that found a feedback sound pending looks again
 * after this long. */
#define XIAOTAI_APP_COMPANION_ANNOUNCE_WAIT_MS 200U

/* [owner] Whether this application presents the event
 * (xiaotai_application_t.claim, named in the manifest by "claims"); see
 * "Several applications" above. Answers from memory, changes nothing. */
bool xiaotai_app_companion_claim(const xiaotai_event_t *event);

/* ------------------------------------------------------------------- pages */

/* Page ids: the id of the view a page shows (xiaotai_view_t.id), the text of
 * XIAOTAI_INTENT_APP_COMPANION_SHOW, the entries of a style and what
 * diagnostics print. A page whose module is not part of the product does not
 * exist: showing it is refused and entries naming it are skipped. Appended,
 * never renamed. */
#define XIAOTAI_APP_COMPANION_PAGE_HOME "home"                /* HOME; with two home screens, value 1 is the clock */
#define XIAOTAI_APP_COMPANION_PAGE_MENU "menu"                /* LIST of the style's menu entries */
#define XIAOTAI_APP_COMPANION_PAGE_TALK "talk"                /* TALK: the conversation (ai_talk) */
#define XIAOTAI_APP_COMPANION_PAGE_CONTACTS "contacts"        /* LIST (contacts); value: kind filter, see below */
#define XIAOTAI_APP_COMPANION_PAGE_CONTACT "contact"          /* NOTICE: one contact, dial actions */
#define XIAOTAI_APP_COMPANION_PAGE_REQUESTS "requests"        /* LIST: pending contact requests */
#define XIAOTAI_APP_COMPANION_PAGE_REQUEST "request"          /* NOTICE: accept or reject one request */
#define XIAOTAI_APP_COMPANION_PAGE_EMOTIONS "emotions"        /* LIST with XIAOTAI_VIEW_GRID: idle expressions */
#define XIAOTAI_APP_COMPANION_PAGE_EMOTION "emotion"          /* TALK: preview of one expression */
#define XIAOTAI_APP_COMPANION_PAGE_SETTINGS "settings"        /* FORM: the settings that exist on this board */
#define XIAOTAI_APP_COMPANION_PAGE_NETWORK "network"          /* NOTICE: link, address, signal, services */
#define XIAOTAI_APP_COMPANION_PAGE_STATUS "status"            /* NOTICE: diagnostics sections as tabs */
#define XIAOTAI_APP_COMPANION_PAGE_AUDIO "audio"              /* LIST: tone, record, replay, stop */
#define XIAOTAI_APP_COMPANION_PAGE_ROOM "room"                /* LIST: the room, its members, push to talk */
#define XIAOTAI_APP_COMPANION_PAGE_ROOM_FORM "room.form"      /* FORM: room code and password digits */
#define XIAOTAI_APP_COMPANION_PAGE_RECOGNITION "recognition"  /* NOTICE: sound recognition results */
#define XIAOTAI_APP_COMPANION_PAGE_CALL "call"                /* CALL: device call, WeChat call, with or without video */
#define XIAOTAI_APP_COMPANION_PAGE_RESULT "result"            /* NOTICE: how the last session ended */
#define XIAOTAI_APP_COMPANION_PAGE_STREAM "stream"            /* NOTICE: a web stream runs (styles with FOLLOW_SESSIONS) */
/* Pages the application shows by itself; they cannot be asked for. */
#define XIAOTAI_APP_COMPANION_PAGE_PREPARING "preparing"      /* PROGRESS: the network part or the services start */
#define XIAOTAI_APP_COMPANION_PAGE_SETUP "setup"              /* NOTICE: how to give the device its network */
#define XIAOTAI_APP_COMPANION_PAGE_BINDING "binding"          /* CODE: the verification code */
#define XIAOTAI_APP_COMPANION_PAGE_MAINTENANCE "maintenance"  /* PROGRESS: the system is quiesced */
/* Overlay ids. */
#define XIAOTAI_APP_COMPANION_OVERLAY_CODE "wechat.code"      /* CODE with image: the mini-program code */
#define XIAOTAI_APP_COMPANION_OVERLAY_CONFIRM "confirm"       /* NOTICE: a question with two actions */
#define XIAOTAI_APP_COMPANION_OVERLAY_RECORD "record"         /* FORM: recording length */
#define XIAOTAI_APP_COMPANION_OVERLAY_TASK "task"             /* LIST: operations on a recognition task */

/* Value of the contacts page: which contacts it lists. */
enum {
    XIAOTAI_APP_COMPANION_CONTACTS_ALL = 0,
    XIAOTAI_APP_COMPANION_CONTACTS_DEVICE,
    XIAOTAI_APP_COMPANION_CONTACTS_WECHAT,
};

/* Name of the picture the application registers with the UI system for the
 * mini-program code (xiaotai_ui_image_register). */
#define XIAOTAI_APP_COMPANION_IMAGE_WECHAT_CODE "companion.wechat_code"

/* ------------------------------------------------------------------ styles */

/* xiaotai_app_companion_style_t.policy: the interaction decisions that differ
 * between a touch display and a display operated by keys. Appended; a bit
 * never changes its meaning. */
enum {
    /* The conversation is shown on HOME (expression and caption); without the
     * bit it has its own TALK page, entered from HOME. */
    XIAOTAI_APP_COMPANION_TALK_ON_HOME = 1u << 0,
    /* Entering a function page (contacts, contact, emotions, settings,
     * network, room) ends a running conversation first; the page is refused
     * when the stop cannot be posted. */
    XIAOTAI_APP_COMPANION_PAGES_END_TALK = 1u << 1,
    /* Every call ends on the result page, which stays until the user leaves
     * it. Without the bit only a failed session is held (until it is
     * dismissed) and a normal end returns to where the session began. */
    XIAOTAI_APP_COMPANION_RESULT_ALWAYS = 1u << 2,
    /* A session that begins (a conversation, a call, a stream, the room)
     * takes the page of that session whatever page shows. Without the bit
     * only calls do, and the others show as the status of HOME. */
    XIAOTAI_APP_COMPANION_FOLLOW_SESSIONS = 1u << 3,
    /* While offline, operating anything but the setup pages, leaving a page
     * and what a page marks as available offline is dropped. Without the bit
     * navigation stays available and each page says what is missing. */
    XIAOTAI_APP_COMPANION_OFFLINE_GATE = 1u << 4,
    /* HOME has a second screen with the clock, reached by the pager. */
    XIAOTAI_APP_COMPANION_CLOCK_HOME = 1u << 5,
    /* The status of HOME is the network state in one word (maintenance,
     * offline, connecting, online) instead of what the session does. */
    XIAOTAI_APP_COMPANION_HOME_NETWORK_STATUS = 1u << 6,
    /* Show key gestures when ringing. During dial/connect/active, Back
     * keeps ending the call without a footer label. A changed device peer
     * invalidates pending key gestures even if its display name is unchanged. */
    XIAOTAI_APP_COMPANION_KEY_CALL_HINTS = 1u << 7,
};

/* How the application behaves on one kind of board. Everything is constant
 * data kept by pointer. */
typedef struct {
    const char *name;                       /* "pointer", "keys", ...; unique, at most 15 characters */
    /* The style applies to a board that has all of needs_all and none of
     * needs_none (XIAOTAI_NEED_*, xiaotai/needs.h). */
    uint32_t needs_all;
    uint32_t needs_none;
    /* Registered with the interaction engine when the style is chosen; NULL:
     * the board's inputs do nothing for this application. */
    const xiaotai_interaction_profile_t *interaction;
    /* Registered with the feedback engine when the style is chosen; NULL: no
     * lights, vibration or sounds from this application. State and effect
     * names are the XIAOTAI_APP_COMPANION_STATE_ / _EFFECT_ names below; a
     * name the table lacks is simply not presented. */
    const xiaotai_feedback_table_t *feedback;
    /* Pages offered as the items of HOME and as the entries of the menu page,
     * in order; NULL-terminated; NULL: none (no menu page). An entry is a
     * page id, optionally followed by ':' and the page's argument in decimal
     * ("contacts:2": the WeChat contacts). */
    const char *const *home_entries;
    const char *const *menu_entries;
    uint32_t policy;                        /* XIAOTAI_APP_COMPANION_* policy bits */
    /* Toast shown for XIAOTAI_EVENT_APP_COMPANION_REFUSED, and how long;
     * NULL / 0: "操作未受理，请重试" for XIAOTAI_TOAST_DEFAULT_MS. A refusal that has
     * a text of its own (the device is not ready, settings are frozen) keeps
     * that text and takes only the duration. */
    const char *refused_text;
    uint16_t toast_ms;
} xiaotai_app_companion_style_t;

/* Built-in styles, registered by the application's init after the product's
 * own, so a product style wins when both apply.
 *   "pointer"  a display with a pointer: the home page is the expression, a
 *              tap starts the conversation, the board's main key toggles it
 *   "keys"     a display without a pointer and a key: one key steps, confirms
 *              and goes back through list pages
 *   "plain"    no display: the main key toggles the conversation and answers
 *              calls; lights and spoken hints present the state */
#define XIAOTAI_APP_COMPANION_STYLE_POINTER "pointer"
#define XIAOTAI_APP_COMPANION_STYLE_KEYS "keys"
#define XIAOTAI_APP_COMPANION_STYLE_PLAIN "plain"

#define XIAOTAI_APP_COMPANION_STYLES_MAX 8U

/* [init] Before xiaotai_framework_start() (product glue) or from another
 * component's init that runs before this application's. Kept by pointer.
 * XIAOTAI_ERR_INVALID_ARG: no name, a name longer than 15 characters, or
 * needs_all and needs_none overlap; XIAOTAI_ERR_INVALID_STATE: the name is
 * taken, or the application is initialized; XIAOTAI_ERR_NO_MEM: more than
 * XIAOTAI_APP_COMPANION_STYLES_MAX. */
xiaotai_err_t xiaotai_app_companion_style_register(const xiaotai_app_companion_style_t *style);

/* ----------------------------------------------------------------- intents */

/* Intents this application handles. Views, interaction profiles, the console
 * (AT+INTENT) and voice commands post them. Module intents that need no
 * session (audio.*, binding.*, provisioning.*, contacts.refresh) are posted
 * by views directly; everything that acts on a session goes through
 * companion.session (see "Sessions" above). While the application is in the
 * background only SHOW and MAIN are handled (MAIN acts on the module;
 * whichever application presents the session then shows it); the others are
 * ignored and counted.
 *
 *   companion.session   args.text: the action, args.value: the session or
 *                       generation the view showed, as the module reported
 *                       it (uint32_t carried in the int32_t; 0 only for
 *                       "talk.start" and "retry"). Actions:
 *                       "accept", "reject", "hangup", "mute" (the call of
 *                       the page: device call or WeChat call), "camera"
 *                       (video call), "talk.start", "talk.stop",
 *                       "talk.interrupt", "stream.stop", "room.disconnect",
 *                       "retry" (the services stopped on a failure:
 *                       xiaotai_binding_retry(), which in the phases UNKNOWN
 *                       and BOUND retries the platform's failed step and
 *                       otherwise starts a binding attempt). The module's own function is
 *                       called with that session; any error, a stale session
 *                       included, emits XIAOTAI_EVENT_APP_COMPANION_REFUSED
 *                       (shown only while that page and session are still
 *                       current).
 *
 *   companion.show      args.text: a page id; args.value: the page's
 *                       argument. Brings the application to the foreground.
 *                       Refused (toast, XIAOTAI_EVENT_APP_COMPANION_REFUSED)
 *                       for an unknown or absent page, while setup guidance
 *                       or a call owns the screen, and by the offline gate.
 *   companion.back      leave the page for the one it was entered from; on
 *                       HOME, when the product's home application is another
 *                       one: back to it (xiaotai_app_return() when the
 *                       framework brought this application forward, else a
 *                       switch to the home application)
 *   companion.home      args.value 0: the first home screen, or the product's
 *                       home application when that is another one; 1: the
 *                       next screen of the pager; -1: the previous one. Past
 *                       the last or the first home screen the pager goes on
 *                       with xiaotai_app_next / _previous, and wraps around
 *                       when the product has no other application
 *   companion.main      what the main key means now: toggle the conversation
 *                       when idle or talking (start only with args.value 1),
 *                       accept an announced call, nothing during a call
 *   companion.end       what "end" means now: reject an announced call, hang
 *                       up, stop the stream, leave the conversation, dismiss
 *                       a held result; otherwise as companion.back
 *   companion.rest      stop the conversation, show HOME and let the screen
 *                       sleep; sounds do not wake it for the next 2500 ms
 *   companion.contact   args.value: index in the page of contacts shown ->
 *                       the contact page
 *   companion.dial      call the contact of the contact page (args.value 0
 *                       voice, 1 video), or with args.text "first": the first
 *                       WeChat contact, the mini-program code when there is none
 *   companion.request   args.value: index in the page of requests shown ->
 *                       the request page; companion.decide args.value 1
 *                       accepts and 0 rejects the request of that page
 *   companion.emotion   args.value: index of xiaotai_app_companion_emotion()
 *                       -> the preview page; companion.emotion_apply stores
 *                       the previewed one as the idle expression
 *   companion.setting.<setting name>
 *                       one intent per field of the settings page, for
 *                       example "companion.setting.screen.sleep"; args.value:
 *                       the field's new value. The application applies it
 *                       through the settings registry and shows the page
 *                       again with the value that was accepted.
 *   companion.confirm   args.value 1: do what the confirm overlay asks; 0:
 *                       dismiss it
 *   companion.room      args.text: "create", "join", "submit", "leave" or
 *                       "page" (args.value -1 / 1: previous / next members)
 *   companion.room.talk the push-to-talk control, a HOLD action: args.text
 *                       XIAOTAI_ARG_PRESS (repeated while held) or
 *                       XIAOTAI_ARG_RELEASE (xiaotai/view.h)
 *   companion.audio     args.value 0 tone, 1 record, 2 replay, 3 stop
 *   companion.recognition  args.text: "record", "stop", "resume", "skip",
 *                       "archive", "detail", "next", "previous", "refresh"
 *   companion.reprovision  start over with new network credentials after
 *                       the confirm overlay (the binding is kept) */
#define XIAOTAI_INTENT_APP_COMPANION_SESSION "companion.session"
#define XIAOTAI_INTENT_APP_COMPANION_SHOW "companion.show"
#define XIAOTAI_INTENT_APP_COMPANION_BACK "companion.back"
#define XIAOTAI_INTENT_APP_COMPANION_HOME "companion.home"
#define XIAOTAI_INTENT_APP_COMPANION_MAIN "companion.main"
#define XIAOTAI_INTENT_APP_COMPANION_END "companion.end"
#define XIAOTAI_INTENT_APP_COMPANION_REST "companion.rest"
#define XIAOTAI_INTENT_APP_COMPANION_CONTACT "companion.contact"
#define XIAOTAI_INTENT_APP_COMPANION_DIAL "companion.dial"
#define XIAOTAI_INTENT_APP_COMPANION_REQUEST "companion.request"
#define XIAOTAI_INTENT_APP_COMPANION_DECIDE "companion.decide"
#define XIAOTAI_INTENT_APP_COMPANION_EMOTION "companion.emotion"
#define XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY "companion.emotion_apply"
#define XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX "companion.setting."
#define XIAOTAI_INTENT_APP_COMPANION_CONFIRM "companion.confirm"
#define XIAOTAI_INTENT_APP_COMPANION_ROOM "companion.room"
#define XIAOTAI_INTENT_APP_COMPANION_ROOM_TALK "companion.room.talk"
#define XIAOTAI_INTENT_APP_COMPANION_AUDIO "companion.audio"
#define XIAOTAI_INTENT_APP_COMPANION_RECOGNITION "companion.recognition"
#define XIAOTAI_INTENT_APP_COMPANION_REPROVISION "companion.reprovision"

/* A request is "pending" from the moment its intent was posted until the
 * module's state changed or this time passed: the same request is ignored,
 * its control shows XIAOTAI_STATE_BUSY and the status says what is under way. */
#define XIAOTAI_APP_COMPANION_PENDING_MS 1500U

/* ------------------------------------------------------------------ events */

/* The page changed. args.text: the page id; args.value: 1 when the
 * application took the page by itself (setup guidance, a call), else 0. */
#define XIAOTAI_EVENT_APP_COMPANION_PAGE "companion.page"
/* Something the user asked for was not done: an intent was refused, or the
 * module answered later that it could not. args.text: the intent or page;
 * args.value: the xiaotai_err_t. The style's feedback table binds its
 * "refused" effect to it; displays show a toast. */
#define XIAOTAI_EVENT_APP_COMPANION_REFUSED "companion.refused"

/* ---------------------------------------------------------------- settings */

/* Settings this application registers (xiaotai/framework.h). Storage keys are
 * what a product's preference binding table (xiaotai/preferences.h) names to
 * keep the values deployed devices hold.
 *
 * Idle expression: int, an index of xiaotai_app_companion_emotion(), default
 * 0. The order of that table is storage format. */
#define XIAOTAI_SETTING_APP_COMPANION_EMOTION "companion.emotion"
#define XIAOTAI_SETTING_KEY_APP_COMPANION_EMOTION "emoji"
/* Voice of the acknowledgement after the wake word: int, 0 the female voice,
 * 1 the male one; default 0. Registered when the product has the wake module. */
#define XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE "companion.ack_voice"
#define XIAOTAI_SETTING_KEY_APP_COMPANION_ACK_VOICE "ack_voice"

/* ---------------------------------------------------------------- emotions */

/* Expressions a user can choose for the idle screen, by stable index: stored
 * devices hold the index. Entries are appended, never reordered or removed. */
#define XIAOTAI_APP_COMPANION_EMOTIONS 23U

/* [any] Name of expression `index` as themes know it ("neutral", "happy",
 * ...; xiaotai_view_t.emotion), NULL at or beyond XIAOTAI_APP_COMPANION_EMOTIONS.
 * Static storage. */
const char *xiaotai_app_companion_emotion(size_t index);

/* [any] Name of the expression for an emotion word of the AI service
 * (xiaotai_ai_talk_caption_t.emotion, passed on unchanged by the module): the
 * protocol's Chinese words map to theme names (中性 neutral, 开心 happy,
 * 兴奋 excited, 温和 calm, 安慰 loving, 思考 thinking, 惊讶 surprised,
 * 严肃 confident, 困倦 sleepy, 困惑 confused); a word that already is a
 * theme name is returned as it is; NULL, "" and unknown words give
 * "neutral". A conversation turn without a word shows "neutral". Static
 * storage. */
const char *xiaotai_app_companion_emotion_face(const char *word);

/* Step of the microphone gain on the settings page, dB. Stored values are
 * kept as they are; the input part applies what its hardware can (a part
 * with a coarser step rounds down to its own step). */
#define XIAOTAI_APP_COMPANION_GAIN_STEP_DB 3U

/* -------------------------------------------------------- feedback, input */

/* States the application switches (xiaotai_feedback_set) and effects it
 * fires (xiaotai_feedback_fire); a style's feedback table gives them lights,
 * vibration and sounds. Several states may be on; the table's priorities
 * decide which one shows. */
#define XIAOTAI_APP_COMPANION_STATE_STARTING "companion.starting"    /* until the services answered once */
#define XIAOTAI_APP_COMPANION_STATE_SETUP "companion.setup"          /* the device waits for its network */
#define XIAOTAI_APP_COMPANION_STATE_BINDING "companion.binding"      /* the device waits to be bound */
#define XIAOTAI_APP_COMPANION_STATE_OFFLINE "companion.offline"      /* bound, not online */
#define XIAOTAI_APP_COMPANION_STATE_ONLINE "companion.online"
#define XIAOTAI_APP_COMPANION_STATE_TALK "companion.talk"            /* a conversation runs */
#define XIAOTAI_APP_COMPANION_STATE_INCOMING "companion.incoming"    /* an announced call rings */
#define XIAOTAI_APP_COMPANION_STATE_OUTGOING "companion.outgoing"    /* the dialled call is not answered yet */
#define XIAOTAI_APP_COMPANION_STATE_CALL "companion.call"            /* a call is connected */
#define XIAOTAI_APP_COMPANION_STATE_ERROR "companion.error"          /* a failed session is held */

#define XIAOTAI_APP_COMPANION_EFFECT_ACK_FEMALE "companion.ack.female"    /* the wake word was accepted */
#define XIAOTAI_APP_COMPANION_EFFECT_ACK_MALE "companion.ack.male"
#define XIAOTAI_APP_COMPANION_EFFECT_STARTUP "companion.startup"
#define XIAOTAI_APP_COMPANION_EFFECT_SETUP "companion.setup.opened"       /* how to set the network up */
#define XIAOTAI_APP_COMPANION_EFFECT_CONNECTING "companion.connecting"
#define XIAOTAI_APP_COMPANION_EFFECT_CONNECTED "companion.connected"      /* credentials verified and stored */
#define XIAOTAI_APP_COMPANION_EFFECT_SETUP_FAILED "companion.setup.failed"
#define XIAOTAI_APP_COMPANION_EFFECT_BOUND "companion.bound"
#define XIAOTAI_APP_COMPANION_EFFECT_ONLINE "companion.came_online"
#define XIAOTAI_APP_COMPANION_EFFECT_OFFLINE "companion.went_offline"
#define XIAOTAI_APP_COMPANION_EFFECT_INCOMING "companion.cue.incoming"    /* spoken once where nothing rings */
#define XIAOTAI_APP_COMPANION_EFFECT_DIALING "companion.cue.dialing"
#define XIAOTAI_APP_COMPANION_EFFECT_ENDED "companion.cue.ended"
#define XIAOTAI_APP_COMPANION_EFFECT_FAILED "companion.cue.failed"
#define XIAOTAI_APP_COMPANION_EFFECT_REFUSED "companion.refused"

/* Sound group of the automatic spoken guidance (startup, setup, connecting,
 * connected, bound, online): the setting "guidance.enabled" switches it.
 * Rows of a style's table that belong to it carry this group. */
#define XIAOTAI_APP_COMPANION_GROUP_GUIDANCE 1U

/* Interaction contexts the application sets (xiaotai_interaction_set_context)
 * and a style's profile matches its rules against. */
#define XIAOTAI_APP_COMPANION_CONTEXT_SETUP "setup"       /* setup guidance owns the screen */
#define XIAOTAI_APP_COMPANION_CONTEXT_IDLE "idle"
#define XIAOTAI_APP_COMPANION_CONTEXT_TALK "talk"
#define XIAOTAI_APP_COMPANION_CONTEXT_RINGING "ringing"
#define XIAOTAI_APP_COMPANION_CONTEXT_CALL "call"         /* dialling, connecting or connected */
#define XIAOTAI_APP_COMPANION_CONTEXT_SESSION "session"   /* the room or a stream */
#define XIAOTAI_APP_COMPANION_CONTEXT_ERROR "error"       /* a failed session is held */

/* ------------------------------------------------------------- diagnostics */

typedef struct {
    bool foreground;
    bool setup;                 /* setup guidance owns the screen */
    bool error_held;            /* a failed session waits to be dismissed */
    char style[16];
    char page[16];              /* id of the page shown, "" in the background */
    char context[XIAOTAI_INTERACTION_CONTEXT_MAX];
    uint32_t pages;             /* page changes since boot */
    uint32_t refused;           /* XIAOTAI_EVENT_APP_COMPANION_REFUSED emitted */
    uint32_t stale;             /* intents dropped because the page or session they were meant for is gone */
} xiaotai_app_companion_status_t;

/* [owner] */
void xiaotai_app_companion_status(xiaotai_app_companion_status_t *status);

/* Commands: the verb DISMISS of AT+CALL (dismisses a held failed session;
 * registered with xiaotai_at_verbs_register, as companion.end). Diagnostics
 * section "app.companion". Event-log source "companion". */

/* ---------------------------------------------------------------- lifecycle */

/* Application lifecycle (called by the framework through the generated
 * tables). init: chooses the style, registers its profile and table, the
 * intents (its own and the shared "system.menu" and "system.network" when its
 * style has those pages), settings, the command verb and the diagnostics
 * provider, the picture of the mini-program code, and subscribes to the
 * events of the modules that are present. start: applies the stored settings
 * it owns. foreground(true): shows the page the present state asks for.
 * claim: above. */
xiaotai_err_t xiaotai_app_companion_init(void);
void xiaotai_app_companion_start(void);
void xiaotai_app_companion_foreground(bool foreground);
xiaotai_err_t xiaotai_app_companion_page_step(int direction);
/* Host tests: forget configuration, styles and state. */
void xiaotai_app_companion_reset(void);

#ifdef __cplusplus
}
#endif
