#include "tirtc_token.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "tirtc_session_token";

#define TIRTC_TOKEN_NONCE_BYTES          16U
#define TIRTC_TOKEN_MAX_TTL_SECONDS      86400U
#define TIRTC_TOKEN_PAYLOAD_JSON_MAX_LEN 512U
#define TIRTC_TOKEN_PAYLOAD_B64_MAX_LEN  768U
#define TIRTC_TOKEN_BASE64_WORK_MAX_LEN  768U
#define TIRTC_TOKEN_SIGNATURE_MAX_LEN    64U
#define TIRTC_TOKEN_SIGN_INPUT_MAX_LEN   896U
#define TIRTC_TOKEN_MIN_UNIX_TIME        1600000000

static bool tirtc_token_is_blank(const char *value)
{
    return value == NULL || value[0] == '\0';
}

static esp_err_t tirtc_token_normalize_device_id(const char *peer_id,
                                                        char *device_id,
                                                        size_t device_id_size)
{
    const char *scheme = "device://";
    const size_t scheme_len = strlen(scheme);

    if (peer_id == NULL || device_id == NULL || device_id_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *start = peer_id;
    while (isspace((unsigned char)*start)) {
        start++;
    }

    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    if ((size_t)(end - start) >= scheme_len && strncmp(start, scheme, scheme_len) == 0) {
        start += scheme_len;
        while (start < end && isspace((unsigned char)*start)) {
            start++;
        }
    } else if (strstr(start, "://") != NULL) {
        ESP_LOGE(TAG, "remote id must be a device id or device:// URI");
        return ESP_ERR_INVALID_ARG;
    }

    if (start >= end) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t device_id_len = (size_t)(end - start);
    if (device_id_len >= device_id_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(device_id, start, device_id_len);
    device_id[device_id_len] = '\0';
    return ESP_OK;
}

static void tirtc_token_fill_nonce(uint8_t nonce[TIRTC_TOKEN_NONCE_BYTES])
{
    for (size_t index = 0U; index < TIRTC_TOKEN_NONCE_BYTES; index += sizeof(uint32_t)) {
        uint32_t random_value = esp_random();
        size_t copy_len = TIRTC_TOKEN_NONCE_BYTES - index;
        if (copy_len > sizeof(random_value)) {
            copy_len = sizeof(random_value);
        }
        memcpy(nonce + index, &random_value, copy_len);
    }
}

static esp_err_t tirtc_token_base64url_encode(const uint8_t *data,
                                                     size_t data_len,
                                                     char *out,
                                                     size_t out_size)
{
    uint8_t base64[TIRTC_TOKEN_BASE64_WORK_MAX_LEN];
    size_t encoded_len = 0U;
    size_t write_index = 0U;

    if (data == NULL || out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (4U * ((data_len + 2U) / 3U) + 1U > sizeof(base64)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (mbedtls_base64_encode(base64, sizeof(base64) - 1U, &encoded_len, data, data_len) != 0) {
        return ESP_FAIL;
    }

    for (size_t read_index = 0U; read_index < encoded_len; ++read_index) {
        char current = (char)base64[read_index];

        if (current == '=') {
            continue;
        }
        if (current == '+') {
            current = '-';
        } else if (current == '/') {
            current = '_';
        }

        if (write_index + 1U >= out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        out[write_index++] = current;
    }

    out[write_index] = '\0';
    return ESP_OK;
}

static esp_err_t tirtc_token_hmac_sha256_base64url(const char *key,
                                                          const char *message,
                                                          char *out,
                                                          size_t out_size)
{
    uint8_t digest[32];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (key == NULL || message == NULL || out == NULL || out_size == 0U || md_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mbedtls_md_hmac(md_info,
                        (const uint8_t *)key,
                        strlen(key),
                        (const uint8_t *)message,
                        strlen(message),
                        digest) != 0) {
        return ESP_FAIL;
    }

    return tirtc_token_base64url_encode(digest, sizeof(digest), out, out_size);
}

static esp_err_t tirtc_token_current_unix_time(time_t *now)
{
    if (now == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time(now);
    if (*now < TIRTC_TOKEN_MIN_UNIX_TIME) {
        ESP_LOGE(TAG, "system time is not synced; cannot generate connect token");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t tirtc_token_fetch_connect(const tirtc_session_config_t *config,
                                           const char *peer_id,
                                           char *out_token,
                                           size_t out_token_size)
{
    char device_id[TIRTC_SESSION_DEVICE_ID_MAX_LEN];
    char nonce_b64[TIRTC_TOKEN_SIGNATURE_MAX_LEN];
    char payload_json[TIRTC_TOKEN_PAYLOAD_JSON_MAX_LEN];
    char payload_b64[TIRTC_TOKEN_PAYLOAD_B64_MAX_LEN];
    char device_sig[TIRTC_TOKEN_SIGNATURE_MAX_LEN];
    char sign_input[TIRTC_TOKEN_SIGN_INPUT_MAX_LEN];
    char app_sig[TIRTC_TOKEN_SIGNATURE_MAX_LEN];
    uint8_t nonce[TIRTC_TOKEN_NONCE_BYTES];
    time_t now = 0;
    int written = 0;

    if (config == NULL || peer_id == NULL || peer_id[0] == '\0' ||
        out_token == NULL || out_token_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tirtc_token_is_blank(config->token_access_id) ||
        tirtc_token_is_blank(config->token_secret_key) ||
        tirtc_token_is_blank(config->token_subject) ||
        tirtc_token_is_blank(config->remote_device_secret_key)) {
        ESP_LOGE(TAG, "connect token config is incomplete");
        return ESP_ERR_INVALID_STATE;
    }

    if (config->token_ttl_seconds == 0U ||
        config->token_ttl_seconds > TIRTC_TOKEN_MAX_TTL_SECONDS) {
        ESP_LOGE(TAG,
                 "connect token ttl must be between 1 and %u seconds",
                 (unsigned)TIRTC_TOKEN_MAX_TTL_SECONDS);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = tirtc_token_normalize_device_id(peer_id, device_id, sizeof(device_id));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tirtc_token_current_unix_time(&now);
    if (ret != ESP_OK) {
        return ret;
    }

    tirtc_token_fill_nonce(nonce);
    ret = tirtc_token_base64url_encode(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64));
    if (ret != ESP_OK) {
        return ret;
    }

    written = snprintf(payload_json,
                       sizeof(payload_json),
                       "{\"sub\":\"%s\",\"scope\":\"connect:device://%s\","
                       "\"iss\":\"%s\",\"iat\":%lld,\"exp\":%lld,\"nonce\":\"%s\"}",
                       config->token_subject,
                       device_id,
                       config->token_access_id,
                       (long long)now,
                       (long long)(now + config->token_ttl_seconds),
                       nonce_b64);
    if (written < 0 || (size_t)written >= sizeof(payload_json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = tirtc_token_base64url_encode((const uint8_t *)payload_json,
                                              (size_t)written,
                                              payload_b64,
                                              sizeof(payload_b64));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tirtc_token_hmac_sha256_base64url(config->remote_device_secret_key,
                                                   payload_b64,
                                                   device_sig,
                                                   sizeof(device_sig));
    if (ret != ESP_OK) {
        return ret;
    }

    written = snprintf(sign_input, sizeof(sign_input), "%s.%s", payload_b64, device_sig);
    if (written < 0 || (size_t)written >= sizeof(sign_input)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = tirtc_token_hmac_sha256_base64url(config->token_secret_key,
                                                   sign_input,
                                                   app_sig,
                                                   sizeof(app_sig));
    if (ret != ESP_OK) {
        return ret;
    }

    written = snprintf(out_token, out_token_size, "v1.%s.%s", payload_b64, app_sig);
    if (written < 0 || (size_t)written >= out_token_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG,
             "connect token generated: remote_id_len=%u subject_len=%u ttl=%lu",
             (unsigned)strlen(device_id),
             (unsigned)strlen(config->token_subject),
             (unsigned long)config->token_ttl_seconds);
    return ESP_OK;
}
