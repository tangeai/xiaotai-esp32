// SPDX-License-Identifier: MIT
#pragma once
#include "esp_transport.h"
#include <stdbool.h>
#include <stdint.h>
#define DEFAULT_HTTP_BUF_SIZE 512
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef struct native_http *esp_http_client_handle_t;
typedef enum {
    HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_ON_HEADER,
} esp_http_client_event_id_t;
typedef enum {
    HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE = 4
} esp_http_client_method_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    int buffer_size, buffer_size_tx;
    bool disable_auto_redirect;
    int max_authorization_retries;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    esp_transport_handle_t transport;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t, const char *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t, const char *);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t, esp_http_client_method_t);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t, void *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
