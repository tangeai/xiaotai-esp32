// SPDX-License-Identifier: MIT
/* Settings: registered at init, loaded once, handed to the setting store on
 * change. The store is the preferences service where the product selects the
 * identity engine (it installs itself at its init); without one the framework
 * keeps integers and booleans as 4-byte blobs and text as a terminated blob in
 * one namespace, keyed by each setting's own short key. */
#include <stdatomic.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"
#include "xiaotai/settings_confirmation.h"

#define SETTINGS_SPACE "xt_settings"
#define SETTINGS_LOAD_WAIT_MS 1000U

typedef struct {
    xiaotai_setting_t setting;
    int16_t app;               /* registering application; -1: an engine or module */
    uint16_t text_max;         /* payload bytes, without NUL */
    uint32_t revision;         /* written and read by the owner only */
    union {
        int32_t integer;
        char *text;            /* sized at registration; never replaced at runtime */
    } value;
} entry_t;

typedef struct int_admission {
    const entry_t *entry;
    xiaotai_setting_accept_int_fn_t accept;
    struct int_admission *next;
} int_admission_t;

static const char *TAG = "xiaotai_settings";
static entry_t *s_entries;     /* XIAOTAI_FRAMEWORK_MAX_SETTINGS, BULK */
static int_admission_t *s_admissions; /* only settings that install admission */
static _Atomic size_t s_count;
static xiaotai_mutex_t s_lock; /* readers on other tasks vs the owner's writes */
static bool s_lock_ready;
static xiaotai_setting_store_t s_store;
static bool s_store_set;
static bool s_store_write_active; /* owner, while a store callback is on its stack */
static _Atomic uint32_t s_refused;

/* ------------------------------------------------- the framework's own store */

static xiaotai_err_t kv_load_int(void *self, const char *key, int32_t default_value, int32_t *value)
{
    (void)self;
    (void)default_value;
    int32_t stored;
    size_t size = sizeof(stored);
    const xiaotai_err_t err = xiaotai_kv_get(SETTINGS_SPACE, key, &stored, &size, SETTINGS_LOAD_WAIT_MS);
    if (err != XIAOTAI_OK) return err;
    if (size != sizeof(stored)) return XIAOTAI_ERR_NOT_FOUND;
    *value = stored;
    return XIAOTAI_OK;
}

static xiaotai_err_t kv_load_text(void *self, const char *key, char *out, size_t capacity)
{
    (void)self;
    size_t size = capacity;
    const xiaotai_err_t err = xiaotai_kv_get(SETTINGS_SPACE, key, out, &size, SETTINGS_LOAD_WAIT_MS);
    if (err != XIAOTAI_OK) return err;
    return size > 0 && out[size - 1] == '\0' ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t kv_store_int(void *self, const char *key, int32_t value)
{
    (void)self;
    return xiaotai_kv_set_async(SETTINGS_SPACE, key, &value, sizeof(value));
}

static xiaotai_err_t kv_store_text(void *self, const char *key, const char *value)
{
    (void)self;
    return xiaotai_kv_set_async(SETTINGS_SPACE, key, value, strlen(value) + 1);
}

/* Waits for every write queued before it, the asynchronous ones included. */
static xiaotai_err_t kv_flush(void *self, uint32_t timeout_ms)
{
    (void)self;
    return xiaotai_kv_flush(timeout_ms);
}

static const xiaotai_setting_store_t KV_STORE = {
    .load_int = kv_load_int, .load_text = kv_load_text, .store_int = kv_store_int, .store_text = kv_store_text,
    .flush = kv_flush,
};

static const xiaotai_setting_store_t *store(void) { return s_store_set ? &s_store : &KV_STORE; }

/* ---------------------------------------------------------------- registry */

void fw_settings_reset(void)
{
    while (s_admissions) {
        int_admission_t *const item = s_admissions;
        s_admissions = item->next;
        xiaotai_mem_free(item);
    }
    const size_t count = atomic_load(&s_count);
    for (size_t i = 0; i < count; ++i) {
        if (s_entries[i].setting.kind == XIAOTAI_SETTING_TEXT) xiaotai_mem_free(s_entries[i].value.text);
    }
    xiaotai_mem_free(s_entries);
    s_entries = NULL;
    atomic_store(&s_count, 0);
    if (s_lock_ready) xiaotai_mutex_deinit(&s_lock);
    s_lock_ready = false;
    memset(&s_store, 0, sizeof(s_store));
    s_store_set = false;
    s_store_write_active = false;
    atomic_store(&s_refused, 0U);
}

xiaotai_err_t fw_settings_flush(uint32_t timeout_ms)
{
    const xiaotai_setting_store_t *current = store();
    return current->flush ? current->flush(current->self, timeout_ms) : XIAOTAI_OK;
}

uint32_t fw_settings_refused(void) { return atomic_load(&s_refused); }

xiaotai_err_t xiaotai_setting_store_set(const xiaotai_setting_store_t *candidate)
{
    if (!candidate || !candidate->load_int || !candidate->load_text || !candidate->store_int ||
        !candidate->store_text) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!fw_initializing() || s_store_set || atomic_load(&s_count)) return XIAOTAI_ERR_INVALID_STATE;
    s_store = *candidate;
    s_store_set = true;
    return XIAOTAI_OK;
}

static entry_t *find(const char *name)
{
    const size_t count = atomic_load(&s_count);
    for (size_t i = 0; name && i < count; ++i) {
        if (strcmp(s_entries[i].setting.name, name) == 0) return &s_entries[i];
    }
    return NULL;
}

static void not_saved(const char *name, xiaotai_err_t result, uint32_t revision)
{
    XIAOTAI_LOGW(TAG, "%s not stored: %d", name, result);
    xiaotai_args_t args = {.value = result};
    strncpy(args.text, name, sizeof(args.text) - 1);
    (void)fw_event_emit_optional(XIAOTAI_EVENT_SETTING_NOT_SAVED, &args, &revision,
                                 revision ? sizeof(revision) : 0);
}

static const entry_t *find_key(const char *key)
{
    const size_t count = atomic_load(&s_count);
    for (size_t i = 0; key && i < count; ++i)
        if (strcmp(s_entries[i].setting.key, key) == 0) return &s_entries[i];
    return NULL;
}

uint32_t xiaotai_setting_store_revision(const char *key)
{
    const entry_t *entry = xiaotai_on_owner() ? find_key(key) : NULL;
    return entry ? entry->revision : 0;
}

void xiaotai_setting_store_result_at(const char *key, xiaotai_err_t result, uint32_t revision)
{
    const entry_t *entry = result != XIAOTAI_OK ? find_key(key) : NULL;
    if (entry) not_saved(entry->setting.name, result, revision);
}

void xiaotai_setting_store_result(const char *key, xiaotai_err_t result)
{
    xiaotai_setting_store_result_at(key, result, 0);
}

bool xiaotai_setting_failure_current(const xiaotai_event_t *event)
{
    if (!event || !event->name || !xiaotai_on_owner() || strcmp(event->name, XIAOTAI_EVENT_SETTING_NOT_SAVED)) return false;
    if (!event->data || event->size != sizeof(uint32_t)) return true;
    uint32_t revision;
    memcpy(&revision, event->data, sizeof(revision));
    const entry_t *entry = find(event->args.text);
    return !revision || (entry && entry->revision == revision);
}

static bool in_range(const xiaotai_setting_t *setting, int32_t value)
{
    if (setting->kind == XIAOTAI_SETTING_BOOL) return value == 0 || value == 1;
    return value >= setting->min && value <= setting->max;
}

static xiaotai_err_t register_setting(const xiaotai_setting_t *setting, uint16_t text_max)
{
    if (!setting || !setting->name || !setting->key || strlen(setting->key) > XIAOTAI_KV_NAME_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!fw_initializing()) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_entries) {
        s_entries = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_FRAMEWORK_MAX_SETTINGS, sizeof(entry_t));
        if (!s_entries) return XIAOTAI_ERR_NO_MEM;
        if (xiaotai_mutex_init(&s_lock) != XIAOTAI_OK) {
            xiaotai_mem_free(s_entries);
            s_entries = NULL;
            return XIAOTAI_FAIL;
        }
        s_lock_ready = true;
    }
    const size_t count = atomic_load(&s_count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_entries[i].setting.name, setting->name) == 0 ||
            strcmp(s_entries[i].setting.key, setting->key) == 0) {
            XIAOTAI_LOGE(TAG, "setting %s or key %s registered twice", setting->name, setting->key);
            return XIAOTAI_ERR_INVALID_STATE;
        }
    }
    if (count >= XIAOTAI_FRAMEWORK_MAX_SETTINGS) return XIAOTAI_ERR_NO_MEM;
    char *text = NULL;
    if (setting->kind == XIAOTAI_SETTING_TEXT) {
        text = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, (size_t)text_max + 1U);
        if (!text) return XIAOTAI_ERR_NO_MEM;
    }
    entry_t *entry = &s_entries[count];
    entry->setting = *setting;
    entry->app = fw_app_current();
    entry->text_max = text_max;
    /* A stored value of the wrong shape or range keeps the default. */
    const xiaotai_setting_store_t *current = store();
    if (text) {
        entry->value.text = text;
        if (setting->default_text) strncpy(text, setting->default_text, text_max);
        char stored[XIAOTAI_SETTING_TEXT_MAX];
        if (current->load_text(current->self, setting->key, stored, sizeof(stored)) == XIAOTAI_OK &&
            memchr(stored, '\0', (size_t)text_max + 1U)) {
            strcpy(text, stored);
        }
    } else {
        entry->value.integer = setting->default_value;
        int32_t stored = setting->default_value;
        if (current->load_int(current->self, setting->key, setting->default_value, &stored) == XIAOTAI_OK &&
            in_range(setting, stored)) {
            entry->value.integer = stored;
        }
    }
    atomic_store(&s_count, count + 1);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_setting_register(const xiaotai_setting_t *setting)
{
    return register_setting(setting, XIAOTAI_SETTING_TEXT_MAX - 1U);
}

xiaotai_err_t xiaotai_setting_register_int_admission(const xiaotai_setting_t *setting,
                                                      xiaotai_setting_accept_int_fn_t accept)
{
    if (!setting || setting->kind == XIAOTAI_SETTING_TEXT || !accept) return XIAOTAI_ERR_INVALID_ARG;
    if (!fw_initializing()) return XIAOTAI_ERR_INVALID_STATE;
    int_admission_t *const item = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*item));
    if (!item) return XIAOTAI_ERR_NO_MEM;
    const size_t index = atomic_load(&s_count);
    const xiaotai_err_t err = xiaotai_setting_register(setting);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(item);
        return err;
    }
    *item = (int_admission_t){.entry = &s_entries[index], .accept = accept, .next = s_admissions};
    s_admissions = item;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_setting_register_text(const xiaotai_setting_t *setting, size_t max_bytes)
{
    if (!setting || setting->kind != XIAOTAI_SETTING_TEXT || max_bytes >= XIAOTAI_SETTING_TEXT_MAX) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (setting->default_text && strlen(setting->default_text) > max_bytes) return XIAOTAI_ERR_INVALID_SIZE;
    return register_setting(setting, (uint16_t)max_bytes);
}

xiaotai_err_t xiaotai_setting_get_int(const char *name, int32_t *value)
{
    if (!value) return XIAOTAI_ERR_INVALID_ARG;
    entry_t *entry = find(name);
    if (!entry || entry->setting.kind == XIAOTAI_SETTING_TEXT) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    *value = entry->value.integer;
    xiaotai_mutex_unlock(&s_lock);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_setting_get_text(const char *name, char *out, size_t capacity)
{
    if (!out || capacity == 0) return XIAOTAI_ERR_INVALID_ARG;
    entry_t *entry = find(name);
    if (!entry || entry->setting.kind != XIAOTAI_SETTING_TEXT) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    const size_t length = strlen(entry->value.text);
    const xiaotai_err_t err = length < capacity ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE;
    if (err == XIAOTAI_OK) memcpy(out, entry->value.text, length + 1);
    xiaotai_mutex_unlock(&s_lock);
    return err;
}

static void changed(entry_t *entry)
{
    if (!entry->setting.changed) return;
    const int16_t previous = fw_app_enter(entry->app);
    entry->setting.changed(entry->setting.name, entry->setting.ctx);
    fw_app_leave(previous);
}

/* While quiesced nothing new may be written: the operation that quiesced
 * decided on the stored state and the restart that follows must not cut a
 * write in half. */
static xiaotai_err_t admit(void)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (xiaotai_quiesced()) {
        atomic_fetch_add(&s_refused, 1U);
        return XIAOTAI_ERR_BUSY;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t admit_int(const char *name, int32_t value, entry_t **admitted)
{
    xiaotai_err_t err = admit();
    if (err != XIAOTAI_OK) return err;
    entry_t *entry = find(name);
    if (!entry || entry->setting.kind == XIAOTAI_SETTING_TEXT) return XIAOTAI_ERR_NOT_FOUND;
    if (!in_range(&entry->setting, value)) return XIAOTAI_ERR_INVALID_ARG;
    *admitted = entry;
    return XIAOTAI_OK;
}

static xiaotai_err_t write_int(const entry_t *entry, int32_t value)
{
    const xiaotai_setting_store_t *current = store();
    const bool nested = s_store_write_active;
    s_store_write_active = true;
    const xiaotai_err_t result = current->store_int(current->self, entry->setting.key, value);
    s_store_write_active = nested;
    return result;
}

xiaotai_err_t xiaotai_setting_set_int(const char *name, int32_t value)
{
    entry_t *entry;
    xiaotai_err_t err = admit_int(name, value, &entry);
    if (err != XIAOTAI_OK) return err;
    if (entry->value.integer == value) return XIAOTAI_OK;
    for (const int_admission_t *item = s_admissions; item; item = item->next) {
        if (item->entry != entry) continue;
        const int16_t previous = fw_app_enter(entry->app);
        const bool accepted = item->accept(value, entry->setting.ctx);
        fw_app_leave(previous);
        if (!accepted) return XIAOTAI_OK;
        break;
    }
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    entry->value.integer = value;
    entry->revision = xiaotai_generation_next();
    xiaotai_mutex_unlock(&s_lock);
    err = write_int(entry, value);
    if (err != XIAOTAI_OK) not_saved(name, err, entry->revision);
    changed(entry);
    return XIAOTAI_OK;
}

xiaotai_err_t fw_settings_confirm_int_selection(const char *name, int32_t value)
{
    entry_t *entry;
    const xiaotai_err_t admitted = admit_int(name, value, &entry);
    if (admitted != XIAOTAI_OK) return admitted;
    if (s_store_write_active) return XIAOTAI_ERR_BUSY;
    if (entry->value.integer != value) return XIAOTAI_ERR_INVALID_STATE;
    const uint32_t revision = entry->revision;
    const xiaotai_err_t result = write_int(entry, value);
    if (result != XIAOTAI_OK) not_saved(name, result, revision);
    return XIAOTAI_OK;
}

static xiaotai_err_t admit_text(const char *name, const char *value, entry_t **accepted, size_t *length)
{
    if (!value) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_err_t err = admit();
    if (err != XIAOTAI_OK) return err;
    entry_t *entry = find(name);
    if (!entry || entry->setting.kind != XIAOTAI_SETTING_TEXT) return XIAOTAI_ERR_NOT_FOUND;
    *length = strlen(value);
    if (*length > entry->text_max) return XIAOTAI_ERR_INVALID_SIZE;
    *accepted = entry;
    return XIAOTAI_OK;
}

static xiaotai_err_t write_text(const entry_t *entry, const char *value)
{
    const xiaotai_setting_store_t *current = store();
    const bool nested = s_store_write_active;
    s_store_write_active = true;
    const xiaotai_err_t result = current->store_text(current->self, entry->setting.key, value);
    s_store_write_active = nested;
    return result;
}

static void change_text(entry_t *entry, const char *value, size_t length)
{
    xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER);
    memcpy(entry->value.text, value, length + 1);
    entry->revision = xiaotai_generation_next();
    xiaotai_mutex_unlock(&s_lock);
    const xiaotai_err_t err = write_text(entry, value);
    if (err != XIAOTAI_OK) not_saved(entry->setting.name, err, entry->revision);
    changed(entry);
}

xiaotai_err_t xiaotai_setting_set_text(const char *name, const char *value)
{
    entry_t *entry;
    size_t length;
    const xiaotai_err_t admitted = admit_text(name, value, &entry, &length);
    if (admitted != XIAOTAI_OK) return admitted;
    if (strcmp(entry->value.text, value) != 0) change_text(entry, value, length);
    return XIAOTAI_OK;
}

xiaotai_err_t fw_settings_confirm_text(const char *name, const char *value)
{
    entry_t *entry;
    size_t length;
    const xiaotai_err_t admitted = admit_text(name, value, &entry, &length);
    if (admitted != XIAOTAI_OK) return admitted;
    if (s_store_write_active) return XIAOTAI_ERR_BUSY;
    if (strcmp(entry->value.text, value) != 0) {
        change_text(entry, value, length);
        return XIAOTAI_OK;
    }

    /* Store callbacks may use the registry and even update this setting.
     * Keep the confirmed value and its failure revision independent. */
    char confirmed[XIAOTAI_SETTING_TEXT_MAX];
    memcpy(confirmed, entry->value.text, length + 1U);
    const uint32_t revision = entry->revision;
    const xiaotai_err_t result = write_text(entry, confirmed);
    if (result != XIAOTAI_OK) not_saved(entry->setting.name, result, revision);
    return XIAOTAI_OK;
}

size_t xiaotai_setting_count(void) { return atomic_load(&s_count); }

const xiaotai_setting_t *xiaotai_setting_at(size_t index)
{
    return index < atomic_load(&s_count) ? &s_entries[index].setting : NULL;
}
