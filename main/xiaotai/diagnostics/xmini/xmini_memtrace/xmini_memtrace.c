#include "xmini_memtrace.h"
#include <string.h>

#ifdef XMINI_MEMTRACE_ENABLED
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xmini_memtrace_record_t s_records[XMINI_MEMTRACE_SLOTS];
static xmini_memtrace_failure_t s_failures[XMINI_MEMTRACE_FAILURES];
static xmini_memtrace_status_t s_status = {.compiled = true};

void *__real_Mx_malloc(size_t size);
void *__real_Mx_calloc(size_t count, size_t size);
void *__real_Mx_realloc(void *pointer, size_t size);
void __real_Mx_free(void *pointer);

static void remember(xmini_memtrace_record_t record, bool force)
{
    if (!record.address || !record.size)
        return;
    portENTER_CRITICAL(&s_lock);
    if (s_status.running || force) {
        unsigned slot;
        for (slot = 0; slot < XMINI_MEMTRACE_SLOTS; ++slot)
            if (!s_records[slot].address)
                break;
        if (slot < XMINI_MEMTRACE_SLOTS) {
            s_records[slot] = record;
            s_status.live_bytes += record.size;
            s_status.live_slots++;
            if (s_status.live_bytes > s_status.peak_bytes)
                s_status.peak_bytes = s_status.live_bytes;
            if (s_status.live_slots > s_status.peak_slots)
                s_status.peak_slots = s_status.live_slots;
        } else {
            s_status.lost++;
        }
        s_status.allocations++;
        s_status.sequence++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static xmini_memtrace_record_t forget(uintptr_t address)
{
    xmini_memtrace_record_t previous = {0};
    if (!address)
        return previous;
    portENTER_CRITICAL(&s_lock);
    for (unsigned slot = 0; slot < XMINI_MEMTRACE_SLOTS; ++slot) {
        if (s_records[slot].address == address) {
            previous = s_records[slot];
            memset(&s_records[slot], 0, sizeof(s_records[slot]));
            s_status.live_bytes -= previous.size;
            s_status.live_slots--;
            s_status.frees++;
            s_status.sequence++;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return previous;
}

static void failure(size_t size, uintptr_t caller, uint8_t operation)
{
    /* Called after the real allocator has returned, never while its heap lock
     * is held. No printf, allocation or SDK calls occur in this observer. */
    xmini_memtrace_failure_t item = {.size = size, .caller = caller, .operation = operation,
        .free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)};
    portENTER_CRITICAL(&s_lock);
    if (s_status.running) {
        item.sequence = ++s_status.sequence;
        if (s_status.failures < XMINI_MEMTRACE_FAILURES)
            s_failures[s_status.failures] = item;
        s_status.failures++;
    }
    portEXIT_CRITICAL(&s_lock);
}

void *__wrap_Mx_malloc(size_t size)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    void *pointer = __real_Mx_malloc(size);
    if (pointer) remember((xmini_memtrace_record_t){(uintptr_t)pointer, caller, size}, false);
    else if (size) failure(size, caller, 0);
    return pointer;
}

void *__wrap_Mx_calloc(size_t count, size_t size)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    size_t bytes = size && count > SIZE_MAX / size ? SIZE_MAX : count * size;
    void *pointer = __real_Mx_calloc(count, size);
    if (pointer) remember((xmini_memtrace_record_t){(uintptr_t)pointer, caller, bytes}, false);
    else if (bytes) failure(bytes, caller, 1);
    return pointer;
}

void *__wrap_Mx_realloc(void *pointer, size_t size)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    /* Retire before the real call so a concurrent malloc cannot reuse the old
     * address and then be mistakenly removed by this completion. On failure,
     * realloc retains the original allocation, so restore that exact record. */
    xmini_memtrace_record_t previous = forget((uintptr_t)pointer);
    void *result = __real_Mx_realloc(pointer, size);
    if (result) remember((xmini_memtrace_record_t){(uintptr_t)result, caller, size}, previous.address != 0);
    else if (size) {
        remember(previous, previous.address != 0);
        failure(size, caller, 2);
    }
    return result;
}

void __wrap_Mx_free(void *pointer)
{
    forget((uintptr_t)pointer);
    __real_Mx_free(pointer);
}

bool xmini_memtrace_start(void)
{
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    return true;
}
void xmini_memtrace_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    s_status.running = false;
    portEXIT_CRITICAL(&s_lock);
}
void xmini_memtrace_status(xmini_memtrace_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
bool xmini_memtrace_record(unsigned slot, xmini_memtrace_record_t *record)
{
    if (slot >= XMINI_MEMTRACE_SLOTS || !record) return false;
    portENTER_CRITICAL(&s_lock);
    *record = s_records[slot];
    portEXIT_CRITICAL(&s_lock);
    return record->address != 0;
}
bool xmini_memtrace_failure(unsigned slot, xmini_memtrace_failure_t *item)
{
    if (slot >= XMINI_MEMTRACE_FAILURES || !item) return false;
    portENTER_CRITICAL(&s_lock);
    *item = s_failures[slot];
    portEXIT_CRITICAL(&s_lock);
    return item->sequence != 0;
}

#else
bool xmini_memtrace_start(void) { return false; }
void xmini_memtrace_stop(void) {}
void xmini_memtrace_status(xmini_memtrace_status_t *status)
{ if (status) memset(status, 0, sizeof(*status)); }
bool xmini_memtrace_record(unsigned slot, xmini_memtrace_record_t *record)
{ (void)slot; (void)record; return false; }
bool xmini_memtrace_failure(unsigned slot, xmini_memtrace_failure_t *failure)
{ (void)slot; (void)failure; return false; }
#endif
