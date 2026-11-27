// SPDX-License-Identifier: MIT
#pragma once
#include "lwip/ip_addr.h"
typedef int err_t;
#define ERR_OK 0
#define ERR_INPROGRESS (-5)
typedef void (*dns_found_callback)(const char *, const ip_addr_t *, void *);
err_t dns_gethostbyname(const char *, ip_addr_t *, dns_found_callback, void *);
