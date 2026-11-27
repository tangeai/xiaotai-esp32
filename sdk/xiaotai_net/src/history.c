// SPDX-License-Identifier: MIT
#include <string.h>
#include "store.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"

enum {HISTORY_MIN = 4, HISTORY_MAX = 5, HISTORY_VERSION = 1};

typedef struct {
    uint8_t version, count;
    xiaotai_net_credentials_t entries[HISTORY_MAX];
} history_t;

_Static_assert(sizeof(history_t) == 492 && offsetof(history_t, entries[4]) == 394, "history layout");
_Static_assert(sizeof(history_t) <= XIAOTAI_KV_SLOT_BYTES, "history fits one storage slot");

/* All reads and writes, including deferred work and reset, are serialized by
 * the network worker. The storage port independently owns flash and job data. */
static bool s_suspended;
static xiaotai_err_t s_invalid;
static xiaotai_net_credentials_t *s_deferred;

static bool enabled(void) { return net_state->config.layout == XIAOTAI_NET_STORE_RECORD && !s_suspended; }

static void report_error(bool failed)
{
    xiaotai_net_scan_status_t *status = net_scan_edit();
    if (status->history_error == failed) {
        net_snapshot_cancel();
    } else {
        status->history_error = failed;
        net_scan_commit();
    }
}

static bool valid(const history_t *history, size_t capacity)
{
    if (history->version != HISTORY_VERSION || history->count > capacity) return false;
    for (size_t i = 0; i < history->count; ++i) {
        if (!xiaotai_net_credentials_valid(&history->entries[i])) return false;
        for (size_t j = 0; j < i; ++j) {
            if (!strcmp(history->entries[i].ssid, history->entries[j].ssid)) return false;
        }
    }
    return true;
}

static void dispose(history_t *history)
{
    if (!history) return;
    net_store_wipe(history, sizeof(*history));
    xiaotai_mem_free(history);
}

static xiaotai_err_t read_history(history_t **out, size_t *capacity)
{
    *out = NULL;
    *capacity = 0;
    if (!enabled()) return XIAOTAI_ERR_NOT_FOUND;
    if (s_invalid != XIAOTAI_OK) return s_invalid;
    history_t *history = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*history));
    if (!history) {
        report_error(true);
        return XIAOTAI_ERR_NO_MEM;
    }
    size_t size = sizeof(*history);
    xiaotai_err_t result = xiaotai_kv_get_blob_strict(net_state->config.space, "known", history, &size,
                                                     XIAOTAI_KV_WAIT_MAX_MS);
    if (result == XIAOTAI_ERR_NOT_FOUND) {
        *capacity = net_state->config.history_capacity;
        history->version = HISTORY_VERSION;
        result = XIAOTAI_OK;
    } else if (result == XIAOTAI_OK) {
        if (size == offsetof(history_t, entries[HISTORY_MIN])) *capacity = HISTORY_MIN;
        else if (size == sizeof(*history)) *capacity = HISTORY_MAX;
        if (!*capacity) s_invalid = result = XIAOTAI_ERR_INVALID_SIZE;
        else if (!valid(history, *capacity)) s_invalid = result = XIAOTAI_FAIL;
    } else if (result == XIAOTAI_ERR_INVALID_SIZE || result == XIAOTAI_ERR_INVALID_ARG) {
        /* A larger or differently typed key is an existing incompatible history. */
        s_invalid = result;
    }
    report_error(result != XIAOTAI_OK);
    if (result == XIAOTAI_OK) *out = history;
    else dispose(history);
    return result;
}

static void clear_pending(void)
{
    if (!s_deferred) return;
    net_store_wipe(s_deferred, sizeof(*s_deferred));
    xiaotai_mem_free(s_deferred);
    s_deferred = NULL;
}

void net_history_suspend(void)
{
    s_suspended = true;
    clear_pending();
}

static void remember_now(const xiaotai_net_credentials_t *entry)
{
    history_t *history;
    size_t capacity;
    if (read_history(&history, &capacity) != XIAOTAI_OK) return;
    size_t pos = 0;
    while (pos < history->count && strcmp(entry->ssid, history->entries[pos].ssid)) ++pos;
    if (pos || !history->count || strcmp(entry->password, history->entries[0].password)) {
        if (pos == history->count && history->count < capacity) ++history->count;
        if (pos == capacity) --pos;
        memmove(&history->entries[1], &history->entries[0], pos * sizeof(*entry));
        memset(&history->entries[0], 0, sizeof(*entry));
        memcpy(history->entries[0].ssid, entry->ssid, strlen(entry->ssid));
        memcpy(history->entries[0].password, entry->password, strlen(entry->password));
        /* The port owns and wipes its copy. Jobs queued for this same key coalesce. */
        const xiaotai_err_t result = xiaotai_kv_set_async(net_state->config.space, "known", history,
            offsetof(history_t, entries) + capacity * sizeof(*entry));
        if (result != XIAOTAI_OK) report_error(true);
    }
    dispose(history);
}

void net_history_remember(const xiaotai_net_credentials_t *credentials)
{
    if (!enabled() || s_invalid != XIAOTAI_OK || !xiaotai_net_credentials_valid(credentials)) return;
    if (net_gate_config_begin() != XIAOTAI_OK) {
        if (!s_deferred) s_deferred = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*s_deferred));
        if (!s_deferred) {
            report_error(true);
            return;
        }
        *s_deferred = *credentials;
        xiaotai_net_diag_t *diag = net_diag_edit();
        ++diag->history_deferred;
        net_diag_done();
        net_maintenance_defer_history();
        return;
    }
    clear_pending();
    remember_now(credentials);
    net_gate_config_end();
}

void net_history_flush_deferred(void)
{
    /* The gate's maintenance-release hook already owns a configuration lease. */
    if (!s_deferred) return;
    remember_now(s_deferred);
    clear_pending();
}

static xiaotai_err_t find(const char *ssid, bool secure, char *password)
{
    if (password) memset(password, 0, XIAOTAI_NET_PASSWORD_MAX + 1);
    if (!ssid || !*ssid) return XIAOTAI_ERR_INVALID_ARG;
    history_t *history;
    size_t capacity;
    xiaotai_err_t result = read_history(&history, &capacity);
    if (result != XIAOTAI_OK) return result;
    result = XIAOTAI_ERR_NOT_FOUND;
    for (size_t i = 0; i < history->count; ++i) {
        const xiaotai_net_credentials_t *entry = &history->entries[i];
        if (secure != (entry->password[0] != '\0') || strcmp(ssid, entry->ssid)) continue;
        if (password) memcpy(password, entry->password, strlen(entry->password) + 1U);
        result = XIAOTAI_OK;
        break;
    }
    dispose(history);
    return result;
}

xiaotai_err_t net_history_password(const char *ssid, bool secure, char password[XIAOTAI_NET_PASSWORD_MAX + 1])
{
    return password ? find(ssid, secure, password) : XIAOTAI_ERR_INVALID_ARG;
}

bool net_history_saved(const char *ssid, bool secure) { return find(ssid, secure, NULL) == XIAOTAI_OK; }

xiaotai_err_t net_history_list(xiaotai_net_network_t *out, size_t capacity, size_t *count)
{
    if (!count || (capacity && !out)) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
    if (!capacity) return XIAOTAI_OK;
    history_t *history;
    size_t stored_capacity;
    const xiaotai_err_t result = read_history(&history, &stored_capacity);
    if (result == XIAOTAI_ERR_NOT_FOUND) return XIAOTAI_OK;
    if (result != XIAOTAI_OK) return result;
    while (*count < capacity && *count < history->count) {
        const xiaotai_net_credentials_t *entry = &history->entries[*count];
        const bool secure = entry->password[0] != '\0';
        xiaotai_net_network_t *network = &out[(*count)++];
        *network = (xiaotai_net_network_t){.secure = secure, .supported = true, .saved = true,
            .security = secure ? XIAOTAI_NET_SECURITY_OTHER : XIAOTAI_NET_SECURITY_OPEN};
        memcpy(network->ssid, entry->ssid, strlen(entry->ssid) + 1U);
    }
    dispose(history);
    return XIAOTAI_OK;
}

static void reset(void)
{
    s_suspended = false;
    s_invalid = XIAOTAI_OK;
    clear_pending();
}

const net_unit_t net_history_unit = {.name = "history", .link_address = net_history_remember,
    .maintenance_released = net_history_flush_deferred, .reset = reset};
