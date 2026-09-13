#include "ota.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#include "app_task_affinity.h"

static const char *TAG = "ota";

#define OTA_DOWNLOAD_BUF_SIZE      4096
/* OTA is user-started, but HTTP I/O still needs a bound when Wi-Fi or server stalls. */
#define OTA_HTTP_IO_TIMEOUT_MS     (5 * 1000)
#define OTA_MANIFEST_TIMEOUT_MS    (30 * 1000)
#define OTA_DOWNLOAD_STALL_MS      (30 * 1000)
#define OTA_DOWNLOAD_LOG_STEP      (64 * 1024)
#define OTA_DOWNLOAD_YIELD_TICKS   1
#define OTA_MANIFEST_MAX_SIZE      2048
#define OTA_SHA256_BIN_SIZE        32
#define OTA_SHA256_HEX_SIZE        65
/* TLS, JSON parsing, SHA256, and flash writes run outside UI/network callbacks. */
#define OTA_TASK_STACK_SIZE        (12 * 1024)
#define OTA_TASK_PRIORITY          3
#define OTA_TASK_ALLOC_CAPS        APP_TASK_STACK_CAPS_INTERNAL

typedef struct {
    bool update;
    bool force;
    bool compatible;
    char version[OTA_VERSION_MAX];
    char target_chip[OTA_CHIP_MAX];
    char reason[OTA_REASON_MAX];
    char firmware_url[OTA_URL_MAX];
    size_t firmware_size;
    char firmware_sha256[OTA_SHA256_HEX_SIZE];
} ota_manifest_t;

typedef struct {
    char base_url[OTA_URL_MAX];
    char device_id[OTA_DEVICE_ID_MAX];
    char chip[OTA_CHIP_MAX];
} ota_task_ctx_t;

static void *ota_malloc_psram(size_t size)
{
    return app_memory_alloc_psram(size);
}

static void *ota_malloc_internal(size_t size)
{
    return heap_caps_malloc(size, APP_MEMORY_CAPS_CONTROL);
}

static void *ota_calloc_internal(size_t count, size_t size)
{
    return heap_caps_calloc(count, size, APP_MEMORY_CAPS_CONTROL);
}

static int64_t ota_deadline_from_now_us(uint32_t timeout_ms)
{
    return esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
}

static bool ota_deadline_expired(int64_t deadline_us)
{
    return esp_timer_get_time() >= deadline_us;
}

static ota_snapshot_t s_ota_snapshot;
static TaskHandle_t s_ota_task;
static ota_config_t s_ota_config = {
    .default_url = "",
};
static portMUX_TYPE s_ota_lock = portMUX_INITIALIZER_UNLOCKED;

const char *ota_default_url(void)
{
    return s_ota_config.default_url;
}

static const char *ota_device_chip(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    return "p4";
#else
    return CONFIG_IDF_TARGET;
#endif
}

static bool ota_snprintf_ok(int ret, size_t buffer_size)
{
    return ret >= 0 && (size_t)ret < buffer_size;
}

static void ota_fill_current_version(char *version, size_t version_size)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    if (version == NULL || version_size == 0) {
        return;
    }

    if (app_desc != NULL && app_desc->version[0] != '\0') {
        strlcpy(version, app_desc->version, version_size);
    } else {
        strlcpy(version, "unknown", version_size);
    }
}

static void ota_fill_version_locked(void)
{
    ota_fill_current_version(s_ota_snapshot.current_version,
                                      sizeof(s_ota_snapshot.current_version));
}

static void ota_set_url(const char *url)
{
    if (url == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_ota_lock);
    strlcpy(s_ota_snapshot.url, url, sizeof(s_ota_snapshot.url));
    taskEXIT_CRITICAL(&s_ota_lock);
}

static void ota_set_status(ota_state_t state,
                                    bool running,
                                    esp_err_t err,
                                    const char *message)
{
    taskENTER_CRITICAL(&s_ota_lock);
    s_ota_snapshot.state = state;
    s_ota_snapshot.running = running;
    s_ota_snapshot.last_error = err;
    if (message != NULL) {
        strlcpy(s_ota_snapshot.message, message, sizeof(s_ota_snapshot.message));
    }
    ota_fill_version_locked();
    taskEXIT_CRITICAL(&s_ota_lock);
}

static void ota_set_target_version(const char *version)
{
    taskENTER_CRITICAL(&s_ota_lock);
    if (version != NULL) {
        strlcpy(s_ota_snapshot.target_version,
                version,
                sizeof(s_ota_snapshot.target_version));
    } else {
        s_ota_snapshot.target_version[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_ota_lock);
}

static uint8_t ota_progress_percent(size_t bytes_read, size_t total_size)
{
    if (total_size == 0) {
        return 0;
    }

    size_t percent = (bytes_read * 100U) / total_size;
    return percent > 100U ? 100U : (uint8_t)percent;
}

static void ota_set_progress(size_t bytes_read, size_t total_size, const char *message)
{
    taskENTER_CRITICAL(&s_ota_lock);
    s_ota_snapshot.bytes_read = bytes_read;
    s_ota_snapshot.total_size = total_size;
    s_ota_snapshot.progress_percent = ota_progress_percent(bytes_read, total_size);
    if (message != NULL) {
        strlcpy(s_ota_snapshot.message, message, sizeof(s_ota_snapshot.message));
    }
    taskEXIT_CRITICAL(&s_ota_lock);
}

static void ota_clear_task_handle(void)
{
    taskENTER_CRITICAL(&s_ota_lock);
    s_ota_task = NULL;
    taskEXIT_CRITICAL(&s_ota_lock);
}

static bool ota_is_url_unreserved(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static esp_err_t ota_url_encode(const char *input, char *output, size_t output_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t out_index = 0;

    if (input == NULL || output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0; input[index] != '\0'; ++index) {
        unsigned char ch = (unsigned char)input[index];
        if (ota_is_url_unreserved((char)ch)) {
            if (out_index + 1 >= output_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            output[out_index++] = (char)ch;
        } else {
            if (out_index + 3 >= output_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            output[out_index++] = '%';
            output[out_index++] = hex[(ch >> 4) & 0x0F];
            output[out_index++] = hex[ch & 0x0F];
        }
    }

    output[out_index] = '\0';
    return ESP_OK;
}

static void ota_trim_url_suffix(char *url, const char *suffix)
{
    size_t url_len = 0;
    size_t suffix_len = 0;

    if (url == NULL || suffix == NULL) {
        return;
    }

    url_len = strlen(url);
    suffix_len = strlen(suffix);
    if (url_len >= suffix_len && strcmp(&url[url_len - suffix_len], suffix) == 0) {
        url[url_len - suffix_len] = '\0';
    }
}

static esp_err_t ota_normalize_base_url(const char *input, char *output, size_t output_size)
{
    size_t len = 0;

    if (input == NULL || output == NULL || output_size == 0 || input[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(output, input, output_size);
    if (strlen(input) >= output_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    len = strlen(output);
    while (len > 0 && output[len - 1] == '/') {
        output[len - 1] = '\0';
        len--;
    }

    ota_trim_url_suffix(output, "/sample_project.bin");
    ota_trim_url_suffix(output, "/firmware/sample_project.bin");
    ota_trim_url_suffix(output, "/api/ota/manifest");
    ota_trim_url_suffix(output, "/manifest.json");

    if (output[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t ota_http_get_text(const char *url,
                                            char *buffer,
                                            size_t buffer_size,
                                            int *status_code)
{
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t client = NULL;
    size_t bytes_read = 0;
    int64_t deadline_us = 0;

    if (url == NULL || buffer == NULL || buffer_size < 2 || status_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_IO_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "manifest request begin");
    deadline_us = ota_deadline_from_now_us(OTA_MANIFEST_TIMEOUT_MS);

    ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "manifest open failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    *status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,
             "manifest response: status=%d length=%lld",
             *status_code,
             (long long)content_length);
    if (*status_code != 200) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (content_length >= (int64_t)buffer_size) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    while (true) {
        if (ota_deadline_expired(deadline_us)) {
            ESP_LOGE(TAG, "manifest read timeout: read=%u", (unsigned)bytes_read);
            ret = ESP_ERR_TIMEOUT;
            goto cleanup;
        }

        int read_len = esp_http_client_read(client,
                                            buffer + bytes_read,
                                            (int)(buffer_size - bytes_read - 1U));
        if (read_len < 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (ota_deadline_expired(deadline_us)) {
                ESP_LOGE(TAG, "manifest read stalled: read=%u", (unsigned)bytes_read);
                ret = ESP_ERR_TIMEOUT;
                goto cleanup;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bytes_read += (size_t)read_len;
        if (bytes_read >= buffer_size - 1U) {
            ret = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        vTaskDelay(OTA_DOWNLOAD_YIELD_TICKS);
    }

    buffer[bytes_read] = '\0';
    ESP_LOGI(TAG, "manifest read done: bytes=%u", (unsigned)bytes_read);

cleanup:
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return ret;
}

static esp_err_t ota_parse_manifest(const char *json_text,
                                             ota_manifest_t *manifest)
{
    cJSON *root = NULL;
    cJSON *firmware = NULL;
    cJSON *item = NULL;
    esp_err_t ret = ESP_OK;

    if (json_text == NULL || manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json_text);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(manifest, 0, sizeof(*manifest));

    item = cJSON_GetObjectItemCaseSensitive(root, "update");
    manifest->update = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(root, "force");
    manifest->force = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(root, "compatible");
    manifest->compatible = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(manifest->version, item->valuestring, sizeof(manifest->version));
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "target_chip");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(manifest->target_chip, item->valuestring, sizeof(manifest->target_chip));
        if (strlen(item->valuestring) >= sizeof(manifest->target_chip)) {
            ret = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "reason");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(manifest->reason, item->valuestring, sizeof(manifest->reason));
        if (strlen(item->valuestring) >= sizeof(manifest->reason)) {
            ret = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }

    firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (!cJSON_IsObject(firmware)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    item = cJSON_GetObjectItemCaseSensitive(firmware, "url");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(manifest->firmware_url, item->valuestring, sizeof(manifest->firmware_url));
        if (strlen(item->valuestring) >= sizeof(manifest->firmware_url)) {
            ret = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(firmware, "size");
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        manifest->firmware_size = (size_t)item->valuedouble;
    }

    item = cJSON_GetObjectItemCaseSensitive(firmware, "sha256");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(manifest->firmware_sha256, item->valuestring, sizeof(manifest->firmware_sha256));
        if (strlen(item->valuestring) >= sizeof(manifest->firmware_sha256)) {
            ret = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }

cleanup:
    cJSON_Delete(root);
    return ret;
}

static esp_err_t ota_fetch_manifest(const char *base_url,
                                             const char *device_id,
                                             const char *chip,
                                             const char *current_version,
                                             ota_manifest_t *manifest)
{
    char *manifest_json = NULL;
    char manifest_url[OTA_URL_MAX + 160] = {0};
    char encoded_device_id[OTA_DEVICE_ID_MAX * 3] = {0};
    char encoded_chip[OTA_CHIP_MAX * 3] = {0};
    char encoded_version[OTA_VERSION_MAX * 3] = {0};
    int status_code = 0;
    int ret_len = 0;
    esp_err_t ret = ESP_OK;

    if (base_url == NULL || device_id == NULL || chip == NULL || current_version == NULL || manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ota_url_encode(device_id, encoded_device_id, sizeof(encoded_device_id)),
                        TAG,
                        "encode device_id failed");
    ESP_RETURN_ON_ERROR(ota_url_encode(chip, encoded_chip, sizeof(encoded_chip)),
                        TAG,
                        "encode chip failed");
    ESP_RETURN_ON_ERROR(ota_url_encode(current_version, encoded_version, sizeof(encoded_version)),
                        TAG,
                        "encode version failed");

    ret_len = snprintf(manifest_url,
                       sizeof(manifest_url),
                       "%s/api/ota/manifest?device_id=%s&chip=%s&version=%s",
                       base_url,
                       encoded_device_id,
                       encoded_chip,
                       encoded_version);
    if (!ota_snprintf_ok(ret_len, sizeof(manifest_url))) {
        return ESP_ERR_INVALID_SIZE;
    }

    ota_set_url(manifest_url);
    manifest_json = ota_malloc_psram(OTA_MANIFEST_MAX_SIZE);
    if (manifest_json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = ota_http_get_text(manifest_url,
                                     manifest_json,
                                     OTA_MANIFEST_MAX_SIZE,
                                     &status_code);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "manifest request failed: status=%d err=%s", status_code, esp_err_to_name(ret));
        goto cleanup;
    }

    ret = ota_parse_manifest(manifest_json, manifest);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "manifest parse failed: %s", esp_err_to_name(ret));
    }

cleanup:
    free(manifest_json);
    return ret;
}

static bool ota_sha256_hex_equal(const char *expected, const char *actual)
{
    if (expected == NULL || actual == NULL || expected[0] == '\0') {
        return true;
    }

    for (size_t index = 0; index < OTA_SHA256_HEX_SIZE - 1U; ++index) {
        if (expected[index] == '\0' || actual[index] == '\0') {
            return expected[index] == actual[index];
        }
        if (toupper((unsigned char)expected[index]) != toupper((unsigned char)actual[index])) {
            return false;
        }
    }

    return expected[OTA_SHA256_HEX_SIZE - 1U] == '\0' &&
           actual[OTA_SHA256_HEX_SIZE - 1U] == '\0';
}

static void ota_sha256_to_hex(const uint8_t *digest, char *hex, size_t hex_size)
{
    static const char *digits = "0123456789ABCDEF";

    if (digest == NULL || hex == NULL || hex_size < OTA_SHA256_HEX_SIZE) {
        return;
    }

    for (size_t index = 0; index < OTA_SHA256_BIN_SIZE; ++index) {
        hex[index * 2U] = digits[(digest[index] >> 4) & 0x0F];
        hex[index * 2U + 1U] = digits[digest[index] & 0x0F];
    }
    hex[OTA_SHA256_HEX_SIZE - 1U] = '\0';
}

static esp_err_t ota_download_and_stage(const ota_manifest_t *manifest)
{
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    uint8_t *buffer = NULL;
    int64_t content_length = 0;
    size_t total_size = 0;
    size_t bytes_read = 0;
    size_t next_progress_log = OTA_DOWNLOAD_LOG_STEP;
    bool ota_started = false;
    bool ota_finished = false;
    int64_t stall_deadline_us = 0;
    uint8_t digest[OTA_SHA256_BIN_SIZE] = {0};
    char digest_hex[OTA_SHA256_HEX_SIZE] = {0};
    mbedtls_sha256_context sha_ctx;

    if (manifest == NULL || manifest->firmware_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_sha256_init(&sha_ctx);

    esp_http_client_config_t http_cfg = {
        .url = manifest->firmware_url,
        .timeout_ms = OTA_HTTP_IO_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    ota_set_url(manifest->firmware_url);
    ota_set_progress(0, manifest->firmware_size, "Opening firmware URL");
    ESP_LOGI(TAG,
             "firmware request begin: manifest_size=%u",
             (unsigned)manifest->firmware_size);

    client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "open OTA URL failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,
             "firmware response: status=%d length=%lld",
             status_code,
             (long long)content_length);
    if (status_code != 200) {
        ESP_LOGE(TAG, "OTA HTTP status=%d", status_code);
        ret = ESP_FAIL;
        goto cleanup;
    }

    if (content_length > 0) {
        total_size = (size_t)content_length;
    } else {
        total_size = manifest->firmware_size;
    }

    if (manifest->firmware_size > 0 && total_size > 0 && total_size != manifest->firmware_size) {
        ESP_LOGE(TAG,
                 "manifest size mismatch: content=%u manifest=%u",
                 (unsigned)total_size,
                 (unsigned)manifest->firmware_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    ESP_LOGI(TAG,
             "OTA target partition: label=%s offset=0x%lx size=%u",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned)update_partition->size);
    if (total_size > 0 && total_size > update_partition->size) {
        ESP_LOGE(TAG,
                 "firmware too large: size=%u partition=%u",
                 (unsigned)total_size,
                 (unsigned)update_partition->size);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    ota_set_progress(0, total_size, "Preparing flash");
    ESP_LOGI(TAG, "esp_ota_begin start: image_size=%u", (unsigned)total_size);
    ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG, "esp_ota_begin done");
    ota_started = true;

    buffer = ota_malloc_internal(OTA_DOWNLOAD_BUF_SIZE);
    if (buffer == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG, "OTA download buffer allocated in internal RAM: size=%u", (unsigned)OTA_DOWNLOAD_BUF_SIZE);

    if (mbedtls_sha256_starts(&sha_ctx, 0) != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    ota_set_status(OTA_STATE_DOWNLOADING, true, ESP_OK, "Downloading firmware");
    ota_set_progress(0, total_size, "Downloading firmware");
    stall_deadline_us = ota_deadline_from_now_us(OTA_DOWNLOAD_STALL_MS);

    while (true) {
        if (ota_deadline_expired(stall_deadline_us)) {
            ESP_LOGE(TAG,
                     "OTA download stalled: read=%u total=%u",
                     (unsigned)bytes_read,
                     (unsigned)total_size);
            ret = ESP_ERR_TIMEOUT;
            goto cleanup;
        }

        int read_len = esp_http_client_read(client, (char *)buffer, OTA_DOWNLOAD_BUF_SIZE);
        if (read_len < 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (ota_deadline_expired(stall_deadline_us)) {
                ESP_LOGE(TAG,
                         "OTA download no-data timeout: read=%u total=%u",
                         (unsigned)bytes_read,
                         (unsigned)total_size);
                ret = ESP_ERR_TIMEOUT;
                goto cleanup;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        stall_deadline_us = ota_deadline_from_now_us(OTA_DOWNLOAD_STALL_MS);
        ret = esp_ota_write(ota_handle, buffer, (size_t)read_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        if (mbedtls_sha256_update(&sha_ctx, buffer, (size_t)read_len) != 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }

        bytes_read += (size_t)read_len;
        ota_set_progress(bytes_read, total_size, "Downloading firmware");
        if (bytes_read >= next_progress_log || (total_size > 0 && bytes_read == total_size)) {
            ESP_LOGI(TAG,
                     "OTA download progress: read=%u total=%u percent=%u",
                     (unsigned)bytes_read,
                     (unsigned)total_size,
                     (unsigned)ota_progress_percent(bytes_read, total_size));
            while (next_progress_log <= bytes_read) {
                next_progress_log += OTA_DOWNLOAD_LOG_STEP;
            }
        }
        vTaskDelay(OTA_DOWNLOAD_YIELD_TICKS);
    }

    if (total_size > 0 && bytes_read != total_size) {
        ESP_LOGE(TAG, "OTA size mismatch: read=%u expected=%u", (unsigned)bytes_read, (unsigned)total_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    ota_set_status(OTA_STATE_VERIFYING, true, ESP_OK, "Verifying firmware");
    ota_set_progress(bytes_read, total_size, "Verifying firmware");
    ESP_LOGI(TAG, "OTA download complete: bytes=%u", (unsigned)bytes_read);

    if (mbedtls_sha256_finish(&sha_ctx, digest) != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    ota_sha256_to_hex(digest, digest_hex, sizeof(digest_hex));
    if (!ota_sha256_hex_equal(manifest->firmware_sha256, digest_hex)) {
        ESP_LOGE(TAG, "OTA sha256 mismatch: actual=%s expected=%s", digest_hex, manifest->firmware_sha256);
        ret = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }

    ret = esp_ota_end(ota_handle);
    ota_finished = true;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ota_set_progress(bytes_read, total_size, "Ready to reboot");
    ESP_LOGI(TAG, "OTA image staged: version=%s bytes=%u", manifest->version, (unsigned)bytes_read);
    ret = ESP_OK;

cleanup:
    if (buffer != NULL) {
        free(buffer);
    }
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (ota_started && !ota_finished) {
        ESP_LOGW(TAG, "aborting unfinished OTA write");
        (void)esp_ota_abort(ota_handle);
    }
    mbedtls_sha256_free(&sha_ctx);
    return ret;
}

static bool ota_manifest_has_valid_firmware(const ota_manifest_t *manifest)
{
    return manifest != NULL &&
           manifest->firmware_url[0] != '\0' &&
           manifest->version[0] != '\0' &&
           manifest->firmware_size > 0 &&
           manifest->firmware_sha256[0] != '\0';
}

static void ota_task(void *ctx)
{
    ota_task_ctx_t *task_ctx = (ota_task_ctx_t *)ctx;
    ota_manifest_t manifest = {0};
    esp_err_t ret = ESP_OK;
    bool staged_update = false;
    char failure_message[OTA_MESSAGE_MAX] = {0};
    char current_version[OTA_VERSION_MAX] = {0};

    if (task_ctx == NULL) {
        ota_set_status(OTA_STATE_FAILED, false, ESP_ERR_INVALID_ARG, "OTA context missing");
        ota_clear_task_handle();
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ota_fill_current_version(current_version, sizeof(current_version));

    ota_set_status(OTA_STATE_CHECKING, true, ESP_OK, "Checking update");
    ota_set_progress(0, 0, "Checking update");
    ESP_LOGI(TAG,
             "OTA task start: chip=%s device_id_len=%u current=%s",
             task_ctx->chip,
             (unsigned)strlen(task_ctx->device_id),
             current_version);

    ret = ota_fetch_manifest(task_ctx->base_url,
                                      task_ctx->device_id,
                                      task_ctx->chip,
                                      current_version,
                                      &manifest);
    if (ret == ESP_OK) {
        ota_set_target_version(manifest.update ? manifest.version : NULL);
        if (manifest.update) {
            if (!manifest.compatible) {
                ret = ESP_ERR_INVALID_RESPONSE;
                strlcpy(failure_message,
                        manifest.reason[0] != '\0' ? manifest.reason : "OTA release is not compatible",
                        sizeof(failure_message));
                goto done;
            }
            if (manifest.target_chip[0] == '\0' || strcmp(manifest.target_chip, task_ctx->chip) != 0) {
                ret = ESP_ERR_INVALID_RESPONSE;
                strlcpy(failure_message, "OTA target chip mismatch", sizeof(failure_message));
                goto done;
            }
        }

        if (!manifest.update) {
            char no_update_message[OTA_MESSAGE_MAX] = {0};
            const char *reason = manifest.reason[0] != '\0' ? manifest.reason : "up_to_date";
            ESP_LOGI(TAG, "OTA no update: reason=%s", reason);
            int msg_len = snprintf(no_update_message, sizeof(no_update_message), "No update: %s", reason);
            if (!ota_snprintf_ok(msg_len, sizeof(no_update_message))) {
                strlcpy(no_update_message, "No update", sizeof(no_update_message));
            }
            ota_set_status(OTA_STATE_IDLE, false, ESP_OK, no_update_message);
            ota_set_progress(0, 0, no_update_message);
            goto done;
        }

        if (!ota_manifest_has_valid_firmware(&manifest)) {
            ret = ESP_ERR_INVALID_RESPONSE;
            strlcpy(failure_message, "OTA firmware metadata invalid", sizeof(failure_message));
            goto done;
        }

        ret = ota_download_and_stage(&manifest);
        staged_update = ret == ESP_OK;
    }

done:
    if (staged_update) {
        ESP_LOGI(TAG, "OTA task done: staged update version=%s", manifest.version);
        ota_set_status(OTA_STATE_READY_TO_REBOOT,
                                false,
                                ESP_OK,
                                "Update ready. Reboot to apply.");
    } else if (ret != ESP_OK) {
        char message[OTA_MESSAGE_MAX] = {0};
        if (failure_message[0] != '\0') {
            strlcpy(message, failure_message, sizeof(message));
        } else {
            snprintf(message, sizeof(message), "OTA failed: %s", esp_err_to_name(ret));
        }
        ESP_LOGE(TAG, "OTA task failed: %s", message);
        ota_set_status(OTA_STATE_FAILED, false, ret, message);
    }

    free(task_ctx);
    ota_clear_task_handle();
    vTaskDeleteWithCaps(NULL);
}

esp_err_t ota_init(const ota_config_t *config)
{
    if (config != NULL) {
        s_ota_config = *config;
    }
    if (s_ota_config.default_url == NULL) {
        s_ota_config.default_url = "";
    }

    taskENTER_CRITICAL(&s_ota_lock);
    memset(&s_ota_snapshot, 0, sizeof(s_ota_snapshot));
    s_ota_snapshot.state = OTA_STATE_IDLE;
    strlcpy(s_ota_snapshot.url, s_ota_config.default_url, sizeof(s_ota_snapshot.url));
    strlcpy(s_ota_snapshot.message, "Ready", sizeof(s_ota_snapshot.message));
    ota_fill_version_locked();
    taskEXIT_CRITICAL(&s_ota_lock);

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "marking pending OTA image as valid");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
#endif

    return ESP_OK;
}

esp_err_t ota_start_default(const char *device_id)
{
    return ota_start(s_ota_config.default_url, device_id);
}

esp_err_t ota_start(const char *base_url, const char *device_id)
{
    TaskHandle_t task = NULL;
    ota_task_ctx_t *task_ctx = NULL;
    char normalized_base_url[OTA_URL_MAX] = {0};
    const char *effective_device_id = device_id;

    esp_err_t ret = ota_normalize_base_url(base_url,
                                                    normalized_base_url,
                                                    sizeof(normalized_base_url));
    if (ret != ESP_OK) {
        ota_set_status(OTA_STATE_FAILED,
                                false,
                                ret,
                                "OTA server is not configured");
        return ret;
    }

    if (effective_device_id == NULL || effective_device_id[0] == '\0') {
        effective_device_id = "device-unknown";
    }

    taskENTER_CRITICAL(&s_ota_lock);
    if (s_ota_snapshot.running) {
        taskEXIT_CRITICAL(&s_ota_lock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_ota_lock);

    task_ctx = ota_calloc_internal(1, sizeof(*task_ctx));
    if (task_ctx == NULL) {
        ota_set_status(OTA_STATE_FAILED,
                                false,
                                ESP_ERR_NO_MEM,
                                "OTA task allocation failed");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(task_ctx->base_url, normalized_base_url, sizeof(task_ctx->base_url));
    strlcpy(task_ctx->device_id, effective_device_id, sizeof(task_ctx->device_id));
    strlcpy(task_ctx->chip, ota_device_chip(), sizeof(task_ctx->chip));

    taskENTER_CRITICAL(&s_ota_lock);
    if (s_ota_snapshot.running) {
        taskEXIT_CRITICAL(&s_ota_lock);
        free(task_ctx);
        return ESP_ERR_INVALID_STATE;
    }
    s_ota_snapshot.state = OTA_STATE_CHECKING;
    s_ota_snapshot.running = true;
    s_ota_snapshot.progress_percent = 0;
    s_ota_snapshot.bytes_read = 0;
    s_ota_snapshot.total_size = 0;
    s_ota_snapshot.last_error = ESP_OK;
    s_ota_snapshot.target_version[0] = '\0';
    strlcpy(s_ota_snapshot.url, normalized_base_url, sizeof(s_ota_snapshot.url));
    strlcpy(s_ota_snapshot.message, "Checking update", sizeof(s_ota_snapshot.message));
    taskEXIT_CRITICAL(&s_ota_lock);

    BaseType_t ok = xTaskCreateWithCaps(ota_task,
                                        "ota",
                                        OTA_TASK_STACK_SIZE,
                                        task_ctx,
                                        OTA_TASK_PRIORITY,
                                        &task,
                                        OTA_TASK_ALLOC_CAPS);
    if (ok != pdPASS) {
        free(task_ctx);
        ota_set_status(OTA_STATE_FAILED,
                                false,
                                ESP_ERR_NO_MEM,
                                "OTA task allocation failed");
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_ota_lock);
    s_ota_task = task;
    taskEXIT_CRITICAL(&s_ota_lock);

    return ESP_OK;
}

void ota_get_snapshot(ota_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_ota_lock);
    *snapshot = s_ota_snapshot;
    ota_fill_version_locked();
    strlcpy(snapshot->current_version,
            s_ota_snapshot.current_version,
            sizeof(snapshot->current_version));
    strlcpy(snapshot->target_version,
            s_ota_snapshot.target_version,
            sizeof(snapshot->target_version));
    taskEXIT_CRITICAL(&s_ota_lock);
}

void ota_restart(void)
{
    esp_restart();
}
