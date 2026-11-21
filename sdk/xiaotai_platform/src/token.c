// SPDX-License-Identifier: MIT
#include <string.h>
#include "platform_internal.h"

static int sextet(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    return c == '_' ? 63 : -1;
}

xiaotai_err_t xiaotai_platform_token_expiry(const char *token, int64_t *expiry)
{
    if (!token || !expiry) return XIAOTAI_ERR_INVALID_ARG;
    const size_t length = strnlen(token, XIAOTAI_PLATFORM_BEARER_MAX);
    if (length == XIAOTAI_PLATFORM_BEARER_MAX) return XIAOTAI_ERR_INVALID_ARG;
    const char *payload = NULL, *signature = NULL;
    for (const char *p = token; *p; ++p) {
        if (*p == '.') {
            if (!payload) payload = p + 1;
            else if (!signature) signature = p + 1;
            else return XIAOTAI_ERR_NOT_FOUND;
        } else if (sextet((unsigned char)*p) < 0) return XIAOTAI_ERR_NOT_FOUND;
    }
    if (!payload || !signature || payload == token + 1 || !*signature || signature <= payload + 1 ||
        (signature - payload - 1) % 4 == 1) return XIAOTAI_ERR_NOT_FOUND;

    char json[(XIAOTAI_PLATFORM_BEARER_MAX * 3U) / 4U];
    size_t size = 0;
    unsigned bits = 0;
    uint32_t group = 0;
    for (const char *p = payload; p < signature - 1; ++p) {
        group = (group << 6) | (unsigned)sextet((unsigned char)*p);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            json[size++] = (char)((group >> bits) & 0xffU);
        }
    }
    const char *value;
    size_t bytes;
    int64_t parsed;
    const bool ok = platform_json_field(json, size, "exp", &value, &bytes) == XIAOTAI_OK &&
        platform_json_integer_read(value, bytes, &parsed) == XIAOTAI_OK && parsed > 0;
    xiaotai_identity_wipe(json, sizeof(json));
    if (!ok) return XIAOTAI_ERR_NOT_FOUND;
    *expiry = parsed;
    return XIAOTAI_OK;
}

int64_t platform_token_refresh_at(int64_t expiry, int64_t now)
{
    if (expiry <= now) return 0;
    const int64_t lead = (expiry - now) / 4;
    return expiry - (lead < 60 ? lead : 60);
}
