// SPDX-License-Identifier: MIT
#pragma once
/* Base module "wechat": voice calls between the device and a WeChat user.
 *
 * The module owns one call at a time, in either direction: the request to the
 * voice service, the pairing of that request with the room the server pushes
 * back, the link to the media server, the "answered" handshake, the audio
 * session and the ways a call ends, including the refusals the service
 * expects for rooms the device does not take. It rings no bell and shows
 * nothing: applications read the status, subscribe to the events and call the
 * functions below.
 *
 * Foreground activity "call" (xiaotai/framework.h), the same name every call
 * module uses: resources audio in, audio out, link and screen. It begins when
 * a call is dialled or announced; when it cannot begin, an outgoing call is
 * refused with XIAOTAI_ERR_BUSY and an announced one is refused to the
 * service (an announced one also while an activity of incoming_busy_during
 * is current). A revocation ends the call like a local hang-up, but closes
 * the link at once: the preempting session may need it. The activity's
 * generation (xiaotai_activity_set_generation), which attachments (video
 * call) ride on, is set to the call's link when the call becomes ACTIVE
 * (after "answered" and the audio session; never while the link is open and
 * the other side has not answered) and back to 0 as the first step of every
 * ending; it is 0 in every other state.
 *
 * Generations. `session` names one call: a value of xiaotai_generation_next(),
 * never 0. Calls that act on a call name it; a call naming another than the
 * current one does nothing, returns XIAOTAI_ERR_INVALID_STATE and is counted
 * (stale_calls). 0 means "the current call". `link` is the generation of the
 * call's link (xiaotai/link.h) and of its media session; 0 until it exists.
 *
 * Who may be called: contacts of the kind this module registers with the
 * contacts module in its init (xiaotai_contacts_kind_register, name
 * XIAOTAI_WECHAT_CONTACT_KIND, alias "WX", server type "voip", extra fields
 * "wx_app_id" and "wx_model_id", required only as require_app_id says, no
 * presence, no friendships), in that module's list of the present binding
 * (xiaotai/modules/contacts.h); the extra fields are the mini program and
 * model ids a call needs, sent as listed (empty when the entry had none).
 * Lookups pass XIAOTAI_CONTACTS_STALE_OK unless require_fresh_contacts.
 * A WeChat user's identifier is never logged, printed or shown by this
 * module; the status carries the remark the account owner gave, or "".
 *
 * Audio format: what the board can send and receive, chosen once at start
 * from the formats the module declared (G.711 A-law 8 kHz; Opus 16 kHz for a
 * board whose capture is encoded) and reported to the service in the device
 * profile scene "voip".
 *
 * Protocol facts kept here and nowhere else: service "voip" and
 * POST /v1/voip/device/call, the signals call_incoming and call_cancel on
 * channel "wx", a WHIP link, command 0x2000 "answered" and 0x2001 "hang up"
 * with {"reason":n}, compared exactly, command 0x1105 "send your camera", and
 * the RTC service request /v1/wxvoip/reject with its reason numbers. After a
 * local "hang up" on an open link the module waits
 * XIAOTAI_WECHAT_RELEASE_DELAY_MS before it disconnects: the transport keeps
 * the order of the two but not the delivery of a command queued just before
 * a disconnect (xiaotai/link.h).
 *
 * Downlink audio declared to the media engine: packets of 20 ms, no burst
 * beyond the pacing profile (a live peer paces itself), the configured
 * pacing profile (default XIAOTAI_MEDIA_LATENCY_VOIP); for Opus a packet up
 * to XIAOTAI_WECHAT_OPUS_PACKET_MAX bytes and an encoder rate up to
 * XIAOTAI_WECHAT_OPUS_BITRATE_MAX. A larger packet is counted by the media
 * engine (rx_invalid) and dropped; a lasting rate above the bound shows as
 * rx_overflow.
 *
 * Tags as in xiaotai/framework.h: [init], [owner], [any]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/link.h"
#include "xiaotai/media.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* With the terminator. */
#define XIAOTAI_WECHAT_PEER_NAME_MAX 97U

/* Time between a local "hang up" on an open link and the disconnect. */
#define XIAOTAI_WECHAT_RELEASE_DELAY_MS 150U

/* Name of the contact kind this module registers (views, "contacts.show",
 * AT+CONTACTS). */
#define XIAOTAI_WECHAT_CONTACT_KIND "wechat"

/* Largest downlink Opus packet accepted, in bytes: what the devices that
 * receive Opus from this service accept today. Lowered once the bench has
 * measured the largest packet the service sends. */
#define XIAOTAI_WECHAT_OPUS_PACKET_MAX 512U
/* Bound on the other side's Opus encoder rate, in bit/s, declared with the
 * packet bound (xiaotai_media_requirement_t.max_bitrate_bps) so the downlink
 * is sized by the audio it holds rather than by packets of the largest size.
 * The service documents no rate; 16 kHz speech Opus runs at 16-48 kbit/s.
 * Lowered once the bench has measured the rate. */
#define XIAOTAI_WECHAT_OPUS_BITRATE_MAX 64000U

/* Most names in xiaotai_wechat_config_t.incoming_busy_during. */
#define XIAOTAI_WECHAT_BUSY_DURING_MAX 8U

/* ----------------------------------------------------------- configuration */

/* Which build of the mini program the service rings. */
typedef enum {
    XIAOTAI_WECHAT_RELEASE = 0,
    XIAOTAI_WECHAT_DEVELOPMENT,
    XIAOTAI_WECHAT_TRIAL,
} xiaotai_wechat_version_t;

/* Reason numbers this side gives the service: the {"reason":n} of a "hang up"
 * (0x2001) on an open link, and the hangup_reason of a refusal
 * (/v1/wxvoip/reject) without one. Deployed devices of different families
 * send different numbers for the same situation, so they are product
 * configuration; each value is 0..20 or one of the two words below. */
/* Send nothing: no "hang up" (the link is just closed), or no refusal. */
#define XIAOTAI_WECHAT_REASON_NONE 0xFFU
/* "hang up" without a payload. Only for the three hang-up fields. */
#define XIAOTAI_WECHAT_REASON_BARE 0xFEU

typedef struct {
    /* --- "hang up" on an open link --- */
    uint8_t hangup;          /* the user or the application ends the call; also a revocation */
    uint8_t hangup_timeout;  /* a deadline ends it (no answer after the link opened) */
    uint8_t hangup_failure;  /* a failure on this side ends it (audio session, memory) */
    /* --- refusals of a room this side holds, before its link opened --- */
    uint8_t declined;        /* an announced call ends before it was accepted */
    uint8_t missed;          /* an announced call rang out */
    uint8_t abandoned;       /* an outgoing call whose room arrived ends before the link opened */
    uint8_t failed;          /* the link could not be requested or opened (a room is held) */
    /* --- refusals of a room pushed by the service --- */
    uint8_t busy;            /* the device is busy (another call, an activity that is not preempted) */
    uint8_t busy_dialing;    /* the same while this side's own WeChat call waits for its room */
    uint8_t stale;           /* a room of this side's own that matches no current call */
    uint8_t retired_call;    /* a room for a call id this side already ended */
    uint8_t retired_room;    /* a room id this side already ended */
    uint8_t not_ready;       /* the service has not accepted the device profile (NONE: ignored and reported again) */
    uint8_t unsupported;     /* a room type no module here takes (see ignore_room_type) */
    uint8_t overflow;        /* an early room that cannot be held until the request's answer */
} xiaotai_wechat_reasons_t;

/* Product configuration. 0 selects the stated default. */
typedef struct {
    xiaotai_wechat_version_t version;
    /* An announced call that is neither accepted nor rejected ends as missed
     * after this time. Default 45000. */
    uint32_t ring_timeout_ms;
    /* Budget of the outgoing request, until the service answered it.
     * Default 28000. */
    uint32_t request_timeout_ms;
    /* After the request was accepted: until the room arrived, and again
     * until the link is open. Default 30000. */
    uint32_t connect_timeout_ms;
    /* After the link opened: until the other side answered. Default 35000. */
    uint32_t answer_timeout_ms;
    /* Audio stream this side sends on (xiaotai_link_media_t.send_stream):
     * 10, or uplink_stream as given (0..15) when custom_uplink_stream is
     * true, so stream 0 can be chosen. */
    bool custom_uplink_stream;
    uint8_t uplink_stream;
    /* Filter received audio locally without requesting a peer subscription.
     * With the switch off, accept any stream in the selected audio format. */
    bool custom_downlink_stream;
    uint32_t downlink_stream; /* 0..15, including an explicit stream 0. */
    /* What the other side's subscription to another audio stream does
     * (xiaotai_link_media_t.peer_streams). Default (0)
     * XIAOTAI_LINK_PEER_STREAM_REFUSE: keep sending on the stream above. */
    xiaotai_link_peer_stream_t uplink_policy;
    /* Reported to the service as the time it lets a call ring. Default 30. */
    uint16_t calling_timeout_s;
    /* true: dial only while the contact list is fresh (xiaotai/modules/
     * contacts.h), so never during a refresh or after a failed one. Default
     * false: any list of the present binding will do; the service checks the
     * target again. */
    bool require_fresh_contacts;
    /* With require_fresh_contacts false: true also refuses a dial (NOT_FOUND)
     * from the moment the list is known to have changed on the server until
     * the next successful refresh; a refresh in flight or a failed one alone
     * does not. Known to have changed: the signal "callers_update" on channel
     * "wx", to which the module then subscribes itself in its init (beside
     * the contacts module's own subscription; the list of WeChat users is
     * the one that signal announces), or business code 40205 on a dial of
     * this module. Cleared by the first XIAOTAI_EVENT_CONTACTS_CHANGED whose
     * status shows fresh for the present binding generation, and by a
     * binding change. Default false. */
    bool refuse_changed_contacts;
    /* Defer refreshing after authorization denial until the matching result is dismissed. */
    bool refresh_denied_after_dismiss;
    /* true: a contact of this module's kind without "wx_app_id" is malformed
     * (extra_required bit 0) and is skipped by the contacts module. Default
     * false: such an entry is listed and dialled with an empty mini program
     * id, as the product families that list contacts on a screen do today. */
    bool require_app_id;
    /* true: any end of the platform session (platform.offline, whatever the
     * reason; a signaling hold emits none) ends the call with END_NETWORK, so
     * no TLS reconnection runs beside the link. Default false: a binding
     * change ends it (END_SYSTEM), an outgoing call whose request is still
     * unanswered ends with END_NETWORK, any other call goes on while its link
     * lives. */
    bool end_on_platform_offline;
    /* true: a complete dial reply HTTP 200 with business code 6006 is
     * authoritative. Default false: ask the platform to verify by logging in. */
    bool trust_http_unbound;
    /* true: the call's audio session gates the idle uplink
     * (XIAOTAI_MEDIA_GATE_IDLE_NOISE). Default false: sent as captured. */
    bool uplink_noise_gate;
    /* Pacing profile of the call's audio session and of the media
     * requirements. All zero: XIAOTAI_MEDIA_LATENCY_VOIP. A product keeps the
     * playout curve its family uses today here. */
    xiaotai_media_latency_t latency;
    /* Activities (priority table names) during which an announced call is
     * refused as busy although the priority table lets "call" preempt them;
     * a dialled call still preempts them. NULL-terminated, at most
     * XIAOTAI_WECHAT_BUSY_DURING_MAX names, static storage. NULL: none. */
    const char *const *incoming_busy_during;
    /* true: a room's type is not checked; every announced room is taken as a
     * voice call (or as a video call where video is contributed). Default
     * false: a type that is neither voice nor taken by a video contribution
     * is refused (reasons.unsupported). */
    bool ignore_room_type;
    /* true: reasons as given. Default false: the numbers
     * XIAOTAI_WECHAT_REASONS_DEFAULT lists. */
    bool custom_reasons;
    xiaotai_wechat_reasons_t reasons;
    /* true: a refusal needs a nonempty wx_server_token. false: an absent or
     * empty token is sent as "wx_session_token":"" for older services.
     * An invalid or oversize token never becomes an empty one. */
    bool require_reject_server_token;
    /* Maximum encoded refusal body size including the NUL. Zero selects
     * XIAOTAI_TRANSPORT_SERVICE_BODY_MAX. Set a tighter limit only for a
     * service with an existing bound. */
    uint16_t reject_body_bytes;
    /* Reject AT+CALL HANGUP while an incoming call is still ringing.
     * AT+CALL REJECT and the owner API are unchanged. Default false. */
    bool at_reject_incoming_hangup;
    /* Keep the neutral camera and receive geometry fields in an audio-only
     * device profile. Default false. Real video contributions supply theirs. */
    bool voice_profile_geometry;
    /* Default false: retire finished incoming rooms. true allows their
     * reannouncement but keeps outgoing and pending-dial protection. */
    bool allow_reopen_incoming_room;
    /* Use busy_dialing instead of stale when a room arrives without a
     * source or call ID while waiting for an outgoing room. Default false. */
    bool anonymous_waiting_is_busy;
    /* For RTC 0x2000, accept an empty payload or a complete JSON object.
     * Default false: the answer command's payload is opaque. */
    bool answer_payload_requires_object;
} xiaotai_wechat_config_t;

/* The reason numbers of a product that gives none, in field order. */
#define XIAOTAI_WECHAT_REASONS_DEFAULT                                                                 \
    {0U, 0U, XIAOTAI_WECHAT_REASON_NONE, 7U, 7U, XIAOTAI_WECHAT_REASON_NONE,                           \
     XIAOTAI_WECHAT_REASON_NONE, 8U, 5U, 7U, XIAOTAI_WECHAT_REASON_NONE, XIAOTAI_WECHAT_REASON_NONE,    \
     XIAOTAI_WECHAT_REASON_NONE, 7U, XIAOTAI_WECHAT_REASON_NONE}

/* [glue] Before xiaotai_framework_start(), by the product glue; copied (the
 * names of incoming_busy_during are kept by pointer). NULL or never called:
 * all defaults. XIAOTAI_ERR_INVALID_ARG for uplink_stream above 15, an
 * unknown uplink_policy, a reason above 20 that is not NONE (or BARE in a
 * hang-up field), an empty name or more than XIAOTAI_WECHAT_BUSY_DURING_MAX
 * names in incoming_busy_during, or a reject_body_bytes over the transport
 * bound; XIAOTAI_ERR_INVALID_STATE after the
 * module's init. */
xiaotai_err_t xiaotai_wechat_configure(const xiaotai_wechat_config_t *config);

/* ------------------------------------------------------------------- state */

typedef enum {
    XIAOTAI_WECHAT_IDLE = 0,
    XIAOTAI_WECHAT_OUTGOING,     /* dialled: the service rings the user */
    XIAOTAI_WECHAT_INCOMING,     /* announced: waiting for accept or reject */
    /* The link is being made, or it is open and the other side has not
     * answered yet. Nothing is heard or sent. */
    XIAOTAI_WECHAT_CONNECTING,
    XIAOTAI_WECHAT_ACTIVE,       /* both ends talk */
} xiaotai_wechat_state_t;

/* How the last call ended. Appended, never renumbered. */
typedef enum {
    XIAOTAI_WECHAT_END_NONE = 0,     /* no call ended yet, or one is running */
    XIAOTAI_WECHAT_END_LOCAL,        /* hung up, cancelled or rejected here */
    XIAOTAI_WECHAT_END_PEER_HANGUP,  /* the other side hung up; see peer_reason */
    XIAOTAI_WECHAT_END_PEER_REJECT,
    XIAOTAI_WECHAT_END_PEER_CANCEL,  /* the other side gave up before the call was connected */
    XIAOTAI_WECHAT_END_NO_ANSWER,    /* outgoing: not answered in time */
    XIAOTAI_WECHAT_END_MISSED,       /* incoming: neither accepted nor rejected in time */
    XIAOTAI_WECHAT_END_CONTACT,      /* the service no longer accepts this contact; the contact list was told */
    XIAOTAI_WECHAT_END_FAILED,       /* the service refused or did not answer the request */
    XIAOTAI_WECHAT_END_NETWORK,      /* the link could not be made or broke */
    XIAOTAI_WECHAT_END_NO_MEMORY,
    XIAOTAI_WECHAT_END_AUDIO,        /* the audio session could not start or failed */
    XIAOTAI_WECHAT_END_PREEMPTED,    /* a higher-priority activity began */
    XIAOTAI_WECHAT_END_SYSTEM,       /* the system was quiesced, or the binding ended */
} xiaotai_wechat_end_t;

enum {
    /* The call carries video. Needs a video contribution (see "video"
     * below). */
    XIAOTAI_WECHAT_VIDEO = 1u << 0,
};

/* A snapshot. Counters count since boot. */
typedef struct {
    xiaotai_wechat_state_t state;
    uint32_t session;                    /* current call; 0 when idle */
    uint32_t link;                       /* its link and media generation; 0 while there is none */
    bool outgoing;                       /* of the current or the last call */
    uint32_t flags;                      /* XIAOTAI_WECHAT_* of the current or the last call */
    bool muted;                          /* the microphone is muted for this call (xiaotai_wechat_set_muted, or the setting) */
    bool ready;                          /* the service accepted the device profile: calls can be made and received */
    /* The last call is IDLE for the application but still being released in
     * the background (hang-up sent, link closing); the activity "call" is
     * still held, so a new call is refused as busy until it clears. */
    bool releasing;
    /* The remark of the user in the contact list or in the announcement; ""
     * when there is none. Kept after the call for the result screen. */
    char peer_name[XIAOTAI_WECHAT_PEER_NAME_MAX];
    int64_t active_since_us;             /* xiaotai_time_us() when the call became ACTIVE; 0 otherwise */
    xiaotai_wechat_end_t end;            /* how the last call ended */
    int32_t peer_reason;                 /* the reason number the other side gave with its hang-up; -1: none */
    xiaotai_err_t error;                 /* detail of `end` for diagnostics */
    uint32_t dialled, announced, answered;
    uint32_t refused;                    /* rooms refused to the service: busy, stale, unsupported media */
    uint32_t refusals_failed;            /* refusals the service did not confirm */
    uint32_t early_rooms;                /* rooms that arrived before the request's answer and were replayed */
    uint32_t stale_calls;                /* calls, signals and commands that named another call */
    uint32_t authorization_result;       /* denied call awaiting explicit dismissal; otherwise zero */
} xiaotai_wechat_status_t;

/* [owner] */
void xiaotai_wechat_status(xiaotai_wechat_status_t *status);

/* [owner] Acknowledge explicit dismissal of this authorization result.
 * A stale result or one from a different binding does not refresh contacts. */
void xiaotai_wechat_result_dismissed(uint32_t session);

/* ------------------------------------------------------------------ control */

/* [owner] Calls the WeChat contact with this identifier (the contact's id in
 * the contact list). Returns without waiting for the network; on XIAOTAI_OK
 * *session (may be NULL) names the call, the state is OUTGOING and events
 * follow until IDLE.
 *   XIAOTAI_ERR_INVALID_ARG       NULL, empty or too long id, unknown flags
 *   XIAOTAI_ERR_NOT_FOUND         not a WeChat contact in the list of the
 *                                 present binding (none loaded yet; with
 *                                 require_fresh_contacts also while the list
 *                                 is not fresh)
 *   XIAOTAI_ERR_NOT_SUPPORTED     XIAOTAI_WECHAT_VIDEO without a video
 *                                 contribution
 *   XIAOTAI_ERR_PLATFORM_OFFLINE  the platform session or the transport is
 *                                 not online
 *   XIAOTAI_ERR_INVALID_STATE     the service has not accepted the device
 *                                 profile yet (status.ready is false; a new
 *                                 report was requested)
 *   XIAOTAI_ERR_BUSY              a call exists, an activity of equal or
 *                                 higher priority is current, or the system
 *                                 is quiesced
 *   XIAOTAI_ERR_NO_MEM            no request slot is free */
xiaotai_err_t xiaotai_wechat_dial(const char *contact_id, uint32_t flags, uint32_t *session);

/* [owner] As xiaotai_wechat_dial for the contact at `index` of the contact
 * list's selected page (xiaotai_contacts_at). XIAOTAI_ERR_NOT_FOUND also
 * beyond the page and when that contact is not a WeChat user. */
xiaotai_err_t xiaotai_wechat_dial_contact(size_t index, uint32_t flags, uint32_t *session);

/* [owner] Accepts the announced call: state CONNECTING, then ACTIVE or IDLE.
 * XIAOTAI_ERR_INVALID_STATE unless INCOMING (and for a stale session). */
xiaotai_err_t xiaotai_wechat_accept(uint32_t session);

/* [owner] Rejects the announced call. XIAOTAI_ERR_INVALID_STATE unless INCOMING. */
xiaotai_err_t xiaotai_wechat_reject(uint32_t session);

/* [owner] Ends the call in any state but IDLE. The state is IDLE for the
 * application when the call returns; telling the other side and releasing
 * the link continue in the background (status.releasing). An outgoing call whose request is
 * still unanswered has no room to cancel: the room that arrives later is
 * refused. */
xiaotai_err_t xiaotai_wechat_hangup(uint32_t session);

/* [owner] Mutes or unmutes the microphone for the named call (0: the current
 * one), on top of the user's lasting setting "audio.mic_muted" and with the
 * same rules as xiaotai_call_set_muted: a call starts as the setting has it,
 * a change of the setting during the call becomes the call's state, unmuting
 * and the end of the call apply the setting's present value (unmuted without
 * the setting); the setting is never written. XIAOTAI_ERR_INVALID_STATE when
 * IDLE and for a stale session, so a tap on the page of an earlier call never
 * mutes the next one. */
xiaotai_err_t xiaotai_wechat_set_muted(uint32_t session, bool muted);

/* ------------------------------------------------------------------- video */

/* Scene of this module's sessions: the device-profile scene it writes and
 * the scene under which session extensions contribute (xiaotai/framework.h,
 * xiaotai_contribution_t). The module has no API for extensions. An
 * attachment of the activity "call" that contributes for this scene (or for
 * every scene):
 *   - the video part of a link (XIAOTAI_LINK_SLOT_VIDEO, xiaotai/link.h):
 *     from then on the module accepts announced video calls, dials them
 *     (XIAOTAI_WECHAT_VIDEO), treats an announcement without a media type as
 *     a video call and copies that part into the profile of every video
 *     call's link. Only the first such contribution is used; further ones
 *     are counted.
 *   - scene parts (XIAOTAI_PLATFORM_SLOT_SCENE, xiaotai/platform.h): their
 *     members are written into the scene after the module's own screen and
 *     audio members, and the scene then says "no_video":false. A part that
 *     does not fit makes the module report the scene without video (counted).
 * Contributions are read from start() on. Without a link video part an
 * announced video call is refused (reasons.unsupported) and an announcement
 * without a media type is a voice call. The extension finds the call it
 * serves by the activity's generation (the link of the ACTIVE call) and
 * xiaotai_wechat_status(): link, state and XIAOTAI_WECHAT_VIDEO. */
#define XIAOTAI_WECHAT_SCENE "voip"

/* [owner] Asks the other side to send its camera (command 0x1105): called by
 * the extension that contributed video once it receives video on the call's
 * link. XIAOTAI_ERR_INVALID_STATE unless `link` is the link of the ACTIVE
 * call with XIAOTAI_WECHAT_VIDEO; otherwise the result of
 * xiaotai_link_command. */
xiaotai_err_t xiaotai_wechat_request_video(uint32_t link);

/* -------------------------------------------------- events and intents */

/* Events on the framework bus; data is a xiaotai_wechat_status_t.
 *
 * "wechat.state": anything in the status an application shows changed (state,
 * mute, link, ready, releasing). args.value: the xiaotai_wechat_state_t. The three below
 * are emitted in addition, before it. */
#define XIAOTAI_EVENT_WECHAT_STATE "wechat.state"
/* "wechat.incoming": a call was announced. */
#define XIAOTAI_EVENT_WECHAT_INCOMING "wechat.incoming"
/* "wechat.connected": the call became ACTIVE. */
#define XIAOTAI_EVENT_WECHAT_CONNECTED "wechat.connected"
/* "wechat.ended": the call is over. args.value: the xiaotai_wechat_end_t.
 *
 * Failures, for applications that show "the call failed" apart from "the
 * call is over" (the voice terminal's warning tone): NO_ANSWER, MISSED,
 * FAILED, NETWORK, NO_MEMORY, AUDIO, CONTACT, and PEER_HANGUP with
 * peer_reason 6 (the other side's time ran out) or 8 (the other side
 * failed). Not failures: LOCAL, PEER_REJECT, PEER_CANCEL, PREEMPTED, SYSTEM
 * and every other PEER_HANGUP. */
#define XIAOTAI_EVENT_WECHAT_ENDED "wechat.ended"

/* Intents the module handles.
 *   "wechat.dial"        args.text: the contact id; args.value: XIAOTAI_WECHAT_* flags
 *   "wechat.accept"      args.value: the session (0: the current call)
 *   "wechat.reject"      as accept
 *   "wechat.hangup"      as accept
 *   "wechat.mic_off"     as accept; xiaotai_wechat_set_muted(session, true)
 *   "wechat.mic_on"      as accept; xiaotai_wechat_set_muted(session, false)
 *   "wechat.mic_toggle"  as accept; the opposite of status.muted */
#define XIAOTAI_INTENT_WECHAT_DIAL "wechat.dial"
#define XIAOTAI_INTENT_WECHAT_ACCEPT "wechat.accept"
#define XIAOTAI_INTENT_WECHAT_REJECT "wechat.reject"
#define XIAOTAI_INTENT_WECHAT_HANGUP "wechat.hangup"
#define XIAOTAI_INTENT_WECHAT_MIC_OFF "wechat.mic_off"
#define XIAOTAI_INTENT_WECHAT_MIC_ON "wechat.mic_on"
#define XIAOTAI_INTENT_WECHAT_MIC_TOGGLE "wechat.mic_toggle"

/* Name of the foreground activity (priority table key), shared with every
 * other module that makes calls. */
#define XIAOTAI_WECHAT_ACTIVITY "call"

/* Commands, as verbs of AT+CALL (xiaotai_at_verbs_register): the status line
 * +CALL: kind=wechat ..., WX <contact-id>, INDEX <n>, ACCEPT, REJECT, HANGUP,
 * MUTE 0|1. Shared verbs pass (XIAOTAI_AT_PASS) when the call is not this
 * module's. Device profile scene XIAOTAI_WECHAT_SCENE. Diagnostics section "wechat".
 * Event-log source "wechat"; these labels are frozen (test tools wait for
 * them), each with the call's session as generation: "outgoing" and
 * "incoming" (value: the XIAOTAI_WECHAT_* flags), "connected" (value: the
 * link), "ended" (value: the xiaotai_wechat_end_t). Other labels may change. */

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: registers its contact kind (extra_required from require_app_id; the
 * definition is static, filled from the configuration before registering),
 * declares its needs to the platform, the
 * transport (one link and its command share) and the media engine (one
 * requirement per audio format, with the configured latency), registers the
 * signal subscriptions (with refuse_changed_contacts also "callers_update"
 * and the contacts module's change event), the profile scene, intents,
 * commands and
 * diagnostics, and subscribes to the settings module's changes when the
 * product has it. start: chooses the audio format the board supports and
 * reads the contributions. */
xiaotai_err_t xiaotai_module_wechat_init(void);
void xiaotai_module_wechat_start(void);
/* Host tests: end the call and forget configuration and state. */
void xiaotai_wechat_reset(void);

#ifdef __cplusplus
}
#endif
