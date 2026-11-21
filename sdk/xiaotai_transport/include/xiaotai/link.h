// SPDX-License-Identifier: MIT
#pragma once
/* Links: real-time connections to one peer, carrying audio, video and commands.
 *
 * The transport knows how a link is made (its kind) and what it carries (its
 * profile); it does not know which base module uses it. A module describes the
 * link it wants - streams, formats, limits - and the transport applies that
 * description identically for every module. Stream numbers, command words and
 * handshakes are protocol facts of the module, never of the transport.
 *
 * Generations. Every link has a generation: a non-zero uint32_t taken from the
 * device-wide counter (xiaotai_generation_next() in xiaotai/framework.h), so
 * it never equals the generation of a media session that has no link. It is
 * assigned when the link is requested (outbound) or admitted (inbound). Every
 * call names the generation it means; a call that names a link
 * which is no longer live does nothing, returns XIAOTAI_ERR_INVALID_STATE where
 * it returns a result, and is counted (xiaotai_transport_status_t.stale_calls).
 * A module opens its media session with the link's generation, so one number
 * identifies the session on both engines.
 *
 * Threads. [init], [owner], [any] and [rt] as defined in xiaotai/framework.h;
 * [init] here is "[init], applications excluded" (links are a module's
 * concern), and an [any] call that waits for another control call says how
 * long. Plus the transport's own:
 *   [uplink] a task that sends media: the media engine's uplink path, a video
 *            encoder or uplink task. The transport's own part never blocks,
 *            allocates or logs, but the call enters the RTC stack, a
 *            precompiled library that may take its own locks and allocate
 *            inside its send buffer (bounded by the engine's send buffer
 *            limit; full means XIAOTAI_ERR_BUSY, never a wait for the
 *            network). The longest call seen is reported
 *            (xiaotai_transport_status_t.send_call_max_us). Never from a
 *            task that reads PCM from an input part or writes to an output
 *            part, a receiver, or a handler.
 *   [receive thread] the transport's receive thread, where receivers run
 *            (see "Callbacks"): as [rt], and no function of this header.
 * Nothing in this header may be called from an interrupt handler.
 *
 * Callbacks.
 *   Handlers (xiaotai_link_handlers_t) run on the owner task, one at a time,
 *   in order for a given link: opened, then commands and peer subscriptions as
 *   they arrived, then closed. A handler may call every function here,
 *   including xiaotai_link_disconnect() of its own link and
 *   xiaotai_link_connect() for a new one.
 *   Commands and peer subscriptions that the RTC stack delivers for an
 *   outbound link before its connect result are kept, in order, and delivered
 *   after opened() (a peer may answer before the stack reports the link open).
 *   If the link ends without opening they are dropped and counted
 *   (xiaotai_transport_status_t.early_dropped); a product whose configuration
 *   sets early_events_dropped (xiaotai/transport.h) drops and counts them at
 *   once instead.
 *   Receivers (xiaotai_link_receive_fn_t) run on the transport's receive
 *   thread, which belongs to the RTC stack. A receiver copies the frame and
 *   returns: it must not block, allocate, log, or call any function of this
 *   header or of xiaotai/transport.h. Receivers of one media kind never run
 *   concurrently, whatever link they belong to: a frame that arrives while
 *   another one of its kind is being delivered is dropped and counted.
 *
 * Results. XIAOTAI_OK; XIAOTAI_ERR_INVALID_ARG (a parameter breaks the rules
 * stated at the function); XIAOTAI_ERR_INVALID_STATE (the transport is not
 * online, or the generation is not a live link in the state the call needs);
 * XIAOTAI_ERR_BUSY (no free link, another control call held the link too long,
 * or the send path is congested: the frame was dropped); XIAOTAI_ERR_NO_MEM;
 * XIAOTAI_ERR_NOT_SUPPORTED (this build's RTC stack cannot do it, or the
 * link's profile excludes it); XIAOTAI_ERR_INVALID_SIZE; XIAOTAI_FAIL (the RTC
 * stack refused; the reason is counted). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/codec.h"
#include "xiaotai/port/err.h"
#include "xiaotai/transport_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ kinds and formats */

/* How a link is made. New ways are appended; values never change. */
typedef enum {
    XIAOTAI_LINK_WHIP = 0,   /* outbound to a media server: peer id and token */
    XIAOTAI_LINK_P2P,        /* outbound to another device: peer id and token */
    XIAOTAI_LINK_INBOUND,    /* a peer dialled in and a listener admitted it */
} xiaotai_link_kind_t;

typedef enum {
    XIAOTAI_LINK_AUDIO = 0,
    XIAOTAI_LINK_VIDEO,
} xiaotai_link_media_kind_t;

/* Wire codecs: the device's one codec vocabulary (xiaotai/codec.h), which
 * the hardware abstraction, the media engine and the video engine name too,
 * so no component converts between lists. The transport carries audio and
 * video in one frame type. Nothing in this header limits which codec a link
 * may carry: whether the linked RTC stack can frame a codec is the stack's
 * fact (see the format below), so a new codec is one more value in
 * xiaotai/codec.h and one more row in the backend's table, never a change to
 * a function of this header. The former names stay as spellings of the same
 * values. */
typedef xiaotai_codec_t xiaotai_link_codec_t;
#define XIAOTAI_LINK_CODEC_NONE XIAOTAI_CODEC_NONE
#define XIAOTAI_LINK_CODEC_G711A XIAOTAI_CODEC_G711A   /* audio */
#define XIAOTAI_LINK_CODEC_OPUS XIAOTAI_CODEC_OPUS     /* audio */
#define XIAOTAI_LINK_CODEC_JPEG XIAOTAI_CODEC_JPEG     /* video: every frame decodes alone */
#define XIAOTAI_LINK_CODEC_H264 XIAOTAI_CODEC_H264     /* video: Annex-B access units */

/* Any rate and channel count may be named; whether the linked RTC stack can
 * carry it is the stack's fact: a profile naming a format it cannot frame is
 * refused with XIAOTAI_ERR_NOT_SUPPORTED (by xiaotai_link_connect and
 * xiaotai_link_listen), and a received frame in a format the stack cannot
 * describe arrives with codec NONE and is filtered. */
typedef struct {
    xiaotai_link_codec_t codec;
    uint32_t rate_hz;    /* audio sample rate in Hz; video: 0 */
    uint8_t channels;    /* audio: 1 or more; video: 0 */
} xiaotai_link_format_t;

/* Streams of a link are numbered 0..15. Audio and video of one link never
 * share a number. */
#define XIAOTAI_LINK_STREAM_MAX 15U
/* receive_stream: accept every stream of this media kind. */
#define XIAOTAI_LINK_STREAM_ANY 0xFFU

/* Formats a link accepts per media kind. */
#define XIAOTAI_LINK_RECEIVE_FORMATS 4U

/* What a peer's subscription to a stream other than send_stream does. A
 * subscription to send_stream itself is always accepted. */
typedef enum {
    /* Refused: the peer is told this side has no such stream. */
    XIAOTAI_LINK_PEER_STREAM_REFUSE = 0,
    /* Accepted and reported to peer_subscribed, changes nothing: sending stays
     * on send_stream and the subscription does not open the send gate. */
    XIAOTAI_LINK_PEER_STREAM_IGNORE,
    /* Accepted: the peer names the stream. Sending moves to that stream and
     * the subscription opens the send gate (send_stream is the one used until
     * then). Audio only. */
    XIAOTAI_LINK_PEER_STREAM_FOLLOW,
} xiaotai_link_peer_stream_t;

/* One media kind of a link. A zeroed struct means the link carries none of it. */
typedef struct {
    /* --- sending --- */
    bool send;               /* this side sends this media kind */
    uint8_t send_stream;     /* 0..15 */
    xiaotai_link_peer_stream_t peer_streams;
    /* Audio: the format of every packet sent. A G.711 packet is exactly 20 ms
     * (rate_hz / 50 * channels bytes). Video: codec NONE lets each frame name
     * any codec the stack can frame; a codec restricts every frame to it. */
    xiaotai_link_format_t send_format;

    /* --- receiving --- */
    bool receive;            /* this side accepts this media kind */
    uint8_t receive_stream;  /* 0..15, or XIAOTAI_LINK_STREAM_ANY */
    /* xiaotai_link_receive() also asks the peer to send receive_stream (it must
     * then be 0..15). Without it the peer is expected to send on its own. */
    bool receive_request;
    /* Accept any valid stream number in the configured formats from this
     * connection. Subscription/stop still request receive_stream; default
     * false keeps receive_stream as the filter as well. */
    bool accept_peer_stream;
    /* Accepted formats; unused entries are zeroed (codec NONE). A frame in any
     * other format is dropped and counted (and, for audio, ends the link with
     * XIAOTAI_LINK_END_FORMAT where the product sets format_error_ends_link
     * in xiaotai/transport.h). */
    xiaotai_link_format_t receive_formats[XIAOTAI_LINK_RECEIVE_FORMATS];
} xiaotai_link_media_t;

/* Session extensions (xiaotai/framework.h, xiaotai_contribution_t). An
 * extension that adds video to a host's links contributes under this slot a
 * `const xiaotai_link_media_t` (data points to it, static storage) for the
 * host's scene; the host copies it into the video part of every link profile
 * it builds for that scene. A host that finds no contribution has no video. */
#define XIAOTAI_LINK_SLOT_VIDEO "link.video"

/* Default for command_max_bytes. */
#define XIAOTAI_LINK_COMMAND_DEFAULT_BYTES 4096U
/* Largest command_max_bytes and command_pool_bytes. A sanity bound, not a
 * budget (a room link on a board with external RAM: 40960 and 81920). */
#define XIAOTAI_LINK_COMMAND_LIMIT_BYTES 131072U

/* What a link carries. Given once, when the link is requested or its listener
 * is registered; copied by the transport; fixed for the life of the link.
 *
 * Commands. The RTC stack owns an incoming payload only during its upcall,
 * so the transport copies it there. Resident mode copies into a BULK block;
 * on-demand mode allocates a bounded copy for each control command. Link
 * opening, admission and arming a listener never allocate command payloads.
 * A payload that does not fit is still reported with dropped set. A failed
 * on-demand allocation is counted but never dispatches a control word. */
typedef struct {
    xiaotai_link_media_t audio;
    xiaotai_link_media_t video;
    /* Largest inbound command payload delivered to the command handler; a
     * larger one is reported with dropped set. 0: the default above. */
    uint32_t command_max_bytes;
    /* This link's share of the engine's command pool: payload bytes of this
     * link that may wait for the owner task at once, so one link's burst
     * cannot starve another. A payload is kept when it fits both the share
     * and the free pool. 0: twice command_max_bytes. At least
     * command_max_bytes (INVALID_ARG otherwise). A module declares its
     * largest share in xiaotai_transport_require(); xiaotai_link_stats()
     * reports the peak a link reached, which is how a product without
     * external RAM sizes it. */
    uint32_t command_pool_bytes;
    /* Opt a peer link in to generation-tagged close on a supported backend. */
    bool track_peer_retirement;
} xiaotai_link_profile_t;

/* ---------------------------------------------------------------- handlers */

/* Why a link ended without xiaotai_link_disconnect(). Appended, never renumbered. */
typedef enum {
    XIAOTAI_LINK_END_PEER = 0,        /* the peer closed it in an orderly way */
    XIAOTAI_LINK_END_CONNECT_FAILED,  /* outbound: never opened (refused, unreachable, bad token) */
    XIAOTAI_LINK_END_TIMEOUT,         /* outbound: not open within the request's timeout; open: the peer went silent */
    XIAOTAI_LINK_END_NETWORK,         /* an open link broke */
    XIAOTAI_LINK_END_NO_MEMORY,       /* the RTC stack ran out of memory */
    XIAOTAI_LINK_END_OVERFLOW,        /* the transport could not keep an event of this link; nothing was delivered out of order (with overflow_fault the engine then faults) */
    XIAOTAI_LINK_END_STOPPED,         /* the transport went down (hold, restart, account change, fault) */
    /* Product rules of xiaotai/transport.h: */
    XIAOTAI_LINK_END_FORMAT,          /* a received audio frame outside the profile's formats (format_error_ends_link) */
    XIAOTAI_LINK_END_ALERT,           /* the stack reported its service requests redirected (alert_ends_links) */
    XIAOTAI_LINK_END_PEER_FAULT,      /* the stack reported a peer close as an error, with its diagnostic detail */
} xiaotai_link_end_t;

/* One received command. */
typedef struct {
    uint32_t word;       /* the command word exactly as received; never re-encoded */
    uint16_t id;         /* (word >> 1) & 0x7fff: the id when the peer packed the word */
    uint16_t sequence;   /* word >> 16 */
    bool response;       /* word & 1 */
    /* The payload could not be kept (larger than command_max_bytes, or no
     * room in the link's share or in the engine's command pool): data is
     * NULL, size is still the received size. */
    bool dropped;
    /* Copy of the payload with a terminating NUL after data[size - 1] (not
     * counted in size). Valid only during the handler. NULL when size is 0. */
    const uint8_t *data;
    size_t size;
} xiaotai_link_command_t;

/* For signalling words that a peer may send either bare or packed with a
 * sequence number and a response bit (the JSON-RPC words of the AI and room
 * services): true when word carries the even id in either form. Words that
 * differ from another word only in bit 0 (a bare "answered" and "hang up"
 * pair) are compared with ==, never with this. */
static inline bool xiaotai_link_command_is(uint32_t word, uint16_t id)
{
    return ((word >> 1) & 0x7fffU) == (uint32_t)id || (word & 0xfffeU) == (uint32_t)id;
}

/* All [owner]. Any member may be NULL. Copied by the transport (the struct
 * need not outlive the call that passes it; ctx must outlive the link). */
typedef struct {
    /* The link is open: commands may be sent, receiving and sending may start. */
    void (*opened)(void *ctx, uint32_t generation);
    /* The link is gone and its generation is dead; the last call for it. The
     * transport has already released it: the handler does not disconnect.
     * Called once for every link that ends for a reason other than this
     * side's xiaotai_link_disconnect() (after which nothing is called).
     * detail is the RTC stack's own code, for diagnostics only: do not branch on it. */
    void (*closed)(void *ctx, uint32_t generation, xiaotai_link_end_t reason, int detail);
    void (*command)(void *ctx, uint32_t generation, const xiaotai_link_command_t *command);
    /* The peer started or stopped wanting a stream of this side. Reported for
     * send_stream and for every stream accepted under peer_streams IGNORE or
     * FOLLOW; a refused subscription is never reported. A peer that withdraws
     * normally closes only its subscription gate: a module that wants to
     * stop sending also withdraws its grant (xiaotai_link_permit) here.
     * With audio_unsubscribe_revokes_permit, the current audio stream's
     * withdrawal clears both gates before this handler runs. */
    void (*peer_subscribed)(void *ctx, uint32_t generation, xiaotai_link_media_kind_t kind, uint8_t stream,
                            bool subscribed);
    void *ctx;
} xiaotai_link_handlers_t;

/* -------------------------------------------------------- outbound links */

/* Sizes including the terminating NUL. */
#define XIAOTAI_LINK_PEER_MAX 2048U
#define XIAOTAI_LINK_TOKEN_MAX 1024U

typedef struct {
    xiaotai_link_kind_t kind;   /* XIAOTAI_LINK_WHIP or XIAOTAI_LINK_P2P */
    /* Copied before the call returns; the copies are wiped when the link ends.
     * Never logged. */
    const char *peer;
    const char *token;
    /* The link opens within this time or ends with XIAOTAI_LINK_END_TIMEOUT
     * (and, where the product sets connect_timeout_fault in
     * xiaotai/transport.h, the stack is brought down as a fault).
     * 0: the transport sets no limit and the caller keeps its own deadline. */
    uint32_t timeout_ms;
    xiaotai_link_profile_t profile;
} xiaotai_link_request_t;

/* [owner] Requests an outbound link; returns without waiting for the network.
 * On XIAOTAI_OK *generation names it and exactly one of handlers->opened or
 * handlers->closed follows (unless the caller disconnects or finishes the
 * opening first). Fails with
 * INVALID_STATE while the transport is not online (always in a provisioning
 * boot), BUSY when every link is in use, NO_MEM when the copies of peer and
 * token or the memory the RTC stack needs to connect are not available
 * (xiaotai_transport_status_t.connect_ready tells in advance), NOT_SUPPORTED
 * for a kind or format this build's RTC stack
 * lacks, INVALID_ARG for a profile that breaks the rules above, INVALID_SIZE
 * for a peer, token or command size past the limits. A link disconnected by
 * this side frees its place at once: a request made right after
 * xiaotai_link_disconnect() is accepted, and the transport finishes releasing
 * the old link before it starts the new one, inside the new request's timeout
 * (a product with release_failures_tolerated starts the new one at once).
 * A release the RTC stack does not finish within the product's
 * release_timeout_ms (xiaotai/transport.h) is a fault of the stack: every link
 * ends with XIAOTAI_LINK_END_STOPPED and the engine recovers by its fault
 * rules, so one stuck release never blocks later links for good (with
 * release_failures_tolerated it is counted and forgotten instead). */
xiaotai_err_t xiaotai_link_connect(const xiaotai_link_request_t *request, const xiaotai_link_handlers_t *handlers,
                                   uint32_t *generation);

/* [owner] Ends a link in any state (requested, open). When it returns, no
 * handler and no receiver of that generation is running or will run, and
 * every later call naming it is stale; the release towards the peer goes on in
 * the background (xiaotai_transport_status_t.links_releasing). It waits only
 * for a receiver that is delivering a frame at that moment. Unknown or dead
 * generations are ignored. */
void xiaotai_link_disconnect(uint32_t generation);

typedef enum {
    XIAOTAI_LINK_RELEASE_UNWATCHED,
    XIAOTAI_LINK_RELEASE_PENDING,
    XIAOTAI_LINK_RELEASE_COMPLETE,
    XIAOTAI_LINK_RELEASE_ABORTED,
} xiaotai_link_release_state_t;

/* Watch one open WHIP or opted-in, tagged peer link by generation, not its reusable SDK handle.
 * COMPLETE means the acknowledged record was retired with no outstanding
 * pins, its borrowed arguments were freed, and the backend proved published
 * connection retirement. An unsupported backend rejects the watch.
 * Confirmed stack shutdown after cleanup is
 * ABORTED, and a close without a release record does not prove COMPLETE. */
xiaotai_err_t xiaotai_link_watch_release(uint32_t generation);
xiaotai_link_release_state_t xiaotai_link_release_state(uint32_t generation);
void xiaotai_link_unwatch_release(uint32_t generation);

/* [owner] Ends the opening phase of an outbound link before its opened()
 * handler. Atomically chooses between withdrawing a pending request and
 * keeping a physically opened link just for its final command. OK with
 * *opened=false means it never opened and has been withdrawn or already
 * failed; no handler will run. *opened=true means it physically opened and
 * must not be rejected as an unjoined call, even if the command fails or the
 * link has since closed. This consumes opened() and discards any queued
 * inbound commands; no media starts. Only closed() may follow, unless the
 * caller disconnects first. Disconnect an open link after the required
 * signaling delay. If send=false the command arguments are ignored. If
 * send=true, command acceptance and errors are as for xiaotai_link_command().
 * INVALID_STATE with *opened=false is an unrelated, already reported or dead
 * generation, not permission to signal rejection. Does not wait for a
 * connect result or guarantee delivery to the peer. */
xiaotai_err_t xiaotai_link_finish_opening(uint32_t generation, bool *opened, bool send,
                                        uint32_t word, const void *data, size_t size);

/* --------------------------------------------------------- inbound links */

/* Listeners a build may register: the build option
 * XIAOTAI_TRANSPORT_LISTENERS (xiaotai/transport_config.h), default 4. Their
 * table is one block of BULK memory taken at the first registration of a
 * normal boot; in a provisioning boot registrations are checked and numbered
 * and nothing is stored, since nothing is admitted in that boot. */
#define XIAOTAI_LINK_LISTENERS_MAX (XIAOTAI_TRANSPORT_LISTENERS + 0U)

/* A listener admits links that peers open towards this device.
 *
 * Classification. The RTC stack reports nothing about an inbound link before
 * it is admitted - no peer, no service - so a listener cannot filter, and
 * which module an inbound link belongs to is decided by arming: a module that
 * expects a particular peer learns of it through its own signaling and arms
 * ONCE for that wait, which takes precedence over every OPEN listener; OPEN is
 * for the one module that accepts unannounced peers (the web stream). A
 * second OPEN listener receives inbound links only while every earlier one is
 * OFF. A module that must tell peers apart after admission does so with its
 * own protocol (the first command) and disconnects the wrong ones. */
typedef struct {
    const char *name;                  /* for diagnostics; a string constant */
    xiaotai_link_profile_t profile;    /* applied to every link it admits */
    xiaotai_link_handlers_t handlers;  /* opened() reports an admitted link */
    /* Optional OPEN guard, called with handlers.ctx before taking a slot
     * and again before reporting the tentative link. It runs under the
     * transport lock: only read lock-free facts; never wait, allocate, log
     * or call back into transport. False refuses rather than falling through
     * to another listener. ONCE never calls it. NULL admits as usual. */
    bool (*admit_open)(void *ctx);
} xiaotai_link_listener_t;

typedef enum {
    XIAOTAI_LINK_LISTEN_OFF = 0,  /* admits nothing */
    XIAOTAI_LINK_LISTEN_ONCE,     /* admits the next inbound link, then turns itself OFF */
    XIAOTAI_LINK_LISTEN_OPEN,     /* admits every inbound link while a link is free */
} xiaotai_link_listen_t;

/* [init] or from the start() of an engine or module (the owner task, before
 * the framework dispatches anything; a module whose profile depends on what
 * another module declared in its init registers in start). Registers a
 * listener (copied); it starts OFF, so nothing is admitted before its module
 * arms it, whenever it registered. *listener_id identifies it from then on and
 * is never 0. NO_MEM when XIAOTAI_LINK_LISTENERS_MAX are registered;
 * INVALID_ARG for a profile that breaks the rules above; NOT_SUPPORTED for a
 * profile with a media kind or format this build's RTC stack lacks (a video
 * profile on a build without video): a module may register an audio-only and
 * a video listener and keep whichever succeeds. A failed registration takes no
 * place. NO_MEM also when the listener table cannot be taken. */
xiaotai_err_t xiaotai_link_listen(const xiaotai_link_listener_t *listener, uint32_t *listener_id);

/* [owner] Sets what the listener admits. An inbound link goes to the first
 * registered listener that is ONCE, else to the first that is OPEN (so while a
 * ONCE listener is armed, no OPEN listener receives an inbound link), provided
 * a link is free; otherwise it is refused and counted, and a ONCE listener
 * stays armed (it turns OFF where the product sets once_consumed_on_refusal,
 * xiaotai/transport.h). The change and admission exclude each other: once this returns
 * with OFF the listener admits nothing more, and a link it admitted whose
 * opened() has not run yet is closed without being reported. Arming allocates
 * nothing; the mode survives the transport going down and coming up. In a
 * provisioning boot a registered listener's mode is accepted and not kept:
 * nothing is admitted in that boot, so there is nothing to keep it for.
 * INVALID_ARG for an unknown listener. */
xiaotai_err_t xiaotai_link_listen_set(uint32_t listener_id, xiaotai_link_listen_t mode);

/* [owner] Block new inbound links while this listener holds a block. A
 * block is independent of the listener's mode: it refuses and counts new
 * inbound connections before choosing any listener or taking a link slot.
 * It does not consume ONCE or affect already admitted or outbound links.
 * Other listeners' blocks are not changed. Like the listener's mode, a
 * block survives reconnect; it is not kept during a provisioning boot.
 * INVALID_ARG for an unknown listener. */
xiaotai_err_t xiaotai_link_listener_block_inbound(uint32_t listener_id, bool blocked);

/* [owner] Close only this listener's OPEN admission, atomically with native
 * acceptance. A physical link which already won (including before opened())
 * returns BUSY and remains unchanged. Does not affect ONCE, LISTEN_OFF, or
 * the independent global inbound block. Release only a successful acquire. */
xiaotai_err_t xiaotai_link_listener_open_gate(uint32_t listener_id, bool acquire);

/* [owner] Pause new OPEN admissions to this listener while tearing down an
 * active link. Existing admissions and ONCE listeners are unchanged. Unlike
 * open_gate, an active link does not prevent this pause. The two blockers
 * are independent; releasing one never releases the other. */
xiaotai_err_t xiaotai_link_listener_pause_open(uint32_t listener_id, bool paused);

/* [owner] True while a link admitted by this listener is being released.
 * Includes links explicitly ended by their opened() handler, but never
 * links still open or links of another listener. A guarded OPEN's retirement
 * is not forgotten until RTC confirms its release (or the stack is released).
 * Recheck on transport.links: the event is coalesced, not the source of truth. */
bool xiaotai_link_listener_retiring(uint32_t listener_id);

/* ---------------------------------------------------------------- commands */

/* Longest a control call (command, receive, key-frame request, bitrate) waits
 * for another control call on the same engine before it gives up with BUSY. */
#define XIAOTAI_LINK_CONTROL_WAIT_MS 50U

/* [any] Sends a command on an open link. word goes out unchanged; data may be
 * NULL when size is 0, and is copied before the call returns. BUSY when the
 * RTC stack cannot take it now or another control call held the link for
 * XIAOTAI_LINK_CONTROL_WAIT_MS; the transport does not retry. Not from a
 * receiver. XIAOTAI_OK means the RTC stack took the command, not that the
 * peer received it: a xiaotai_link_disconnect() made right after it reaches
 * the stack later, but whether the stack sends what it queued before
 * releasing the connection is the stack's behaviour and is not promised. A
 * module that needs its last command delivered waits before disconnecting. */
xiaotai_err_t xiaotai_link_command(uint32_t generation, uint32_t word, const void *data, size_t size);

/* --------------------------------------------------------------- receiving */

/* One received frame. */
typedef struct {
    xiaotai_link_media_kind_t kind;
    xiaotai_link_format_t format;
    uint8_t stream;
    bool key_frame;          /* video */
    uint32_t timestamp_ms;   /* the sender's media clock */
} xiaotai_link_frame_t;

/* [receive thread] data is valid only during the call. Return false when the
 * frame was not taken (counted as refused). See "Callbacks" above. */
typedef bool (*xiaotai_link_receive_fn_t)(void *ctx, uint32_t generation, const xiaotai_link_frame_t *frame,
                                          const uint8_t *data, size_t size);

/* [any] Starts receiving one media kind on an open link whose profile
 * receives it: frames that pass the profile's stream and format rules go to
 * receiver. Acceptance is in place before the peer is asked to send
 * (receive_request), because the first frame may arrive before the request
 * returns; if asking fails, acceptance is withdrawn and the error returned.
 * Audio with a NULL receiver goes to the media engine's downlink
 * (xiaotai_media_push) under the link's generation, without blocking and in
 * the packets the peer sent, whatever their length; codec, rate, timestamp
 * and stream pass unchanged (one codec vocabulary, nothing converted). Video
 * needs a receiver.
 * Calling it again replaces the receiver and does not ask the peer again.
 * Frames that arrive while receiving is not started are dropped and counted.
 * Not from a receiver. */
xiaotai_err_t xiaotai_link_receive(uint32_t generation, xiaotai_link_media_kind_t kind,
                                   xiaotai_link_receive_fn_t receiver, void *ctx);

/* [any] Stops receiving; with receive_request the peer is asked to stop.
 * When it returns the receiver is not running and will not be called again.
 * Not from a receiver. */
void xiaotai_link_receive_stop(uint32_t generation, xiaotai_link_media_kind_t kind);

/* ----------------------------------------------------------------- sending */

/* A media kind may be sent while the link is open, its profile sends that
 * kind, and either the peer subscribed to the stream or the module permitted
 * it. A module permits after its own handshake (a session reply, a call
 * answer, a push-to-talk press); a link that waits for its peer never permits. */

/* [any] Grants or withdraws the module's permission. Cleared when the link
 * ends; a peer's unsubscription does not clear it. Never waits.
 * NOT_SUPPORTED when the profile does not send that kind. */
xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool allowed);

/* [rt] True when a frame of that kind sent now would pass the rule above. */
bool xiaotai_link_can_send(uint32_t generation, xiaotai_link_media_kind_t kind);

/* [uplink] Sends one audio packet in the profile's send_format on the current
 * send stream. data is copied before the call returns. INVALID_STATE: not
 * live or not allowed by the rule above (dropped, counted). BUSY: congested
 * (dropped, counted). INVALID_SIZE: empty, or not one G.711 packet. FAIL: the
 * stack refused it (counted; consecutive failures end the link where the
 * product sets audio_failures_end in xiaotai/transport.h). */
xiaotai_err_t xiaotai_link_send_audio(uint32_t generation, uint32_t timestamp_ms, const uint8_t *data, size_t size);

typedef struct {
    /* A video codec the RTC stack can frame and, when the profile's video
     * send_format names a codec, that codec. NOT_SUPPORTED otherwise. */
    xiaotai_link_codec_t codec;
    bool key_frame;                /* decodes without earlier frames; the first frame of a stream must be one */
    uint32_t timestamp_ms;         /* capture time on the sender's media clock */
    const uint8_t *data;           /* copied before the call returns */
    size_t size;
    /* Refuse with BUSY when the link already holds at least this many unsent
     * bytes, so video never delays audio. 0: only the stack's own limit. */
    uint32_t backlog_limit_bytes;
} xiaotai_link_video_frame_t;

/* [uplink] Sends one video frame on the profile's video send_stream. Results
 * as xiaotai_link_send_audio; FAIL when the RTC stack refused the frame (the
 * sender then resumes from a key frame; video failures never end the link).
 * May run concurrently with audio. */
xiaotai_err_t xiaotai_link_send_video(uint32_t generation, const xiaotai_link_video_frame_t *frame);

/* [uplink] Bytes accepted from this side and not yet sent (asks the RTC
 * stack). 0 for a dead generation. */
size_t xiaotai_link_backlog(uint32_t generation);

/* The media engine's uplink sink (xiaotai/media_format.h), implemented by this
 * component over xiaotai_link_can_send() ([rt]) and xiaotai_link_send_audio()
 * ([uplink]): the sink's generation is the link's generation. The media
 * engine calls send() only from its own uplink task, which exists in every
 * pipeline form, never from a task that reads PCM from an input part or
 * writes to an output part; the uplink task may itself read an input part
 * that delivers encoded packets from a queue of its own (see
 * xiaotai_media_sink_t). The transport adds no queue or task of its own.
 * send_stack_bytes is the stack one xiaotai_link_send_audio() uses with the
 * linked RTC package, a package fact measured on the bench; for a package not
 * measured yet it is a conservative placeholder, never 0 (so never the media
 * engine's smaller default). A static object, valid from the engine's init
 * on. [any] */
struct xiaotai_media_sink;
const struct xiaotai_media_sink *xiaotai_link_audio_sink(void);

/* ------------------------------------------------------------ video control */

/* [any] Asks the peer for a key frame on the profile's video receive_stream
 * (which must be 0..15). Not from a receiver. */
xiaotai_err_t xiaotai_link_request_key_frame(uint32_t generation);

/* [rt] True once after the peer asked this side for a key frame, or subscribed
 * to its video (a new viewer starts from a key frame). Requests that arrive
 * before the next call merge into one. */
bool xiaotai_link_take_key_frame_request(uint32_t generation);

/* [any] Lets the RTC stack steer the bitrate of this side's video within
 * 0 < min_bps < start_bps < max_bps. NOT_SUPPORTED when the stack or the peer
 * cannot; the sender then keeps its own rate. Not from a receiver. */
xiaotai_err_t xiaotai_link_set_video_bitrate(uint32_t generation, uint32_t min_bps, uint32_t start_bps,
                                             uint32_t max_bps);

/* [rt] True when the stack suggested a target since the last call; *bps is the
 * latest one. The transport never touches an encoder. */
bool xiaotai_link_take_video_bitrate(uint32_t generation, uint32_t *bps);

/* -------------------------------------------------------------- diagnostics */

typedef enum {
    XIAOTAI_LINK_STATE_NONE = 0,
    XIAOTAI_LINK_STATE_REQUESTED,  /* outbound, not open yet */
    XIAOTAI_LINK_STATE_OPEN,
} xiaotai_link_state_t;

/* Counters of one media kind; they only grow during a link. */
typedef struct {
    uint32_t sent;               /* frames handed to the RTC stack */
    uint32_t send_gated;         /* refused: neither subscribed nor permitted */
    uint32_t send_congested;     /* refused: backlog limit, or the stack's buffer is full */
    uint32_t send_failed;        /* the stack reported an error other than congestion */
    uint32_t received;           /* frames the receiver took */
    uint32_t receive_closed;     /* dropped: receiving not started */
    uint32_t receive_filtered;   /* dropped: stream or format outside the profile */
    uint32_t receive_refused;    /* the receiver returned false */
    uint32_t receive_overlapped; /* dropped: another frame of this kind was being delivered */
} xiaotai_link_media_stats_t;

typedef struct {
    xiaotai_link_kind_t kind;
    xiaotai_link_state_t state;
    bool audio_permitted, video_permitted;
    bool audio_subscribed, video_subscribed;   /* the peer subscribed to the current send stream */
    uint8_t audio_send_stream;                 /* differs from the profile after XIAOTAI_LINK_PEER_STREAM_FOLLOW */
    xiaotai_link_media_stats_t audio, video;
    uint32_t commands_sent, commands_send_failed;
    uint32_t commands_received, commands_dropped;
    uint32_t commands_early;                   /* received before the connect result, delivered after opened() */
    uint32_t command_largest;                  /* largest payload received, kept or dropped */
    uint32_t command_pool_peak;                /* most payload bytes of this link held in the pool at once */
    uint32_t peer_subscriptions_refused;       /* streams outside the profile, or no room to report them */
    uint32_t control_busy;                     /* control calls that gave up after XIAOTAI_LINK_CONTROL_WAIT_MS */
} xiaotai_link_stats_t;

/* [any] True for a live link, and for the link that ended last in its place
 * (state XIAOTAI_LINK_STATE_NONE) until another link takes that place, so the
 * counters of a finished session can still be read. False otherwise. */
bool xiaotai_link_stats(uint32_t generation, xiaotai_link_stats_t *stats);

#ifdef __cplusplus
}
#endif
