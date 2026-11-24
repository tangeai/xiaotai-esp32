// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>

#include "contacts_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/platform.h"

static uint32_t next_serial(uint32_t serial) { return serial == UINT32_MAX ? 1U : serial + 1U; }

static bool has_friendships(void) { return g_contacts.source && g_contacts.source->friendships; }

static xiaotai_err_t read_envelope(const xiaotai_platform_reply_t *reply, contacts_reply_t *parsed)
{
    parsed->code = XIAOTAI_PLATFORM_NO_CODE;
    if (reply->result != XIAOTAI_OK) return reply->result;
    if (g_contacts.provisioning_boot) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    return contacts_parse_envelope(g_contacts.token_capacity, reply->http_status,
        reply->body, reply->size, parsed);
}

static xiaotai_err_t envelope(const xiaotai_platform_reply_t *reply, contacts_reply_t *parsed)
{
    const xiaotai_err_t err = read_envelope(reply, parsed);
    if (err != XIAOTAI_OK) return err;
    return parsed->code == g_contacts.source->success_code ? XIAOTAI_OK : XIAOTAI_FAIL;
}

static void stale(void)
{
    contacts_lock();
    ++g_contacts.stats.stale_replies;
    contacts_unlock();
    xiaotai_eventlog_warn(CONTACTS_TAG, "stale.dropped", 0, 0);
}

static void pending_changed(void)
{
    contacts_lock();
    ++g_contacts.pending.revision;
    g_contacts.pending.loading = g_contacts.pending_wanted || g_contacts.pending_inflight;
    const xiaotai_contacts_pending_status_t status = g_contacts.pending;
    contacts_unlock();
    const xiaotai_args_t args = {.value = status.total};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_CONTACTS_PENDING, &args, &status, sizeof(status));
}

static void clear_page_locked(void)
{
    g_contacts.pending.fresh = false;
    g_contacts.pending.page_count = 0;
    g_contacts.pending.total = 0;
    g_contacts.pending.pages = 1;
    memset(g_contacts.pending_ids, 0, g_contacts.pending.page_size * sizeof(*g_contacts.pending_ids));
}

void contacts_pending_invalidate(bool refresh, bool clear_selection)
{
    if (!has_friendships() || !g_contacts.pending_ids) return;
    g_contacts.pending_wanted = refresh;
    g_contacts.pending_again = g_contacts.pending_inflight;
    const uint32_t gen = xiaotai_identity_generation();
    contacts_lock();
    clear_page_locked();
    g_contacts.pending.error = XIAOTAI_OK;
    g_contacts.pending.code = 0;
    if (clear_selection) g_contacts.pending.page = 0;
    g_contacts.pending_generation = gen;
    contacts_unlock();
    pending_changed();
}

bool contacts_pending_check_generation(void)
{
    if (!has_friendships() || !g_contacts.pending_generation ||
        g_contacts.pending_generation == xiaotai_identity_generation()) return false;
    contacts_pending_invalidate(false, true);
    return true;
}

static bool pending_kind(const xiaotai_json_view_t *entry)
{
    xiaotai_json_view_t type;
    return xiaotai_json_view_get(entry, "type", &type) && xiaotai_json_view_equals(&type, "device");
}

static xiaotai_err_t pending_id(const xiaotai_json_view_t *entry, char *out, size_t capacity)
{
    xiaotai_json_view_t id;
    if (!xiaotai_json_view_get(entry, "peer_device_id", &id)) return XIAOTAI_ERR_NOT_FOUND;
    return xiaotai_json_view_string(&id, out, capacity, NULL);
}

static xiaotai_err_t pending_parse(const contacts_reply_t *parsed, uint32_t generation)
{
    xiaotai_json_view_t array, entry;
    if (!contacts_reply_array(parsed, "pending", &array)) return XIAOTAI_ERR_INVALID_RESPONSE;
    xiaotai_json_view_iter_t iter;
    const uint8_t page_size = g_contacts.pending.page_size;
    uint16_t total = 0;
    char one[XIAOTAI_CONTACT_ID_MAX + 1U];
    xiaotai_json_view_iter(&array, &iter);
    while (xiaotai_json_view_next(&iter, NULL, &entry)) {
        if (!pending_kind(&entry)) continue;
        if (pending_id(&entry, one, sizeof(one)) != XIAOTAI_OK || !*one || total == UINT16_MAX)
            return XIAOTAI_ERR_INVALID_RESPONSE;
        ++total;
    }

    const uint16_t pages = total ? (uint16_t)((total - 1U) / page_size + 1U) : 1U;
    const uint16_t page = g_contacts.pending.page >= pages ? pages - 1U : g_contacts.pending.page;
    const uint32_t offset = (uint32_t)page * page_size;
    uint32_t seen = 0;
    uint8_t count = 0;
    char selected[CONTACTS_PENDING_PAGE_SIZE_MAX][XIAOTAI_CONTACT_ID_MAX + 1U] = {{0}};
    xiaotai_json_view_iter(&array, &iter);
    while (xiaotai_json_view_next(&iter, NULL, &entry)) {
        if (!pending_kind(&entry) || seen++ < offset) continue;
        (void)pending_id(&entry, selected[count], sizeof(one));
        if (++count == page_size) break;
    }
    contacts_lock();
    memcpy(g_contacts.pending_ids, selected, (size_t)page_size * sizeof(one));
    g_contacts.pending_generation = generation;
    g_contacts.pending.fresh = true;
    g_contacts.pending.error = XIAOTAI_OK;
    g_contacts.pending.code = 0;
    g_contacts.pending.total = total;
    g_contacts.pending.page = page;
    g_contacts.pending.pages = pages;
    g_contacts.pending.page_count = count;
    contacts_unlock();
    return XIAOTAI_OK;
}

static xiaotai_err_t pending_result(const xiaotai_platform_reply_t *reply, int32_t *code)
{
    contacts_reply_t parsed;
    const xiaotai_err_t err = read_envelope(reply, &parsed);
    if (err != XIAOTAI_OK)
        return err == XIAOTAI_FAIL && reply->result == XIAOTAI_OK ? XIAOTAI_ERR_INVALID_RESPONSE : err;
    if (parsed.code == g_contacts.source->success_code) return pending_parse(&parsed, reply->generation);
    if (parsed.code <= 0) return XIAOTAI_ERR_INVALID_RESPONSE;
    *code = parsed.code;
    return XIAOTAI_FAIL;
}

static void pending_failure(xiaotai_err_t err, int32_t code)
{
    contacts_lock();
    clear_page_locked();
    g_contacts.pending.error = err;
    g_contacts.pending.code = code;
    ++g_contacts.stats.pending_failures;
    contacts_unlock();
    xiaotai_eventlog_error(CONTACTS_TAG, "pending.failed", 0, code ? (int)code : err);
}

static void on_pending_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    if (!g_contacts.initialized || !g_contacts.pending_inflight ||
        g_contacts.pending_serial != (uint32_t)(uintptr_t)ctx) return;
    const bool superseded = g_contacts.pending_again;
    g_contacts.pending_inflight = g_contacts.pending_again = false;
    (void)contacts_check_generation();
    if (reply->generation != xiaotai_identity_generation() || reply->result == XIAOTAI_ERR_PLATFORM_STALE) {
        stale();
        if (reply->generation == xiaotai_identity_generation()) {
            g_contacts.pending_wanted = true;
            /* The stale session cannot take a retry until its next online event. */
            pending_changed();
            return;
        }
    } else if (reply->result == XIAOTAI_ERR_BUSY) {
        g_contacts.pending_wanted = true;
    } else if (!superseded) {
        int32_t code = 0;
        const xiaotai_err_t err = pending_result(reply, &code);
        if (err != XIAOTAI_OK) pending_failure(err, code);
        if (g_contacts.require_initial_confirmation)
            g_contacts.confirmation_generation = reply->generation;
    }
    pending_changed();
    contacts_fetch_kick();
}

static bool make_path(const char *suffix, char *path, size_t capacity)
{
    const int n = snprintf(path, capacity, "%s%s", g_contacts.source->path, suffix);
    return n >= 0 && (size_t)n < capacity;
}

xiaotai_err_t xiaotai_contacts_require_initial_confirmation(void)
{
    if (!g_contacts.initialized || g_contacts.started) return XIAOTAI_ERR_INVALID_STATE;
    if (!has_friendships()) return XIAOTAI_ERR_NOT_SUPPORTED;
    g_contacts.require_initial_confirmation = true;
    return XIAOTAI_OK;
}

bool xiaotai_contacts_initial_confirmation_done(void)
{
    return g_contacts.online && g_contacts.profile_ok && g_contacts.confirmation_generation != 0 &&
           g_contacts.confirmation_generation == xiaotai_identity_generation();
}

void contacts_pending_kick(void)
{
    if (!g_contacts.started || !g_contacts.pending_wanted || g_contacts.pending_inflight || g_contacts.want ||
        g_contacts.inflight ||
        !g_contacts.online ||
        ((XIAOTAI_CONTACTS_FETCH_NEEDS_PROFILE || g_contacts.require_initial_confirmation) &&
         !g_contacts.profile_ok) ||
        (g_contacts.idle_only && xiaotai_activity_current(NULL))) return;
    char path[XIAOTAI_PLATFORM_PATH_MAX];
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_SIZE;
    if (make_path("/pending", path, sizeof(path))) {
        const xiaotai_platform_request_t request = {.service = g_contacts.source->service,
            .method = XIAOTAI_PLATFORM_GET, .path = path, .auth = XIAOTAI_PLATFORM_AUTH_SESSION,
            .flags = g_contacts.idle_only ? XIAOTAI_PLATFORM_BACKGROUND : 0};
        const uint32_t serial = next_serial(g_contacts.pending_serial);
        err = xiaotai_platform_request(&request, on_pending_reply, (void *)(uintptr_t)serial);
        if (err == XIAOTAI_OK) {
            g_contacts.pending_serial = serial;
            g_contacts.pending_inflight = true;
            g_contacts.pending_again = false;
            g_contacts.pending_wanted = false;
            contacts_lock();
            ++g_contacts.stats.pending_fetches;
            contacts_unlock();
        }
    }
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_BUSY && err != XIAOTAI_ERR_PLATFORM_OFFLINE && err != XIAOTAI_ERR_NO_MEM) {
        g_contacts.pending_wanted = false;
        pending_failure(err, 0);
    }
    pending_changed();
}

xiaotai_err_t xiaotai_contacts_pending_show(uint16_t page)
{
    if (!g_contacts.initialized || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (!has_friendships()) return XIAOTAI_ERR_NOT_SUPPORTED;
    (void)contacts_check_generation();
    contacts_lock();
    g_contacts.pending.page = page;
    contacts_unlock();
    contacts_pending_invalidate(true, false);
    contacts_fetch_kick();
    return XIAOTAI_OK;
}

void xiaotai_contacts_pending_status(xiaotai_contacts_pending_status_t *status)
{
    if (!status) return;
    const uint32_t generation = xiaotai_identity_generation();
    contacts_lock();
    *status = g_contacts.pending;
    if (!status->pages) status->pages = 1;
    if (g_contacts.pending_generation != generation) {
        status->fresh = status->loading = false;
        status->page = status->total = status->page_count = 0;
        status->pages = 1;
        status->error = XIAOTAI_OK;
        status->code = 0;
    }
    contacts_unlock();
}

size_t xiaotai_contacts_pending_page(char (*ids)[XIAOTAI_CONTACT_ID_MAX + 1U], size_t capacity)
{
    if (!ids || !capacity) return 0;
    const uint32_t generation = xiaotai_identity_generation();
    contacts_lock();
    size_t count = g_contacts.pending_generation == generation ? g_contacts.pending.page_count : 0;
    if (count > capacity) count = capacity;
    if (count) memcpy(ids, g_contacts.pending_ids, count * sizeof(*ids));
    contacts_unlock();
    return count;
}

static bool valid_id(const char *id)
{
    return id && *id && strnlen(id, XIAOTAI_CONTACT_ID_MAX + 1U) <= XIAOTAI_CONTACT_ID_MAX;
}

static bool responding(xiaotai_contacts_change_kind_t kind)
{
    return kind == XIAOTAI_CONTACTS_ACCEPT || kind == XIAOTAI_CONTACTS_REJECT;
}

static void change_done(const xiaotai_contacts_change_result_t *result)
{
    const xiaotai_args_t args = {.value = result->result};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_CONTACTS_CHANGE_DONE, &args, result, sizeof(*result));
}

static void on_change_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    if (!g_contacts.initialized || !g_contacts.change_inflight ||
        g_contacts.change_serial != (uint32_t)(uintptr_t)ctx) return;
    g_contacts.change_inflight = false;
    xiaotai_contacts_change_result_t result = g_contacts.change;
    memset(&g_contacts.change, 0, sizeof(g_contacts.change));
    if (reply->generation != g_contacts.change_generation || reply->generation != xiaotai_identity_generation() ||
        reply->result == XIAOTAI_ERR_PLATFORM_STALE) {
        stale();
        /* Retire the operation, without disclosing the old binding's peer. */
        memset(result.id, 0, sizeof(result.id));
        result.result = XIAOTAI_ERR_PLATFORM_STALE;
    } else {
        contacts_reply_t parsed;
        result.result = envelope(reply, &parsed);
        result.code = parsed.code;
    }
    if (result.result == XIAOTAI_OK) {
        /* A mutation acknowledgement does not supply a replacement list. */
        if (responding(result.kind)) contacts_pending_invalidate(true, false);
        (void)xiaotai_contacts_refresh();
    } else {
        contacts_lock();
        ++g_contacts.stats.change_failures;
        contacts_unlock();
        xiaotai_eventlog_error(CONTACTS_TAG, "change.failed", 0, result.result);
    }
    change_done(&result);
}

xiaotai_err_t xiaotai_contacts_change(const xiaotai_contacts_change_t *change)
{
    if (!g_contacts.initialized || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (!has_friendships()) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!change || !valid_id(change->id) || change->kind < XIAOTAI_CONTACTS_REQUEST ||
        change->kind > XIAOTAI_CONTACTS_REMOVE) return XIAOTAI_ERR_INVALID_ARG;
    if (change->kind == XIAOTAI_CONTACTS_REMARK) {
        const xiaotai_err_t err = contacts_validate_remark(change->remark);
        if (err != XIAOTAI_OK) return err;
    }
    if (g_contacts.change_inflight) return XIAOTAI_ERR_BUSY;
    if (!g_contacts.online) return XIAOTAI_ERR_PLATFORM_OFFLINE;
    (void)contacts_check_generation();
    if (responding(change->kind)) {
        bool pending = false;
        contacts_lock();
        for (uint8_t i = 0; i < g_contacts.pending.page_count && !pending; ++i)
            pending = !strcmp(change->id, g_contacts.pending_ids[i]);
        contacts_unlock();
        if (!pending) return XIAOTAI_ERR_NOT_FOUND;
    }

    static const struct { const char *suffix, *id; xiaotai_platform_method_t method; } routes[] = {
        [XIAOTAI_CONTACTS_REQUEST] = {"/request", "target_device_id", XIAOTAI_PLATFORM_POST},
        [XIAOTAI_CONTACTS_ACCEPT] = {"/respond", "peer_device_id", XIAOTAI_PLATFORM_POST},
        [XIAOTAI_CONTACTS_REJECT] = {"/respond", "peer_device_id", XIAOTAI_PLATFORM_POST},
        [XIAOTAI_CONTACTS_REMARK] = {"/remark", "peer_id", XIAOTAI_PLATFORM_PUT},
        [XIAOTAI_CONTACTS_REMOVE] = {"", "peer_id", XIAOTAI_PLATFORM_DELETE},
    };
    char path[XIAOTAI_PLATFORM_PATH_MAX];
    char body[CONTACTS_REQUEST_BODY_BYTES + 1U];
    const unsigned kind = (unsigned)change->kind;
    if (!make_path(routes[kind].suffix, path, sizeof(path))) return XIAOTAI_ERR_INVALID_SIZE;
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, body, sizeof(body));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, routes[kind].id, change->id);
    if (responding(change->kind))
        xiaotai_json_put_string(&writer, "action", kind == XIAOTAI_CONTACTS_ACCEPT ? "accept" : "reject");
    if (kind == XIAOTAI_CONTACTS_REMARK) xiaotai_json_put_string(&writer, "remark", change->remark ? change->remark : "");
    xiaotai_json_object_end(&writer);
    const xiaotai_err_t written = xiaotai_json_writer_end(&writer, NULL);
    if (written != XIAOTAI_OK) return written;
    const xiaotai_platform_request_t request = {.service = g_contacts.source->service, .method = routes[kind].method,
        .path = path, .body = body, .auth = XIAOTAI_PLATFORM_AUTH_SESSION};
    const uint32_t serial = next_serial(g_contacts.change_serial);
    const uint32_t generation = xiaotai_identity_generation();
    const xiaotai_err_t err = xiaotai_platform_request(&request, on_change_reply, (void *)(uintptr_t)serial);
    if (err == XIAOTAI_OK) {
        g_contacts.change_serial = serial;
        g_contacts.change_generation = generation;
        g_contacts.change_inflight = true;
        g_contacts.change = (xiaotai_contacts_change_result_t){.kind = change->kind, .code = XIAOTAI_PLATFORM_NO_CODE};
        memcpy(g_contacts.change.id, change->id, strlen(change->id) + 1U);
        contacts_lock();
        ++g_contacts.stats.changes;
        contacts_unlock();
    }
    return err;
}

static void intent_pending(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (args->value >= 0 && args->value <= UINT16_MAX) (void)xiaotai_contacts_pending_show((uint16_t)args->value);
}

static void intent_change(const xiaotai_args_t *args, void *ctx)
{
    const xiaotai_contacts_change_kind_t kind = (xiaotai_contacts_change_kind_t)(uintptr_t)ctx;
    const int32_t index = args->value;
    const uint32_t generation = xiaotai_identity_generation();
    xiaotai_contacts_change_result_t refused = {.kind = kind, .result = XIAOTAI_ERR_NOT_FOUND, .code = XIAOTAI_PLATFORM_NO_CODE};
    if (index >= 0 && index <= UINT8_MAX && has_friendships()) {
        if (responding(kind)) {
            contacts_lock();
            if (g_contacts.pending_generation == generation && index < g_contacts.pending.page_count)
                memcpy(refused.id, g_contacts.pending_ids[index], sizeof(refused.id));
            contacts_unlock();
        } else {
            xiaotai_contact_t contact;
            if (xiaotai_contacts_at((uint8_t)index, 0, &contact) == XIAOTAI_OK) {
                const xiaotai_contact_kind_def_t *def = xiaotai_contacts_kind(contact.kind);
                if (def && def->friendships) memcpy(refused.id, contact.id, sizeof(refused.id));
            }
        }
    }
    if (*refused.id) {
        refused.result = xiaotai_contacts_change(&(xiaotai_contacts_change_t){.kind = kind, .id = refused.id});
        if (refused.result == XIAOTAI_OK) return;
    }
    change_done(&refused);
}

xiaotai_err_t contacts_change_init(void)
{
    g_contacts.pending.pages = 1;
    g_contacts.pending.page_size = g_contacts.config.pending_page_size;
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_CONTACTS_PENDING, intent_pending, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_CONTACTS_ACCEPT, intent_change, (void *)(uintptr_t)XIAOTAI_CONTACTS_ACCEPT);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_CONTACTS_REJECT, intent_change, (void *)(uintptr_t)XIAOTAI_CONTACTS_REJECT);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_CONTACTS_REMOVE, intent_change, (void *)(uintptr_t)XIAOTAI_CONTACTS_REMOVE);
    return err;
}
