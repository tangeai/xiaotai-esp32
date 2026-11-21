// SPDX-License-Identifier: MIT
#pragma once
/* Contacts module: the people and devices this device may call.
 *
 * The list lives on the server, at the place the product's source descriptor
 * names (xiaotai_contacts_source_t: by default GET <call>/v1/call/device/contacts,
 * where every kind of contact is listed together; a product whose contacts
 * come from another service names that one). This module mirrors it, keeps it
 * current (when the platform is online or after the device profile was
 * accepted, as configured; on the signals "callers_update" and
 * "contacts_update"; on request), offers it to applications as pages and to
 * the call modules as lookups, and, where the source serves them, carries the
 * friendship operations between devices (request, accept, reject, remark,
 * remove) and the list of requests waiting for an answer.
 *
 * Cache and pages. The module caches up to `capacity` contacts (a product
 * option; small on boards without external RAM). An application looks at the
 * list through a view: a filter and a page of at most page_size entries.
 * When the whole filtered list fits the cache, changing the view is served
 * from memory; when it does not, the module fetches again and keeps the
 * entries around the selected page. Callers therefore never assume that the
 * whole list is in memory: lookups search what is cached, and status.total
 * tells how many there are.
 *
 * The cache belongs to one binding: it is emptied when the binding generation
 * changes (xiaotai/identity.h) and survives everything else (sessions, link
 * loss, a failed refresh keeps the previous list).
 *
 * Kinds. Who a contact is (another device, a user of a messaging service, a
 * telephone line) is a kind that the module dialling it registers at init
 * (xiaotai_contacts_kind_register): the server's type text, the extra fields
 * an entry carries and which of them are required. This module knows no kind
 * by itself; a further call module registers one more and changes nothing
 * here.
 *
 * The module shows nothing and dials nothing. Texts for contacts without a
 * name are the application's.
 *
 * Manifest name "contacts". Depends on the module binding and on the engines
 * platform, identity, eventlog and at. No hardware need of its own.
 *
 * Task tags as in xiaotai/framework.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/identity.h"
#include "xiaotai/modules/contacts_config.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ contacts */

/* Text lengths without the terminator. */
#define XIAOTAI_CONTACT_ID_MAX 64U
#define XIAOTAI_CONTACT_NAME_MAX 64U
/* Longest extra field of a kind (xiaotai_contact_kind_def_t.extra_fields). */
#define XIAOTAI_CONTACT_EXTRA_MAX 64U
/* Extra fields a kind may name. Together with the extra storage of every
 * cached entry this is fixed, so a kind costs no memory per entry beyond it. */
#define XIAOTAI_CONTACT_EXTRAS 2U
/* XIAOTAI_CONTACT_KINDS_MAX, the kinds that can be registered in one product,
 * is a build option (xiaotai/modules/contacts_config.h). */
/* Longest remark accepted by xiaotai_contacts_change: 64 Unicode characters
 * and 256 bytes of well-formed UTF-8, whichever is reached first. */
#define XIAOTAI_CONTACT_REMARK_CHARS_MAX 64U
#define XIAOTAI_CONTACT_REMARK_BYTES_MAX 256U

/* Kind argument of views, lookups and queries meaning "every kind". A kind
 * itself is the number xiaotai_contacts_kind_register() gave it: 0 ..
 * XIAOTAI_CONTACT_KINDS_MAX - 1, in registration order. */
#define XIAOTAI_CONTACT_ANY (-1)

typedef struct {
    /* The registered kind; never XIAOTAI_CONTACT_ANY. */
    int8_t kind;
    /* The contact's id from the source's id field (a peer's device id, a
     * user's id at its service). Never empty. */
    char id[XIAOTAI_CONTACT_ID_MAX + 1U];
    /* The remark, else the server's name for the contact, else "": the
     * application then shows its own text (or the id). UTF-8, cut at a
     * character boundary. */
    char name[XIAOTAI_CONTACT_NAME_MAX + 1U];
    /* Meaningful only for a kind with presence, and then only when
     * online_known (the entry carried "online"). */
    bool online_known;
    bool online;
    /* The kind's extra fields, in the order of its extra_fields; "" where the
     * kind names none or the entry left an optional one out. The module that
     * registered the kind reads them by index. */
    char extra[XIAOTAI_CONTACT_EXTRAS][XIAOTAI_CONTACT_EXTRA_MAX + 1U];
} xiaotai_contact_t;

/* ---------------------------------------------------------------------- kinds */

/* A kind, as the module that dials it describes it. All strings static. */
typedef struct {
    /* Name in views, intents ("contacts.show") and AT+CONTACTS: "device".
     * Lower-case letters, digits and '_', 1..15 characters, unique. */
    const char *name;
    /* Second word AT+CONTACTS accepts for it (case-insensitive); NULL: none. */
    const char *alias;
    /* Value of the source's type field that selects this kind ("device"), and
     * the type a source without a type field names as its default_type.
     * Unique among the registered kinds. */
    const char *server_type;
    /* Entry fields copied into xiaotai_contact_t.extra, by index; NULL for an
     * unused slot. A value longer than XIAOTAI_CONTACT_EXTRA_MAX makes the
     * entry malformed, except that the strict list policy ignores malformed
     * optional extras. */
    const char *extra_fields[XIAOTAI_CONTACT_EXTRAS];
    /* Bit i: an entry whose extra i is missing or empty is malformed. */
    uint8_t extra_required;
    /* Entries carry an online state ("online"); without, online_known stays
     * false and nobody pretends to know. */
    bool presence;
    /* Contacts of this kind take part in the source's friendship operations
     * (removal by position; pending requests are of this kind). */
    bool friendships;
} xiaotai_contact_kind_def_t;

/* [init] Register a kind; called by the module that dials it, in its own
 * init (after this module's: it lists "contacts" among its depends). def is
 * kept by pointer. *kind (may be NULL) receives its number.
 * XIAOTAI_ERR_INVALID_ARG  a name, alias or type that is missing, malformed or
 *                          already registered; extra_required naming an
 *                          unused slot
 * XIAOTAI_ERR_NO_MEM       XIAOTAI_CONTACT_KINDS_MAX kinds exist
 * Entries of a server type no kind registered are skipped (status.skipped). */
xiaotai_err_t xiaotai_contacts_kind_register(const xiaotai_contact_kind_def_t *def, int *kind);

/* [any] The number of the kind with this name or alias (case-insensitive).
 * XIAOTAI_ERR_NOT_FOUND when none has it. */
xiaotai_err_t xiaotai_contacts_kind_find(const char *name, int *kind);

/* [any] The registered description of a kind; NULL for a number that is not
 * one. */
const xiaotai_contact_kind_def_t *xiaotai_contacts_kind(int kind);

/* [any] Kinds registered so far. */
size_t xiaotai_contacts_kind_count(void);

/* -------------------------------------------------------------------- status */

typedef struct {
    /* Increases whenever the cached list, the view or any field below changes. */
    uint32_t revision;
    /* Binding generation the cache belongs to; 0 while it is empty because no
     * list was ever fetched for the present binding. */
    uint32_t generation;
    /* A list was received for the present binding and nothing has invalidated
     * it since (a refresh that began, a signal, a failed refresh, a binding
     * change). The lookups below answer from a list that is not fresh only
     * with XIAOTAI_CONTACTS_STALE_OK; the pages stay readable for display. */
    bool fresh;
    bool loading;            /* a fetch is in flight or waits for the device to be idle */
    /* Result of the last fetch that ended: XIAOTAI_OK, a platform result, or
     * XIAOTAI_FAIL for a reply that is not a contact list. A failed fetch
     * keeps the previous list (fresh is then false). */
    xiaotai_err_t error;
    uint16_t total;          /* contacts on the server that this module accepts */
    uint16_t totals[XIAOTAI_CONTACT_KINDS_MAX];   /* of which, by kind number */
    uint16_t skipped;        /* entries of the last reply dropped as malformed or of an unregistered type */
    uint16_t cached;         /* contacts in memory */
    /* The view: a kind number, or XIAOTAI_CONTACT_ANY. */
    int8_t kind;
    uint16_t page;           /* from 0 */
    uint16_t pages;          /* pages the filtered list has; at least 1 */
    uint8_t page_size;       /* entries per page (product option) */
    uint8_t page_count;      /* entries on the selected page now; 0 while it is being fetched */
    bool truncated;          /* the filtered list is longer than the cache: other pages need a fetch */
} xiaotai_contacts_status_t;

/* [any] Snapshot. */
void xiaotai_contacts_status(xiaotai_contacts_status_t *status);

/* ---------------------------------------------------------------------- list */

/* [owner] Fetch the list again. Marks the cache not fresh at once.
 * XIAOTAI_OK also when a fetch is already in flight (it is then repeated once
 * it ended, because the list may have changed meanwhile).
 * XIAOTAI_ERR_PLATFORM_OFFLINE (xiaotai/platform.h) while the device is not
 * online: the fetch is remembered and runs when it is. */
xiaotai_err_t xiaotai_contacts_refresh(void);

/* [owner] Select the view: the contacts of one kind (a registered kind number)
 * or of every kind (XIAOTAI_CONTACT_ANY). A page beyond the end selects the
 * last page. When the selected page is not in memory a fetch begins
 * (status.loading, page_count 0) and XIAOTAI_EVENT_CONTACTS_CHANGED follows.
 * XIAOTAI_ERR_INVALID_ARG for an unknown kind. */
xiaotai_err_t xiaotai_contacts_show(int kind, uint16_t page);

/* [any] Copies the entries of the selected page, in the server's order, and
 * returns how many (at most capacity and status.page_count). For display: it
 * answers from a list that is not fresh too (check status.fresh). */
size_t xiaotai_contacts_page(xiaotai_contact_t *contacts, size_t capacity);

/* The lookups below are what a call module dials from. Which list they
 * answer from is the caller's choice, through flags:
 *   0                           a fresh list only (status.fresh): during a
 *                               refresh, after a signal and after a failed
 *                               refresh they answer XIAOTAI_ERR_INVALID_STATE
 *                               and copy nothing;
 *   XIAOTAI_CONTACTS_STALE_OK   any list of the present binding: the cached
 *                               list answers also while it is not fresh, as
 *                               long as it was received for the binding
 *                               generation in force now (status.generation ==
 *                               xiaotai_identity_generation(), not 0); the
 *                               server checks the target again when it is
 *                               dialled.
 * Either way a list of an earlier binding is never handed out: before the
 * first list of the present binding every lookup answers
 * XIAOTAI_ERR_INVALID_STATE. status.fresh is reported as it is in both modes.
 * Unknown flag bits: XIAOTAI_ERR_INVALID_ARG. */
enum {
    XIAOTAI_CONTACTS_STALE_OK = 1u << 0,
};

/* [any] One entry of the selected page by its position (0 is the first).
 * XIAOTAI_ERR_NOT_FOUND beyond the page. */
xiaotai_err_t xiaotai_contacts_at(uint8_t index, uint32_t flags, xiaotai_contact_t *contact);

/* [any] The cached contact with this id. kind narrows the search when the
 * same id could exist under several kinds; XIAOTAI_CONTACT_ANY for any.
 * XIAOTAI_ERR_NOT_FOUND when it is not in memory (which, with
 * status.truncated, does not prove it is not a contact). */
xiaotai_err_t xiaotai_contacts_find(const char *id, int kind, uint32_t flags, xiaotai_contact_t *contact);

/* [any] Require one exact id of a concrete kind with known online presence
 * in the first at_device_dial_prefix accepted rows, irrespective of the view.
 * NOT_FOUND for no match, INVALID_STATE if ambiguous or the prefix cannot be
 * used under the freshness/binding flags, NOT_SUPPORTED when it is disabled. */
xiaotai_err_t xiaotai_contacts_find_unique_online_prefix(const char *id, int kind, uint32_t flags,
                                                        xiaotai_contact_t *contact);

/* [any] The first contact of a kind in the server's order ("call the first
 * contact of this kind"). It is kept in memory even when it lies outside the
 * cached range. XIAOTAI_ERR_NOT_FOUND when there is none;
 * XIAOTAI_ERR_INVALID_ARG for a number that is not a registered kind. */
xiaotai_err_t xiaotai_contacts_first(int kind, uint32_t flags, xiaotai_contact_t *contact);

/* [any] A contact's zero-based position within one registered kind in the
 * server's order, independent of the selected view. Position 0 is also
 * available from the separately kept first entry. XIAOTAI_ERR_NOT_FOUND if
 * the cache cannot prove and supply the position. Does not change the view
 * or start a request. */
xiaotai_err_t xiaotai_contacts_at_kind(int kind, uint16_t index, uint32_t flags, xiaotai_contact_t *contact);

/* [any] Find a contact by a spoken or typed name. Names are compared after
 * removing ASCII white space and lower-casing ASCII letters; they must then be
 * equal. kind: a kind number, or XIAOTAI_CONTACT_ANY. id: when not NULL and
 * not "", the match must also have this id (a name is never replaced by an id
 * somebody else supplied). Searches the cache only.
 * XIAOTAI_OK: exactly one match, copied. XIAOTAI_ERR_NOT_FOUND: none.
 * XIAOTAI_ERR_INVALID_STATE: several (*matches says how many; nothing is
 * copied), or no list the flags accept (*matches 0). matches may be NULL. */
xiaotai_err_t xiaotai_contacts_match(const char *name, int kind, const char *id, uint32_t flags,
                                     xiaotai_contact_t *contact, size_t *matches);

/* ------------------------------------------------------------ online queries */

/* Result of xiaotai_contacts_query. contacts points to storage of the module
 * and is valid during the callback only. */
typedef struct {
    /* XIAOTAI_OK: contacts holds the answer. XIAOTAI_ERR_PLATFORM_STALE:
     * the binding changed. Other platform results as they are. */
    xiaotai_err_t result;
    const xiaotai_contact_t *contacts;
    size_t count;
    bool truncated;        /* more contacts matched than the cache holds */
    bool deadline_expired; /* TIMEOUT from the query timer, not the upstream request */
    size_t response_bytes; /* received list envelope, when an answer was received */
    const void *source;    /* borrowed by xiaotai_contacts_answer_remark only */
} xiaotai_contacts_answer_t;

/* [owner] Within the callback, read the original remark before display
 * truncation or fallback to the server name. NOT_FOUND if absent/empty.
 * Capacity and measured length follow xiaotai_json_string. */
xiaotai_err_t xiaotai_contacts_answer_remark(const xiaotai_contacts_answer_t *answer, size_t index,
                                            char *out, size_t capacity, size_t *length);

/* [owner] Exactly once for every accepted query. */
typedef void (*xiaotai_contacts_answer_fn_t)(const xiaotai_contacts_answer_t *answer, void *ctx);

/* [owner] Ask the server now and answer with the contacts of the kind (or
 * XIAOTAI_CONTACT_ANY) and their present online state (an assistant's "is
 * anybody online?"). XIAOTAI_ERR_INVALID_ARG for an unknown kind. The reply
 * also refreshes the cache. One query at a time: XIAOTAI_ERR_BUSY while one is
 * pending. timeout_ms: for the whole query; 0 selects 10000. The request
 * itself is given 4000 ms. Unlike a refresh, a query is sent while a
 * foreground activity is current.
 * XIAOTAI_ERR_PLATFORM_OFFLINE when the device is not online (answer is then
 * never called). */
xiaotai_err_t xiaotai_contacts_query(int kind, uint32_t timeout_ms, xiaotai_contacts_answer_fn_t answer,
                                     void *ctx);
/* [owner] Cancel only the query named by its callback and context. The
 * callback receives INVALID_STATE once; a later upstream result is stale. */
xiaotai_err_t xiaotai_contacts_query_cancel(xiaotai_contacts_answer_fn_t answer, void *ctx);

/* -------------------------------------------------------- friendship changes */

/* A further operation is one more value. */
typedef enum {
    XIAOTAI_CONTACTS_REQUEST = 0,  /* ask `id` (of a kind with friendships) to become a contact */
    XIAOTAI_CONTACTS_ACCEPT,       /* answer a pending request of `id` */
    XIAOTAI_CONTACTS_REJECT,
    XIAOTAI_CONTACTS_REMARK,       /* set the remark of contact `id`; NULL or "" clears it */
    XIAOTAI_CONTACTS_REMOVE,       /* end the friendship with `id` (of a kind with friendships) */
} xiaotai_contacts_change_kind_t;

typedef struct {
    xiaotai_contacts_change_kind_t kind;
    const char *id;       /* 1..XIAOTAI_CONTACT_ID_MAX characters; copied */
    const char *remark;   /* REMARK only; copied */
} xiaotai_contacts_change_t;

/* [owner] Send one change. One at a time; the outcome is
 * XIAOTAI_EVENT_CONTACTS_CHANGE_DONE, and after a success the list (and for
 * ACCEPT and REJECT the pending list) is fetched again.
 * XIAOTAI_ERR_NOT_SUPPORTED     the source serves no friendships
 *                               (xiaotai_contacts_source_t.friendships)
 * XIAOTAI_ERR_INVALID_ARG       bad id or malformed UTF-8 remark
 * XIAOTAI_ERR_INVALID_SIZE      remark exceeds the byte or character limit
 * XIAOTAI_ERR_NOT_FOUND         ACCEPT or REJECT of an id that is not in the
 *                               cached pending list
 * XIAOTAI_ERR_BUSY              another change is in flight
 * XIAOTAI_ERR_PLATFORM_OFFLINE  the device is not online */
xiaotai_err_t xiaotai_contacts_change(const xiaotai_contacts_change_t *change);

/* ---------------------------------------------------------- pending requests */

typedef struct {
    uint32_t revision;       /* increases whenever the pending list or its view changes */
    bool fresh;
    bool loading;
    xiaotai_err_t error;     /* of the last fetch that ended */
    uint16_t total;
    uint16_t page;           /* from 0 */
    uint16_t pages;          /* at least 1 */
    uint8_t page_size;       /* product option */
    uint8_t page_count;
    int32_t code;            /* validated positive platform rejection code; 0 if none */
} xiaotai_contacts_pending_status_t;

/* [any] */
void xiaotai_contacts_pending_status(xiaotai_contacts_pending_status_t *status);

/* [init] Ask the owner to fetch pending confirmations after the initial list
 * of each platform session. [owner] Read whether that request completed in
 * the current binding. Completion does not imply a successful reply; errors
 * remain in xiaotai_contacts_pending_status(). */
xiaotai_err_t xiaotai_contacts_require_initial_confirmation(void);
bool xiaotai_contacts_initial_confirmation_done(void);

/* [owner] Select a page of the requests waiting for this device's answer
 * (GET <source path>/pending) and fetch it. A page beyond the end selects the
 * last one. XIAOTAI_ERR_NOT_SUPPORTED when the source serves no friendships. */
xiaotai_err_t xiaotai_contacts_pending_show(uint16_t page);

/* [any] Ids of the selected page (contacts-to-be of the kind with
 * friendships); returns how many. Each id is terminated; ids points to
 * capacity rows. */
size_t xiaotai_contacts_pending_page(char (*ids)[XIAOTAI_CONTACT_ID_MAX + 1U], size_t capacity);

/* ------------------------------------------------------------- configuration */

/* When the list is fetched. */
typedef enum {
    /* IDLE_ONLY on a board without external RAM, ANYTIME otherwise. */
    XIAOTAI_CONTACTS_FETCH_AUTO = 0,
    XIAOTAI_CONTACTS_FETCH_ANYTIME,
    /* Fetches wait until no foreground activity is current: the reply buffer
     * and the session's link do not have to fit together. The list and
     * pending requests are sent with XIAOTAI_PLATFORM_BACKGROUND, so a fetch
     * that was queued when an activity began comes back XIAOTAI_ERR_BUSY
     * unsent (with the platform core's defer_background_while_active) and is
     * deferred again; it runs once XIAOTAI_EVENT_ACTIVITY reports none. A
     * query (xiaotai_contacts_query) is exempt and never flagged. */
    XIAOTAI_CONTACTS_FETCH_IDLE_ONLY,
} xiaotai_contacts_fetch_t;

/* When the list is first fetched after the platform came online. */
typedef enum {
    /* Once the device profile was accepted (platform.profile). A profile
     * that keeps failing leaves the list empty. */
    XIAOTAI_CONTACTS_AFTER_PROFILE = 0,
    /* As soon as the platform is online (platform.online), whatever becomes of
     * the profile, and not again for its acceptance: for products whose device
     * calls must survive a failed profile (the list does not depend on it). */
    XIAOTAI_CONTACTS_ON_ONLINE,
} xiaotai_contacts_first_fetch_t;

/* Where the list comes from and how its reply reads. All strings static. */
typedef struct {
    const char *service;      /* platform service name: "call" */
    const char *path;         /* GET path of the list: "/v1/call/device/contacts" */
    /* Business code of a good reply (200 for the call service); the reply
     * also needs HTTP 200 and data.contacts to be an array. */
    int32_t success_code;
    /* Entry field holding the contact's id: "device_id". */
    const char *id_field;
    /* Entry field naming the kind ("type"): its value is matched against the
     * registered kinds' server_type, an entry of no registered type is
     * skipped. NULL: entries carry no kind and all are of default_type. */
    const char *type_field;
    /* type_field NULL only: the server_type every entry has (a kind must be
     * registered for it, else every entry is skipped). */
    const char *default_type;
    /* The service also serves the pending requests and the friendship changes
     * next to path (path + "/pending", "/request", "/respond", "/remark",
     * DELETE path). false: those functions answer XIAOTAI_ERR_NOT_SUPPORTED. */
    bool friendships;
} xiaotai_contacts_source_t;

/* Product configuration. Every numeric field: 0 selects the default. */
typedef struct {
    /* Contacts kept in memory; default 64 with external RAM, otherwise 8.
     * At most 256. */
    uint16_t capacity;
    uint8_t page_size;            /* default 8; at most 32 and at most capacity */
    uint8_t pending_page_size;    /* default 4; at most 16 */
    /* Largest reply accepted, bytes; default 8192. Declared to the platform
     * core's pool (xiaotai_platform_require). */
    uint16_t response_bytes;
    /* Reject the whole list if a recognized row has invalid required fields.
     * false (default): skip bad rows and install the valid ones. */
    bool reject_invalid_known_rows;
    /* For AT+CALL DEVICE, keep the first N valid rows of all kinds and
     * require a unique online match within them. Zero uses ordinary lookup. */
    uint16_t at_device_dial_prefix;
    xiaotai_contacts_fetch_t fetch;
    xiaotai_contacts_first_fetch_t first_fetch;
    /* NULL: the call service's list, {"call", "/v1/call/device/contacts", 200,
     * "device_id", "type", NULL, true}. The module requires the source's
     * service from the platform core (xiaotai_platform_require), no other.
     * Kept by pointer (static storage). */
    const xiaotai_contacts_source_t *source;
} xiaotai_contacts_config_t;

/* [glue] Before xiaotai_module_contacts_init(), from the product glue; copied. NULL
 * restores the defaults. XIAOTAI_ERR_INVALID_ARG beyond the limits above, an
 * unknown fetch or first_fetch, a source without service, path or id_field,
 * or without type_field and default_type; XIAOTAI_ERR_INVALID_STATE once
 * initialized. Whether a kind exists for default_type is known only after
 * every module's init; start logs it once when none does (the entries are
 * then counted in entries_unregistered). */
xiaotai_err_t xiaotai_contacts_configure(const xiaotai_contacts_config_t *config);

/* -------------------------------------------------------------------- events */

/* The cached list, its freshness or the view changed. args.value: the low 31
 * bits of status.revision; data: a copy of xiaotai_contacts_status_t. */
#define XIAOTAI_EVENT_CONTACTS_CHANGED "contacts.changed"
/* The pending list or its view changed. args.value: status.total; data: a copy
 * of xiaotai_contacts_pending_status_t. */
#define XIAOTAI_EVENT_CONTACTS_PENDING "contacts.pending"
/* A change ended. args.value: the xiaotai_err_t (XIAOTAI_OK, XIAOTAI_FAIL when
 * the server refused, a platform result otherwise); data: a copy of
 * xiaotai_contacts_change_result_t. */
#define XIAOTAI_EVENT_CONTACTS_CHANGE_DONE "contacts.change_done"

typedef struct {
    xiaotai_contacts_change_kind_t kind;
    xiaotai_err_t result;
    int32_t code;          /* the server's business code; XIAOTAI_PLATFORM_NO_CODE when there was none */
    char id[XIAOTAI_CONTACT_ID_MAX + 1U];
} xiaotai_contacts_change_result_t;

/* ------------------------------------------------------------------- intents */

/* xiaotai_contacts_refresh(). No arguments. */
#define XIAOTAI_INTENT_CONTACTS_REFRESH "contacts.refresh"
/* xiaotai_contacts_show(). args.value: the page; args.text: "all" or a kind's
 * name or alias ("" keeps the present kind; an unknown name is refused and
 * only counted). */
#define XIAOTAI_INTENT_CONTACTS_SHOW "contacts.show"
/* xiaotai_contacts_pending_show(args.value). */
#define XIAOTAI_INTENT_CONTACTS_PENDING "contacts.pending_show"
/* Friendship changes by position, for inputs that cannot carry an id:
 * args.value is the index on the selected pending page (accept, reject) or
 * on the selected contact page (remove; contacts of a kind with friendships
 * only). A stale index
 * is refused and reported with XIAOTAI_EVENT_CONTACTS_CHANGE_DONE
 * (XIAOTAI_ERR_NOT_FOUND). Requests and remarks have no intent: an id of full
 * length and a remark do not fit intent arguments. */
#define XIAOTAI_INTENT_CONTACTS_ACCEPT "contacts.accept"
#define XIAOTAI_INTENT_CONTACTS_REJECT "contacts.reject"
#define XIAOTAI_INTENT_CONTACTS_REMOVE "contacts.remove"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t fetches, fetch_failures;
    uint32_t fetches_deferred;     /* waited for the device to be idle or online */
    uint32_t replies_malformed;    /* not a contact list */
    uint32_t entries_skipped;
    uint32_t entries_unregistered; /* of which, of a type no kind was registered for */
    uint32_t entries_dropped;      /* accepted but beyond the cache */
    uint32_t signals;              /* "callers_update" and "contacts_update" handled */
    uint32_t stale_replies;        /* replies of an older binding generation dropped */
    uint32_t cleared;              /* cache emptied for a binding change */
    uint32_t queries, query_timeouts;
    uint32_t changes, change_failures;
    uint32_t pending_fetches, pending_failures;
} xiaotai_contacts_stats_t;

/* [any] */
void xiaotai_contacts_stats(xiaotai_contacts_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* Called by the framework through the generated tables. init: allocates the
 * cache (XIAOTAI_MEM_BULK), declares its platform needs, subscribes to the
 * platform's events and the two signals and to the binding module's events,
 * registers the intents, the commands AT+CONTACTS (status and page; REFRESH;
 * PAGE <ALL|kind name or alias> [<page>]; PENDING [<page>]) and AT+CONTACT
 * (REQUEST, ACCEPT, REJECT, DELETE <id>; REMARK <id> <hex|->) and the
 * diagnostics provider "contacts"; kinds are registered afterwards by the
 * call modules' init. start: checks the source against the registered kinds;
 * nothing is fetched before the device is online (and, with
 * XIAOTAI_CONTACTS_AFTER_PROFILE, its profile was accepted). */
xiaotai_err_t xiaotai_module_contacts_init(void);
void xiaotai_module_contacts_start(void);

/* Host tests: free the cache and forget configuration, kinds, view and
 * counters. */
void xiaotai_contacts_reset(void);

#ifdef __cplusplus
}
#endif
