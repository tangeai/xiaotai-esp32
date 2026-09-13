#include "device_auth_http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "app_memory_policy.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "thing_http_client.h"

static const char *TAG = "device_auth";

#define DEVICE_AUTH_URL_MAX_LEN      256
#define DEVICE_AUTH_RESPONSE_MAX_LEN 2048
#define DEVICE_AUTH_NONCE_HEX_LEN    16
#define DEVICE_AUTH_SIGNATURE_MAX    96

static void device_auth_make_nonce(char *nonce, size_t nonce_size)
{
    if (nonce == NULL || nonce_size == 0) {
        return;
    }
    nonce[0] = '\0';
    uint8_t raw[8];
    esp_fill_random(raw, sizeof(raw));
    for (size_t index = 0; index < sizeof(raw) && (index * 2 + 2) < nonce_size; ++index) {
        snprintf(nonce + index * 2, 3, "%02x", raw[index]);
    }
}

static esp_err_t device_auth_sign(const char *device_id,
                                  const char *device_key,
                                  const char *timestamp,
                                  const char *nonce,
                                  char *signature,
                                  size_t signature_size)
{
    char raw[256];
    uint8_t digest[32];
    size_t encoded_len = 0;

    if (device_id == NULL || device_key == NULL || timestamp == NULL || nonce == NULL ||
        signature == NULL || signature_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(raw, sizeof(raw), "%s%s%s", device_id, timestamp, nonce);
    if (written <= 0 || written >= (int)sizeof(raw)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return ESP_FAIL;
    }
    if (mbedtls_md_hmac(md_info,
                        (const uint8_t *)device_key,
                        strlen(device_key),
                        (const uint8_t *)raw,
                        strlen(raw),
                        digest) != 0) {
        return ESP_FAIL;
    }
    if (mbedtls_base64_encode((uint8_t *)signature,
                              signature_size - 1,
                              &encoded_len,
                              digest,
                              sizeof(digest)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    signature[encoded_len] = '\0';
    return ESP_OK;
}

static esp_err_t device_auth_parse_token_response(const char *json, device_auth_token_t *token)
{
    if (json == NULL || token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    int code_value = cJSON_IsNumber(code) ? code->valueint : -1;
    if (code_value != 0 && code_value != 200) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        ESP_LOGW(TAG, "device token rejected: code=%d msg=%s",
                 code_value,
                 cJSON_GetStringValue(msg) != NULL ? cJSON_GetStringValue(msg) : "");
        cJSON_Delete(root);
        return code_value == 6006 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *mqtt_token = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "mqtt_token") : NULL;
    const char *value = cJSON_GetStringValue(mqtt_token);
    if (value == NULL || value[0] == '\0' || strlen(value) >= sizeof(token->mqtt_token)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(token, 0, sizeof(*token));
    strlcpy(token->mqtt_token, value, sizeof(token->mqtt_token));
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t device_auth_http_get_mqtt_token(const char *api_base,
                                          const char *device_id,
                                          const char *device_key,
                                          const char *mac,
                                          device_auth_token_t *token)
{
    if (api_base == NULL || api_base[0] == '\0' ||
        device_id == NULL || device_id[0] == '\0' ||
        device_key == NULL || device_key[0] == '\0' ||
        token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[DEVICE_AUTH_URL_MAX_LEN] = {0};
    ESP_RETURN_ON_ERROR(thing_http_join_url(url, sizeof(url), api_base, "/v1/device/token"),
                        TAG,
                        "device token URL build failed");

    char timestamp[24];
    snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
    char nonce[DEVICE_AUTH_NONCE_HEX_LEN + 1] = {0};
    device_auth_make_nonce(nonce, sizeof(nonce));

    char signature[DEVICE_AUTH_SIGNATURE_MAX] = {0};
    ESP_RETURN_ON_ERROR(device_auth_sign(device_id,
                                         device_key,
                                         timestamp,
                                         nonce,
                                         signature,
                                         sizeof(signature)),
                        TAG,
                        "device token signature failed");

    thing_http_header_t headers[] = {
        {"X-Device-Id", device_id},
        {"X-Timestamp", timestamp},
        {"X-Nonce", nonce},
        {"X-Signature", signature},
        {"X-Mac", mac != NULL ? mac : ""},
    };
    char *response = app_memory_calloc_psram(1, DEVICE_AUTH_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    const thing_http_request_t request = {
        .url = url,
        .method = "POST",
        .body = "",
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
    };

    ESP_LOGD(TAG, "request mqtt token: device_id_len=%u", (unsigned)strlen(device_id));
    esp_err_t ret = thing_http_request_json(&request, response, DEVICE_AUTH_RESPONSE_MAX_LEN, &status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "request mqtt token failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    if (status != 200) {
        esp_err_t parse_ret = device_auth_parse_token_response(response, token);
        if (parse_ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "request mqtt token requires rebind: http=%d body_len=%u",
                     status,
                     (unsigned)strlen(response));
            ret = ESP_ERR_NOT_FOUND;
            goto cleanup;
        }
        ESP_LOGW(TAG, "request mqtt token HTTP status=%d body_len=%u",
                 status,
                 (unsigned)strlen(response));
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = device_auth_parse_token_response(response, token);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "mqtt token ready: token_len=%u", (unsigned)strlen(token->mqtt_token));
    }
cleanup:
    free(response);
    return ret;
}
