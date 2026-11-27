// SPDX-License-Identifier: MIT
#include <string.h>
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "power.h"
#include "processors/frontend_pairing.h"
#include "xiaotai/media.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define TAG "aec"

/* A canceller can come up unable to remove any echo. Measured on the
   ESP32-S31 (ESP-SR 2.6.1, 2026-11-26): the one created while the device
   started passed the microphone through during AI talk (0.1 dB over 440
   blocks with the speaker playing), while one created later removed 31 dB
   and still did after 90 s of silence; creating and dropping a first one at
   start did not help. So the canceller has to prove itself: once it has
   heard HEALTH_HEARD blocks with sound both from the speaker and at the
   microphone, at least HEALTH_PROVEN of them must have lost 10 dB or more,
   or it is replaced by a new one (at most HEALTH_RESTARTS times). A working
   one converges in about 0.5 s. One that has proved itself is not checked
   again, so double talk later on cannot trigger a restart. */
enum {
    HEALTH_LEVEL = 100 * 100, /* mean square of a block that counts as sound: RMS 100, -50 dBFS */
    HEALTH_HEARD = 32,        /* about 1 s of far-end sound in 32 ms blocks */
    HEALTH_PROVEN = 8,
    HEALTH_RESTARTS = 3,
};

typedef struct {
    aec_handle_t *aec;
    aec_config_t config;
    xiaotai_critical_t lock;
    xiaotai_processor_stats_t stats;
    int16_t *pcm[3]; /* MIC, REF, CLEAN, separately SIMD-aligned */
    uint16_t samples;
    uint8_t channels, slots[2];
    int64_t time_us;
    uint32_t pending_loss, frame_loss;
    /* Health check: energies of the previous block (the canceller's output
       lags its input by one block), blocks heard and blocks with echo removed. */
    int64_t last_mic, last_reference;
    uint16_t heard, removed;
    uint8_t restarts;
    bool checking;
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
    /* The same tuning keys as the speech front end: 0 low cost, 1 high
       performance; the filter in blocks; the residual suppression level. */
    const int32_t mode = xiaotai_media_param("aec.mode", 0);
    const int32_t filter = xiaotai_media_param("aec.filter_length", 4);
    const int32_t nlp = xiaotai_media_param("aec.nlp", 1);
    if (mode < 0 || mode > 1 || filter < 1 || filter > 8 || nlp < 0 || nlp > 2) {
        cleanup(state);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    state->config = (aec_config_t){.mic_num = 1, .ref_num = 1, .out_num = 1, .filter_length = (int)filter,
        .sample_rate = 16000, .mode = mode ? AEC_MODE_FD_HIGH_PERF : AEC_MODE_FD_LOW_COST,
        .nlp_level = nlp == 0 ? AEC_NLP_LEVEL_NORMAL : nlp == 1 ? AEC_NLP_LEVEL_AGGR : AEC_NLP_LEVEL_VERYAGGR,
        .caps = MALLOC_CAP_8BIT | (xiaotai_mem_has_external() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL)};
    state->checking = true;
    state->aec = aec_create_from_config(&state->config);
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

static void restart(direct_aec_t *state)
{
    const int64_t begin = xiaotai_time_us();
    aec_handle_t *fresh = aec_create_from_config(&state->config);
    if (!fresh || aec_get_chunksize(fresh) != (int)state->samples) {
        if (fresh) aec_destroy(fresh);
        state->checking = false; /* keep the one there is */
        XIAOTAI_LOGW(TAG, "restart failed");
        return;
    }
    aec_destroy(state->aec);
    state->aec = fresh;
    ++state->restarts;
    XIAOTAI_LOGW(TAG, "restart n=%u heard=%u removed=%u us=%u", (unsigned)state->restarts, (unsigned)state->heard,
                 (unsigned)state->removed, (unsigned)(xiaotai_time_us() - begin));
    state->heard = state->removed = 0;
    xiaotai_critical_enter(&state->lock);
    ++state->stats.restarts;
    xiaotai_critical_exit(&state->lock);
}

static void check_health(direct_aec_t *state, int64_t mic, int64_t reference, int64_t out)
{
    const int64_t level = (int64_t)HEALTH_LEVEL * state->samples;
    if (state->checking && state->last_mic >= level && state->last_reference >= level) {
        ++state->heard;
        if (out * 10 <= state->last_mic) ++state->removed;
        if (state->removed >= HEALTH_PROVEN) {
            state->checking = false;
        } else if (state->heard >= HEALTH_HEARD) {
            if (state->restarts < HEALTH_RESTARTS) restart(state);
            else state->checking = false;
        }
    }
    state->last_mic = mic;
    state->last_reference = reference;
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
    int64_t power[2] = {0, 0};
    for (size_t i = 0; i < state->samples; ++i) {
        for (unsigned c = 0; c < 2; ++c) {
            int16_t sample;
            memcpy(&sample, bytes + (i * state->channels + state->slots[c]) * sizeof(sample), sizeof(sample));
            state->pcm[c][i] = sample;
            clipped[c] += sample >= 32700 || sample <= -32700;
            power[c] += (int32_t)sample * sample;
        }
    }
    aec_process(state->aec, state->pcm[0], state->pcm[1], state->pcm[2]);
    if (state->checking) {
        int64_t out = 0;
        for (size_t i = 0; i < state->samples; ++i) out += (int32_t)state->pcm[2][i] * state->pcm[2][i];
        check_health(state, power[0], power[1], out);
    }
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
