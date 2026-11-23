// SPDX-License-Identifier: MIT
#pragma once
/* Platform core: the device's session with the XiaoTai cloud, offered to
 * modules as channels.
 *
 * What it does on its own: waits for the network, synchronizes the clock once
 * per boot, discovers the service endpoints, logs the device in with its
 * identity (xiaotai/identity.h), keeps the signaling connection subscribed and
 * alive, reports the device profile, and follows the binding (a server-side
 * unbind ends the session and advances the binding generation).
 *
 * What modules do through it:
 *   - send their own HTTP requests to a discovered service
 *     (xiaotai_platform_request) or stream a large upload or download
 *     (xiaotai_platform_transfer);
 *   - subscribe to signaling messages by type (xiaotai_platform_signal_subscribe);
 *   - read an endpoint and connect to it themselves (the transport engine reads
 *     "tirtc"; this component never touches a real-time link);
 *   - contribute their scene of the device profile;
 *   - run a binding over a temporary signaling session (xiaotai_platform_guest_*).
 * The platform core knows no module, no request path of a module and no signal
 * type except "unbind".
 *
 * Tasks. One platform worker task runs the session steps and every HTTP
 * exchange, one at a time. Signaling arrives on the signaling port's task and
 * is copied into a bounded pool there (no logging: counters; no allocation
 * where the board has external RAM and the pool is preallocated).
 * Every handler a module registers runs on the framework owner task unless
 * marked otherwise. Tags: [init] from a module's or application's init();
 * [owner] the framework owner task; [any] any task, never from an interrupt;
 * [worker] the platform worker (callbacks only): copy or produce bytes and
 * return, never wait for the owner task and never call this header's functions.
 *
 * Generation. Requests and signals are stamped with the binding generation
 * (xiaotai_identity_generation(), never 0) current when they were accepted. A
 * request whose generation is no longer current, or whose session ended before
 * it completed, is answered with XIAOTAI_ERR_PLATFORM_STALE and no body; a
 * signal of an older generation is dropped. Both are counted.
 *
 * Provisioning boot. When the network engine reports a boot reserved for
 * provisioning (xiaotai_net_provisioning_boot()), the platform core stays
 * STOPPED for the whole boot and allocates nothing at all: init only reads
 * the stored environment into static storage and passes the time
 * configuration to the clock port (no synchronization follows, since no
 * session ever starts); registrations (signal subscriptions, profile scenes)
 * are accepted and not kept, because this boot never delivers a signal or
 * reports a profile; start() creates no task and no pool; link events are
 * ignored; and every request, transfer and guest session is refused with
 * XIAOTAI_ERR_PLATFORM_OFFLINE. Boards without external RAM need that memory
 * for the access point and the portal.
 *
 * Events (platform.*) are never lost to a full owner queue: an event that
 * cannot be posted is latched and re-posted (see XIAOTAI_EVENT_PLATFORM_*).
 *
 * Product behaviour. Where deployed products of different families behave
 * differently on the wire (how the subscription is sent, what counts as an
 * authentication failure, token renewal, the profile's retries and fields,
 * connection handling), the configuration has one field per difference, and
 * each product sets the value of its current firmware (the specification's
 * per-product table). Defaults are the values of the products with external
 * RAM unless a field says otherwise.
 *
 * Results are xiaotai_err_t. Transport URLs follow the project policy of the
 * deployed firmware: http:// and https:// service addresses are accepted and
 * used as http://. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ results */

/* Platform results (SDK-specific range 0x7100..0x71FF). */
#define XIAOTAI_ERR_PLATFORM_OFFLINE 0x7101   /* no session that could carry this (not discovered, not logged in) */
#define XIAOTAI_ERR_PLATFORM_STALE 0x7102     /* the binding or session it belonged to has ended */
#define XIAOTAI_ERR_PLATFORM_GATED 0x7103     /* the maintenance gate is closed */
#define XIAOTAI_ERR_PLATFORM_CANCELLED 0x7104 /* cancelled by its submitter */
/* The discovery document names the service, but its address failed admission
 * (only possible for a service nobody required: a required one fails discovery). */
#define XIAOTAI_ERR_PLATFORM_BAD_SERVICE 0x7105

/* Business codes of the reply envelope {"code":…,"msg":…,"data":…} that the
 * platform core itself acts on. Modules interpret every other code. */
#define XIAOTAI_PLATFORM_CODE_OK 200
#define XIAOTAI_PLATFORM_CODE_UNBOUND 6006
/* reply.code when the body has no single integral "code". */
#define XIAOTAI_PLATFORM_NO_CODE INT32_MIN

/* ------------------------------------------------------------------- limits */

/* With terminator. */
#define XIAOTAI_PLATFORM_URL_MAX 256U
/* Longest environment base (so base + "/services" fits a URL). */
#define XIAOTAI_PLATFORM_BASE_MAX 246U
/* With terminator. Holds the longest path in use: a speech download
 * "/v1/recognition/requests/<64>/audio/<64>" is 160 characters. */
#define XIAOTAI_PLATFORM_PATH_MAX 192U
#define XIAOTAI_PLATFORM_SERVICE_NAME_MAX 24U
#define XIAOTAI_PLATFORM_CONTENT_TYPE_MAX 96U
#define XIAOTAI_PLATFORM_BEARER_MAX 1024U
#define XIAOTAI_PLATFORM_SCENE_NAME_MAX 16U

#define XIAOTAI_PLATFORM_TIMEOUT_DEFAULT_MS 15000U
#define XIAOTAI_PLATFORM_TIMEOUT_MAX_MS 60000U
#define XIAOTAI_PLATFORM_TRANSFER_TIMEOUT_MAX_MS 120000U

/* Service names: the discovery document's keys without their "-srv" suffix.
 * Any other key of that form can be read by its name; these are the ones in
 * use today. "device" and "mqtt" are always required.
 * Admission of addresses (http:// or https://, mqtt:// or mqtts:// for "mqtt"
 * unless mqtt_address_unchecked) decides per service: a required service
 * with an absent or inadmissible address fails the whole discovery; any
 * other service with an inadmissible
 * value (wrong type, too long, bad address) is only marked unusable, counted
 * (services_invalid) and logged, and discovery succeeds for the rest. */
#define XIAOTAI_PLATFORM_SERVICE_DEVICE "device"
#define XIAOTAI_PLATFORM_SERVICE_MQTT "mqtt"
#define XIAOTAI_PLATFORM_SERVICE_AI "ai"
#define XIAOTAI_PLATFORM_SERVICE_CALL "call"
#define XIAOTAI_PLATFORM_SERVICE_VOIP "voip"
#define XIAOTAI_PLATFORM_SERVICE_TIRTC "tirtc"
#define XIAOTAI_PLATFORM_SERVICE_RECOGNITION "recognition"

/* ------------------------------------------------------------ configuration */

/* retry_ms value that disables automatic retries. */
#define XIAOTAI_PLATFORM_NO_RETRY UINT32_MAX

/* Least time between two logins the platform core starts by itself after an
 * authentication failure or for a token renewal (XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN). */
#define XIAOTAI_PLATFORM_RELOGIN_INTERVAL_MS 5000U

/* What the server's refusal of the credentials means: a refused signaling
 * connection, a subscription answered 0x80, HTTP 401 on a session request or
 * on a session transfer. Each value is the behaviour of deployed firmware of
 * one product family; the reply handler of a refused request still receives
 * its 401 under every value. */
typedef enum {
    /* Not an authentication failure (default): a refused connection is left
     * to the signaling port's reconnection, a 0x80 counts as one failed
     * subscription attempt (retried like any other), a 401 only reaches the
     * handler. */
    XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE = 0,
    /* The session ends (OFFLINE reason AUTH, signaling stopped, token wiped)
     * and the platform enters PAUSED (last_error XIAOTAI_FAIL) whatever
     * retry_ms says: nothing logs in again until xiaotai_platform_retry(). */
    XIAOTAI_PLATFORM_AUTH_FAILURE_PAUSE,
    /* The session ends as with PAUSE; the device then logs in again at once
     * when more than XIAOTAI_PLATFORM_RELOGIN_INTERVAL_MS passed since the
     * last login the platform core started by itself (an earlier login of
     * this rule or a token renewal; a boot, a link-up or retry() is not
     * one), and otherwise enters PAUSED as with PAUSE. */
    XIAOTAI_PLATFORM_AUTH_FAILURE_RELOGIN,
} xiaotai_platform_auth_failure_t;

/* Product configuration. Every numeric field: 0 selects the default. Strings
 * are not copied and must stay valid (static storage). Capacities are
 * ceilings; the pools are sized at start for the largest need modules
 * declared (xiaotai_platform_require), within these ceilings. With external
 * RAM all of it is allocated at start; without, only the request slots are,
 * and response and signal storage is taken per exchange and per message
 * within the same limits, so an idle device holds none of it.
 * A default that depends on external RAM says so; the others are the values
 * deployed firmware with external RAM uses. What each product sets is listed
 * in the component's specification (per-product table). */
typedef struct {
    /* Discovery URL built into the product. A stored environment base
     * (xiaotai_platform_environment_store) replaces it from the next boot. */
    const char *discovery_url;
    /* Admit the "mqtt" service address as the discovery document gives it
     * (any non-empty string shorter than XIAOTAI_PLATFORM_URL_MAX), without
     * the mqtt:// / mqtts:// check, and hand it to the signaling port
     * unchanged. Default false: an address of another form fails discovery
     * like any inadmissible required service. */
    bool mqtt_address_unchecked;

    /* Time. In a product with this engine these fields are the only place
     * time is configured: init passes them to xiaotai_clock_init (the
     * engines initialize before every module, so the clock module finds the
     * port initialized and only watches its gate), and the engine synchronizes
     * (xiaotai_clock_sync) after each link-up until it succeeded once. */
    /* POSIX time zone the clock port starts with; NULL: "CST-8". A clock
     * module that offers the time zone as a setting starts from this rule,
     * which it reads back with xiaotai_clock_timezone (xiaotai/port/clock.h);
     * there is no second default. */
    const char *timezone;
    /* Time servers by host name, in order of preference, at most
     * XIAOTAI_CLOCK_SERVERS_MAX (xiaotai/port/clock.h). NULL / 0: the two
     * public servers deployed firmware uses. */
    const char *const *time_servers;
    size_t time_server_count;
    /* Release the time service's sockets and timers after each successful
     * synchronization (boards short of internal RAM). Default false: it keeps
     * running so the clock follows drift. */
    bool clock_release_after_sync;
    /* xiaotai_clock_config_t.probe: ask every time server at once before
     * waiting for the time service. Default false. */
    bool clock_probe;
    /* xiaotai_clock_config_t.trust_kept_time: a plausible time kept across a
     * reset passes the clock gate without a synchronization in this boot.
     * Default false: only a synchronization in this boot opens the gate. */
    bool clock_trust_kept_time;
    /* Device profile "hardware" object, from the board package. Both set, or
     * both NULL to leave the object out. Sent verbatim: the server knows a
     * device model by this exact text (spaces, case and punctuation
     * included), so nothing is trimmed, case-folded or escaped. Each is 1..63
     * printable ASCII characters other than '"' and '\\' (the two that would
     * need escaping in JSON); anything else is XIAOTAI_ERR_INVALID_ARG. */
    const char *chip_model;
    const char *board_model;

    /* Wait before a failed session step (clock, discovery, login) is tried
     * again. Default 5000. XIAOTAI_PLATFORM_NO_RETRY: stay paused until
     * xiaotai_platform_retry(). */
    uint32_t retry_ms;
    /* How long an idle HTTP connection is kept for the next request to the
     * same host. Default 0: kept only while another request is already
     * waiting, and never across a request flagged XIAOTAI_PLATFORM_RELEASE. */
    uint32_t idle_keep_ms;
    /* A GET that fails on a reused connection before any response arrived is
     * sent once more on a new connection within its timeout. Default false:
     * the failure goes to the handler. POST, PUT and DELETE are never sent
     * twice either way. */
    bool get_retry_on_reuse;
    /* Disable Nagle's algorithm on the platform's HTTP connections
     * (xiaotai_http_options_t.no_delay). Default false. */
    bool http_no_delay;
    /* Return TIMEOUT immediately when an ordinary HTTP request or an opted-in
     * transfer finds no free slot. Default false: return NO_MEM. Allocation
     * failures remain NO_MEM. */
    bool request_full_timeout;
    /* Session requests (XIAOTAI_PLATFORM_AUTH_SESSION) are admitted and
     * completed only while signaling is up; a signaling drop ends the session
     * for them. Requests that need no session (binding) are not affected. For
     * boards that cannot afford an HTTP exchange beside a reconnecting
     * signaling link. */
    bool requests_need_signaling;
    /* Renew the session token before it expires: a token whose expiry can be
     * read (xiaotai_platform_token_expiry) ends the session at expiry minus
     * min(60 s, a quarter of its remaining life) and logs in again (OFFLINE
     * reason REFRESH). Default false: a token is kept for the whole session. */
    bool token_refresh;
    /* Refuse to go online with a token that carries no future expiry time. */
    bool token_needs_expiry;
    /* Handling of a refusal of the credentials (see the type). Default
     * XIAOTAI_PLATFORM_AUTH_FAILURE_IGNORE. */
    xiaotai_platform_auth_failure_t auth_failure;
    /* Subscribe the two device topics with one SUBSCRIBE each instead of one
     * SUBSCRIBE carrying both. Readiness, deadline and retries are the same:
     * both acknowledged (0 or 1) within subscribe_timeout_s of the attempt. */
    bool subscribe_per_topic;
    /* A well-formed signal refused because every pool slot was taken is only
     * counted and left to the server's redelivery. Default false: the binding
     * is also verified by a new login, because the refused signal may have
     * been an unbind. Other drops never verify the binding (see
     * xiaotai_platform_signal_subscribe). */
    bool tolerate_signal_loss;
    /* Legacy signaling policies: restrict unbind to cmd; do not ACK a
     * complete but malformed command. Both are false by default. */
    bool unbind_command_only;
    bool drop_malformed_commands;
    uint16_t unbind_puback_wait_ms; /* 0: no wait; otherwise wait before destroying the old link */
    /* While a foreground activity exists (XIAOTAI_EVENT_ACTIVITY names one),
     * keep TLS work away from the real-time link: the profile report waits
     * until the activity ended, and requests flagged
     * XIAOTAI_PLATFORM_BACKGROUND are refused (XIAOTAI_ERR_BUSY). For boards
     * without external RAM, where an HTTPS exchange beside a real-time link
     * can exhaust internal RAM. Default false. */
    bool defer_background_while_active;

    /* Device profile reports. Attempts per session (the first included);
     * default 3, 1 means no automatic retry (a module may still call
     * xiaotai_platform_profile_refresh, which starts a new round). Retried,
     * 15000 ms apart: no status, 429, 5xx; with
     * profile_retry_business_errors also HTTP 200 whose business code is
     * missing or 50000. */
    uint8_t profile_attempts;
    bool profile_retry_business_errors;
    /* Leave "firmware_version" out of the profile document (devices whose
     * deployed firmware never sent it). Default false: it is sent. */
    bool profile_omit_firmware_version;
    /* Wait for an external readiness owner before submitting the profile.
     * Default false; a product can retain its transport-before-profile order. */
    bool profile_requires_ready;

    uint16_t keepalive_s;          /* signaling keep-alive; default 60 */
    uint16_t reconnect_ms;         /* signaling reconnect interval; default 3000 */
    uint16_t subscribe_timeout_s;  /* wait for a subscription acknowledgement; default 12 */
    uint16_t heartbeat_s;          /* default 30 */
    /* Start the heartbeat period at boot and advance it only while ready.
     * The other mode advances its sequence even across disconnected periods. */
    bool heartbeat_when_ready_only;
    /* On final release of a hold, log in again instead of reusing a
     * still-valid token. Retains the deployed release policy where needed. */
    bool hold_relogin_on_release;

    /* Most request slots; default 4 with external RAM, otherwise 2. */
    uint8_t request_slots;
    uint8_t signal_slots;          /* default 8 with external RAM, otherwise 3 */
    uint16_t request_body_bytes;   /* default 2048 with external RAM, otherwise 768 */
    uint16_t response_bytes;       /* default 8192 */
    /* Default 8192: deployed signaling messages reach 8191 bytes (a call
     * invitation with its tokens), and a module declaring that size must
     * start on every board. It is a ceiling: with external RAM the default
     * pool is signal_slots x the largest declared size. Without external RAM
     * a message takes its own size when it arrives, with at most 12 KiB in
     * flight together. */
    uint16_t signal_bytes;
    uint16_t signaling_buffer_bytes; /* signaling client's receive and send buffers; default: the port's */
    uint16_t signaling_outbox_bytes; /* most bytes the signaling client queues for sending; default: the port's */
    uint16_t signaling_stack_bytes;  /* signaling client's task stack; default: the port's */
    uint32_t worker_stack_bytes;   /* default 13312 with external RAM, otherwise 8192 */
    /* RTOS priority of the worker. 0: one below the framework owner task's
     * (XIAOTAI_FRAMEWORK_OWNER_PRIORITY - 1, at least 1), so the worker and
     * the task that handles its replies never share time slices; deployed
     * firmware runs its platform task below the task that handles platform
     * replies. Equal to the owner's only where a product does that today.
     * Never above the owner's: a higher value is lowered to it and logged at
     * start, because the worker waits for the owner's reply handlers and must
     * not starve the owner task. */
    uint8_t worker_priority;
    /* With external RAM, take each signal payload at its first fragment and
     * return it when retired. Slot count and per-message ceiling are unchanged.
     * Default false keeps the external payload arena resident. */
    bool signal_payload_on_demand;
    /* Reserve one existing signal slot for a validated unbind on /cmd.
     * Default false; a one-slot pool cannot use this policy. */
    bool reserve_unbind_signal_slot;
} xiaotai_platform_config_t;

/* What one module needs from the pools. */
typedef struct {
    uint16_t request_body_bytes;   /* largest JSON body it submits, with terminator */
    uint16_t response_bytes;       /* largest response body it expects from xiaotai_platform_request */
    uint16_t signal_bytes;         /* largest signaling message it handles */
    uint8_t concurrent_requests;   /* requests it would like to have waiting at once */
} xiaotai_platform_requirement_t;

/* [glue] Before xiaotai_framework_start(), by the product glue. NULL or
 * never called: all defaults and no discovery URL (the platform then stays
 * stopped unless an environment base is stored). XIAOTAI_ERR_INVALID_ARG for a
 * malformed URL or model text; XIAOTAI_ERR_INVALID_STATE after the engine's init. */
xiaotai_err_t xiaotai_platform_configure(const xiaotai_platform_config_t *config);

/* [init] The largest declared value wins for every field. Sizes are hard
 * limits: a size above its ceiling is XIAOTAI_ERR_INVALID_SIZE (names a
 * product that cannot run this module). concurrent_requests is a wish: the
 * pool gets max(largest declared, 1) slots, clamped to request_slots; a clamp
 * is counted (slot_clamps) and is not an error, because every submitter
 * handles a full table by trying again later (see request_full_timeout). */
xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *requirement);

/* [init] This service must be in the discovery document with an admissible
 * address, or discovery fails. Services nobody requires are optional: reading
 * or addressing an absent one gives XIAOTAI_ERR_NOT_SUPPORTED, an inadmissible
 * one XIAOTAI_ERR_PLATFORM_BAD_SERVICE; other services are not affected. */
xiaotai_err_t xiaotai_platform_require_service(const char *service);

/* Engine lifecycle (called by the framework through the generated tables).
 * init: reads the stored environment, initializes the clock port with the
 * configured servers and time zone, subscribes to the network engine's link
 * events and to XIAOTAI_EVENT_ACTIVITY, registers its commands and the
 * diagnostics section "platform". A clock port that is already initialized
 * (host tests, firmware without the generated product glue) is left as it
 * is. start: allocates the pools for the declared needs and starts the
 * worker; in a provisioning boot it does neither. Neither touches the
 * network: the session begins with the first link-up.
 *
 * Commands (declared in the component's manifest):
 *   AT+PLATFORM        same content as AT+STATUS platform:
 *     +PLATFORM: phase=<n> online=<0|1> clock=<0|1> discovered=<0|1>
 *       authorized=<0|1> signaling=<0|1> verifying=<0|1> held=<0|1>
 *       gated=<0|1> profile=<0|1> gen=<n> revision=<n> error=<n> exchanges=<n>
 *     (xiaotai_platform_status_t: online is phase ONLINE, clock clock_synced,
 *     profile profile_ready, revision notify_revision, error last_error)
 *     +PLATFORM-STATS: <field>=<n> ... (one bounded row per group: lifecycle,
 *     requests/transfers, signaling/binding, profile/worker. Together the
 *     rows contain every xiaotai_platform_stats_t field, once and in its
 *     declaration order, from one snapshot. Keys are only ever appended.)
 *     Consumers merge the same-prefix rows only after the final OK.
 *   AT+PLATFORM RECONNECT [<1..50>]   only with XIAOTAI_PLATFORM_RECONNECT_COMMAND
 *     (xiaotai/platform_config.h): default 1 round; each round takes a hold,
 *     waits for suspended, releases it and waits for ONLINE (at most 30 s),
 *     then reads the internal free memory and largest block once the
 *     signaling task has been reclaimed, writes the event-log record
 *     platform/reconnect and prints
 *     +PLATFORM-RECONNECT: round=<n> free=<bytes> largest=<bytes> ms=<n>.
 *     The handler hands back a stream whose next() waits on the console
 *     task for the worker's round; the owner task never waits. */
xiaotai_err_t xiaotai_engine_platform_init(void);
void xiaotai_engine_platform_start(void);

/* ------------------------------------------------------------------- status */

typedef enum {
    XIAOTAI_PLATFORM_STOPPED = 0,   /* not started, no network, or no usable discovery URL */
    XIAOTAI_PLATFORM_CLOCK,         /* synchronizing the clock (first time this boot) */
    XIAOTAI_PLATFORM_DISCOVERY,     /* fetching the service endpoints (once per boot) */
    XIAOTAI_PLATFORM_NO_IDENTITY,   /* endpoints known, no identity to log in with: a binding is needed */
    XIAOTAI_PLATFORM_LOGIN,         /* requesting the session token */
    XIAOTAI_PLATFORM_CONNECTING,    /* token held; signaling connecting or subscribing */
    XIAOTAI_PLATFORM_ONLINE,        /* token held and signaling subscribed */
    XIAOTAI_PLATFORM_UNBOUND,       /* the server no longer knows this binding: a binding is needed */
    XIAOTAI_PLATFORM_PAUSED,        /* a step failed and nothing retries it: xiaotai_platform_retry() */
} xiaotai_platform_phase_t;

typedef struct {
    xiaotai_platform_phase_t phase;
    bool clock_synced;     /* a synchronization succeeded in this boot */
    bool discovered;       /* endpoints can be read */
    bool authorized;       /* a session token is held: token requests are admitted */
    bool signaling;        /* both device topics are subscribed */
    bool verifying;        /* the binding is being verified by a new login */
    bool held;             /* signaling is suspended by a hold */
    bool gated;            /* the maintenance gate is closed */
    bool profile_ready;    /* the profile was accepted in this session */
    uint32_t generation;   /* binding generation of the session */
    /* Advances whenever notifications may have been missed: a notification was
     * refused or dropped (not a malformed one, which carried nothing), or
     * signaling came (back) up. Modules that mirror server state
     * compare it and re-read that state. */
    uint32_t notify_revision;
    xiaotai_err_t last_error; /* of the step that failed last; XIAOTAI_OK when none */
    /* Exchanges in flight: requests and transfers queued, running or waiting
     * for their handler, plus the platform's own profile report while it is
     * queued, running or held back by defer_background_while_active. Session
     * steps (clock, discovery, login) are not counted: the phase tells. Back
     * at 0, XIAOTAI_EVENT_PLATFORM_IDLE is emitted. */
    uint16_t exchanges;
} xiaotai_platform_status_t;

/* [any] */
void xiaotai_platform_status(xiaotai_platform_status_t *status);

/* [any] True while this product defers background exchanges for a
 * foreground activity. Also used by modules whose request uses a different
 * worker but must follow the same admission policy. */
bool xiaotai_platform_background_deferred(void);

/* [rt] Whether an accepted platform exchange is outstanding. One atomic
 * read; never waits, takes a lock, or allocates. A link listener may use it
 * at its own admission point, before an owner-task notification can run. */
bool xiaotai_platform_exchanges_busy(void);

/* [owner] Called before the worker opens HTTP for an accepted exchange.
 * OK allows I/O. BUSY defers it until resume(), then the worker asks again
 * within the same request deadline. Any other error completes the exchange
 * without I/O. The callback never blocks waiting for transport or for HTTP. */
typedef xiaotai_err_t (*xiaotai_platform_exchange_preflight_fn_t)(void *ctx);

/* [init] Optional, one registration per boot. Used by a module that must
 * finish withdrawing an admission before HTTP may use the same memory.
 * Registering in a provisioning boot is accepted but unused. */
xiaotai_err_t xiaotai_platform_exchange_preflight_register(xiaotai_platform_exchange_preflight_fn_t fn, void *ctx);
/* [owner] The asynchronous condition a BUSY preflight was waiting for has
 * changed. Safe to call before a queued/running preflight reports BUSY. */
void xiaotai_platform_exchange_preflight_resume(void);

/* Events on the framework event bus (xiaotai_event_subscribe). They are emitted
 * in the order things happened; a handler reads xiaotai_platform_status() for
 * the present truth (an ONLINE may be delivered after the session already
 * dropped again, and is then followed by an OFFLINE). */
#define XIAOTAI_EVENT_PLATFORM_CLOCK "platform.clock"            /* first synchronization of this boot */
#define XIAOTAI_EVENT_PLATFORM_DISCOVERED "platform.discovered"  /* endpoints can be read */
/* Signaling is subscribed and requests are admitted. Emitted on every
 * (re)connection: modules re-read what they mirror from the server. */
#define XIAOTAI_EVENT_PLATFORM_ONLINE "platform.online"
/* The session or its signaling ended. args.value: xiaotai_platform_offline_t.
 * Not emitted for a signaling hold. */
#define XIAOTAI_EVENT_PLATFORM_OFFLINE "platform.offline"
/* The device cannot log in until it is bound. args.value: 0 no identity is
 * stored (first binding), 1 the server unbound the stored identity (signed
 * re-binding). The binding generation has already advanced in case 1. */
#define XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED "platform.binding_needed"
/* Outcome of a profile report. args.value: 1 accepted, 0 given up for this session. */
#define XIAOTAI_EVENT_PLATFORM_PROFILE "platform.profile"
/* Entered XIAOTAI_PLATFORM_PAUSED. args.value: the error. */
#define XIAOTAI_EVENT_PLATFORM_PAUSED "platform.paused"
/* status.exchanges came back to 0, after the handler of the last exchange
 * returned. args.value 0. For modules that must not start something beside
 * an HTTP exchange (an admission that waits for the platform to be idle on a
 * board without external RAM): they re-check status().exchanges when it
 * arrives. */
#define XIAOTAI_EVENT_PLATFORM_IDLE "platform.idle"
/* Delivery of these events. Each is posted with xiaotai_event_emit at the
 * moment it happens. When a post fails (owner queue full), that event and
 * every later one are latched instead, one latch per event name holding the
 * newest argument, and the worker re-posts the latches every 10 ms until each
 * was posted (counted: event_post_retries). Re-posting order: binding_needed,
 * paused, offline, online, profile, discovered, clock, idle. An online or
 * profile latched before a later offline is discarded (it is no longer
 * true), and so is a latched idle once exchanges is no longer 0 (another
 * idle follows when it is); an offline latched before a later online is kept
 * and goes first. No event is dropped for lack of queue space. */

typedef enum {
    XIAOTAI_PLATFORM_OFFLINE_NETWORK = 0, /* the network went down */
    XIAOTAI_PLATFORM_OFFLINE_SIGNALING,   /* the signaling connection dropped; it reconnects by itself */
    XIAOTAI_PLATFORM_OFFLINE_AUTH,        /* the credentials were refused; what follows: auth_failure */
    XIAOTAI_PLATFORM_OFFLINE_REFRESH,     /* the token is being renewed before it expires */
    XIAOTAI_PLATFORM_OFFLINE_BINDING,     /* the binding ended or is being verified */
    XIAOTAI_PLATFORM_OFFLINE_MAINTENANCE, /* the gate closed while the session was being set up */
} xiaotai_platform_offline_t;

/* [owner] Completion of an operation of this header. */
typedef void (*xiaotai_platform_done_fn_t)(xiaotai_err_t result, void *ctx);

/* ---------------------------------------------------------- session control */

/* [any] The network link came up or went down. mac: the station MAC (6 bytes)
 * the platform knows this device by, read when up (may be NULL when down); it
 * is copied. The platform core calls this itself on the network engine's
 * XIAOTAI_EVENT_NET_UP and XIAOTAI_EVENT_NET_DOWN (xiaotai/net.h), with the
 * MAC of that event's status; it is public for host tests and for a firmware
 * that reports a link the network engine does not manage. Going down ends the
 * session (requests in flight become stale); the clock and the discovered
 * endpoints are kept. An up without a valid MAC is treated as down: the
 * device never invents a fingerprint.
 * One address may be reported up twice: the network engine emits NET_UP
 * again with the same address generation when the station MAC becomes known
 * after the address (a co-processor answers late). The platform core keeps
 * the generation of the up it follows: an up of that same generation is a
 * status update (a MAC that just became valid starts the session; anything
 * else changes nothing and never restarts a running session), while an up of
 * another generation is a new address: the session ends as on a down and
 * starts again. Calls of this function without an event behave the same with
 * the MAC as the only key: up again with the MAC already followed changes
 * nothing; with another MAC it is a new link. The login's X-Mac header is exactly
 * xiaotai_identity_fingerprint(mac) (xiaotai/identity.h): the text the
 * device was bound with, including the compact form of early devices and the
 * spelling the product configured; the platform core never formats the MAC
 * itself. Ignored in a provisioning boot. */
void xiaotai_platform_set_network(bool up, const uint8_t *mac);

/* [any] Try the session again from the first step that has not succeeded:
 * leaves PAUSED, and leaves NO_IDENTITY or UNBOUND after a binding stored or
 * confirmed the identity (every login reads the identity anew, so it logs in
 * with what xiaotai_identity_save stored). An open guest session is closed
 * first. No effect while a step is running or the device is online.
 * XIAOTAI_ERR_PLATFORM_GATED while the gate is closed. */
xiaotai_err_t xiaotai_platform_retry(void);

/* [any] A module learned that the binding may be gone. confirmed: the server
 * said so for this device (for example a transfer answered HTTP 410 with code
 * 6006, which the platform core never sees because the body goes to the
 * module); false: only a suspicion (for example when the product verifies a
 * business reply HTTP 200 with code 6006 by logging in again).
 * The first report of a session advances the binding generation, ends the
 * session (OFFLINE with reason BINDING, queued requests become stale) and
 * then either goes to UNBOUND (confirmed) or verifies with a new login, which
 * leads back to ONLINE or to UNBOUND. A report while that is under way only
 * upgrades a suspicion to confirmed. Nothing is erased: the stored identity
 * signs the re-binding. The platform core reports by itself on an "unbind"
 * signal, on HTTP 410 with code 6006 from a request (xiaotai_platform_request,
 * its own login and profile; never from a transfer, whose module reports),
 * and (unless tolerate_signal_loss) on a well-formed signal refused for lack
 * of a pool slot. */
void xiaotai_platform_report_unbound(bool confirmed);

/* ---------------------------------------------------------------- endpoints */

/* [any] Base URL of a discovered service, without a trailing slash; for
 * "mqtt" the broker URL. XIAOTAI_ERR_PLATFORM_OFFLINE before discovery,
 * XIAOTAI_ERR_NOT_SUPPORTED when the document has no such service,
 * XIAOTAI_ERR_PLATFORM_BAD_SERVICE when its address failed admission,
 * XIAOTAI_ERR_INVALID_SIZE when it does not fit. Endpoints do not change
 * during a boot. */
xiaotai_err_t xiaotai_platform_endpoint(const char *service, char *out, size_t capacity);

/* ----------------------------------------------------------------- requests */

typedef enum {
    XIAOTAI_PLATFORM_GET = 0,
    XIAOTAI_PLATFORM_POST,
    XIAOTAI_PLATFORM_PUT,
    XIAOTAI_PLATFORM_DELETE,
} xiaotai_platform_method_t;

typedef enum {
    /* "Authorization: Bearer <session token>". Needs an authorized session
     * (and, with requests_need_signaling, subscribed signaling). */
    XIAOTAI_PLATFORM_AUTH_SESSION = 0,
    /* Device signature headers (device id, timestamp, nonce, signature) made
     * from the stored identity. Needs a VALID identity and a synchronized
     * clock; no session. */
    XIAOTAI_PLATFORM_AUTH_SIGNED,
    /* "Authorization: Bearer <bearer>" with a token the caller obtained (a
     * temporary binding token). No session. One such request or transfer at a
     * time: a second one is refused with XIAOTAI_ERR_NO_MEM until the first
     * was answered. The copy of the bearer is wiped after the exchange. */
    XIAOTAI_PLATFORM_AUTH_BEARER,
    /* No credentials. No session. */
    XIAOTAI_PLATFORM_AUTH_NONE,
} xiaotai_platform_auth_t;
/* The three kinds without a session are what a binding uses: they are admitted
 * in every phase once discovery succeeded (NO_IDENTITY, UNBOUND and PAUSED
 * included), AUTH_SIGNED also needing the clock gate and a VALID identity. */

enum {
    /* Goes in front of waiting requests (never interrupts the running one):
     * for requests that end something a peer is waiting on (hang up, cancel,
     * reject) or that a user is waiting on (a session token). */
    XIAOTAI_PLATFORM_URGENT = 1u << 0,
    /* The reply handler starts something that needs the memory of the HTTP
     * connection (a real-time link): the connection is closed before the
     * reply is delivered, unless the product keeps idle connections
     * (idle_keep_ms). */
    XIAOTAI_PLATFORM_RELEASE = 1u << 1,
    /* Nobody waits on it (metadata refresh, a retry of a cleanup): with
     * defer_background_while_active it is refused with XIAOTAI_ERR_BUSY while
     * a foreground activity exists, and one already queued when an activity
     * begins is answered XIAOTAI_ERR_BUSY without being sent. The submitter
     * tries again after XIAOTAI_EVENT_ACTIVITY reports none. */
    XIAOTAI_PLATFORM_BACKGROUND = 1u << 2,
    /* Transfer only: apply request_full_timeout to this transfer. Does not
     * change the admission decision, response deadline or allocation errors. */
    XIAOTAI_PLATFORM_TRANSFER_FULL_TIMEOUT = 1u << 3,
};

typedef struct {
    const char *service;       /* discovery name, e.g. XIAOTAI_PLATFORM_SERVICE_CALL */
    xiaotai_platform_method_t method;
    const char *path;          /* starts with '/', may carry a query; shorter than XIAOTAI_PLATFORM_PATH_MAX */
    /* JSON text. GET: must be NULL. Other methods: NULL or "" sends an empty
     * body; the content type is application/json either way. */
    const char *body;
    uint32_t timeout_ms;       /* whole exchange; 0: the default; at most XIAOTAI_PLATFORM_TIMEOUT_MAX_MS */
    xiaotai_platform_auth_t auth;
    const char *bearer;        /* XIAOTAI_PLATFORM_AUTH_BEARER only; shorter than XIAOTAI_PLATFORM_BEARER_MAX */
    uint32_t flags;            /* XIAOTAI_PLATFORM_* request flags above */
} xiaotai_platform_request_t;

typedef struct {
    /* XIAOTAI_OK: an HTTP response was received and fits; look at http_status
     * and code. XIAOTAI_ERR_TIMEOUT, XIAOTAI_FAIL: no usable response.
     * XIAOTAI_ERR_INVALID_SIZE: the response body exceeds the pool's size.
     * XIAOTAI_ERR_PLATFORM_STALE: the binding changed or the session ended
     * after submission; the request was not sent, or its result is void.
     * XIAOTAI_ERR_BUSY: a background request not sent because an activity
     * began (defer_background_while_active). */
    xiaotai_err_t result;
    int http_status;       /* status line received, also with a non-OK result; 0 when none */
    int32_t code;          /* business code of the body, or XIAOTAI_PLATFORM_NO_CODE */
    const char *body;      /* terminated; NULL unless result is XIAOTAI_OK; valid during the handler only */
    size_t size;           /* body length without the terminator */
    uint32_t generation;   /* binding generation at submission */
} xiaotai_platform_reply_t;

/* [owner] Called exactly once for every accepted request. Non-2xx responses
 * are delivered with their body like any other. HTTP 410 with code 6006 is
 * also reported as unbound by the platform core before the handler runs. */
typedef void (*xiaotai_platform_reply_fn_t)(const xiaotai_platform_reply_t *reply, void *ctx);

/* [any] Queue one request. Everything in request is copied before returning;
 * requests run one at a time in order (urgent ones first). The next request
 * starts only after this one's handler returned. A failed POST, PUT or DELETE
 * is never sent twice; with get_retry_on_reuse a GET that failed on a reused
 * connection is tried once more on a new one within the same timeout.
 * Not accepted (reply is then never called):
 *   XIAOTAI_ERR_INVALID_ARG       malformed request, path, body or bearer too long
 *   XIAOTAI_ERR_PLATFORM_OFFLINE  not discovered yet, no session of the kind
 *                                 auth needs, or a provisioning boot
 *   XIAOTAI_ERR_NOT_SUPPORTED     the service is not in the discovery document
 *   XIAOTAI_ERR_PLATFORM_BAD_SERVICE  its address failed admission
 *   XIAOTAI_ERR_PLATFORM_GATED    the maintenance gate is closed
 *   XIAOTAI_ERR_BUSY              a background request during an activity
 *   XIAOTAI_ERR_NO_MEM            every slot is taken; try again later */
xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *request,
                                       xiaotai_platform_reply_fn_t reply, void *ctx);

/* [any] As request, but require the named nonzero binding generation at
 * admission. A mismatch returns XIAOTAI_ERR_PLATFORM_STALE without a callback.
 * The worker retains the same generation for its before-wire checks. */
xiaotai_err_t xiaotai_platform_request_for_generation(const xiaotai_platform_request_t *request,
                                                      uint32_t generation,
                                                      xiaotai_platform_reply_fn_t reply, void *ctx);

/* [any] Strict read of the business code of a reply or signal: the text must
 * be one JSON object, nested at most 32 deep, with exactly one member "code"
 * whose value is an integral number. XIAOTAI_ERR_NOT_FOUND otherwise. */
xiaotai_err_t xiaotai_platform_json_code(const char *json, size_t size, int32_t *code);

/* [any] Expiry of a token issued by the platform (a session token or a
 * temporary binding token): the integral "exp" claim, in seconds since 1970,
 * of a JWT's payload (three Base64url parts separated by '.'). Reads the text
 * only: no allocation, no clock, nothing logged. XIAOTAI_ERR_NOT_FOUND when
 * the text is not such a JWT or has no integral, positive "exp";
 * XIAOTAI_ERR_INVALID_ARG for NULL or a token of XIAOTAI_PLATFORM_BEARER_MAX
 * bytes or more. Whether the expiry lies in the future is the caller's
 * comparison with xiaotai_clock_unix_ms(). The platform core uses the same
 * function for its own session token. */
xiaotai_err_t xiaotai_platform_token_expiry(const char *token, int64_t *exp_unix_s);

/* ---------------------------------------------------------------- transfers */

/* A transfer is a request whose body is produced in pieces, or whose response
 * is consumed in pieces: uploads that are not JSON, responses larger than the
 * pool's buffer, binary downloads. It takes one request slot and runs on the
 * same worker in the same queue; between pieces the worker keeps the
 * signaling heartbeat and its timers going.
 * The platform core never reads a transfer's response body, so it does not
 * recognize HTTP 410 with code 6006 there: the module that consumes the body
 * calls xiaotai_platform_report_unbound(true) when it sees them. A 401 on a
 * session transfer is still handled as auth_failure says (the status line is
 * enough). */

typedef struct {
    int http_status;
    const char *content_type;      /* "" when absent; the first one when repeated */
    const char *content_encoding;  /* "" when absent; the first one when repeated */
    int64_t content_length;        /* -1 when not announced */
    /* The header appeared more than once in the response (the HTTP port's
     * capture count, xiaotai_http_capture_t.count, above 1). A module that
     * validates the type or encoding refuses such a response (deployed
     * firmware treats a repeated Content-Type as invalid). */
    bool content_type_repeated;
    bool content_encoding_repeated;
    /* A value was longer than XIAOTAI_PLATFORM_CONTENT_TYPE_MAX - 1 bytes and
     * is cut there. */
    bool truncated;
} xiaotai_platform_response_head_t;

typedef uint8_t xiaotai_platform_receive_fault_t;
enum {
    XIAOTAI_PLATFORM_RECEIVE_FAULT_NONE = 0,
    XIAOTAI_PLATFORM_RECEIVE_FAULT_HEAD,
    XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY,
    XIAOTAI_PLATFORM_RECEIVE_FAULT_BODY_TRUNCATED, /* EOF after a body prefix, before the framing completed */
};

typedef struct {
    xiaotai_err_t result;  /* as in xiaotai_platform_reply_t, or XIAOTAI_ERR_PLATFORM_CANCELLED,
                              or the error a stream function returned */
    int http_status;       /* 0 when no status line was received */
    /* Part of the request may have reached the server. When true and the
     * result is not XIAOTAI_OK, the submitter must find out what the server
     * did before sending the same thing again. */
    bool sent;
    /* Receive I/O error or per-operation stall. The original result is kept. */
    xiaotai_platform_receive_fault_t receive_fault;
    size_t received;       /* response body bytes passed to write() */
    uint32_t generation;
} xiaotai_platform_transfer_result_t;

typedef struct {
    /* [worker] Next piece of the request body: at most capacity bytes into
     * data, the count in *size. Called until body_size bytes were produced.
     * A non-OK result aborts the transfer. NULL when body_size is 0. */
    xiaotai_err_t (*read)(void *ctx, uint8_t *data, size_t capacity, size_t *size);
    /* [worker] The response's status and headers, before any body. A non-OK
     * result aborts (wrong type, too long). May be NULL. Strings are valid
     * during the call. */
    xiaotai_err_t (*begin)(void *ctx, const xiaotai_platform_response_head_t *head);
    /* [worker] Next piece of the response body. A non-OK result aborts. NULL:
     * the body is read and discarded. */
    xiaotai_err_t (*write)(void *ctx, const uint8_t *data, size_t size);
    /* [owner] Exactly once for every accepted transfer, after the last
     * read/begin/write call has returned. */
    void (*done)(void *ctx, const xiaotai_platform_transfer_result_t *result);
    void *ctx;
} xiaotai_platform_stream_t;

typedef struct {
    /* request.body is sent as is when body_size is 0. timeout_ms bounds the
     * whole transfer: 0 the default, at most XIAOTAI_PLATFORM_TRANSFER_TIMEOUT_MAX_MS. */
    xiaotai_platform_request_t request;
    /* Content type of a streamed body; shorter than XIAOTAI_PLATFORM_CONTENT_TYPE_MAX. */
    const char *content_type;
    size_t body_size;          /* exact number of bytes read() will produce; 0: none */
    size_t response_max;       /* a longer response body aborts with XIAOTAI_ERR_INVALID_SIZE; 0: no limit */
    uint32_t stall_timeout_ms; /* longest wait for one socket read or write; 0: 5000 */
    /* Optional budget from the start of wire I/O, within request.timeout_ms.
     * Zero keeps only the overall limit. */
    uint16_t exchange_timeout_ms;
} xiaotai_platform_transfer_t;

/* [any] Queue a transfer. The structures are copied; stream->ctx and whatever
 * the stream functions use must stay valid until done() ran. *id (may be NULL)
 * identifies it for cancelling. Not accepted: as xiaotai_platform_request. */
xiaotai_err_t xiaotai_platform_transfer(const xiaotai_platform_transfer_t *transfer,
                                        const xiaotai_platform_stream_t *stream, uint32_t *id);

/* [any] Stop a transfer as soon as possible. done() still runs, with
 * XIAOTAI_ERR_PLATFORM_CANCELLED unless it had already finished. Unknown or
 * finished ids are ignored. */
void xiaotai_platform_transfer_cancel(uint32_t id);

/* ---------------------------------------------------------------- signaling */

typedef struct {
    const char *type;      /* the envelope's "type" */
    const char *channel;   /* the envelope's "channel"; "" when absent */
    const char *json;      /* the whole envelope, terminated; valid during the handler only */
    size_t size;           /* length of json without the terminator */
    uint32_t generation;   /* binding generation at receipt; the current one when delivered */
    bool command;          /* arrived on the acknowledged command topic, not as a notification */
} xiaotai_platform_signal_t;

/* [owner] */
typedef void (*xiaotai_platform_signal_fn_t)(const xiaotai_platform_signal_t *signal, void *ctx);

/* [init] Deliver signaling messages of this type. channel: only messages with
 * exactly this channel, or NULL for any. Several subscriptions may match one
 * message; they run in registration order. A handler that needs the message
 * later copies it. type and channel are not copied (static storage).
 *
 * What happens to a message, decided on the signaling task:
 *   - accepted: a well-formed envelope (one JSON object, nested at most 32
 *     deep, "type" a string) and a free pool slot, whether or not anybody
 *     subscribed to its type. A command is acknowledged to the server at that
 *     moment; it means "will be handled": the slot stays occupied until the
 *     message was handed to the owner task. When that hand-off fails (owner
 *     queue full) the slot is marked pending and the worker re-posts pending
 *     slots every 10 ms, in arrival order, until each was handed over
 *     (counted: signal_post_retries); later messages queue behind them.
 *   - refused for lack of a slot (well-formed, every slot taken): counted;
 *     a command is left unacknowledged for the server to send again; a
 *     notification advances notify_revision; unless tolerate_signal_loss the
 *     binding is verified (xiaotai_platform_report_unbound(false)), because
 *     the refused message may have been an unbind.
 *   - dropped as oversized (total length of signal_bytes or more), as
 *     inconsistently fragmented, or for a foreign topic: counted; a
 *     notification advances notify_revision, a command is not acknowledged;
 *     the binding is never verified for it.
 *   - malformed (complete and within size, but not a well-formed envelope):
 *     counted and never delivered; a command is acknowledged unless the
 *     product sets drop_malformed_commands; notify_revision
 *     does not advance (deployed firmware ignores such a message entirely);
 *     the binding is never verified for it.
 * Messages received while the session is not authorized, or of an older
 * binding generation by the time they are delivered, are dropped and
 * counted. "unbind" is consumed by the platform core and never delivered,
 * except on the notify topic when the product sets unbind_command_only. */
xiaotai_err_t xiaotai_platform_signal_subscribe(const char *type, const char *channel,
                                                xiaotai_platform_signal_fn_t handler, void *ctx);

/* ------------------------------------------------- guest signaling session */

/* A temporary signaling session under credentials the server issued for one
 * binding attempt. It subscribes device/<client_id>/cmd and nothing else, is
 * separate from the device's own signaling, and exists only while the device
 * has no session of its own (phases NO_IDENTITY, UNBOUND, PAUSED). One at a time. */
typedef struct {
    const char *client_id;     /* client id, user name and topic id; at most 64 characters; copied */
    const char *token;         /* password; shorter than XIAOTAI_PLATFORM_BEARER_MAX; copied */
    /* [owner] Subscribed; called again after each reconnection. */
    void (*ready)(uint32_t id, void *ctx);
    /* [owner] One message. Messages are not acknowledged by the platform core:
     * the module calls xiaotai_platform_guest_ack once it has made the content
     * durable. signal->generation is the generation at receipt. */
    void (*message)(uint32_t id, const xiaotai_platform_signal_t *signal, void *ctx);
    /* [owner] The session ended by itself: the credentials or the subscription
     * were refused (XIAOTAI_FAIL), the subscription was never confirmed
     * (XIAOTAI_ERR_TIMEOUT), the network went down
     * (XIAOTAI_ERR_PLATFORM_OFFLINE), or the device's own session began
     * (XIAOTAI_ERR_INVALID_STATE: a login started, for example after
     * xiaotai_platform_retry(); the guest session is closed before the login's
     * first exchange). A dropped connection alone does not end it: it
     * reconnects. Not called after xiaotai_platform_guest_close. */
    void (*closed)(uint32_t id, xiaotai_err_t reason, void *ctx);
    void *ctx;
    /* Temporary SUBACK timeout, including resubscribe; zero inherits the
     * platform timeout. The caller may also cap first readiness with an
     * absolute xiaotai_time_us() deadline (zero leaves it uncapped). */
    uint16_t subscribe_timeout_s;
    int64_t initial_subscribe_deadline_us;
    /* [owner] An already subscribed guest lost its connection. Optional;
     * when supplied, messages not yet handed to the owner are also retired. */
    void (*unready)(uint32_t id, void *ctx);
} xiaotai_platform_guest_t;

/* [owner] Open the session; *id is never 0. XIAOTAI_ERR_PLATFORM_OFFLINE before
 * discovery or without network, XIAOTAI_ERR_INVALID_STATE while the device's
 * own session exists, XIAOTAI_ERR_BUSY when a guest session is open,
 * XIAOTAI_ERR_TIMEOUT when the supplied initial deadline has passed. */
xiaotai_err_t xiaotai_platform_guest_open(const xiaotai_platform_guest_t *guest, uint32_t *id);

/* [any] True only for the currently subscribed guest of this id. */
bool xiaotai_platform_guest_subscribed(uint32_t id);

/* [owner] Publish the acknowledgement on device/<client_id>/ack and wait for
 * the broker's confirmation of that very publication. done: XIAOTAI_OK when
 * confirmed, XIAOTAI_ERR_TIMEOUT after timeout_ms, XIAOTAI_ERR_PLATFORM_STALE
 * when the session closed first. */
xiaotai_err_t xiaotai_platform_guest_ack(uint32_t id, uint32_t timeout_ms, xiaotai_platform_done_fn_t done,
                                         void *ctx);

/* [owner] Request close and revoke further handlers immediately. The worker
 * stops and frees the session; unknown ids are ignored. */
void xiaotai_platform_guest_close(uint32_t id);

/* ----------------------------------------------------------- signaling hold */

/* While at least one hold exists the platform keeps no signaling connection
 * and starts no login: its memory and its network use are free for something
 * that cannot share them (starting or connecting a real-time link on a board
 * short of internal RAM). A hold may be taken before the session starts.
 * Requests of an authorized session keep working unless
 * requests_need_signaling. Signals sent meanwhile are not replayed: when the
 * last hold is released the platform reconnects (reusing a token that is
 * still good, else logging in again), emits ONLINE and advances
 * notify_revision, and modules re-read what they mirror. Reconnection
 * failures are retried every second.
 * A hold destroys the signaling client (its task, buffers, outbox and TLS
 * context), since freeing that memory is its purpose. A signaling drop on
 * the same network is left to the port's reconnection. A network drop ends
 * the session and destroys the client and its outbox; the next link-up logs
 * in again, without resending queued publications from the old session. */

/* [any] Take a hold. suspended (may be NULL) runs [owner] once the signaling
 * connection is gone and its memory is free, at once when there was none.
 * *hold is never 0. */
xiaotai_err_t xiaotai_platform_signaling_hold(xiaotai_platform_done_fn_t suspended, void *ctx,
                                              uint32_t *hold);
/* [any] Optional online hold with an upper bound of 60,000 ms. Requires an
 * authorized, signaling session with known expiry and no other hold or active
 * exchange. Ends before token renewal/expiry, or sooner on a gate, network or
 * session change, or an admitted exchange. Neither timeout nor release waits
 * for the owner. An undelivered callback can be canceled on automatic release;
 * an already delivered callback describes the initial suspension only. */
xiaotai_err_t xiaotai_platform_signaling_hold_bounded(uint32_t max_ms,
                                                      xiaotai_platform_done_fn_t suspended, void *ctx,
                                                      uint32_t *hold);
/* [any] A startup hold keeps login and signaling stopped, but allows the
 * clock and initial service discovery needed to start the transport. An
 * ordinary hold still stops setup when both kinds are active. */
xiaotai_err_t xiaotai_platform_signaling_hold_startup(xiaotai_platform_done_fn_t suspended, void *ctx,
                                                     uint32_t *hold);

/* [any] Release a hold. Unknown and already released holds are ignored. */
void xiaotai_platform_signaling_release(uint32_t hold);

/* Scoped access for a deferred owner of a bounded signaling hold. Matching
 * the original callback context prevents a stale token touching a reused ID. */
bool xiaotai_platform_signaling_bounded_hold_active(uint32_t hold, const void *ctx);
void xiaotai_platform_signaling_release_bounded(uint32_t hold, const void *ctx);

/* --------------------------------------------------------- maintenance gate */

/* Before the device saves an environment or restarts, every HTTP exchange of
 * the platform must have ended. Closing the gate refuses new requests and
 * transfers, interrupts a session that is still being set up (it resumes when
 * the gate opens), and postpones retries and token renewal; what was already
 * queued runs to its end. Signaling stays as it is. A platform that has
 * deliberately stayed offline in exclusive provisioning, or whose worker
 * could not start, has no exchanges. Its local gate starts drained and
 * neither starts a worker nor enables platform traffic.
 * Gate calls may run concurrently during that lifecycle. Host teardown is
 * serialized against platform callers. */

/* [any] Close the gate; *ticket is never 0. XIAOTAI_ERR_PLATFORM_GATED when it
 * is already closed (*ticket is left alone). Success does not mean drained. */
xiaotai_err_t xiaotai_platform_gate_close(uint32_t *ticket);

/* [any] Close the same gate, but return XIAOTAI_ERR_BUSY without changing
 * the ticket while verifying or replacing a cloud-rejected stored identity.
 * First-time provisioning is allowed. The binding check and close are atomic. */
xiaotai_err_t xiaotai_platform_gate_close_binding_idle(uint32_t *ticket);

/* [any] True when ticket holds the gate, no request or transfer is queued,
 * running or waiting for its handler, and the worker has closed its
 * connections since. */
bool xiaotai_platform_gate_drained(uint32_t ticket);

/* [any] Open the gate. XIAOTAI_ERR_INVALID_ARG for a ticket that does not hold
 * it (an old ticket cannot open a newer closing). */
xiaotai_err_t xiaotai_platform_gate_open(uint32_t ticket);

/* ----------------------------------------------------------- device profile */

/* The profile tells the server what this device can do, one object per scene
 * ("stream", "call", "voip", …). Each scene belongs to the module that
 * implements it; the platform core adds the hardware object and the firmware
 * version (unless configured out), sends the document after every ONLINE,
 * and retries it (profile_attempts). The server
 * replaces a scene as a whole, so a scene writer always writes its complete
 * object. Writers run at report time, and the first report follows the first
 * ONLINE, which comes after start(), when every module's init() registered
 * its scenes and whatever pieces they assemble. status.profile_ready is true
 * from the server's acceptance until the session ends; a module that needs
 * an accepted profile and finds it false calls xiaotai_platform_profile_refresh.
 * With defer_background_while_active the report waits while an activity exists. */

/* [owner] Write this scene's JSON object ("{…}", no terminator needed) into
 * out. Returns its length; 0 leaves the scene out of this report; a negative
 * value means capacity was too small (the report is then not sent and the
 * failure is counted). */
typedef int (*xiaotai_platform_profile_fn_t)(char *out, size_t capacity, void *ctx);

/* [init] Register the writer of one scene. scene: at most
 * XIAOTAI_PLATFORM_SCENE_NAME_MAX - 1 characters, static storage, registered once. */
xiaotai_err_t xiaotai_platform_profile_scene(const char *scene, xiaotai_platform_profile_fn_t write,
                                             void *ctx);

/* Session extensions (xiaotai/framework.h, xiaotai_contribution_t). An
 * extension that adds to a host's scene (the video members of "call",
 * "voip", "stream") contributes under this slot a
 * `const xiaotai_platform_scene_part_t` for that scene; the host's scene
 * writer splices every part in with xiaotai_platform_scene_parts. So the
 * scene still has one writer, and an extension needs no API of its host.
 * Members are split by meaning, never written twice:
 *   - the host writes the members of its own session (audio, screen,
 *     timeouts), every stream number of the scene, audio and video (stream
 *     numbers belong to the side that puts them into the link description,
 *     which is the host), and "no_video", decided by the number of parts
 *     spliced in;
 *   - a part writes the video members proper (codecs, rotation, aspect
 *     ratio, mirrors, fit, resolution mode) and never a stream number or
 *     "no_video", so the same part is right on every host.
 * When no part was spliced in, the host writes its "no video" members
 * instead: empty or "none" codecs and "no_video":true, plus whatever further
 * members the deployed firmware of the product reports without video (a
 * per-product option of the host). The server reads members by name; their
 * order carries no meaning. */
#define XIAOTAI_PLATFORM_SLOT_SCENE "profile.scene"
typedef struct {
    /* [owner] Writes `"name":value` members, separated by commas, without
     * braces and without a leading or trailing comma, into out (not
     * terminated). Returns the length, 0 for none, negative when capacity is
     * too small. */
    int (*write)(char *out, size_t capacity, void *ctx);
    void *ctx;
} xiaotai_platform_scene_part_t;

/* Most parts one scene collects; more are left out and counted. */
#define XIAOTAI_PLATFORM_SCENE_PARTS_MAX 4U

/* [owner] For a host's scene writer: splice in the parts that session
 * extensions contributed to this scene, that is every
 * XIAOTAI_PLATFORM_SLOT_SCENE contribution for `scene` of the attachments of
 * the host's activity `activity` (xiaotai_activity_contributions), in
 * attachment order.
 * out[0 .. *length) is what the writer has written so far: "{" and its own
 * members, without a trailing comma. Each part that writes something is
 * appended after a ',' (none right after the "{"), and *length grows by what
 * was appended; a part that writes nothing adds nothing. A part that fails or
 * does not fit in capacity leaves out and *length as they were and is
 * counted (profile_parts_dropped), and so is a part beyond
 * XIAOTAI_PLATFORM_SCENE_PARTS_MAX. The text is neither closed nor
 * terminated: the writer adds its remaining members and the "}", so it
 * passes as capacity only what the parts may use (its buffer minus what it
 * still writes). Returns the number of parts spliced in, which tells the
 * writer whether the scene has video ("no_video"). */
size_t xiaotai_platform_scene_parts(const char *activity, const char *scene, char *out, size_t capacity,
                                    size_t *length);

/* [owner] Report again now: a scene's content changed, or a module found the
 * profile not ready when it needed it. Starts a new round of attempts.
 * Ignored while a report is in flight or the device is not online. */
void xiaotai_platform_profile_refresh(void);

/* [owner] Notify the profile scheduler of a readiness transition. Ignored
 * unless profile_requires_ready. False postpones unsent reports without
 * reserving an exchange; admitted HTTP still finishes normally. */
void xiaotai_platform_profile_ready(bool ready);

/* -------------------------------------------------------------- environment */

/* The environment is the base address the discovery URL is made from
 * (base + "/services"). It is stored under the namespace and entry deployed
 * firmware uses, and takes effect at the next boot. */
typedef struct {
    char active[XIAOTAI_PLATFORM_URL_MAX]; /* discovery URL in use this boot; "" when boot_error */
    char saved[XIAOTAI_PLATFORM_URL_MAX];  /* stored base; "" when none (the built-in URL applies) */
    char next[XIAOTAI_PLATFORM_URL_MAX];   /* discovery URL the next boot will use */
    /* Reading the stored base failed at boot (anything but "not stored"): the
     * platform stays STOPPED and never falls back to the built-in URL.
     * Everything else keeps working, so the environment can still be stored
     * (that is how such a device is repaired). */
    xiaotai_err_t boot_error;
    /* A store since boot ended without a matching read-back (a timeout, a
     * storage error, a mismatch): storage may hold the new base, the old one
     * or, for an interrupted write, none. saved and next still show the last
     * confirmed value and must be shown as unknown while this is true. Cleared
     * by the next store that is confirmed; a restart reads storage anew. */
    bool unconfirmed;
} xiaotai_platform_environment_t;

/* [any] Never touches storage: the stored base is read once at init and kept
 * in step with confirmed stores (see unconfirmed). */
void xiaotai_platform_environment(xiaotai_platform_environment_t *environment);

/* [any] True only after this boot's stored environment failed to load.
 * Later discovery/network errors are retryable and do not set this flag;
 * a repaired stored value takes effect only on the next boot. */
bool xiaotai_platform_environment_boot_failed(void);

/* [any] Engine startup ended with an error and cannot run this boot.
 * Does not include intentional provisioning or recoverable session errors. */
bool xiaotai_platform_start_failed(void);

/* [any] Bring an address into the stored form: http:// or https:// (written
 * back as http://), a host, an optional port other than 0 and an optional
 * path; no blanks, control characters, '?', '#', backslash or user
 * information; trailing slashes removed; at most XIAOTAI_PLATFORM_BASE_MAX
 * characters. XIAOTAI_ERR_INVALID_ARG otherwise. out may be NULL to validate only. */
xiaotai_err_t xiaotai_platform_environment_normalize(const char *base, char *out, size_t capacity);

/* [any] Store a new base (normalized first), or remove the stored one with
 * NULL, then read storage back and compare. Runs on the platform worker; done
 * (may be NULL) reports XIAOTAI_OK only when the read-back matched (saved and
 * next then show the new value). A failure is not retried or rolled back; a
 * timeout may still have written, so any failure after the write was handed
 * to storage sets environment.unconfirmed before done runs. The caller
 * has closed and drained the gate before, and restarts after success. */
xiaotai_err_t xiaotai_platform_environment_store(const char *base, xiaotai_platform_done_fn_t done,
                                                 void *ctx);

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t clock_attempts, discoveries, services_invalid, logins, login_failures, token_renewals;
    uint32_t signaling_connects, signaling_drops, signaling_rebuilds, subscribe_retries, heartbeats;
    uint32_t requests, replies, request_failures, requests_stale, requests_refused, requests_busy;
    uint32_t transfers, transfers_cancelled, reply_post_retries, slot_clamps;
    uint32_t signals, signals_delivered, signals_acknowledged, signal_post_retries;
    uint32_t signals_malformed, signals_oversized, signals_fragment_errors, signals_no_slot;
    uint32_t signals_stale, signals_unauthorized;
    uint32_t unbound_reports, profile_reports, profile_failures, profile_deferrals;
    uint32_t profile_parts_dropped;
    uint32_t event_post_retries;
    uint32_t worker_stack_free; /* least free stack seen, bytes */
} xiaotai_platform_stats_t;

/* [any] */
void xiaotai_platform_stats(xiaotai_platform_stats_t *stats);

/* Host tests only: stop the worker and forget configuration, registrations and
 * state. The fixture must serialize reset with lifecycle calls and API callers. */
void xiaotai_platform_reset(void);

#ifdef __cplusplus
}
#endif
