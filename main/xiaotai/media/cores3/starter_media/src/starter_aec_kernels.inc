/* ESP-SR 2.4.7/2.5.0 DWARF prototypes; observe the BSS owner's real calls only. */
enum { AFE_KERNEL_FFT, AFE_KERNEL_IFFT, AFE_KERNEL_VARIANCE,
       AFE_KERNEL_COVARIANCE, AFE_KERNEL_MIDDLE, AFE_KERNEL_NORMALIZE,
       AFE_KERNEL_DIVIDE, AFE_KERNEL_SQRT_INVERSE,
       AFE_KERNEL_MULTIPLY, AFE_KERNEL_POWER, AFE_KERNEL_COPY,
       AFE_KERNEL_ZERO, AFE_KERNEL_SHORT_TO_FLOAT, AFE_KERNEL_FLOAT_TO_SHORT,
       AFE_KERNEL_AUDIO_FLOAT_TO_SHORT, AFE_KERNEL_COMPLEX_REAL, AFE_KERNEL_COUNT };

static EXT_RAM_BSS_ATTR struct {
    uint64_t total_us;
    atomic_uint calls, avg_us, max_us;
    atomic_bool signature_ready;
    int n, k, allocation[5];
    uint32_t offset[5];
} s_afe_kernel[AFE_KERNEL_COUNT];

static void afe_kernel_signature(unsigned kernel, int64_t started, int n, int k,
                                 const void *a, const void *b, const void *c,
                                 const void *d, const void *e)
{
    if (started < 0 || atomic_load(&s_afe_kernel[kernel].signature_ready)) return;
    const void *arguments[5] = {a, b, c, d, e};
    s_afe_kernel[kernel].n = n;
    s_afe_kernel[kernel].k = k;
    for (unsigned i = 0; i < 5; ++i)
        s_afe_kernel[kernel].allocation[i] = afe_allocation_id(arguments[i],
                                                              &s_afe_kernel[kernel].offset[i]);
    atomic_store(&s_afe_kernel[kernel].signature_ready, true);
}

static int64_t afe_kernel_begin(void)
{
    if (atomic_load(&s_afe_cpu.stage) != 3U ||
        (uintptr_t)xTaskGetCurrentTaskHandle() != atomic_load(&s_afe_cpu.owner)) return -1;
    return esp_timer_get_time();
}

static void afe_kernel_end(unsigned kernel, int64_t started)
{
    if (started < 0) return;
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    s_afe_kernel[kernel].total_us += elapsed;
    uint32_t count = atomic_fetch_add(&s_afe_kernel[kernel].calls, 1U) + 1U;
    atomic_store(&s_afe_kernel[kernel].avg_us,
                 (uint32_t)(s_afe_kernel[kernel].total_us / count));
    if (elapsed > atomic_load(&s_afe_kernel[kernel].max_us))
        atomic_store(&s_afe_kernel[kernel].max_us, elapsed);
}

int __real_test_real_fft_hp(int16_t *pcm, void *handle);
int __wrap_test_real_fft_hp(int16_t *pcm, void *handle)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_FFT, started, 0, 0, pcm, handle, NULL, NULL, NULL);
    int result = __real_test_real_fft_hp(pcm, handle);
    afe_kernel_end(AFE_KERNEL_FFT, started);
    return result;
}

int __real_test_real_ifft_hp(int16_t *pcm, void *handle);
int __wrap_test_real_ifft_hp(int16_t *pcm, void *handle)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_IFFT, started, 0, 0, pcm, handle, NULL, NULL, NULL);
    int result = __real_test_real_ifft_hp(pcm, handle);
    afe_kernel_end(AFE_KERNEL_IFFT, started);
    return result;
}

void __real_bss_update_variance_728(float *a, float *b, float *c, float *d, float *e, int n);
void __wrap_bss_update_variance_728(float *a, float *b, float *c, float *d, float *e, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_VARIANCE, started, n, 0, a, b, c, d, e);
    __real_bss_update_variance_728(a, b, c, d, e, n);
    afe_kernel_end(AFE_KERNEL_VARIANCE, started);
}

void __real_bss_update_covariance_728(float *a, float *b, float *c, float *d, float *e, int n);
void __wrap_bss_update_covariance_728(float *a, float *b, float *c, float *d, float *e, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_COVARIANCE, started, n, 0, a, b, c, d, e);
    __real_bss_update_covariance_728(a, b, c, d, e, n);
    afe_kernel_end(AFE_KERNEL_COVARIANCE, started);
}

void __real_bss_compute_middleW_728(float *a, float *b, float *c, int n, int k);
void __wrap_bss_compute_middleW_728(float *a, float *b, float *c, int n, int k)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_MIDDLE, started, n, k, a, b, c, NULL, NULL);
    __real_bss_compute_middleW_728(a, b, c, n, k);
    afe_kernel_end(AFE_KERNEL_MIDDLE, started);
}

void __real_bss_compute_normalizing_factor_728(float *a, float *b, float *c, float *d, float *e, int n);
void __wrap_bss_compute_normalizing_factor_728(float *a, float *b, float *c, float *d, float *e, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_NORMALIZE, started, n, 0, a, b, c, d, e);
    __real_bss_compute_normalizing_factor_728(a, b, c, d, e, n);
    afe_kernel_end(AFE_KERNEL_NORMALIZE, started);
}

void __real_div_rvf_rvf_rvf_728(float *a, float *b, float *c, int n);
void __wrap_div_rvf_rvf_rvf_728(float *a, float *b, float *c, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_DIVIDE, started, n, 0, a, b, c, NULL, NULL);
    __real_div_rvf_rvf_rvf_728(a, b, c, n);
    afe_kernel_end(AFE_KERNEL_DIVIDE, started);
}

void __real_sqrtinv_rvf_rvf_728(float *a, float *b, int n);
void __wrap_sqrtinv_rvf_rvf_728(float *a, float *b, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_SQRT_INVERSE, started, n, 0, a, b, NULL, NULL, NULL);
    __real_sqrtinv_rvf_rvf_728(a, b, n);
    afe_kernel_end(AFE_KERNEL_SQRT_INVERSE, started);
}

void __real_mul_cvf_ccvf_cvf_728(float *a, float *b, float *c, int n);
void __wrap_mul_cvf_ccvf_cvf_728(float *a, float *b, float *c, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_MULTIPLY, started, n, 0, a, b, c, NULL, NULL);
    __real_mul_cvf_ccvf_cvf_728(a, b, c, n);
    afe_kernel_end(AFE_KERNEL_MULTIPLY, started);
}

void __real_power_cvf_rvf_728(float *a, float *b, int n);
void __wrap_power_cvf_rvf_728(float *a, float *b, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_POWER, started, n, 0, a, b, NULL, NULL, NULL);
    __real_power_cvf_rvf_728(a, b, n);
    afe_kernel_end(AFE_KERNEL_POWER, started);
}

void __real_memcpy_int16_728(int16_t *a, int16_t *b, int n);
void __wrap_memcpy_int16_728(int16_t *a, int16_t *b, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_COPY, started, n, 0, a, b, NULL, NULL, NULL);
    __real_memcpy_int16_728(a, b, n);
    afe_kernel_end(AFE_KERNEL_COPY, started);
}

void __real_memzero_int16_728(int16_t *a, int n);
void __wrap_memzero_int16_728(int16_t *a, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_ZERO, started, n, 0, a, NULL, NULL, NULL, NULL);
    __real_memzero_int16_728(a, n);
    afe_kernel_end(AFE_KERNEL_ZERO, started);
}

void __real_short2float_728(int16_t *a, float *b, int n, int q);
void __wrap_short2float_728(int16_t *a, float *b, int n, int q)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_SHORT_TO_FLOAT, started, n, q, a, b, NULL, NULL, NULL);
    __real_short2float_728(a, b, n, q);
    afe_kernel_end(AFE_KERNEL_SHORT_TO_FLOAT, started);
}

void __real_float2short_728(float *a, int16_t *b, int n, int q);
void __wrap_float2short_728(float *a, int16_t *b, int n, int q)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_FLOAT_TO_SHORT, started, n, q, a, b, NULL, NULL, NULL);
    __real_float2short_728(a, b, n, q);
    afe_kernel_end(AFE_KERNEL_FLOAT_TO_SHORT, started);
}

void __real_dl_audio_float2short_728(float *a, int16_t *b, int n, int q);
void __wrap_dl_audio_float2short_728(float *a, int16_t *b, int n, int q)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_AUDIO_FLOAT_TO_SHORT, started, n, q, a, b, NULL, NULL, NULL);
    __real_dl_audio_float2short_728(a, b, n, q);
    afe_kernel_end(AFE_KERNEL_AUDIO_FLOAT_TO_SHORT, started);
}

void __real_mul_cvf_rvf_cvf_728(float *a, float *b, float *c, int n);
void __wrap_mul_cvf_rvf_cvf_728(float *a, float *b, float *c, int n)
{
    int64_t started = afe_kernel_begin();
    afe_kernel_signature(AFE_KERNEL_COMPLEX_REAL, started, n, 0, a, b, c, NULL, NULL);
    __real_mul_cvf_rvf_cvf_728(a, b, c, n);
    afe_kernel_end(AFE_KERNEL_COMPLEX_REAL, started);
}

void starter_aec_log_kernels(void)
{
    afe_pc_log();
    static const char *const names[AFE_KERNEL_COUNT] = {
        "fft", "ifft", "variance", "covariance", "middle", "normalize",
        "divide", "sqrt-inverse", "multiply", "power", "copy", "zero",
        "short-to-float", "float-to-short", "audio-float-to-short", "complex-real"
    };
    for (unsigned n = 0; n < s_afe_alloc.count; ++n)
        ESP_LOGI(TAG, "BSS placement: alloc=%u set-phase=%u line-offset=%u",
                 n, (unsigned)(s_afe_alloc.record[n].address & 0x3fffU),
                 (unsigned)(s_afe_alloc.record[n].address & 63U));
    for (unsigned n = 0; n < AFE_KERNEL_COUNT; ++n) {
        ESP_LOGI(TAG, "BSS kernel: name=%s calls=%u avg/max=%u/%u us",
                 names[n], atomic_load(&s_afe_kernel[n].calls),
                 atomic_load(&s_afe_kernel[n].avg_us), atomic_load(&s_afe_kernel[n].max_us));
        if (atomic_load(&s_afe_kernel[n].signature_ready))
            ESP_LOGI(TAG, "BSS operands: name=%s i0=%d i1=%d alloc+offset=%d+%u/%d+%u/%d+%u/%d+%u/%d+%u",
                     names[n], s_afe_kernel[n].n, s_afe_kernel[n].k,
                     s_afe_kernel[n].allocation[0], (unsigned)s_afe_kernel[n].offset[0],
                     s_afe_kernel[n].allocation[1], (unsigned)s_afe_kernel[n].offset[1],
                     s_afe_kernel[n].allocation[2], (unsigned)s_afe_kernel[n].offset[2],
                     s_afe_kernel[n].allocation[3], (unsigned)s_afe_kernel[n].offset[3],
                     s_afe_kernel[n].allocation[4], (unsigned)s_afe_kernel[n].offset[4]);
    }
}
