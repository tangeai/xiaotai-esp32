// SPDX-License-Identifier: MIT
#pragma once
#include "esp_transport.h"
#include <stdbool.h>
#include <sys/types.h>
#define ESP_TLS_ERR_SSL_WANT_READ (-0x6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-0x6880)
typedef struct esp_tls esp_tls_t;
typedef struct esp_tls *esp_tls_error_handle_t;
typedef struct {
    bool non_block;
    int timeout_ms;
    const char *common_name;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_tls_cfg_t;
esp_tls_t *esp_tls_init(void);
int esp_tls_conn_destroy(esp_tls_t *);
int esp_tls_conn_new_async(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
ssize_t esp_tls_conn_read(esp_tls_t *, void *, size_t);
ssize_t esp_tls_conn_write(esp_tls_t *, const void *, size_t);
ssize_t esp_tls_get_bytes_avail(esp_tls_t *);
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *, int *);
esp_err_t esp_tls_get_error_handle(esp_tls_t *, esp_tls_error_handle_t *);
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t, int *, int *);
