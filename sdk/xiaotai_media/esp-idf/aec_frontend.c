// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdatomic.h>
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_err.h"
#include "processors/frontend_pairing.h"
#include "xiaotai/media.h"
#include "xiaotai/media_config.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

typedef struct {
    const esp_afe_sr_iface_t *iface;
    esp_afe_sr_data_t *afe;
    media_frontend_pairing_t *pairing;
    xiaotai_task_t worker;
    atomic_bool stopping, stopped;
    bool worker_live;
    uint16_t yield_period_ms;
} frontend_t;

static int afe_feed(void *opaque, const int16_t *pcm)
{
    frontend_t *front = opaque;
    return front->iface->feed(front->afe, pcm);
}

static xiaotai_err_t afe_fetch(void *opaque, const int16_t **pcm, size_t *samples, uint32_t timeout_ms)
{
    frontend_t *front = opaque;
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    const afe_fetch_result_t *result = front->iface->fetch_with_delay(front->afe, ticks);
    if (!result || result->ret_value == ESP_FAIL) return XIAOTAI_ERR_TIMEOUT;
    if (result->ret_value < 0 || result->data_size <= 0 || result->data_size % sizeof(int16_t)) {
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    *pcm = result->data;
    *samples = (size_t)result->data_size / sizeof(int16_t);
    return XIAOTAI_OK;
}

static void feed_worker(void *opaque)
{
    frontend_t *front = opaque;
    int64_t last_yield = xiaotai_time_us();
    while (!atomic_load_explicit(&front->stopping, memory_order_acquire)) {
        const xiaotai_err_t err = media_frontend_pairing_feed_next(front->pairing, 20);
        if (err == XIAOTAI_ERR_TIMEOUT) continue;
        if (err == XIAOTAI_ERR_INVALID_STATE) break;
        if (front->yield_period_ms == 1 || xiaotai_time_us() - last_yield >= (int64_t)front->yield_period_ms * 1000) {
            xiaotai_delay_ms(1);
            last_yield = xiaotai_time_us();
        }
    }
    atomic_store_explicit(&front->stopped, true, memory_order_release);
    xiaotai_task_exit();
}

static void cleanup(void *opaque)
{
    frontend_t *front = opaque;
    if (front->worker_live) {
        atomic_store_explicit(&front->stopping, true, memory_order_release);
        while (!atomic_load_explicit(&front->stopped, memory_order_acquire)) xiaotai_delay_ms(1);
    }
    media_frontend_pairing_close(front->pairing);
    if (front->afe) front->iface->destroy(front->afe);
    xiaotai_mem_free(front);
}

static xiaotai_err_t open_frontend(const xiaotai_audio_input_t *input, xiaotai_processor_info_t *info,
                                  void **state, uint8_t microphones)
{
    if (!input || !info || !state || !media_frontend_pairing_slots(&input->desc, microphones, NULL)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    const bool dual = microphones == 2;
    const int32_t core = xiaotai_media_param("aec.task_core", 0);
    const int32_t priority = xiaotai_media_param("aec.feed_priority", 7);
    const int32_t feeder = xiaotai_media_param("aec.feed_core", -1);
    const int32_t ring = xiaotai_media_param("aec.ring_frames", 17);
    const int32_t requested_slots = xiaotai_media_param("aec.input_slots", 0);
    const int32_t wait = xiaotai_media_param("aec.stamp_wait_ms", 20);
    const bool strict = xiaotai_media_param("aec.strict_feed", 0) != 0;
    /* The canceller itself: 0 low cost, 1 high performance; its filter in
       blocks; how hard the residual suppression works (0 normal, 1
       aggressive, 2 very aggressive); the front end's output gain in
       centi-dB; separation of the two microphones; noise suppression. */
    const int32_t mode = xiaotai_media_param("aec.mode", 0);
    const int32_t filter = xiaotai_media_param("aec.filter_length", 4);
    const int32_t nlp = xiaotai_media_param("aec.nlp", 1);
    const int32_t gain_cdb = xiaotai_media_param("aec.gain_cdb", 600);
    const bool separate = dual && xiaotai_media_param("aec.separation", 1) != 0;
    const bool denoise = xiaotai_media_param("aec.noise_suppression", 0) != 0;
    if (core < 0 || core > 1 || feeder < -1 || feeder > 1 || priority <= 0 || priority >= configMAX_PRIORITIES ||
        ring <= 0 || ring > 32 || requested_slots < 0 || requested_slots == 1 || requested_slots > 8 ||
        wait < 0 || wait > 20 || feeder == core || mode < 0 || mode > 1 || filter < 1 || filter > 8 ||
        nlp < 0 || nlp > 2 || gain_cdb < -2000 || gain_cdb > 2000)
        return XIAOTAI_ERR_INVALID_ARG;
    static const int nlp_levels[] = {AEC_NLP_LEVEL_NORMAL, AEC_NLP_LEVEL_AGGR, AEC_NLP_LEVEL_VERYAGGR};
    const int aec_mode = mode ? AEC_MODE_FD_HIGH_PERF : AEC_MODE_FD_LOW_COST;
    const int nlp_level = nlp_levels[nlp];

    frontend_t *front = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*front));
    if (!front) return XIAOTAI_ERR_NO_MEM;
    atomic_init(&front->stopping, false);
    atomic_init(&front->stopped, false);
    afe_config_t *cfg = afe_config_init(dual ? "MMR" : "MR", NULL, AFE_TYPE_FD, mode ? AFE_MODE_HIGH_PERF : AFE_MODE_LOW_COST);
    if (!cfg) { cleanup(front); return XIAOTAI_ERR_NO_MEM; }
    cfg->aec_init = true;
    cfg->aec_mode = aec_mode;
    cfg->aec_filter_length = (int)filter;
    cfg->aec_nlp_level = nlp_level;
    cfg->se_init = separate;
    cfg->ns_init = denoise;
    if (denoise) {
        cfg->ns_model_name = NULL; /* no model partition: the WebRTC suppressor */
        cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    }
    cfg->vad_init = true;
    cfg->vad_model_name = NULL;
    cfg->vad_mode = VAD_MODE_2;
    cfg->vad_enable_channel_trigger = true;
    cfg->vad_mute_playback = false;
    cfg->wakenet_init = false;
    cfg->agc_init = false;
    cfg->afe_linear_gain = gain_cdb == 600 ? 1.9952623f : powf(10.0f, (float)gain_cdb / 2000.0f);
    cfg->memory_alloc_mode = XIAOTAI_MEDIA_AFE_MEMORY_EXTERNAL ? AFE_MEMORY_ALLOC_MORE_PSRAM
                                                               : AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    cfg->afe_ringbuf_size = ring;
    cfg->afe_perferred_core = core;
    cfg->afe_perferred_priority = priority;
    cfg->fixed_first_channel = false;
    cfg->fixed_output_channel = false;
    cfg->output_playback_channel = false;
    afe_config_check(cfg);
    if (!cfg->aec_init || cfg->aec_mode != aec_mode || cfg->aec_filter_length != (int)filter ||
        cfg->aec_nlp_level != nlp_level || cfg->se_init != separate || cfg->ns_init != denoise || !cfg->vad_init ||
        !cfg->vad_enable_channel_trigger || cfg->fixed_output_channel || cfg->pcm_config.ref_num != 1 ||
        cfg->pcm_config.mic_num != microphones || cfg->pcm_config.total_ch_num != microphones + 1) {
        afe_config_free(cfg);
        cleanup(front);
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    front->iface = esp_afe_handle_from_config(cfg);
    if (front->iface) front->afe = front->iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (!front->afe) { cleanup(front); return XIAOTAI_ERR_NO_MEM; }
    const int feed = front->iface->get_feed_chunksize(front->afe);
    const int fetch = front->iface->get_fetch_chunksize(front->afe);
    if (feed <= 0 || fetch <= 0 || feed > (int)XIAOTAI_PROCESSOR_SAMPLES_MAX ||
        fetch > feed || (fetch & 1) || feed % fetch || feed / fetch > 2 ||
        front->iface->get_feed_channel_num(front->afe) != microphones + 1 ||
        front->iface->get_fetch_channel_num(front->afe) != 1 || front->iface->get_samp_rate(front->afe) != 16000 ||
        (!requested_slots && (4096 % feed || 4096 / feed > 8 || 4096 / feed < 2))) {
        cleanup(front);
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    const media_frontend_pairing_config_t pairing_cfg = {.input = &input->desc, .feed_samples = (uint16_t)feed,
        .fetch_samples = (uint16_t)fetch, .microphones = microphones,
        .input_slots = (uint8_t)(requested_slots ? requested_slots : 4096 / feed),
        .ring_frames = (uint8_t)ring, .stamp_wait_ms = (uint8_t)wait, .strict_feed = strict,
        .count_monitor_clipping = xiaotai_media_param("aec.count_monitor_clipping", 0) != 0};
    const media_frontend_ops_t ops = {.feed = afe_feed, .fetch = afe_fetch, .ctx = front};
    xiaotai_err_t err = media_frontend_pairing_open(&pairing_cfg, &ops, &front->pairing);
    if (err != XIAOTAI_OK) { cleanup(front); return err; }
    front->yield_period_ms = dual ? 1U : 250U;
    const xiaotai_task_config_t task = {.name = "xt_afe_feed", .entry = feed_worker, .arg = front,
        .stack_bytes = 8192, .stack = XIAOTAI_MEM_BULK, .priority = (unsigned)priority,
        .core = feeder == -1 ? 1 - core : feeder};
    err = xiaotai_task_create(&task, &front->worker);
    if (err != XIAOTAI_OK) { cleanup(front); return err; }
    front->worker_live = true;
    *info = (xiaotai_processor_info_t){.flags = XIAOTAI_PROCESSOR_ASYNC | XIAOTAI_PROCESSOR_CANCELS_ECHO,
        .rate_hz = 16000, .feed_frames = (uint16_t)feed, .fetch_max = (uint16_t)fetch,
        .feed_stack_bytes = 1024, .fetch_stack_bytes = 2048};
    *state = front;
    return XIAOTAI_OK;
}

static xiaotai_err_t feed(void *state, const void *data, size_t bytes, int64_t time_us, uint32_t lost)
{
    return media_frontend_pairing_feed(((frontend_t *)state)->pairing, data, bytes, time_us, lost);
}

static xiaotai_err_t fetch(void *state, xiaotai_processor_frame_t *frame, uint32_t timeout)
{
    return media_frontend_pairing_fetch(((frontend_t *)state)->pairing, frame, timeout);
}

static void stats(void *state, xiaotai_processor_stats_t *out)
{
    media_frontend_pairing_stats(((frontend_t *)state)->pairing, out);
}

#if XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL
static int match_dual(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    (void)out;
    return media_frontend_pairing_slots(in, 2, NULL) ? 3 : 0;
}

static xiaotai_err_t open_dual(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                               xiaotai_processor_info_t *info, void **state)
{
    (void)out;
    return open_frontend(in, info, state, 2);
}

const xiaotai_audio_processor_t media_aec_dual = {.name = "aec.dual", .match = match_dual, .open = open_dual,
    .feed = feed, .fetch = fetch, .stats = stats, .close = cleanup};
#endif

#if XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND
static int match_single(const xiaotai_audio_desc_t *in, const xiaotai_audio_desc_t *out)
{
    (void)out;
    return media_frontend_pairing_slots(in, 1, NULL) ? 2 : 0;
}

static xiaotai_err_t open_single(const xiaotai_audio_input_t *in, const xiaotai_audio_output_t *out,
                                 xiaotai_processor_info_t *info, void **state)
{
    (void)out;
    return open_frontend(in, info, state, 1);
}

const xiaotai_audio_processor_t media_aec_single_frontend = {.name = "aec.single.frontend", .match = match_single,
    .open = open_single, .feed = feed, .fetch = fetch, .stats = stats, .close = cleanup};
#endif
