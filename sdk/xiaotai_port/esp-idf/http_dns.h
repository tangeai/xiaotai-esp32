// SPDX-License-Identifier: MIT
#pragma once

#include "http_rules.h"
#include "lwip/ip_addr.h"

typedef xiaotai_err_t (*http_dns_check_t)(void *ctx);

/* Resolve on lwIP's worker, without pinning the caller's stack after timeout. */
xiaotai_err_t http_idf_resolve(const char *host, ip_addr_t *address, http_dns_check_t check, void *ctx);
