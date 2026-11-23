// SPDX-License-Identifier: MIT
/* Keep the product's real wake pipeline and its model geometry. Only the
 * target inference backend is replaced with a deterministic silent model. */
#include "xiaotai/port/mem.h"
#include "xiaotai/wake.h"

static xiaotai_err_t open_model(void **state)
{
    *state = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 16, 64);
    return *state ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

static xiaotai_err_t score(void *state, const int16_t *pcm, uint32_t stream, uint64_t end_sample,
                            float *probability, xiaotai_wake_timing_t *timing)
{
    (void)stream;
    (void)end_sample;
    if (!state || !pcm || !probability) return XIAOTAI_ERR_INVALID_ARG;
    *probability = 0.0f;
    if (timing) *timing = (xiaotai_wake_timing_t){0};
    return XIAOTAI_OK;
}

xiaotai_err_t wake_builtin_model_register(void)
{
    static const char *const phrases[] = {
        "\xe4\xbd\xa0\xe5\xa5\xbd\xe5\xb0\x8f\xe9\x92\x9b",
        "\xe5\xb0\x8f\xe9\x92\x9b\xe5\xb0\x8f\xe9\x92\x9b",
        "\xe5\xb0\x8f\xe9\x92\x9b\xe5\x90\x8c\xe5\xad\xa6", NULL,
    };
    static const xiaotai_wake_model_t model = {
        .name = "nihaoxiaotai", .phrases = phrases,
        .rate_hz = 16000, .window_samples = 16192, .grid_samples = 160, .stride_samples = 2560,
        .stack_bytes = 12288, .open = open_model, .detect = score, .close = xiaotai_mem_free,
    };
    return xiaotai_wake_model_register(&model);
}
