// SPDX-License-Identifier: MIT
#include "memtrace.h"
#include "xiaotai/port/mem.h"

void *__real_Mx_malloc(size_t size);
void *__real_Mx_calloc(size_t count, size_t size);
void *__real_Mx_realloc(void *pointer, size_t size);
void __real_Mx_free(void *pointer);

static void failure(size_t size, uintptr_t caller, unsigned operation)
{
    if (!size || !xt_memtrace_running()) return;
    xiaotai_mem_stats_t stats;
    xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &stats);
    xt_memtrace_failed(size, caller, operation, stats.free, stats.largest_block);
}

void *__wrap_Mx_malloc(size_t size)
{
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    void *pointer = __real_Mx_malloc(size);
    if (pointer) xt_memtrace_alloc((uintptr_t)pointer, size, caller);
    else failure(size, caller, 0);
    return pointer;
}

void *__wrap_Mx_calloc(size_t count, size_t size)
{
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    const size_t bytes = size && count > SIZE_MAX / size ? SIZE_MAX : count * size;
    void *pointer = __real_Mx_calloc(count, size);
    if (pointer) xt_memtrace_alloc((uintptr_t)pointer, bytes, caller);
    else failure(bytes, caller, 1);
    return pointer;
}

void *__wrap_Mx_realloc(void *pointer, size_t size)
{
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    const unsigned ticket = xt_memtrace_realloc_begin((uintptr_t)pointer);
    void *result = __real_Mx_realloc(pointer, size);
    xt_memtrace_realloc_end(ticket, (uintptr_t)result, size, caller);
    if (!result) failure(size, caller, 2);
    return result;
}

void __wrap_Mx_free(void *pointer)
{
    xt_memtrace_free((uintptr_t)pointer);
    __real_Mx_free(pointer);
}
