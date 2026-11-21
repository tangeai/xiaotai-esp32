// SPDX-License-Identifier: MIT
/* KV backend on the host: an in-memory table typed like NVS (string, blob, or
 * one of the eight integer types) with namespaces that exist once a writer
 * opened them. Tests can preset values, count what reaches the backend, and
 * inject a delay, a gate or failures (xiaotai_kv_host.h). */
#include "kv_backend.h"

#include <pthread.h>
#include <string.h>
#include <time.h>
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_kv_host.h"

#define ENTRIES 64U
#define SPACES 16U

typedef enum { TYPE_BLOB, TYPE_STR, TYPE_INT } entry_type_t;

typedef struct {
    bool used;
    entry_type_t type;
    xiaotai_kv_int_t int_type;
    unsigned space; /* index into s_spaces */
    char key[XIAOTAI_KV_NAME_MAX + 1];
    size_t size;
    int64_t number;
    uint8_t data[XIAOTAI_KV_VALUE_MAX];
} entry_t;

static entry_t s_entries[ENTRIES];
static char s_spaces[SPACES][XIAOTAI_KV_NAME_MAX + 1];

/* Test controls and counts, shared with the test's threads. */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
static uint32_t s_delay_ms;
static xiaotai_err_t s_fail;
static char s_fail_open_space[XIAOTAI_KV_NAME_MAX + 1];
static xiaotai_err_t s_fail_open_error;
static char s_fail_key[XIAOTAI_KV_NAME_MAX + 1];
static xiaotai_err_t s_fail_key_error;
static bool s_gate_closed, s_held;
static xiaotai_kv_host_counts_t s_counts;

void xiaotai_kv_host_delay(uint32_t ms)
{
    pthread_mutex_lock(&s_lock);
    s_delay_ms = ms;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_kv_host_fail(xiaotai_err_t error)
{
    pthread_mutex_lock(&s_lock);
    s_fail = error;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_kv_host_fail_open_space(const char *space, xiaotai_err_t error)
{
    pthread_mutex_lock(&s_lock);
    memset(s_fail_open_space, 0, sizeof(s_fail_open_space));
    s_fail_open_error = XIAOTAI_OK;
    if (space && error != XIAOTAI_OK) {
        strncpy(s_fail_open_space, space, sizeof(s_fail_open_space) - 1);
        s_fail_open_error = error;
    }
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_kv_host_fail_key(const char *key, xiaotai_err_t error)
{
    pthread_mutex_lock(&s_lock);
    memset(s_fail_key, 0, sizeof(s_fail_key));
    s_fail_key_error = XIAOTAI_OK;
    if (key && error != XIAOTAI_OK) {
        strncpy(s_fail_key, key, sizeof(s_fail_key) - 1);
        s_fail_key_error = error;
    }
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_kv_host_gate(bool open)
{
    pthread_mutex_lock(&s_lock);
    s_gate_closed = !open;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
}

bool xiaotai_kv_host_wait_held(uint32_t timeout_ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec += 1;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s_lock);
    while (!s_held && pthread_cond_timedwait(&s_changed, &s_lock, &until) == 0) {
    }
    const bool held = s_held;
    pthread_mutex_unlock(&s_lock);
    return held;
}

void xiaotai_kv_host_counts(xiaotai_kv_host_counts_t *counts)
{
    pthread_mutex_lock(&s_lock);
    *counts = s_counts;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_kv_host_clear(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_spaces, 0, sizeof(s_spaces));
    memset(&s_counts, 0, sizeof(s_counts));
    s_delay_ms = 0;
    s_fail = XIAOTAI_OK;
    memset(s_fail_open_space, 0, sizeof(s_fail_open_space));
    s_fail_open_error = XIAOTAI_OK;
    memset(s_fail_key, 0, sizeof(s_fail_key));
    s_fail_key_error = XIAOTAI_OK;
    s_gate_closed = false;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
}

/* The injected error for an operation on key, counting a write. */
static xiaotai_err_t operation(const char *key, bool write)
{
    pthread_mutex_lock(&s_lock);
    if (write) ++s_counts.writes;
    const xiaotai_err_t err = s_fail_key[0] && strcmp(s_fail_key, key) == 0 ? s_fail_key_error : XIAOTAI_OK;
    pthread_mutex_unlock(&s_lock);
    return err;
}

static entry_t *find(unsigned space, const char *key)
{
    for (unsigned i = 0; i < ENTRIES; ++i) {
        if (s_entries[i].used && s_entries[i].space == space && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

/* An existing key whatever its type (a set replaces it, as NVS does), or a
 * free entry. */
static entry_t *slot_for(unsigned space, const char *key)
{
    entry_t *entry = find(space, key);
    for (unsigned i = 0; !entry && i < ENTRIES; ++i) {
        if (!s_entries[i].used) entry = &s_entries[i];
    }
    if (!entry) return NULL;
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->space = space;
    strcpy(entry->key, key);
    return entry;
}

bool xiaotai_kv_backend_recovery_error(xiaotai_err_t error)
{
    /* The host injects real ESP NVS error codes at backend init. */
    return error == 0x110d || error == 0x1110;
}

xiaotai_err_t xiaotai_kv_backend_init(void)
{
    return xiaotai_require_internal_stack("nvs_flash_init");
}

xiaotai_err_t xiaotai_kv_backend_open(const char *space, bool write, xiaotai_kv_handle_t *handle)
{
    if (write) {
        const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_open(write)");
        if (stack != XIAOTAI_OK) return stack;
    }
    pthread_mutex_lock(&s_lock);
    ++s_counts.opens;
    const uint32_t delay_ms = s_delay_ms;
    pthread_mutex_unlock(&s_lock);
    if (delay_ms) xiaotai_delay_ms(delay_ms);

    pthread_mutex_lock(&s_lock);
    while (s_gate_closed) {
        s_held = true;
        pthread_cond_broadcast(&s_changed);
        pthread_cond_wait(&s_changed, &s_lock);
    }
    s_held = false;
    const xiaotai_err_t fail = s_fail != XIAOTAI_OK ? s_fail :
        !write && !strcmp(s_fail_open_space, space) ? s_fail_open_error : XIAOTAI_OK;
    pthread_mutex_unlock(&s_lock);
    if (fail != XIAOTAI_OK) return fail;

    unsigned found = SPACES, free_space = SPACES;
    for (unsigned i = 0; i < SPACES; ++i) {
        if (s_spaces[i][0] && strcmp(s_spaces[i], space) == 0) found = i;
        if (!s_spaces[i][0] && free_space == SPACES) free_space = i;
    }
    if (found == SPACES) {
        if (!write) return XIAOTAI_ERR_NOT_FOUND;
        if (free_space == SPACES) return XIAOTAI_ERR_NO_MEM;
        strcpy(s_spaces[free_space], space);
        found = free_space;
    }
    *handle = (xiaotai_kv_handle_t)found + 1U;
    return XIAOTAI_OK;
}

void xiaotai_kv_backend_close(xiaotai_kv_handle_t handle)
{
    (void)handle;
    pthread_mutex_lock(&s_lock);
    ++s_counts.closes;
    pthread_mutex_unlock(&s_lock);
}

static xiaotai_err_t get_bytes(xiaotai_kv_handle_t handle, const char *key, bool text, bool strict,
                               void *data, size_t *size)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack(text ? "nvs_get_str" : "nvs_get_blob");
    if (stack != XIAOTAI_OK) return stack;
    const xiaotai_err_t err = operation(key, false);
    if (err != XIAOTAI_OK) return err;
    entry_t *entry = find(handle - 1U, key);
    if (!entry) return XIAOTAI_ERR_NOT_FOUND;
    if (entry->type != (text ? TYPE_STR : TYPE_BLOB)) return strict ? XIAOTAI_ERR_INVALID_ARG : XIAOTAI_ERR_NOT_FOUND;
    if (entry->size > *size) {
        *size = entry->size; /* as NVS: the stored length */
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    memcpy(data, entry->data, entry->size);
    *size = entry->size;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_backend_get(xiaotai_kv_handle_t handle, const char *key, bool text, void *data,
                                     size_t *size)
{
    return get_bytes(handle, key, text, false, data, size);
}

xiaotai_err_t xiaotai_kv_backend_get_blob_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                 size_t *size)
{
    return get_bytes(handle, key, false, true, data, size);
}

xiaotai_err_t xiaotai_kv_backend_get_str_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                size_t *size)
{
    return get_bytes(handle, key, true, true, data, size);
}

xiaotai_err_t xiaotai_kv_backend_get_int(xiaotai_kv_handle_t handle, const char *key, xiaotai_kv_int_t type,
                                         int64_t *value)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_get_int");
    if (stack != XIAOTAI_OK) return stack;
    const xiaotai_err_t err = operation(key, false);
    if (err != XIAOTAI_OK) return err;
    entry_t *entry = find(handle - 1U, key);
    if (!entry || entry->type != TYPE_INT || entry->int_type != type) return XIAOTAI_ERR_NOT_FOUND;
    *value = entry->number;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_backend_set(xiaotai_kv_handle_t handle, const char *key, bool text, const void *data,
                                     size_t size)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack(text ? "nvs_set_str" : "nvs_set_blob");
    if (stack != XIAOTAI_OK) return stack;
    const xiaotai_err_t err = operation(key, true);
    if (err != XIAOTAI_OK) return err;
    if (size > XIAOTAI_KV_VALUE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    if (text && (!size || ((const char *)data)[size - 1] != '\0')) return XIAOTAI_ERR_INVALID_ARG;
    entry_t *entry = slot_for(handle - 1U, key);
    if (!entry) return XIAOTAI_ERR_NO_MEM;
    entry->type = text ? TYPE_STR : TYPE_BLOB;
    if (size) memcpy(entry->data, data, size);
    entry->size = size;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_backend_set_int(xiaotai_kv_handle_t handle, const char *key, xiaotai_kv_int_t type,
                                         int64_t value)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_set_int");
    if (stack != XIAOTAI_OK) return stack;
    const xiaotai_err_t err = operation(key, true);
    if (err != XIAOTAI_OK) return err;
    entry_t *entry = slot_for(handle - 1U, key);
    if (!entry) return XIAOTAI_ERR_NO_MEM;
    entry->type = TYPE_INT;
    entry->int_type = type;
    entry->number = value;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_backend_erase(xiaotai_kv_handle_t handle, const char *key)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_erase_key");
    if (stack != XIAOTAI_OK) return stack;
    const xiaotai_err_t err = operation(key, true);
    if (err != XIAOTAI_OK) return err;
    entry_t *entry = find(handle - 1U, key);
    if (entry) memset(entry, 0, sizeof(*entry));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_backend_erase_all(xiaotai_kv_handle_t handle)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_erase_all");
    if (stack != XIAOTAI_OK) return stack;
    pthread_mutex_lock(&s_lock);
    ++s_counts.writes;
    ++s_counts.erase_alls;
    pthread_mutex_unlock(&s_lock);
    for (unsigned i = 0; i < ENTRIES; ++i) {
        if (s_entries[i].used && s_entries[i].space == handle - 1U) memset(&s_entries[i], 0, sizeof(s_entries[i]));
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_commit");
    if (stack != XIAOTAI_OK) return stack;
    (void)handle;
    pthread_mutex_lock(&s_lock);
    ++s_counts.commits;
    pthread_mutex_unlock(&s_lock);
    return XIAOTAI_OK;
}
