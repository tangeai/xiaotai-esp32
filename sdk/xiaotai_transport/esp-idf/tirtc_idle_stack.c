// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tirtc_internal.h"
#include "xiaotai/port/mem.h"

#if !CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY
#error The audited TiRTC stack policy needs the FreeRTOS stack canary
#endif
_Static_assert(sizeof(StackType_t) == 1 && sizeof(StaticSemaphore_t) == 84, "audited Mini FreeRTOS ABI");
_Static_assert(portBYTE_ALIGNMENT == 16, "audited stack alignment");

extern const unsigned char __xt_idle_stack_origin_first[], __xt_idle_stack_origin_limit[];
extern __typeof__(xTaskCreatePinnedToCore) __real_xTaskCreatePinnedToCore;
extern __typeof__(xTaskCreatePinnedToCore) __wrap_xTaskCreatePinnedToCore;

enum { VENDOR_BYTES = 8192, MIN_CAP_BYTES = 4096 };
#if defined(CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT) && CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT
enum { COMPACT_QUEUE_BYTES = 2048 };
#endif
static _Atomic(uintptr_t) owner;
/* Only the synchronous Start task can read or change its own scope. */
static struct {
    uint32_t bytes;
    uint8_t listeners, queues;
} policy;

__attribute__((noinline, noclone))
int xt_tirtc_start_with_idle_stack(const char *id, const TIRTCCALLBACKS *callbacks, uint32_t bytes)
{
    if (bytes < MIN_CAP_BYTES || bytes >= VENDOR_BYTES || bytes % portBYTE_ALIGNMENT || xiaotai_mem_has_external())
        return TiRtcStart(id, callbacks);

    const uintptr_t self = (uintptr_t)xTaskGetCurrentTaskHandle();
    uintptr_t empty = 0;
    if (!self || !atomic_compare_exchange_strong(&owner, &empty, self))
        return TiRtcStart(id, callbacks);

    policy.bytes = bytes;
    policy.listeners = 2;
    policy.queues = 1;
    const int result = TiRtcStart(id, callbacks);
    atomic_store(&owner, 0);
    return result;
}

__attribute__((noinline, noclone))
BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t code, const char *name,
                                       uint32_t bytes, void *arg, UBaseType_t priority,
                                       TaskHandle_t *created, BaseType_t core)
{
    const uintptr_t pc = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
    if (bytes == VENDOR_BYTES && name &&
            pc > (uintptr_t)__xt_idle_stack_origin_first && pc < (uintptr_t)__xt_idle_stack_origin_limit) {
        const uintptr_t start_task = atomic_load(&owner);
        if (start_task && start_task == (uintptr_t)xTaskGetCurrentTaskHandle()) {
            if (policy.listeners && !strcmp(name, "rtc_thread")) {
                --policy.listeners;
#if defined(CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT) && CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT
                /* The queue-only compact profile leaves the listener request intact. */
                if (policy.bytes != MIN_CAP_BYTES)
#endif
                    bytes = policy.bytes;
            } else if (policy.queues && !strcmp(name, "tg_taskq")) {
                --policy.queues;
#if defined(CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT) && CONFIG_XIAOTAI_TIRTC_IDLE_STACK_COMPACT
                bytes = policy.bytes == MIN_CAP_BYTES ? COMPACT_QUEUE_BYTES : policy.bytes;
#else
                bytes = policy.bytes;
#endif
            }
        }
    }
    return __real_xTaskCreatePinnedToCore(code, name, bytes, arg, priority, created, core);
}
