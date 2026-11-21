// SPDX-License-Identifier: MIT
#include "fft.h"

#include "dsps_fft2r.h"
#include "xiaotai/wake_config.h"

xiaotai_err_t wake_platform_fft_prepare(void)
{
    /* ESP-DSP owns one global table, not one per interpreter. Another
     * DSP caller may already use it; never replace a live table. */
    if (!dsps_fft2r_initialized) {
        if (dsps_fft2r_init_fc32(NULL, WAKE_MEL_FFT_SIZE) != ESP_OK) {
            dsps_fft2r_deinit_fc32();
            return XIAOTAI_FAIL;
        }
    }
    return dsps_fft_w_table_size >= (int)WAKE_MEL_FFT_SIZE ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
}

int wake_platform_fft(void *ctx, float *pairs, size_t n)
{
    (void)ctx;
    if (!pairs || ((uintptr_t)pairs % WAKE_MEL_FFT_ALIGN) != 0 ||
        n != WAKE_MEL_FFT_SIZE || !dsps_fft2r_initialized || dsps_fft_w_table_size < (int)n) return -1;
#if XIAOTAI_WAKE_FFT_PORTABLE
    const esp_err_t err = dsps_fft2r_fc32_ansi(pairs, (int)n);
#else
    const esp_err_t err = dsps_fft2r_fc32(pairs, (int)n);
#endif
    return err == ESP_OK && dsps_bit_rev2r_fc32(pairs, (int)n) == ESP_OK ? 0 : -1;
}
