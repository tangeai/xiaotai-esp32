// SPDX-License-Identifier: MIT
#pragma once
/* Internal interface of the portable transport core (src/). Not installed.
 *
 * One engine object (xt) holds everything; its parts belong to the files of
 * the core as the specification splits them:
 *   transport.c    configuration, requirements, sizing, state, status, lifecycle
 *   run_control.c  worker task and its jobs, run rules, account, start and
 *                  stop, holds, restart, faults, both reserves, signaling holds
 *   links.c        link slots and release records (engine-wide operations here;
 *                  per-link requests, timeouts and releases join with the link
 *                  package)
 *   inbound.c, events.c, send.c, receive.c, commands.c, service.c,
 *   diagnostics.c, memtrace.c   the other work packages
 * The RTC package is reached only through transport_backend.h.
 *
 * Tasks.
 *   owner   the framework owner task: event handlers, connect, disconnect
 *   worker  xt_transport: every backend call that may take time or stack,
 *           every timer; it alone changes xt.run
 *   stack   the RTC stack's threads: upcalls set atomic bits or ring entries
 *           and kick the worker; on-demand control commands copy from the heap
 * Fields say which task writes them; atomics may be read anywhere. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "transport_backend.h"
#include "xiaotai/link.h"
#include "xiaotai/media_uplink.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XT_TAG "transport"
/* Event-log sources (xiaotai/eventlog.h): the engine, and single links. */
#define XT_LOG_SOURCE "transport"
#define XT_LOG_SOURCE_LINK "link"
#define XT_WORKER_NAME "xt_transport"

/* ------------------------------------------------------------ fixed values */

#define XT_HOLDS_MAX 8U
#define XT_JOB_DEPTH 8U
#define XT_SERVICE_REPLY_RETRY_MS 10U
#define XT_EVENT_ITEM_BYTES 16U
#define XT_START_RETRY_DEFAULT_MS 5000U
#define XT_RELEASE_TIMEOUT_DEFAULT_MS 35000U
#define XT_RELEASE_TIMEOUT_MIN_MS 1000U
#define XT_RELEASE_TIMEOUT_MAX_MS 600000U
/* The connect reserve: first attempt after the stack is online, retried every
 * XT_RESERVE_RETRY_MS; after a connect it is taken back every
 * XT_RESTORE_RETRY_MS until it is in place again. */
#define XT_RESERVE_RETRY_MS 5000U
#define XT_RESTORE_RETRY_MS 1000U
/* The worker re-judges the run rules at least this often. */
#define XT_EVALUATE_PERIOD_MS 1000U
/* A framework event the owner queue refused is posted again after this. */
#define XT_EVENT_RETRY_MS 10U
#define XT_STATE_EVENTS_PENDING 8U
#define XT_WORKER_STACK_MIN 4096U
#define XT_WORKER_PRIORITY_MAX 24U
#define XT_POLL_TIMEOUT_MAX_MS 50U
#define XT_STACK_LOG_LEVEL_MAX 5U

/* Defaults that depend only on external RAM (xiaotai_mem_has_external()). */
#define XT_SEND_BUFFER_CEILING_EXTERNAL (2U * 1024U * 1024U)
#define XT_SEND_BUFFER_CEILING_INTERNAL (16U * 1024U)
#define XT_SEND_BUFFER_DEFAULT_EXTERNAL (256U * 1024U)
#define XT_SEND_BUFFER_DEFAULT_INTERNAL (16U * 1024U)
#define XT_EVENT_SLOTS_EXTERNAL 16U
#define XT_EVENT_SLOTS_INTERNAL 8U
#define XT_COMMAND_RESIDENT_COMPACT_BYTES 2048U
#define XT_WORKER_STACK_EXTERNAL 24576U
#define XT_WORKER_STACK_INTERNAL 16384U

/* -------------------------------------------------------- platform pieces */

/* Monotonic milliseconds of the engine's timers. esp-idf/clock.c reads the
 * port clock; host/script_backend.c can run a manual clock for tests. */
int64_t xt_now_ms(void);
/* How long the worker may block when its next deadline is delta_ms away
 * (delta_ms >= 1). A manual host clock blocks until kicked instead. */
uint32_t xt_clock_wait_ms(int64_t delta_ms);

/* ------------------------------------------------------------------- jobs */

/* Work handed to the worker. fn runs on the worker; NULL only wakes it. */
typedef struct {
    void (*fn)(uint32_t a, uint32_t b);
    uint32_t a, b;
} xt_job_t;

/* [any, rt] Queues a job without waiting; false when the queue is full or the
 * worker does not exist (nothing queued). */
bool xt_worker_post(void (*fn)(uint32_t a, uint32_t b), uint32_t a, uint32_t b);

enum { XT_DIAG_RX, XT_DIAG_TX, XT_DIAG_DISPATCH };
void xt_diag_command_locked(unsigned direction, uint32_t generation, uint32_t word, uint32_t bytes, int result);
xiaotai_err_t xt_diag_init(void);
void xt_diag_reset(void);
void xt_diag_link(const char *label, uint32_t generation, int detail, bool warning);
bool xt_service_replies(void);
void xt_service_reset(void);
/* [any, rt] Wakes the worker so it looks at upcall bits and timers. */
void xt_worker_kick(void);
/* [worker] The worker wakes no later than at_ms for the caller's timer (the
 * request is forgotten after the next pass, so a caller with a standing
 * deadline asks again on every pass, see xt_links_tick). */
void xt_worker_wake_at(int64_t at_ms);
/* [any] True on the worker task. */
bool xt_on_worker(void);
/* Host tests: true while the worker waits with nothing queued, no upcall bit
 * set, no event to post and no deadline reached. */
bool xt_worker_settled(void);

/* ------------------------------------------------------------ link slots */

typedef enum {
    XT_SLOT_FREE = 0,       /* no live link (counters of the last one kept) */
    XT_SLOT_REQUESTED,      /* outbound, waiting for the connect result */
    XT_SLOT_OPEN,
} xt_slot_state_t;

/* Flags of a slot (xt_slot_t.flags). */
enum {
    XT_SLOT_ENDED = 1u << 0,         /* ended for the module; closed() still to be delivered */
    XT_SLOT_OPENED = 1u << 1,        /* opened() still to be delivered */
    XT_SLOT_ERROR = 1u << 2,         /* the worker must release it (stack error, failures) */
    XT_SLOT_REPORTED = 1u << 3,      /* owner handed an open link to its module */
    XT_SLOT_FINISHING = 1u << 4,     /* terminal command claimed an unreported open */
    XT_SLOT_WAS_OPEN = 1u << 5,      /* ended after a physical connect */
    XT_SLOT_CLOSE_CONFIRMED = 1u << 6, /* native close acknowledged for this ended generation */
    XT_SLOT_REPORT_RETIRE_OPEN = 1u << 7, /* unguarded OPEN with admission audit enabled */
    XT_SLOT_PEER_RETIREMENT_TAGGED = 1u << 8, /* opted-in peer object carries this generation */
};

typedef struct xt_connect_args {
    struct xt_connect_args *next_submitted;
    uint32_t generation;
    size_t bytes;
    size_t token_offset;
    char text[];
} xt_connect_args_t;

typedef struct {
    xiaotai_link_receive_fn_t fn;
    void *ctx;
    uint32_t revision;
    bool on, subscribed, stop_pending;
} xt_receiver_t;

/* One link place. The link package (links.c, inbound.c, send.c, receive.c,
 * commands.c) appends what it needs; run control uses the fields below. */
typedef struct {
    _Atomic uint32_t generation;     /* 0: no live link; written under xt.lock */
    _Atomic uint8_t state;           /* xt_slot_state_t; written under xt.lock */
    _Atomic uint32_t flags;
    xiaotai_link_kind_t kind;
    xiaotai_rtc_conn_t conn;         /* 0 until the stack named the connection */
    uint32_t request;                /* number given to xiaotai_rtc_connect; 0 inbound */
    xiaotai_link_handlers_t handlers;
    xiaotai_link_profile_t profile;
    xt_connect_args_t *args;         /* taken by the worker before calling the stack */
    union {
        int64_t deadline;            /* outbound request timeout; 0 means no timeout */
        uint32_t predecessor;       /* inbound admission waits for this acknowledged terminal event */
    } opening;
    uint32_t listener_id;            /* 0 outbound */
    bool guarded_open;
    uint16_t uses;
    xiaotai_link_stats_t stats;
    xt_receiver_t rx[2];
    _Atomic uint32_t send_ready[2];   /* generation when the gate is open */
    _Atomic uint32_t key_frame_for;
    _Atomic uint32_t bitrate_sequence, bitrate_consumed, bitrate_bps, bitrate_for;
    bool tx_busy[2], video_needs_key, bitrate_enabled;
    uint8_t audio_failures;
    uint32_t video_key_epoch;
    uint32_t receive_accepted, receive_rejected;
    /* Why and with which stack code the link ended (valid with XT_SLOT_ENDED). */
    xiaotai_link_end_t end_reason;
    int end_detail;
    uint32_t ended_generation;       /* the generation closed() is for */
} xt_slot_t;

/* A connection the stack still holds after its link ended for the module. */
typedef struct {
    xiaotai_rtc_conn_t conn;         /* 0 with request set: a withdrawn request */
    uint32_t request;
    int64_t since_ms;                /* release_timeout_ms counts from here */
    int64_t poll_at;
    uint32_t listener_id;            /* 0 outbound */
    uint32_t generation;
    uint16_t uses;
    bool disconnect_sent, cancel_sent, closed, strict_retire;
    bool used;
} xt_release_t;

typedef struct {
    xiaotai_link_listener_t listener;
    xiaotai_link_listen_t mode;
} xt_listener_t;

typedef struct {
    uint32_t generation, value, size, meta;
} xt_event_t;

typedef struct xt_command_payload xt_command_payload_t;

_Static_assert(sizeof(xt_event_t) == XT_EVENT_ITEM_BYTES, "transport event ring item size");

/* [worker, start] Allocates xt.links_max slots and as many release records
 * (one INTERNAL block: the slots hold atomics). */
xiaotai_err_t xt_links_alloc(void);
void xt_links_free(void);
/* [worker] Ends every live link for its module with this reason (closed()
 * follows on the owner task; nothing for links this side disconnected).
 * Called by run control for STOPPED, faults, alerts. */
void xt_links_end_all(xiaotai_link_end_t reason, int detail);
/* [worker] Disconnects every connection the stack still holds for an ended
 * link, before the stack itself is stopped. */
void xt_links_release_all(void);
/* [worker] The stack released everything by itself (it stopped): forget the
 * release records without calling it. */
void xt_links_stack_released(void);
/* [any] Links requested, open, and still being released. */
void xt_links_count(uint8_t *requested, uint8_t *open, uint8_t *releasing);
/* [worker] Timers of the link package (request timeouts, release limits);
 * called on every pass of the worker. */
void xt_links_tick(int64_t now_ms);
/* Under xt.lock unless indicated otherwise. */
xt_slot_t *xt_link_free_locked(void);
bool xt_link_physical_room_locked(void);
void xt_link_open_inbound_locked(xt_slot_t *slot, xiaotai_rtc_conn_t conn, uint32_t listener_id,
                                  const xt_listener_t *listener);
void xt_link_tag_peer_retirement(xiaotai_rtc_conn_t conn, uint32_t generation, xiaotai_rtc_peer_tag_fn tag);
void xt_link_refuse_locked(xiaotai_rtc_conn_t conn);
xt_connect_args_t *xt_link_end_locked(xt_slot_t *slot, xiaotai_link_end_t reason, int detail,
                                     bool silent, bool closed);
bool xt_inbound_open_allowed_locked(const xt_slot_t *slot);
xiaotai_err_t xt_link_profile_check(const xiaotai_link_profile_t *profile);
void xt_link_work(void); /* [any, stack] wake for slot change, never call while locked */
xt_slot_t *xt_link_event_slot_locked(xiaotai_rtc_conn_t conn, uint32_t request, bool *early);
void xt_link_events_drop_locked(uint32_t generation);
void xt_link_events_tick(void); /* [worker] try scheduling owner drain */
void xt_command_payloads_release(void); /* start failure or quiescent reset */
void xt_listeners_free(void);
bool xt_media_kind(xiaotai_link_media_kind_t kind);
xt_slot_t *xt_link_generation_locked(uint32_t generation);
xt_slot_t *xt_link_stats_locked(uint32_t generation);
xt_slot_t *xt_link_conn_locked(xiaotai_rtc_conn_t conn);
bool xt_link_open_locked(const xt_slot_t *slot);
bool xt_link_pin_locked(xt_slot_t *slot);
xiaotai_err_t xt_link_finish_opening(uint32_t generation, bool *opened, bool pin, xiaotai_rtc_conn_t *conn);
void xt_link_unpin(uint32_t generation);
xiaotai_link_media_stats_t *xt_media_stats(xt_slot_t *slot, xiaotai_link_media_kind_t kind);
const xiaotai_link_media_t *xt_profile_media(const xt_slot_t *slot, xiaotai_link_media_kind_t kind);
void xt_queue_link_error_locked(xt_slot_t *slot, xiaotai_link_end_t reason, int detail);
void xt_send_sync_gate_locked(xt_slot_t *slot, xiaotai_link_media_kind_t kind);
void xt_send_peer_locked(xt_slot_t *slot, xiaotai_link_media_kind_t kind, uint8_t stream, bool on);
void xt_audio_sink_init(void);
extern const xiaotai_media_uplink_executor_t xt_audio_executor;
void xt_receive_topics_init(void);
bool xt_receive_busy(uint32_t generation);
void xt_receive_wait(uint32_t generation, xiaotai_link_media_kind_t kind);
void xt_receive_stop_deferred(void);
bool xt_control_begin(uint32_t generation);
void xt_control_end(void);

/* ------------------------------------------------------ run control (WP1) */

/* The worker's view of the RTC stack. */
typedef enum {
    XT_STACK_DOWN = 0,      /* uninitialized */
    XT_STACK_STARTING,      /* start accepted, STARTED not seen */
    XT_STACK_UP,            /* STARTED seen */
    XT_STACK_STOPPING,      /* stop accepted, STOPPED not seen yet */
    XT_STACK_STUCK,         /* a stop failed or was not confirmed: still initialized */
} xt_stack_t;

/* What led to a fault (event-log value of "fault"). */
typedef enum {
    XT_FAULT_STOP_FAILED = 1,
    XT_FAULT_STOP_TIMEOUT,
    XT_FAULT_RELEASE_FAILED,
    XT_FAULT_RELEASE_TIMEOUT,
    XT_FAULT_OVERFLOW,
    XT_FAULT_CONNECT_TIMEOUT,
    XT_FAULT_START_FAILED,
    XT_FAULT_UNSOLICITED_STOP,
    XT_FAULT_NO_MEMORY,
} xt_fault_t;

/* Upcall bits (xt.upcalls), set by xiaotai_rtc_on_system. */
enum {
    XT_UP_STARTED = 1u << 0,
    XT_UP_STOPPED = 1u << 1,
    XT_UP_ALERT = 1u << 2,
};

/* State that only the worker changes. */
typedef struct {
    xt_stack_t stack;
    bool fault;                /* latched FAULT */
    bool restart_pending;      /* restart() in FAULT: takes effect once the stack is released */
    bool restart_after_stop;   /* a restart's stop of a stuck stack is under way */
    bool bounce;               /* restart() while running: stop, then start again */
    bool start_failed;         /* RECOVER_ON_EVENT latch (WAIT_START_FAILED) */
    bool prestart;             /* waiting for the start's signaling hold to take effect */
    int64_t retry_at;          /* RECOVER_RETRY: no start before; 0 none */
    int64_t fault_restart_at;  /* fault_restart_ms deadline; 0 none */
    int64_t stop_deadline;     /* STOPPING must see STOPPED before; 0 none */
    int64_t next_evaluate;
    int64_t wake_at;           /* earliest deadline asked through xt_worker_wake_at */
    /* Signaling holds of the engine (xiaotai_platform_signaling_hold). The
     * tag passed as the callback context tells a late "suspended" of an older
     * hold from the current one. */
    uint32_t start_hold, start_hold_tag;
    bool start_hold_suspended;
    int64_t start_hold_until;  /* start_signaling_hold_max_ms deadline; 0 none */
    uint32_t link_hold, link_hold_tag;
    uint32_t connect_wait_generation; /* still queued; owns the pre-submit hold */
    bool yield_connect;        /* the link hold is (also) for a connect */
    bool yield_open;           /* the link hold is (also) for open links */
    uint32_t hold_tags;
    /* Connect reserve: held, or lent to this request with its optional
     * signaling hold. Worker writes the owner under xt.lock. */
    void *connect_reserve;
    uint32_t connect_loan_generation;
    int64_t connect_retry_at;  /* next attempt to take it (back); 0 none */
    uint64_t account;          /* digest of the account the stack runs with */
    bool statistics;           /* the stack's statistics are on */
    bool gated;                /* the maintenance gate as last seen */
    bool drain_to_stop, stopped_pending;
    xiaotai_transport_wait_t drain_wait;
    int stopped_detail;
    uint32_t conditions_seen;  /* xt.conditions value last acted on */
    uint8_t mac[6];
    bool mac_valid;
    uint8_t network_kind;
} xt_run_t;

/* Counters of xiaotai_transport_status_t that several files update. */
typedef struct {
    _Atomic uint32_t starts, start_failures, stops, faults, unsolicited_stops, release_stalls;
    _Atomic int last_start_detail, last_fault_detail;
    _Atomic uint32_t inbound_admitted, inbound_refused, late_links_closed, early_dropped, stale_calls;
    _Atomic uint32_t send_call_max_us, events_peak, events_lost, connect_memory_misses;
    _Atomic uint32_t signaling_yields, service_requests, service_failures, alerts;
    _Atomic uint32_t command_pool_peak, command_largest;
    _Atomic uint32_t reserve_misses;   /* start or connect reserve not taken (event "reserve_miss") */
    _Atomic uint32_t media_tx, media_rx, media_tx_dropped, media_rx_dropped;
} xt_counters_t;

typedef struct {
    /* [glue, init] */
    xiaotai_transport_config_t config;
    struct {
        uint32_t send_buffer_bytes;
        uint32_t command_pool_bytes;   /* largest declared share; 0: none declared */
        uint8_t links;
    } need;
    _Atomic bool initialized;
    _Atomic bool sized;                /* start() ran: requirements are closed */
    _Atomic bool provisioning;         /* this boot is a provisioning boot */
    bool audio_executor_attached;       /* written at init; never by the worker */
    atomic_bool audio_wake_posted, audio_pending;
    bool worker_created;               /* reset must consume this worker's exit ack */

    /* Sizing (written by start, read-only afterwards). */
    bool external, command_on_demand;
    uint8_t links_max;
    uint8_t event_slots;
    uint32_t send_buffer_bytes, send_buffer_ceiling;
    uint32_t command_pool_bytes;       /* 0 when the pool could not be taken */
    uint32_t command_resident_bytes;   /* payload bytes in the startup reservation */
    uint32_t worker_stack_bytes;
    unsigned worker_priority;
    uint32_t release_timeout_ms, start_retry_ms;

    /* Memory the engine holds. */
    xt_slot_t *slots;                  /* links_max, INTERNAL (atomics) */
    xt_release_t *releases;            /* links_max, same block */
    uint8_t *event_ring;               /* event_slots * XT_EVENT_ITEM_BYTES, INTERNAL */
    uint8_t *command_pool;             /* usable bytes plus NUL storage, BULK */
    xt_command_payload_t **command_slots; /* on demand: event_slots + owner, under xt.lock */
    xt_command_payload_t *command_retired; /* xt.lock: payloads detached from their events */
    uint32_t command_retired_bytes;    /* not reusable until the retired payloads are freed */
    uint32_t command_tail;             /* xt.lock: next reservation starts here */
    _Atomic uint32_t engine_bytes;

    /* Synchronization. lock: a few instructions (slot state, reserves,
     * holds); control: backend control calls, waited for at most
     * XIAOTAI_LINK_CONTROL_WAIT_MS. */
    xiaotai_critical_t lock;
    xiaotai_mutex_t control;
    bool sync_ready;

    /* Worker. */
    xiaotai_queue_t jobs;
    bool jobs_ready;
    xiaotai_task_t worker;
    _Atomic bool worker_running;
    _Atomic bool worker_quit;
    _Atomic bool worker_waiting;
    _Atomic uint32_t service_epoch;
    _Atomic uint32_t worker_stack_free;
    bool service_reserved[XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX]; /* under lock */
    bool service_reply_retry;     /* worker-owned */
    xiaotai_signal_t worker_exited;
    xiaotai_task_config_t worker_config;   /* as created: diagnostics and tests */

    /* Public state. */
    _Atomic int state;                 /* xiaotai_transport_state_t */
    _Atomic int wait;                  /* xiaotai_transport_wait_t */
    _Atomic uint32_t upcalls;          /* XT_UP_* */
    _Atomic int stop_detail;
    uint32_t environment_epoch, stop_environment_epoch; /* xt.lock; odd while held */
    uint32_t classifying_stop_epoch;                    /* xt.lock; 0 when none */
    xiaotai_err_t environment_error;                    /* xt.lock */

    /* Holds ([any], lock-free: a hold may be taken before init). */
    _Atomic uint32_t holds[XT_HOLDS_MAX];
    _Atomic uint32_t hold_next;
    _Atomic uint8_t hold_count;
    uint8_t holds_seen;                /* [worker] hold_count last acted on */
    bool stop_request_committed, stop_report_expected; /* xt.lock: native STOP ordering */
    _Atomic uint32_t restarts;         /* xiaotai_transport_restart() calls accepted */
    uint32_t restarts_seen;            /* [worker] */
    /* Odd tag of the start hold whose "suspended" arrived. */
    _Atomic uint32_t suspended_start_tag;

    /* Start reserve: taken on the owner task when the network started, given
     * up by the worker; the pointer is swapped under lock. stack_active is
     * set by the worker before it gives the reserve up for a start and
     * cleared when the stack is released, so the owner never takes it while
     * the stack runs. */
    void *start_reserve;
    bool stack_active;
    _Atomic bool net_started;

    /* Latches the owner's event handlers set. */
    _Atomic bool unbound;              /* binding needed, until the next platform online */
    _Atomic uint32_t conditions;       /* advanced by every platform or network event */
    _Atomic bool trace_changed;

    /* [worker] Framework events still to be posted, oldest first. */
    struct {
        bool links;                    /* transport.links; else transport.state */
        int32_t value;
    } pending[XT_STATE_EVENTS_PENDING];
    uint8_t pending_count;
    uint32_t pending_dropped;
    int64_t event_retry_at;            /* real time (xiaotai_time_us / 1000); 0 none */
    bool links_event_pending;          /* coalesced current count, never evicted by states */
    uint16_t paused_open;              /* xt.lock: same-listener pauses of new OPEN admissions */

    xt_listener_t *listeners;
    uint32_t listener_count;
    uint16_t inbound_blocks;           /* one bit for each registered listener */
    uint16_t open_blocks;              /* same-listener OPEN admission gates */
    xt_release_t refused[XIAOTAI_TRANSPORT_LINKS_MAX];
    uint16_t orphan_listeners;         /* only cleared by a confirmed stack release */
    bool orphan_link_inputs;            /* xt.lock: release overflow lost ownership tracking */
    _Atomic bool admission_closed;
    _Atomic bool links_dirty, lifecycle_overflow;
    _Atomic uint32_t connect_result_generation;
    _Atomic uint32_t backend_users;
    _Atomic uint32_t receiving[2];    /* generation of the one active receive callback per kind */
    unsigned sending;                 /* xt.lock: internal-memory variant admits one uplink call */
    bool owner_drain_posted;            /* xt.lock */
    int64_t owner_drain_retry_at;      /* worker only, real clock */
    uint8_t event_count;               /* xt.lock */
    xt_event_t owner_event;            /* keeps borrowed command data reserved during a callback */

    xt_run_t run;
    xt_counters_t counters;
} xt_engine_t;

extern xt_engine_t xt;

/* Each queued payload and the owner callback's borrowed payload may need
 * one terminator beyond the advertised payload-byte budget. */
static inline uint32_t xt_command_storage_bytes(void)
{
    return xt.command_resident_bytes + xt.event_slots + 1U;
}

/* Run-control entry points (run_control.c). */
xiaotai_err_t xt_run_init(void);       /* [init] subscriptions */
xiaotai_err_t xt_run_start(void);      /* [owner] start(): queue, holds, worker */
bool xt_run_stop_worker(void);         /* false if the worker still owns resources */

/* [worker] Enters FAULT (see xiaotai/transport.h): every link ends with
 * XIAOTAI_LINK_END_STOPPED, an initialized stack is stopped and released.
 * The link package calls it for release failures and timeouts (unless
 * release_failures_tolerated), overflows (overflow_fault) and connect
 * timeouts (connect_timeout_fault). */
void xt_fault(xt_fault_t cause, int detail);

/* [any] The engine accepts outbound requests now (ONLINE, not a provisioning
 * boot): XIAOTAI_OK, else XIAOTAI_ERR_INVALID_STATE. */
xiaotai_err_t xt_link_admissible(void);
/* [owner] For xiaotai_link_connect: XIAOTAI_OK when the memory the stack
 * needs to connect is in place (the connect reserve, when the product has
 * one, is held and not lent); otherwise XIAOTAI_ERR_NO_MEM, counted in
 * connect_memory_misses. */
xiaotai_err_t xt_connect_memory_check(void);
/* [any] Same question without counting (status.connect_ready). */
bool xt_connect_memory_ready(void);
/* [worker] Before RTC connect, request signaling suspension when required.
 * READY lends the reserve; WAIT leaves the unsent generation queued for a
 * later pass; FAILED leaves the reserve intact and returns the failure in
 * error so the caller can close that unsent generation locally. */
typedef enum {XT_CONNECT_MEMORY_READY, XT_CONNECT_MEMORY_WAIT, XT_CONNECT_MEMORY_FAILED} xt_connect_memory_t;
xt_connect_memory_t xt_connect_memory_lend(uint32_t generation, xiaotai_err_t *error);
/* [worker] Give up the hold for an unsent generation removed by cancellation or timeout. */
void xt_connect_memory_abandon_unsubmitted(void);
/* [worker] After the owner's result, cancellation proof or abandoned submit:
 * takes the reserve back (retried every XT_RESTORE_RETRY_MS) and then gives
 * signaling back unless yield_while_open keeps it. Other generations do nothing. */
void xt_connect_memory_restore(uint32_t generation);
/* [worker] The number of open or releasing links changed: applies
 * yield_while_open (signaling held from the first open link until no link
 * is requested, open or releasing). */
void xt_run_links_changed(void);
/* [worker] Posts transport.links with the current count (in order with
 * transport.state, retried when the owner queue is full). */
void xt_publish_links(void);

/* Accounting of xt.engine_bytes. */
static inline void xt_account(int64_t delta)
{
    if (delta >= 0) {
        atomic_fetch_add(&xt.engine_bytes, (uint32_t)delta);
    } else {
        atomic_fetch_sub(&xt.engine_bytes, (uint32_t)(-delta));
    }
}

#ifdef __cplusplus
}
#endif
