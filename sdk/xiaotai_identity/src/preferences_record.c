// SPDX-License-Identifier: MIT
#include "preferences_record.h"

#include <string.h>

#define PREF_NATIVE_READ_TIMEOUT_MS 5000U

bool pref_name_valid(const char *name)
{
    return name && name[0] && strlen(name) <= XIAOTAI_KV_NAME_MAX;
}

static bool same_entry(const char *space, const char *key, const char *other_space, const char *other_key)
{
    return strcmp(space, other_space) == 0 && strcmp(key, other_key) == 0;
}

bool pref_same_record(const xiaotai_preference_record_t *a, const xiaotai_preference_record_t *b)
{
    return a && b && (a == b || same_entry(a->space, a->key, b->space, b->key));
}

static bool same_layout(const xiaotai_preference_record_t *a, const xiaotai_preference_record_t *b)
{
    return a->size == b->size && a->version_offset == b->version_offset &&
           a->elide_unchanged == b->elide_unchanged &&
           a->debounce_as_record == b->debounce_as_record &&
           (a->version_offset == XIAOTAI_PREFERENCE_NO_VERSION ||
            (a->version_min == b->version_min && a->version_max == b->version_max &&
             a->version_write == b->version_write));
}

static bool valid_int_range(xiaotai_kv_int_t type, int64_t min, int64_t max)
{
    switch (type) {
    case XIAOTAI_KV_U8:  return min >= 0 && max <= UINT8_MAX;
    case XIAOTAI_KV_I8:  return min >= INT8_MIN && max <= INT8_MAX;
    case XIAOTAI_KV_U16: return min >= 0 && max <= UINT16_MAX;
    case XIAOTAI_KV_I16: return min >= INT16_MIN && max <= INT16_MAX;
    case XIAOTAI_KV_U32: return min >= 0 && max <= UINT32_MAX;
    case XIAOTAI_KV_I32: return min >= INT32_MIN && max <= INT32_MAX;
    case XIAOTAI_KV_U64: return min >= 0; /* The public binding range ends at INT64_MAX. */
    case XIAOTAI_KV_I64: return true;
    default:            return false;
    }
}

static bool valid_field(const xiaotai_preference_binding_t *binding)
{
    const xiaotai_preference_record_t *r = binding->record;
    if (binding->space || binding->key || !r || !pref_name_valid(r->space) ||
        !pref_name_valid(r->key) || r->size == 0 || r->size > XIAOTAI_PREFERENCE_RECORD_MAX ||
        binding->offset >= r->size || binding->raw_min < 0 || binding->raw_max > UINT8_MAX) {
        return false;
    }
    if (r->version_offset == XIAOTAI_PREFERENCE_NO_VERSION) return binding->since_version == 0;
    if (r->version_offset >= r->size || r->version_offset == binding->offset ||
        r->version_min > r->version_write || r->version_write > r->version_max ||
        binding->since_version > r->version_write) return false;
    return binding->since_version <= r->version_min || binding->values ||
           (binding->before_value >= binding->raw_min && binding->before_value <= binding->raw_max);
}

static bool valid_binding(const xiaotai_preference_binding_t *binding)
{
    if (!pref_name_valid(binding->setting)) return false;
    switch (binding->storage) {
    case XIAOTAI_PREFERENCE_FIELD:
        if (!valid_field(binding)) return false;
        break;
    case XIAOTAI_PREFERENCE_INT:
    case XIAOTAI_PREFERENCE_STRING:
        if (!pref_name_valid(binding->space) || !pref_name_valid(binding->key) ||
            binding->record || binding->offset != 0 || binding->since_version != 0) return false;
        if (binding->storage == XIAOTAI_PREFERENCE_STRING) return !binding->values && binding->value_count == 0;
        if (!valid_int_range(binding->int_type, binding->raw_min, binding->raw_max)) return false;
        break;
    default:
        return false;
    }
    if (binding->raw_min > binding->raw_max) return false;
    if (!binding->values) {
        return binding->value_count == 0 && binding->raw_min >= INT32_MIN && binding->raw_max <= INT32_MAX;
    }
    return binding->value_count != 0 && binding->raw_min == 0 &&
           (uintmax_t)binding->raw_max == (uintmax_t)binding->value_count - 1U;
}

static bool bindings_share_entry(const xiaotai_preference_binding_t *a, const xiaotai_preference_binding_t *b)
{
    return same_entry(a->record ? a->record->space : a->space, a->record ? a->record->key : a->key,
                      b->record ? b->record->space : b->space, b->record ? b->record->key : b->key);
}

bool pref_bindings_valid(const xiaotai_preferences_config_t *config)
{
    if (!config) return true;
    if ((config->bindings != NULL) != (config->binding_count != 0)) return false;
    if (config->retain_maintenance_failures && !config->binding_count) return false;
    size_t records = 0;
    size_t snapshot_count = 0;
    const char *snapshot_space = NULL;
    bool snapshot_has_text = false;
    for (size_t i = 0; i < config->binding_count; ++i) {
        const xiaotai_preference_binding_t *b = &config->bindings[i];
        if (!valid_binding(b)) return false;
        if (config->retain_maintenance_failures && (b->snapshot || b->storage == XIAOTAI_PREFERENCE_FIELD))
            return false;
        if (b->snapshot) {
            if (b->storage == XIAOTAI_PREFERENCE_FIELD || ++snapshot_count > XIAOTAI_KV_OPS_MAX ||
                (snapshot_space && strcmp(snapshot_space, b->space))) return false;
            snapshot_space = b->space;
            if (b->storage == XIAOTAI_PREFERENCE_STRING) {
                if (snapshot_has_text) return false;
                snapshot_has_text = true;
            }
        }
        bool new_record = b->record != NULL;
        for (size_t j = 0; j < i; ++j) {
            const xiaotai_preference_binding_t *other = &config->bindings[j];
            if (strcmp(b->setting, other->setting) == 0) return false;
            if (!bindings_share_entry(b, other)) continue;
            if (!b->record || !other->record || !same_layout(b->record, other->record) ||
                b->offset == other->offset) return false;
            new_record = false;
        }
        if (new_record && ++records > PREF_RECORDS_MAX) return false;
    }
    return true;
}

static int32_t raw_to_value(const xiaotai_preference_binding_t *binding, int64_t raw)
{
    return binding->values ? binding->values[(size_t)raw] : (int32_t)raw;
}

static bool raw_in_range(const xiaotai_preference_binding_t *binding, int64_t raw)
{
    return raw >= binding->raw_min && raw <= binding->raw_max;
}

bool pref_value_to_raw(const xiaotai_preference_binding_t *binding, int32_t value, int64_t *raw)
{
    if (!binding->values) {
        if (!raw_in_range(binding, value)) return false;
        *raw = value;
        return true;
    }
    size_t nearest = 0;
    int64_t distance = INT64_MAX;
    for (size_t i = 0; i < binding->value_count; ++i) {
        int64_t d = (int64_t)value - binding->values[i];
        if (d < 0) d = -d;
        if (d < distance) {
            nearest = i;
            distance = d;
            if (d == 0) break;
        }
    }
    *raw = (int64_t)nearest;
    return true;
}

void pref_int_to_blob(int32_t value, uint8_t bytes[PREF_INT_BLOB_SIZE])
{
    const uint32_t bits = (uint32_t)value;
    for (unsigned i = 0; i < PREF_INT_BLOB_SIZE; ++i) bytes[i] = (uint8_t)(bits >> (8U * i));
}

static int32_t int_from_blob(const uint8_t bytes[PREF_INT_BLOB_SIZE])
{
    uint32_t bits = 0;
    for (unsigned i = 0; i < PREF_INT_BLOB_SIZE; ++i) bits |= (uint32_t)bytes[i] << (8U * i);
    return bits <= INT32_MAX ? (int32_t)bits : -1 - (int32_t)(UINT32_MAX - bits);
}

static xiaotai_err_t malformed(bool *invalid)
{
    *invalid = true;
    return XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t pref_load_plain_int(const char *setting, const xiaotai_preference_binding_t *binding,
                                 int32_t *value, bool native_snapshot, bool *invalid, xiaotai_kv_read_stage_t *stage)
{
    *invalid = false;
    if (binding) {
        if (binding->storage != XIAOTAI_PREFERENCE_INT) return XIAOTAI_ERR_INVALID_ARG;
        int64_t raw;
        const uint32_t timeout = native_snapshot ? PREF_NATIVE_READ_TIMEOUT_MS : PREF_READ_TIMEOUT_MS;
        /* The deployed typed NVS lookup reports a same-name wrong type as missing. */
        const xiaotai_err_t err =
            xiaotai_kv_get_int_with_stage(binding->space, binding->key, binding->int_type, &raw, stage, timeout);
        if (err != XIAOTAI_OK) return err;
        if (!raw_in_range(binding, raw)) {
            if (!native_snapshot) return malformed(invalid);
            *invalid = true;
            return XIAOTAI_ERR_INVALID_RESPONSE;
        }
        *value = raw_to_value(binding, raw);
    } else {
        uint8_t bytes[PREF_INT_BLOB_SIZE];
        size_t size = sizeof(bytes);
        const xiaotai_err_t err = xiaotai_kv_get(PREF_SPACE_DEFAULT, setting, bytes, &size, PREF_READ_TIMEOUT_MS);
        if (err == XIAOTAI_ERR_INVALID_SIZE) return malformed(invalid);
        if (err != XIAOTAI_OK) return err;
        if (size != sizeof(bytes)) return malformed(invalid);
        *value = int_from_blob(bytes);
    }
    return XIAOTAI_OK;
}

xiaotai_err_t pref_load_text(const char *setting, const xiaotai_preference_binding_t *binding,
                            char *out, size_t capacity, bool native_snapshot, bool *invalid,
                            xiaotai_kv_read_stage_t *stage)
{
    *invalid = false;
    char text[XIAOTAI_PREFERENCE_TEXT_MAX];
    xiaotai_err_t err;
    if (binding) {
        if (binding->storage != XIAOTAI_PREFERENCE_STRING) return XIAOTAI_ERR_INVALID_ARG;
        const uint32_t timeout = native_snapshot ? PREF_NATIVE_READ_TIMEOUT_MS : PREF_READ_TIMEOUT_MS;
        err = xiaotai_kv_get_str_with_stage(binding->space, binding->key, text, sizeof(text), stage, timeout);
        if (err == XIAOTAI_FAIL && !native_snapshot) return malformed(invalid); /* Missing terminator. */
    } else {
        size_t size = sizeof(text);
        err = xiaotai_kv_get(PREF_SPACE_DEFAULT, setting, text, &size, PREF_READ_TIMEOUT_MS);
        if (err == XIAOTAI_OK && (size == 0 || text[size - 1] != '\0')) return malformed(invalid);
    }
    if (err == XIAOTAI_ERR_INVALID_SIZE) {
        if (!native_snapshot) return malformed(invalid);
        *invalid = true;
    }
    if (err != XIAOTAI_OK) return err;
    if (text[0] == '\0' && !native_snapshot) return malformed(invalid);
    const size_t size = strlen(text) + 1;
    if (size > capacity) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(out, text, size);
    return XIAOTAI_OK;
}

static bool before_field_version(const xiaotai_preference_binding_t *binding, const uint8_t *bytes)
{
    return binding->since_version != 0 && bytes[binding->record->version_offset] < binding->since_version;
}

xiaotai_err_t pref_load_record(const xiaotai_preferences_config_t *config,
                              const xiaotai_preference_record_t *record, uint8_t *bytes, bool *invalid)
{
    *invalid = false;
    size_t size = record->size;
    const xiaotai_err_t err = xiaotai_kv_get_blob_strict(record->space, record->key, bytes, &size,
                                                       PREF_READ_TIMEOUT_MS);
    if (err == XIAOTAI_ERR_INVALID_SIZE || err == XIAOTAI_ERR_INVALID_ARG) return malformed(invalid);
    if (err != XIAOTAI_OK) return err;
    if (size != record->size) return malformed(invalid);
    if (record->version_offset != XIAOTAI_PREFERENCE_NO_VERSION) {
        const uint8_t version = bytes[record->version_offset];
        if (version < record->version_min || version > record->version_max) return malformed(invalid);
    }
    for (size_t i = 0; i < config->binding_count; ++i) {
        const xiaotai_preference_binding_t *binding = &config->bindings[i];
        if (!pref_same_record(record, binding->record)) continue;
        const uint8_t raw = bytes[binding->offset];
        if (before_field_version(binding, bytes) ? raw != binding->before_raw : !raw_in_range(binding, raw)) {
            return malformed(invalid);
        }
    }
    return XIAOTAI_OK;
}

int32_t pref_decode_field(const xiaotai_preference_binding_t *binding, const uint8_t *bytes)
{
    return before_field_version(binding, bytes) ? binding->before_value : raw_to_value(binding, bytes[binding->offset]);
}

bool pref_encode_field(const xiaotai_preference_binding_t *binding, int32_t value, uint8_t *bytes)
{
    int64_t raw;
    if (!pref_value_to_raw(binding, value, &raw)) return false;
    bytes[binding->offset] = (uint8_t)raw;
    return true;
}

bool pref_copy_field(const xiaotai_preference_binding_t *binding, const uint8_t *old_bytes, uint8_t *new_bytes)
{
    if (before_field_version(binding, old_bytes)) {
        return pref_encode_field(binding, binding->before_value, new_bytes);
    }
    /* Preserve untouched indices even when several map to the same value. */
    new_bytes[binding->offset] = old_bytes[binding->offset];
    return true;
}
