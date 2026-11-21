// SPDX-License-Identifier: MIT
#include <string.h>
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "power.h"
#include "processors/frontend_pairing.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

typedef struct {
    aec_handle_t *aec;
    xiaotai_critical_t lock;
    xiaotai_processor_stats_t stats;
    int16_t *pcm[3]; /* MIC, REF, CLEAN, separately SIMD-aligned */
    uint16_t samples;
    uint8_t channels, slots[2];
    int64_t time_us;
    uint32_t pending_loss, frame_loss;
    bool ready;
} direct_aec_t;

static void cleanup(void *opaque)
{
    direct_aec_t *state = opaque;
    for (size_t i = 0; i < 3; ++i) xiaotai_mem_free(state->pcm[i]);
    if (state->aec) aec_destroy(state->aec);
    xiaotai_mem_free(state);
}

static int match(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    (void)out;
    return media_frontend_pairing_slots(in, 1, NULL) ? 2 : 0;
}

static xiaotai_err_t open_aec(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                              xiaotai_processor_info_t *info, void **opaque)
{
    (void)out;
    if (!in || !info || !opaque || !media_frontend_pairing_slots(&in->desc, 1, NULL)) return XIAOTAI_ERR_INVALID_ARG;
    direct_aec_t *state = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*state));
    if (!state) return XIAOTAI_ERR_NO_MEM;
    xiaotai_critical_init(&state->lock);
    (void)media_frontend_pairing_slots(&in->desc, 1, state->slots);
    state->channels = in->desc.channels;
    aec_config_t config = {.mic_num = 1, .ref_num = 1, .out_num = 1, .filter_length = 4,
        .sample_rate = 16000, .mode = AEC_MODE_FD_LOW_COST, .nlp_level = AEC_NLP_LEVEL_AGGR,
        .caps = MALLOC_CAP_8BIT | (xiaotai_mem_has_external() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL)};
    state->aec = aec_create_from_config(&config);
    if (!state->aec) { cleanup(state); return XIAOTAI_ERR_NO_MEM; }
    const int chunk = aec_get_chunksize(state->aec);
    if (chunk <= 0 || (chunk & 1) || (unsigned)chunk > XIAOTAI_PROCESSOR_SAMPLES_MAX) {
        cleanup(state);
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    state->samples = (uint16_t)chunk;
    for (size_t i = 0; i < 3; ++i) {
        state->pcm[i] = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 16, (size_t)chunk * sizeof(int16_t));
        if (!state->pcm[i]) { cleanup(state); return XIAOTAI_ERR_NO_MEM; }
    }
    *info = (xiaotai_processor_info_t){.flags = XIAOTAI_PROCESSOR_CANCELS_ECHO, .rate_hz = 16000,
        .feed_frames = (uint16_t)chunk, .fetch_max = (uint16_t)chunk, .feed_stack_bytes = 2048, .fetch_stack_bytes = 512};
    *opaque = state;
    return XIAOTAI_OK;
}

static xiaotai_err_t feed(void *opaque, const void *data, size_t size, int64_t time_us, uint32_t lost)
{
    direct_aec_t *state = opaque;
    if (!data || size != (size_t)state->samples * state->channels * sizeof(int16_t)) return XIAOTAI_ERR_INVALID_SIZE;
    state->pending_loss = media_add_sat_u32(state->pending_loss, lost);
    if (state->ready) {
        state->pending_loss = media_add_sat_u32(state->pending_loss, state->samples);
        xiaotai_critical_enter(&state->lock);
        ++state->stats.input_full;
        xiaotai_critical_exit(&state->lock);
        return XIAOTAI_ERR_NO_MEM;
    }
    const uint8_t *bytes = data;
    uint32_t clipped[2] = {0, 0};
    for (size_t i = 0; i < state->samples; ++i) {
        for (unsigned c = 0; c < 2; ++c) {
            int16_t sample;
            memcpy(&sample, bytes + (i * state->channels + state->slots[c]) * sizeof(sample), sizeof(sample));
            state->pcm[c][i] = sample;
            clipped[c] += sample >= 32700 || sample <= -32700;
        }
    }
    aec_process(state->aec, state->pcm[0], state->pcm[1], state->pcm[2]);
    state->frame_loss = state->pending_loss;
    state->pending_loss = 0;
    state->time_us = time_us;
    state->ready = true;
    xiaotai_critical_enter(&state->lock);
    ++state->stats.feeds;
    state->stats.clipped_mic += clipped[0];
    state->stats.clipped_reference += clipped[1];
    xiaotai_critical_exit(&state->lock);
    return XIAOTAI_OK;
}

static xiaotai_err_t fetch(void *opaque, xiaotai_processor_frame_t *frame, uint32_t timeout_ms)
{
    (void)timeout_ms;
    direct_aec_t *state = opaque;
    if (!frame) return XIAOTAI_ERR_INVALID_ARG;
    if (!state->ready) return XIAOTAI_ERR_TIMEOUT;
    state->ready = false;
    *frame = (xiaotai_processor_frame_t){.encoding = XIAOTAI_CODEC_PCM16, .data = state->pcm[2],
        .size = state->samples * sizeof(int16_t), .samples = state->samples, .captured_us = state->time_us,
        .lost_samples = state->frame_loss};
    xiaotai_critical_enter(&state->lock);
    ++state->stats.frames;
    state->stats.lost_samples = media_add_sat_u32(state->stats.lost_samples, state->frame_loss);
    xiaotai_critical_exit(&state->lock);
    return XIAOTAI_OK;
}

static void stats(void *opaque, xiaotai_processor_stats_t *out)
{
    direct_aec_t *state = opaque;
    xiaotai_critical_enter(&state->lock);
    *out = state->stats;
    xiaotai_critical_exit(&state->lock);
}

const xiaotai_audio_processor_t media_aec_single = {.name = "aec.single", .match = match, .open = open_aec,
    .feed = feed, .fetch = fetch, .stats = stats, .close = cleanup};
