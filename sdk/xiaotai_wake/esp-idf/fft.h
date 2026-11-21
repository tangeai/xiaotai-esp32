// SPDX-License-Identifier: MIT
#pragma once

#include "model/mel.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

xiaotai_err_t wake_platform_fft_prepare(void);
int wake_platform_fft(void *ctx, float *complex_pairs, size_t n);

#ifdef __cplusplus
}
#endif
