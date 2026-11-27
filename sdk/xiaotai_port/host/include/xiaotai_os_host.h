// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Positive offsets advance xiaotai_time_us, not condition-wait deadlines. */
void xiaotai_os_host_advance_us(int64_t delta);

#ifdef __cplusplus
}
#endif
