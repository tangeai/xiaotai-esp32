// SPDX-License-Identifier: MIT
#pragma once
/* Internal seams of the contacts module, shared by its source files and its
 * host tests.
 *
 *   contacts_parse.c   the list reply, read through the source descriptor
 *                      (pure: no state, no lock, no allocation)
 *   contacts.c         state, configuration, kinds, view, lookups, events,
 *                      intents, lifecycle
 *   contacts_fetch.c   when to fetch, replies, deferral, signals, queries
 *   contacts_match.c   name normalization and matching
 *
 * Threading. The owner task writes the state; every [any] reader copies under
 * `lock`, and nothing but memory copies happens while it is held. Fields
 * marked "owner" are read and written on the owner task only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/json_view.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/port/os.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTACTS_TAG "contacts"

/* Defaults of xiaotai_contacts_config_t. */
#define CONTACTS_CAPACITY_EXTERNAL 64U
#define CONTACTS_CAPACITY_INTERNAL 8U
#define CONTACTS_CAPACITY_MAX 256U
#define CONTACTS_PAGE_SIZE 8U
#define CONTACTS_PAGE_SIZE_MAX 32U
#define CONTACTS_PENDING_PAGE_SIZE 4U
#define CONTACTS_PENDING_PAGE_SIZE_MAX 16U
#define CONTACTS_RESPONSE_BYTES 8192U

/* The parser admits one token per this many configured reply bytes. The densest
 * entry the sources serve ({"type":"voip","device_id":"…","wx_app_id":"…"}
 * with real ids) takes more than 9 bytes per token. Keep the original
 * admission limit even though validation no longer stores the tokens. */
#define CONTACTS_REPLY_BYTES_PER_TOKEN 8U

/* A query: the request itself, and the whole query by default. */
#define CONTACTS_QUERY_REQUEST_MS 4000U
#define CONTACTS_QUERY_TIMEOUT_MS 10000U

/* Largest body of a friendship change: a remark of 256 bytes escaped, the
 * peer id and the keys. Declared only for sources with friendships. */
#define CONTACTS_REQUEST_BODY_BYTES 768U

/* Longest remark or name read from a reply before it is cut to
 * XIAOTAI_CONTACT_NAME_MAX at a character boundary (a remark is at most
 * XIAOTAI_CONTACT_REMARK_BYTES_MAX bytes). */
#define CONTACTS_NAME_READ_MAX XIAOTAI_CONTACT_REMARK_BYTES_MAX

/* ------------------------------------------------------------------- parse */

typedef struct {
    const xiaotai_contacts_source_t *source;
    const xiaotai_contact_kind_def_t *const *kinds;   /* kind_count descriptions, by kind number */
    size_t kind_count;
    /* The entries the cache should hold: those of this kind (or
     * XIAOTAI_CONTACT_ANY) around this page. */
    int kind;
    uint16_t page;
    uint8_t page_size;
    uint16_t capacity;
    xiaotai_contact_t *cache;     /* capacity entries */
    xiaotai_contact_t *firsts;    /* kind_count entries */
    xiaotai_contact_t *dial_prefix;
    uint16_t dial_prefix_capacity;
    size_t token_capacity;
    bool reject_invalid_known_rows;
} contacts_parse_in_t;

typedef struct {
    xiaotai_json_view_t root;
    xiaotai_json_view_t contacts;
    size_t body_size;            /* whole reply, including surrounding whitespace */
    int32_t code;
} contacts_reply_t;

typedef struct {
    uint16_t total;                            /* accepted entries */
    uint16_t totals[XIAOTAI_CONTACT_KINDS_MAX];
    uint16_t skipped;                          /* malformed or of an unregistered type */
    uint16_t unregistered;                     /* of which, of an unregistered type */
    bool first[XIAOTAI_CONTACT_KINDS_MAX];     /* firsts[k] holds the first entry of kind k */
    uint16_t dial_prefix_count;
    /* The cache now holds the entries cache_offset .. cache_offset + cached - 1
     * of the list filtered by cache_kind (XIAOTAI_CONTACT_ANY: unfiltered). */
    int8_t cache_kind;
    uint16_t cache_offset;
    uint16_t cached;
    uint16_t dropped;                          /* accepted entries not in memory */
} contacts_parse_out_t;

/* Reads the envelope: HTTP 200, exactly one integral top-level "code" equal
 * to the source's success code, "data.contacts" an array. Views borrow the
 * reply body. XIAOTAI_FAIL: not a contact list (also for a NULL body);
 * XIAOTAI_ERR_INVALID_SIZE: more tokens than the configured limit. */
xiaotai_err_t contacts_parse_reply(const contacts_parse_in_t *in, int http_status, const char *body, size_t size,
                                   contacts_reply_t *reply);
xiaotai_err_t contacts_parse_envelope(size_t capacity, int http_status, const char *body, size_t size,
                                     contacts_reply_t *reply);
bool contacts_reply_array(const contacts_reply_t *reply, const char *name, xiaotai_json_view_t *array);
xiaotai_err_t contacts_validate_remark(const char *remark);

/* Counts and copies a reply contacts_parse_reply() accepted. A strict rejection
 * returns INVALID_RESPONSE without touching in->cache or in->firsts. */
xiaotai_err_t contacts_parse_fill(const contacts_parse_in_t *in, const contacts_reply_t *reply, contacts_parse_out_t *out);
xiaotai_err_t contacts_query_remark(const contacts_parse_in_t *input, const contacts_reply_t *reply, int kind, size_t index,
                                    char *out, size_t capacity, size_t *length);

/* Both steps; for the parser's own tests. */
xiaotai_err_t contacts_parse_list(const contacts_parse_in_t *in, int http_status, const char *body, size_t size,
                                  contacts_parse_out_t *out);

/* Copies source into dest (capacity bytes with the terminator) up to the last
 * whole well-formed UTF-8 character that fits; stops at the first malformed
 * sequence. */
void contacts_copy_utf8(char *dest, size_t capacity, const char *source);

/* ------------------------------------------------------------------ state */

typedef struct {
    /* Configuration, resolved at init. */
    xiaotai_contacts_config_t config;
    const xiaotai_contacts_source_t *source;
    bool idle_only;
    bool provisioning_boot;
    bool initialized, started;

    /* Storage (XIAOTAI_MEM_BULK, init). */
    xiaotai_contact_t *cache;
    xiaotai_contact_t *firsts;
    char (*pending_ids)[XIAOTAI_CONTACT_ID_MAX + 1U];
    size_t token_capacity;

    /* Under lock. */
    xiaotai_mutex_t lock;
    bool lock_ready;
    xiaotai_contacts_status_t status;
    bool first[XIAOTAI_CONTACT_KINDS_MAX];
    int8_t cache_kind;
    uint16_t cache_offset;
    uint16_t dial_prefix_count;
    xiaotai_contacts_stats_t stats;
    xiaotai_contacts_pending_status_t pending;
    uint32_t pending_generation;

    /* Owner. */
    bool online;              /* platform.online and no platform.offline since */
    bool profile_ok;          /* the profile was accepted in this session */
    bool want;                /* a fetch is requested and not yet sent */
    bool deferred;            /* the present request has been counted as deferred */
    bool inflight;            /* a list request was accepted by the platform */
    bool again;               /* a refresh began while one was in flight: its reply is void */
    uint32_t serial;          /* of the list request in flight */
    bool query_pending;
    uint32_t query_serial;
    uint32_t query_timer;
    int8_t query_kind;
    xiaotai_contacts_answer_fn_t query_answer;
    void *query_ctx;
    bool default_type_checked;
    uint32_t intents_refused; /* contacts.show with an unknown kind name */
    bool pending_wanted, pending_inflight, pending_again;
    bool require_initial_confirmation;
    uint32_t pending_serial;
    uint32_t confirmation_generation;
    bool change_inflight;
    uint32_t change_generation, change_serial;
    xiaotai_contacts_change_result_t change;
} contacts_state_t;

extern contacts_state_t g_contacts;

/* contacts.c */
void contacts_lock(void);
void contacts_unlock(void);
/* Kinds registered, as the parser reads them. */
const xiaotai_contact_kind_def_t *const *contacts_kinds(size_t *count);
bool contacts_kind_valid(int kind);
/* Recomputes the derived fields of status (pages, page_count, truncated,
 * cached, loading) and clamps the page. Lock held. */
void contacts_view_update_locked(void);
/* Whether the selected page is in memory. Lock held. */
bool contacts_view_served_locked(void);
/* Entry `index` of the list filtered by `kind`, when it is in memory. Lock held. */
const xiaotai_contact_t *contacts_entry_locked(int kind, uint16_t index);
/* Whether a lookup may answer under these flags. Lock held. */
xiaotai_err_t contacts_usable_locked(uint32_t flags);
/* revision + 1, then XIAOTAI_EVENT_CONTACTS_CHANGED with a snapshot. Owner. */
void contacts_changed(void);
/* Empties the cache when it belongs to an earlier binding generation;
 * returns true when it did. Owner. */
bool contacts_check_generation(void);
/* Takes a reply contacts_parse_reply() accepted into the cache, for the
 * selection (kind, page), with fresh as given. Owner. */
xiaotai_err_t contacts_apply(const contacts_reply_t *reply, int kind, uint16_t page, uint32_t generation, bool fresh);

/* contacts_fetch.c */
xiaotai_err_t contacts_fetch_init(void);    /* subscriptions; init */
/* A fetch is wanted: the list stops being fresh; a fetch in flight becomes
 * void. Owner. */
void contacts_fetch_want(void);
/* Sends the wanted fetch when nothing holds it back. Owner. */
void contacts_fetch_kick(void);

xiaotai_err_t contacts_change_init(void);
void contacts_pending_invalidate(bool refresh, bool clear_selection);
void contacts_pending_kick(void);
bool contacts_pending_check_generation(void);
xiaotai_err_t contacts_at_init(void);

/* contacts_match.c */
/* Removes ASCII white space and lower-cases ASCII letters. False when the
 * result is empty or does not fit capacity (with the terminator). */
bool contacts_normalize_name(const char *input, char *output, size_t capacity);

#ifdef __cplusplus
}
#endif
