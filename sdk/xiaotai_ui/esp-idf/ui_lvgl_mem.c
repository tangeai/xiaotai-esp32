// SPDX-License-Identifier: MIT
#include "lvgl.h"

#if defined(ESP_PLATFORM) && LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error Native XiaoTai LVGL requires CONFIG_LV_USE_CUSTOM_MALLOC=y
#endif

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include "esp_heap_caps.h"
#include "xiaotai/port/mem.h"

static uint32_t bulk_caps(void)
{
    return MALLOC_CAP_8BIT | (xiaotai_mem_has_external() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
}

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *memory, size_t bytes)
{
    (void)memory;
    (void)bytes;
    return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t bytes) { return xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes); }
void *lv_realloc_core(void *ptr, size_t bytes) { return heap_caps_realloc(ptr, bytes, bulk_caps()); }
void lv_free_core(void *ptr) { xiaotai_mem_free(ptr); }

void lv_mem_monitor_core(lv_mem_monitor_t *out)
{
    multi_heap_info_t heap;
    heap_caps_get_info(&heap, bulk_caps());
    *out = (lv_mem_monitor_t){.total_size = heap.total_free_bytes + heap.total_allocated_bytes,
        .free_cnt = heap.free_blocks, .free_size = heap.total_free_bytes,
        .free_biggest_size = heap.largest_free_block, .used_cnt = heap.allocated_blocks,
        .max_used = heap.total_free_bytes + heap.total_allocated_bytes - heap.minimum_free_bytes};
    if (out->total_size) out->used_pct = (uint8_t)((uint64_t)heap.total_allocated_bytes * 100U / out->total_size);
    if (out->free_size) out->frag_pct = (uint8_t)(100U - (uint64_t)out->free_biggest_size * 100U / out->free_size);
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(bulk_caps(), false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
#endif
