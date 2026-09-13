#include "ai_chat_token.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "app_memory_policy.h"
#include "device_online.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "thing_http_client.h"

static const char *TAG = "ai_chat_token";

#define AI_CHAT_AI_TOKEN_PATH          "/v1/ai/token"
#define AI_CHAT_TOKEN_API_URL_MAX_LEN  224U
#define AI_CHAT_TOKEN_RESPONSE_MAX_LEN 4096U
#define AI_CHAT_TOKEN_SHA256_HEX_LEN   65U
#define AI_CHAT_TOKEN_CACHE_SKEW_US    (60LL * 1000LL * 1000LL)
#define AI_CHAT_TOKEN_CACHE_FALLBACK_US (5LL * 60LL * 1000LL * 1000LL)

typedef struct {
    SemaphoreHandle_t lock;
    ai_chat_join_info_t *join_info;
    ai_chat_config_t config;
    int64_t expires_us;
    bool valid;
} ai_chat_token_cache_t;

static ai_chat_token_cache_t s_join_cache;
static portMUX_TYPE s_join_cache_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void *ai_chat_token_calloc_psram(size_t count, size_t size)
{
    return app_memory_calloc_psram(count, size);
}

static bool ai_chat_token_is_blank(const char *value)
{
    return value == NULL || value[0] == '\0';
}

static void ai_chat_token_hex_encode(const uint8_t *data, size_t data_len, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";

    if (data == NULL || out == NULL || out_size == 0U) {
        return;
    }

    size_t max_bytes = (out_size - 1U) / 2U;
    if (data_len > max_bytes) {
        data_len = max_bytes;
    }

    for (size_t index = 0; index < data_len; ++index) {
        out[index * 2U] = hex[(data[index] >> 4) & 0x0FU];
        out[index * 2U + 1U] = hex[data[index] & 0x0FU];
    }
    out[data_len * 2U] = '\0';
}

static esp_err_t ai_chat_token_sha256_hex(const char *data,
                                          size_t data_len,
                                          char out[AI_CHAT_TOKEN_SHA256_HEX_LEN])
{
    uint8_t digest[32] = {0};
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (data == NULL || out == NULL || md_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mbedtls_md(md_info, (const uint8_t *)data, data_len, digest) != 0) {
        return ESP_FAIL;
    }

    ai_chat_token_hex_encode(digest, sizeof(digest), out, AI_CHAT_TOKEN_SHA256_HEX_LEN);
    return ESP_OK;
}

static int64_t ai_chat_token_now_wall_us(void)
{
    time_t now = 0;

    time(&now);
    if (now > 1600000000) {
        return (int64_t)now * 1000000LL;
    }
    return esp_timer_get_time();
}

static long long ai_chat_token_json_number_i64(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? (long long)item->valuedouble : 0;
}

static const char *ai_chat_token_json_string_or_dash(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    const char *value = cJSON_GetStringValue(item);
    return value != NULL ? value : "-";
}

static char *ai_chat_token_decode_jwt_payload(const char *token)
{
    const char *payload_begin = NULL;
    const char *payload_end = NULL;
    char *encoded = NULL;
    char *decoded = NULL;
    size_t payload_len = 0;
    size_t encoded_len = 0;
    size_t decoded_len = 0;
    size_t decoded_cap = 0;

    if (token == NULL) {
        return NULL;
    }

    payload_begin = strchr(token, '.');
    if (payload_begin == NULL) {
        return NULL;
    }
    payload_begin++;
    payload_end = strchr(payload_begin, '.');
    if (payload_end == NULL || payload_end <= payload_begin) {
        return NULL;
    }

    payload_len = (size_t)(payload_end - payload_begin);
    encoded_len = payload_len + ((4U - (payload_len % 4U)) % 4U);
    encoded = (char *)ai_chat_token_calloc_psram(1, encoded_len + 1U);
    decoded_cap = ((encoded_len / 4U) * 3U) + 4U;
    decoded = (char *)ai_chat_token_calloc_psram(1, decoded_cap);
    if (encoded == NULL || decoded == NULL) {
        free(encoded);
        free(decoded);
        return NULL;
    }

    for (size_t index = 0; index < payload_len; ++index) {
        char ch = payload_begin[index];
        if (ch == '-') {
            ch = '+';
        } else if (ch == '_') {
            ch = '/';
        }
        encoded[index] = ch;
    }
    for (size_t index = payload_len; index < encoded_len; ++index) {
        encoded[index] = '=';
    }

    if (mbedtls_base64_decode((uint8_t *)decoded,
                              decoded_cap - 1U,
                              &decoded_len,
                              (const uint8_t *)encoded,
                              encoded_len) != 0) {
        free(encoded);
        free(decoded);
        return NULL;
    }
    decoded[decoded_len] = '\0';
    free(encoded);
    return decoded;
}

static void ai_chat_token_log_token_claims(const char *token)
{
    char token_hash[AI_CHAT_TOKEN_SHA256_HEX_LEN] = {0};
    char *payload_json = NULL;
    cJSON *payload = NULL;

    if (token == NULL) {
        return;
    }

    if (ai_chat_token_sha256_hex(token, strlen(token), token_hash) != ESP_OK) {
        strlcpy(token_hash, "hash-failed", sizeof(token_hash));
    }

    payload_json = ai_chat_token_decode_jwt_payload(token);
    if (payload_json == NULL) {
        ESP_LOGD(TAG, "AI Chat token claims unavailable: hash=%.16s reason=not-jwt", token_hash);
        return;
    }

    payload = cJSON_Parse(payload_json);
    if (payload == NULL) {
        ESP_LOGD(TAG, "AI Chat token claims unavailable: hash=%.16s reason=parse-failed", token_hash);
        free(payload_json);
        return;
    }

    ESP_LOGD(TAG,
             "AI Chat token claims: hash=%.16s iss_len=%u sub_len=%u aud_len=%u scope_len=%u peer_id_len=%u iat=%lld nbf=%lld exp=%lld",
             token_hash,
             (unsigned)strlen(ai_chat_token_json_string_or_dash(payload, "iss")),
             (unsigned)strlen(ai_chat_token_json_string_or_dash(payload, "sub")),
             (unsigned)strlen(ai_chat_token_json_string_or_dash(payload, "aud")),
             (unsigned)strlen(ai_chat_token_json_string_or_dash(payload, "scope")),
             (unsigned)strlen(ai_chat_token_json_string_or_dash(payload, "peer_id")),
             ai_chat_token_json_number_i64(payload, "iat"),
             ai_chat_token_json_number_i64(payload, "nbf"),
             ai_chat_token_json_number_i64(payload, "exp"));

    cJSON_Delete(payload);
    free(payload_json);
}

static int64_t ai_chat_token_expiry_us(const char *token)
{
    char *payload_json = ai_chat_token_decode_jwt_payload(token);
    int64_t expires_us = ai_chat_token_now_wall_us() + AI_CHAT_TOKEN_CACHE_FALLBACK_US;

    if (payload_json == NULL) {
        return expires_us;
    }

    cJSON *payload = cJSON_Parse(payload_json);
    if (payload != NULL) {
        long long exp = ai_chat_token_json_number_i64(payload, "exp");
        if (exp > 1600000000LL) {
            expires_us = (int64_t)exp * 1000000LL;
        }
        cJSON_Delete(payload);
    }
    free(payload_json);
    return expires_us;
}

static bool ai_chat_token_config_matches(const ai_chat_config_t *lhs,
                                         const ai_chat_config_t *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->enabled == rhs->enabled &&
           strcmp(lhs->device_id, rhs->device_id) == 0 &&
           strcmp(lhs->user_id, rhs->user_id) == 0 &&
           strcmp(lhs->role_id, rhs->role_id) == 0 &&
           strcmp(lhs->device_key, rhs->device_key) == 0 &&
           strcmp(lhs->token_api_base, rhs->token_api_base) == 0;
}

static esp_err_t ai_chat_token_cache_ensure(void)
{
    if (s_join_cache.lock == NULL) {
        SemaphoreHandle_t new_lock = xSemaphoreCreateMutex();
        if (new_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
        taskENTER_CRITICAL(&s_join_cache_init_lock);
        if (s_join_cache.lock == NULL) {
            s_join_cache.lock = new_lock;
            new_lock = NULL;
        }
        taskEXIT_CRITICAL(&s_join_cache_init_lock);
        if (new_lock != NULL) {
            vSemaphoreDelete(new_lock);
        }
    }
    if (s_join_cache.join_info == NULL) {
        ai_chat_join_info_t *new_join_info =
            (ai_chat_join_info_t *)ai_chat_token_calloc_psram(1, sizeof(*s_join_cache.join_info));
        if (new_join_info == NULL) {
            return ESP_ERR_NO_MEM;
        }
        taskENTER_CRITICAL(&s_join_cache_init_lock);
        if (s_join_cache.join_info == NULL) {
            s_join_cache.join_info = new_join_info;
            new_join_info = NULL;
        }
        taskEXIT_CRITICAL(&s_join_cache_init_lock);
        free(new_join_info);
    }
    return ESP_OK;
}

static bool ai_chat_token_cache_take(const ai_chat_config_t *config,
                                     ai_chat_join_info_t *join_info)
{
    bool hit = false;
    int64_t now_us = ai_chat_token_now_wall_us();

    if (config == NULL || join_info == NULL || ai_chat_token_cache_ensure() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(s_join_cache.lock, portMAX_DELAY);
    if (s_join_cache.valid &&
        ai_chat_token_config_matches(config, &s_join_cache.config) &&
        s_join_cache.expires_us > now_us + AI_CHAT_TOKEN_CACHE_SKEW_US) {
        *join_info = *s_join_cache.join_info;
        memset(s_join_cache.join_info, 0, sizeof(*s_join_cache.join_info));
        memset(&s_join_cache.config, 0, sizeof(s_join_cache.config));
        s_join_cache.expires_us = 0;
        s_join_cache.valid = false;
        hit = true;
    } else if (s_join_cache.valid &&
               (!ai_chat_token_config_matches(config, &s_join_cache.config) ||
                s_join_cache.expires_us <= now_us + AI_CHAT_TOKEN_CACHE_SKEW_US)) {
        memset(s_join_cache.join_info, 0, sizeof(*s_join_cache.join_info));
        memset(&s_join_cache.config, 0, sizeof(s_join_cache.config));
        s_join_cache.expires_us = 0;
        s_join_cache.valid = false;
    }
    xSemaphoreGive(s_join_cache.lock);

    if (hit) {
        ESP_LOGI(TAG,
                 "AI Chat token cache hit: peer_id_len=%u role_id_len=%u token_len=%u",
                 (unsigned)strlen(join_info->peer_id),
                 (unsigned)strlen(join_info->role_id),
                 (unsigned)strlen(join_info->token));
    }
    return hit;
}

static esp_err_t ai_chat_token_cache_store(const ai_chat_config_t *config,
                                           const ai_chat_join_info_t *join_info)
{
    ESP_RETURN_ON_FALSE(config != NULL && join_info != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid token cache args");
    ESP_RETURN_ON_ERROR(ai_chat_token_cache_ensure(), TAG, "init token cache failed");

    xSemaphoreTake(s_join_cache.lock, portMAX_DELAY);
    s_join_cache.config = *config;
    *s_join_cache.join_info = *join_info;
    s_join_cache.expires_us = ai_chat_token_expiry_us(join_info->token);
    s_join_cache.valid = true;
    xSemaphoreGive(s_join_cache.lock);
    return ESP_OK;
}

static esp_err_t ai_chat_token_validate_config(const ai_chat_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ai_chat_token_is_blank(config->device_id) ||
        ai_chat_token_is_blank(config->device_key) ||
        ai_chat_token_is_blank(config->token_api_base)) {
        ESP_LOGW(TAG,
                 "AI Chat token config incomplete: device_id=%d device_key=%d api_base=%d role_id_optional=%d",
                 !ai_chat_token_is_blank(config->device_id),
                 !ai_chat_token_is_blank(config->device_key),
                 !ai_chat_token_is_blank(config->token_api_base),
                 !ai_chat_token_is_blank(config->role_id));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static bool ai_chat_token_response_code_ok(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");

    if (!cJSON_IsNumber(code)) {
        return true;
    }
    return code->valueint == 0 || code->valueint == 200;
}

static const char *ai_chat_token_service_message(const cJSON *root)
{
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "msg"));
    if (msg == NULL) {
        msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "message"));
    }
    return msg != NULL ? msg : "-";
}

static const cJSON *ai_chat_token_pick_payload_object(const cJSON *root)
{
    const cJSON *data = NULL;

    if (!cJSON_IsObject(root)) {
        return NULL;
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(data) ? data : root;
}

static void ai_chat_token_query_value(const char *uri,
                                      const char *key,
                                      char *out,
                                      size_t out_size)
{
    const char *query = NULL;
    const size_t key_len = key != NULL ? strlen(key) : 0U;

    if (uri == NULL || key == NULL || out == NULL || out_size == 0U || key_len == 0U) {
        return;
    }

    out[0] = '\0';
    query = strchr(uri, '?');
    if (query == NULL) {
        return;
    }
    query++;

    while (*query != '\0') {
        const char *value = NULL;
        const char *end = NULL;
        size_t value_len = 0U;

        if (strncmp(query, key, key_len) == 0 && query[key_len] == '=') {
            value = query + key_len + 1U;
            end = strchr(value, '&');
            if (end == NULL) {
                end = value + strlen(value);
            }
            value_len = (size_t)(end - value);
            if (value_len >= out_size) {
                value_len = out_size - 1U;
            }
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return;
        }

        query = strchr(query, '&');
        if (query == NULL) {
            return;
        }
        query++;
    }
}

static esp_err_t ai_chat_token_parse_ai_token(const char *response_body,
                                              const ai_chat_config_t *config,
                                              ai_chat_join_info_t *join_info)
{
    cJSON *root = NULL;
    const cJSON *payload = NULL;
    const cJSON *code = NULL;
    const char *peer_id = NULL;
    const char *token = NULL;

    if (response_body == NULL || config == NULL || join_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(response_body);
    if (root == NULL) {
        ESP_LOGW(TAG, "AI token response is not JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!ai_chat_token_response_code_ok(root)) {
        ESP_LOGW(TAG,
                 "AI token service error: code=%d msg=%s",
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 ai_chat_token_service_message(root));
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    payload = ai_chat_token_pick_payload_object(root);
    if (payload != NULL) {
        peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "peer_id"));
        token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "token"));
    }
    if (peer_id == NULL || token == NULL) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "AI token response missing peer_id/token");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(join_info, 0, sizeof(*join_info));
    strlcpy(join_info->device_id, config->device_id, sizeof(join_info->device_id));
    strlcpy(join_info->peer_id, peer_id, sizeof(join_info->peer_id));
    strlcpy(join_info->token, token, sizeof(join_info->token));
    ai_chat_token_query_value(join_info->peer_id, "role_id", join_info->role_id, sizeof(join_info->role_id));
    if (join_info->role_id[0] == '\0') {
        strlcpy(join_info->role_id, config->role_id, sizeof(join_info->role_id));
    }
    if (join_info->role_id[0] == '\0') {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "AI token response missing role_id and no local fallback role_id");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG,
             "AI Chat token accepted: peer_id_len=%u role_id_len=%u token_len=%u",
             (unsigned)strlen(join_info->peer_id),
             (unsigned)strlen(join_info->role_id),
             (unsigned)strlen(join_info->token));
    ai_chat_token_log_token_claims(join_info->token);
    return ESP_OK;
}

static esp_err_t ai_chat_token_request_ai_token(const ai_chat_config_t *config,
                                                const char *mqtt_token,
                                                ai_chat_join_info_t *join_info)
{
    char url[AI_CHAT_TOKEN_API_URL_MAX_LEN] = {0};
    char *authorization = NULL;
    char *response = NULL;
    int status = 0;
    int written = 0;
    esp_err_t ret = ESP_OK;

    if (config == NULL || mqtt_token == NULL || mqtt_token[0] == '\0' || join_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    authorization = (char *)ai_chat_token_calloc_psram(1, strlen(mqtt_token) + 16U);
    response = (char *)ai_chat_token_calloc_psram(1, AI_CHAT_TOKEN_RESPONSE_MAX_LEN);
    if (authorization == NULL || response == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ret = thing_http_join_url(url, sizeof(url), config->token_api_base, AI_CHAT_AI_TOKEN_PATH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "build AI token URL failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    written = snprintf(authorization, strlen(mqtt_token) + 16U, "Bearer %s", mqtt_token);
    if (written < 0 || (size_t)written >= strlen(mqtt_token) + 16U) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    const thing_http_header_t headers[] = {
        {"Authorization", authorization},
    };
    const thing_http_request_t request = {
        .url = url,
        .method = "GET",
        .body = NULL,
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
    };

    ESP_LOGI(TAG,
             "AI Chat AI token request: device_id_len=%u role_id_len=%u",
             (unsigned)strlen(config->device_id),
             (unsigned)strlen(config->role_id));
    ret = thing_http_request_json(&request, response, AI_CHAT_TOKEN_RESPONSE_MAX_LEN, &status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AI token HTTP failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "AI token HTTP status=%d body_len=%u", status, (unsigned)strlen(response));
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "AI Chat AI token response: status=%d body_len=%u", status, (unsigned)strlen(response));
    ret = ai_chat_token_parse_ai_token(response, config, join_info);

cleanup:
    free(response);
    if (authorization != NULL) {
        memset(authorization, 0, strlen(authorization));
    }
    free(authorization);
    return ret;
}

static esp_err_t ai_chat_token_request_join_uncached(const ai_chat_config_t *config,
                                                     ai_chat_join_info_t *join_info)
{
    device_auth_token_t *mqtt_token = NULL;
    esp_err_t ret = ESP_OK;

    mqtt_token = (device_auth_token_t *)ai_chat_token_calloc_psram(1, sizeof(*mqtt_token));
    if (mqtt_token == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = device_online_get_cached_mqtt_token(mqtt_token);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ret = ai_chat_token_request_ai_token(config, mqtt_token->mqtt_token, join_info);

cleanup:
    if (mqtt_token != NULL) {
        memset(mqtt_token, 0, sizeof(*mqtt_token));
    }
    free(mqtt_token);
    return ret;
}

esp_err_t ai_chat_token_prefetch_join(const ai_chat_config_t *config)
{
    ai_chat_join_info_t *join_info = NULL;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_ERROR(ai_chat_token_validate_config(config), TAG, "invalid AI Chat token config");
    ESP_RETURN_ON_ERROR(ai_chat_token_cache_ensure(), TAG, "init token cache failed");

    xSemaphoreTake(s_join_cache.lock, portMAX_DELAY);
    bool cache_ready = s_join_cache.valid &&
                       ai_chat_token_config_matches(config, &s_join_cache.config) &&
                       s_join_cache.expires_us >
                           ai_chat_token_now_wall_us() + AI_CHAT_TOKEN_CACHE_SKEW_US;
    xSemaphoreGive(s_join_cache.lock);
    if (cache_ready) {
        return ESP_OK;
    }

    join_info = (ai_chat_join_info_t *)ai_chat_token_calloc_psram(1, sizeof(*join_info));
    if (join_info == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = ai_chat_token_request_join_uncached(config, join_info);
    if (ret == ESP_OK) {
        ret = ai_chat_token_cache_store(config, join_info);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "AI Chat token prefetched: peer_id_len=%u role_id_len=%u token_len=%u",
                     (unsigned)strlen(join_info->peer_id),
                     (unsigned)strlen(join_info->role_id),
                     (unsigned)strlen(join_info->token));
        }
    }

    memset(join_info, 0, sizeof(*join_info));
    free(join_info);
    return ret;
}

esp_err_t ai_chat_token_request_join(const ai_chat_config_t *config,
                                     ai_chat_join_info_t *join_info)
{
    ESP_RETURN_ON_ERROR(ai_chat_token_validate_config(config), TAG, "invalid AI Chat token config");
    if (join_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ai_chat_token_cache_take(config, join_info)) {
        return ESP_OK;
    }
    return ai_chat_token_request_join_uncached(config, join_info);
}

void ai_chat_token_invalidate_cache(void)
{
    if (s_join_cache.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_join_cache.lock, portMAX_DELAY);
    if (s_join_cache.join_info != NULL) {
        memset(s_join_cache.join_info, 0, sizeof(*s_join_cache.join_info));
    }
    memset(&s_join_cache.config, 0, sizeof(s_join_cache.config));
    s_join_cache.expires_us = 0;
    s_join_cache.valid = false;
    xSemaphoreGive(s_join_cache.lock);
}
