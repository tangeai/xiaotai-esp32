// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/preferences.h"

#define PREF_SPACE_DEFAULT "xt_settings"
#define PREF_READ_TIMEOUT_MS 1000U
#define PREF_RECORDS_MAX 4U
#define PREF_INT_BLOB_SIZE 4U

typedef void (*pref_setting_written_fn)(const char *key, xiaotai_err_t result, uint32_t revision);
xiaotai_err_t pref_setting_listener(pref_setting_written_fn listener);
xiaotai_err_t pref_store_setting_int(const char *setting, int32_t value, uint32_t revision);
xiaotai_err_t pref_store_setting_text(const char *setting, const char *value, uint32_t revision);

bool pref_name_valid(const char *name);
bool pref_bindings_valid(const xiaotai_preferences_config_t *config);
bool pref_same_record(const xiaotai_preference_record_t *a, const xiaotai_preference_record_t *b);

void pref_int_to_blob(int32_t value, uint8_t bytes[PREF_INT_BLOB_SIZE]);
bool pref_value_to_raw(const xiaotai_preference_binding_t *binding, int32_t value, int64_t *raw);

/* Corrupt values and records report NOT_FOUND with invalid set. Storage errors
 * are distinct from absence: in particular a packed record may not be written
 * before its load succeeds or establishes that the old entry is absent. */
xiaotai_err_t pref_load_plain_int(const char *setting, const xiaotai_preference_binding_t *binding,
                                 int32_t *value, bool native_snapshot, bool *invalid, xiaotai_kv_read_stage_t *stage);
xiaotai_err_t pref_load_text(const char *setting, const xiaotai_preference_binding_t *binding,
                            char *out, size_t capacity, bool native_snapshot, bool *invalid,
                            xiaotai_kv_read_stage_t *stage);
xiaotai_err_t pref_load_setting_int(const char *setting, int32_t fallback, int32_t *value);
xiaotai_err_t pref_load_setting_text(const char *setting, char *out, size_t capacity);
xiaotai_err_t pref_load_record(const xiaotai_preferences_config_t *config,
                              const xiaotai_preference_record_t *record, uint8_t *bytes, bool *invalid);

/* Only call decode with a record already validated by pref_load_record. Encode
 * sets this field's byte in a fresh image for the configured write version. */
int32_t pref_decode_field(const xiaotai_preference_binding_t *binding, const uint8_t *bytes);
bool pref_encode_field(const xiaotai_preference_binding_t *binding, int32_t value, uint8_t *bytes);
bool pref_copy_field(const xiaotai_preference_binding_t *binding, const uint8_t *old_bytes, uint8_t *new_bytes);
