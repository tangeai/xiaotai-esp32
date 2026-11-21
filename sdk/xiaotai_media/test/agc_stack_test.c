// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "esp_agc.h"
#include "xiaotai/media.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/port/mem.h"

extern const xiaotai_audio_effect_t media_agc, media_boost;

enum { RATE = 16000, FRAME = 160, UNTUNED_STACK = 49152 };
static const xiaotai_processor_info_t processor = {.rate_hz = RATE};
static int32_t capture_stack, boost_stack;
static bool capture_override, boost_override;
static unsigned allocations, opens, closes, frames;
static agc_mode_t current_mode;

int32_t __wrap_xiaotai_media_param(const char *name, int32_t fallback)
{
    if (!strcmp(name, "capture.agc_db")) return 15;
    if (!strcmp(name, "uplink.boost_cdb")) return 600;
    if (!strcmp(name, "capture.agc_stack_bytes")) return capture_override ? capture_stack : fallback;
    if (!strcmp(name, "uplink.boost_stack_bytes")) return boost_override ? boost_stack : fallback;
    assert(false);
    return fallback;
}

void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    assert(kind == XIAOTAI_MEM_BULK);
    ++allocations;
    return __real_xiaotai_mem_calloc(kind, count, size);
}

void *esp_agc_open(agc_mode_t mode, int rate)
{
    assert(opens == closes && rate == RATE);
    current_mode = mode;
    ++opens;
    return &current_mode;
}

void set_agc_config(void *handle, int gain_db, int limiter, int target_level_dbfs)
{
    assert(handle == &current_mode && limiter == 1);
    if (current_mode == AGC_MODE_2) assert(gain_db == 15 && target_level_dbfs == 6);
    else assert(current_mode == AGC_MODE_3 && gain_db == 6 && target_level_dbfs == 3);
}

int esp_agc_process(void *handle, short *in, short *out, int samples, int rate)
{
    assert(handle == &current_mode && in != out && samples == FRAME && rate == RATE);
    memcpy(out, in, (size_t)samples * sizeof(*in));
    ++frames;
    return ESP_AGC_SUCCESS;
}

void esp_agc_close(void *handle)
{
    assert(handle == &current_mode && opens == closes + 1);
    ++closes;
}

static void check(const xiaotai_audio_effect_t *effect, agc_mode_t mode, uint32_t stack)
{
    xiaotai_effect_info_t info;
    void *state = NULL;
    assert(effect->open(&processor, &info, &state) == XIAOTAI_OK);
    assert(state && current_mode == mode && info.stack_bytes == stack);
    const unsigned delay = mode == AGC_MODE_2 ? FRAME : 0;
    assert(info.delay_samples == delay);
    int16_t pcm[2 * FRAME];
    for (unsigned i = 0; i < 2 * FRAME; ++i) pcm[i] = (int16_t)(i + 1);
    const unsigned before = frames;
    assert(effect->process(state, pcm, 2 * FRAME) == XIAOTAI_OK);
    assert(frames == before + 2);
    for (unsigned i = 0; i < 2 * FRAME; ++i)
        assert(pcm[i] == (i < delay ? 0 : (int16_t)(i - delay + 1)));
    effect->close(state);
}

static void invalid(const xiaotai_audio_effect_t *effect)
{
    const unsigned before_alloc = allocations, before_open = opens;
    xiaotai_effect_info_t info = {0};
    void *state = NULL;
    assert(effect->open(&processor, &info, &state) == XIAOTAI_ERR_INVALID_ARG);
    assert(!state && info.stack_bytes == 0 && allocations == before_alloc && opens == before_open);
}

int main(void)
{
    check(&media_agc, AGC_MODE_2, UNTUNED_STACK);
    check(&media_boost, AGC_MODE_3, UNTUNED_STACK);
    capture_override = true;
    capture_stack = 6144;
    check(&media_agc, AGC_MODE_2, 6144);
    check(&media_boost, AGC_MODE_3, UNTUNED_STACK);
    boost_override = true;
    boost_stack = 8192;
    check(&media_agc, AGC_MODE_2, 6144);
    check(&media_boost, AGC_MODE_3, 8192);
    const int32_t bad[] = {0, -1, 6128, 6145, 49168};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        capture_stack = boost_stack = bad[i];
        invalid(&media_agc);
        invalid(&media_boost);
    }
    assert(opens == closes && allocations == opens);
    return 0;
}
