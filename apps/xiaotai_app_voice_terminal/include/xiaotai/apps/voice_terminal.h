// SPDX-License-Identifier: MIT
#pragma once
/* Application "voice_terminal": a voice terminal without a display. One key,
 * a row of lights, a speaker and a microphone; it calls and is called over
 * WeChat voice.
 *
 * What the application is: what the key means in each situation, which light
 * pattern and which sound present each state, and the two local resets. All
 * of it is three tables and a few rules:
 *   - an interaction profile (xiaotai/interaction.h): the key's gestures to
 *     the four intents below;
 *   - a feedback table (xiaotai/feedback.h): the states and effects named
 *     below to light patterns, rings and cues;
 *   - the rules of this header: what "main" and "end" do in each call state,
 *     and when a gesture is too old to act on.
 * The application has no view and needs no UI system.
 *
 * Spoken verification code. Without a display, hearing the code is the only
 * way to bind, so when it is heard is this application's rule (the binding
 * module only speaks it once when asked, xiaotai/modules/binding.h), set by
 * the announce_* fields of the configuration: spoken when it becomes visible
 * (XIAOTAI_EVENT_BINDING_CODE), repeated at an interval counted from the start
 * or the end (XIAOTAI_EVENT_BINDING_ANNOUNCED) of the previous announcement,
 * and, when the product asks for it, only while the setting
 * "guidance.enabled" is on (turning it off stops an automatic announcement in
 * progress; without the settings module it counts as on). An announcement
 * waits until no feedback sound is pending (xiaotai_feedback_sound_pending(),
 * checked again every XIAOTAI_APP_VOICE_TERMINAL_ANNOUNCE_WAIT_MS); a refused
 * one (BUSY) is tried again at the next interval; repeats stop when the code
 * is no longer visible. Besides, the application switches the binding state
 * on and off.
 *
 * Modules. Required: wechat (which brings contacts), provisioning, binding
 * and the engines feedback, interaction and sounds. Optional: environment
 * (a reset cancels a service environment switch that has not started writing
 * and runs once the switch is released; while the switch writes or restarts
 * the reset is refused) and settings (for the fixed volume and the binding
 * announcement guidance setting). Lights are not required: on a board without them
 * the light patterns of the table do nothing.
 *
 * Credentials that were verified but could not be stored are stored again
 * every store_retry_ms (xiaotai_provisioning_retry), with the "refused"
 * warning each time, until they are.
 *
 * Key rules (one key with the role "main"):
 *   main   idle: call the configured contact (the first WeChat contact);
 *          when the service is not ready the request is refused; when the
 *          contact list is empty or outdated it is refreshed and the request
 *          is refused. Announced call: accept. Dialling, connecting or
 *          connected: nothing, and the "blocked" effect says that ending a
 *          call takes the other gesture.
 *   end    announced call: reject. Dialling, connecting or connected: hang
 *          up. Otherwise: refused.
 * A gesture belongs to what the user saw when it began. At the first press
 * (XIAOTAI_EVENT_INPUT_BEGAN) the application notes the call's session and
 * which of the three situations above holds; an intent that arrives when
 * either changed is refused. Refusals fire the "refused" effect unless the
 * product suppresses delayed warnings for a superseded key gesture.
 * The two resets act in every call state without a call intent. The
 * selected restart policy determines whether active work is quiesced.
 *
 * Tags as in xiaotai/framework.h: [init], [owner], [any]. Every handler of
 * this application runs on the framework owner task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

/* Product configuration. Every field: 0 (false) selects the stated default,
 * so a zeroed struct is valid. */
typedef struct {
    /* Index, among the WeChat contacts, of the contact "main" calls when
     * idle; default 0, the first. */
    uint8_t dial_contact;
    /* The gestures for the two local resets are not offered (their intents
     * are then not registered). Default false: both are offered. */
    bool no_network_reset;
    bool no_binding_reset;
    /* Longest wait for the confirmation sound before restarting anyway,
     * milliseconds; default 30000. For a reset or stored credentials the
     * selected framework restart waits on the application's hold
     * (xiaotai_restart_hold) from before the request until the
     * confirmation sound ended or this time passed, and the device restarts
     * XIAOTAI_FRAMEWORK_RESTART_DELAY_MS after the hold is released (a build
     * option of the product). */
    uint32_t confirm_timeout_ms;
    /* Interval of the automatic retry after verified credentials could not
     * be stored, milliseconds; default 2000. */
    uint16_t store_retry_ms;
    /* Spoken verification code (see above). announce_on_code: speak it when
     * it becomes visible; default false. announce_repeat_s: then again every
     * so many seconds while it is visible; 0: once.
     * announce_repeat_from_end: count the interval from the end of the
     * previous announcement; false: from its start.
     * announce_follows_guidance: automatic announcements only while
     * "guidance.enabled" is on. */
    bool announce_on_code;
    uint16_t announce_repeat_s;
    bool announce_repeat_from_end;
    bool announce_follows_guidance;
    /* Delayed physical-post refusals after a newer gesture began are logged
     * but do not replace the newer gesture's light feedback. Default false. */
    bool quiet_superseded_gesture;
    /* Local binding reset commits then uses the direct restart policy:
     * no preemptive hangup or unrelated settings flush. Default false. */
    bool direct_binding_reset;
    /* Have feedback refuse confirmation while the speaker is busy, rather
     * than holding restart for the output to become free. Default false. */
    bool skip_busy_confirmation;
} xiaotai_app_voice_terminal_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; the struct is
 * copied. NULL or never called: all defaults. XIAOTAI_ERR_INVALID_ARG:
 * announce_repeat_s above 3600; XIAOTAI_ERR_INVALID_STATE after the
 * application's init. */
xiaotai_err_t xiaotai_app_voice_terminal_configure(const xiaotai_app_voice_terminal_config_t *config);

/* An automatic announcement that found a feedback sound pending looks again
 * after this long. */
#define XIAOTAI_APP_VOICE_TERMINAL_ANNOUNCE_WAIT_MS 200U

/* ----------------------------------------------------------------- intents */

/* Intents this application handles; none carries arguments. The interaction
 * profile maps the key to them. The console may post terminal.main and
 * terminal.end (AT+INTENT); the two resets erase stored data and are
 * registered as local intents (XIAOTAI_INTENT_LOCAL), so AT+INTENT gets
 * XIAOTAI_ERR_NOT_ALLOWED for them and nothing is erased from the console
 * (the binding module's development command AT+BIND CLEAR aside).
 *
 *   terminal.main            see the key rules above
 *   terminal.end             see the key rules above
 *   terminal.reset_network   local. Erase the stored network credentials,
 *                            keep the binding, restart into provisioning
 *                            (xiaotai_provisioning_begin with FORGET and
 *                            PREEMPT). A service environment switch that
 *                            has not started writing is cancelled first;
 *                            refused while the switch writes or restarts.
 *   terminal.reset_binding   local. Erase the stored identity, keep the
 *                            network credentials, restart
 *                            (xiaotai_binding_forget_local). The server is not
 *                            told. Switch as above.
 * While a call is being ended (CLOSING) "main" and "end" are refused, as is
 * "end" when idle. */
#define XIAOTAI_INTENT_APP_VOICE_TERMINAL_MAIN "terminal.main"
#define XIAOTAI_INTENT_APP_VOICE_TERMINAL_END "terminal.end"
#define XIAOTAI_INTENT_APP_VOICE_TERMINAL_RESET_NETWORK "terminal.reset_network"
#define XIAOTAI_INTENT_APP_VOICE_TERMINAL_RESET_BINDING "terminal.reset_binding"

/* Timing of the application's interaction profile ("terminal-key"): clicks
 * closer than the first belong to one gesture (one click: main, two: end,
 * three: nothing, four: reset_network, resolved at the fourth release); a
 * press held for the second is reset_binding. */
#define XIAOTAI_APP_VOICE_TERMINAL_MULTI_CLICK_MS 350U
#define XIAOTAI_APP_VOICE_TERMINAL_LONG_PRESS_MS 7000U

/* ------------------------------------------------------------------ events */

/* A key request was not done: the gesture was too old, the situation does
 * not take it, or the module refused. args.text: the intent; args.value: the
 * xiaotai_err_t (XIAOTAI_ERR_INVALID_STATE for "too old" and "not now"). The
 * feedback table binds the "refused" effect to it. */
#define XIAOTAI_EVENT_APP_VOICE_TERMINAL_REFUSED "terminal.refused"
/* "main" was accepted while a call is dialling, connecting or connected,
 * where it does nothing. The table binds the "blocked" effect to it. */
#define XIAOTAI_EVENT_APP_VOICE_TERMINAL_BLOCKED "terminal.blocked"

/* ---------------------------------------------------------------- feedback */

/* States the application switches; the feedback table gives each a light
 * pattern and, for the two rings, a looped sound. Listed from the highest
 * priority to the lowest: the first that is on shows. */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_SETUP "terminal.setup"            /* the device waits for its network */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_CLOSING "terminal.closing"        /* a call is being ended */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_ACTIVE "terminal.active"          /* a call is connected */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_LINKING "terminal.linking"        /* accepted or answered, not yet connected */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_INCOMING "terminal.incoming"      /* an announced call rings */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_OUTGOING "terminal.outgoing"      /* the dialled call rings at the other end */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_ERROR "terminal.error"            /* the network part is BLOCKED, the network could not be started, or the portal failed to open */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_BOOT "terminal.boot"              /* the network engine has not started yet */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_BINDING "terminal.binding"        /* no identity: waits to be bound */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_READY "terminal.ready"            /* online */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_CONNECTING "terminal.connecting"  /* bound, not online */
/* Sound-only states: the ring-back continues from OUTGOING through LINKING
 * of a dialled call, and a ring is silenced for the rest of a call that was
 * answered or ended from the console. */
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_RING "terminal.ring"
#define XIAOTAI_APP_VOICE_TERMINAL_STATE_RINGBACK "terminal.ringback"

/* Effects the application fires. */
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_TAP "terminal.tap"               /* "main" was accepted */
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_DOUBLE_TAP "terminal.double_tap" /* "end" was accepted */
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_BLOCKED "terminal.blocked"
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_REFUSED "terminal.refused"
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_CONNECTED "terminal.connected"   /* a call became connected: light only */
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_ENDED "terminal.ended"           /* a call ended normally */
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_FAILED "terminal.failed"         /* a call or a start failed */
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_SETUP "terminal.setup.opened"    /* how to set the network up, once */
#define XIAOTAI_APP_VOICE_TERMINAL_EFFECT_CONFIRM "terminal.confirm"       /* a reset or stored credentials: the restart follows */

/* ------------------------------------------------------------- diagnostics */

typedef struct {
    char shown[24];             /* the state whose pattern shows, "" when none */
    bool ring_silenced;         /* rings are off for the rest of this call */
    bool reset_pending;         /* a reset or credential confirmation is in progress */
    uint32_t gesture_session;   /* call session noted at the last first press; 0: none */
    uint32_t accepted;          /* key requests done */
    uint32_t refused;           /* XIAOTAI_EVENT_APP_VOICE_TERMINAL_REFUSED emitted */
    uint32_t stale;             /* of which because the gesture was too old */
    uint32_t blocked;           /* XIAOTAI_EVENT_APP_VOICE_TERMINAL_BLOCKED emitted */
    uint32_t superseded;        /* obsolete key refusals logged without affecting feedback */
} xiaotai_app_voice_terminal_status_t;

/* [owner] */
void xiaotai_app_voice_terminal_status(xiaotai_app_voice_terminal_status_t *status);

/* Diagnostics section "app.voice_terminal". Event-log source "terminal". */

/* ---------------------------------------------------------------- lifecycle */

/* Application lifecycle (called by the framework through the generated
 * tables). init: registers the interaction profile, the feedback table, the
 * intents and the diagnostics provider, and subscribes to the events of the
 * modules it uses. start: switches the state the device is in on.
 * The application has no views, so it has no foreground function. */
xiaotai_err_t xiaotai_app_voice_terminal_init(void);
void xiaotai_app_voice_terminal_start(void);
/* Host tests: forget configuration and state. */
void xiaotai_app_voice_terminal_reset(void);

#ifdef __cplusplus
}
#endif
