// SPDX-License-Identifier: MIT
/* KV backend on ESP-IDF NVS. Runs only on the KV worker task. Initialization
 * never erases: incompatible NVS needs explicit recovery, so identity and
 * settings survive firmware updates.
 *
 * Job data may sit in external RAM: it is handed to NVS directly. The flash
 * layer bounces buffers that are not in internal RAM through an internal one
 * and NVS keeps its page cache internal; the worker's stack is internal
 * (xiaotai_port.md 4.4, storage item 4, PQ8; confirmed on the board before
 * the switch). */
#include "kv_backend.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "xiaotai/port/os.h"

static const char *TAG = "xiaotai_kv";

/* Absent, or present with another type. */
static xiaotai_err_t read_result(esp_err_t err)
{
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH) return XIAOTAI_ERR_NOT_FOUND;
    /* Value larger than the buffer: NVS has set *size to the stored length. */
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return XIAOTAI_ERR_INVALID_SIZE;
    return err;
}

bool xiaotai_kv_backend_recovery_error(xiaotai_err_t error)
{
    return error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND;
}

xiaotai_err_t xiaotai_kv_backend_init(void)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_flash_init");
    if (stack != XIAOTAI_OK) return stack;
    const esp_err_t err = nvs_flash_init();
    if (xiaotai_kv_backend_recovery_error(err)) {
        ESP_LOGE(TAG, "NVS needs recovery: %s; existing data was not erased", esp_err_to_name(err));
    }
    return err;
}

xiaotai_err_t xiaotai_kv_backend_open(const char *space, bool write, xiaotai_kv_handle_t *handle)
{
    if (write) {
        const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_open(write)");
        if (stack != XIAOTAI_OK) return stack;
    }
    nvs_handle_t nvs = 0;
    const esp_err_t err = nvs_open(space, write ? NVS_READWRITE : NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return XIAOTAI_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    *handle = (xiaotai_kv_handle_t)nvs;
    return XIAOTAI_OK;
}

void xiaotai_kv_backend_close(xiaotai_kv_handle_t handle) { nvs_close((nvs_handle_t)handle); }

xiaotai_err_t xiaotai_kv_backend_get(xiaotai_kv_handle_t handle, const char *key, bool text, void *data,
                                     size_t *size)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack(text ? "nvs_get_str" : "nvs_get_blob");
    if (stack != XIAOTAI_OK) return stack;
    const nvs_handle_t nvs = (nvs_handle_t)handle;
    return read_result(text ? nvs_get_str(nvs, key, data, size) : nvs_get_blob(nvs, key, data, size));
}

static xiaotai_err_t get_strict(xiaotai_kv_handle_t handle, const char *key, bool text, void *data, size_t *size)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack(text ? "nvs_get_str" : "nvs_get_blob");
    if (stack != XIAOTAI_OK) return stack;
    const nvs_handle_t nvs = (nvs_handle_t)handle;
    esp_err_t err = text ? nvs_get_str(nvs, key, data, size) : nvs_get_blob(nvs, key, data, size);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) return XIAOTAI_ERR_INVALID_ARG;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* A typed lookup can miss an existing key stored as a different type. */
        nvs_type_t type;
        err = nvs_find_key(nvs, key, &type);
        if (err == ESP_OK) return type == (text ? NVS_TYPE_STR : NVS_TYPE_BLOB) ? XIAOTAI_FAIL : XIAOTAI_ERR_INVALID_ARG;
    }
    return read_result(err);
}

xiaotai_err_t xiaotai_kv_backend_get_blob_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                 size_t *size)
{
    return get_strict(handle, key, false, data, size);
}

xiaotai_err_t xiaotai_kv_backend_get_str_strict(xiaotai_kv_handle_t handle, const char *key, void *data,
                                                size_t *size)
{
    return get_strict(handle, key, true, data, size);
}

xiaotai_err_t xiaotai_kv_backend_get_int(xiaotai_kv_handle_t handle, const char *key, xiaotai_kv_int_t type,
                                         int64_t *value)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_get_int");
    if (stack != XIAOTAI_OK) return stack;
    const nvs_handle_t nvs = (nvs_handle_t)handle;
    esp_err_t err;
    switch (type) {
    case XIAOTAI_KV_U8: {
        uint8_t v = 0;
        err = nvs_get_u8(nvs, key, &v);
        *value = v;
        break;
    }
    case XIAOTAI_KV_I8: {
        int8_t v = 0;
        err = nvs_get_i8(nvs, key, &v);
        *value = v;
        break;
    }
    case XIAOTAI_KV_U16: {
        uint16_t v = 0;
        err = nvs_get_u16(nvs, key, &v);
        *value = v;
        break;
    }
    case XIAOTAI_KV_I16: {
        int16_t v = 0;
        err = nvs_get_i16(nvs, key, &v);
        *value = v;
        break;
    }
    case XIAOTAI_KV_U32: {
        uint32_t v = 0;
        err = nvs_get_u32(nvs, key, &v);
        *value = v;
        break;
    }
    case XIAOTAI_KV_I32: {
        int32_t v = 0;
        err = nvs_get_i32(nvs, key, &v);
        *value = v;
        break;
    }
    case XIAOTAI_KV_U64: {
        uint64_t v = 0;
        err = nvs_get_u64(nvs, key, &v);
        *value = (int64_t)v; /* the same 64 bits (kv.h) */
        break;
    }
    case XIAOTAI_KV_I64: {
        int64_t v = 0;
        err = nvs_get_i64(nvs, key, &v);
        *value = v;
        break;
    }
    default:
        return XIAOTAI_ERR_INVALID_ARG;
    }
    return read_result(err);
}

xiaotai_err_t xiaotai_kv_backend_set(xiaotai_kv_handle_t handle, const char *key, bool text, const void *data,
                                     size_t size)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack(text ? "nvs_set_str" : "nvs_set_blob");
    if (stack != XIAOTAI_OK) return stack;
    const nvs_handle_t nvs = (nvs_handle_t)handle;
    if (text) {
        /* Stored as an NVS string, as earlier firmware did. */
        if (!size || ((const char *)data)[size - 1] != '\0') return XIAOTAI_ERR_INVALID_ARG;
        return nvs_set_str(nvs, key, data);
    }
    return nvs_set_blob(nvs, key, data, size);
}

xiaotai_err_t xiaotai_kv_backend_set_int(xiaotai_kv_handle_t handle, const char *key, xiaotai_kv_int_t type,
                                         int64_t value)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_set_int");
    if (stack != XIAOTAI_OK) return stack;
    const nvs_handle_t nvs = (nvs_handle_t)handle;
    switch (type) {
    case XIAOTAI_KV_U8: return nvs_set_u8(nvs, key, (uint8_t)value);
    case XIAOTAI_KV_I8: return nvs_set_i8(nvs, key, (int8_t)value);
    case XIAOTAI_KV_U16: return nvs_set_u16(nvs, key, (uint16_t)value);
    case XIAOTAI_KV_I16: return nvs_set_i16(nvs, key, (int16_t)value);
    case XIAOTAI_KV_U32: return nvs_set_u32(nvs, key, (uint32_t)value);
    case XIAOTAI_KV_I32: return nvs_set_i32(nvs, key, (int32_t)value);
    case XIAOTAI_KV_U64: return nvs_set_u64(nvs, key, (uint64_t)value);
    case XIAOTAI_KV_I64: return nvs_set_i64(nvs, key, value);
    default: return XIAOTAI_ERR_INVALID_ARG;
    }
}

xiaotai_err_t xiaotai_kv_backend_erase(xiaotai_kv_handle_t handle, const char *key)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_erase_key");
    if (stack != XIAOTAI_OK) return stack;
    const esp_err_t err = nvs_erase_key((nvs_handle_t)handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? XIAOTAI_OK : err;
}

xiaotai_err_t xiaotai_kv_backend_erase_all(xiaotai_kv_handle_t handle)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_erase_all");
    return stack != XIAOTAI_OK ? stack : nvs_erase_all((nvs_handle_t)handle);
}

xiaotai_err_t xiaotai_kv_backend_commit(xiaotai_kv_handle_t handle)
{
    const xiaotai_err_t stack = xiaotai_require_internal_stack("nvs_commit");
    return stack != XIAOTAI_OK ? stack : nvs_commit((nvs_handle_t)handle);
}
