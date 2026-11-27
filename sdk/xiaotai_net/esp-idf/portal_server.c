// SPDX-License-Identifier: MIT
#include "net_platform.h"
#include "xiaotai/net_config.h"
#include "xiaotai/port/mem.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#define PORTAL_PATH_BYTES 512U
#define PORTAL_RESPONSE_HEADERS 8U
#define PORTAL_HEADER_BYTES 384U
#define PORTAL_TYPE_BYTES 64U

struct net_exchange {
    httpd_req_t *request;
    size_t body_left;
    bool started, ended, failed;
    char type[PORTAL_TYPE_BYTES];
    char headers[PORTAL_HEADER_BYTES];
};

/* Only the engine worker starts or stops HTTPD. Start publishes the callback
 * and IP before opening admission; stop withdraws admission before joining. */
static httpd_handle_t s_server;
static atomic_bool s_open;
static uint32_t s_ap;
static uint32_t s_stack;
static net_request_fn_t s_handler;
static void *s_context;

static bool socket_allowed(int fd)
{
    if (fd < 0 || !atomic_load_explicit(&s_open, memory_order_acquire)) return false;
    struct sockaddr_storage local = {0};
    socklen_t size = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &size) != 0) return false;
    uint32_t ipv4 = 0;
    if (local.ss_family == AF_INET && size >= sizeof(struct sockaddr_in)) {
        ipv4 = ((const struct sockaddr_in *)&local)->sin_addr.s_addr;
#if CONFIG_LWIP_IPV6
    } else if (local.ss_family == AF_INET6 && size >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&local;
        if (!IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) return false;
        memcpy(&ipv4, v6->sin6_addr.s6_addr + 12, sizeof(ipv4));
#endif
    } else {
        return false;
    }
    return ipv4 != 0 && ipv4 == s_ap;
}

static esp_err_t accept_connection(httpd_handle_t server, int fd)
{
    (void)server;
    /* HTTPD listens on all interfaces. Never expose the provisioning handler
     * to a station-side connection. Check again on each accepted request. */
    return socket_allowed(fd) ? ESP_OK : ESP_FAIL;
}

static esp_err_t dispatch(httpd_req_t *request)
{
    const char *method = request->method == HTTP_GET ? "GET" : request->method == HTTP_POST ? "POST" : NULL;
    if (!method) return ESP_FAIL;
    const char *query = strchr(request->uri, '?');
    const char *path = request->uri;
    char copied_path[PORTAL_PATH_BYTES];
    if (query) {
        const size_t length = (size_t)(query - path);
        if (length >= sizeof(copied_path)) path = ""; /* Unmatched, not truncated into a known route. */
        else {
            memcpy(copied_path, path, length);
            copied_path[length] = '\0';
            path = copied_path;
        }
    }
    /* Only the exact AP origin can be accepted. Never turn a present but
     * truncated/empty/unreadable Origin into an absent one. */
    char origin[sizeof(XIAOTAI_NET_AP_URL)] = {0};
    const size_t origin_len = httpd_req_get_hdr_value_len(request, "Origin");
    const char *origin_value = "";
    if (origin_len < sizeof(origin)) {
        const esp_err_t got_origin = httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin));
        if (got_origin == ESP_OK && strlen(origin) == origin_len) origin_value = origin;
        else if (got_origin == ESP_ERR_NOT_FOUND && !origin_len) origin_value = NULL;
    }
    net_exchange_t exchange = {.request = request, .body_left = request->content_len};
    const net_request_t translated = {
        .method = method, .path = path, .origin = origin_value, .content_length = request->content_len,
        .local_is_ap = socket_allowed(httpd_req_to_sockfd(request)),
    };
    s_handler(&translated, &exchange, s_context);
    return exchange.ended && !exchange.failed ? ESP_OK : ESP_FAIL;
}

xiaotai_err_t net_platform_server_start(uint32_t ap_ipv4, uint32_t stack_bytes,
                                        net_request_fn_t handler, void *ctx)
{
    if (!ap_ipv4 || !stack_bytes || !handler) return XIAOTAI_ERR_INVALID_ARG;
    if (s_server) {
        return atomic_load_explicit(&s_open, memory_order_relaxed) && s_ap == ap_ipv4 &&
               s_stack == stack_bytes && s_handler == handler && s_context == ctx
                   ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
    }
    s_ap = ap_ipv4;
    s_stack = stack_bytes;
    s_handler = handler;
    s_context = ctx;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = stack_bytes;
    config.task_caps = MALLOC_CAP_8BIT | (xiaotai_mem_has_external() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    config.max_open_sockets = XIAOTAI_NET_AP_CLIENTS_MAX;
    config.max_uri_handlers = 2;
    config.max_resp_headers = PORTAL_RESPONSE_HEADERS;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    config.lru_purge_enable = true;
    config.open_fn = accept_connection;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    s_server = server;
    const httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = dispatch};
    const httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = dispatch};
    err = httpd_register_uri_handler(server, &get);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &post);
    if (err != ESP_OK) {
        const esp_err_t stopped = net_platform_server_stop();
        return stopped == ESP_OK ? err : stopped;
    }
    atomic_store_explicit(&s_open, true, memory_order_release);
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_server_stop(void)
{
    atomic_store_explicit(&s_open, false, memory_order_release);
    if (!s_server) return XIAOTAI_OK;
    const esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) return err; /* Ownership stays here for a retry; admission remains closed. */
    s_server = NULL;
    s_handler = NULL;
    s_context = NULL;
    s_ap = 0;
    s_stack = 0;
    return XIAOTAI_OK;
}

xiaotai_err_t net_platform_body_read(net_exchange_t *exchange, uint8_t *data, size_t capacity, size_t *size)
{
    if (size) *size = 0;
    if (!exchange || !size || (!data && capacity)) return XIAOTAI_ERR_INVALID_ARG;
    if (exchange->started || exchange->failed) return XIAOTAI_ERR_INVALID_STATE;
    if (!capacity || !exchange->body_left) return XIAOTAI_OK;
    size_t read = capacity < exchange->body_left ? capacity : exchange->body_left;
    if (read > INT_MAX) read = INT_MAX;
    const int received = httpd_req_recv(exchange->request, (char *)data, read);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) return XIAOTAI_ERR_TIMEOUT;
    if (received <= 0 || (size_t)received > read) return XIAOTAI_FAIL;
    exchange->body_left -= (size_t)received;
    *size = (size_t)received;
    return XIAOTAI_OK;
}

static const char *status_line(int status)
{
    switch (status) {
        case 200: return "200 OK";
        case 303: return "303 See Other";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 408: return "408 Request Timeout";
        case 409: return "409 Conflict";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default: return NULL;
    }
}

static bool header_name(const char *name, const char *colon)
{
    if (!colon || colon == name) return false;
    for (; name != colon; ++name) {
        const char c = *name;
        if (!(c == '-' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

static xiaotai_err_t save_response_error(net_exchange_t *exchange, esp_err_t err)
{
    if (err != ESP_OK) exchange->failed = true;
    return err;
}

xiaotai_err_t net_platform_reply_begin(net_exchange_t *exchange, int status, const char *content_type,
                                        const char *const *headers, size_t count)
{
    const char *line = status_line(status);
    if (!exchange || !line || !content_type || count > PORTAL_RESPONSE_HEADERS || (!headers && count))
        return XIAOTAI_ERR_INVALID_ARG;
    if (exchange->started || exchange->failed) return XIAOTAI_ERR_INVALID_STATE;
    const size_t type_len = strlen(content_type);
    if (type_len >= sizeof(exchange->type) || strpbrk(content_type, "\r\n")) return XIAOTAI_ERR_INVALID_ARG;
    size_t used = 0;
    char *fields[PORTAL_RESPONSE_HEADERS], *values[PORTAL_RESPONSE_HEADERS];
    for (size_t i = 0; i < count; ++i) {
        const char *header = headers[i];
        const char *colon = header ? strchr(header, ':') : NULL;
        if (!header_name(header, colon) || strpbrk(header, "\r\n"))
            return XIAOTAI_ERR_INVALID_ARG;
        const size_t len = strlen(header) + 1;
        if (len > sizeof(exchange->headers) - used) return XIAOTAI_ERR_INVALID_SIZE;
        fields[i] = exchange->headers + used;
        memcpy(fields[i], header, len);
        values[i] = fields[i] + (colon - header);
        *values[i]++ = '\0';
        while (*values[i] == ' ' || *values[i] == '\t') ++values[i];
        used += len;
    }
    /* HTTPD borrows these strings until the first chunk. The caller need not. */
    memcpy(exchange->type, content_type, type_len + 1);
    esp_err_t err = httpd_resp_set_status(exchange->request, line);
    if (err == ESP_OK) err = httpd_resp_set_type(exchange->request, exchange->type);
    for (size_t i = 0; err == ESP_OK && i < count; ++i)
        err = httpd_resp_set_hdr(exchange->request, fields[i], values[i]);
    exchange->started = err == ESP_OK;
    return save_response_error(exchange, err);
}

xiaotai_err_t net_platform_reply_chunk(net_exchange_t *exchange, const void *data, size_t size)
{
    if (!exchange || (!data && size) || size > INT_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (!exchange->started || exchange->ended || exchange->failed) return XIAOTAI_ERR_INVALID_STATE;
    if (!size) return XIAOTAI_OK; /* A zero native chunk ends the response. */
    return save_response_error(exchange, httpd_resp_send_chunk(exchange->request, data, (ssize_t)size));
}

xiaotai_err_t net_platform_reply_end(net_exchange_t *exchange)
{
    if (!exchange) return XIAOTAI_ERR_INVALID_ARG;
    if (!exchange->started || exchange->ended || exchange->failed) return XIAOTAI_ERR_INVALID_STATE;
    const esp_err_t err = httpd_resp_send_chunk(exchange->request, NULL, 0);
    exchange->ended = err == ESP_OK;
    return save_response_error(exchange, err);
}
