// SPDX-License-Identifier: MIT
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include "xiaotai/port/mem.h"

#if !CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY
#error The audited TiRTC session stack policy needs the FreeRTOS stack canary
#endif

extern const unsigned char _xt_session_inbound_start[], _xt_session_inbound_end[];
extern const unsigned char _xt_session_outbound_start[], _xt_session_outbound_end[];
extern void *__real_rtc_thread_create(uint32_t bytes);
void *__wrap_rtc_thread_create(uint32_t bytes);

__attribute__((noinline, noclone))
void *__wrap_rtc_thread_create(uint32_t bytes)
{
    const uintptr_t pc = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    const bool independent =
        (pc > (uintptr_t)_xt_session_inbound_start && pc < (uintptr_t)_xt_session_inbound_end) ||
        (pc > (uintptr_t)_xt_session_outbound_start && pc < (uintptr_t)_xt_session_outbound_end);
    if (bytes == 8192U && independent && !xiaotai_mem_has_external())
        bytes = 9216U;
    return __real_rtc_thread_create(bytes);
}
