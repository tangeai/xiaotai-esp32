// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/sys.h"

static void signature_base64(const uint8_t digest[XIAOTAI_SHA256_BYTES], char out[45])
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t pos = 0;
    for (size_t i = 0; i < XIAOTAI_SHA256_BYTES; i += 3) {
        const uint32_t triple = ((uint32_t)digest[i] << 16) | ((uint32_t)digest[i + 1] << 8) |
                                (i + 2 < XIAOTAI_SHA256_BYTES ? digest[i + 2] : 0);
        out[pos++] = alphabet[triple >> 18];
        out[pos++] = alphabet[(triple >> 12) & 63U];
        out[pos++] = alphabet[(triple >> 6) & 63U];
        out[pos++] = i + 2 < XIAOTAI_SHA256_BYTES ? alphabet[triple & 63U] : '=';
    }
    out[pos] = '\0';
}

xiaotai_err_t platform_signature_make(platform_signature_t *sig)
{
    xiaotai_identity_t identity = {0};
    xiaotai_err_t err = xiaotai_identity_get(&identity);
    if (err == XIAOTAI_OK) {
        strcpy(sig->device, identity.device_id);
        (void)snprintf(sig->stamp, sizeof(sig->stamp), "%" PRId64, xiaotai_clock_unix_ms() / 1000);
        const uint32_t high = xiaotai_random(), low = xiaotai_random();
        (void)snprintf(sig->nonce, sizeof(sig->nonce), "%08" PRIx32 "%08" PRIx32, high, low);
        char text[sizeof(sig->device) + sizeof(sig->stamp) + sizeof(sig->nonce)];
        const int n = snprintf(text, sizeof(text), "%s%s%s", sig->device, sig->stamp, sig->nonce);
        uint8_t digest[XIAOTAI_SHA256_BYTES];
        if (n < 0 || (size_t)n >= sizeof(text)) err = XIAOTAI_ERR_INVALID_SIZE;
        else err = xiaotai_hmac_sha256(identity.device_key, strlen(identity.device_key), text, (size_t)n, digest);
        if (err == XIAOTAI_OK) signature_base64(digest, sig->signature);
        xiaotai_identity_wipe(text, sizeof(text));
        xiaotai_identity_wipe(digest, sizeof(digest));
    }
    xiaotai_identity_wipe(&identity, sizeof(identity));
    return err;
}

size_t platform_signature_headers(const platform_signature_t *sig, xiaotai_http_header_t out[4])
{
    out[0] = (xiaotai_http_header_t){"X-Device-Id", sig->device};
    out[1] = (xiaotai_http_header_t){"X-Timestamp", sig->stamp};
    out[2] = (xiaotai_http_header_t){"X-Nonce", sig->nonce};
    out[3] = (xiaotai_http_header_t){"X-Signature", sig->signature};
    return 4;
}

static bool token_valid(const char *token)
{
    if (!*token) return false;
    for (; *token; ++token) if ((unsigned char)*token < 0x20U || (unsigned char)*token > 0x7eU) return false;
    return true;
}

xiaotai_err_t platform_login(uint32_t revision, const uint8_t mac[6], char *device, char *token,
                              bool *unbound, int64_t *expiry)
{
    *unbound = false;
    *expiry = 0;
    if (!xiaotai_clock_synced()) return XIAOTAI_ERR_INVALID_STATE;
    char base[XIAOTAI_PLATFORM_URL_MAX], url[XIAOTAI_PLATFORM_URL_MAX + sizeof("/v1/device/token")];
    char fingerprint[XIAOTAI_IDENTITY_FINGERPRINT_MAX + 1U];
    platform_signature_t signed_values = {0};
    xiaotai_err_t err = xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_DEVICE, base, sizeof(base));
    if (err == XIAOTAI_OK) err = platform_url_join(base, "/v1/device/token", url, sizeof(url));
    if (err == XIAOTAI_OK) err = xiaotai_identity_fingerprint(mac, fingerprint, sizeof(fingerprint));
    if (err == XIAOTAI_OK) err = platform_signature_make(&signed_values);
    if (err != XIAOTAI_OK) goto exit;

    xiaotai_http_header_t headers[6];
    const size_t count = platform_signature_headers(&signed_values, headers);
    headers[count] = headers[count - 1U];
    headers[count - 1U] = (xiaotai_http_header_t){"X-Mac", fingerprint};
    headers[count + 1U] = (xiaotai_http_header_t){"Content-Type", "application/json"};
    const xiaotai_http_request_t req = {.method = XIAOTAI_HTTP_POST, .url = url, .headers = headers,
        .header_count = count + 2U, .timeout_ms = XIAOTAI_PLATFORM_TIMEOUT_DEFAULT_MS};
    platform_setup_reply_t reply = {0};
    err = platform_session_http(revision, &req, &reply);
    if (err == XIAOTAI_OK) {
        int32_t code;
        const char *data, *value;
        size_t size, len;
        if (reply.response.redirect || xiaotai_platform_json_code(reply.body, reply.size, &code) != XIAOTAI_OK) err = XIAOTAI_FAIL;
        else if (reply.response.status == 410 && code == XIAOTAI_PLATFORM_CODE_UNBOUND) *unbound = true;
        else if (reply.response.status != 200 || code != 200 ||
            platform_json_field(reply.body, reply.size, "data", &data, &size) != XIAOTAI_OK ||
            platform_json_field(data, size, "mqtt_token", &value, &len) != XIAOTAI_OK ||
            platform_json_text_read(value, len, token, XIAOTAI_PLATFORM_BEARER_MAX) != XIAOTAI_OK || !token_valid(token))
            err = XIAOTAI_FAIL;
        else {
            (void)xiaotai_platform_token_expiry(token, expiry);
            if (*expiry <= xiaotai_clock_unix_ms() / 1000) *expiry = 0;
            if (platform.config.token_needs_expiry && !*expiry) err = XIAOTAI_FAIL;
            else strcpy(device, signed_values.device);
        }
    }
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_PLATFORM_STALE)
        XIAOTAI_LOGW("platform", "device login failed: result=%d http=%d", err, reply.response.status);
    platform_setup_reply_release(&reply, true);
exit:
    xiaotai_identity_wipe(&signed_values, sizeof(signed_values));
    return err;
}
