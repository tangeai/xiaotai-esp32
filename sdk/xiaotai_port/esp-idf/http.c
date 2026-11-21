// SPDX-License-Identifier: MIT
#include "http_dns.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_transport.h"
#include "http_parser.h"
#include "lwip/sockets.h"
#if LWIP_IPV6
#include "lwip/inet.h"
#endif
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#if !CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
#error "XiaoTai HTTP requires ESP-IDF custom transport (selected by the port Kconfig)"
#endif

enum { IDLE, RUNNING, CANCELLED, FINISHING };
#define HTTP_TX_DEFAULT 1536U
#define HTTP_HEAD_RX_DEFAULT 4096U
#define HTTP_INFORMATIONAL_MAX 8U
#define INC(field) atomic_fetch_add_explicit(&totals.field, 1, memory_order_relaxed)

static struct {
    atomic_uint requests, completed, failed, timeouts, oversize, cancelled, connections, reused;
} totals;

typedef struct {
    const xiaotai_http_request_t *request;
    xiaotai_http_response_t *response;
    int64_t start, deadline, operation_end, stage_start, first_byte;
    bool saw_byte, peer_closes, reusing, oversize;
    bool fetching_headers, read_fin;
    uint8_t header_end;
    size_t pending_start, pending_end, head_bytes;
    uint8_t *scratch;
    xiaotai_err_t transport_error;
    ip_addr_t address;
    char hostname[HTTP_HOST_MAX + 1]; /* TLS identity: never replaced by the resolved address. */
} exchange_t;

struct xiaotai_http {
    atomic_uint state;
    xiaotai_http_options_t options;
    esp_http_client_handle_t native;
    esp_transport_handle_t transport;
    esp_tls_t *tls;
    int fd;
    http_origin_t origin;
    bool kept;
    exchange_t *exchange;
};

static uint32_t elapsed_ms(int64_t start, int64_t end)
{
    return end > start ? (uint32_t)((end - start) / 1000) : 0;
}

static bool valid_socket(int fd)
{
    return fd >= LWIP_SOCKET_OFFSET && fd < LWIP_SELECT_MAXNFDS;
}

static xiaotai_err_t check_until(xiaotai_http_t *client, int64_t end)
{
    if (atomic_load_explicit(&client->state, memory_order_acquire) == CANCELLED)
        return XIAOTAI_ERR_INVALID_STATE;
    return xiaotai_time_us() >= end ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

static xiaotai_err_t check_dns(void *ctx)
{
    xiaotai_http_t *client = ctx;
    return check_until(client, client->exchange->operation_end);
}

static int64_t begin_operation(exchange_t *x)
{
    const int64_t now = xiaotai_time_us();
    const int64_t end = now + (int64_t)x->request->io_timeout_ms * 1000;
    return x->request->io_timeout_ms && end < x->deadline ? end : x->deadline;
}

static int failure(xiaotai_http_t *c, xiaotai_err_t result, int error)
{
    c->exchange->transport_error = result;
    c->exchange->response->platform_error = error;
    return result == XIAOTAI_ERR_TIMEOUT ? ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT :
           result == XIAOTAI_ERR_NO_MEM ? ERR_TCP_TRANSPORT_NO_MEM : ERR_TCP_TRANSPORT_CONNECTION_FAILED;
}

/* No foreign thread closes or shuts down a descriptor; a cancelled request
 * ends at its next native operation/readiness deadline, with no fd-reuse race. */
static xiaotai_err_t wait_socket(xiaotai_http_t *c, int64_t end, bool write)
{
    xiaotai_err_t result = check_until(c, end);
    if (result != XIAOTAI_OK) return result;
    if (!valid_socket(c->fd)) return XIAOTAI_FAIL;
    int64_t left = end - xiaotai_time_us();
    if (left <= 0) return XIAOTAI_ERR_TIMEOUT;
    struct timeval tv = { .tv_sec = left / 1000000, .tv_usec = left % 1000000 };
    fd_set ready, problem;
    FD_ZERO(&ready);
    FD_ZERO(&problem);
    FD_SET(c->fd, &ready);
    FD_SET(c->fd, &problem);
    int rc = lwip_select(c->fd + 1, write ? NULL : &ready, write ? &ready : NULL, &problem, &tv);
    result = check_until(c, end);
    if (result != XIAOTAI_OK) return result;
    if (rc == 0) return XIAOTAI_ERR_TIMEOUT;
    if (rc < 0 || FD_ISSET(c->fd, &problem)) return XIAOTAI_FAIL;
    return XIAOTAI_OK;
}

static int close_socket(xiaotai_http_t *c)
{
    int result = 0;
    if (c->tls) {
        result = esp_tls_conn_destroy(c->tls);
        c->tls = NULL;
    } else if (c->fd >= 0) {
        result = lwip_close(c->fd);
    }
    c->fd = -1;
    return result;
}

static int port_close(esp_transport_handle_t transport)
{
    return close_socket(esp_transport_get_context_data(transport));
}

static xiaotai_err_t connect_tcp(xiaotai_http_t *c, uint16_t port, int64_t end)
{
    const ip_addr_t *address = &c->exchange->address;
    struct sockaddr_storage storage = {0};
    int family;
    socklen_t len;
#if LWIP_IPV6
    if (IP_IS_V6(address)) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&storage;
        v6->sin6_family = family = AF_INET6;
        v6->sin6_port = htons(port);
        inet6_addr_from_ip6addr(&v6->sin6_addr, ip_2_ip6(address));
        v6->sin6_scope_id = ip6_addr_zone(ip_2_ip6(address));
        len = sizeof(*v6);
    } else
#endif
    {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&storage;
        v4->sin_family = family = AF_INET;
        v4->sin_port = htons(port);
        v4->sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(address));
        len = sizeof(*v4);
    }
    c->fd = lwip_socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (c->fd < 0) return errno == ENOMEM || errno == ENOBUFS ? XIAOTAI_ERR_NO_MEM : XIAOTAI_FAIL;
    if (lwip_fcntl(c->fd, F_SETFL, O_NONBLOCK) < 0) return XIAOTAI_FAIL;
    if (lwip_connect(c->fd, (const struct sockaddr *)&storage, len) == 0) return check_until(c, end);
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) return XIAOTAI_FAIL;
    xiaotai_err_t result = wait_socket(c, end, true);
    if (result != XIAOTAI_OK) return result;
    int error = 0;
    len = sizeof(error);
    if (lwip_getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0) return XIAOTAI_FAIL;
    errno = error;
    return error ? XIAOTAI_FAIL : XIAOTAI_OK;
}

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
static xiaotai_err_t connect_tls(xiaotai_http_t *c, uint16_t port, int64_t end)
{
    char address[IPADDR_STRLEN_MAX];
    if (!ipaddr_ntoa_r(&c->exchange->address, address, sizeof(address))) return XIAOTAI_FAIL;
    c->tls = esp_tls_init();
    if (!c->tls) return XIAOTAI_ERR_NO_MEM;
    /* Numeric connection target avoids a second blocking DNS lookup in esp-tls.
     * common_name supplies the original host to certificate verification and SNI. */
    const esp_tls_cfg_t config = {
        .non_block = true, .timeout_ms = 1, .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = c->exchange->hostname,
    };
    xiaotai_err_t result;
    while ((result = check_until(c, end)) == XIAOTAI_OK) {
        int progress = esp_tls_conn_new_async(address, strlen(address), port, &config, c->tls);
        if (progress > 0) {
            if (esp_tls_get_conn_sockfd(c->tls, &c->fd) != ESP_OK) return XIAOTAI_FAIL;
            return check_until(c, end);
        }
        if (progress < 0) {
            esp_tls_error_handle_t errors;
            int tls_code = 0;
            if (esp_tls_get_error_handle(c->tls, &errors) == ESP_OK) {
                int error = esp_tls_get_and_clear_last_error(errors, &tls_code, NULL);
                c->exchange->response->platform_error = tls_code ? tls_code : error;
            }
            return XIAOTAI_FAIL;
        }
        xiaotai_delay_ms(1);
    }
    return result;
}
#endif

static int port_connect(esp_transport_handle_t transport, const char *host, int port, int timeout)
{
    (void)host;
    (void)timeout;
    xiaotai_http_t *c = esp_transport_get_context_data(transport);
    const int64_t end = begin_operation(c->exchange);
    xiaotai_err_t result = check_until(c, end);
    errno = 0;
    if (result == XIAOTAI_OK) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        if (c->origin.tls) result = connect_tls(c, (uint16_t)port, end);
        else
#endif
            result = connect_tcp(c, (uint16_t)port, end);
    }
    if (result != XIAOTAI_OK) {
        int error = c->exchange->response->platform_error;
        failure(c, result, error ? error : errno);
        close_socket(c);
        return -1;
    }
    if (c->options.no_delay) {
        int enabled = 1;
        if (lwip_setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
            failure(c, XIAOTAI_FAIL, errno);
            close_socket(c);
            return -1;
        }
    }
    INC(connections);
    return 0;
}

static int socket_io_failure(xiaotai_http_t *c, xiaotai_err_t result, int error, bool reading, int64_t end)
{
    exchange_t *x = c->exchange;
    if (reading && x->response->stage == XIAOTAI_HTTP_STAGE_RECEIVE &&
        (result == XIAOTAI_FAIL || (result == XIAOTAI_ERR_TIMEOUT && end < x->deadline))) {
        x->response->receive_fault = x->fetching_headers ? XIAOTAI_HTTP_RECEIVE_FAULT_HEAD : XIAOTAI_HTTP_RECEIVE_FAULT_BODY;
    }
    return failure(c, result, error);
}

static int transfer(esp_transport_handle_t transport, char *out, const char *in, int len)
{
    xiaotai_http_t *c = esp_transport_get_context_data(transport);
    exchange_t *x = c->exchange;
    const int64_t end = begin_operation(x);
    bool write = in != NULL;
    xiaotai_err_t result;
    while ((result = check_until(c, end)) == XIAOTAI_OK) {
        if (write && x->reusing && !x->response->reused) {
            x->response->reused = true;
            INC(reused);
        }
        errno = 0;
        ssize_t size = c->tls ? (write ? esp_tls_conn_write(c->tls, in, len) : esp_tls_conn_read(c->tls, out, len)) :
                               (write ? lwip_send(c->fd, in, len, 0) : lwip_recv(c->fd, out, len, 0));
        if (size > 0) {
            if (!write && !x->saw_byte) {
                x->first_byte = xiaotai_time_us();
                x->saw_byte = true;
            }
            return (int)size;
        }
        if (!write && size == 0) {
            x->read_fin = true;
            return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
        }
        bool wants_read = c->tls ? size == ESP_TLS_ERR_SSL_WANT_READ : !write;
        bool again = c->tls ? wants_read || size == ESP_TLS_ERR_SSL_WANT_WRITE :
                             errno == EWOULDBLOCK || errno == EAGAIN;
        if (!again) return socket_io_failure(c, XIAOTAI_FAIL, c->tls ? (int)size : errno, !write, end);
        result = wait_socket(c, end, !wants_read);
        if (result != XIAOTAI_OK) return socket_io_failure(c, result, errno, !write, end);
    }
    return socket_io_failure(c, result, 0, !write, end);
}

static int port_read(esp_transport_handle_t transport, char *data, int len, int timeout)
{
    (void)timeout;
    xiaotai_http_t *c = esp_transport_get_context_data(transport);
    exchange_t *x = c->exchange;
    const xiaotai_err_t state = check_until(c, x->deadline);
    if (state != XIAOTAI_OK) return failure(c, state, 0);
    if (!x->fetching_headers && x->pending_start == x->pending_end)
        return transfer(transport, data, NULL, len);
    if (x->pending_start == x->pending_end) {
        const int cap = len < (int)HTTP_CHUNK_MAX ? len : (int)HTTP_CHUNK_MAX;
        int rc = transfer(transport, (char *)x->scratch, NULL, cap);
        if (rc <= 0) return rc;
        x->pending_start = 0;
        x->pending_end = (size_t)rc;
    }
    /* Stop at a response head boundary so IDF never coalesces headers from
     * 100/103 and the final response in one fetch/capture pass. */
    const char end[] = "\r\n\r\n";
    size_t size = x->pending_end - x->pending_start;
    if (size > (size_t)len) size = (size_t)len;
    if (x->fetching_headers) {
        for (size_t i = 0; i < size; ++i) {
            const char byte = (char)x->scratch[x->pending_start + i];
            x->header_end = byte == end[x->header_end] ? x->header_end + 1 : byte == '\r';
            if (x->header_end == 4) {
                x->header_end = 0;
                size = i + 1;
                break;
            }
        }
        const size_t limit = c->options.rx_buffer_bytes ? c->options.rx_buffer_bytes : HTTP_HEAD_RX_DEFAULT;
        if (size > limit - x->head_bytes) return failure(c, XIAOTAI_ERR_INVALID_SIZE, 0);
        x->head_bytes += size;
    }
    memcpy(data, x->scratch + x->pending_start, size);
    x->pending_start += size;
    return (int)size;
}

static int port_write(esp_transport_handle_t transport, const char *data, int len, int timeout)
{
    (void)timeout;
    return transfer(transport, NULL, data, len);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    exchange_t *x = event->user_data;
    if (!x) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        const int64_t now = xiaotai_time_us();
        x->response->timing.connect_ms = elapsed_ms(x->stage_start, now);
        x->response->stage = XIAOTAI_HTTP_STAGE_SEND;
        x->stage_start = now;
    } else if (event->event_id == HTTP_EVENT_ON_HEADER) {
        http_rules_header(x->response, event->header_key, event->header_value, &x->peer_closes);
    }
    return ESP_OK;
}

static void disconnect(xiaotai_http_t *c)
{
    c->kept = false;
    if (c->native) esp_http_client_close(c->native);
    close_socket(c); /* also closes a failure that happened before HTTP marked it open */
}

static void drop_native(xiaotai_http_t *c)
{
    disconnect(c);
    if (c->native) esp_http_client_cleanup(c->native);
    if (c->transport) esp_transport_destroy(c->transport);
    c->native = NULL;
    c->transport = NULL;
}

static xiaotai_err_t configure(xiaotai_http_t *c, const xiaotai_http_request_t *r)
{
    if (!c->native) {
        c->transport = esp_transport_init();
        if (!c->transport) return XIAOTAI_ERR_NO_MEM;
        if (esp_transport_set_context_data(c->transport, c) != ESP_OK ||
            esp_transport_set_func(c->transport, port_connect, port_read, port_write, port_close,
                                   NULL, NULL, NULL) != ESP_OK) return XIAOTAI_FAIL;
        const esp_http_client_config_t config = {
            .url = r->url, .transport = c->transport, .event_handler = http_event,
            .disable_auto_redirect = true, .max_authorization_retries = -1,
            .buffer_size = c->options.rx_buffer_bytes,
            .buffer_size_tx = c->options.tx_buffer_bytes ? c->options.tx_buffer_bytes : HTTP_TX_DEFAULT,
        };
        c->native = esp_http_client_init(&config);
        if (!c->native) return XIAOTAI_ERR_NO_MEM;
    } else {
        esp_err_t rc = esp_http_client_set_url(c->native, r->url);
        if (rc != ESP_OK) return rc;
    }
    static const esp_http_client_method_t methods[] = {
        HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE,
    };
    esp_err_t rc = esp_http_client_set_method(c->native, methods[r->method]);
    if (rc != ESP_OK) return rc;
    rc = esp_http_client_set_user_data(c->native, c->exchange);
    if (rc != ESP_OK) return rc;
    for (size_t i = 0; i < r->header_count && rc == ESP_OK; ++i)
        rc = esp_http_client_set_header(c->native, r->headers[i].name, r->headers[i].value);
    if (rc == ESP_OK)
        rc = esp_http_client_set_header(c->native, "Connection", r->keep_alive ? "keep-alive" : "close");
    if (rc == ESP_OK) {
        char host[HTTP_HOST_MAX + 8];
        if (c->origin.port == (c->origin.tls ? 443 : 80))
            snprintf(host, sizeof(host), "%s", c->origin.host);
        else
            snprintf(host, sizeof(host), "%s:%u", c->origin.host, c->origin.port);
        rc = esp_http_client_set_header(c->native, "Host", host);
    }
    return rc;
}

static bool clean_request(xiaotai_http_t *c, const xiaotai_http_request_t *r)
{
    bool ok = true;
    for (size_t i = 0; i < r->header_count; ++i) {
        esp_err_t rc = esp_http_client_delete_header(c->native, r->headers[i].name);
        ok = ok && (rc == ESP_OK || rc == ESP_ERR_NOT_FOUND);
    }
    static const char *const names[] = {"Authorization", "Content-Type", "Content-Length", "Connection"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        esp_err_t rc = esp_http_client_delete_header(c->native, names[i]);
        ok = ok && (rc == ESP_OK || rc == ESP_ERR_NOT_FOUND);
    }
    esp_err_t rc = esp_http_client_set_post_field(c->native, NULL, 0);
    ok = ok && (rc == ESP_OK || rc == ESP_ERR_NOT_FOUND);
    if (esp_http_client_set_method(c->native, HTTP_METHOD_GET) != ESP_OK) ok = false;
    if (esp_http_client_set_user_data(c->native, NULL) != ESP_OK) ok = false;
    return ok;
}

static xiaotai_err_t native_result(exchange_t *x, int error)
{
    if (x->transport_error != XIAOTAI_OK) return x->transport_error;
    if (!x->response->platform_error) x->response->platform_error = error == ESP_FAIL ? 0 : error;
    if (error == ESP_ERR_NO_MEM) return XIAOTAI_ERR_NO_MEM;
    if (error == ESP_ERR_TIMEOUT || error == -ESP_ERR_HTTP_EAGAIN || error == ESP_ERR_HTTP_EAGAIN)
        return XIAOTAI_ERR_TIMEOUT;
    return XIAOTAI_FAIL;
}

static xiaotai_err_t native_receive_result(exchange_t *x, int error, xiaotai_http_receive_fault_t phase)
{
    const xiaotai_err_t result = native_result(x, error);
    /* When the socket set the timeout, it already knew which deadline won.
     * Otherwise a timeout returned by esp_http_client before the deadline is
     * a stall in this receive operation. */
    if (result == XIAOTAI_FAIL || (result == XIAOTAI_ERR_TIMEOUT && x->transport_error == XIAOTAI_OK &&
        xiaotai_time_us() < x->deadline)) x->response->receive_fault = phase;
    return result;
}

static xiaotai_err_t receive_body_failure(xiaotai_http_t *c, int error)
{
    exchange_t *x = c->exchange;
    xiaotai_http_response_t *res = x->response;
    const xiaotai_err_t result = native_receive_result(x, error, XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
    /* IDF may turn FIN into a generic error, or return the prefix first. */
    if (result == XIAOTAI_FAIL && x->read_fin && x->transport_error == XIAOTAI_OK && res->size &&
        (res->content_length >= 0 || esp_http_client_is_chunked_response(c->native)) &&
        !esp_http_client_is_complete_data_received(c->native))
        res->receive_fault = XIAOTAI_HTTP_RECEIVE_FAULT_BODY_TRUNCATED;
    return result;
}

/* v5.5.5 can silently stop header pagination if a line cannot fit. Check the
 * complete request head before borrowing the request or opening a socket. */
static bool head_fits(const xiaotai_http_t *c, const xiaotai_http_request_t *r, const http_origin_t *origin)
{
    size_t remaining = c->options.tx_buffer_bytes ? c->options.tx_buffer_bytes : HTTP_TX_DEFAULT;
    const char *target = r->url + (origin->tls ? 8 : 7);
    target += strcspn(target, "/?#");
    /* Request method/version, default headers, Host, framing and terminator. */
    const size_t mandatory = strlen(origin->host) + 112;
    const size_t target_size = strcspn(target, "#");
    if (mandatory > remaining || target_size > remaining - mandatory) return false;
    remaining -= mandatory + target_size;
    size_t longest_line = 0;
    for (size_t i = 0; i < r->header_count; ++i) {
        const size_t name = strlen(r->headers[i].name);
        const size_t value = strlen(r->headers[i].value);
        if (remaining < 4 || name > remaining - 4 || value > remaining - 4 - name) return false;
        const size_t line = name + value + 4;
        if (line > longest_line) longest_line = line;
    }
    return longest_line <= remaining;
}

static xiaotai_err_t exchange(xiaotai_http_t *c, size_t body_size)
{
    exchange_t *x = c->exchange;
    const xiaotai_http_request_t *r = x->request;
    xiaotai_http_response_t *res = x->response;
    xiaotai_err_t result;
    if (!x->reusing) {
        res->stage = XIAOTAI_HTTP_STAGE_RESOLVE;
        x->stage_start = xiaotai_time_us();
        x->operation_end = begin_operation(x);
        result = http_idf_resolve(x->hostname, &x->address, check_dns, c);
        if (result != XIAOTAI_OK) return result;
        res->timing.resolve_ms = elapsed_ms(x->stage_start, xiaotai_time_us());
        res->stage = XIAOTAI_HTTP_STAGE_CONNECT;
    } else {
        res->stage = XIAOTAI_HTTP_STAGE_SEND;
    }
    x->stage_start = xiaotai_time_us();
    if ((result = check_until(c, x->deadline)) != XIAOTAI_OK) return result;
    esp_err_t rc = esp_http_client_open(c->native, (int)body_size);
    if (rc != ESP_OK) return native_result(x, rc);

    uint8_t chunk[HTTP_CHUNK_MAX], buffered[HTTP_CHUNK_MAX];
    x->scratch = buffered;
    for (size_t offset = 0; offset < body_size;) {
        if ((result = check_until(c, x->deadline)) != XIAOTAI_OK) return result;
        size_t size = body_size - offset;
        const uint8_t *data = r->body;
        if (r->source) {
            result = http_rules_source(r, chunk, size, &size);
            if (result != XIAOTAI_OK) return result;
            data = chunk;
        } else {
            data += offset;
            if (size > sizeof(chunk)) size = sizeof(chunk);
        }
        int written = esp_http_client_write(c->native, (const char *)data, (int)size);
        if (written != (int)size) return native_result(x, written);
        offset += size;
    }
    const int64_t sent = xiaotai_time_us();
    res->timing.send_ms = elapsed_ms(x->stage_start, sent);
    res->stage = XIAOTAI_HTTP_STAGE_RECEIVE;
    x->stage_start = sent;
    unsigned interim = 0;
    x->fetching_headers = true;
    for (;;) {
        int64_t header_result = esp_http_client_fetch_headers(c->native);
        int status = esp_http_client_get_status_code(c->native);
        if (status > 0) res->status = status;
        if (header_result < 0 || status < 100)
            return native_receive_result(x, (int)header_result, XIAOTAI_HTTP_RECEIVE_FAULT_HEAD);
        if (status >= 200 || status == 101) break;
        if (++interim > HTTP_INFORMATIONAL_MAX) return XIAOTAI_FAIL;
        /* Same exchange, not a replay. Ignore captures from the interim head. */
        for (size_t i = 0; i < res->capture_count; ++i) {
            xiaotai_http_capture_t *v = &res->captures[i];
            v->count = 0;
            v->truncated = false;
            if (v->value && v->capacity) v->value[0] = '\0';
        }
        res->redirect = x->peer_closes = false;
        x->head_bytes = 0;
    }
    x->fetching_headers = false;
    if (res->status == 101) return XIAOTAI_FAIL; /* Upgraded protocols are not HTTP exchanges. */
    res->content_length = esp_http_client_get_content_length(c->native);
    if (res->status == 204 || res->status == 304) {
        if (x->pending_start != x->pending_end) x->peer_closes = true;
        return check_until(c, x->deadline);
    }
    if (res->content_length >= 0 && (uint64_t)res->content_length > http_rules_receive_limit(res)) {
        x->oversize = true;
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    for (;;) {
        if ((result = check_until(c, x->deadline)) != XIAOTAI_OK) return result;
        int size = esp_http_client_read(c->native, (char *)chunk, sizeof(chunk));
        if (size < 0) return receive_body_failure(c, size);
        if (!size) {
            if (!esp_http_client_is_complete_data_received(c->native))
                return receive_body_failure(c, ESP_FAIL);
            if (x->pending_start != x->pending_end) x->peer_closes = true;
            break;
        }
        if ((size_t)size > sizeof(chunk))
            return native_receive_result(x, ESP_FAIL, XIAOTAI_HTTP_RECEIVE_FAULT_BODY);
        const size_t limit = http_rules_receive_limit(res);
        if (res->size > limit || (size_t)size > limit - res->size) x->oversize = true;
        result = http_rules_consume(res, chunk, (size_t)size);
        if (result != XIAOTAI_OK) {
            res->receive_fault = XIAOTAI_HTTP_RECEIVE_FAULT_NONE;
            return result;
        }
        if (x->transport_error != XIAOTAI_OK) return x->transport_error;
    }
    return check_until(c, x->deadline);
}

xiaotai_err_t xiaotai_http_create(const xiaotai_http_options_t *options, xiaotai_http_t **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    *out = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(**out));
    if (!*out) return XIAOTAI_ERR_NO_MEM;
    atomic_init(&(*out)->state, IDLE);
    if (options) (*out)->options = *options;
    (*out)->fd = -1;
    return XIAOTAI_OK;
}

void xiaotai_http_destroy(xiaotai_http_t *c)
{
    if (!c) return;
    drop_native(c);
    xiaotai_mem_free(c);
}

static bool idle(xiaotai_http_t *c)
{
    if (!c->kept || !valid_socket(c->fd)) return false;
    int error = 0;
    socklen_t len = sizeof(error);
    fd_set reads, errors;
    FD_ZERO(&reads);
    FD_ZERO(&errors);
    FD_SET(c->fd, &reads);
    FD_SET(c->fd, &errors);
    struct timeval now = {0};
    return (!c->tls || esp_tls_get_bytes_avail(c->tls) == 0) &&
        lwip_getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0 &&
        lwip_select(c->fd + 1, &reads, NULL, &errors, &now) == 0;
}

bool xiaotai_http_idle_connection(xiaotai_http_t *c)
{
    if (!c || atomic_load_explicit(&c->state, memory_order_acquire) != IDLE) return false;
    if (idle(c)) return true;
    disconnect(c);
    return false;
}

void xiaotai_http_close(xiaotai_http_t *c)
{
    if (c && atomic_load_explicit(&c->state, memory_order_acquire) == IDLE) disconnect(c);
}

void xiaotai_http_cancel(xiaotai_http_t *c)
{
    if (!c) return;
    unsigned expect = RUNNING;
    atomic_compare_exchange_strong_explicit(&c->state, &expect, CANCELLED,
                                            memory_order_release, memory_order_relaxed);
}

xiaotai_err_t xiaotai_http_perform(xiaotai_http_t *c, const xiaotai_http_request_t *r, xiaotai_http_response_t *res)
{
    if (!c) return XIAOTAI_ERR_INVALID_ARG;
    unsigned expect = IDLE;
    if (!atomic_compare_exchange_strong_explicit(&c->state, &expect, RUNNING,
                                                 memory_order_acquire, memory_order_relaxed))
        return XIAOTAI_ERR_INVALID_STATE;
    http_rules_reset(res);
    http_origin_t origin;
    xiaotai_err_t result = http_rules_validate(r, res, &origin);
    if (result != XIAOTAI_OK) goto rejected;
    if (r->cancelled && r->cancelled(r->cancel_ctx)) {
        result = XIAOTAI_ERR_INVALID_STATE;
        goto rejected;
    }
    /* Avoid the native set_url() error path, which can log the raw URL.
     * Its URL parser uses uint16_t offsets; never pass truncated positions. */
    struct http_parser_url parsed = {0};
    const size_t url_size = strlen(r->url);
    if (url_size > UINT16_MAX || http_parser_parse_url(r->url, url_size, 0, &parsed) != 0 ||
        (parsed.field_set & (1U << UF_USERINFO))) {
        result = XIAOTAI_ERR_INVALID_ARG;
        goto rejected;
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (origin.tls) { result = XIAOTAI_ERR_NOT_SUPPORTED; goto rejected; }
#endif
    size_t body_size = r->source ? r->source_size : r->body_size;
    if (body_size > INT_MAX) { result = XIAOTAI_ERR_INVALID_SIZE; goto rejected; }
    /* The port, not extra caller headers, owns origin and message framing. */
    for (size_t i = 0; i < r->header_count; ++i) {
        const char *name = r->headers[i].name;
        if (!strcasecmp(name, "Host") || !strcasecmp(name, "Transfer-Encoding") ||
            !strcasecmp(name, "Content-Length") || !strcasecmp(name, "Connection")) {
            result = XIAOTAI_ERR_INVALID_ARG;
            goto rejected;
        }
    }
    if (!head_fits(c, r, &origin)) {
        result = XIAOTAI_ERR_INVALID_SIZE;
        goto rejected;
    }
    exchange_t x = {.request = r, .response = res, .start = xiaotai_time_us()};
    x.deadline = x.start + (int64_t)r->timeout_ms * 1000;
    if (origin.host[0] == '[') {
        const size_t len = strlen(origin.host) - 2;
        memcpy(x.hostname, origin.host + 1, len);
        if (strchr(x.hostname, '%')) { result = XIAOTAI_ERR_NOT_SUPPORTED; goto rejected; }
    } else {
        memcpy(x.hostname, origin.host, strlen(origin.host) + 1);
    }
    if (!http_rules_same_origin(&c->origin, &origin)) drop_native(c);
    x.reusing = idle(c);
    if (!x.reusing) disconnect(c);
    c->origin = origin;
    c->exchange = &x;
    result = configure(c, r);
    const bool entered = result == XIAOTAI_OK;
    if (entered) {
        INC(requests);
        result = exchange(c, body_size);
    }
    const int64_t end = xiaotai_time_us();
    if (res->stage == XIAOTAI_HTTP_STAGE_RESOLVE) res->timing.resolve_ms = elapsed_ms(x.stage_start, end);
    if (res->stage == XIAOTAI_HTTP_STAGE_CONNECT) res->timing.connect_ms = elapsed_ms(x.stage_start, end);
    if (res->stage == XIAOTAI_HTTP_STAGE_SEND) res->timing.send_ms = elapsed_ms(x.stage_start, end);
    if (res->stage == XIAOTAI_HTTP_STAGE_RECEIVE) {
        res->timing.wait_ms = elapsed_ms(x.stage_start, x.saw_byte ? x.first_byte : end);
        if (x.saw_byte) res->timing.receive_ms = elapsed_ms(x.first_byte, end);
    }
    res->timing.total_ms = elapsed_ms(x.start, end);
    bool clean = c->native && clean_request(c, r);
    const bool cancelled = atomic_exchange_explicit(&c->state, FINISHING, memory_order_acq_rel) == CANCELLED;
    if (cancelled)
        result = XIAOTAI_ERR_INVALID_STATE;
    if (result == XIAOTAI_OK) {
        res->stage = XIAOTAI_HTTP_STAGE_DONE;
        INC(completed);
    } else if (entered) {
        INC(failed);
        if (result == XIAOTAI_ERR_TIMEOUT) INC(timeouts);
        if (result == XIAOTAI_ERR_INVALID_SIZE && x.oversize) INC(oversize);
        if (cancelled) INC(cancelled);
    }
    c->kept = clean && result == XIAOTAI_OK && r->keep_alive && !res->redirect && !x.peer_closes &&
        c->fd >= 0 && esp_http_client_is_persistent_connection(c->native);
    if (!c->kept) drop_native(c);
    res->kept = c->kept;
    c->exchange = NULL;
    atomic_store_explicit(&c->state, IDLE, memory_order_release);
    return result;

rejected:
    drop_native(c);
    atomic_store_explicit(&c->state, IDLE, memory_order_release);
    return result;
}

void xiaotai_http_stats(xiaotai_http_stats_t *out)
{
    if (!out) return;
#define READ(field) out->field = atomic_load_explicit(&totals.field, memory_order_relaxed)
    READ(requests); READ(completed); READ(failed); READ(timeouts);
    READ(oversize); READ(cancelled); READ(connections); READ(reused);
#undef READ
}
