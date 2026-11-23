// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAKE_MODEL_EMBEDDING 256U

/* The trained input uses lrintf and saturating affine INT8 quantization. */
int wake_model_quantize(const float *features, int8_t *out, size_t count, float scale, int zero);
/* Normalize the embedding, score each trained prototype, then apply the logistic head. */
int wake_model_head(const int8_t embedding[WAKE_MODEL_EMBEDDING], float scale, int zero, float *probability);

#ifdef __cplusplus
}
#endif
