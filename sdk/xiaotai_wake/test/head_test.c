// SPDX-License-Identifier: MIT
#include "model/head.h"
#include "model/postprocess.h"
#include <assert.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <string.h>

static double reference(const int8_t *embedding, double scale, int zero)
{
    double den = 0.0, total = 0.0;
    for (size_t j = 0; j < KWS_HEAD_D; ++j) {
        const double x = ((int)embedding[j] - zero) * scale;
        den += x * x;
        double weights = 0.0;
        for (size_t p = 0; p < KWS_HEAD_K; ++p) weights += (double)KWS_PROTO_NORM[p][j] * KWS_FC_W[p];
        total += x * weights;
    }
    const double logit = KWS_FC_B + total / ((sqrt(den) + 1e-12) * KWS_ABS_TEMP);
    return 1.0 / (1.0 + exp(-logit));
}

int main(void)
{
    assert(fesetround(FE_TONEAREST) == 0);
    const float raw[] = {-1000.0f, -1.0f, -0.75f, -0.25f, 0.0f, 0.25f, 0.75f, 1.25f, 1000.0f};
    const int8_t expected[] = {-128, -9, -9, -7, -7, -7, -5, -5, 127};
    int8_t quantized[sizeof(expected)];
    assert(wake_model_quantize(raw, quantized, sizeof(expected), 0.5f, -7) == 0);
    assert(!memcmp(quantized, expected, sizeof(expected)));
    assert(wake_model_quantize(raw, quantized, sizeof(expected), 0.0f, -7) != 0);
    assert(wake_model_quantize(raw, quantized, sizeof(expected), INFINITY, -7) != 0);
    assert(wake_model_quantize(raw, quantized, sizeof(expected), 1.0f, 128) != 0);
    const float malformed[] = {NAN};
    assert(wake_model_quantize(malformed, quantized, 1, 1.0f, 0) != 0);
    const float extremes[] = {-FLT_MAX, FLT_MAX};
    assert(wake_model_quantize(extremes, quantized, 2, FLT_MIN, 0) == 0 &&
           quantized[0] == INT8_MIN && quantized[1] == INT8_MAX);

    int8_t embedding[WAKE_MODEL_EMBEDDING];
    memset(embedding, 12, sizeof(embedding));
    float p = -1.0f;
    assert(wake_model_head(embedding, 0.02f, 12, &p) == 0);
    assert(fabs((double)p - 1.0 / (1.0 + exp(-KWS_FC_B))) < 1e-7);
    for (int pattern = 0; pattern < 8; ++pattern) {
        for (size_t i = 0; i < sizeof(embedding); ++i)
            embedding[i] = (int8_t)(((int)i * (17 + 2 * pattern) + pattern * 23) % 256 - 128);
        const int zero = pattern * 5 - 13;
        const float scale = 0.002f * (float)(pattern + 1);
        assert(wake_model_head(embedding, scale, zero, &p) == 0);
        assert(isfinite(p) && p >= 0.0f && p <= 1.0f && fabs(p - reference(embedding, scale, zero)) < 3e-6);
    }
    const float last = p;
    assert(wake_model_head(NULL, 1.0f, 0, &p) != 0 && p == last);
    assert(wake_model_head(embedding, NAN, 0, &p) != 0 && p == last);
    assert(wake_model_head(embedding, FLT_MAX, 0, &p) != 0 && p == last);
}
