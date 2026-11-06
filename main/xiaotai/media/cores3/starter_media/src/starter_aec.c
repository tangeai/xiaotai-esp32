/* One official single-mic FD AFE ("MR") owns MIC1 and the MIC3 electrical
 * reference. D039's dual-mic MMR output was proven bit-identical to this MR
 * configuration (finite on-device test, 4 cases x 65536 samples), so its BSS
 * and second AEC/NLP were pure cost. MIC2 is still measured for diagnostics.
 * Acquisition must keep draining DMA independently of AFE processing. */
#include "starter_aec.h"
#include "starter_agc.h"
#include "starter_log.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_afe_sr_models.h"
#include "esp_aec.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
#include "esp_freertos_hooks.h"
#include "xtensa_perfmon_access.h"
#include "xtensa/xt_perf_consts.h"
#endif

/* Input-pool time depth stays 256 ms (D039: 4 x 1024). A 512-sample feed gets
 * 8 slots of the same total duration; this is not a larger buffer. */
#define AFE_INPUT_DEPTH_SAMPLES 4096U
#define AFE_INPUT_SLOTS 8U
#define AFE_FEED_CHANNELS 2U
/* The 1-mic ring holds 1024 B per frame and reserves 8 frames for the VAD
 * cache in silence. 17 frames keep ~9 frames (288 ms) of fetch-stall
 * tolerance, matching D039's MMR ring instead of shrinking it to ~96 ms. */
#define AFE_RING_FRAMES 17U
/* Carried-gap counters saturate at 60 s instead of wrapping during a long
 * read outage; the next accepted frame still marks a gap. */
#define AFE_LOST_SAMPLES_MAX 960000U
#define AFE_MAX_SAMPLES 1024U
#define AFE_IDLE_YIELD_MS 250U
/* A frame can be fetchable just before the feed owner publishes its stamp. */
#define AFE_STAMP_WAIT_MS 20U
#define AFE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
_Static_assert(AFE_MAX_SAMPLES <= INT32_MAX / 32768U, "PCM level sum must fit int32_t");

typedef struct {
    int64_t captured_ms;
    uint32_t capture_epoch, mute_epoch, mic_clipped, reference_clipped;
    uint32_t lost_samples; /* Gap before this frame; set on the first chunk only. */
    starter_signal_level_t mic[2], reference;
} afe_frame_stamp_t;

typedef struct {
    int16_t *pcm;
    afe_frame_stamp_t stamp[2];
} afe_input_slot_t;

typedef struct {
    const esp_afe_sr_iface_t *iface;
    esp_afe_sr_data_t *handle;
    size_t feed_samples, fetch_samples, chunks_per_feed, input_slots;
    int16_t *tdm, *clean, *uplink;
    afe_input_slot_t input[AFE_INPUT_SLOTS];
    QueueHandle_t free_slots, pending_slots, stamps;
    TaskHandle_t feed_worker;
    /* Gap carried to the next frame: ADC owner, then fetch consumer. */
    uint32_t capture_lost, fetch_lost, stamps_owed;
    atomic_bool failed;
    atomic_uint feed_frames, fetch_frames, input_full, fetch_timeouts;
    atomic_uint feed_ring_full, stamp_timeouts;
    atomic_uint max_feed_us, last_feed_us;
    atomic_uint feed_avg_us;
    uint64_t feed_total_us;
    atomic_bool detail_enabled;
    atomic_uint aec_calls, aec_avg_us, aec_max_us;
    uint64_t aec_total_us; /* Written only by the official feed owner. */
    atomic_uint nlp_calls, nlp_avg_us, nlp_max_us;
    uint64_t nlp_total_us;
    atomic_uint se_calls, se_avg_us, se_max_us;
    uint64_t se_total_us;
    atomic_int channel_id;
} starter_aec_context_t;

static const char *TAG = "starter_aec";
static EXT_RAM_BSS_ATTR starter_aec_context_t s_aec;

#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
/* Tick ISR reads only these 32 internal bytes, never the PSRAM AFE context.
 * Samples distinguish scheduler preemption from time spent on the DSP task;
 * the latter still includes memory stalls. No RTOS trace/ABI change is needed. */
static DRAM_ATTR struct {
    atomic_uintptr_t owner;
    atomic_uint stage;
    atomic_uint running[3], preempted[3];
} s_afe_cpu;
_Static_assert(sizeof(s_afe_cpu) == 32U, "AFE tick sampler internal RAM budget changed");
_Static_assert(_Alignof(atomic_uint) >= 4U, "AFE ISR counters must be word-aligned");
/* IDF disables inline atomics for its PSRAM workaround. Aligned DRAM counters
 * take the official S32C1I fast path instead of the PSRAM spinlock path. */
#if !CONFIG_LIBC_MISC_IN_IRAM
#error "AFE tick sampler requires IDF atomic helpers in IRAM"
#endif
static bool s_afe_tick_registered;

#if !CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL1
#error "AFE interrupted-PC probe requires the verified level-1 SYSTIMER path"
#endif
#include "starter_aec_pc.inc"

/* Hardware counters are per-core, so totals include intervening ISRs/tasks.
 * IDF marks the architectural D-cache-miss selector unused on ESP32-S3.
 * Separate supported stall causes instead of interpreting its zero as a hit. */
#define AFE_PERF_MODES 8U
static const uint16_t s_afe_perf_masks[AFE_PERF_MODES] = {
    XTPERF_MASK_D_STALL_ALL, XTPERF_MASK_D_STALL_BUSY,
    XTPERF_MASK_D_STALL_IN_PIF,
    XTPERF_MASK_D_STALL_STORE_BUF_FULL | XTPERF_MASK_D_STALL_STORE_BUF_CONFLICT,
    XTPERF_MASK_D_STALL_MHT_LOOKUP, XTPERF_MASK_D_STALL_UNCACHED_LOAD,
    XTPERF_MASK_D_STALL_BANK_CONFLICT, XTPERF_MASK_I_STALL_ALL,
};
static EXT_RAM_BSS_ATTR struct {
    struct {
        uint64_t cycles[AFE_PERF_MODES], stalls[AFE_PERF_MODES];
        uint32_t calls[AFE_PERF_MODES];
        unsigned next_mode;
        atomic_uint avg_cycles, stall_pm[AFE_PERF_MODES];
    } stage[3];
    bool active;
    unsigned active_stage, active_mode;
    atomic_uint errors;
} s_afe_perf;

#include "starter_aec_preempt.inc"

static void IRAM_ATTR afe_cpu_tick(void)
{
    unsigned stage = atomic_load_explicit(&s_afe_cpu.stage, memory_order_relaxed);
    if (stage == 0 || stage > 3) return;
    uintptr_t owner = atomic_load_explicit(&s_afe_cpu.owner, memory_order_relaxed);
    if (!owner) return;
    uintptr_t current = (uintptr_t)xTaskGetCurrentTaskHandle();
    bool running = current == owner;
    atomic_uint *counter = running
        ? &s_afe_cpu.running[stage - 1U] : &s_afe_cpu.preempted[stage - 1U];
    atomic_fetch_add_explicit(counter, 1U, memory_order_relaxed);
    if (!running) afe_preempt_sample(current);
#if defined(__XTENSA__)
    if (stage == 3U && running) {
        uint32_t pc, cycle;
        __asm__ __volatile__("rsr.epc1 %0\nrsr.ccount %1" : "=a"(pc), "=a"(cycle));
        afe_pc_sample(pc, cycle);
    }
#endif
}

static unsigned afe_cpu_begin(unsigned stage)
{
    unsigned previous = atomic_exchange_explicit(&s_afe_cpu.stage, stage, memory_order_relaxed);
    if (previous || stage == 0 || stage > 3 || !atomic_load(&s_afe_cpu.owner) ||
        (uintptr_t)xTaskGetCurrentTaskHandle() != atomic_load(&s_afe_cpu.owner)) return previous;
    unsigned mode = s_afe_perf.stage[stage - 1U].next_mode;
    xtensa_perfmon_stop();
    esp_err_t cycles = xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, XTPERF_MASK_CYCLES, 0, 6);
    esp_err_t stalls = xtensa_perfmon_init(1,
        mode < AFE_PERF_MODES - 1U ? XTPERF_CNT_D_STALL : XTPERF_CNT_I_STALL,
        s_afe_perf_masks[mode], 0, 6);
    if (cycles != ESP_OK || stalls != ESP_OK) {
        atomic_fetch_add(&s_afe_perf.errors, 1U);
        return previous;
    }
    s_afe_perf.active_stage = stage - 1U;
    s_afe_perf.active_mode = mode;
    s_afe_perf.active = true;
    xtensa_perfmon_start();
    return previous;
}

static void afe_cpu_end(unsigned previous)
{
    if (previous == 0 && s_afe_perf.active) {
        xtensa_perfmon_stop();
        uint32_t cycles = xtensa_perfmon_value(0), stalls = xtensa_perfmon_value(1);
        unsigned stage = s_afe_perf.active_stage, mode = s_afe_perf.active_mode;
        s_afe_perf.active = false;
        if (xtensa_perfmon_overflow(0) != ESP_OK || xtensa_perfmon_overflow(1) != ESP_OK || !cycles) {
            atomic_fetch_add(&s_afe_perf.errors, 1U);
        } else {
            s_afe_perf.stage[stage].cycles[mode] += cycles;
            s_afe_perf.stage[stage].stalls[mode] += stalls;
            ++s_afe_perf.stage[stage].calls[mode];
            uint64_t total_cycles = 0;
            uint32_t total_calls = 0;
            for (unsigned i = 0; i < AFE_PERF_MODES; ++i) {
                total_cycles += s_afe_perf.stage[stage].cycles[i];
                total_calls += s_afe_perf.stage[stage].calls[i];
            }
            atomic_store(&s_afe_perf.stage[stage].avg_cycles, (uint32_t)(total_cycles / total_calls));
            atomic_store(&s_afe_perf.stage[stage].stall_pm[mode], (uint32_t)(
                s_afe_perf.stage[stage].stalls[mode] * 1000U / s_afe_perf.stage[stage].cycles[mode]));
        }
        s_afe_perf.stage[stage].next_mode = (mode + 1U) % AFE_PERF_MODES;
    }
    atomic_store_explicit(&s_afe_cpu.stage, previous, memory_order_relaxed);
}

/* Initialization records also identify the real kernel operands. No opaque
 * vendor state is dereferenced or moved by this development-only probe. */
#define AFE_ALLOC_RECORDS 24U
static EXT_RAM_BSS_ATTR struct {
    atomic_uintptr_t owner;
    unsigned count, omitted;
    struct {
        int count, size, align;
        uintptr_t address;
        bool psram_request, external, failed;
    } record[AFE_ALLOC_RECORDS];
} s_afe_alloc;

#if XIAOTAI_AFE_KERNEL_PROBE
static int afe_allocation_id(const void *pointer, uint32_t *offset)
{
    uintptr_t address = (uintptr_t)pointer;
    *offset = 0;
    if (!address) return -1;
    for (unsigned n = 0; n < s_afe_alloc.count; ++n) {
        if (s_afe_alloc.record[n].failed || s_afe_alloc.record[n].count <= 0 ||
            s_afe_alloc.record[n].size <= 0 ||
            (size_t)s_afe_alloc.record[n].count > SIZE_MAX / (size_t)s_afe_alloc.record[n].size)
            continue;
        size_t bytes = (size_t)s_afe_alloc.record[n].count * (size_t)s_afe_alloc.record[n].size;
        uintptr_t base = s_afe_alloc.record[n].address;
        if (base && address >= base && address - base < bytes) {
            *offset = (uint32_t)(address - base);
            return (int)n;
        }
    }
    return -1;
}

#include "starter_aec_kernels.inc"
#else
void starter_aec_log_kernels(void)
{
    afe_preempt_log();
    afe_pc_log();
    ESP_LOGI(TAG, "BSS inner-kernel probe disabled; stage timing remains active");
}
#endif

void __real_aec_linear_process(const aec_handle_t *handle, int16_t *in, int16_t *ref, int16_t *out);

void __wrap_aec_linear_process(const aec_handle_t *handle, int16_t *in, int16_t *ref, int16_t *out)
{
    if (!atomic_load_explicit(&s_aec.detail_enabled, memory_order_relaxed)) {
        __real_aec_linear_process(handle, in, ref, out);
        return;
    }
    if (atomic_load(&s_aec.aec_calls) == 0) {
        ESP_LOGI(TAG, "AEC linear: samples=%d mic=%d caps=0x%lx pcm-external=%d/%d/%d",
                 handle->frame_size, handle->config.mic_num, (unsigned long)handle->config.caps,
                 esp_ptr_external_ram(in), esp_ptr_external_ram(ref), esp_ptr_external_ram(out));
    }
    int64_t started = esp_timer_get_time();
    unsigned previous = afe_cpu_begin(1U);
    __real_aec_linear_process(handle, in, ref, out);
    afe_cpu_end(previous);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    s_aec.aec_total_us += elapsed;
    uint32_t count = atomic_fetch_add(&s_aec.aec_calls, 1) + 1U;
    atomic_store(&s_aec.aec_avg_us, (uint32_t)(s_aec.aec_total_us / count));
    if (elapsed > atomic_load(&s_aec.aec_max_us)) atomic_store(&s_aec.aec_max_us, elapsed);
}

int __real_aec_nlp_process(const aec_handle_t *handle, int16_t *out);

int __wrap_aec_nlp_process(const aec_handle_t *handle, int16_t *out)
{
    if (!atomic_load_explicit(&s_aec.detail_enabled, memory_order_relaxed))
        return __real_aec_nlp_process(handle, out);
    int64_t started = esp_timer_get_time();
    unsigned previous = afe_cpu_begin(2U);
    int result = __real_aec_nlp_process(handle, out);
    afe_cpu_end(previous);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    s_aec.nlp_total_us += elapsed;
    uint32_t count = atomic_fetch_add(&s_aec.nlp_calls, 1) + 1U;
    atomic_store(&s_aec.nlp_avg_us, (uint32_t)(s_aec.nlp_total_us / count));
    if (elapsed > atomic_load(&s_aec.nlp_max_us)) atomic_store(&s_aec.nlp_max_us, elapsed);
    return result;
}

/* ESP-SR 2.4.7/2.5.0 archive ABI: one opaque BSS context, planar input/output,
 * void return. This development probe never owns or changes that context.
 * The MR pipeline has no SE stage, so the BSS probes stay idle at runtime. */
void __real_esp_bss_hps16fft_process(void *handle, int16_t *in, int16_t *out);

void __wrap_esp_bss_hps16fft_process(void *handle, int16_t *in, int16_t *out)
{
    if (!atomic_load_explicit(&s_aec.detail_enabled, memory_order_relaxed)) {
        __real_esp_bss_hps16fft_process(handle, in, out);
        return;
    }
    int64_t started = esp_timer_get_time();
    unsigned previous = afe_cpu_begin(3U);
    __real_esp_bss_hps16fft_process(handle, in, out);
    afe_cpu_end(previous);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    s_aec.se_total_us += elapsed;
    uint32_t count = atomic_fetch_add(&s_aec.se_calls, 1) + 1U;
    atomic_store(&s_aec.se_avg_us, (uint32_t)(s_aec.se_total_us / count));
    if (elapsed > atomic_load(&s_aec.se_max_us)) atomic_store(&s_aec.se_max_us, elapsed);
}

/* The official HIGH_PERF dual-mic path uses the same void/context/PCM ABI. */
void __real_esp_bss_728_process(void *handle, int16_t *in, int16_t *out);

void __wrap_esp_bss_728_process(void *handle, int16_t *in, int16_t *out)
{
    if (!atomic_load_explicit(&s_aec.detail_enabled, memory_order_relaxed)) {
        __real_esp_bss_728_process(handle, in, out);
        return;
    }
    int64_t started = esp_timer_get_time();
    unsigned previous = afe_cpu_begin(3U);
    __real_esp_bss_728_process(handle, in, out);
    afe_cpu_end(previous);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    s_aec.se_total_us += elapsed;
    uint32_t count = atomic_fetch_add(&s_aec.se_calls, 1) + 1U;
    atomic_store(&s_aec.se_avg_us, (uint32_t)(s_aec.se_total_us / count));
    if (elapsed > atomic_load(&s_aec.se_max_us)) atomic_store(&s_aec.se_max_us, elapsed);
}

/* Initialization-only allocation evidence. Preserve the vendor allocator and
 * opaque context; never infer placement from memory mode or malloc threshold. */
static void afe_record_allocation(int count, int size, int align, bool psram, void *result)
{
    uintptr_t owner = atomic_load(&s_afe_alloc.owner);
    if (!owner || owner != (uintptr_t)xTaskGetCurrentTaskHandle()) return;
    if (s_afe_alloc.count >= AFE_ALLOC_RECORDS) {
        ++s_afe_alloc.omitted;
        return;
    }
    unsigned n = s_afe_alloc.count++;
    s_afe_alloc.record[n].count = count;
    s_afe_alloc.record[n].size = size;
    s_afe_alloc.record[n].align = align;
    s_afe_alloc.record[n].address = (uintptr_t)result;
    s_afe_alloc.record[n].psram_request = psram;
    s_afe_alloc.record[n].external = result && esp_ptr_external_ram(result);
    s_afe_alloc.record[n].failed = !result;
}

void *__real_dl_lib_calloc(int count, int size, int align);
void *__real_dl_lib_calloc_psram(int count, int size, int align);

#if XIAOTAI_BSS_WEIGHT_INTERNAL
#include "starter_aec_weight.inc"
#endif

void *__wrap_dl_lib_calloc(int count, int size, int align)
{
    void *result = __real_dl_lib_calloc(count, size, align);
    afe_record_allocation(count, size, align, false, result);
    return result;
}

void *__wrap_dl_lib_calloc_psram(int count, int size, int align)
{
    void *result = NULL;
#if XIAOTAI_BSS_WEIGHT_INTERNAL
    result = afe_weight_allocate(count, size, align);
#endif
    if (!result) result = __real_dl_lib_calloc_psram(count, size, align);
    afe_record_allocation(count, size, align, true, result);
    return result;
}

/* ESP-SR 2.4.7/2.5.0 DWARF/ABI: frame length, memory mode, opaque result. */
void *__real_esp_bss_hps16fft_init(int frame_ms, int memory_mode);

void *__wrap_esp_bss_hps16fft_init(int frame_ms, int memory_mode)
{
    if (atomic_load(&s_afe_alloc.owner))
        return __real_esp_bss_hps16fft_init(frame_ms, memory_mode);
    size_t ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_afe_alloc.count = s_afe_alloc.omitted = 0;
    atomic_store(&s_afe_alloc.owner, (uintptr_t)xTaskGetCurrentTaskHandle());
    void *result = __real_esp_bss_hps16fft_init(frame_ms, memory_mode);
    atomic_store(&s_afe_alloc.owner, 0);
    ESP_LOGI(TAG, "BSS init: frame_ms=%d mem=%d ok=%d RAM/PSRAM=%d/%d B alloc=%u omitted=%u",
        frame_ms, memory_mode, result != NULL,
        (int)(ram - heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        (int)(psram - heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        s_afe_alloc.count, s_afe_alloc.omitted);
    for (unsigned n = 0; n < s_afe_alloc.count; ++n) {
        ESP_LOGI(TAG, "BSS alloc: n=%u count=%d size=%d align=%d request=%s actual=%s phase=%u",
            n, s_afe_alloc.record[n].count, s_afe_alloc.record[n].size,
            s_afe_alloc.record[n].align, s_afe_alloc.record[n].psram_request ? "PSRAM" : "default",
            s_afe_alloc.record[n].failed ? "failed" :
                s_afe_alloc.record[n].external ? "PSRAM" : "RAM",
            (unsigned)(s_afe_alloc.record[n].address & 0x3fffU));
    }
    return result;
}
#endif

#if !CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
void starter_aec_log_kernels(void) {}
#endif

static uint32_t lost_add(uint32_t total, uint32_t samples)
{
    uint64_t sum = (uint64_t)total + samples;
    return sum > AFE_LOST_SAMPLES_MAX ? AFE_LOST_SAMPLES_MAX : (uint32_t)sum;
}

static int16_t *allocate_samples(size_t count)
{
    return heap_caps_aligned_calloc(16, count, sizeof(int16_t), AFE_CAPS);
}

static void fail_pipeline(const char *stage, int result)
{
    if (!atomic_exchange(&s_aec.failed, true))
        ESP_LOGE(TAG, "AFE fail: %s rc=%d", stage, result);
}

static void afe_feed_yield(TickType_t *last_yield)
{
    /* The pending queue already blocks under normal load. Under backlog, let
     * IDLE1 run without adding 1 ms to every 64 ms audio frame. */
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - *last_yield) >= pdMS_TO_TICKS(AFE_IDLE_YIELD_MS)) {
        vTaskDelay(pdMS_TO_TICKS(1));
        *last_yield = xTaskGetTickCount();
    }
}

/* Feed owner only. feed() == 0 is the library's recoverable ring-full
 * back-pressure: it already advanced AEC state but wrote no PCM, so the block
 * is dropped, never retried, and reported as a gap before the next fed block.
 * Stamps follow a successful feed, giving every fetched frame exactly one. */
static void afe_feed_block(afe_input_slot_t *input, uint32_t *dropped_samples)
{
    int64_t started = esp_timer_get_time();
    int ret = s_aec.iface->feed(s_aec.handle, input->pcm);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    atomic_store(&s_aec.last_feed_us, elapsed);
    if (elapsed > atomic_load(&s_aec.max_feed_us))
        atomic_store(&s_aec.max_feed_us, elapsed);
    if (ret < 0) {
        fail_pipeline("feed", ret);
        return;
    }
    if (ret == 0) {
        atomic_fetch_add(&s_aec.feed_ring_full, 1);
        *dropped_samples = lost_add(lost_add(*dropped_samples, input->stamp[0].lost_samples),
                                    (uint32_t)s_aec.feed_samples);
        return;
    }
    s_aec.feed_total_us += elapsed;
    unsigned frames = atomic_fetch_add(&s_aec.feed_frames, 1) + 1U;
    atomic_store(&s_aec.feed_avg_us, (uint32_t)(s_aec.feed_total_us / frames));
    input->stamp[0].lost_samples = lost_add(input->stamp[0].lost_samples, *dropped_samples);
    *dropped_samples = 0;
    for (size_t n = 0; n < s_aec.chunks_per_feed; ++n) {
        /* Capacity exceeds the AFE ring, so a full queue is an invariant error. */
        if (xQueueSend(s_aec.stamps, &input->stamp[n], 0) != pdTRUE) {
            fail_pipeline("timestamp queue", -1);
            return;
        }
    }
}

static void afe_feed_worker(void *argument)
{
    (void)argument;
    TickType_t last_yield = xTaskGetTickCount();
    uint32_t dropped_samples = 0;
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
    atomic_store(&s_afe_cpu.owner, (uintptr_t)xTaskGetCurrentTaskHandle());
#endif
    for (;;) {
        uint8_t slot;
        if (xQueueReceive(s_aec.pending_slots, &slot, portMAX_DELAY) != pdTRUE) continue;
        if (!atomic_load(&s_aec.failed)) afe_feed_block(&s_aec.input[slot], &dropped_samples);
        (void)xQueueSend(s_aec.free_slots, &slot, 0);
        afe_feed_yield(&last_yield);
    }
}

static void starter_aec_release(void)
{
    /* Initialization failure only, before the ADC reader starts. */
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
    if (s_afe_tick_registered) {
        esp_deregister_freertos_tick_hook_for_cpu(afe_cpu_tick, 1);
        s_afe_tick_registered = false;
    }
#endif
    if (s_aec.feed_worker) vTaskDeleteWithCaps(s_aec.feed_worker);
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
    atomic_store(&s_afe_cpu.stage, 0);
    atomic_store(&s_afe_cpu.owner, 0);
#endif
    if (s_aec.free_slots) vQueueDeleteWithCaps(s_aec.free_slots);
    if (s_aec.pending_slots) vQueueDeleteWithCaps(s_aec.pending_slots);
    if (s_aec.stamps) vQueueDeleteWithCaps(s_aec.stamps);
    for (size_t i = 0; i < AFE_INPUT_SLOTS; ++i) heap_caps_free(s_aec.input[i].pcm);
    heap_caps_free(s_aec.tdm);
    heap_caps_free(s_aec.clean);
    heap_caps_free(s_aec.uplink);
    starter_agc_deinit();
    if (s_aec.handle) s_aec.iface->destroy(s_aec.handle);
    memset(&s_aec, 0, sizeof(s_aec));
}

esp_err_t starter_aec_init(void)
{
    if (s_aec.handle) return ESP_OK;
    atomic_store(&s_aec.detail_enabled, true);
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    afe_config_t *config = afe_config_init("MR", NULL, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (!config) return ESP_ERR_NO_MEM;
    config->aec_init = true;
    config->aec_mode = AEC_MODE_FD_LOW_COST;
    config->aec_filter_length = 4;
    /* NORMAL leaked the cold-start greeting into ASR in the acoustic test.
     * Keep the official default, not VERYAGGR; preserve ADC headroom below. */
    config->aec_nlp_level = AEC_NLP_LEVEL_AGGR;
    config->se_init = false; /* Single mic: no BSS. D039 discarded its outputs. */
    config->ns_init = false; /* Unchanged from D039: no added NS stage. */
    config->vad_init = true;
    config->vad_model_name = NULL; /* Official WebRTC VAD, no extra neural model. */
    config->vad_mode = VAD_MODE_2;
    config->vad_enable_channel_trigger = true;
    config->vad_mute_playback = false;
    config->wakenet_init = false; /* Keep the existing custom wake word in voice. */
    config->agc_init = false; /* One existing ESP-SR WebRTC AGC follows fetch. */
    config->afe_linear_gain = STARTER_AEC_OUTPUT_GAIN;
    /* DSP working-set policy; PCM pools and queues remain in PSRAM. */
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    config->afe_ringbuf_size = AFE_RING_FRAMES;
    /* AEC/NLP execute synchronously in the feed owner (CPU1). This
     * preference applies only to an AFE-owned worker. */
    config->afe_perferred_core = 0;
    config->afe_perferred_priority = 7;
    config->fixed_first_channel = false;
    config->fixed_output_channel = false;
    config->output_playback_channel = false;
    afe_config_check(config);
    if (!config->aec_init || config->aec_mode != AEC_MODE_FD_LOW_COST ||
        config->aec_filter_length != 4 || config->aec_nlp_level != AEC_NLP_LEVEL_AGGR ||
        config->se_init || config->ns_init || !config->vad_init ||
        config->fixed_output_channel || config->pcm_config.mic_num != 1 ||
        config->pcm_config.ref_num != 1 ||
        config->pcm_config.total_ch_num != (int)AFE_FEED_CHANNELS) {
        afe_config_free(config);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_aec.iface = esp_afe_handle_from_config(config);
    if (s_aec.iface) s_aec.handle = s_aec.iface->create_from_config(config);
    afe_config_free(config);
    if (!s_aec.handle) return ESP_ERR_NO_MEM;
    int feed = s_aec.iface->get_feed_chunksize(s_aec.handle);
    int fetch = s_aec.iface->get_fetch_chunksize(s_aec.handle);
    if (feed <= 0 || fetch <= 0 || feed > AFE_MAX_SAMPLES || fetch > feed ||
        feed % fetch != 0 || feed / fetch > 2 || (fetch & 1) ||
        AFE_INPUT_DEPTH_SAMPLES % (unsigned)feed != 0 ||
        AFE_INPUT_DEPTH_SAMPLES / (unsigned)feed > AFE_INPUT_SLOTS ||
        AFE_INPUT_DEPTH_SAMPLES / (unsigned)feed < 2U ||
        s_aec.iface->get_feed_channel_num(s_aec.handle) != (int)AFE_FEED_CHANNELS ||
        s_aec.iface->get_fetch_channel_num(s_aec.handle) != 1 ||
        s_aec.iface->get_samp_rate(s_aec.handle) != STARTER_AEC_SAMPLE_RATE_HZ) {
        starter_aec_release();
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_aec.feed_samples = (size_t)feed;
    s_aec.fetch_samples = (size_t)fetch;
    s_aec.chunks_per_feed = (size_t)(feed / fetch);
    s_aec.input_slots = AFE_INPUT_DEPTH_SAMPLES / (size_t)feed;
    s_aec.tdm = allocate_samples(s_aec.feed_samples * STARTER_AEC_CAPTURE_DMA_CHANNELS);
    s_aec.clean = allocate_samples(s_aec.fetch_samples);
    s_aec.free_slots = xQueueCreateWithCaps(s_aec.input_slots, sizeof(uint8_t), AFE_CAPS);
    s_aec.pending_slots = xQueueCreateWithCaps(s_aec.input_slots, sizeof(uint8_t), AFE_CAPS);
    /* Same stamp-queue capacity in fetch frames as D039 (12 x 2 ring + 4 x 2 input). */
    s_aec.stamps = xQueueCreateWithCaps(AFE_RING_FRAMES * 2U +
                                      s_aec.input_slots * s_aec.chunks_per_feed,
                                      sizeof(afe_frame_stamp_t), AFE_CAPS);
    bool buffers_ok = s_aec.tdm && s_aec.clean &&
                      s_aec.free_slots && s_aec.pending_slots && s_aec.stamps;
    for (uint8_t i = 0; i < s_aec.input_slots; ++i) {
        s_aec.input[i].pcm = allocate_samples(s_aec.feed_samples * AFE_FEED_CHANNELS);
        buffers_ok = buffers_ok && s_aec.input[i].pcm;
        if (s_aec.free_slots) (void)xQueueSend(s_aec.free_slots, &i, 0);
    }
#if CONFIG_XIAOTAI_CAPTURE_AGC
    s_aec.uplink = allocate_samples(s_aec.fetch_samples);
    buffers_ok = buffers_ok && s_aec.uplink && starter_agc_init() == ESP_OK;
#endif
    if (!buffers_ok || xTaskCreatePinnedToCoreWithCaps(afe_feed_worker, "afe_feed", 8192,
            NULL, 7, &s_aec.feed_worker, 1, AFE_CAPS) != pdPASS) {
        starter_aec_release();
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
    esp_err_t tick_result = esp_register_freertos_tick_hook_for_cpu(afe_cpu_tick, 1);
    if (tick_result != ESP_OK) {
        starter_aec_release();
        return tick_result;
    }
    s_afe_tick_registered = true;
#endif
    s_aec.iface->print_pipeline(s_aec.handle);
    ESP_LOGI(TAG, "AFE 2.5.0 MR/16k in/out=%d/%d FD-L NLP=AGGR tail=4 "
             "SE=off VAD=W2 pga=%ddB gain=%d/1000 core=feed1/fetch0 feed-prio=7 mem=balanced q=%u slots=%u RAM/PSRAM=%u/%u B",
             feed, fetch, (int)STARTER_AEC_MIC_PGA_DB,
             (int)(STARTER_AEC_OUTPUT_GAIN * 1000.0f), AFE_RING_FRAMES, (unsigned)s_aec.input_slots,
             (unsigned)(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             (unsigned)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ESP_OK;
}

size_t starter_aec_capture_bytes(void)
{
    return s_aec.feed_samples * STARTER_AEC_CAPTURE_DMA_CHANNELS * sizeof(int16_t);
}

int16_t *starter_aec_capture_buffer(void) { return s_aec.tdm; }
size_t starter_aec_frame_samples(void) { return s_aec.fetch_samples; }

esp_err_t starter_aec_submit_capture(size_t bytes, int64_t captured_ms,
                                     uint32_t capture_epoch, uint32_t mute_epoch)
{
    if (!s_aec.handle || bytes != starter_aec_capture_bytes()) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_aec.failed)) return ESP_ERR_INVALID_STATE;
    uint8_t slot;
    if (xQueueReceive(s_aec.free_slots, &slot, 0) != pdTRUE) {
        unsigned count = atomic_fetch_add(&s_aec.input_full, 1) + 1U;
        if (count == 1 || count % 100U == 0)
            starter_log_record(STARTER_LOG_AFE_FULL,
                               (const uint32_t[STARTER_LOG_VALUES]){count});
        s_aec.capture_lost = lost_add(s_aec.capture_lost, (uint32_t)s_aec.feed_samples);
        return ESP_ERR_NO_MEM;
    }
    afe_input_slot_t *input = &s_aec.input[slot];
    /* Repack M/R/M/N to M/R and measure M1/M2/R in the same pass. MIC2 is
     * measured for diagnostics only; neither gain nor MIC1/reference PCM changes. */
    for (size_t f = 0; f < s_aec.chunks_per_feed; ++f) {
        afe_frame_stamp_t *stamp = &input->stamp[f];
        memset(stamp, 0, sizeof(*stamp));
        stamp->captured_ms = captured_ms - (int64_t)((s_aec.chunks_per_feed - f - 1U) *
            s_aec.fetch_samples * 1000U / STARTER_AEC_SAMPLE_RATE_HZ);
        stamp->capture_epoch = capture_epoch;
        stamp->mute_epoch = mute_epoch;
        if (f == 0) stamp->lost_samples = s_aec.capture_lost;
        int32_t sum[3] = {0};
        uint64_t square[3] = {0};
        uint32_t peak[3] = {0};
        for (size_t i = 0; i < s_aec.fetch_samples; ++i) {
            size_t sample = f * s_aec.fetch_samples + i;
            const int32_t values[3] = {
                s_aec.tdm[sample * STARTER_AEC_CAPTURE_DMA_CHANNELS + STARTER_AEC_MIC_SLOT],
                s_aec.tdm[sample * STARTER_AEC_CAPTURE_DMA_CHANNELS + STARTER_AEC_SECOND_MIC_SLOT],
                s_aec.tdm[sample * STARTER_AEC_CAPTURE_DMA_CHANNELS + STARTER_AEC_REFERENCE_SLOT],
            };
            input->pcm[sample * AFE_FEED_CHANNELS] = (int16_t)values[0];
            input->pcm[sample * AFE_FEED_CHANNELS + 1U] = (int16_t)values[2];
            for (size_t c = 0; c < 3; ++c) {
                int32_t value = values[c];
                uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
                sum[c] += value;
                square[c] += (uint32_t)(value * value);
                if (magnitude > peak[c]) peak[c] = magnitude;
                if (magnitude >= 32700U) {
                    if (c == 2) ++stamp->reference_clipped;
                    else ++stamp->mic_clipped;
                }
            }
        }
        for (size_t c = 0; c < 3; ++c) {
            starter_signal_level_t *level = c < 2 ? &stamp->mic[c] : &stamp->reference;
            level->peak = peak[c];
            level->dc = sum[c] / (int32_t)s_aec.fetch_samples;
            level->rms = starter_signal_isqrt(square[c] / s_aec.fetch_samples);
        }
    }
    if (xQueueSend(s_aec.pending_slots, &slot, 0) != pdTRUE) {
        (void)xQueueSend(s_aec.free_slots, &slot, 0);
        fail_pipeline("input queue", -1);
        return ESP_FAIL;
    }
    s_aec.capture_lost = 0;
    return ESP_OK;
}

void starter_aec_report_capture_loss(void)
{
    /* Same single caller as submit. An estimate: one requested block, which a
     * partial or stalled read can under- or overstate. */
    if (s_aec.handle) s_aec.capture_lost = lost_add(s_aec.capture_lost, (uint32_t)s_aec.feed_samples);
}

esp_err_t starter_aec_fetch(starter_aec_output_t *output)
{
    if (!s_aec.handle || !output) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_aec.failed)) return ESP_ERR_INVALID_STATE;
    int64_t fetch_begin = esp_timer_get_time();
    afe_fetch_result_t *result = s_aec.iface->fetch_with_delay(s_aec.handle, pdMS_TO_TICKS(200));
    uint32_t fetch_wait_us = (uint32_t)(esp_timer_get_time() - fetch_begin);
    output->fetch_wait_us = fetch_wait_us;
    if (!result || result->ret_value != ESP_OK || !result->data) {
        atomic_fetch_add(&s_aec.fetch_timeouts, 1);
        return ESP_ERR_TIMEOUT;
    }
    if (result->data_size != s_aec.fetch_samples * sizeof(int16_t)) {
        fail_pipeline("fetch size", result->data_size);
        return ESP_ERR_INVALID_SIZE;
    }
    /* Stamps are published in feed order, one per frame. A frame whose stamp
     * misses the wait is dropped as a gap; its late stamp is skipped next. */
    afe_frame_stamp_t stamp;
    for (;;) {
        if (xQueueReceive(s_aec.stamps, &stamp, pdMS_TO_TICKS(AFE_STAMP_WAIT_MS)) != pdTRUE) {
            ++s_aec.stamps_owed;
            s_aec.fetch_lost = lost_add(s_aec.fetch_lost, (uint32_t)s_aec.fetch_samples);
            atomic_fetch_add(&s_aec.stamp_timeouts, 1);
            return ESP_ERR_TIMEOUT;
        }
        s_aec.fetch_lost = lost_add(s_aec.fetch_lost, stamp.lost_samples);
        if (s_aec.stamps_owed == 0) break;
        --s_aec.stamps_owed;
    }
    uint32_t lost_samples = s_aec.fetch_lost;
    s_aec.fetch_lost = 0;
    atomic_fetch_add(&s_aec.fetch_frames, 1);
    atomic_store(&s_aec.channel_id, result->trigger_channel_id);
    /* VAD selects channels, it does not gate this continuous stream.
     * Replaying its speech-onset cache here would duplicate old samples. */
    memcpy(s_aec.clean, result->data, result->data_size);
    const int16_t *uplink = s_aec.clean;
    uint32_t agc_us = 0;
#if CONFIG_XIAOTAI_CAPTURE_AGC
    /* The AGC delay line holds pre-gap samples; silence them so the gap stays
     * immediately before this frame on the delayed transport/wake path too. */
    if (lost_samples) starter_agc_discard_pending();
    int64_t agc_begin = esp_timer_get_time();
    esp_err_t ret = starter_agc_process(s_aec.clean, s_aec.uplink, s_aec.fetch_samples);
    agc_us = (uint32_t)(esp_timer_get_time() - agc_begin);
    output->agc_us = agc_us;
    if (ret != ESP_OK) {
        s_aec.fetch_lost = lost_add(lost_samples, (uint32_t)s_aec.fetch_samples);
        return ret;
    }
    uplink = s_aec.uplink;
#endif
    int64_t measure_start = esp_timer_get_time();
    starter_signal_level_t clean_level = starter_signal_measure(s_aec.clean, s_aec.fetch_samples);
    starter_signal_level_t uplink_level = starter_signal_measure(uplink, s_aec.fetch_samples);
    *output = (starter_aec_output_t) {
        .pcm_16k = s_aec.clean, .samples_16k = s_aec.fetch_samples,
#if CONFIG_XIAOTAI_WAKE_CAPTURE_AGC
        .wake_pcm_16k = uplink, .wake_delay_ms = STARTER_AGC_DELAY_MS,
#else
        .wake_pcm_16k = s_aec.clean, .wake_delay_ms = 0,
#endif
        .transport_pcm = uplink, .transport_samples = s_aec.fetch_samples,
        .captured_ms = stamp.captured_ms, .capture_epoch = stamp.capture_epoch,
        .mute_epoch = stamp.mute_epoch, .lost_samples = lost_samples,
        .mic_clipped = stamp.mic_clipped, .reference_clipped = stamp.reference_clipped,
        /* The stronger physical input, not a claim of selected-source ERLE. */
        .level = {stamp.mic[stamp.mic[1].rms > stamp.mic[0].rms], stamp.reference, clean_level},
        .measurement_us = (uint32_t)(esp_timer_get_time() - measure_start),
        .feed_us = atomic_load(&s_aec.last_feed_us),
        .fetch_wait_us = fetch_wait_us, .agc_us = agc_us,
        .uplink_level = uplink_level,
    };
    return ESP_OK;
}

void starter_aec_get_stats(starter_aec_stats_t *stats)
{
    if (!stats) return;
    *stats = (starter_aec_stats_t) {
        .feed_frames = atomic_load(&s_aec.feed_frames), .fetch_frames = atomic_load(&s_aec.fetch_frames),
        .input_full = atomic_load(&s_aec.input_full), .fetch_timeouts = atomic_load(&s_aec.fetch_timeouts),
        .feed_ring_full = atomic_load(&s_aec.feed_ring_full),
        .stamp_timeouts = atomic_load(&s_aec.stamp_timeouts),
        .max_feed_us = atomic_load(&s_aec.max_feed_us), .channel_id = atomic_load(&s_aec.channel_id),
        .feed_avg_us = atomic_load(&s_aec.feed_avg_us),
        .detail_enabled = atomic_load(&s_aec.detail_enabled),
        .aec_calls = atomic_load(&s_aec.aec_calls), .aec_avg_us = atomic_load(&s_aec.aec_avg_us),
        .aec_max_us = atomic_load(&s_aec.aec_max_us),
        .nlp_calls = atomic_load(&s_aec.nlp_calls), .nlp_avg_us = atomic_load(&s_aec.nlp_avg_us),
        .nlp_max_us = atomic_load(&s_aec.nlp_max_us),
        .se_calls = atomic_load(&s_aec.se_calls), .se_avg_us = atomic_load(&s_aec.se_avg_us),
        .se_max_us = atomic_load(&s_aec.se_max_us),
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
        .aec_run_ticks = atomic_load(&s_afe_cpu.running[0]),
        .aec_preempt_ticks = atomic_load(&s_afe_cpu.preempted[0]),
        .nlp_run_ticks = atomic_load(&s_afe_cpu.running[1]),
        .nlp_preempt_ticks = atomic_load(&s_afe_cpu.preempted[1]),
        .se_run_ticks = atomic_load(&s_afe_cpu.running[2]),
        .se_preempt_ticks = atomic_load(&s_afe_cpu.preempted[2]),
        .dsp_cycles = {atomic_load(&s_afe_perf.stage[0].avg_cycles),
                       atomic_load(&s_afe_perf.stage[1].avg_cycles),
                       atomic_load(&s_afe_perf.stage[2].avg_cycles)},
        .dsp_perf_errors = atomic_load(&s_afe_perf.errors),
#endif
        .failed = atomic_load(&s_aec.failed),
    };
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
    for (unsigned stage = 0; stage < 3U; ++stage)
        for (unsigned mode = 0; mode < AFE_PERF_MODES; ++mode)
            stats->dsp_stall_pm[stage][mode] = atomic_load(&s_afe_perf.stage[stage].stall_pm[mode]);
#endif
}

#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
esp_err_t starter_aec_debug_detail(bool enabled)
{
    if (!s_aec.handle) return ESP_ERR_INVALID_STATE;
    /* Only diagnostic work is bypassed. An in-flight measured call completes
     * and restores its tick stage; PCM, scheduling and allocation are unchanged. */
    atomic_store_explicit(&s_aec.detail_enabled, enabled, memory_order_relaxed);
    return ESP_OK;
}
#endif
