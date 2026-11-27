// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
typedef enum { LWIP_CORE_IS_TCPIP_INITIALIZED = 1 } sys_thread_core_lock_t;
bool sys_thread_tcpip(sys_thread_core_lock_t state);
