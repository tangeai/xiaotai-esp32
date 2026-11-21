// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_captive_portal_contract.py
#include "net_platform.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

#define AP_URL "http://192.168.6.1"
#define NET_FD 17
#define PORTAL_FD 12
static uint32_t ap, station;
static int server_token, starts, stops, registrations, fail_registration;
static bool live, external, start_fails, stop_fails, stopping;
static httpd_config_t applied;
static httpd_uri_t routes[2];
typedef enum { LOCAL_AP, LOCAL_STATION, LOCAL_MAPPED, LOCAL_V6, LOCAL_FAIL, LOCAL_SHORT } local_kind_t;
static local_kind_t local;
static const char *origin;
static const char *status, *content_type;
static const char *header_names[8], *header_values[8];
static size_t response_headers;
static unsigned chunks, ends, fail_chunk;
static char response[64];
static size_t response_size;
static const char *const body = "ABCDE";
static size_t body_offset;
static bool body_timeout, body_closed;

bool xiaotai_mem_has_external(void) { return external; }

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *cfg)
{
    ++starts;
    applied = *cfg;
    if (start_fails) return ESP_FAIL;
    assert(!live);
    live = true;
    registrations = 0;
    *server = &server_token;
    assert(applied.open_fn(*server, PORTAL_FD) == ESP_FAIL);
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t server)
{
    assert(server == &server_token && live);
    ++stops;
    stopping = true;
    assert(applied.open_fn(server, PORTAL_FD) == ESP_FAIL);
    stopping = false;
    if (stop_fails) return ESP_FAIL;
    live = false;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route)
{
    assert(server == &server_token && live && registrations < 2);
    if (fail_registration == registrations + 1) return XIAOTAI_ERR_NO_MEM;
    routes[registrations++] = *route;
    return ESP_OK;
}

bool httpd_uri_match_wildcard(const char *pattern, const char *uri, size_t len)
{
    return !strcmp(pattern, "/*") && len > 0 && uri[0] == '/';
}

int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return PORTAL_FD; }

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    (void)r;
    assert(!strcmp(field, "Origin"));
    return origin ? strlen(origin) : 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *value, size_t size)
{
    (void)r;
    assert(!strcmp(field, "Origin"));
    if (!origin) return ESP_ERR_NOT_FOUND;
    size_t len = strlen(origin);
    if (len >= size) {
        memcpy(value, origin, size - 1);
        value[size - 1] = 0;
        /* Match IDF guards: length is authoritative even at an exact boundary. */
        return len == size ? ESP_OK : XIAOTAI_ERR_INVALID_SIZE;
    }
    memcpy(value, origin, len + 1);
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *r, char *data, size_t len)
{
    if (body_timeout) {
        body_timeout = false;
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    if (body_closed) return 0;
    assert(body_offset + len <= r->content_len && len <= strlen(body) - body_offset);
    size_t got = len > 2 ? 2 : len;
    memcpy(data, body + body_offset, got);
    body_offset += got;
    return (int)got;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *value) { (void)r; status = value; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *value) { (void)r; content_type = value; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *name, const char *value)
{
    (void)r;
    assert(response_headers < 8);
    header_names[response_headers] = name;
    header_values[response_headers++] = value;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *data, ssize_t len)
{
    (void)r;
    assert(status && !strcmp(content_type, "text/plain"));
    assert(response_headers == 2);
    assert(!strcmp(header_names[0], "Cache-Control") && !strcmp(header_values[0], "no-store"));
    assert(!strcmp(header_names[1], "X-Fixture") && !strcmp(header_values[1], "copied"));
    if (!len) {
        assert(!data && !ends);
        ++ends;
        return ESP_OK;
    }
    if (++chunks == fail_chunk) return ESP_FAIL;
    assert(len > 0 && response_size + (size_t)len < sizeof(response));
    memcpy(response + response_size, data, (size_t)len);
    response_size += (size_t)len;
    return ESP_OK;
}

int portal_test_getsockname(int fd, struct sockaddr *address, socklen_t *len)
{
    assert(fd == PORTAL_FD);
    if (local == LOCAL_FAIL) return -1;
    if (local == LOCAL_MAPPED || local == LOCAL_V6) {
        assert(*len >= sizeof(struct sockaddr_in6));
        struct sockaddr_in6 v6 = {.sin6_family = AF_INET6};
        if (local == LOCAL_MAPPED) {
            v6.sin6_addr.s6_addr[10] = v6.sin6_addr.s6_addr[11] = 0xff;
            memcpy(v6.sin6_addr.s6_addr + 12, &ap, 4);
        } else v6.sin6_addr.s6_addr[15] = 1;
        memcpy(address, &v6, sizeof(v6));
        *len = sizeof(v6);
    } else {
        assert(*len >= sizeof(struct sockaddr_in));
        struct sockaddr_in v4 = {.sin_family = AF_INET,
            .sin_addr.s_addr = local == LOCAL_STATION ? station : ap};
        memcpy(address, &v4, sizeof(v4));
        *len = local == LOCAL_SHORT ? 2 : sizeof(v4);
    }
    return 0;
}

static int udp_opens, udp_closes, udp_bind, udp_opt, udp_fail_step;
static bool udp_bound;
static int receive_errno, send_errno;
static bool udp_truncated, udp_bad_family;
static uint16_t expected_port;
static uint32_t expected_ip;
static struct sockaddr_in last_peer;

int portal_test_socket(int domain, int type, int protocol)
{
    assert(domain == AF_INET && type == SOCK_DGRAM && protocol == IPPROTO_UDP);
    ++udp_opens;
    udp_opt = 0;
    return udp_fail_step == 1 ? -1 : NET_FD;
}

int portal_test_setsockopt(int fd, int level, int name, const void *arg, socklen_t len)
{
    assert(fd == NET_FD && level == SOL_SOCKET);
    ++udp_opt;
    if (udp_opt == 1) assert(name == SO_REUSEADDR && len == sizeof(int) && *(const int *)arg == 1);
    else {
        assert(name == (udp_opt == 2 ? SO_RCVTIMEO : SO_SNDTIMEO) && len == sizeof(struct timeval));
        const struct timeval *timeout = arg;
        assert(timeout->tv_sec == 1 && timeout->tv_usec == 235000);
    }
    return (udp_opt == 2 && udp_fail_step == 2) || (udp_opt == 3 && udp_fail_step == 3) ? -1 : 0;
}

int portal_test_bind(int fd, const struct sockaddr *address, socklen_t len)
{
    assert(fd == NET_FD && len == sizeof(struct sockaddr_in));
    const struct sockaddr_in *input = (const struct sockaddr_in *)address;
    assert(input->sin_family == AF_INET && ntohs(input->sin_port) == expected_port &&
           input->sin_addr.s_addr == expected_ip);
    ++udp_bind;
    if (udp_fail_step == 4) return -1;
    udp_bound = true;
    return 0;
}

ssize_t portal_test_recvmsg(int fd, struct msghdr *msg, int flags)
{
    assert(fd == NET_FD && udp_bound && !flags && msg->msg_iovlen == 1);
    if (receive_errno) {
        errno = receive_errno;
        return -1;
    }
    struct sockaddr_in from = {.sin_family = udp_bad_family ? AF_INET6 : AF_INET,
        .sin_port = htons(4321), .sin_addr.s_addr = station};
    assert(msg->msg_namelen == sizeof(from) && msg->msg_iov[0].iov_len >= 3);
    memcpy(msg->msg_name, &from, sizeof(from));
    memcpy(msg->msg_iov[0].iov_base, "UDP", 3);
    msg->msg_flags = udp_truncated ? MSG_TRUNC : 0;
    return 3;
}

ssize_t portal_test_sendto(int fd, const void *data, size_t size, int flags,
                           const struct sockaddr *address, socklen_t len)
{
    assert(fd == NET_FD && udp_bound && !flags && len == sizeof(last_peer));
    assert(size == 3 && !memcmp(data, "UDP", 3));
    memcpy(&last_peer, address, len);
    if (send_errno) {
        errno = send_errno;
        return -1;
    }
    return 3;
}

int portal_test_close(int fd)
{
    assert(fd == NET_FD);
    ++udp_closes;
    udp_bound = false;
    return 0;
}

typedef struct {
    const char *path, *method, *expected_origin;
    int status;
    bool allowed, read_body;
} request_case_t;

static void engine_handler(const net_request_t *req, net_exchange_t *ex, void *ctx)
{
    const request_case_t *expect = ctx;
    assert(!strcmp(req->path, expect->path) && !strcmp(req->method, expect->method) &&
           req->local_is_ap == expect->allowed);
    assert((!req->origin && !expect->expected_origin) ||
           (req->origin && expect->expected_origin && !strcmp(req->origin, expect->expected_origin)));
    assert(net_platform_reply_chunk(ex, "early", 5) == XIAOTAI_ERR_INVALID_STATE);
    if (expect->read_body) {
        uint8_t data[8];
        size_t n = 42;
        assert(req->content_length == 5);
        assert(net_platform_body_read(ex, data, 8, &n) == XIAOTAI_ERR_TIMEOUT && n == 0);
        assert(net_platform_body_read(ex, data, 8, &n) == XIAOTAI_OK && n == 2 && !memcmp(data, "AB", 2));
        assert(net_platform_body_read(ex, data, 8, &n) == XIAOTAI_OK && n == 2 && !memcmp(data, "CD", 2));
        assert(net_platform_body_read(ex, data, 8, &n) == XIAOTAI_OK && n == 1 && data[0] == 'E');
        assert(net_platform_body_read(ex, data, 8, &n) == XIAOTAI_OK && n == 0);
    }
    if (body_closed) {
        uint8_t data[4];
        size_t n = 42;
        assert(net_platform_body_read(ex, data, 4, &n) == XIAOTAI_FAIL && n == 0);
    }
    char cache[] = "Cache-Control: no-store", fixture[] = "X-Fixture:\tcopied", mime[] = "text/plain";
    const char *const headers[] = {cache, fixture};
    const char *const invalid[] = {"x: something\r\nother: header"};
    assert(net_platform_reply_begin(ex, expect->status, mime, invalid, 1) == XIAOTAI_ERR_INVALID_ARG);
    assert(!status && response_headers == 0);
    assert(net_platform_reply_begin(ex, expect->status, mime, headers, 2) == XIAOTAI_OK);
    assert(net_platform_reply_begin(ex, expect->status, mime, headers, 2) == XIAOTAI_ERR_INVALID_STATE);
    memset(cache, 'X', sizeof(cache));
    memset(fixture, 'X', sizeof(fixture));
    memset(mime, 'X', sizeof(mime));
    assert(net_platform_reply_chunk(ex, NULL, 0) == XIAOTAI_OK && !ends);
    assert(net_platform_reply_chunk(ex, "aa", 2) == ESP_OK);
    if (fail_chunk == 2) {
        assert(net_platform_reply_chunk(ex, "b", 1) == ESP_FAIL);
        assert(net_platform_reply_end(ex) == XIAOTAI_ERR_INVALID_STATE);
    } else {
        assert(net_platform_reply_chunk(ex, "b", 1) == ESP_OK);
        assert(net_platform_reply_end(ex) == ESP_OK);
        assert(net_platform_reply_end(ex) == XIAOTAI_ERR_INVALID_STATE);
    }
}

static void exchange_once(const request_case_t *expect, const char *uri, httpd_method_t method)
{
    status = content_type = NULL;
    response_headers = response_size = body_offset = chunks = ends = 0;
    body_timeout = expect->read_body;
    httpd_req_t req = {.method = method, .content_len = expect->read_body || body_closed ? 5 : 0};
    assert(strlen(uri) < sizeof(req.uri));
    strcpy(req.uri, uri);
    const size_t route = method == HTTP_GET ? 0 : 1;
    assert(applied.uri_match_fn(routes[route].uri, uri, strlen(uri)));
    assert(routes[route].handler(&req) == (fail_chunk ? ESP_FAIL : ESP_OK));
    assert(routes[route].method == method && chunks == 2);
    assert(ends == (fail_chunk ? 0U : 1U) && response_size == (fail_chunk ? 2U : 3U));
    assert(!memcmp(response, "aab", response_size));
}

static void server(void)
{
    request_case_t ctx = {"/api/wifi", "POST", AP_URL, 200, true, true};
    assert(net_platform_server_start(0, 8192, engine_handler, &ctx) == XIAOTAI_ERR_INVALID_ARG);
    start_fails = true;
    assert(net_platform_server_start(ap, 8192, engine_handler, &ctx) == ESP_FAIL);
    start_fails = false;
    fail_registration = 2;
    assert(net_platform_server_start(ap, 8192, engine_handler, &ctx) == XIAOTAI_ERR_NO_MEM);
    assert(!live && stops == 1);
    stop_fails = true;
    assert(net_platform_server_start(ap, 8192, engine_handler, &ctx) == ESP_FAIL);
    assert(live && applied.open_fn(&server_token, PORTAL_FD) == ESP_FAIL);
    assert(net_platform_server_start(ap, 8192, engine_handler, &ctx) == XIAOTAI_ERR_INVALID_STATE);
    stop_fails = false;
    assert(net_platform_server_stop() == ESP_OK && !live);
    fail_registration = 0;
    external = true;
    assert(net_platform_server_start(ap, 8192, engine_handler, &ctx) == ESP_OK);
    assert(applied.task_caps == (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM) &&
           applied.stack_size == 8192 && applied.max_open_sockets == 4 && applied.max_uri_handlers == 2 &&
           applied.recv_wait_timeout == 1 && applied.send_wait_timeout == 1 && applied.lru_purge_enable);
    const int once = starts;
    assert(net_platform_server_start(ap, 8192, engine_handler, &ctx) == ESP_OK && starts == once);
    assert(net_platform_server_start(station, 8192, engine_handler, &ctx) == XIAOTAI_ERR_INVALID_STATE);
    const local_kind_t local_options[] = {LOCAL_AP, LOCAL_STATION, LOCAL_MAPPED, LOCAL_V6, LOCAL_FAIL, LOCAL_SHORT};
    for (size_t i = 0; i < sizeof(local_options) / sizeof(local_options[0]); ++i) {
        local = local_options[i];
        const bool ok = local == LOCAL_AP || (CONFIG_LWIP_IPV6 && local == LOCAL_MAPPED);
        assert(applied.open_fn(&server_token, PORTAL_FD) == (ok ? ESP_OK : ESP_FAIL));
    }
    local = LOCAL_AP;
    origin = AP_URL;
    exchange_once(&ctx, "/api/wifi?next=portal", HTTP_POST);
    local = LOCAL_STATION; /* AP-local changed after acceptance. */
    ctx = (request_case_t){"/api/wifi", "POST", NULL, 403, false, false};
    origin = NULL;
    exchange_once(&ctx, "/api/wifi", HTTP_POST);
    local = LOCAL_AP;
    ctx = (request_case_t){"/api/wifi", "POST", "", 403, true, false};
    origin = AP_URL "/extra";
    exchange_once(&ctx, "/api/wifi", HTTP_POST);
    origin = AP_URL "/";
    exchange_once(&ctx, "/api/wifi", HTTP_POST);
    origin = "";
    exchange_once(&ctx, "/api/wifi", HTTP_POST);
    ctx = (request_case_t){"/generate_204", "GET", NULL, 303, true, false};
    origin = NULL;
    fail_chunk = 2;
    exchange_once(&ctx, "/generate_204", HTTP_GET);
    fail_chunk = 0;
    ctx = (request_case_t){"/api/wifi", "POST", NULL, 400, true, false};
    body_closed = true;
    exchange_once(&ctx, "/api/wifi", HTTP_POST);
    body_closed = false;
    assert(net_platform_server_stop() == ESP_OK && !live && !stopping);
    external = false;
    assert(net_platform_server_start(ap, 7168, engine_handler, &ctx) == ESP_OK);
    assert(applied.task_caps == (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) && applied.stack_size == 7168);
    assert(net_platform_server_stop() == ESP_OK);
}

static void udp(void)
{
    expected_ip = ap;
    expected_port = 53;
    int out = 42;
    assert(net_platform_udp_open(ap, 53, 0, &out) == XIAOTAI_ERR_INVALID_ARG && out == -1);
    for (udp_fail_step = 1; udp_fail_step <= 4; ++udp_fail_step) {
        assert(net_platform_udp_open(ap, 53, 1235, &out) == XIAOTAI_FAIL && out == -1);
        assert(udp_closes == udp_fail_step - 1 && !udp_bound);
    }
    assert(net_platform_udp_open(ap, 53, 1235, &out) == XIAOTAI_OK && out == NET_FD && udp_bound);
    uint8_t data[16];
    net_peer_t peer = {0};
    receive_errno = EAGAIN;
    assert(net_platform_udp_receive(out, data, sizeof(data), &peer) == 0 && !peer.port);
    receive_errno = EBADF;
    assert(net_platform_udp_receive(out, data, sizeof(data), &peer) == -1 && !peer.port);
    receive_errno = 0;
    udp_truncated = true;
    assert(net_platform_udp_receive(out, data, sizeof(data), &peer) == 0 && !peer.port);
    udp_truncated = false;
    udp_bad_family = true;
    assert(net_platform_udp_receive(out, data, sizeof(data), &peer) == 0 && !peer.port);
    udp_bad_family = false;
    assert(net_platform_udp_receive(out, data, sizeof(data), &peer) == 3 && !memcmp(data, "UDP", 3) &&
           peer.port == 4321 && peer.ipv4 == station);
    assert(net_platform_udp_send(out, &peer, data, 3) == XIAOTAI_OK);
    assert(ntohs(last_peer.sin_port) == 4321 && last_peer.sin_addr.s_addr == station);
    send_errno = EAGAIN;
    assert(net_platform_udp_send(out, &peer, data, 3) == XIAOTAI_ERR_TIMEOUT);
    net_platform_udp_close(out);
    net_platform_udp_close(-1);
    assert(udp_closes == 4 && !udp_bound && udp_bind == 2 && udp_opens == 5);
}

/* A deterministic linker asset with a trailing non-NUL byte, not the product page. */
__asm__(".pushsection .rodata\n"
        ".globl _binary_setup_html_start\n"
        "_binary_setup_html_start:\n"
        ".ascii \"A<B>\"\n"
        ".globl _binary_setup_html_end\n"
        "_binary_setup_html_end:\n"
        ".byte 127\n"
        ".popsection\n");

int main(void)
{
    ap = htonl(0xc0a80601U);
    station = htonl(0xc0a80199U);
    size_t bytes = 0;
    const char *page = net_platform_page(&bytes);
    assert(bytes == 4 && !memcmp(page, "A<B>", bytes) && net_platform_page(NULL) == page);
    server();
    udp();
    return 0;
}
