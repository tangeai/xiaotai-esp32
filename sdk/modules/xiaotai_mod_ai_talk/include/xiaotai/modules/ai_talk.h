// SPDX-License-Identifier: MIT
#pragma once
/* Base module "ai_talk": a spoken conversation with the AI service.
 *
 * The module owns one conversation at a time: it fetches the session token,
 * requests the link, performs the session handshake, runs the audio session,
 * keeps the caption and the conversation phase, dispatches the service's tool
 * calls to whoever registered them, and ends the conversation in an orderly
 * way whoever ends it. It decides nothing about screens, sounds or what a tool
 * does: applications read the status, subscribe to the events and register
 * tools.
 *
 * Foreground activity "ai" (xiaotai/framework.h), resources audio in, audio
 * out and link. It begins when a conversation is started and ends when the
 * conversation is over (after the tail of the answer has played, when the
 * service ended it). A revocation closes the audio session and the link in
 * the callback; the service is told on a best-effort basis. The activity's
 * generation (xiaotai_activity_set_generation) is what attachments (AI
 * vision) ride on: 0 when the activity begins, the link's generation once the
 * session handshake passed (LISTENING), and 0 again when the conversation
 * enters ENDING, so nothing is sent on the link before the service accepted
 * the session or after it ended it.
 *
 * Generations. `session` names one conversation: a value of
 * xiaotai_generation_next(), never 0, new for every start. Calls that act on
 * a conversation name it; a call naming another conversation than the current
 * one does nothing, returns XIAOTAI_ERR_INVALID_STATE and is counted
 * (stale_calls). Passing 0 means "the current conversation" and is meant for
 * sources that hold no snapshot (a console command, a key). `link` is the
 * generation of the conversation's link (xiaotai/link.h), which is also the
 * generation of its media session; it is 0 until the link was requested.
 *
 * Protocol facts kept here and nowhere else: service "ai", GET /v1/ai/token,
 * a WHIP link with audio on stream 1 in both directions (G.711 A-law, 8 kHz,
 * mono, 20 ms), JSON-RPC 2.0 on command word 0x2100 (bare or packed).
 *
 * Tags as in xiaotai/framework.h: [init], [owner], [any]. Every handler
 * registered here runs on the owner task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk_config.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

/* Product configuration. Every field: 0 (false) selects the stated default,
 * so a zeroed struct is valid. The values describe the product's service path
 * and memory, never a board. */
typedef struct {
    /* Whole budget from the start until the token arrived. Default 17000. */
    uint32_t token_timeout_ms;
    /* From the link request until the link is open. Default 12000. */
    uint32_t connect_timeout_ms;
    /* From the session request until the service answered it. Default 10000. */
    uint32_t handshake_timeout_ms;
    /* The service ended the conversation: true ends at once; default false
     * plays out the audio already received (at most
     * XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS) before the conversation is over. */
    bool no_tail_drain;
    /* Ask the service for the downlink stream (stream 1) instead of accepting
     * whatever stream it sends on its own. Default false. */
    bool request_downlink;
    /* Bytes of caption text kept, with the terminator. Default 4096 with
     * external RAM, 193 without. At least 64. */
    uint16_t caption_bytes;
    /* Largest inbound command payload, and the conversation link's share of
     * the transport engine's command pool: payload bytes of this link that
     * may wait for the owner task at once (xiaotai_link_profile_t). The
     * module declares the share to the transport (xiaotai_transport_require),
     * which sizes its one pool, held for the whole boot, from the largest
     * share unless the product sets the pool. Default 4096 and 8192 with
     * external RAM, 4096 and 4096 without; a board without external RAM sizes
     * the share from the measured peak (xiaotai_link_stats), not from the
     * largest message the protocol allows. A larger command arrives dropped
     * and is ignored (the handshake reply then ends the conversation by its
     * deadline). */
    uint32_t command_max_bytes;
    uint32_t command_pool_bytes;
    /* Pacing profile of the conversation's audio session and of the media
     * requirement (xiaotai/media.h). All zero: XIAOTAI_MEDIA_LATENCY_BURSTY.
     * A product keeps the playout curve its family uses today here. */
    xiaotai_media_latency_t latency;
    /* true: any end of the platform session (platform.offline, whatever the
     * reason; a signaling hold emits none) ends the conversation with
     * END_NETWORK, so no TLS reconnection runs beside the link. Default
     * false: only a binding change ends it (END_SYSTEM) and a conversation
     * whose link is open goes on. */
    bool end_on_platform_offline;
    /* Keep the bounded incoming-audio trace for this conversation when the
     * local diagnostic option is built. Default false. */
    bool trace_arrivals;
    /* Opt in to suspending platform signaling after this conversation owns
     * active audio, for at most 60 seconds and never into token renewal.
     * Default false. Incoming platform signals are not replayed. */
    bool pause_platform_signaling_while_active;
    /* When paused, defer resume to exact link retirement without extending
     * the existing bounded platform hold. Report unconfirmed after two seconds. */
    bool resume_signaling_after_link_release;
} xiaotai_ai_talk_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; copied. NULL
 * or never called: all defaults. XIAOTAI_ERR_INVALID_ARG for caption_bytes
 * below 64 (and not 0) or command sizes the transport refuses;
 * XIAOTAI_ERR_INVALID_STATE after the module's init. */
xiaotai_err_t xiaotai_ai_talk_configure(const xiaotai_ai_talk_config_t *config);

/* ------------------------------------------------------------------- state */

typedef enum {
    XIAOTAI_AI_TALK_IDLE = 0,
    /* Token, link and session handshake are under way; nothing is heard or sent. */
    XIAOTAI_AI_TALK_CONNECTING,
    XIAOTAI_AI_TALK_LISTENING,   /* the session runs; the user's turn */
    XIAOTAI_AI_TALK_THINKING,    /* the user's sentence is final; the answer is on its way */
    XIAOTAI_AI_TALK_SPEAKING,    /* the answer is being delivered */
    /* The service ended the conversation; the audio already received plays
     * out. The link is gone, nothing is sent, tools are no longer called. */
    XIAOTAI_AI_TALK_ENDING,
} xiaotai_ai_talk_phase_t;

/* Why the last conversation ended. Appended, never renumbered. */
typedef enum {
    XIAOTAI_AI_TALK_END_NONE = 0,      /* no conversation ended yet, or one is running */
    XIAOTAI_AI_TALK_END_LOCAL,         /* xiaotai_ai_talk_stop */
    XIAOTAI_AI_TALK_END_REMOTE,        /* the service ended it */
    XIAOTAI_AI_TALK_END_PREEMPTED,     /* a higher-priority activity began */
    /* The system was quiesced, or the binding ended or is being verified
     * (platform.offline with reason BINDING). */
    XIAOTAI_AI_TALK_END_SYSTEM,
    XIAOTAI_AI_TALK_END_TIMEOUT,       /* a step did not finish in its time */
    XIAOTAI_AI_TALK_END_REFUSED,       /* the token or the session was refused */
    XIAOTAI_AI_TALK_END_UNSUPPORTED,   /* the service offered an audio format this device cannot use */
    /* The link could not be made or broke, the transport stopped, or the
     * token request failed on the network. Any other end of the platform
     * session (signaling reconnect, token renewal) leaves a conversation
     * whose link is open running, unless end_on_platform_offline. */
    XIAOTAI_AI_TALK_END_NETWORK,
    XIAOTAI_AI_TALK_END_NO_MEMORY,
    XIAOTAI_AI_TALK_END_AUDIO,         /* the audio session failed, or its pre-roll was lost */
    XIAOTAI_AI_TALK_END_PROTOCOL,      /* a malformed command, or a tool reply could not be sent */
} xiaotai_ai_talk_end_t;

typedef enum {
    XIAOTAI_AI_TALK_ORIGIN_USER = 0,   /* a key, a touch, a command */
    XIAOTAI_AI_TALK_ORIGIN_WAKE,       /* the wake word */
    XIAOTAI_AI_TALK_ORIGIN_OTHER,
} xiaotai_ai_talk_origin_t;

/* A snapshot. Counters count since boot. */
typedef struct {
    xiaotai_ai_talk_phase_t phase;
    uint32_t session;                /* current conversation; 0 when idle */
    uint32_t link;                   /* its link and media generation; 0 until requested and after it closed */
    xiaotai_ai_talk_origin_t origin; /* of the current or the last conversation */
    bool preroll;                    /* it started with the audio captured since the wake word */
    xiaotai_ai_talk_end_t end;       /* why the last conversation ended */
    xiaotai_err_t error;             /* detail of `end` for diagnostics; XIAOTAI_OK for LOCAL and REMOTE */
    uint32_t caption_revision;       /* see xiaotai_ai_talk_caption_read */
    uint32_t started, completed, failed;
    uint32_t stale_calls;            /* calls and replies that named another conversation */
    uint32_t stale_commands;         /* commands of another link, or outside the phase that takes them */
    uint32_t tool_calls, tool_unhandled;
} xiaotai_ai_talk_status_t;

/* [owner] */
void xiaotai_ai_talk_status(xiaotai_ai_talk_status_t *status);

/* ------------------------------------------------------------------ control */

typedef struct {
    xiaotai_ai_talk_origin_t origin;
    /* A token of xiaotai_media_preroll_hold (xiaotai/media.h): the
     * conversation begins with what was said since the wake word. The module
     * owns the token from the call on, whatever it returns: it binds it to
     * the audio session or releases it. 0: none. */
    uint32_t preroll_token;
} xiaotai_ai_talk_start_t;

/* [owner] Starts a conversation; options may be NULL (origin USER, no
 * pre-roll). Returns without waiting for the network; on XIAOTAI_OK *session
 * (may be NULL) names the conversation, the phase is CONNECTING and
 * XIAOTAI_EVENT_AI_TALK_STATE follows for every change until IDLE.
 *   XIAOTAI_ERR_INVALID_STATE     a conversation is already running, the
 *                                 microphone is muted, or the board has no
 *                                 usable audio format
 *   XIAOTAI_ERR_PLATFORM_OFFLINE  the platform session or the transport is
 *                                 not online (xiaotai/platform.h)
 *   XIAOTAI_ERR_BUSY              an activity of equal or higher priority is
 *                                 current, or the system is quiesced
 *   XIAOTAI_ERR_NO_MEM            no request slot is free */
xiaotai_err_t xiaotai_ai_talk_start(const xiaotai_ai_talk_start_t *options, uint32_t *session);

/* [owner] Ends the conversation now: the service is told when the link is
 * open, nothing is played out, the phase is IDLE when the call returns. */
xiaotai_err_t xiaotai_ai_talk_stop(uint32_t session);

/* [owner] The pre-roll token handed to xiaotai_ai_talk_start was lost before
 * the audio session bound it: whoever held it forwards the media engine's
 * lost() callback here (xiaotai_media_preroll_t.lost goes to the holder, the
 * local wake module). The conversation that owns the token ends at once
 * with XIAOTAI_AI_TALK_END_AUDIO instead of failing at activation after the
 * token and the link. XIAOTAI_ERR_INVALID_STATE, counted in stale_calls,
 * when no running conversation owns `token` unbound. A loss after binding
 * arrives as XIAOTAI_MEDIA_EVENT_PREROLL_LOST and the conversation goes on
 * live. */
xiaotai_err_t xiaotai_ai_talk_preroll_lost(uint32_t token);

/* [owner] Cuts the answer short: audio held and not yet played is dropped and
 * the service is told to stop speaking. Only while LISTENING, THINKING or
 * SPEAKING. A failed send ends the conversation (END_NETWORK) and is the
 * result. */
xiaotai_err_t xiaotai_ai_talk_interrupt(uint32_t session);

/* [owner] Tells the service the user finished the sentence, without waiting
 * for its own end-of-speech detection. Same conditions as interrupt. */
xiaotai_err_t xiaotai_ai_talk_submit(uint32_t session);

/* ----------------------------------------------------------------- caption */

typedef enum {
    XIAOTAI_AI_TALK_SPEAKER_NONE = 0,  /* no caption yet in this conversation */
    XIAOTAI_AI_TALK_SPEAKER_USER,      /* what the service understood */
    XIAOTAI_AI_TALK_SPEAKER_AI,        /* what the service answers */
} xiaotai_ai_talk_speaker_t;

/* Longest emotion word, with the terminator. */
#define XIAOTAI_AI_TALK_EMOTION_MAX 32U

/* The current paragraph: the text of one speaker's turn, as far as it has
 * arrived. The text is the service's, without any prefix; applications and
 * themes add what they show around it. */
typedef struct {
    /* In: the revision the caller already has (0: none). Out: the current one.
     * It advances with every change of the text, the speaker or the flags and
     * restarts at 0 with every conversation. */
    uint32_t revision;
    uint32_t session;
    xiaotai_ai_talk_speaker_t speaker;
    uint32_t paragraph;        /* counts the turns of this conversation, from 1 */
    bool final;                /* the service marked the sentence as complete */
    bool truncated;            /* the turn was longer than the module keeps; the end is missing */
    /* The emotion word the service attached to the answer, unchanged (UTF-8);
     * "" when none came with this turn. Mapping it to a face is the
     * application's: themes know face names only. */
    char emotion[XIAOTAI_AI_TALK_EMOTION_MAX];
    /* The caller's buffer (keep it in XIAOTAI_MEM_BULK where the board has
     * it). The text is cut at a UTF-8 character boundary when it does not fit
     * and is always terminated. NULL with capacity 0 reads the fields only. */
    char *text;
    size_t capacity;
    size_t size;               /* bytes written without the terminator */
} xiaotai_ai_talk_caption_t;

/* [owner] True when the caption changed since caption->revision: every field
 * is then filled. False: nothing was written. */
bool xiaotai_ai_talk_caption_read(xiaotai_ai_talk_caption_t *caption);

/* ------------------------------------------------------------------- tools */

/* The service may ask the device to do something during a conversation (dial
 * a contact, look up who is reachable). The module knows no tool: whoever
 * implements one registers it, and a request for an unregistered name is
 * answered with the protocol's "unsupported" error. */

/* Longest tool name, with the terminator. */
#define XIAOTAI_AI_TALK_TOOL_NAME_MAX 48U

/* JSON-RPC error codes the module and tools answer with. */
#define XIAOTAI_AI_TALK_ERROR_INVALID_PARAMS (-32602)
#define XIAOTAI_AI_TALK_ERROR_UNSUPPORTED (-32601)
#define XIAOTAI_AI_TALK_ERROR_FAILED (-32000)

typedef struct {
    uint32_t session;
    /* Names the request for the answer; 0 when the service expects none (a
     * notification): replying is then a successful no-op. */
    uint32_t request;
    const char *name;        /* the registered name that matched */
    /* The request's argument object as JSON text, terminated; "{}" when it
     * has none. Arguments wrapped by the service in "data", "arguments",
     * "args", "input", "payload" or "parameters" are unwrapped. Valid during
     * the handler only. */
    const char *arguments;
    size_t size;
    bool device_action;      /* distinct from an old method-name notification */
    bool has_arguments;      /* the peer sent a parameter object */
    size_t id_text_bytes;    /* decoded string id, 0 for a number or absent */
} xiaotai_ai_talk_tool_call_t;

/* [owner] */
typedef void (*xiaotai_ai_talk_tool_fn_t)(const xiaotai_ai_talk_tool_call_t *call, void *ctx);

/* [init] Registers a tool: `name` is matched exactly against the action name
 * of a device-action request and against the method name of a notification
 * the module does not handle itself. Static storage; one handler per name.
 * The handler runs in its registering application or module's owner scope.
 * XIAOTAI_ERR_INVALID_ARG: an empty or too long name, no handler;
 * XIAOTAI_ERR_INVALID_STATE: the name is taken, or not during init;
 * XIAOTAI_ERR_NO_MEM: the table is full (XIAOTAI_AI_TALK_TOOLS_MAX entries,
 * a build option: xiaotai/modules/ai_talk_config.h). */
xiaotai_err_t xiaotai_ai_talk_tool_register(const char *name, xiaotai_ai_talk_tool_fn_t handler, void *ctx);

/* [init] Optional error responder for unrecognized device_action requests.
 * call.name is empty for an invalid action name; notifications never use it. */
xiaotai_err_t xiaotai_ai_talk_tool_fallback_register(xiaotai_ai_talk_tool_fn_t handler, void *ctx);

/* Largest result or error-data object a tool may answer with, with the
 * terminator. */
#define XIAOTAI_AI_TALK_TOOL_REPLY_MAX 4096U

/* [owner] Answers a tool request, from the handler or later. result_json is
 * one JSON object (NULL: "{}"), copied before the call returns. Each request
 * is answered once; a request that is not answered before the conversation
 * ends is dropped with it.
 *   XIAOTAI_ERR_INVALID_STATE  the conversation, the link or the request is
 *                              gone, or it was answered (counted in stale_calls)
 *   XIAOTAI_ERR_INVALID_SIZE   object longer than XIAOTAI_AI_TALK_TOOL_REPLY_MAX,
 *                              or complete response exceeds the link limit
 *   XIAOTAI_ERR_INVALID_ARG    not one JSON object
 *   XIAOTAI_ERR_NO_MEM         unable to assemble the complete response
 *   other                      the send failed; the conversation continues */
xiaotai_err_t xiaotai_ai_talk_tool_reply(uint32_t session, uint32_t request, const char *result_json);

/* [owner] Answers with a JSON-RPC error: code (one of the three above, or the
 * tool's own negative code), message (a fixed ASCII word such as "not_found";
 * static storage) and an optional data object. Results as above. */
xiaotai_err_t xiaotai_ai_talk_tool_fail(uint32_t session, uint32_t request, int32_t code, const char *message,
                                        const char *data_json);

typedef struct {
    const char *object;     /* result or error.data, as above */
    int32_t code;          /* 0 for result, a negative value for error */
    const char *message;    /* code < 0: printable UTF-8, 1..128 bytes, JSON escaped */
    size_t max_bytes;       /* optional complete JSON-RPC byte limit, without terminator */
} xiaotai_ai_talk_tool_response_t;

/* [owner] Compatibility response using the same handle and framing. A limit
 * refusal leaves the handle valid for a smaller response. */
xiaotai_err_t xiaotai_ai_talk_tool_respond(uint32_t session, uint32_t request,
                                          const xiaotai_ai_talk_tool_response_t *response);

/* Reading a call's arguments. The service's language model fills them in, so
 * these reads accept what a model writes for the type a tool asked for and
 * nothing else: an integer also as a number with a zero fraction (7.0) or as
 * digits in a string ("60", "60%"); a boolean also as "true" / "false" or the
 * numbers 1 / 0; a word also from a number or a boolean, as written. Only
 * top-level members of call->arguments are read. Each call parses the
 * arguments again on the caller's stack (about 400 bytes; 700 for a text):
 * nothing is kept and nothing is allocated.
 *   XIAOTAI_OK                the value was read
 *   XIAOTAI_ERR_NOT_FOUND     no such member, or it is null or "" (an
 *                             optional argument left out); *value is
 *                             untouched, out is ""
 *   XIAOTAI_ERR_INVALID_ARG   another type, not a whole number, outside
 *                             int32_t, or an argument object with nested
 *                             values or more than
 *                             XIAOTAI_AI_TALK_TOOL_ARGS_MAX members
 *   XIAOTAI_ERR_INVALID_SIZE  a word longer than capacity - 1 bytes */
#define XIAOTAI_AI_TALK_TOOL_ARGS_MAX 11U
xiaotai_err_t xiaotai_ai_talk_tool_arg_int(const xiaotai_ai_talk_tool_call_t *call, const char *name,
                                           int32_t *value);
xiaotai_err_t xiaotai_ai_talk_tool_arg_bool(const xiaotai_ai_talk_tool_call_t *call, const char *name, bool *value);
/* Surrounding spaces are dropped and ASCII letters lowered, so an enumeration
 * compares with strcmp. */
xiaotai_err_t xiaotai_ai_talk_tool_arg_word(const xiaotai_ai_talk_tool_call_t *call, const char *name,
                                            char *out, size_t capacity);
/* A string as written (UTF-8), without surrounding spaces, cut at a character
 * boundary when longer than capacity - 1 bytes: for a label the device
 * shows. Never INVALID_SIZE; a string of 256 bytes or more is INVALID_ARG. */
xiaotai_err_t xiaotai_ai_talk_tool_arg_text(const xiaotai_ai_talk_tool_call_t *call, const char *name,
                                            char *out, size_t capacity);

/* ------------------------------------------------------- link extensions */

/* Scene of the conversation's sessions for session extensions
 * (xiaotai/framework.h, xiaotai_contribution_t). The module has no API for
 * extensions: an attachment of the activity "ai" (AI vision) that
 * contributes, for this scene or for every scene, the video part of a link
 * (XIAOTAI_LINK_SLOT_VIDEO, xiaotai/link.h) has it copied into every link the
 * module requests. Only the first such contribution is used; further ones are
 * counted in the diagnostics. The audio part and the commands stay this
 * module's. Contributions are read when a link is requested (valid from
 * start() on); without one the link carries no video. */
#define XIAOTAI_AI_TALK_SCENE "ai"

/* -------------------------------------------------- events and intents */

/* Events on the framework bus. data is a xiaotai_ai_talk_status_t.
 *
 * "ai_talk.state": the phase changed. args.value: the new
 * xiaotai_ai_talk_phase_t; args.text: "". A conversation's events are, in
 * order: CONNECTING, LISTENING, any number of LISTENING / THINKING /
 * SPEAKING, optionally ENDING, IDLE. With IDLE the status carries the end
 * reason. */
#define XIAOTAI_EVENT_AI_TALK_STATE "ai_talk.state"
/* "ai_talk.media": the held media session has opened and the session
 * request was sent. data: xiaotai_ai_talk_status_t, args.value: its phase.
 * A local cue can use this link generation while still CONNECTING. Check
 * the current session and phase; the notice can arrive after either ends. */
#define XIAOTAI_EVENT_AI_TALK_MEDIA "ai_talk.media"
/* "ai_talk.caption": the caption changed. args.value: its new revision.
 * Changes that arrive faster than they are handled merge: a handler reads
 * the present caption with xiaotai_ai_talk_caption_read. */
#define XIAOTAI_EVENT_AI_TALK_CAPTION "ai_talk.caption"

/* Intents the module handles. args.value names the conversation for the last
 * four (0: the current one); args.text is unused.
 *   "talk.start"      start with origin USER; ignored while one runs
 *   "talk.toggle"     start when idle, stop otherwise
 *   "talk.stop"       xiaotai_ai_talk_stop
 *   "talk.interrupt"  xiaotai_ai_talk_interrupt
 *   "talk.submit"     xiaotai_ai_talk_submit */
#define XIAOTAI_INTENT_TALK_START "talk.start"
#define XIAOTAI_INTENT_TALK_TOGGLE "talk.toggle"
#define XIAOTAI_INTENT_TALK_STOP "talk.stop"
#define XIAOTAI_INTENT_TALK_INTERRUPT "talk.interrupt"
#define XIAOTAI_INTENT_TALK_SUBMIT "talk.submit"

/* Name of the foreground activity (priority table key). */
#define XIAOTAI_AI_TALK_ACTIVITY "ai"

/* Commands: AT+AI (status), AT+AI START, STOP, INTERRUPT, SUBMIT. Diagnostics
 * section "ai_talk". Event-log source "ai_talk". No caption text ever appears
 * in a reply, a log or the event log. */

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: declares its needs to the platform, the transport (one link and its
 * command share) and the media engine (with the configured latency),
 * registers intents, commands and diagnostics, subscribes to the platform's
 * events. start: allocates the caption (XIAOTAI_MEM_BULK). */
xiaotai_err_t xiaotai_module_ai_talk_init(void);
void xiaotai_module_ai_talk_start(void);
/* Host tests: end the conversation and forget configuration, tools and state. */
void xiaotai_ai_talk_reset(void);

#ifdef __cplusplus
}
#endif
