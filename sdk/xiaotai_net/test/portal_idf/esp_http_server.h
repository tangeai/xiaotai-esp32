// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "xiaotai/port/err.h"

typedef int esp_err_t;
#define ESP_OK XIAOTAI_OK
#define ESP_FAIL XIAOTAI_FAIL
#define ESP_ERR_NOT_FOUND XIAOTAI_ERR_NOT_FOUND
#define HTTPD_SOCK_ERR_TIMEOUT (-3)
#define HTTPD_SOCK_ERR_FAIL (-1)
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})

typedef void *httpd_handle_t;
typedef enum { HTTP_GET, HTTP_POST, HTTP_PUT } httpd_method_t;
typedef struct {
    httpd_method_t method;
    char uri[1024];
    size_t content_len;
} httpd_req_t;

typedef struct {
    uint32_t stack_size, task_caps;
    unsigned max_open_sockets, max_uri_handlers, max_resp_headers, recv_wait_timeout, send_wait_timeout;
    bool lru_purge_enable;
    esp_err_t (*open_fn)(httpd_handle_t, int);
    bool (*uri_match_fn)(const char *, const char *, size_t);
} httpd_config_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t server);
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *handler);
bool httpd_uri_match_wildcard(const char *pattern, const char *uri, size_t len);
int httpd_req_to_sockfd(httpd_req_t *request);
size_t httpd_req_get_hdr_value_len(httpd_req_t *request, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *field, char *value, size_t size);
int httpd_req_recv(httpd_req_t *r, char *data, size_t len);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *value);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *value);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *data, ssize_t len);
