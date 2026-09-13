/* One official AFE owns both microphones and the synchronous MIC3 reference.
 * Acquisition must keep draining DMA independently of AFE processing. */
#include "starter_aec.h"
#include "starter_audio_resampler.h"
#include "starter_agc.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_afe_sr_models.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AFE_INPUT_SLOTS 4U
#define AFE_RING_FRAMES 12U
#define AFE_MAX_SAMPLES 1024U
#define AFE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    int64_t captured_ms;
    uint32_t capture_epoch, mute_epoch, mic_clipped, reference_clipped;
    starter_signal_level_t mic[2], reference;
} afe_frame_stamp_t;

typedef struct {
    int16_t *pcm;
    afe_frame_stamp_t stamp[2];
} afe_input_slot_t;

typedef struct {
    const esp_afe_sr_iface_t *iface;
    esp_afe_sr_data_t *handle;
    size_t feed_samples, fetch_samples, chunks_per_feed;
    int16_t *tdm, *clean, *uplink, *output_8k;
    afe_input_slot_t input[AFE_INPUT_SLOTS];
    QueueHandle_t free_slots, pending_slots, stamps;
    TaskHandle_t feed_worker;
    atomic_bool failed;
    atomic_uint feed_frames, fetch_frames, input_full, fetch_timeouts;
    atomic_uint max_feed_us, last_feed_us;
    atomic_int channel_id;
    starter_audio_resampler_16k_to_8k_t resampler;
} starter_aec_context_t;

static const char *TAG = "starter_aec";
static EXT_RAM_BSS_ATTR starter_aec_context_t s_aec;

static int16_t *allocate_samples(size_t count)
{
    return heap_caps_aligned_calloc(16, count, sizeof(int16_t), AFE_CAPS);
}

static void fail_pipeline(const char *stage, int result)
{
    if (!atomic_exchange(&s_aec.failed, true))
        ESP_LOGE(TAG, "AFE fail: %s rc=%d", stage, result);
}

static void afe_feed_worker(void *argument)
{
    (void)argument;
    for (;;) {
        uint8_t slot;
        if (xQueueReceive(s_aec.pending_slots, &slot, portMAX_DELAY) != pdTRUE) continue;
        afe_input_slot_t *input = &s_aec.input[slot];
        if (!atomic_load(&s_aec.failed)) {
            /* Metadata becomes visible before feed publishes PCM to fetch. */
            for (size_t n = 0; n < s_aec.chunks_per_feed; ++n) {
                if (xQueueSend(s_aec.stamps, &input->stamp[n], 0) != pdTRUE) {
                    fail_pipeline("timestamp queue", -1);
                    break;
                }
            }
            if (!atomic_load(&s_aec.failed)) {
                int64_t started = esp_timer_get_time();
                int ret = s_aec.iface->feed(s_aec.handle, input->pcm);
                uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
                atomic_store(&s_aec.last_feed_us, elapsed);
                if (elapsed > atomic_load(&s_aec.max_feed_us))
                    atomic_store(&s_aec.max_feed_us, elapsed);
                if (ret <= 0) fail_pipeline("feed", ret);
                else atomic_fetch_add(&s_aec.feed_frames, 1);
            }
        }
        (void)xQueueSend(s_aec.free_slots, &slot, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void starter_aec_release(void)
{
    /* Initialization failure only, before the ADC reader starts. */
    if (s_aec.feed_worker) vTaskDeleteWithCaps(s_aec.feed_worker);
    if (s_aec.free_slots) vQueueDeleteWithCaps(s_aec.free_slots);
    if (s_aec.pending_slots) vQueueDeleteWithCaps(s_aec.pending_slots);
    if (s_aec.stamps) vQueueDeleteWithCaps(s_aec.stamps);
    for (size_t i = 0; i < AFE_INPUT_SLOTS; ++i) heap_caps_free(s_aec.input[i].pcm);
    heap_caps_free(s_aec.tdm);
    heap_caps_free(s_aec.clean);
    heap_caps_free(s_aec.uplink);
    heap_caps_free(s_aec.output_8k);
    starter_agc_deinit();
    if (s_aec.handle) s_aec.iface->destroy(s_aec.handle);
    memset(&s_aec, 0, sizeof(s_aec));
}

esp_err_t starter_aec_init(void)
{
    if (s_aec.handle) return ESP_OK;
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    afe_config_t *config = afe_config_init("MMR", NULL, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (!config) return ESP_ERR_NO_MEM;
    config->aec_init = true;
    config->aec_mode = AEC_MODE_FD_LOW_COST;
    config->aec_filter_length = 4;
    /* NORMAL leaked the cold-start greeting into ASR in the acoustic test.
     * Keep the official default, not VERYAGGR; preserve ADC headroom below. */
    config->aec_nlp_level = AEC_NLP_LEVEL_AGGR;
    config->se_init = true;
    config->ns_init = false; /* Official dual-mic SE takes precedence over mono NS. */
    config->vad_init = true;
    config->vad_model_name = NULL; /* Official WebRTC VAD, no extra neural model. */
    config->vad_mode = VAD_MODE_2;
    config->vad_enable_channel_trigger = true;
    config->vad_mute_playback = false;
    config->wakenet_init = false; /* Keep the existing custom wake word in voice. */
    config->agc_init = false; /* One existing ESP-SR WebRTC AGC follows fetch. */
    config->afe_linear_gain = STARTER_AEC_OUTPUT_GAIN;
    /* User-selected official DSP working-set policy. Only AFE internals may
     * use more internal RAM; our PCM pools, queues and stacks stay in PSRAM. */
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    config->afe_ringbuf_size = AFE_RING_FRAMES;
    /* SE and feed must not serialize on CPU1 during far-end playback. Keep
     * feed/ADC on CPU1 and SE/fetch on CPU0; moving feed also stalls wake NN. */
    config->afe_perferred_core = 0;
    config->afe_perferred_priority = 7;
    config->fixed_first_channel = false;
    config->fixed_output_channel = false;
    config->output_playback_channel = false;
    afe_config_check(config);
    if (!config->aec_init || !config->se_init || !config->vad_enable_channel_trigger ||
        config->fixed_output_channel || config->pcm_config.mic_num != 2 ||
        config->pcm_config.ref_num != 1 || config->pcm_config.total_ch_num != 3) {
        afe_config_free(config);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_aec.iface = esp_afe_handle_from_config(config);
    if (s_aec.iface) s_aec.handle = s_aec.iface->create_from_config(config);
    afe_config_free(config);
    if (!s_aec.handle) return ESP_ERR_NO_MEM;
    int feed = s_aec.iface->get_feed_chunksize(s_aec.handle);
    int fetch = s_aec.iface->get_fetch_chunksize(s_aec.handle);
    if (feed <= 0 || fetch <= 0 || feed > AFE_MAX_SAMPLES || fetch > feed ||
        feed % fetch != 0 || feed / fetch > 2 || (fetch & 1) ||
        s_aec.iface->get_feed_channel_num(s_aec.handle) != 3 ||
        s_aec.iface->get_fetch_channel_num(s_aec.handle) != 1 ||
        s_aec.iface->get_samp_rate(s_aec.handle) != STARTER_AEC_SAMPLE_RATE_HZ) {
        starter_aec_release();
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_aec.feed_samples = (size_t)feed;
    s_aec.fetch_samples = (size_t)fetch;
    s_aec.chunks_per_feed = (size_t)(feed / fetch);
    s_aec.tdm = allocate_samples(s_aec.feed_samples * STARTER_AEC_CAPTURE_DMA_CHANNELS);
    s_aec.clean = allocate_samples(s_aec.fetch_samples);
    s_aec.output_8k = allocate_samples(s_aec.fetch_samples / 2U);
    s_aec.free_slots = xQueueCreateWithCaps(AFE_INPUT_SLOTS, sizeof(uint8_t), AFE_CAPS);
    s_aec.pending_slots = xQueueCreateWithCaps(AFE_INPUT_SLOTS, sizeof(uint8_t), AFE_CAPS);
    s_aec.stamps = xQueueCreateWithCaps((AFE_RING_FRAMES + AFE_INPUT_SLOTS) * 2U,
                                      sizeof(afe_frame_stamp_t), AFE_CAPS);
    bool buffers_ok = s_aec.tdm && s_aec.clean && s_aec.output_8k &&
                      s_aec.free_slots && s_aec.pending_slots && s_aec.stamps;
    for (uint8_t i = 0; i < AFE_INPUT_SLOTS; ++i) {
        s_aec.input[i].pcm = allocate_samples(s_aec.feed_samples * 3U);
        buffers_ok = buffers_ok && s_aec.input[i].pcm;
        if (s_aec.free_slots) (void)xQueueSend(s_aec.free_slots, &i, 0);
    }
#if CONFIG_XIAOTAI_CAPTURE_AGC
    s_aec.uplink = allocate_samples(s_aec.fetch_samples);
    buffers_ok = buffers_ok && s_aec.uplink && starter_agc_init() == ESP_OK;
#endif
    if (!buffers_ok || xTaskCreatePinnedToCoreWithCaps(afe_feed_worker, "afe_feed", 8192,
            NULL, 7, &s_aec.feed_worker, 1, AFE_CAPS) != pdPASS) {
        starter_aec_release();
        return ESP_ERR_NO_MEM;
    }
    s_aec.iface->print_pipeline(s_aec.handle);
    ESP_LOGI(TAG, "AFE 2.4.7 MMR/16k in/out=%d/%d FD-L NLP=AGGR tail=4 "
             "SE=1 VAD=W2 pga=%ddB gain=%d/1000 core=feed1/se0 mem=balanced q=%u RAM/PSRAM=%u/%u B",
             feed, fetch, (int)STARTER_AEC_MIC_PGA_DB,
             (int)(STARTER_AEC_OUTPUT_GAIN * 1000.0f), AFE_RING_FRAMES,
             (unsigned)(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             (unsigned)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ESP_OK;
}

size_t starter_aec_capture_bytes(void)
{
    return s_aec.feed_samples * STARTER_AEC_CAPTURE_DMA_CHANNELS * sizeof(int16_t);
}

int16_t *starter_aec_capture_buffer(void) { return s_aec.tdm; }
size_t starter_aec_frame_samples(void) { return s_aec.fetch_samples; }

esp_err_t starter_aec_submit_capture(size_t bytes, int64_t captured_ms,
                                     uint32_t capture_epoch, uint32_t mute_epoch)
{
    if (!s_aec.handle || bytes != starter_aec_capture_bytes()) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_aec.failed)) return ESP_ERR_INVALID_STATE;
    uint8_t slot;
    if (xQueueReceive(s_aec.free_slots, &slot, 0) != pdTRUE) {
        unsigned count = atomic_fetch_add(&s_aec.input_full, 1) + 1U;
        if (count == 1 || count % 100U == 0)
            ESP_LOGE(TAG, "AFE in-ovf=%u", count);
        return ESP_ERR_NO_MEM;
    }
    afe_input_slot_t *input = &s_aec.input[slot];
    /* Repack physical M/R/M/N to official interleaved M/M/R. No gain or
     * software estimate of speaker output is applied before AFE. */
    for (size_t i = 0; i < s_aec.feed_samples; ++i) {
        input->pcm[i * 3U] = s_aec.tdm[i * STARTER_AEC_CAPTURE_DMA_CHANNELS + STARTER_AEC_MIC_SLOT];
        input->pcm[i * 3U + 1U] = s_aec.tdm[i * STARTER_AEC_CAPTURE_DMA_CHANNELS + STARTER_AEC_SECOND_MIC_SLOT];
        input->pcm[i * 3U + 2U] = s_aec.tdm[i * STARTER_AEC_CAPTURE_DMA_CHANNELS + STARTER_AEC_REFERENCE_SLOT];
    }
    for (size_t f = 0; f < s_aec.chunks_per_feed; ++f) {
        afe_frame_stamp_t *stamp = &input->stamp[f];
        memset(stamp, 0, sizeof(*stamp));
        stamp->captured_ms = captured_ms - (int64_t)((s_aec.chunks_per_feed - f - 1U) *
            s_aec.fetch_samples * 1000U / STARTER_AEC_SAMPLE_RATE_HZ);
        stamp->capture_epoch = capture_epoch;
        stamp->mute_epoch = mute_epoch;
        int64_t sum[3] = {0};
        uint64_t square[3] = {0};
        uint32_t peak[3] = {0};
        for (size_t i = 0; i < s_aec.fetch_samples; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                int32_t value = input->pcm[(f * s_aec.fetch_samples + i) * 3U + c];
                uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
                sum[c] += value;
                square[c] += (int64_t)value * value;
                if (magnitude > peak[c]) peak[c] = magnitude;
                if (magnitude >= 32700U) {
                    if (c == 2) ++stamp->reference_clipped;
                    else ++stamp->mic_clipped;
                }
            }
        }
        for (size_t c = 0; c < 3; ++c) {
            starter_signal_level_t *level = c < 2 ? &stamp->mic[c] : &stamp->reference;
            level->peak = peak[c];
            level->dc = (int32_t)(sum[c] / (int64_t)s_aec.fetch_samples);
            level->rms = starter_signal_isqrt(square[c] / s_aec.fetch_samples);
        }
    }
    if (xQueueSend(s_aec.pending_slots, &slot, 0) != pdTRUE) {
        (void)xQueueSend(s_aec.free_slots, &slot, 0);
        fail_pipeline("input queue", -1);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t starter_aec_fetch(starter_aec_output_t *output)
{
    if (!s_aec.handle || !output) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s_aec.failed)) return ESP_ERR_INVALID_STATE;
    int64_t fetch_begin = esp_timer_get_time();
    afe_fetch_result_t *result = s_aec.iface->fetch_with_delay(s_aec.handle, pdMS_TO_TICKS(200));
    uint32_t fetch_wait_us = (uint32_t)(esp_timer_get_time() - fetch_begin);
    output->fetch_wait_us = fetch_wait_us;
    if (!result || result->ret_value != ESP_OK || !result->data) {
        atomic_fetch_add(&s_aec.fetch_timeouts, 1);
        return ESP_ERR_TIMEOUT;
    }
    if (result->data_size != s_aec.fetch_samples * sizeof(int16_t)) {
        fail_pipeline("fetch size", result->data_size);
        return ESP_ERR_INVALID_SIZE;
    }
    afe_frame_stamp_t stamp;
    if (xQueueReceive(s_aec.stamps, &stamp, 0) != pdTRUE) {
        fail_pipeline("fetch timestamp", -1);
        return ESP_FAIL;
    }
    atomic_fetch_add(&s_aec.fetch_frames, 1);
    atomic_store(&s_aec.channel_id, result->trigger_channel_id);
    /* VAD selects channels, it does not gate this continuous stream.
     * Replaying its speech-onset cache here would duplicate old samples. */
    memcpy(s_aec.clean, result->data, result->data_size);
    const int16_t *uplink = s_aec.clean;
    uint32_t agc_us = 0;
#if CONFIG_XIAOTAI_CAPTURE_AGC
    int64_t agc_begin = esp_timer_get_time();
    esp_err_t ret = starter_agc_process(s_aec.clean, s_aec.uplink, s_aec.fetch_samples);
    agc_us = (uint32_t)(esp_timer_get_time() - agc_begin);
    output->agc_us = agc_us;
    if (ret != ESP_OK) return ret;
    uplink = s_aec.uplink;
#endif
    size_t count_8k = s_aec.fetch_samples / 2U;
    int64_t resample_begin = esp_timer_get_time();
    size_t resampled = starter_audio_resampler_16k_to_8k_process(&s_aec.resampler, uplink,
            s_aec.fetch_samples, s_aec.output_8k, count_8k);
    uint32_t resample_us = (uint32_t)(esp_timer_get_time() - resample_begin);
    output->resample_us = resample_us;
    if (resampled != count_8k) return ESP_FAIL;
    int64_t measure_start = esp_timer_get_time();
    starter_signal_level_t clean_level = starter_signal_measure(s_aec.clean, s_aec.fetch_samples);
    starter_signal_level_t uplink_level = starter_signal_measure(s_aec.output_8k, count_8k);
    *output = (starter_aec_output_t) {
        .pcm_16k = s_aec.clean, .samples_16k = s_aec.fetch_samples,
#if CONFIG_XIAOTAI_WAKE_CAPTURE_AGC
        .wake_pcm_16k = uplink, .wake_delay_ms = STARTER_AGC_DELAY_MS,
#else
        .wake_pcm_16k = s_aec.clean, .wake_delay_ms = 0,
#endif
        .pcm_8k = s_aec.output_8k, .samples = count_8k,
        .captured_ms = stamp.captured_ms, .capture_epoch = stamp.capture_epoch,
        .mute_epoch = stamp.mute_epoch,
        .mic_clipped = stamp.mic_clipped, .reference_clipped = stamp.reference_clipped,
        /* The stronger physical input, not a claim of selected-source ERLE. */
        .level = {stamp.mic[stamp.mic[1].rms > stamp.mic[0].rms], stamp.reference, clean_level},
        .measurement_us = (uint32_t)(esp_timer_get_time() - measure_start),
        .feed_us = atomic_load(&s_aec.last_feed_us),
        .fetch_wait_us = fetch_wait_us, .agc_us = agc_us, .resample_us = resample_us,
        .uplink_level = uplink_level,
    };
    return ESP_OK;
}

void starter_aec_get_stats(starter_aec_stats_t *stats)
{
    if (!stats) return;
    *stats = (starter_aec_stats_t) {
        .feed_frames = atomic_load(&s_aec.feed_frames), .fetch_frames = atomic_load(&s_aec.fetch_frames),
        .input_full = atomic_load(&s_aec.input_full), .fetch_timeouts = atomic_load(&s_aec.fetch_timeouts),
        .max_feed_us = atomic_load(&s_aec.max_feed_us), .channel_id = atomic_load(&s_aec.channel_id),
        .failed = atomic_load(&s_aec.failed),
    };
}
