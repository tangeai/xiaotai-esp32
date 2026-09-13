#include "media_governor.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "media_tuning.h"

static const char *TAG = "media_governor";

#define MEDIA_GOVERNOR_CAPTURE_WIDTH APP_MEDIA_CAMERA_CAPTURE_WIDTH
#define MEDIA_GOVERNOR_CAPTURE_HEIGHT APP_MEDIA_CAMERA_CAPTURE_HEIGHT
#define MEDIA_GOVERNOR_FULL_WIDTH APP_MEDIA_RTC_VIDEO_WIDTH
#define MEDIA_GOVERNOR_FULL_HEIGHT APP_MEDIA_RTC_VIDEO_HEIGHT

#ifndef CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE
#define CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE 0
#endif

#define MEDIA_GOVERNOR_FULL_FPS APP_MEDIA_RTC_H264_FPS
#define MEDIA_GOVERNOR_BACKPRESSURE_HOLD_MS APP_MEDIA_TRANSPORT_BACKPRESSURE_HOLD_MS
#define MEDIA_GOVERNOR_BACKPRESSURE_LOG_INTERVAL_MS 5000U
#define MEDIA_GOVERNOR_AUTO_DEGRADE_SAMPLES APP_MEDIA_AUTO_DEGRADE_SAMPLES
#define MEDIA_GOVERNOR_AUTO_RECOVER_SAMPLES APP_MEDIA_AUTO_RECOVER_SAMPLES
#define MEDIA_GOVERNOR_AUTO_COOLDOWN_MS APP_MEDIA_AUTO_COOLDOWN_MS
#define MEDIA_GOVERNOR_AUTO_PRESSURE_BUFFER_PCT APP_MEDIA_AUTO_PRESSURE_BUFFER_PCT
#define MEDIA_GOVERNOR_AUTO_SEVERE_BUFFER_PCT APP_MEDIA_AUTO_SEVERE_BUFFER_PCT
#define MEDIA_GOVERNOR_AUTO_HEALTHY_BUFFER_PCT APP_MEDIA_AUTO_HEALTHY_BUFFER_PCT
#define MEDIA_GOVERNOR_AUTO_PRESSURE_QUEUE_DEPTH APP_MEDIA_AUTO_PRESSURE_QUEUE_DEPTH
#define MEDIA_GOVERNOR_AUTO_SEVERE_QUEUE_DEPTH APP_MEDIA_AUTO_SEVERE_QUEUE_DEPTH
#define MEDIA_GOVERNOR_VIDEO_MIN_BITRATE_BPS (200U * 1000U)
#define MEDIA_GOVERNOR_TRANSPORT_COMPACT_MAX_PIXELS (640U * 480U)
/* Compact calls can remain decodable at 200 kbps. The floor gives TGMP room
 * to preserve continuity on cellular uplinks without rebuilding the active
 * H264 resolution chain. */
#define MEDIA_GOVERNOR_TRANSPORT_COMPACT_MIN_BITRATE_BPS APP_MEDIA_TGMP_COMPACT_MIN_BITRATE_BPS
#define MEDIA_GOVERNOR_TRANSPORT_LARGE_MIN_BITRATE_BPS APP_MEDIA_TGMP_LARGE_MIN_BITRATE_BPS
#define MEDIA_GOVERNOR_TRANSPORT_MIN_RATIO_DIVISOR APP_MEDIA_TGMP_MIN_RATIO_DIVISOR
#define MEDIA_GOVERNOR_TRANSPORT_START_RANGE_PERCENT APP_MEDIA_TGMP_START_RANGE_PERCENT
#define MEDIA_GOVERNOR_TRANSPORT_MIN_STEP_BPS APP_MEDIA_TGMP_MIN_STEP_BPS
#define MEDIA_GOVERNOR_TRANSPORT_MIN_STEP_PERCENT APP_MEDIA_TGMP_MIN_STEP_PERCENT
/*
 * TGMP owns the target bitrate once feedback is available. Preserve urgent
 * downshifts, but rate-limit ordinary encoder reconfiguration and make quality
 * recovery deliberately slower than congestion protection.
 */
#define MEDIA_GOVERNOR_TRANSPORT_PROTECTION_INTERVAL_MS APP_MEDIA_TGMP_PROTECTION_INTERVAL_MS
#define MEDIA_GOVERNOR_TRANSPORT_EMERGENCY_DROP_PERCENT APP_MEDIA_TGMP_EMERGENCY_DROP_PERCENT
#define MEDIA_GOVERNOR_TRANSPORT_RECOVERY_HOLD_MS APP_MEDIA_TGMP_RECOVERY_HOLD_MS
#define MEDIA_GOVERNOR_TRANSPORT_RECOVERY_INTERVAL_MS APP_MEDIA_TGMP_RECOVERY_INTERVAL_MS
#define MEDIA_GOVERNOR_TRANSPORT_RECOVERY_STEP_PERCENT APP_MEDIA_TGMP_RECOVERY_STEP_PERCENT
#define MEDIA_GOVERNOR_TRANSPORT_RECOVERY_MIN_STEP_BPS APP_MEDIA_TGMP_RECOVERY_MIN_STEP_BPS

#define MEDIA_GOVERNOR_FULL_VIDEO_CONFIG_INIT \
    { \
        .width = MEDIA_GOVERNOR_FULL_WIDTH, \
        .height = MEDIA_GOVERNOR_FULL_HEIGHT, \
        .fps = MEDIA_GOVERNOR_FULL_FPS, \
        .bitrate_bps = APP_MEDIA_RTC_H264_BITRATE_BPS, \
        .weak_network_mode = MEDIA_GOVERNOR_WEAK_NETWORK_OFF, \
        .weak_network_level = 0, \
        .h264_min_qp = APP_MEDIA_RTC_H264_MIN_QP, \
        .h264_max_qp = APP_MEDIA_RTC_H264_MAX_QP, \
    }

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static media_governor_profile_t s_profile = MEDIA_GOVERNOR_PROFILE_IDLE;
static uint32_t s_backpressure_count;
static TickType_t s_backpressure_until_tick;
static TickType_t s_last_backpressure_log_tick;
static bool s_auto_adapt_disabled_logged;
static media_governor_video_config_t s_rtc_video_config = MEDIA_GOVERNOR_FULL_VIDEO_CONFIG_INIT;
static media_governor_video_config_t s_adaptation_base_config =
    MEDIA_GOVERNOR_FULL_VIDEO_CONFIG_INIT;
static uint8_t s_auto_pressure_samples;
static uint8_t s_auto_healthy_samples;
static TickType_t s_auto_cooldown_until_tick;
static bool s_transport_adaptation_active;
static uint32_t s_transport_requested_bitrate_bps;
static TickType_t s_transport_protection_not_before_tick;
static TickType_t s_transport_recovery_not_before_tick;

static const media_governor_camera_policy_t s_policy_idle = {
    .capture_width = MEDIA_GOVERNOR_CAPTURE_WIDTH,
    .capture_height = MEDIA_GOVERNOR_CAPTURE_HEIGHT,
    .capture_fps = 1,
    .rtc_video_fps = 0,
    .rtc_width = 480,
    .rtc_height = 360,
    .h264_bitrate_bps = 800U * 1000U,
    .h264_min_qp = APP_MEDIA_RTC_H264_MIN_QP,
    .h264_max_qp = APP_MEDIA_RTC_H264_MAX_QP,
    .h264_output_buffer_bytes = 512U * 1024U,
    .h264_max_delta_payload_bytes = 96U * 1024U,
    .dma_free_min_bytes = 24U * 1024U,
    .dma_largest_min_bytes = 12U * 1024U,
};

static void media_governor_select_native_capture_size(const media_governor_video_config_t *config,
                                                       uint16_t *width,
                                                       uint16_t *height)
{
    *width = MEDIA_GOVERNOR_CAPTURE_WIDTH;
    *height = MEDIA_GOVERNOR_CAPTURE_HEIGHT;

    if (config != NULL &&
        config->width <= MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH &&
        config->height <= MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT) {
        *width = MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH;
        *height = MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT;
    }
}

static media_governor_camera_policy_t media_governor_make_rtc_av_policy(const media_governor_video_config_t *config)
{
    media_governor_video_config_t safe_config = {0};
    uint16_t capture_width = MEDIA_GOVERNOR_CAPTURE_WIDTH;
    uint16_t capture_height = MEDIA_GOVERNOR_CAPTURE_HEIGHT;

    if (config != NULL) {
        safe_config = *config;
    } else {
        safe_config = s_rtc_video_config;
    }

    if (safe_config.width == 0U) {
        safe_config.width = MEDIA_GOVERNOR_FULL_WIDTH;
    }
    if (safe_config.height == 0U) {
        safe_config.height = MEDIA_GOVERNOR_FULL_HEIGHT;
    }
    if (safe_config.fps == 0U) {
        safe_config.fps = MEDIA_GOVERNOR_FULL_FPS;
    }
    if (safe_config.bitrate_bps == 0U) {
        safe_config.bitrate_bps = APP_MEDIA_RTC_H264_BITRATE_BPS;
    }
    if (safe_config.h264_min_qp < 10U || safe_config.h264_min_qp > 51U) {
        safe_config.h264_min_qp = APP_MEDIA_RTC_H264_MIN_QP;
    }
    if (safe_config.h264_max_qp < safe_config.h264_min_qp ||
        safe_config.h264_max_qp > 51U) {
        safe_config.h264_max_qp = APP_MEDIA_RTC_H264_MAX_QP;
        if (safe_config.h264_max_qp < safe_config.h264_min_qp) {
            safe_config.h264_max_qp = safe_config.h264_min_qp;
        }
    }
    media_governor_select_native_capture_size(&safe_config, &capture_width, &capture_height);

    return (media_governor_camera_policy_t) {
        .capture_width = capture_width,
        .capture_height = capture_height,
        .capture_fps = safe_config.fps,
        .rtc_video_fps = safe_config.fps,
        .rtc_width = safe_config.width,
        .rtc_height = safe_config.height,
        .h264_bitrate_bps = safe_config.bitrate_bps,
        .h264_min_qp = safe_config.h264_min_qp,
        .h264_max_qp = safe_config.h264_max_qp,
        .h264_output_buffer_bytes = APP_MEDIA_H264_OUTPUT_BUFFER_BYTES,
        .h264_max_delta_payload_bytes = APP_MEDIA_H264_MAX_DELTA_PAYLOAD_BYTES,
        .dma_free_min_bytes = 8U * 1024U,
        /* ESP-Hosted SDIO packets are 1536 bytes and the driver keeps an
         * early reserved burst pool. Keep one aligned fallback packet
         * available without treating normal late-stage fragmentation as
         * network congestion. */
        .dma_largest_min_bytes = 2U * 1024U,
    };
}

static const media_governor_rtc_policy_t s_rtc_policy_normal = {
    .defer_audio_for_local_video = false,
    .prepare_playback_while_video_first = true,
};

static const media_governor_rtc_policy_t s_rtc_policy_video_first = {
    .defer_audio_for_local_video = false,
    /*
     * Full AV sessions start the speaker lazily on the first remote PCM packet.
     * One-way IPC viewing then avoids reserving I2S/DMA bandwidth it never uses.
     */
    .prepare_playback_while_video_first = false,
};

esp_err_t media_governor_init(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

const char *media_governor_profile_name(media_governor_profile_t profile)
{
    switch (profile) {
    case MEDIA_GOVERNOR_PROFILE_IDLE:
        return "idle";
    case MEDIA_GOVERNOR_PROFILE_QR_SCAN:
        return "qr_scan";
    case MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW:
        return "local_preview";
    case MEDIA_GOVERNOR_PROFILE_RTC_AUDIO:
        return "rtc_audio";
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE:
        return "rtc_av_safe";
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW:
        return "rtc_av_preview";
    case MEDIA_GOVERNOR_PROFILE_AI_CHAT:
        return "ai_chat";
    default:
        return "unknown";
    }
}

static bool media_governor_tick_before(TickType_t tick, TickType_t deadline)
{
    return (int32_t)(tick - deadline) < 0;
}

static TickType_t media_governor_tick_after_ms(TickType_t now, uint32_t delay_ms)
{
    TickType_t deadline = now + pdMS_TO_TICKS(delay_ms);
    return deadline == 0U ? 1U : deadline;
}

void media_governor_set_profile(media_governor_profile_t profile)
{
    media_governor_profile_t old_profile = MEDIA_GOVERNOR_PROFILE_IDLE;
    bool changed = false;

    taskENTER_CRITICAL(&s_lock);
    old_profile = s_profile;
    if (s_profile != profile) {
        s_profile = profile;
        s_backpressure_count = 0;
        s_backpressure_until_tick = 0;
        s_last_backpressure_log_tick = 0;
        s_auto_pressure_samples = 0;
        s_auto_healthy_samples = 0;
        s_auto_cooldown_until_tick = 0;
        changed = true;
    }
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "media profile: %s -> %s",
                 media_governor_profile_name(old_profile),
                 media_governor_profile_name(profile));
    }
}

media_governor_profile_t media_governor_get_profile(void)
{
    media_governor_profile_t profile;

    taskENTER_CRITICAL(&s_lock);
    profile = s_profile;
    taskEXIT_CRITICAL(&s_lock);
    return profile;
}

static media_governor_video_config_t media_governor_normalize_video_config(const media_governor_video_config_t *config)
{
    media_governor_video_config_t normalized = s_rtc_video_config;

    if (config != NULL) {
        normalized = *config;
    }

    if (normalized.width < 320U) {
        normalized.width = 320U;
    }
    if (normalized.height < 240U) {
        normalized.height = 240U;
    }
    if (normalized.fps < 5U) {
        normalized.fps = 5U;
    } else if (normalized.fps > 30U) {
        normalized.fps = 30U;
    }
    if (normalized.bitrate_bps < MEDIA_GOVERNOR_VIDEO_MIN_BITRATE_BPS) {
        normalized.bitrate_bps = MEDIA_GOVERNOR_VIDEO_MIN_BITRATE_BPS;
    } else if (normalized.bitrate_bps > 12U * 1000U * 1000U) {
        normalized.bitrate_bps = 12U * 1000U * 1000U;
    }
    if ((normalized.width & 1U) != 0U) {
        normalized.width--;
    }
    if ((normalized.height & 1U) != 0U) {
        normalized.height--;
    }
    if (normalized.weak_network_mode > MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY) {
        normalized.weak_network_mode = MEDIA_GOVERNOR_WEAK_NETWORK_OFF;
    }
    if (normalized.weak_network_mode == MEDIA_GOVERNOR_WEAK_NETWORK_OFF) {
        normalized.weak_network_level = 0U;
    } else if (normalized.weak_network_level > 3U) {
        normalized.weak_network_level = 3U;
    }
    if (normalized.h264_min_qp < 10U || normalized.h264_min_qp > 51U) {
        normalized.h264_min_qp = APP_MEDIA_RTC_H264_MIN_QP;
    }
    if (normalized.h264_max_qp < normalized.h264_min_qp ||
        normalized.h264_max_qp > 51U) {
        normalized.h264_max_qp = APP_MEDIA_RTC_H264_MAX_QP;
        if (normalized.h264_max_qp < normalized.h264_min_qp) {
            normalized.h264_max_qp = normalized.h264_min_qp;
        }
    }

    return normalized;
}

esp_err_t media_governor_set_rtc_video_config(const media_governor_video_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "rtc video config is null");

    media_governor_video_config_t normalized = media_governor_normalize_video_config(config);
    media_governor_video_config_t old_config = {0};
    bool changed = false;

    taskENTER_CRITICAL(&s_lock);
    old_config = s_rtc_video_config;
    if (memcmp(&s_rtc_video_config, &normalized, sizeof(normalized)) != 0) {
        s_rtc_video_config = normalized;
        changed = true;
    }
    if (normalized.weak_network_level == 0U) {
        const bool base_changed =
            memcmp(&s_adaptation_base_config, &normalized, sizeof(normalized)) != 0;
        s_adaptation_base_config = normalized;
        s_adaptation_base_config.weak_network_level = 0U;
        if (base_changed) {
            s_transport_adaptation_active = false;
            s_transport_requested_bitrate_bps = 0U;
            s_transport_protection_not_before_tick = 0;
            s_transport_recovery_not_before_tick = 0;
        }
    }
    s_initialized = true;
    taskEXIT_CRITICAL(&s_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "rtc video config: %ux%u@%u %ukbps qp=%u-%u mode=%u level=%u -> "
                 "%ux%u@%u %ukbps qp=%u-%u mode=%u level=%u",
                 (unsigned)old_config.width,
                 (unsigned)old_config.height,
                 (unsigned)old_config.fps,
                 (unsigned)(old_config.bitrate_bps / 1000U),
                 (unsigned)old_config.h264_min_qp,
                 (unsigned)old_config.h264_max_qp,
                 (unsigned)old_config.weak_network_mode,
                 (unsigned)old_config.weak_network_level,
                 (unsigned)normalized.width,
                 (unsigned)normalized.height,
                 (unsigned)normalized.fps,
                 (unsigned)(normalized.bitrate_bps / 1000U),
                 (unsigned)normalized.h264_min_qp,
                 (unsigned)normalized.h264_max_qp,
                 (unsigned)normalized.weak_network_mode,
                 (unsigned)normalized.weak_network_level);
    }

    return ESP_OK;
}

void media_governor_build_device_call_video_config(media_governor_video_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = (media_governor_video_config_t) {
        .width = APP_MEDIA_CALL_VIDEO_WIDTH,
        .height = APP_MEDIA_CALL_VIDEO_HEIGHT,
        .fps = APP_MEDIA_CALL_VIDEO_FPS,
        .bitrate_bps = APP_MEDIA_CALL_VIDEO_BITRATE_BPS,
        .weak_network_mode = CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE ?
                                 MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY :
                                 MEDIA_GOVERNOR_WEAK_NETWORK_OFF,
        .weak_network_level = 0U,
        .h264_min_qp = APP_MEDIA_CALL_VIDEO_MIN_QP,
        .h264_max_qp = APP_MEDIA_CALL_VIDEO_MAX_QP,
    };
}

void media_governor_build_camera_policy(const media_governor_video_config_t *config,
                                        media_governor_camera_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    *policy = media_governor_make_rtc_av_policy(config);
}

bool media_governor_auto_adaptation_enabled(void)
{
    return CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE != 0;
}

static uint8_t media_governor_resolution_priority_fps(uint8_t base_fps,
                                                      uint8_t level)
{
    static const uint8_t fps_pct[] = {100U, 100U, 80U, 67U};

    if (level >= sizeof(fps_pct) / sizeof(fps_pct[0])) {
        level = (uint8_t)(sizeof(fps_pct) / sizeof(fps_pct[0]) - 1U);
    }
    uint8_t fps =
        (uint8_t)(((uint32_t)base_fps * fps_pct[level] + 50U) / 100U);
    return fps < 5U ? 5U : fps;
}

esp_err_t media_governor_apply_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level)
{
    media_governor_video_config_t config = {0};
    static const uint8_t bitrate_pct_framerate_priority[] = {100U, 88U, 77U, 67U};
    static const uint8_t bitrate_pct_resolution_priority[] = {100U, 90U, 78U, 67U};

    if (mode > MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    config = s_adaptation_base_config;
    taskEXIT_CRITICAL(&s_lock);

    if (mode == MEDIA_GOVERNOR_WEAK_NETWORK_OFF) {
        config.weak_network_mode = MEDIA_GOVERNOR_WEAK_NETWORK_OFF;
        config.weak_network_level = 0U;
        return media_governor_set_rtc_video_config(&config);
    }

    if (level > 3U) {
        level = 3U;
    }
    config.weak_network_mode = mode;
    config.weak_network_level = level;
    if (level == 0U) {
        return media_governor_set_rtc_video_config(&config);
    }

    const uint8_t bitrate_pct =
        mode == MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY ?
            bitrate_pct_framerate_priority[level] :
            bitrate_pct_resolution_priority[level];
    config.bitrate_bps =
        (uint32_t)(((uint64_t)config.bitrate_bps * bitrate_pct + 50ULL) / 100ULL);

    /*
     * Both modes preserve the negotiated frame dimensions. Changing resolution
     * mid-call would rebuild the decoder crop and H264 reference chain. The
     * frame-rate-priority mode spends quality first; resolution priority drops
     * cadence at levels 2/3 so each retained frame keeps more bits.
     */
    if (mode == MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY) {
        config.fps = media_governor_resolution_priority_fps(config.fps, level);
    }

    return media_governor_set_rtc_video_config(&config);
}

esp_err_t media_governor_apply_auto_weak_network_level(media_governor_weak_network_mode_t mode, uint8_t level)
{
    /*
     * Automatic downshift is intentionally gated by Kconfig. The normal RTC
     * path should keep the full configured output capability; only explicit
     * user control or the enabled sustained-pressure controller may lower it.
     */
    if (!media_governor_auto_adaptation_enabled()) {
        bool log_disabled = false;

        taskENTER_CRITICAL(&s_lock);
        if (!s_auto_adapt_disabled_logged) {
            s_auto_adapt_disabled_logged = true;
            log_disabled = true;
        }
        taskEXIT_CRITICAL(&s_lock);

        if (log_disabled) {
            ESP_LOGI(TAG,
                     "auto weak-network video adaptation disabled; keep full output profile");
        }
        return ESP_OK;
    }

    return media_governor_apply_weak_network_level(mode, level);
}

bool media_governor_update_auto_adaptation(const media_governor_network_sample_t *sample)
{
    if (sample == NULL || !media_governor_auto_adaptation_enabled()) {
        return false;
    }

    media_governor_video_config_t current = {0};
    uint8_t pressure_samples = 0U;
    uint8_t healthy_samples = 0U;
    TickType_t cooldown_until = 0;

    taskENTER_CRITICAL(&s_lock);
    current = s_rtc_video_config;
    if (s_transport_adaptation_active) {
        s_auto_pressure_samples = 0U;
        s_auto_healthy_samples = 0U;
        s_auto_cooldown_until_tick = 0;
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    if (!sample->active ||
        current.weak_network_mode == MEDIA_GOVERNOR_WEAK_NETWORK_OFF) {
        s_auto_pressure_samples = 0U;
        s_auto_healthy_samples = 0U;
        s_auto_cooldown_until_tick = 0;
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    pressure_samples = s_auto_pressure_samples;
    healthy_samples = s_auto_healthy_samples;
    cooldown_until = s_auto_cooldown_until_tick;
    taskEXIT_CRITICAL(&s_lock);

    uint32_t buffer_pct = 0U;
    if (sample->send_buffer_limit > 0U) {
        buffer_pct =
            (uint32_t)(((uint64_t)sample->send_buffer_used * 100ULL) /
                       sample->send_buffer_limit);
    }
    uint32_t expected_fps_x10 = sample->camera_fps_x10;
    if (expected_fps_x10 == 0U) {
        uint8_t target_fps = sample->target_fps > 0U ?
                                 sample->target_fps :
                                 current.fps;
        expected_fps_x10 = (uint32_t)target_fps * 10U;
    }
    const bool tx_rate_lagging =
        expected_fps_x10 >= 50U &&
        sample->tx_fps_x10 * 100U < expected_fps_x10 * 82U;
    const bool tx_rate_healthy =
        expected_fps_x10 == 0U ||
        sample->tx_fps_x10 * 100U >= expected_fps_x10 * 90U;
    const bool severe =
        sample->tx_failures >= 3U ||
        sample->backpressure_events >= 3U ||
        sample->tx_queue_depth >= MEDIA_GOVERNOR_AUTO_SEVERE_QUEUE_DEPTH ||
        buffer_pct >= MEDIA_GOVERNOR_AUTO_SEVERE_BUFFER_PCT;
    const bool pressured =
        severe ||
        sample->backpressure_active ||
        sample->backpressure_events > 0U ||
        sample->tx_failures > 0U ||
        sample->tx_queue_depth >= MEDIA_GOVERNOR_AUTO_PRESSURE_QUEUE_DEPTH ||
        buffer_pct >= MEDIA_GOVERNOR_AUTO_PRESSURE_BUFFER_PCT ||
        (tx_rate_lagging &&
         (sample->tx_queue_depth > 0U || sample->send_buffer_used > 0U));
    const bool healthy =
        !pressured &&
        sample->tx_queue_depth == 0U &&
        buffer_pct <= MEDIA_GOVERNOR_AUTO_HEALTHY_BUFFER_PCT &&
        tx_rate_healthy;

    if (pressured) {
        pressure_samples = severe ?
                               MEDIA_GOVERNOR_AUTO_DEGRADE_SAMPLES :
                               (uint8_t)(pressure_samples + 1U);
        if (pressure_samples > MEDIA_GOVERNOR_AUTO_DEGRADE_SAMPLES) {
            pressure_samples = MEDIA_GOVERNOR_AUTO_DEGRADE_SAMPLES;
        }
        healthy_samples = 0U;
    } else if (healthy) {
        healthy_samples++;
        if (healthy_samples > MEDIA_GOVERNOR_AUTO_RECOVER_SAMPLES) {
            healthy_samples = MEDIA_GOVERNOR_AUTO_RECOVER_SAMPLES;
        }
        pressure_samples = 0U;
    } else {
        pressure_samples = 0U;
        healthy_samples = 0U;
    }

    const TickType_t now = xTaskGetTickCount();
    uint8_t next_level = current.weak_network_level;
    bool cooldown_active =
        cooldown_until != 0U && media_governor_tick_before(now, cooldown_until);
    if (!cooldown_active &&
        pressure_samples >= MEDIA_GOVERNOR_AUTO_DEGRADE_SAMPLES &&
        next_level < 3U) {
        next_level++;
    } else if (!cooldown_active &&
               healthy_samples >= MEDIA_GOVERNOR_AUTO_RECOVER_SAMPLES &&
               next_level > 0U) {
        next_level--;
    }

    taskENTER_CRITICAL(&s_lock);
    s_auto_pressure_samples = pressure_samples;
    s_auto_healthy_samples = healthy_samples;
    if (next_level != current.weak_network_level) {
        s_auto_pressure_samples = 0U;
        s_auto_healthy_samples = 0U;
        s_auto_cooldown_until_tick =
            now + pdMS_TO_TICKS(MEDIA_GOVERNOR_AUTO_COOLDOWN_MS);
        if (s_auto_cooldown_until_tick == 0U) {
            s_auto_cooldown_until_tick = 1U;
        }
    }
    taskEXIT_CRITICAL(&s_lock);

    if (next_level == current.weak_network_level) {
        return false;
    }

    esp_err_t ret =
        media_governor_apply_weak_network_level(current.weak_network_mode,
                                                next_level);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_auto_cooldown_until_tick = 0U;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG,
                 "auto video adaptation failed: level=%u->%u ret=%s",
                 (unsigned)current.weak_network_level,
                 (unsigned)next_level,
                 esp_err_to_name(ret));
        return false;
    }

    media_governor_video_config_t updated = {0};
    media_governor_get_rtc_video_config(&updated);
    ESP_LOGI(TAG,
             "auto video adaptation: level=%u->%u profile=%ux%u@%u %ukbps "
             "net=buf:%u%% q:%u bp:%u fail:%u tx:%u.%u/%u.%u rssi:%d",
             (unsigned)current.weak_network_level,
             (unsigned)next_level,
             (unsigned)updated.width,
             (unsigned)updated.height,
             (unsigned)updated.fps,
             (unsigned)(updated.bitrate_bps / 1000U),
             (unsigned)buffer_pct,
             (unsigned)sample->tx_queue_depth,
             (unsigned)sample->backpressure_events,
             (unsigned)sample->tx_failures,
             (unsigned)(sample->tx_fps_x10 / 10U),
             (unsigned)(sample->tx_fps_x10 % 10U),
             (unsigned)(expected_fps_x10 / 10U),
             (unsigned)(expected_fps_x10 % 10U),
             (int)sample->wifi_rssi);
    return true;
}

static void media_governor_build_transport_bitrate_range(
    const media_governor_video_config_t *base,
    media_governor_transport_bitrate_range_t *range)
{
    const uint32_t frame_pixels = (uint32_t)base->width * (uint32_t)base->height;
    const uint32_t profile_floor_bps =
        frame_pixels <= MEDIA_GOVERNOR_TRANSPORT_COMPACT_MAX_PIXELS ?
            MEDIA_GOVERNOR_TRANSPORT_COMPACT_MIN_BITRATE_BPS :
            MEDIA_GOVERNOR_TRANSPORT_LARGE_MIN_BITRATE_BPS;
    uint32_t min_bitrate_bps =
        base->bitrate_bps / MEDIA_GOVERNOR_TRANSPORT_MIN_RATIO_DIVISOR;
    if (min_bitrate_bps < profile_floor_bps) {
        min_bitrate_bps = profile_floor_bps;
    }
    if (min_bitrate_bps >= base->bitrate_bps) {
        min_bitrate_bps = base->bitrate_bps > 1U ? base->bitrate_bps - 1U : 0U;
    }

    const uint32_t max_bitrate_bps = base->bitrate_bps;
    uint32_t start_bitrate_bps =
        min_bitrate_bps +
        (uint32_t)(((uint64_t)(max_bitrate_bps - min_bitrate_bps) *
                    MEDIA_GOVERNOR_TRANSPORT_START_RANGE_PERCENT) /
                   100ULL);
    if (start_bitrate_bps <= min_bitrate_bps) {
        start_bitrate_bps = min_bitrate_bps + 1U;
    }
    if (start_bitrate_bps >= max_bitrate_bps) {
        start_bitrate_bps = max_bitrate_bps - 1U;
    }

    *range = (media_governor_transport_bitrate_range_t) {
        .min_bitrate_bps = min_bitrate_bps,
        .max_bitrate_bps = max_bitrate_bps,
        /* Start near the quality target while preserving min < start < max. */
        .start_bitrate_bps = start_bitrate_bps,
    };
}

void media_governor_build_wechat_video_config(media_governor_video_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = (media_governor_video_config_t) {
        .width = APP_MEDIA_WECHAT_VIDEO_WIDTH,
        .height = APP_MEDIA_WECHAT_VIDEO_HEIGHT,
        .fps = APP_MEDIA_WECHAT_VIDEO_FPS,
        .bitrate_bps = APP_MEDIA_WECHAT_VIDEO_BITRATE_BPS,
        .weak_network_mode = CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE ?
                                 MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY :
                                 MEDIA_GOVERNOR_WEAK_NETWORK_OFF,
        .weak_network_level = 0U,
        .h264_min_qp = APP_MEDIA_WECHAT_VIDEO_MIN_QP,
        .h264_max_qp = APP_MEDIA_WECHAT_VIDEO_MAX_QP,
    };
}

void media_governor_get_transport_bitrate_range(
    media_governor_transport_bitrate_range_t *range)
{
    if (range == NULL) {
        return;
    }

    media_governor_video_config_t base = {0};
    taskENTER_CRITICAL(&s_lock);
    base = s_adaptation_base_config;
    taskEXIT_CRITICAL(&s_lock);

    media_governor_build_transport_bitrate_range(&base, range);
}

static uint32_t media_governor_transport_clamp_bitrate(
    uint32_t target_bitrate_bps,
    const media_governor_transport_bitrate_range_t *range)
{
    if (target_bitrate_bps < range->min_bitrate_bps) {
        return range->min_bitrate_bps;
    }
    if (target_bitrate_bps > range->max_bitrate_bps) {
        return range->max_bitrate_bps;
    }
    return target_bitrate_bps;
}

static uint32_t media_governor_transport_recovery_step(uint32_t current_bitrate_bps)
{
    uint32_t step_bps =
        (uint32_t)(((uint64_t)current_bitrate_bps *
                    MEDIA_GOVERNOR_TRANSPORT_RECOVERY_STEP_PERCENT) /
                   100ULL);
    if (step_bps < MEDIA_GOVERNOR_TRANSPORT_RECOVERY_MIN_STEP_BPS) {
        step_bps = MEDIA_GOVERNOR_TRANSPORT_RECOVERY_MIN_STEP_BPS;
    }
    return step_bps;
}

static void media_governor_build_transport_video_config(
    const media_governor_video_config_t *base,
    const media_governor_transport_bitrate_range_t *range,
    uint32_t bitrate_bps,
    media_governor_video_config_t *updated)
{
    *updated = *base;
    updated->bitrate_bps = bitrate_bps;
    if (bitrate_bps >= range->max_bitrate_bps) {
        updated->weak_network_level = 0U;
        return;
    }

    const uint32_t target_pct =
        range->max_bitrate_bps > 0U ?
            (uint32_t)(((uint64_t)bitrate_bps * 100ULL) /
                       range->max_bitrate_bps) :
            100U;
    updated->weak_network_mode =
        base->weak_network_mode != MEDIA_GOVERNOR_WEAK_NETWORK_OFF ?
            base->weak_network_mode :
            MEDIA_GOVERNOR_WEAK_NETWORK_FRAMERATE_PRIORITY;
    updated->weak_network_level =
        target_pct >= 85U ? 1U : (target_pct >= 70U ? 2U : 3U);
    if (updated->weak_network_mode ==
        MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY) {
        updated->fps = media_governor_resolution_priority_fps(
            base->fps,
            updated->weak_network_level);
    }
}

static bool media_governor_commit_transport_video_config(
    const media_governor_video_config_t *expected_base,
    const media_governor_video_config_t *expected_current,
    uint32_t expected_requested_bitrate_bps,
    TickType_t now,
    bool recovering,
    const media_governor_video_config_t *updated)
{
    bool committed = false;

    taskENTER_CRITICAL(&s_lock);
    if (s_transport_adaptation_active &&
        s_transport_requested_bitrate_bps == expected_requested_bitrate_bps &&
        memcmp(&s_adaptation_base_config,
               expected_base,
               sizeof(*expected_base)) == 0 &&
        memcmp(&s_rtc_video_config,
               expected_current,
               sizeof(*expected_current)) == 0) {
        s_rtc_video_config = *updated;
        if (recovering) {
            s_transport_recovery_not_before_tick =
                media_governor_tick_after_ms(
                    now,
                    MEDIA_GOVERNOR_TRANSPORT_RECOVERY_INTERVAL_MS);
        } else {
            s_transport_protection_not_before_tick =
                media_governor_tick_after_ms(
                    now,
                    MEDIA_GOVERNOR_TRANSPORT_PROTECTION_INTERVAL_MS);
            s_transport_recovery_not_before_tick =
                media_governor_tick_after_ms(
                    now,
                    MEDIA_GOVERNOR_TRANSPORT_RECOVERY_HOLD_MS);
        }
        s_initialized = true;
        committed = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    return committed;
}

static esp_err_t media_governor_converge_transport_bitrate(bool *changed)
{
    media_governor_video_config_t base = {0};
    media_governor_video_config_t current = {0};
    media_governor_transport_bitrate_range_t range = {0};
    uint32_t requested_bitrate_bps = 0U;
    TickType_t protection_not_before_tick = 0;
    TickType_t recovery_not_before_tick = 0;
    bool active = false;

    if (changed != NULL) {
        *changed = false;
    }

    taskENTER_CRITICAL(&s_lock);
    base = s_adaptation_base_config;
    current = s_rtc_video_config;
    active = s_transport_adaptation_active;
    requested_bitrate_bps = s_transport_requested_bitrate_bps;
    protection_not_before_tick = s_transport_protection_not_before_tick;
    recovery_not_before_tick = s_transport_recovery_not_before_tick;
    taskEXIT_CRITICAL(&s_lock);

    if (!active || requested_bitrate_bps == 0U) {
        return ESP_OK;
    }

    media_governor_build_transport_bitrate_range(&base, &range);
    requested_bitrate_bps =
        media_governor_transport_clamp_bitrate(requested_bitrate_bps, &range);
    if (requested_bitrate_bps == current.bitrate_bps) {
        return ESP_OK;
    }

    const uint32_t delta_bps =
        requested_bitrate_bps > current.bitrate_bps ?
            requested_bitrate_bps - current.bitrate_bps :
            current.bitrate_bps - requested_bitrate_bps;
    uint32_t min_step_bps =
        (uint32_t)(((uint64_t)current.bitrate_bps *
                    MEDIA_GOVERNOR_TRANSPORT_MIN_STEP_PERCENT) /
                   100ULL);
    if (min_step_bps < MEDIA_GOVERNOR_TRANSPORT_MIN_STEP_BPS) {
        min_step_bps = MEDIA_GOVERNOR_TRANSPORT_MIN_STEP_BPS;
    }
    const bool endpoint_target =
        requested_bitrate_bps == range.min_bitrate_bps ||
        requested_bitrate_bps == range.max_bitrate_bps;
    if (delta_bps < min_step_bps && !endpoint_target) {
        return ESP_OK;
    }

    uint32_t applied_bitrate_bps = requested_bitrate_bps;
    const bool recovering = requested_bitrate_bps > current.bitrate_bps;
    const TickType_t now = xTaskGetTickCount();
    if (recovering) {
        if (media_governor_tick_before(now, recovery_not_before_tick)) {
            return ESP_OK;
        }

        const uint32_t step_bps =
            media_governor_transport_recovery_step(current.bitrate_bps);
        if (requested_bitrate_bps - current.bitrate_bps > step_bps) {
            applied_bitrate_bps = current.bitrate_bps + step_bps;
        }
    } else {
        const bool emergency_drop =
            (uint64_t)requested_bitrate_bps * 100ULL <=
            (uint64_t)current.bitrate_bps *
                (100U - MEDIA_GOVERNOR_TRANSPORT_EMERGENCY_DROP_PERCENT);
        if (!emergency_drop &&
            protection_not_before_tick != 0U &&
            media_governor_tick_before(now, protection_not_before_tick)) {
            return ESP_OK;
        }
    }

    media_governor_video_config_t updated = {0};
    media_governor_build_transport_video_config(&base,
                                                &range,
                                                applied_bitrate_bps,
                                                &updated);
    if (!media_governor_commit_transport_video_config(&base,
                                                      &current,
                                                      requested_bitrate_bps,
                                                      now,
                                                      recovering,
                                                      &updated)) {
        return ESP_OK;
    }

    if (changed != NULL) {
        *changed = true;
    }
    ESP_LOGI(TAG,
             "transport bitrate: request=%ukbps apply=%u->%ukbps range=%u-%ukbps mode=%s",
             (unsigned)(requested_bitrate_bps / 1000U),
             (unsigned)(current.bitrate_bps / 1000U),
             (unsigned)(applied_bitrate_bps / 1000U),
             (unsigned)(range.min_bitrate_bps / 1000U),
             (unsigned)(range.max_bitrate_bps / 1000U),
             recovering ? "recover" : "protect");
    return ESP_OK;
}

esp_err_t media_governor_apply_transport_bitrate_target(uint32_t target_bitrate_bps,
                                                        bool *changed)
{
    media_governor_video_config_t base = {0};
    media_governor_video_config_t current = {0};
    media_governor_transport_bitrate_range_t range = {0};

    if (changed != NULL) {
        *changed = false;
    }
    if (target_bitrate_bps == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    base = s_adaptation_base_config;
    current = s_rtc_video_config;
    taskEXIT_CRITICAL(&s_lock);
    media_governor_build_transport_bitrate_range(&base, &range);
    target_bitrate_bps =
        media_governor_transport_clamp_bitrate(target_bitrate_bps, &range);
    const bool reducing_bitrate = target_bitrate_bps < current.bitrate_bps;
    const TickType_t now = xTaskGetTickCount();
    const TickType_t recovery_not_before_tick =
        reducing_bitrate ?
            media_governor_tick_after_ms(
                now,
                MEDIA_GOVERNOR_TRANSPORT_RECOVERY_HOLD_MS) :
            0;

    taskENTER_CRITICAL(&s_lock);
    s_transport_adaptation_active = true;
    s_transport_requested_bitrate_bps = target_bitrate_bps;
    if (reducing_bitrate) {
        s_transport_recovery_not_before_tick = recovery_not_before_tick;
    }
    taskEXIT_CRITICAL(&s_lock);

    return media_governor_converge_transport_bitrate(changed);
}

esp_err_t media_governor_step_transport_adaptation(bool *changed)
{
    return media_governor_converge_transport_bitrate(changed);
}

esp_err_t media_governor_reset_transport_adaptation(bool restore_base,
                                                    bool *changed)
{
    media_governor_video_config_t base = {0};
    media_governor_video_config_t current = {0};
    bool active = false;

    if (changed != NULL) {
        *changed = false;
    }

    taskENTER_CRITICAL(&s_lock);
    active = s_transport_adaptation_active;
    base = s_adaptation_base_config;
    current = s_rtc_video_config;
    s_transport_adaptation_active = false;
    s_transport_requested_bitrate_bps = 0U;
    s_transport_protection_not_before_tick = 0;
    s_transport_recovery_not_before_tick = 0;
    taskEXIT_CRITICAL(&s_lock);

    if (!active || !restore_base ||
        memcmp(&current, &base, sizeof(current)) == 0) {
        return ESP_OK;
    }

    esp_err_t ret = media_governor_set_rtc_video_config(&base);
    if (ret == ESP_OK && changed != NULL) {
        *changed = true;
    }
    return ret;
}

bool media_governor_transport_adaptation_active(void)
{
    bool active = false;

    taskENTER_CRITICAL(&s_lock);
    active = s_transport_adaptation_active;
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

void media_governor_get_rtc_video_config(media_governor_video_config_t *config)
{
    if (config == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    *config = s_rtc_video_config;
    taskEXIT_CRITICAL(&s_lock);
}

void media_governor_get_camera_policy(media_governor_camera_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    media_governor_profile_t profile = media_governor_get_profile();
    media_governor_video_config_t video_config = {0};

    media_governor_get_rtc_video_config(&video_config);
    switch (profile) {
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE:
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW:
        *policy = media_governor_make_rtc_av_policy(&video_config);
        break;
    case MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW:
    case MEDIA_GOVERNOR_PROFILE_QR_SCAN:
    case MEDIA_GOVERNOR_PROFILE_AI_CHAT:
    case MEDIA_GOVERNOR_PROFILE_RTC_AUDIO:
    case MEDIA_GOVERNOR_PROFILE_IDLE:
    default:
        *policy = s_policy_idle;
        break;
    }

    if (!s_initialized) {
        (void)media_governor_init();
    }
}

void media_governor_get_rtc_av_camera_policy(media_governor_camera_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    media_governor_video_config_t video_config = {0};
    media_governor_get_rtc_video_config(&video_config);
    *policy = media_governor_make_rtc_av_policy(&video_config);
    if (!s_initialized) {
        (void)media_governor_init();
    }
}

void media_governor_get_rtc_policy(media_governor_rtc_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }

    media_governor_profile_t profile = media_governor_get_profile();
    switch (profile) {
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_SAFE:
        *policy = s_rtc_policy_video_first;
        break;
    case MEDIA_GOVERNOR_PROFILE_RTC_AV_PREVIEW:
    case MEDIA_GOVERNOR_PROFILE_RTC_AUDIO:
    case MEDIA_GOVERNOR_PROFILE_LOCAL_PREVIEW:
    case MEDIA_GOVERNOR_PROFILE_QR_SCAN:
    case MEDIA_GOVERNOR_PROFILE_AI_CHAT:
    case MEDIA_GOVERNOR_PROFILE_IDLE:
    default:
        *policy = s_rtc_policy_normal;
        break;
    }

    if (!s_initialized) {
        (void)media_governor_init();
    }
}

void media_governor_note_network_backpressure(void)
{
    uint32_t count = 0;
    bool should_log = false;
    TickType_t now = xTaskGetTickCount();
    TickType_t until = now + pdMS_TO_TICKS(MEDIA_GOVERNOR_BACKPRESSURE_HOLD_MS);
    if (until == 0) {
        until = 1;
    }

    taskENTER_CRITICAL(&s_lock);
    s_backpressure_count++;
    count = s_backpressure_count;
    if (s_backpressure_until_tick == 0 ||
        !media_governor_tick_before(until, s_backpressure_until_tick)) {
        s_backpressure_until_tick = until;
    }
    if (s_last_backpressure_log_tick == 0 ||
        now - s_last_backpressure_log_tick >=
            pdMS_TO_TICKS(MEDIA_GOVERNOR_BACKPRESSURE_LOG_INTERVAL_MS)) {
        s_last_backpressure_log_tick = now;
        should_log = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (should_log) {
        ESP_LOGW(TAG, "network backpressure noted: count=%lu", (unsigned long)count);
    }
}

uint32_t media_governor_get_network_backpressure_count(void)
{
    uint32_t count = 0U;

    taskENTER_CRITICAL(&s_lock);
    count = s_backpressure_count;
    taskEXIT_CRITICAL(&s_lock);
    return count;
}

bool media_governor_is_network_backpressured(void)
{
    TickType_t until = 0;

    taskENTER_CRITICAL(&s_lock);
    until = s_backpressure_until_tick;
    taskEXIT_CRITICAL(&s_lock);

    if (until == 0) {
        return false;
    }
    return media_governor_tick_before(xTaskGetTickCount(), until);
}
