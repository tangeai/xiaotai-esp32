// SPDX-License-Identifier: MIT
#pragma once
/* Base module "call": voice calls between two devices of one account.
 *
 * The module owns one call at a time, in either direction: the request to the
 * call service, the peer-to-peer link, the "answered" handshake, the audio
 * session, the ways a call ends, and the release of the call's room on the
 * server, which continues in the background after the call is over for the
 * user. It rings no bell and shows nothing: applications read the status,
 * subscribe to the events and call the functions below.
 *
 * Foreground activity "call" (xiaotai/framework.h), resources audio in, audio
 * out, link and screen. It begins when a call is dialled or an incoming call
 * is announced, so a call preempts whatever the product's priority table puts
 * below it; when the activity cannot begin, an outgoing call is refused with
 * XIAOTAI_ERR_BUSY and an incoming one is answered "busy" to the server (an
 * incoming one also while an activity of incoming_busy_during is current). A
 * revocation ends the call like a local hang-up and tells the peer, but
 * closes the link at once: the preempting session may need it. The
 * activity's generation (xiaotai_activity_set_generation), which attachments
 * (video call) ride on, is set to the call's link when the call becomes
 * ACTIVE (after "answered" and the audio session) and back to 0 as the first
 * step of every ending, before the audio session and the link close; it is
 * 0 in every other state. After a local hang-up on an open link the activity
 * is held for the release delay (status.releasing).
 *
 * Generations. `session` names one call: a value of xiaotai_generation_next(),
 * never 0, new for every dialled or announced call. Calls that act on a call
 * name it; a call naming another than the current one does nothing, returns
 * XIAOTAI_ERR_INVALID_STATE and is counted (stale_calls). 0 means "the
 * current call", for sources that hold no snapshot. `link` is the generation
 * of the call's link (xiaotai/link.h) and of its media session; 0 until the
 * link exists.
 *
 * Who may be called: contacts of the kind this module registers with the
 * contacts module in its init (xiaotai_contacts_kind_register, name
 * XIAOTAI_CALL_CONTACT_KIND, alias "DEV", server type "device", no extra
 * fields, with presence and friendships), in that module's list of the
 * present binding (xiaotai/modules/contacts.h). Lookups pass
 * XIAOTAI_CONTACTS_STALE_OK unless require_fresh_contacts. The module never
 * dials an identifier that is not in that list.
 *
 * Protocol facts kept here and nowhere else: service "call" and its
 * /v1/call/... requests, the signals call_incoming, callee_answered,
 * room_cancel, call_cancel and call_reject, a peer-to-peer link (the callee
 * connects, the caller listens) with G.711 A-law audio (8 kHz, mono, 20 ms),
 * sent and requested on stream 10. Receive may accept a different audio
 * stream number from the same peer when configured. H.264 video uses stream
 * 11 in both directions. Command 0x2000 "answered" and 0x2001 "hang up"
 * are compared exactly. After sending "hang up" on an open link the
 * module waits XIAOTAI_CALL_RELEASE_DELAY_MS before it disconnects: the
 * transport keeps the order of the two but not the delivery of a command
 * queued just before a disconnect (xiaotai/link.h).
 *
 * Tags as in xiaotai/framework.h: [init], [owner], [any]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/media.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sizes with the terminator. */
#define XIAOTAI_CALL_PEER_ID_MAX 65U
#define XIAOTAI_CALL_PEER_NAME_MAX 97U

/* Time between a local "hang up" on an open link and the disconnect. */
#define XIAOTAI_CALL_RELEASE_DELAY_MS 150U

/* Name of the contact kind this module registers (views, "contacts.show",
 * AT+CONTACTS). */
#define XIAOTAI_CALL_CONTACT_KIND "device"

/* Most names in xiaotai_call_config_t.incoming_busy_during. */
#define XIAOTAI_CALL_BUSY_DURING_MAX 8U

/* ----------------------------------------------------------- configuration */

/* Product configuration. 0 selects the stated default. */
typedef struct {
    /* An announced call that is neither accepted nor rejected ends as missed
     * after this time. Default 45000. */
    uint32_t ring_timeout_ms;
    /* An outgoing call that is not answered ends as "no answer" after this
     * time, counted from the request and again from the server's
     * confirmation. Default 30000. */
    uint32_t answer_timeout_ms;
    /* An accepted call must be connected within this time. Default 30000. */
    uint32_t connect_timeout_ms;
    /* Outgoing calls whose server room may still need releasing that are
     * tracked at once; a dial beyond it is refused. Default 4, at most 8. */
    uint8_t pending_releases;
    /* true: dial only while the contact list is fresh (xiaotai/modules/
     * contacts.h), so never during a refresh or after a failed one. Default
     * false: any list of the present binding will do; the server checks the
     * target again. */
    bool require_fresh_contacts;
    /* true: any end of the platform session (platform.offline, whatever the
     * reason; a signaling hold emits none) ends the call with END_NETWORK, so
     * no TLS reconnection runs beside the link. Default false: a binding
     * change ends it (END_SYSTEM), a call not yet ACTIVE ends with
     * END_NETWORK, an ACTIVE call goes on while its link lives. */
    bool end_on_platform_offline;
    /* Suspend platform signaling only after both directions of an ordinary
     * call carry audio. Resume after RTC retirement. Default false. */
    bool pause_platform_signaling_while_active;
    /* true: the call's audio session gates the idle uplink
     * (XIAOTAI_MEDIA_GATE_IDLE_NOISE), so a peer that switches its own
     * direction by the far end's voice (an echo gate) can be heard. Default
     * false: the uplink is sent as captured. */
    bool uplink_noise_gate;
    /* Pacing profile of the call's audio session and of the media
     * requirement. All zero: XIAOTAI_MEDIA_LATENCY_RELAYED. A product keeps
     * the playout curve its family uses today here. */
    xiaotai_media_latency_t latency;
    /* Activities (priority table names) during which an announced call is
     * answered "busy" although the priority table lets "call" preempt them;
     * a dialled call still preempts them. NULL-terminated, at most
     * XIAOTAI_CALL_BUSY_DURING_MAX names, static storage. NULL: none, the
     * priority table alone decides. */
    const char *const *incoming_busy_during;
    /* Reject AT+CALL HANGUP while an incoming call is still ringing.
     * AT+CALL REJECT and the owner API are unchanged. Default false. */
    bool at_reject_incoming_hangup;
    /* Keep the neutral camera geometry fields in an audio-only device
     * profile. Default false. Real video contributions supply their own. */
    bool voice_profile_geometry;
    /* Require a canonical device room ID for automatic cleanup after a
     * failed incoming accept. Default false; ordinary call actions are unchanged. */
    bool strict_failed_accept_room_id;
    /* Accept the peer's supported audio on any valid stream of this call.
     * Sending and subscription stay on stream 10. Default false. */
    bool accept_peer_audio_stream;
} xiaotai_call_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; copied (the
 * names of incoming_busy_during are kept by pointer). NULL or never called:
 * all defaults. XIAOTAI_ERR_INVALID_ARG for pending_releases above 8, an
 * empty name or more than XIAOTAI_CALL_BUSY_DURING_MAX names in
 * incoming_busy_during; XIAOTAI_ERR_INVALID_STATE after the module's init. */
xiaotai_err_t xiaotai_call_configure(const xiaotai_call_config_t *config);

/* ------------------------------------------------------------------- state */

typedef enum {
    XIAOTAI_CALL_IDLE = 0,
    XIAOTAI_CALL_OUTGOING,     /* dialled: the server and the peer are being asked */
    XIAOTAI_CALL_INCOMING,     /* announced: waiting for accept or reject */
    XIAOTAI_CALL_CONNECTING,   /* accepted here: the link is being made */
    XIAOTAI_CALL_ACTIVE,       /* both ends talk */
} xiaotai_call_state_t;

/* How the last call ended: the facts an application turns into words.
 * Appended, never renumbered. */
typedef enum {
    XIAOTAI_CALL_END_NONE = 0,     /* no call ended yet, or one is running */
    XIAOTAI_CALL_END_LOCAL,        /* hung up, cancelled or rejected here */
    XIAOTAI_CALL_END_PEER_HANGUP,  /* the peer hung up */
    XIAOTAI_CALL_END_PEER_REJECT,  /* the peer rejected the call */
    XIAOTAI_CALL_END_PEER_CANCEL,  /* the caller gave up before it was accepted here */
    XIAOTAI_CALL_END_NO_ANSWER,    /* outgoing: nobody answered in time */
    XIAOTAI_CALL_END_MISSED,       /* incoming: neither accepted nor rejected in time */
    XIAOTAI_CALL_END_BUSY,         /* the server holds another call of this device, or of the peer */
    XIAOTAI_CALL_END_FAILED,       /* the server refused or did not answer the request */
    XIAOTAI_CALL_END_NETWORK,      /* the link could not be made or broke */
    XIAOTAI_CALL_END_NO_MEMORY,
    XIAOTAI_CALL_END_AUDIO,        /* the audio session could not start or failed */
    XIAOTAI_CALL_END_PREEMPTED,    /* a higher-priority activity began */
    XIAOTAI_CALL_END_SYSTEM,       /* the system was quiesced, or the binding ended */
} xiaotai_call_end_t;

enum {
    /* The call carries video in both directions. Needs a video contribution
     * (see "video" below). */
    XIAOTAI_CALL_VIDEO = 1u << 0,
};

/* A snapshot. Counters count since boot. */
typedef struct {
    xiaotai_call_state_t state;
    uint32_t session;                    /* current call; 0 when idle */
    uint32_t link;                       /* its link and media generation; 0 while there is none */
    bool outgoing;                       /* of the current or the last call */
    uint32_t flags;                      /* XIAOTAI_CALL_* of the current or the last call */
    bool muted;                          /* the microphone is muted for this call (xiaotai_call_set_muted, or the setting) */
    /* The last call is IDLE for the application but its link is still being
     * closed (XIAOTAI_CALL_RELEASE_DELAY_MS after "hang up"); the activity
     * "call" is still held, so a new call is refused as busy until it clears. */
    bool releasing;
    /* The other device: its id, and the name the contact list or the caller
     * gave ("" when neither did; the id is then what an application shows).
     * Kept after the call for the result screen. */
    char peer_id[XIAOTAI_CALL_PEER_ID_MAX];
    char peer_name[XIAOTAI_CALL_PEER_NAME_MAX];
    int64_t active_since_us;             /* xiaotai_time_us() when the call became ACTIVE; 0 otherwise */
    xiaotai_call_end_t end;              /* how the last call ended */
    xiaotai_err_t error;                 /* detail of `end` for diagnostics */
    uint32_t dialled, announced, answered, refused_busy, refused_media;
    uint32_t stale_calls;                /* calls, signals and commands that named another call */
    uint8_t releases_pending;            /* server rooms of finished calls still being released */
    uint32_t releases_unconfirmed;       /* releases given up after their attempts; tried again at the next sign-on */
} xiaotai_call_status_t;

/* [owner] */
void xiaotai_call_status(xiaotai_call_status_t *status);

/* ------------------------------------------------------------------ control */

/* [owner] Calls the contact with this device id. Returns without waiting for
 * the network; on XIAOTAI_OK *session (may be NULL) names the call, the state
 * is OUTGOING and events follow until IDLE.
 *   XIAOTAI_ERR_INVALID_ARG       NULL, empty or too long id, unknown flags
 *   XIAOTAI_ERR_NOT_FOUND         not a device contact in the list of the
 *                                 present binding (none loaded yet; with
 *                                 require_fresh_contacts also while the list
 *                                 is not fresh)
 *   XIAOTAI_ERR_NOT_SUPPORTED     XIAOTAI_CALL_VIDEO without a video
 *                                 contribution
 *   XIAOTAI_ERR_PLATFORM_OFFLINE  the platform session or the transport is
 *                                 not online (xiaotai/platform.h)
 *   XIAOTAI_ERR_BUSY              a call exists, an activity of equal or
 *                                 higher priority is current, or the system
 *                                 is quiesced
 *   XIAOTAI_ERR_NO_MEM            no request slot, or pending_releases calls
 *                                 are still being released */
xiaotai_err_t xiaotai_call_dial(const char *device_id, uint32_t flags, uint32_t *session);

/* [owner] As xiaotai_call_dial for the contact at `index` of the contact
 * list's selected page (xiaotai_contacts_at). XIAOTAI_ERR_NOT_FOUND also
 * beyond the page and when that contact is not a device (it belongs to
 * another call module). */
xiaotai_err_t xiaotai_call_dial_contact(size_t index, uint32_t flags, uint32_t *session);

/* [owner] Accepts the announced call: state CONNECTING, then ACTIVE or IDLE.
 * XIAOTAI_ERR_INVALID_STATE unless INCOMING (and for a stale session). */
xiaotai_err_t xiaotai_call_accept(uint32_t session);

/* [owner] Rejects the announced call: the caller is told "declined".
 * XIAOTAI_ERR_INVALID_STATE unless INCOMING. */
xiaotai_err_t xiaotai_call_reject(uint32_t session);

/* [owner] Ends the call in any state but IDLE: cancels an outgoing call,
 * rejects an announced one, hangs up otherwise. The peer and the server are
 * told; the state is IDLE when the call returns (closing an open link
 * continues in the background, status.releasing). A failed peer command
 * does not undo the local hangup; status.error keeps the send error. */
xiaotai_err_t xiaotai_call_hangup(uint32_t session);

/* [owner] Mutes or unmutes the microphone for the named call (0: the current
 * one). The call's mute lies on top of the user's lasting setting
 * "audio.mic_muted" (xiaotai/modules/settings.h) and never writes it: a call
 * starts with the microphone as the setting has it; a change of the setting
 * during the call is applied by the settings module and becomes the call's
 * state; unmuting here and the end of the call apply the setting's present
 * value (unmuted when the product has no such setting), never the value seen
 * when the call began. XIAOTAI_ERR_INVALID_STATE when IDLE and for a stale
 * session, so a tap on the page of an earlier call never mutes the next one. */
xiaotai_err_t xiaotai_call_set_muted(uint32_t session, bool muted);

/* [owner] An explicit user action to change the lasting microphone preference
 * from an active call. Requires the exact, nonzero session. Settings own
 * media admission and persistence; on success the call snapshot reflects
 * the accepted request. NOT_SUPPORTED without the settings module.
 * Temporary call mute and its intents do not use this operation. */
xiaotai_err_t xiaotai_call_set_saved_mute(uint32_t session, bool muted);

/* ------------------------------------------------------------------- video */

/* Scene of this module's sessions: the device-profile scene it writes and
 * the scene under which session extensions contribute (xiaotai/framework.h,
 * xiaotai_contribution_t). The module has no API for extensions. An
 * attachment of the activity "call" that contributes for this scene (or for
 * every scene):
 *   - the video part of a link (XIAOTAI_LINK_SLOT_VIDEO, xiaotai/link.h):
 *     from then on the module accepts announced video calls, dials them
 *     (XIAOTAI_CALL_VIDEO), registers the inbound listener with video and
 *     copies that part into the profile of every video call's link. Only the
 *     first such contribution is used; further ones are counted.
 *   - scene parts (XIAOTAI_PLATFORM_SLOT_SCENE, xiaotai/platform.h): their
 *     members are written into the scene after the module's own audio
 *     members, and the scene then says "no_video":false. A part that does
 *     not fit makes the module report the scene without video (counted).
 * Contributions are read from start() on. Without a link video part an
 * announced video call is refused as unsupported media. The extension finds
 * the call it serves by the activity's generation (the link of the ACTIVE
 * call) and xiaotai_call_status(): link, state and XIAOTAI_CALL_VIDEO. */
#define XIAOTAI_CALL_SCENE "call"

/* -------------------------------------------------- events and intents */

/* Events on the framework bus; data is a xiaotai_call_status_t.
 *
 * "call.state": anything in the status an application shows changed (state,
 * mute, link, releasing). args.value: the xiaotai_call_state_t. Emitted for every call
 * in order; the three below are emitted in addition, before it. */
#define XIAOTAI_EVENT_CALL_STATE "call.state"
/* "call.incoming": a call was announced. args.text: the peer's id. */
#define XIAOTAI_EVENT_CALL_INCOMING "call.incoming"
/* "call.connected": the call became ACTIVE. */
#define XIAOTAI_EVENT_CALL_CONNECTED "call.connected"
/* "call.ended": the call is over. args.value: the xiaotai_call_end_t. */
#define XIAOTAI_EVENT_CALL_ENDED "call.ended"

/* Intents the module handles.
 *   "call.dial"        args.text: the device id; args.value: XIAOTAI_CALL_* flags
 *   "call.accept"      args.value: the session (0: the current call)
 *   "call.reject"      as accept
 *   "call.hangup"      as accept
 *   "call.mic_off"     as accept; xiaotai_call_set_muted(session, true)
 *   "call.mic_on"      as accept; xiaotai_call_set_muted(session, false)
 *   "call.mic_toggle"  as accept; the opposite of status.muted */
#define XIAOTAI_INTENT_CALL_DIAL "call.dial"
#define XIAOTAI_INTENT_CALL_ACCEPT "call.accept"
#define XIAOTAI_INTENT_CALL_REJECT "call.reject"
#define XIAOTAI_INTENT_CALL_HANGUP "call.hangup"
#define XIAOTAI_INTENT_CALL_MIC_OFF "call.mic_off"
#define XIAOTAI_INTENT_CALL_MIC_ON "call.mic_on"
#define XIAOTAI_INTENT_CALL_MIC_TOGGLE "call.mic_toggle"

/* Name of the foreground activity (priority table key), shared with every
 * other module that makes calls: two calls never preempt each other. */
#define XIAOTAI_CALL_ACTIVITY "call"

/* Commands, as verbs of AT+CALL (xiaotai_at_verbs_register): the status line
 * +CALL: kind=device ..., DEVICE <device-id>, INDEX <n>, ACCEPT, REJECT,
 * HANGUP, MUTE 0|1. Verbs shared with other call modules pass
 * (XIAOTAI_AT_PASS) when the call is not this module's. Device profile scene
 * XIAOTAI_CALL_SCENE. Diagnostics section "call". Event-log source "call";
 * these labels are frozen (test tools wait for them), each with the call's
 * session as generation: "outgoing" and "incoming" (value: the
 * XIAOTAI_CALL_* flags), "connected" (value: the link), "ended" (value: the
 * xiaotai_call_end_t). Other labels may change. */

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: registers its contact kind, declares its needs to the platform, the
 * transport (one link and its command share) and the media engine (with the
 * configured latency), registers the signal subscriptions, the profile scene,
 * intents, commands and diagnostics, and subscribes to the settings module's
 * changes when the product has it. start: registers the inbound listeners,
 * the one with video only when a link video part was contributed
 * (attachments register in other modules' init, so the profile of every
 * listener is known only now). */
xiaotai_err_t xiaotai_module_call_init(void);
void xiaotai_module_call_start(void);
/* Host tests: end the call and forget configuration and state. */
void xiaotai_call_reset(void);

#ifdef __cplusplus
}
#endif
