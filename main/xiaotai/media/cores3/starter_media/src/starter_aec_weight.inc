/* Opt-in experiment, pinned to the verified ESP-SR 2.5.0 archives in CMake.
 * The measured middle/normalization operand is allocation 11 (16 KiB).
 * Never edit opaque BSS state or apply this policy to another allocator. */
static EXT_RAM_BSS_ATTR atomic_uintptr_t s_afe_weight;

static void *afe_weight_allocate(int count, int size, int align)
{
    uintptr_t owner = atomic_load(&s_afe_alloc.owner);
    if (!owner || owner != (uintptr_t)xTaskGetCurrentTaskHandle() ||
        s_afe_alloc.count != 11U || count != 4096 || size != 4 || align != 16 ||
        atomic_load(&s_afe_weight)) return NULL;
    void *result = heap_caps_aligned_calloc(16, 4096, 4,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (result) atomic_store(&s_afe_weight, (uintptr_t)result);
    return result;
}

void __real_dl_lib_free(void *pointer);

void __wrap_dl_lib_free(void *pointer)
{
    uintptr_t expected = (uintptr_t)pointer;
    if (pointer && atomic_compare_exchange_strong(&s_afe_weight, &expected, 0)) {
        heap_caps_free(pointer);
        return;
    }
    /* Vendor allocations have a private base pointer before the aligned data.
     * Only our IDF-owned block bypasses that vendor-specific free operation. */
    __real_dl_lib_free(pointer);
}
