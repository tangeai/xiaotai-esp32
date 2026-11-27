// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/port/http.h"

#define HTTP_CHUNK_MAX 1024U
#define HTTP_HOST_MAX 255U

/* Owned origin, without a path or any per-request secrets. */
typedef struct {
    char host[HTTP_HOST_MAX + 1];
    uint16_t port;
    bool tls;
} http_origin_t;

void http_rules_reset(xiaotai_http_response_t *response);
xiaotai_err_t http_rules_validate(const xiaotai_http_request_t *request, const xiaotai_http_response_t *response,
                                  http_origin_t *origin);
bool http_rules_same_origin(const http_origin_t *first, const http_origin_t *second);
void http_rules_header(xiaotai_http_response_t *response, const char *name, const char *value, bool *peer_closes);
size_t http_rules_receive_limit(const xiaotai_http_response_t *response);
xiaotai_err_t http_rules_consume(xiaotai_http_response_t *response, const uint8_t *data, size_t size);
xiaotai_err_t http_rules_source(const xiaotai_http_request_t *request, uint8_t *data, size_t remaining, size_t *size);
uint32_t http_rules_io_budget(const xiaotai_http_request_t *request, uint32_t elapsed);
