// SPDX-License-Identifier: MIT
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include "preferences_record.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define PREF_SLOTS 16U
#define PREF_TEXT_SLOTS 4U
#define PREF_RECORD_SLOTS PREF_RECORDS_MAX
#define PREF_DELAY_MS 1000U
#define PREF_FLUSH_POLL_MS 5U
#define SNAPSHOT_ENQUEUE_US INT64_C(2000000)
#define SNAPSHOT_RETRY_US INT64_C(100000)

typedef uint16_t slots_t;
#define SLOT(i) ((slots_t)(1U << (i)))
_Static_assert(PREF_SLOTS < sizeof(uintptr_t) * CHAR_BIT, "KV callback needs a snapshot flag");
_Static_assert(XIAOTAI_KV_OPS_MAX <= PREF_SLOTS, "snapshot edit mask must fit");
#define SNAPSHOT_JOB ((uintptr_t)1U << PREF_SLOTS)

typedef struct {
    int64_t due;
    char key[XIAOTAI_KV_NAME_MAX + 1U];
    int32_t value;
    uint32_t revision;
} change_t;

typedef enum { RECORD_UNREAD, RECORD_ABSENT, RECORD_VALID, RECORD_INVALID } record_state_t;

typedef struct {
    const xiaotai_preference_record_t *layout;
    uint32_t default_fields;
    record_state_t state;
    uint8_t raw[XIAOTAI_PREFERENCE_RECORD_MAX]; /* committed image, used by loads */
    uint8_t defaults[XIAOTAI_PREFERENCE_RECORD_MAX];
    uint8_t intended[XIAOTAI_PREFERENCE_RECORD_MAX]; /* retained across a failed write for the next edit */
    xiaotai_err_t read_result;
    bool reading, has_intent, committing, durable_uncertain;
} record_t;

typedef struct {
    int64_t numbers[XIAOTAI_KV_OPS_MAX]; /* indexed by marked binding order */
    char text[XIAOTAI_PREFERENCE_TEXT_MAX];
} snapshot_image_t;

typedef struct {
    slots_t mask;
    uint32_t revision[XIAOTAI_KV_OPS_MAX];
} snapshot_edits_t;

typedef struct {
    snapshot_image_t captured; /* only the owner accesses the working copy */
    snapshot_image_t waiting;
    snapshot_edits_t edits, active_edits;
    int64_t next_retry, waiting_until, active_until;
    const xiaotai_preference_binding_t *cold_binding;
    xiaotai_err_t error, first_other_failure, cold_load_error;
    bool in_flight;
} snapshot_t;

typedef struct {
    xiaotai_err_t cold_load_error;
    xiaotai_err_t other_failure;
    xiaotai_err_t errors[]; /* indexed by typed binding, not transient write slot */
} maintenance_t;

static struct {
    xiaotai_preferences_config_t config;
    bool used;
    bool setting_listener;
    bool packed_retry;
    change_t changes[PREF_SLOTS];
    uint8_t text_slot[PREF_SLOTS]; /* zero for an integer; one-based text index otherwise */
    char texts[PREF_TEXT_SLOTS][XIAOTAI_PREFERENCE_TEXT_MAX];
    uint8_t text_used;
    slots_t waiting, in_flight;
    record_t records[PREF_RECORD_SLOTS];
    xiaotai_preferences_stats_t stats;
    union {
        xiaotai_preferences_written_fn_t unversioned;
        pref_setting_written_fn versioned;
    } listener;
    void *listener_ctx;
    xiaotai_err_t first_failure;
    snapshot_t *snapshot;
    maintenance_t *maintenance;
} s;

static xiaotai_critical_t s_lock;
static atomic_uint s_lock_state;
static atomic_uint s_pending;
static atomic_bool s_pumping;
static atomic_uint s_epoch;
static uint32_t s_timer;
static int64_t s_timer_due;

/* The lock protects only in-memory state, never calls into KV or the framework. */
static void enter(void)
{
    if (atomic_load(&s_lock_state) != 2U) {
        unsigned expected = 0;
        if (atomic_compare_exchange_strong(&s_lock_state, &expected, 1U)) {
            xiaotai_critical_init(&s_lock);
            atomic_store(&s_lock_state, 2U);
        } else {
            while (atomic_load(&s_lock_state) != 2U) xiaotai_delay_ms(1);
        }
    }
    xiaotai_critical_enter(&s_lock);
}

static void leave(void) { xiaotai_critical_exit(&s_lock); }

static const xiaotai_preference_binding_t *binding(const char *name)
{
    for (size_t i = 0; i < s.config.binding_count; ++i)
        if (!strcmp(s.config.bindings[i].setting, name)) return &s.config.bindings[i];
    return NULL;
}

static record_t *record_for(const xiaotai_preference_record_t *layout)
{
    for (size_t i = 0; i < PREF_RECORD_SLOTS; ++i) {
        record_t *record = &s.records[i];
        if (!record->layout) record->layout = layout;
        if (pref_same_record(record->layout, layout)) return record;
    }
    return NULL;
}

xiaotai_err_t xiaotai_preferences_configure(const xiaotai_preferences_config_t *config)
{
    if (config && !pref_bindings_valid(config)) return XIAOTAI_ERR_INVALID_ARG;
    snapshot_t *image = NULL;
    for (size_t i = 0; config && i < config->binding_count; ++i) {
        if (!config->bindings[i].snapshot) continue;
        image = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, sizeof(*image));
        if (!image) return XIAOTAI_ERR_NO_MEM;
        break;
    }
    maintenance_t *maintenance = NULL;
    if (config && config->retain_maintenance_failures) {
        if (config->binding_count > (SIZE_MAX - sizeof(*maintenance)) / sizeof(xiaotai_err_t))
            return XIAOTAI_ERR_INVALID_ARG;
        maintenance = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U,
                                         sizeof(*maintenance) + config->binding_count * sizeof(xiaotai_err_t));
        if (!maintenance) return XIAOTAI_ERR_NO_MEM;
    }
    enter();
    const bool used = s.used;
    if (!used) {
        s.config = config ? *config : (xiaotai_preferences_config_t){0};
        snapshot_t *old = s.snapshot;
        s.snapshot = image;
        image = old;
        maintenance_t *old_maintenance = s.maintenance;
        s.maintenance = maintenance;
        maintenance = old_maintenance;
    }
    leave();
    xiaotai_mem_free(image);
    xiaotai_mem_free(maintenance);
    return used ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
}

static bool listener_taken(void)
{
    return s.setting_listener ? s.listener.versioned != NULL : s.listener.unversioned != NULL;
}

xiaotai_err_t xiaotai_preferences_set_listener(xiaotai_preferences_written_fn_t listener, void *ctx)
{
    enter();
    const bool taken = listener && listener_taken();
    if (!taken) {
        s.setting_listener = false;
        s.listener.unversioned = listener;
        s.listener_ctx = listener ? ctx : NULL;
    }
    leave();
    return taken ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
}

xiaotai_err_t pref_setting_listener(pref_setting_written_fn listener)
{
    enter();
    const bool taken = listener && listener_taken();
    if (!taken) {
        s.setting_listener = true;
        s.listener.versioned = listener;
        s.listener_ctx = NULL;
    }
    leave();
    return taken ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_OK;
}

static void count_load(xiaotai_err_t err, bool invalid)
{
    if (err == XIAOTAI_OK) ++s.stats.loads;
    if (invalid) ++s.stats.invalid;
}

static void blank_record(const xiaotai_preference_record_t *layout, uint8_t *raw)
{
    memset(raw, 0, layout->size);
    if (layout->version_offset != XIAOTAI_PREFERENCE_NO_VERSION)
        raw[layout->version_offset] = layout->version_write;
}

/* A failed read may be retried; a successfully observed invalid record stays
 * blocked until reboot, even if later settings look individually plausible. */
static xiaotai_err_t load_field(const xiaotai_preference_binding_t *b, int32_t fallback, int32_t *value)
{
    record_t *record = record_for(b->record);
    if (!record) { leave(); return XIAOTAI_ERR_INVALID_STATE; }
    uint8_t raw[XIAOTAI_PREFERENCE_RECORD_MAX];
    blank_record(b->record, raw);
    if (pref_encode_field(b, fallback, raw)) {
        record->defaults[b->offset] = raw[b->offset];
        record->default_fields |= UINT32_C(1) << b->offset;
    }
    xiaotai_err_t err = XIAOTAI_ERR_NOT_FOUND;
    if (record->state == RECORD_UNREAD && record->reading) {
        const int64_t deadline = xiaotai_time_us() + (int64_t)PREF_READ_TIMEOUT_MS * 1000;
        do {
            leave();
            if (xiaotai_time_us() >= deadline) return XIAOTAI_ERR_TIMEOUT;
            xiaotai_delay_ms(1);
            enter();
        } while (record->reading);
        err = record->read_result;
    } else if (record->state == RECORD_UNREAD) {
        record->reading = true;
        leave();
        bool invalid = false;
        err = pref_load_record(&s.config, b->record, raw, &invalid);
        enter();
        record->read_result = err;
        record->reading = false;
        if (err == XIAOTAI_OK) {
            memcpy(record->raw, raw, b->record->size);
            record->state = RECORD_VALID;
        } else if (invalid) {
            record->state = RECORD_INVALID;
            ++s.stats.invalid;
        } else if (err == XIAOTAI_ERR_NOT_FOUND) {
            record->state = RECORD_ABSENT;
        }
    }
    if (record->state == RECORD_VALID) {
        *value = pref_decode_field(b, record->raw);
        err = XIAOTAI_OK;
    } else if (record->state == RECORD_INVALID && s.config.report_invalid_packed_load) {
        err = XIAOTAI_ERR_INVALID_STATE;
    } else if (record->state != RECORD_UNREAD) {
        err = XIAOTAI_ERR_NOT_FOUND;
    }
    count_load(err, false);
    leave();
    return err;
}

/* Only setting registration records cold errors. A runtime read or a
 * later successful save must not rewrite this startup verdict. */
static void note_cold_load(const xiaotai_preference_binding_t *binding, xiaotai_err_t result)
{
    if (!binding || result == XIAOTAI_OK || result == XIAOTAI_ERR_NOT_FOUND) return;
    /* The manifest's binding order is the native layout order. The modules
     * need not register settings in that same order. */
    if (!s.snapshot->cold_binding || binding < s.snapshot->cold_binding) {
        s.snapshot->cold_binding = binding;
        s.snapshot->cold_load_error = result;
    }
}

/* Legacy individual reads keep a failed key's default, but a failed storage
 * job or namespace open makes the boot's preferences status uncertain. */
static void note_cold_namespace(xiaotai_kv_read_stage_t stage, xiaotai_err_t result)
{
    if (!s.maintenance || stage == XIAOTAI_KV_READ_STAGE_ENTRY || result == XIAOTAI_OK ||
        result == XIAOTAI_ERR_NOT_FOUND) return;
    if (s.maintenance->cold_load_error == XIAOTAI_OK) s.maintenance->cold_load_error = result;
}

static xiaotai_err_t load_int(const char *key, int32_t fallback, int32_t *value, bool registering)
{
    if (!pref_name_valid(key) || !value) return XIAOTAI_ERR_INVALID_ARG;
    enter();
    s.used = true;
    const xiaotai_preference_binding_t *b = binding(key);
    if (b && b->storage == XIAOTAI_PREFERENCE_FIELD) return load_field(b, fallback, value);
    const bool native = registering && b && b->snapshot;
    const bool capture_stage = registering && b && s.maintenance;
    leave();
    bool invalid = false;
    xiaotai_kv_read_stage_t stage = XIAOTAI_KV_READ_STAGE_JOB;
    const xiaotai_err_t err = pref_load_plain_int(key, b, value, native, &invalid, capture_stage ? &stage : NULL);
    enter();
    note_cold_load(native ? b : NULL, err);
    if (capture_stage) note_cold_namespace(stage, err);
    count_load(err, invalid);
    leave();
    return err;
}

static xiaotai_err_t load_text(const char *key, char *out, size_t capacity, bool registering)
{
    if (!pref_name_valid(key) || !out || !capacity) return XIAOTAI_ERR_INVALID_ARG;
    enter();
    s.used = true;
    const xiaotai_preference_binding_t *b = binding(key);
    const bool native = registering && b && b->snapshot;
    const bool capture_stage = registering && b && s.maintenance;
    leave();
    bool invalid = false;
    xiaotai_kv_read_stage_t stage = XIAOTAI_KV_READ_STAGE_JOB;
    const xiaotai_err_t err = pref_load_text(key, b, out, capacity, native, &invalid, capture_stage ? &stage : NULL);
    enter();
    note_cold_load(native ? b : NULL, err);
    if (capture_stage) note_cold_namespace(stage, err);
    count_load(err, invalid);
    leave();
    return err;
}

xiaotai_err_t xiaotai_preferences_load_int(const char *key, int32_t fallback, int32_t *value)
{
    return load_int(key, fallback, value, false);
}

xiaotai_err_t xiaotai_preferences_packed_load_error(void)
{
    xiaotai_err_t result = XIAOTAI_OK;
    enter();
    if (s.config.report_invalid_packed_load) {
        for (size_t i = 0; i < PREF_RECORD_SLOTS; ++i) {
            if (s.records[i].state == RECORD_INVALID) {
                result = XIAOTAI_ERR_INVALID_STATE;
                break;
            }
        }
    }
    leave();
    return result;
}

xiaotai_err_t xiaotai_preferences_load_text(const char *key, char *out, size_t capacity)
{
    return load_text(key, out, capacity, false);
}

xiaotai_err_t pref_load_setting_int(const char *key, int32_t fallback, int32_t *value)
{
    return load_int(key, fallback, value, true);
}

xiaotai_err_t pref_load_setting_text(const char *key, char *out, size_t capacity)
{
    return load_text(key, out, capacity, true);
}

static uint32_t delay_ms(void)
{
    const int32_t configured = s.config.write_delay_ms;
    return configured > 0 ? (uint32_t)configured : configured < 0 ? 0 : PREF_DELAY_MS;
}

static void timer_fired(void *ctx);

static void arm(int64_t not_before)
{
    int64_t due = INT64_MAX;
    const int64_t now = xiaotai_time_us();
    enter();
    for (unsigned i = 0; i < PREF_SLOTS; ++i)
        if ((s.waiting & SLOT(i)) && s.changes[i].due < due) due = s.changes[i].due;
    if (due != INT64_MAX && due < not_before) due = not_before;
    if (s.snapshot && s.snapshot->edits.mask) {
        const int64_t candidate = s.snapshot->in_flight ? now + SNAPSHOT_RETRY_US : s.snapshot->next_retry;
        if (candidate < due) due = candidate;
    }
    leave();
    if (s_timer && due == s_timer_due) return;
    xiaotai_timer_cancel(s_timer);
    s_timer = 0;
    if (due == INT64_MAX) return;
    const int64_t left = due - xiaotai_time_us();
    const uint32_t ms = left <= 0 ? 0 : (uint32_t)((left + 999) / 1000);
    s_timer_due = due;
    if (xiaotai_timer_start(ms, timer_fired, (void *)(uintptr_t)atomic_load(&s_epoch), &s_timer) != XIAOTAI_OK) {
        enter();
        ++s.stats.deferred;
        leave();
    }
}

static bool encode_record(record_t *record, const xiaotai_preference_binding_t *changed,
                          int32_t value, uint8_t *raw)
{
    if (!record || (record->state != RECORD_ABSENT && record->state != RECORD_VALID)) return false;
    blank_record(record->layout, raw);
    for (size_t i = 0; i < s.config.binding_count; ++i) {
        const xiaotai_preference_binding_t *b = &s.config.bindings[i];
        if (b->storage != XIAOTAI_PREFERENCE_FIELD || !pref_same_record(record->layout, b->record)) continue;
        if (b == changed) {
            if (!pref_encode_field(b, value, raw)) return false;
        } else if (record->has_intent || record->state == RECORD_VALID) {
            if (!pref_copy_field(b, record->has_intent ? record->intended : record->raw, raw)) return false;
        } else {
            if (!(record->default_fields & (UINT32_C(1) << b->offset))) return false;
            raw[b->offset] = record->defaults[b->offset];
        }
    }
    return true;
}

/* The owner captures a complete image after the framework has applied the
 * edit. A flush may submit it from another task, but never reads UI state. */
static xiaotai_err_t capture_snapshot(const xiaotai_preference_binding_t *changed, int32_t value,
                                      const char *text, snapshot_image_t *image, unsigned *changed_index)
{
    unsigned index = 0;
    for (size_t i = 0; i < s.config.binding_count; ++i) {
        const xiaotai_preference_binding_t *b = &s.config.bindings[i];
        if (!b->snapshot) continue;
        if (b == changed) {
            *changed_index = index;
            if (b->storage == XIAOTAI_PREFERENCE_STRING) {
                if (!text) return XIAOTAI_ERR_INVALID_ARG;
                strcpy(image->text, text);
            } else if (text || !pref_value_to_raw(b, value, &image->numbers[index])) {
                return XIAOTAI_ERR_INVALID_ARG;
            }
        } else {
            const xiaotai_setting_t *setting = NULL;
            for (size_t j = 0; j < xiaotai_setting_count(); ++j) {
                const xiaotai_setting_t *candidate = xiaotai_setting_at(j);
                if (candidate && !strcmp(candidate->key, b->setting)) { setting = candidate; break; }
            }
            if (!setting) return XIAOTAI_ERR_INVALID_STATE;
            if (b->storage == XIAOTAI_PREFERENCE_STRING) {
                const xiaotai_err_t read = xiaotai_setting_get_text(setting->name, image->text, sizeof(image->text));
                if (read != XIAOTAI_OK) return read;
            } else {
                int32_t current;
                const xiaotai_err_t read = xiaotai_setting_get_int(setting->name, &current);
                if (read != XIAOTAI_OK) return read;
                if (!pref_value_to_raw(b, current, &image->numbers[index])) return XIAOTAI_ERR_INVALID_ARG;
            }
        }
        ++index;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t remember_snapshot(const xiaotai_preference_binding_t *binding, int32_t value,
                                       const char *text, uint32_t revision)
{
    unsigned index = 0;
    snapshot_t *group = s.snapshot;
    const xiaotai_err_t read = capture_snapshot(binding, value, text, &group->captured, &index);
    if (read != XIAOTAI_OK) return read;
    const int64_t now = xiaotai_time_us();
    enter();
    if (!group->edits.mask) atomic_fetch_add(&s_pending, 1U);
    if (group->edits.mask & SLOT(index)) ++s.stats.coalesced;
    group->waiting = group->captured;
    group->edits.mask |= SLOT(index);
    group->edits.revision[index] = revision;
    group->next_retry = now;
    group->waiting_until = now + SNAPSHOT_ENQUEUE_US;
    ++s.stats.stores;
    leave();
    arm(0);
    return XIAOTAI_OK;
}

static xiaotai_err_t remember(const char *key, int32_t value, const char *text, uint32_t revision)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    size_t length = 0;
    if (text) {
        while (length < XIAOTAI_PREFERENCE_TEXT_MAX && text[length]) ++length;
        if (length == XIAOTAI_PREFERENCE_TEXT_MAX) return XIAOTAI_ERR_INVALID_SIZE;
        ++length;
    }
    enter();
    s.used = true;
    const xiaotai_preference_binding_t *b = binding(key);
    if (b && b->snapshot) {
        leave();
        return remember_snapshot(b, value, text, revision);
    }
    xiaotai_err_t err = XIAOTAI_OK;
    if (b) {
        if (text ? b->storage != XIAOTAI_PREFERENCE_STRING : b->storage == XIAOTAI_PREFERENCE_STRING) {
            err = XIAOTAI_ERR_INVALID_ARG;
        } else if (b->storage == XIAOTAI_PREFERENCE_FIELD) {
            uint8_t candidate[XIAOTAI_PREFERENCE_RECORD_MAX];
            int64_t ignored;
            record_t *record = record_for(b->record);
            if (!record || (record->state != RECORD_ABSENT && record->state != RECORD_VALID)) {
                ++s.stats.refused;
                err = XIAOTAI_ERR_INVALID_STATE;
            } else if (!pref_value_to_raw(b, value, &ignored)) {
                err = XIAOTAI_ERR_INVALID_ARG;
            } else if (!encode_record(record, b, value, candidate)) {
                ++s.stats.refused;
                err = XIAOTAI_ERR_INVALID_STATE;
            }
        } else if (b->storage == XIAOTAI_PREFERENCE_INT) {
            int64_t ignored;
            if (!pref_value_to_raw(b, value, &ignored)) err = XIAOTAI_ERR_INVALID_ARG;
        }
    }
    unsigned free_slot = PREF_SLOTS, same = PREF_SLOTS;
    for (unsigned i = 0; err == XIAOTAI_OK && i < PREF_SLOTS; ++i) {
        if ((s.waiting & SLOT(i)) && !strcmp(s.changes[i].key, key)) { same = i; break; }
        if (!((s.waiting | s.in_flight) & SLOT(i)) && free_slot == PREF_SLOTS) free_slot = i;
    }
    const unsigned index = same != PREF_SLOTS ? same : free_slot;
    if (err == XIAOTAI_OK && index == PREF_SLOTS) err = XIAOTAI_ERR_NO_MEM;
    if (err == XIAOTAI_OK) {
        change_t *change = &s.changes[index];
        uint8_t slot = same != PREF_SLOTS ? s.text_slot[index] : 0;
        if (text && !slot) {
            for (unsigned i = 0; i < PREF_TEXT_SLOTS; ++i)
                if (!(s.text_used & SLOT(i))) { slot = (uint8_t)(i + 1U); break; }
            if (!slot) err = XIAOTAI_ERR_NO_MEM;
        }
        if (err == XIAOTAI_OK) {
            if (text) {
                s.text_used |= SLOT(slot - 1U);
                memcpy(s.texts[slot - 1U], text, length);
            } else if (slot) {
                s.text_used &= ~SLOT(slot - 1U);
                slot = 0;
            }
            memcpy(change->key, key, strlen(key) + 1U);
            change->value = value;
            change->revision = revision;
            s.text_slot[index] = slot;
            change->due = xiaotai_time_us() + (int64_t)delay_ms() * 1000;
            ++s.stats.stores;
            if (same != PREF_SLOTS) ++s.stats.coalesced;
            else atomic_fetch_add(&s_pending, 1U);
            if (b && b->record && b->record->debounce_as_record) {
                /* Postpone only waiting peers. The admitted image and its
                 * revisions belong to the storage worker until completion. */
                for (unsigned i = 0; i < PREF_SLOTS; ++i) {
                    if (i == index || !(s.waiting & SLOT(i))) continue;
                    const xiaotai_preference_binding_t *peer = binding(s.changes[i].key);
                    if (peer && pref_same_record(b->record, peer->record)) s.changes[i].due = change->due;
                }
            }
            s.waiting |= SLOT(index);
        }
    }
    leave();
    if (err == XIAOTAI_OK) arm(0);
    return err;
}

xiaotai_err_t xiaotai_preferences_store_int(const char *key, int32_t value)
{
    return pref_store_setting_int(key, value, 0);
}

xiaotai_err_t xiaotai_preferences_store_text(const char *key, const char *value)
{
    return pref_store_setting_text(key, value, 0);
}

xiaotai_err_t pref_store_setting_int(const char *key, int32_t value, uint32_t revision)
{
    return pref_name_valid(key) ? remember(key, value, NULL, revision) : XIAOTAI_ERR_INVALID_ARG;
}

xiaotai_err_t pref_store_setting_text(const char *key, const char *value, uint32_t revision)
{
    return pref_name_valid(key) && value ? remember(key, 0, value, revision) : XIAOTAI_ERR_INVALID_ARG;
}

typedef struct {
    const char *space;
    slots_t selected;
    uint8_t count;
    xiaotai_kv_op_t ops[XIAOTAI_KV_OPS_MAX];
    uint8_t integers[XIAOTAI_KV_OPS_MAX][PREF_INT_BLOB_SIZE];
    union {
        uint8_t records[PREF_RECORD_SLOTS][XIAOTAI_PREFERENCE_RECORD_MAX];
        char snapshot_text[XIAOTAI_PREFERENCE_TEXT_MAX];
    };
    uint8_t prior_records[PREF_RECORD_SLOTS][XIAOTAI_PREFERENCE_RECORD_MAX];
    uint8_t record_mask;
    uint8_t prior_mask;
    bool snapshot;
} batch_t;

static void snapshot_batch(batch_t *batch)
{
    batch->snapshot = true;
    for (size_t i = 0; i < s.config.binding_count; ++i) {
        const xiaotai_preference_binding_t *b = &s.config.bindings[i];
        if (!b->snapshot) continue;
        batch->space = b->space;
        xiaotai_kv_op_t *op = &batch->ops[batch->count];
        *op = (xiaotai_kv_op_t){.key = b->key};
        if (b->storage == XIAOTAI_PREFERENCE_STRING) {
            strcpy(batch->snapshot_text, s.snapshot->waiting.text);
            op->kind = XIAOTAI_KV_OP_SET_STR;
            op->data = batch->snapshot_text;
            op->size = strlen(batch->snapshot_text) + 1U;
        } else {
            op->kind = XIAOTAI_KV_OP_SET_INT;
            op->int_type = b->int_type;
            op->number = s.snapshot->waiting.numbers[batch->count];
        }
        ++batch->count;
    }
    s.snapshot->active_edits = s.snapshot->edits;
    s.snapshot->active_until = s.snapshot->waiting_until;
    s.snapshot->edits.mask = 0;
    s.snapshot->in_flight = true;
}

static bool batch_add(batch_t *batch, unsigned index)
{
    const change_t *change = &s.changes[index];
    const xiaotai_preference_binding_t *b = binding(change->key);
    const bool field = b && b->storage == XIAOTAI_PREFERENCE_FIELD;
    const char *space = !b ? PREF_SPACE_DEFAULT : field ? b->record->space : b->space;
    if (batch->space && strcmp(batch->space, space)) return false;

    if (field) {
        record_t *record = record_for(b->record);
        if (!record || record->committing) return false;
        const unsigned n = (unsigned)(record - s.records);
        uint8_t *raw = batch->records[n];
        if (batch->record_mask & SLOT(n)) {
            if (!pref_encode_field(b, change->value, raw)) return false;
        } else {
            if (batch->count == XIAOTAI_KV_OPS_MAX || !encode_record(record, b, change->value, raw)) return false;
            batch->record_mask |= SLOT(n);
            batch->ops[batch->count++] = (xiaotai_kv_op_t){.key = b->record->key, .data = raw, .size = b->record->size};
        }
    } else {
        if (batch->count == XIAOTAI_KV_OPS_MAX) return false;
        xiaotai_kv_op_t *op = &batch->ops[batch->count];
        *op = (xiaotai_kv_op_t){.key = b ? b->key : change->key};
        const uint8_t text = s.text_slot[index];
        if (text) {
            op->data = s.texts[text - 1U];
            op->size = strlen(op->data) + 1U;
            if (b) op->kind = op->size == 1U ? XIAOTAI_KV_OP_ERASE : XIAOTAI_KV_OP_SET_STR;
        } else if (b) {
            if (!pref_value_to_raw(b, change->value, &op->number)) return false;
            op->kind = XIAOTAI_KV_OP_SET_INT;
            op->int_type = b->int_type;
        } else {
            uint8_t *raw = batch->integers[batch->count];
            pref_int_to_blob(change->value, raw);
            op->data = raw;
            op->size = PREF_INT_BLOB_SIZE;
        }
        ++batch->count;
    }
    batch->space = space;
    batch->selected |= SLOT(index);
    return true;
}

static void clear_slot(unsigned index)
{
    const uint8_t text = s.text_slot[index];
    if (text) s.text_used &= ~SLOT(text - 1U);
    s.text_slot[index] = 0;
    s.changes[index] = (change_t){0};
    atomic_fetch_sub(&s_pending, 1U);
}

static bool same_durable_record(const record_t *record, const uint8_t *raw)
{
    if (!record->layout->elide_unchanged || record->committing || record->durable_uncertain) return false;
    for (size_t i = 0; i < s.config.binding_count; ++i) {
        const xiaotai_preference_binding_t *b = &s.config.bindings[i];
        if (b->storage != XIAOTAI_PREFERENCE_FIELD || !pref_same_record(record->layout, b->record)) continue;
        if (record->state == RECORD_VALID) {
            if (pref_decode_field(b, raw) != pref_decode_field(b, record->raw)) return false;
        } else if (record->state != RECORD_ABSENT || !(record->default_fields & (UINT32_C(1) << b->offset)) ||
                   raw[b->offset] != record->defaults[b->offset]) {
            return false;
        }
    }
    return true;
}

/* Only discard complete equal images. Nothing reached KV, so there is no
 * write result to publish and no in-flight revision to replace. */
static bool discard_unchanged_records(batch_t *batch)
{
    uint8_t unchanged = 0;
    for (unsigned i = 0; i < PREF_RECORD_SLOTS; ++i) {
        if ((batch->record_mask & SLOT(i)) && same_durable_record(&s.records[i], batch->records[i]))
            unchanged |= SLOT(i);
    }
    if (!unchanged) return false;

    unsigned kept = 0;
    for (unsigned i = 0; i < batch->count; ++i) {
        bool discard = false;
        for (unsigned j = 0; j < PREF_RECORD_SLOTS; ++j)
            if ((unchanged & SLOT(j)) && batch->ops[i].data == batch->records[j]) { discard = true; break; }
        if (!discard) batch->ops[kept++] = batch->ops[i];
    }
    batch->count = (uint8_t)kept;
    batch->record_mask &= ~unchanged;

    for (unsigned i = 0; i < PREF_SLOTS; ++i) {
        if (!(batch->selected & SLOT(i))) continue;
        const xiaotai_preference_binding_t *b = binding(s.changes[i].key);
        if (!b || b->storage != XIAOTAI_PREFERENCE_FIELD) continue;
        for (unsigned j = 0; j < PREF_RECORD_SLOTS; ++j) {
            if (!(unchanged & SLOT(j)) || !pref_same_record(s.records[j].layout, b->record)) continue;
            clear_slot(i);
            s.waiting &= ~SLOT(i);
            batch->selected &= ~SLOT(i);
            break;
        }
    }
    return true;
}

static void complete_packed_retry(xiaotai_err_t result, void *ctx)
{
    record_t *record = ctx;
    enter();
    if (result == XIAOTAI_OK) {
        memcpy(record->raw, record->intended, record->layout->size);
        record->state = RECORD_VALID;
        record->durable_uncertain = false;
    } else {
        ++s.stats.write_failures;
        if (s.first_failure == XIAOTAI_OK) s.first_failure = result;
    }
    record->committing = false;
    s.packed_retry = false;
    atomic_fetch_sub(&s_pending, 1U);
    leave();
}

xiaotai_err_t xiaotai_preferences_packed_maintenance(bool retry)
{
    if (!xiaotai_on_owner() || !xiaotai_quiesced()) return XIAOTAI_ERR_INVALID_STATE;
    enter();
    if (atomic_load(&s_pending)) {
        leave();
        return XIAOTAI_ERR_BUSY;
    }
    record_t *record = NULL;
    for (unsigned i = 0; i < PREF_RECORD_SLOTS; ++i) {
        if (s.records[i].durable_uncertain) { record = &s.records[i]; break; }
    }
    if (!record) {
        leave();
        return XIAOTAI_OK;
    }
    if (!retry || !record->has_intent || !record->layout) {
        leave();
        return XIAOTAI_FAIL;
    }
    /* The failed image already belongs to the record. Reserve it until KV
     * completes; only the storage port copies the request for its worker. */
    const char *space = record->layout->space;
    const xiaotai_kv_op_t op = {.key = record->layout->key, .data = record->intended, .size = record->layout->size};
    record->committing = true;
    s.packed_retry = true;
    atomic_fetch_add(&s_pending, 1U);
    leave();
    const xiaotai_err_t result = xiaotai_kv_commit_async(space, &op, 1, complete_packed_retry, record);
    enter();
    if (result == XIAOTAI_OK) {
        ++s.stats.writes;
    } else {
        record->committing = false;
        s.packed_retry = false;
        atomic_fetch_sub(&s_pending, 1U);
    }
    leave();
    return result;
}

static void complete_snapshot(xiaotai_err_t result, bool worker)
{
    enter();
    snapshot_t *group = s.snapshot;
    const snapshot_edits_t edits = group->active_edits;
    const xiaotai_preferences_written_fn_t listener =
        worker && !s.setting_listener ? s.listener.unversioned : NULL;
    const pref_setting_written_fn setting_listener = s.setting_listener ? s.listener.versioned : NULL;
    void *const listener_ctx = s.listener_ctx;
    group->error = result;
    if (result != XIAOTAI_OK && s.first_failure == XIAOTAI_OK) s.first_failure = result;
    leave();

    unsigned index = 0;
    unsigned failures = 0;
    for (size_t i = 0; i < s.config.binding_count; ++i) {
        const xiaotai_preference_binding_t *b = &s.config.bindings[i];
        if (!b->snapshot) continue;
        if (edits.mask & SLOT(index)) {
            if (setting_listener) setting_listener(b->setting, result, edits.revision[index]);
            else if (listener) listener(b->setting, result, listener_ctx);
            if (result != XIAOTAI_OK) ++failures;
        }
        ++index;
    }

    enter();
    s.stats.write_failures += failures;
    group->active_edits.mask = 0;
    group->in_flight = false;
    atomic_fetch_sub(&s_pending, 1U);
    leave();
}

static void complete(xiaotai_err_t result, void *ctx)
{
    const uintptr_t job = (uintptr_t)ctx;
    if (job & SNAPSHOT_JOB) {
        complete_snapshot(result, true);
        return;
    }
    const slots_t finished = (slots_t)job;
    enter();
    const xiaotai_preferences_written_fn_t listener = s.setting_listener ? NULL : s.listener.unversioned;
    const pref_setting_written_fn setting_listener = s.setting_listener ? s.listener.versioned : NULL;
    void *const listener_ctx = s.listener_ctx;
    if (result != XIAOTAI_OK && s.first_failure == XIAOTAI_OK) s.first_failure = result;
    if (s.snapshot && result != XIAOTAI_OK && s.snapshot->first_other_failure == XIAOTAI_OK)
        s.snapshot->first_other_failure = result;
    for (unsigned i = 0; i < PREF_SLOTS; ++i) {
        if (!(finished & SLOT(i))) continue;
        const xiaotai_preference_binding_t *b = binding(s.changes[i].key);
        if (s.maintenance) {
            if (b) s.maintenance->errors[b - s.config.bindings] = result;
            else if (result != XIAOTAI_OK && s.maintenance->other_failure == XIAOTAI_OK)
                s.maintenance->other_failure = result;
        }
        if (!b || b->storage != XIAOTAI_PREFERENCE_FIELD) continue;
        record_t *r = record_for(b->record);
        if (r && r->committing) {
            r->durable_uncertain = result != XIAOTAI_OK;
            if (result == XIAOTAI_OK) {
                memcpy(r->raw, r->intended, r->layout->size);
                r->state = RECORD_VALID;
            }
            r->committing = false;
        }
    }
    leave();

    /* An in-flight slot remains reserved until its result callback returns. */
    for (unsigned i = 0; (listener || setting_listener) && i < PREF_SLOTS; ++i) {
        if (!(finished & SLOT(i))) continue;
        const change_t *change = &s.changes[i];
        if (setting_listener) setting_listener(change->key, result, change->revision);
        else listener(change->key, result, listener_ctx);
    }

    enter();
    for (unsigned i = 0; i < PREF_SLOTS; ++i) {
        if (!(finished & SLOT(i))) continue;
        if (result != XIAOTAI_OK) ++s.stats.write_failures;
        clear_slot(i);
    }
    s.in_flight &= ~finished;
    leave();
}

static void restore_batch(slots_t selected)
{
    for (unsigned i = 0; i < PREF_SLOTS; ++i) {
        if (!(selected & SLOT(i))) continue;
        bool superseded = false;
        for (unsigned j = 0; j < PREF_SLOTS; ++j) {
            if ((s.waiting & SLOT(j)) && !strcmp(s.changes[i].key, s.changes[j].key)) {
                superseded = true;
                break;
            }
        }
        if (superseded) { ++s.stats.coalesced; clear_slot(i); }
        else s.waiting |= SLOT(i);
    }
    s.in_flight &= ~selected;
}

/* An accepted job has no admission cutoff. If a submission failed, preserve
 * a racing newer edit and merge only unreplaced notification revisions. */
static bool retry_snapshot(xiaotai_err_t result, int64_t now)
{
    snapshot_t *group = s.snapshot;
    if (result != XIAOTAI_ERR_NO_MEM && result != XIAOTAI_ERR_TIMEOUT) return false;
    const bool newer = group->edits.mask != 0;
    const int64_t until = newer ? group->waiting_until : group->active_until;
    if (now >= until) return false;
    if (newer) atomic_fetch_sub(&s_pending, 1U);
    for (unsigned i = 0; i < XIAOTAI_KV_OPS_MAX; ++i) {
        if ((group->active_edits.mask & SLOT(i)) && !(group->edits.mask & SLOT(i)))
            group->edits.revision[i] = group->active_edits.revision[i];
    }
    group->edits.mask |= group->active_edits.mask;
    group->waiting_until = until;
    const int64_t paced = now + SNAPSHOT_RETRY_US;
    group->next_retry = paced < until ? paced : until;
    group->active_edits.mask = 0;
    group->in_flight = false;
    return true;
}

/* A flush may submit on another task, but cannot race the owner's commit
 * order. Not even the state lock is held across async admission. */
static xiaotai_err_t pump(bool force, int64_t deadline)
{
    if (atomic_exchange(&s_pumping, true)) return XIAOTAI_ERR_BUSY;
    xiaotai_err_t err = XIAOTAI_OK;
    for (unsigned groups = 0; groups < PREF_SLOTS; ++groups) {
        batch_t batch = {0};
        const int64_t now = xiaotai_time_us();
        if (force && now >= deadline) { err = XIAOTAI_ERR_TIMEOUT; break; }
        enter();
        bool due = s.snapshot && s.snapshot->edits.mask && !s.snapshot->in_flight &&
                   s.snapshot->next_retry <= now;
        if (due) {
            snapshot_batch(&batch);
        } else {
            for (unsigned i = 0; i < PREF_SLOTS; ++i) {
                if (!(s.waiting & SLOT(i)) || (!force && s.changes[i].due > now)) continue;
                due = true;
                (void)batch_add(&batch, i);
            }
        }
        const bool discarded = !batch.snapshot && discard_unchanged_records(&batch);
        s.waiting &= ~batch.selected;
        s.in_flight |= batch.selected;
        for (unsigned i = 0; i < PREF_RECORD_SLOTS; ++i) {
            if (!(batch.record_mask & SLOT(i))) continue;
            record_t *r = &s.records[i];
            if (r->has_intent) batch.prior_mask |= SLOT(i);
            memcpy(batch.prior_records[i], r->intended, r->layout->size);
            memcpy(r->intended, batch.records[i], r->layout->size);
            r->has_intent = r->committing = true;
        }
        leave();
        if (!batch.selected && !batch.snapshot) {
            if (discarded) continue;
            if (due) err = XIAOTAI_ERR_BUSY;
            break;
        }

        const uintptr_t job = batch.selected | (batch.snapshot ? SNAPSHOT_JOB : 0U);
        err = xiaotai_kv_commit_async(batch.space, batch.ops, batch.count, complete, (void *)job);
        bool snapshot_rejected = false;
        enter();
        if (err == XIAOTAI_OK) {
            s.stats.writes += batch.count;
        } else if (batch.snapshot) {
            if (retry_snapshot(err, xiaotai_time_us())) ++s.stats.deferred;
            else snapshot_rejected = true;
        } else {
            restore_batch(batch.selected);
            ++s.stats.deferred;
            for (unsigned i = 0; i < PREF_RECORD_SLOTS; ++i) {
                if (!(batch.record_mask & SLOT(i))) continue;
                record_t *r = &s.records[i];
                memcpy(r->intended, batch.prior_records[i], r->layout->size);
                r->has_intent = (batch.prior_mask & SLOT(i)) != 0;
                r->committing = false;
            }
        }
        leave();
        if (snapshot_rejected) complete_snapshot(err, false);
        if (err != XIAOTAI_OK) break;
    }
    atomic_store(&s_pumping, false);
    return err;
}

static void timer_fired(void *ctx)
{
    if ((unsigned)(uintptr_t)ctx != atomic_load(&s_epoch)) return;
    s_timer = 0;
    const xiaotai_err_t err = pump(false, 0);
    const uint32_t retry = err == XIAOTAI_ERR_BUSY || !delay_ms() ? PREF_FLUSH_POLL_MS : delay_ms();
    arm(err == XIAOTAI_OK ? 0 : xiaotai_time_us() + (int64_t)retry * 1000);
}

bool xiaotai_preferences_pending(void) { return atomic_load(&s_pending) != 0U; }

static xiaotai_err_t drain(uint32_t timeout_ms)
{
    const int64_t deadline = xiaotai_time_us() + (int64_t)timeout_ms * 1000;
    while (xiaotai_preferences_pending()) {
        if (xiaotai_time_us() >= deadline) return XIAOTAI_ERR_TIMEOUT;
        const xiaotai_err_t err = pump(true, deadline);
        if (err != XIAOTAI_OK && err != XIAOTAI_ERR_NO_MEM && err != XIAOTAI_ERR_BUSY && err != XIAOTAI_ERR_TIMEOUT)
            return err;
        const int64_t now = xiaotai_time_us();
        enter();
        const bool queued = s.waiting != 0;
        const bool snapshot_queued = s.snapshot && s.snapshot->edits.mask;
        const int64_t retry = !snapshot_queued ? 0 :
            s.snapshot->in_flight ? now + SNAPSHOT_RETRY_US : s.snapshot->next_retry;
        leave();
        const int64_t left = deadline - now;
        if (left <= 0 && xiaotai_preferences_pending()) return XIAOTAI_ERR_TIMEOUT;
        if (snapshot_queued && !queued && left > 0) {
            /* The forced barrier honors the same retry clock as the owner:
             * no busy loop, and no repeated attempts between retry points. */
            const int64_t until_retry = retry > now ? retry - now : 1000;
            const int64_t pause = until_retry < left ? until_retry : left;
            xiaotai_delay_ms((uint32_t)((pause + 999) / 1000));
        } else if (queued || err == XIAOTAI_ERR_BUSY) {
            if (left >= 1000) xiaotai_delay_ms(left >= PREF_FLUSH_POLL_MS * 1000U ? PREF_FLUSH_POLL_MS : 1U);
        } else {
            const xiaotai_err_t wait = xiaotai_kv_flush(left > 0 ? (uint32_t)(left / 1000) : 0);
            if (wait != XIAOTAI_OK) return wait;
        }
    }
    if (xiaotai_on_owner()) arm(0);
    return XIAOTAI_OK;
}

static xiaotai_err_t flush(uint32_t timeout_ms, bool maintenance)
{
    if (maintenance) {
        enter();
        const xiaotai_err_t cold = s.snapshot ? s.snapshot->cold_load_error :
                                   s.maintenance ? s.maintenance->cold_load_error : XIAOTAI_OK;
        leave();
        if (cold != XIAOTAI_OK) return cold;
    }
    const xiaotai_err_t drained = drain(timeout_ms);
    if (drained != XIAOTAI_OK) return drained;
    enter();
    xiaotai_err_t result = s.first_failure;
    if (maintenance && s.snapshot) {
        result = s.snapshot->error;
        if (result != XIAOTAI_OK) { leave(); return result; }
        result = s.snapshot->first_other_failure;
    } else if (maintenance && s.maintenance) {
        for (size_t i = 0; i < s.config.binding_count; ++i) {
            result = s.maintenance->errors[i];
            if (result != XIAOTAI_OK) { leave(); return result; }
        }
        result = s.maintenance->other_failure;
    }
    s.first_failure = XIAOTAI_OK;
    if (s.snapshot) s.snapshot->first_other_failure = XIAOTAI_OK;
    if (s.maintenance) s.maintenance->other_failure = XIAOTAI_OK;
    leave();
    return result;
}

xiaotai_err_t xiaotai_preferences_flush(uint32_t timeout_ms) { return flush(timeout_ms, false); }

xiaotai_err_t xiaotai_preferences_maintenance_flush(uint32_t timeout_ms) { return flush(timeout_ms, true); }

void xiaotai_preferences_stats(xiaotai_preferences_stats_t *stats)
{
    if (!stats) return;
    enter();
    *stats = s.stats;
    if (s.snapshot && s.snapshot->edits.mask) ++stats->pending;
    const slots_t waiting = s.waiting;
    leave();
    for (unsigned i = 0; i < PREF_SLOTS; ++i) if (waiting & SLOT(i)) ++stats->pending;
}

void xiaotai_preferences_reset(void)
{
    /* Host reboot fixtures stop the framework and drain KV before dropping
     * static state. An old timer cannot act on the next boot's configuration. */
    atomic_fetch_add(&s_epoch, 1U);
    xiaotai_timer_cancel(s_timer);
    s_timer = 0;
    enter();
    snapshot_t *image = NULL;
    maintenance_t *maintenance = NULL;
    if (!s.in_flight && !s.packed_retry && !(s.snapshot && s.snapshot->in_flight) && !atomic_load(&s_pumping)) {
        image = s.snapshot;
        maintenance = s.maintenance;
        memset(&s, 0, sizeof(s));
        atomic_store(&s_pending, 0U);
    }
    leave();
    xiaotai_mem_free(image);
    xiaotai_mem_free(maintenance);
}
