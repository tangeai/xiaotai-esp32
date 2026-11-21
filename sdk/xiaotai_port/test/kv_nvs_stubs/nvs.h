// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
typedef enum {NVS_TYPE_U8 = 1, NVS_TYPE_STR = 0x21, NVS_TYPE_BLOB = 0x42} nvs_type_t;
enum {ESP_OK = 0, NVS_READONLY = 0, NVS_READWRITE = 1};
enum {ESP_ERR_NVS_NOT_FOUND = 0x1102, ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_NO_FREE_PAGES,
      ESP_ERR_NVS_NEW_VERSION_FOUND, ESP_ERR_NVS_INVALID_LENGTH};

esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_find_key(nvs_handle_t, const char *, nvs_type_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);

#define NVS_SCALAR(name, type) \
    esp_err_t nvs_get_##name(nvs_handle_t, const char *, type *); \
    esp_err_t nvs_set_##name(nvs_handle_t, const char *, type);
NVS_SCALAR(u8, uint8_t)
NVS_SCALAR(i8, int8_t)
NVS_SCALAR(u16, uint16_t)
NVS_SCALAR(i16, int16_t)
NVS_SCALAR(u32, uint32_t)
NVS_SCALAR(i32, int32_t)
NVS_SCALAR(u64, uint64_t)
NVS_SCALAR(i64, int64_t)
#undef NVS_SCALAR
