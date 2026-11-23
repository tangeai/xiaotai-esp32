// SPDX-License-Identifier: MIT
#include "postprocess.h"
#include "head.h"
#include <math.h>

_Static_assert(KWS_HEAD_D == WAKE_MODEL_EMBEDDING, "embedding contract");

static int quantization_valid(float scale, int zero)
{
    return isfinite(scale) && scale > 0.0f && zero >= INT8_MIN && zero <= INT8_MAX;
}

int wake_model_quantize(const float *features, int8_t *out, size_t count, float scale, int zero)
{
    if (!features || !out || !quantization_valid(scale, zero)) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!isfinite(features[i])) return -1;
        const float affine = features[i] / scale;
        if (affine <= (float)(INT8_MIN - zero)) out[i] = INT8_MIN;
        else if (affine >= (float)(INT8_MAX - zero)) out[i] = INT8_MAX;
        else out[i] = (int8_t)((int)lrintf(affine) + zero);
    }
    return 0;
}

int wake_model_head(const int8_t embedding[WAKE_MODEL_EMBEDDING], float scale, int zero, float *probability)
{
    if (!embedding || !probability || !quantization_valid(scale, zero)) return -1;
    float feature[WAKE_MODEL_EMBEDDING];
    float norm = 0.0f;
    for (size_t i = 0; i < WAKE_MODEL_EMBEDDING; ++i) {
        feature[i] = ((float)embedding[i] - (float)zero) * scale;
        norm += feature[i] * feature[i];
    }
    if (!isfinite(norm)) return -1;
    norm = sqrtf(norm) + 1e-12f;
    for (size_t i = 0; i < WAKE_MODEL_EMBEDDING; ++i) feature[i] /= norm;
    float logit = KWS_FC_B;
    for (size_t k = 0; k < KWS_HEAD_K; ++k) {
        float dot = 0.0f;
        for (size_t i = 0; i < WAKE_MODEL_EMBEDDING; ++i) dot += feature[i] * KWS_PROTO_NORM[k][i];
        dot /= KWS_ABS_TEMP;
        logit += KWS_FC_W[k] * dot;
    }
    if (!isfinite(logit)) return -1;
    *probability = 1.0f / (1.0f + expf(-logit));
    return 0;
}
