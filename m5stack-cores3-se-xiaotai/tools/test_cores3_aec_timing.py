"""The development-only wrapper measures the actual AEC call without altering PCM."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_aec.c").read_text(encoding="utf-8")
wrapper = re.search(r"void __wrap_aec_linear_process\([^\n]*\)\n\{.*?\n\}", source, re.S)[0]
wrapper += re.search(r"int __wrap_aec_nlp_process\([^\n]*\)\n\{.*?\n\}", source, re.S)[0]
wrapper += re.search(r"void __wrap_esp_bss_hps16fft_process\([^\n]*\)\n\{.*?\n\}", source, re.S)[0]
wrapper += re.search(r"void __wrap_esp_bss_728_process\([^\n]*\)\n\{.*?\n\}", source, re.S)[0]
profile = source.split('/* Tick ISR reads only', 1)[1].split('void __real_aec_linear_process', 1)[0]
profile = '/* Tick ISR reads only' + profile
profile = profile.split('\n#if XIAOTAI_AFE_KERNEL_PROBE', 1)[0]
profile += source[source.index('static int afe_allocation_id'):
                  source.index('#include "starter_aec_kernels.inc"')]
profile = profile.replace('#include "starter_aec_pc.inc"', '')
# The bounded recorder has its own real-source test; this fixture checks its ISR boundary.
profile = profile.replace('#include "starter_aec_preempt.inc"', '')
# Host pointers are 64-bit; the production 32-byte gate is checked by the S3 build.
profile = re.sub(r'_Static_assert\(sizeof\(s_afe_cpu\).*?;', '', profile, flags=re.S)
assert 'esp_register_freertos_tick_hook_for_cpu(afe_cpu_tick, 1)' in source
assert 'esp_deregister_freertos_tick_hook_for_cpu(afe_cpu_tick, 1)' in source
cmake = (root / "components/starter_media/CMakeLists.txt").read_text(encoding="utf-8")
assert 'if(CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE)' in cmake
assert '"-Wl,--wrap=aec_linear_process"' in cmake
assert '"-Wl,--wrap=aec_nlp_process"' in cmake
assert '"-Wl,--wrap=esp_bss_hps16fft_process"' in cmake
assert '"-Wl,--wrap=esp_bss_728_process"' in cmake
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>
#define ESP_LOGI(...) ((void)0)
#define DRAM_ATTR
#define IRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define CONFIG_LIBC_MISC_IN_IRAM 1
#define CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL1 1
#define ESP_OK 0
#define ESP_FAIL -1
#define XTPERF_CNT_CYCLES 0
#define XTPERF_MASK_CYCLES 1
#define XTPERF_CNT_D_STALL 3
#define XTPERF_CNT_I_STALL 4
#define XTPERF_MASK_D_STALL_ALL 0x01fe
#define XTPERF_MASK_D_STALL_BUSY 0x10
#define XTPERF_MASK_D_STALL_IN_PIF 0x20
#define XTPERF_MASK_D_STALL_STORE_BUF_FULL 2
#define XTPERF_MASK_D_STALL_STORE_BUF_CONFLICT 4
#define XTPERF_MASK_D_STALL_MHT_LOOKUP 0x40
#define XTPERF_MASK_D_STALL_UNCACHED_LOAD 0x80
#define XTPERF_MASK_D_STALL_BANK_CONFLICT 0x100
#define XTPERF_MASK_I_STALL_ALL 0x01ff
typedef int esp_err_t;
static bool perf_started, perf_init_fail, perf_overflow_fail;
static uint32_t perf_select, perf_cycles, perf_stalls, perf_mask;
static uint32_t perf_divisor(uint16_t mask) {
    switch (mask) {
        case 0x01fe: return 4;
        case 0x10: return 5;
        case 0x20: return 10;
        case 6: return 20;
        case 0x40: return 25;
        case 0x80: return 50;
        case 0x100: return 100;
        case 0x01ff: return 20;
        default: assert(false); return 1;
    }
}
static esp_err_t xtensa_perfmon_init(int id, uint16_t select, uint16_t mask, int kernel, int trace) {
    assert(id == 0 || id == 1);
    assert(kernel == 0 && trace == 6 && !perf_started);
    if (perf_init_fail) return ESP_FAIL;
    if (id == 0) {
        assert(select == XTPERF_CNT_CYCLES && mask == XTPERF_MASK_CYCLES);
        perf_cycles = 100;
    } else {
        assert((select == XTPERF_CNT_D_STALL && mask != XTPERF_MASK_I_STALL_ALL) ||
               (select == XTPERF_CNT_I_STALL && mask == XTPERF_MASK_I_STALL_ALL));
        perf_select = select;
        perf_mask = mask;
        perf_stalls = perf_cycles / perf_divisor(mask);
    }
    return ESP_OK;
}
static void xtensa_perfmon_start(void) { assert(!perf_started); perf_started = true; }
static void xtensa_perfmon_stop(void) { perf_started = false; }
static uint32_t xtensa_perfmon_value(int id) { assert(!perf_started); return id ? perf_stalls : perf_cycles; }
static esp_err_t xtensa_perfmon_overflow(int id) { assert(id == 0 || id == 1); return perf_overflow_fail ? ESP_FAIL : ESP_OK; }
static uintptr_t current_task;
static void *xTaskGetCurrentTaskHandle(void) { return (void *)current_task; }
static unsigned preempt_samples;
static void afe_preempt_sample(uintptr_t task) {
    assert(task == 3);
    ++preempt_samples;
}
static void sample_stage(unsigned expected_stage);
typedef struct { int frame_size; struct { int mic_num; uint32_t caps; } config; } aec_handle_t;
static struct {
    atomic_bool detail_enabled;
    atomic_uint aec_calls, aec_avg_us, aec_max_us; uint64_t aec_total_us;
    atomic_uint nlp_calls, nlp_avg_us, nlp_max_us; uint64_t nlp_total_us;
    atomic_uint se_calls, se_avg_us, se_max_us; uint64_t se_total_us;
} s_aec;
static int64_t clock_us;
static uint32_t cost;
static const aec_handle_t *expected_handle;
static int16_t *expected_in, *expected_ref, *expected_out;
static unsigned real_calls;
static unsigned nlp_calls, se_calls;
static int64_t esp_timer_get_time(void) { return clock_us; }
static void __real_aec_linear_process(const aec_handle_t *h, int16_t *in, int16_t *ref, int16_t *out) {
    assert(h == expected_handle && in == expected_in && ref == expected_ref && out == expected_out);
    ++real_calls;
    sample_stage(1);
    for (int i = 0; i < 32; ++i) out[i] = (int16_t)(in[i] - ref[i]);
    clock_us += cost;
}
static int __real_aec_nlp_process(const aec_handle_t *h, int16_t *out) {
    assert(h == expected_handle && out == expected_out);
    ++nlp_calls; clock_us += cost;
    sample_stage(2);
    return -7;
}
static void __real_esp_bss_hps16fft_process(void *h, int16_t *in, int16_t *out) {
    assert(h == expected_handle && in == expected_in && out == expected_out);
    ++se_calls; clock_us += cost;
    sample_stage(3);
}
static void __real_esp_bss_728_process(void *h, int16_t *in, int16_t *out) {
    __real_esp_bss_hps16fft_process(h, in, out);
}
'''
test = r'''
static void sample_stage(unsigned expected_stage) {
    if (!atomic_load(&s_aec.detail_enabled)) {
        assert(atomic_load(&s_afe_cpu.stage) == 0 && !perf_started);
        return;
    }
    assert(atomic_load(&s_afe_cpu.stage) == expected_stage);
    current_task = 2; afe_cpu_tick(); afe_cpu_tick();
    current_task = 3; afe_cpu_tick();
    current_task = 2;
    if (perf_started) {
        perf_cycles += cost * 240U;
        perf_stalls = perf_cycles / perf_divisor(perf_mask);
    }
}
int main(void) {
    atomic_store(&s_aec.detail_enabled, true);
    uint32_t allocation_offset = 99;
    assert(afe_allocation_id(NULL, &allocation_offset) == -1 && allocation_offset == 0);
    assert(!s_afe_tick_registered);
    atomic_store(&s_afe_cpu.stage, 1);
    afe_cpu_tick(); /* No owner, no attribution. */
    assert(atomic_load(&s_afe_cpu.preempted[0]) == 0);
    atomic_store(&s_afe_cpu.owner, 2);
    atomic_store(&s_afe_cpu.stage, 0); afe_cpu_tick();
    atomic_store(&s_afe_cpu.stage, 4); afe_cpu_tick();
    atomic_store(&s_afe_cpu.stage, 0);
    current_task = 2;
    unsigned first = afe_cpu_begin(1), nested = afe_cpu_begin(3);
    assert(first == 0 && nested == 1);
    afe_cpu_end(nested); assert(atomic_load(&s_afe_cpu.stage) == 1 && perf_started);
    afe_cpu_end(first); assert(atomic_load(&s_afe_cpu.stage) == 0 && !perf_started);
    aec_handle_t handle = {.frame_size = 32, .config = {.mic_num = 2, .caps = 0x408}};
    int16_t in[32], ref[32], out[32], copy[32];
    for (int i = 0; i < 32; ++i) in[i] = (int16_t)(i * 17 - 250), ref[i] = (int16_t)(i - 15);
    memcpy(copy, in, sizeof(in));
    expected_handle = &handle; expected_in = in; expected_ref = ref; expected_out = out;
    const uint32_t costs[] = {0, 1000, 2500, 6000, 100000, 100, 4500, 1000, 3000, 2000, 5000, 0, 2500, 500, 1000, 6000};
    uint64_t total = 0; uint32_t maximum = 0;
    for (unsigned n = 0; n < sizeof(costs) / sizeof(costs[0]); ++n) {
        cost = costs[n]; total += cost;
        if (cost > maximum) maximum = cost;
        __wrap_aec_linear_process(&handle, in, ref, out);
        assert(real_calls == n + 1 && atomic_load(&s_aec.aec_calls) == n + 1);
        assert(atomic_load(&s_aec.aec_avg_us) == total / (n + 1));
        assert(atomic_load(&s_aec.aec_max_us) == maximum);
        assert(__wrap_aec_nlp_process(&handle, out) == -7);
        if (n & 1U) __wrap_esp_bss_728_process(&handle, in, out);
        else __wrap_esp_bss_hps16fft_process(&handle, in, out);
        assert(atomic_load(&s_afe_cpu.stage) == 0);
        for (unsigned i = 0; i < 3; ++i) {
            assert(atomic_load(&s_afe_cpu.running[i]) == (n + 1) * 2);
            assert(atomic_load(&s_afe_cpu.preempted[i]) == n + 1);
        }
        assert(preempt_samples == (n + 1) * 3);
        assert(nlp_calls == n + 1 && se_calls == n + 1);
        assert(atomic_load(&s_aec.nlp_calls) == n + 1 && atomic_load(&s_aec.se_calls) == n + 1);
        assert(atomic_load(&s_aec.nlp_avg_us) == total / (n + 1));
        assert(atomic_load(&s_aec.se_avg_us) == total / (n + 1));
        assert(atomic_load(&s_aec.nlp_max_us) == maximum);
        assert(atomic_load(&s_aec.se_max_us) == maximum);
        assert(memcmp(copy, in, sizeof(in)) == 0);
        for (int i = 0; i < 32; ++i) assert(out[i] == (int16_t)(in[i] - ref[i]));
    }
    for (unsigned i = 0; i < 3; ++i) {
        assert(atomic_load(&s_afe_perf.stage[i].avg_cycles) > 0);
        const uint32_t expected[] = {250, 200, 100, 50, 40, 20, 10, 50};
        for (unsigned mode = 0; mode < AFE_PERF_MODES; ++mode) {
            assert(s_afe_perf.stage[i].calls[mode] > 0);
            assert(atomic_load(&s_afe_perf.stage[i].stall_pm[mode]) == expected[mode]);
        }
    }
    uint32_t valid = atomic_load(&s_afe_perf.stage[0].avg_cycles);
    perf_overflow_fail = true;
    first = afe_cpu_begin(1); afe_cpu_end(first);
    assert(atomic_load(&s_afe_perf.errors) == 1 && !perf_started);
    assert(atomic_load(&s_afe_perf.stage[0].avg_cycles) == valid);
    perf_overflow_fail = false; perf_init_fail = true;
    first = afe_cpu_begin(1); afe_cpu_end(first);
    assert(atomic_load(&s_afe_perf.errors) == 2 && !perf_started);
    assert(atomic_load(&s_afe_perf.stage[0].avg_cycles) == valid);
    perf_init_fail = false; current_task = 3;
    first = afe_cpu_begin(1); assert(!perf_started); afe_cpu_end(first);
    assert(atomic_load(&s_afe_perf.errors) == 2);
    current_task = 2;
    unsigned measured = atomic_load(&s_aec.aec_calls);
    unsigned real_before = real_calls, nlp_before = nlp_calls, se_before = se_calls;
    atomic_store(&s_aec.detail_enabled, false);
    for (unsigned n = 0; n < 1000; ++n) {
        __wrap_aec_linear_process(&handle, in, ref, out);
        assert(__wrap_aec_nlp_process(&handle, out) == -7);
        __wrap_esp_bss_hps16fft_process(&handle, in, out);
        __wrap_esp_bss_728_process(&handle, in, out);
        assert(atomic_load(&s_afe_cpu.stage) == 0 && !perf_started);
        assert(memcmp(copy, in, sizeof(in)) == 0);
        for (int i = 0; i < 32; ++i) assert(out[i] == (int16_t)(in[i] - ref[i]));
    }
    assert(real_calls == real_before + 1000 && nlp_calls == nlp_before + 1000);
    assert(se_calls == se_before + 2000);
    assert(atomic_load(&s_aec.aec_calls) == measured);
    assert(atomic_load(&s_aec.nlp_calls) == measured && atomic_load(&s_aec.se_calls) == measured);
    atomic_store(&s_aec.detail_enabled, true);
    __wrap_aec_linear_process(&handle, in, ref, out);
    assert(atomic_load(&s_aec.aec_calls) == measured + 1 && atomic_load(&s_afe_cpu.stage) == 0);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-aec-timing-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + profile + wrapper + test, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: unchanged AEC pointers/PCM/return; timing, nested restoration, ISR attribution, rotating PM counters and error/non-owner isolation")
