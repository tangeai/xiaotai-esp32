// SPDX-License-Identifier: MIT
#include "portal.h"
#include "store.h"
#include <string.h>

typedef struct {
    const uint8_t *at, *end;
} reader_t;

static void whitespace(reader_t *r)
{
    while (r->at < r->end && (*r->at == ' ' || *r->at == '\t' || *r->at == '\n' || *r->at == '\r'))
        ++r->at;
}

static bool take(reader_t *r, uint8_t byte)
{
    whitespace(r);
    if (r->at == r->end || *r->at != byte) return false;
    ++r->at;
    return true;
}

static int nibble(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t utf8_span(const uint8_t *p, size_t count)
{
    const unsigned first = p[0];
    if (first < 0x80U) return 1;
    const size_t span = first >= 0xC2U && first <= 0xDFU ? 2U :
                        first >= 0xE0U && first <= 0xEFU ? 3U :
                        first >= 0xF0U && first <= 0xF4U ? 4U : 0U;
    if (!span || count < span) return 0;
    for (size_t i = 1; i < span; ++i) {
        if ((p[i] & 0xC0U) != 0x80U) return 0;
    }
    if ((first == 0xE0U && p[1] < 0xA0U) || (first == 0xEDU && p[1] >= 0xA0U) ||
        (first == 0xF0U && p[1] < 0x90U) || (first == 0xF4U && p[1] >= 0x90U)) return 0;
    return span;
}

static bool hex4(reader_t *r, uint32_t *value)
{
    if ((size_t)(r->end - r->at) < 4) return false;
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int n = nibble(*r->at++);
        if (n < 0) return false;
        *value = (*value << 4) | (unsigned)n;
    }
    return true;
}

static size_t escape(reader_t *r, uint8_t bytes[4])
{
    if (r->at == r->end) return 0;
    const uint8_t ch = *r->at++;
    switch (ch) {
    case '"': case '\\': case '/': bytes[0] = ch; return 1;
    case 'b': bytes[0] = '\b'; return 1;
    case 'f': bytes[0] = '\f'; return 1;
    case 'n': bytes[0] = '\n'; return 1;
    case 'r': bytes[0] = '\r'; return 1;
    case 't': bytes[0] = '\t'; return 1;
    case 'u': break;
    default: return 0;
    }
    uint32_t point;
    if (!hex4(r, &point) || !point) return 0;
    if (point >= 0xD800U && point <= 0xDBFFU) {
        if ((size_t)(r->end - r->at) < 2 || r->at[0] != '\\' || r->at[1] != 'u') return 0;
        r->at += 2;
        uint32_t low;
        if (!hex4(r, &low) || low < 0xDC00U || low > 0xDFFFU) return 0;
        point = 0x10000U + ((point - 0xD800U) << 10) + low - 0xDC00U;
    } else if (point >= 0xDC00U && point <= 0xDFFFU) {
        return 0;
    }
    if (point < 0x80U) { bytes[0] = (uint8_t)point; return 1; }
    const size_t span = point < 0x800U ? 2U : point < 0x10000U ? 3U : 4U;
    for (size_t i = span - 1; i > 0; --i) {
        bytes[i] = (uint8_t)(0x80U | (point & 0x3FU));
        point >>= 6;
    }
    bytes[0] = (uint8_t)((span == 2U ? 0xC0U : span == 3U ? 0xE0U : 0xF0U) | point);
    return span;
}

/* Validate the complete string even if the destination is deliberately
 * shorter. Length is checked after choosing ssid_hex over a display name. */
static bool string(reader_t *r, char *dst, size_t capacity, size_t *length)
{
    *length = 0;
    if (!take(r, '"')) return false;
    while (r->at < r->end && *r->at != '"') {
        uint8_t escaped[4];
        const uint8_t *bytes = r->at;
        size_t span;
        if (*r->at == '\\') {
            ++r->at;
            bytes = escaped;
            span = escape(r, escaped);
        } else {
            if (*r->at < 0x20U) return false;
            span = utf8_span(r->at, (size_t)(r->end - r->at));
            r->at += span;
        }
        if (!span) return false;
        for (size_t i = 0; i < span; ++i) {
            if (*length + 1U < capacity) dst[*length] = (char)bytes[i];
            ++*length;
        }
    }
    if (r->at == r->end) return false;
    ++r->at;
    if (capacity) dst[*length < capacity ? *length : capacity - 1U] = '\0';
    return true;
}

static bool boolean(reader_t *r, bool *value)
{
    whitespace(r);
    const size_t left = (size_t)(r->end - r->at);
    if (left >= 4 && !memcmp(r->at, "true", 4)) {
        *value = true;
        r->at += 4;
    } else if (left >= 5 && !memcmp(r->at, "false", 5)) {
        *value = false;
        r->at += 5;
    } else return false;
    return true;
}

static bool decode_hex(const char *hex, size_t length, char *ssid)
{
    if (!length || (length & 1U) || length >= NET_PORTAL_HEX_MAX) return false;
    for (size_t i = 0; i < length; i += 2) {
        const int high = nibble((uint8_t)hex[i]), low = nibble((uint8_t)hex[i + 1]);
        if (high < 0 || low < 0 || !(high || low)) return false;
        ssid[i / 2] = (char)(uint8_t)(high * 16 + low);
    }
    ssid[length / 2] = '\0';
    return true;
}

bool net_portal_json_credentials(const uint8_t *body, size_t size, xiaotai_net_submission_t *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    if (!body || !size || size > NET_PORTAL_BODY_MAX) return false;
    reader_t r = {body, body + size};
    enum { SSID = 1, HEX = 2, PASSWORD = 4, SAVED = 8 };
    unsigned seen = 0;
    char hex[NET_PORTAL_HEX_MAX];
    size_t ssid_len = 0, hex_len = 0, password_len = 0;
    if (!take(&r, '{')) return false;
    do {
        char key[12];
        size_t length;
        if (!string(&r, key, sizeof(key), &length) || !take(&r, ':')) goto invalid;
        unsigned field = 0;
        if (length < sizeof(key)) {
            if (!strcmp(key, "ssid")) field = SSID;
            else if (!strcmp(key, "ssid_hex")) field = HEX;
            else if (!strcmp(key, "password")) field = PASSWORD;
            else if (!strcmp(key, "use_saved")) field = SAVED;
        }
        if (seen & field) goto invalid;
        seen |= field;
        if (field == SSID) {
            if (!string(&r, result->credentials.ssid, sizeof(result->credentials.ssid), &ssid_len)) goto invalid;
        } else if (field == HEX) {
            if (!string(&r, hex, sizeof(hex), &hex_len)) goto invalid;
        } else if (field == PASSWORD) {
            if (!string(&r, result->credentials.password, sizeof(result->credentials.password), &password_len))
                goto invalid;
        } else if (field == SAVED) {
            if (!boolean(&r, &result->use_saved)) goto invalid;
        } else {
            bool ignored;
            whitespace(&r);
            if (r.at < r.end && *r.at == '"') {
                if (!string(&r, NULL, 0, &length)) goto invalid;
            } else if (!boolean(&r, &ignored)) goto invalid;
        }
    } while (take(&r, ','));
    if (!take(&r, '}')) goto invalid;
    whitespace(&r);
    if (r.at != r.end || (result->use_saved ? (seen & PASSWORD) != 0 : !(seen & PASSWORD)) ||
        password_len > XIAOTAI_NET_PASSWORD_MAX || (password_len && password_len < 8U)) goto invalid;
    if (seen & HEX) {
        if (!decode_hex(hex, hex_len, result->credentials.ssid)) goto invalid;
    } else if (!ssid_len || ssid_len > XIAOTAI_NET_SSID_MAX) goto invalid;
    result->method = XIAOTAI_PROVISION_PORTAL;
    return true;
invalid:
    net_store_wipe(result, sizeof(*result));
    return false;
}

void net_portal_json_ssid(const char *ssid, char text[NET_PORTAL_TEXT_MAX], char hex[NET_PORTAL_HEX_MAX])
{
    static const char digits[] = "0123456789abcdef";
    const uint8_t *bytes = (const uint8_t *)ssid;
    size_t count = 0;
    while (count < XIAOTAI_NET_SSID_MAX && bytes[count]) {
        hex[count * 2] = digits[bytes[count] >> 4];
        hex[count * 2 + 1] = digits[bytes[count] & 0xFU];
        ++count;
    }
    hex[count * 2] = '\0';
    size_t used = 0;
    text[used++] = '"';
    for (size_t i = 0; i < count;) {
        const uint8_t ch = bytes[i];
        const size_t span = utf8_span(bytes + i, count - i);
        if (!span) {
            memcpy(text + used, "\\ufffd", 6);
            used += 6;
            ++i;
        } else if (ch == '"' || ch == '\\') {
            text[used++] = '\\';
            text[used++] = (char)ch;
            ++i;
        } else if (ch < 0x20U || ch == '<' || ch == '>' || ch == '&') {
            memcpy(text + used, "\\u00", 4);
            text[used + 4] = digits[ch >> 4];
            text[used + 5] = digits[ch & 0xFU];
            used += 6;
            ++i;
        } else {
            memcpy(text + used, bytes + i, span);
            used += span;
            i += span;
        }
    }
    text[used++] = '"';
    text[used] = '\0';
}
