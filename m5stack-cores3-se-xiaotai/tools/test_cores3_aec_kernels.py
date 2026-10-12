"""Execute the actual development wrappers with their vendor DWARF ABI."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_media/src/starter_aec_kernels.inc").read_text(encoding="utf-8")
aec = (root / "components/starter_media/src/starter_aec.c").read_text(encoding="utf-8")
allocation = aec[aec.index("#define AFE_ALLOC_RECORDS"):aec.index('#include "starter_aec_kernels.inc"')]
allocation = allocation.replace("#if XIAOTAI_AFE_KERNEL_PROBE\n", "")
cmake = (root / "components/starter_media/CMakeLists.txt").read_text(encoding="utf-8")
assert 'option(XIAOTAI_AFE_KERNEL_PROBE "Enable intrusive ESP-SR inner-kernel timing" OFF)' in cmake
assert "if(XIAOTAI_AFE_KERNEL_PROBE)" in cmake
for symbol in ("test_real_fft_hp", "test_real_ifft_hp", "bss_update_variance_728",
               "bss_update_covariance_728", "bss_compute_middleW_728",
               "bss_compute_normalizing_factor_728", "div_rvf_rvf_rvf_728",
               "sqrtinv_rvf_rvf_728", "mul_cvf_ccvf_cvf_728", "power_cvf_rvf_728",
               "memcpy_int16_728", "memzero_int16_728", "short2float_728",
               "float2short_728", "dl_audio_float2short_728", "mul_cvf_rvf_cvf_728"):
    assert f'"-Wl,--wrap={symbol}"' in cmake

prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define EXT_RAM_BSS_ATTR
#define ESP_LOGI(tag, fmt, ...) ((void)printf(fmt "\n", __VA_ARGS__))
static struct { atomic_uint stage; atomic_uintptr_t owner; } s_afe_cpu;
static uintptr_t current;
static uint32_t cost, real_calls;
static int64_t clock_us;
static void *expected_handle;
static int16_t *expected_pcm;
static int16_t output_pcm[2];
static float *expected[5];
static void *xTaskGetCurrentTaskHandle(void) { return (void *)current; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static void afe_pc_log(void) {}
static int __real_test_real_fft_hp(int16_t *pcm, void *handle) {
    assert(pcm == expected_pcm && handle == expected_handle);
    ++real_calls; ++pcm[0]; clock_us += cost; return -17;
}
static int __real_test_real_ifft_hp(int16_t *pcm, void *handle) {
    assert(pcm == expected_pcm && handle == expected_handle);
    ++real_calls; --pcm[0]; clock_us += cost; return 23;
}
static void five_args(float *a, float *b, float *c, float *d, float *e, int n) {
    assert(a == expected[0] && b == expected[1] && c == expected[2] &&
           d == expected[3] && e == expected[4] && n == 1024);
    ++real_calls; a[0] += 1; clock_us += cost;
}
static void __real_bss_update_variance_728(float *a, float *b, float *c, float *d, float *e, int n) {
    five_args(a, b, c, d, e, n);
}
static void __real_bss_update_covariance_728(float *a, float *b, float *c, float *d, float *e, int n) {
    five_args(a, b, c, d, e, n);
}
static void __real_bss_compute_normalizing_factor_728(float *a, float *b, float *c, float *d, float *e, int n) {
    five_args(a, b, c, d, e, n);
}
static void __real_bss_compute_middleW_728(float *a, float *b, float *c, int n, int k) {
    assert(a == expected[0] && b == expected[1] && c == expected[2] && n == 1024 && k == 2);
    ++real_calls; a[0] += 1; clock_us += cost;
}
static void three_args(float *a, float *b, float *c, int n) {
    assert(a == expected[0] && b == expected[1] && c == expected[2] && n == 1024);
    ++real_calls; a[0] += 1; clock_us += cost;
}
static void two_args(float *a, float *b, int n) {
    assert(a == expected[0] && b == expected[1] && n == 1024);
    ++real_calls; a[0] += 1; clock_us += cost;
}
static void __real_div_rvf_rvf_rvf_728(float *a, float *b, float *c, int n) {
    three_args(a, b, c, n);
}
static void __real_mul_cvf_ccvf_cvf_728(float *a, float *b, float *c, int n) {
    three_args(a, b, c, n);
}
static void __real_sqrtinv_rvf_rvf_728(float *a, float *b, int n) { two_args(a, b, n); }
static void __real_power_cvf_rvf_728(float *a, float *b, int n) { two_args(a, b, n); }
static void __real_memcpy_int16_728(int16_t *a, int16_t *b, int n) {
    assert(a == expected_pcm && b == output_pcm && n == 2);
    memcpy(b, a, (size_t)n * sizeof(*b)); ++real_calls; clock_us += cost;
}
static void __real_memzero_int16_728(int16_t *a, int n) {
    assert(a == output_pcm && n == 2);
    memset(a, 0, (size_t)n * sizeof(*a)); ++real_calls; clock_us += cost;
}
static void __real_short2float_728(int16_t *a, float *b, int n, int q) {
    assert(a == expected_pcm && b == expected[0] && n == 2 && q == -3);
    ++real_calls; clock_us += cost;
}
static void __real_float2short_728(float *a, int16_t *b, int n, int q) {
    assert(a == expected[0] && b == output_pcm && n == 2 && q == 7);
    b[0] = 987; ++real_calls; clock_us += cost;
}
static void __real_dl_audio_float2short_728(float *a, int16_t *b, int n, int q) {
    assert(a == expected[0] && b == output_pcm && n == 2 && q == 11);
    b[1] = -654; ++real_calls; clock_us += cost;
}
static void __real_mul_cvf_rvf_cvf_728(float *a, float *b, float *c, int n) {
    three_args(a, b, c, n);
}
'''
test = r'''
static void run_all(int16_t *pcm, void *handle, float *a, float *b, float *c, float *d, float *e) {
    assert(__wrap_test_real_fft_hp(pcm, handle) == -17);
    assert(__wrap_test_real_ifft_hp(pcm, handle) == 23);
    __wrap_bss_update_variance_728(a, b, c, d, e, 1024);
    __wrap_bss_update_covariance_728(a, b, c, d, e, 1024);
    __wrap_bss_compute_middleW_728(a, b, c, 1024, 2);
    __wrap_bss_compute_normalizing_factor_728(a, b, c, d, e, 1024);
    __wrap_div_rvf_rvf_rvf_728(a, b, c, 1024);
    __wrap_sqrtinv_rvf_rvf_728(a, b, 1024);
    __wrap_mul_cvf_ccvf_cvf_728(a, b, c, 1024);
    __wrap_power_cvf_rvf_728(a, b, 1024);
    __wrap_memcpy_int16_728(pcm, output_pcm, 2);
    assert(output_pcm[0] == 123 && output_pcm[1] == 456);
    __wrap_memzero_int16_728(output_pcm, 2);
    assert(output_pcm[0] == 0 && output_pcm[1] == 0);
    __wrap_short2float_728(pcm, a, 2, -3);
    __wrap_float2short_728(a, output_pcm, 2, 7);
    __wrap_dl_audio_float2short_728(a, output_pcm, 2, 11);
    assert(output_pcm[0] == 987 && output_pcm[1] == -654);
    __wrap_mul_cvf_rvf_cvf_728(a, b, c, 1024);
}
int main(void) {
    int16_t pcm[2] = {123, 456}; int handle;
    float a[1] = {0}, b[1] = {2}, c[1] = {3}, d[1] = {4}, e[1] = {5};
    expected_handle = &handle; expected_pcm = pcm;
    expected[0] = a; expected[1] = b; expected[2] = c; expected[3] = d; expected[4] = e;
    atomic_store(&s_afe_cpu.owner, 2); atomic_store(&s_afe_cpu.stage, 3); current = 2;
    s_afe_alloc.count = 2;
    s_afe_alloc.record[0].address = (uintptr_t)a;
    s_afe_alloc.record[0].count = 1; s_afe_alloc.record[0].size = sizeof(a);
    s_afe_alloc.record[1].address = (uintptr_t)pcm;
    s_afe_alloc.record[1].count = 2; s_afe_alloc.record[1].size = sizeof(pcm[0]);
    uint32_t offset;
    assert(afe_allocation_id(NULL, &offset) == -1 && offset == 0);
    assert(afe_allocation_id(a, &offset) == 0 && offset == 0);
    assert(afe_allocation_id((char *)a + sizeof(a) - 1, &offset) == 0 && offset == sizeof(a) - 1);
    assert(afe_allocation_id(&pcm[1], &offset) == 1 && offset == sizeof(pcm[0]));
    assert(afe_allocation_id((char *)pcm + sizeof(pcm), &offset) == -1 && offset == 0);
    uint64_t total = 0;
    for (unsigned n = 0; n < 64; ++n) {
        cost = n * 100; total += cost;
        run_all(pcm, &handle, a, b, c, d, e);
        for (unsigned i = 0; i < AFE_KERNEL_COUNT; ++i) {
            assert(atomic_load(&s_afe_kernel[i].calls) == n + 1);
            assert(atomic_load(&s_afe_kernel[i].avg_us) == total / (n + 1));
            assert(atomic_load(&s_afe_kernel[i].max_us) == cost);
            assert(atomic_load(&s_afe_kernel[i].signature_ready));
        }
    }
    assert(s_afe_kernel[AFE_KERNEL_FFT].allocation[0] == 1);
    assert(s_afe_kernel[AFE_KERNEL_MIDDLE].n == 1024 && s_afe_kernel[AFE_KERNEL_MIDDLE].k == 2);
    assert(s_afe_kernel[AFE_KERNEL_MIDDLE].allocation[0] == 0);
    assert(s_afe_kernel[AFE_KERNEL_MIDDLE].allocation[3] == -1);
    assert(s_afe_kernel[AFE_KERNEL_NORMALIZE].n == 1024);
    assert(s_afe_kernel[AFE_KERNEL_COPY].allocation[0] == 1);
    assert(s_afe_kernel[AFE_KERNEL_COPY].n == 2);
    assert(s_afe_kernel[AFE_KERNEL_SHORT_TO_FLOAT].k == -3);
    assert(s_afe_kernel[AFE_KERNEL_FLOAT_TO_SHORT].k == 7);
    assert(s_afe_kernel[AFE_KERNEL_AUDIO_FLOAT_TO_SHORT].k == 11);
    s_afe_alloc.record[0].failed = true;
    assert(afe_allocation_id(a, &offset) == -1);
    s_afe_alloc.record[0].failed = false; s_afe_alloc.record[0].count = -1;
    assert(afe_allocation_id(a, &offset) == -1);
    current = 7; run_all(pcm, &handle, a, b, c, d, e); /* Other CPU/task. */
    current = 2; atomic_store(&s_afe_cpu.stage, 1); run_all(pcm, &handle, a, b, c, d, e); /* AEC. */
    atomic_store(&s_afe_cpu.owner, 0); atomic_store(&s_afe_cpu.stage, 3);
    run_all(pcm, &handle, a, b, c, d, e); /* Before feed registration. */
    for (unsigned i = 0; i < AFE_KERNEL_COUNT; ++i) assert(atomic_load(&s_afe_kernel[i].calls) == 64);
    assert(real_calls == 67 * AFE_KERNEL_COUNT && pcm[0] == 123 && pcm[1] == 456);
    assert(a[0] == 67 * 9 && b[0] == 2 && c[0] == 3 && d[0] == 4 && e[0] == 5);
    starter_aec_log_kernels();
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-aec-kernels-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + allocation + source + test, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: sixteen vendor kernel ABIs, unchanged arguments/PCM/returns, owner/stage isolation and timing")
