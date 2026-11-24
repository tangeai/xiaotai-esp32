// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/port/err.h"
typedef int esp_err_t;
#define ESP_OK XIAOTAI_OK
#define ESP_FAIL XIAOTAI_FAIL
#define ESP_ERR_NOT_FOUND XIAOTAI_ERR_NOT_FOUND
#define ESP_ERR_NO_MEM XIAOTAI_ERR_NO_MEM
#define ESP_ERR_TIMEOUT XIAOTAI_ERR_TIMEOUT
typedef struct esp_transport_item_t *esp_transport_handle_t;
typedef int (*connect_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*io_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*io_read_func)(esp_transport_handle_t, char *, int, int);
typedef int (*trans_func)(esp_transport_handle_t);
typedef int (*poll_func)(esp_transport_handle_t, int);
#define ERR_TCP_TRANSPORT_NO_MEM (-3)
#define ERR_TCP_TRANSPORT_CONNECTION_FAILED (-2)
#define ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN (-1)
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT 0
esp_transport_handle_t esp_transport_init(void);
esp_err_t esp_transport_destroy(esp_transport_handle_t);
esp_err_t esp_transport_set_context_data(esp_transport_handle_t, void *);
void *esp_transport_get_context_data(esp_transport_handle_t);
esp_err_t esp_transport_set_func(esp_transport_handle_t, connect_func, io_read_func, io_func,
                                 trans_func, poll_func, poll_func, trans_func);
