/* Bounded development-only CPU1 histogram. The ISR touches internal DRAM only.
 * Skip warmup and duplicate catch-up ticks; freeze after one finite window. */
#define AFE_PC_BUCKETS 128U
#define AFE_PC_WARMUP 2048U
#define AFE_PC_SAMPLES 8192U
#define AFE_PC_MIN_CYCLES 120000U
static DRAM_ATTR struct {
    struct { atomic_uint pc, hits; } bucket[AFE_PC_BUCKETS];
    atomic_uint seen, omitted, duplicates;
    uint32_t last_cycle;
} s_afe_pc;
_Static_assert(sizeof(s_afe_pc) == 1040U, "AFE PC sampler internal RAM budget changed");

static void IRAM_ATTR afe_pc_sample(uint32_t pc, uint32_t cycle)
{
    unsigned seen = atomic_load_explicit(&s_afe_pc.seen, memory_order_relaxed);
    if (seen >= AFE_PC_WARMUP + AFE_PC_SAMPLES) return;
    if (seen && cycle - s_afe_pc.last_cycle < AFE_PC_MIN_CYCLES) {
        atomic_fetch_add_explicit(&s_afe_pc.duplicates, 1U, memory_order_relaxed);
        return;
    }
    s_afe_pc.last_cycle = cycle;
    if (seen >= AFE_PC_WARMUP) {
        pc &= ~31U;
        unsigned slot = ((pc >> 5U) ^ (pc >> 13U)) & (AFE_PC_BUCKETS - 1U);
        bool recorded = false;
        for (unsigned probe = 0; pc && probe < 4U; ++probe) {
            unsigned index = (slot + probe) & (AFE_PC_BUCKETS - 1U);
            unsigned key = atomic_load_explicit(&s_afe_pc.bucket[index].pc, memory_order_relaxed);
            if (!key || key == pc) {
                atomic_store_explicit(&s_afe_pc.bucket[index].pc, pc, memory_order_relaxed);
                atomic_fetch_add_explicit(&s_afe_pc.bucket[index].hits, 1U, memory_order_relaxed);
                recorded = true;
                break;
            }
        }
        if (!recorded) atomic_fetch_add_explicit(&s_afe_pc.omitted, 1U, memory_order_relaxed);
    }
    atomic_store_explicit(&s_afe_pc.seen, seen + 1U, memory_order_release);
}

static void afe_pc_log(void)
{
    unsigned seen = atomic_load_explicit(&s_afe_pc.seen, memory_order_acquire);
    unsigned samples = seen > AFE_PC_WARMUP ? seen - AFE_PC_WARMUP : 0;
    ESP_LOGI(TAG, "BSS PC: samples=%u/%u omitted=%u duplicate=%u frozen=%d bin=32B",
             samples, AFE_PC_SAMPLES, atomic_load(&s_afe_pc.omitted),
             atomic_load(&s_afe_pc.duplicates), samples == AFE_PC_SAMPLES);
    bool printed[AFE_PC_BUCKETS] = {false};
    unsigned shown = 0;
    for (unsigned rank = 0; rank < 20U; ++rank) {
        unsigned best = AFE_PC_BUCKETS, hits = 0;
        for (unsigned n = 0; n < AFE_PC_BUCKETS; ++n) {
            unsigned count = atomic_load(&s_afe_pc.bucket[n].hits);
            if (!printed[n] && count > hits) { best = n; hits = count; }
        }
        if (best == AFE_PC_BUCKETS) break;
        printed[best] = true;
        shown += hits;
        ESP_LOGI(TAG, "BSS PC: rank=%u pc=0x%08x hits=%u", rank,
                 atomic_load(&s_afe_pc.bucket[best].pc), hits);
    }
    ESP_LOGI(TAG, "BSS PC: top20=%u samples=%u", shown, samples);
}
