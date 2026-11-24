// SPDX-License-Identifier: MIT
#pragma once
/* Base module "room": multi-party push-to-talk rooms.
 *
 * A device is a member of at most one room. Membership (the assignment) is
 * kept by the server and survives restarts; this module mirrors it, and while
 * an application attends the room it keeps the device connected to it: it
 * fetches a connection token, opens the link, joins, keeps the lease alive
 * with heartbeats, mirrors the member list and carries push-to-talk. It offers
 * a typed API, events and intents; it decides no business flow and shows no
 * view (which page means "attending", what a member row looks like and every
 * text belong to the application).
 *
 * Foreground: a connected room is the foreground activity "room"
 * (xiaotai/framework.h). The module begins it by itself whenever the device is
 * attending, assigned and free, and begins it again when the activity that
 * displaced it has ended; when the activity is revoked it leaves the
 * conversation, tells the server and waits (XIAOTAI_REVOKE_PREEMPTED, an
 * incoming call for instance: until the device is free again;
 * XIAOTAI_REVOKE_SYSTEM: until the quiesce ends). Attendance and membership
 * are untouched. Whether a room outranks another activity is the product's
 * priority table, never this module. The activity is begun before the
 * connection token is requested, so its generation is not known then: the
 * module sets it to the link's generation (xiaotai_activity_set_generation)
 * when the room has answered the join (LISTENING) and back to 0 when the
 * conversation starts ending, before the media session and the link close.
 *
 * Generations: the link's generation (xiaotai/link.h) names the connection in
 * the status, in push-to-talk calls and on every engine. A press that names
 * another generation is refused and counted. Server replies carry the binding
 * generation (xiaotai/identity.h); a change of binding forgets everything.
 *
 * Tags are those of xiaotai/framework.h: [init], [owner], [any], [rt].
 * Functions tagged [any] that change something queue the request to the owner
 * task and return: XIAOTAI_OK means accepted, never done. What became of it is
 * told by the events and by xiaotai_room_status(). They return
 * XIAOTAI_ERR_NO_MEM when the owner's queue is full (nothing was queued) and
 * XIAOTAI_ERR_INVALID_STATE before the module started. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- limits */

/* A room code is exactly this many decimal digits; a password is empty or
 * exactly this many. Both are digit strings, never numbers. */
#define XIAOTAI_ROOM_CODE_DIGITS 6U
#define XIAOTAI_ROOM_PASSWORD_DIGITS 4U
/* Text sizes including the terminator. */
#define XIAOTAI_ROOM_ID_MAX 65U
#define XIAOTAI_ROOM_NAME_MAX 65U
#define XIAOTAI_ROOM_PARTICIPANT_MAX 129U
/* Most members a room has by protocol; a snapshot with more is refused. */
#define XIAOTAI_ROOM_MEMBERS_LIMIT 100U

/* ------------------------------------------------------------ configuration */

/* members_max value: keep no member list, only the counts. */
#define XIAOTAI_ROOM_NO_MEMBER_LIST UINT16_MAX
/* talk_tail_ms value: nothing captured before the release is sent after it. */
#define XIAOTAI_ROOM_NO_TAIL UINT16_MAX

/* What an application chooses. Every field: 0 (false) selects the default, so
 * a zeroed struct is valid. Nothing here names a board: the defaults depend
 * only on whether the memory port reports external RAM. */
typedef struct {
    /* Members kept for xiaotai_room_members(), 1..XIAOTAI_ROOM_MEMBERS_LIMIT,
     * or XIAOTAI_ROOM_NO_MEMBER_LIST. Default: the limit with external RAM,
     * no list without. A room with more members than are kept is still joined:
     * the rest is counted and members_truncated is set. */
    uint16_t members_max;
    /* false: connect whenever the device is attending, assigned and free.
     * true: connect only on xiaotai_room_connect(); attending alone then
     * mirrors the assignment. */
    bool manual_connect;
    /* Audio captured before a release that is still sent after it, in
     * milliseconds (a word is not cut at the key). Default 600. */
    uint16_t talk_tail_ms;
    /* 0: a press holds until it is released. Otherwise the caller repeats
     * xiaotai_room_talk(generation, true) while the control is held, and the
     * module releases by itself when no press arrived for this long: for
     * controls whose release can be lost (a touch target that disappears).
     * At least 200. */
    uint16_t talk_keepalive_ms;
    /* Consecutive failed connection attempts after which the module stops
     * retrying and waits for the user (XIAOTAI_ROOM_BLOCKED). Default 5. */
    uint8_t retry_max;
    /* After joining, ask the room for its audio (stream 1, xiaotai/link.h
     * receive_request) instead of accepting what it sends on its own.
     * Default false. */
    bool request_downlink;
    /* Keep the press through the exact keepalive millisecond; the owner
     * expires it at the next millisecond. Default false. */
    bool talk_keepalive_exclusive;
    /* Recover the connection (XIAOTAI_ERR_TIMEOUT) on the first room command
     * that exceeds command_max_bytes. Drops at or below this limit and parser
     * failures keep their usual policy. Default false. */
    bool recover_oversized_command;
    /* Largest room command kept (xiaotai_link_profile_t.command_max_bytes),
     * 2048..XIAOTAI_LINK_COMMAND_LIMIT_BYTES, and the link's share of the
     * transport's command pool (command_pool_bytes), at least the former.
     * Default 40960 and 81920 with external RAM, 12288 and 16384 without (a
     * member snapshot of a full room is the largest command). A command
     * that does not fit arrives dropped: by default, while connected it is
     * counted, a fresh snapshot is asked for, and the second one in a row
     * ends the conversation (XIAOTAI_ERR_INVALID_SIZE). The share is declared
     * to the transport in the module's init (xiaotai_transport_require); only a
     * configuration given before init (the product glue) can change either
     * size. In a later one 0 keeps what was
     * declared and other values are refused with XIAOTAI_ERR_INVALID_STATE
     * (nothing of that configuration is taken). The transport's
     * pool itself is the product's (xiaotai/transport.h): a product that
     * sets it below the share keeps fewer bytes. */
    uint32_t command_max_bytes;
    uint32_t command_pool_bytes;
    /* Quiesce reasons, besides XIAOTAI_QUIESCE_ENVIRONMENT, that wait until
     * the room is settled (xiaotai_room_settled, below): true makes the
     * module's quiesce guard refuse starting over with provisioning
     * (XIAOTAI_QUIESCE_PROVISIONING), or a binding change
     * (XIAOTAI_QUIESCE_BINDING), while it is not. Default false: that quiesce
     * goes ahead at once and the room winds down as for any other one
     * (XIAOTAI_REVOKE_SYSTEM). */
    bool settle_before_provisioning;
    bool settle_before_binding;
    /* Notify the attempt observer once when no memory defers admission.
     * A later retry still uses the short pending deadline. Default false. */
    bool notify_deferred_no_mem;
    /* Accept "alaw" as the legacy G.711 A-law spelling in a join_room reply.
     * Requests always send "g711a". Default false. */
    bool accept_alaw_join_reply;
    /* Submit speaking before opening the microphone; the failure of either
     * talk edge leaves the room joined. Default false (local gate first). */
    bool talk_signal_before_gate;
    /* Submit a best-effort connecting presence before opening each RTC link.
     * It does not wait for the HTTP response. Default false. */
    bool connecting_presence;
    /* On 40921, wait one lease interval unless the replaced reservation's
     * release is confirmed. A token rejection cannot identify an earlier
     * reservation. Default false. */
    bool wait_superseded_lease;
} xiaotai_room_config_t;

/* [glue, init] From the product glue or an application's init(); applied when
 * the module starts (the command sizes: see above). The struct is copied.
 * NULL: all defaults. XIAOTAI_ERR_INVALID_ARG for a value outside its range;
 * XIAOTAI_ERR_INVALID_STATE once the module has started, or for non-zero
 * command sizes other than those declared at init. */
xiaotai_err_t xiaotai_room_configure(const xiaotai_room_config_t *config);

/* Local presentation of an owned connection attempt, independent of its RTC
 * link generation. One optional observer registers on the owner during init.
 * It runs synchronously on that owner; it must not call back into Room. */
typedef enum {
    XIAOTAI_ROOM_ATTEMPT_STARTED,
    XIAOTAI_ROOM_ATTEMPT_ENDED,
    XIAOTAI_ROOM_ATTEMPT_FAILED,
} xiaotai_room_attempt_t;
typedef void (*xiaotai_room_attempt_fn_t)(uint32_t activity, xiaotai_room_attempt_t phase);
xiaotai_err_t xiaotai_room_observe_attempts(xiaotai_room_attempt_fn_t observer);

/* -------------------------------------------------------------------- state */

/* New phases are appended; values never change. */
typedef enum {
    XIAOTAI_ROOM_OFFLINE = 0,  /* the assignment is not known (never attended, offline, unbound) */
    XIAOTAI_ROOM_SYNCING,      /* reading the assignment for the first time since it became unknown */
    XIAOTAI_ROOM_EMPTY,        /* member of no room */
    XIAOTAI_ROOM_WAITING,      /* member of a room, not connected; wait says why */
    XIAOTAI_ROOM_CONNECTING,   /* token, link or join in progress */
    XIAOTAI_ROOM_LISTENING,    /* joined; this device is silent */
    XIAOTAI_ROOM_SPEAKING,     /* joined; this device is talking */
    XIAOTAI_ROOM_LEAVING,      /* giving up the membership */
    XIAOTAI_ROOM_BLOCKED,      /* member of a room; gave up connecting until refresh, connect or attending again */
} xiaotai_room_phase_t;

/* Why a member is not connected (phase XIAOTAI_ROOM_WAITING). Appended, never
 * renumbered. */
typedef enum {
    XIAOTAI_ROOM_WAIT_NONE = 0,
    XIAOTAI_ROOM_WAIT_NOT_ATTENDING, /* nobody attends the room */
    XIAOTAI_ROOM_WAIT_MANUAL,        /* disconnected on request, or manual_connect and not asked yet */
    XIAOTAI_ROOM_WAIT_OFFLINE,       /* the platform session or the transport is not online */
    XIAOTAI_ROOM_WAIT_ACTIVITY,      /* another activity holds the device (a call, a conversation) */
    XIAOTAI_ROOM_WAIT_RELEASE,       /* the server has not yet released the previous connection's reservation */
    XIAOTAI_ROOM_WAIT_RETRY,         /* backing off after a failed attempt */
} xiaotai_room_wait_t;

/* A snapshot; taken under one short lock, consistent in itself. */
typedef struct {
    /* Advances with every change of this struct. */
    uint32_t revision;
    xiaotai_room_phase_t phase;
    xiaotai_room_wait_t wait;
    /* Generation of the room's link from the moment it is requested until it
     * ends; 0 otherwise. What xiaotai_room_talk() names. */
    uint32_t generation;
    bool attending;
    bool assignment_known;        /* the server's answer about membership is held */
    bool assigned;                /* member of the room named below */
    bool busy;                    /* a create, join or leave request is in flight */
    bool transmitting;            /* the uplink is open: audio captured now reaches the room */
    bool members_ready;           /* a member snapshot of this connection is held */
    bool members_truncated;       /* the room has more members than are kept */
    uint16_t member_count;        /* members the server reports */
    uint16_t listed_count;        /* members xiaotai_room_members() can return */
    uint16_t speaking_count;      /* members talking, this device included */
    uint32_t members_revision;    /* advances with every change of the member list */
    uint8_t attempts;             /* consecutive failed connection attempts */
    /* Why the last attempt or request failed; XIAOTAI_OK when it did not.
     * code is the server's business code that came with it (40300, 40400,
     * 40920, ...; XIAOTAI_ROOM_CODE_* below), or 0. */
    xiaotai_err_t error;
    int32_t code;
    uint32_t assignment_version;
    char room_id[XIAOTAI_ROOM_ID_MAX];                  /* "" unless assigned */
    char room_code[XIAOTAI_ROOM_CODE_DIGITS + 1U];      /* "" unless assigned */
    char self_name[XIAOTAI_ROOM_NAME_MAX];              /* the server's name for this device; "" when it gave none */
} xiaotai_room_status_t;

/* Business codes of the room service that an application may want to tell
 * apart. Any other code is passed through unchanged. */
#define XIAOTAI_ROOM_CODE_UNBOUND 40300        /* the device may not use rooms: not retried */
#define XIAOTAI_ROOM_CODE_WRONG_PASSWORD 40320
#define XIAOTAI_ROOM_CODE_NOT_FOUND 40400      /* no such room, or it was closed */
#define XIAOTAI_ROOM_CODE_FULL 40920
#define XIAOTAI_ROOM_CODE_SUPERSEDED 40921     /* the membership or the session was replaced */
#define XIAOTAI_ROOM_CODE_ALREADY_MEMBER 40923 /* leave the current room first (also produced locally) */
#define XIAOTAI_ROOM_CODE_PASSWORD_LOCKED 42920

/* [any] */
void xiaotai_room_status(xiaotai_room_status_t *status);

typedef enum {
    XIAOTAI_ROOM_MIC_OFF = 0,
    XIAOTAI_ROOM_MIC_ON,        /* open, nobody talking into it */
    XIAOTAI_ROOM_MIC_SPEAKING,
} xiaotai_room_mic_t;

typedef struct {
    char participant_id[XIAOTAI_ROOM_PARTICIPANT_MAX];
    char device_id[XIAOTAI_ROOM_ID_MAX];   /* "" for a participant that is not a device */
    char name[XIAOTAI_ROOM_NAME_MAX];      /* the server's display name; "" when it gave none */
    xiaotai_room_mic_t mic;
    bool self;
} xiaotai_room_member_t;

/* [any] Copies up to capacity members starting at offset, in the server's
 * order, into out (the caller's storage; a page of a few members keeps it off
 * small stacks). Returns the number copied: 0 past the end, without a list or
 * before a snapshot arrived. *revision (may be NULL) receives the list's
 * revision the copy belongs to (xiaotai_room_status_t.members_revision). */
size_t xiaotai_room_members(uint16_t offset, xiaotai_room_member_t *out, size_t capacity, uint32_t *revision);

/* ------------------------------------------------------------------ actions */

/* [any] The application attends the room (its room page is shown, its room
 * mode is entered) or stops attending. Attending reads the assignment and,
 * unless manual_connect, connects. Not attending ends push-to-talk at once,
 * leaves the conversation, releases the lease and stops every periodic
 * request; the membership is untouched. Requests queued before a change of
 * attendance are dropped when they are reached, so a tap on a page that is
 * gone does nothing. Not attending at start. */
xiaotai_err_t xiaotai_room_attend(bool attending);

/* [any] Create a room and become its member. password: NULL or "" for none,
 * else exactly XIAOTAI_ROOM_PASSWORD_DIGITS digits (XIAOTAI_ERR_INVALID_ARG).
 * Also refused with XIAOTAI_ERR_INVALID_STATE while not attending. Later
 * refusals (already a member, another request in flight, offline, assignment
 * unknown) are reported by XIAOTAI_EVENT_ROOM_RESULT. The text is copied. */
xiaotai_err_t xiaotai_room_create(const char *password);

/* [any] Join the room with this code (exactly XIAOTAI_ROOM_CODE_DIGITS digits).
 * Otherwise as xiaotai_room_create(). */
xiaotai_err_t xiaotai_room_join(const char *code, const char *password);

/* [any] Give up the membership: the conversation ends first, then the server
 * is told; the assignment is cleared only by the server's new answer. Carried
 * through even when attendance ends meanwhile. */
xiaotai_err_t xiaotai_room_leave(void);

/* [any] Read the assignment again now, forget the failure count and leave
 * XIAOTAI_ROOM_BLOCKED; when joined, also ask for a fresh member snapshot. */
xiaotai_err_t xiaotai_room_refresh(void);

/* [any] Connect to the assigned room (manual_connect, or after
 * xiaotai_room_disconnect()), forgetting the failure count. */
xiaotai_err_t xiaotai_room_connect(void);

/* [any] Leave the conversation and stay a member: no reconnection until
 * xiaotai_room_connect(), xiaotai_room_refresh() or attending again. */
xiaotai_err_t xiaotai_room_disconnect(void);

/* [any] Push to talk. generation is xiaotai_room_status_t.generation as the
 * caller saw it when the control was pressed.
 * pressed true: accepted only while the phase is LISTENING or SPEAKING, the
 * generation is the current one, the device is attending and the microphone is
 * not muted; the uplink opens at once unless talk_signal_before_gate is set.
 * With that configuration a press only queues admission; a failed send
 * leaves the uplink shut and reports the error in room status.
 * XIAOTAI_ERR_INVALID_STATE otherwise
 * (counted). With talk_keepalive_ms the call is repeated while held.
 * pressed false: closes the uplink on the calling task before it returns,
 * whatever the generation and even when the owner's queue is full; audio
 * captured up to the call is still sent for talk_tail_ms. Always XIAOTAI_OK.
 * It never changes the microphone mute setting. The room is told about the
 * change of speaker by the owner task afterwards. */
xiaotai_err_t xiaotai_room_talk(uint32_t generation, bool pressed);

/* [any] Toggle the current generation's requested press, including a press
 * still awaiting its owner. On acceptance, *pressed is the requested state.
 * A rejected press does not consume the next toggle. */
xiaotai_err_t xiaotai_room_toggle_talk(uint32_t generation, bool *pressed);

/* [any] Physical local HOLD edges on a ticketed control. Tickets increase
 * for new holds (wrapping from UINT32_MAX to 1); repeated, older or ongoing
 * tickets cannot open the gate. Releases are idempotent and only close the
 * gate accepted for this exact generation and ticket. This preserves the
 * configured tail and the normal owner-side speaking/off notification. Do
 * not use this for the generic stop command, which may close any hold. */
xiaotai_err_t xiaotai_room_hold_local(uint32_t generation, uint32_t ticket, bool pressed);
/* [any] Renew only the exact already accepted local hold. An expired or
 * muted hold closes without allowing this pointer to open the gate again. */
xiaotai_err_t xiaotai_room_hold_local_refresh(uint32_t generation, uint32_t ticket);

/* [owner] False while the module holds something a maintenance operation must
 * not interrupt: a request in flight, a reservation not yet released at the
 * server, a leave not yet confirmed. Answers from memory.
 *
 * The module registers a quiesce guard (xiaotai_quiesce_guard_register, name
 * "room"; xiaotai/framework.h). While this is false it answers
 * XIAOTAI_ERR_BUSY for XIAOTAI_QUIESCE_ENVIRONMENT, so a switch of the
 * service environment is refused until the room is settled, and for
 * XIAOTAI_QUIESCE_PROVISIONING and XIAOTAI_QUIESCE_BINDING where the
 * configuration asks for it (settle_before_provisioning,
 * settle_before_binding). Every other reason (XIAOTAI_QUIESCE_COPROCESSOR, a
 * restart's own word, a component's own word), and every reason while this
 * is true, gets XIAOTAI_OK: an orderly restart does not wait for the room.
 * The guard reads the configuration in effect when it is asked. While the
 * system is quiesced the module sends no request: create, join, leave and
 * refresh end at once in XIAOTAI_EVENT_ROOM_RESULT with
 * XIAOTAI_ERR_INVALID_STATE, periodic reads wait, attendance is still
 * recorded; when the quiesce ends an attending device reads the assignment at
 * once. */
bool xiaotai_room_settled(void);

/* ------------------------------------------------------------------- events */

/* Emitted on the framework event bus, in the order things happened; handlers
 * read xiaotai_room_status() for the present truth. */

/* The status changed (anything but the member list). args.value: the new
 * xiaotai_room_phase_t; args.text: the room code, "" when not assigned. */
#define XIAOTAI_EVENT_ROOM_STATE "room.state"
/* The member list or a member's microphone changed. args.value: member_count. */
#define XIAOTAI_EVENT_ROOM_MEMBERS "room.members"
/* Somebody else started or stopped talking. args.value: members talking, this
 * device not counted. For products that show a room with a light. */
#define XIAOTAI_EVENT_ROOM_VOICE "room.voice"
/* A create, join, leave, refresh, connect or disconnect request reached its
 * end. args.value: its xiaotai_err_t; args.text: the action's word ("create",
 * "join", "leave", "refresh", "connect", "disconnect"); data: a
 * xiaotai_room_result_t. */
#define XIAOTAI_EVENT_ROOM_RESULT "room.result"

typedef enum {
    XIAOTAI_ROOM_ACTION_CREATE = 0,
    XIAOTAI_ROOM_ACTION_JOIN,
    XIAOTAI_ROOM_ACTION_LEAVE,
    XIAOTAI_ROOM_ACTION_REFRESH,
    XIAOTAI_ROOM_ACTION_CONNECT,
    XIAOTAI_ROOM_ACTION_DISCONNECT,
} xiaotai_room_action_t;

typedef struct {
    xiaotai_room_action_t action;
    /* XIAOTAI_OK: done. XIAOTAI_ERR_INVALID_STATE: refused where it stood (not
     * attending any more, assignment unknown, the system is quiesced, already
     * a member: code 40923).
     * XIAOTAI_ERR_BUSY: another request is in flight. XIAOTAI_ERR_TIMEOUT: no
     * answer. XIAOTAI_FAIL: the server refused; code says how. */
    xiaotai_err_t error;
    int32_t code;   /* the server's business code, or 0 */
} xiaotai_room_result_t;

/* ------------------------------------------------------------------ intents */

/* Handled by this module; posted by input tables, views and commands. They
 * map one to one to the functions above.
 *   room.attend      args.value: 1 attend, 0 stop attending
 *   room.create      args.text: the password, "" for none
 *   room.join        args.text: "<code>" or "<code> <password>"
 *   room.leave, room.refresh, room.connect, room.disconnect: no arguments
 *   room.talk        args.value: 1 pressed, 0 released; names the connection
 *                    that is current when the intent is handled
 * An intent is queued like every intent: a control whose release must close
 * the uplink even when the queue is full calls xiaotai_room_talk() instead,
 * or the application sets talk_keepalive_ms. */
#define XIAOTAI_INTENT_ROOM_ATTEND "room.attend"
#define XIAOTAI_INTENT_ROOM_CREATE "room.create"
#define XIAOTAI_INTENT_ROOM_JOIN "room.join"
#define XIAOTAI_INTENT_ROOM_LEAVE "room.leave"
#define XIAOTAI_INTENT_ROOM_REFRESH "room.refresh"
#define XIAOTAI_INTENT_ROOM_CONNECT "room.connect"
#define XIAOTAI_INTENT_ROOM_DISCONNECT "room.disconnect"
#define XIAOTAI_INTENT_ROOM_TALK "room.talk"

/* The foreground activity this module begins (the priority table's key). */
#define XIAOTAI_ROOM_ACTIVITY "room"

/* -------------------------------------------------------------- diagnostics */

/* Counters since boot. */
typedef struct {
    uint32_t syncs, sync_failures;          /* assignment reads */
    uint32_t assignments_stale;             /* answers older than the version held */
    uint32_t tokens, token_failures;
    uint32_t connects, connect_failures;    /* links requested, links that never opened */
    uint32_t joins, join_failures;
    uint32_t heartbeats, heartbeat_failures;
    uint32_t lease_expired;
    uint32_t snapshots, snapshots_rejected; /* member snapshots taken, refused as malformed */
    uint32_t members_dropped;               /* members not kept for lack of room */
    uint32_t signals_sent, signals_failed;  /* commands to the room */
    uint32_t commands_dropped;              /* commands from the room too large to keep */
    uint32_t releases, release_failures;    /* reservations released at the server */
    uint32_t preempted;                     /* conversations ended by a revoked activity */
    uint32_t activity_busy;                 /* connection attempts postponed because the device was not free */
    uint32_t talk_presses, talk_refused, talk_keepalive_releases;
    uint32_t stale;                         /* replies, signals and requests dropped for an old generation, ticket or attendance */
    uint32_t requests_lost;                 /* requests whose reply never came */
} xiaotai_room_stats_t;

/* [any] */
void xiaotai_room_stats(xiaotai_room_stats_t *stats);

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: declares what it needs from the platform, transport (one link, the
 * command share above) and media engines, registers its intents, signal
 * subscriptions, the quiesce guard "room", the AT+ROOM command and the
 * diagnostics section "room". start: applies the configuration and allocates
 * the member list. Neither touches the network. */
xiaotai_err_t xiaotai_module_room_init(void);
void xiaotai_module_room_start(void);
/* Host tests: forget configuration, state and counters. */
void xiaotai_room_reset(void);

#ifdef __cplusplus
}
#endif
