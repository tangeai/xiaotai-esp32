// SPDX-License-Identifier: MIT
/* When the list is fetched, what happens to the replies, and online queries.
 *
 * One list request at a time. A fetch that is wanted while one is in flight
 * makes that one void (its reply may predate the change that asked for the
 * new one) and is sent once it ended. A wanted fetch waits while the platform
 * is offline (it then runs at the first-fetch moment: platform.online or the
 * accepted profile, as configured), while a foreground activity exists on a
 * product that fetches only when idle, and while the platform's request table
 * is full (until platform.idle). Replies of an earlier binding generation are
 * dropped and counted; a refused or failed fetch keeps the previous list.
 * Everything here runs on the owner task. */
#include <stdint.h>
#include <string.h>
#include "contacts_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"

static uint32_t next_serial(uint32_t serial) { return serial + 1U ? serial + 1U : 1U; }

static void note_deferred(void)
{
    if (g_contacts.deferred) return;
    g_contacts.deferred = true;
    contacts_lock();
    ++g_contacts.stats.fetches_deferred;
    contacts_unlock();
}

static void note_failure(xiaotai_err_t err)
{
    contacts_lock();
    ++g_contacts.stats.fetch_failures;
    g_contacts.status.error = err;
    contacts_unlock();
    xiaotai_eventlog_error(CONTACTS_TAG, "fetch.failed", 0, err);
}

static void note_stale(void)
{
    contacts_lock();
    ++g_contacts.stats.stale_replies;
    contacts_unlock();
    xiaotai_eventlog_warn(CONTACTS_TAG, "stale.dropped", 0, 0);
}

static bool activity_current(void)
{
    const char *name = NULL;
    return xiaotai_activity_current(&name) != 0;
}

static xiaotai_err_t parse_reply(const xiaotai_platform_reply_t *reply, contacts_reply_t *parsed)
{
    if (g_contacts.provisioning_boot) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    contacts_parse_in_t in = {
        .source = g_contacts.source,
        .token_capacity = g_contacts.token_capacity,
    };
    xiaotai_err_t err = contacts_parse_reply(&in, reply->http_status, reply->body, reply->size, parsed);
    if (err != XIAOTAI_OK) {
        contacts_lock();
        ++g_contacts.stats.replies_malformed;
        contacts_unlock();
    }
    return err;
}

static xiaotai_platform_request_t list_request(uint32_t timeout_ms, uint32_t flags)
{
    xiaotai_platform_request_t request = {
        .service = g_contacts.source->service,
        .method = XIAOTAI_PLATFORM_GET,
        .path = g_contacts.source->path,
        .body = NULL,
        .timeout_ms = timeout_ms,
        .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
        .bearer = NULL,
        .flags = flags,
    };
    return request;
}

/* ------------------------------------------------------------------- list */

static void on_list_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    uint32_t serial = (uint32_t)(uintptr_t)ctx;
    if (!g_contacts.initialized || !g_contacts.inflight || serial != g_contacts.serial) return;
    g_contacts.inflight = false;
    bool superseded = g_contacts.again;
    g_contacts.again = false;
    (void)contacts_check_generation();

    if (reply->generation != xiaotai_identity_generation()) {
        note_stale();
        g_contacts.want = true;
    } else if (reply->result == XIAOTAI_ERR_BUSY || reply->result == XIAOTAI_ERR_PLATFORM_STALE) {
        /* Not sent (an activity began while it was queued) or its session
         * ended: not a failure, the fetch waits again. */
        g_contacts.want = true;
        note_deferred();
    } else if (superseded) {
        g_contacts.want = true;
    } else if (reply->result != XIAOTAI_OK) {
        note_failure(reply->result);
    } else {
        contacts_reply_t parsed;
        xiaotai_err_t err = parse_reply(reply, &parsed);
        if (err == XIAOTAI_OK) {
            err = contacts_apply(&parsed, g_contacts.status.kind, g_contacts.status.page, reply->generation, true);
        }
        if (err != XIAOTAI_OK) note_failure(err);
    }
    contacts_changed();
    contacts_fetch_kick();
}

void contacts_fetch_want(void)
{
    g_contacts.want = true;
    if (g_contacts.inflight) g_contacts.again = true;
    contacts_lock();
    g_contacts.status.fresh = false;
    contacts_view_update_locked();
    contacts_unlock();
}

void contacts_fetch_kick(void)
{
    if (!g_contacts.started) return;
    if (!g_contacts.want || g_contacts.inflight) {
        contacts_pending_kick();
        return;
    }
    if (!g_contacts.online ||
        ((XIAOTAI_CONTACTS_FETCH_NEEDS_PROFILE || g_contacts.require_initial_confirmation) &&
         !g_contacts.profile_ok) ||
        (g_contacts.idle_only && activity_current())) {
        note_deferred();
        return;
    }
    /* Only fetches that nobody waits on can be held back by the platform. */
    xiaotai_platform_request_t request = list_request(0, g_contacts.idle_only ? XIAOTAI_PLATFORM_BACKGROUND : 0U);
    uint32_t serial = next_serial(g_contacts.serial);
    xiaotai_err_t err = xiaotai_platform_request(&request, on_list_reply, (void *)(uintptr_t)serial);
    switch (err) {
    case XIAOTAI_OK:
        g_contacts.serial = serial;
        g_contacts.inflight = true;
        g_contacts.want = false;
        g_contacts.again = false;
        g_contacts.deferred = false;
        contacts_lock();
        ++g_contacts.stats.fetches;
        contacts_unlock();
        break;
    case XIAOTAI_ERR_PLATFORM_OFFLINE: /* the session ended; platform.online follows */
    case XIAOTAI_ERR_BUSY:             /* an activity began; activity.changed follows */
    case XIAOTAI_ERR_NO_MEM:           /* every slot taken; platform.idle follows */
        note_deferred();
        break;
    default:
        g_contacts.want = false;
        g_contacts.deferred = false;
        note_failure(err);
        contacts_changed();
        break;
    }
    contacts_pending_kick();
}

xiaotai_err_t xiaotai_contacts_refresh(void)
{
    if (!g_contacts.initialized) return XIAOTAI_ERR_INVALID_STATE;
    (void)contacts_check_generation();
    contacts_fetch_want();
    contacts_changed();
    if (!g_contacts.online) {
        note_deferred();
        return XIAOTAI_ERR_PLATFORM_OFFLINE;
    }
    contacts_fetch_kick();
    return XIAOTAI_OK;
}

/* ----------------------------------------------------------------- queries */

typedef struct {
    const contacts_reply_t *reply;
    int kind;
} query_source_t;

static void finish_answer(const xiaotai_contacts_answer_t *answer)
{
    xiaotai_contacts_answer_fn_t callback = g_contacts.query_answer;
    void *ctx = g_contacts.query_ctx;
    g_contacts.query_pending = false;
    g_contacts.query_answer = NULL;
    g_contacts.query_ctx = NULL;
    xiaotai_timer_cancel(g_contacts.query_timer);
    g_contacts.query_timer = 0;
    if (callback) callback(answer, ctx);
}

static void finish_source(xiaotai_err_t result, const xiaotai_contact_t *contacts, size_t count, bool truncated,
                          const contacts_reply_t *source, size_t response_bytes)
{
    const query_source_t origin = {.reply = source, .kind = g_contacts.query_kind};
    const xiaotai_contacts_answer_t out = {
        .result = result,
        .contacts = count ? contacts : NULL,
        .count = count,
        .truncated = truncated,
        .response_bytes = response_bytes,
        .source = source ? &origin : NULL,
    };
    finish_answer(&out);
}

static void finish_query(xiaotai_err_t result)
{
    finish_answer(&(const xiaotai_contacts_answer_t){.result = result});
}

xiaotai_err_t xiaotai_contacts_answer_remark(const xiaotai_contacts_answer_t *answer, size_t index,
                                            char *out, size_t capacity, size_t *length)
{
    if (length) *length = 0;
    if (!xiaotai_on_owner() || !answer || index >= answer->count || !answer->source)
        return XIAOTAI_ERR_NOT_FOUND;
    const query_source_t *source = answer->source;
    size_t kind_count;
    const xiaotai_contact_kind_def_t *const *kinds = contacts_kinds(&kind_count);
    const contacts_parse_in_t input = {.source = g_contacts.source, .kinds = kinds, .kind_count = kind_count,
        .reject_invalid_known_rows = g_contacts.config.reject_invalid_known_rows};
    return contacts_query_remark(&input, source->reply, source->kind, index, out, capacity, length);
}

/* Answers with the cached entries of the query's kind. The query's selection
 * put them at the start of the cache, or the whole list is in memory. Read on
 * the owner task, which is the only writer, so no lock is taken. */
static void answer_from_cache(const contacts_reply_t *source)
{
    int kind = g_contacts.query_kind;
    const xiaotai_contacts_status_t *s = &g_contacts.status;
    uint16_t filtered = kind == XIAOTAI_CONTACT_ANY ? s->total : s->totals[kind];
    if (g_contacts.cache_kind == kind || kind == XIAOTAI_CONTACT_ANY) {
        finish_source(XIAOTAI_OK, g_contacts.cache, s->cached, filtered > s->cached, source, source->body_size);
        return;
    }
    /* The whole list is cached and the kind's entries are spread over it. */
    size_t count = 0;
    for (uint16_t i = 0; i < s->cached; ++i) count += g_contacts.cache[i].kind == kind;
    xiaotai_contact_t *list = count ? xiaotai_mem_alloc(XIAOTAI_MEM_BULK, count * sizeof(*list)) : NULL;
    if (count && !list) {
        finish_query(XIAOTAI_ERR_NO_MEM);
        return;
    }
    size_t used = 0;
    for (uint16_t i = 0; i < s->cached; ++i) {
        if (g_contacts.cache[i].kind == kind) list[used++] = g_contacts.cache[i];
    }
    finish_source(XIAOTAI_OK, list, used, filtered > used, source, source->body_size);
    xiaotai_mem_free(list);
}

static void on_query_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    uint32_t serial = (uint32_t)(uintptr_t)ctx;
    if (!g_contacts.initialized || !g_contacts.query_pending || serial != g_contacts.query_serial) return;
    (void)contacts_check_generation();
    if (reply->generation != xiaotai_identity_generation()) {
        note_stale();
        finish_query(XIAOTAI_ERR_PLATFORM_STALE);
    } else if (reply->result != XIAOTAI_OK) {
        finish_query(reply->result);
    } else {
        contacts_reply_t parsed;
        xiaotai_err_t err = parse_reply(reply, &parsed);
        if (err == XIAOTAI_OK) {
            /* The reply is a list as new as any: it refreshes the cache, and
             * stays fresh unless a fetch was asked for meanwhile. */
            bool fresh = !g_contacts.want && !g_contacts.inflight;
            err = contacts_apply(&parsed, g_contacts.query_kind, 0, reply->generation, fresh);
        }
        if (err == XIAOTAI_OK) answer_from_cache(&parsed);
        else finish_source(err, NULL, 0, false, NULL, reply->size);
    }
    contacts_changed();
    contacts_fetch_kick();
}

static void on_query_timeout(void *ctx)
{
    uint32_t serial = (uint32_t)(uintptr_t)ctx;
    if (!g_contacts.query_pending || serial != g_contacts.query_serial) return;
    g_contacts.query_timer = 0;
    contacts_lock();
    ++g_contacts.stats.query_timeouts;
    contacts_unlock();
    finish_answer(&(const xiaotai_contacts_answer_t){.result = XIAOTAI_ERR_TIMEOUT, .deadline_expired = true});
}

xiaotai_err_t xiaotai_contacts_query(int kind, uint32_t timeout_ms, xiaotai_contacts_answer_fn_t answer, void *ctx)
{
    if (!g_contacts.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (!answer || (kind != XIAOTAI_CONTACT_ANY && !contacts_kind_valid(kind))) return XIAOTAI_ERR_INVALID_ARG;
    if (g_contacts.query_pending) return XIAOTAI_ERR_BUSY;
    if (!g_contacts.online) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    /* A query is somebody waiting: never flagged as background, never held
     * back by an activity. */
    xiaotai_platform_request_t request = list_request(CONTACTS_QUERY_REQUEST_MS, 0U);
    uint32_t serial = next_serial(g_contacts.query_serial);
    xiaotai_err_t err = xiaotai_platform_request(&request, on_query_reply, (void *)(uintptr_t)serial);
    if (err != XIAOTAI_OK) return err;
    g_contacts.query_pending = true;
    g_contacts.query_serial = serial;
    g_contacts.query_kind = (int8_t)kind;
    g_contacts.query_answer = answer;
    g_contacts.query_ctx = ctx;
    uint32_t delay = timeout_ms ? timeout_ms : CONTACTS_QUERY_TIMEOUT_MS;
    if (delay > XIAOTAI_TIMER_DELAY_MAX_MS) delay = XIAOTAI_TIMER_DELAY_MAX_MS;
    /* Without a timer slot the request's own time still ends the query. */
    if (xiaotai_timer_start(delay, on_query_timeout, (void *)(uintptr_t)serial, &g_contacts.query_timer) !=
        XIAOTAI_OK) {
        g_contacts.query_timer = 0;
    }
    contacts_lock();
    ++g_contacts.stats.queries;
    contacts_unlock();
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_query_cancel(xiaotai_contacts_answer_fn_t answer, void *ctx)
{
    if (!xiaotai_on_owner() || !g_contacts.query_pending || !answer ||
        g_contacts.query_answer != answer || g_contacts.query_ctx != ctx) return XIAOTAI_ERR_INVALID_STATE;
    finish_query(XIAOTAI_ERR_INVALID_STATE);
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------------ events */

static void on_signal(const xiaotai_platform_signal_t *signal, void *ctx)
{
    (void)signal;
    (void)ctx;
    contacts_lock();
    ++g_contacts.stats.signals;
    contacts_unlock();
    (void)contacts_check_generation();
    contacts_pending_invalidate(true, false);
    contacts_fetch_want();
    contacts_changed();
    contacts_fetch_kick();
}

static void on_online(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    g_contacts.online = true;
    g_contacts.profile_ok = false;
    (void)contacts_check_generation();
    if (g_contacts.require_initial_confirmation) {
        g_contacts.confirmation_generation = 0;
        contacts_fetch_want();
        contacts_pending_invalidate(true, false);
    }
    if (g_contacts.config.first_fetch == XIAOTAI_CONTACTS_ON_ONLINE) {
        contacts_fetch_want();
        contacts_changed();
        contacts_fetch_kick();
    }
}

static void on_offline(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    g_contacts.online = false;
    g_contacts.profile_ok = false;
    g_contacts.confirmation_generation = 0;
    (void)contacts_check_generation();
}

static void on_profile(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->args.value != 1) return;
    g_contacts.profile_ok = true;
    (void)contacts_check_generation();
    if (g_contacts.config.first_fetch == XIAOTAI_CONTACTS_AFTER_PROFILE) {
        contacts_fetch_want();
        contacts_changed();
    }
    contacts_fetch_kick();
}

static void on_activity(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->args.text[0]) return;
    contacts_fetch_kick();
}

static void on_platform_idle(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    contacts_fetch_kick();
}

static void on_binding(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    (void)contacts_check_generation();
}

xiaotai_err_t contacts_fetch_init(void)
{
    const xiaotai_contacts_source_t *source = g_contacts.source;
    xiaotai_platform_requirement_t need = {
        .request_body_bytes = source->friendships ? CONTACTS_REQUEST_BODY_BYTES : 0U,
        .response_bytes = g_contacts.config.response_bytes,
        .signal_bytes = 0,
        .concurrent_requests = source->friendships ? 2U : 1U,
    };
    xiaotai_err_t err = xiaotai_platform_require(&need);
    if (err == XIAOTAI_OK) err = xiaotai_platform_require_service(source->service);
    if (err == XIAOTAI_OK) err = xiaotai_platform_signal_subscribe("callers_update", NULL, on_signal, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_platform_signal_subscribe("contacts_update", NULL, on_signal, NULL);
    static const xiaotai_subscriber_descriptor_t EVENTS[] = {
        {XIAOTAI_EVENT_PLATFORM_ONLINE, on_online, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, on_offline, NULL},
        {XIAOTAI_EVENT_PLATFORM_PROFILE, on_profile, NULL},
        {XIAOTAI_EVENT_PLATFORM_IDLE, on_platform_idle, NULL},
        {XIAOTAI_EVENT_BINDING_NEEDED, on_binding, NULL},
        {XIAOTAI_EVENT_BINDING_BOUND, on_binding, NULL},
        {XIAOTAI_EVENT_ACTIVITY, on_activity, NULL},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(EVENTS) / sizeof(EVENTS[0]); ++i) {
        err = xiaotai_event_subscribe_static(&EVENTS[i]);
    }
    return err;
}
