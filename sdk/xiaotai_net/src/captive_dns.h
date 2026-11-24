// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

/* IPv4 is in the address byte order used by net_platform.h. */
size_t net_captive_dns_reply(uint32_t ipv4, const uint8_t *query, size_t size, uint8_t *reply, size_t capacity);
/* Start and stop are serialized by the network worker. */
xiaotai_err_t net_captive_dns_start(uint32_t ipv4);
xiaotai_err_t net_captive_dns_stop(void);
