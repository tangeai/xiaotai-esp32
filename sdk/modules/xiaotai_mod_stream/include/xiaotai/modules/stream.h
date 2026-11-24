// SPDX-License-Identifier: MIT
#pragma once
/* Base module "stream": the web real-time stream.
 *
 * A viewer (a web page) dials the device; the device answers without being
 * asked and both sides exchange audio until either ends it. This module owns
 * the inbound listener of that link, decides when it admits, runs the audio
 * session and tells applications when a stream starts and ends. It has no
 * business flow and shows no view. Video rides on it as a session extension
 * (xiaotai/framework.h; the module "stream_video") that contributes the video
 * part of the link and the video members of the profile scene (see "Video"
 * below); this module itself never touches a camera.
 *
 * Admission. The listener admits only while all of these hold: the user allows
 * it (setting "stream.enabled"), no hold exists, the system is not quiesced,
 * the platform session and the transport are online, (unless the product
 * says otherwise) the server has accepted this session's device profile, which
 * is what tells the viewer the stream numbers below, and, where the
 * configuration asks for it (platform_idle), the platform core has no HTTP
 * exchange in flight. A stream never outlives the binding it was admitted
 * under: when the binding ends or is being verified it ends with
 * XIAOTAI_STREAM_END_BINDING. An admitted link becomes
 * the foreground activity "stream" (xiaotai/framework.h); when the framework
 * answers "busy" the link is closed at once. Whether a stream outranks another
 * activity is the product's priority table; whether a link can be admitted
 * while another one is open is the transport's capacity. The listener is
 * OPEN, so an inbound link reaches it only while no other module's listener
 * is ONCE (xiaotai/link.h): a device call that waits for its callee, or holds
 * the window after a cancelled dial, takes those links itself.
 *
 * Sending. The device never permits itself to send: audio and video go out
 * only while the viewer subscribes to the stream concerned.
 *
 * Generations: the admitted link's generation (xiaotai/link.h) names the
 * stream everywhere. A stop that names another generation does nothing and is
 * counted.
 *
 * Tags are those of xiaotai/framework.h: [init], [owner], [any], [rt]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/link.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- the protocol */

/* Stream numbers the device announces in its profile and uses on the link,
 * from the device's point of view. */
#define XIAOTAI_STREAM_UP_AUDIO 10U
#define XIAOTAI_STREAM_UP_VIDEO 11U
#define XIAOTAI_STREAM_DOWN_AUDIO 14U
#define XIAOTAI_STREAM_DOWN_VIDEO 15U

/* The foreground activity this module begins (the priority table's key, and
 * the name extensions attach to with xiaotai_activity_attach). The activity
 * begins after the link has opened and carries the link's generation from its
 * begin (xiaotai_activity_t.generation): an attachment's begin() receives it,
 * and finds the phase XIAOTAI_STREAM_STARTING and the same generation in
 * xiaotai_stream_status(). When this module ends a stream itself it sets the
 * generation back to 0 (xiaotai_activity_set_generation: attachments hear
 * session(id, 0), stop), then ends the activity (end()), and only then closes
 * the media session and the link. On a revocation the framework runs end()
 * before the module's revoked(); the link may be closing by then, so calls
 * that name the generation are stale and harmless. */
#define XIAOTAI_STREAM_ACTIVITY "stream"
/* Scene of the device profile this module writes (xiaotai/platform.h). */
#define XIAOTAI_STREAM_PROFILE_SCENE "stream"

/* ------------------------------------------------------------ configuration */

/* platform_idle values. */
typedef enum {
    /* Wait where the memory port reports no external RAM, otherwise ignore. */
    XIAOTAI_STREAM_PLATFORM_IDLE_AUTO = 0,
    XIAOTAI_STREAM_PLATFORM_IDLE_WAIT,
    XIAOTAI_STREAM_PLATFORM_IDLE_IGNORE,
} xiaotai_stream_platform_idle_t;

/* What a product chooses. Every field: 0 (false) selects the default. */
typedef struct {
    /* Admit viewers as soon as the platform session is online, without waiting
     * for the server to accept the device profile. Default false. */
    bool admit_before_profile;
    /* Environment maintenance refuses if a viewer already won its native
     * slot, even before the stream owner received opened(). Default false. */
    bool preserve_inbound_on_environment;
    /* Decline new viewers while an exact local stop closes its media and link.
     * Keep a replacement already admitted before the stop began. Default false. */
    bool pause_admission_on_local_stop;
    /* List only PCMA 8 kHz mono in the viewer receive profile. Default false:
     * also list Opus 16 kHz mono when available. A product that enables the
     * transport-wide accept_pcma_opus_downlink override still accepts Opus. */
    bool receive_pcma_only;
    /* Wait for the Contacts owner's first pending-confirmation request in
     * every platform session. A received error remains diagnostic; devices
     * that cannot issue the request do not listen. Default false. */
    bool wait_initial_contact_confirmation;
    /* WAIT: admit only while the platform core has no request or transfer
     * queued, running or awaiting its handler (xiaotai/platform.h). For a
     * device whose memory cannot hold a secure HTTP exchange and an admitted
     * link at once: its own exchanges (profile, contacts) go first and a
     * viewer waits for them. A stream that is active is not ended. Nothing in
     * this choice names a board: AUTO decides by external RAM. */
    xiaotai_stream_platform_idle_t platform_idle;
    /* Stream numbers 1..15 announced and used instead of the ones above, for
     * products whose deployed viewers were told other numbers. 0: the default.
     * Audio and video of one direction never share a number. */
    uint8_t up_audio_stream, down_audio_stream;
    uint8_t up_video_stream, down_video_stream;
} xiaotai_stream_config_t;

/* [glue, init] From the product glue or an application's init(); applied when
 * the module starts. The struct is copied. NULL: all defaults.
 * XIAOTAI_ERR_INVALID_ARG for a stream number above 15, two equal numbers in
 * one direction or an unknown platform_idle; XIAOTAI_ERR_INVALID_STATE once
 * the module has started. */
xiaotai_err_t xiaotai_stream_configure(const xiaotai_stream_config_t *config);

/* -------------------------------------------------------------------- state */

typedef enum {
    XIAOTAI_STREAM_CLOSED = 0,  /* not admitting; closed says why */
    XIAOTAI_STREAM_LISTENING,   /* a viewer would be admitted */
    XIAOTAI_STREAM_STARTING,    /* a link was admitted; the audio session is being opened */
    XIAOTAI_STREAM_ACTIVE,
} xiaotai_stream_phase_t;

/* Why the listener admits nothing. New reasons are appended. */
enum {
    XIAOTAI_STREAM_CLOSED_DISABLED = 1u << 0,  /* the setting "stream.enabled" is off */
    XIAOTAI_STREAM_CLOSED_HELD = 1u << 1,      /* a hold exists */
    XIAOTAI_STREAM_CLOSED_QUIESCED = 1u << 2,  /* the system is quiesced */
    XIAOTAI_STREAM_CLOSED_OFFLINE = 1u << 3,   /* the platform session or the transport is not online */
    XIAOTAI_STREAM_CLOSED_PROFILE = 1u << 4,   /* the device profile is not accepted yet */
    XIAOTAI_STREAM_CLOSED_MEMORY = 1u << 5,    /* the listener could not be armed (command pool); retried */
    XIAOTAI_STREAM_CLOSED_PLATFORM = 1u << 6,  /* waiting for the platform core's exchanges (platform_idle) */
    XIAOTAI_STREAM_CLOSED_CONTACTS = 1u << 7,  /* waiting for the initial pending-confirmation request */
};

/* How a stream ended. Appended, never renumbered. */
typedef enum {
    XIAOTAI_STREAM_END_PEER = 0,   /* the viewer left */
    XIAOTAI_STREAM_END_LOCAL,      /* xiaotai_stream_stop() or the intent */
    XIAOTAI_STREAM_END_REVOKED,    /* the activity was revoked: XIAOTAI_REVOKE_PREEMPTED (an incoming
                                      call, any higher-priority activity) or XIAOTAI_REVOKE_SYSTEM */
    XIAOTAI_STREAM_END_BUSY,       /* refused on arrival: the device was not free */
    XIAOTAI_STREAM_END_CLOSED,     /* refused on arrival: admission had just been withdrawn */
    XIAOTAI_STREAM_END_LINK,       /* the link broke (network, timeout, transport down) */
    XIAOTAI_STREAM_END_MEDIA,      /* the audio session could not start or failed */
    XIAOTAI_STREAM_END_BINDING,    /* the binding ended or is being verified (the platform went offline
                                      with reason BINDING, or a binding is needed) */
} xiaotai_stream_end_t;

typedef struct {
    xiaotai_stream_phase_t phase;
    uint32_t closed;            /* XIAOTAI_STREAM_CLOSED_* while the phase is CLOSED, else 0 */
    uint32_t generation;        /* the stream's link; 0 unless STARTING or ACTIVE */
    bool peer_audio;            /* the viewer takes this device's audio */
    bool peer_video;            /* the viewer takes this device's video */
    bool video_declared;        /* a usable video contribution is part of the link (see "Video") */
    uint8_t holds;              /* holds in effect */
    xiaotai_stream_end_t last_end;   /* how the previous stream ended */
    xiaotai_err_t last_error;        /* the engine's result that came with it; XIAOTAI_OK */
} xiaotai_stream_status_t;

/* [any] */
void xiaotai_stream_status(xiaotai_stream_status_t *status);

/* ------------------------------------------------------------------ actions */

/* [any] End the stream of this generation (as the caller saw it in the status
 * or in XIAOTAI_EVENT_STREAM_STARTED). Queued to the owner task: XIAOTAI_OK
 * means accepted. A generation that is no longer the stream when the request
 * is reached is ignored and counted. Admission is not changed: the viewer may
 * dial again. XIAOTAI_ERR_INVALID_ARG for 0; XIAOTAI_ERR_NO_MEM when the
 * owner's queue is full. */
xiaotai_err_t xiaotai_stream_stop(uint32_t generation);

/* Holds a build may have at once. */
#define XIAOTAI_STREAM_HOLDS_MAX 8U

/* [owner] Keeps the listener closed while at least one hold exists: for a
 * module or an application that must not be dialled into for a while (an
 * operation that expects another inbound link, a privacy mode). A stream that
 * is active is not ended. reason is a fixed word for diagnostics (static
 * storage). *hold is never 0. XIAOTAI_ERR_NO_MEM when XIAOTAI_STREAM_HOLDS_MAX
 * exist. Once this returns nothing more is admitted, and a link admitted
 * before whose opening has not been reported yet is closed unreported. */
xiaotai_err_t xiaotai_stream_hold(const char *reason, uint32_t *hold);
/* [owner] Unknown and already released holds are ignored. */
void xiaotai_stream_release(uint32_t hold);

/* -------------------------------------------------------------------- video */

/* Video is a session extension (xiaotai/framework.h, "Session extensions"):
 * a module that adds video attaches to XIAOTAI_STREAM_ACTIVITY with these
 * contributions, for the scene XIAOTAI_STREAM_PROFILE_SCENE or for every
 * scene, and this module has no API of its own for it:
 *   - XIAOTAI_LINK_SLOT_VIDEO (xiaotai/link.h): a xiaotai_link_media_t. This
 *     module copies it into the video part of the link profile it admits
 *     with, then puts its own video stream numbers in (send_stream: the up
 *     video stream, receive_stream: the down video stream), because it
 *     announces them in the profile. send, send_format, peer_streams,
 *     receive, receive_request and receive_formats are the extension's; it
 *     starts the viewer's video itself with xiaotai_link_receive(). Frames
 *     are its business (xiaotai/video_link.h).
 *   - XIAOTAI_PLATFORM_SLOT_SCENE (xiaotai/platform.h): a
 *     xiaotai_platform_scene_part_t writing the video members of the scene
 *     (codecs, rotation, mirrors, aspect ratio, fit). As with every host of
 *     that slot, "no_video" is the host's: a part never writes it.
 *     This module writes its audio stream numbers and audio members, splices
 *     the parts in attachment order (xiaotai_platform_scene_parts), and then
 *     writes, by whether a part was spliced in, either both video stream
 *     numbers and "no_video":false, or the "no video" members. The video
 *     stream numbers are this module's, unlike on the other hosts, because it
 *     is the one that puts them into the link profile (above), so a part
 *     never writes them either.
 * The contributions are collected once, in start(). The first video record
 * is the one used; a later one, and a record that neither sends nor receives
 * or receives without a format, is ignored (counted: extensions_ignored).
 * Without a usable record the link carries no video (the viewer's video
 * subscription is refused) and the scene reports a stream without video
 * ("no_video":true, empty codec lists, no video stream numbers) whatever
 * parts were contributed. With one, the scene carries the parts, both video
 * stream numbers and "no_video":false; when no part was spliced in (each
 * wrote nothing, failed or did not fit) the scene is reported as without
 * video as well and profile_overflows counts it. A record that sends adds
 * XIAOTAI_RESOURCE_CAMERA to the activity. */

/* ------------------------------------------------------------------- events */

/* Emitted on the framework event bus, in the order things happened. */

/* Admission changed. args.value: the new xiaotai_stream_phase_t (CLOSED or
 * LISTENING). */
#define XIAOTAI_EVENT_STREAM_STATE "stream.state"
/* A viewer's stream is active: the audio session runs. args.value: the
 * generation. */
#define XIAOTAI_EVENT_STREAM_STARTED "stream.started"
/* A stream ended, or an arriving viewer was turned away. args.value: the
 * xiaotai_stream_end_t; data: a xiaotai_stream_ended_t. A viewer turned away
 * (BUSY, CLOSED) never had a STARTED event. */
#define XIAOTAI_EVENT_STREAM_ENDED "stream.ended"

typedef struct {
    uint32_t generation;
    xiaotai_stream_end_t end;
    xiaotai_err_t error;     /* the engine's result that came with it; XIAOTAI_OK */
    bool started;            /* XIAOTAI_EVENT_STREAM_STARTED had been emitted for it */
} xiaotai_stream_ended_t;

/* ------------------------------------------------------- intents and settings */

/* stream.stop: ends the stream that is active when the intent is handled; no
 * arguments. */
#define XIAOTAI_INTENT_STREAM_STOP "stream.stop"

/* Setting (xiaotai/framework.h), boolean, default 1: viewers may dial in.
 * Turning it off closes the listener; a stream that is active goes on until
 * it is stopped. Storage key "stream_on". */
#define XIAOTAI_SETTING_STREAM_ENABLED "stream.enabled"

/* -------------------------------------------------------------- diagnostics */

/* Counters since boot. */
typedef struct {
    uint32_t admitted;            /* links reported open by the listener */
    uint32_t started;             /* streams that became active */
    uint32_t ended_peer, ended_local, ended_revoked, ended_link, ended_media, ended_binding;
    uint32_t refused_busy;        /* turned away: the device was not free */
    uint32_t refused_closed;      /* turned away: admission had been withdrawn */
    uint32_t preempted_others;    /* streams that began by displacing another activity */
    uint32_t stale_stops;         /* stops naming a generation that was not the stream */
    uint32_t listen_failures;     /* the listener could not be armed */
    uint32_t profile_overflows;   /* the scene did not fit the report, or no video part was spliced in */
    uint32_t extensions_ignored;  /* video contributions not used (a second one, or unusable) */
} xiaotai_stream_stats_t;

/* [any] */
void xiaotai_stream_stats(xiaotai_stream_stats_t *stats);

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: declares what it needs from the media and transport engines (the
 * transport: one link and a command share of 512 bytes, see below), registers
 * the profile scene, the setting, the intent and the diagnostics section
 * "stream", and subscribes to the events that move admission and to the
 * platform's binding events. start: applies the configuration, collects the
 * video contributions (valid from start on, after every extension attached in
 * its init) and registers the inbound listener (xiaotai_link_listen allows it
 * there); from then on the listener exists, closed until admission holds.
 * Neither touches the network.
 *
 * Commands: the protocol has none, so the link takes payloads of at most 256
 * bytes with a share of 512 bytes of the transport's command pool
 * (xiaotai_link_profile_t; declared with xiaotai_transport_require). */
xiaotai_err_t xiaotai_module_stream_init(void);
void xiaotai_module_stream_start(void);
/* Host tests: forget configuration, extension, holds, state and counters. */
void xiaotai_stream_reset(void);

#ifdef __cplusplus
}
#endif
