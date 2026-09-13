#include "audio_echo_cancel.h"

#include <stdbool.h>
#include <string.h>

#include "app_config.h"

#if APP_CONFIG_AUDIO_AEC_ENABLE

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "app_log_policy.h"
#include "app_memory_policy.h"

/*
 * ESP-SR AEC adapter for the app PCM path.
 *
 * The capture task uploads 16 kHz mono PCM in 20 ms frames, while ESP-SR AEC
 * consumes 32 ms frames. On ES8311 boards, capture channel 1 carries the
 * codec's synchronous DAC reference and is preferred over guessed timing.
 * The delayed software playback ring remains available as a board-independent
 * fallback. Capture samples are accumulated into ESP-SR sized frames, and
 * processed output is returned through a small PSRAM FIFO.
 */

#define AUDIO_AEC_SAMPLE_RATE_HZ              16000U
#define AUDIO_AEC_REF_RING_SAMPLES            8192U
#define AUDIO_AEC_REF_RING_MASK               (AUDIO_AEC_REF_RING_SAMPLES - 1U)
#define AUDIO_AEC_OUT_FIFO_SAMPLES            2048U
#define AUDIO_AEC_OUT_FIFO_MASK               (AUDIO_AEC_OUT_FIFO_SAMPLES - 1U)
#define AUDIO_AEC_REF_DELAY_MS                ((uint32_t)APP_CONFIG_AUDIO_AEC_REF_DELAY_MS)
#define AUDIO_AEC_REF_DELAY_SAMPLES           ((AUDIO_AEC_SAMPLE_RATE_HZ * AUDIO_AEC_REF_DELAY_MS) / 1000U)
#define AUDIO_AEC_REF_ACTIVE_US               500000LL
#define AUDIO_AEC_REF_ACTIVE_PEAK             64U
#define AUDIO_AEC_CODEC_REF_DETECT_PEAK       32U
#define AUDIO_AEC_CODEC_REF_PROBE_FRAMES      5U
#define AUDIO_AEC_REF_RESYNC_TOLERANCE_SAMPLES (AUDIO_AEC_SAMPLE_RATE_HZ / 25U)
#define AUDIO_AEC_SOFTWARE_CONVERGENCE_FRAMES 12U
#define AUDIO_AEC_CODEC_CONVERGENCE_FRAMES    4U
#define AUDIO_AEC_PROFILE_NAME                "fd-high-perf-double-talk"
#define AUDIO_AEC_FILTER_LENGTH               4
#define AUDIO_AEC_BUFFER_ALIGNMENT            64U
#define AUDIO_AEC_PSRAM_CAPS                  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AUDIO_AEC_DEINIT_WAIT_MS              60U
#define AUDIO_AEC_ANALYSIS_SHIFT              8U
#define AUDIO_AEC_COHERENCE_MAX_LAG_SAMPLES   64
#define AUDIO_AEC_COHERENCE_LAG_STEP          4
#define AUDIO_AEC_COHERENCE_SAMPLE_STEP       4U
#define AUDIO_AEC_NEAR_END_MIN_OUT_PEAK       256U
#define AUDIO_AEC_NEAR_END_MIN_RETAINED_PERCENT 2U
#define AUDIO_AEC_NEAR_END_MAX_COHERENCE_PERCENT 25U
#define AUDIO_AEC_NEAR_END_ADAPT_FRAMES       12U
#define AUDIO_AEC_NEAR_END_CONFIRM_FRAMES     2U
#define AUDIO_AEC_NEAR_END_HANGOVER_FRAMES    8U

static const char *TAG = "audio_aec";

static portMUX_TYPE s_aec_lock = portMUX_INITIALIZER_UNLOCKED;

static aec_handle_t *s_aec;
static int s_aec_frame_size;
static int16_t *s_ref_ring;
static int16_t *s_mic_frame;
static int16_t *s_ref_frame;
static int16_t *s_out_frame;
static int16_t *s_analysis_mic_frame;
static int16_t *s_analysis_ref_frame;
static int16_t *s_out_fifo;
static uint32_t s_ref_write_pos;
static uint32_t s_ref_filled_samples;
static uint32_t s_ref_read_pos;
static uint32_t s_ref_resync_count;
static uint32_t s_last_ref_peak;
static int64_t s_last_playback_us;
static uint32_t s_frame_fill;
static uint32_t s_convergence_processed_frames;
static uint32_t s_out_fifo_read_pos;
static uint32_t s_out_fifo_used;
static bool s_ref_read_ready;
static bool s_out_fifo_ready;
static bool s_initializing;
static bool s_deinit_requested;
static bool s_create_failed_logged;
static bool s_runtime_active;
static bool s_first_processed_logged;
static bool s_warmup_guard_logged;
static bool s_codec_reference_detected;
static bool s_codec_reference_rejected;
static uint32_t s_codec_reference_probe_frames;
static audio_echo_cancel_reference_t s_reference_source;
static uint32_t s_active_users;
static uint32_t s_process_frames;
static uint64_t s_process_us_total;
static uint32_t s_process_us_max;
static aec_nlp_level_t s_requested_nlp_level = AEC_NLP_LEVEL_AGGR;
static aec_nlp_level_t s_applied_nlp_level = AEC_NLP_LEVEL_AGGR;
static uint32_t s_reference_active_frames;
static uint32_t s_near_end_candidate_frames;
static uint32_t s_near_end_hangover_frames;
static bool s_last_near_end_detected;

typedef enum {
    AUDIO_AEC_REF_INACTIVE = 0,
    AUDIO_AEC_REF_WARMING,
    AUDIO_AEC_REF_READY,
} audio_aec_ref_state_t;

static inline uint32_t audio_aec_abs_i16(int32_t value)
{
    return (uint32_t)(value < 0 ? -value : value);
}

static inline uint32_t audio_aec_ring_prev(uint32_t pos, uint32_t back)
{
    return (pos + AUDIO_AEC_REF_RING_SAMPLES - (back & AUDIO_AEC_REF_RING_MASK)) & AUDIO_AEC_REF_RING_MASK;
}

static inline uint32_t audio_aec_ring_distance(uint32_t from, uint32_t to)
{
    return (to - from) & AUDIO_AEC_REF_RING_MASK;
}

static const char *audio_aec_reference_name(audio_echo_cancel_reference_t reference)
{
    switch (reference) {
    case AUDIO_ECHO_CANCEL_REFERENCE_CODEC:
        return "codec";
    case AUDIO_ECHO_CANCEL_REFERENCE_SOFTWARE:
        return "software";
    case AUDIO_ECHO_CANCEL_REFERENCE_NONE:
    default:
        return "none";
    }
}

static void *audio_aec_aligned_calloc(size_t count, size_t element_size)
{
    void *buffer = app_memory_aligned_calloc_psram(AUDIO_AEC_BUFFER_ALIGNMENT,
                                                   count,
                                                   element_size,
                                                   0U);
    if (buffer == NULL) {
        ESP_LOGW(TAG,
                 "AEC PSRAM allocation failed: count=%u size=%u",
                 (unsigned)count,
                 (unsigned)element_size);
    }
    return buffer;
}

#if CONFIG_APP_VERBOSE_RUNTIME_LOGS
static void audio_aec_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "%s: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "heap",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
#endif

static aec_handle_t *audio_aec_create_handle(aec_mode_t mode,
                                              int filter_length,
                                              aec_nlp_level_t nlp_level)
{
    aec_config_t config = {
        .mic_num = 1,
        .ref_num = 1,
        .out_num = 1,
        .filter_length = filter_length,
        .sample_rate = (int)AUDIO_AEC_SAMPLE_RATE_HZ,
        .caps = AUDIO_AEC_PSRAM_CAPS,
        .mode = mode,
        .nlp_level = nlp_level,
    };
    aec_handle_t *handle = aec_create_from_config(&config);
    if (handle != NULL) {
        (void)aec_set_nlp_level(handle, nlp_level);
    }
    return handle;
}

static aec_nlp_level_t audio_aec_suppression_to_nlp(
    audio_echo_suppression_t suppression)
{
    return suppression == AUDIO_ECHO_SUPPRESSION_STRONG ?
           AEC_NLP_LEVEL_VERYAGGR : AEC_NLP_LEVEL_AGGR;
}

static void audio_aec_apply_requested_suppression(void)
{
    aec_nlp_level_t requested = AEC_NLP_LEVEL_AGGR;
    aec_nlp_level_t applied = AEC_NLP_LEVEL_AGGR;

    taskENTER_CRITICAL(&s_aec_lock);
    requested = s_requested_nlp_level;
    applied = s_applied_nlp_level;
    taskEXIT_CRITICAL(&s_aec_lock);
    if (requested == applied || s_aec == NULL) {
        return;
    }

    aec_nlp_level_t actual = aec_set_nlp_level(s_aec, requested);
    taskENTER_CRITICAL(&s_aec_lock);
    s_applied_nlp_level = actual;
    taskEXIT_CRITICAL(&s_aec_lock);
    APP_LOG_DETAIL(TAG,
                   "AEC suppression changed: requested=%s applied=%s",
                   aec_get_nlp_string(requested),
                   aec_get_nlp_string(actual));
}

static uint8_t audio_aec_reference_coherence_percent(const int16_t *mic,
                                                       const int16_t *reference,
                                                       uint32_t sample_count)
{
    uint32_t best_percent = 0U;

    if (mic == NULL || reference == NULL || sample_count == 0U) {
        return 0U;
    }

    for (int lag = -AUDIO_AEC_COHERENCE_MAX_LAG_SAMPLES;
         lag <= AUDIO_AEC_COHERENCE_MAX_LAG_SAMPLES;
         lag += AUDIO_AEC_COHERENCE_LAG_STEP) {
        uint32_t mic_start = lag < 0 ? (uint32_t)(-lag) : 0U;
        uint32_t ref_start = lag > 0 ? (uint32_t)lag : 0U;
        uint32_t start_offset = mic_start > ref_start ? mic_start : ref_start;
        int64_t cross = 0;
        uint64_t mic_energy = 0U;
        uint64_t ref_energy = 0U;

        if (sample_count <= start_offset) {
            continue;
        }
        uint32_t overlap = sample_count - start_offset;
        if (overlap <= AUDIO_AEC_COHERENCE_SAMPLE_STEP) {
            continue;
        }
        for (uint32_t index = 0U; index < overlap;
             index += AUDIO_AEC_COHERENCE_SAMPLE_STEP) {
            int32_t mic_sample = mic[mic_start + index] >> AUDIO_AEC_ANALYSIS_SHIFT;
            int32_t ref_sample = reference[ref_start + index] >> AUDIO_AEC_ANALYSIS_SHIFT;
            cross += (int64_t)mic_sample * ref_sample;
            mic_energy += (uint64_t)((int64_t)mic_sample * mic_sample);
            ref_energy += (uint64_t)((int64_t)ref_sample * ref_sample);
        }
        if (mic_energy == 0U || ref_energy == 0U) {
            continue;
        }

        uint64_t cross_abs = (uint64_t)(cross < 0 ? -cross : cross);
        uint64_t numerator = cross_abs * cross_abs * 100U;
        uint64_t denominator = mic_energy * ref_energy;
        uint32_t percent = denominator == 0U ? 0U :
                           (uint32_t)(numerator / denominator);
        if (percent > 100U) {
            percent = 100U;
        }
        if (percent > best_percent) {
            best_percent = percent;
        }
    }

    return (uint8_t)best_percent;
}

static void audio_aec_update_near_end_state(bool reference_active,
                                             const int16_t *mic,
                                             const int16_t *reference,
                                             const int16_t *output,
                                             uint32_t sample_count)
{
    if (!reference_active || mic == NULL || reference == NULL || output == NULL) {
        s_reference_active_frames = 0U;
        s_near_end_candidate_frames = 0U;
        s_near_end_hangover_frames = 0U;
        s_last_near_end_detected = false;
        return;
    }

    uint64_t mic_energy = 0U;
    uint64_t output_energy = 0U;
    uint32_t output_peak = 0U;
    for (uint32_t index = 0U; index < sample_count; ++index) {
        int32_t mic_sample = mic[index] >> AUDIO_AEC_ANALYSIS_SHIFT;
        int32_t output_sample = output[index] >> AUDIO_AEC_ANALYSIS_SHIFT;
        uint32_t output_abs = audio_aec_abs_i16(output[index]);
        mic_energy += (uint64_t)((int64_t)mic_sample * mic_sample);
        output_energy += (uint64_t)((int64_t)output_sample * output_sample);
        if (output_abs > output_peak) {
            output_peak = output_abs;
        }
    }

    if (s_reference_active_frames < UINT32_MAX) {
        s_reference_active_frames++;
    }
    uint64_t retained_percent = mic_energy == 0U ? 0U :
                                (output_energy * 100U) / mic_energy;
    uint8_t coherence_percent =
        audio_aec_reference_coherence_percent(mic, reference, sample_count);
    bool candidate =
        s_reference_active_frames >= AUDIO_AEC_NEAR_END_ADAPT_FRAMES &&
        output_peak >= AUDIO_AEC_NEAR_END_MIN_OUT_PEAK &&
        retained_percent >= AUDIO_AEC_NEAR_END_MIN_RETAINED_PERCENT &&
        coherence_percent <= AUDIO_AEC_NEAR_END_MAX_COHERENCE_PERCENT;

    if (candidate) {
        if (s_near_end_candidate_frames < AUDIO_AEC_NEAR_END_CONFIRM_FRAMES) {
            s_near_end_candidate_frames++;
        }
    } else {
        s_near_end_candidate_frames = 0U;
    }
    if (s_near_end_candidate_frames >= AUDIO_AEC_NEAR_END_CONFIRM_FRAMES) {
        s_near_end_hangover_frames = AUDIO_AEC_NEAR_END_HANGOVER_FRAMES;
    } else if (s_near_end_hangover_frames > 0U) {
        s_near_end_hangover_frames--;
    }
    s_last_near_end_detected = s_near_end_hangover_frames > 0U;
}

static void audio_aec_reset_state_locked(void)
{
    s_ref_write_pos = 0;
    s_ref_filled_samples = 0;
    s_ref_read_pos = 0;
    s_ref_resync_count = 0;
    s_last_ref_peak = 0;
    s_last_playback_us = 0;
    s_frame_fill = 0;
    s_convergence_processed_frames = 0;
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_ref_read_ready = false;
    s_out_fifo_ready = false;
    s_first_processed_logged = false;
    s_warmup_guard_logged = false;
    s_codec_reference_detected = false;
    s_codec_reference_rejected = false;
    s_codec_reference_probe_frames = 0;
    s_reference_source = AUDIO_ECHO_CANCEL_REFERENCE_NONE;
    s_process_frames = 0;
    s_process_us_total = 0;
    s_process_us_max = 0;
    s_reference_active_frames = 0;
    s_near_end_candidate_frames = 0;
    s_near_end_hangover_frames = 0;
    s_last_near_end_detected = false;
}

static void audio_aec_free_handle_and_buffers(aec_handle_t *handle,
                                              int16_t *ref_ring,
                                              int16_t *mic_frame,
                                              int16_t *ref_frame,
                                              int16_t *out_frame,
                                              int16_t *analysis_mic_frame,
                                              int16_t *analysis_ref_frame,
                                              int16_t *out_fifo)
{
    if (handle != NULL) {
        aec_destroy(handle);
    }
    heap_caps_free(ref_ring);
    heap_caps_free(mic_frame);
    heap_caps_free(ref_frame);
    heap_caps_free(out_frame);
    heap_caps_free(analysis_mic_frame);
    heap_caps_free(analysis_ref_frame);
    heap_caps_free(out_fifo);
}

static bool audio_aec_ensure_ready(void)
{
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_aec != NULL) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return true;
    }
    if (s_initializing || s_deinit_requested) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return false;
    }
    s_initializing = true;
    taskEXIT_CRITICAL(&s_aec_lock);

    aec_nlp_level_t requested_nlp = AEC_NLP_LEVEL_AGGR;
    taskENTER_CRITICAL(&s_aec_lock);
    requested_nlp = s_requested_nlp_level;
    taskEXIT_CRITICAL(&s_aec_lock);

    aec_handle_t *handle = audio_aec_create_handle(AEC_MODE_FD_HIGH_PERF,
                                                    AUDIO_AEC_FILTER_LENGTH,
                                                    requested_nlp);
    if (handle == NULL) {
        handle = audio_aec_create_handle(AEC_MODE_FD_LOW_COST,
                                         AUDIO_AEC_FILTER_LENGTH,
                                         requested_nlp);
    }

    int frame_size = handle == NULL ? 0 : aec_get_chunksize(handle);
    int16_t *ref_ring = NULL;
    int16_t *mic_frame = NULL;
    int16_t *ref_frame = NULL;
    int16_t *out_frame = NULL;
    int16_t *analysis_mic_frame = NULL;
    int16_t *analysis_ref_frame = NULL;
    int16_t *out_fifo = NULL;

    if (handle != NULL && frame_size > 0) {
        ref_ring = audio_aec_aligned_calloc(AUDIO_AEC_REF_RING_SAMPLES, sizeof(int16_t));
        mic_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        ref_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        out_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        analysis_mic_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        analysis_ref_frame = audio_aec_aligned_calloc((size_t)frame_size, sizeof(int16_t));
        out_fifo = audio_aec_aligned_calloc(AUDIO_AEC_OUT_FIFO_SAMPLES, sizeof(int16_t));
    }

    if (handle == NULL || frame_size <= 0 || ref_ring == NULL || mic_frame == NULL ||
        ref_frame == NULL || out_frame == NULL || analysis_mic_frame == NULL ||
        analysis_ref_frame == NULL || out_fifo == NULL) {
        audio_aec_free_handle_and_buffers(handle,
                                          ref_ring,
                                          mic_frame,
                                          ref_frame,
                                          out_frame,
                                          analysis_mic_frame,
                                          analysis_ref_frame,
                                          out_fifo);
        taskENTER_CRITICAL(&s_aec_lock);
        s_initializing = false;
        taskEXIT_CRITICAL(&s_aec_lock);
        if (!s_create_failed_logged) {
            s_create_failed_logged = true;
            ESP_LOGW(TAG, "official ESP-SR AEC unavailable, capture continues without AEC");
        }
        return false;
    }

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_deinit_requested) {
        s_initializing = false;
        taskEXIT_CRITICAL(&s_aec_lock);
        audio_aec_free_handle_and_buffers(handle,
                                          ref_ring,
                                          mic_frame,
                                          ref_frame,
                                          out_frame,
                                          analysis_mic_frame,
                                          analysis_ref_frame,
                                          out_fifo);
        return false;
    }
    s_aec = handle;
    s_aec_frame_size = frame_size;
    s_ref_ring = ref_ring;
    s_mic_frame = mic_frame;
    s_ref_frame = ref_frame;
    s_out_frame = out_frame;
    s_analysis_mic_frame = analysis_mic_frame;
    s_analysis_ref_frame = analysis_ref_frame;
    s_out_fifo = out_fifo;
    s_applied_nlp_level = handle->config.nlp_level;
    audio_aec_reset_state_locked();
    s_runtime_active = false;
    s_initializing = false;
    taskEXIT_CRITICAL(&s_aec_lock);

    APP_LOG_DETAIL(TAG,
                   "official ESP-SR AEC ready: profile=%s mode=%s nlp=%s frame=%d filter=%d codec_ref=preferred fallback_delay=%ums psram=%uB",
                   AUDIO_AEC_PROFILE_NAME,
                   aec_get_mode_string(handle->config.mode),
                   aec_get_nlp_string(handle->config.nlp_level),
                   frame_size,
                   handle->config.filter_length,
                   (unsigned)AUDIO_AEC_REF_DELAY_MS,
                   (unsigned)((AUDIO_AEC_REF_RING_SAMPLES + AUDIO_AEC_OUT_FIFO_SAMPLES +
                               (uint32_t)frame_size * 5U) * sizeof(int16_t)));
#if CONFIG_APP_VERBOSE_RUNTIME_LOGS
    audio_aec_log_heap("AEC ready");
#endif
    return true;
}

static bool audio_aec_enter(void)
{
    bool ready = false;

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_aec != NULL && s_runtime_active && !s_deinit_requested) {
        s_active_users++;
        ready = true;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    return ready;
}

static void audio_aec_leave(void)
{
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_active_users > 0U) {
        s_active_users--;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
}

static audio_aec_ref_state_t audio_aec_ref_snapshot(size_t sample_count,
                                                     int16_t **ring,
                                                     uint32_t *read_start,
                                                    uint32_t *ref_peak,
                                                    bool *resynced)
{
    int16_t *ref_ring = NULL;
    uint32_t write_pos = 0;
    uint32_t filled_samples = 0;
    uint32_t last_ref_peak = 0;
    int64_t last_playback_us = 0;
    bool read_ready = false;
    uint32_t read_pos = 0;
    const uint32_t requested_samples = (uint32_t)sample_count;
    const uint32_t desired_distance = AUDIO_AEC_REF_DELAY_SAMPLES + requested_samples;
    const int64_t now_us = esp_timer_get_time();
    audio_aec_ref_state_t state = AUDIO_AEC_REF_INACTIVE;

    taskENTER_CRITICAL(&s_aec_lock);
    ref_ring = s_ref_ring;
    write_pos = s_ref_write_pos;
    filled_samples = s_ref_filled_samples;
    last_ref_peak = s_last_ref_peak;
    last_playback_us = s_last_playback_us;
    read_ready = s_ref_read_ready;
    read_pos = s_ref_read_pos;

    if (ref_peak != NULL) {
        *ref_peak = last_ref_peak;
    }
    if (resynced != NULL) {
        *resynced = false;
    }

    if (ref_ring == NULL ||
        last_ref_peak < AUDIO_AEC_REF_ACTIVE_PEAK ||
        now_us - last_playback_us > AUDIO_AEC_REF_ACTIVE_US) {
        s_ref_read_ready = false;
        state = AUDIO_AEC_REF_INACTIVE;
    } else if (filled_samples < desired_distance) {
        state = AUDIO_AEC_REF_WARMING;
    } else {
        if (!read_ready) {
            read_pos = audio_aec_ring_prev(write_pos, desired_distance);
            s_ref_read_ready = true;
        } else {
            uint32_t available = audio_aec_ring_distance(read_pos, write_pos);
            uint32_t max_distance = desired_distance + AUDIO_AEC_REF_RESYNC_TOLERANCE_SAMPLES;

            if (available < requested_samples || available > max_distance) {
                /*
                 * Keep the M/R sample pairing continuous across capture calls.
                 * Only rebase after a true underrun or a large scheduling gap;
                 * following the latest write pointer on every 20 ms block
                 * repeats/skips reference samples inside ESP-SR's 32 ms frame.
                 */
                read_pos = audio_aec_ring_prev(write_pos, desired_distance);
                s_ref_resync_count++;
                if (resynced != NULL) {
                    *resynced = true;
                }
            }
        }

        *ring = ref_ring;
        *read_start = read_pos;
        s_ref_read_pos = (read_pos + requested_samples) & AUDIO_AEC_REF_RING_MASK;
        state = AUDIO_AEC_REF_READY;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    return state;
}

static bool audio_aec_codec_reference_snapshot(const int16_t *reference,
                                               size_t sample_count,
                                               uint32_t *ref_peak,
                                               bool *playback_active_out,
                                               bool *source_changed,
                                               bool *first_detected,
                                               bool *first_rejected)
{
    if (reference == NULL || sample_count == 0U) {
        return false;
    }

    uint32_t peak = 0U;
    for (size_t index = 0; index < sample_count; ++index) {
        uint32_t value = audio_aec_abs_i16(reference[index]);
        if (value > peak) {
            peak = value;
        }
    }

    const int64_t now_us = esp_timer_get_time();
    bool ready = false;
    bool detected_now = false;
    bool rejected_now = false;
    bool changed = false;
    bool playback_active = false;

    taskENTER_CRITICAL(&s_aec_lock);
    playback_active =
        s_last_ref_peak >= AUDIO_AEC_REF_ACTIVE_PEAK &&
        s_last_playback_us > 0 &&
        now_us - s_last_playback_us <= AUDIO_AEC_REF_ACTIVE_US;
    if (!s_codec_reference_rejected) {
        /*
         * Start processing the synchronous codec channel immediately so the
         * AEC and output FIFO are warm before the first far-end syllable.
         * Validate it only while known speaker PCM is active; if the board
         * does not expose DACR, fall back after a bounded 100 ms probe.
         */
        ready = true;
        if (playback_active && !s_codec_reference_detected) {
            if (peak >= AUDIO_AEC_CODEC_REF_DETECT_PEAK) {
                s_codec_reference_detected = true;
                s_codec_reference_probe_frames = 0;
                detected_now = true;
                /*
                 * Silent capture before the first playback can fill the AEC
                 * output FIFO without training the echo path. Treat the first
                 * real codec reference as a new reference epoch so capture is
                 * held only while the filter adapts to the loudspeaker path.
                 */
                changed = true;
            } else if (++s_codec_reference_probe_frames >= AUDIO_AEC_CODEC_REF_PROBE_FRAMES) {
                s_codec_reference_rejected = true;
                ready = false;
                rejected_now = true;
            }
        } else if (!playback_active && !s_codec_reference_detected) {
            s_codec_reference_probe_frames = 0;
        }
    }
    if (ready) {
        if (s_reference_source != AUDIO_ECHO_CANCEL_REFERENCE_CODEC) {
            s_reference_source = AUDIO_ECHO_CANCEL_REFERENCE_CODEC;
            changed = true;
        }
    }
    if (ref_peak != NULL) {
        *ref_peak = peak;
    }
    taskEXIT_CRITICAL(&s_aec_lock);

    if (source_changed != NULL) {
        *source_changed = changed;
    }
    if (playback_active_out != NULL) {
        *playback_active_out = playback_active;
    }
    if (first_detected != NULL) {
        *first_detected = detected_now;
    }
    if (first_rejected != NULL) {
        *first_rejected = rejected_now;
    }
    return ready;
}

static bool audio_aec_select_software_reference(audio_aec_ref_state_t state)
{
    if (state == AUDIO_AEC_REF_INACTIVE) {
        return false;
    }

    bool changed = false;
    taskENTER_CRITICAL(&s_aec_lock);
    if (s_reference_source != AUDIO_ECHO_CANCEL_REFERENCE_SOFTWARE) {
        s_reference_source = AUDIO_ECHO_CANCEL_REFERENCE_SOFTWARE;
        changed = true;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    return changed;
}

static void audio_aec_fifo_clear(void)
{
    s_out_fifo_read_pos = 0;
    s_out_fifo_used = 0;
    s_out_fifo_ready = false;
}

static esp_err_t audio_aec_wait_for_idle(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_AEC_DEINIT_WAIT_MS);

    while (true) {
        uint32_t active_users = 0;

        taskENTER_CRITICAL(&s_aec_lock);
        active_users = s_active_users;
        taskEXIT_CRITICAL(&s_aec_lock);
        if (active_users == 0U) {
            return ESP_OK;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGW(TAG, "AEC state transition timed out: users=%lu", (unsigned long)active_users);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void audio_aec_fifo_push(const int16_t *samples, uint32_t count)
{
    if (s_out_fifo == NULL || count == 0U) {
        return;
    }
    if (count > AUDIO_AEC_OUT_FIFO_SAMPLES) {
        samples += count - AUDIO_AEC_OUT_FIFO_SAMPLES;
        count = AUDIO_AEC_OUT_FIFO_SAMPLES;
    }
    if (count > AUDIO_AEC_OUT_FIFO_SAMPLES - s_out_fifo_used) {
        uint32_t drop = count - (AUDIO_AEC_OUT_FIFO_SAMPLES - s_out_fifo_used);
        s_out_fifo_read_pos = (s_out_fifo_read_pos + drop) & AUDIO_AEC_OUT_FIFO_MASK;
        s_out_fifo_used -= drop;
    }

    uint32_t write_pos = (s_out_fifo_read_pos + s_out_fifo_used) & AUDIO_AEC_OUT_FIFO_MASK;
    for (uint32_t index = 0; index < count; ++index) {
        s_out_fifo[write_pos] = samples[index];
        write_pos = (write_pos + 1U) & AUDIO_AEC_OUT_FIFO_MASK;
    }
    s_out_fifo_used += count;
}

static bool audio_aec_fifo_pop(int16_t *samples, uint32_t count, uint32_t *out_peak)
{
    if (s_out_fifo == NULL || samples == NULL || count == 0U) {
        return false;
    }

    if (!s_out_fifo_ready) {
        uint32_t prefill = count + (uint32_t)(s_aec_frame_size > 0 ? s_aec_frame_size : 0);
        if (s_out_fifo_used < prefill) {
            return false;
        }
        s_out_fifo_ready = true;
    }
    if (s_out_fifo_used < count) {
        s_out_fifo_ready = false;
        return false;
    }

    uint32_t peak = 0;
    for (uint32_t index = 0; index < count; ++index) {
        int16_t sample = s_out_fifo[s_out_fifo_read_pos];
        samples[index] = sample;
        uint32_t abs_sample = audio_aec_abs_i16(sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        s_out_fifo_read_pos = (s_out_fifo_read_pos + 1U) & AUDIO_AEC_OUT_FIFO_MASK;
    }
    s_out_fifo_used -= count;
    if (out_peak != NULL) {
        *out_peak = peak;
    }
    return true;
}

static bool audio_aec_mute_capture_during_warmup(int16_t *samples,
                                                 size_t sample_count,
                                                 uint32_t ref_peak,
                                                 bool reference_active,
                                                 audio_echo_cancel_reference_t reference,
                                                 uint16_t delay_samples,
                                                 audio_echo_cancel_metrics_t *metrics)
{
    uint32_t mic_peak = 0U;

    for (size_t index = 0; index < sample_count; ++index) {
        uint32_t value = audio_aec_abs_i16(samples[index]);
        if (value > mic_peak) {
            mic_peak = value;
        }
    }
    memset(samples, 0, sample_count * sizeof(*samples));

    if (metrics != NULL) {
        metrics->active = true;
        metrics->warming_up = true;
        metrics->reference_active = reference_active;
        metrics->reference = reference;
        metrics->ref_peak = ref_peak;
        metrics->mic_peak = mic_peak;
        metrics->out_peak = 0U;
        metrics->delay_samples = delay_samples;
        metrics->suppress_percent = mic_peak > 0U ? 100U : 0U;
    }

    bool first_guard = false;
    taskENTER_CRITICAL(&s_aec_lock);
    if (!s_warmup_guard_logged) {
        s_warmup_guard_logged = true;
        first_guard = true;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    return first_guard;
}

esp_err_t audio_echo_cancel_prepare(void)
{
    return audio_aec_ensure_ready() ? ESP_OK : ESP_FAIL;
}

void audio_echo_cancel_set_suppression(audio_echo_suppression_t suppression)
{
    aec_nlp_level_t requested = audio_aec_suppression_to_nlp(suppression);

    taskENTER_CRITICAL(&s_aec_lock);
    s_requested_nlp_level = requested;
    taskEXIT_CRITICAL(&s_aec_lock);
}

esp_err_t audio_echo_cancel_set_active(bool active)
{
    bool changed = false;

    if (active && !audio_aec_ensure_ready()) {
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_aec_lock);
    if (active) {
        if (s_aec == NULL || s_deinit_requested) {
            taskEXIT_CRITICAL(&s_aec_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (!s_runtime_active) {
            audio_aec_reset_state_locked();
            s_runtime_active = true;
            changed = true;
        }
    } else if (s_runtime_active) {
        s_runtime_active = false;
        changed = true;
    }
    taskEXIT_CRITICAL(&s_aec_lock);

    if (!active) {
        esp_err_t ret = audio_aec_wait_for_idle();
        if (ret != ESP_OK) {
            return ret;
        }
        taskENTER_CRITICAL(&s_aec_lock);
        audio_aec_reset_state_locked();
        taskEXIT_CRITICAL(&s_aec_lock);
    }

    if (changed) {
        ESP_LOGI(TAG,
                 "AEC state: active=%d codec_ref=preferred fallback_delay=%ums",
                 active ? 1 : 0,
                 (unsigned)AUDIO_AEC_REF_DELAY_MS);
    }
    return ESP_OK;
}

void audio_echo_cancel_get_status(audio_echo_cancel_status_t *status)
{
    if (status == NULL) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    int64_t last_playback_us = 0;
    uint32_t last_ref_peak = 0;

    memset(status, 0, sizeof(*status));
    taskENTER_CRITICAL(&s_aec_lock);
    status->prepared = s_aec != NULL;
    status->active = s_runtime_active;
    status->reference = s_reference_source;
    status->process_frames = s_process_frames;
    status->process_us_total = s_process_us_total;
    status->process_us_max = s_process_us_max;
    last_playback_us = s_last_playback_us;
    last_ref_peak = s_last_ref_peak;
    taskEXIT_CRITICAL(&s_aec_lock);

    status->reference_active =
        status->active &&
        last_ref_peak >= AUDIO_AEC_REF_ACTIVE_PEAK &&
        last_playback_us > 0 &&
        now_us - last_playback_us <= AUDIO_AEC_REF_ACTIVE_US;
}

void audio_echo_cancel_feed_playback(const int16_t *samples,
                                     size_t sample_count,
                                     uint8_t channels)
{
    if (samples == NULL || sample_count == 0U || channels == 0U) {
        return;
    }
    if (!audio_aec_enter()) {
        return;
    }

    int16_t *ref_ring = NULL;
    uint32_t write_pos = 0;
    uint32_t filled_samples = 0;

    taskENTER_CRITICAL(&s_aec_lock);
    ref_ring = s_ref_ring;
    write_pos = s_ref_write_pos;
    filled_samples = s_ref_filled_samples;
    taskEXIT_CRITICAL(&s_aec_lock);

    if (ref_ring == NULL) {
        audio_aec_leave();
        return;
    }

    const size_t frame_count = sample_count / channels;
    uint32_t peak = 0;
    for (size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        int32_t mono = 0;
        const size_t base = frame_index * channels;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mono += samples[base + channel];
        }
        mono /= channels;

        int16_t sample = (int16_t)mono;
        uint32_t abs_sample = audio_aec_abs_i16(sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }

        ref_ring[write_pos] = sample;
        write_pos = (write_pos + 1U) & AUDIO_AEC_REF_RING_MASK;
        if (filled_samples < AUDIO_AEC_REF_RING_SAMPLES) {
            filled_samples++;
        }
    }

    int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_aec_lock);
    s_ref_write_pos = write_pos;
    s_ref_filled_samples = filled_samples;
    if (peak >= AUDIO_AEC_REF_ACTIVE_PEAK) {
        s_last_ref_peak = peak;
        s_last_playback_us = now_us;
    } else if (s_last_playback_us == 0 ||
               now_us - s_last_playback_us > AUDIO_AEC_REF_ACTIVE_US) {
        /*
         * Keep the last meaningful reference level across short pauses in
         * speech. Replacing it with every zero/quiet PCM chunk disables AEC
         * while the loudspeaker's acoustic tail is still reaching the mic.
         */
        s_last_ref_peak = peak;
    }
    taskEXIT_CRITICAL(&s_aec_lock);
    audio_aec_leave();
}

void audio_echo_cancel_process_capture(int16_t *samples,
                                       size_t sample_count,
                                       audio_echo_cancel_metrics_t *metrics)
{
    audio_echo_cancel_process_capture_with_reference(samples, NULL, sample_count, metrics);
}

void audio_echo_cancel_process_capture_with_reference(
    int16_t *samples,
    const int16_t *codec_reference,
    size_t sample_count,
    audio_echo_cancel_metrics_t *metrics)
{
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
    if (samples == NULL || sample_count == 0U) {
        return;
    }
    if (!audio_aec_enter()) {
        return;
    }
    audio_aec_apply_requested_suppression();

    int16_t *ref_ring = NULL;
    uint32_t read_start = 0;
    uint32_t ref_peak = 0;
    bool ref_resynced = false;
    bool playback_reference_active = false;
    bool source_changed = false;
    bool first_codec_reference = false;
    bool codec_reference_rejected = false;
    audio_echo_cancel_reference_t reference = AUDIO_ECHO_CANCEL_REFERENCE_SOFTWARE;
    audio_aec_ref_state_t ref_state = AUDIO_AEC_REF_INACTIVE;

    if (audio_aec_codec_reference_snapshot(codec_reference,
                                           sample_count,
                                           &ref_peak,
                                           &playback_reference_active,
                                           &source_changed,
                                           &first_codec_reference,
                                           &codec_reference_rejected)) {
        reference = AUDIO_ECHO_CANCEL_REFERENCE_CODEC;
        ref_state = AUDIO_AEC_REF_READY;
    } else {
        ref_state = audio_aec_ref_snapshot(sample_count,
                                           &ref_ring,
                                           &read_start,
                                           &ref_peak,
                                           &ref_resynced);
        playback_reference_active = ref_state != AUDIO_AEC_REF_INACTIVE;
        if (audio_aec_select_software_reference(ref_state)) {
            source_changed = true;
        }
    }

    if (source_changed) {
        s_frame_fill = 0;
        s_convergence_processed_frames = 0;
        audio_aec_fifo_clear();
    }
    if (codec_reference_rejected) {
        const uint32_t probe_ms =
            (uint32_t)(((uint64_t)AUDIO_AEC_CODEC_REF_PROBE_FRAMES *
                        sample_count * 1000U) /
                       AUDIO_AEC_SAMPLE_RATE_HZ);
        ESP_LOGW(TAG,
                 "AEC codec reference unavailable after %ums probe; using %ums software fallback",
                 (unsigned)probe_ms,
                 (unsigned)AUDIO_AEC_REF_DELAY_MS);
    }

    const uint16_t delay_samples =
        reference == AUDIO_ECHO_CANCEL_REFERENCE_CODEC ? 0U :
        (uint16_t)AUDIO_AEC_REF_DELAY_SAMPLES;
    if (ref_state != AUDIO_AEC_REF_READY) {
        s_frame_fill = 0;
        audio_aec_fifo_clear();
        bool log_warmup_guard = false;
        if (ref_state == AUDIO_AEC_REF_WARMING) {
            log_warmup_guard =
                audio_aec_mute_capture_during_warmup(samples,
                                                     sample_count,
                                                     ref_peak,
                                                     playback_reference_active,
                                                     reference,
                                                     delay_samples,
                                                     metrics);
        }
        audio_aec_leave();
        if (first_codec_reference) {
            ESP_LOGI(TAG,
                     "AEC codec reference locked: channel=right peak=%u software_delay_fallback=%ums",
                     (unsigned)ref_peak,
                     (unsigned)AUDIO_AEC_REF_DELAY_MS);
        }
        if (log_warmup_guard) {
            APP_LOG_DETAIL(TAG,
                           "AEC warmup guard active: source=%s capture muted until reference and output FIFO are ready",
                           audio_aec_reference_name(reference));
        }
        return;
    }
    if (ref_resynced) {
        s_frame_fill = 0;
        audio_aec_fifo_clear();
    }

    uint32_t mic_peak = 0;
    uint32_t process_us = 0;
    uint32_t process_frames = 0;
    uint32_t process_max_us = 0;
    bool log_convergence_ready = false;
    const uint32_t convergence_frames =
        reference == AUDIO_ECHO_CANCEL_REFERENCE_CODEC ?
        AUDIO_AEC_CODEC_CONVERGENCE_FRAMES :
        AUDIO_AEC_SOFTWARE_CONVERGENCE_FRAMES;
    for (size_t index = 0; index < sample_count; ++index) {
        int16_t mic_sample = samples[index];
        int16_t ref_sample =
            reference == AUDIO_ECHO_CANCEL_REFERENCE_CODEC ?
            codec_reference[index] :
            ref_ring[(read_start + (uint32_t)index) & AUDIO_AEC_REF_RING_MASK];
        uint32_t mic_abs = audio_aec_abs_i16(mic_sample);
        if (mic_abs > mic_peak) {
            mic_peak = mic_abs;
        }

        s_mic_frame[s_frame_fill] = mic_sample;
        s_ref_frame[s_frame_fill] = ref_sample;
        s_frame_fill++;

        if (s_frame_fill >= (uint32_t)s_aec_frame_size) {
            /*
             * ESP-SR accepts writable input buffers and does not promise to
             * preserve them. Keep an immutable copy for double-talk analysis;
             * otherwise the detector can classify samples already changed by
             * aec_process() instead of the real aligned microphone/reference.
             */
            memcpy(s_analysis_mic_frame,
                   s_mic_frame,
                   (size_t)s_aec_frame_size * sizeof(s_analysis_mic_frame[0]));
            memcpy(s_analysis_ref_frame,
                   s_ref_frame,
                   (size_t)s_aec_frame_size * sizeof(s_analysis_ref_frame[0]));
            int64_t process_start_us = esp_timer_get_time();
            aec_process(s_aec, s_mic_frame, s_ref_frame, s_out_frame);
            audio_aec_update_near_end_state(playback_reference_active,
                                            s_analysis_mic_frame,
                                            s_analysis_ref_frame,
                                            s_out_frame,
                                            (uint32_t)s_aec_frame_size);
            int64_t elapsed_us = esp_timer_get_time() - process_start_us;
            if (elapsed_us > 0) {
                uint32_t elapsed_us_u32 =
                    elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
                process_us += elapsed_us_u32;
                process_frames++;
                if (elapsed_us_u32 > process_max_us) {
                    process_max_us = elapsed_us_u32;
                }
            }
            if (s_convergence_processed_frames < convergence_frames) {
                s_convergence_processed_frames++;
                if (s_convergence_processed_frames == convergence_frames) {
                    log_convergence_ready = true;
                }
            } else {
                audio_aec_fifo_push(s_out_frame, (uint32_t)s_aec_frame_size);
            }
            s_frame_fill = 0;
        }
    }

    if (process_frames > 0U) {
        taskENTER_CRITICAL(&s_aec_lock);
        s_process_frames += process_frames;
        s_process_us_total += process_us;
        if (process_max_us > s_process_us_max) {
            s_process_us_max = process_max_us;
        }
        taskEXIT_CRITICAL(&s_aec_lock);
    }

    uint32_t out_peak = 0;
    bool processed = audio_aec_fifo_pop(samples, (uint32_t)sample_count, &out_peak);
    bool log_first_processed = false;
    bool log_warmup_guard = false;
    if (metrics != NULL && processed) {
        metrics->active = true;
        metrics->reference_active = playback_reference_active;
        metrics->near_end_detected = s_last_near_end_detected;
        metrics->reference = reference;
        metrics->ref_peak = ref_peak;
        metrics->mic_peak = mic_peak;
        metrics->out_peak = out_peak;
        metrics->delay_samples = delay_samples;
        metrics->process_us = process_us;
        if (mic_peak > 0U && out_peak < mic_peak) {
            metrics->suppress_percent = (uint8_t)(((uint64_t)(mic_peak - out_peak) * 100U) / mic_peak);
        }
    }
    if (!processed) {
        log_warmup_guard =
            audio_aec_mute_capture_during_warmup(samples,
                                                 sample_count,
                                                 ref_peak,
                                                 playback_reference_active,
                                                 reference,
                                                 delay_samples,
                                                 metrics);
    }
    if (processed) {
        taskENTER_CRITICAL(&s_aec_lock);
        if (!s_first_processed_logged) {
            s_first_processed_logged = true;
            log_first_processed = true;
        }
        taskEXIT_CRITICAL(&s_aec_lock);
    }
    audio_aec_leave();
    if (first_codec_reference) {
        ESP_LOGI(TAG,
                 "AEC codec reference locked: channel=right peak=%u software_delay_fallback=%ums",
                 (unsigned)ref_peak,
                 (unsigned)AUDIO_AEC_REF_DELAY_MS);
    }
    if (log_warmup_guard) {
        APP_LOG_DETAIL(TAG,
                       "AEC warmup guard active: source=%s capture muted until reference and output FIFO are ready",
                       audio_aec_reference_name(reference));
    }
    if (log_convergence_ready) {
        APP_LOG_DETAIL(TAG,
                       "AEC convergence guard complete: source=%s frames=%u muted_ms=%u ref_resyncs=%u",
                       audio_aec_reference_name(reference),
                       (unsigned)convergence_frames,
                       (unsigned)(convergence_frames * 32U),
                       (unsigned)s_ref_resync_count);
    }
    if (log_first_processed) {
        uint32_t suppress_percent = 0U;
        if (mic_peak > 0U && out_peak < mic_peak) {
            suppress_percent =
                (uint32_t)(((uint64_t)(mic_peak - out_peak) * 100U) / mic_peak);
        }
        ESP_LOGI(TAG,
                 "AEC uplink ready: source=%s delay=%ums ref_peak=%u mic_peak=%u out_peak=%u suppress=%u%% process=%uus",
                 audio_aec_reference_name(reference),
                 (unsigned)((uint32_t)delay_samples * 1000U / AUDIO_AEC_SAMPLE_RATE_HZ),
                 (unsigned)ref_peak,
                 (unsigned)mic_peak,
                 (unsigned)out_peak,
                 (unsigned)suppress_percent,
                 (unsigned)process_us);
    }
}

void audio_echo_cancel_reset(void)
{
    (void)audio_echo_cancel_set_active(false);
}

void audio_echo_cancel_deinit(void)
{
    aec_handle_t *handle = NULL;
    int16_t *ref_ring = NULL;
    int16_t *mic_frame = NULL;
    int16_t *ref_frame = NULL;
    int16_t *out_frame = NULL;
    int16_t *analysis_mic_frame = NULL;
    int16_t *analysis_ref_frame = NULL;
    int16_t *out_fifo = NULL;

    (void)audio_echo_cancel_set_active(false);

    taskENTER_CRITICAL(&s_aec_lock);
    if (s_aec == NULL && !s_initializing) {
        taskEXIT_CRITICAL(&s_aec_lock);
        return;
    }
    s_deinit_requested = true;
    taskEXIT_CRITICAL(&s_aec_lock);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_AEC_DEINIT_WAIT_MS);
    while (true) {
        bool idle = false;

        taskENTER_CRITICAL(&s_aec_lock);
        idle = !s_initializing && s_active_users == 0U;
        taskEXIT_CRITICAL(&s_aec_lock);
        if (idle) {
            break;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            uint32_t active_users = 0;
            bool initializing = false;

            taskENTER_CRITICAL(&s_aec_lock);
            active_users = s_active_users;
            initializing = s_initializing;
            s_deinit_requested = false;
            taskEXIT_CRITICAL(&s_aec_lock);
            ESP_LOGW(TAG, "AEC deinit skipped: busy users=%lu initializing=%d",
                     (unsigned long)active_users,
                     initializing ? 1 : 0);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    taskENTER_CRITICAL(&s_aec_lock);
    handle = s_aec;
    ref_ring = s_ref_ring;
    mic_frame = s_mic_frame;
    ref_frame = s_ref_frame;
    out_frame = s_out_frame;
    analysis_mic_frame = s_analysis_mic_frame;
    analysis_ref_frame = s_analysis_ref_frame;
    out_fifo = s_out_fifo;
    s_aec = NULL;
    s_aec_frame_size = 0;
    s_ref_ring = NULL;
    s_mic_frame = NULL;
    s_ref_frame = NULL;
    s_out_frame = NULL;
    s_analysis_mic_frame = NULL;
    s_analysis_ref_frame = NULL;
    s_out_fifo = NULL;
    audio_aec_reset_state_locked();
    s_runtime_active = false;
    s_deinit_requested = false;
    s_create_failed_logged = false;
    taskEXIT_CRITICAL(&s_aec_lock);

    audio_aec_free_handle_and_buffers(handle,
                                      ref_ring,
                                      mic_frame,
                                      ref_frame,
                                      out_frame,
                                      analysis_mic_frame,
                                      analysis_ref_frame,
                                      out_fifo);
    if (handle != NULL || ref_ring != NULL || mic_frame != NULL || ref_frame != NULL ||
        out_frame != NULL || analysis_mic_frame != NULL ||
        analysis_ref_frame != NULL || out_fifo != NULL) {
        APP_LOG_DETAIL(TAG, "official ESP-SR AEC released");
#if CONFIG_APP_VERBOSE_RUNTIME_LOGS
        audio_aec_log_heap("AEC released");
#endif
    }
}

#else

esp_err_t audio_echo_cancel_prepare(void)
{
    return ESP_OK;
}

esp_err_t audio_echo_cancel_set_active(bool active)
{
    (void)active;
    return ESP_OK;
}

void audio_echo_cancel_set_suppression(audio_echo_suppression_t suppression)
{
    (void)suppression;
}

void audio_echo_cancel_get_status(audio_echo_cancel_status_t *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

void audio_echo_cancel_feed_playback(const int16_t *samples,
                                     size_t sample_count,
                                     uint8_t channels)
{
    (void)samples;
    (void)sample_count;
    (void)channels;
}

void audio_echo_cancel_process_capture(int16_t *samples,
                                       size_t sample_count,
                                       audio_echo_cancel_metrics_t *metrics)
{
    audio_echo_cancel_process_capture_with_reference(samples, NULL, sample_count, metrics);
}

void audio_echo_cancel_process_capture_with_reference(
    int16_t *samples,
    const int16_t *codec_reference,
    size_t sample_count,
    audio_echo_cancel_metrics_t *metrics)
{
    (void)samples;
    (void)codec_reference;
    (void)sample_count;
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

void audio_echo_cancel_reset(void)
{
}

void audio_echo_cancel_deinit(void)
{
}

#endif
