// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "xiaotai/port/err.h"

uint32_t platform_guest_step(void);           /* worker; includes owner repost */
xiaotai_err_t platform_guest_before_login(void *ctx);
xiaotai_err_t platform_guest_shutdown(void);  /* worker; fail if provider still owns resources */
void platform_guest_reset(void);             /* after worker joined; revoke queued owner jobs */
