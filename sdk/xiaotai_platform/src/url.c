// SPDX-License-Identifier: MIT
/* Service address admission. Deployed firmware checked every address with the
 * HTTP client's URL parser in its strict mode; the grammar below is that
 * parser's, state for state, so the same addresses pass and the same fail:
 *   - the authority (after "//", up to the first '/') is not empty, holds only
 *     user-information characters, '[', ']' and '@' (never two '@' in a row);
 *   - read again as [userinfo@]host[:port]: the host is letters, digits, '.'
 *     and '-', or "[...]" of hex digits, ':' and '.', optionally "%zone"; the
 *     port is one or more digits up to 65535 (leading zeros allowed); an
 *     address may not end inside userinfo, a bracket or after ':';
 *   - path bytes are printable ASCII except '#' and '?'.
 * On top of the parser, the project's policy refuses user information (any
 * userinfo character before an '@'; an empty one, "http://@host", passed the
 * deployed check and still does), port 0, and blanks, control characters,
 * DEL, '?', '#' and backslash anywhere. */
#include <string.h>
#include "platform_internal.h"

static bool is_alpha(unsigned char c)
{
    unsigned char lower = (unsigned char)(c | 0x20U);
    return lower >= 'a' && lower <= 'z';
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool is_alnum(unsigned char c) { return is_alpha(c) || is_digit(c); }

static bool is_hex(unsigned char c)
{
    unsigned char lower = (unsigned char)(c | 0x20U);
    return is_digit(c) || (lower >= 'a' && lower <= 'f');
}

static bool is_mark(unsigned char c)
{
    return c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' ||
           c == ')';
}

static bool is_userinfo(unsigned char c)
{
    return is_alnum(c) || is_mark(c) || c == '%' || c == ';' || c == ':' || c == '&' || c == '=' || c == '+' ||
           c == '$' || c == ',';
}

static bool is_host(unsigned char c) { return is_alnum(c) || c == '.' || c == '-'; }

/* Path bytes of the strict parser: printable ASCII; '?' and '#' would start
 * a query or fragment and are refused before this is consulted. */
static bool is_path(unsigned char c) { return c > 32U && c < 127U && c != '#' && c != '?'; }

/* Blanks, control characters, DEL, '?', '#' and backslash anywhere. */
static bool policy_bytes(const char *url, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 32U || c == 127U || c == '?' || c == '#' || c == '\\') {
            return false;
        }
    }
    return true;
}

typedef enum {
    HOST_DEAD = 0,
    USERINFO_START,
    USERINFO,
    HOST_START,
    HOST,
    HOST_V6_START,
    HOST_V6,
    HOST_V6_END,
    HOST_V6_ZONE_START,
    HOST_V6_ZONE,
    PORT_START,
    PORT,
} host_state_t;

static host_state_t host_step(host_state_t s, unsigned char c)
{
    switch (s) {
    case USERINFO:
    case USERINFO_START:
        if (c == '@') return HOST_START;
        if (is_userinfo(c)) return USERINFO;
        break;
    case HOST_START:
        if (c == '[') return HOST_V6_START;
        if (is_host(c)) return HOST;
        break;
    case HOST:
        /* ':' after a host or a closed IPv6 literal starts the port. */
        if (is_host(c)) return HOST;
        /* fall through */
    case HOST_V6_END:
        if (c == ':') return PORT_START;
        break;
    case HOST_V6:
        if (c == ']') return HOST_V6_END;
        /* fall through */
    case HOST_V6_START:
        if (is_hex(c) || c == ':' || c == '.') return HOST_V6;
        if (s == HOST_V6 && c == '%') return HOST_V6_ZONE_START;
        break;
    case HOST_V6_ZONE:
        if (c == ']') return HOST_V6_END;
        /* fall through */
    case HOST_V6_ZONE_START:
        if (is_alnum(c) || c == '%' || c == '.' || c == '-' || c == '_' || c == '~') return HOST_V6_ZONE;
        break;
    case PORT:
    case PORT_START:
        if (is_digit(c)) return PORT;
        break;
    default:
        break;
    }
    return HOST_DEAD;
}

/* rest: everything after "<scheme>://". */
static bool authority_and_path_valid(const char *rest, size_t length)
{
    size_t authority = 0;
    while (authority < length && rest[authority] != '/') {
        ++authority;
    }
    /* A scheme without a host is refused ("http:///x"). */
    if (authority == 0) {
        return false;
    }
    bool found_at = false;
    for (size_t i = 0; i < authority; ++i) {
        unsigned char c = (unsigned char)rest[i];
        if (c == '@') {
            if (i > 0 && rest[i - 1] == '@') {
                return false;
            }
            found_at = true;
        } else if (!is_userinfo(c) && c != '[' && c != ']') {
            return false;
        }
    }

    host_state_t s = found_at ? USERINFO_START : HOST_START;
    bool userinfo = false;
    bool has_port = false;
    unsigned long port = 0;
    for (size_t i = 0; i < authority; ++i) {
        host_state_t next = host_step(s, (unsigned char)rest[i]);
        if (next == HOST_DEAD) {
            return false;
        }
        if (next == USERINFO) {
            userinfo = true;
        } else if (next == PORT) {
            if (s != PORT) {
                has_port = true;
                port = 0;
            }
            /* Saturates above 65535 like the parser's strtoul check. */
            if (port <= 0xFFFFUL) {
                port = port * 10UL + (unsigned long)(rest[i] - '0');
            }
        }
        s = next;
    }
    switch (s) {
    case HOST_START:
    case HOST_V6_START:
    case HOST_V6:
    case HOST_V6_ZONE_START:
    case HOST_V6_ZONE:
    case PORT_START:
    case USERINFO:
    case USERINFO_START:
        return false;
    default:
        break;
    }
    if (has_port && port > 0xFFFFUL) {
        return false;
    }
    /* Project policy on top of the parser. */
    if (userinfo || (has_port && port == 0)) {
        return false;
    }
    for (size_t i = authority; i < length; ++i) {
        if (!is_path((unsigned char)rest[i])) {
            return false;
        }
    }
    return true;
}

static size_t prefix_of(const char *url, const char *const *schemes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        size_t n = strlen(schemes[i]);
        size_t k = 0;
        while (k < n) {
            unsigned char c = (unsigned char)url[k];
            if (c >= 'A' && c <= 'Z') {
                c = (unsigned char)(c - 'A' + 'a');
            }
            if (c != (unsigned char)schemes[i][k]) {
                break;
            }
            ++k;
        }
        if (k == n) {
            return n;
        }
    }
    return 0;
}

static void clear(char *out, size_t capacity)
{
    if (out && capacity > 0) {
        out[0] = '\0';
    }
}

xiaotai_err_t platform_url_http(const char *url, bool trim, char *out, size_t capacity)
{
    static const char *const schemes[] = {"https://", "http://"};
    if (!out || capacity == 0) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!url) {
        clear(out, capacity);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    size_t length = strlen(url);
    if (length >= capacity) {
        clear(out, capacity);
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    size_t prefix = prefix_of(url, schemes, 2);
    if (prefix == 0 || !policy_bytes(url, length) || !authority_and_path_valid(url + prefix, length - prefix)) {
        clear(out, capacity);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    /* "http://" is 7 bytes and never longer than the prefix it replaces, so
     * the rewrite fits wherever the input did and works in place. */
    size_t rest = length - prefix;
    memmove(out + 7, url + prefix, rest + 1);
    memcpy(out, "http://", 7);
    size_t written = 7 + rest;
    if (trim) {
        while (written > 7 && out[written - 1] == '/') {
            out[--written] = '\0';
        }
    }
    return XIAOTAI_OK;
}

bool platform_url_mqtt_valid(const char *url)
{
    static const char *const schemes[] = {"mqtts://", "mqtt://"};
    if (!url) {
        return false;
    }
    size_t length = strlen(url);
    size_t prefix = prefix_of(url, schemes, 2);
    return prefix != 0 && policy_bytes(url, length) && authority_and_path_valid(url + prefix, length - prefix);
}

xiaotai_err_t platform_url_mqtt(const char *url, bool unchecked, char *out, size_t capacity)
{
    if (!out || capacity == 0) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!url || url[0] == '\0') {
        clear(out, capacity);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    size_t length = strlen(url);
    if (length >= capacity) {
        clear(out, capacity);
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    if (!unchecked && !platform_url_mqtt_valid(url)) {
        clear(out, capacity);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    memmove(out, url, length + 1);
    return XIAOTAI_OK;
}

bool platform_url_path_valid(const char *path)
{
    if (!path || path[0] != '/') {
        return false;
    }
    size_t i = 0;
    for (; path[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (i + 1 >= XIAOTAI_PLATFORM_PATH_MAX || c <= 32U || c >= 127U || c == '#') {
            return false;
        }
    }
    return true;
}

xiaotai_err_t platform_url_join(const char *base, const char *path, char *out, size_t capacity)
{
    if (!base || !path || !out || capacity == 0) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    size_t a = strlen(base);
    size_t b = strlen(path);
    if (a + b >= capacity) {
        out[0] = '\0';
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    memmove(out, base, a);
    memcpy(out + a, path, b + 1);
    return XIAOTAI_OK;
}
