// SPDX-License-Identifier: MIT
#include "fft.h"

#include <assert.h>
#include "dsps_fft2r.h"
#include "xiaotai/wake_config.h"

int dsps_fft_w_table_size;
uint8_t dsps_fft2r_initialized;

static unsigned s_init, s_deinit, s_fast, s_c, s_reversed;
static bool s_bad_init, s_bad_fft, s_bad_reverse;

esp_err_t dsps_fft2r_init_fc32(float *table, int size)
{
    assert(!table && size == WAKE_MEL_FFT_SIZE && !dsps_fft2r_initialized);
    ++s_init;
    dsps_fft_w_table_size = size;
    if (s_bad_init) return -1;
    dsps_fft2r_initialized = 1;
    return ESP_OK;
}

void dsps_fft2r_deinit_fc32(void)
{
    ++s_deinit;
    dsps_fft2r_initialized = 0;
    dsps_fft_w_table_size = 0;
}

static void aligned(float *pairs, int n)
{
    assert(((uintptr_t)pairs % WAKE_MEL_FFT_ALIGN) == 0 && n == WAKE_MEL_FFT_SIZE);
}

esp_err_t dsps_fft2r_fc32(float *pairs, int n)
{
    aligned(pairs, n);
    ++s_fast;
    return s_bad_fft ? -1 : ESP_OK;
}

esp_err_t dsps_fft2r_fc32_ansi(float *pairs, int n)
{
    aligned(pairs, n);
    ++s_c;
    return s_bad_fft ? -1 : ESP_OK;
}

esp_err_t dsps_bit_rev2r_fc32(float *pairs, int n)
{
    aligned(pairs, n);
    ++s_reversed;
    return s_bad_reverse ? -1 : ESP_OK;
}

int main(void)
{
    _Alignas(WAKE_MEL_FFT_ALIGN) float pairs[WAKE_MEL_FFT_FLOATS] = {0};
    assert(wake_platform_fft(NULL, pairs, WAKE_MEL_FFT_SIZE) == -1);
    dsps_fft2r_initialized = 1;
    dsps_fft_w_table_size = 256;
    assert(wake_platform_fft_prepare() == XIAOTAI_ERR_NOT_SUPPORTED && s_init == 0 && s_deinit == 0);
    assert(wake_platform_fft(NULL, pairs, WAKE_MEL_FFT_SIZE) == -1 && s_reversed == 0);
    dsps_fft_w_table_size = 1024;
    assert(wake_platform_fft_prepare() == XIAOTAI_OK && s_init == 0 && s_deinit == 0);
    dsps_fft2r_initialized = 0;

    s_bad_init = true;
    assert(wake_platform_fft_prepare() == XIAOTAI_FAIL && s_init == 1 && s_deinit == 1);
    s_bad_init = false;
    assert(wake_platform_fft_prepare() == XIAOTAI_OK && s_init == 2);
    assert(wake_platform_fft_prepare() == XIAOTAI_OK && s_init == 2);
    assert(wake_platform_fft(NULL, pairs, 510) == -1);
    assert(wake_platform_fft(NULL, pairs + 1, WAKE_MEL_FFT_SIZE) == -1);
    assert(wake_platform_fft(NULL, NULL, WAKE_MEL_FFT_SIZE) == -1);
    assert(s_fast == 0 && s_c == 0 && s_reversed == 0);

    assert(wake_platform_fft(NULL, pairs, WAKE_MEL_FFT_SIZE) == 0);
    s_bad_fft = true;
    assert(wake_platform_fft(NULL, pairs, WAKE_MEL_FFT_SIZE) == -1 && s_reversed == 1);
    s_bad_fft = false;
    s_bad_reverse = true;
    assert(wake_platform_fft(NULL, pairs, WAKE_MEL_FFT_SIZE) == -1 && s_reversed == 2);
#if XIAOTAI_WAKE_FFT_PORTABLE
    assert(s_c == 3 && s_fast == 0);
#else
    assert(s_c == 0 && s_fast == 3);
#endif
    return 0;
}
