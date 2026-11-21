// SPDX-License-Identifier: MIT
/* A wake model that answers from a script: open() results, then per detect()
 * a result, a probability, a duration on the virtual clock, phase timings
 * and an optional action taken while "scoring". Every call is recorded with
 * what the engine passed. */
#include <string.h>
#include "fakes.h"
#include "xiaotai/port/mem.h"

scripted_model_t g_script;

static const char *const k_phrases[] = {"hello device", "device device", NULL};

static xiaotai_err_t scripted_open(void **state)
{
    size_t index = g_script.opens++;
    if (index < SCRIPT_MAX) g_script.open_at_us[index] = g_fake_now_us;
    xiaotai_err_t result = index < g_script.open_result_count ? g_script.open_results[index] : XIAOTAI_OK;
    if (result != XIAOTAI_OK) return result;
    /* Stands in for the model copy and the tensor arena. */
    void *memory = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 16, 64);
    if (!memory) return XIAOTAI_ERR_NO_MEM;
    *state = memory;
    g_script.is_open = true;
    ++g_script.open_ok;
    return XIAOTAI_OK;
}

static xiaotai_err_t scripted_detect(void *state, const int16_t *pcm, uint32_t stream, uint64_t end_sample,
                                     float *probability, xiaotai_wake_timing_t *timing)
{
    (void)state;
    uint32_t window = g_script.model.window_samples;
    size_t index = g_script.call_count;
    if (index < CALLS_MAX) {
        detect_call_t *call = &g_script.calls[index];
        call->stream = stream;
        call->end_sample = end_sample;
        call->first = pcm[0];
        call->last = pcm[window - 1U];
        call->all_zero = true;
        for (uint32_t i = 0; i < window; ++i) {
            if (pcm[i] != 0) {
                call->all_zero = false;
                break;
            }
        }
        call->at_us = g_fake_now_us;
    }
    ++g_script.call_count;
    const script_step_t *step = index < g_script.step_count ? &g_script.steps[index] : &g_script.default_step;
    g_fake_now_us += step->duration_us;
    if (step->during) step->during();
    *probability = step->probability;
    if (timing) *timing = step->timing;
    return step->result;
}

static void scripted_close(void *state)
{
    xiaotai_mem_free(state);
    g_script.is_open = false;
    ++g_script.closes;
}

void scripted_model_reset(void)
{
    memset(&g_script, 0, sizeof(g_script));
    g_script.model = (xiaotai_wake_model_t){
        .name = "scripted",
        .phrases = k_phrases,
        .rate_hz = 16000,
        .window_samples = 16192,
        .grid_samples = 160,
        .stride_samples = 2560,
        .stack_bytes = 12288,
        .open = scripted_open,
        .detect = scripted_detect,
        .close = scripted_close,
    };
}
