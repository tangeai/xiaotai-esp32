// SPDX-License-Identifier: MIT
/* Contacts module: state, configuration, the kind registry, the view and the
 * lookups, events, intents and the lifecycle. Fetching lives in
 * contacts_fetch.c, the reply reader in contacts_parse.c, name matching in
 * contacts_match.c.
 *
 * The cache belongs to one binding generation (status.generation): it is
 * emptied when the generation in force differs, and lookups compare the two
 * themselves, so a list of an earlier binding is never handed out even before
 * the owner task noticed the change. */
#include <ctype.h>
#include <string.h>
#include "contacts_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/net.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

contacts_state_t g_contacts;

/* The kinds, by number. Written only before start, by the call modules'
 * init (owner task); read from any task afterwards. */
static const xiaotai_contact_kind_def_t *s_kinds[XIAOTAI_CONTACT_KINDS_MAX];
static size_t s_kind_count;

/* Product configuration as given (copied by xiaotai_contacts_configure). */
static xiaotai_contacts_config_t s_given;
static bool s_given_set;

/* The call service's list: every kind together, friendships served next to it. */
static const xiaotai_contacts_source_t DEFAULT_SOURCE = {
    .service = XIAOTAI_PLATFORM_SERVICE_CALL,
    .path = "/v1/call/device/contacts",
    .success_code = XIAOTAI_PLATFORM_CODE_OK,
    .id_field = "device_id",
    .type_field = "type",
    .default_type = NULL,
    .friendships = true,
};

/* -------------------------------------------------------------------- lock */

void contacts_lock(void)
{
    if (g_contacts.lock_ready) (void)xiaotai_mutex_lock(&g_contacts.lock, XIAOTAI_WAIT_FOREVER);
}

void contacts_unlock(void)
{
    if (g_contacts.lock_ready) xiaotai_mutex_unlock(&g_contacts.lock);
}

/* ------------------------------------------------------------------- kinds */

static bool word_ok(const char *text, bool lower_only)
{
    size_t length = text ? strlen(text) : 0;
    if (length == 0 || length > 15U) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                  (!lower_only && c >= 'A' && c <= 'Z');
        if (!ok) return false;
    }
    return true;
}

static bool same_word(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

/* True when `word` is "all" or a name or alias already registered. */
static bool word_taken(const char *word)
{
    if (same_word(word, "all")) return true;
    for (size_t k = 0; k < s_kind_count; ++k) {
        if (same_word(word, s_kinds[k]->name) || same_word(word, s_kinds[k]->alias)) return true;
    }
    return false;
}

xiaotai_err_t xiaotai_contacts_kind_register(const xiaotai_contact_kind_def_t *def, int *kind)
{
    if (!def || !word_ok(def->name, true) || word_taken(def->name)) return XIAOTAI_ERR_INVALID_ARG;
    if (def->alias && (!word_ok(def->alias, false) || word_taken(def->alias) || same_word(def->alias, def->name))) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!def->server_type || !def->server_type[0]) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t k = 0; k < s_kind_count; ++k) {
        if (strcmp(s_kinds[k]->server_type, def->server_type) == 0) return XIAOTAI_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < 8U; ++i) {
        bool used = i < XIAOTAI_CONTACT_EXTRAS && def->extra_fields[i];
        if ((def->extra_required & (1U << i)) && !used) return XIAOTAI_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < XIAOTAI_CONTACT_EXTRAS; ++i) {
        if (def->extra_fields[i] && !def->extra_fields[i][0]) return XIAOTAI_ERR_INVALID_ARG;
    }
    if (g_contacts.started) return XIAOTAI_ERR_INVALID_STATE;
    if (s_kind_count >= XIAOTAI_CONTACT_KINDS_MAX) return XIAOTAI_ERR_NO_MEM;
    s_kinds[s_kind_count] = def;
    if (kind) *kind = (int)s_kind_count;
    ++s_kind_count;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_kind_find(const char *name, int *kind)
{
    if (!name || !kind) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t k = 0; k < s_kind_count; ++k) {
        if (same_word(name, s_kinds[k]->name) || same_word(name, s_kinds[k]->alias)) {
            *kind = (int)k;
            return XIAOTAI_OK;
        }
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

const xiaotai_contact_kind_def_t *xiaotai_contacts_kind(int kind)
{
    return contacts_kind_valid(kind) ? s_kinds[kind] : NULL;
}

size_t xiaotai_contacts_kind_count(void) { return s_kind_count; }

const xiaotai_contact_kind_def_t *const *contacts_kinds(size_t *count)
{
    *count = s_kind_count;
    return s_kinds;
}

bool contacts_kind_valid(int kind) { return kind >= 0 && (size_t)kind < s_kind_count; }

/* -------------------------------------------------------------------- view */

static uint16_t filtered_total_locked(int kind)
{
    return kind == XIAOTAI_CONTACT_ANY ? g_contacts.status.total : g_contacts.status.totals[kind];
}

/* The cache holds every entry of `kind`. */
static bool complete_locked(int kind)
{
    const xiaotai_contacts_status_t *s = &g_contacts.status;
    if (s->generation == 0 || g_contacts.cache_offset != 0) return false;
    if (g_contacts.cache_kind == XIAOTAI_CONTACT_ANY) return s->cached == s->total;
    return g_contacts.cache_kind == kind && s->cached == filtered_total_locked(kind);
}

const xiaotai_contact_t *contacts_entry_locked(int kind, uint16_t index)
{
    const xiaotai_contacts_status_t *s = &g_contacts.status;
    if (s->generation == 0) return NULL;
    if (g_contacts.cache_kind == kind) {
        if (index < g_contacts.cache_offset || index - g_contacts.cache_offset >= s->cached) return NULL;
        return &g_contacts.cache[index - g_contacts.cache_offset];
    }
    if (g_contacts.cache_kind != XIAOTAI_CONTACT_ANY || !complete_locked(XIAOTAI_CONTACT_ANY)) return NULL;
    uint16_t seen = 0;
    for (uint16_t i = 0; i < s->cached; ++i) {
        if (g_contacts.cache[i].kind != kind) continue;
        if (seen++ == index) return &g_contacts.cache[i];
    }
    return NULL;
}

/* First position and count of the selected page. */
static void page_bounds_locked(uint16_t *start, uint16_t *count)
{
    const xiaotai_contacts_status_t *s = &g_contacts.status;
    uint16_t filtered = filtered_total_locked(s->kind);
    uint32_t first = (uint32_t)s->page * s->page_size;
    *start = (uint16_t)(first < filtered ? first : filtered);
    uint32_t left = filtered - *start;
    *count = (uint16_t)(left < s->page_size ? left : s->page_size);
}

bool contacts_view_served_locked(void)
{
    if (g_contacts.status.generation == 0) return false;
    uint16_t start, count;
    page_bounds_locked(&start, &count);
    if (count == 0) return true;
    return contacts_entry_locked(g_contacts.status.kind, start) &&
           contacts_entry_locked(g_contacts.status.kind, (uint16_t)(start + count - 1U));
}

void contacts_view_update_locked(void)
{
    xiaotai_contacts_status_t *s = &g_contacts.status;
    uint16_t filtered = filtered_total_locked(s->kind);
    s->pages = filtered ? (uint16_t)((filtered + s->page_size - 1U) / s->page_size) : 1U;
    if (s->generation != 0 && s->page >= s->pages) s->page = (uint16_t)(s->pages - 1U);
    uint16_t start, count;
    page_bounds_locked(&start, &count);
    s->page_count = contacts_view_served_locked() ? (uint8_t)count : 0;
    s->truncated = s->generation != 0 && !complete_locked(s->kind);
    s->loading = g_contacts.want || g_contacts.inflight;
}

xiaotai_err_t contacts_usable_locked(uint32_t flags)
{
    if (flags & ~(uint32_t)XIAOTAI_CONTACTS_STALE_OK) return XIAOTAI_ERR_INVALID_ARG;
    uint32_t generation = g_contacts.status.generation;
    if (generation == 0 || generation != xiaotai_identity_generation()) return XIAOTAI_ERR_INVALID_STATE;
    if (!(flags & XIAOTAI_CONTACTS_STALE_OK) && !g_contacts.status.fresh) return XIAOTAI_ERR_INVALID_STATE;
    return XIAOTAI_OK;
}

void contacts_changed(void)
{
    xiaotai_contacts_status_t snapshot;
    contacts_lock();
    contacts_view_update_locked();
    ++g_contacts.status.revision;
    snapshot = g_contacts.status;
    contacts_unlock();
    xiaotai_args_t args = {.value = (int32_t)(snapshot.revision & 0x7FFFFFFFU)};
    (void)xiaotai_event_emit(XIAOTAI_EVENT_CONTACTS_CHANGED, &args, &snapshot, sizeof(snapshot));
}

bool contacts_check_generation(void)
{
    uint32_t generation = xiaotai_identity_generation();
    contacts_lock();
    uint32_t cached = g_contacts.status.generation;
    bool clear = cached != 0 && cached != generation;
    if (clear) {
        xiaotai_contacts_status_t *s = &g_contacts.status;
        s->generation = 0;
        s->fresh = false;
        s->error = XIAOTAI_OK;
        s->total = 0;
        memset(s->totals, 0, sizeof(s->totals));
        s->skipped = 0;
        s->cached = 0;
        s->page = 0;
        memset(g_contacts.first, 0, sizeof(g_contacts.first));
        g_contacts.cache_kind = XIAOTAI_CONTACT_ANY;
        g_contacts.cache_offset = 0;
        g_contacts.dial_prefix_count = 0;
        ++g_contacts.stats.cleared;
    }
    contacts_unlock();
    (void)contacts_pending_check_generation();
    if (clear) {
        xiaotai_eventlog_note(CONTACTS_TAG, "cleared", 0, 0);
        contacts_changed();
    }
    return clear;
}

xiaotai_err_t contacts_apply(const contacts_reply_t *reply, int kind, uint16_t page, uint32_t generation, bool fresh)
{
    size_t kind_count = 0;
    const xiaotai_contact_kind_def_t *const *kinds = contacts_kinds(&kind_count);
    contacts_parse_in_t in = {
        .source = g_contacts.source,
        .kinds = kinds,
        .kind_count = kind_count,
        .kind = kind,
        .page = page,
        .page_size = g_contacts.config.page_size,
        .capacity = g_contacts.config.capacity,
        .cache = g_contacts.cache,
        .firsts = g_contacts.firsts,
        .dial_prefix = g_contacts.cache + g_contacts.config.capacity,
        .dial_prefix_capacity = g_contacts.config.at_device_dial_prefix,
        .reject_invalid_known_rows = g_contacts.config.reject_invalid_known_rows,
    };
    contacts_parse_out_t out;
    contacts_lock();
    const xiaotai_err_t err = contacts_parse_fill(&in, reply, &out);
    if (err != XIAOTAI_OK) {
        ++g_contacts.stats.replies_malformed;
        contacts_unlock();
        return err;
    }
    xiaotai_contacts_status_t *s = &g_contacts.status;
    s->generation = generation;
    s->fresh = fresh;
    s->error = XIAOTAI_OK;
    s->total = out.total;
    memcpy(s->totals, out.totals, sizeof(s->totals));
    s->skipped = out.skipped;
    s->cached = out.cached;
    memcpy(g_contacts.first, out.first, sizeof(g_contacts.first));
    g_contacts.cache_kind = out.cache_kind;
    g_contacts.cache_offset = out.cache_offset;
    g_contacts.dial_prefix_count = out.dial_prefix_count;
    xiaotai_contacts_stats_t *st = &g_contacts.stats;
    st->entries_skipped += out.skipped;
    st->entries_unregistered += out.unregistered;
    st->entries_dropped += out.dropped;
    contacts_view_update_locked();
    bool served = contacts_view_served_locked();
    contacts_unlock();
    /* A query's selection can leave the selected page outside the cache. */
    if (!served) contacts_fetch_want();
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------ status, list */

void xiaotai_contacts_status(xiaotai_contacts_status_t *status)
{
    if (!status) return;
    contacts_lock();
    *status = g_contacts.status;
    contacts_unlock();
}

void xiaotai_contacts_stats(xiaotai_contacts_stats_t *stats)
{
    if (!stats) return;
    contacts_lock();
    *stats = g_contacts.stats;
    contacts_unlock();
}

xiaotai_err_t xiaotai_contacts_show(int kind, uint16_t page)
{
    if (!g_contacts.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (kind != XIAOTAI_CONTACT_ANY && !contacts_kind_valid(kind)) return XIAOTAI_ERR_INVALID_ARG;
    (void)contacts_check_generation();
    contacts_lock();
    g_contacts.status.kind = (int8_t)kind;
    g_contacts.status.page = page;
    contacts_view_update_locked();
    bool served = contacts_view_served_locked();
    contacts_unlock();
    if (!served) contacts_fetch_want();
    contacts_changed();
    contacts_fetch_kick();
    return XIAOTAI_OK;
}

size_t xiaotai_contacts_page(xiaotai_contact_t *contacts, size_t capacity)
{
    if (!contacts) return 0;
    size_t copied = 0;
    contacts_lock();
    if (contacts_usable_locked(XIAOTAI_CONTACTS_STALE_OK) == XIAOTAI_OK && contacts_view_served_locked()) {
        uint16_t start, count;
        page_bounds_locked(&start, &count);
        for (uint16_t i = 0; i < count && copied < capacity; ++i) {
            const xiaotai_contact_t *entry = contacts_entry_locked(g_contacts.status.kind, (uint16_t)(start + i));
            if (!entry) break;
            contacts[copied++] = *entry;
        }
    }
    contacts_unlock();
    return copied;
}

xiaotai_err_t xiaotai_contacts_at(uint8_t index, uint32_t flags, xiaotai_contact_t *contact)
{
    if (!contact) return XIAOTAI_ERR_INVALID_ARG;
    contacts_lock();
    xiaotai_err_t err = contacts_usable_locked(flags);
    if (err == XIAOTAI_OK) {
        err = XIAOTAI_ERR_NOT_FOUND;
        uint16_t start, count;
        page_bounds_locked(&start, &count);
        const xiaotai_contact_t *entry = index < count && contacts_view_served_locked()
                                             ? contacts_entry_locked(g_contacts.status.kind, (uint16_t)(start + index))
                                             : NULL;
        if (entry) {
            *contact = *entry;
            err = XIAOTAI_OK;
        }
    }
    contacts_unlock();
    return err;
}

xiaotai_err_t xiaotai_contacts_find(const char *id, int kind, uint32_t flags, xiaotai_contact_t *contact)
{
    if (!id || !id[0] || !contact) return XIAOTAI_ERR_INVALID_ARG;
    if (kind != XIAOTAI_CONTACT_ANY && !contacts_kind_valid(kind)) return XIAOTAI_ERR_INVALID_ARG;
    contacts_lock();
    xiaotai_err_t err = contacts_usable_locked(flags);
    if (err == XIAOTAI_OK) {
        err = XIAOTAI_ERR_NOT_FOUND;
        for (uint16_t i = 0; i < g_contacts.status.cached && err != XIAOTAI_OK; ++i) {
            const xiaotai_contact_t *entry = &g_contacts.cache[i];
            if ((kind == XIAOTAI_CONTACT_ANY || entry->kind == kind) && strcmp(entry->id, id) == 0) {
                *contact = *entry;
                err = XIAOTAI_OK;
            }
        }
        /* The first entry of a kind is kept outside the cached range too. */
        for (size_t k = 0; k < s_kind_count && err != XIAOTAI_OK; ++k) {
            const xiaotai_contact_t *entry = &g_contacts.firsts[k];
            if (g_contacts.first[k] && (kind == XIAOTAI_CONTACT_ANY || (int)k == kind) &&
                strcmp(entry->id, id) == 0) {
                *contact = *entry;
                err = XIAOTAI_OK;
            }
        }
    }
    contacts_unlock();
    return err;
}

xiaotai_err_t xiaotai_contacts_find_unique_online_prefix(const char *id, int kind, uint32_t flags,
                                                        xiaotai_contact_t *contact)
{
    if (!id || !id[0] || !contact || !contacts_kind_valid(kind)) return XIAOTAI_ERR_INVALID_ARG;
    contacts_lock();
    const uint16_t limit = g_contacts.config.at_device_dial_prefix;
    xiaotai_err_t err = limit ? contacts_usable_locked(flags) : XIAOTAI_ERR_NOT_SUPPORTED;
    if (err == XIAOTAI_OK) {
        const uint16_t total = g_contacts.status.total;
        const uint16_t count = total < limit ? total : limit;
        const xiaotai_contact_t *match = NULL;
        const xiaotai_contact_t *prefix = g_contacts.cache + g_contacts.config.capacity;
        if (count != g_contacts.dial_prefix_count) err = XIAOTAI_ERR_INVALID_STATE;
        for (uint16_t i = 0; err == XIAOTAI_OK && i < count; ++i) {
            const xiaotai_contact_t *entry = &prefix[i];
            if (entry->kind != kind || !entry->online_known || !entry->online || strcmp(entry->id, id) != 0) continue;
            if (match) {
                err = XIAOTAI_ERR_INVALID_STATE;
            } else {
                match = entry;
            }
        }
        if (err == XIAOTAI_OK) {
            if (!match) err = XIAOTAI_ERR_NOT_FOUND;
            else *contact = *match;
        }
    }
    contacts_unlock();
    return err;
}

xiaotai_err_t xiaotai_contacts_first(int kind, uint32_t flags, xiaotai_contact_t *contact)
{
    if (!contact || !contacts_kind_valid(kind)) return XIAOTAI_ERR_INVALID_ARG;
    contacts_lock();
    xiaotai_err_t err = contacts_usable_locked(flags);
    if (err == XIAOTAI_OK) {
        err = XIAOTAI_ERR_NOT_FOUND;
        if (g_contacts.first[kind]) {
            *contact = g_contacts.firsts[kind];
            err = XIAOTAI_OK;
        }
    }
    contacts_unlock();
    return err;
}

xiaotai_err_t xiaotai_contacts_at_kind(int kind, uint16_t index, uint32_t flags, xiaotai_contact_t *contact)
{
    if (!index) return xiaotai_contacts_first(kind, flags, contact);
    if (!contact || !contacts_kind_valid(kind)) return XIAOTAI_ERR_INVALID_ARG;
    contacts_lock();
    xiaotai_err_t err = contacts_usable_locked(flags);
    if (err == XIAOTAI_OK) {
        const xiaotai_contact_t *entry = NULL;
        if (g_contacts.cache_kind == kind) {
            entry = contacts_entry_locked(kind, index);
        } else if (g_contacts.cache_kind == XIAOTAI_CONTACT_ANY && g_contacts.cache_offset == 0) {
            /* An unfiltered suffix does not tell us how many of this kind preceded it. */
            uint16_t seen = 0;
            for (uint16_t i = 0; i < g_contacts.status.cached; ++i) {
                const xiaotai_contact_t *candidate = &g_contacts.cache[i];
                if (candidate->kind == kind && seen++ == index) {
                    entry = candidate;
                    break;
                }
            }
        }
        err = entry ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
        if (entry) *contact = *entry;
    }
    contacts_unlock();
    return err;
}

/* ---------------------------------------------------------------- intents */

static void intent_refresh(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    (void)xiaotai_contacts_refresh();
}

static void intent_show(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    int kind = g_contacts.status.kind;
    if (args->text[0]) {
        if (same_word(args->text, "all")) {
            kind = XIAOTAI_CONTACT_ANY;
        } else if (xiaotai_contacts_kind_find(args->text, &kind) != XIAOTAI_OK) {
            ++g_contacts.intents_refused;
            return;
        }
    }
    int32_t page = args->value < 0 ? 0 : args->value;
    (void)xiaotai_contacts_show(kind, page > UINT16_MAX ? UINT16_MAX : (uint16_t)page);
}

/* -------------------------------------------------------------- lifecycle */

static xiaotai_err_t check_config(const xiaotai_contacts_config_t *c)
{
    if (c->capacity > CONTACTS_CAPACITY_MAX || c->page_size > CONTACTS_PAGE_SIZE_MAX ||
        c->at_device_dial_prefix > CONTACTS_CAPACITY_MAX ||
        c->pending_page_size > CONTACTS_PENDING_PAGE_SIZE_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    uint16_t capacity = c->capacity ? c->capacity
                                    : (xiaotai_mem_has_external() ? CONTACTS_CAPACITY_EXTERNAL
                                                                  : CONTACTS_CAPACITY_INTERNAL);
    if (c->page_size > capacity) return XIAOTAI_ERR_INVALID_ARG;
    if (c->fetch > XIAOTAI_CONTACTS_FETCH_IDLE_ONLY || c->first_fetch > XIAOTAI_CONTACTS_ON_ONLINE) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    const xiaotai_contacts_source_t *s = c->source;
    if (s) {
        if (!s->service || !s->service[0] || !s->path || s->path[0] != '/' || !s->id_field || !s->id_field[0]) {
            return XIAOTAI_ERR_INVALID_ARG;
        }
        if (strlen(s->path) + sizeof("/pending") > XIAOTAI_PLATFORM_PATH_MAX) return XIAOTAI_ERR_INVALID_ARG;
        if (!s->type_field && (!s->default_type || !s->default_type[0])) return XIAOTAI_ERR_INVALID_ARG;
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_contacts_configure(const xiaotai_contacts_config_t *config)
{
    if (g_contacts.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (!config) {
        memset(&s_given, 0, sizeof(s_given));
        s_given_set = false;
        return XIAOTAI_OK;
    }
    xiaotai_err_t err = check_config(config);
    if (err != XIAOTAI_OK) return err;
    s_given = *config;
    s_given_set = true;
    return XIAOTAI_OK;
}

/* Defaults filled in. The page holds at most what the cache holds. */
static void resolve_config(void)
{
    xiaotai_contacts_config_t c = {0};
    if (s_given_set) c = s_given;
    bool external = xiaotai_mem_has_external();
    if (!c.capacity) c.capacity = external ? CONTACTS_CAPACITY_EXTERNAL : CONTACTS_CAPACITY_INTERNAL;
    if (!c.page_size) c.page_size = CONTACTS_PAGE_SIZE < c.capacity ? CONTACTS_PAGE_SIZE : (uint8_t)c.capacity;
    if (!c.pending_page_size) c.pending_page_size = CONTACTS_PENDING_PAGE_SIZE;
    if (!c.response_bytes) c.response_bytes = CONTACTS_RESPONSE_BYTES;
    if (!c.source) c.source = &DEFAULT_SOURCE;
    g_contacts.config = c;
    g_contacts.source = c.source;
    g_contacts.idle_only = c.fetch == XIAOTAI_CONTACTS_FETCH_IDLE_ONLY ||
                           (c.fetch == XIAOTAI_CONTACTS_FETCH_AUTO && !external);
}

static void release_storage(void)
{
    xiaotai_mem_free(g_contacts.cache);
    xiaotai_mem_free(g_contacts.firsts);
    xiaotai_mem_free(g_contacts.pending_ids);
    g_contacts.cache = NULL;
    g_contacts.firsts = NULL;
    g_contacts.pending_ids = NULL;
    g_contacts.token_capacity = 0;
    if (g_contacts.lock_ready) {
        xiaotai_mutex_deinit(&g_contacts.lock);
        g_contacts.lock_ready = false;
    }
}

xiaotai_err_t xiaotai_module_contacts_init(void)
{
    if (g_contacts.initialized) return XIAOTAI_ERR_INVALID_STATE;
    resolve_config();
    const xiaotai_contacts_config_t *c = &g_contacts.config;
    /* Platform requests stay offline throughout an exclusive portal boot. */
    g_contacts.provisioning_boot = xiaotai_net_provisioning_boot();
    g_contacts.token_capacity = c->response_bytes / CONTACTS_REPLY_BYTES_PER_TOKEN;
    g_contacts.cache = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, (size_t)c->capacity + c->at_device_dial_prefix,
                                         sizeof(xiaotai_contact_t));
    if (g_contacts.cache) {
        g_contacts.firsts = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_CONTACT_KINDS_MAX, sizeof(xiaotai_contact_t));
    }
    if (g_contacts.firsts && c->source->friendships) {
        g_contacts.pending_ids = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, c->pending_page_size, sizeof(*g_contacts.pending_ids));
    }
    if (!g_contacts.firsts || (c->source->friendships && !g_contacts.pending_ids)) {
        release_storage();
        return XIAOTAI_ERR_NO_MEM;
    }
    xiaotai_err_t err = xiaotai_mutex_init(&g_contacts.lock);
    if (err != XIAOTAI_OK) {
        release_storage();
        return err;
    }
    g_contacts.lock_ready = true;

    xiaotai_contacts_status_t *s = &g_contacts.status;
    memset(s, 0, sizeof(*s));
    s->kind = XIAOTAI_CONTACT_ANY;
    s->pages = 1;
    s->page_size = c->page_size;
    g_contacts.cache_kind = XIAOTAI_CONTACT_ANY;

    err = contacts_fetch_init();
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_CONTACTS_REFRESH, intent_refresh, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_CONTACTS_SHOW, intent_show, NULL);
    if (err == XIAOTAI_OK) err = contacts_change_init();
    if (err == XIAOTAI_OK) err = contacts_at_init();
    if (err != XIAOTAI_OK) {
        release_storage();
        return err;
    }
    g_contacts.initialized = true;
    return XIAOTAI_OK;
}

void xiaotai_module_contacts_start(void)
{
    g_contacts.started = true;
    /* A source without a type field names the kind of all its entries; a
     * product without that kind's module would see every entry skipped. */
    const xiaotai_contacts_source_t *source = g_contacts.source;
    if (source && !source->type_field && !g_contacts.default_type_checked) {
        g_contacts.default_type_checked = true;
        bool found = false;
        for (size_t k = 0; k < s_kind_count; ++k) {
            if (strcmp(s_kinds[k]->server_type, source->default_type) == 0) found = true;
        }
        if (!found) {
            XIAOTAI_LOGW(CONTACTS_TAG, "no kind registered for the source's default type; every entry is skipped");
            xiaotai_eventlog_warn(CONTACTS_TAG, "default_type.unregistered", 0, 0);
        }
    }
}

void xiaotai_contacts_reset(void)
{
    release_storage();
    memset(&g_contacts, 0, sizeof(g_contacts));
    memset(s_kinds, 0, sizeof(s_kinds));
    s_kind_count = 0;
    memset(&s_given, 0, sizeof(s_given));
    s_given_set = false;
}
