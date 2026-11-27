// SPDX-License-Identifier: MIT
#pragma once
/* Media engine: audio pipelines shared by every module and product.
 *
 * Modules declare what they need while they initialize (xiaotai_media_require);
 * the engine sizes its buffers, codecs and tasks once, at start, for the
 * largest need. A session (a conversation, a call, a room, a stream...) then
 * opens a pipeline described only by its network formats, whether both ends
 * talk at once, the burst it must absorb and where the uplink goes:
 *
 *   input part -> processor -> effects -> packets -> encoder -> session sink
 *   xiaotai_media_push -> downlink buffer -> decoder -> pacing -> mixer -> output part
 *
 * The processor is chosen by the board's part descriptors, codecs by the
 * formats (xiaotai/media_capture.h, xiaotai/media_codec.h). Beside sessions the
 * engine plays sounds (prompts, rings, guidance, borrowed files), records
 * clean microphone audio, keeps the pre-roll that bridges a wake word to its
 * conversation, and counts everything it drops.
 *
 * One session at a time; which one is the framework's foreground activity. The
 * engine holds only the hard rules that protect the audio path and privacy: a
 * muted microphone feeds nothing, an active session silences sounds, a
 * recording excludes both.
 *
 * Generations: every session call carries the caller's uint32_t generation
 * (0 is invalid). A call or packet for another generation is dropped and
 * counted, never applied.
 *
 * Tags: [init] during init, before the engine starts; [owner] the framework's
 * owner task; [any] any task; [rt] a real-time path (the transport's receive
 * callback, the engine's audio tasks): never blocks, allocates or logs;
 * [worker] a task that may block, never the owner task or a real-time task.
 * Nothing here may be called from an interrupt. [owner] and [any] calls take
 * one short internal lock and return without waiting for hardware; completion
 * and failure arrive as callbacks on the owner task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/media_downlink.h"
#include "xiaotai/media_format.h"
#include "xiaotai/port/err.h"
#include "xiaotai/sounds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- requirements */

/* How much delay the playout may trade for continuity. The pacer estimates
 * arrival jitter and moves its target between min_ms and max_ms; these numbers
 * describe the network path, not who uses it. All zero: XIAOTAI_MEDIA_LATENCY_BURSTY. */
typedef struct {
    uint16_t start_ms;      /* audio queued before the first sample plays */
    uint16_t base_ms;       /* target on a clean network */
    uint16_t min_ms;
    /* Highest target. The engine lowers it so that max_ms + surplus_ms fits
     * its downlink (xiaotai_media_info().downlink_ms); the session's
     * downlink_burst_ms plays no part in this. */
    uint16_t max_ms;
    uint16_t surplus_ms;    /* audio above the target tolerated before it is played out fast */
    uint8_t settle_s;       /* quiet seconds before the target steps down */
    uint8_t slow_permille;  /* strongest slow-down while below the target; 0: 12 */
    /* A one-second window counts as quiet (the target may step down after
     * settle_s of them) only while arrival jitter stays at or below
     * steady_jitter_ms and the largest timing error at or below
     * steady_peak_ms; a larger timing error is network pressure and raises the
     * target. step_down_ms is one step down. 0: 8, 10 and 10. A relayed path
     * with recoverable holes needs wider bounds, or nearly every window reads
     * as pressure and the target only rises. */
    uint8_t steady_jitter_ms;
    uint8_t steady_peak_ms;
    uint8_t step_down_ms;
} xiaotai_media_latency_t;

/* A direct path where delay matters most. */
#define XIAOTAI_MEDIA_LATENCY_LOW {20U, 40U, 20U, 100U, 160U, 3U, 12U, 0U, 0U, 0U}
/* A relayed path with recoverable holes of a few hundred milliseconds. */
#define XIAOTAI_MEDIA_LATENCY_RELAYED {120U, 140U, 120U, 560U, 320U, 3U, 12U, 16U, 40U, 20U}
/* A sender that delivers speech in bursts (a synthesized answer). */
#define XIAOTAI_MEDIA_LATENCY_BURSTY {120U, 160U, 120U, 320U, 320U, 6U, 12U, 0U, 0U, 0U}
/* LOW with the stronger slow-down (30 per mille) of a voice-call path: a queue
 * running below its target is stretched harder before it underruns. */
#define XIAOTAI_MEDIA_LATENCY_VOIP {20U, 40U, 20U, 100U, 160U, 3U, 30U, 0U, 0U, 0U}

typedef struct {
    /* Audio the downlink must absorb at once (AI talk: 1000). */
    uint32_t downlink_burst_ms;
    /* Packet duration of a declared format whose own packet_ms is 0. */
    uint16_t packet_ms;
    /* Bounds of a framed downlink encoding (Opus), which the engine cannot
     * cut into nominal pieces; at least one is required for it. Ignored for
     * G.711 and PCM: those are stored as nominal pieces
     * (xiaotai/media_format.h), whatever size the peer sends.
     * max_packet_bytes: largest packet, at most XIAOTAI_MEDIA_PACKET_BYTES_MAX;
     * a larger one is dropped (rx_invalid). 0: what 120 ms
     * (XIAOTAI_MEDIA_PACKET_MS_MAX) at max_bitrate_bps carries, at most
     * XIAOTAI_MEDIA_PACKET_BYTES_MAX.
     * max_bitrate_bps: the peer encoder's maximum bitrate, which no stretch of
     * held audio exceeds on average. Speech packets stay far below the
     * largest packet, so sizing by the rate keeps the downlink near what the
     * audio needs; 0: unknown, the downlink is sized as packets x
     * max_packet_bytes. */
    uint16_t max_packet_bytes;
    uint32_t max_bitrate_bps;
    /* Formats the module's sessions will send and receive (rate_hz 0: none).
     * The engine opens a codec for each distinct format at start and sizes
     * its task stacks for them; a session may only use declared formats. A
     * module with several formats declares one requirement per format. A
     * format this build cannot serve (no registered codec, no conversion to
     * the board's rate) is not an error: it stays unavailable
     * (xiaotai_media_can_send, xiaotai_media_can_receive), and a requirement
     * whose downlink format is unavailable does not count when the downlink
     * is sized, so a module may declare every format it knows on every board. */
    xiaotai_media_format_t uplink;
    xiaotai_media_format_t downlink;
    /* Clean microphone audio the pre-roll must hold between a wake word and
     * the session it starts (AI talk: 20000). 0: no pre-roll. Needs PCM
     * capture; ignored on boards without it. */
    uint32_t preroll_ms;
    /* The pacing profile the module's sessions use with this downlink; all
     * zero: XIAOTAI_MEDIA_LATENCY_BURSTY. */
    xiaotai_media_latency_t latency;
} xiaotai_media_requirement_t;

/* [init] Declared by modules before the engine starts.
 *
 * At start the engine sizes the downlink once, for the largest single need:
 * every requirement whose downlink format this build can receive asks for
 * max(downlink_burst_ms + headroom, latency.max_ms + latency.surplus_ms) of
 * audio in packets of its format's duration. G.711 and PCM are stored as
 * nominal packets plus a small header (XIAOTAI_DOWNLINK_BYTES). A framed
 * encoding takes the smaller of packets x max_packet_bytes and, when
 * max_bitrate_bps is given, the audio at that rate plus a header per packet
 * and one largest packet (XIAOTAI_DOWNLINK_BYTES_AT_RATE). The buffer takes
 * the largest of these byte counts, never the longest burst times the largest
 * packet. headroom is the tuning "downlink.headroom_ms" (default
 * XIAOTAI_DOWNLINK_HEADROOM_MS); a board without external RAM may lower it.
 * The optional tuning "downlink.capacity_packets" (zero means automatic)
 * replaces the reservation with an explicit count of at least two queued
 * entries. "downlink.max_queued_ms" (zero means no additional limit) bounds
 * the total audio duration admitted without increasing the byte reservation.
 * Neither setting alters the session's declared wire burst or codec.
 * Pre-roll needs and formats accumulate the same way.
 *
 * A requirement without formats (only preroll_ms) is valid and packet_ms is
 * not checked for it. XIAOTAI_ERR_INVALID_ARG: NULL, a format with a rate but
 * no packet duration (its own or packet_ms) or whose encoding is not an audio
 * codec (XIAOTAI_CODEC_IS_AUDIO; an audio codec without a registered codec is
 * merely unavailable, see above), max_packet_bytes above
 * XIAOTAI_MEDIA_PACKET_BYTES_MAX, or a framed downlink format with neither
 * max_packet_bytes nor max_bitrate_bps (on every board, so a module that
 * forgets the bound fails in host tests rather than costing a board without
 * external RAM a downlink of 1500-byte slots); XIAOTAI_ERR_NO_MEM: more
 * requirements than the engine keeps; XIAOTAI_ERR_INVALID_STATE: the engine
 * has started. */
xiaotai_err_t xiaotai_media_require(const xiaotai_media_requirement_t *requirement);

/* The engine's downlink, sized at start; NULL before. Owned by the engine:
 * modules use xiaotai_media_push, diagnostics read its statistics. */
xiaotai_downlink_t *xiaotai_media_downlink(void);

/* ------------------------------------------------------------------- tuning */

/* Acoustic and scheduling tuning by name: what differs between products
 * without being a part or a format (gain after echo cancellation, the wake
 * threshold, task priorities and cores, downlink headroom). Facts of the board
 * (speaker-to-microphone coupling, volume law, gain range) are part info
 * (xiaotai/hal.h), never tuning. The product configuration supplies one table;
 * processors, effects and the engine read their own names. A new processor
 * brings new names, not a new field. Names are documented beside what reads
 * them and listed in the media specification. */
typedef struct {
    const char *name;   /* "capture.agc_db", "uplink.boost_cdb", "downlink.headroom_ms", ... */
    int32_t value;
} xiaotai_media_param_t;

/* [init] May also be called before the framework starts. The table is not
 * copied: it and its names have static lifetime. A later table overrides an
 * earlier one name by name. "task.capture.compact_stack" (0/1) opts the
 * folded worker into the 3584-byte stack on systems without external RAM;
 * larger plugin stack declarations keep the regular budget. A larger
 * "task.capture.stack_bytes" still overrides the computed stack.
 * XIAOTAI_ERR_INVALID_ARG: NULL with a count or an invalid bounded value;
 * XIAOTAI_ERR_NO_MEM: too many tables; XIAOTAI_ERR_INVALID_STATE: the engine
 * has started. */
xiaotai_err_t xiaotai_media_tune(const xiaotai_media_param_t *params, size_t count);
/* [any] The value for name, or fallback when no table has it. */
int32_t xiaotai_media_param(const char *name, int32_t fallback);

/* [owner] Temporarily change a configured echo gate, starting with the next
 * capture block. No session restart or persistent setting. When the product
 * did not enable the gate, setting it returns NOT_SUPPORTED. */
xiaotai_err_t xiaotai_media_echo_gate_set(bool enabled);
/* [owner] The configured gate's current switch, false when unavailable. */
bool xiaotai_media_echo_gate_enabled(void);

/* --------------------------------------------------------------------- info */

/* What the engine found and built at start; all zero before. */
typedef struct {
    bool capture, playback;         /* the board has an audio input, an audio output */
    bool capture_pcm;               /* clean PCM exists: taps, pre-roll and recording work */
    bool cancels_echo;              /* a reference-based canceller runs (false: echo gate or none) */
    bool mixer;                     /* XIAOTAI_PLAY_OVER_SESSION is honoured */
    xiaotai_codec_t capture_encoding; /* XIAOTAI_CODEC_PCM16, or what an encoded input delivers; NONE: no input */
    uint32_t capture_rate_hz;       /* rate of clean capture audio: what a recording has */
    uint32_t playback_rate_hz;
    const char *processor;          /* name of the chosen processor; "" when none */
    /* Audio the downlink holds for the largest counted requirement (its
     * format's packets); sessions' max_ms + surplus_ms is kept within it. */
    uint32_t downlink_ms;
    uint32_t preroll_ms;            /* pre-roll capacity; 0: none */
} xiaotai_media_info_t;

/* [any] */
void xiaotai_media_info(xiaotai_media_info_t *info);
/* [any] After start: whether a session may send or receive this format on this
 * board (a codec was opened for it and the engine can convert between its rate
 * and the board's). Modules use it to choose what they negotiate. */
bool xiaotai_media_can_send(const xiaotai_media_format_t *format);
bool xiaotai_media_can_receive(const xiaotai_media_format_t *format);

/* ----------------------------------------------------------------- sessions */

enum {
    /* Open armed: the downlink accepts and keeps this generation's packets,
     * but nothing plays, nothing is sent and sounds keep playing until
     * xiaotai_media_activate. For a peer that answers before the session is
     * confirmed. */
    XIAOTAI_MEDIA_HOLD = 1u << 0,
    /* A person listens at the far end: attenuate the uplink between words, so
     * steady room noise neither plays there nor keeps a half-duplex peer's
     * microphone closed. */
    XIAOTAI_MEDIA_GATE_IDLE_NOISE = 1u << 1,
    /* Request the optional bounded arrival diagnostic for this generation.
     * No effect on packet admission, wire format or the playout policy. */
    XIAOTAI_MEDIA_SESSION_TRACE_ARRIVALS = 1u << 2,
};

typedef enum {
    /* The pipeline runs: the output is powered and capture flows. */
    XIAOTAI_MEDIA_EVENT_STARTED = 0,
    /* The drain (xiaotai_media_drain, or the one that ends
     * xiaotai_media_finish) is over: error XIAOTAI_OK when everything held has
     * left the speaker, XIAOTAI_ERR_TIMEOUT when the time ran out first. */
    XIAOTAI_MEDIA_EVENT_DRAINED,
    /* The pipeline stopped by itself (a part, codec or processor failed; error
     * says which result). Nothing more is played or sent; close the session. */
    XIAOTAI_MEDIA_EVENT_FAILED,
    /* The pre-roll bound to the session was lost after activation (a capture
     * gap). The backlog is dropped and counted; the session continues live. */
    XIAOTAI_MEDIA_EVENT_PREROLL_LOST,
} xiaotai_media_event_t;

/* [owner] Never called after xiaotai_media_close returned for the generation.
 *
 * The audio tasks hand these to the owner task with xiaotai_run, which never
 * waits. When the owner's queue is full the task keeps one pending event per
 * kind, merges a later event of the same kind and generation into it, and
 * posts it again at its next block until accepted (counted in
 * owner_post_retries), so an event is neither lost nor doubled. Completions
 * of sounds and recordings (done()) and pre-roll losses follow the same rule. */
typedef void (*xiaotai_media_event_fn_t)(uint32_t generation, xiaotai_media_event_t event, xiaotai_err_t error,
                                         void *ctx);

typedef struct {
    uint32_t generation;            /* the caller's, not 0, not the previous session's */
    xiaotai_media_format_t uplink;     /* rate_hz 0: the session sends nothing */
    xiaotai_media_format_t downlink;   /* rate_hz 0: the session receives nothing */
    /* Burst this session's downlink delivers at once; at most what was
     * declared with xiaotai_media_require. */
    uint32_t downlink_burst_ms;
    /* true: both ends talk at once, the uplink is open from activation.
     * false: push to talk, the uplink is closed until xiaotai_media_uplink_gate
     * opens it. The downlink plays in both cases. */
    bool duplex;
    /* Where uplink packets go; required with an uplink. Static lifetime. */
    const xiaotai_media_sink_t *sink;
    xiaotai_media_latency_t latency;
    uint32_t flags;                 /* XIAOTAI_MEDIA_* */
    /* A token from xiaotai_media_preroll_hold: at activation the uplink starts
     * with the audio captured since the wake word, then continues live. 0: none. */
    uint32_t preroll_token;
    xiaotai_media_event_fn_t event; /* may be NULL */
    void *ctx;
} xiaotai_media_session_t;

/* [owner] Opens the session's pipeline; the struct is copied. Without
 * XIAOTAI_MEDIA_HOLD this is open and activate in one call. Opening ends a
 * recording, drops any pre-roll token the session does not name, and (when it
 * activates) stops sounds that are not played over sessions.
 * XIAOTAI_ERR_INVALID_ARG: generation 0, neither direction, an uplink without
 * a sink, a malformed format; XIAOTAI_ERR_INVALID_STATE: the engine has not
 * started or started for a provisioning boot (see the lifecycle below),
 * another session is open, or preroll_token is no longer valid or the
 * microphone is muted with one (the token is released);
 * XIAOTAI_ERR_NOT_SUPPORTED: no part, declared codec or rate conversion for a
 * direction, or the uplink sink is not supported by the selected executor;
 * XIAOTAI_ERR_INVALID_SIZE: downlink_burst_ms above the declared
 * requirement or an uplink sink above the configured stack budget. On failure
 * nothing is open. */
xiaotai_err_t xiaotai_media_open(const xiaotai_media_session_t *session);

/* [owner] Starts a session opened with XIAOTAI_MEDIA_HOLD: stops sounds, powers
 * the output, plays what the downlink kept, opens the uplink (duplex) and binds
 * the pre-roll. The hardware work happens on the engine's tasks:
 * XIAOTAI_MEDIA_EVENT_STARTED or _FAILED follows. XIAOTAI_ERR_INVALID_STATE:
 * not the open generation, already active, or the pre-roll token is no longer
 * valid (the session stays armed; close it). */
xiaotai_err_t xiaotai_media_activate(uint32_t generation);

/* [owner] Ends the session: the uplink stops, held downlink audio is dropped,
 * the output is powered down unless a sound plays. After it returns the sink
 * is called at most for one packet already being sent, and no event follows.
 * A pre-roll token the session bound (it was activated) ends with it. A token
 * the session only named (closed while armed) stays held and unbound: its
 * holder names it in the next session or gives it up with
 * xiaotai_media_preroll_release; otherwise it expires and lost() follows.
 * Another generation, or no session: counted, nothing happens. */
void xiaotai_media_close(uint32_t generation);

/* [rt] One producer at a time (the transport's receive callback). Copies one
 * downlink packet into the buffer; the packet's encoding and rate must be the
 * session's downlink or another declared, receivable format. False when
 * dropped, each reason counted: not the open generation or draining (rx_stale),
 * malformed or not receivable (rx_invalid), speaker muted (rx_blocked), buffer
 * full (rx_overflow). */
bool xiaotai_media_push(uint32_t generation, const xiaotai_media_packet_t *packet);

/* Longest a drain can take: a full downlink plus the output's own buffer. */
#define XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS 7000U

/* [owner] The far end finished: stop the uplink, refuse further downlink
 * packets and play out what is held. XIAOTAI_MEDIA_EVENT_DRAINED follows
 * within timeout_ms; the session stays open until closed. It may cut a
 * xiaotai_media_finish short.
 * XIAOTAI_ERR_INVALID_STATE: not the active generation, or already draining. */
xiaotai_err_t xiaotai_media_drain(uint32_t generation, uint32_t timeout_ms);

/* What may still come from a far end that says it is done. */
typedef struct {
    uint32_t wait_ms;    /* go on receiving at least this long, from the call on */
    uint32_t quiet_ms;   /* and until no packet has come for this long */
    uint32_t limit_ms;   /* but not longer than this, from the call on */
} xiaotai_media_tail_t;

/* [owner] The far end says it is done while its last sound may still be on the
 * way: stop the uplink, go on receiving and playing as before, and drain (at
 * most XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS) once everything that came has been heard
 * and the tail is over. XIAOTAI_MEDIA_EVENT_DRAINED follows as for
 * xiaotai_media_drain.
 * XIAOTAI_ERR_INVALID_ARG: no tail. XIAOTAI_ERR_INVALID_STATE: not the active
 * generation, or already finishing or draining. */
xiaotai_err_t xiaotai_media_finish(uint32_t generation, const xiaotai_media_tail_t *tail);

/* [owner] Drop the downlink audio held but not yet played (the far end was
 * interrupted); capture and uplink are untouched and new packets are accepted.
 * XIAOTAI_ERR_INVALID_STATE: not the open generation. */
xiaotai_err_t xiaotai_media_flush(uint32_t generation);

/* [any] Opens or closes the uplink of a push-to-talk session. Opening needs the
 * active generation and an unmuted microphone (XIAOTAI_ERR_INVALID_STATE
 * otherwise). Closing always succeeds, also from a task whose queue is full:
 * audio captured up to the call is still sent for at most tail_ms, later
 * audio never (tail_ms 0: packets not yet handed to the sink are dropped). */
xiaotai_err_t xiaotai_media_uplink_gate(uint32_t generation, bool open, uint32_t tail_ms);

/* Longest test tone one call may ask for. */
#define XIAOTAI_MEDIA_TONE_MS_MAX 5000U

/* [any] Diagnostics: replaces the next duration_ms of the session's uplink
 * audio with a 400 Hz test tone. The tone is inserted after echo control and
 * every gate, so it reaches the peer at full level whatever the microphone
 * hears; packets carrying it are counted in tx_injected. A later call restarts
 * the duration. XIAOTAI_ERR_INVALID_STATE: not the active generation, or its
 * uplink is closed or muted; XIAOTAI_ERR_INVALID_ARG: 0 or more than
 * XIAOTAI_MEDIA_TONE_MS_MAX; XIAOTAI_ERR_NOT_SUPPORTED: the session has no
 * uplink, or the capture is encoded and no encoder for the uplink format was
 * declared. */
xiaotai_err_t xiaotai_media_uplink_tone(uint32_t generation, uint32_t duration_ms);

/* Framework event (xiaotai/framework.h) emitted when the speaker became free
 * for sounds again: a session closed, a recording ended, the speaker was
 * unmuted, or a play that made a play request fail with XIAOTAI_ERR_BUSY
 * ended. No arguments. Whoever queues sounds for a busy speaker subscribes
 * instead of polling. */
#define XIAOTAI_EVENT_MEDIA_OUTPUT_FREE "media.output_free"

/* Initial audio part open completed on the media worker, successfully or
 * not. No payload: read the sticky startup result in xiaotai_media_status(),
 * also on system.started in case the result was already available. */
#define XIAOTAI_EVENT_MEDIA_STARTUP "media.startup"

/* Framework event emitted when the published voice level of the clean
 * microphone audio changes (xiaotai_media_status().voice_level). args.value
 * is the new level, 0..3. The level only changes after it held for 300 ms, so
 * the event comes at most a few times a second; it is emitted also while a
 * session is active or the microphone is muted (the level is measured, never
 * sent). Whoever follows
 * speech (a face, an idle timer) subscribes instead of polling. */
#define XIAOTAI_EVENT_MEDIA_VOICE "media.voice"

/* Framework event emitted when a volume, speaker-mute, input-gain or output
 * power request was accepted but the hardware refused it later. args.value is
 * the error (xiaotai_err_t), args.text "volume", "mute", "gain" or "power".
 * The applied values in xiaotai_media_status stay at the last success, so a
 * setting can be taken back to them. */
#define XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR "media.output_error"

/* ----------------------------------------------------------------- controls */

/* [owner] Privacy switch for the whole capture path. Muted: no uplink (queued
 * packets are dropped), taps closed, pre-roll cleared and its token released,
 * a recording ends, a push-to-talk gate closes. Echo control keeps running so
 * unmuting is clean. The value is not stored here: settings own persistence.
 * During product initialization the owner can stage the initial mute before
 * starting the media engine; no capture lane is released with an older gate.
 * XIAOTAI_ERR_NOT_SUPPORTED without an audio input. */
xiaotai_err_t xiaotai_media_set_mic_muted(bool muted);

/* [owner] Speaker volume (0..100), mute and input gain are requests: recorded
 * at once and applied by the task that owns the part within one block period,
 * also while nothing plays, so the owner task never waits for a codec. While
 * the idle close has the part closed, the request is applied right after the
 * next open. The applied values and the last failure are in
 * xiaotai_media_status; a failure also emits XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR.
 * Requests made during product initialization are staged for the first open.
 * Muting drops held downlink audio and stops sounds; packets arriving while
 * muted are dropped (rx_blocked).
 * XIAOTAI_ERR_INVALID_ARG: percent above 100, or a gain the part refuses;
 * XIAOTAI_ERR_NOT_SUPPORTED: no output, or the input part has no gain control. */
xiaotai_err_t xiaotai_media_set_volume(uint8_t percent);
xiaotai_err_t xiaotai_media_set_speaker_muted(bool muted);
xiaotai_err_t xiaotai_media_set_input_gain(int8_t gain_db);

/* ------------------------------------------------------------------- sounds */

typedef enum {
    XIAOTAI_PLAY_CUE = 0,   /* short feedback: skipped when anything else plays */
    XIAOTAI_PLAY_NORMAL,    /* prompts, rings, guidance, files: the newest replaces the current */
    XIAOTAI_PLAY_URGENT,    /* not replaced by the two above */
} xiaotai_play_priority_t;

enum {
    /* Mix over an active session instead of being refused or stopped by it.
     * XIAOTAI_ERR_NOT_SUPPORTED where xiaotai_media_info reports no mixer. */
    XIAOTAI_PLAY_OVER_SESSION = 1u << 0,
    /* A new request with this flag gets BUSY instead of replacing an active
     * equal-priority play. It does not protect this play from later requests. */
    XIAOTAI_PLAY_REFUSE_EQUAL = 1u << 1,
};

/* Pieces of one play: a guidance followed by a code, with room to spare. */
#define XIAOTAI_MEDIA_PLAY_CLIPS_MAX 12U
/* repeat value that loops until stopped. */
#define XIAOTAI_PLAY_FOREVER UINT16_MAX

/* One piece of a play: a registered sound by name, or data the caller lends. */
typedef struct {
    const char *name;               /* looked up in xiaotai/sounds.h when sound is NULL */
    /* Borrowed: the descriptor and its data stay valid and unchanged until
     * done() is called; the engine never frees or writes them. */
    const xiaotai_sound_t *sound;
} xiaotai_media_clip_t;

/* [owner] result: XIAOTAI_OK played to the end (the output's own buffer
 * included); XIAOTAI_ERR_INVALID_STATE stopped, replaced, or cut by a session,
 * a recording or the speaker mute; XIAOTAI_ERR_TIMEOUT a streamed sound whose
 * writer stopped; XIAOTAI_FAIL decoding or the output failed. Called exactly
 * once per accepted play; borrowed data is free afterwards. */
typedef void (*xiaotai_media_play_fn_t)(uint32_t id, xiaotai_err_t result, void *ctx);

typedef struct {
    /* Played back to back without a gap (a guidance followed by six digits). */
    xiaotai_media_clip_t clips[XIAOTAI_MEDIA_PLAY_CLIPS_MAX];
    uint8_t clip_count;             /* 1..XIAOTAI_MEDIA_PLAY_CLIPS_MAX */
    xiaotai_play_priority_t priority;
    uint32_t flags;                 /* XIAOTAI_PLAY_* */
    /* 0: the sound belongs to no session. Otherwise the session it belongs to:
     * accepted only while that generation is the open session, and cut when
     * that session closes, so a sound for a session that ended is never heard.
     * (An armed session lets it play; activation cuts it unless it is played
     * over the session.) */
    uint32_t generation;
    uint16_t repeat;                /* extra passes after the first; XIAOTAI_PLAY_FOREVER */
    uint16_t gap_ms;                /* silence between passes */
    xiaotai_media_play_fn_t done;   /* may be NULL */
    void *ctx;
} xiaotai_media_play_t;

/* [any] Plays one sound at a time on the engine's playout task; the request
 * is copied (names must have static lifetime) and the call never waits for
 * the speaker. A play of higher priority, or of equal priority without
 * XIAOTAI_PLAY_REFUSE_EQUAL, replaces the current one,
 * whose done() reports XIAOTAI_ERR_INVALID_STATE. *id (may be NULL) identifies
 * the play, never 0.
 * XIAOTAI_ERR_INVALID_ARG: no clips, too many, a clip without name and sound;
 * XIAOTAI_ERR_NOT_FOUND: no pack has a name; XIAOTAI_ERR_NOT_SUPPORTED: no
 * output, no decoder for a clip's format, or its container is damaged;
 * XIAOTAI_ERR_INVALID_STATE: not started, the speaker is muted, or generation
 * is not the open session (counted in stale_calls);
 * XIAOTAI_ERR_BUSY: a session is active (without XIAOTAI_PLAY_OVER_SESSION),
 * a recording runs, a sound of higher priority plays, or a sound of equal
 * priority plays and the new request sets XIAOTAI_PLAY_REFUSE_EQUAL.
 * On failure done() is not called. */
xiaotai_err_t xiaotai_media_play_clips(const xiaotai_media_play_t *request, uint32_t *id);

/* [any] One registered sound, once, no completion report. Same results. */
xiaotai_err_t xiaotai_media_play(const char *sound, xiaotai_play_priority_t priority);

/* [any] Stops the play with this id (0: whatever plays). The play stops
 * counting for admission when this returns, so a play requested right after
 * (a ring after a stopped announcement) is accepted whatever the stopped
 * play's priority; its audio ends at the next block boundary and its done()
 * follows with XIAOTAI_ERR_INVALID_STATE. An id that already ended is
 * ignored. */
void xiaotai_media_stop(uint32_t id);

/* A sound the caller produces while it plays: speech fetched from the network
 * on a board that cannot hold it whole. Mono, in any prepared audio format.
 * XIAOTAI_CODEC_PCM16 and XIAOTAI_CODEC_G711A are built in. */
typedef struct {
    xiaotai_codec_t encoding;
    uint32_t rate_hz;                   /* 8000..48000 */
    xiaotai_play_priority_t priority;
    uint32_t flags;                     /* XIAOTAI_PLAY_* */
    uint32_t generation;                /* as xiaotai_media_play_t.generation */
    xiaotai_media_play_fn_t done;       /* may be NULL */
    void *ctx;
} xiaotai_media_stream_t;

/* Audio the engine buffers between the writer and the speaker. */
#define XIAOTAI_MEDIA_STREAM_BUFFER_MS 200U
/* A stream that is neither written nor finished for this long ends with
 * XIAOTAI_ERR_TIMEOUT. */
#define XIAOTAI_MEDIA_STREAM_IDLE_MS 5000U

/* [init] Declare an exact stream shape (codec and rate_hz, 8000..48000).
 * The engine asks the registered decoder for mono with a nominal 20 ms packet;
 * actual packet durations are measured by the decoder. The declaration is
 * copied and duplicates are free. Native output-rate formats and matching
 * registered clips are already prepared; built-in PCM16/G.711 A-law need
 * no declaration and do not use a slot. No decoder is opened by this call.
 * If a module declares a format this build cannot serve, the engine can
 * still start, and play_stream returns XIAOTAI_ERR_NOT_SUPPORTED.
 * XIAOTAI_ERR_INVALID_ARG: not an audio codec or rate outside the range;
 * XIAOTAI_ERR_NO_MEM: more than eight distinct stateful declarations;
 * XIAOTAI_ERR_INVALID_STATE: the engine has started. Prepared sound formats
 * share an eight-decoder startup budget; exhaustion fails audio startup. */
xiaotai_err_t xiaotai_media_require_stream_format(xiaotai_codec_t encoding, uint32_t rate_hz);

/* [any] Opens a streamed sound: admission, replacement and results as
 * xiaotai_media_play_clips (XIAOTAI_ERR_INVALID_ARG also for a rate outside
 * the range above; XIAOTAI_ERR_NOT_SUPPORTED if the rate can't be converted,
 * there is no matching prepared decoder, or the encoding is NONE or video).
 * Nothing is heard until data is written; if the writer falls behind, the
 * output is silence, never repeated audio. The buffer holds
 * XIAOTAI_MEDIA_STREAM_BUFFER_MS of audio and never more bytes than that much
 * PCM16 at rate_hz. */
xiaotai_err_t xiaotai_media_play_stream(const xiaotai_media_stream_t *request, uint32_t *id);

/* [worker] Appends audio; data is copied. G.711 and PCM16: whole samples. A
 * framed encoding (Opus): exactly one packet per call, at most
 * XIAOTAI_MEDIA_PACKET_BYTES_MAX bytes and XIAOTAI_MEDIA_PACKET_MS_MAX of
 * audio. While the stream buffer is full the call waits up to timeout_ms, so
 * the speaker paces the writer. XIAOTAI_ERR_INVALID_STATE: the play has ended
 * (stopped, cut, failed): stop writing; XIAOTAI_ERR_TIMEOUT: still full,
 * nothing was taken; XIAOTAI_ERR_INVALID_SIZE: not whole samples, or not one
 * packet the decoder can time; XIAOTAI_ERR_INVALID_ARG: NULL or 0 bytes. */
xiaotai_err_t xiaotai_media_play_write(uint32_t id, const void *data, size_t size, uint32_t timeout_ms);

/* [any] No more data follows: what is buffered plays out and done() reports
 * XIAOTAI_OK. XIAOTAI_ERR_INVALID_STATE: the play has ended. */
xiaotai_err_t xiaotai_media_play_finish(uint32_t id);

/* ---------------------------------------------------------------- recording */

#define XIAOTAI_MEDIA_RECORD_SECONDS_MAX 60U

/* [owner] result: XIAOTAI_OK the buffer is full; XIAOTAI_ERR_INVALID_STATE
 * stopped, or ended by a session or the microphone mute; XIAOTAI_FAIL the
 * capture had a gap or failed, the recording is unusable. samples is what the
 * buffer holds. Called exactly once; the buffer is the caller's again. */
typedef void (*xiaotai_media_record_fn_t)(uint32_t id, xiaotai_err_t result, uint32_t samples, void *ctx);
/* [owner] Latest whole-second boundary of valid samples while recording.
 * Intermediate updates may coalesce; done remains the only terminal callback. */
typedef void (*xiaotai_media_record_progress_fn_t)(uint32_t id, uint32_t samples, void *ctx);

typedef struct {
    /* The caller's buffer (allocate it as XIAOTAI_MEM_BULK), lent until done(). */
    int16_t *pcm;
    uint32_t samples;               /* samples to record: the buffer's capacity */
    xiaotai_media_record_fn_t done; /* required */
    void *ctx;
    xiaotai_media_record_progress_fn_t progress; /* optional */
    /* 0: pcm holds all `samples`. Otherwise pcm is a ring of this many
     * samples (at most `samples`): sample n lands at pcm[n % ring_samples],
     * and whoever takes them away (to flash, say) says how far it got with
     * xiaotai_media_record_taken(). A sample that would land on one not yet
     * taken ends the recording with XIAOTAI_FAIL. */
    uint32_t ring_samples;
} xiaotai_media_record_t;

/* [owner] Records clean mono PCM (after the processor and capture effects,
 * before uplink effects) at xiaotai_media_info().capture_rate_hz, starting
 * with audio captured after this call. No session is open while it runs, so a
 * processor that attenuates only during sessions (an echo gate) passes the
 * microphone through. Taps close and the pre-roll stops while it runs. XIAOTAI_ERR_NOT_SUPPORTED: no PCM capture; XIAOTAI_ERR_INVALID_ARG:
 * NULL, 0 samples or more than XIAOTAI_MEDIA_RECORD_SECONDS_MAX seconds;
 * XIAOTAI_ERR_INVALID_STATE: not started, microphone muted, or a pre-roll
 * token is held; XIAOTAI_ERR_BUSY: a session is open, a sound plays, or a
 * recording already runs. */
xiaotai_err_t xiaotai_media_record(const xiaotai_media_record_t *request, uint32_t *id);

/* [owner] done() follows with XIAOTAI_ERR_INVALID_STATE and the samples so far. */
void xiaotai_media_record_stop(uint32_t id);
/* [any] A ring recording (ring_samples): the first `samples` samples are
 * taken, their places may be filled again. Ignored for another recording,
 * and when it goes back or past what was recorded. */
void xiaotai_media_record_taken(uint32_t id, uint32_t samples);

/* ----------------------------------------------------------------- pre-roll */

/* While a tap keeps the idle capture running, the engine keeps the last
 * moments of clean audio. A wake word turns that history into a token: from
 * then on everything said is kept, up to the declared preroll_ms, until a
 * session names the token and sends it ahead of live audio.
 *
 * A held, valid token keeps capture and pre-roll appending running by itself,
 * whatever taps are enabled and whatever power.idle_close_ms says: the wake
 * tap closes as soon as the conversation's activity begins, while the session
 * that will bind the token opens only after the token fetch and the link
 * connect, and the speech in between must stay contiguous. */
typedef struct {
    int64_t wake_us;            /* capture time of the wake phrase's end (xiaotai_time_us clock) */
    uint32_t keep_before_ms;    /* audio kept from before wake_us (AI talk: 800) */
    uint32_t ttl_ms;            /* an unbound token expires after this (AI talk: 19000) */
    /* [owner] A held token that no session has bound became invalid: a capture
     * gap, the buffer filled, it expired, or the microphone was muted. Not
     * called for xiaotai_media_preroll_release or once bound. May be NULL. */
    void (*lost)(uint32_t token, void *ctx);
    void *ctx;
} xiaotai_media_preroll_t;

/* [owner] *token is never 0 on success. XIAOTAI_ERR_NOT_SUPPORTED: no pre-roll
 * was declared or built (none in a provisioning boot), or the board has no
 * PCM capture; XIAOTAI_ERR_INVALID_STATE:
 * microphone muted, a session active, a recording running, a token already
 * held, no contiguous history, or wake_us is in the future, older than one
 * second, or later than the newest captured audio; XIAOTAI_ERR_TIMEOUT: the
 * capture task held the buffer too long, try again. */
xiaotai_err_t xiaotai_media_preroll_hold(const xiaotai_media_preroll_t *request, uint32_t *token);

/* [owner] Gives up a token no session has bound; anything else is ignored. */
void xiaotai_media_preroll_release(uint32_t token);

/* ------------------------------------------------------- status, statistics */

/* A snapshot; fields are read one by one, not as one transaction. */
typedef struct {
    bool ready;                     /* started and the audio parts are open */
    bool startup_complete;          /* initial open or an earlier startup stage finished */
    xiaotai_err_t startup_error;     /* valid when startup_complete; OK on success */
    uint32_t generation;            /* open session; 0: none */
    bool active;                    /* activated (not merely held) */
    bool draining;                  /* finishing or draining: the uplink stopped, the rest is played */
    bool uplink_open;               /* the session's uplink gate */
    bool mic_muted;
    bool speaker_muted, speaker_muted_applied;
    uint8_t volume_percent, volume_applied;
    int8_t input_gain_db;
    xiaotai_err_t output_error;     /* last failure applying volume, mute, gain or output power; XIAOTAI_OK */
    uint8_t voice_level;            /* 0..3: smoothed level of the clean microphone audio */
    bool voice_active;              /* voice_level >= 2 */
    uint32_t playing;               /* id of the sound being played; 0: none */
    uint32_t recording;             /* id of the running recording; 0: none */
    uint32_t recorded_samples;      /* samples the running recording holds so far */
    uint32_t preroll_token;         /* held token; 0: none */
    bool preroll_bound;             /* a session took it and is replaying */
    uint32_t preroll_ms;            /* audio the pre-roll holds now */
} xiaotai_media_status_t;

/* Counters. Session counters restart at xiaotai_media_open and stay readable
 * after close; the others count since boot. */
typedef struct {
    /* Downlink, this session (packets). rx_dropped is the total of the reasons after it. */
    uint32_t rx_received, rx_played;
    uint32_t rx_dropped;
    uint32_t rx_stale, rx_invalid, rx_blocked, rx_overflow, rx_decode_failed, rx_flushed;
    uint32_t rx_underruns;          /* playout ran dry while the session was active */
    uint32_t rx_concealed_ms;       /* sound made up for packets that were lost or late */
    uint32_t rx_before_active;      /* packets kept while the session was armed */
    uint32_t rx_pcm_waits;          /* times a packet stayed encoded: the PCM stash had no room */
    /* Largest packet xiaotai_media_push received for this session, in bytes as
     * the peer sent it (before G.711 and PCM are cut into nominal pieces),
     * whatever its format and whether or not it was kept. A requirement's
     * max_packet_bytes is tuned against it on the bench. */
    uint32_t rx_packet_max_bytes;
    /* Downlink arrival, this session: gaps above 60 ms, bursts closer than
     * 5 ms, the largest gap, and sender timestamps that repeat, go back or
     * jump ahead of the arrival time by more than one packet. */
    uint32_t rx_gaps, rx_bursts, rx_gap_max_ms;
    uint32_t rx_ts_repeats, rx_ts_backwards, rx_ts_jumps;
    /* Uplink, this session (packets). tx_dropped is the total of the reasons after it. */
    uint32_t tx_sent;
    uint32_t tx_dropped;
    uint32_t tx_full, tx_stale, tx_gated, tx_effect_failed, tx_encode_errors, tx_send_errors;
    uint32_t tx_preroll;            /* packets sent from the pre-roll */
    uint32_t tx_injected;           /* packets that carried the test tone */
    /* Uplink timing, this session: longest wait in the uplink queue, longest
     * encode() and send() call; gaps and bursts between sends as for rx. */
    uint32_t tx_queue_age_max_ms, tx_encode_max_us, tx_send_max_us;
    uint32_t tx_gaps, tx_bursts, tx_gap_max_ms;
    /* Push to talk, this session: gate openings and closings, packets sent in
     * the tail after a closing. */
    uint32_t ptt_opens, ptt_closes, ptt_tail_packets;
    /* Playout, this session. */
    uint16_t target_ms, queued_ms, jitter_ms;
    uint32_t playout_starts, late_bursts, fast_blocks, slow_blocks;
    uint32_t write_failed, slow_writes, output_waits;
    /* PCM conservation, this session (samples): decoded = written + discarded + pending. */
    uint32_t pcm_decoded, pcm_written, pcm_discarded, pcm_pending;
    /* FNV-1a over the session's PCM entering the encoder and leaving the
     * decoder: two devices compare them to prove a lossless path. */
    uint32_t pcm_in_hash, pcm_out_hash;
    /* Capture, since boot. Deadline misses: a frame took longer than its own
     * duration to process (synchronous processors are timed by the engine,
     * asynchronous ones report xiaotai_processor_frame_t.process_us). */
    uint32_t capture_errors, capture_lost_samples, capture_overruns, capture_deadline_misses;
    uint32_t capture_process_max_us;
    /* Levels of the last diagnostic window (32 capture frames). Microphone,
     * reference and monitor levels are measured by the engine on the block as
     * read, by channel role, before the processor; clean and uplink after it.
     * Processed is the processor's output before the capture effects (gain,
     * gates), so it compares with the microphone level without their gain. */
    uint16_t mic_rms, mic_peak, reference_rms, reference_peak, monitor_rms, monitor_peak;
    uint16_t processed_rms, processed_peak, clean_rms, clean_peak;
    uint16_t uplink_rms, uplink_peak;
    uint32_t uplink_clipped;
    /* Calls for a generation that is not the open one, since boot. */
    uint32_t stale_calls;
    /* Events and completions posted again because the owner's queue was full, since boot. */
    uint32_t owner_post_retries;
    /* Sounds and recordings, since boot. */
    uint32_t plays, plays_completed, plays_cut, plays_failed;
    uint32_t recordings, recordings_completed, recordings_failed;
    uint32_t preroll_tokens, preroll_lost;
} xiaotai_media_stats_t;

/* [any] */
void xiaotai_media_status(xiaotai_media_status_t *status);
void xiaotai_media_stats(xiaotai_media_stats_t *stats);

/* The loudness of the moment, for whoever draws along with sound (a character
 * that moves its mouth, a level meter). Sample amplitudes, full scale 32768;
 * the microphone's is 0 once its last frame is older than a quarter of a
 * second, the speaker's once its last block has been heard to the end (and 40
 * ms more, in which the next one may still come late), so a reader needs no
 * session state. The audio tasks publish them with plain
 * stores as they pass (every clean capture frame, every block written to the
 * speaker during a session): reading costs nothing on the audio path. The
 * speaker's level is filed under the moment a block will be heard, which is
 * later than it was written by as much as the driver holds
 * (xiaotai_audio_io_info_t.latency_ms), so what is drawn and what is heard go
 * together. Only the voice received counts: sounds mixed into it, sounds
 * played outside a session (prompts, tones) and the tail that is made up when
 * packets stop coming are not followed. */
typedef struct {
    uint16_t capture;   /* RMS of the latest clean microphone frame */
    uint16_t playout;   /* peak of the block of received voice that is being heard */
} xiaotai_media_loudness_t;
/* [any] */
void xiaotai_media_loudness(xiaotai_media_loudness_t *loudness);
/* [any] The speaker's level as it will be heard ahead_ms from now, as far as
 * its blocks are written already (as much as the driver holds: little more
 * than a block when it holds little); for whoever must begin to move a moment
 * before a sound is heard. The microphone's is the present one. Returns
 * whether a block is written for that moment: false when the driver does not
 * hold that far, or nothing is being played; the speaker's level is 0 then
 * and says nothing of a pause to come. */
bool xiaotai_media_loudness_ahead(xiaotai_media_loudness_t *loudness, uint32_t ahead_ms);

/* ---------------------------------------------------------------- lifecycle */

/* Engine lifecycle (called by the framework through the generated tables).
 * init registers the built-in processors, effects and codecs, the diagnostics
 * provider "media" and, through xiaotai_at_verbs_register, the commands
 * AT+AUDIO (status, always); when built with XIAOTAI_MEDIA_TEST_COMMANDS,
 * AT+AUDIO TONE, RECORD, PLAY, STOP, DUMP, SENDTONE; when built with
 * XIAOTAI_MEDIA_SOUND_COMMANDS, AT+SOUND (list; PLAY, STOP); when built with
 * XIAOTAI_MEDIA_TRACE, AT+TRACE (ARM, DUMP, OFF) (xiaotai/media_config.h). The
 * manifest lists the same spellings with the same options. Those options only
 * decide which console commands exist; xiaotai_media_record,
 * xiaotai_media_uplink_tone and the sound functions above work in every build.
 *
 * start chooses the processor from the board's part descriptors, allocates
 * every buffer by purpose, opens the codecs, then the audio parts, then starts
 * the tasks; a board without audio parts starts an engine that refuses
 * sessions.
 *
 * Provisioning boot. When the network engine reports a boot reserved for
 * provisioning (xiaotai_net_provisioning_boot(); read at start, only when the
 * product has the network engine, an optional dependency), start builds only
 * what plays sounds: the output part, the playout (in the folded form the one
 * capture-and-playout task, whose blocking read is its clock, so its capture,
 * taps and the diagnostic recording keep working), sound decoding and the
 * output buffers. It builds no downlink buffer, PCM stash, uplink task,
 * uplink or send queue, encoder, pre-roll, and no capture task of its own.
 * Requirements are still accepted, so modules need no special case, and they
 * count for nothing: xiaotai_media_info reports downlink_ms and preroll_ms 0,
 * can_send and can_receive are false, xiaotai_media_open returns
 * XIAOTAI_ERR_INVALID_STATE and xiaotai_media_preroll_hold
 * XIAOTAI_ERR_NOT_SUPPORTED. Nothing leaves a provisioning boot but a
 * restart, and a board without external RAM needs that memory for the access
 * point and the setup page.
 *
 * Removable audio parts (a forward rule: xiaotai/hal.h has no removable parts
 * in this version). The engine chooses and sizes at start for the device
 * present then. It takes up a device inserted later only while it has no
 * session, recording or sound on that part, and then chooses the processor,
 * rates and part-dependent buffers again, outside the real-time paths, and
 * reports them in xiaotai_media_info, xiaotai_media_can_send and
 * xiaotai_media_can_receive. A removal during a session ends the session as a
 * failure of the part, exactly like a device error
 * (XIAOTAI_MEDIA_EVENT_FAILED); a sound or recording on that part ends with
 * XIAOTAI_FAIL. The downlink keeps its size: requirements size it, not the
 * device. */
xiaotai_err_t xiaotai_engine_media_init(void);
void xiaotai_engine_media_start(void);
/* Host tests: stop the tasks, release everything and forget every registration. */
void xiaotai_media_reset(void);

#ifdef __cplusplus
}
#endif
