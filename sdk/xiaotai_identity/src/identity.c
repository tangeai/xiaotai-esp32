// SPDX-License-Identifier: MIT
/* Identity core: configuration, the loaded identity and its state, the
 * binding generation, validation, the constant-time comparison, wiping and
 * counters. Storage access goes through the layouts (identity_layout.c) and
 * the storage port only.
 *
 * Locking: one mutex guards the loaded identity, its state and the counters;
 * it is created by the engine init and kept for the life of the program
 * (host tests reset the component without destroying it). Before the engine
 * init every reader answers from the "nothing loaded" state without locking.
 * The generation is a lone atomic so real-time paths read it without a lock. */
#include <stdatomic.h>
#include <string.h>

#include "identity_internal.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"

#define TAG "identity"

#define DEFAULT_SETTINGS                                                                                  \
    {                                                                                                     \
        .layout = XIAOTAI_IDENTITY_LAYOUT_STRINGS, .space = XIAOTAI_IDENTITY_STRINGS_SPACE,               \
        .fingerprint = XIAOTAI_FINGERPRINT_UPPER_COLON, .client_prefix = NULL, .storage_error_unbound = false \
    }

static xiaotai_identity_settings_t s_settings = DEFAULT_SETTINGS;
static xiaotai_mutex_t s_lock;
static bool s_lock_created;
static atomic_bool s_ready;
static xiaotai_identity_t s_identity;
static xiaotai_identity_state_t s_state = XIAOTAI_IDENTITY_ABSENT;
static xiaotai_identity_stats_t s_stats;
/* Starts at 1 on every boot; never 0. */
static _Atomic uint32_t s_generation = 1U;
static _Atomic uint32_t s_generation_advances;

static void lock(void) { (void)xiaotai_mutex_lock(&s_lock, XIAOTAI_WAIT_FOREVER); }
static void unlock(void) { xiaotai_mutex_unlock(&s_lock); }

static const char *state_name(xiaotai_identity_state_t state)
{
    switch (state) {
    case XIAOTAI_IDENTITY_ABSENT: return "absent";
    case XIAOTAI_IDENTITY_VALID: return "valid";
    case XIAOTAI_IDENTITY_INCOMPLETE: return "incomplete";
    case XIAOTAI_IDENTITY_UNREADABLE: return "unreadable";
    }
    return "?";
}

/* Bounded text fields: 1..max characters for required ones. */
static bool text_ok(const char *text, size_t capacity, bool required)
{
    const size_t length = strnlen(text, capacity);
    return length < capacity && (!required || length > 0U);
}

bool xiaotai_identity_valid(const xiaotai_identity_t *identity)
{
    return identity != NULL && text_ok(identity->device_id, sizeof(identity->device_id), true) &&
           text_ok(identity->device_key, sizeof(identity->device_key), true) &&
           text_ok(identity->client_id, sizeof(identity->client_id), false);
}

void xiaotai_identity_wipe(void *data, size_t size)
{
    /* Through a volatile pointer, so the stores are not removed as dead. */
    volatile uint8_t *bytes = data;
    if (bytes == NULL) return;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0U;
}

/* Copies a valid identity with every byte after a terminator zero, the form
 * the comparison below relies on. */
static void copy_identity(xiaotai_identity_t *to, const xiaotai_identity_t *from)
{
    xiaotai_identity_wipe(to, sizeof(*to));
    memcpy(to->device_id, from->device_id, strnlen(from->device_id, XIAOTAI_IDENTITY_DEVICE_ID_MAX));
    memcpy(to->device_key, from->device_key, strnlen(from->device_key, XIAOTAI_IDENTITY_DEVICE_KEY_MAX));
    memcpy(to->client_id, from->client_id, strnlen(from->client_id, XIAOTAI_IDENTITY_CLIENT_ID_MAX));
}

/* True when given equals the zero-padded field. Always walks the whole field,
 * so the time does not depend on where the texts differ; given is read up to
 * its terminator only (its own length is the caller's knowledge anyway). */
static bool field_equal(const char *field, size_t capacity, const char *given)
{
    unsigned diff = 0U;
    bool ended = false;
    for (size_t i = 0; i < capacity; ++i) {
        unsigned char c = 0U;
        if (!ended) {
            c = (unsigned char)given[i];
            ended = c == 0U;
        }
        diff |= (unsigned)(c ^ (unsigned char)field[i]);
    }
    if (!ended) diff |= 1U; /* longer than the field can hold */
    return diff == 0U;
}

static bool same_binding(const xiaotai_identity_t *loaded, const char *device_id, const char *device_key)
{
    /* Both comparisons always run (no short circuit). */
    const bool id = field_equal(loaded->device_id, sizeof(loaded->device_id), device_id);
    const bool key = field_equal(loaded->device_key, sizeof(loaded->device_key), device_key);
    return id & key;
}

/* ---------------------------------------------------------------- lifecycle */

xiaotai_err_t xiaotai_identity_configure(const xiaotai_identity_config_t *config)
{
    if (atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_identity_settings_t settings = DEFAULT_SETTINGS;
    if (config == NULL) {
        s_settings = settings;
        return XIAOTAI_OK;
    }
    if (config->layout != XIAOTAI_IDENTITY_LAYOUT_STRINGS && config->layout != XIAOTAI_IDENTITY_LAYOUT_RECORD) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (config->fingerprint != XIAOTAI_FINGERPRINT_UPPER_COLON &&
        config->fingerprint != XIAOTAI_FINGERPRINT_LOWER_COLON) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    const char *space = config->space;
    if (space == NULL) {
        /* The record layout has no default: its namespace also holds other
         * owners' data, so guessing it could erase the wrong entry. */
        if (config->layout == XIAOTAI_IDENTITY_LAYOUT_RECORD) return XIAOTAI_ERR_INVALID_ARG;
        space = XIAOTAI_IDENTITY_STRINGS_SPACE;
    }
    const size_t space_length = strnlen(space, XIAOTAI_KV_NAME_MAX + 1U);
    if (space_length == 0U || space_length > XIAOTAI_KV_NAME_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (config->client_prefix != NULL) {
        const size_t prefix_length = strnlen(config->client_prefix, XIAOTAI_IDENTITY_PREFIX_MAX + 1U);
        if (prefix_length == 0U || prefix_length > XIAOTAI_IDENTITY_PREFIX_MAX) return XIAOTAI_ERR_INVALID_ARG;
    }
    settings.layout = config->layout;
    memcpy(settings.space, space, space_length);
    settings.space[space_length] = '\0';
    settings.fingerprint = config->fingerprint;
    settings.client_prefix = config->client_prefix;
    settings.storage_error_unbound = config->storage_error_unbound;
    s_settings = settings;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_engine_identity_init(void)
{
    if (atomic_load(&s_ready)) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_lock_created) {
        const xiaotai_err_t err = xiaotai_mutex_init(&s_lock);
        if (err != XIAOTAI_OK) return err;
        s_lock_created = true;
    }
    const xiaotai_err_t jobs = identity_jobs_init();
    if (jobs != XIAOTAI_OK) return jobs;
    const xiaotai_err_t store = identity_install_setting_store();
    if (store != XIAOTAI_OK) return store;

    xiaotai_identity_t loaded;
    xiaotai_identity_state_t state = XIAOTAI_IDENTITY_ABSENT;
    const xiaotai_err_t read = xiaotai_identity_layout_read(&s_settings, &loaded, &state);
    if (read != XIAOTAI_OK) {
        /* A state, not an init failure: the device runs on and binding decides. */
        state = s_settings.storage_error_unbound ? XIAOTAI_IDENTITY_INCOMPLETE : XIAOTAI_IDENTITY_UNREADABLE;
        xiaotai_identity_wipe(&loaded, sizeof(loaded));
    }
    lock();
    if (state == XIAOTAI_IDENTITY_VALID) {
        copy_identity(&s_identity, &loaded);
    } else {
        xiaotai_identity_wipe(&s_identity, sizeof(s_identity));
    }
    s_state = state;
    if (read == XIAOTAI_OK) {
        ++s_stats.loads;
    } else {
        ++s_stats.load_failures;
    }
    unlock();
    xiaotai_identity_wipe(&loaded, sizeof(loaded));
    atomic_store(&s_ready, true);

    if (read != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "identity storage read failed (0x%x); state=%s", (unsigned)read, state_name(state));
    } else {
        XIAOTAI_LOGI(TAG, "identity state=%s layout=%s space=%s", state_name(state),
                     s_settings.layout == XIAOTAI_IDENTITY_LAYOUT_RECORD ? "record" : "strings", s_settings.space);
    }
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------------ reading */

xiaotai_identity_state_t xiaotai_identity_state(void)
{
    if (!atomic_load(&s_ready)) return XIAOTAI_IDENTITY_ABSENT;
    lock();
    const xiaotai_identity_state_t state = s_state;
    unlock();
    return state;
}

xiaotai_err_t xiaotai_identity_get(xiaotai_identity_t *out)
{
    if (out == NULL) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_identity_wipe(out, sizeof(*out));
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_NOT_FOUND;
    lock();
    const bool valid = s_state == XIAOTAI_IDENTITY_VALID;
    if (valid) *out = s_identity;
    unlock();
    return valid ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_identity_device_id(char *out, size_t capacity)
{
    if (out == NULL || capacity == 0U) return XIAOTAI_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!atomic_load(&s_ready)) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_err_t err = XIAOTAI_ERR_NOT_FOUND;
    lock();
    if (s_state == XIAOTAI_IDENTITY_VALID) {
        const size_t length = strnlen(s_identity.device_id, sizeof(s_identity.device_id));
        if (length < capacity) {
            memcpy(out, s_identity.device_id, length + 1U);
            err = XIAOTAI_OK;
        } else {
            err = XIAOTAI_ERR_INVALID_SIZE;
        }
    }
    unlock();
    return err;
}

bool xiaotai_identity_matches(const char *device_id, const char *device_key)
{
    if (device_id == NULL || device_key == NULL || !atomic_load(&s_ready)) return false;
    lock();
    const bool valid = s_state == XIAOTAI_IDENTITY_VALID;
    const bool same = same_binding(&s_identity, device_id, device_key);
    unlock();
    return valid & same;
}

/* ------------------------------------------------- state of finished jobs */

const xiaotai_identity_settings_t *xiaotai_identity_settings(void) { return &s_settings; }

bool xiaotai_identity_ready(void) { return atomic_load(&s_ready); }

bool xiaotai_identity_apply(xiaotai_identity_apply_t kind, const xiaotai_identity_t *identity,
                            xiaotai_identity_state_t state)
{
    if (!s_lock_created) return false;
    if ((kind == XIAOTAI_IDENTITY_APPLY_SAVE ||
         (kind == XIAOTAI_IDENTITY_APPLY_RELOAD && state == XIAOTAI_IDENTITY_VALID)) &&
        !xiaotai_identity_valid(identity)) {
        return false;
    }
    bool advance = false;
    lock();
    const bool had = s_state == XIAOTAI_IDENTITY_VALID;
    switch (kind) {
    case XIAOTAI_IDENTITY_APPLY_SAVE:
        advance = !had || !same_binding(&s_identity, identity->device_id, identity->device_key);
        copy_identity(&s_identity, identity);
        s_state = XIAOTAI_IDENTITY_VALID;
        break;
    case XIAOTAI_IDENTITY_APPLY_RELOAD:
        if (state == XIAOTAI_IDENTITY_VALID) {
            advance = had && !same_binding(&s_identity, identity->device_id, identity->device_key);
            copy_identity(&s_identity, identity);
        } else {
            advance = had;
            xiaotai_identity_wipe(&s_identity, sizeof(s_identity));
        }
        s_state = state;
        break;
    case XIAOTAI_IDENTITY_APPLY_ERASE:
        advance = true;
        xiaotai_identity_wipe(&s_identity, sizeof(s_identity));
        s_state = XIAOTAI_IDENTITY_ABSENT;
        break;
    }
    /* A reader that acquires the replacement identity must also see its
     * generation. Otherwise an older reply can still look current. */
    if (advance) (void)xiaotai_identity_generation_advance();
    unlock();
    return advance;
}

void xiaotai_identity_stored_client_id(char out[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U])
{
    out[0] = '\0';
    if (!atomic_load(&s_ready)) return;
    lock();
    if (s_state == XIAOTAI_IDENTITY_VALID) memcpy(out, s_identity.client_id, sizeof(s_identity.client_id));
    unlock();
}

/* ------------------------------------------------------- binding generation */

uint32_t xiaotai_identity_generation_next(uint32_t current)
{
    const uint32_t next = current + 1U;
    return next == 0U ? 1U : next;
}

uint32_t xiaotai_identity_generation(void) { return atomic_load(&s_generation); }

uint32_t xiaotai_identity_generation_advance(void)
{
    uint32_t current = atomic_load(&s_generation);
    uint32_t next;
    do {
        next = xiaotai_identity_generation_next(current);
    } while (!atomic_compare_exchange_weak(&s_generation, &current, next));
    atomic_fetch_add(&s_generation_advances, 1U);
    return next;
}

/* -------------------------------------------------------------- diagnostics */

void xiaotai_identity_stats(xiaotai_identity_stats_t *stats)
{
    if (stats == NULL) return;
    memset(stats, 0, sizeof(*stats));
    if (s_lock_created) {
        lock();
        *stats = s_stats;
        unlock();
    }
    stats->generation_advances = atomic_load(&s_generation_advances);
}

void identity_count_job(identity_job_stat_t outcome)
{
    lock();
    switch (outcome) {
    case IDENTITY_LOADED: ++s_stats.loads; break;
    case IDENTITY_LOAD_FAILED: ++s_stats.load_failures; break;
    case IDENTITY_SAVED: ++s_stats.saves; break;
    case IDENTITY_SAVE_FAILED: ++s_stats.save_failures; break;
    case IDENTITY_ERASED: ++s_stats.erases; break;
    case IDENTITY_REFUSED: ++s_stats.rejected_jobs; break;
    }
    unlock();
}

void xiaotai_identity_reset(void)
{
    identity_jobs_silence();
    if (s_lock_created) lock();
    atomic_store(&s_ready, false);
    xiaotai_identity_wipe(&s_identity, sizeof(s_identity));
    s_state = XIAOTAI_IDENTITY_ABSENT;
    memset(&s_stats, 0, sizeof(s_stats));
    s_settings = (xiaotai_identity_settings_t)DEFAULT_SETTINGS;
    atomic_store(&s_generation, 1U);
    atomic_store(&s_generation_advances, 0U);
    if (s_lock_created) unlock();
}
