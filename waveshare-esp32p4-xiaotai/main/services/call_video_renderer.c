#include "call_video_renderer.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "driver/jpeg_decode.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"
#include "app_task_affinity.h"
#include "media_dma_reserve.h"
#include "video_frame_converter.h"

#ifndef CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
#define CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS 0
#endif
#ifndef CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
#define CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG 0
#endif

static const char *TAG = "call_video";

#define CALL_VIDEO_DECODED_SLOT_CAPACITY  \
    (CALL_VIDEO_SOURCE_CROP_WIDTH * CALL_VIDEO_SOURCE_CROP_HEIGHT * 3U / 2U)
#define CALL_VIDEO_OUTPUT_SLOT_INVALID    UINT8_MAX
#define CALL_VIDEO_H264_BASELINE_PROFILE_IDC 66U
#define CALL_VIDEO_H264_CONSTRAINT_SET1_FLAG 0x40U
#define CALL_VIDEO_FRAME_PIXELS           (CALL_VIDEO_RENDER_WIDTH * CALL_VIDEO_RENDER_HEIGHT)
#define CALL_VIDEO_FRAME_BYTES            (CALL_VIDEO_FRAME_PIXELS * sizeof(uint16_t))
#define CALL_VIDEO_MJPEG_DIMENSION_ALIGNMENT 16U
#define CALL_VIDEO_MJPEG_MAX_EDGE          640U
#define CALL_VIDEO_MJPEG_MAX_PIXELS        (640U * 480U)
#define CALL_VIDEO_MJPEG_DECODE_BYTES      \
    (CALL_VIDEO_MJPEG_MAX_PIXELS * sizeof(uint16_t))
#define CALL_VIDEO_MJPEG_PPA_STAGING_BYTES \
    (CALL_VIDEO_FRAME_PIXELS * 3U / 2U)
#define CALL_VIDEO_MJPEG_FIT_MODE           VIDEO_FRAME_FIT_COVER
#define CALL_VIDEO_MJPEG_PREVENT_UPSCALE    false
#define CALL_VIDEO_MJPEG_STARTUP_SAMPLE_US  (1LL * 1000LL * 1000LL)
#define CALL_VIDEO_MJPEG_GEOMETRY_LOG_LIMIT 4U
#define CALL_VIDEO_MEMORY_TRACE_DECODE_BEGIN  (1U << 0)
#define CALL_VIDEO_MEMORY_TRACE_DECODE_OUTPUT (1U << 1)
#define CALL_VIDEO_MEMORY_TRACE_DECODE_QUEUE  (1U << 2)
#define CALL_VIDEO_MEMORY_TRACE_CONVERT       (1U << 3)
#define CALL_VIDEO_MEMORY_TRACE_PRESENT       (1U << 4)
#define CALL_VIDEO_PSRAM_POOL_BYTES        \
    ((CALL_VIDEO_INPUT_SLOT_COUNT * CALL_VIDEO_INPUT_SLOT_CAPACITY) + \
     (CALL_VIDEO_DECODED_SLOT_COUNT * CALL_VIDEO_DECODED_SLOT_CAPACITY) + \
     (CALL_VIDEO_OUTPUT_SLOT_COUNT * CALL_VIDEO_FRAME_BYTES) + \
     CALL_VIDEO_MJPEG_DECODE_BYTES + CALL_VIDEO_MJPEG_PPA_STAGING_BYTES)

_Static_assert(CALL_VIDEO_SOURCE_CROP_X + CALL_VIDEO_SOURCE_CROP_WIDTH <=
                   CALL_VIDEO_DECODE_MAX_WIDTH,
               "landscape downlink crop must fit the decoder width");
_Static_assert(CALL_VIDEO_SOURCE_CROP_Y + CALL_VIDEO_SOURCE_CROP_HEIGHT <=
                   CALL_VIDEO_DECODE_MAX_HEIGHT,
               "landscape downlink crop must fit the decoder height");
_Static_assert(CALL_VIDEO_RENDER_WIDTH * CALL_VIDEO_SOURCE_CROP_HEIGHT ==
                   CALL_VIDEO_RENDER_HEIGHT * CALL_VIDEO_SOURCE_CROP_WIDTH,
               "landscape downlink source and viewport must share one aspect ratio");
_Static_assert((CALL_VIDEO_RENDER_WIDTH % 16U) == 0U &&
                   (CALL_VIDEO_RENDER_HEIGHT % 16U) == 0U,
               "hardware JPEG output dimensions must be aligned to 16 pixels");
_Static_assert(CALL_VIDEO_ADAPTIVE_PLAYOUT_DEPTH < CALL_VIDEO_OUTPUT_SLOT_COUNT,
               "adaptive playout must leave one RGB slot retained by the LCD");
_Static_assert(CALL_VIDEO_ADAPTIVE_PLAYOUT_MAX_DEPTH >=
                   CALL_VIDEO_ADAPTIVE_PLAYOUT_DEPTH &&
                   CALL_VIDEO_ADAPTIVE_PLAYOUT_MAX_DEPTH <
                       CALL_VIDEO_OUTPUT_SLOT_COUNT,
               "adaptive playout depth must fit the RGB reservoir");

#if CALL_VIDEO_H264_DUAL_TASK_ENABLE
#define CALL_VIDEO_H264_DECODER_MODE       "dual-task"
#else
#define CALL_VIDEO_H264_DECODER_MODE       "single-task"
#endif

#if !CONFIG_FREERTOS_UNICORE && CALL_VIDEO_H264_DUAL_TASK_ENABLE
_Static_assert(APP_TASK_CORE_VIDEO_DECODE != CALL_VIDEO_H264_HELPER_TASK_CORE,
               "TinyH264 helper must run on the core opposite the decoder caller");
_Static_assert(CALL_VIDEO_H264_HELPER_TASK_PRIORITY > 0U &&
                   CALL_VIDEO_H264_HELPER_TASK_PRIORITY < configMAX_PRIORITIES,
               "TinyH264 helper priority must be a valid FreeRTOS priority");
_Static_assert(CALL_VIDEO_H264_HELPER_TASK_PRIORITY > CALL_VIDEO_TASK_PRIORITY,
               "TinyH264 helper must outrank its synchronously waiting caller");
_Static_assert(CALL_VIDEO_H264_HELPER_TASK_PRIORITY >=
                   APP_TASK_PRIORITY_AUDIO_PLAYBACK,
               "TinyH264 helper must not be starved by continuous audio playback");
_Static_assert(CALL_VIDEO_H264_HELPER_TASK_PRIORITY <
                   APP_TASK_PRIORITY_AUDIO_CAPTURE,
               "audio capture must remain the highest-priority media deadline");
#endif

_Static_assert(CALL_VIDEO_INGRESS_TASK_PRIORITY < CALL_VIDEO_TASK_PRIORITY,
               "compressed ingress relay must not preempt the RTC callback");

#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CALL_VIDEO_CACHE_LINE_SIZE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define CALL_VIDEO_CACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif

_Static_assert((CALL_VIDEO_MJPEG_DECODE_BYTES % CALL_VIDEO_CACHE_LINE_SIZE) == 0U,
               "MJPEG decode buffer must be cache-line aligned");
_Static_assert(CALL_VIDEO_MJPEG_MAX_PIXELS >= CALL_VIDEO_FRAME_PIXELS,
               "MJPEG decoder pool must cover the display viewport");

typedef struct {
    uint8_t *data;
    size_t data_len;
    bool key_frame;
    bool decoder_bootstrap;
    uint32_t pts;
    uint32_t generation;
    int64_t queued_at_us;
    uint32_t trace_frame_index;
    int64_t trace_received_at_us;
    int64_t trace_decode_started_at_us;
} call_video_input_slot_t;

typedef struct {
    bool annexb;
    bool has_nal;
    bool has_sps;
    bool has_pps;
    bool has_idr;
    bool profile_valid;
    uint8_t profile_idc;
    uint8_t constraint_flags;
    uint8_t level_idc;
    uint32_t sps_hash;
    uint32_t pps_hash;
} call_video_h264_access_unit_t;

typedef struct {
    uint8_t *data;
    size_t data_len;
    uint16_t width;
    uint16_t height;
    uint32_t pts;
    uint32_t generation;
    call_video_frame_trace_t trace;
    int64_t trace_ready_at_us;
} call_video_decoded_slot_t;

typedef enum {
    CALL_VIDEO_OUTPUT_FREE = 0,
    CALL_VIDEO_OUTPUT_WRITING,
    CALL_VIDEO_OUTPUT_READY,
    CALL_VIDEO_OUTPUT_PRESENTED,
} call_video_output_state_t;

typedef struct {
    uint16_t *pixels;
    call_video_output_state_t state;
    uint32_t sequence;
    uint32_t pts;
    call_video_frame_trace_t trace;
    int64_t trace_ready_at_us;
} call_video_output_slot_t;

typedef struct {
    portMUX_TYPE lock;
    QueueHandle_t free_slots;
    QueueHandle_t ingress_slots;
    QueueHandle_t ready_slots;
    QueueHandle_t decoded_free_slots;
    QueueHandle_t decoded_ready_slots;
    SemaphoreHandle_t frame_mutex;
    SemaphoreHandle_t submit_mutex;
    SemaphoreHandle_t start_done;
    SemaphoreHandle_t stop_done;
    SemaphoreHandle_t ingress_stop_done;
    SemaphoreHandle_t convert_stop_done;
    TaskHandle_t task;
    TaskHandle_t ingress_task;
    TaskHandle_t convert_task;
    jpeg_decoder_handle_t mjpeg_decoder;
    video_frame_converter_handle_t mjpeg_converter;
    uint8_t *mjpeg_decode_buffer;
    call_video_input_slot_t slots[CALL_VIDEO_INPUT_SLOT_COUNT];
    call_video_decoded_slot_t decoded_slots[CALL_VIDEO_DECODED_SLOT_COUNT];
    call_video_output_slot_t output_slots[CALL_VIDEO_OUTPUT_SLOT_COUNT];
    uint8_t ready_output_slots[CALL_VIDEO_OUTPUT_SLOT_COUNT];
    uint8_t ready_output_head;
    uint8_t ready_output_count;
    uint8_t presented_output_slot;
    uint32_t adaptive_playout_generation;
    uint32_t adaptive_playout_generation_seen;
    uint8_t adaptive_playout_depth;
    uint8_t adaptive_gap_samples;
    int64_t adaptive_gap_confirm_until_us;
    int64_t adaptive_playout_activated_at_us;
    int64_t adaptive_playout_until_us;
    int64_t adaptive_next_present_at_us;
    uint32_t adaptive_playout_interval_us;
    bool adaptive_playout_started;
    bool start_pending;
    bool running;
    bool stop_requested;
    bool faulted;
    bool decode_in_progress;
    int64_t decode_started_at_us;
    uint32_t decode_generation;
    uint32_t decode_pts;
    size_t decode_payload_bytes;
    bool decode_key_frame;
    bool waiting_for_key_frame;
    bool latency_recovery_pending;
    uint8_t latency_pressure_samples;
    bool h264_profile_known;
    bool h264_profile_supported;
    bool h264_sps_queued;
    bool h264_pps_queued;
    bool h264_sps_hash_valid;
    bool h264_pps_hash_valid;
    bool h264_format_error_logged;
    bool h264_missing_parameter_sets_logged;
    bool frame_ready;
    bool session_active;
    esp_err_t start_result;
    uint32_t generation;
    uint32_t h264_sps_hash;
    uint32_t h264_pps_hash;
    uint8_t h264_profile_idc;
    uint8_t h264_constraint_flags;
    uint8_t h264_level_idc;
    uint16_t source_width;
    uint16_t source_height;
    uint32_t startup_trace_frames;
    uint32_t received_frames;
    uint64_t received_bytes;
    uint32_t submitted_frames;
    uint32_t decoded_frames;
    uint32_t converted_frames;
    uint32_t conversion_dropped_frames;
    uint32_t conversion_failures;
    uint32_t dropped_frames;
    uint32_t decode_failures;
    uint32_t latest_sequence;
    uint32_t presented_frames;
    uint32_t stale_received_frames;
    uint32_t stale_presented_frames;
    bool received_pts_valid;
    uint32_t received_pts;
    bool presented_pts_valid;
    uint32_t presented_pts;
    int64_t last_received_at_us;
    uint32_t receive_gap_window_max_us;
    int64_t last_presented_at_us;
    uint32_t present_gap_window_max_us;
    uint32_t decode_process_calls;
    uint64_t decode_time_us;
    uint32_t decode_access_units;
    uint64_t decode_access_unit_time_us;
    uint32_t decode_access_unit_max_us;
    uint32_t decode_key_access_units;
    uint64_t decode_key_time_us;
    uint32_t decode_delta_access_units;
    uint64_t decode_delta_time_us;
    uint64_t decode_copy_time_us;
    uint32_t decode_copy_max_us;
    uint64_t convert_time_us;
    uint32_t convert_max_us;
    uint64_t convert_pack_time_us;
    uint32_t convert_pack_max_us;
    uint64_t convert_ppa_time_us;
    uint32_t convert_ppa_max_us;
    uint64_t convert_swap_time_us;
    uint32_t convert_swap_max_us;
    uint64_t present_copy_time_us;
    uint32_t present_copy_max_us;
    uint32_t input_queue_age_samples;
    uint64_t input_queue_age_us;
    uint32_t input_queue_age_max_us;
    uint32_t decoder_creations;
    uint32_t decoder_restarts;
    uint32_t discontinuities;
    uint32_t input_overflows;
    uint32_t memory_trace_generation;
    uint32_t memory_trace_mask;
    bool resources_preparing;
    bool resources_ready;
    bool mjpeg_decoder_preparing;
    call_video_codec_t codec;
} call_video_renderer_t;

static call_video_renderer_t s_renderer = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .presented_output_slot = CALL_VIDEO_OUTPUT_SLOT_INVALID,
    .codec = CALL_VIDEO_CODEC_H264,
};

static uint32_t call_video_elapsed_us(int64_t started_at_us, int64_t finished_at_us)
{
    if (started_at_us <= 0 || finished_at_us <= started_at_us) {
        return 0U;
    }

    uint64_t elapsed_us = (uint64_t)(finished_at_us - started_at_us);
    return elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
}

static void call_video_log_memory_once(uint32_t generation,
                                       uint32_t stage_bit,
                                       const char *stage)
{
    bool should_log = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    if (generation == s_renderer.generation) {
        if (s_renderer.memory_trace_generation != generation) {
            s_renderer.memory_trace_generation = generation;
            s_renderer.memory_trace_mask = 0U;
        }
        if ((s_renderer.memory_trace_mask & stage_bit) == 0U) {
            s_renderer.memory_trace_mask |= stage_bit;
            should_log = true;
        }
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!should_log) {
        return;
    }

    ESP_LOGI(TAG,
             "downlink first-frame memory: stage=%s gen=%lu internal=%u largest=%u "
             "dma=%u largest=%u psram=%u largest=%u internal_min=%u",
             stage,
             (unsigned long)generation,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static bool call_video_adaptive_playout_is_active(int64_t now_us)
{
    int64_t adaptive_until_us = 0;

    taskENTER_CRITICAL(&s_renderer.lock);
    adaptive_until_us = s_renderer.adaptive_playout_until_us;
    taskEXIT_CRITICAL(&s_renderer.lock);
    return adaptive_until_us > now_us;
}

static uint32_t call_video_adaptive_playout_interval(uint8_t ready_frames,
                                                     uint8_t target_depth)
{
    if (target_depth >= 4U && (uint32_t)ready_frames * 4U <= target_depth) {
        return CALL_VIDEO_ADAPTIVE_CRITICAL_INTERVAL_US;
    }
    if (target_depth >= 2U && (uint32_t)ready_frames * 2U <= target_depth) {
        return CALL_VIDEO_ADAPTIVE_REFILL_INTERVAL_US;
    }
    return CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US;
}

static bool call_video_pts_is_newer(uint32_t current, uint32_t previous)
{
    return (int32_t)(current - previous) > 0;
}

static void call_video_note_received(int64_t received_at_us, uint32_t pts)
{
    bool adaptive_playout_activated = false;
    bool adaptive_target_raised = false;
    bool adaptive_gap_confirmed = false;
    uint32_t adaptive_gap_us = 0U;
    uint8_t adaptive_depth = CALL_VIDEO_ADAPTIVE_PLAYOUT_DEPTH;

    taskENTER_CRITICAL(&s_renderer.lock);
    if (s_renderer.last_received_at_us > 0 &&
        received_at_us > s_renderer.last_received_at_us) {
        uint32_t gap_us = call_video_elapsed_us(s_renderer.last_received_at_us,
                                                received_at_us);
        if (gap_us > s_renderer.receive_gap_window_max_us) {
            s_renderer.receive_gap_window_max_us = gap_us;
        }
        if (gap_us >= CALL_VIDEO_ADAPTIVE_PLAYOUT_GAP_US) {
            adaptive_gap_us = gap_us;
            const bool adaptive_already_active =
                s_renderer.adaptive_playout_until_us > received_at_us;
            if (adaptive_already_active ||
                gap_us >= CALL_VIDEO_ADAPTIVE_IMMEDIATE_GAP_US) {
                adaptive_gap_confirmed = true;
            } else {
                if (received_at_us <= s_renderer.adaptive_gap_confirm_until_us) {
                    if (s_renderer.adaptive_gap_samples < UINT8_MAX) {
                        s_renderer.adaptive_gap_samples++;
                    }
                } else {
                    s_renderer.adaptive_gap_samples = 1U;
                }
                s_renderer.adaptive_gap_confirm_until_us =
                    received_at_us + CALL_VIDEO_ADAPTIVE_CONFIRM_WINDOW_US;
                adaptive_gap_confirmed =
                    s_renderer.adaptive_gap_samples >=
                    CALL_VIDEO_ADAPTIVE_CONFIRM_SAMPLES;
            }

            if (adaptive_gap_confirmed) {
                uint32_t required_depth =
                    (gap_us + CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US - 1U) /
                    CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US + 1U;
                if (required_depth < CALL_VIDEO_ADAPTIVE_PLAYOUT_DEPTH) {
                    required_depth = CALL_VIDEO_ADAPTIVE_PLAYOUT_DEPTH;
                }
                if (required_depth > CALL_VIDEO_ADAPTIVE_PLAYOUT_MAX_DEPTH) {
                    required_depth = CALL_VIDEO_ADAPTIVE_PLAYOUT_MAX_DEPTH;
                }
                adaptive_depth = (uint8_t)required_depth;
                if (!adaptive_already_active) {
                    s_renderer.adaptive_playout_generation++;
                    if (s_renderer.adaptive_playout_generation == 0U) {
                        s_renderer.adaptive_playout_generation = 1U;
                    }
                    s_renderer.adaptive_playout_depth = adaptive_depth;
                    s_renderer.adaptive_playout_activated_at_us = received_at_us;
                    adaptive_playout_activated = true;
                } else if (adaptive_depth > s_renderer.adaptive_playout_depth) {
                    /* Raise the reserve target without restarting presentation.
                     * The low-watermark pace controller refills continuously;
                     * forcing a new prime for every larger jitter sample turns a
                     * healthy growing reservoir into repeated visible freezes. */
                    s_renderer.adaptive_playout_depth = adaptive_depth;
                    adaptive_target_raised = true;
                }
                s_renderer.adaptive_playout_until_us =
                    received_at_us + CALL_VIDEO_ADAPTIVE_PLAYOUT_HOLD_US;
                s_renderer.adaptive_gap_samples = 0U;
                s_renderer.adaptive_gap_confirm_until_us = 0;
            }
        }
    }
    s_renderer.last_received_at_us = received_at_us;
    if (s_renderer.received_pts_valid &&
        (pts != 0U || s_renderer.received_pts != 0U) &&
        !call_video_pts_is_newer(pts, s_renderer.received_pts)) {
        s_renderer.stale_received_frames++;
    } else {
        s_renderer.received_pts = pts;
        s_renderer.received_pts_valid = true;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (adaptive_playout_activated) {
        ESP_LOGI(TAG,
                 "adaptive video playout active: input_gap=%lums depth=%u pace=%lums hold=%lus",
                 (unsigned long)(adaptive_gap_us / 1000U),
                 (unsigned)adaptive_depth,
                 (unsigned long)(CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US / 1000U),
                 (unsigned long)(CALL_VIDEO_ADAPTIVE_PLAYOUT_HOLD_US / 1000000LL));
    } else if (adaptive_target_raised) {
        ESP_LOGI(TAG,
                 "adaptive video reserve target raised: input_gap=%lums depth=%u",
                 (unsigned long)(adaptive_gap_us / 1000U),
                 (unsigned)adaptive_depth);
    }
}

static const char *call_video_codec_name(call_video_codec_t codec)
{
    switch (codec) {
    case CALL_VIDEO_CODEC_MJPEG:
        return "mjpeg";
    case CALL_VIDEO_CODEC_H264:
    default:
        return "h264";
    }
}

static void call_video_reset_h264_stream_state_locked(void)
{
    s_renderer.h264_profile_known = false;
    s_renderer.h264_profile_supported = false;
    s_renderer.h264_sps_queued = false;
    s_renderer.h264_pps_queued = false;
    s_renderer.h264_sps_hash_valid = false;
    s_renderer.h264_pps_hash_valid = false;
    s_renderer.h264_format_error_logged = false;
    s_renderer.h264_missing_parameter_sets_logged = false;
    s_renderer.h264_sps_hash = 0U;
    s_renderer.h264_pps_hash = 0U;
    s_renderer.h264_profile_idc = 0U;
    s_renderer.h264_constraint_flags = 0U;
    s_renderer.h264_level_idc = 0U;
}

static uint32_t call_video_h264_hash_nal(const uint8_t *data, size_t data_len)
{
    uint32_t hash = 2166136261U;

    for (size_t index = 0; index < data_len; ++index) {
        hash ^= data[index];
        hash *= 16777619U;
    }
    return hash;
}

static bool call_video_h264_find_start_code(const uint8_t *data,
                                            size_t data_len,
                                            size_t search_offset,
                                            size_t *start_offset,
                                            size_t *start_code_len)
{
    if (data == NULL || start_offset == NULL || start_code_len == NULL ||
        search_offset >= data_len) {
        return false;
    }

    for (size_t index = search_offset; index + 2U < data_len; ++index) {
        if (data[index] != 0U || data[index + 1U] != 0U) {
            continue;
        }
        if (data[index + 2U] == 1U) {
            *start_offset = index;
            *start_code_len = 3U;
            return true;
        }
        if (index + 3U < data_len &&
            data[index + 2U] == 0U && data[index + 3U] == 1U) {
            *start_offset = index;
            *start_code_len = 4U;
            return true;
        }
    }
    return false;
}

static call_video_h264_access_unit_t call_video_h264_inspect_access_unit(
    const uint8_t *data,
    size_t data_len)
{
    call_video_h264_access_unit_t info = {0};
    size_t start_offset = 0U;
    size_t start_code_len = 0U;

    if (!call_video_h264_find_start_code(data,
                                         data_len,
                                         0U,
                                         &start_offset,
                                         &start_code_len)) {
        return info;
    }
    info.annexb = true;

    while (start_offset + start_code_len < data_len) {
        size_t nal_offset = start_offset + start_code_len;
        size_t next_start_offset = 0U;
        size_t next_start_code_len = 0U;
        bool has_next = call_video_h264_find_start_code(data,
                                                        data_len,
                                                        nal_offset + 1U,
                                                        &next_start_offset,
                                                        &next_start_code_len);
        size_t nal_end = has_next ? next_start_offset : data_len;

        if (nal_end > nal_offset) {
            uint8_t nal_type = data[nal_offset] & 0x1FU;
            size_t nal_len = nal_end - nal_offset;
            info.has_nal = true;
            if (nal_type == 7U) {
                info.has_sps = true;
                info.sps_hash = call_video_h264_hash_nal(data + nal_offset, nal_len);
                if (nal_len >= 4U) {
                    info.profile_valid = true;
                    info.profile_idc = data[nal_offset + 1U];
                    info.constraint_flags = data[nal_offset + 2U];
                    info.level_idc = data[nal_offset + 3U];
                }
            } else if (nal_type == 8U) {
                info.has_pps = true;
                info.pps_hash = call_video_h264_hash_nal(data + nal_offset, nal_len);
            } else if (nal_type == 5U) {
                info.has_idr = true;
            }
        }

        if (!has_next) {
            break;
        }
        start_offset = next_start_offset;
        start_code_len = next_start_code_len;
    }
    return info;
}

static bool call_video_h264_profile_is_supported(const call_video_h264_access_unit_t *info)
{
    return info != NULL &&
           info->profile_valid &&
           info->profile_idc == CALL_VIDEO_H264_BASELINE_PROFILE_IDC &&
           (info->constraint_flags & CALL_VIDEO_H264_CONSTRAINT_SET1_FLAG) != 0U;
}

static esp_err_t call_video_prepare_h264_access_unit(const uint8_t *data,
                                                     size_t data_len,
                                                     bool sdk_key_frame,
                                                     bool waiting_for_key_frame,
                                                     bool *effective_key_frame,
                                                     bool *decoder_bootstrap)
{
    call_video_h264_access_unit_t info =
        call_video_h264_inspect_access_unit(data, data_len);
    bool log_format_error = false;
    bool log_profile = false;
    bool log_missing_parameters = false;
    bool profile_supported = false;
    bool sps_ready = false;
    bool pps_ready = false;

    ESP_RETURN_ON_FALSE(effective_key_frame != NULL && decoder_bootstrap != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid H264 access-unit outputs");

    if (!info.annexb || !info.has_nal || (info.has_sps && !info.profile_valid)) {
        taskENTER_CRITICAL(&s_renderer.lock);
        if (!s_renderer.h264_format_error_logged) {
            s_renderer.h264_format_error_logged = true;
            log_format_error = true;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (log_format_error) {
            ESP_LOGW(TAG,
                     "H264 downlink format rejected: len=%u annexb=%d nal=%d sps=%d head=%02X%02X%02X%02X%02X%02X%02X%02X",
                     (unsigned)data_len,
                     info.annexb ? 1 : 0,
                     info.has_nal ? 1 : 0,
                     info.has_sps ? 1 : 0,
                     data_len > 0U ? data[0] : 0U,
                     data_len > 1U ? data[1] : 0U,
                     data_len > 2U ? data[2] : 0U,
                     data_len > 3U ? data[3] : 0U,
                     data_len > 4U ? data[4] : 0U,
                     data_len > 5U ? data[5] : 0U,
                     data_len > 6U ? data[6] : 0U,
                     data_len > 7U ? data[7] : 0U);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    if (info.profile_valid) {
        bool profile_changed =
            !s_renderer.h264_profile_known ||
            s_renderer.h264_profile_idc != info.profile_idc ||
            s_renderer.h264_constraint_flags != info.constraint_flags ||
            s_renderer.h264_level_idc != info.level_idc;
        bool sps_changed =
            !s_renderer.h264_sps_hash_valid ||
            s_renderer.h264_sps_hash != info.sps_hash;

        if (sps_changed) {
            s_renderer.h264_sps_queued = false;
            s_renderer.h264_pps_queued = false;
            s_renderer.h264_sps_hash = info.sps_hash;
            s_renderer.h264_sps_hash_valid = true;
        }
        s_renderer.h264_profile_known = true;
        s_renderer.h264_profile_supported =
            call_video_h264_profile_is_supported(&info);
        s_renderer.h264_profile_idc = info.profile_idc;
        s_renderer.h264_constraint_flags = info.constraint_flags;
        s_renderer.h264_level_idc = info.level_idc;
        log_profile = profile_changed;
    }
    if (info.has_pps) {
        bool pps_changed =
            !s_renderer.h264_pps_hash_valid ||
            s_renderer.h264_pps_hash != info.pps_hash;
        if (pps_changed) {
            s_renderer.h264_pps_queued = false;
            s_renderer.h264_pps_hash = info.pps_hash;
            s_renderer.h264_pps_hash_valid = true;
        }
    }

    profile_supported =
        !s_renderer.h264_profile_known || s_renderer.h264_profile_supported;
    sps_ready = s_renderer.h264_sps_queued || info.profile_valid;
    pps_ready = s_renderer.h264_pps_queued || info.has_pps;
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (log_profile) {
        if (profile_supported) {
            ESP_LOGI(TAG,
                     "H264 downlink stream: annexb=1 profile=%u constraints=0x%02X level=%u sps=%d pps=%d idr=%d",
                     (unsigned)info.profile_idc,
                     (unsigned)info.constraint_flags,
                     (unsigned)info.level_idc,
                     info.has_sps ? 1 : 0,
                     info.has_pps ? 1 : 0,
                     info.has_idr ? 1 : 0);
        } else {
            ESP_LOGE(TAG,
                     "H264 downlink profile unsupported: profile=%u constraints=0x%02X level=%u; decoder requires constrained-baseline profile_idc=66",
                     (unsigned)info.profile_idc,
                     (unsigned)info.constraint_flags,
                     (unsigned)info.level_idc);
        }
    }
    if (!profile_supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    *effective_key_frame = sdk_key_frame || info.has_idr;
    *decoder_bootstrap = info.profile_valid || info.has_pps;

    bool parameters_missing =
        (*effective_key_frame && (!sps_ready || !pps_ready)) ||
        (waiting_for_key_frame && info.has_pps && !sps_ready);
    if (parameters_missing) {
        taskENTER_CRITICAL(&s_renderer.lock);
        if (!s_renderer.h264_missing_parameter_sets_logged) {
            s_renderer.h264_missing_parameter_sets_logged = true;
            log_missing_parameters = true;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (log_missing_parameters) {
            ESP_LOGW(TAG,
                     "H264 downlink key frame waiting for SPS/PPS: sdk_key=%d idr=%d sps=%d pps=%d",
                     sdk_key_frame ? 1 : 0,
                     info.has_idr ? 1 : 0,
                     sps_ready ? 1 : 0,
                     pps_ready ? 1 : 0);
        }
        return ESP_ERR_NOT_FINISHED;
    }
    if (waiting_for_key_frame && !*effective_key_frame && !*decoder_bootstrap) {
        return ESP_ERR_NOT_FINISHED;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    if (info.profile_valid) {
        s_renderer.h264_sps_queued = true;
    }
    if (info.has_pps) {
        s_renderer.h264_pps_queued = true;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    return ESP_OK;
}

#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
static void call_video_log_small_rejected_au(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len == 0U || data_len > 128U) {
        return;
    }

    for (size_t offset = 0U; offset < data_len; offset += 16U) {
        const size_t chunk_len = data_len - offset < 16U ? data_len - offset : 16U;
        char line[(16U * 3U) + 1U] = {0};
        size_t written = 0U;

        for (size_t index = 0U; index < chunk_len; ++index) {
            int ret = snprintf(line + written,
                               sizeof(line) - written,
                               "%02X%s",
                               data[offset + index],
                               index + 1U == chunk_len ? "" : " ");
            if (ret <= 0 || (size_t)ret >= sizeof(line) - written) {
                break;
            }
            written += (size_t)ret;
        }
        ESP_LOGW(TAG,
                 "H264 rejected AU bytes: offset=%u/%u data=%s",
                 (unsigned)offset,
                 (unsigned)data_len,
                 line);
    }
}
#endif

static uint32_t call_video_input_queue_depth(void);

static bool call_video_stop_requested(void)
{
    bool requested = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    requested = s_renderer.stop_requested;
    taskEXIT_CRITICAL(&s_renderer.lock);
    return requested;
}

static const char *call_video_task_state_name(eTaskState state)
{
    switch (state) {
    case eRunning:
        return "run";
    case eReady:
        return "ready";
    case eBlocked:
        return "blocked";
    case eSuspended:
        return "suspended";
    case eDeleted:
        return "deleted";
    case eInvalid:
    default:
        return "invalid";
    }
}

static void call_video_log_task_snapshot(const char *role,
                                         const char *expected_name,
                                         TaskHandle_t task)
{
    if (task == NULL) {
        ESP_LOGE(TAG,
                 "H264 hang task: role=%s task=%s missing",
                 role,
                 expected_name);
        return;
    }

    ESP_LOGE(TAG,
             "H264 hang task: role=%s task=%s state=%s prio=%u core=%ld hwm=%u",
             role,
             pcTaskGetName(task),
             call_video_task_state_name(eTaskGetState(task)),
             (unsigned)uxTaskPriorityGet(task),
             (long)xTaskGetCoreID(task),
             (unsigned)uxTaskGetStackHighWaterMark(task));
}

static void call_video_log_named_task_snapshot(const char *role,
                                               const char *task_name)
{
    call_video_log_task_snapshot(role, task_name, xTaskGetHandle(task_name));
}

static bool call_video_renderer_detect_decode_fault(int64_t now_us)
{
    bool faulted = false;
    bool newly_faulted = false;
    uint64_t blocked_us = 0U;
    uint32_t generation = 0U;
    uint32_t pts = 0U;
    size_t payload_bytes = 0U;
    bool key_frame = false;
    TaskHandle_t decode_task = NULL;

    taskENTER_CRITICAL(&s_renderer.lock);
    if (s_renderer.codec == CALL_VIDEO_CODEC_H264 &&
        s_renderer.running &&
        s_renderer.decode_in_progress &&
        s_renderer.decode_started_at_us > 0 &&
        now_us > s_renderer.decode_started_at_us) {
        blocked_us = (uint64_t)(now_us - s_renderer.decode_started_at_us);
        if (!s_renderer.faulted &&
            blocked_us >= CALL_VIDEO_DECODE_HANG_TIMEOUT_US) {
            s_renderer.faulted = true;
            newly_faulted = true;
        }
        generation = s_renderer.decode_generation;
        pts = s_renderer.decode_pts;
        payload_bytes = s_renderer.decode_payload_bytes;
        key_frame = s_renderer.decode_key_frame;
        decode_task = s_renderer.task;
    }
    faulted = s_renderer.faulted;
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (newly_faulted) {
        ESP_LOGE(TAG,
                 "H264 decoder stalled: blocked=%llums gen=%lu pts=%lu bytes=%u key=%d q=%lu/%lu; quarantined",
                 (unsigned long long)(blocked_us / 1000ULL),
                 (unsigned long)generation,
                 (unsigned long)pts,
                 (unsigned)payload_bytes,
                 key_frame ? 1 : 0,
                 (unsigned long)call_video_input_queue_depth(),
                 s_renderer.decoded_ready_slots != NULL ?
                     (unsigned long)uxQueueMessagesWaiting(s_renderer.decoded_ready_slots) : 0UL);
        call_video_log_task_snapshot("caller", "call_h264_rx", decode_task);
        call_video_log_named_task_snapshot("helper", "h264FilterTask");
        call_video_log_named_task_snapshot("capture", "audio_capture");
        call_video_log_named_task_snapshot("playback", "media_audio_rx");
    }
    return faulted;
}

static void call_video_return_slot(uint8_t index)
{
    if (s_renderer.free_slots != NULL) {
        (void)xQueueSend(s_renderer.free_slots, &index, 0);
    }
}

static uint32_t call_video_input_queue_depth(void)
{
    uint32_t depth = 0U;

    if (s_renderer.ingress_slots != NULL) {
        depth += (uint32_t)uxQueueMessagesWaiting(s_renderer.ingress_slots);
    }
    if (s_renderer.ready_slots != NULL) {
        depth += (uint32_t)uxQueueMessagesWaiting(s_renderer.ready_slots);
    }
    return depth;
}

static void call_video_drain_ingress_queue(void)
{
    uint8_t index = 0U;

    while (s_renderer.ingress_slots != NULL &&
           xQueueReceive(s_renderer.ingress_slots, &index, 0) == pdTRUE) {
        call_video_return_slot(index);
    }
}

/*
 * The TiRTC socket thread must only copy and enqueue compressed access units.
 * Sending directly to the high-priority decoder queue wakes the decoder before
 * the SDK callback can return, so the 40-80 ms software decode is charged to
 * one ICE receive iteration. This lower-priority relay transfers slot ownership
 * after the socket thread blocks again; it adds no payload copy.
 */
static void call_video_ingress_task(void *arg)
{
    (void)arg;

    while (!call_video_stop_requested()) {
        uint8_t index = 0U;
        if (xQueueReceive(s_renderer.ingress_slots,
                          &index,
                          pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        bool accept = false;
        taskENTER_CRITICAL(&s_renderer.lock);
        accept = !s_renderer.stop_requested &&
                 s_renderer.running &&
                 index < CALL_VIDEO_INPUT_SLOT_COUNT &&
                 s_renderer.slots[index].generation == s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);

        if (!accept ||
            xQueueSend(s_renderer.ready_slots, &index, 0) != pdTRUE) {
            call_video_return_slot(index);
        }
    }

    call_video_drain_ingress_queue();
    xSemaphoreGive(s_renderer.ingress_stop_done);
    vTaskSuspend(NULL);
    abort();
}

static void call_video_drain_ready_queue(void)
{
    uint8_t index = 0;

    while (s_renderer.ready_slots != NULL &&
           xQueueReceive(s_renderer.ready_slots, &index, 0) == pdTRUE) {
        call_video_return_slot(index);
    }
}

static void call_video_return_decoded_slot(uint8_t index)
{
    if (s_renderer.decoded_free_slots != NULL) {
        (void)xQueueSend(s_renderer.decoded_free_slots, &index, 0);
    }
}

static void call_video_drain_decoded_ready_queue(void)
{
    uint8_t index = 0;

    while (s_renderer.decoded_ready_slots != NULL &&
           xQueueReceive(s_renderer.decoded_ready_slots, &index, 0) == pdTRUE) {
        call_video_return_decoded_slot(index);
    }
}

static bool call_video_take_latest_decoded_slot(uint8_t *slot_index,
                                                TickType_t wait_ticks)
{
    if (slot_index == NULL || s_renderer.decoded_ready_slots == NULL) {
        return false;
    }

    uint8_t newest_index = 0;
    if (xQueueReceive(s_renderer.decoded_ready_slots,
                      &newest_index,
                      wait_ticks) != pdTRUE) {
        return false;
    }

    /* A transport recovery burst is useful playout inventory. Preserve its
     * order while adaptive playout is active; the fixed RGB reservoir remains
     * the latency bound. Normal calls still collapse a decode burst to the
     * newest frame for minimum glass-to-glass delay. */
    if (call_video_adaptive_playout_is_active(esp_timer_get_time())) {
        *slot_index = newest_index;
        return true;
    }

    uint32_t stale_count = 0U;
    uint8_t queued_index = 0;
    while (xQueueReceive(s_renderer.decoded_ready_slots,
                         &queued_index,
                         0) == pdTRUE) {
        if (newest_index < CALL_VIDEO_DECODED_SLOT_COUNT) {
            call_video_return_decoded_slot(newest_index);
        }
        newest_index = queued_index;
        stale_count++;
    }

    if (stale_count > 0U) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames += stale_count;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }
    *slot_index = newest_index;
    return true;
}

static void call_video_drain_binary_semaphore(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL) {
        return;
    }
    while (xSemaphoreTake(semaphore, 0) == pdTRUE) {
    }
}

/* The RGB pool separates conversion from LCD DMA. Presentation collapses a
 * burst to its newest completed frame, so a realtime call never replays stale
 * pictures merely because decode briefly outran the panel. Callers hold
 * frame_mutex. */
static bool call_video_output_pop_ready_locked(uint8_t *slot_index)
{
    if (slot_index == NULL || s_renderer.ready_output_count == 0U) {
        return false;
    }

    *slot_index = s_renderer.ready_output_slots[s_renderer.ready_output_head];
    s_renderer.ready_output_head =
        (uint8_t)((s_renderer.ready_output_head + 1U) % CALL_VIDEO_OUTPUT_SLOT_COUNT);
    s_renderer.ready_output_count--;
    return true;
}

static bool call_video_output_pop_latest_locked(uint8_t *slot_index)
{
    uint32_t stale_frames = 0U;
    uint8_t stale_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;

    if (slot_index == NULL || s_renderer.ready_output_count == 0U) {
        return false;
    }
    while (s_renderer.ready_output_count > 1U) {
        if (!call_video_output_pop_ready_locked(&stale_index)) {
            break;
        }
        if (stale_index < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
            s_renderer.output_slots[stale_index].state = CALL_VIDEO_OUTPUT_FREE;
            stale_frames++;
        }
    }
    if (stale_frames > 0U) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames += stale_frames;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }
    return call_video_output_pop_ready_locked(slot_index);
}

static void call_video_output_push_ready_locked(uint8_t slot_index)
{
    uint8_t tail = (uint8_t)((s_renderer.ready_output_head +
                              s_renderer.ready_output_count) %
                             CALL_VIDEO_OUTPUT_SLOT_COUNT);
    s_renderer.ready_output_slots[tail] = slot_index;
    s_renderer.ready_output_count++;
}

static void call_video_output_release_ready_locked(void)
{
    uint8_t slot_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;

    while (call_video_output_pop_ready_locked(&slot_index)) {
        if (slot_index < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
            s_renderer.output_slots[slot_index].state = CALL_VIDEO_OUTPUT_FREE;
        }
    }
    s_renderer.ready_output_head = 0U;
}

static void call_video_mark_discontinuity(void)
{
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.generation++;
    s_renderer.waiting_for_key_frame = true;
    s_renderer.latency_recovery_pending = false;
    s_renderer.latency_pressure_samples = 0U;
    s_renderer.h264_sps_queued = false;
    s_renderer.h264_pps_queued = false;
    s_renderer.discontinuities++;
    taskEXIT_CRITICAL(&s_renderer.lock);
    call_video_drain_ingress_queue();
    call_video_drain_ready_queue();
    call_video_drain_decoded_ready_queue();
}

static esp_err_t call_video_decoder_create(esp_h264_dec_handle_t *decoder,
                                           esp_h264_dec_param_sw_handle_t *parameters)
{
    esp_h264_dec_cfg_sw_t config = {
        .pic_type = ESP_H264_RAW_FMT_I420,
    };

    if (esp_h264_dec_sw_new(&config, decoder) != ESP_H264_ERR_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (esp_h264_dec_sw_get_param_hd(*decoder, parameters) != ESP_H264_ERR_OK ||
        esp_h264_dec_open(*decoder) != ESP_H264_ERR_OK) {
        (void)esp_h264_dec_del(*decoder);
        *decoder = NULL;
        *parameters = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void call_video_decoder_destroy(esp_h264_dec_handle_t *decoder)
{
    if (decoder == NULL || *decoder == NULL) {
        return;
    }
    (void)esp_h264_dec_close(*decoder);
    (void)esp_h264_dec_del(*decoder);
    *decoder = NULL;
}

static esp_err_t call_video_decoder_dma_guard_begin(bool *escrow_lent,
                                                    const char *reason)
{
    ESP_RETURN_ON_FALSE(escrow_lent != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid DMA guard state");
    *escrow_lent = false;

    /* The escrow protects a contiguous DMA block by occupying it while media
     * drivers are idle. Decoder bootstrap must borrow that block, not allocate
     * another escrow block before it is allowed to run. An absent escrow is
     * valid: the early H264 encoder reservation may already own that memory. */
    if (!media_dma_reserve_is_reserved()) {
        return ESP_OK;
    }

    media_dma_reserve_release(reason);
    *escrow_lent = true;
    return ESP_OK;
}

static void call_video_decoder_dma_guard_end(bool escrow_lent,
                                             esp_err_t decoder_ret,
                                             const char *codec_name)
{
    esp_err_t reclaim_ret = ESP_OK;
    if (escrow_lent) {
        reclaim_ret = media_dma_reserve_reclaim(
            decoder_ret == ESP_OK ? "downlink-decoder-bootstrap-done" :
                                    "downlink-decoder-bootstrap-failed");
    }
    if (escrow_lent && reclaim_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s downlink DMA escrow remains lent: decoder=%s reclaim=%s",
                 codec_name,
                 esp_err_to_name(decoder_ret),
                 esp_err_to_name(reclaim_ret));
    }
    ESP_LOGI(TAG,
             "%s downlink decoder bootstrap: ret=%s escrow_lent=%d escrow_restore=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u",
             codec_name,
             esp_err_to_name(decoder_ret),
             escrow_lent ? 1 : 0,
             esp_err_to_name(reclaim_ret),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

esp_err_t call_video_renderer_prewarm_mjpeg_decoder(void)
{
    bool already_ready = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    already_ready = s_renderer.mjpeg_decoder != NULL;
    if (s_renderer.mjpeg_decoder_preparing) {
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!already_ready) {
        s_renderer.mjpeg_decoder_preparing = true;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (already_ready) {
        return ESP_OK;
    }

    /*
     * Keep the JPEG engine for the lifetime of the application. Creating it
     * after WHIP has connected is too late: RTC, audio and the realtime task
     * stack can fragment internal DMA memory even when PSRAM is mostly free.
     * Frame payloads remain in the renderer's fixed PSRAM pools.
     */
    const jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 0,
        .timeout_ms = CALL_VIDEO_MJPEG_DECODE_TIMEOUT_MS,
    };
    jpeg_decoder_handle_t decoder = NULL;
    esp_err_t ret = jpeg_new_decoder_engine(&engine_config, &decoder);
    bool stored = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.mjpeg_decoder_preparing = false;
    if (ret == ESP_OK && s_renderer.mjpeg_decoder == NULL) {
        s_renderer.mjpeg_decoder = decoder;
        decoder = NULL;
        stored = true;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (decoder != NULL) {
        (void)jpeg_del_decoder_engine(decoder);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "MJPEG hardware decoder prewarm failed: ret=%s internal_free=%u "
                 "internal_largest=%u dma_free=%u dma_largest=%u",
                 esp_err_to_name(ret),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        return ret;
    }

    if (stored) {
        ESP_LOGI(TAG,
                 "MJPEG hardware decoder prewarmed: persistent=1 internal_free=%u "
                 "internal_largest=%u dma_free=%u dma_largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    return ESP_OK;
}

typedef struct {
    int64_t last_log_us;
    uint32_t received_frames;
    uint64_t received_bytes;
    uint32_t submitted_frames;
    uint32_t decoded_frames;
    uint32_t converted_frames;
    uint32_t presented_frames;
    uint32_t stale_received_frames;
    uint32_t stale_presented_frames;
    uint32_t dropped_frames;
    uint32_t conversion_dropped_frames;
    uint32_t decode_failures;
    uint32_t conversion_failures;
    uint32_t decode_process_calls;
    uint64_t decode_time_us;
    uint32_t decode_access_units;
    uint64_t decode_access_unit_time_us;
    uint32_t decode_access_unit_max_us;
    uint32_t decode_key_access_units;
    uint64_t decode_key_time_us;
    uint32_t decode_delta_access_units;
    uint64_t decode_delta_time_us;
    uint64_t decode_copy_time_us;
    uint32_t decode_copy_max_us;
    uint64_t convert_time_us;
    uint32_t convert_max_us;
    uint64_t present_copy_time_us;
    uint32_t present_copy_max_us;
    uint32_t input_queue_age_samples;
    uint64_t input_queue_age_us;
    uint32_t input_queue_age_max_us;
    uint32_t decoder_creations;
    uint32_t decoder_restarts;
    uint32_t discontinuities;
    uint32_t input_overflows;
    video_frame_converter_stats_t converter;
} call_video_log_window_t;

static void call_video_log_stats_if_due(call_video_log_window_t *window,
                                        video_frame_converter_handle_t converter)
{
    int64_t now_us = esp_timer_get_time();
    if (window->last_log_us == 0) {
        window->last_log_us = now_us;
        return;
    }
    int64_t elapsed_us = now_us - window->last_log_us;
    if (elapsed_us < CALL_VIDEO_STATS_INTERVAL_US) {
        return;
    }

    call_video_log_window_t current = {
        .last_log_us = now_us,
    };
    uint16_t source_width = 0;
    uint16_t source_height = 0;
    bool session_active = false;
    uint32_t receive_gap_max_us = 0;
    uint32_t present_gap_max_us = 0;
    taskENTER_CRITICAL(&s_renderer.lock);
    current.received_frames = s_renderer.received_frames;
    current.received_bytes = s_renderer.received_bytes;
    current.submitted_frames = s_renderer.submitted_frames;
    current.decoded_frames = s_renderer.decoded_frames;
    current.converted_frames = s_renderer.converted_frames;
    current.presented_frames = s_renderer.presented_frames;
    current.stale_received_frames = s_renderer.stale_received_frames;
    current.stale_presented_frames = s_renderer.stale_presented_frames;
    current.dropped_frames = s_renderer.dropped_frames;
    current.conversion_dropped_frames = s_renderer.conversion_dropped_frames;
    current.decode_failures = s_renderer.decode_failures;
    current.conversion_failures = s_renderer.conversion_failures;
    current.decode_process_calls = s_renderer.decode_process_calls;
    current.decode_time_us = s_renderer.decode_time_us;
    current.decode_access_units = s_renderer.decode_access_units;
    current.decode_access_unit_time_us = s_renderer.decode_access_unit_time_us;
    current.decode_access_unit_max_us = s_renderer.decode_access_unit_max_us;
    current.decode_key_access_units = s_renderer.decode_key_access_units;
    current.decode_key_time_us = s_renderer.decode_key_time_us;
    current.decode_delta_access_units = s_renderer.decode_delta_access_units;
    current.decode_delta_time_us = s_renderer.decode_delta_time_us;
    current.decode_copy_time_us = s_renderer.decode_copy_time_us;
    current.decode_copy_max_us = s_renderer.decode_copy_max_us;
    current.convert_time_us = s_renderer.convert_time_us;
    current.convert_max_us = s_renderer.convert_max_us;
    current.present_copy_time_us = s_renderer.present_copy_time_us;
    current.present_copy_max_us = s_renderer.present_copy_max_us;
    current.input_queue_age_samples = s_renderer.input_queue_age_samples;
    current.input_queue_age_us = s_renderer.input_queue_age_us;
    current.input_queue_age_max_us = s_renderer.input_queue_age_max_us;
    current.decoder_creations = s_renderer.decoder_creations;
    current.decoder_restarts = s_renderer.decoder_restarts;
    current.discontinuities = s_renderer.discontinuities;
    current.input_overflows = s_renderer.input_overflows;
    source_width = s_renderer.source_width;
    source_height = s_renderer.source_height;
    session_active = s_renderer.session_active;
    receive_gap_max_us = s_renderer.receive_gap_window_max_us;
    present_gap_max_us = s_renderer.present_gap_window_max_us;
    s_renderer.receive_gap_window_max_us = 0;
    s_renderer.present_gap_window_max_us = 0;
    taskEXIT_CRITICAL(&s_renderer.lock);
    video_frame_converter_get_stats(converter, &current.converter);

    uint32_t received_delta = current.received_frames - window->received_frames;
    uint64_t received_bytes_delta = current.received_bytes - window->received_bytes;
    uint32_t submitted_delta = current.submitted_frames - window->submitted_frames;
    uint32_t decoded_delta = current.decoded_frames - window->decoded_frames;
    uint32_t converted_delta = current.converted_frames - window->converted_frames;
    uint32_t presented_delta = current.presented_frames - window->presented_frames;
    uint32_t stale_received_delta =
        current.stale_received_frames - window->stale_received_frames;
    uint32_t stale_presented_delta =
        current.stale_presented_frames - window->stale_presented_frames;
    uint32_t dropped_delta = current.dropped_frames - window->dropped_frames;
    uint32_t conversion_dropped_delta =
        current.conversion_dropped_frames - window->conversion_dropped_frames;
    uint32_t failure_delta = current.decode_failures - window->decode_failures;
    uint32_t conversion_failure_delta =
        current.conversion_failures - window->conversion_failures;
    uint32_t process_delta = current.decode_process_calls - window->decode_process_calls;
    uint64_t decode_time_delta = current.decode_time_us - window->decode_time_us;
    uint32_t access_unit_delta = current.decode_access_units - window->decode_access_units;
    uint64_t access_unit_time_delta =
        current.decode_access_unit_time_us - window->decode_access_unit_time_us;
    uint32_t key_access_unit_delta =
        current.decode_key_access_units - window->decode_key_access_units;
    uint64_t key_time_delta = current.decode_key_time_us - window->decode_key_time_us;
    uint32_t delta_access_unit_delta =
        current.decode_delta_access_units - window->decode_delta_access_units;
    uint64_t delta_time_delta = current.decode_delta_time_us - window->decode_delta_time_us;
    uint64_t decode_copy_time_delta = current.decode_copy_time_us - window->decode_copy_time_us;
    uint64_t convert_time_delta = current.convert_time_us - window->convert_time_us;
    uint64_t present_copy_time_delta =
        current.present_copy_time_us - window->present_copy_time_us;
    uint32_t queue_age_samples_delta =
        current.input_queue_age_samples - window->input_queue_age_samples;
    uint64_t queue_age_time_delta = current.input_queue_age_us - window->input_queue_age_us;
    uint32_t decoder_creations_delta = current.decoder_creations - window->decoder_creations;
    uint32_t decoder_restarts_delta = current.decoder_restarts - window->decoder_restarts;
    uint32_t discontinuities_delta = current.discontinuities - window->discontinuities;
    uint32_t input_overflows_delta = current.input_overflows - window->input_overflows;
    uint32_t ppa_frames_delta = current.converter.ppa_frames - window->converter.ppa_frames;
    uint32_t software_frames_delta =
        current.converter.software_frames - window->converter.software_frames;
    uint64_t pack_time_delta = current.converter.pack_time_us - window->converter.pack_time_us;
    uint64_t ppa_time_delta = current.converter.ppa_time_us - window->converter.ppa_time_us;
    uint64_t swap_time_delta = current.converter.swap_time_us - window->converter.swap_time_us;
    uint64_t software_time_delta =
        current.converter.software_time_us - window->converter.software_time_us;
    uint32_t received_fps_x10 = (uint32_t)(((uint64_t)received_delta * 10000000ULL) /
                                           (uint64_t)elapsed_us);
    uint32_t received_kbps = (uint32_t)((received_bytes_delta * 8ULL * 1000ULL) /
                                        (uint64_t)elapsed_us);
    uint32_t queued_fps_x10 = (uint32_t)(((uint64_t)submitted_delta * 10000000ULL) /
                                         (uint64_t)elapsed_us);
    uint32_t decoded_fps_x10 = (uint32_t)(((uint64_t)decoded_delta * 10000000ULL) /
                                           (uint64_t)elapsed_us);
    uint32_t converted_fps_x10 = (uint32_t)(((uint64_t)converted_delta * 10000000ULL) /
                                             (uint64_t)elapsed_us);
    uint32_t presented_fps_x10 = (uint32_t)(((uint64_t)presented_delta * 10000000ULL) /
                                            (uint64_t)elapsed_us);
    uint32_t average_decode_us = process_delta > 0U ?
                                 (uint32_t)(decode_time_delta / process_delta) : 0U;
    uint32_t average_access_unit_us = access_unit_delta > 0U ?
                                      (uint32_t)(access_unit_time_delta / access_unit_delta) : 0U;
    uint32_t average_key_us = key_access_unit_delta > 0U ?
                              (uint32_t)(key_time_delta / key_access_unit_delta) : 0U;
    uint32_t average_delta_us = delta_access_unit_delta > 0U ?
                                (uint32_t)(delta_time_delta / delta_access_unit_delta) : 0U;
    uint32_t average_copy_us = decoded_delta > 0U ?
                               (uint32_t)(decode_copy_time_delta / decoded_delta) : 0U;
    uint32_t average_convert_us = converted_delta > 0U ?
                                   (uint32_t)(convert_time_delta / converted_delta) : 0U;
    uint32_t average_present_copy_us = presented_delta > 0U ?
                                        (uint32_t)(present_copy_time_delta / presented_delta) : 0U;
    uint32_t average_queue_age_us = queue_age_samples_delta > 0U ?
                                     (uint32_t)(queue_age_time_delta / queue_age_samples_delta) : 0U;
    uint32_t average_pack_us = ppa_frames_delta > 0U ?
                                (uint32_t)(pack_time_delta / ppa_frames_delta) : 0U;
    uint32_t average_ppa_us = ppa_frames_delta > 0U ?
                               (uint32_t)(ppa_time_delta / ppa_frames_delta) : 0U;
    uint32_t average_swap_us = ppa_frames_delta > 0U ?
                                (uint32_t)(swap_time_delta / ppa_frames_delta) : 0U;
    uint32_t average_software_us = software_frames_delta > 0U ?
                                    (uint32_t)(software_time_delta / software_frames_delta) : 0U;
    uint32_t input_queue_depth = call_video_input_queue_depth();
    uint32_t decoded_queue_depth = s_renderer.decoded_ready_slots != NULL ?
                                   (uint32_t)uxQueueMessagesWaiting(s_renderer.decoded_ready_slots) : 0U;
    uint32_t output_queue_depth = 0U;
    uint32_t adaptive_target_depth = 0U;
    uint32_t adaptive_interval_us = CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US;
    bool adaptive_playout_active =
        call_video_adaptive_playout_is_active(esp_timer_get_time());
    if (s_renderer.frame_mutex != NULL &&
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        output_queue_depth = s_renderer.ready_output_count;
        adaptive_target_depth = s_renderer.adaptive_playout_depth;
        adaptive_interval_us = s_renderer.adaptive_playout_interval_us;
        xSemaphoreGive(s_renderer.frame_mutex);
    }

    if (!session_active) {
        *window = current;
        return;
    }

    if (CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS) {
        ESP_LOGI(TAG,
                 "H264 downlink stats: src=%ux%u rx=%u.%ufps/%ukbps queued=%u.%ufps "
                 "decoded=%u.%ufps converted=%u.%ufps presented=%u.%ufps "
                 "drop=input:%u display:%u fail=decode:%u convert:%u q=input:%u display:%u "
                 "mode=%s decode_us=au:%u max:%u proc:%u key:%u delta:%u copy:%u/%u "
                 "convert_us=%u/%u phases=%u/%u/%u sw:%u queue_age_us=%u/%u "
                 "sync=create:%u restart:%u reset:%u overflow:%u ui_handoff_us=%u/%u "
                 "cadence_ms=rx:%u present:%u stale_pts=rx:%u present:%u",
                 source_width,
                 source_height,
                 received_fps_x10 / 10U,
                 received_fps_x10 % 10U,
                 received_kbps,
                 queued_fps_x10 / 10U,
                 queued_fps_x10 % 10U,
                 decoded_fps_x10 / 10U,
                 decoded_fps_x10 % 10U,
                 converted_fps_x10 / 10U,
                 converted_fps_x10 % 10U,
                 presented_fps_x10 / 10U,
                 presented_fps_x10 % 10U,
                 dropped_delta,
                 conversion_dropped_delta,
                 failure_delta,
                 conversion_failure_delta,
                 input_queue_depth,
                 decoded_queue_depth,
                 video_frame_converter_mode_name(video_frame_converter_get_mode(converter)),
                 average_access_unit_us,
                 current.decode_access_unit_max_us,
                 average_decode_us,
                 average_key_us,
                 average_delta_us,
                 average_copy_us,
                 current.decode_copy_max_us,
                 average_convert_us,
                 current.convert_max_us,
                 average_pack_us,
                 average_ppa_us,
                 average_swap_us,
                 average_software_us,
                 average_queue_age_us,
                 current.input_queue_age_max_us,
                 decoder_creations_delta,
                 decoder_restarts_delta,
                 discontinuities_delta,
                 input_overflows_delta,
                 average_present_copy_us,
                 current.present_copy_max_us,
                 receive_gap_max_us / 1000U,
                 present_gap_max_us / 1000U,
                 stale_received_delta,
                 stale_presented_delta);
    } else if (CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG) {
        ESP_LOGI(TAG,
                 "VRX %ux%u in=%u.%u/%uk q/d/c/o=%u.%u/%u.%u/%u.%u/%u.%u "
                 "drop=%u/%u fail=%u/%u age=%u/%ums depth=%u/%u "
                 "buf=%u/%u@%ums adapt=%u ms=au/cvt/ppa:%u/%u/%u kd=%u/%u gap=%u/%u "
                 "old=%u/%u reset=%u/%u ovf=%u parts=pack/swap/ui:%u/%u/%u "
                 "max=au/cvt/ppa:%u/%u/%u",
                 source_width,
                 source_height,
                 received_fps_x10 / 10U,
                 received_fps_x10 % 10U,
                 received_kbps,
                 queued_fps_x10 / 10U,
                 queued_fps_x10 % 10U,
                 decoded_fps_x10 / 10U,
                 decoded_fps_x10 % 10U,
                 converted_fps_x10 / 10U,
                 converted_fps_x10 % 10U,
                 presented_fps_x10 / 10U,
                 presented_fps_x10 % 10U,
                 dropped_delta,
                 conversion_dropped_delta,
                 failure_delta,
                 conversion_failure_delta,
                 average_queue_age_us / 1000U,
                 current.input_queue_age_max_us / 1000U,
                 input_queue_depth,
                 decoded_queue_depth,
                 output_queue_depth,
                 adaptive_target_depth,
                 adaptive_interval_us / 1000U,
                 adaptive_playout_active ? 1U : 0U,
                 average_access_unit_us / 1000U,
                 average_convert_us / 1000U,
                 average_ppa_us / 1000U,
                 average_key_us / 1000U,
                 average_delta_us / 1000U,
                 receive_gap_max_us / 1000U,
                 present_gap_max_us / 1000U,
                 stale_received_delta,
                 stale_presented_delta,
                 decoder_restarts_delta,
                 discontinuities_delta,
                 input_overflows_delta,
                 average_pack_us / 1000U,
                 average_swap_us / 1000U,
                 average_present_copy_us / 1000U,
                 current.decode_access_unit_max_us / 1000U,
                 current.convert_max_us / 1000U,
                 current.converter.ppa_max_us / 1000U);
    }
    *window = current;
}

static esp_err_t call_video_copy_display_i420(const uint8_t *source,
                                              uint16_t source_width,
                                              uint16_t source_height,
                                              uint8_t *output)
{
    uint16_t crop_x = CALL_VIDEO_SOURCE_CROP_X;
    uint16_t crop_y = CALL_VIDEO_SOURCE_CROP_Y;
    const uint16_t crop_width = CALL_VIDEO_SOURCE_CROP_WIDTH;
    const uint16_t crop_height = CALL_VIDEO_SOURCE_CROP_HEIGHT;

    ESP_RETURN_ON_FALSE(source != NULL && output != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid I420 crop buffer");
    if (source_width >= crop_width && source_height >= crop_height) {
        crop_x = (uint16_t)(((source_width - crop_width) / 2U) & ~1U);
        crop_y = (uint16_t)(((source_height - crop_height) / 2U) & ~1U);
    }
    ESP_RETURN_ON_FALSE(((crop_x | crop_y | crop_width | crop_height) & 1U) == 0U &&
                            (uint32_t)crop_x + crop_width <= source_width &&
                            (uint32_t)crop_y + crop_height <= source_height,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "decoded resolution does not contain display crop");

    const size_t source_luma_size = (size_t)source_width * source_height;
    const size_t source_chroma_stride = source_width / 2U;
    const uint8_t *source_y = source;
    const uint8_t *source_u = source_y + source_luma_size;
    const uint8_t *source_v = source_u + (source_luma_size / 4U);
    const size_t output_luma_size = (size_t)crop_width * crop_height;
    uint8_t *output_y = output;
    uint8_t *output_u = output_y + output_luma_size;
    uint8_t *output_v = output_u + (output_luma_size / 4U);

    if (crop_x == 0U && crop_width == source_width) {
        size_t chroma_copy_size = output_luma_size / 4U;
        size_t source_chroma_offset = (size_t)(crop_y / 2U) * source_chroma_stride;
        memcpy(output_y,
               source_y + ((size_t)crop_y * source_width),
               output_luma_size);
        memcpy(output_u, source_u + source_chroma_offset, chroma_copy_size);
        memcpy(output_v, source_v + source_chroma_offset, chroma_copy_size);
        return ESP_OK;
    }

    for (uint16_t row = 0; row < crop_height; ++row) {
        memcpy(output_y + ((size_t)row * crop_width),
               source_y + ((size_t)(crop_y + row) * source_width) + crop_x,
               crop_width);
    }
    for (uint16_t row = 0; row < crop_height / 2U; ++row) {
        size_t source_offset =
            ((size_t)((crop_y / 2U) + row) * source_chroma_stride) + (crop_x / 2U);
        size_t output_offset = (size_t)row * (crop_width / 2U);
        memcpy(output_u + output_offset, source_u + source_offset, crop_width / 2U);
        memcpy(output_v + output_offset, source_v + source_offset, crop_width / 2U);
    }
    return ESP_OK;
}

static esp_err_t call_video_queue_decoded_frame(const esp_h264_dec_out_frame_t *frame,
                                                 esp_h264_dec_param_sw_handle_t parameters,
                                                 const call_video_input_slot_t *input_slot)
{
    esp_h264_resolution_t resolution = {0};
    uint8_t index = 0;

    ESP_RETURN_ON_FALSE(frame != NULL && frame->outbuf != NULL && frame->out_size > 0U &&
                            input_slot != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid decoded frame");
    ESP_RETURN_ON_FALSE(esp_h264_dec_get_resolution(parameters, &resolution) == ESP_H264_ERR_OK,
                        ESP_FAIL,
                        TAG,
                        "read decoded resolution failed");
    size_t required = (size_t)resolution.width * resolution.height * 3U / 2U;
    ESP_RETURN_ON_FALSE(resolution.width >= 16U && resolution.height >= 16U &&
                            (resolution.width & 1U) == 0U && (resolution.height & 1U) == 0U &&
                            frame->out_size >= required,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "invalid I420 frame size");

    bool generation_matches = false;
    bool waiting_for_key_frame = false;
    taskENTER_CRITICAL(&s_renderer.lock);
    generation_matches = input_slot->generation == s_renderer.generation;
    waiting_for_key_frame = s_renderer.waiting_for_key_frame;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!generation_matches) {
        return ESP_ERR_INVALID_STATE;
    }
    if (waiting_for_key_frame && !input_slot->key_frame) {
        return ESP_ERR_NOT_FINISHED;
    }

    if (xQueueReceive(s_renderer.decoded_free_slots, &index, 0) != pdTRUE) {
        /* The decoder must keep consuming every reference frame. If display
         * conversion falls behind, replace its oldest already-decoded frame. */
        if (xQueueReceive(s_renderer.decoded_ready_slots, &index, 0) != pdTRUE) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.conversion_dropped_frames++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            return ESP_ERR_NOT_FINISHED;
        }
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }

    call_video_decoded_slot_t *slot = &s_renderer.decoded_slots[index];
    int64_t copy_started_us = esp_timer_get_time();
    esp_err_t copy_ret = call_video_copy_display_i420(frame->outbuf,
                                                       resolution.width,
                                                       resolution.height,
                                                       slot->data);
    uint32_t copy_elapsed_us = (uint32_t)(esp_timer_get_time() - copy_started_us);
    if (copy_ret != ESP_OK) {
        call_video_return_decoded_slot(index);
        return copy_ret;
    }
    slot->data_len = CALL_VIDEO_DECODED_SLOT_CAPACITY;
    slot->width = CALL_VIDEO_SOURCE_CROP_WIDTH;
    slot->height = CALL_VIDEO_SOURCE_CROP_HEIGHT;
    slot->pts = frame->pts;
    slot->generation = input_slot->generation;
    memset(&slot->trace, 0, sizeof(slot->trace));
    slot->trace_ready_at_us = esp_timer_get_time();
    if (input_slot->trace_frame_index > 0U) {
        uint32_t decode_total_us = call_video_elapsed_us(
            input_slot->trace_decode_started_at_us,
            slot->trace_ready_at_us);
        slot->trace.frame_index = input_slot->trace_frame_index;
        slot->trace.pts = input_slot->pts;
        slot->trace.payload_bytes = (uint32_t)input_slot->data_len;
        slot->trace.submit_us = call_video_elapsed_us(input_slot->trace_received_at_us,
                                                      input_slot->queued_at_us);
        slot->trace.input_wait_us = call_video_elapsed_us(
            input_slot->queued_at_us,
            input_slot->trace_decode_started_at_us);
        slot->trace.decode_copy_us = copy_elapsed_us;
        slot->trace.decode_us = decode_total_us > copy_elapsed_us ?
                                    decode_total_us - copy_elapsed_us : 0U;
        slot->trace.key_frame = input_slot->key_frame;
        slot->trace.decoder_bootstrap = input_slot->decoder_bootstrap;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    generation_matches = input_slot->generation == s_renderer.generation;
    s_renderer.decode_copy_time_us += copy_elapsed_us;
    if (copy_elapsed_us > s_renderer.decode_copy_max_us) {
        s_renderer.decode_copy_max_us = copy_elapsed_us;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!generation_matches) {
        call_video_return_decoded_slot(index);
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_renderer.decoded_ready_slots, &index, 0) != pdTRUE) {
        call_video_return_decoded_slot(index);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_NOT_FINISHED;
    }

    call_video_log_memory_once(input_slot->generation,
                               CALL_VIDEO_MEMORY_TRACE_DECODE_QUEUE,
                               "decoded-queue");

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.source_width = resolution.width;
    s_renderer.source_height = resolution.height;
    s_renderer.decoded_frames++;
    if (s_renderer.generation == input_slot->generation) {
        s_renderer.waiting_for_key_frame = false;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    return ESP_OK;
}

static esp_err_t call_video_decode_slot(esp_h264_dec_handle_t decoder,
                                        esp_h264_dec_param_sw_handle_t parameters,
                                        call_video_input_slot_t *slot)
{
    esp_h264_dec_in_frame_t input = {
        .raw_data = {
            .buffer = slot->data,
            .len = (uint32_t)slot->data_len,
        },
        .pts = slot->pts,
        .dts = slot->pts,
    };

    while (input.raw_data.len > 0U) {
        esp_h264_dec_out_frame_t output = {0};
        int64_t decode_started_us = esp_timer_get_time();
        esp_h264_err_t decode_ret = esp_h264_dec_process(decoder, &input, &output);
        uint32_t decode_elapsed_us = (uint32_t)(esp_timer_get_time() - decode_started_us);

        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.decode_process_calls++;
        s_renderer.decode_time_us += decode_elapsed_us;
        taskEXIT_CRITICAL(&s_renderer.lock);

        if (decode_ret != ESP_H264_ERR_OK || input.consume == 0U) {
            const call_video_h264_access_unit_t au =
                call_video_h264_inspect_access_unit(slot->data, slot->data_len);
            const uint32_t au_hash =
                call_video_h264_hash_nal(slot->data, slot->data_len);
            const size_t tail = slot->data_len >= 4U ? slot->data_len - 4U : 0U;
            ESP_LOGW(TAG,
                     "H264 AU rejected: decoder=%d consume=%u remaining=%u "
                     "pts=%lu key=%d len=%u hash=%08lx "
                     "annexb=%d sps=%d pps=%d idr=%d "
                     "head=%02X%02X%02X%02X tail=%02X%02X%02X%02X",
                     (int)decode_ret,
                     (unsigned)input.consume,
                     (unsigned)input.raw_data.len,
                     (unsigned long)slot->pts,
                     slot->key_frame ? 1 : 0,
                     (unsigned)slot->data_len,
                     (unsigned long)au_hash,
                     au.annexb ? 1 : 0,
                     au.has_sps ? 1 : 0,
                     au.has_pps ? 1 : 0,
                     au.has_idr ? 1 : 0,
                     slot->data_len > 0U ? slot->data[0] : 0U,
                     slot->data_len > 1U ? slot->data[1] : 0U,
                     slot->data_len > 2U ? slot->data[2] : 0U,
                     slot->data_len > 3U ? slot->data[3] : 0U,
                     slot->data_len > tail ? slot->data[tail] : 0U,
                     slot->data_len > tail + 1U ? slot->data[tail + 1U] : 0U,
                     slot->data_len > tail + 2U ? slot->data[tail + 2U] : 0U,
                     slot->data_len > tail + 3U ? slot->data[tail + 3U] : 0U);
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            call_video_log_small_rejected_au(slot->data, slot->data_len);
#endif
            return ESP_FAIL;
        }
        input.raw_data.buffer += input.consume;
        input.raw_data.len -= input.consume;
        if (output.out_size > 0U) {
            call_video_log_memory_once(slot->generation,
                                       CALL_VIDEO_MEMORY_TRACE_DECODE_OUTPUT,
                                       "decoder-output");
            esp_err_t queue_ret = call_video_queue_decoded_frame(&output,
                                                                  parameters,
                                                                  slot);
            if (queue_ret == ESP_ERR_NOT_FINISHED) {
                /* Display pressure is not a decoder failure. The decoded
                 * reference state is already valid, so skip presentation and
                 * continue consuming the bitstream. */
                continue;
            }
            if (queue_ret != ESP_OK) {
                return queue_ret;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t call_video_output_begin_write(uint8_t *slot_index, uint16_t **pixels)
{
    ESP_RETURN_ON_FALSE(slot_index != NULL && pixels != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid output slot request");
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t selected = CALL_VIDEO_OUTPUT_SLOT_INVALID;
    for (uint8_t index = 0; index < CALL_VIDEO_OUTPUT_SLOT_COUNT; ++index) {
        if (s_renderer.output_slots[index].state == CALL_VIDEO_OUTPUT_FREE) {
            selected = index;
            break;
        }
    }
    if (selected == CALL_VIDEO_OUTPUT_SLOT_INVALID) {
        /* Bound latency when the renderer outruns the LCD: reuse the oldest
         * frame that has not been presented, never the frame in flight. */
        if (!call_video_output_pop_ready_locked(&selected) ||
            selected >= CALL_VIDEO_OUTPUT_SLOT_COUNT) {
            xSemaphoreGive(s_renderer.frame_mutex);
            return ESP_ERR_NOT_FOUND;
        }
        s_renderer.output_slots[selected].state = CALL_VIDEO_OUTPUT_FREE;
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }

    s_renderer.output_slots[selected].state = CALL_VIDEO_OUTPUT_WRITING;
    s_renderer.output_slots[selected].pts = 0;
    memset(&s_renderer.output_slots[selected].trace,
           0,
           sizeof(s_renderer.output_slots[selected].trace));
    s_renderer.output_slots[selected].trace_ready_at_us = 0;
    *slot_index = selected;
    *pixels = s_renderer.output_slots[selected].pixels;
    xSemaphoreGive(s_renderer.frame_mutex);
    return ESP_OK;
}

static void call_video_output_finish_write(uint8_t slot_index,
                                           bool publish,
                                           uint32_t convert_elapsed_us,
                                           uint32_t source_pts)
{
    if (slot_index >= CALL_VIDEO_OUTPUT_SLOT_COUNT || s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    call_video_output_slot_t *slot = &s_renderer.output_slots[slot_index];
    if (!publish || slot->state != CALL_VIDEO_OUTPUT_WRITING) {
        slot->state = CALL_VIDEO_OUTPUT_FREE;
        xSemaphoreGive(s_renderer.frame_mutex);
        return;
    }

    if (s_renderer.ready_output_count >= CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        uint8_t dropped_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;
        if (call_video_output_pop_ready_locked(&dropped_index) &&
            dropped_index < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
            s_renderer.output_slots[dropped_index].state = CALL_VIDEO_OUTPUT_FREE;
        }
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.conversion_dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.latest_sequence++;
    slot->sequence = s_renderer.latest_sequence;
    slot->pts = source_pts;
    s_renderer.frame_ready = true;
    s_renderer.converted_frames++;
    s_renderer.convert_time_us += convert_elapsed_us;
    if (convert_elapsed_us > s_renderer.convert_max_us) {
        s_renderer.convert_max_us = convert_elapsed_us;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    slot->state = CALL_VIDEO_OUTPUT_READY;
    call_video_output_push_ready_locked(slot_index);
    xSemaphoreGive(s_renderer.frame_mutex);
}

static void call_video_converter_task(void *arg)
{
    video_frame_converter_handle_t converter = (video_frame_converter_handle_t)arg;
    int64_t last_stall_log_at_us = 0;

    while (!call_video_stop_requested()) {
        uint8_t index = 0;
        if (!call_video_take_latest_decoded_slot(&index,
                                                 pdMS_TO_TICKS(50))) {
            continue;
        }

        if (index >= CALL_VIDEO_DECODED_SLOT_COUNT) {
            continue;
        }

        call_video_decoded_slot_t *slot = &s_renderer.decoded_slots[index];
        uint32_t generation = 0;
        taskENTER_CRITICAL(&s_renderer.lock);
        generation = s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (slot->generation != generation) {
            call_video_return_decoded_slot(index);
            continue;
        }

        uint8_t output_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;
        uint16_t *output_pixels = NULL;
        esp_err_t output_ret = call_video_output_begin_write(&output_index, &output_pixels);
        if (output_ret != ESP_OK) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.conversion_dropped_frames++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            call_video_return_decoded_slot(index);
            continue;
        }

        int64_t convert_started_us = esp_timer_get_time();
        uint32_t decoded_wait_us = call_video_elapsed_us(slot->trace_ready_at_us,
                                                         convert_started_us);
        if (slot->trace.frame_index > 0U) {
            slot->trace.decoded_wait_us = decoded_wait_us;
        }
        esp_err_t convert_ret = video_frame_converter_i420_to_rgb565(converter,
                                                                     slot->data,
                                                                     slot->width,
                                                                     slot->height,
                                                                     output_pixels,
                                                                     NULL);
        uint32_t convert_elapsed_us =
            (uint32_t)(esp_timer_get_time() - convert_started_us);
        int64_t convert_finished_us = esp_timer_get_time();
        if (output_index < CALL_VIDEO_OUTPUT_SLOT_COUNT &&
            slot->trace.frame_index > 0U) {
            s_renderer.output_slots[output_index].trace = slot->trace;
            s_renderer.output_slots[output_index].trace.convert_us = convert_elapsed_us;
            s_renderer.output_slots[output_index].trace_ready_at_us = esp_timer_get_time();
        }
        video_frame_converter_stats_t converter_stats = {0};
        video_frame_converter_get_stats(converter, &converter_stats);
        if ((decoded_wait_us >= CALL_VIDEO_LATENCY_RECOVERY_US ||
             convert_elapsed_us >= CALL_VIDEO_LATENCY_RECOVERY_US) &&
            (last_stall_log_at_us == 0 ||
             convert_finished_us - last_stall_log_at_us >=
                 CALL_VIDEO_STALL_LOG_INTERVAL_US)) {
            ESP_LOGW(TAG,
                     "video stall: stage=%s decoded_wait=%luus convert=%luus "
                     "pack_max=%luus ppa_max=%luus swap_max=%luus q=%lu/%u hwm=%u",
                     decoded_wait_us >= CALL_VIDEO_LATENCY_RECOVERY_US ?
                         "convert-schedule" : "convert",
                     (unsigned long)decoded_wait_us,
                     (unsigned long)convert_elapsed_us,
                     (unsigned long)converter_stats.pack_max_us,
                     (unsigned long)converter_stats.ppa_max_us,
                     (unsigned long)converter_stats.swap_max_us,
                     (unsigned long)uxQueueMessagesWaiting(
                         s_renderer.decoded_ready_slots),
                     (unsigned)s_renderer.ready_output_count,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_stall_log_at_us = convert_finished_us;
        }
        taskENTER_CRITICAL(&s_renderer.lock);
        generation = s_renderer.generation;
        s_renderer.convert_pack_time_us = converter_stats.pack_time_us;
        s_renderer.convert_pack_max_us = converter_stats.pack_max_us;
        s_renderer.convert_ppa_time_us = converter_stats.ppa_time_us;
        s_renderer.convert_ppa_max_us = converter_stats.ppa_max_us;
        s_renderer.convert_swap_time_us = converter_stats.swap_time_us;
        s_renderer.convert_swap_max_us = converter_stats.swap_max_us;
        taskEXIT_CRITICAL(&s_renderer.lock);
        bool generation_matches = slot->generation == generation;
        call_video_output_finish_write(output_index,
                                       convert_ret == ESP_OK && generation_matches,
                                       convert_elapsed_us,
                                       slot->pts);
        if (convert_ret == ESP_OK && generation_matches) {
            call_video_log_memory_once(slot->generation,
                                       CALL_VIDEO_MEMORY_TRACE_CONVERT,
                                       "converted");
        }
        if (convert_ret != ESP_OK && generation_matches) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.conversion_failures++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            ESP_LOGW(TAG,
                     "H264 downlink display conversion failed: %s",
                     esp_err_to_name(convert_ret));
        }
        call_video_return_decoded_slot(index);
        /* Conversion and LVGL intentionally share CPU1 and the same priority.
         * Yield to a ready UI owner without adding one mandatory tick to every
         * frame when the converter is the only runnable peer. */
        taskYIELD();
    }

    call_video_drain_decoded_ready_queue();
    xSemaphoreGive(s_renderer.convert_stop_done);
    /*
     * Tasks created with xTaskCreate*WithCaps must be deleted by their owner.
     * Self deletion creates a temporary cleanup task in ESP-IDF and can return
     * the stop semaphore before the stack is actually freed. The H264 owner
     * needs that contiguous memory immediately when restoring the full uplink
     * encoder, so suspend here and let the parent delete this task explicitly.
     */
    vTaskSuspend(NULL);
    abort();
}

static void call_video_renderer_task(void *arg)
{
    (void)arg;
    esp_h264_dec_handle_t decoder = NULL;
    esp_h264_dec_param_sw_handle_t parameters = NULL;
    video_frame_converter_handle_t converter = NULL;
    call_video_log_window_t log_window = {0};
    int64_t last_input_received_at_us = 0;
    int64_t last_decode_completed_at_us = 0;
    int64_t last_stall_log_at_us = 0;
    uint32_t last_trace_generation = UINT32_MAX;
    uint32_t decoder_generation = UINT32_MAX;
    uint32_t idle_window_countdown =
        CALL_VIDEO_DECODE_IDLE_WINDOW_EVERY_FRAMES;
    const video_frame_converter_config_t converter_config = {
        .output_width = CALL_VIDEO_RENDER_WIDTH,
        .output_height = CALL_VIDEO_RENDER_HEIGHT,
        /* Panel orientation is owned by display_driver/LVGL. Keep the decoded
         * landscape picture unrotated and let PPA scale it to the viewport. */
        .source_crop_x = 0,
        .source_crop_y = 0,
        .source_crop_width = CALL_VIDEO_SOURCE_CROP_WIDTH,
        .source_crop_height = CALL_VIDEO_SOURCE_CROP_HEIGHT,
        .fit_mode = VIDEO_FRAME_FIT_CONTAIN,
        .prevent_upscale = false,
        .output_rgb565_byte_swap = true,
    };

    /* Reserve the decoder's internal state before the call profile releases
     * the larger IPC encoder reference block. This keeps first-frame work out
     * of the RTC callback and prevents late allocations from fragmenting that
     * block during a profile transition. */
    bool dma_escrow_lent = false;
    esp_err_t startup_ret = call_video_decoder_dma_guard_begin(
        &dma_escrow_lent,
        "h264-downlink-bootstrap");
    if (startup_ret == ESP_OK) {
        startup_ret = call_video_decoder_create(&decoder, &parameters);
        if (startup_ret == ESP_OK) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.decoder_creations++;
            decoder_generation = s_renderer.generation;
            taskEXIT_CRITICAL(&s_renderer.lock);
        }
    }
    call_video_decoder_dma_guard_end(dma_escrow_lent, startup_ret, "H264");

    if (startup_ret == ESP_OK) {
        startup_ret = video_frame_converter_create(&converter_config, &converter);
    }
    if (startup_ret == ESP_OK) {
        BaseType_t convert_task_ret = xTaskCreatePinnedToCoreWithCaps(
            call_video_converter_task,
            "call_video_cvt",
            CALL_VIDEO_CONVERT_TASK_STACK_SIZE,
            converter,
            CALL_VIDEO_CONVERT_TASK_PRIORITY,
            &s_renderer.convert_task,
            APP_TASK_CORE_VIDEO_CONVERT,
            APP_TASK_STACK_CAPS_BACKGROUND);
        if (convert_task_ret != pdPASS) {
            startup_ret = ESP_ERR_NO_MEM;
        }
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.start_result = startup_ret;
    s_renderer.running = startup_ret == ESP_OK;
    s_renderer.start_pending = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.start_done);

    if (startup_ret != ESP_OK) {
        ESP_LOGE(TAG, "H264 downlink startup failed: %s", esp_err_to_name(startup_ret));
        goto task_exit;
    }

    ESP_LOGI(TAG,
             "H264 downlink renderer ready: decoder=%s conversion=pipelined-%s output=%ux%u "
             "source_crop=%ux%u+%u+%u orientation=panel-owned input_slots=%u input_cap=%u decoded_slots=%u "
             "decoded_cap=%u presentation=latest-%u priorities=decode:%u ingress:%u helper:%u convert:%u "
             "cores=decode:%d helper:%d convert:%d ui:%d camera:%d",
             CALL_VIDEO_H264_DECODER_MODE,
             video_frame_converter_mode_name(video_frame_converter_get_mode(converter)),
             CALL_VIDEO_RENDER_WIDTH,
             CALL_VIDEO_RENDER_HEIGHT,
             CALL_VIDEO_SOURCE_CROP_WIDTH,
             CALL_VIDEO_SOURCE_CROP_HEIGHT,
             CALL_VIDEO_SOURCE_CROP_X,
             CALL_VIDEO_SOURCE_CROP_Y,
             CALL_VIDEO_INPUT_SLOT_COUNT,
             CALL_VIDEO_INPUT_SLOT_CAPACITY,
             CALL_VIDEO_DECODED_SLOT_COUNT,
             CALL_VIDEO_DECODED_SLOT_CAPACITY,
             CALL_VIDEO_OUTPUT_SLOT_COUNT,
             CALL_VIDEO_TASK_PRIORITY,
             CALL_VIDEO_INGRESS_TASK_PRIORITY,
             CALL_VIDEO_H264_HELPER_TASK_PRIORITY,
             CALL_VIDEO_CONVERT_TASK_PRIORITY,
             APP_TASK_CORE_VIDEO_DECODE,
             CALL_VIDEO_H264_HELPER_TASK_CORE,
             APP_TASK_CORE_VIDEO_CONVERT,
             APP_TASK_CORE_UI,
             APP_TASK_CORE_CAMERA);

    while (!call_video_stop_requested()) {
        uint8_t index = 0;
        if (xQueueReceive(s_renderer.ready_slots, &index, pdMS_TO_TICKS(50)) != pdTRUE) {
            call_video_log_stats_if_due(&log_window, converter);
            continue;
        }
        if (index >= CALL_VIDEO_INPUT_SLOT_COUNT) {
            continue;
        }

        call_video_input_slot_t *slot = &s_renderer.slots[index];
        bool generation_matches = false;
        bool waiting_for_key_frame = false;
        taskENTER_CRITICAL(&s_renderer.lock);
        generation_matches = slot->generation == s_renderer.generation;
        waiting_for_key_frame = s_renderer.waiting_for_key_frame;
        taskEXIT_CRITICAL(&s_renderer.lock);

        if (!generation_matches ||
            (waiting_for_key_frame && !slot->key_frame && !slot->decoder_bootstrap)) {
            call_video_return_slot(index);
            continue;
        }

        if (slot->generation != last_trace_generation) {
            /* The renderer stays warm between calls. A new generation is a
             * new timing epoch, so the idle interval must not be reported as
             * a decode/input stall for the first frame of the next peer. */
            last_trace_generation = slot->generation;
            last_input_received_at_us = 0;
            last_decode_completed_at_us = 0;
            last_stall_log_at_us = 0;
            idle_window_countdown =
                CALL_VIDEO_DECODE_IDLE_WINDOW_EVERY_FRAMES;
        }

        int64_t dequeued_at_us = esp_timer_get_time();
        uint32_t queue_age_us = slot->queued_at_us > 0 && dequeued_at_us > slot->queued_at_us ?
                                    (uint32_t)(dequeued_at_us - slot->queued_at_us) : 0U;
        bool latency_recovery_armed = false;
        uint32_t input_queue_depth = call_video_input_queue_depth();
        bool adaptive_playout_active = call_video_adaptive_playout_is_active(dequeued_at_us);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.input_queue_age_samples++;
        s_renderer.input_queue_age_us += queue_age_us;
        if (queue_age_us > s_renderer.input_queue_age_max_us) {
            s_renderer.input_queue_age_max_us = queue_age_us;
        }
        /* Queue age means stale glass-to-glass latency only on the normal
         * latest-frame path. During adaptive weak-network playout the same age
         * is intentional reservoir inventory from a TGTRP recovery burst.
         * Outside that mode, require sustained depth as well as age: a dynamic
         * frame burst can briefly make one dequeued frame old while the decoder
         * still catches up in the next window. */
        if (!adaptive_playout_active &&
            queue_age_us >= CALL_VIDEO_LATENCY_RECOVERY_US &&
            input_queue_depth >= CALL_VIDEO_LATENCY_RECOVERY_DEPTH) {
            if (s_renderer.latency_pressure_samples < UINT8_MAX) {
                s_renderer.latency_pressure_samples++;
            }
        } else if (adaptive_playout_active ||
                   queue_age_us < CALL_VIDEO_LATENCY_RECOVERY_CLEAR_US ||
                   input_queue_depth < CALL_VIDEO_LATENCY_RECOVERY_DEPTH / 2U) {
            s_renderer.latency_pressure_samples = 0U;
        }
        if (s_renderer.latency_pressure_samples >= CALL_VIDEO_LATENCY_RECOVERY_SAMPLES &&
            !s_renderer.latency_recovery_pending) {
            s_renderer.latency_recovery_pending = true;
            s_renderer.latency_pressure_samples = 0U;
            latency_recovery_armed = true;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (latency_recovery_armed) {
            ESP_LOGW(TAG,
                     "H264 latency recovery armed: queue_age=%luus q=%lu sustained=%u; waiting for fresh IDR",
                     (unsigned long)queue_age_us,
                     (unsigned long)input_queue_depth,
                     CALL_VIDEO_LATENCY_RECOVERY_SAMPLES);
        }

        bool dma_escrow_lent = false;
        esp_err_t decode_ret = ESP_OK;
        if (decoder != NULL && decoder_generation != slot->generation) {
            /* A generation is one continuous H264 reference chain. Flushes,
             * peer changes, and IDR recovery must not reuse DPB/SPS/PPS state
             * from the previous chain merely because the worker stayed warm. */
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.decoder_restarts++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            call_video_decoder_destroy(&decoder);
            parameters = NULL;
            decoder_generation = UINT32_MAX;
        }
        if (decoder == NULL) {
            decode_ret = call_video_decoder_dma_guard_begin(
                &dma_escrow_lent,
                "h264-downlink-restart");
            if (decode_ret == ESP_OK) {
                decode_ret = call_video_decoder_create(&decoder, &parameters);
                if (decode_ret == ESP_OK) {
                    taskENTER_CRITICAL(&s_renderer.lock);
                    s_renderer.decoder_creations++;
                    taskEXIT_CRITICAL(&s_renderer.lock);
                    decoder_generation = slot->generation;
                }
            }
            /* Decoder creation is the only operation that borrows the DMA
             * escrow. Restore it and record the memory snapshot once per real
             * bootstrap/restart, never once per decoded frame. */
            call_video_decoder_dma_guard_end(dma_escrow_lent, decode_ret, "H264");
        }
        int64_t access_unit_started_us = esp_timer_get_time();
        slot->trace_decode_started_at_us = access_unit_started_us;
        call_video_log_memory_once(slot->generation,
                                   CALL_VIDEO_MEMORY_TRACE_DECODE_BEGIN,
                                   "decode-begin");
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.decode_in_progress = true;
        s_renderer.decode_started_at_us = access_unit_started_us;
        s_renderer.decode_generation = slot->generation;
        s_renderer.decode_pts = slot->pts;
        s_renderer.decode_payload_bytes = slot->data_len;
        s_renderer.decode_key_frame = slot->key_frame;
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (decode_ret == ESP_OK) {
            decode_ret = call_video_decode_slot(decoder, parameters, slot);
        }
        int64_t access_unit_finished_us = esp_timer_get_time();
        bool decode_timed_out = false;
        taskENTER_CRITICAL(&s_renderer.lock);
        decode_timed_out = s_renderer.faulted && s_renderer.decode_in_progress;
        s_renderer.decode_in_progress = false;
        s_renderer.decode_started_at_us = 0;
        s_renderer.decode_generation = 0U;
        s_renderer.decode_pts = 0U;
        s_renderer.decode_payload_bytes = 0U;
        s_renderer.decode_key_frame = false;
        if (decode_timed_out) {
            /* The call eventually returned, so the decoder can now be
             * destroyed safely and rebuilt from a fresh IDR. */
            s_renderer.faulted = false;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (decode_timed_out) {
            decode_ret = ESP_FAIL;
        }
        uint32_t access_unit_elapsed_us = call_video_elapsed_us(
            access_unit_started_us,
            access_unit_finished_us);
        uint32_t input_gap_us = call_video_elapsed_us(
            last_input_received_at_us,
            slot->trace_received_at_us);
        uint32_t decode_completion_gap_us = call_video_elapsed_us(
            last_decode_completed_at_us,
            access_unit_finished_us);
        last_input_received_at_us = slot->trace_received_at_us;
        last_decode_completed_at_us = access_unit_finished_us;
        uint32_t slot_generation = slot->generation;
        bool slot_key_frame = slot->key_frame;
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.decode_access_units++;
        s_renderer.decode_access_unit_time_us += access_unit_elapsed_us;
        if (access_unit_elapsed_us > s_renderer.decode_access_unit_max_us) {
            s_renderer.decode_access_unit_max_us = access_unit_elapsed_us;
        }
        if (slot_key_frame) {
            s_renderer.decode_key_access_units++;
            s_renderer.decode_key_time_us += access_unit_elapsed_us;
        } else {
            s_renderer.decode_delta_access_units++;
            s_renderer.decode_delta_time_us += access_unit_elapsed_us;
        }
        bool generation_stale = slot_generation != s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);
        bool slow_decode = access_unit_elapsed_us >= CALL_VIDEO_SLOW_DECODE_US;
        bool input_gap = input_gap_us >= CALL_VIDEO_INPUT_GAP_US;
        if (!generation_stale &&
            (slow_decode || input_gap) &&
            (last_stall_log_at_us == 0 ||
             access_unit_finished_us - last_stall_log_at_us >=
                 CALL_VIDEO_STALL_LOG_INTERVAL_US)) {
            uint32_t input_queue_depth = call_video_input_queue_depth();
            uint32_t conversion_queue_depth = s_renderer.decoded_ready_slots != NULL ?
                (uint32_t)uxQueueMessagesWaiting(s_renderer.decoded_ready_slots) : 0U;
            ESP_LOGW(TAG,
                     "video stall: stage=%s decode=%luus input_gap=%luus done_gap=%luus "
                     "queue_age=%luus key=%d bytes=%u q=%lu/%lu hwm=%u",
                     slow_decode ? "decode" : "input",
                     (unsigned long)access_unit_elapsed_us,
                     (unsigned long)input_gap_us,
                     (unsigned long)decode_completion_gap_us,
                     (unsigned long)queue_age_us,
                     slot_key_frame ? 1 : 0,
                     (unsigned)slot->data_len,
                     (unsigned long)input_queue_depth,
                     (unsigned long)conversion_queue_depth,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_stall_log_at_us = access_unit_finished_us;
        }
        call_video_return_slot(index);
        /* The RTC task runs above this worker and already preempts it whenever
         * compressed input arrives. Sleeping after every access unit therefore
         * adds pure latency when high-motion frames consume the full 66.7 ms
         * budget. Only reserve an explicit scheduling window after the input
         * backlog has been drained; while backlog exists, continue decoding so
         * the queue cannot accumulate one tick per frame. */
        uint32_t queued_after_decode = call_video_input_queue_depth();
        if (idle_window_countdown > 0U) {
            idle_window_countdown--;
        }
        if (idle_window_countdown == 0U) {
            /* IDLE0 must run before the five-second TWDT deadline even while
             * continuous motion keeps compressed input queued. Reserve a short
             * window every two seconds; a 20 ms window every second consumed
             * the entire remaining budget when TinyH264 needed about 66 ms per
             * high-motion access unit. */
            vTaskDelay(pdMS_TO_TICKS(CALL_VIDEO_DECODE_IDLE_WINDOW_MS));
            idle_window_countdown =
                CALL_VIDEO_DECODE_IDLE_WINDOW_EVERY_FRAMES;
        } else if (queued_after_decode == 0U) {
            uint32_t scheduling_window_ms =
                CALL_VIDEO_DECODE_SCHEDULING_WINDOW_MS;
            vTaskDelay(pdMS_TO_TICKS(scheduling_window_ms));
        }
        if (generation_stale) {
            if (decoder != NULL) {
                taskENTER_CRITICAL(&s_renderer.lock);
                s_renderer.decoder_restarts++;
                taskEXIT_CRITICAL(&s_renderer.lock);
            }
            call_video_decoder_destroy(&decoder);
            parameters = NULL;
            decoder_generation = UINT32_MAX;
            call_video_log_stats_if_due(&log_window, converter);
            continue;
        }
        if (decode_ret != ESP_OK) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.decode_failures++;
            if (decoder != NULL) {
                s_renderer.decoder_restarts++;
            }
            taskEXIT_CRITICAL(&s_renderer.lock);
            ESP_LOGW(TAG, "H264 downlink decode lost sync: ret=%s", esp_err_to_name(decode_ret));
            call_video_mark_discontinuity();
            call_video_decoder_destroy(&decoder);
            parameters = NULL;
            decoder_generation = UINT32_MAX;
        }
        call_video_log_stats_if_due(&log_window, converter);
    }

task_exit:
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "heap integrity failed before H264 downlink teardown");
    }
    call_video_decoder_destroy(&decoder);
    parameters = NULL;
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "heap integrity failed after H264 decoder teardown");
    }
    call_video_drain_decoded_ready_queue();
    TaskHandle_t convert_task = NULL;
    taskENTER_CRITICAL(&s_renderer.lock);
    convert_task = s_renderer.convert_task;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (convert_task != NULL) {
        if (xSemaphoreTake(s_renderer.convert_stop_done,
                           pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) == pdTRUE) {
            vTaskDeleteWithCaps(convert_task);
            taskENTER_CRITICAL(&s_renderer.lock);
            if (s_renderer.convert_task == convert_task) {
                s_renderer.convert_task = NULL;
            }
            taskEXIT_CRITICAL(&s_renderer.lock);
        } else {
            ESP_LOGE(TAG, "H264 downlink converter stop timed out");
        }
    }
    video_frame_converter_destroy(converter);
    converter = NULL;
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.running = false;
    s_renderer.decode_in_progress = false;
    s_renderer.decode_started_at_us = 0;
    s_renderer.decode_generation = 0U;
    s_renderer.decode_pts = 0U;
    s_renderer.decode_payload_bytes = 0U;
    s_renderer.decode_key_frame = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.stop_done);
    vTaskSuspend(NULL);
    abort();
}

typedef struct {
    int64_t last_log_us;
    uint32_t received_frames;
    uint64_t received_bytes;
    uint32_t decoded_frames;
    uint32_t presented_frames;
    uint32_t dropped_frames;
    uint32_t decode_failures;
    uint64_t decode_time_us;
    uint32_t decode_process_calls;
} call_video_mjpeg_log_window_t;

static void call_video_log_mjpeg_stats_if_due(call_video_mjpeg_log_window_t *window)
{
#if !CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
    (void)window;
    return;
#else
    int64_t now_us = esp_timer_get_time();
    if (window->last_log_us == 0) {
        window->last_log_us = now_us;
        return;
    }
    int64_t elapsed_us = now_us - window->last_log_us;
    if (elapsed_us < CALL_VIDEO_STATS_INTERVAL_US) {
        return;
    }

    call_video_mjpeg_log_window_t current = {
        .last_log_us = now_us,
    };
    uint16_t source_width = 0U;
    uint16_t source_height = 0U;
    taskENTER_CRITICAL(&s_renderer.lock);
    current.received_frames = s_renderer.received_frames;
    current.received_bytes = s_renderer.received_bytes;
    current.decoded_frames = s_renderer.decoded_frames;
    current.presented_frames = s_renderer.presented_frames;
    current.dropped_frames = s_renderer.dropped_frames;
    current.decode_failures = s_renderer.decode_failures;
    current.decode_time_us = s_renderer.decode_time_us;
    current.decode_process_calls = s_renderer.decode_process_calls;
    source_width = s_renderer.source_width;
    source_height = s_renderer.source_height;
    taskEXIT_CRITICAL(&s_renderer.lock);

    uint32_t received_delta = current.received_frames - window->received_frames;
    uint64_t bytes_delta = current.received_bytes - window->received_bytes;
    uint32_t decoded_delta = current.decoded_frames - window->decoded_frames;
    uint32_t presented_delta = current.presented_frames - window->presented_frames;
    uint32_t dropped_delta = current.dropped_frames - window->dropped_frames;
    uint32_t failure_delta = current.decode_failures - window->decode_failures;
    uint64_t decode_time_delta = current.decode_time_us - window->decode_time_us;
    uint32_t process_delta =
        current.decode_process_calls - window->decode_process_calls;
    uint32_t received_fps_x10 =
        (uint32_t)(((uint64_t)received_delta * 10000000ULL) / (uint64_t)elapsed_us);
    uint32_t decoded_fps_x10 =
        (uint32_t)(((uint64_t)decoded_delta * 10000000ULL) / (uint64_t)elapsed_us);
    uint32_t presented_fps_x10 =
        (uint32_t)(((uint64_t)presented_delta * 10000000ULL) / (uint64_t)elapsed_us);
    uint32_t received_kbps =
        (uint32_t)((bytes_delta * 8ULL * 1000ULL) / (uint64_t)elapsed_us);
    uint32_t average_decode_us =
        process_delta > 0U ? (uint32_t)(decode_time_delta / process_delta) : 0U;
    uint32_t queue_depth = call_video_input_queue_depth();

    ESP_LOGI(TAG,
             "MJPEG downlink stats: src=%ux%u rx=%u.%ufps/%ukbps "
             "decoded=%u.%ufps presented=%u.%ufps drop=%u fail=%u "
             "decode_us=%u q=%u",
             source_width,
             source_height,
             received_fps_x10 / 10U,
             received_fps_x10 % 10U,
             received_kbps,
             decoded_fps_x10 / 10U,
             decoded_fps_x10 % 10U,
             presented_fps_x10 / 10U,
             presented_fps_x10 % 10U,
             dropped_delta,
             failure_delta,
             average_decode_us,
             queue_depth);
    *window = current;
#endif
}

static void call_video_mjpeg_renderer_task(void *arg)
{
    (void)arg;
    jpeg_decoder_handle_t decoder = NULL;
    video_frame_converter_handle_t converter = NULL;
    uint8_t *decode_buffer = NULL;
    call_video_mjpeg_log_window_t log_window = {0};
    int64_t last_decode_warning_us = 0;
    int64_t last_geometry_log_us = 0;
    int64_t first_frame_ready_us = 0;
    bool first_frame_logged = false;
    bool startup_sample_logged = false;
    uint8_t geometry_change_logs = 0U;
    uint32_t published_frames = 0U;
    uint16_t last_source_width = 0U;
    uint16_t last_source_height = 0U;
    video_frame_rotation_t last_display_rotation =
        VIDEO_FRAME_ROTATION_CLOCKWISE_0;
    uint16_t last_crop_x = 0U;
    uint16_t last_crop_y = 0U;
    uint16_t last_crop_width = 0U;
    uint16_t last_crop_height = 0U;
    uint16_t last_render_width = 0U;
    uint16_t last_render_height = 0U;
    uint16_t last_offset_x = 0U;
    uint16_t last_offset_y = 0U;
    const jpeg_decode_cfg_t decode_config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* Keep the decoder's panel-order big-endian RGB565. The converter
         * normalizes it at PPA input and restores panel order at output. */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    taskENTER_CRITICAL(&s_renderer.lock);
    decoder = s_renderer.mjpeg_decoder;
    converter = s_renderer.mjpeg_converter;
    decode_buffer = s_renderer.mjpeg_decode_buffer;
    taskEXIT_CRITICAL(&s_renderer.lock);
    esp_err_t startup_ret =
        decoder != NULL && converter != NULL && decode_buffer != NULL ?
            ESP_OK :
            ESP_ERR_INVALID_STATE;

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.start_result = startup_ret;
    s_renderer.running = startup_ret == ESP_OK;
    s_renderer.start_pending = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.start_done);

    if (startup_ret != ESP_OK) {
        ESP_LOGE(TAG, "MJPEG downlink startup failed: %s", esp_err_to_name(startup_ret));
        goto task_exit;
    }

    ESP_LOGI(TAG,
             "MJPEG downlink renderer ready: decoder=hardware-jpeg-rgb565 "
             "rotation=cw90 source_rotation=not-signaled "
             "scale=%s fit=%s upscale=%s output=RGB565-%ux%u "
             "source_cap=%upx/%u-edge "
             "input_slots=%u input_cap=%u presentation=latest-%u core=%d",
             video_frame_converter_mode_name(
                 video_frame_converter_get_mode(converter)),
             video_frame_fit_mode_name(CALL_VIDEO_MJPEG_FIT_MODE),
             CALL_VIDEO_MJPEG_PREVENT_UPSCALE ? "deny" : "allow",
             CALL_VIDEO_RENDER_WIDTH,
             CALL_VIDEO_RENDER_HEIGHT,
             CALL_VIDEO_MJPEG_MAX_PIXELS,
             CALL_VIDEO_MJPEG_MAX_EDGE,
             CALL_VIDEO_INPUT_SLOT_COUNT,
             CALL_VIDEO_INPUT_SLOT_CAPACITY,
             CALL_VIDEO_OUTPUT_SLOT_COUNT,
             APP_TASK_CORE_VIDEO_DECODE);

    while (!call_video_stop_requested()) {
        uint8_t index = 0;
        if (xQueueReceive(s_renderer.ready_slots, &index, pdMS_TO_TICKS(50)) != pdTRUE) {
            call_video_log_mjpeg_stats_if_due(&log_window);
            continue;
        }
        if (index >= CALL_VIDEO_INPUT_SLOT_COUNT) {
            continue;
        }

        /* MJPEG frames are independently decodable. Decode the newest queued
         * frame and release stale compressed frames instead of building
         * latency when the network arrives in a burst. */
        uint32_t stale_frames = 0U;
        uint8_t newer_index = 0U;
        while (xQueueReceive(s_renderer.ready_slots, &newer_index, 0) == pdTRUE) {
            call_video_return_slot(index);
            index = newer_index;
            stale_frames++;
        }
        if (stale_frames > 0U) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.dropped_frames += stale_frames;
            taskEXIT_CRITICAL(&s_renderer.lock);
        }
        if (index >= CALL_VIDEO_INPUT_SLOT_COUNT) {
            continue;
        }

        call_video_input_slot_t *slot = &s_renderer.slots[index];
        size_t compressed_size = slot->data_len;
        uint32_t generation = 0U;
        taskENTER_CRITICAL(&s_renderer.lock);
        generation = s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);
        if (slot->generation != generation) {
            call_video_return_slot(index);
            continue;
        }

        int64_t dequeued_at_us = esp_timer_get_time();
        uint32_t queue_age_us =
            slot->queued_at_us > 0 && dequeued_at_us > slot->queued_at_us ?
                (uint32_t)(dequeued_at_us - slot->queued_at_us) : 0U;
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.input_queue_age_samples++;
        s_renderer.input_queue_age_us += queue_age_us;
        if (queue_age_us > s_renderer.input_queue_age_max_us) {
            s_renderer.input_queue_age_max_us = queue_age_us;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);

        jpeg_decode_picture_info_t picture = {0};
        esp_err_t decode_ret = jpeg_decoder_get_info(slot->data,
                                                     (uint32_t)slot->data_len,
                                                     &picture);
        uint32_t aligned_width = 0U;
        uint32_t aligned_height = 0U;
        size_t expected_output_size = 0U;
        video_frame_rotation_t display_rotation =
            VIDEO_FRAME_ROTATION_CLOCKWISE_90;
        if (decode_ret == ESP_OK) {
            if (picture.width == 0U || picture.height == 0U ||
                picture.width > CALL_VIDEO_MJPEG_MAX_EDGE ||
                picture.height > CALL_VIDEO_MJPEG_MAX_EDGE) {
                decode_ret = ESP_ERR_INVALID_SIZE;
            } else {
                aligned_width =
                    (picture.width + CALL_VIDEO_MJPEG_DIMENSION_ALIGNMENT - 1U) &
                    ~(CALL_VIDEO_MJPEG_DIMENSION_ALIGNMENT - 1U);
                aligned_height =
                    (picture.height + CALL_VIDEO_MJPEG_DIMENSION_ALIGNMENT - 1U) &
                    ~(CALL_VIDEO_MJPEG_DIMENSION_ALIGNMENT - 1U);
                uint64_t aligned_pixels =
                    (uint64_t)aligned_width * (uint64_t)aligned_height;
                if (aligned_pixels > CALL_VIDEO_MJPEG_MAX_PIXELS) {
                    decode_ret = ESP_ERR_INVALID_SIZE;
                } else {
                    expected_output_size =
                        (size_t)aligned_pixels * sizeof(uint16_t);
                }
            }
        }

        uint32_t output_size = 0U;
        bool decode_process_called = false;
        int64_t decode_started_us = esp_timer_get_time();
        if (decode_ret == ESP_OK) {
            decode_process_called = true;
            decode_ret = jpeg_decoder_process(decoder,
                                              &decode_config,
                                              slot->data,
                                              (uint32_t)slot->data_len,
                                              decode_buffer,
                                              CALL_VIDEO_MJPEG_DECODE_BYTES,
                                              &output_size);
            if (decode_ret == ESP_OK && output_size != expected_output_size) {
                decode_ret = ESP_ERR_INVALID_SIZE;
            }
        }
        uint32_t decode_elapsed_us =
            (uint32_t)(esp_timer_get_time() - decode_started_us);

        taskENTER_CRITICAL(&s_renderer.lock);
        bool generation_matches = slot->generation == s_renderer.generation;
        if (decode_process_called) {
            s_renderer.decode_process_calls++;
            s_renderer.decode_time_us += decode_elapsed_us;
        }
        s_renderer.decode_access_units++;
        s_renderer.decode_access_unit_time_us += decode_elapsed_us;
        if (decode_elapsed_us > s_renderer.decode_access_unit_max_us) {
            s_renderer.decode_access_unit_max_us = decode_elapsed_us;
        }
        if (decode_ret == ESP_OK && generation_matches) {
            s_renderer.source_width = (uint16_t)picture.width;
            s_renderer.source_height = (uint16_t)picture.height;
            s_renderer.decoded_frames++;
            s_renderer.waiting_for_key_frame = false;
        } else if (generation_matches) {
            s_renderer.decode_failures++;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);

        uint8_t output_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;
        uint16_t *output_pixels = NULL;
        esp_err_t convert_ret = ESP_ERR_INVALID_STATE;
        uint32_t convert_elapsed_us = 0U;
        bool convert_attempted = false;
        video_frame_converter_mode_t convert_mode =
            VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;
        if (decode_ret == ESP_OK && generation_matches) {
            convert_ret =
                call_video_output_begin_write(&output_index, &output_pixels);
            if (convert_ret != ESP_OK) {
                taskENTER_CRITICAL(&s_renderer.lock);
                s_renderer.conversion_dropped_frames++;
                taskEXIT_CRITICAL(&s_renderer.lock);
            }
        }
        if (convert_ret == ESP_OK) {
            convert_attempted = true;
            int64_t convert_started_us = esp_timer_get_time();
            convert_ret = video_frame_converter_rgb565_to_rgb565(
                converter,
                (const uint16_t *)decode_buffer,
                (uint16_t)picture.width,
                (uint16_t)picture.height,
                (uint16_t)aligned_width,
                true,
                display_rotation,
                output_pixels,
                &convert_mode);
            convert_elapsed_us =
                (uint32_t)(esp_timer_get_time() - convert_started_us);
            taskENTER_CRITICAL(&s_renderer.lock);
            generation_matches = slot->generation == s_renderer.generation;
            taskEXIT_CRITICAL(&s_renderer.lock);
        }
        if (output_index != CALL_VIDEO_OUTPUT_SLOT_INVALID) {
            bool publish = convert_ret == ESP_OK && generation_matches;
            call_video_output_finish_write(output_index,
                                           publish,
                                           convert_elapsed_us,
                                           slot->pts);
            if (publish) {
                video_frame_converter_stats_t converter_stats = {0};
                video_frame_converter_get_stats(converter, &converter_stats);
                bool geometry_changed =
                    last_source_width != picture.width ||
                    last_source_height != picture.height ||
                    last_display_rotation != display_rotation ||
                    last_crop_x != converter_stats.last_crop_x ||
                    last_crop_y != converter_stats.last_crop_y ||
                    last_crop_width != converter_stats.last_crop_width ||
                    last_crop_height != converter_stats.last_crop_height ||
                    last_render_width != converter_stats.last_render_width ||
                    last_render_height != converter_stats.last_render_height ||
                    last_offset_x != converter_stats.last_offset_x ||
                    last_offset_y != converter_stats.last_offset_y;
                int64_t now_us = esp_timer_get_time();
                published_frames++;

                if (!first_frame_logged) {
                    first_frame_logged = true;
                    first_frame_ready_us = now_us;
                    if (picture.width < CALL_VIDEO_RENDER_WIDTH ||
                        picture.height < CALL_VIDEO_RENDER_HEIGHT) {
                        ESP_LOGI(TAG,
                                 "MJPEG downlink source below panel: "
                                 "source=%ux%u render=%ux%u; "
                                 "uniform upscale active",
                                 (unsigned)picture.width,
                                 (unsigned)picture.height,
                                 (unsigned)converter_stats.last_render_width,
                                 (unsigned)converter_stats.last_render_height);
                    }
                    ESP_LOGI(TAG,
                             "MJPEG downlink first frame ready: source=%ux%u "
                             "stride=%u rotation=%s scale=%s fit=%s "
                             "crop=%ux%u+%u+%u render=%ux%u+%u+%u "
                             "output=%ux%u payload=%u pts=%u "
                             "decode=%uus convert=%uus",
                             (unsigned)picture.width,
                             (unsigned)picture.height,
                             (unsigned)aligned_width,
                             video_frame_rotation_name(display_rotation),
                             video_frame_converter_mode_name(convert_mode),
                             video_frame_fit_mode_name(CALL_VIDEO_MJPEG_FIT_MODE),
                             (unsigned)converter_stats.last_crop_width,
                             (unsigned)converter_stats.last_crop_height,
                             (unsigned)converter_stats.last_crop_x,
                             (unsigned)converter_stats.last_crop_y,
                             (unsigned)converter_stats.last_render_width,
                             (unsigned)converter_stats.last_render_height,
                             (unsigned)converter_stats.last_offset_x,
                             (unsigned)converter_stats.last_offset_y,
                             CALL_VIDEO_RENDER_WIDTH,
                             CALL_VIDEO_RENDER_HEIGHT,
                             (unsigned)compressed_size,
                             (unsigned)slot->pts,
                             (unsigned)decode_elapsed_us,
                             (unsigned)convert_elapsed_us);
                    last_geometry_log_us = now_us;
                } else if (geometry_changed &&
                           (geometry_change_logs <
                                CALL_VIDEO_MJPEG_GEOMETRY_LOG_LIMIT ||
                            now_us - last_geometry_log_us >=
                                2LL * 1000LL * 1000LL)) {
                    ESP_LOGI(TAG,
                             "MJPEG downlink geometry changed: source=%ux%u "
                             "rotation=%s fit=%s crop=%ux%u+%u+%u "
                             "render=%ux%u+%u+%u age=%llums frame=%u "
                             "payload=%u pts=%u",
                             (unsigned)picture.width,
                             (unsigned)picture.height,
                             video_frame_rotation_name(display_rotation),
                             video_frame_fit_mode_name(CALL_VIDEO_MJPEG_FIT_MODE),
                             (unsigned)converter_stats.last_crop_width,
                             (unsigned)converter_stats.last_crop_height,
                             (unsigned)converter_stats.last_crop_x,
                             (unsigned)converter_stats.last_crop_y,
                             (unsigned)converter_stats.last_render_width,
                             (unsigned)converter_stats.last_render_height,
                             (unsigned)converter_stats.last_offset_x,
                             (unsigned)converter_stats.last_offset_y,
                             first_frame_ready_us != 0 &&
                                     now_us >= first_frame_ready_us ?
                                 (unsigned long long)(
                                     (now_us - first_frame_ready_us) / 1000LL) :
                                 0ULL,
                             (unsigned)published_frames,
                             (unsigned)compressed_size,
                             (unsigned)slot->pts);
                    last_geometry_log_us = now_us;
                    if (geometry_change_logs < UINT8_MAX) {
                        geometry_change_logs++;
                    }
                    if (first_frame_ready_us != 0 &&
                        now_us - first_frame_ready_us >=
                            CALL_VIDEO_MJPEG_STARTUP_SAMPLE_US) {
                        startup_sample_logged = true;
                    }
                }

                if (!startup_sample_logged &&
                    first_frame_ready_us != 0 &&
                    now_us - first_frame_ready_us >=
                        CALL_VIDEO_MJPEG_STARTUP_SAMPLE_US) {
                    startup_sample_logged = true;
                    ESP_LOGI(TAG,
                             "MJPEG downlink startup sample: source=%ux%u "
                             "rotation=%s crop=%ux%u+%u+%u "
                             "render=%ux%u+%u+%u age=%llums frame=%u "
                             "payload=%u pts=%u geometry_changed=%d",
                             (unsigned)picture.width,
                             (unsigned)picture.height,
                             video_frame_rotation_name(display_rotation),
                             (unsigned)converter_stats.last_crop_width,
                             (unsigned)converter_stats.last_crop_height,
                             (unsigned)converter_stats.last_crop_x,
                             (unsigned)converter_stats.last_crop_y,
                             (unsigned)converter_stats.last_render_width,
                             (unsigned)converter_stats.last_render_height,
                             (unsigned)converter_stats.last_offset_x,
                             (unsigned)converter_stats.last_offset_y,
                             (unsigned long long)(
                                 (now_us - first_frame_ready_us) / 1000LL),
                             (unsigned)published_frames,
                             (unsigned)compressed_size,
                             (unsigned)slot->pts,
                             geometry_changed ? 1 : 0);
                }

                /*
                 * Geometry state tracks every published frame, not only a
                 * throttled log entry. This makes the next transition report
                 * the actual before/after layout instead of an old baseline.
                 */
                last_source_width = (uint16_t)picture.width;
                last_source_height = (uint16_t)picture.height;
                last_display_rotation = display_rotation;
                last_crop_x = converter_stats.last_crop_x;
                last_crop_y = converter_stats.last_crop_y;
                last_crop_width = converter_stats.last_crop_width;
                last_crop_height = converter_stats.last_crop_height;
                last_render_width = converter_stats.last_render_width;
                last_render_height = converter_stats.last_render_height;
                last_offset_x = converter_stats.last_offset_x;
                last_offset_y = converter_stats.last_offset_y;
            }
        }
        if (convert_attempted && convert_ret != ESP_OK && generation_matches) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.conversion_failures++;
            taskEXIT_CRITICAL(&s_renderer.lock);
        }
        call_video_return_slot(index);

        if ((decode_ret != ESP_OK ||
             (convert_attempted && convert_ret != ESP_OK)) &&
            generation_matches) {
            int64_t now_us = esp_timer_get_time();
            if (last_decode_warning_us == 0 ||
                now_us - last_decode_warning_us >= 2LL * 1000LL * 1000LL) {
                ESP_LOGW(TAG,
                         "MJPEG downlink frame rejected: stage=%s ret=%s "
                         "compressed=%u source=%ux%u "
                         "cap=%upx/%u-edge aligned=%ux%u output=%ux%u",
                         decode_ret != ESP_OK ? "decode" : "scale",
                         esp_err_to_name(decode_ret != ESP_OK ?
                                             decode_ret :
                                             convert_ret),
                         (unsigned)compressed_size,
                         (unsigned)picture.width,
                         (unsigned)picture.height,
                         CALL_VIDEO_MJPEG_MAX_PIXELS,
                         CALL_VIDEO_MJPEG_MAX_EDGE,
                         (unsigned)aligned_width,
                         (unsigned)aligned_height,
                         CALL_VIDEO_RENDER_WIDTH,
                         CALL_VIDEO_RENDER_HEIGHT);
                last_decode_warning_us = now_us;
            }
        }
        call_video_log_mjpeg_stats_if_due(&log_window);
    }

task_exit:
    decoder = NULL;
    converter = NULL;
    decode_buffer = NULL;
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.running = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.stop_done);
    vTaskSuspend(NULL);
    abort();
}

static esp_err_t call_video_prepare_output_pool(void)
{
    for (uint8_t index = 0; index < CALL_VIDEO_OUTPUT_SLOT_COUNT; ++index) {
        if (s_renderer.output_slots[index].pixels != NULL) {
            continue;
        }
        s_renderer.output_slots[index].pixels = app_memory_aligned_calloc_psram(
            CALL_VIDEO_CACHE_LINE_SIZE,
            1,
            CALL_VIDEO_FRAME_BYTES,
            MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
        if (s_renderer.output_slots[index].pixels == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_renderer.output_slots[index].state = CALL_VIDEO_OUTPUT_FREE;
    }
    return ESP_OK;
}

static void call_video_reset_output_pool_for_start(void)
{
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    call_video_output_release_ready_locked();
    for (uint8_t index = 0; index < CALL_VIDEO_OUTPUT_SLOT_COUNT; ++index) {
        if (index != s_renderer.presented_output_slot) {
            s_renderer.output_slots[index].state = CALL_VIDEO_OUTPUT_FREE;
        }
    }
    xSemaphoreGive(s_renderer.frame_mutex);
}

static void call_video_reset_session_queues(bool populate)
{
    if (s_renderer.free_slots != NULL) {
        (void)xQueueReset(s_renderer.free_slots);
    }
    if (s_renderer.ingress_slots != NULL) {
        (void)xQueueReset(s_renderer.ingress_slots);
    }
    if (s_renderer.ready_slots != NULL) {
        (void)xQueueReset(s_renderer.ready_slots);
    }
    if (s_renderer.decoded_free_slots != NULL) {
        (void)xQueueReset(s_renderer.decoded_free_slots);
    }
    if (s_renderer.decoded_ready_slots != NULL) {
        (void)xQueueReset(s_renderer.decoded_ready_slots);
    }

    for (uint8_t index = 0; index < CALL_VIDEO_INPUT_SLOT_COUNT; ++index) {
        uint8_t *data = s_renderer.slots[index].data;
        s_renderer.slots[index] = (call_video_input_slot_t) {
            .data = data,
        };
        if (populate && s_renderer.free_slots != NULL) {
            (void)xQueueSend(s_renderer.free_slots, &index, 0);
        }
    }
    for (uint8_t index = 0; index < CALL_VIDEO_DECODED_SLOT_COUNT; ++index) {
        uint8_t *data = s_renderer.decoded_slots[index].data;
        s_renderer.decoded_slots[index] = (call_video_decoded_slot_t) {
            .data = data,
        };
        if (populate && s_renderer.decoded_free_slots != NULL) {
            (void)xQueueSend(s_renderer.decoded_free_slots, &index, 0);
        }
    }
}

static esp_err_t call_video_prepare_persistent_resources(void)
{
    const video_frame_converter_config_t mjpeg_converter_config = {
        .output_width = CALL_VIDEO_RENDER_WIDTH,
        .output_height = CALL_VIDEO_RENDER_HEIGHT,
        /*
         * The cloud may change the JPEG source size, but the display contract
         * stays fixed at 480x320. Crop symmetrically and apply one uniform PPA
         * scale without a second display-side resize or aspect-ratio
         * distortion. Adaptive low-resolution renditions are allowed to scale
         * up so a 320-pixel-wide frame fills the 480-pixel-wide viewport.
         */
        .source_crop_x = 0U,
        .source_crop_y = 0U,
        .source_crop_width = 0U,
        .source_crop_height = 0U,
        .fit_mode = CALL_VIDEO_MJPEG_FIT_MODE,
        .prevent_upscale = CALL_VIDEO_MJPEG_PREVENT_UPSCALE,
        .output_rgb565_byte_swap = true,
    };

    if (s_renderer.frame_mutex == NULL) {
        s_renderer.frame_mutex = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.submit_mutex == NULL) {
        s_renderer.submit_mutex = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.free_slots == NULL) {
        s_renderer.free_slots = xQueueCreateWithCaps(CALL_VIDEO_INPUT_SLOT_COUNT,
                                                     sizeof(uint8_t),
                                                     APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.ingress_slots == NULL) {
        s_renderer.ingress_slots = xQueueCreateWithCaps(CALL_VIDEO_INPUT_SLOT_COUNT,
                                                        sizeof(uint8_t),
                                                        APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.ready_slots == NULL) {
        s_renderer.ready_slots = xQueueCreateWithCaps(CALL_VIDEO_INPUT_SLOT_COUNT,
                                                      sizeof(uint8_t),
                                                      APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.decoded_free_slots == NULL) {
        s_renderer.decoded_free_slots = xQueueCreateWithCaps(CALL_VIDEO_DECODED_SLOT_COUNT,
                                                             sizeof(uint8_t),
                                                             APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.decoded_ready_slots == NULL) {
        s_renderer.decoded_ready_slots = xQueueCreateWithCaps(CALL_VIDEO_DECODED_SLOT_COUNT,
                                                              sizeof(uint8_t),
                                                              APP_QUEUE_CAPS_CONTROL);
    }
    if (s_renderer.start_done == NULL) {
        s_renderer.start_done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.stop_done == NULL) {
        s_renderer.stop_done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.ingress_stop_done == NULL) {
        s_renderer.ingress_stop_done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.convert_stop_done == NULL) {
        s_renderer.convert_stop_done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_renderer.free_slots == NULL || s_renderer.ingress_slots == NULL ||
        s_renderer.ready_slots == NULL ||
        s_renderer.decoded_free_slots == NULL || s_renderer.decoded_ready_slots == NULL ||
        s_renderer.frame_mutex == NULL || s_renderer.submit_mutex == NULL ||
        s_renderer.start_done == NULL ||
        s_renderer.stop_done == NULL || s_renderer.ingress_stop_done == NULL ||
        s_renderer.convert_stop_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t index = 0; index < CALL_VIDEO_INPUT_SLOT_COUNT; ++index) {
        if (s_renderer.slots[index].data == NULL) {
            s_renderer.slots[index].data = app_memory_alloc_psram(CALL_VIDEO_INPUT_SLOT_CAPACITY);
        }
        if (s_renderer.slots[index].data == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    for (uint8_t index = 0; index < CALL_VIDEO_DECODED_SLOT_COUNT; ++index) {
        if (s_renderer.decoded_slots[index].data == NULL) {
            s_renderer.decoded_slots[index].data =
                app_memory_alloc_psram(CALL_VIDEO_DECODED_SLOT_CAPACITY);
        }
        if (s_renderer.decoded_slots[index].data == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_renderer.mjpeg_decode_buffer == NULL) {
        s_renderer.mjpeg_decode_buffer = app_memory_aligned_calloc_psram(
            CALL_VIDEO_CACHE_LINE_SIZE,
            1,
            CALL_VIDEO_MJPEG_DECODE_BYTES,
            MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
    }
    if (s_renderer.mjpeg_decode_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_renderer.mjpeg_converter == NULL) {
        ESP_RETURN_ON_ERROR(video_frame_converter_create(
                                &mjpeg_converter_config,
                                &s_renderer.mjpeg_converter),
                            TAG,
                            "create persistent MJPEG scaler failed");
    }

    return call_video_prepare_output_pool();
}

esp_err_t call_video_renderer_prewarm(void)
{
    bool already_ready = false;

    taskENTER_CRITICAL(&s_renderer.lock);
    already_ready = s_renderer.resources_ready;
    if (s_renderer.resources_preparing) {
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!already_ready) {
        s_renderer.resources_preparing = true;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (already_ready) {
        return ESP_OK;
    }

    esp_err_t ret = call_video_prepare_persistent_resources();
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.resources_preparing = false;
    s_renderer.resources_ready = ret == ESP_OK;
    taskEXIT_CRITICAL(&s_renderer.lock);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "call video PSRAM pools reserved: input=%u h264_decoded=%u "
                 "rgb=%u mjpeg_decode=%u mjpeg_scale=%u total=%u "
                 "psram_free=%u psram_largest=%u internal_largest=%u",
                 (unsigned)(CALL_VIDEO_INPUT_SLOT_COUNT * CALL_VIDEO_INPUT_SLOT_CAPACITY),
                 (unsigned)(CALL_VIDEO_DECODED_SLOT_COUNT * CALL_VIDEO_DECODED_SLOT_CAPACITY),
                 (unsigned)(CALL_VIDEO_OUTPUT_SLOT_COUNT * CALL_VIDEO_FRAME_BYTES),
                 (unsigned)CALL_VIDEO_MJPEG_DECODE_BYTES,
                 (unsigned)CALL_VIDEO_MJPEG_PPA_STAGING_BYTES,
                 (unsigned)CALL_VIDEO_PSRAM_POOL_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return ret;
}

esp_err_t call_video_renderer_start_for_codec(call_video_codec_t codec)
{
    ESP_RETURN_ON_FALSE(codec == CALL_VIDEO_CODEC_H264 ||
                            codec == CALL_VIDEO_CODEC_MJPEG,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "unsupported call video codec");
    ESP_RETURN_ON_ERROR(call_video_renderer_prewarm(), TAG, "reserve call video pools failed");
    if (codec == CALL_VIDEO_CODEC_MJPEG) {
        ESP_RETURN_ON_ERROR(call_video_renderer_prewarm_mjpeg_decoder(),
                            TAG,
                            "reserve persistent MJPEG decoder failed");
    }

    if (call_video_renderer_detect_decode_fault(esp_timer_get_time())) {
        ESP_LOGE(TAG, "%s downlink renderer is quarantined", call_video_codec_name(codec));
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    if (s_renderer.faulted) {
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_renderer.running || s_renderer.start_pending) {
        bool same_codec = s_renderer.codec == codec;
        taskEXIT_CRITICAL(&s_renderer.lock);
        return same_codec ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_renderer.task != NULL || s_renderer.ingress_task != NULL) {
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_renderer.codec = codec;
    s_renderer.start_pending = true;
    s_renderer.stop_requested = false;
    s_renderer.faulted = false;
    s_renderer.decode_in_progress = false;
    s_renderer.decode_started_at_us = 0;
    s_renderer.decode_generation = 0U;
    s_renderer.decode_pts = 0U;
    s_renderer.decode_payload_bytes = 0U;
    s_renderer.decode_key_frame = false;
    s_renderer.waiting_for_key_frame = codec == CALL_VIDEO_CODEC_H264;
    s_renderer.latency_recovery_pending = false;
    s_renderer.latency_pressure_samples = 0U;
    call_video_reset_h264_stream_state_locked();
    s_renderer.frame_ready = false;
    s_renderer.session_active = false;
    s_renderer.start_result = ESP_ERR_INVALID_STATE;
    s_renderer.generation++;
    s_renderer.source_width = 0;
    s_renderer.source_height = 0;
    s_renderer.startup_trace_frames = 0;
    s_renderer.received_frames = 0;
    s_renderer.received_bytes = 0;
    s_renderer.submitted_frames = 0;
    s_renderer.decoded_frames = 0;
    s_renderer.converted_frames = 0;
    s_renderer.conversion_dropped_frames = 0;
    s_renderer.conversion_failures = 0;
    s_renderer.dropped_frames = 0;
    s_renderer.decode_failures = 0;
    s_renderer.latest_sequence = 0;
    s_renderer.presented_frames = 0;
    s_renderer.stale_received_frames = 0;
    s_renderer.stale_presented_frames = 0;
    s_renderer.received_pts_valid = false;
    s_renderer.received_pts = 0;
    s_renderer.presented_pts_valid = false;
    s_renderer.presented_pts = 0;
    s_renderer.last_received_at_us = 0;
    s_renderer.receive_gap_window_max_us = 0;
    s_renderer.adaptive_playout_generation = 0U;
    s_renderer.adaptive_playout_generation_seen = 0U;
    s_renderer.adaptive_playout_depth = CALL_VIDEO_ADAPTIVE_PLAYOUT_DEPTH;
    s_renderer.adaptive_gap_samples = 0U;
    s_renderer.adaptive_gap_confirm_until_us = 0;
    s_renderer.adaptive_playout_activated_at_us = 0;
    s_renderer.adaptive_playout_until_us = 0;
    s_renderer.adaptive_next_present_at_us = 0;
    s_renderer.adaptive_playout_interval_us = CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US;
    s_renderer.adaptive_playout_started = false;
    s_renderer.last_presented_at_us = 0;
    s_renderer.present_gap_window_max_us = 0;
    s_renderer.decode_process_calls = 0;
    s_renderer.decode_time_us = 0;
    s_renderer.decode_access_units = 0;
    s_renderer.decode_access_unit_time_us = 0;
    s_renderer.decode_access_unit_max_us = 0;
    s_renderer.decode_key_access_units = 0;
    s_renderer.decode_key_time_us = 0;
    s_renderer.decode_delta_access_units = 0;
    s_renderer.decode_delta_time_us = 0;
    s_renderer.decode_copy_time_us = 0;
    s_renderer.decode_copy_max_us = 0;
    s_renderer.convert_time_us = 0;
    s_renderer.convert_max_us = 0;
    s_renderer.convert_pack_time_us = 0;
    s_renderer.convert_pack_max_us = 0;
    s_renderer.convert_ppa_time_us = 0;
    s_renderer.convert_ppa_max_us = 0;
    s_renderer.convert_swap_time_us = 0;
    s_renderer.convert_swap_max_us = 0;
    s_renderer.present_copy_time_us = 0;
    s_renderer.present_copy_max_us = 0;
    s_renderer.input_queue_age_samples = 0;
    s_renderer.input_queue_age_us = 0;
    s_renderer.input_queue_age_max_us = 0;
    s_renderer.decoder_creations =
        codec == CALL_VIDEO_CODEC_MJPEG && s_renderer.mjpeg_decoder != NULL ? 1U : 0U;
    s_renderer.decoder_restarts = 0;
    s_renderer.discontinuities = 0;
    s_renderer.input_overflows = 0;
    s_renderer.ingress_task = NULL;
    s_renderer.convert_task = NULL;
    taskEXIT_CRITICAL(&s_renderer.lock);

    call_video_reset_session_queues(true);
    call_video_drain_binary_semaphore(s_renderer.start_done);
    call_video_drain_binary_semaphore(s_renderer.stop_done);
    call_video_drain_binary_semaphore(s_renderer.ingress_stop_done);
    call_video_drain_binary_semaphore(s_renderer.convert_stop_done);
    call_video_reset_output_pool_for_start();

    TaskFunction_t task_entry = codec == CALL_VIDEO_CODEC_MJPEG ?
                                call_video_mjpeg_renderer_task :
                                call_video_renderer_task;
    const char *task_name = codec == CALL_VIDEO_CODEC_MJPEG ?
                            "call_mjpeg_rx" :
                            "call_h264_rx";
    uint32_t task_stack_size = codec == CALL_VIDEO_CODEC_MJPEG ?
                               CALL_VIDEO_MJPEG_TASK_STACK_SIZE :
                               CALL_VIDEO_TASK_STACK_SIZE;
    uint32_t task_stack_caps = codec == CALL_VIDEO_CODEC_H264 ?
                               APP_TASK_STACK_CAPS_BACKGROUND :
                               APP_TASK_STACK_CAPS_REALTIME;

    /* The H264 worker is a long-lived owner with a large stack. Keep it in
     * PSRAM so RTC, Wi-Fi and codec DMA retain enough contiguous internal RAM;
     * compressed, decoded and RGB frame payloads are preallocated in PSRAM as
     * well. MJPEG keeps the smaller real-time stack in internal memory. */
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(task_entry,
                                                          task_name,
                                                          task_stack_size,
                                                          NULL,
                                                          CALL_VIDEO_TASK_PRIORITY,
                                                          &s_renderer.task,
                                                          APP_TASK_CORE_VIDEO_DECODE,
                                                          task_stack_caps);
    if (task_ret != pdPASS) {
        call_video_reset_session_queues(false);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.start_pending = false;
        s_renderer.task = NULL;
        taskEXIT_CRITICAL(&s_renderer.lock);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_renderer.start_done,
                       pdMS_TO_TICKS(CALL_VIDEO_START_TIMEOUT_MS)) != pdTRUE) {
        (void)call_video_renderer_stop();
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_OK;
    taskENTER_CRITICAL(&s_renderer.lock);
    ret = s_renderer.start_result;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (ret != ESP_OK) {
        if (xSemaphoreTake(s_renderer.stop_done,
                           pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) == pdTRUE) {
            TaskHandle_t failed_task = NULL;
            taskENTER_CRITICAL(&s_renderer.lock);
            failed_task = s_renderer.task;
            taskEXIT_CRITICAL(&s_renderer.lock);
            if (failed_task != NULL) {
                vTaskDeleteWithCaps(failed_task);
                taskENTER_CRITICAL(&s_renderer.lock);
                if (s_renderer.task == failed_task) {
                    s_renderer.task = NULL;
                }
                taskEXIT_CRITICAL(&s_renderer.lock);
            }
        }
        call_video_reset_session_queues(false);
        return ret;
    }

    BaseType_t ingress_task_ret = xTaskCreatePinnedToCoreWithCaps(
        call_video_ingress_task,
        "call_video_in",
        CALL_VIDEO_INGRESS_TASK_STACK_SIZE,
        NULL,
        CALL_VIDEO_INGRESS_TASK_PRIORITY,
        &s_renderer.ingress_task,
        APP_TASK_CORE_VIDEO_DECODE,
        APP_TASK_STACK_CAPS_BACKGROUND);
    if (ingress_task_ret != pdPASS) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.ingress_task = NULL;
        taskEXIT_CRITICAL(&s_renderer.lock);
        (void)call_video_renderer_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t call_video_renderer_start(void)
{
    return call_video_renderer_start_for_codec(CALL_VIDEO_CODEC_H264);
}

esp_err_t call_video_renderer_stop(void)
{
    TaskHandle_t task = NULL;
    TaskHandle_t ingress_task = NULL;
    bool submit_locked = false;
    bool frame_locked = false;
    call_video_codec_t codec = CALL_VIDEO_CODEC_H264;

    taskENTER_CRITICAL(&s_renderer.lock);
    task = s_renderer.task;
    ingress_task = s_renderer.ingress_task;
    codec = s_renderer.codec;
    s_renderer.stop_requested = true;
    s_renderer.generation++;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (s_renderer.submit_mutex != NULL) {
        submit_locked = xSemaphoreTake(s_renderer.submit_mutex,
                                       pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) == pdTRUE;
        if (!submit_locked) {
            if (!call_video_renderer_detect_decode_fault(esp_timer_get_time())) {
                taskENTER_CRITICAL(&s_renderer.lock);
                s_renderer.faulted = true;
                taskEXIT_CRITICAL(&s_renderer.lock);
                ESP_LOGE(TAG, "%s downlink submit path did not quiesce",
                         call_video_codec_name(codec));
            }
            return ESP_ERR_TIMEOUT;
        }
    }
    if (ingress_task != NULL) {
        if (xSemaphoreTake(s_renderer.ingress_stop_done,
                           pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "%s downlink ingress stop timed out",
                     call_video_codec_name(codec));
            if (submit_locked) {
                xSemaphoreGive(s_renderer.submit_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }
        vTaskDeleteWithCaps(ingress_task);
        taskENTER_CRITICAL(&s_renderer.lock);
        if (s_renderer.ingress_task == ingress_task) {
            s_renderer.ingress_task = NULL;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);
    }
    call_video_drain_ingress_queue();
    call_video_drain_ready_queue();

    if (task != NULL) {
        if (xSemaphoreTake(s_renderer.stop_done,
                           pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG,
                     "%s downlink renderer stop timed out",
                     call_video_codec_name(codec));
            if (!call_video_renderer_detect_decode_fault(esp_timer_get_time())) {
                taskENTER_CRITICAL(&s_renderer.lock);
                s_renderer.faulted = true;
                taskEXIT_CRITICAL(&s_renderer.lock);
            }
            if (submit_locked) {
                xSemaphoreGive(s_renderer.submit_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }

        /* The worker suspended after releasing all codec/converter resources.
         * Delete it from this owner context so WithCaps stack memory is freed
         * synchronously before the caller rebuilds another media profile. */
        vTaskDeleteWithCaps(task);
        taskENTER_CRITICAL(&s_renderer.lock);
        if (s_renderer.task == task) {
            s_renderer.task = NULL;
        }
        taskEXIT_CRITICAL(&s_renderer.lock);
    }
    if (s_renderer.frame_mutex != NULL) {
        frame_locked = xSemaphoreTake(s_renderer.frame_mutex,
                                      pdMS_TO_TICKS(CALL_VIDEO_STOP_TIMEOUT_MS)) == pdTRUE;
        if (!frame_locked) {
            if (submit_locked) {
                xSemaphoreGive(s_renderer.submit_mutex);
            }
            return ESP_ERR_TIMEOUT;
        }
    }
    call_video_reset_session_queues(false);
    call_video_output_release_ready_locked();
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.stop_requested = false;
    s_renderer.start_pending = false;
    s_renderer.running = false;
    s_renderer.frame_ready = false;
    s_renderer.session_active = false;
    s_renderer.faulted = false;
    s_renderer.decode_in_progress = false;
    s_renderer.decode_started_at_us = 0;
    s_renderer.decode_generation = 0U;
    s_renderer.decode_pts = 0U;
    s_renderer.decode_payload_bytes = 0U;
    s_renderer.decode_key_frame = false;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (frame_locked) {
        xSemaphoreGive(s_renderer.frame_mutex);
    }
    if (submit_locked) {
        xSemaphoreGive(s_renderer.submit_mutex);
    }
    return ESP_OK;
}

void call_video_renderer_flush(void)
{
    call_video_codec_t codec = CALL_VIDEO_CODEC_H264;

    if (s_renderer.submit_mutex != NULL &&
        xSemaphoreTake(s_renderer.submit_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    codec = s_renderer.codec;
    s_renderer.generation++;
    s_renderer.waiting_for_key_frame = codec == CALL_VIDEO_CODEC_H264;
    s_renderer.latency_recovery_pending = false;
    s_renderer.latency_pressure_samples = 0U;
    call_video_reset_h264_stream_state_locked();
    s_renderer.frame_ready = false;
    s_renderer.session_active = false;
    s_renderer.startup_trace_frames = 0;
    s_renderer.received_pts_valid = false;
    s_renderer.received_pts = 0;
    s_renderer.presented_pts_valid = false;
    s_renderer.presented_pts = 0;
    s_renderer.last_received_at_us = 0;
    s_renderer.receive_gap_window_max_us = 0;
    s_renderer.adaptive_playout_generation++;
    if (s_renderer.adaptive_playout_generation == 0U) {
        s_renderer.adaptive_playout_generation = 1U;
    }
    s_renderer.adaptive_playout_until_us = 0;
    s_renderer.adaptive_playout_activated_at_us = 0;
    s_renderer.adaptive_gap_samples = 0U;
    s_renderer.adaptive_gap_confirm_until_us = 0;
    s_renderer.last_presented_at_us = 0;
    s_renderer.present_gap_window_max_us = 0;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (s_renderer.frame_mutex != NULL &&
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        call_video_output_release_ready_locked();
        s_renderer.adaptive_playout_generation_seen =
            s_renderer.adaptive_playout_generation;
        s_renderer.adaptive_next_present_at_us = 0;
        s_renderer.adaptive_playout_interval_us = CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US;
        s_renderer.adaptive_playout_started = false;
        xSemaphoreGive(s_renderer.frame_mutex);
    }
    call_video_drain_ingress_queue();
    call_video_drain_ready_queue();
    call_video_drain_decoded_ready_queue();
    if (s_renderer.submit_mutex != NULL) {
        xSemaphoreGive(s_renderer.submit_mutex);
    }
}

bool call_video_renderer_requires_key_frame(void)
{
    call_video_codec_t codec = CALL_VIDEO_CODEC_H264;

    taskENTER_CRITICAL(&s_renderer.lock);
    codec = s_renderer.codec;
    taskEXIT_CRITICAL(&s_renderer.lock);
    return codec == CALL_VIDEO_CODEC_H264;
}

esp_err_t call_video_renderer_submit_h264(const uint8_t *data,
                                          size_t data_len,
                                          bool key_frame,
                                          uint32_t pts)
{
    uint8_t index = 0;
    bool running = false;
    bool waiting_for_key_frame = false;
    bool latency_recovery_pending = false;
    bool effective_key_frame = false;
    bool decoder_bootstrap = false;
    uint32_t generation = 0;
    uint32_t trace_frame_index = 0;
    int64_t trace_received_at_us = esp_timer_get_time();

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0U, ESP_ERR_INVALID_ARG, TAG, "invalid H264 frame");
    if (call_video_renderer_detect_decode_fault(trace_received_at_us)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_renderer.submit_mutex == NULL ||
        xSemaphoreTake(s_renderer.submit_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    running = s_renderer.running && !s_renderer.stop_requested &&
              s_renderer.codec == CALL_VIDEO_CODEC_H264;
    waiting_for_key_frame = s_renderer.waiting_for_key_frame;
    latency_recovery_pending = s_renderer.latency_recovery_pending;
    generation = s_renderer.generation;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!running) {
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    call_video_note_received(trace_received_at_us, pts);
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.session_active = true;
    s_renderer.received_frames++;
    s_renderer.received_bytes += data_len;
    if (s_renderer.startup_trace_frames < UINT32_MAX) {
        s_renderer.startup_trace_frames++;
    }
    if (s_renderer.startup_trace_frames <= CALL_VIDEO_STARTUP_TRACE_FRAMES) {
        trace_frame_index = s_renderer.startup_trace_frames;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (data_len > CALL_VIDEO_INPUT_SLOT_CAPACITY) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        call_video_mark_discontinuity();
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t inspect_ret = call_video_prepare_h264_access_unit(data,
                                                                data_len,
                                                                key_frame,
                                                                waiting_for_key_frame,
                                                                &effective_key_frame,
                                                                &decoder_bootstrap);
    if (inspect_ret != ESP_OK) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        xSemaphoreGive(s_renderer.submit_mutex);
        return inspect_ret;
    }
    taskENTER_CRITICAL(&s_renderer.lock);
    latency_recovery_pending = s_renderer.latency_recovery_pending;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (latency_recovery_pending && effective_key_frame) {
        /*
         * A fresh IDR is the safe cut-over point for a delayed H264 chain.
         * Drain only here, then parse the same access unit again against the
         * new decoder generation so SPS/PPS readiness is rebuilt correctly.
         */
        call_video_mark_discontinuity();
        taskENTER_CRITICAL(&s_renderer.lock);
        waiting_for_key_frame = s_renderer.waiting_for_key_frame;
        generation = s_renderer.generation;
        taskEXIT_CRITICAL(&s_renderer.lock);
        inspect_ret = call_video_prepare_h264_access_unit(data,
                                                          data_len,
                                                          key_frame,
                                                          waiting_for_key_frame,
                                                          &effective_key_frame,
                                                          &decoder_bootstrap);
        if (inspect_ret != ESP_OK) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.dropped_frames++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            xSemaphoreGive(s_renderer.submit_mutex);
            return inspect_ret;
        }
        ESP_LOGD(TAG, "H264 rx recovered at IDR");
    }
    if (latency_recovery_pending && !effective_key_frame) {
        /* Once the decoder is late, extending the dependent P-frame chain only
         * increases latency. Returning a drop asks TiRTC for a fresh IDR. */
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_NOT_FINISHED;
    }
    if (xQueueReceive(s_renderer.free_slots, &index, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        s_renderer.input_overflows++;
        s_renderer.latency_recovery_pending = true;
        taskEXIT_CRITICAL(&s_renderer.lock);
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_TIMEOUT;
    }

    call_video_input_slot_t *slot = &s_renderer.slots[index];
    memcpy(slot->data, data, data_len);
    slot->data_len = data_len;
    slot->key_frame = effective_key_frame;
    slot->decoder_bootstrap = decoder_bootstrap;
    slot->pts = pts;
    slot->generation = generation;
    slot->queued_at_us = esp_timer_get_time();
    slot->trace_frame_index = trace_frame_index;
    slot->trace_received_at_us = trace_received_at_us;
    slot->trace_decode_started_at_us = 0;
    if (xQueueSend(s_renderer.ingress_slots, &index, 0) != pdTRUE) {
        call_video_return_slot(index);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        s_renderer.input_overflows++;
        s_renderer.latency_recovery_pending = true;
        taskEXIT_CRITICAL(&s_renderer.lock);
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.submitted_frames++;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.submit_mutex);
    return ESP_OK;
}

esp_err_t call_video_renderer_submit_mjpeg(const uint8_t *data,
                                           size_t data_len,
                                           uint32_t pts)
{
    uint8_t index = 0U;
    bool running = false;
    uint32_t generation = 0U;
    int64_t received_at_us = esp_timer_get_time();

    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid MJPEG frame");
    if (s_renderer.submit_mutex == NULL ||
        xSemaphoreTake(s_renderer.submit_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    running = s_renderer.running && !s_renderer.stop_requested &&
              s_renderer.codec == CALL_VIDEO_CODEC_MJPEG;
    generation = s_renderer.generation;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (!running) {
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    call_video_note_received(received_at_us, pts);
    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.session_active = true;
    s_renderer.received_frames++;
    s_renderer.received_bytes += data_len;
    taskEXIT_CRITICAL(&s_renderer.lock);
    if (data_len > CALL_VIDEO_INPUT_SLOT_CAPACITY) {
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    if (xQueueReceive(s_renderer.free_slots, &index, 0) != pdTRUE) {
        /*
         * Each MJPEG picture is independent, so replacing the oldest queued
         * compressed frame is safe and gives the display bounded latency.
         */
        if (xQueueReceive(s_renderer.ready_slots, &index, 0) != pdTRUE) {
            taskENTER_CRITICAL(&s_renderer.lock);
            s_renderer.dropped_frames++;
            s_renderer.input_overflows++;
            taskEXIT_CRITICAL(&s_renderer.lock);
            xSemaphoreGive(s_renderer.submit_mutex);
            return ESP_ERR_TIMEOUT;
        }
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
    }

    call_video_input_slot_t *slot = &s_renderer.slots[index];
    memcpy(slot->data, data, data_len);
    slot->data_len = data_len;
    slot->key_frame = true;
    slot->decoder_bootstrap = false;
    slot->pts = pts;
    slot->generation = generation;
    slot->queued_at_us = esp_timer_get_time();
    if (xQueueSend(s_renderer.ingress_slots, &index, 0) != pdTRUE) {
        call_video_return_slot(index);
        taskENTER_CRITICAL(&s_renderer.lock);
        s_renderer.dropped_frames++;
        taskEXIT_CRITICAL(&s_renderer.lock);
        xSemaphoreGive(s_renderer.submit_mutex);
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    s_renderer.submitted_frames++;
    taskEXIT_CRITICAL(&s_renderer.lock);
    xSemaphoreGive(s_renderer.submit_mutex);
    return ESP_OK;
}

esp_err_t call_video_renderer_present_next_rgb565(const uint16_t **pixels,
                                                   size_t *pixel_count,
                                                   uint32_t *sequence,
                                                   call_video_frame_trace_t *trace)
{
    ESP_RETURN_ON_FALSE(pixels != NULL && pixel_count != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid video presentation request");
    if (trace != NULL) {
        memset(trace, 0, sizeof(*trace));
    }
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t ready_index = CALL_VIDEO_OUTPUT_SLOT_INVALID;
    const int64_t now_us = esp_timer_get_time();
    uint32_t adaptive_generation = 0U;
    uint8_t adaptive_depth = CALL_VIDEO_ADAPTIVE_PLAYOUT_DEPTH;
    int64_t adaptive_activated_at_us = 0;
    int64_t adaptive_until_us = 0;
    taskENTER_CRITICAL(&s_renderer.lock);
    adaptive_generation = s_renderer.adaptive_playout_generation;
    adaptive_depth = s_renderer.adaptive_playout_depth;
    adaptive_activated_at_us = s_renderer.adaptive_playout_activated_at_us;
    adaptive_until_us = s_renderer.adaptive_playout_until_us;
    taskEXIT_CRITICAL(&s_renderer.lock);

    /*
     * Normal calls remain a latest-frame, minimum-latency path. A measured
     * receive outage switches temporarily to an ordered PSRAM reservoir paced
     * at the source rate. Its depth is derived from the measured access-unit
     * gap, so a retransmit burst becomes playout inventory instead of being
     * rendered back-to-back. This is a presentation policy only: it does not
     * hide decode/transport errors or keep a stale weak-network mode after the
     * input cadence has recovered.
     */
    const bool adaptive_playout = adaptive_until_us > now_us;
    if (adaptive_generation != s_renderer.adaptive_playout_generation_seen) {
        s_renderer.adaptive_playout_generation_seen = adaptive_generation;
        /* When weak-network mode is detected after video is already visible,
         * keep presenting from the recovery burst and let the waterline pace
         * build reserve progressively. Waiting for a full target depth here
         * adds avoidable startup freeze on top of the outage already seen.
         * A stream with no presented frame still follows the prime path. */
        s_renderer.adaptive_playout_started =
            s_renderer.presented_output_slot < CALL_VIDEO_OUTPUT_SLOT_COUNT;
        s_renderer.adaptive_next_present_at_us =
            s_renderer.adaptive_playout_started ? now_us : 0;
        s_renderer.adaptive_playout_interval_us = CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US;
    }
    if (!adaptive_playout) {
        s_renderer.adaptive_playout_started = false;
        s_renderer.adaptive_next_present_at_us = 0;
        s_renderer.adaptive_playout_interval_us = CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US;
    } else {
        if (!s_renderer.adaptive_playout_started) {
            if (s_renderer.ready_output_count < adaptive_depth) {
                xSemaphoreGive(s_renderer.frame_mutex);
                return ESP_ERR_NOT_FOUND;
            }
            s_renderer.adaptive_playout_started = true;
            s_renderer.adaptive_next_present_at_us = now_us;
            s_renderer.adaptive_playout_interval_us =
                CALL_VIDEO_ADAPTIVE_PLAYOUT_INTERVAL_US;
            ESP_LOGI(TAG,
                     "adaptive video playout primed: depth=%u delay=%lums",
                     (unsigned)s_renderer.ready_output_count,
                     (unsigned long)(call_video_elapsed_us(adaptive_activated_at_us,
                                                           now_us) /
                                     1000U));
        }
        if (now_us < s_renderer.adaptive_next_present_at_us) {
            xSemaphoreGive(s_renderer.frame_mutex);
            return ESP_ERR_NOT_FOUND;
        }
    }

    bool frame_available = adaptive_playout ?
        call_video_output_pop_ready_locked(&ready_index) :
        call_video_output_pop_latest_locked(&ready_index);
    if (!frame_available || ready_index >= CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        xSemaphoreGive(s_renderer.frame_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (adaptive_playout) {
        uint32_t next_interval_us = call_video_adaptive_playout_interval(
            s_renderer.ready_output_count,
            adaptive_depth);
        s_renderer.adaptive_playout_interval_us = next_interval_us;
        /* Advance a stable deadline instead of restarting the interval after
         * each blocking LCD transfer. Restart only after a real outage so the
         * timer quantization and the 30 ms panel DMA do not erode the source
         * cadence. When the reservoir falls below its watermarks, the
         * source-derived refill/critical cadence trades a small, temporary
         * frame-rate reduction for avoiding a much longer empty-queue freeze. */
        if (s_renderer.adaptive_next_present_at_us <= 0 ||
            now_us - s_renderer.adaptive_next_present_at_us >
                (int64_t)(next_interval_us * 2U)) {
            s_renderer.adaptive_next_present_at_us =
                now_us + next_interval_us;
        } else {
            s_renderer.adaptive_next_present_at_us +=
                next_interval_us;
        }
    }

    call_video_output_slot_t *ready = &s_renderer.output_slots[ready_index];
    if (sequence != NULL && *sequence == ready->sequence) {
        ready->state = CALL_VIDEO_OUTPUT_FREE;
        xSemaphoreGive(s_renderer.frame_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    taskENTER_CRITICAL(&s_renderer.lock);
    if (s_renderer.presented_pts_valid &&
        (ready->pts != 0U || s_renderer.presented_pts != 0U) &&
        !call_video_pts_is_newer(ready->pts, s_renderer.presented_pts)) {
        /* Observe timestamp regressions without changing media behavior. PTS
         * alone is not sufficient evidence to discard a decoded H264 frame. */
        s_renderer.stale_presented_frames++;
    } else {
        s_renderer.presented_pts = ready->pts;
        s_renderer.presented_pts_valid = true;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);

    int64_t handoff_started_us = esp_timer_get_time();
    if (trace != NULL && ready->trace.frame_index > 0U) {
        ready->trace.output_wait_us = call_video_elapsed_us(
            ready->trace_ready_at_us,
            handoff_started_us);
        *trace = ready->trace;
    }
    if (s_renderer.presented_output_slot < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        s_renderer.output_slots[s_renderer.presented_output_slot].state = CALL_VIDEO_OUTPUT_FREE;
    }
    ready->state = CALL_VIDEO_OUTPUT_PRESENTED;
    s_renderer.presented_output_slot = ready_index;
    *pixels = ready->pixels;
    *pixel_count = CALL_VIDEO_FRAME_PIXELS;
    if (sequence != NULL) {
        *sequence = ready->sequence;
    }
    uint32_t handoff_elapsed_us = (uint32_t)(esp_timer_get_time() - handoff_started_us);
    int64_t presented_at_us = esp_timer_get_time();
    uint32_t presented_generation = 0U;
    taskENTER_CRITICAL(&s_renderer.lock);
    presented_generation = s_renderer.generation;
    s_renderer.presented_frames++;
    if (s_renderer.last_presented_at_us > 0 &&
        presented_at_us > s_renderer.last_presented_at_us) {
        uint32_t gap_us = call_video_elapsed_us(s_renderer.last_presented_at_us,
                                                presented_at_us);
        if (gap_us > s_renderer.present_gap_window_max_us) {
            s_renderer.present_gap_window_max_us = gap_us;
        }
    }
    s_renderer.last_presented_at_us = presented_at_us;
    s_renderer.present_copy_time_us += handoff_elapsed_us;
    if (handoff_elapsed_us > s_renderer.present_copy_max_us) {
        s_renderer.present_copy_max_us = handoff_elapsed_us;
    }
    taskEXIT_CRITICAL(&s_renderer.lock);
    call_video_log_memory_once(presented_generation,
                               CALL_VIDEO_MEMORY_TRACE_PRESENT,
                               "presented");
    xSemaphoreGive(s_renderer.frame_mutex);
    return ESP_OK;
}

void call_video_renderer_release_presented_rgb565(void)
{
    if (s_renderer.frame_mutex == NULL ||
        xSemaphoreTake(s_renderer.frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (s_renderer.presented_output_slot < CALL_VIDEO_OUTPUT_SLOT_COUNT) {
        s_renderer.output_slots[s_renderer.presented_output_slot].state = CALL_VIDEO_OUTPUT_FREE;
        s_renderer.presented_output_slot = CALL_VIDEO_OUTPUT_SLOT_INVALID;
    }
    xSemaphoreGive(s_renderer.frame_mutex);
}

void call_video_renderer_get_stats(call_video_renderer_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    /* Runtime/UI polling must still expose a decoder stall if TiRTC stops
     * invoking the media callback after the input queue begins rejecting it. */
    (void)call_video_renderer_detect_decode_fault(esp_timer_get_time());
    memset(stats, 0, sizeof(*stats));
    taskENTER_CRITICAL(&s_renderer.lock);
    stats->running = s_renderer.running;
    stats->waiting_for_key_frame = s_renderer.waiting_for_key_frame;
    stats->frame_ready = s_renderer.frame_ready;
    stats->codec = s_renderer.codec;
    stats->source_width = s_renderer.source_width;
    stats->source_height = s_renderer.source_height;
    stats->received_frames = s_renderer.received_frames;
    stats->received_bytes = s_renderer.received_bytes;
    stats->submitted_frames = s_renderer.submitted_frames;
    stats->decoded_frames = s_renderer.decoded_frames;
    stats->converted_frames = s_renderer.converted_frames;
    stats->presented_frames = s_renderer.presented_frames;
    stats->stale_received_frames = s_renderer.stale_received_frames;
    stats->stale_presented_frames = s_renderer.stale_presented_frames;
    stats->dropped_frames = s_renderer.dropped_frames;
    stats->conversion_dropped_frames = s_renderer.conversion_dropped_frames;
    stats->decode_failures = s_renderer.decode_failures;
    stats->conversion_failures = s_renderer.conversion_failures;
    stats->conversion_time_us = s_renderer.convert_time_us;
    stats->conversion_max_us = s_renderer.convert_max_us;
    stats->conversion_pack_time_us = s_renderer.convert_pack_time_us;
    stats->conversion_pack_max_us = s_renderer.convert_pack_max_us;
    stats->conversion_ppa_time_us = s_renderer.convert_ppa_time_us;
    stats->conversion_ppa_max_us = s_renderer.convert_ppa_max_us;
    stats->conversion_swap_time_us = s_renderer.convert_swap_time_us;
    stats->conversion_swap_max_us = s_renderer.convert_swap_max_us;
    stats->discontinuities = s_renderer.discontinuities;
    stats->input_overflows = s_renderer.input_overflows;
    stats->latest_sequence = s_renderer.latest_sequence;
    taskEXIT_CRITICAL(&s_renderer.lock);
    stats->queue_depth = call_video_input_queue_depth();
    stats->conversion_queue_depth = s_renderer.decoded_ready_slots != NULL ?
                                    (uint32_t)uxQueueMessagesWaiting(
                                        s_renderer.decoded_ready_slots) : 0U;
}
