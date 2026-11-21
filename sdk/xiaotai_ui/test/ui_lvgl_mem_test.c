// SPDX-License-Identifier: MIT
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "xiaotai/port/mem.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static bool external;
static void *allocation;

static void check_caps(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_8BIT | (external ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL)));
}

bool xiaotai_mem_has_external(void) { return external; }
void *xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    assert(kind == XIAOTAI_MEM_BULK && !allocation);
    allocation = malloc(size);
    return allocation;
}
void xiaotai_mem_free(void *ptr)
{
    assert(ptr == allocation);
    free(ptr);
    allocation = NULL;
}
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    check_caps(caps);
    assert(ptr == allocation);
    if (!size) return NULL;
    void *next = realloc(ptr, size);
    if (next) allocation = next;
    return next;
}
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps)
{
    check_caps(caps);
    *info = (multi_heap_info_t){70, 30, 35, 65, 2, 3};
}
bool heap_caps_check_integrity(uint32_t caps, bool print_errors)
{
    check_caps(caps);
    assert(!print_errors);
    return !external;
}

int main(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        external = i != 0;
        lv_mem_init();
        char *p = lv_malloc_core(4);
        assert(p);
        memcpy(p, "a1b", 4);
        p = lv_realloc_core(p, 64);
        assert(p && !memcmp(p, "a1b", 4));
        lv_mem_monitor_t metrics;
        lv_mem_monitor_core(&metrics);
        assert(metrics.free_cnt == 3 && metrics.total_size == 100 && metrics.free_size == 70 &&
            metrics.free_biggest_size == 35 && metrics.used_cnt == 2 && metrics.max_used == 35 &&
            metrics.used_pct == 30 && metrics.frag_pct == 50);
        assert(lv_mem_test_core() == (external ? LV_RESULT_INVALID : LV_RESULT_OK));
        lv_free_core(p);
        lv_mem_deinit();
    }
    return 0;
}
