// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_s3_discovered_endpoint.py
// Replaces legacy: tests/regression/test_platform_http_trace.py
#include "xiaotai/port/http.h"
#include "xiaotai/port_config.h"
#include "xiaotai_mem_host.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "arch/sys_arch.h"
#include "http_parser.h"
#include "sdkconfig.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { SOCKET_FD = 12, DIAL = 1, HEADER = 2, BODY = 3 };
enum { NORMAL, TRICKLE_HEADER, STALLED_CONNECT, STALLED_BODY, STALE_WRITE, TLS_REJECT, RESET_BODY };
static struct {
    int64_t now, max_wait;
    int stage, mode, requests, sockets, closes, nodelay, tls_calls, dns_calls, interim;
    int bytes, status, duration, content_size, body_prefix;
    const char *wire_head;
    size_t wire_head_pos;
    unsigned wait_bar, cancel_bar;
    size_t maximum_write;
    bool readable, socket_error, open, close_asked, redirect, duplicate, silent_dns, no_tcpip;
    bool used_auth, used_type, used_extra, eof_after_header, fin_as_failure, chunked;
    bool reject_dns_post, queue_dns_post, expect_prewrite_nodelay;
    char host[64];
    xiaotai_http_method_t method;
} f;
static pthread_mutex_t waiter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waiter_cond = PTHREAD_COND_INITIALIZER;
static dns_found_callback delayed_dns[4];
static void *dns_args[4];
static unsigned delayed_count;
static void (*queued_dns)(void *);
static void *queued_arg;

int64_t __wrap_xiaotai_time_us(void) { return f.now; }
void __wrap_xiaotai_delay_ms(uint32_t ms) { f.now += ms * 1000; }

int ipaddr_aton(const char *s, ip_addr_t *ip) { return inet_pton(AF_INET, s, ip); }
char *ipaddr_ntoa_r(const ip_addr_t *ip, char *buf, int cap)
{
    return (char *)inet_ntop(AF_INET, ip, buf, (socklen_t)cap);
}
err_t tcpip_try_callback(void (*callback)(void *), void *ctx)
{
    if (f.reject_dns_post) return -1;
    if (f.queue_dns_post) {
        assert(!queued_dns);
        queued_dns = callback;
        queued_arg = ctx;
        return ERR_OK;
    }
    callback(ctx);
    return ERR_OK;
}
bool sys_thread_tcpip(sys_thread_core_lock_t state)
{
    assert(state == LWIP_CORE_IS_TCPIP_INITIALIZED);
    return !f.no_tcpip;
}
int http_parser_parse_url(const char *url, size_t len, int connect, struct http_parser_url *parsed)
{
    assert(url && strlen(url) == len && !connect && !parsed->field_set);
    return strstr(url, "/invalid-by-native") != NULL;
}
err_t dns_gethostbyname(const char *s, ip_addr_t *ip, dns_found_callback complete, void *ctx)
{
    assert(s[0]);
    ++f.dns_calls;
    f.now += 3000;
    if (f.silent_dns) {
        assert(delayed_count < 4);
        delayed_dns[delayed_count] = complete;
        dns_args[delayed_count++] = ctx;
        return ERR_INPROGRESS;
    }
    assert(inet_pton(AF_INET, "192.0.2.1", ip) == 1);
    return ERR_OK;
}

int lwip_socket(int domain, int type, int protocol)
{
    assert(domain == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP && !f.open);
    f.open = true;
    ++f.sockets;
    return SOCKET_FD;
}
int lwip_fcntl(int fd, int op, int flag)
{
    assert(fd == SOCKET_FD && op == F_SETFL && flag == O_NONBLOCK);
    return 0;
}
int lwip_connect(int fd, const struct sockaddr *sa, socklen_t len)
{
    assert(fd == SOCKET_FD && sa->sa_family == AF_INET && len == sizeof(struct sockaddr_in));
    f.stage = DIAL;
    errno = EINPROGRESS;
    return -1;
}
int lwip_select(int nc, fd_set *rd, fd_set *wr, fd_set *err, struct timeval *tv)
{
    assert(nc == SOCKET_FD + 1 && err && (!!rd != !!wr));
    FD_ZERO(err);
    int64_t us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    if (!us) {
        if (!f.readable) FD_ZERO(rd);
        return f.readable;
    }
    if (us > f.max_wait) f.max_wait = us;
    if ((f.mode == STALLED_CONNECT && f.stage == DIAL) ||
        (f.mode == STALLED_BODY && f.stage == BODY)) {
        assert(pthread_mutex_lock(&waiter_lock) == 0);
        if (f.wait_bar) {
            ++f.wait_bar;
            assert(pthread_cond_signal(&waiter_cond) == 0);
            while (!f.cancel_bar) assert(pthread_cond_wait(&waiter_cond, &waiter_lock) == 0);
        }
        assert(pthread_mutex_unlock(&waiter_lock) == 0);
        f.now += us;
        if (rd) FD_ZERO(rd);
        if (wr) FD_ZERO(wr);
        return 0;
    }
    assert(f.stage == DIAL && wr);
    f.now += 2000;
    return 1;
}
int lwip_getsockopt(int fd, int level, int option, void *err, socklen_t *size)
{
    assert(fd == SOCKET_FD && level == SOL_SOCKET && option == SO_ERROR && *size == sizeof(int));
    *(int *)err = f.socket_error ? ECONNRESET : 0;
    return 0;
}
int lwip_setsockopt(int fd, int level, int option, const void *value, socklen_t size)
{
    assert(fd == SOCKET_FD && level == IPPROTO_TCP && option == TCP_NODELAY && size == sizeof(int));
    assert(*(const int *)value == 1);
    ++f.nodelay;
    return 0;
}
int lwip_close(int fd)
{
    assert(fd == SOCKET_FD && f.open);
    f.open = false;
    ++f.closes;
    return 0;
}
ssize_t lwip_send(int fd, const void *data, size_t size, int flags)
{
    assert(fd == SOCKET_FD && data && flags == 0);
    if (f.expect_prewrite_nodelay) assert(f.sockets > 0 && f.nodelay == f.sockets);
    f.now += 1000;
    if (f.mode == STALE_WRITE) {
        errno = EPIPE;
        return -1;
    }
    if (size > f.maximum_write) f.maximum_write = size;
    return (ssize_t)size;
}
ssize_t lwip_recv(int fd, void *data, size_t size, int flags)
{
    assert(fd == SOCKET_FD && size && flags == 0);
    if (f.mode == STALLED_BODY && f.stage == BODY && (!f.body_prefix || f.bytes >= f.body_prefix)) {
        errno = EWOULDBLOCK;
        return -1;
    }
    f.now += 2000;
    if (f.stage == HEADER) {
        if (f.mode == TRICKLE_HEADER) {
            *(char *)data = 'H';
            return 1;
        }
        size_t remain = strlen(f.wire_head) - f.wire_head_pos;
        if (size > remain) size = remain;
        memcpy(data, f.wire_head + f.wire_head_pos, size);
        f.wire_head_pos += size;
        return (ssize_t)size;
    }
    assert(f.stage == BODY);
    if (f.eof_after_header) return 0;
    if (f.mode == RESET_BODY && f.bytes >= f.body_prefix) {
        errno = ECONNRESET;
        return -1;
    }
    int remaining = (f.body_prefix ? f.body_prefix : f.content_size) - f.bytes;
    if (remaining < (int)size) size = (size_t)remaining;
    memset(data, 'R', size);
    f.bytes += (int)size;
    return (ssize_t)size;
}

struct esp_tls { int stamp; };
esp_err_t esp_crt_bundle_attach(void *cfg) { (void)cfg; return ESP_OK; }
esp_tls_t *esp_tls_init(void) { return calloc(1, sizeof(esp_tls_t)); }
int esp_tls_conn_destroy(esp_tls_t *tls)
{
    if (tls->stamp > 1) lwip_close(SOCKET_FD);
    free(tls);
    return 0;
}
int esp_tls_conn_new_async(const char *target, int len, int port, const esp_tls_cfg_t *cfg, esp_tls_t *tls)
{
    assert(strcmp(target, "192.0.2.1") == 0 && len == 9 && port == 443);
    assert(strcmp(cfg->common_name, "edge.local") == 0 && cfg->crt_bundle_attach == esp_crt_bundle_attach);
    assert(cfg->non_block && cfg->timeout_ms == 1);
    ++f.tls_calls;
    f.now += 1000;
    if (f.mode == TLS_REJECT) return -1;
    if (++tls->stamp == 1) return 0;
    assert(lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) == SOCKET_FD);
    return 1;
}
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *out, size_t size)
{
    assert(tls->stamp > 1);
    ssize_t rc = lwip_recv(SOCKET_FD, out, size, 0);
    return rc < 0 ? ESP_TLS_ERR_SSL_WANT_READ : rc;
}
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t size)
{
    assert(tls->stamp > 1);
    return lwip_send(SOCKET_FD, data, size, 0);
}
ssize_t esp_tls_get_bytes_avail(esp_tls_t *tls) { assert(tls); return 0; }
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *fd)
{
    assert(tls->stamp > 1);
    *fd = SOCKET_FD;
    return ESP_OK;
}
esp_err_t esp_tls_get_error_handle(esp_tls_t *tls, esp_tls_error_handle_t *err) { *err = tls; return ESP_OK; }
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t err, int *code, int *flags)
{
    assert(err && !flags);
    *code = -0x1234;
    return ESP_FAIL;
}

struct esp_transport_item_t {
    void *ctx;
    connect_func connect;
    io_func write;
    io_read_func read;
    trans_func close;
};
esp_transport_handle_t esp_transport_init(void) { return calloc(1, sizeof(struct esp_transport_item_t)); }
esp_err_t esp_transport_destroy(esp_transport_handle_t t) { free(t); return ESP_OK; }
esp_err_t esp_transport_set_context_data(esp_transport_handle_t t, void *ctx) { t->ctx = ctx; return ESP_OK; }
void *esp_transport_get_context_data(esp_transport_handle_t t) { return t->ctx; }
esp_err_t esp_transport_set_func(esp_transport_handle_t t, connect_func co, io_read_func rd, io_func wr,
                                 trans_func cl, poll_func pr, poll_func pw, trans_func del)
{
    assert(!pr && !pw && !del);
    t->connect = co; t->read = rd; t->write = wr; t->close = cl;
    return ESP_OK;
}

struct native_http {
    esp_http_client_config_t config;
    void *data;
    unsigned headers;
    bool connected;
    int status, port;
    esp_http_client_method_t method;
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(cfg->transport && cfg->disable_auto_redirect && cfg->max_authorization_retries == -1);
    esp_http_client_handle_t h = calloc(1, sizeof(*h));
    assert(h);
    h->config = *cfg;
    assert(esp_http_client_set_url(h, cfg->url) == ESP_OK);
    return h;
}
esp_err_t esp_http_client_set_url(esp_http_client_handle_t h, const char *url)
{
    h->port = strncmp(url, "https://", 8) == 0 ? 443 : strstr(url, ":8080") ? 8080 : 80;
    return ESP_OK;
}
static unsigned header_bit(const char *name)
{
    const char *const names[] = {"Authorization", "Content-Type", "X-Test", "Host", "Connection", "Content-Length"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i)
        if (!strcasecmp(name, names[i])) return 1U << i;
    assert(false);
    return 0;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h, const char *name, const char *val)
{
    unsigned bit = header_bit(name);
    h->headers |= bit;
    if (bit == 8) {
        assert(strlen(val) < sizeof(f.host));
        strcpy(f.host, val);
    }
    return ESP_OK;
}
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t h, const char *name)
{
    unsigned bit = header_bit(name);
    int rc = (h->headers & bit) ? ESP_OK : ESP_ERR_NOT_FOUND;
    h->headers &= ~bit;
    return rc;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t h, esp_http_client_method_t m) { h->method = m; return ESP_OK; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t h, const char *body, int n)
{
    assert(!body && !n);
    return esp_http_client_delete_header(h, "Content-Type");
}
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t h, void *data) { h->data = data; return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t h)
{
    if (!h->connected) return ESP_OK;
    h->connected = false;
    return h->config.transport->close(h->config.transport);
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h)
{
    assert(!h->connected && h->data == NULL);
    free(h);
    return ESP_OK;
}
static void event(esp_http_client_handle_t h, esp_http_client_event_id_t event_id, char *key, char *value)
{
    esp_http_client_event_t ev = { .event_id = event_id, .user_data = h->data, .header_key = key, .header_value = value };
    assert(h->config.event_handler(&ev) == ESP_OK);
}
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int size)
{
    esp_transport_handle_t t = h->config.transport;
    assert(size >= 0);
    if (!h->connected) {
        if (t->connect(t, "unused.example", h->port, 99999) < 0) return ESP_FAIL;
        h->connected = true;
        event(h, HTTP_EVENT_ON_CONNECTED, NULL, NULL);
    }
    ++f.requests;
    f.wire_head_pos = 0;
    f.wire_head = f.interim ? "I\r\n\r\nF\r\n\r\n" : "F\r\n\r\n";
    f.used_auth = !!(h->headers & 1);
    f.used_type = !!(h->headers & 2);
    f.used_extra = !!(h->headers & 4);
    f.method = h->method == HTTP_METHOD_DELETE ? XIAOTAI_HTTP_DELETE : (xiaotai_http_method_t)h->method;
    if (size || h->method == HTTP_METHOD_POST || h->method == HTTP_METHOD_PUT) h->headers |= 32;
    f.bytes = 0;
    return t->write(t, "request head", 12, 99999) > 0 ? ESP_OK : ESP_FAIL;
}
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t h) { return h->connected; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t h)
{
    assert(h->status);
    return f.chunked;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h)
{
    return h->status && f.bytes == f.content_size;
}
int esp_http_client_get_status_code(esp_http_client_handle_t h) { return h->status; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t h)
{
    assert(h->status);
    return f.duration || f.chunked ? -1 : f.content_size;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h)
{
    esp_transport_handle_t t = h->config.transport;
    h->status = -1;
    f.stage = HEADER;
    char head[9] = {0};
    size_t used = 0;
    for (;;) {
        const size_t cap = sizeof(head) - 1 - used;
        int rc = t->read(t, head + used, (int)(cap ? cap : 1), 99999);
        if (rc <= 0) return rc == 0 ? -ESP_ERR_HTTP_EAGAIN : ESP_FAIL;
        if (f.mode == TRICKLE_HEADER) continue;
        used += (size_t)rc;
        if (used >= 5) break;
    }
    assert(used == 5 && !strcmp(head + 1, "\r\n\r\n"));
    if (f.interim) {
        assert(head[0] == 'I');
        --f.interim;
        h->status = 103;
        event(h, HTTP_EVENT_ON_HEADER, "Content-Type", "must-not-be-captured");
        return 0;
    }
    assert(head[0] == 'F');
    h->status = f.status;
    event(h, HTTP_EVENT_ON_HEADER, "Content-Type", "application/test");
    if (f.duplicate) event(h, HTTP_EVENT_ON_HEADER, "CONTENT-TYPE", "text/html");
    if (f.redirect) event(h, HTTP_EVENT_ON_HEADER, "Location", "http://other.local/never");
    if (f.close_asked) event(h, HTTP_EVENT_ON_HEADER, "Connection", "close");
    f.stage = BODY;
    return f.duration ? 0 : f.content_size;
}
int esp_http_client_write(esp_http_client_handle_t h, const char *data, int size)
{
    return h->config.transport->write(h->config.transport, data, size, 99999);
}
int esp_http_client_read(esp_http_client_handle_t h, char *data, int size)
{
    if (f.bytes == f.content_size) return 0;
    int rc = h->config.transport->read(h->config.transport, data, size, 99999);
    if (rc == ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN && f.fin_as_failure) return ESP_FAIL;
    return rc > 0 ? rc : rc == 0 ? -ESP_ERR_HTTP_EAGAIN : rc == -1 ? 0 : ESP_FAIL;
}

static const xiaotai_http_request_t read_request = {
    .url = "http://edge.local:8080/a", .timeout_ms = 100, .io_timeout_ms = 20, .keep_alive = true,
};
static xiaotai_http_t *setup(void)
{
    memset(&f, 0, sizeof(f));
    f.status = 200;
    f.content_size = 3;
    xiaotai_http_t *client;
    assert(xiaotai_http_create(&(xiaotai_http_options_t){.no_delay = true}, &client) == XIAOTAI_OK);
    assert(!f.open);
    return client;
}

static void reuse_and_isolation(void)
{
    xiaotai_http_t *c = setup();
    f.expect_prewrite_nodelay = true;
    xiaotai_http_request_t r = read_request;
    char data[16];
    xiaotai_http_response_t res = {.body = (uint8_t *)data, .capacity = sizeof(data)};
    const xiaotai_http_header_t headers[] = {
        {"Authorization", "test-fake"}, {"Content-Type", "text/plain"}, {"X-Test", "test"},
    };
    r.method = XIAOTAI_HTTP_POST; r.headers = headers; r.header_count = 3; r.body = "hello"; r.body_size = 5;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK);
    assert(res.status == 200 && res.size == 3 && res.kept && !res.reused);
    assert(res.timing.resolve_ms == 3 && res.timing.connect_ms == 2 && res.timing.send_ms == 2 &&
           res.timing.wait_ms == 2 && res.timing.receive_ms == 2 && res.timing.total_ms == 11);
    assert(f.used_auth && f.used_type && f.used_extra && f.nodelay == 1 && !strcmp(f.host, "edge.local:8080"));
    assert(xiaotai_http_idle_connection(c));
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_OK);
    assert(res.reused && res.kept && !res.timing.resolve_ms && !res.timing.connect_ms);
    assert(!f.used_auth && !f.used_type && !f.used_extra && f.method == XIAOTAI_HTTP_GET && f.sockets == 1);
    r = read_request; r.url = "http://other.local:8080/b"; r.method = XIAOTAI_HTTP_DELETE;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK && !res.reused && res.kept);
    assert(f.sockets == 2 && f.method == XIAOTAI_HTTP_DELETE && !f.used_auth);
    f.readable = true;
    assert(!xiaotai_http_idle_connection(c) && f.closes == 2);
    f.readable = false;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK && !res.reused);
    f.socket_error = true;
    assert(!xiaotai_http_idle_connection(c) && f.closes == 3);
    f.socket_error = false;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK && !res.reused);
    f.mode = STALE_WRITE;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_FAIL);
    assert(res.reused && !res.kept && res.platform_error == EPIPE && res.stage == XIAOTAI_HTTP_STAGE_SEND);
    assert(f.sockets == 4 && f.closes == 4 && f.requests == 6);
    xiaotai_http_destroy(c);
}

static xiaotai_err_t sink(void *ctx, const uint8_t *data, size_t size)
{
    xiaotai_http_response_t *res = ctx;
    assert(res->status == 503 && res->content_length == -1 && res->redirect);
    assert(res->captures[0].count == 2 && res->captures[0].truncated);
    assert(!strcmp(res->captures[0].value, "app") && size == 3 && !memcmp(data, "RRR", size));
    return XIAOTAI_OK;
}
static xiaotai_err_t source(void *ctx, uint8_t *data, size_t cap, size_t *size)
{
    unsigned *calls = ctx;
    assert(cap == (*calls < 2 ? 1024 : 2));
    ++*calls;
    memset(data, 'a', cap);
    *size = cap;
    return XIAOTAI_OK;
}
static xiaotai_err_t abort_source(void *ctx, uint8_t *data, size_t cap, size_t *size)
{
    (void)ctx;
    (void)data;
    (void)cap;
    (void)size;
    return XIAOTAI_ERR_INVALID_STATE;
}
static void methods_and_responses(void)
{
    xiaotai_http_t *c = setup();
    f.status = 503; f.redirect = f.duplicate = true; f.duration = 1;
    f.interim = 1;
    char type[4];
    xiaotai_http_capture_t captures[] = {{"Content-Type", type, sizeof(type), false, 0}};
    xiaotai_http_response_t res = {.sink = sink, .sink_ctx = &res, .sink_limit = 3,
                                  .captures = captures, .capture_count = 1};
    unsigned calls = 0;
    xiaotai_http_request_t r = read_request;
    r.method = XIAOTAI_HTTP_PUT; r.source = source; r.source_ctx = &calls; r.source_size = 2050;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK);
    assert(res.status == 503 && res.size == 3 && !res.kept && res.stage == XIAOTAI_HTTP_STAGE_DONE);
    assert(calls == 3 && f.maximum_write == 1024 && f.requests == 1);
    f.duration = 0; f.redirect = false; f.duplicate = false;
    uint8_t small[2];
    res = (xiaotai_http_response_t){.body = small, .capacity = sizeof(small)};
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_ERR_INVALID_SIZE);
    assert(res.status == 503 && res.size == 0 && !res.kept && res.stage == XIAOTAI_HTTP_STAGE_RECEIVE);
    f.status = 204; f.content_size = 0; f.close_asked = true;
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_OK);
    assert(res.content_length == 0 && res.status == 204 && !res.kept);
    f.status = 200; f.content_size = 1; f.eof_after_header = true;
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_FAIL);
    assert(res.status == 200 && !res.kept && res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    f.eof_after_header = false;
    r.source = abort_source;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_INVALID_STATE);
    assert(res.stage == XIAOTAI_HTTP_STAGE_SEND && !res.kept && res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_NONE);
    xiaotai_http_destroy(c);
}

static void admission_and_head_limit(void)
{
    xiaotai_http_t *c = setup();
    xiaotai_http_response_t response = {0};
    xiaotai_http_request_t request = read_request;
    request.url = "http://edge.local/invalid-by-native";
    assert(xiaotai_http_perform(c, &request, &response) == XIAOTAI_ERR_INVALID_ARG);
    request.url = "http://person:secret@edge.local";
    assert(xiaotai_http_perform(c, &request, &response) == XIAOTAI_ERR_INVALID_ARG);
    request.url = read_request.url;
    const xiaotai_http_header_t invalid[] = {{"Host", "other.local"}};
    request.headers = invalid; request.header_count = 1;
    assert(xiaotai_http_perform(c, &request, &response) == XIAOTAI_ERR_INVALID_ARG);
    assert(!f.dns_calls && !f.sockets && !f.requests);
    xiaotai_http_destroy(c);

    assert(xiaotai_http_create(&(xiaotai_http_options_t){.rx_buffer_bytes = 4}, &c) == XIAOTAI_OK);
    assert(xiaotai_http_perform(c, &read_request, &response) == XIAOTAI_ERR_INVALID_SIZE);
    assert(response.stage == XIAOTAI_HTTP_STAGE_RECEIVE && !response.kept && !f.open);
    xiaotai_http_destroy(c);
}

static void *run_blocked(void *arg)
{
    xiaotai_http_response_t res = {0};
    assert(xiaotai_http_perform(arg, &read_request, &res) == XIAOTAI_ERR_INVALID_STATE);
    assert(res.status == 200 && res.stage == XIAOTAI_HTTP_STAGE_RECEIVE && !res.kept);
    return NULL;
}
static void deadlines_and_cancellation(void)
{
    xiaotai_http_t *c = setup();
    xiaotai_http_response_t res = {0};
    xiaotai_http_request_t r = read_request;
    r.timeout_ms = 24;
    f.mode = TRICKLE_HEADER;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_TIMEOUT);
    assert(res.stage == XIAOTAI_HTTP_STAGE_RECEIVE && !res.kept && res.timing.total_ms == 24);
    assert(res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_NONE);
    f.mode = STALLED_CONNECT;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_TIMEOUT);
    assert(res.stage == XIAOTAI_HTTP_STAGE_CONNECT && res.timing.connect_ms == 20 && f.max_wait == 20000);
    f.mode = STALLED_BODY;
    f.wait_bar = 1;
    pthread_t other;
    assert(pthread_create(&other, NULL, run_blocked, c) == 0);
    assert(pthread_mutex_lock(&waiter_lock) == 0);
    while (f.wait_bar == 1) assert(pthread_cond_wait(&waiter_cond, &waiter_lock) == 0);
    int closed = f.closes;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_http_cancel(c);
    assert(f.open && f.closes == closed);
    f.cancel_bar = 1;
    assert(pthread_cond_signal(&waiter_cond) == 0);
    assert(pthread_mutex_unlock(&waiter_lock) == 0);
    assert(pthread_join(other, NULL) == 0 && f.closes == closed + 1);
    f.mode = NORMAL;
    xiaotai_http_cancel(c);
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK);
    xiaotai_http_destroy(c);
}

static bool cancelled_before_transport(void *ctx)
{
    unsigned *calls = ctx;
    ++*calls;
    return true;
}

static void early_cancellation(void)
{
    xiaotai_http_t *c = setup();
    xiaotai_http_response_t res = {0};
    unsigned calls = 0;
    xiaotai_http_request_t r = read_request;
    r.cancelled = cancelled_before_transport;
    r.cancel_ctx = &calls;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_INVALID_STATE);
    assert(calls == 1U && res.stage == XIAOTAI_HTTP_STAGE_NONE && !res.status && !res.kept);
    assert(!f.dns_calls && !f.sockets && !f.requests && !f.open);
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_OK);
    assert(calls == 1U && f.sockets == 1 && f.requests == 1);
    xiaotai_http_destroy(c);
}

static void dns_and_tls(void)
{
    xiaotai_http_t *c = setup();
    xiaotai_http_response_t res = {0};
    xiaotai_http_request_t r = read_request;
    f.no_tcpip = true;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_INVALID_STATE);
    assert(res.stage == XIAOTAI_HTTP_STAGE_RESOLVE && !f.dns_calls && !f.sockets);
    f.no_tcpip = false;
    f.silent_dns = true;
    const int64_t baseline = xiaotai_mem_host_outstanding();
    const uint32_t allocations = xiaotai_mem_host_allocations();
    for (int i = 0; i < 4; ++i) {
        assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_TIMEOUT);
        assert(res.stage == XIAOTAI_HTTP_STAGE_RESOLVE && res.timing.total_ms == 20 && !f.sockets);
    }
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_NO_MEM);
    assert(delayed_count == 4 && f.dns_calls == 4);
    assert(xiaotai_mem_host_allocations() - allocations == 4U * XIAOTAI_PORT_HTTP_DNS_ON_DEMAND);
    assert((xiaotai_mem_host_outstanding() > baseline) == !!XIAOTAI_PORT_HTTP_DNS_ON_DEMAND);
    for (unsigned i = 0; i < delayed_count; ++i) delayed_dns[i]("edge.local", NULL, dns_args[i]);
    delayed_count = 0;
    assert(xiaotai_mem_host_outstanding() == baseline);
    f.silent_dns = false;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK && !res.reused);
    r.url = "https://edge.local/test";
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_OK && !res.reused);
    assert(f.tls_calls == 2 && f.sockets == 2 && f.closes == 1);
    xiaotai_http_close(c);
    f.mode = TLS_REJECT;
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_FAIL);
    assert(res.stage == XIAOTAI_HTTP_STAGE_CONNECT && res.platform_error == -0x1234);
    assert(!f.open && f.sockets == 2 && f.requests == 2);
#else
    assert(xiaotai_http_perform(c, &r, &res) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(res.stage == XIAOTAI_HTTP_STAGE_NONE && !f.open && f.tls_calls == 0 && f.requests == 1);
#endif
    xiaotai_http_destroy(c);
}

static void dns_pool_errors(void)
{
    xiaotai_http_t *c = setup();
    xiaotai_http_response_t res = {0};
    const int64_t baseline = xiaotai_mem_host_outstanding();
    f.reject_dns_post = true;
    for (unsigned i = 0; i < 5; ++i) {
        assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_ERR_NO_MEM);
        assert(res.stage == XIAOTAI_HTTP_STAGE_RESOLVE && xiaotai_mem_host_outstanding() == baseline);
    }
    f.reject_dns_post = false;
    assert(!f.dns_calls && !f.sockets);
    if (XIAOTAI_PORT_HTTP_DNS_ON_DEMAND) {
        xiaotai_mem_host_fail_nth(1);
        assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_ERR_NO_MEM);
        assert(res.stage == XIAOTAI_HTTP_STAGE_RESOLVE && !xiaotai_mem_host_fail_pending());
        assert(xiaotai_mem_host_outstanding() == baseline && !f.dns_calls && !f.sockets);
    }
    f.queue_dns_post = true;
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_ERR_TIMEOUT);
    assert(res.stage == XIAOTAI_HTTP_STAGE_RESOLVE && !f.dns_calls && !f.sockets && queued_dns);
    assert((xiaotai_mem_host_outstanding() > baseline) == !!XIAOTAI_PORT_HTTP_DNS_ON_DEMAND);
    queued_dns(queued_arg); /* The already-expired posting must not start DNS. */
    queued_dns = NULL;
    f.queue_dns_post = false;
    assert(xiaotai_mem_host_outstanding() == baseline && !f.dns_calls);
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_OK && !res.reused);
    assert(xiaotai_mem_host_outstanding() == baseline && f.dns_calls == 1 && f.sockets == 1);
    xiaotai_http_destroy(c);
}

static void receive_deadline_origin(void)
{
    xiaotai_http_t *c = setup();
    xiaotai_http_request_t req = read_request;
    xiaotai_http_response_t res = {0};
    f.mode = STALLED_BODY;
    assert(xiaotai_http_perform(c, &req, &res) == XIAOTAI_ERR_TIMEOUT && res.status == 200 &&
           res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    req.timeout_ms = req.io_timeout_ms;
    assert(xiaotai_http_perform(c, &req, &res) == XIAOTAI_ERR_TIMEOUT && res.status == 200 &&
           res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_NONE);
    xiaotai_http_destroy(c);
}

static void partial_response_fin(void)
{
    xiaotai_http_t *c = setup();
    uint8_t body[48];
    xiaotai_http_response_t res = {.body = body, .capacity = sizeof(body)};
    f.status = 202;
    f.content_size = sizeof(body);
    f.body_prefix = 23;
    for (unsigned native_fail = 0; native_fail < 2U; ++native_fail) {
        f.fin_as_failure = native_fail != 0;
        assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_FAIL);
        assert(res.status == 202 && res.size == 23U && !res.kept &&
               res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED);
    }
    f.chunked = true;
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_FAIL);
    assert(res.size == 23U && res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED);
    f.chunked = false;
    f.duration = 1; /* No declared framing: do not infer a truncated body. */
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_FAIL);
    assert(res.size == 23U && res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    f.duration = 0;
    f.mode = RESET_BODY;
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_FAIL);
    assert(res.size == 23U && res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY && res.platform_error == ECONNRESET);
    f.mode = STALLED_BODY;
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_ERR_TIMEOUT);
    assert(res.size == 23U && res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    f.mode = NORMAL;
    f.body_prefix = 0;
    assert(xiaotai_http_perform(c, &read_request, &res) == XIAOTAI_OK);
    assert(res.size == sizeof(body) && res.receive_fault == XIAOTAI_HTTP_RECEIVE_FAULT_NONE);
    xiaotai_http_destroy(c);
}

int main(void)
{
    reuse_and_isolation();
    methods_and_responses();
    admission_and_head_limit();
    deadlines_and_cancellation();
    early_cancellation();
    dns_and_tls();
    xiaotai_http_stats_t stats;
    xiaotai_http_stats(&stats);
    assert(stats.reused == 2 && stats.oversize == 1 && stats.cancelled == 1);
    assert(stats.timeouts == 6 && stats.requests == stats.completed + stats.failed);
    dns_pool_errors();
    receive_deadline_origin();
    partial_response_fin();
    return 0;
}
