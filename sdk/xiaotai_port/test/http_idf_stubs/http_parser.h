// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
enum { UF_USERINFO = 6, UF_MAX };
struct http_parser_url {
    uint16_t field_set, port;
    struct { uint16_t off, len; } field_data[UF_MAX];
};
int http_parser_parse_url(const char *, size_t, int, struct http_parser_url *);
