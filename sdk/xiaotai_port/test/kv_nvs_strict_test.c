// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_environment_startup.py
#include <assert.h>
#include <string.h>
#include "kv_backend.h"
#include "nvs_flash.h"
#include "xiaotai/port/os.h"

static esp_err_t blob_err, str_err, find_err, init_err;
static esp_err_t commit_err;
static nvs_type_t entry_type;
static unsigned lookups, blob_reads, str_reads, inits, erases, opens, commits;
static const char *guarded_operation;
static xiaotai_err_t stack_result;

xiaotai_err_t xiaotai_require_internal_stack(const char *operation)
{
    guarded_operation = operation;
    return stack_result;
}

esp_err_t nvs_flash_init(void)
{
    ++inits;
    return init_err;
}

esp_err_t nvs_flash_erase(void)
{
    ++erases;
    return ESP_OK;
}

esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(space, "config") && (mode == NVS_READONLY || mode == NVS_READWRITE));
    ++opens;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++commits;
    return commit_err;
}

/* Sanitizer metadata may keep the native scalar wrappers in the link. */
#define UNUSED_SCALAR(name, type) \
    esp_err_t nvs_get_##name(nvs_handle_t h, const char *key, type *value) \
    { (void)h; (void)key; (void)value; assert(!"unexpected scalar read"); return XIAOTAI_FAIL; } \
    esp_err_t nvs_set_##name(nvs_handle_t h, const char *key, type value) \
    { (void)h; (void)key; (void)value; assert(!"unexpected scalar write"); return XIAOTAI_FAIL; }
UNUSED_SCALAR(u8, uint8_t)
UNUSED_SCALAR(i8, int8_t)
UNUSED_SCALAR(u16, uint16_t)
UNUSED_SCALAR(i16, int16_t)
UNUSED_SCALAR(u32, uint32_t)
UNUSED_SCALAR(i32, int32_t)
UNUSED_SCALAR(u64, uint64_t)
UNUSED_SCALAR(i64, int64_t)
#undef UNUSED_SCALAR

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *buffer, size_t *size)
{
    assert(handle == 1 && !strcmp(key, "record"));
    ++blob_reads;
    if (blob_err == ESP_OK) {
        assert(*size >= 2);
        memcpy(buffer, "ab", 2);
    }
    if (blob_err == ESP_OK || blob_err == ESP_ERR_NVS_INVALID_LENGTH) *size = 2;
    return blob_err;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *buffer, size_t *size)
{
    assert(handle == 1 && !strcmp(key, "record"));
    ++str_reads;
    if (str_err == ESP_OK) {
        assert(*size >= 2);
        memcpy(buffer, "a", 2);
    }
    if (str_err == ESP_OK || str_err == ESP_ERR_NVS_INVALID_LENGTH) *size = 2;
    return str_err;
}

esp_err_t nvs_find_key(nvs_handle_t handle, const char *key, nvs_type_t *type)
{
    assert(handle == 1 && !strcmp(key, "record"));
    ++lookups;
    if (find_err == ESP_OK) *type = entry_type;
    return find_err;
}

int main(void)
{
    char value[2] = {0};
    size_t size = sizeof(value);
    xiaotai_kv_handle_t handle = 0;
    stack_result = XIAOTAI_ERR_INVALID_STATE;
    assert(xiaotai_kv_backend_init() == stack_result && !inits);
    assert(!strcmp(guarded_operation, "nvs_flash_init"));
    assert(xiaotai_kv_backend_open("config", true, &handle) == stack_result && !opens && !handle);
    assert(!strcmp(guarded_operation, "nvs_open(write)"));
    assert(xiaotai_kv_backend_open("config", false, &handle) == XIAOTAI_OK && opens == 1 && handle == 1);
    assert(xiaotai_kv_backend_get_blob_strict(handle, "record", value, &size) == stack_result);
    assert(!strcmp(guarded_operation, "nvs_get_blob") && !blob_reads && !lookups);
    assert(size == sizeof(value) && !value[0] && !value[1]);
    assert(xiaotai_kv_backend_commit(handle) == stack_result && !commits);
    assert(!strcmp(guarded_operation, "nvs_commit"));

    stack_result = XIAOTAI_OK;
    assert(xiaotai_kv_backend_open("config", true, &handle) == XIAOTAI_OK && opens == 2 && handle == 1);
    commit_err = -2; /* The legacy S3 preference-save mock's commit error. */
    assert(xiaotai_kv_backend_commit(handle) == commit_err && commits == 1);
    assert(!strcmp(guarded_operation, "nvs_commit"));
    commit_err = ESP_OK;
    assert(xiaotai_kv_backend_commit(handle) == ESP_OK && commits == 2);
    static const esp_err_t boot_results[] = {
        ESP_ERR_NVS_NO_FREE_PAGES, ESP_ERR_NVS_NEW_VERSION_FOUND, XIAOTAI_FAIL, ESP_OK,
    };
    for (unsigned i = 0; i < sizeof(boot_results) / sizeof(*boot_results); ++i) {
        init_err = boot_results[i];
        assert(xiaotai_kv_backend_init() == init_err && inits == i + 1 && !erases);
    }
    assert(xiaotai_kv_backend_get_blob_strict(1, "record", value, &size) == XIAOTAI_OK && size == 2);
    assert(!memcmp(value, "ab", 2) && !lookups);
    blob_err = ESP_ERR_NVS_INVALID_LENGTH;
    size = 1;
    assert(xiaotai_kv_backend_get_blob_strict(1, "record", value, &size) == XIAOTAI_ERR_INVALID_SIZE && size == 2);
    blob_err = ESP_ERR_NVS_TYPE_MISMATCH;
    assert(xiaotai_kv_backend_get_blob_strict(1, "record", value, &size) == XIAOTAI_ERR_INVALID_ARG && !lookups);
    assert(xiaotai_kv_backend_get(1, "record", false, value, &size) == XIAOTAI_ERR_NOT_FOUND && !lookups);
    blob_err = ESP_ERR_NVS_NOT_FOUND;
    entry_type = NVS_TYPE_U8;
    assert(xiaotai_kv_backend_get_blob_strict(1, "record", value, &size) == XIAOTAI_ERR_INVALID_ARG && lookups == 1);
    entry_type = NVS_TYPE_BLOB;
    assert(xiaotai_kv_backend_get_blob_strict(1, "record", value, &size) == XIAOTAI_FAIL && lookups == 2);
    find_err = ESP_ERR_NVS_NOT_FOUND;
    assert(xiaotai_kv_backend_get_blob_strict(1, "record", value, &size) == XIAOTAI_ERR_NOT_FOUND && lookups == 3);
    find_err = XIAOTAI_ERR_TIMEOUT;
    assert(xiaotai_kv_backend_get_blob_strict(1, "record", value, &size) == XIAOTAI_ERR_TIMEOUT && lookups == 4);
    size = sizeof(value);
    stack_result = XIAOTAI_ERR_INVALID_STATE;
    assert(xiaotai_kv_backend_get_str_strict(1, "record", value, &size) == stack_result && !str_reads);
    assert(!strcmp(guarded_operation, "nvs_get_str"));
    stack_result = XIAOTAI_OK;
    assert(xiaotai_kv_backend_get_str_strict(1, "record", value, &size) == XIAOTAI_OK && !strcmp(value, "a"));
    str_err = ESP_ERR_NVS_TYPE_MISMATCH;
    assert(xiaotai_kv_backend_get_str_strict(1, "record", value, &size) == XIAOTAI_ERR_INVALID_ARG && lookups == 4);
    assert(xiaotai_kv_backend_get(1, "record", true, value, &size) == XIAOTAI_ERR_NOT_FOUND && lookups == 4);
    str_err = ESP_ERR_NVS_NOT_FOUND;
    find_err = ESP_OK;
    entry_type = NVS_TYPE_U8;
    assert(xiaotai_kv_backend_get_str_strict(1, "record", value, &size) == XIAOTAI_ERR_INVALID_ARG && lookups == 5);
    entry_type = NVS_TYPE_STR;
    assert(xiaotai_kv_backend_get_str_strict(1, "record", value, &size) == XIAOTAI_FAIL && lookups == 6);
    find_err = ESP_ERR_NVS_NOT_FOUND;
    assert(xiaotai_kv_backend_get_str_strict(1, "record", value, &size) == XIAOTAI_ERR_NOT_FOUND && lookups == 7);
    str_err = ESP_ERR_NVS_INVALID_LENGTH;
    size = 1;
    assert(xiaotai_kv_backend_get_str_strict(1, "record", value, &size) == XIAOTAI_ERR_INVALID_SIZE && size == 2);
    return 0;
}
