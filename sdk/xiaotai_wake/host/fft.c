// SPDX-License-Identifier: MIT
#include "model/mel.h"
#include <math.h>

int wake_fft_host(void *ctx, float *data, size_t n)
{
    (void)ctx;
    if (!data || !n || n > WAKE_MEL_FFT_SIZE || (n & (n - 1U))) return -1;
    /* Radix-2 DIT: bit-reverse the input so output bins are in natural order. */
    for (size_t i = 1, reversed = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (i >= reversed) continue;
        for (size_t part = 0; part < 2; ++part) {
            const float save = data[2 * i + part];
            data[2 * i + part] = data[2 * reversed + part];
            data[2 * reversed + part] = save;
        }
    }
    for (size_t size = 2; size <= n; size <<= 1) {
        const size_t half = size >> 1;
        const double step = -6.28318530717958647692 / (double)size;
        for (size_t offset = 0; offset < half; ++offset) {
            const float wr = (float)cos(step * (double)offset);
            const float wi = (float)sin(step * (double)offset);
            for (size_t i = offset; i < n; i += size) {
                const size_t j = i + half;
                const float ar = data[2 * i], ai = data[2 * i + 1];
                const float br = data[2 * j], bi = data[2 * j + 1];
                const float cr = br * wr - bi * wi, ci = br * wi + bi * wr;
                data[2 * i] = ar + cr;
                data[2 * i + 1] = ai + ci;
                data[2 * j] = ar - cr;
                data[2 * j + 1] = ai - ci;
            }
        }
    }
    return 0;
}
