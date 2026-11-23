// SPDX-License-Identifier: MIT
#pragma once
/* Transport engine: the RTC stack that every link (xiaotai/link.h) runs on.
 *
 * This header is the engine as a whole: the product's configuration, what the
 * selected modules need from it, when the stack runs, requests to the RTC
 * service, and diagnostics. Which RTC package is linked is decided by the
 * build, per chip and ESP-IDF line; nothing here names it.
 *
 * Running. The engine runs the stack by itself. It brings it up when the
 * device has a valid identity (xiaotai/identity.h) and the RTC service
 * endpoint is discovered (xiaotai/platform.h), with the account made of
 * exactly these; it brings it down and up again when that account changes,
 * and keeps it down while a hold exists (xiaotai_transport_hold), while the
 * platform's maintenance gate is closed at the moment it would start, and,
 * when the product says so, while the device is unbound or (at the moment it
 * would start) while the platform session is not online. Modules and
 * applications never start or stop the stack; they read the state, take a
 * hold when they need its memory, and ask for a restart after a fault.
 *
 * Stops and faults. When the stack reports by itself that it stopped while
 * the engine did not ask for it, every link ends with
 * XIAOTAI_LINK_END_STOPPED, the stack is released (its uninit, on the worker)
 * and the engine recovers as the product's unsolicited_stop says. The stack
 * misbehaves - and the engine enters XIAOTAI_TRANSPORT_FAULT - when stopping
 * fails or a stop is not confirmed within release_timeout_ms, and, unless the
 * product tolerates release failures, when releasing a connection fails or a
 * released connection or a withdrawn request is still held by the stack
 * after release_timeout_ms. The product may also make an event overflow, a
 * connect timeout, a failed start or an unsolicited stop a fault (see the
 * configuration).
 * Entering FAULT ends every link with XIAOTAI_LINK_END_STOPPED and brings a
 * stack that is still initialized down exactly as a requested stop does
 * (stop, its STOPPED, uninit on the worker), so a faulted stack never stays
 * online or holds its memory; FAULT then stays latched. The one exception is
 * a fault that is itself a stop that failed or was not confirmed: uninit
 * only ever follows STOPPED, so that stack stays initialized in FAULT until a
 * restart asks it to stop again. FAULT is left only by
 * xiaotai_transport_restart() or, where the product sets it, by
 * fault_restart_ms.
 *
 * Provisioning boot. When the network engine reports a boot reserved for
 * provisioning (xiaotai_net_provisioning_boot()), the engine stays DOWN with
 * wait XIAOTAI_TRANSPORT_WAIT_PROVISIONING for the whole boot: start()
 * creates no worker and allocates nothing (no link table, event ring, command
 * pool or start reserve), no stack is started, every link request fails with
 * XIAOTAI_ERR_INVALID_STATE and nothing is admitted. Requirements, listeners
 * (checked and numbered; their table is not taken) and holds are still
 * accepted, so modules need no special case. Boards without external RAM
 * need that memory for the access point and the portal.
 *
 * Tasks. One worker task makes every call into the RTC stack that can take
 * time or stack (start, stop, connect, release, service requests) and keeps
 * the engine's timers. Media and commands are sent on the caller's task; the
 * worker never sends media. Thread tags: [init], [owner], [any] and [rt] as
 * defined in xiaotai/framework.h, and [uplink] as defined in xiaotai/link.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

/* Most links any product may have at once. */
#define XIAOTAI_TRANSPORT_LINKS_MAX 4U

/* An option of the RTC stack that the product may force either way. */
typedef enum {
    XIAOTAI_TRANSPORT_STACK_DEFAULT = 0,  /* leave it as the stack ships it */
    XIAOTAI_TRANSPORT_OFF,
    XIAOTAI_TRANSPORT_ON,
} xiaotai_transport_switch_t;

/* What the engine does after a start failed or the stack stopped by itself.
 * Appended, never renumbered. */
typedef enum {
    /* Start again after start_retry_ms while the start conditions hold. */
    XIAOTAI_TRANSPORT_RECOVER_RETRY = 0,
    /* Start again as soon as the start conditions hold (no wait). */
    XIAOTAI_TRANSPORT_RECOVER_AT_ONCE,
    /* Stay down (wait XIAOTAI_TRANSPORT_WAIT_START_FAILED) until a start
     * condition changes - a platform or network event, a hold given back -
     * or xiaotai_transport_restart() is called; then start again. */
    XIAOTAI_TRANSPORT_RECOVER_ON_EVENT,
    /* Enter XIAOTAI_TRANSPORT_FAULT; fault_restart_ms decides what follows. */
    XIAOTAI_TRANSPORT_RECOVER_FAULT,
} xiaotai_transport_recovery_t;

typedef enum {
    XIAOTAI_TRANSPORT_COMMAND_POOL_DEFAULT = 0, /* resident with external RAM; hybrid without */
    XIAOTAI_TRANSPORT_COMMAND_POOL_RESIDENT,
    XIAOTAI_TRANSPORT_COMMAND_POOL_ON_DEMAND,
} xiaotai_transport_command_pool_mode_t;

/* Product configuration: what differs between products without being a need
 * of a module. Every field: 0 (false) selects the stated default, so a zeroed
 * struct is a valid configuration. The values describe the product's memory
 * and network path, never a board or a chip by name. Fields under "rules
 * kept per product" exist so that every product keeps the behaviour of its
 * deployed firmware; their zero is the rule the engine applies when nothing
 * older has to be kept. */
typedef struct {
    /* --- capacity --- */
    /* Links the product wants at once beyond what its modules need by
     * themselves: 2 lets an inbound link be admitted while another link is
     * open (the newcomer's module then asks the framework which activity
     * wins). Default 1. The engine is sized for the larger of this and the
     * modules' requirements, at most XIAOTAI_TRANSPORT_LINKS_MAX. */
    uint8_t links;
    /* When the peer withdraws the active audio send stream, also revoke the
     * module's send permission synchronously. Default: keep independent gates. */
    bool audio_unsubscribe_revokes_permit;
    /* Ceiling for the bytes the RTC stack may hold unsent; a module
     * requirement above it is XIAOTAI_ERR_INVALID_SIZE. Default: 2 MiB with
     * external RAM, 16 KiB without. */
    uint32_t send_buffer_max_bytes;
    /* Audio packets are refused (XIAOTAI_ERR_BUSY, counted) while their link
     * already holds at least this many unsent bytes: the bound on how late
     * speech may leave a device whose network stalls. 0: only the stack's own
     * limit. */
    uint32_t audio_backlog_limit_bytes;
    /* Commands and peer subscriptions that may wait for the owner task at
     * once, over all links. Default: 16 with external RAM, 8 without. */
    uint8_t event_slots;
    /* Bound on command payloads (xiaotai/link.h, "Commands"), independent of
     * storage mode. 0: the largest declared share times the number of links.
     * At most XIAOTAI_LINK_COMMAND_LIMIT_BYTES times XIAOTAI_TRANSPORT_LINKS_MAX. */
    uint32_t command_pool_bytes;
    /* Resident: one BULK block held until reset. On demand: one bounded BULK
     * copy per received command, freed after delivery or discard. Hybrid
     * reserves up to 2048 payload bytes at start and uses the same on-demand
     * path when a whole payload does not fit. All modes use the same limits. */
    xiaotai_transport_command_pool_mode_t command_pool_mode;
    /* Stack of the worker task, placed in BULK memory. It must hold the RTC
     * stack's connect and service-request calls: a package that builds its
     * connect offer on the caller's stack needs more than 8 KiB for that
     * alone. Default: 24576 with external RAM, 16384 without; a product
     * lowers it only after a bench reading of worker_stack_free over every
     * link kind and failure path. */
    uint32_t worker_stack_bytes;
    /* RTOS priority of the worker task (xiaotai/port/os.h numbering). 0: the
     * framework owner task's priority (XIAOTAI_FRAMEWORK_OWNER_PRIORITY), so
     * the worker never preempts the owner it hands events to and stays below
     * every media task. At most 24. */
    uint8_t worker_priority;
    /* Prefer one-frame uplink dispatch on the RTC worker for an eligible
     * capture pipeline. Default: enabled without external RAM. OFF keeps the
     * independent uplink task; unsupported media topologies keep it as well. */
    xiaotai_transport_switch_t media_uplink_worker;

    /* --- contiguous internal memory the RTC stack needs --- */
    /* Taken from INTERNAL memory when the network has started
     * (XIAOTAI_EVENT_NET_STATE left STOPPED and PREPARING: the network stack
     * has made its own start-up allocations) and before the platform session
     * begins, so signaling and TLS cannot fragment the heap first; given up
     * the moment the stack is started. This is the order deployed firmware
     * boots in. The network starts its driver only after system.started, so
     * the reserve is taken after every engine's start; it is taken on the
     * owner task inside the handler of that net.state event, which the
     * network emits before XIAOTAI_EVENT_NET_UP, the event the platform
     * session waits for, so the order holds without any wait. Never taken in
     * a provisioning boot. 0: none. */
    uint32_t start_reserve_bytes;
    /* Taken once the stack is online - before the hold of
     * start_before_signaling is given back, so signaling and TLS cannot take
     * that memory first - given up for each outbound request and taken back
     * when the request has opened or ended. While it cannot be taken (back),
     * outbound requests fail with XIAOTAI_ERR_NO_MEM and the engine retries
     * every 5000 ms. 0: none. */
    uint32_t connect_reserve_bytes;

    /* --- sharing memory with the platform's signaling connection --- */
    /* Keep signaling suspended (xiaotai_platform_signaling_hold) from the
     * moment the stack is started until it is online, so the two do not
     * compete for the memory the start reserve gave up. Order when the stack
     * comes online: the connect reserve is attempted first (a miss is counted
     * and retried), only then is the hold given back. */
    bool start_before_signaling;
    /* Longest that hold lasts when the stack does not come online: then
     * signaling is given back and the stack keeps trying. 0: the hold lasts
     * until the stack is online or the start is abandoned. */
    uint32_t start_signaling_hold_max_ms;
    /* Suspend signaling while an outbound request is connecting when, at the
     * request, INTERNAL memory has less than yield_connect_free_bytes free or
     * no block of yield_connect_block_bytes. Both 0: never. Signals sent
     * meanwhile are lost to the device (see the platform's signaling hold). */
    uint32_t yield_connect_free_bytes;
    uint32_t yield_connect_block_bytes;
    /* Suspend signaling for as long as any link is open. */
    bool yield_while_open;
    /* Bring the stack down while the device is unbound (the platform reports
     * that a binding is needed), so a binding has its memory. */
    bool down_while_unbound;

    /* --- behaviour of the RTC stack --- */
    /* Startup-only listener and queue stacks of an audited vendor package.
     * 0 leaves vendor values intact; otherwise 4096..8192, aligned to 16.
     * Generated product glue defaults to 4096 without external RAM.
     * Unsupported packages, external RAM and per-session tasks are unchanged. */
    uint32_t vendor_idle_stack_bytes;
    /* Poll interval of the stack's media transport, 1..50 ms. 0: the stack's own. */
    uint8_t poll_timeout_ms;
    /* Cache of connection parameters per peer inside the stack. */
    xiaotai_transport_switch_t connect_cache;
    /* The stack's optional media transport; a package without it ignores this. */
    xiaotai_transport_switch_t media_transport;
    /* 0: the stack logs nothing. 1..5: its log level; its text is never
     * printed (it may hold addresses and tokens): known lines are turned into
     * counters and one-line facts, everything else is counted by size. */
    uint8_t stack_log_level;
    /* With media_transport ON: enable the optional media transport before
     * the stack's init instead of after it. */
    bool media_transport_before_init;
    /* String options are passed to the stack without their terminating NUL
     * (the length the package expects). false: length includes the NUL. */
    bool option_strings_without_nul;
    /* A package without TLS gets the RTC service's official https entries
     * rewritten to http (other hosts, ports, paths and look-alikes are left
     * alone). false: the discovered entry is passed as it is. */
    bool endpoint_http_rewrite;
    /* Do not start without a discovered service endpoint (wait
     * XIAOTAI_TRANSPORT_WAIT_ENDPOINT for as long as discovery has none).
     * false: without one the stack's built-in entry is used. */
    bool endpoint_required;
    /* An endpoint equal to the package's built-in entry, in its http or its
     * https spelling, is not set as an option, so the stack uses its built-in
     * entry. false: every endpoint is set as it is (after
     * endpoint_http_rewrite), so an https entry stays https, or fails to start
     * on a package without TLS. */
    bool endpoint_default_unset;

    /* --- recovery --- */
    /* Wait before a failed start is tried again (start_failure RETRY, and
     * unsolicited_stop RETRY). Default 5000. */
    uint32_t start_retry_ms;
    /* What follows a failed start. Default RETRY. */
    xiaotai_transport_recovery_t start_failure;
    /* What follows a stop the stack reported without being asked (after the
     * links ended and the stack was released). Default RETRY. */
    xiaotai_transport_recovery_t unsolicited_stop;
    /* Wait, counted from entering the fault, after which the engine leaves
     * XIAOTAI_TRANSPORT_FAULT by itself as xiaotai_transport_restart() would
     * (start once the stack is released; ask an unreleased stack to stop
     * again). 0: it stays in XIAOTAI_TRANSPORT_FAULT until
     * xiaotai_transport_restart(), whoever calls it and on whatever trigger. */
    uint32_t fault_restart_ms;
    /* Longest the stack may take to finish releasing a connection (its close
     * report, or the late result of a withdrawn request, or its pending count
     * reaching 0 where requests can be cancelled), and to confirm a stop.
     * Longer is a fault (see the top of this header). 1000..600000; default
     * 35000. */
    uint32_t release_timeout_ms;

    /* --- rules kept per product --- */
    /* Fail an environment switch before writing when an unsolicited stack
     * stop is recorded during that switch. Requested STOP acknowledgements
     * and existing transport faults do not invalidate the hold. */
    bool fail_environment_on_unsolicited_stop;
    /* A failed release (the stack refuses to disconnect) or a release not
     * finished within release_timeout_ms is counted (release_stalls) and the
     * record dropped, never a fault; a new outbound request is submitted at
     * once instead of after earlier releases. A late result of a withdrawn
     * request is still disconnected when it arrives. false: both are faults
     * and requests wait for earlier releases (link.h, xiaotai_link_connect). */
    bool release_failures_tolerated;
    /* An event the ring cannot keep is a fault of the engine: the link it
     * belongs to ends with XIAOTAI_LINK_END_OVERFLOW, every other with
     * XIAOTAI_LINK_END_STOPPED, and the engine enters
     * XIAOTAI_TRANSPORT_FAULT. false: only that link ends. */
    bool overflow_fault;
    /* Recover the affected link on a real on-demand command allocation failure.
     * Size and capacity rejections still deliver the existing dropped event. */
    bool command_oom_ends_link;
    /* An outbound request whose timeout_ms (xiaotai/link.h) ends before the
     * stack reported its result is a fault of the stack: the request's module
     * gets closed(XIAOTAI_LINK_END_TIMEOUT) as always, and the engine enters
     * XIAOTAI_TRANSPORT_FAULT (so the stack is brought down) instead of
     * withdrawing the request and waiting for its late result. Requests ended
     * by xiaotai_link_disconnect() are not affected. false: withdraw. */
    bool connect_timeout_fault;
    /* Commands and peer subscriptions that arrive before an outbound link's
     * connect result are dropped and counted (early_dropped) instead of being
     * delivered after opened() (xiaotai/link.h, "Callbacks"). */
    bool early_events_dropped;
    /* A ONCE listener is disarmed by an inbound link it could not take for
     * lack of a free link (xiaotai/link.h, xiaotai_link_listen_set). false:
     * it stays armed. */
    bool once_consumed_on_refusal;
    /* Record the generation of an unguarded OPEN that claimed a slot but was
     * closed by LISTEN_OFF before the owner could accept it. Only a diagnostic;
     * it does not make the retired link usable or run its opened callback. */
    bool report_retired_open;
    /* Consecutive audio sends the stack fails for a reason other than
     * congestion after which the link ends with XIAOTAI_LINK_END_NETWORK
     * (detail: the last failure's code); a success resets the count. 0: a
     * failed send only drops that packet. */
    uint8_t audio_failures_end;
    /* A received audio frame in a format outside the link's receive_formats
     * ends that link with XIAOTAI_LINK_END_FORMAT (the frame is dropped and
     * counted as always). false: it is only dropped and counted. */
    bool format_error_ends_link;
    /* On a default media receiver that accepts PCMA 8 kHz mono, also admit
     * Opus 16 kHz mono if the media engine prepared it. Local RX only;
     * it neither adds a subscription nor changes a negotiated or TX format. */
    bool accept_pcma_opus_downlink;
    /* The stack's alert that its service requests were redirected (counted
     * in alerts) ends every link with XIAOTAI_LINK_END_ALERT; the stack keeps
     * running. false: only counted and recorded. */
    bool alert_ends_links;
    /* Start the stack only while the platform session is online
     * (xiaotai/platform.h, phase XIAOTAI_PLATFORM_ONLINE: signaling
     * subscribed); wait XIAOTAI_TRANSPORT_WAIT_PLATFORM meanwhile. Like the
     * maintenance gate it only gates a start: a session that goes offline
     * later does not stop a running stack. false: the stack starts once
     * identity, endpoint and network allow, independently of the session
     * (start_before_signaling orders the two). */
    bool start_after_online;
    /* With start_before_signaling and connect_reserve_bytes: once the stack
     * is online, the start's signaling hold is kept until the connect reserve
     * has been taken (retried every 5000 ms), so signaling and TLS never start
     * on memory the reserve still needs. start_signaling_hold_max_ms still
     * bounds the hold. false: the hold is given back right after the first
     * attempt, whether the reserve was taken or not. */
    bool start_hold_until_reserve;
    /* Preserve an error reported as peer closure separately from an orderly
     * close callback. Default false: both are delivered as PEER. */
    bool distinguish_reported_peer_close;
} xiaotai_transport_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; the
 * struct is copied. NULL or never called: all defaults. INVALID_ARG for a
 * value outside its range (links above XIAOTAI_TRANSPORT_LINKS_MAX,
 * poll_timeout_ms above 50, stack_log_level above 5, a worker stack below
 * 4096, an invalid vendor_idle_stack_bytes, worker_priority above 24,
 * one yield threshold without the other,
 * release_timeout_ms other than 0 outside 1000..600000, an unknown
 * command_pool_mode, command_pool_bytes
 * above its bound, a recovery value this header does not list,
 * media_transport_before_init without media_transport ON); INVALID_STATE
 * after the engine's init. */
xiaotai_err_t xiaotai_transport_configure(const xiaotai_transport_config_t *config);

/* ------------------------------------------------------------ requirements */

/* What a module needs from the engine. The engine is sized once, at start
 * (after every module has initialized), for the largest value of each field. */
typedef struct {
    /* Bytes the RTC stack may hold unsent for this module's uplink; a module
     * that sends large frames (video key frames) declares the largest burst it
     * may hand over. 0: no opinion; the engine then uses a default that
     * depends only on whether the board has external RAM (256 KiB with,
     * 16 KiB without). */
    uint32_t send_buffer_bytes;
    /* Links the module itself needs at the same time, counting requested,
     * open and not yet released ones. 0: no opinion (1). */
    uint8_t links;
    /* The largest command_pool_bytes (xiaotai/link.h) of any link this module
     * requests or admits, with the profile's default applied. Sizes the
     * engine's command pool unless the product sets it. 0: twice
     * XIAOTAI_LINK_COMMAND_DEFAULT_BYTES. */
    uint32_t command_pool_bytes;
} xiaotai_transport_requirement_t;

/* [init] Declared by modules before the engine starts. INVALID_ARG: NULL, or
 * links above XIAOTAI_TRANSPORT_LINKS_MAX; INVALID_SIZE: send_buffer_bytes
 * above the product's ceiling (a product that cannot run this module), or
 * command_pool_bytes above XIAOTAI_LINK_COMMAND_LIMIT_BYTES; INVALID_STATE
 * once the engine has been sized. Accepted in a provisioning boot too (and
 * then not used). */
xiaotai_err_t xiaotai_transport_require(const xiaotai_transport_requirement_t *requirement);

/* ------------------------------------------------------------------- state */

typedef enum {
    XIAOTAI_TRANSPORT_DOWN = 0,   /* not running: nothing to run with yet, held, or brought down */
    XIAOTAI_TRANSPORT_STARTING,   /* registering with the RTC service; retried until it succeeds or is brought down */
    XIAOTAI_TRANSPORT_ONLINE,     /* links can be requested and admitted */
    XIAOTAI_TRANSPORT_STOPPING,   /* bounded by release_timeout_ms, then FAULT */
    /* the stack misbehaved; it is brought down and released (unless its stop
     * itself failed) and the state is latched; see the top of this header */
    XIAOTAI_TRANSPORT_FAULT,
} xiaotai_transport_state_t;

/* Framework events (xiaotai/framework.h), emitted on every change, in the
 * order things happened; a handler reads xiaotai_transport_state() for the
 * present truth. args.value: the new xiaotai_transport_state_t. */
#define XIAOTAI_EVENT_TRANSPORT_STATE "transport.state"
/* args.value: links requested, open or not yet released; 0 means the engine
 * holds no link and no memory of one. */
#define XIAOTAI_EVENT_TRANSPORT_LINKS "transport.links"

/* [rt] */
xiaotai_transport_state_t xiaotai_transport_state(void);

/* ------------------------------------------------------------- run control */

/* [any] Keeps the stack down while at least one hold exists: every link ends
 * with XIAOTAI_LINK_END_STOPPED, the state event reports STOPPING, then DOWN,
 * and the stack's memory is free from then on. For work that cannot share
 * memory with the stack (a firmware update, a provisioning portal on a board
 * without external RAM). Returns without waiting; *hold is never 0. A hold
 * may be taken before the engine starts. NO_MEM when too many holds exist. */
xiaotai_err_t xiaotai_transport_hold(uint32_t *hold);

/* [any] Gives a hold back; with the last one the engine runs again by its own
 * rules. Unknown and already released holds are ignored. */
void xiaotai_transport_release(uint32_t hold);

/* [any] Brings the stack down and up again with the current account: every
 * link ends with XIAOTAI_LINK_END_STOPPED. The way out of
 * XIAOTAI_TRANSPORT_FAULT and of XIAOTAI_TRANSPORT_WAIT_START_FAILED; a
 * caller that only wants that way out (not to bounce a healthy stack) calls
 * it only in those two states. In FAULT: called while the fault's stop is
 * still running, it takes effect once the stack is released; with a stack
 * whose stop failed or was not confirmed, it asks it to stop again first.
 * The start that follows waits for the start conditions like any other (a
 * closed maintenance gate defers it). INVALID_STATE while the engine is down
 * for a reason of its own (no account, a hold, a provisioning boot): it
 * starts by itself when that ends. */
xiaotai_err_t xiaotai_transport_restart(void);

/* ---------------------------------------------------------- service request */

#define XIAOTAI_TRANSPORT_SERVICE_PATH_MAX 96U    /* including the NUL */
#define XIAOTAI_TRANSPORT_SERVICE_BODY_MAX 4096U  /* including the NUL */
/* Requests that may be accepted and not yet answered. */
#define XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX 2U

/* [owner] result: XIAOTAI_OK when the service accepted the request, FAIL when
 * it answered with an error or could not be reached, INVALID_STATE when the
 * engine went down first. status: the HTTP status or the service's own error
 * code, 0 when accepted; for diagnostics. body: the answer when there is one,
 * else NULL; terminated; valid only during the call. */
typedef void (*xiaotai_transport_reply_fn_t)(void *ctx, xiaotai_err_t result, int status, const char *body,
                                             size_t size);

/* [any] Sends a request to the RTC service as this device (no link needed):
 * path starts with '/', json is the body (NULL: a request without body). Both
 * are copied before the call returns. Requests run one at a time on the
 * worker, which serves nothing else meanwhile: use it for short requests that
 * end something a peer is waiting on. On XIAOTAI_OK reply (may be NULL) is
 * called exactly once. INVALID_ARG for a path without '/'; INVALID_STATE
 * unless ONLINE; BUSY when XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX are waiting;
 * INVALID_SIZE past the limits above; NO_MEM when the copies cannot be made. */
xiaotai_err_t xiaotai_transport_service_request(const char *path, const char *json,
                                                xiaotai_transport_reply_fn_t reply, void *ctx);

/* -------------------------------------------------------------- diagnostics */

/* Why the engine is not online. */
typedef enum {
    XIAOTAI_TRANSPORT_WAIT_NONE = 0,     /* online, or on its way */
    XIAOTAI_TRANSPORT_WAIT_IDENTITY,     /* no valid identity */
    XIAOTAI_TRANSPORT_WAIT_ENDPOINT,     /* the RTC service endpoint is not discovered */
    XIAOTAI_TRANSPORT_WAIT_NETWORK,      /* the network has not reported its address yet */
    XIAOTAI_TRANSPORT_WAIT_HOLD,         /* a hold exists */
    XIAOTAI_TRANSPORT_WAIT_MAINTENANCE,  /* the platform's maintenance gate is closed */
    XIAOTAI_TRANSPORT_WAIT_UNBOUND,      /* down_while_unbound and a binding is needed */
    XIAOTAI_TRANSPORT_WAIT_RETRY,        /* a start failed; the next try is scheduled */
    XIAOTAI_TRANSPORT_WAIT_FAULT,        /* in FAULT: xiaotai_transport_restart() or fault_restart_ms */
    XIAOTAI_TRANSPORT_WAIT_PROVISIONING, /* a provisioning boot: down until the device restarts */
    /* XIAOTAI_TRANSPORT_RECOVER_ON_EVENT after a failed start (start_failure)
     * or a stop the stack made by itself (unsolicited_stop): down until a
     * start condition changes or xiaotai_transport_restart() */
    XIAOTAI_TRANSPORT_WAIT_START_FAILED,
    /* start_after_online and the platform session is not online */
    XIAOTAI_TRANSPORT_WAIT_PLATFORM,
} xiaotai_transport_wait_t;

/* A snapshot; fields are read one by one, not as one transaction. */
typedef struct {
    xiaotai_transport_state_t state;
    xiaotai_transport_wait_t wait;
    uint8_t holds;             /* holds in effect */
    uint8_t links_max;         /* links this engine was sized for */
    uint8_t links_requested;   /* outbound, not open yet */
    uint8_t links_open;
    uint8_t links_releasing;   /* ended for the module, still being released by the stack */
    /* An outbound request made now would not fail for state, space or memory. */
    bool connect_ready;
    bool start_reserve_held, connect_reserve_held;
    bool signaling_suspended;        /* a signaling hold of this engine is in effect */
    uint32_t send_buffer_bytes;      /* the stack's limit in effect */
    uint32_t starts, start_failures, stops, faults;
    uint32_t unsolicited_stops;      /* the stack stopped without being asked */
    /* Releases or stops not finished within release_timeout_ms, and releases
     * the stack refused. A stop is always a fault; a release is one unless
     * release_failures_tolerated. */
    uint32_t release_stalls;
    int last_start_detail;           /* the stack's code of the last failed start; 0: none */
    int last_fault_detail;           /* the stack's code that led to the last fault; 0: none */
    uint32_t inbound_admitted;
    uint32_t inbound_refused;        /* no listener armed, or no link free */
    uint32_t late_links_closed;      /* opened after their request was withdrawn */
    uint32_t early_dropped;          /* commands and subscriptions that arrived before a connect result
                                        of a link that then never opened, or that no request could own */
    uint32_t stale_calls;            /* calls naming a generation that was not live */
    uint32_t send_call_max_us;       /* longest single media send or backlog call into the stack */
    uint32_t events_peak;            /* most events that waited for the owner task at once */
    uint32_t events_lost;            /* links ended with XIAOTAI_LINK_END_OVERFLOW */
    uint32_t connect_memory_misses;  /* requests refused because connect memory was not in place */
    uint32_t signaling_yields;       /* times signaling was suspended for a start or a link */
    uint32_t service_requests, service_failures;
    uint32_t alerts;                 /* the stack reported tampering with its service requests */
    uint32_t stack_log_bytes;        /* stack log text counted and not printed */
    uint32_t worker_stack_free;      /* least free worker stack seen, bytes */
    uint32_t command_pool_bytes;     /* payload-byte limit in effect; 0 if storage setup failed */
    uint32_t command_pool_peak;      /* most payload bytes held in the pool at once, over all links */
    uint32_t command_largest;        /* largest command payload received, kept or dropped */
    /* Memory the engine itself holds now (link table, listener table, event
     * ring, command pool, work queue, worker stack, reserves; not the RTC
     * stack's own):
     * the transport's line of the internal-memory ledger. 0 in a
     * provisioning boot. */
    uint32_t engine_bytes;
} xiaotai_transport_status_t;

/* [any] */
void xiaotai_transport_status(xiaotai_transport_status_t *status);

/* [any] Version of the linked RTC stack; a static string. */
const char *xiaotai_transport_version(void);

/* ---------------------------------------------------------------- lifecycle */

/* Engine lifecycle (called by the framework through the generated tables).
 * init: registers the diagnostics provider and the commands, subscribes to the
 * network's and the platform's events (the start reserve is taken on the
 * network's start, see start_reserve_bytes; on XIAOTAI_EVENT_NET_TRACE the
 * worker turns the RTC stack's statistics on exactly while
 * xiaotai_net_trace_transport_active() is true and feeds each sample to
 * xiaotai_net_trace_transport(), with media 1 for audio and 2 for video,
 * because those statistics perturb weak-network timing); in a build with the
 * option XIAOTAI_TRANSPORT_MEMTRACE (xiaotai/transport_config.h) it also
 * starts the allocation trace of the RTC stack, before the stack's first
 * allocation, and registers AT+MEMTRACE. Init allocates nothing.
 * start: in a provisioning boot only records it (see the top of this
 * header); otherwise sizes the engine for the declared requirements, takes
 * the link table, event ring and command pool storage, creates the worker, and from
 * then on runs the stack by the rules at the top of this header. */
xiaotai_err_t xiaotai_engine_transport_init(void);
void xiaotai_engine_transport_start(void);
/* Host tests: bring the stack down, stop the worker, release everything and
 * forget configuration, requirements and listeners. */
void xiaotai_transport_reset(void);

#ifdef __cplusplus
}
#endif
