// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#define LWIP_IPV6 0
#define IPADDR_STRLEN_MAX 16
typedef uint32_t ip_addr_t;
#define ip_2_ip4(addr) (addr)
#define ip4_addr_get_u32(addr) (*(addr))
int ipaddr_aton(const char *, ip_addr_t *);
char *ipaddr_ntoa_r(const ip_addr_t *, char *, int);
