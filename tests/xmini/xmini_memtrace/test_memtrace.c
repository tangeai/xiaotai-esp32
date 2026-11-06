#include "xmini_memtrace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void *__wrap_Mx_malloc(size_t size);
void *__wrap_Mx_calloc(size_t count, size_t size);
void *__wrap_Mx_realloc(void *pointer, size_t size);
void __wrap_Mx_free(void *pointer);
static bool s_fail;
void *__real_Mx_malloc(size_t size) { return s_fail ? NULL : malloc(size); }
void *__real_Mx_calloc(size_t count, size_t size) { return s_fail ? NULL : calloc(count, size); }
void *__real_Mx_realloc(void *pointer, size_t size)
{
    if (!size) { free(pointer); return NULL; }
    return s_fail ? NULL : realloc(pointer, size);
}
void __real_Mx_free(void *pointer) { free(pointer); }
size_t heap_caps_get_free_size(unsigned capabilities) { assert(capabilities == 3); return 4000; }
size_t heap_caps_get_largest_free_block(unsigned capabilities) { assert(capabilities == 3); return 1984; }
static xmini_memtrace_status_t status(void)
{ xmini_memtrace_status_t value; xmini_memtrace_status(&value); return value; }
int main(void)
{
    assert(xmini_memtrace_start());
    void *a = __wrap_Mx_malloc(160), *b = __wrap_Mx_calloc(3, 128);
    assert(a && b && status().live_bytes == 544 && status().live_slots == 2);
    a = __wrap_Mx_realloc(a, 320);
    assert(a && status().live_bytes == 704 && status().live_slots == 2);
    s_fail = true;
    assert(!__wrap_Mx_malloc(4256));
    assert(!__wrap_Mx_realloc(a, 8192));
    assert(status().live_bytes == 704 && status().live_slots == 2); /* Failed resize preserves old owner. */
    for (unsigned i = 0; i < 12; ++i) assert(!__wrap_Mx_malloc(5152));
    xmini_memtrace_failure_t first;
    assert(xmini_memtrace_failure(0, &first) && first.size == 4256 && first.caller);
    assert(first.free_bytes == 4000 && first.largest_block == 1984 && status().failures == 14);
    s_fail = false;
    xmini_memtrace_stop();
    __wrap_Mx_free(a); __wrap_Mx_free(b);
    assert(status().live_bytes == 0 && status().live_slots == 0);
    a = __wrap_Mx_malloc(32); assert(a && status().live_slots == 0); __wrap_Mx_free(a);
    assert(xmini_memtrace_start());
    void *pointers[XMINI_MEMTRACE_SLOTS + 1];
    for (unsigned i = 0; i < XMINI_MEMTRACE_SLOTS + 1; ++i) pointers[i] = __wrap_Mx_malloc(16);
    assert(status().lost == 1 && status().live_slots == XMINI_MEMTRACE_SLOTS);
    for (unsigned i = 0; i < XMINI_MEMTRACE_SLOTS + 1; ++i) __wrap_Mx_free(pointers[i]);
    assert(status().live_bytes == 0 && status().live_slots == 0);
    puts("Mx allocation observer: ownership, failed realloc, first-failure preservation, bounded slots, stop/free PASS");
    return 0;
}
