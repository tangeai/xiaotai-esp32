// SPDX-License-Identifier: MIT
#include "http_rules.h"
#include <string.h>

static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c;
}

static bool ascii_equal_n(const char *first, const char *second, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (ascii_lower((unsigned char)first[i]) != ascii_lower((unsigned char)second[i])) return false;
    return true;
}

static bool ascii_equal(const char *first, const char *second)
{
    const size_t n = strlen(first);
    return strlen(second) == n && ascii_equal_n(first, second, n);
}

static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool alpha(unsigned char c) { c = ascii_lower(c); return c >= 'a' && c <= 'z'; }
static bool hex(unsigned char c) { c = ascii_lower(c); return digit(c) || (c >= 'a' && c <= 'f'); }

static bool ipv4_tail(const char *p, const char *end)
{
    for (unsigned parts = 0; parts < 4; ++parts) {
        const char *first = p;
        unsigned value = 0;
        while (p < end && digit(*p)) {
            value = 10U * value + (unsigned)(*p++ - '0');
            if (value > 255U || p - first > 3) return false;
        }
        if (p == first || (p - first > 1 && *first == '0')) return false;
        if (parts == 3) return p == end;
        if (p == end || *p++ != '.') return false;
    }
    return false;
}

static bool ipv6_literal(const char *p, const char *end)
{
    unsigned groups = 0;
    bool compressed = false;
    if (p == end) return false;
    if (*p == ':') {
        if (end - p < 2 || p[1] != ':') return false;
        compressed = true;
        p += 2;
    }
    while (p < end) {
        const char *start = p;
        while (p < end && hex(*p)) ++p;
        if (p == start || p - start > 4) return false;
        if (p < end && *p == '.') {
            if (!ipv4_tail(start, end)) return false;
            groups += 2;
            break;
        }
        ++groups;
        if (p == end) break;
        if (*p++ != ':' || p == end) return false;
        if (*p == ':') {
            if (compressed) return false;
            compressed = true;
            ++p;
        }
    }
    return compressed ? groups < 8 : groups == 8;
}

static bool parse_origin(const char *url, http_origin_t *origin)
{
    if (!url) return false;
    const char *end_scheme = strchr(url, ':');
    if (!end_scheme || end_scheme[1] != '/' || end_scheme[2] != '/') return false;
    const size_t scheme_size = (size_t)(end_scheme - url);
    if (scheme_size == 4 && ascii_equal_n(url, "http", 4)) origin->tls = false;
    else if (scheme_size == 5 && ascii_equal_n(url, "https", 5)) origin->tls = true;
    else return false;
    const char *host = end_scheme + 3;
    const char *authority_end = host + strcspn(host, "/?#");
    const char *end_host = host;
    const char *zone = NULL;
    if (*host == '[') {
        const char *pos = host + 1;
        while (pos < authority_end && (hex(*pos) || *pos == '.' || *pos == ':')) ++pos;
        if (!ipv6_literal(host + 1, pos)) return false;
        if (pos < authority_end && *pos == '%') {
            zone = ++pos;
            while (pos < authority_end && (alpha(*pos) || digit(*pos) || *pos == '-' ||
                   *pos == '_' || *pos == '.' || *pos == '~' || *pos == '%')) ++pos;
            if (pos == zone) return false;
        }
        if (pos == authority_end || *pos != ']') return false;
        end_host = pos + 1;
    } else {
        while (end_host < authority_end &&
               (alpha(*end_host) || digit(*end_host) || *end_host == '-' || *end_host == '.')) ++end_host;
    }
    const size_t host_size = (size_t)(end_host - host);
    if (!host_size || host_size > HTTP_HOST_MAX) return false;

    uint32_t port = origin->tls ? 443U : 80U;
    if (end_host != authority_end) {
        const char *pos = end_host;
        if (*pos++ != ':' || pos == authority_end) return false;
        port = 0;
        for (; pos != authority_end; ++pos) {
            if (!digit(*pos)) return false;
            port = port * 10U + (unsigned)(*pos - '0');
            if (port > UINT16_MAX) return false;
        }
        if (!port) return false;
    }
    /* URL admission and rewriting are caller policy; reject only malformed
     * authority and bytes that could change the HTTP request line. */
    for (const unsigned char *p = (const unsigned char *)authority_end; *p; ++p)
        if (*p <= ' ' || *p >= 127 || *p == '\\') return false;
    for (size_t i = 0; i < host_size; ++i)
        origin->host[i] = zone && host + i >= zone ? host[i] : (char)ascii_lower((unsigned char)host[i]);
    origin->host[host_size] = '\0';
    origin->port = (uint16_t)port;
    return true;
}

static bool header_name(const char *name)
{
    if (!name || !*name) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!alpha(*p) && !digit(*p) && !strchr("!#$%&'*+-.^`_|~", *p)) return false;
    return true;
}

static bool header_value(const char *value)
{
    if (!value) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if ((*p < 32 && *p != '\t') || *p == 127) return false;
    return true;
}

void http_rules_reset(xiaotai_http_response_t *r)
{
    if (!r) return;
    r->status = 0;
    r->size = 0;
    r->content_length = -1;
    r->redirect = false;
    r->reused = false;
    r->kept = false;
    r->receive_fault = XIAOTAI_HTTP_RECEIVE_FAULT_NONE;
    r->stage = XIAOTAI_HTTP_STAGE_NONE;
    r->platform_error = 0;
    r->timing = (xiaotai_http_timing_t){0};
    if (!r->captures) return;
    for (size_t i = 0; i < r->capture_count; ++i) {
        xiaotai_http_capture_t *cap = &r->captures[i];
        cap->count = 0;
        cap->truncated = false;
        if (cap->value && cap->capacity) cap->value[0] = '\0';
    }
}

xiaotai_err_t http_rules_validate(const xiaotai_http_request_t *req, const xiaotai_http_response_t *res,
                                  http_origin_t *origin)
{
    if (!req || !res || !origin || !parse_origin(req->url, origin) ||
        req->method < XIAOTAI_HTTP_GET || req->method > XIAOTAI_HTTP_DELETE ||
        !req->timeout_ms || req->timeout_ms > 600000U ||
        (!req->body && req->body_size) || (!req->source && req->source_size) ||
        (req->source && req->body) ||
        (req->method == XIAOTAI_HTTP_GET && (req->body || req->source)) ||
        (req->header_count && !req->headers) || (res->capture_count && !res->captures) ||
        (res->body && res->sink)) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < req->header_count; ++i)
        if (!header_name(req->headers[i].name) || !header_value(req->headers[i].value))
            return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < res->capture_count; ++i) {
        const xiaotai_http_capture_t *cap = &res->captures[i];
        if (!header_name(cap->name) || (!cap->value && cap->capacity))
            return XIAOTAI_ERR_INVALID_ARG;
    }
    return XIAOTAI_OK;
}

bool http_rules_same_origin(const http_origin_t *a, const http_origin_t *b)
{
    return a->tls == b->tls && a->port == b->port && strcmp(a->host, b->host) == 0;
}

static bool close_token(const char *value)
{
    while (*value) {
        while (*value == ' ' || *value == '\t' || *value == ',') ++value;
        const char *end = strchr(value, ',');
        const char *tail = end ? end : value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) --tail;
        if (tail - value == 5 && ascii_equal_n(value, "close", 5)) return true;
        if (!end) break;
        value = end + 1;
    }
    return false;
}

void http_rules_header(xiaotai_http_response_t *r, const char *name, const char *value, bool *peer_closes)
{
    if (!name || !value) return;
    if (ascii_equal(name, "Location")) r->redirect = true;
    if (ascii_equal(name, "Connection") && close_token(value)) *peer_closes = true;
    for (size_t i = 0; i < r->capture_count; ++i) {
        xiaotai_http_capture_t *cap = &r->captures[i];
        if (!ascii_equal(cap->name, name)) continue;
        if (!cap->count) {
            const size_t n = strlen(value);
            cap->truncated = cap->capacity ? n >= cap->capacity : n != 0;
            if (cap->capacity) {
                const size_t copied = cap->truncated ? cap->capacity - 1 : n;
                memcpy(cap->value, value, copied);
                cap->value[copied] = '\0';
            }
        }
        if (cap->count != UINT32_MAX) ++cap->count;
    }
}

size_t http_rules_receive_limit(const xiaotai_http_response_t *r)
{
    return r->sink ? r->sink_limit : r->body ? r->capacity : SIZE_MAX;
}

xiaotai_err_t http_rules_consume(xiaotai_http_response_t *r, const uint8_t *data, size_t size)
{
    if (!size || (!r->body && !r->sink)) return XIAOTAI_OK;
    const size_t limit = http_rules_receive_limit(r);
    if (r->size > limit || size > limit - r->size) return XIAOTAI_ERR_INVALID_SIZE;
    if (r->sink) {
        const xiaotai_err_t rc = r->sink(r->sink_ctx, data, size);
        if (rc != XIAOTAI_OK) return rc;
    } else memcpy(r->body + r->size, data, size);
    r->size += size;
    return XIAOTAI_OK;
}

xiaotai_err_t http_rules_source(const xiaotai_http_request_t *r, uint8_t *data, size_t remaining, size_t *size)
{
    const size_t cap = remaining < HTTP_CHUNK_MAX ? remaining : HTTP_CHUNK_MAX;
    *size = 0;
    const xiaotai_err_t rc = r->source(r->source_ctx, data, cap, size);
    if (rc != XIAOTAI_OK) return rc;
    if (!*size) return XIAOTAI_FAIL;
    return *size <= cap ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE;
}

uint32_t http_rules_io_budget(const xiaotai_http_request_t *r, uint32_t elapsed)
{
    const uint32_t left = elapsed < r->timeout_ms ? r->timeout_ms - elapsed : 0;
    return r->io_timeout_ms && r->io_timeout_ms < left ? r->io_timeout_ms : left;
}
