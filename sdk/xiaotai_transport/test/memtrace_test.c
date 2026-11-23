// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_memtrace/test_memtrace.c
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memtrace.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/transport_config.h"

static bool s_fail;
static unsigned s_reentrant_slots;
static unsigned s_probes;

void *__wrap_Mx_malloc(size_t bytes);
void *__wrap_Mx_calloc(size_t count, size_t bytes);
void *__wrap_Mx_realloc(void *pointer, size_t bytes);
void __wrap_Mx_free(void *pointer);
void *__real_Mx_malloc(size_t bytes) { return s_fail ? NULL : malloc(bytes); }
void *__real_Mx_calloc(size_t count, size_t bytes) { return s_fail ? NULL : calloc(count, bytes); }
void *__real_Mx_realloc(void *pointer, size_t bytes)
{
    if (!bytes) { free(pointer); return NULL; }
    for (unsigned i = 0; i < s_reentrant_slots; ++i) xt_memtrace_alloc(0x10000U + i, 16, 0x55U);
    return s_fail ? NULL : realloc(pointer, bytes);
}
void __real_Mx_free(void *pointer) { free(pointer); }

void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    assert(kind == XIAOTAI_MEM_INTERNAL);
    ++s_probes;
    *stats = (xiaotai_mem_stats_t){.free = 4000, .largest_block = 1984};
}

static xt_memtrace_status_t status(void) { xt_memtrace_status_t s; xt_memtrace_status(&s); return s; }
static unsigned s_lines;
static void output(void *ctx, const char *line)
{
    (void)ctx;
    if (s_lines == 0) assert(strstr(line, "+MEMTRACE: compiled=1 running=1 "));
    if (s_lines == 1) assert(strstr(line, "+MEMTRACE-LIVE: slot=0 "));
    if (s_lines == 3) assert(strstr(line, "+MEMTRACE-FAIL: slot=0 ") && strstr(line, "free=4000 largest=1984"));
    ++s_lines;
}

int main(void)
{
    static const xiaotai_module_t module = {.name = "memtrace", .init = xiaotai_transport_memtrace_init};
    const xiaotai_module_t *modules[] = {&module};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    void *a = __wrap_Mx_malloc(160), *b = __wrap_Mx_calloc(3, 128);
    assert(a && b && status().live == 544 && status().slots == 2);
    a = __wrap_Mx_realloc(a, 320);
    assert(a && status().live == 704 && status().slots == 2);
    xt_memtrace_record_t original;
    assert(xt_memtrace_record(0, &original) && original.address == (uintptr_t)a && original.caller && original.bytes == 320);
    s_fail = true;
    assert(!__wrap_Mx_malloc(4256));
    s_reentrant_slots = XIAOTAI_TRANSPORT_MEMTRACE_SLOTS;
    assert(!__wrap_Mx_realloc(a, 8192));
    s_reentrant_slots = 0;
    assert(status().lost == 2 && status().slots == XIAOTAI_TRANSPORT_MEMTRACE_SLOTS);
    xt_memtrace_record_t after;
    assert(xt_memtrace_record(0, &after) && after.address == original.address &&
           after.bytes == original.bytes && after.caller == original.caller);
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS; ++i) xt_memtrace_free(0x10000U + i);
    for (unsigned i = 0; i < 12; ++i) assert(!__wrap_Mx_calloc(2, 5152));
    assert(status().live == 704 && status().slots == 2 && status().failures == 14 && s_probes == 14);
    xt_memtrace_failure_t first;
    assert(xt_memtrace_failure(0, &first) && first.bytes == 4256 && first.caller && first.operation == 0);
    assert(first.free_bytes == 4000 && first.largest == 1984);
    assert(xt_memtrace_failure(7, &first) && !xt_memtrace_failure(8, &first));
    const xiaotai_reply_t out = {.write = output};
    assert(xiaotai_command_run("AT+MEMTRACE", &out, 1000) == XIAOTAI_OK);
    assert(s_lines == 11);
    xt_memtrace_stop();
    assert(!__wrap_Mx_malloc(4) && s_probes == 14);
    s_fail = false;
    __wrap_Mx_free(b);
    assert(!__wrap_Mx_realloc(a, 0));
    assert(!status().live && !status().slots && !status().running);
    void *ignored = __wrap_Mx_malloc(50);
    assert(ignored && !status().live);
    __wrap_Mx_free(ignored);
    xiaotai_framework_reset();

    /* Start again through the observer's owning registration lifecycle. */
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    assert(status().running && !status().live && !status().slots);
    const unsigned lost = status().lost;
    void *pointers[XIAOTAI_TRANSPORT_MEMTRACE_SLOTS + 1];
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS + 1U; ++i) {
        pointers[i] = __wrap_Mx_malloc(16);
        assert(pointers[i]);
    }
    assert(status().lost == lost + 1U && status().slots == XIAOTAI_TRANSPORT_MEMTRACE_SLOTS &&
           status().live == 16U * XIAOTAI_TRANSPORT_MEMTRACE_SLOTS);
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_MEMTRACE_SLOTS + 1U; ++i) __wrap_Mx_free(pointers[i]);
    assert(!status().live && !status().slots);
    xt_memtrace_stop();
    xiaotai_framework_reset();
    puts("Mx observer: restart, one excess slot, and all allocations freed");
    return 0;
}
