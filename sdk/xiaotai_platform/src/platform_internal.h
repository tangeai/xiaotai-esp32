// SPDX-License-Identifier: MIT
#pragma once
/* Interfaces between the platform core's translation units. Nothing here is
 * public: modules use xiaotai/platform.h only. Tags as in xiaotai/platform.h,
 * plus [storage worker] (a callback the storage port's worker runs,
 * xiaotai/port/kv.h). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "xiaotai/platform.h"
#include "xiaotai/identity.h"
#include "xiaotai/port/http.h"
#include "xiaotai/port/mqtt.h"
#include "xiaotai/port/os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ url.c */

/* Address admission as deployed firmware applies it: the scheme prefix is
 * matched without regard to case, then every byte is checked (no blank,
 * control character, DEL, '?', '#' or backslash), then authority and path
 * follow the URL grammar of the HTTP client's strict parser (host of letters,
 * digits, '.' and '-', or a bracketed IPv6 literal with an optional zone; a
 * port of digits up to 65535; path bytes printable ASCII), and finally user
 * information and port 0 are refused. All functions are [any, rt]: no
 * allocation, no lock, no log. */

/* An http:// or https:// address, written to out as http:// (the project's
 * transport policy, see xiaotai/platform.h). trim removes trailing slashes
 * (service and environment bases). The input itself must be shorter than
 * capacity, as deployed firmware copied it into a fixed buffer before
 * rewriting it. out may be url (rewritten in place). XIAOTAI_ERR_INVALID_ARG
 * for an inadmissible address, XIAOTAI_ERR_INVALID_SIZE when it does not fit;
 * out[0] is '\0' after any failure (capacity > 0). */
xiaotai_err_t platform_url_http(const char *url, bool trim, char *out, size_t capacity);

/* An mqtt:// or mqtts:// address, admitted by the same rules. */
bool platform_url_mqtt_valid(const char *url);

/* The "mqtt" service address of a discovery document, copied unchanged:
 * non-empty, shorter than capacity, and (unless unchecked) admitted by
 * platform_url_mqtt_valid. Errors and out as for platform_url_http. */
xiaotai_err_t platform_url_mqtt(const char *url, bool unchecked, char *out, size_t capacity);

/* A request path: starts with '/', shorter than XIAOTAI_PLATFORM_PATH_MAX,
 * printable ASCII without blanks and without '#'; a query after '?' is part
 * of it. The HTTP client's parser refuses blanks and control characters in a
 * request target, and a fragment never reaches the server. */
bool platform_url_path_valid(const char *path);

/* base + path into out (terminated). XIAOTAI_ERR_INVALID_SIZE when it does
 * not fit (out[0] is then '\0'), XIAOTAI_ERR_INVALID_ARG for NULL. */
xiaotai_err_t platform_url_join(const char *base, const char *path, char *out, size_t capacity);

/* ------------------------------------------------------------ json_scan.c */

/* Raw bytes of a JSON string inside a text: between its quotes, escapes as
 * written. */
typedef struct {
    uint32_t offset;
    uint32_t length;
} platform_json_span_t;

/* The parts of a signaling envelope the platform core reads. */
typedef struct {
    platform_json_span_t type;
    /* Present only when the envelope has a top-level "channel" whose value is
     * a string; absent otherwise (the handler then sees ""). */
    platform_json_span_t channel;
    bool has_channel;
} platform_envelope_t;

/* [any, rt] A well-formed signaling envelope (xiaotai/platform.h,
 * xiaotai_platform_signal_subscribe): one JSON object nested at most
 * XIAOTAI_JSON_DEPTH_MAX deep whose first top-level "type" is a string. The
 * text is read as deployed firmware's strict parse read it: a NUL byte
 * anywhere refuses it; a UTF-8 byte-order mark (texts of 4 bytes or more)
 * and bytes 0x01..0x20 before and after the value are skipped; inside, the
 * JSON library's rules apply (xiaotai/json.h). The type and channel strings
 * must decode (no malformed escape, lone surrogate or \u0000), because a
 * handler receives them as C strings. XIAOTAI_ERR_INVALID_ARG when it is
 * not well-formed (malformed), *envelope untouched. No allocation; about 200
 * bytes of stack. */
xiaotai_err_t platform_json_envelope(const char *json, size_t size, platform_envelope_t *envelope);

/* [any, rt] The decoded text of span into out, terminated.
 * XIAOTAI_ERR_INVALID_ARG for a refused escape, XIAOTAI_ERR_INVALID_SIZE when
 * it does not fit (*length, when not NULL, is the decoded length). */
xiaotai_err_t platform_json_span_text(const char *json, platform_json_span_t span, char *out, size_t capacity,
                                      size_t *length);

/* [any, rt] True when span decodes to exactly text. */
bool platform_json_span_equals(const char *json, platform_json_span_t span, const char *text);

typedef xiaotai_err_t (*platform_json_member_fn)(const char *key, size_t key_size,
    const char *value, size_t value_size, void *ctx);
xiaotai_err_t platform_json_members(const char *json, size_t size, platform_json_member_fn visit, void *ctx);
xiaotai_err_t platform_json_field(const char *json, size_t size, const char *name, const char **value, size_t *length);
xiaotai_err_t platform_json_text_read(const char *json, size_t size, char *out, size_t capacity);
xiaotai_err_t platform_json_integer_read(const char *json, size_t size, int64_t *out);

/* ---------------------------------------------------------- environment.c */

/* Storage of the environment base, as deployed firmware wrote it: a string
 * (the normalized base, without "/services") under this namespace and key. */
#define PLATFORM_ENVIRONMENT_NAMESPACE "service_env"
#define PLATFORM_ENVIRONMENT_KEY "ep_base"
/* Appended to a base to make the discovery URL. */
#define PLATFORM_ENVIRONMENT_SUFFIX "/services"
/* Longest wait for each storage step of a store, the write and the read-back:
 * deployed firmware waited this long for each synchronous storage call. */
#define PLATFORM_ENVIRONMENT_STEP_MS 5000U

/* [init] Reads the stored base once (the engine's init; synchronous storage
 * read). builtin_url: the product's discovery URL (configuration), or NULL.
 * Returns the boot error (also in xiaotai_platform_environment): XIAOTAI_OK,
 * or the storage error, XIAOTAI_ERR_INVALID_SIZE for a stored base longer
 * than XIAOTAI_PLATFORM_BASE_MAX, XIAOTAI_FAIL for a stored value that is not
 * an admissible base. "Not stored" is not an error: the built-in URL applies.
 * A built-in URL that is not admissible counts as none (the configuration
 * refuses it earlier). Allocates nothing: the state is static. A second call
 * reads storage again (after platform_environment_reset). */
xiaotai_err_t platform_environment_boot(const char *builtin_url);

/* [any] The discovery URL of this boot. XIAOTAI_OK; the boot error when the
 * stored base could not be read (the platform then stays stopped);
 * XIAOTAI_ERR_NOT_FOUND when there is none (no built-in URL, nothing
 * stored); XIAOTAI_ERR_INVALID_STATE before platform_environment_boot;
 * XIAOTAI_ERR_INVALID_SIZE when out is too small. */
xiaotai_err_t platform_environment_discovery_url(char *out, size_t capacity);

/* Host tests (xiaotai_platform_reset): forget the boot state. A store still
 * in progress is abandoned without its done. */
void platform_environment_reset(void);

/* Host tests: the wait of each storage step (0: PLATFORM_ENVIRONMENT_STEP_MS). */
void platform_environment_set_step_ms(uint32_t ms);

/* -------------------------------------------------------------- P-2 core */

#define PLATFORM_DEFAULT_STALL_MS 5000U
#define PLATFORM_OWNER_REPOST_MS 10U

typedef struct {
    const char *name;
    /* NULL for an optional service listed with an invalid address. */
    const char *base;
} platform_service_t;

typedef struct {
    /* Optional session layer callbacks, installed at init (P-3/P-5). */
    void (*binding_lost)(uint32_t generation);  /* [owner] confirmed 410 and code 6006 */
    void (*auth_failed)(void);   /* [owner] session exchange replied 401 */
} platform_reply_hooks_t;

/* Later session packages call these production interfaces; host tests feed
 * only these inputs, then exercise the real queue/HTTP/owner handoff. */
xiaotai_err_t platform_services_replace(const platform_service_t *services, size_t count);
xiaotai_err_t platform_services_discovered(const platform_service_t *services, size_t count, uint32_t network_revision);
bool platform_service_name_valid(const char *name);
void platform_session_publish(const xiaotai_platform_status_t *status, const char *token);
void platform_reply_hooks(const platform_reply_hooks_t *hooks);
void platform_profile_exchange(bool pending); /* queued/running/deferred: one logical exchange */

/* The single platform worker owns setup HTTP and calls these transport
 * hooks outside the platform lock. A start copies the borrowed credentials;
 * a destroy drains and releases the handle/outbox before it returns OK. */
typedef struct {
    xiaotai_err_t (*start)(void *ctx, uint32_t epoch, const char *broker, const char *device, const char *token);
    xiaotai_err_t (*destroy)(void *ctx);
    void *ctx;
} platform_session_link_t;
typedef enum { PLATFORM_LINK_READY, PLATFORM_LINK_DROPPED, PLATFORM_LINK_AUTH_REFUSED } platform_link_event_t;
typedef enum { PLATFORM_EVENT_BINDING, PLATFORM_EVENT_PAUSED, PLATFORM_EVENT_OFFLINE, PLATFORM_EVENT_ONLINE,
    PLATFORM_EVENT_PROFILE, PLATFORM_EVENT_DISCOVERED, PLATFORM_EVENT_CLOCK, PLATFORM_EVENT_IDLE,
    PLATFORM_EVENT_COUNT } platform_event_t;
typedef struct {
    uint32_t id;
    xiaotai_platform_done_fn_t callback;
    void *ctx;
    xiaotai_err_t result;
    bool complete, posted, startup;
} platform_hold_t;

typedef enum {
    PLATFORM_BIND_NONE, PLATFORM_BIND_QUIESCE, PLATFORM_BIND_CUT, PLATFORM_BIND_WAIT_GATE,
    PLATFORM_BIND_DRAIN, PLATFORM_BIND_VERIFY,
} platform_binding_phase_t;

typedef struct {
    platform_binding_phase_t phase;
    bool confirmed, offline_posted, fence_posted;
    uint32_t generation, revised_generation, fence;
    int32_t ack_id;
    int64_t ack_due;
} platform_binding_t;

typedef struct {
    platform_session_link_t link;
    xiaotai_err_t (*before_login)(void *ctx);
    void *guest_ctx;
    xiaotai_http_t *http;
    atomic_uint receive_generation;
    uint32_t address_generation, link_epoch, hold_next, bounded_hold;
    int64_t retry_at, clock_deadline, expiry, refresh_at, last_autologin, bounded_hold_until;
    bool enabled, boot_failed, net_event, mac_valid, link_live, destroy_pending, destroy_failed;
    bool auth_pending, step_running, autologin_done, link_starting, ready_pending, pause_after_destroy, refresh_retry;
    bool resume_link, shutdown_failed;
    uint8_t mac[6];
    platform_hold_t hold[4];
} platform_session_t;

typedef struct {
    char device[XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1], stamp[24], nonce[17], signature[45];
} platform_signature_t;
enum { PLATFORM_SETUP_BODY_BYTES = 8192 };
typedef struct {
    xiaotai_http_response_t response;
    char *body;
    size_t capacity, size;
    bool allocated;
} platform_setup_reply_t;
xiaotai_err_t platform_signature_make(platform_signature_t *sig);
size_t platform_signature_headers(const platform_signature_t *sig, xiaotai_http_header_t out[4]);
xiaotai_err_t platform_session_http(uint32_t revision, const xiaotai_http_request_t *request,
                                    platform_setup_reply_t *reply);
void platform_setup_reply_release(platform_setup_reply_t *reply, bool wipe);
xiaotai_err_t platform_discover(uint32_t revision);
xiaotai_err_t platform_login(uint32_t revision, const uint8_t mac[6], char *device, char *token,
                              bool *unbound, int64_t *expiry);
int64_t platform_token_refresh_at(int64_t expiry, int64_t now);
void platform_session_network(bool up, const uint8_t *mac, bool net_event, uint32_t generation);
void platform_session_auth_failed(void);
void platform_session_binding_clean(void); /* worker, lock held, old provider destroyed */
bool platform_event_binding_offline(void); /* worker, lock held, post once before the owner fence */
void platform_event_cancel_online(void); /* lock held, offline supersedes pending ONLINE/PROFILE */
uint32_t platform_session_step(void); /* worker: wait to next due step, 0: ready */
void platform_session_shutdown(void); /* worker: host reset */
bool platform_session_current(uint32_t revision); /* lock held */
xiaotai_err_t platform_session_link_register(const platform_session_link_t *link);
/* Call after forwarding from any real-time signaling callback. This updates
 * session state through the core mutex and is not a raw receive callback. */
void platform_session_link_event(uint32_t epoch, platform_link_event_t event);
xiaotai_err_t platform_session_guest_register(xiaotai_err_t (*before_login)(void *), void *ctx);
void platform_event_publish(platform_event_t event, int32_t value); /* lock held */
void platform_events_flush(void); /* worker, locks internally */
xiaotai_err_t platform_signaling_default_link(void); /* before starting the worker */
uint32_t platform_signaling_step(void); /* worker, forwards port events */
void platform_signaling_reset(void); /* worker, after provider stop/destroy */
bool platform_signaling_heap_reaped(void); /* diagnostics, after successful hold */
bool platform_signaling_unbind_issuing(xiaotai_mqtt_t *client); /* MQTT receive owner */
void platform_signaling_unbind_queued(int32_t id); /* MQTT receive owner */
bool platform_signaling_unbind_published(int32_t id); /* worker */
xiaotai_err_t xiaotai_platform_at_dev_register(void); /* development builds only */
void platform_holds_finish(xiaotai_err_t result); /* lock held */
bool platform_holds_allow_discovery(void); /* lock held, at least one hold active */
uint32_t platform_holds_step(void); /* worker, locks internally */
uint32_t platform_holds_check(void); /* lock held; includes the bounded hold's wake deadline */
void platform_holds_drop_bounded(void); /* lock held */
void platform_holds_retry_destroy(void); /* lock held; one recovery on bounded release */

void platform_binding_signal(bool confirmed, int32_t ack_id, uint32_t generation, void *ctx);
void platform_binding_request_lost(uint32_t generation); /* owner */
bool platform_binding_prior_generation(uint32_t generation); /* owner, unbind only */
bool platform_binding_in_transition(void); /* lock held, quiesce/stop/old replies */
void platform_binding_login_unbound(void); /* worker, lock held */
void platform_binding_online(void); /* lock held */
uint32_t platform_binding_step(void); /* worker */
size_t platform_binding_queue_index(void); /* lock held */

typedef enum {PLATFORM_FREE, PLATFORM_DRAFT, PLATFORM_QUEUED, PLATFORM_RUNNING, PLATFORM_REPLY} platform_slot_state_t;

typedef struct {
    platform_slot_state_t state;
    bool is_transfer, cancelled;
    uint16_t exchange_timeout_ms;
    uint32_t id, generation, session_revision, network_revision, profile_id;
    int64_t deadline_us;
    xiaotai_platform_method_t method;
    xiaotai_platform_auth_t auth;
    uint32_t flags;
    char service[XIAOTAI_PLATFORM_SERVICE_NAME_MAX];
    char path[XIAOTAI_PLATFORM_PATH_MAX];
    char *body;                     /* resident on external-RAM products, otherwise owned by this slot */
    size_t body_size;
    union {
        struct { xiaotai_platform_reply_fn_t fn; void *ctx; } reply;
        struct {
            xiaotai_platform_stream_t stream;
            char content_type[XIAOTAI_PLATFORM_CONTENT_TYPE_MAX];
            size_t source_size, response_max;
            uint32_t stall_ms;
        } transfer;
    } handler;
} platform_slot_t;

typedef struct {
    xiaotai_platform_reply_t reply;
    xiaotai_platform_transfer_result_t transfer;
    /* When the response limit fits an internal-RAM body, allocated only from
     * that worker exchange and wiped after the owner's handler returns. */
    char *temporary_body;
    size_t temporary_capacity;
} platform_result_t;

typedef enum { PROFILE_IDLE, PROFILE_WAIT, PROFILE_POSTED, PROFILE_SENDING } platform_profile_phase_t;
typedef struct {
    platform_profile_phase_t phase;
    uint32_t id;
    int64_t due;
    unsigned attempts;
    bool deferred, in_slot, held_before_wire;
} platform_profile_t;

typedef struct {
    xiaotai_platform_config_t config;
    xiaotai_platform_requirement_t need;
    xiaotai_platform_status_t status;
    xiaotai_platform_stats_t stats;
    xiaotai_mutex_t lock;
    xiaotai_signal_t wake, exited;
    xiaotai_task_t task;
    atomic_bool initialized, started, start_failed, running, stopped;
    bool external, provisioning, objects, profile_pending, profile_ready_input;
    bool network_up;
    bool idle_pending, connection_close;
    uint32_t session_revision, network_revision, gate, gate_closed, id_next;
    atomic_uint_least32_t dormant_gate;
    char *token;                    /* session package publishes a copy here */
    char *bearer;                   /* one AUTH_BEARER exchange at a time */
    platform_service_t *services;
    size_t service_count;
    char required[16][XIAOTAI_PLATFORM_SERVICE_NAME_MAX];
    size_t required_count;
    platform_slot_t *slots;
    uint8_t *pending;               /* bounded queue of byte-sized slot indices */
    size_t slot_count, queue_count, request_limit, profile_limit, response_limit;
    char *response;                 /* boot-long only with external RAM */
    xiaotai_http_t *client;          /* worker owns, except HTTP cancel */
    platform_slot_t *io;            /* which transfer may cancel client I/O */
    int64_t keep_until;
    unsigned active;                /* occupied request/transfer slots */
    xiaotai_platform_exchange_preflight_fn_t preflight;
    void *preflight_ctx;
    uint32_t resume_revision;
    platform_reply_hooks_t reply_hooks;
    platform_session_t session;
    platform_binding_t binding;
    platform_profile_t profile;
    uint16_t event_pending;
    int32_t event_values[PLATFORM_EVENT_COUNT];

    /* One retained owner handoff; worker never advances past a reply until
     * owner has finished it. Preflights can expire before an owner turn. */
    uint32_t handoff;
    bool handoff_done, handoff_running, preflight_deferred;
    xiaotai_err_t preflight_result;
    uint32_t preflight_resume_at;
    platform_slot_t *handoff_slot;
    platform_result_t *handoff_reply;
} platform_t;

extern platform_t platform;
void platform_lock(void);
void platform_unlock(void);
void platform_wake(void);
bool platform_active_now(void);
void platform_count_publish(void);  /* lock held */
void platform_idle_flush(void);      /* worker: keeps a lost zero-transition */
xiaotai_err_t platform_service_find(const char *name, const char **base); /* lock held */
bool platform_slot_stale(const platform_slot_t *slot); /* lock held */
void platform_slot_release(platform_slot_t *slot); /* worker, lock held */
void platform_slot_body_free(platform_slot_t *slot); /* release on-demand storage only */
void platform_http_run(platform_slot_t *slot, platform_result_t *result);
void platform_worker(void *ctx);
uint32_t platform_remaining_ms(int64_t deadline_us);
void platform_profile_online(void); /* lock held */
void platform_profile_offline(void); /* lock held */
uint32_t platform_profile_step(void); /* worker */
void platform_profile_reset(void); /* after worker exits, before freeing request slots */
bool platform_profile_current(uint32_t id); /* lock held */
bool platform_profile_hold_blocks(uint32_t id); /* lock held, unsent request */
int platform_profile_document(char *out, size_t capacity); /* owner */
size_t platform_profile_body_limit(void); /* start, after scene registration */
xiaotai_err_t platform_profile_request(uint32_t id, xiaotai_platform_reply_fn_t reply); /* owner */
xiaotai_err_t platform_diagnostics_register(void); /* init */
#define PLATFORM_DIAGNOSTIC_STATS_ROWS 4U
bool platform_diagnostic_status(const xiaotai_platform_status_t *s, char *out, size_t capacity);
bool platform_diagnostic_stats_row(const xiaotai_platform_stats_t *stats, unsigned row, char *out, size_t capacity);
void platform_http_trace(const platform_slot_t *slot, const xiaotai_http_response_t *response, xiaotai_err_t err);
bool platform_http_trace_format(const platform_slot_t *slot, const xiaotai_http_response_t *response,
                                xiaotai_err_t err, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
