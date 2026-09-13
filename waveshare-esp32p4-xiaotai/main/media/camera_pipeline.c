#include "camera_pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"
#include "tiRTC.h"

#include "app_log_policy.h"
#include "app_memory_policy.h"
#include "app_task_affinity.h"
#include "camera_driver.h"
#include "media_dma_reserve.h"
#include "media_governor.h"
#include "media_tuning.h"
#include "video_yuv420_scaler.h"

static const char *TAG = "camera_pipeline";

#ifndef CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
#define CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS 0
#endif
#ifndef CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
#define CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG 0
#endif

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#define CAMERA_PIPELINE_TASK_STACK       (9U * 1024U)
/* Full-duplex video shares the PPA/PSRAM path with downlink conversion. Give
 * capture one scheduler level over conversion so both peers cannot phase-lock
 * into an asymmetric state where downlink repeatedly enters PPA first and the
 * local camera holds a CSI buffer across multiple sensor periods. Decode stays
 * at the same level, while both audio tasks remain above the video path. */
#define CAMERA_PIPELINE_TASK_PRIORITY    5U
#define CAMERA_PIPELINE_RETRY_DELAY_MS   200U
#define CAMERA_PIPELINE_START_DELAY_MS   40U
#define CAMERA_PIPELINE_LOG_INTERVAL_MS 10000U
#define CAMERA_PIPELINE_H264_OPEN_RETRY_MS 2000U
#define CAMERA_PIPELINE_H264_BUFFER_CNT  1U
#ifndef CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER
#define CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER 0
#endif
#ifndef CONFIG_APP_RTC_H264_RESOURCE_FALLBACK_ENABLE
#define CONFIG_APP_RTC_H264_RESOURCE_FALLBACK_ENABLE 0
#endif
#define CAMERA_PIPELINE_H264_BITRATE     APP_MEDIA_RTC_H264_BITRATE_BPS
#define CAMERA_PIPELINE_H264_MIN_QP      APP_MEDIA_RTC_H264_MIN_QP
#define CAMERA_PIPELINE_H264_MAX_QP      APP_MEDIA_RTC_H264_MAX_QP
#define CAMERA_PIPELINE_H264_RESOURCE_FALLBACK_ENABLE \
    CONFIG_APP_RTC_H264_RESOURCE_FALLBACK_ENABLE
#define CAMERA_PIPELINE_H264_DIM_ALIGN   16U
#define CAMERA_PIPELINE_H264_MIN_WIDTH   320U
#define CAMERA_PIPELINE_H264_MIN_HEIGHT  240U
#define CAMERA_PIPELINE_H264_INTERNAL_MARGIN 2048U
#define CAMERA_PIPELINE_H264_FALLBACK_OUTPUT_BUFFER_BYTES APP_MEDIA_H264_OUTPUT_BUFFER_BYTES
#define CAMERA_PIPELINE_H264_FALLBACK_MAX_DELTA_PAYLOAD APP_MEDIA_H264_MAX_DELTA_PAYLOAD_BYTES
#define CAMERA_PIPELINE_FALLBACK_DMA_FREE_MIN_BYTES     (8U * 1024U)
#define CAMERA_PIPELINE_FALLBACK_DMA_LARGEST_MIN_BYTES  (2U * 1024U)
#define CAMERA_PIPELINE_TRANSPORT_GUARD_LOG_INTERVAL_MS 1000U
#define CAMERA_PIPELINE_FRAME_TRACE_INITIAL_COUNT       APP_MEDIA_CAMERA_FRAME_TRACE_INITIAL_COUNT
#define CAMERA_PIPELINE_FRAME_TRACE_INTERVAL_MS         APP_MEDIA_CAMERA_FRAME_TRACE_INTERVAL_MS
#define CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US       75000U
#define CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US        100000U
#define CAMERA_PIPELINE_FRAME_TRACE_LARGE_PAYLOAD_BYTES (224U * 1024U)
#define CAMERA_PIPELINE_LUMA_PROBE_GRID                  8U
#define CAMERA_PIPELINE_LUMA_PROBE_SAMPLES \
    (CAMERA_PIPELINE_LUMA_PROBE_GRID * CAMERA_PIPELINE_LUMA_PROBE_GRID)
#define CAMERA_PIPELINE_KEY_FRAME_REQUEST_MIN_INTERVAL_US \
    ((uint64_t)APP_MEDIA_H264_KEY_FRAME_REQUEST_MIN_INTERVAL_MS * 1000ULL)
#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CAMERA_PIPELINE_CACHE_LINE_SIZE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define CAMERA_PIPELINE_CACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif

typedef struct {
    bool rtc_enabled;
    camera_pipeline_video_cb_t video_cb;
    void *video_ctx;
} camera_pipeline_runtime_t;

typedef struct {
    int fd;
    esp_h264_enc_handle_t direct_handle;
    esp_h264_enc_param_hw_handle_t direct_param;
    uint8_t *capture_buffer;
    size_t capture_buffer_size;
    size_t output_buffer_bytes;
    uint16_t width;
    uint16_t height;
    uint32_t bitrate_bps;
    uint8_t fps;
    uint8_t gop;
    uint8_t min_qp;
    uint8_t max_qp;
    uint8_t direct_active_gop;
    uint8_t v4l2_active_gop;
    bool direct_encoder;
    bool capture_buffer_from_workspace;
    bool capture_streaming;
    bool output_streaming;
} camera_pipeline_h264_encoder_t;

typedef struct {
    int64_t sync_in_us;
    int64_t hw_us;
    int64_t sync_out_us;
} camera_pipeline_h264_timing_t;

typedef struct {
    uint8_t samples[CAMERA_PIPELINE_LUMA_PROBE_SAMPLES];
    uint8_t min_luma;
    uint8_t max_luma;
    uint32_t hash;
} camera_pipeline_luma_probe_t;

typedef struct {
    camera_pipeline_luma_probe_t previous;
    uint64_t delta_total;
    uint32_t transition_count;
    uint32_t changed_count;
    uint32_t sample_count;
    uint8_t window_min_luma;
    uint8_t window_max_luma;
    bool previous_valid;
} camera_pipeline_luma_stats_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_rtc_enabled;
static bool s_starting;
static TaskHandle_t s_task;
static camera_pipeline_video_cb_t s_video_cb;
static void *s_video_ctx;
static TickType_t s_last_rtc_fail_log_tick;
static TickType_t s_last_transport_guard_log_tick;
static TickType_t s_last_frame_trace_log_tick;
static bool s_h264_output_sync_noncacheable_logged;
static bool s_h264_input_sync_noncacheable_logged;
static bool s_h264_reserve_in_progress;
static bool s_scaler_reserve_in_progress;
static bool s_key_frame_request_pending;
static uint64_t s_last_key_frame_request_us;
static uint64_t s_last_key_frame_request_drop_log_us;
static camera_pipeline_h264_encoder_t s_reserved_h264 = {
    .fd = -1,
};
static uint8_t *s_h264_output_workspace;
static size_t s_h264_output_workspace_capacity;
static bool s_h264_output_workspace_in_use;
static bool s_h264_output_workspace_allocating;
static video_yuv420_scaler_handle_t s_reserved_call_scaler;
static camera_pipeline_metrics_t s_metrics = {
    .configured_bitrate_bps = CAMERA_PIPELINE_H264_BITRATE,
};

static void camera_pipeline_reconcile_h264_reservation(const char *reason);

static uint32_t camera_pipeline_interval_ms(uint8_t fps)
{
    if (fps == 0U) {
        return 1000U;
    }
    /* Round up so a nominal rate is an upper bound. At 15 fps this gives a
     * stable 67 ms cadence (14.9 fps) instead of overscheduling at 66 ms. */
    uint32_t interval = (1000U + fps - 1U) / fps;
    return interval == 0U ? 1U : interval;
}

static uint8_t camera_pipeline_h264_gop_for_profile(uint16_t width,
                                                    uint16_t height,
                                                    uint8_t fps)
{
    uint32_t safe_fps = fps > 0U ? fps : 1U;
    uint32_t duration_ms =
        width == APP_MEDIA_CALL_VIDEO_WIDTH &&
                height == APP_MEDIA_CALL_VIDEO_HEIGHT ?
            APP_MEDIA_CALL_H264_GOP_DURATION_MS :
            APP_MEDIA_H264_GOP_DURATION_MS;
    uint32_t gop_frames =
        (safe_fps * duration_ms + 999U) / 1000U;

    if (gop_frames == 0U) {
        gop_frames = 1U;
    } else if (gop_frames > 255U) {
        gop_frames = 255U;
    }
    return (uint8_t)gop_frames;
}

static uint64_t camera_pipeline_abs_delta_us(uint64_t a, uint64_t b)
{
    return a >= b ? a - b : b - a;
}

/* ESP32-P4 H264 consumes the packed O_UYY_E_VYY YUV420 layout.  Sampling a
 * small luma grid lets runtime logs distinguish a frozen camera/PPA frame from
 * an encoder or transport problem without scanning the full image. */
static bool camera_pipeline_probe_ouev_luma(const uint8_t *data,
                                            size_t len,
                                            uint16_t width,
                                            uint16_t height,
                                            camera_pipeline_luma_probe_t *probe)
{
    if (data == NULL || probe == NULL || width < 2U || height < 2U ||
        (width & 1U) != 0U) {
        return false;
    }

    size_t row_stride = ((size_t)width * 3U) / 2U;
    size_t expected_len = row_stride * height;
    if (len < expected_len) {
        return false;
    }

    uint8_t min_luma = UINT8_MAX;
    uint8_t max_luma = 0U;
    uint32_t hash = 2166136261U;
    size_t sample_index = 0U;
    for (uint32_t row = 0U; row < CAMERA_PIPELINE_LUMA_PROBE_GRID; ++row) {
        uint32_t y = ((row * 2U + 1U) * height) /
                     (CAMERA_PIPELINE_LUMA_PROBE_GRID * 2U);
        if (y >= height) {
            y = height - 1U;
        }
        for (uint32_t column = 0U; column < CAMERA_PIPELINE_LUMA_PROBE_GRID; ++column) {
            uint32_t x = ((column * 2U + 1U) * width) /
                         (CAMERA_PIPELINE_LUMA_PROBE_GRID * 2U);
            if (x >= width) {
                x = width - 1U;
            }

            size_t offset = (size_t)y * row_stride +
                            ((size_t)x / 2U) * 3U + 1U + (x & 1U);
            uint8_t luma = data[offset];
            probe->samples[sample_index++] = luma;
            if (luma < min_luma) {
                min_luma = luma;
            }
            if (luma > max_luma) {
                max_luma = luma;
            }
            hash = (hash ^ luma) * 16777619U;
        }
    }

    probe->min_luma = min_luma;
    probe->max_luma = max_luma;
    probe->hash = hash;
    return true;
}

static void camera_pipeline_luma_stats_update(camera_pipeline_luma_stats_t *stats,
                                              const camera_pipeline_luma_probe_t *probe)
{
    if (stats == NULL || probe == NULL) {
        return;
    }

    if (stats->sample_count == 0U) {
        stats->window_min_luma = probe->min_luma;
        stats->window_max_luma = probe->max_luma;
    } else {
        if (probe->min_luma < stats->window_min_luma) {
            stats->window_min_luma = probe->min_luma;
        }
        if (probe->max_luma > stats->window_max_luma) {
            stats->window_max_luma = probe->max_luma;
        }
    }
    stats->sample_count++;

    if (stats->previous_valid) {
        uint32_t delta = 0U;
        for (size_t index = 0U; index < CAMERA_PIPELINE_LUMA_PROBE_SAMPLES; ++index) {
            uint8_t current = probe->samples[index];
            uint8_t previous = stats->previous.samples[index];
            delta += current >= previous ? current - previous : previous - current;
        }
        stats->delta_total += delta;
        stats->transition_count++;
        if (probe->hash != stats->previous.hash) {
            stats->changed_count++;
        }
    }

    stats->previous = *probe;
    stats->previous_valid = true;
}

#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS || CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
static uint32_t camera_pipeline_luma_delta_x10(const camera_pipeline_luma_stats_t *stats)
{
    if (stats == NULL || stats->transition_count == 0U) {
        return 0U;
    }
    return (uint32_t)((stats->delta_total * 10ULL) /
                      ((uint64_t)stats->transition_count *
                       CAMERA_PIPELINE_LUMA_PROBE_SAMPLES));
}
#endif

static void camera_pipeline_luma_stats_reset_window(camera_pipeline_luma_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    stats->delta_total = 0U;
    stats->transition_count = 0U;
    stats->changed_count = 0U;
    stats->sample_count = 0U;
    stats->window_min_luma = 0U;
    stats->window_max_luma = 0U;
}

static bool camera_pipeline_time_due(TickType_t now, TickType_t *last_tick, uint32_t interval_ms)
{
    TickType_t interval_ticks = pdMS_TO_TICKS(interval_ms == 0U ? 1U : interval_ms);
    if (*last_tick == 0) {
        *last_tick = now;
        return true;
    }

    TickType_t elapsed_ticks = now - *last_tick;
    if (elapsed_ticks >= interval_ticks) {
        /* Keep the capture clock phase-locked. Advancing from `now` makes
         * every scheduler wake-up delay permanent and turns a configured
         * realtime target into a visibly uneven lower-rate stream. Skip
         * missed periods, but retain the original cadence for the next frame. */
        TickType_t elapsed_periods = elapsed_ticks / interval_ticks;
        *last_tick += elapsed_periods * interval_ticks;
        return true;
    }
    return false;
}

static uint32_t camera_pipeline_wait_until_due_ms(TickType_t now,
                                                  TickType_t last_tick,
                                                  uint32_t interval_ms)
{
    if (last_tick == 0) {
        return 1U;
    }

    TickType_t interval_ticks = pdMS_TO_TICKS(interval_ms == 0U ? 1U : interval_ms);
    TickType_t elapsed_ticks = now - last_tick;
    if (elapsed_ticks >= interval_ticks) {
        return 1U;
    }

    TickType_t wait_ticks = interval_ticks - elapsed_ticks;
    uint32_t wait_ms = pdTICKS_TO_MS(wait_ticks);
    return wait_ms == 0U ? 1U : wait_ms;
}

static bool camera_pipeline_tick_reached(TickType_t now, TickType_t due)
{
    return (int32_t)(now - due) >= 0;
}

static camera_pipeline_runtime_t camera_pipeline_snapshot(void)
{
    camera_pipeline_runtime_t runtime = {0};

    taskENTER_CRITICAL(&s_lock);
    runtime.rtc_enabled = s_rtc_enabled;
    runtime.video_cb = s_video_cb;
    runtime.video_ctx = s_video_ctx;
    taskEXIT_CRITICAL(&s_lock);
    return runtime;
}

static void camera_pipeline_reset_metrics(void)
{
    media_governor_camera_policy_t policy = {0};
    media_governor_get_camera_policy(&policy);

    taskENTER_CRITICAL(&s_lock);
    s_metrics = (camera_pipeline_metrics_t) {
        .running = true,
        .rtc_enabled = s_rtc_enabled,
        .width = policy.rtc_width,
        .height = policy.rtc_height,
        .target_fps = policy.rtc_video_fps,
        .configured_bitrate_bps = policy.h264_bitrate_bps,
    };
    s_last_transport_guard_log_tick = 0;
    s_last_frame_trace_log_tick = 0;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_note_frame_metrics(uint16_t width,
                                               uint16_t height,
                                               uint8_t target_fps,
                                               uint32_t configured_bitrate_bps,
                                               bool direct_input)
{
    taskENTER_CRITICAL(&s_lock);
    s_metrics.running = true;
    s_metrics.rtc_enabled = s_rtc_enabled;
    s_metrics.width = width;
    s_metrics.height = height;
    s_metrics.target_fps = target_fps;
    s_metrics.configured_bitrate_bps = configured_bitrate_bps;
    s_metrics.direct_input = direct_input;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_note_interval_metrics(uint32_t measured_fps_x10,
                                                  uint32_t measured_bitrate_kbps,
                                                  uint32_t avg_payload_bytes,
                                                  uint32_t dropped_frames,
                                                  uint32_t capture_failures,
                                                  uint32_t encode_failures)
{
    taskENTER_CRITICAL(&s_lock);
    s_metrics.measured_fps_x10 = measured_fps_x10;
    s_metrics.measured_bitrate_kbps = measured_bitrate_kbps;
    s_metrics.avg_payload_bytes = avg_payload_bytes;
    s_metrics.dropped_frames = dropped_frames;
    s_metrics.capture_failures = capture_failures;
    s_metrics.encode_failures = encode_failures;
    taskEXIT_CRITICAL(&s_lock);
}

static bool camera_pipeline_should_run(void)
{
    bool should_run = false;

    taskENTER_CRITICAL(&s_lock);
    should_run = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);
    return should_run;
}

static void camera_pipeline_mark_task_started(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_task = xTaskGetCurrentTaskHandle();
    s_starting = false;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_mark_task_stopped(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_task == xTaskGetCurrentTaskHandle()) {
        s_task = NULL;
    }
    s_starting = false;
    s_metrics.running = false;
    s_metrics.rtc_enabled = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_apply_profile(bool rtc_enabled)
{
    media_governor_set_profile(rtc_enabled ?
                               MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE :
                               MEDIA_GOVERNOR_PROFILE_IDLE);
}

static bool camera_pipeline_h264_is_key_frame(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 5U) {
        return false;
    }

    for (size_t i = 0; i + 4U < len; ++i) {
        size_t nal_offset = 0;
        if (data[i] == 0x00 && data[i + 1U] == 0x00 && data[i + 2U] == 0x01) {
            nal_offset = i + 3U;
        } else if (i + 5U < len &&
                   data[i] == 0x00 && data[i + 1U] == 0x00 &&
                   data[i + 2U] == 0x00 && data[i + 3U] == 0x01) {
            nal_offset = i + 4U;
        } else {
            continue;
        }

        if (nal_offset >= len) {
            break;
        }
        uint8_t nal_type = data[nal_offset] & 0x1fU;
        if (nal_type == 5U || nal_type == 7U) {
            return true;
        }
    }
    return false;
}

static void camera_pipeline_get_policy(media_governor_camera_policy_t *policy)
{
    media_governor_get_camera_policy(policy);
    if (policy->h264_output_buffer_bytes == 0U) {
        policy->h264_output_buffer_bytes = CAMERA_PIPELINE_H264_FALLBACK_OUTPUT_BUFFER_BYTES;
    }
    if (policy->h264_max_delta_payload_bytes == 0U) {
        policy->h264_max_delta_payload_bytes = CAMERA_PIPELINE_H264_FALLBACK_MAX_DELTA_PAYLOAD;
    }
    if (policy->dma_free_min_bytes == 0U) {
        policy->dma_free_min_bytes = CAMERA_PIPELINE_FALLBACK_DMA_FREE_MIN_BYTES;
    }
    if (policy->dma_largest_min_bytes == 0U) {
        policy->dma_largest_min_bytes = CAMERA_PIPELINE_FALLBACK_DMA_LARGEST_MIN_BYTES;
    }
}

static bool camera_pipeline_should_hold_video_for_transport(size_t payload_len,
                                                            bool key_frame,
                                                            const media_governor_camera_policy_t *policy,
                                                            uint32_t stream_age_ms,
                                                            size_t *effective_max_delta,
                                                            const char **reason)
{
    if (reason != NULL) {
        *reason = NULL;
    }

    size_t max_delta = policy != NULL ? policy->h264_max_delta_payload_bytes :
                       CAMERA_PIPELINE_H264_FALLBACK_MAX_DELTA_PAYLOAD;
#if APP_MEDIA_H264_STARTUP_GUARD_MS > 0
    if (stream_age_ms < APP_MEDIA_H264_STARTUP_GUARD_MS &&
        max_delta > APP_MEDIA_H264_STARTUP_MAX_DELTA_PAYLOAD_BYTES) {
        max_delta = APP_MEDIA_H264_STARTUP_MAX_DELTA_PAYLOAD_BYTES;
    }
#endif
    if (effective_max_delta != NULL) {
        *effective_max_delta = max_delta;
    }

    if (!key_frame && payload_len > max_delta) {
        if (reason != NULL) {
            *reason = "delta payload burst";
        }
        return true;
    }

    return false;
}

static bool camera_pipeline_transport_guard_needs_key_frame(const char *reason)
{
    return reason != NULL && strcmp(reason, "delta payload burst") == 0;
}

static void camera_pipeline_log_transport_guard(const char *reason,
                                                size_t payload_len,
                                                bool key_frame,
                                                const media_governor_camera_policy_t *policy,
                                                size_t effective_max_delta,
                                                uint32_t stream_age_ms)
{
    TickType_t now_tick = xTaskGetTickCount();
    if (s_last_transport_guard_log_tick != 0 &&
        now_tick - s_last_transport_guard_log_tick <
            pdMS_TO_TICKS(CAMERA_PIPELINE_TRANSPORT_GUARD_LOG_INTERVAL_MS)) {
        return;
    }
    s_last_transport_guard_log_tick = now_tick;

    ESP_LOGW(TAG,
             "camera video transport guard: reason=%s payload=%u key=%d max_delta=%u policy_delta=%u stream_age_ms=%u target=%ux%u@%u dma_free=%u dma_largest=%u",
             reason != NULL ? reason : "unknown",
             (unsigned)payload_len,
             key_frame ? 1 : 0,
             (unsigned)effective_max_delta,
             (unsigned)(policy != NULL ? policy->h264_max_delta_payload_bytes : 0U),
             (unsigned)stream_age_ms,
             (unsigned)(policy != NULL ? policy->rtc_width : 0U),
             (unsigned)(policy != NULL ? policy->rtc_height : 0U),
             (unsigned)(policy != NULL ? policy->rtc_video_fps : 0U),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

static uint16_t camera_pipeline_even_dimension(uint16_t value)
{
    return (uint16_t)(value & (uint16_t)~1U);
}

static uint16_t camera_pipeline_align_down_u16(uint16_t value, uint16_t align)
{
    if (align == 0U) {
        return value;
    }
    return (uint16_t)(value - (value % align));
}

static size_t camera_pipeline_h264_input_size(uint16_t width, uint16_t height)
{
    return ((size_t)width * height * 3U) / 2U;
}

static size_t camera_pipeline_h264_ref_internal_estimate(uint16_t width)
{
    uint16_t mb_width = (uint16_t)((width + 15U) / 16U);
    return ((size_t)3U * 16U * (16U + 8U) * mb_width) + 7U;
}

static size_t camera_pipeline_align_up_size(size_t value, size_t align)
{
    if (align == 0U) {
        return value;
    }
    return (value + align - 1U) & ~(align - 1U);
}

static uintptr_t camera_pipeline_align_down_ptr(uintptr_t value, size_t align)
{
    if (align == 0U) {
        return value;
    }
    return value & ~((uintptr_t)align - 1U);
}

static esp_err_t camera_pipeline_h264_error_to_esp(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return ESP_OK;
    case ESP_H264_ERR_ARG:
        return ESP_ERR_INVALID_ARG;
    case ESP_H264_ERR_MEM:
        return ESP_ERR_NO_MEM;
    case ESP_H264_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ESP_H264_ERR_OVERFLOW:
        return ESP_ERR_INVALID_SIZE;
    case ESP_H264_ERR_UNSUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_H264_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

static esp_err_t camera_pipeline_h264_set_control(int fd, uint32_t id, int32_t value, const char *name);

static bool camera_pipeline_h264_is_open(const camera_pipeline_h264_encoder_t *enc)
{
    if (enc == NULL || enc->capture_buffer == NULL) {
        return false;
    }
    if (enc->direct_encoder) {
        return enc->direct_handle != NULL;
    }
    return enc->fd >= 0;
}

static bool camera_pipeline_take_key_frame_request(void)
{
    bool requested = false;

    taskENTER_CRITICAL(&s_lock);
    if (s_key_frame_request_pending) {
        s_key_frame_request_pending = false;
        requested = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    return requested;
}

static bool camera_pipeline_h264_force_next_idr(camera_pipeline_h264_encoder_t *enc,
                                                const char *reason)
{
    if (enc == NULL || !camera_pipeline_h264_is_open(enc)) {
        return false;
    }

    uint8_t base_gop = enc->gop != 0U ? enc->gop : enc->fps;
    if (base_gop == 0U) {
        return false;
    }

    uint8_t old_gop = enc->direct_encoder ?
                      (enc->direct_active_gop != 0U ? enc->direct_active_gop : base_gop) :
                      (enc->v4l2_active_gop != 0U ? enc->v4l2_active_gop : base_gop);

    if (enc->direct_encoder) {
        if (enc->direct_handle == NULL || enc->direct_param == NULL) {
            return false;
        }
        esp_h264_err_t ret = esp_h264_enc_force_idr(&enc->direct_param->base);
        if (ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG,
                     "direct H264 key-frame request failed: reason=%s err=%d",
                     reason != NULL ? reason : "unknown",
                     ret);
            return false;
        }
        if (reason != NULL && strcmp(reason, "stream-start") == 0) {
            APP_LOG_DETAIL(TAG,
                           "H264 key-frame requested: reason=%s mode=direct_hw force-next-idr",
                           reason);
        } else {
            ESP_LOGD(TAG,
                     "H264 key-frame requested: reason=%s mode=direct_hw force-next-idr",
                     reason != NULL ? reason : "unknown");
        }
        return true;
    }

    uint8_t alternate_gop = base_gop > 1U ? (uint8_t)(base_gop - 1U) : 2U;
    uint8_t next_gop = old_gop == base_gop ? alternate_gop : base_gop;

    if (camera_pipeline_h264_set_control(enc->fd,
                                         V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                                         next_gop,
                                         "gop-key-request") != ESP_OK) {
        return false;
    }
    enc->v4l2_active_gop = next_gop;

    if (reason != NULL && strcmp(reason, "stream-start") == 0) {
        APP_LOG_DETAIL(TAG,
                       "H264 key-frame requested: reason=%s mode=%s gop_switch=%u->%u",
                 reason,
                 enc->direct_encoder ? "direct_hw" : "v4l2_m2m",
                 (unsigned)old_gop,
                 (unsigned)next_gop);
    } else {
        ESP_LOGD(TAG,
                 "H264 key-frame requested: reason=%s mode=%s gop_switch=%u->%u",
                 reason != NULL ? reason : "unknown",
                 enc->direct_encoder ? "direct_hw" : "v4l2_m2m",
                 (unsigned)old_gop,
                 (unsigned)next_gop);
    }
    return true;
}

static bool camera_pipeline_select_h264_internal_fit(uint16_t requested_width,
                                                     uint16_t requested_height,
                                                     uint16_t *width,
                                                     uint16_t *height)
{
    if (width == NULL || height == NULL ||
        requested_width < CAMERA_PIPELINE_H264_MIN_WIDTH ||
        requested_height < CAMERA_PIPELINE_H264_MIN_HEIGHT) {
        return false;
    }

    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t budget = largest_internal > CAMERA_PIPELINE_H264_INTERNAL_MARGIN ?
                    largest_internal - CAMERA_PIPELINE_H264_INTERNAL_MARGIN :
                    largest_internal;
    uint16_t candidate_width = camera_pipeline_align_down_u16(requested_width,
                                                             CAMERA_PIPELINE_H264_DIM_ALIGN);

    /*
     * If the requested profile no longer fits, prefer the tested compact ISP
     * output over the largest arbitrary encoder width. An arbitrary size such
     * as 1216x912 keeps more pixels but forces a full-frame PPA resize from
     * 1280x960. On P4 that extra pass can exceed the entire 20 fps frame
     * budget. The compact pair lets camera output and H264 input match, so the
     * hot path remains YUV420 direct.
     */
    if (camera_pipeline_h264_ref_internal_estimate(candidate_width) > budget &&
        requested_width >= MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH &&
        requested_height >= MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT &&
        camera_pipeline_h264_ref_internal_estimate(
            MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH) <= budget) {
        *width = MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH;
        *height = MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT;
        return true;
    }

    while (candidate_width >= CAMERA_PIPELINE_H264_MIN_WIDTH &&
           camera_pipeline_h264_ref_internal_estimate(candidate_width) > budget) {
        candidate_width = (uint16_t)(candidate_width - CAMERA_PIPELINE_H264_DIM_ALIGN);
    }

    if (candidate_width < CAMERA_PIPELINE_H264_MIN_WIDTH) {
        return false;
    }

    uint32_t candidate_height = ((uint32_t)candidate_width * requested_height) / requested_width;
    candidate_height = camera_pipeline_align_down_u16((uint16_t)candidate_height,
                                                      CAMERA_PIPELINE_H264_DIM_ALIGN);
    if (candidate_height < CAMERA_PIPELINE_H264_MIN_HEIGHT) {
        return false;
    }

    *width = candidate_width;
    *height = (uint16_t)candidate_height;
    return candidate_width != requested_width || candidate_height != requested_height;
}

static esp_err_t camera_pipeline_sync_h264_output_for_cpu(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uintptr_t sync_start = camera_pipeline_align_down_ptr((uintptr_t)data,
                                                          CAMERA_PIPELINE_CACHE_LINE_SIZE);
    uintptr_t sync_end = (uintptr_t)data + len;
    size_t sync_len = camera_pipeline_align_up_size((size_t)(sync_end - sync_start),
                                                    CAMERA_PIPELINE_CACHE_LINE_SIZE);
    const void *sync_ptr = (const void *)sync_start;
    const void *sync_last = (const void *)(sync_start + sync_len - 1U);
    bool internal_range = esp_ptr_internal(sync_ptr) && esp_ptr_internal(sync_last);
    bool external_range = esp_ptr_external_ram(sync_ptr) && esp_ptr_external_ram(sync_last);
    if (!internal_range && !external_range) {
        if (!s_h264_output_sync_noncacheable_logged) {
            s_h264_output_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 output cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }

    esp_err_t ret = esp_cache_msync((void *)sync_start,
                                    sync_len,
                                    ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                                        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (ret == ESP_ERR_INVALID_ARG) {
        if (!s_h264_output_sync_noncacheable_logged) {
            s_h264_output_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 output cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }
    return ret;
}

static esp_err_t camera_pipeline_sync_h264_input_for_device(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uintptr_t sync_start = camera_pipeline_align_down_ptr((uintptr_t)data,
                                                          CAMERA_PIPELINE_CACHE_LINE_SIZE);
    uintptr_t sync_end = (uintptr_t)data + len;
    size_t sync_len = camera_pipeline_align_up_size((size_t)(sync_end - sync_start),
                                                    CAMERA_PIPELINE_CACHE_LINE_SIZE);
    const void *sync_ptr = (const void *)sync_start;
    const void *sync_last = (const void *)(sync_start + sync_len - 1U);
    bool internal_range = esp_ptr_internal(sync_ptr) && esp_ptr_internal(sync_last);
    bool external_range = esp_ptr_external_ram(sync_ptr) && esp_ptr_external_ram(sync_last);
    if (!internal_range && !external_range) {
        if (!s_h264_input_sync_noncacheable_logged) {
            s_h264_input_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 input cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }

    esp_err_t ret = esp_cache_msync((void *)sync_start,
                                    sync_len,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (ret == ESP_ERR_INVALID_ARG) {
        if (!s_h264_input_sync_noncacheable_logged) {
            s_h264_input_sync_noncacheable_logged = true;
            ESP_LOGD(TAG, "H264 input cache sync skipped for non-cacheable memory");
        }
        return ESP_OK;
    }
    return ret;
}

static esp_err_t camera_pipeline_h264_set_control(int fd, uint32_t id, int32_t value, const char *name)
{
    struct v4l2_ext_control control = {
        .id = id,
        .value = value,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_CODEC_CLASS,
        .count = 1,
        .controls = &control,
    };

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "set H264 control failed: %s=%ld errno=%d", name, (long)value, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool camera_pipeline_h264_get_control(int fd, uint32_t id, int32_t *value, const char *name)
{
    struct v4l2_ext_control control = {
        .id = id,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_CODEC_CLASS,
        .count = 1,
        .controls = &control,
    };

    if (value == NULL) {
        return false;
    }
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        ESP_LOGD(TAG, "get H264 control failed: %s errno=%d", name != NULL ? name : "unknown", errno);
        return false;
    }
    *value = control.value;
    return true;
}

static uint8_t *camera_pipeline_h264_output_workspace_acquire(size_t required_bytes)
{
    if (required_bytes == 0U) {
        return NULL;
    }

    uint8_t *workspace = NULL;
    size_t allocation_bytes = required_bytes;
    bool allocate = false;

    if (allocation_bytes < APP_MEDIA_H264_OUTPUT_BUFFER_BYTES) {
        allocation_bytes = APP_MEDIA_H264_OUTPUT_BUFFER_BYTES;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_h264_output_workspace != NULL &&
        s_h264_output_workspace_capacity >= required_bytes &&
        !s_h264_output_workspace_in_use) {
        s_h264_output_workspace_in_use = true;
        workspace = s_h264_output_workspace;
    } else if (s_h264_output_workspace == NULL &&
               !s_h264_output_workspace_in_use &&
               !s_h264_output_workspace_allocating) {
        s_h264_output_workspace_allocating = true;
        allocate = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (workspace != NULL || !allocate) {
        return workspace;
    }

    uint8_t *new_workspace =
        app_memory_aligned_alloc_psram(CAMERA_PIPELINE_CACHE_LINE_SIZE,
                                       allocation_bytes,
                                       MALLOC_CAP_CACHE_ALIGNED);
    bool installed = false;
    taskENTER_CRITICAL(&s_lock);
    if (new_workspace != NULL &&
        s_h264_output_workspace == NULL &&
        !s_h264_output_workspace_in_use) {
        s_h264_output_workspace = new_workspace;
        s_h264_output_workspace_capacity = allocation_bytes;
        s_h264_output_workspace_in_use = true;
        workspace = new_workspace;
        installed = true;
    }
    s_h264_output_workspace_allocating = false;
    taskEXIT_CRITICAL(&s_lock);

    if (new_workspace != NULL && !installed) {
        heap_caps_free(new_workspace);
    }
    if (installed) {
        ESP_LOGI(TAG,
                 "H264 output workspace reserved: capacity=%u requested=%u",
                 (unsigned)allocation_bytes,
                 (unsigned)required_bytes);
    }
    return workspace;
}

static void camera_pipeline_h264_output_workspace_release(uint8_t *workspace)
{
    if (workspace == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    if (workspace == s_h264_output_workspace) {
        s_h264_output_workspace_in_use = false;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void camera_pipeline_h264_close(camera_pipeline_h264_encoder_t *enc)
{
    if (enc == NULL) {
        return;
    }
    if (!camera_pipeline_h264_is_open(enc) &&
        enc->fd < 0 &&
        enc->direct_handle == NULL &&
        enc->capture_buffer == NULL) {
        return;
    }

    if (enc->direct_encoder || enc->direct_handle != NULL) {
        if (enc->direct_handle != NULL) {
            esp_h264_err_t close_ret = esp_h264_enc_close(enc->direct_handle);
            if (close_ret != ESP_H264_ERR_OK) {
                ESP_LOGW(TAG, "direct H264 encoder close failed err=%d", close_ret);
            }
            esp_h264_err_t del_ret = esp_h264_enc_del(enc->direct_handle);
            if (del_ret != ESP_H264_ERR_OK) {
                ESP_LOGW(TAG, "direct H264 encoder delete failed err=%d", del_ret);
            }
            enc->direct_handle = NULL;
        }
        if (enc->capture_buffer != NULL) {
            if (enc->capture_buffer_from_workspace) {
                camera_pipeline_h264_output_workspace_release(enc->capture_buffer);
            } else {
                heap_caps_free(enc->capture_buffer);
            }
            enc->capture_buffer = NULL;
        }
        *enc = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
        return;
    }

    if (enc->output_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (ioctl(enc->fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "H264 output stream off failed errno=%d", errno);
        }
        enc->output_streaming = false;
    }
    if (enc->capture_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(enc->fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "H264 capture stream off failed errno=%d", errno);
        }
        enc->capture_streaming = false;
    }
    if (enc->capture_buffer != NULL) {
        (void)munmap(enc->capture_buffer, enc->capture_buffer_size);
        enc->capture_buffer = NULL;
        enc->capture_buffer_size = 0;
    }
    close(enc->fd);
    enc->fd = -1;
    enc->width = 0;
    enc->height = 0;
}

static esp_err_t camera_pipeline_h264_open(camera_pipeline_h264_encoder_t *enc,
                                           uint16_t width,
                                           uint16_t height,
                                           uint8_t fps,
                                           uint32_t bitrate_bps,
                                           uint8_t min_qp,
                                           uint8_t max_qp,
                                           size_t output_buffer_bytes)
{
    ESP_RETURN_ON_FALSE(enc != NULL, ESP_ERR_INVALID_ARG, TAG, "h264 encoder is null");

    memset(enc, 0, sizeof(*enc));
    enc->fd = -1;

    uint8_t safe_fps = fps == 0U ? 12U : fps;
    uint32_t safe_bitrate_bps = bitrate_bps == 0U ? CAMERA_PIPELINE_H264_BITRATE : bitrate_bps;
    uint8_t safe_min_qp = min_qp >= 10U && min_qp <= 51U ?
                              min_qp : CAMERA_PIPELINE_H264_MIN_QP;
    uint8_t safe_max_qp = max_qp >= safe_min_qp && max_qp <= 51U ?
                              max_qp : CAMERA_PIPELINE_H264_MAX_QP;
    uint8_t safe_gop =
        camera_pipeline_h264_gop_for_profile(width, height, safe_fps);
    size_t safe_output_buffer_bytes =
        output_buffer_bytes == 0U ? APP_MEDIA_H264_OUTPUT_BUFFER_BYTES : output_buffer_bytes;

    APP_LOG_DETAIL(TAG,
                   "H264 encoder open request: mode=%s size=%ux%u fps=%u gop=%u bitrate=%u qp=%u-%u ref_internal_est=%u internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
             CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER ? "direct_hw" : "v4l2_m2m",
             width,
             height,
             safe_fps,
             safe_gop,
             (unsigned)safe_bitrate_bps,
             (unsigned)safe_min_qp,
             (unsigned)safe_max_qp,
             (unsigned)camera_pipeline_h264_ref_internal_estimate(width),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (CONFIG_APP_RTC_H264_DIRECT_HW_ENCODER) {
        enc->direct_encoder = true;
        enc->capture_buffer = camera_pipeline_h264_output_workspace_acquire(
            safe_output_buffer_bytes);
        ESP_RETURN_ON_FALSE(enc->capture_buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "acquire direct H264 output workspace failed size=%u reserved=%u/%u psram_largest=%u",
                            (unsigned)safe_output_buffer_bytes,
                            s_h264_output_workspace != NULL ? 1U : 0U,
                            s_h264_output_workspace_in_use ? 1U : 0U,
                            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        enc->capture_buffer_from_workspace = true;
        enc->capture_buffer_size = safe_output_buffer_bytes;

        esp_h264_enc_cfg_hw_t config = {
            .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
            .gop = safe_gop,
            .fps = safe_fps,
            .res = {
                .width = width,
                .height = height,
            },
            .rc = {
                .bitrate = safe_bitrate_bps,
                .qp_min = safe_min_qp,
                .qp_max = safe_max_qp,
            },
        };

        esp_h264_err_t h264_ret = esp_h264_enc_hw_new(&config, &enc->direct_handle);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "create direct H264 encoder failed err=%d", h264_ret);
            camera_pipeline_h264_close(enc);
            return camera_pipeline_h264_error_to_esp(h264_ret);
        }
        h264_ret = esp_h264_enc_hw_get_param_hd(enc->direct_handle, &enc->direct_param);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "get direct H264 parameter handle failed err=%d; key-frame requests disabled", h264_ret);
            enc->direct_param = NULL;
        }
        h264_ret = esp_h264_enc_open(enc->direct_handle);
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "open direct H264 encoder failed err=%d", h264_ret);
            camera_pipeline_h264_close(enc);
            return camera_pipeline_h264_error_to_esp(h264_ret);
        }

        enc->fd = -1;
        enc->direct_encoder = true;
        enc->width = width;
        enc->height = height;
        enc->fps = safe_fps;
        enc->gop = safe_gop;
        enc->min_qp = safe_min_qp;
        enc->max_qp = safe_max_qp;
        enc->direct_active_gop = safe_gop;
        enc->bitrate_bps = safe_bitrate_bps;
        enc->output_buffer_bytes = safe_output_buffer_bytes;

        ESP_LOGI(TAG,
                 "H264 encoder ready: input=O_UYY_E_VYY output=H264 mode=direct_hw size=%ux%u fps=%u gop=%u bitrate=%u qp=%u-%u out_buf=%u",
                 width,
                 height,
                 safe_fps,
                 safe_gop,
                 safe_bitrate_bps,
                 safe_min_qp,
                 safe_max_qp,
                 (unsigned)enc->capture_buffer_size);
        return ESP_OK;
    }

    int fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_FAIL, TAG, "open %s failed errno=%d", ESP_VIDEO_H264_DEVICE_NAME, errno);
    enc->fd = fd;

    struct v4l2_capability capability = {0};
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0) {
        APP_LOG_DETAIL(TAG,
                       "H264 encoder device: driver=%s card=%s caps=0x%08" PRIx32,
                 capability.driver,
                 capability.card,
                 capability.capabilities);
    }

    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                                           safe_gop,
                                           "gop");
    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_BITRATE,
                                           safe_bitrate_bps,
                                           "bitrate");
    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
                                           safe_min_qp,
                                           "min_qp");
    (void)camera_pipeline_h264_set_control(fd,
                                           V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                                           safe_max_qp,
                                           "max_qp");

    int32_t actual_gop = -1;
    int32_t actual_bitrate = -1;
    int32_t actual_min_qp = -1;
    int32_t actual_max_qp = -1;
    bool have_gop = camera_pipeline_h264_get_control(fd,
                                                     V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                                                     &actual_gop,
                                                     "gop");
    bool have_bitrate = camera_pipeline_h264_get_control(fd,
                                                         V4L2_CID_MPEG_VIDEO_BITRATE,
                                                         &actual_bitrate,
                                                         "bitrate");
    bool have_min_qp = camera_pipeline_h264_get_control(fd,
                                                        V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
                                                        &actual_min_qp,
                                                        "min_qp");
    bool have_max_qp = camera_pipeline_h264_get_control(fd,
                                                        V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                                                        &actual_max_qp,
                                                        "max_qp");
    APP_LOG_DETAIL(TAG,
                   "H264 encoder controls: requested bitrate=%u gop=%u qp=%u-%u actual bitrate=%ld gop=%ld qp=%ld-%ld valid=%d%d%d%d",
             (unsigned)safe_bitrate_bps,
             (unsigned)safe_gop,
             (unsigned)safe_min_qp,
             (unsigned)safe_max_qp,
             (long)actual_bitrate,
             (long)actual_gop,
             (long)actual_min_qp,
             (long)actual_max_qp,
             have_bitrate ? 1 : 0,
             have_gop ? 1 : 0,
             have_min_qp ? 1 : 0,
             have_max_qp ? 1 : 0);

    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
    };
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "set H264 input format failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    struct v4l2_requestbuffers req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "request H264 input buffers failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    format.fmt.pix.sizeimage = (uint32_t)safe_output_buffer_bytes;
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "set H264 output format failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    memset(&req, 0, sizeof(req));
    req.count = CAMERA_PIPELINE_H264_BUFFER_CNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "request H264 output buffers failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    struct v4l2_buffer buf = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "query H264 output buffer failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    enc->capture_buffer = (uint8_t *)mmap(NULL,
                                          buf.length,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED,
                                          fd,
                                          buf.m.offset);
    if (enc->capture_buffer == NULL || enc->capture_buffer == MAP_FAILED) {
        enc->capture_buffer = NULL;
        ESP_LOGE(TAG, "map H264 output buffer failed length=%u", (unsigned)buf.length);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }
    enc->capture_buffer_size = buf.length;

    if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "queue H264 output buffer failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "H264 capture stream on failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }
    enc->capture_streaming = true;

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "H264 output stream on failed errno=%d", errno);
        camera_pipeline_h264_close(enc);
        return ESP_FAIL;
    }
    enc->output_streaming = true;
    enc->width = width;
    enc->height = height;
    enc->fps = safe_fps;
    enc->gop = safe_gop;
    enc->min_qp = safe_min_qp;
    enc->max_qp = safe_max_qp;
    enc->v4l2_active_gop = safe_gop;
    enc->bitrate_bps = safe_bitrate_bps;
    enc->output_buffer_bytes = safe_output_buffer_bytes;

    ESP_LOGI(TAG,
             "H264 encoder ready: input=YUV420 output=H264 mode=v4l2_m2m size=%ux%u fps=%u gop=%u bitrate=%u qp=%u-%u out_buf=%u",
             width,
             height,
             safe_fps,
             safe_gop,
             safe_bitrate_bps,
             safe_min_qp,
             safe_max_qp,
             (unsigned)enc->capture_buffer_size);
    return ESP_OK;
}

static esp_err_t camera_pipeline_h264_open_with_dma_escrow(camera_pipeline_h264_encoder_t *enc,
                                                           uint16_t width,
                                                           uint16_t height,
                                                           uint8_t fps,
                                                           uint32_t bitrate_bps,
                                                           uint8_t min_qp,
                                                           uint8_t max_qp,
                                                           size_t output_buffer_bytes,
                                                           const char *stage)
{
    esp_err_t ret = camera_pipeline_h264_open(enc,
                                              width,
                                              height,
                                              fps,
                                              bitrate_bps,
                                              min_qp,
                                              max_qp,
                                              output_buffer_bytes);
    if (ret == ESP_OK || !media_dma_reserve_is_reserved()) {
        return ret;
    }

    ESP_LOGW(TAG,
             "H264 encoder open failed before DMA escrow lend: stage=%s ret=%s size=%ux%u fps=%u bitrate=%u",
             stage != NULL ? stage : "unknown",
             esp_err_to_name(ret),
             width,
             height,
             fps,
             (unsigned)bitrate_bps);

    media_dma_reserve_release("h264-open-retry");
    ret = camera_pipeline_h264_open(enc,
                                    width,
                                    height,
                                    fps,
                                    bitrate_bps,
                                    min_qp,
                                    max_qp,
                                    output_buffer_bytes);

    esp_err_t reclaim_ret =
        media_dma_reserve_reclaim(ret == ESP_OK ? "h264-open-retry-success" : "h264-open-retry-failed");
    if (reclaim_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "DMA escrow reclaim after H264 retry failed: open=%s reclaim=%s",
                 esp_err_to_name(ret),
                 esp_err_to_name(reclaim_ret));
    }

    return ret;
}

static bool camera_pipeline_h264_matches(const camera_pipeline_h264_encoder_t *enc,
                                         uint16_t width,
                                         uint16_t height,
                                         uint8_t fps,
                                         uint32_t bitrate_bps,
                                         uint8_t min_qp,
                                         uint8_t max_qp,
                                         size_t output_buffer_bytes)
{
    uint8_t safe_fps = fps == 0U ? 12U : fps;
    uint32_t safe_bitrate_bps = bitrate_bps == 0U ? CAMERA_PIPELINE_H264_BITRATE : bitrate_bps;
    uint8_t safe_min_qp = min_qp >= 10U && min_qp <= 51U ?
                              min_qp : CAMERA_PIPELINE_H264_MIN_QP;
    uint8_t safe_max_qp = max_qp >= safe_min_qp && max_qp <= 51U ?
                              max_qp : CAMERA_PIPELINE_H264_MAX_QP;
    uint8_t safe_gop =
        camera_pipeline_h264_gop_for_profile(width, height, safe_fps);
    size_t safe_output_buffer_bytes =
        output_buffer_bytes == 0U ? APP_MEDIA_H264_OUTPUT_BUFFER_BYTES : output_buffer_bytes;

    return enc != NULL &&
           camera_pipeline_h264_is_open(enc) &&
           enc->width == width &&
           enc->height == height &&
           enc->fps == safe_fps &&
           enc->gop == safe_gop &&
           enc->bitrate_bps == safe_bitrate_bps &&
           enc->min_qp == safe_min_qp &&
           enc->max_qp == safe_max_qp &&
           enc->output_buffer_bytes == safe_output_buffer_bytes;
}

static bool camera_pipeline_h264_static_config_matches(
    const camera_pipeline_h264_encoder_t *enc,
    uint16_t width,
    uint16_t height,
    uint8_t min_qp,
    uint8_t max_qp,
    size_t output_buffer_bytes)
{
    uint8_t safe_min_qp = min_qp >= 10U && min_qp <= 51U ?
                              min_qp : CAMERA_PIPELINE_H264_MIN_QP;
    uint8_t safe_max_qp = max_qp >= safe_min_qp && max_qp <= 51U ?
                              max_qp : CAMERA_PIPELINE_H264_MAX_QP;
    size_t safe_output_buffer_bytes =
        output_buffer_bytes == 0U ?
            APP_MEDIA_H264_OUTPUT_BUFFER_BYTES :
            output_buffer_bytes;

    return enc != NULL &&
           camera_pipeline_h264_is_open(enc) &&
           enc->width == width &&
           enc->height == height &&
           enc->min_qp == safe_min_qp &&
           enc->max_qp == safe_max_qp &&
           enc->output_buffer_bytes == safe_output_buffer_bytes;
}

static esp_err_t camera_pipeline_h264_update_rate(camera_pipeline_h264_encoder_t *enc,
                                                  uint8_t fps,
                                                  uint32_t bitrate_bps)
{
    ESP_RETURN_ON_FALSE(enc != NULL && camera_pipeline_h264_is_open(enc),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "H264 encoder is not ready for rate update");

    const uint8_t safe_fps = fps == 0U ? 12U : fps;
    const uint32_t safe_bitrate_bps =
        bitrate_bps == 0U ? CAMERA_PIPELINE_H264_BITRATE : bitrate_bps;
    const uint8_t safe_gop = camera_pipeline_h264_gop_for_profile(
        enc->width,
        enc->height,
        safe_fps);
    const uint8_t old_fps = enc->fps;
    const uint8_t old_gop = enc->gop;
    const uint32_t old_bitrate_bps = enc->bitrate_bps;
    const bool bitrate_changed = old_bitrate_bps != safe_bitrate_bps;
    const bool fps_changed = old_fps != safe_fps;
    const bool gop_changed = old_gop != safe_gop;

    if (old_fps == safe_fps &&
        old_gop == safe_gop &&
        old_bitrate_bps == safe_bitrate_bps) {
        return ESP_OK;
    }

    const int64_t update_started_us = esp_timer_get_time();
    if (enc->direct_encoder) {
        ESP_RETURN_ON_FALSE(enc->direct_param != NULL,
                            ESP_ERR_NOT_SUPPORTED,
                            TAG,
                            "direct H264 parameter handle unavailable");
        esp_h264_err_t h264_ret = ESP_H264_ERR_OK;
        if (bitrate_changed) {
            h264_ret = esp_h264_enc_set_bitrate(&enc->direct_param->base,
                                                safe_bitrate_bps);
        }
        if (h264_ret == ESP_H264_ERR_OK && fps_changed) {
            h264_ret =
                esp_h264_enc_set_fps(&enc->direct_param->base, safe_fps);
        }
        if (h264_ret == ESP_H264_ERR_OK && gop_changed) {
            h264_ret =
                esp_h264_enc_set_gop(&enc->direct_param->base, safe_gop);
        }
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG,
                     "direct H264 rate update failed err=%d fps=%u bitrate=%u",
                     h264_ret,
                     safe_fps,
                     (unsigned)safe_bitrate_bps);
            return camera_pipeline_h264_error_to_esp(h264_ret);
        }
        if (gop_changed) {
            enc->direct_active_gop = safe_gop;
        }
    } else {
        ESP_RETURN_ON_FALSE(enc->fd >= 0,
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "V4L2 H264 fd unavailable");
        if (bitrate_changed) {
            ESP_RETURN_ON_ERROR(
                camera_pipeline_h264_set_control(enc->fd,
                                                 V4L2_CID_MPEG_VIDEO_BITRATE,
                                                 safe_bitrate_bps,
                                                 "bitrate"),
                TAG,
                "update V4L2 H264 bitrate failed");
        }
        if (gop_changed) {
            ESP_RETURN_ON_ERROR(
                camera_pipeline_h264_set_control(enc->fd,
                                                 V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
                                                 safe_gop,
                                                 "gop"),
                TAG,
                "update V4L2 H264 GOP failed");
            enc->v4l2_active_gop = safe_gop;
        }
    }

    enc->fps = safe_fps;
    enc->gop = safe_gop;
    enc->bitrate_bps = safe_bitrate_bps;
    ESP_LOGI(TAG,
             "H264 encoder rate updated in place: fps=%u->%u bitrate=%u->%u gop=%u->%u cost=%uus",
             old_fps,
             safe_fps,
             (unsigned)old_bitrate_bps,
             (unsigned)safe_bitrate_bps,
             old_gop,
             safe_gop,
             (unsigned)(esp_timer_get_time() - update_started_us));
    return ESP_OK;
}

static bool camera_pipeline_h264_take_reserved(camera_pipeline_h264_encoder_t *enc,
                                               uint16_t width,
                                               uint16_t height,
                                               uint8_t fps,
                                               uint32_t bitrate_bps,
                                               uint8_t min_qp,
                                               uint8_t max_qp,
                                               size_t output_buffer_bytes)
{
    bool taken = false;
    bool rate_update_required = false;
    bool exact_match = false;
    bool static_match = false;

    if (enc == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    exact_match = camera_pipeline_h264_matches(&s_reserved_h264,
                                               width,
                                               height,
                                               fps,
                                               bitrate_bps,
                                               min_qp,
                                               max_qp,
                                               output_buffer_bytes);
    static_match = camera_pipeline_h264_static_config_matches(
        &s_reserved_h264,
        width,
        height,
        min_qp,
        max_qp,
        output_buffer_bytes);
    if (exact_match || static_match) {
        *enc = s_reserved_h264;
        s_reserved_h264 = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
        taken = true;
        rate_update_required = !exact_match;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (taken && rate_update_required) {
        esp_err_t ret =
            camera_pipeline_h264_update_rate(enc, fps, bitrate_bps);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "H264 reserved resource rate update failed: %s",
                     esp_err_to_name(ret));
            camera_pipeline_h264_close(enc);
            taken = false;
        }
    }

    if (taken) {
        APP_LOG_DETAIL(TAG,
                       "H264 encoder reserved resource adopted: mode=%s size=%ux%u fps=%u gop=%u bitrate=%u qp=%u-%u out_buf=%u internal_largest=%u dma_largest=%u",
                 enc->direct_encoder ? "direct_hw" : "v4l2_m2m",
                 width,
                 height,
                 fps,
                 (unsigned)enc->gop,
                 (unsigned)bitrate_bps,
                 (unsigned)enc->min_qp,
                 (unsigned)enc->max_qp,
                 (unsigned)output_buffer_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    return taken;
}

static bool camera_pipeline_h264_store_reserved(camera_pipeline_h264_encoder_t *enc,
                                                const char *reason)
{
    bool stored = false;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t fps = 0;
    uint32_t bitrate_bps = 0;
    size_t output_buffer_bytes = 0;

    if (enc == NULL || !camera_pipeline_h264_is_open(enc)) {
        return false;
    }

    width = enc->width;
    height = enc->height;
    fps = enc->fps;
    bitrate_bps = enc->bitrate_bps;
    output_buffer_bytes = enc->output_buffer_bytes;

    taskENTER_CRITICAL(&s_lock);
    if (!camera_pipeline_h264_is_open(&s_reserved_h264)) {
        s_reserved_h264 = *enc;
        *enc = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
        stored = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (stored) {
        APP_LOG_DETAIL(TAG,
                       "H264 encoder reserved resource stored: reason=%s mode=%s size=%ux%u fps=%u gop=%u bitrate=%u qp=%u-%u out_buf=%u internal_largest=%u dma_largest=%u",
                 reason != NULL ? reason : "unknown",
                 s_reserved_h264.direct_encoder ? "direct_hw" : "v4l2_m2m",
                 width,
                 height,
                 fps,
                 (unsigned)s_reserved_h264.gop,
                 (unsigned)bitrate_bps,
                 (unsigned)s_reserved_h264.min_qp,
                 (unsigned)s_reserved_h264.max_qp,
                 (unsigned)output_buffer_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    return stored;
}

static bool camera_pipeline_build_call_scaler_config(video_yuv420_scaler_config_t *config)
{
    media_governor_video_config_t video_config = {0};
    media_governor_camera_policy_t policy = {0};

    if (config == NULL) {
        return false;
    }

    media_governor_build_device_call_video_config(&video_config);
    media_governor_build_camera_policy(&video_config, &policy);
    *config = (video_yuv420_scaler_config_t) {
        .input_width = camera_pipeline_even_dimension(policy.capture_width),
        .input_height = camera_pipeline_even_dimension(policy.capture_height),
        .output_width = camera_pipeline_even_dimension(policy.rtc_width),
        .output_height = camera_pipeline_even_dimension(policy.rtc_height),
    };

    return config->input_width > 0U && config->input_height > 0U &&
           config->output_width > 0U && config->output_height > 0U &&
           config->output_width <= config->input_width &&
           config->output_height <= config->input_height &&
           (config->input_width != config->output_width ||
            config->input_height != config->output_height);
}

static video_yuv420_scaler_handle_t camera_pipeline_scaler_take_reserved(
    const video_yuv420_scaler_config_t *config)
{
    video_yuv420_scaler_handle_t scaler = NULL;

    if (config == NULL) {
        return NULL;
    }

    taskENTER_CRITICAL(&s_lock);
    if (video_yuv420_scaler_matches(s_reserved_call_scaler, config)) {
        scaler = s_reserved_call_scaler;
        s_reserved_call_scaler = NULL;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (scaler != NULL) {
        APP_LOG_DETAIL(TAG,
                       "call YUV420 scaler reserved resource adopted: input=%ux%u output=%ux%u",
                 config->input_width,
                 config->input_height,
                 config->output_width,
                 config->output_height);
    }
    return scaler;
}

static bool camera_pipeline_scaler_store_reserved(video_yuv420_scaler_handle_t *scaler,
                                                  const char *reason)
{
    video_yuv420_scaler_config_t call_config = {0};
    bool stored = false;

    if (scaler == NULL || *scaler == NULL ||
        !camera_pipeline_build_call_scaler_config(&call_config) ||
        !video_yuv420_scaler_matches(*scaler, &call_config)) {
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_reserved_call_scaler == NULL) {
        s_reserved_call_scaler = *scaler;
        *scaler = NULL;
        stored = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (stored) {
        APP_LOG_DETAIL(TAG,
                       "call YUV420 scaler reserved resource stored: reason=%s input=%ux%u output=%ux%u",
                 reason != NULL ? reason : "unknown",
                 call_config.input_width,
                 call_config.input_height,
                 call_config.output_width,
                 call_config.output_height);
    }
    return stored;
}

static void camera_pipeline_scaler_release(video_yuv420_scaler_handle_t *scaler,
                                           const char *reason)
{
    if (scaler == NULL || *scaler == NULL) {
        return;
    }

    if (!camera_pipeline_scaler_store_reserved(scaler, reason)) {
        video_yuv420_scaler_destroy(*scaler);
        *scaler = NULL;
    }
}

static esp_err_t camera_pipeline_h264_encode(camera_pipeline_h264_encoder_t *enc,
                                             const uint8_t *input_data,
                                             size_t input_len,
                                             const uint8_t **data,
                                             size_t *data_len,
                                             bool *key_frame,
                                             camera_pipeline_h264_timing_t *timing)
{
    ESP_RETURN_ON_FALSE(enc != NULL && input_data != NULL && data != NULL && data_len != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid H264 encode args");
    ESP_RETURN_ON_FALSE(camera_pipeline_h264_is_open(enc),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "H264 encoder not open");

    if (enc->direct_encoder) {
        if (timing != NULL) {
            *timing = (camera_pipeline_h264_timing_t) {0};
        }

        if (camera_pipeline_take_key_frame_request()) {
            (void)camera_pipeline_h264_force_next_idr(enc, "peer-request");
        }

        int64_t sync_start_us = esp_timer_get_time();
        esp_err_t sync_ret = camera_pipeline_sync_h264_input_for_device(input_data, input_len);
        if (timing != NULL) {
            timing->sync_in_us = esp_timer_get_time() - sync_start_us;
        }
        if (sync_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "sync direct H264 input cache failed: len=%u ret=%s",
                     (unsigned)input_len,
                     esp_err_to_name(sync_ret));
            return sync_ret;
        }

        esp_h264_enc_in_frame_t in_frame = {
            .raw_data = {
                .buffer = (uint8_t *)input_data,
                .len = (uint32_t)input_len,
            },
            .pts = (uint32_t)(esp_timer_get_time() / 1000LL),
        };
        esp_h264_enc_out_frame_t out_frame = {
            .raw_data = {
                .buffer = enc->capture_buffer,
                .len = (uint32_t)enc->capture_buffer_size,
            },
        };

        int64_t hw_start_us = esp_timer_get_time();
        esp_h264_err_t h264_ret = esp_h264_enc_process(enc->direct_handle, &in_frame, &out_frame);
        if (timing != NULL) {
            timing->hw_us = esp_timer_get_time() - hw_start_us;
        }
        if (h264_ret != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG,
                     "direct H264 encode failed err=%d input=%u out_cap=%u",
                     h264_ret,
                     (unsigned)input_len,
                     (unsigned)enc->capture_buffer_size);
            return camera_pipeline_h264_error_to_esp(h264_ret);
        }

        if (out_frame.length > 0U) {
            sync_start_us = esp_timer_get_time();
            sync_ret = camera_pipeline_sync_h264_output_for_cpu(enc->capture_buffer, out_frame.length);
            if (timing != NULL) {
                timing->sync_out_us = esp_timer_get_time() - sync_start_us;
            }
            if (sync_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "sync direct H264 output cache failed: len=%u ret=%s",
                         (unsigned)out_frame.length,
                         esp_err_to_name(sync_ret));
            }
        }

        *data = enc->capture_buffer;
        *data_len = out_frame.length;
        if (key_frame != NULL) {
            *key_frame = out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                         out_frame.frame_type == ESP_H264_FRAME_TYPE_I ||
                         camera_pipeline_h264_is_key_frame(enc->capture_buffer, out_frame.length);
        }
        return ESP_OK;
    }

    struct v4l2_buffer out_buf = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    out_buf.m.userptr = (unsigned long)input_data;
    out_buf.length = (uint32_t)input_len;
    out_buf.bytesused = (uint32_t)input_len;

    if (timing != NULL) {
        *timing = (camera_pipeline_h264_timing_t) {0};
    }

    if (camera_pipeline_take_key_frame_request()) {
        (void)camera_pipeline_h264_force_next_idr(enc, "peer-request");
    }

    int64_t sync_start_us = esp_timer_get_time();
    esp_err_t sync_ret = camera_pipeline_sync_h264_input_for_device(input_data, input_len);
    if (timing != NULL) {
        timing->sync_in_us = esp_timer_get_time() - sync_start_us;
    }
    if (sync_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "sync H264 input cache failed: len=%u ret=%s",
                 (unsigned)input_len,
                 esp_err_to_name(sync_ret));
        return sync_ret;
    }

    int64_t hw_start_us = esp_timer_get_time();
    if (ioctl(enc->fd, VIDIOC_QBUF, &out_buf) != 0) {
        ESP_LOGW(TAG, "queue H264 input frame failed errno=%d", errno);
        return ESP_FAIL;
    }

    struct v4l2_buffer cap_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(enc->fd, VIDIOC_DQBUF, &cap_buf) != 0) {
        ESP_LOGW(TAG, "dequeue H264 output frame failed errno=%d", errno);
        (void)ioctl(enc->fd, VIDIOC_DQBUF, &out_buf);
        return ESP_FAIL;
    }
    if (timing != NULL) {
        timing->hw_us = esp_timer_get_time() - hw_start_us;
    }
    if (cap_buf.bytesused > 0U) {
        sync_start_us = esp_timer_get_time();
        esp_err_t sync_ret =
            camera_pipeline_sync_h264_output_for_cpu(enc->capture_buffer, cap_buf.bytesused);
        if (timing != NULL) {
            timing->sync_out_us = esp_timer_get_time() - sync_start_us;
        }
        if (sync_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "sync H264 output cache failed: len=%u ret=%s",
                     (unsigned)cap_buf.bytesused,
                     esp_err_to_name(sync_ret));
        }
    }

    struct v4l2_buffer done_out = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(enc->fd, VIDIOC_DQBUF, &done_out) != 0) {
        ESP_LOGW(TAG, "dequeue H264 input frame failed errno=%d", errno);
    }

    *data = enc->capture_buffer;
    *data_len = cap_buf.bytesused;
    if (key_frame != NULL) {
        *key_frame = (cap_buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0 ||
                     camera_pipeline_h264_is_key_frame(enc->capture_buffer, cap_buf.bytesused);
    }
    return ESP_OK;
}

static void camera_pipeline_h264_return_output(camera_pipeline_h264_encoder_t *enc)
{
    if (enc == NULL || enc->fd < 0 || enc->capture_buffer == NULL) {
        if (enc != NULL && enc->direct_encoder) {
            return;
        }
        return;
    }

    struct v4l2_buffer cap_buf = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(enc->fd, VIDIOC_QBUF, &cap_buf) != 0) {
        ESP_LOGW(TAG, "requeue H264 output buffer failed errno=%d", errno);
    }
}

static void camera_pipeline_task(void *arg)
{
    (void)arg;

    camera_pipeline_mark_task_started();

    camera_pipeline_h264_encoder_t h264 = {
        .fd = -1,
    };
    video_yuv420_scaler_handle_t yuv420_scaler = NULL;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t last_rtc_tick = 0;
    TickType_t last_stats_tick = start_tick;
    uint32_t upstream_count = 0;
    uint32_t drop_count = 0;
    uint32_t capture_fail_count = 0;
    uint32_t convert_fail_count = 0;
    uint32_t encode_fail_count = 0;
    uint32_t total_payload_bytes = 0;
    uint32_t backpressure_skip_count = 0;
    uint32_t transport_guard_drop_count = 0;
    uint32_t key_wait_drop_count = 0;
    uint32_t encoded_frame_count = 0;
    uint32_t key_frame_count = 0;
    uint32_t slow_capture_count = 0;
    uint32_t slow_convert_count = 0;
    uint32_t slow_encode_count = 0;
    uint32_t slow_callback_count = 0;
    uint32_t slow_loop_count = 0;
    uint32_t large_frame_count = 0;
    uint32_t min_payload_bytes = UINT32_MAX;
    uint32_t max_payload_bytes = 0;
    uint64_t frame_gap_us_total = 0;
    uint64_t max_frame_gap_us = 0;
    uint64_t last_frame_start_us = 0;
    uint64_t stream_start_us = 0;
    uint64_t capture_us_total = 0;
    uint64_t convert_us_total = 0;
    uint64_t encode_us_total = 0;
    uint64_t h264_sync_in_us_total = 0;
    uint64_t h264_hw_us_total = 0;
    uint64_t h264_sync_out_us_total = 0;
    uint64_t callback_us_total = 0;
    uint64_t loop_us_total = 0;
    uint64_t media_timestamp_lag_us_total = 0;
    uint64_t max_media_timestamp_lag_us = 0;
    uint64_t camera_sequence_delta_total_x10 = 0;
    uint32_t capture_sample_count = 0;
    uint32_t convert_sample_count = 0;
    uint32_t encode_sample_count = 0;
    uint32_t callback_sample_count = 0;
    uint32_t loop_sample_count = 0;
    uint32_t media_timestamp_sample_count = 0;
    uint32_t camera_sequence_sample_count = 0;
    uint32_t camera_stale_frame_drain_count = 0;
    uint32_t max_camera_sequence_delta = 0;
    uint32_t last_camera_sequence = 0;
    uint32_t trace_frame_count = 0;
    camera_pipeline_luma_stats_t source_luma_stats = {0};
    camera_pipeline_luma_stats_t encoder_luma_stats = {0};
    bool first_frame_logged = false;
    bool last_camera_sequence_valid = false;
    bool stream_start_key_frame_requested = false;
    bool video_subsystem_prepared = false;
    bool camera_acquired = false;
    TickType_t next_h264_open_tick = 0;
    TickType_t last_scaler_fail_log_tick = 0;
    uint16_t h264_fallback_width = 0;
    uint16_t h264_fallback_height = 0;
    bool key_frame_required_after_drop = false;
    bool backpressure_key_request_pending = false;
    bool backpressure_hold_counted = false;

    ESP_LOGI(TAG, "camera pipeline starting: mode=h264_upstream_only");
    camera_pipeline_reset_metrics();
    vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_START_DELAY_MS));

    esp_err_t ret = camera_driver_prepare_video_subsystem();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera video subsystem prepare failed: %s", esp_err_to_name(ret));
        goto exit_task;
    }
    video_subsystem_prepared = true;

    media_governor_camera_policy_t preopen_policy = {0};
    camera_pipeline_get_policy(&preopen_policy);
    uint16_t preopen_width = camera_pipeline_even_dimension(preopen_policy.rtc_width);
    uint16_t preopen_height = camera_pipeline_even_dimension(preopen_policy.rtc_height);
    if (preopen_width > 0U && preopen_height > 0U && preopen_policy.rtc_video_fps > 0U) {
        uint16_t capture_width = camera_pipeline_even_dimension(preopen_policy.capture_width);
        uint16_t capture_height = camera_pipeline_even_dimension(preopen_policy.capture_height);
        uint16_t open_width = preopen_width;
        uint16_t open_height = preopen_height;
        bool reserved_h264 =
            camera_pipeline_h264_take_reserved(&h264,
                                               preopen_width,
                                               preopen_height,
                                               preopen_policy.rtc_video_fps,
                                               preopen_policy.h264_bitrate_bps,
                                               preopen_policy.h264_min_qp,
                                               preopen_policy.h264_max_qp,
                                               preopen_policy.h264_output_buffer_bytes);

        if (!reserved_h264 &&
            CAMERA_PIPELINE_H264_RESOURCE_FALLBACK_ENABLE &&
            camera_pipeline_select_h264_internal_fit(preopen_width,
                                                     preopen_height,
                                                     &h264_fallback_width,
                                                     &h264_fallback_height)) {
            open_width = h264_fallback_width;
            open_height = h264_fallback_height;
            if (open_width == MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH &&
                open_height == MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT) {
                capture_width = open_width;
                capture_height = open_height;
            }
            ESP_LOGI(TAG,
                     "H264 encoder resource-fit profile: requested=%ux%u selected=%ux%u capture=%ux%u path=%s internal_largest=%u ref_est=%u",
                     preopen_width,
                     preopen_height,
                     open_width,
                     open_height,
                     capture_width,
                     capture_height,
                     capture_width == open_width && capture_height == open_height ?
                         "yuv420-direct" : "yuv420-ppa-scale",
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)camera_pipeline_h264_ref_internal_estimate(open_width));
        }
        if (capture_width == 0U || capture_height == 0U) {
            capture_width = open_width;
            capture_height = open_height;
        }
        esp_err_t camera_target_ret =
            camera_driver_set_stream_target(capture_width, capture_height, preopen_policy.capture_fps);
        if (camera_target_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "camera stream target preopen failed: %ux%u@%u %s",
                     capture_width,
                     capture_height,
                     preopen_policy.capture_fps,
                     esp_err_to_name(camera_target_ret));
        }
        APP_LOG_DETAIL(TAG,
                       "H264 encoder preopen before camera buffers: size=%ux%u fps=%u bitrate=%u resource_fallback=%d",
                 preopen_width,
                 preopen_height,
                 preopen_policy.rtc_video_fps,
                 (unsigned)preopen_policy.h264_bitrate_bps,
                  CAMERA_PIPELINE_H264_RESOURCE_FALLBACK_ENABLE);
        if (reserved_h264) {
            ret = ESP_OK;
        } else {
            ret = camera_pipeline_h264_open_with_dma_escrow(&h264,
                                                            open_width,
                                                            open_height,
                                                            preopen_policy.rtc_video_fps,
                                                            preopen_policy.h264_bitrate_bps,
                                                            preopen_policy.h264_min_qp,
                                                            preopen_policy.h264_max_qp,
                                                            preopen_policy.h264_output_buffer_bytes,
                                                            "pipeline-preopen");
        }
        if (ret != ESP_OK) {
            encode_fail_count++;
            if (h264_fallback_width == 0U &&
                CAMERA_PIPELINE_H264_RESOURCE_FALLBACK_ENABLE &&
                camera_pipeline_select_h264_internal_fit(preopen_width,
                                                         preopen_height,
                                                         &h264_fallback_width,
                                                         &h264_fallback_height)) {
                ESP_LOGW(TAG,
                         "H264 encoder preopen failed at %ux%u: %s, fallback=%ux%u internal_largest=%u ref_est=%u",
                         preopen_width,
                         preopen_height,
                         esp_err_to_name(ret),
                         h264_fallback_width,
                         h264_fallback_height,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)camera_pipeline_h264_ref_internal_estimate(h264_fallback_width));
                ret = camera_pipeline_h264_open_with_dma_escrow(&h264,
                                                                h264_fallback_width,
                                                                h264_fallback_height,
                                                                preopen_policy.rtc_video_fps,
                                                                preopen_policy.h264_bitrate_bps,
                                                                preopen_policy.h264_min_qp,
                                                                preopen_policy.h264_max_qp,
                                                                preopen_policy.h264_output_buffer_bytes,
                                                                "pipeline-preopen-fallback");
            }
            if (ret != ESP_OK) {
                next_h264_open_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
                ESP_LOGW(TAG,
                         "H264 encoder preopen failed at %ux%u: %s, retry after %ums",
                         preopen_width,
                         preopen_height,
                         esp_err_to_name(ret),
                         (unsigned)CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
            }
        }

    }

    ret = camera_driver_acquire();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera pipeline acquire failed: %s", esp_err_to_name(ret));
        goto exit_task;
    }
    camera_acquired = true;

    while (camera_pipeline_should_run()) {
        camera_pipeline_runtime_t runtime = camera_pipeline_snapshot();
        media_governor_camera_policy_t policy = {0};
        camera_pipeline_get_policy(&policy);

        uint32_t rtc_interval_ms = camera_pipeline_interval_ms(policy.rtc_video_fps);
        TickType_t now_tick = xTaskGetTickCount();
        bool rtc_ready = runtime.rtc_enabled &&
                         runtime.video_cb != NULL &&
                         policy.rtc_video_fps > 0U;

        /* A short transport hold must not consume the next capture deadline.
         * Resume immediately when pressure clears instead of adding another
         * complete frame interval to the network pause. */
        if (rtc_ready && media_governor_is_network_backpressured()) {
            TickType_t interval_ticks =
                pdMS_TO_TICKS(rtc_interval_ms == 0U ? 1U : rtc_interval_ms);
            bool frame_was_due = last_rtc_tick == 0 ||
                                 now_tick - last_rtc_tick >= interval_ticks;
            if (frame_was_due && !backpressure_hold_counted) {
                drop_count++;
                backpressure_skip_count++;
                backpressure_hold_counted = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        backpressure_hold_counted = false;

        bool do_rtc = rtc_ready &&
                      camera_pipeline_time_due(now_tick, &last_rtc_tick, rtc_interval_ms);
        if (!do_rtc) {
            uint32_t wait_ms = 5U;
            if (runtime.rtc_enabled && runtime.video_cb != NULL && policy.rtc_video_fps > 0U) {
                wait_ms = camera_pipeline_wait_until_due_ms(now_tick,
                                                            last_rtc_tick,
                                                            rtc_interval_ms);
            }
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }

        /* Capture only at the RTC cadence. The driver already returns the
         * newest completed frame; extra dequeue/requeue cycles would select
         * older buffers and add needless full-duplex scheduling pressure. */
        camera_driver_frame_t frame = {0};
        int64_t loop_start_us = esp_timer_get_time();
        int64_t capture_start_us = loop_start_us;
        ret = camera_driver_capture(&frame);
        int64_t capture_done_us = esp_timer_get_time();
        int64_t capture_us = capture_done_us - capture_start_us;
        if (ret != ESP_OK) {
            capture_fail_count++;
            ESP_LOGW(TAG, "camera capture failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        }
        capture_us_total += (uint64_t)capture_us;
        capture_sample_count++;
        camera_stale_frame_drain_count += frame.stale_frames_dropped;

        if (last_frame_start_us != 0U &&
            capture_done_us > (int64_t)last_frame_start_us) {
            uint64_t gap_us = (uint64_t)capture_done_us - last_frame_start_us;
            frame_gap_us_total += gap_us;
            if (gap_us > max_frame_gap_us) {
                max_frame_gap_us = gap_us;
            }
        }
        last_frame_start_us = (uint64_t)capture_done_us;
        if (last_camera_sequence_valid) {
            /* The P4 camera V4L2 driver may leave sequence at zero. Treat a
             * positive delta as diagnostics only; never gate video on it. */
            uint32_t sequence_delta = frame.sequence - last_camera_sequence;
            if (sequence_delta > 0U) {
                camera_sequence_delta_total_x10 += (uint64_t)sequence_delta * 10ULL;
                camera_sequence_sample_count++;
                if (sequence_delta > max_camera_sequence_delta) {
                    max_camera_sequence_delta = sequence_delta;
                }
            }
        }
        last_camera_sequence = frame.sequence;
        last_camera_sequence_valid = true;

        uint16_t source_width = frame.width;
        uint16_t source_height = frame.height;
        camera_driver_pixel_format_t source_format = frame.pixel_format;
        size_t source_data_len = frame.data_len;
        uint32_t source_sequence = frame.sequence;
        uint32_t source_stale_frames_dropped = frame.stale_frames_dropped;
        camera_pipeline_luma_probe_t source_luma_probe = {0};
        if (source_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY &&
            camera_pipeline_probe_ouev_luma(frame.data,
                                            source_data_len,
                                            source_width,
                                            source_height,
                                            &source_luma_probe)) {
            camera_pipeline_luma_stats_update(&source_luma_stats, &source_luma_probe);
        }

        uint16_t target_width = camera_pipeline_even_dimension(policy.rtc_width);
        uint16_t target_height = camera_pipeline_even_dimension(policy.rtc_height);
        /* Phone/H5 portrait output corrects the physical camera mounting.
         * Device-call's separate decoder-limited profile remains unchanged. */
        const bool rotate_ccw90 = target_width == APP_MEDIA_WECHAT_VIDEO_WIDTH &&
                                  target_height == APP_MEDIA_WECHAT_VIDEO_HEIGHT;
        if (!rotate_ccw90 && (target_width == 0U || target_width > source_width)) {
            target_width = camera_pipeline_even_dimension(source_width);
        }
        if (!rotate_ccw90 && (target_height == 0U || target_height > source_height)) {
            target_height = camera_pipeline_even_dimension(source_height);
        }
        if (camera_pipeline_h264_is_open(&h264) &&
            h264.width <= target_width &&
            h264.height <= target_height) {
            target_width = h264.width;
            target_height = h264.height;
        } else {
            if ((h264_fallback_width == 0U || h264_fallback_height == 0U) &&
                CAMERA_PIPELINE_H264_RESOURCE_FALLBACK_ENABLE) {
                (void)camera_pipeline_select_h264_internal_fit(target_width,
                                                               target_height,
                                                               &h264_fallback_width,
                                                               &h264_fallback_height);
            }
            if (h264_fallback_width > 0U &&
                h264_fallback_height > 0U &&
                h264_fallback_width <= target_width &&
                h264_fallback_height <= target_height) {
                target_width = h264_fallback_width;
                target_height = h264_fallback_height;
            }
        }
        size_t h264_input_len = camera_pipeline_h264_input_size(target_width, target_height);
        const uint8_t *h264_input_data = NULL;
        bool frame_released = false;
        int64_t convert_us = 0;
        const char *h264_input_path = "unknown";
        bool h264_direct_input = false;

        if (!rotate_ccw90 && source_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY &&
            frame.data != NULL &&
            source_width == target_width &&
            source_height == target_height &&
            source_data_len >= h264_input_len) {
            camera_pipeline_scaler_release(&yuv420_scaler, "direct-input");
            h264_input_data = frame.data;
            h264_input_path = "yuv420-direct";
            h264_direct_input = true;
        } else if (source_format == CAMERA_DRIVER_PIXEL_FORMAT_RGB565) {
            camera_driver_release(&frame);
            frame_released = true;
            capture_fail_count++;
            ESP_LOGW(TAG,
                     "RTC video rejects RGB565 main path: source=%ux%u target=%ux%u bytes=%u",
                     source_width,
                     source_height,
                     target_width,
                     target_height,
                     (unsigned)source_data_len);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        } else if (source_format == CAMERA_DRIVER_PIXEL_FORMAT_YUV420_OUYY_EVYY &&
                   frame.data != NULL &&
                   target_width > 0U && target_height > 0U) {
            video_yuv420_scaler_config_t scaler_config = {
                .input_width = source_width,
                .input_height = source_height,
                .output_width = target_width,
                .output_height = target_height,
                .rotate_ccw90 = rotate_ccw90,
            };
            if (!video_yuv420_scaler_matches(yuv420_scaler, &scaler_config)) {
                camera_pipeline_scaler_release(&yuv420_scaler, "profile-change");
                yuv420_scaler = camera_pipeline_scaler_take_reserved(&scaler_config);
                ret = yuv420_scaler != NULL ? ESP_OK :
                    video_yuv420_scaler_create(&scaler_config, &yuv420_scaler);
                if (ret != ESP_OK) {
                    camera_driver_release(&frame);
                    frame_released = true;
                    drop_count++;
                    convert_fail_count++;
                    TickType_t fail_tick = xTaskGetTickCount();
                    if (last_scaler_fail_log_tick == 0 ||
                        fail_tick - last_scaler_fail_log_tick >= pdMS_TO_TICKS(5000)) {
                        last_scaler_fail_log_tick = fail_tick;
                        ESP_LOGW(TAG,
                                 "YUV420 scaler create failed: source=%ux%u target=%ux%u %s",
                                 source_width,
                                 source_height,
                                 target_width,
                                 target_height,
                                 esp_err_to_name(ret));
                    }
                    vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
                    continue;
                }
            }

            int64_t convert_start_us = esp_timer_get_time();
            ret = video_yuv420_scaler_process(yuv420_scaler,
                                              frame.data,
                                              source_data_len,
                                              &h264_input_data,
                                              &h264_input_len);
            convert_us = esp_timer_get_time() - convert_start_us;
            camera_driver_release(&frame);
            frame_released = true;
            if (ret != ESP_OK || h264_input_data == NULL ||
                h264_input_len < camera_pipeline_h264_input_size(target_width, target_height)) {
                if (ret == ESP_OK) {
                    ret = ESP_ERR_INVALID_SIZE;
                }
                drop_count++;
                convert_fail_count++;
                TickType_t fail_tick = xTaskGetTickCount();
                if (last_scaler_fail_log_tick == 0 ||
                    fail_tick - last_scaler_fail_log_tick >= pdMS_TO_TICKS(5000)) {
                    last_scaler_fail_log_tick = fail_tick;
                    ESP_LOGW(TAG,
                             "YUV420 scaler process failed: source=%ux%u target=%ux%u in=%u out=%u %s",
                             source_width,
                             source_height,
                             target_width,
                             target_height,
                             (unsigned)source_data_len,
                             (unsigned)h264_input_len,
                             esp_err_to_name(ret));
                }
                vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
                continue;
            }
            convert_us_total += (uint64_t)convert_us;
            convert_sample_count++;
            if (convert_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US) {
                slow_convert_count++;
            }
            h264_input_path = rotate_ccw90 ? "yuv420-ppa-ccw90" : "yuv420-ppa-scale";
            h264_direct_input = false;
        } else {
            camera_driver_release(&frame);
            frame_released = true;
            capture_fail_count++;
            ESP_LOGW(TAG,
                     "unsupported camera frame for H264: format=%d bytes=%u expected=%u size=%ux%u target=%ux%u",
                     source_format,
                     (unsigned)source_data_len,
                     (unsigned)h264_input_len,
                     source_width,
                     source_height,
                     target_width,
                     target_height);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        }

        camera_pipeline_note_frame_metrics(target_width,
                                           target_height,
                                           policy.rtc_video_fps,
                                           policy.h264_bitrate_bps,
                                           h264_direct_input);

        bool h264_config_ready =
            camera_pipeline_h264_matches(&h264,
                                         target_width,
                                         target_height,
                                         policy.rtc_video_fps,
                                         policy.h264_bitrate_bps,
                                         policy.h264_min_qp,
                                         policy.h264_max_qp,
                                         policy.h264_output_buffer_bytes);
        if (!h264_config_ready &&
            camera_pipeline_h264_static_config_matches(
                &h264,
                target_width,
                target_height,
                policy.h264_min_qp,
                policy.h264_max_qp,
                policy.h264_output_buffer_bytes)) {
            ret = camera_pipeline_h264_update_rate(&h264,
                                                   policy.rtc_video_fps,
                                                   policy.h264_bitrate_bps);
            h264_config_ready = ret == ESP_OK;
        }
        if (!h264_config_ready) {
            camera_pipeline_h264_close(&h264);
            TickType_t open_tick = xTaskGetTickCount();
            if (next_h264_open_tick != 0 && !camera_pipeline_tick_reached(open_tick, next_h264_open_tick)) {
                encode_fail_count++;
                if (!frame_released) {
                    camera_driver_release(&frame);
                    frame_released = true;
                }
                uint32_t wait_ms = pdTICKS_TO_MS(next_h264_open_tick - open_tick);
                ESP_LOGD(TAG, "H264 encoder open backoff: wait=%ums", (unsigned)wait_ms);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            ret = camera_pipeline_h264_open_with_dma_escrow(&h264,
                                                            target_width,
                                                            target_height,
                                                            policy.rtc_video_fps,
                                                            policy.h264_bitrate_bps,
                                                            policy.h264_min_qp,
                                                            policy.h264_max_qp,
                                                            policy.h264_output_buffer_bytes,
                                                            "pipeline-runtime");
            if (ret != ESP_OK) {
                encode_fail_count++;
                if (CAMERA_PIPELINE_H264_RESOURCE_FALLBACK_ENABLE &&
                    camera_pipeline_select_h264_internal_fit(target_width,
                                                             target_height,
                                                             &h264_fallback_width,
                                                             &h264_fallback_height)) {
                    ESP_LOGW(TAG,
                             "H264 encoder fallback selected after open fail: requested=%ux%u fallback=%ux%u internal_largest=%u ref_est=%u",
                             target_width,
                             target_height,
                             h264_fallback_width,
                             h264_fallback_height,
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                             (unsigned)camera_pipeline_h264_ref_internal_estimate(h264_fallback_width));
                }
                next_h264_open_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
                if (!frame_released) {
                    camera_driver_release(&frame);
                    frame_released = true;
                }
                ESP_LOGW(TAG,
                         "H264 encoder open failed: %s, retry after %ums",
                         esp_err_to_name(ret),
                         (unsigned)CAMERA_PIPELINE_H264_OPEN_RETRY_MS);
                vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
                continue;
            }
            next_h264_open_tick = 0;
        }

        const uint8_t *h264_data = NULL;
        size_t h264_len = 0;
        bool key_frame = false;
        camera_pipeline_h264_timing_t h264_timing = {0};
        if (!stream_start_key_frame_requested) {
            (void)camera_pipeline_h264_force_next_idr(&h264, "stream-start");
            (void)camera_pipeline_take_key_frame_request();
            stream_start_key_frame_requested = true;
            stream_start_us = (uint64_t)esp_timer_get_time();
        }
        if (backpressure_key_request_pending) {
            if (camera_pipeline_h264_force_next_idr(&h264, "backpressure-resume")) {
                (void)camera_pipeline_take_key_frame_request();
                backpressure_key_request_pending = false;
            }
        }
        int64_t encode_start_us = esp_timer_get_time();
        ret = camera_pipeline_h264_encode(&h264,
                                          h264_input_data,
                                          h264_input_len,
                                          &h264_data,
                                          &h264_len,
                                          &key_frame,
                                          &h264_timing);
        int64_t encode_us = esp_timer_get_time() - encode_start_us;
        camera_pipeline_luma_probe_t encoder_luma_probe = {0};
        if (camera_pipeline_probe_ouev_luma(h264_input_data,
                                            h264_input_len,
                                            target_width,
                                            target_height,
                                            &encoder_luma_probe)) {
            camera_pipeline_luma_stats_update(&encoder_luma_stats, &encoder_luma_probe);
        }
        if (!frame_released) {
            camera_driver_release(&frame);
            frame_released = true;
        }

        if (ret != ESP_OK || h264_data == NULL || h264_len == 0U) {
            encode_fail_count++;
            drop_count++;
            camera_pipeline_h264_return_output(&h264);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PIPELINE_RETRY_DELAY_MS));
            continue;
        }
        encode_us_total += (uint64_t)encode_us;
        h264_sync_in_us_total += (uint64_t)h264_timing.sync_in_us;
        h264_hw_us_total += (uint64_t)h264_timing.hw_us;
        h264_sync_out_us_total += (uint64_t)h264_timing.sync_out_us;
        encode_sample_count++;
        encoded_frame_count++;
        trace_frame_count++;
        if (key_frame) {
            key_frame_count++;
            key_frame_required_after_drop = false;
        }
        if (h264_len > max_payload_bytes) {
            max_payload_bytes = (uint32_t)h264_len;
        }
        if (h264_len < min_payload_bytes) {
            min_payload_bytes = (uint32_t)h264_len;
        }
        if ((uint32_t)h264_len >= CAMERA_PIPELINE_FRAME_TRACE_LARGE_PAYLOAD_BYTES) {
            large_frame_count++;
        }
        if (capture_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US) {
            slow_capture_count++;
        }
        if (encode_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US) {
            slow_encode_count++;
        }

        camera_pipeline_note_frame_metrics(h264.width,
                                           h264.height,
                                           policy.rtc_video_fps,
                                           policy.h264_bitrate_bps,
                                           h264_direct_input);

        const char *transport_guard_reason = NULL;
        int64_t now_for_guard_us = esp_timer_get_time();
        uint32_t stream_age_ms = 0U;
        if (stream_start_us != 0U && now_for_guard_us > (int64_t)stream_start_us) {
            uint64_t elapsed_us = (uint64_t)now_for_guard_us - stream_start_us;
            stream_age_ms = (uint32_t)(elapsed_us / 1000ULL);
        }
        size_t effective_max_delta = policy.h264_max_delta_payload_bytes;
        if (camera_pipeline_should_hold_video_for_transport(h264_len,
                                                            key_frame,
                                                            &policy,
                                                            stream_age_ms,
                                                            &effective_max_delta,
                                                            &transport_guard_reason)) {
            drop_count++;
            transport_guard_drop_count++;
            if (camera_pipeline_transport_guard_needs_key_frame(transport_guard_reason)) {
                key_frame_required_after_drop = true;
                (void)camera_pipeline_h264_force_next_idr(&h264, "transport-guard");
                (void)camera_pipeline_take_key_frame_request();
            }
            camera_pipeline_log_transport_guard(transport_guard_reason,
                                                h264_len,
                                                key_frame,
                                                &policy,
                                                effective_max_delta,
                                                stream_age_ms);
            camera_pipeline_h264_return_output(&h264);
            int64_t loop_us = esp_timer_get_time() - loop_start_us;
            loop_us_total += (uint64_t)loop_us;
            loop_sample_count++;
            if (loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US) {
                slow_loop_count++;
            }
            continue;
        }

        if (key_frame_required_after_drop && !key_frame) {
            drop_count++;
            key_wait_drop_count++;
            camera_pipeline_h264_return_output(&h264);
            int64_t loop_us = esp_timer_get_time() - loop_start_us;
            loop_us_total += (uint64_t)loop_us;
            loop_sample_count++;
            if (loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US) {
                slow_loop_count++;
            }
            continue;
        }

        /*
         * Keep the media timestamp on the capture cadence.  Using the time
         * after H264 encode makes hardware-encoder jitter look like frame-time
         * jitter to the RTC stack and can show up as tiny visual stalls.
         */
        uint64_t pts_us = (uint64_t)capture_done_us;
        int64_t callback_start_us = esp_timer_get_time();
        uint64_t media_timestamp_lag_us =
            callback_start_us > capture_done_us ?
                (uint64_t)(callback_start_us - capture_done_us) :
                camera_pipeline_abs_delta_us((uint64_t)callback_start_us, pts_us);
        media_timestamp_lag_us_total += media_timestamp_lag_us;
        media_timestamp_sample_count++;
        if (media_timestamp_lag_us > max_media_timestamp_lag_us) {
            max_media_timestamp_lag_us = media_timestamp_lag_us;
        }
        ret = runtime.video_cb(h264_data,
                               h264_len,
                               h264.width,
                               h264.height,
                               pts_us,
                               TIRTC_VIDEO_H264,
                               key_frame,
                               runtime.video_ctx);
        int64_t callback_us = esp_timer_get_time() - callback_start_us;
        camera_pipeline_h264_return_output(&h264);
        int64_t loop_us = esp_timer_get_time() - loop_start_us;
        callback_us_total += (uint64_t)callback_us;
        callback_sample_count++;
        loop_us_total += (uint64_t)loop_us;
        loop_sample_count++;
        if (callback_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US) {
            slow_callback_count++;
        }
        if (loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US) {
            slow_loop_count++;
        }

        bool trace_initial = trace_frame_count <= CAMERA_PIPELINE_FRAME_TRACE_INITIAL_COUNT;
        bool trace_large = (uint32_t)h264_len >= CAMERA_PIPELINE_FRAME_TRACE_LARGE_PAYLOAD_BYTES;
        bool trace_slow = capture_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US ||
                          convert_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US ||
                          encode_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US ||
                          callback_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_STAGE_US ||
                          loop_us > CAMERA_PIPELINE_FRAME_TRACE_SLOW_LOOP_US;
        TickType_t trace_tick = xTaskGetTickCount();
        bool trace_period_due = s_last_frame_trace_log_tick == 0 ||
                                trace_tick - s_last_frame_trace_log_tick >=
                                    pdMS_TO_TICKS(CAMERA_PIPELINE_FRAME_TRACE_INTERVAL_MS);
        if (trace_initial || ((trace_large || trace_slow || ret != ESP_OK) && trace_period_due)) {
            s_last_frame_trace_log_tick = trace_tick;
            APP_LOG_DETAIL(TAG,
                           "camera frame trace: idx=%lu ret=%s key=%d payload=%u seq=%lu drain=%lu cap=%lldus scale=%lldus enc=%lldus sync_in=%lldus hw=%lldus sync_out=%lldus cb=%lldus loop=%lldus target=%ux%u@%u bitrate_cfg=%u max_delta=%u",
                     (unsigned long)trace_frame_count,
                     esp_err_to_name(ret),
                     key_frame ? 1 : 0,
                     (unsigned)h264_len,
                     (unsigned long)source_sequence,
                     (unsigned long)source_stale_frames_dropped,
                     (long long)capture_us,
                     (long long)convert_us,
                     (long long)encode_us,
                     (long long)h264_timing.sync_in_us,
                     (long long)h264_timing.hw_us,
                     (long long)h264_timing.sync_out_us,
                     (long long)callback_us,
                     (long long)loop_us,
                     (unsigned)h264.width,
                     (unsigned)h264.height,
                     (unsigned)policy.rtc_video_fps,
                     (unsigned)policy.h264_bitrate_bps,
                     (unsigned)policy.h264_max_delta_payload_bytes);
        }

        if (ret == ESP_OK) {
            upstream_count++;
            total_payload_bytes += h264_len;
            if (!first_frame_logged) {
                first_frame_logged = true;
                uint8_t head0 = h264_len > 0U ? h264_data[0] : 0;
                uint8_t head1 = h264_len > 1U ? h264_data[1] : 0;
                uint8_t head2 = h264_len > 2U ? h264_data[2] : 0;
                uint8_t head3 = h264_len > 3U ? h264_data[3] : 0;
                ESP_LOGI(TAG,
                         "camera pipeline first upstream frame: source=%ux%u input=%s output=%ux%u@%u gop=%u bitrate=%u media=h264 key=%d payload=%u seq=%lu drain=%lu head=%02X%02X%02X%02X capture=%lldus convert=%lldus encode=%lldus sync_in=%lldus hw=%lldus sync_out=%lldus cb=%lldus loop=%lldus",
                         source_width,
                         source_height,
                         h264_input_path,
                         h264.width,
                         h264.height,
                         (unsigned)h264.fps,
                         (unsigned)h264.gop,
                         (unsigned)h264.bitrate_bps,
                         key_frame ? 1 : 0,
                         (unsigned)h264_len,
                         (unsigned long)source_sequence,
                         (unsigned long)source_stale_frames_dropped,
                         head0,
                         head1,
                         head2,
                         head3,
                         (long long)capture_us,
                         (long long)convert_us,
                         (long long)encode_us,
                         (long long)h264_timing.sync_in_us,
                         (long long)h264_timing.hw_us,
                         (long long)h264_timing.sync_out_us,
                         (long long)callback_us,
                         (long long)loop_us);
            }
        } else {
            drop_count++;
            if (ret != ESP_ERR_INVALID_STATE) {
                /*
                 * This access unit was encoded but not accepted downstream.
                 * Later P-frames can depend on it, so resume from a fresh IDR.
                 */
                key_frame_required_after_drop = true;
                backpressure_key_request_pending = true;
                media_governor_note_network_backpressure();
            }
            TickType_t fail_tick = xTaskGetTickCount();
            if (s_last_rtc_fail_log_tick == 0 ||
                fail_tick - s_last_rtc_fail_log_tick >= pdMS_TO_TICKS(1000)) {
                s_last_rtc_fail_log_tick = fail_tick;
                ESP_LOG_LEVEL_LOCAL(ret == ESP_ERR_INVALID_STATE ? ESP_LOG_DEBUG : ESP_LOG_WARN,
                                    TAG,
                                    "camera pipeline upstream frame dropped: %s",
                                    esp_err_to_name(ret));
            }
        }

        now_tick = xTaskGetTickCount();
        if (now_tick - last_stats_tick >= pdMS_TO_TICKS(CAMERA_PIPELINE_LOG_INTERVAL_MS)) {
            uint32_t elapsed_ms = pdTICKS_TO_MS(now_tick - last_stats_tick);
            if (elapsed_ms == 0U) {
                elapsed_ms = 1U;
            }
            uint32_t avg_payload = upstream_count > 0U ? total_payload_bytes / upstream_count : 0U;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS || CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
            uint32_t avg_capture_us = capture_sample_count > 0U ? (uint32_t)(capture_us_total / capture_sample_count) : 0U;
            uint32_t avg_convert_us = convert_sample_count > 0U ? (uint32_t)(convert_us_total / convert_sample_count) : 0U;
            uint32_t avg_encode_us = encode_sample_count > 0U ? (uint32_t)(encode_us_total / encode_sample_count) : 0U;
            uint32_t avg_callback_us = callback_sample_count > 0U ? (uint32_t)(callback_us_total / callback_sample_count) : 0U;
            uint32_t avg_loop_us = loop_sample_count > 0U ? (uint32_t)(loop_us_total / loop_sample_count) : 0U;
            uint32_t avg_gap_us = encoded_frame_count > 1U ?
                                  (uint32_t)(frame_gap_us_total / (encoded_frame_count - 1U)) :
                                  0U;
            uint32_t source_luma_delta_x10 =
                camera_pipeline_luma_delta_x10(&source_luma_stats);
            uint32_t encoder_luma_delta_x10 =
                camera_pipeline_luma_delta_x10(&encoder_luma_stats);
#endif
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            uint32_t avg_h264_sync_in_us = encode_sample_count > 0U ?
                                           (uint32_t)(h264_sync_in_us_total / encode_sample_count) :
                                           0U;
            uint32_t avg_h264_hw_us = encode_sample_count > 0U ?
                                      (uint32_t)(h264_hw_us_total / encode_sample_count) :
                                      0U;
            uint32_t avg_h264_sync_out_us = encode_sample_count > 0U ?
                                            (uint32_t)(h264_sync_out_us_total / encode_sample_count) :
                                            0U;
            uint32_t avg_media_timestamp_lag_us =
                media_timestamp_sample_count > 0U ?
                    (uint32_t)(media_timestamp_lag_us_total / media_timestamp_sample_count) :
                    0U;
            uint32_t avg_camera_sequence_delta_x10 =
                camera_sequence_sample_count > 0U ?
                    (uint32_t)(camera_sequence_delta_total_x10 / camera_sequence_sample_count) :
                    0U;
            uint32_t min_payload = min_payload_bytes == UINT32_MAX ? 0U : min_payload_bytes;
#endif
            uint32_t measured_fps_x10 = (uint32_t)(((uint64_t)upstream_count * 10000ULL) / elapsed_ms);
            uint32_t measured_bitrate_kbps = (uint32_t)(((uint64_t)total_payload_bytes * 8ULL) / elapsed_ms);
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
            camera_pipeline_note_interval_metrics(measured_fps_x10,
                                                  measured_bitrate_kbps,
                                                  avg_payload,
                                                  drop_count,
                                                  capture_fail_count,
                                                  encode_fail_count);
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
            ESP_LOGI(TAG,
                     "camera pipeline stats: target=%ux%u@%u cfg_bitrate=%ukbps encoded=%lu upstream=%lu fps=%lu.%lu bitrate=%lukbps drop=%lu bp_skip=%lu guard_drop=%lu keywait_drop=%lu cap_fail=%lu scale_fail=%lu enc_fail=%lu key=%lu large=%lu slow_cap=%lu slow_scale=%lu slow_enc=%lu slow_cb=%lu slow_loop=%lu payload[min/avg/max]=%lu/%lu/%lu luma_src_delta=%lu.%lu luma_src_change=%lu/%lu luma_src_range=%u-%u luma_enc_delta=%lu.%lu luma_enc_change=%lu/%lu luma_enc_range=%u-%u avg_gap_us=%lu max_gap_us=%llu cam_drain=%lu seq_delta_avg=%lu.%lu seq_delta_max=%lu avg_cap_us=%lu avg_scale_us=%lu avg_enc_us=%lu avg_h264[sync_in/hw/sync_out]=%lu/%lu/%lu avg_cb_us=%lu avg_loop_us=%lu avg_ts_lag_us=%lu max_ts_lag_us=%llu internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u h264_out_buf=%u",
                     (unsigned)h264.width,
                     (unsigned)h264.height,
                     (unsigned)policy.rtc_video_fps,
                     (unsigned)(policy.h264_bitrate_bps / 1000U),
                     (unsigned long)encoded_frame_count,
                     (unsigned long)upstream_count,
                     (unsigned long)(measured_fps_x10 / 10U),
                     (unsigned long)(measured_fps_x10 % 10U),
                     (unsigned long)measured_bitrate_kbps,
                     (unsigned long)drop_count,
                     (unsigned long)backpressure_skip_count,
                     (unsigned long)transport_guard_drop_count,
                     (unsigned long)key_wait_drop_count,
                     (unsigned long)capture_fail_count,
                     (unsigned long)convert_fail_count,
                     (unsigned long)encode_fail_count,
                     (unsigned long)key_frame_count,
                     (unsigned long)large_frame_count,
                     (unsigned long)slow_capture_count,
                     (unsigned long)slow_convert_count,
                     (unsigned long)slow_encode_count,
                     (unsigned long)slow_callback_count,
                     (unsigned long)slow_loop_count,
                     (unsigned long)min_payload,
                     (unsigned long)avg_payload,
                     (unsigned long)max_payload_bytes,
                     (unsigned long)(source_luma_delta_x10 / 10U),
                     (unsigned long)(source_luma_delta_x10 % 10U),
                     (unsigned long)source_luma_stats.changed_count,
                     (unsigned long)source_luma_stats.transition_count,
                     (unsigned)source_luma_stats.window_min_luma,
                     (unsigned)source_luma_stats.window_max_luma,
                     (unsigned long)(encoder_luma_delta_x10 / 10U),
                     (unsigned long)(encoder_luma_delta_x10 % 10U),
                     (unsigned long)encoder_luma_stats.changed_count,
                     (unsigned long)encoder_luma_stats.transition_count,
                     (unsigned)encoder_luma_stats.window_min_luma,
                     (unsigned)encoder_luma_stats.window_max_luma,
                     (unsigned long)avg_gap_us,
                     (unsigned long long)max_frame_gap_us,
                     (unsigned long)camera_stale_frame_drain_count,
                     (unsigned long)(avg_camera_sequence_delta_x10 / 10U),
                     (unsigned long)(avg_camera_sequence_delta_x10 % 10U),
                     (unsigned long)max_camera_sequence_delta,
                     (unsigned long)avg_capture_us,
                     (unsigned long)avg_convert_us,
                     (unsigned long)avg_encode_us,
                     (unsigned long)avg_h264_sync_in_us,
                     (unsigned long)avg_h264_hw_us,
                     (unsigned long)avg_h264_sync_out_us,
                     (unsigned long)avg_callback_us,
                     (unsigned long)avg_loop_us,
                     (unsigned long)avg_media_timestamp_lag_us,
                     (unsigned long long)max_media_timestamp_lag_us,
                     (unsigned)internal_free,
                     (unsigned)internal_largest,
                     (unsigned)dma_free,
                     (unsigned)dma_largest,
                     (unsigned)psram_free,
                     (unsigned)psram_largest,
                     (unsigned)h264.capture_buffer_size);
#elif CONFIG_APP_MEDIA_COMPACT_HEALTH_LOG
            ESP_LOGI(TAG,
                     "CAM %ux%u@%u f=%lu.%lu br=%luk gap=%lu/%llums "
                     "us=c/s/e/cb/l:%lu/%lu/%lu/%lu/%lu "
                     "motion=s/e:%lu.%lu/%lu.%lu chg=%lu/%lu,%lu/%lu "
                     "drop=%lu/%lu/%lu/%lu drain=%lu fail=%lu/%lu/%lu",
                     (unsigned)h264.width,
                     (unsigned)h264.height,
                     (unsigned)policy.rtc_video_fps,
                     (unsigned long)(measured_fps_x10 / 10U),
                     (unsigned long)(measured_fps_x10 % 10U),
                     (unsigned long)measured_bitrate_kbps,
                     (unsigned long)(avg_gap_us / 1000U),
                     (unsigned long long)(max_frame_gap_us / 1000ULL),
                     (unsigned long)avg_capture_us,
                     (unsigned long)avg_convert_us,
                     (unsigned long)avg_encode_us,
                     (unsigned long)avg_callback_us,
                     (unsigned long)avg_loop_us,
                     (unsigned long)(source_luma_delta_x10 / 10U),
                     (unsigned long)(source_luma_delta_x10 % 10U),
                     (unsigned long)(encoder_luma_delta_x10 / 10U),
                     (unsigned long)(encoder_luma_delta_x10 % 10U),
                     (unsigned long)source_luma_stats.changed_count,
                     (unsigned long)source_luma_stats.transition_count,
                     (unsigned long)encoder_luma_stats.changed_count,
                     (unsigned long)encoder_luma_stats.transition_count,
                     (unsigned long)drop_count,
                     (unsigned long)backpressure_skip_count,
                     (unsigned long)transport_guard_drop_count,
                     (unsigned long)key_wait_drop_count,
                     (unsigned long)camera_stale_frame_drain_count,
                     (unsigned long)capture_fail_count,
                     (unsigned long)convert_fail_count,
                     (unsigned long)encode_fail_count);
#endif
            last_stats_tick = now_tick;
            upstream_count = 0;
            drop_count = 0;
            capture_fail_count = 0;
            convert_fail_count = 0;
            encode_fail_count = 0;
            total_payload_bytes = 0;
            backpressure_skip_count = 0;
            transport_guard_drop_count = 0;
            key_wait_drop_count = 0;
            encoded_frame_count = 0;
            key_frame_count = 0;
            slow_capture_count = 0;
            slow_convert_count = 0;
            slow_encode_count = 0;
            slow_callback_count = 0;
            slow_loop_count = 0;
            large_frame_count = 0;
            min_payload_bytes = UINT32_MAX;
            max_payload_bytes = 0;
            frame_gap_us_total = 0;
            max_frame_gap_us = 0;
            last_frame_start_us = 0;
            capture_us_total = 0;
            convert_us_total = 0;
            encode_us_total = 0;
            h264_sync_in_us_total = 0;
            h264_hw_us_total = 0;
            h264_sync_out_us_total = 0;
            callback_us_total = 0;
            loop_us_total = 0;
            media_timestamp_lag_us_total = 0;
            max_media_timestamp_lag_us = 0;
            camera_sequence_delta_total_x10 = 0;
            camera_stale_frame_drain_count = 0;
            camera_sequence_sample_count = 0;
            max_camera_sequence_delta = 0;
            capture_sample_count = 0;
            convert_sample_count = 0;
            encode_sample_count = 0;
            callback_sample_count = 0;
            loop_sample_count = 0;
            media_timestamp_sample_count = 0;
            camera_pipeline_luma_stats_reset_window(&source_luma_stats);
            camera_pipeline_luma_stats_reset_window(&encoder_luma_stats);
        }

        /* The next loop iteration waits until the next RTC frame deadline. */
    }

exit_task:
    camera_pipeline_scaler_release(&yuv420_scaler, "pipeline-stop");
    if (!camera_pipeline_h264_store_reserved(&h264, "pipeline-stop")) {
        camera_pipeline_h264_close(&h264);
    }
    if (camera_acquired) {
        camera_driver_release_device();
    } else if (video_subsystem_prepared) {
        (void)camera_driver_deinit();
    }

    ESP_LOGI(TAG, "camera pipeline stopped");
    camera_pipeline_mark_task_stopped();
    camera_pipeline_reconcile_h264_reservation("pipeline-stop");
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t camera_pipeline_ensure_task_started(void)
{
    bool create_task = false;

    taskENTER_CRITICAL(&s_lock);
    if (s_rtc_enabled && s_task == NULL && !s_starting) {
        s_starting = true;
        create_task = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (!create_task) {
        return ESP_OK;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(camera_pipeline_task,
                                                          "camera_pipe",
                                                          CAMERA_PIPELINE_TASK_STACK,
                                                          NULL,
                                                          CAMERA_PIPELINE_TASK_PRIORITY,
                                                          NULL,
                                                          APP_TASK_CORE_CAMERA,
                                                          APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_ret != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_starting = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t camera_pipeline_init(void)
{
    ESP_RETURN_ON_ERROR(media_governor_init(), TAG, "media governor init failed");

    taskENTER_CRITICAL(&s_lock);
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t camera_pipeline_prewarm_h264(void)
{
    ESP_RETURN_ON_ERROR(camera_pipeline_init(), TAG, "camera pipeline init failed");
    ESP_RETURN_ON_FALSE(camera_driver_is_configured(), ESP_ERR_NOT_SUPPORTED, TAG, "camera not configured");

    media_governor_camera_policy_t policy = {0};
    media_governor_get_rtc_av_camera_policy(&policy);
    if (policy.h264_output_buffer_bytes == 0U) {
        policy.h264_output_buffer_bytes = CAMERA_PIPELINE_H264_FALLBACK_OUTPUT_BUFFER_BYTES;
    }

    uint16_t width = camera_pipeline_even_dimension(policy.rtc_width);
    uint16_t height = camera_pipeline_even_dimension(policy.rtc_height);
    ESP_RETURN_ON_FALSE(width >= CAMERA_PIPELINE_H264_MIN_WIDTH &&
                            height >= CAMERA_PIPELINE_H264_MIN_HEIGHT &&
                            policy.rtc_video_fps > 0U,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "invalid H264 prewarm target: %ux%u@%u",
                        width,
                        height,
                        policy.rtc_video_fps);

    taskENTER_CRITICAL(&s_lock);
    if (camera_pipeline_h264_matches(&s_reserved_h264,
                                     width,
                                     height,
                                     policy.rtc_video_fps,
                                     policy.h264_bitrate_bps,
                                     policy.h264_min_qp,
                                     policy.h264_max_qp,
                                     policy.h264_output_buffer_bytes)) {
        taskEXIT_CRITICAL(&s_lock);
        APP_LOG_DETAIL(TAG,
                       "H264 encoder prewarm already reserved: size=%ux%u fps=%u bitrate=%u out_buf=%u",
                 width,
                 height,
                 policy.rtc_video_fps,
                 (unsigned)policy.h264_bitrate_bps,
                 (unsigned)policy.h264_output_buffer_bytes);
        return ESP_OK;
    }
    if (s_rtc_enabled || s_task != NULL || s_starting || s_h264_reserve_in_progress) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_h264_reserve_in_progress = true;
    taskEXIT_CRITICAL(&s_lock);

    /*
     * Admission must be evaluated after the early boot escrow is lent. Checking
     * first made the protected block invisible and could defer a full-profile
     * encoder even though the memory had been reserved specifically for it.
     */
    bool dma_escrow_lent = media_dma_reserve_is_reserved();
    if (dma_escrow_lent) {
        media_dma_reserve_release("h264-early-prewarm");
        APP_LOG_DETAIL(TAG, "DMA escrow lent to H264 early prewarm");
    }

    size_t required_internal =
        camera_pipeline_h264_ref_internal_estimate(width) +
        CAMERA_PIPELINE_H264_INTERNAL_MARGIN;
    size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    /*
     * The retained HW reference pool is allocated during the early full-profile
     * prewarm and is intentionally absent from the heap's largest-free-block
     * metric.  Let the encoder acquire that pool instead of rejecting a valid
     * reopen based on a heap-only admission check.
     */
    if (!CONFIG_APP_H264_PERSISTENT_REF_POOL &&
        CAMERA_PIPELINE_H264_RESOURCE_FALLBACK_ENABLE &&
        required_internal > largest_internal) {
        if (dma_escrow_lent) {
            (void)media_dma_reserve_reclaim("h264-early-prewarm-deferred");
        }
        taskENTER_CRITICAL(&s_lock);
        s_h264_reserve_in_progress = false;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG,
                 "H264 full-profile reservation deferred: size=%ux%u required_internal=%u internal_largest=%u; active stream will select the largest fitting profile",
                 width,
                 height,
                 (unsigned)required_internal,
                 (unsigned)largest_internal);
        return ESP_ERR_NOT_FINISHED;
    }

    APP_LOG_DETAIL(TAG,
                   "H264 encoder early prewarm begin: size=%ux%u fps=%u bitrate=%u out_buf=%u ref_internal_est=%u internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
             width,
             height,
             policy.rtc_video_fps,
             (unsigned)policy.h264_bitrate_bps,
             (unsigned)policy.h264_output_buffer_bytes,
             (unsigned)camera_pipeline_h264_ref_internal_estimate(width),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    camera_pipeline_h264_encoder_t enc = {
        .fd = -1,
    };
    esp_err_t ret = camera_driver_prepare_video_subsystem();
    if (ret == ESP_OK) {
        ret = camera_pipeline_h264_open(&enc,
                                        width,
                                        height,
                                        policy.rtc_video_fps,
                                        policy.h264_bitrate_bps,
                                        policy.h264_min_qp,
                                        policy.h264_max_qp,
                                        policy.h264_output_buffer_bytes);
    }
    if (ret != ESP_OK && dma_escrow_lent) {
        esp_err_t reclaim_ret = media_dma_reserve_reclaim("h264-early-prewarm-failed");
        if (reclaim_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "DMA escrow reclaim after failed H264 prewarm failed: open=%s reclaim=%s",
                     esp_err_to_name(ret),
                     esp_err_to_name(reclaim_ret));
        }
    }

    bool stored = false;
    if (ret == ESP_OK) {
        stored = camera_pipeline_h264_store_reserved(&enc, "early-prewarm");
        if (!stored) {
            ret = ESP_ERR_INVALID_STATE;
        }
    }
    if (camera_pipeline_h264_is_open(&enc)) {
        camera_pipeline_h264_close(&enc);
    }

    taskENTER_CRITICAL(&s_lock);
    s_h264_reserve_in_progress = false;
    taskEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        APP_LOG_DETAIL(TAG,
                       "H264 encoder early prewarm done: internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
        ESP_LOGW(TAG,
                 "H264 encoder early prewarm failed: %s internal_free=%u internal_largest=%u dma_largest=%u psram_free=%u psram_largest=%u",
                 esp_err_to_name(ret),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return ret;
}

static void camera_pipeline_reconcile_h264_reservation(const char *reason)
{
    camera_pipeline_h264_encoder_t stale = {
        .fd = -1,
    };
    media_governor_camera_policy_t policy = {0};

    media_governor_get_rtc_av_camera_policy(&policy);
    if (policy.h264_output_buffer_bytes == 0U) {
        policy.h264_output_buffer_bytes = CAMERA_PIPELINE_H264_FALLBACK_OUTPUT_BUFFER_BYTES;
    }
    uint16_t width = camera_pipeline_even_dimension(policy.rtc_width);
    uint16_t height = camera_pipeline_even_dimension(policy.rtc_height);
    bool ready = false;
    bool idle = false;

    taskENTER_CRITICAL(&s_lock);
    ready = camera_pipeline_h264_matches(&s_reserved_h264,
                                         width,
                                         height,
                                         policy.rtc_video_fps,
                                         policy.h264_bitrate_bps,
                                         policy.h264_min_qp,
                                         policy.h264_max_qp,
                                         policy.h264_output_buffer_bytes) ||
            camera_pipeline_h264_static_config_matches(
                &s_reserved_h264,
                width,
                height,
                policy.h264_min_qp,
                policy.h264_max_qp,
                policy.h264_output_buffer_bytes);
    if (!ready && camera_pipeline_h264_is_open(&s_reserved_h264)) {
        stale = s_reserved_h264;
        s_reserved_h264 = (camera_pipeline_h264_encoder_t) {
            .fd = -1,
        };
    }
    idle = !s_rtc_enabled && s_task == NULL && !s_starting && !s_h264_reserve_in_progress;
    taskEXIT_CRITICAL(&s_lock);

    if (camera_pipeline_h264_is_open(&stale)) {
        APP_LOG_DETAIL(TAG,
                       "H264 reserved resource replaced: reason=%s old=%ux%u@%u %ukbps new=%ux%u@%u %ukbps",
                       reason != NULL ? reason : "unknown",
                       (unsigned)stale.width,
                       (unsigned)stale.height,
                       (unsigned)stale.fps,
                       (unsigned)(stale.bitrate_bps / 1000U),
                       (unsigned)width,
                       (unsigned)height,
                       (unsigned)policy.rtc_video_fps,
                       (unsigned)(policy.h264_bitrate_bps / 1000U));
        camera_pipeline_h264_close(&stale);
    }
    if (ready || !idle) {
        return;
    }

    esp_err_t ret = camera_pipeline_prewarm_h264();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "H264 encoder reservation refreshed: reason=%s size=%ux%u@%u",
                 reason != NULL ? reason : "unknown",
                 (unsigned)width,
                 (unsigned)height,
                 (unsigned)policy.rtc_video_fps);
    } else if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_NOT_FINISHED) {
        APP_LOG_DETAIL(TAG,
                       "H264 encoder reservation refresh deferred: reason=%s",
                       reason != NULL ? reason : "unknown");
    }
}

esp_err_t camera_pipeline_prewarm_call_scaler(void)
{
    ESP_RETURN_ON_ERROR(camera_pipeline_init(), TAG, "camera pipeline init failed");
    ESP_RETURN_ON_FALSE(camera_driver_is_configured(),
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "camera not configured");

    video_yuv420_scaler_config_t config = {0};
    ESP_RETURN_ON_FALSE(camera_pipeline_build_call_scaler_config(&config),
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "device-call profile does not require a YUV420 scaler");

    video_yuv420_scaler_handle_t stale = NULL;
    taskENTER_CRITICAL(&s_lock);
    if (video_yuv420_scaler_matches(s_reserved_call_scaler, &config)) {
        taskEXIT_CRITICAL(&s_lock);
        APP_LOG_DETAIL(TAG,
                       "call YUV420 scaler prewarm already reserved: input=%ux%u output=%ux%u",
                 config.input_width,
                 config.input_height,
                 config.output_width,
                 config.output_height);
        return ESP_OK;
    }
    if (s_rtc_enabled || s_task != NULL || s_starting || s_scaler_reserve_in_progress) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    stale = s_reserved_call_scaler;
    s_reserved_call_scaler = NULL;
    s_scaler_reserve_in_progress = true;
    taskEXIT_CRITICAL(&s_lock);

    video_yuv420_scaler_destroy(stale);
    video_yuv420_scaler_handle_t scaler = NULL;
    esp_err_t ret = video_yuv420_scaler_create(&config, &scaler);
    if (ret == ESP_OK) {
        ret = video_yuv420_scaler_warmup(scaler);
    }
    if (ret == ESP_OK && !camera_pipeline_scaler_store_reserved(&scaler, "early-prewarm")) {
        ret = ESP_ERR_INVALID_STATE;
    }
    video_yuv420_scaler_destroy(scaler);

    taskENTER_CRITICAL(&s_lock);
    s_scaler_reserve_in_progress = false;
    taskEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        APP_LOG_DETAIL(TAG,
                       "call YUV420 scaler early prewarm done: input=%ux%u output=%ux%u psram_free=%u",
                 config.input_width,
                 config.input_height,
                 config.output_width,
                 config.output_height,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
        ESP_LOGW(TAG,
                 "call YUV420 scaler early prewarm failed: input=%ux%u output=%ux%u ret=%s",
                 config.input_width,
                 config.input_height,
                 config.output_width,
                 config.output_height,
                 esp_err_to_name(ret));
    }
    return ret;
}

void camera_pipeline_on_rtc_video_config_changed(void)
{
    /*
     * Sensor format is a stream-open property and is applied before
     * camera_driver_acquire(). Runtime policy changes stay inside the
     * scaler/encoder path until the next stream start; attempting to retarget
     * an active V4L2 capture on every frame only creates rejected reconfigure
     * requests and cannot change the live sensor.
     */
    camera_pipeline_reconcile_h264_reservation("video-config-change");
}

void camera_pipeline_request_key_frame(void)
{
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    bool accept = true;
    bool log_drop = false;
    uint64_t drop_age_us = 0U;
    const char *drop_reason = "unknown";

    taskENTER_CRITICAL(&s_lock);
    uint64_t last_request_us = s_last_key_frame_request_us;
    if (last_request_us != 0U && now_us >= last_request_us &&
        now_us - last_request_us < CAMERA_PIPELINE_KEY_FRAME_REQUEST_MIN_INTERVAL_US) {
        accept = false;
        drop_reason = "recent-request";
        drop_age_us = now_us - last_request_us;
    }
    if (accept) {
        s_key_frame_request_pending = true;
        s_last_key_frame_request_us = now_us;
    } else if (s_last_key_frame_request_drop_log_us == 0U ||
               now_us - s_last_key_frame_request_drop_log_us >= 2000000ULL) {
        s_last_key_frame_request_drop_log_us = now_us;
        log_drop = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (!accept && log_drop) {
        APP_LOG_DETAIL(TAG,
                       "H264 key-frame request coalesced: reason=%s age_ms=%" PRIu64 " min_ms=%u",
                 drop_reason,
                 drop_age_us / 1000ULL,
                 (unsigned)APP_MEDIA_H264_KEY_FRAME_REQUEST_MIN_INTERVAL_MS);
    }
}

void camera_pipeline_request_stream_start_key_frame(void)
{
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    /*
     * A new subscriber has no decoder reference state, so its first IDR is a
     * stream contract rather than a retry hint. Bypass the normal PLI debounce
     * while still recording the request time for later duplicate requests.
     */
    taskENTER_CRITICAL(&s_lock);
    s_key_frame_request_pending = true;
    s_last_key_frame_request_us = now_us;
    s_last_key_frame_request_drop_log_us = 0U;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t camera_pipeline_set_rtc_video_sink(camera_pipeline_video_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_ERROR(camera_pipeline_init(), TAG, "camera pipeline init failed");

    taskENTER_CRITICAL(&s_lock);
    s_video_cb = cb;
    s_video_ctx = ctx;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t camera_pipeline_set_rtc_video_enabled(bool enabled)
{
    if (!s_initialized) {
        ESP_RETURN_ON_ERROR(camera_pipeline_init(), TAG, "camera pipeline init failed");
    }
    if (enabled && !camera_driver_is_configured()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (enabled && s_video_cb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool rtc_enabled = false;
    taskENTER_CRITICAL(&s_lock);
    if (enabled && !s_rtc_enabled) {
        /*
         * A new RTC session owns a fresh key-frame request window. A PLI from
         * the new subscriber must not be suppressed by the previous call.
         */
        s_key_frame_request_pending = false;
        s_last_key_frame_request_us = 0U;
        s_last_key_frame_request_drop_log_us = 0U;
    }
    s_rtc_enabled = enabled;
    rtc_enabled = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);

    camera_pipeline_apply_profile(rtc_enabled);
    esp_err_t ret = camera_pipeline_ensure_task_started();
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_rtc_enabled = false;
        rtc_enabled = s_rtc_enabled;
        taskEXIT_CRITICAL(&s_lock);
        camera_pipeline_apply_profile(rtc_enabled);
    }
    return ret;
}

bool camera_pipeline_is_running(void)
{
    bool running = false;

    taskENTER_CRITICAL(&s_lock);
    running = s_task != NULL || s_starting;
    taskEXIT_CRITICAL(&s_lock);
    return running;
}

bool camera_pipeline_is_rtc_video_active(void)
{
    bool active = false;

    taskENTER_CRITICAL(&s_lock);
    active = s_rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

void camera_pipeline_get_metrics(camera_pipeline_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }

    bool running = false;
    bool rtc_enabled = false;

    taskENTER_CRITICAL(&s_lock);
    *metrics = s_metrics;
    running = s_task != NULL || s_starting;
    rtc_enabled = s_rtc_enabled;
    metrics->running = running;
    metrics->rtc_enabled = rtc_enabled;
    taskEXIT_CRITICAL(&s_lock);

    if (!rtc_enabled) {
        memset(metrics, 0, sizeof(*metrics));
        metrics->running = running;
    }
}
