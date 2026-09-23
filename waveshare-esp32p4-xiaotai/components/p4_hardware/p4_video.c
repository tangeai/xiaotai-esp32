#include "p4_video.h"
#include "p4_video_capture.h"
#include <stdatomic.h>
#include <string.h>
#include "camera_pipeline.h"
#include "call_video_renderer.h"
#include "media_governor.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tiRTC.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_desired_generation;
static starter_tirtc_mode_t s_desired_mode;
static bool s_desired_video;
static atomic_uint s_live_generation;
static uint32_t s_worker_generation;
static starter_tirtc_mode_t s_worker_mode;
static bool s_worker_video;
static lv_obj_t *s_image;
static uint16_t *s_canvas;
static lv_img_dsc_t s_image_desc;
#define VIDEO_INGRESS_SLOTS 4
#define VIDEO_INGRESS_BYTES (256U * 1024U)
static struct {
    uint32_t generation;
    starter_tirtc_frame_t frame;
    uint8_t *data;
} s_ingress[VIDEO_INGRESS_SLOTS];
static QueueHandle_t s_free, s_pending;
static atomic_bool s_need_idr;
static int64_t s_last_idr_us;
static int64_t s_last_subscribe_us;
static uint32_t s_subscribed_generation;
static atomic_uint s_sent;
static atomic_uint s_rx_seen, s_rx_dropped, s_rx_last_media, s_rx_last_stream;
static int64_t s_health_due_us;
static int64_t s_health_last_log_us;
static uint32_t s_health_logged_generation, s_health_last_rx, s_health_last_drop;
static uint32_t s_health_last_decode_fail;
static uint32_t s_health_last_conversion_fail, s_health_last_overflow, s_health_last_discontinuities;
static bool s_health_last_subscribed;
static uint32_t s_bitrate_registered_generation;
static int64_t s_bitrate_step_us;

/* Keep the proven governor as the only encoder controller. The SDK callback
 * only coalesces a target; this media owner applies it outside all UI/SDK locks. */
static void configure_bitrate(uint32_t generation)
{
    s_bitrate_registered_generation = 0;
    s_bitrate_step_us = 0;
#if CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE
    media_governor_transport_bitrate_range_t range = {0};
    media_governor_get_transport_bitrate_range(&range);
    int ret = starter_tirtc_set_video_bitrate(generation, range.min_bitrate_bps,
                                             range.max_bitrate_bps, range.start_bitrate_bps);
    if (ret == 0) {
        s_bitrate_registered_generation = generation;
        ESP_LOGI("p4_video", "bitrate ready: gen=%lu min=%lu start=%lu max=%lu",
                 (unsigned long)generation, (unsigned long)range.min_bitrate_bps,
                 (unsigned long)range.start_bitrate_bps, (unsigned long)range.max_bitrate_bps);
        /* The SDK starts at start_bitrate_bps. Keep the encoder in the same
         * range before the first frame instead of waiting for feedback. */
        bool changed = false;
        esp_err_t apply_ret = media_governor_apply_transport_bitrate_target(
            range.start_bitrate_bps, &changed);
        if (apply_ret != ESP_OK) {
            ESP_LOGW("p4_video", "initial bitrate apply failed: %s",
                     esp_err_to_name(apply_ret));
        } else if (changed) {
            camera_pipeline_on_rtc_video_config_changed();
        }
    } else {
        /* KCP/WHIP peers may not support the TGTRP-only API. Keep their normal
         * profile, report the rejection once, and never pretend feedback works. */
        ESP_LOGW("p4_video", "bitrate unavailable: gen=%lu ret=%d; keep profile",
                 (unsigned long)generation, ret);
    }
#else
    (void)generation;
#endif
}

static void maintain_bitrate(void)
{
    uint32_t generation = atomic_load(&s_live_generation);
    if (!generation || generation != s_bitrate_registered_generation ||
        generation != starter_tirtc_generation()) return;
    bool changed = false;
    uint32_t target = 0;
    esp_err_t ret = ESP_OK;
    int64_t now = esp_timer_get_time();
    if (starter_tirtc_take_video_bitrate(generation, &target)) {
        ret = media_governor_apply_transport_bitrate_target(target, &changed);
    } else if (now >= s_bitrate_step_us) {
        ret = media_governor_step_transport_adaptation(&changed);
    } else {
        return;
    }
    s_bitrate_step_us = now + 200000;
    if (ret != ESP_OK) {
        ESP_LOGW("p4_video", "bitrate apply failed: %s", esp_err_to_name(ret));
    } else if (changed && generation == atomic_load(&s_live_generation)) {
        camera_pipeline_on_rtc_video_config_changed();
    }
}

/* Counters only in SDK callbacks; bounded diagnostics run on the media owner. */
static void video_health(void)
{
    uint32_t generation = atomic_load(&s_live_generation);
    if (!generation) return;
    int64_t now = esp_timer_get_time();
    if (now < s_health_due_us) return;
    s_health_due_us = now + 5000000;
    call_video_renderer_stats_t stats = {0};
    call_video_renderer_get_stats(&stats);
    uint32_t rx = atomic_load(&s_rx_seen);
    uint32_t drop = atomic_load(&s_rx_dropped);
    bool subscribed = s_subscribed_generation == generation;
    bool changed = s_health_logged_generation != generation ||
                   s_health_last_subscribed != subscribed ||
                   (s_health_last_rx == 0 && rx != 0) ||
                   s_health_last_drop != drop ||
                   s_health_last_decode_fail != stats.decode_failures ||
                   s_health_last_conversion_fail != stats.conversion_failures ||
                   s_health_last_overflow != stats.input_overflows ||
                   s_health_last_discontinuities != stats.discontinuities;
    int64_t interval_us = rx == 0 ? 30000000 : 15000000;
    if (!changed && now - s_health_last_log_us < interval_us) return;
    s_health_last_log_us = now;
    s_health_logged_generation = generation;
    s_health_last_rx = rx;
    s_health_last_drop = drop;
    s_health_last_decode_fail = stats.decode_failures;
    s_health_last_conversion_fail = stats.conversion_failures;
    s_health_last_overflow = stats.input_overflows;
    s_health_last_discontinuities = stats.discontinuities;
    s_health_last_subscribed = subscribed;
    ESP_LOGI("p4_video", "VID g=%lu m=%d sub=%u rx/drop=%lu/%lu dec/conv/show=%lu/%lu/%lu fail=%lu/%lu ov/dis=%lu/%lu q=%lu/%lu stream/media=%lu/%lu",
             (unsigned long)generation, (int)starter_tirtc_mode(), (unsigned)subscribed,
             (unsigned long)rx, (unsigned long)drop,
             (unsigned long)stats.decoded_frames, (unsigned long)stats.converted_frames,
             (unsigned long)stats.presented_frames, (unsigned long)stats.decode_failures,
             (unsigned long)stats.conversion_failures,
             (unsigned long)stats.input_overflows, (unsigned long)stats.discontinuities,
             (unsigned long)stats.queue_depth, (unsigned long)stats.conversion_queue_depth,
             (unsigned long)atomic_load(&s_rx_last_stream),
             (unsigned long)atomic_load(&s_rx_last_media));
}
static atomic_bool s_camera_enabled = true;
static atomic_bool s_tx_enabled;
static bool s_worker_camera;

esp_err_t p4_video_set_camera_enabled(uint32_t generation, bool enabled)
{
    taskENTER_CRITICAL(&s_lock);
    bool valid = generation != 0 && generation == s_desired_generation && s_desired_video &&
        (s_desired_mode == STARTER_TIRTC_CALL || s_desired_mode == STARTER_TIRTC_VOIP);
    if (valid) {
        atomic_store(&s_camera_enabled, enabled);
        if (!enabled) atomic_store(&s_tx_enabled, false);
    }
    taskEXIT_CRITICAL(&s_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* Only the media owner changes camera hardware. Camera privacy is independent
 * of the decoder/subscription, so disabling TX preserves remote video/audio. */
static void maintain_camera(void)
{
    if (!atomic_load(&s_live_generation)) return;
    bool enabled = atomic_load(&s_camera_enabled);
    if (s_worker_camera && (!enabled || !atomic_load(&s_tx_enabled))) {
        atomic_store(&s_tx_enabled, false);
        (void)camera_pipeline_set_rtc_video_enabled(false);
        if (camera_pipeline_is_running()) return;
        s_worker_camera = false;
    }
    if (enabled && !s_worker_camera) {
        if (camera_pipeline_is_running()) return;
        if (camera_pipeline_set_rtc_video_enabled(true) == ESP_OK) {
            s_worker_camera = true;
            atomic_store(&s_tx_enabled, atomic_load(&s_camera_enabled));
            camera_pipeline_request_stream_start_key_frame();
        }
    }
}

/* SDK subscription can fail transiently while the call transport settles.
 * Retry on the media owner, never from the receive callback or LVGL task. */
static void maintain_subscription(void)
{
    uint32_t generation = atomic_load(&s_live_generation);
    starter_tirtc_mode_t mode = starter_tirtc_mode();
    if (!generation || generation != starter_tirtc_generation() ||
        (mode != STARTER_TIRTC_H5 && mode != STARTER_TIRTC_CALL &&
         mode != STARTER_TIRTC_VOIP) || s_subscribed_generation == generation) return;
    int64_t now = esp_timer_get_time();
    if (s_last_subscribe_us && now - s_last_subscribe_us < 1000000) return;
    s_last_subscribe_us = now;
    int ret = mode == STARTER_TIRTC_H5
                  ? starter_tirtc_subscribe_h5_video(generation)
                  : starter_tirtc_subscribe_call_video();
    if (ret >= 0) {
        s_subscribed_generation = generation;
        atomic_store(&s_need_idr, mode == STARTER_TIRTC_H5 ||
                                  mode == STARTER_TIRTC_CALL);
    }
}

static void drain_video(void)
{
    uint8_t slot;
    for (unsigned i = 0; s_pending && i < VIDEO_INGRESS_SLOTS &&
         xQueueReceive(s_pending, &slot, 0) == pdTRUE; ++i) {
        const starter_tirtc_frame_t *f = &s_ingress[slot].frame;
        if (s_ingress[slot].generation == atomic_load(&s_live_generation)) {
            esp_err_t ret = ESP_ERR_INVALID_STATE;
            starter_tirtc_mode_t mode = starter_tirtc_mode();
            if ((mode == STARTER_TIRTC_H5 || mode == STARTER_TIRTC_CALL) &&
                f->media == TIRTC_VIDEO_H264)
                ret = call_video_renderer_submit_h264(s_ingress[slot].data, f->length,
                    (f->flags & TIRTC_FRAME_FLAG_KEY_FRAME) != 0, f->timestamp_ms);
            else if (mode == STARTER_TIRTC_VOIP && f->media == TIRTC_VIDEO_JPEG)
                ret = call_video_renderer_submit_mjpeg(s_ingress[slot].data, f->length, f->timestamp_ms);
            if (ret != ESP_OK && f->media == TIRTC_VIDEO_H264) atomic_store(&s_need_idr, true);
        }
        (void)xQueueSend(s_free, &slot, 0);
    }
    starter_tirtc_mode_t mode = starter_tirtc_mode();
    if (atomic_load(&s_live_generation) &&
        (mode == STARTER_TIRTC_H5 || mode == STARTER_TIRTC_CALL) &&
        (atomic_load(&s_need_idr) || call_video_renderer_requires_key_frame()) &&
        esp_timer_get_time() - s_last_idr_us > 1000000) {
        s_last_idr_us = esp_timer_get_time();
        if (starter_tirtc_request_remote_key_frame() >= 0) atomic_store(&s_need_idr, false);
    }
}

static esp_err_t send_video(const uint8_t *data, size_t len, uint16_t w, uint16_t h,
                            uint64_t pts, uint8_t media, bool key, void *ctx)
{
    (void)ctx;
    uint32_t generation = atomic_load(&s_live_generation);
    if (!atomic_load(&s_tx_enabled) || !atomic_load(&s_camera_enabled) ||
        !generation || generation != starter_tirtc_generation() ||
        media != TIRTC_VIDEO_H264 || !starter_tirtc_video_ready()) return ESP_ERR_INVALID_STATE;
    p4_video_capture_offer(data, len, w, h, key, generation);
    if (starter_tirtc_send_h264((uint32_t)(pts / 1000), data, len, key) < 0) {
        /* A dropped reference frame makes subsequent P frames unusable. */
        camera_pipeline_request_key_frame();
        return ESP_FAIL;
    }
    atomic_fetch_add(&s_sent, 1);
    return ESP_OK;
}

esp_err_t p4_video_init(void)
{
    static bool ready;
    if (ready) return ESP_OK;
    esp_err_t ret = ESP_ERR_NO_MEM;
    s_free = xQueueCreate(VIDEO_INGRESS_SLOTS, sizeof(uint8_t));
    s_pending = xQueueCreate(VIDEO_INGRESS_SLOTS, sizeof(uint8_t));
    if (!s_free || !s_pending) goto fail;
    for (uint8_t i = 0; i < VIDEO_INGRESS_SLOTS; ++i) {
        s_ingress[i].data = heap_caps_malloc(VIDEO_INGRESS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ingress[i].data) goto fail;
        (void)xQueueSend(s_free, &i, 0);
    }
    ret = call_video_renderer_prewarm_mjpeg_decoder();
    if (ret == ESP_OK) ret = camera_pipeline_prewarm_h264();
    if (ret == ESP_OK) ret = camera_pipeline_prewarm_call_scaler();
    if (ret == ESP_OK) ret = call_video_renderer_prewarm();
    if (ret == ESP_OK) ret = p4_video_capture_init();
    if (ret == ESP_OK) ret = camera_pipeline_set_rtc_video_sink(send_video, NULL);
    if (ret != ESP_OK) goto fail;
    ready = true;
    return ESP_OK;
fail:
    /* No video worker is running yet. Release only this owner's allocations;
     * codec/scaler prewarm caches remain owned by their idempotent services. */
    for (unsigned i = 0; i < VIDEO_INGRESS_SLOTS; ++i) {
        heap_caps_free(s_ingress[i].data);
        s_ingress[i].data = NULL;
    }
    if (s_free) vQueueDelete(s_free);
    if (s_pending) vQueueDelete(s_pending);
    s_free = s_pending = NULL;
    return ret;
}

void p4_video_set_session(starter_tirtc_mode_t mode, uint32_t generation, bool video)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_desired_mode == mode && s_desired_generation == generation && s_desired_video == video) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    atomic_store(&s_live_generation, 0); /* invalidate before asynchronous drain */
    atomic_store(&s_tx_enabled, false);
    atomic_store(&s_camera_enabled, true); /* reset per session, never persistent */
    s_desired_mode = mode;
    s_desired_generation = generation;
    s_desired_video = video;
    taskEXIT_CRITICAL(&s_lock);
}

void p4_video_poll(void)
{
    maintain_subscription();
    drain_video();
    video_health();
    uint32_t generation;
    starter_tirtc_mode_t mode;
    bool video;
    taskENTER_CRITICAL(&s_lock);
    generation = s_desired_generation;
    mode = s_desired_mode;
    video = s_desired_video;
    taskEXIT_CRITICAL(&s_lock);
    if (generation == s_worker_generation && mode == s_worker_mode && video == s_worker_video) {
        maintain_bitrate();
        maintain_camera();
        return;
    }
    atomic_store(&s_live_generation, 0);
    (void)camera_pipeline_set_rtc_video_enabled(false);
    /* Never reuse a session context while its previous encoder worker lives. */
    int64_t deadline = esp_timer_get_time() + 4000000;
    while (camera_pipeline_is_running() && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (camera_pipeline_is_running()) return; /* retained ownership; retry, no forced deletion */
    if (call_video_renderer_stop() != ESP_OK) return;
    bool bitrate_changed = false;
    (void)media_governor_reset_transport_adaptation(true, &bitrate_changed);
    s_bitrate_registered_generation = 0;
    s_bitrate_step_us = 0;
    s_worker_camera = false;
    atomic_store(&s_tx_enabled, false);
    s_subscribed_generation = 0;
    s_last_subscribe_us = 0;
    s_last_idr_us = 0;
    atomic_store(&s_need_idr, false);
    atomic_store(&s_sent, 0);
    atomic_store(&s_rx_seen, 0);
    atomic_store(&s_rx_dropped, 0);
    atomic_store(&s_rx_last_stream, 0);
    atomic_store(&s_rx_last_media, 0);
    s_health_due_us = esp_timer_get_time() + 5000000;
    if (generation && video && mode != STARTER_TIRTC_AI) {
        media_governor_video_config_t config;
        if (mode == STARTER_TIRTC_VOIP || mode == STARTER_TIRTC_H5)
            media_governor_build_wechat_video_config(&config);
        else media_governor_build_device_call_video_config(&config);
        /* Phone/browser uplink is independent of P4-to-P4 decode limits. */
        esp_err_t ret = media_governor_set_rtc_video_config(&config);
        if (ret == ESP_OK) {
            /* The prewarmed encoder owns the singleton output workspace.
             * Replace an incompatible reservation while capture is stopped,
             * before the new worker attempts to acquire that workspace. */
            camera_pipeline_on_rtc_video_config_changed();
        }
        if (ret == ESP_OK)
            ret = call_video_renderer_start_for_codec(mode == STARTER_TIRTC_VOIP
                ? CALL_VIDEO_CODEC_MJPEG : CALL_VIDEO_CODEC_H264);
        if (ret == ESP_OK) {
            taskENTER_CRITICAL(&s_lock);
            bool current = s_desired_generation == generation && s_desired_mode == mode && s_desired_video;
            if (current) atomic_store(&s_live_generation, generation);
            taskEXIT_CRITICAL(&s_lock);
            if (!current) return;
            configure_bitrate(generation);
            if (atomic_load(&s_camera_enabled)) {
                ret = camera_pipeline_set_rtc_video_enabled(true);
                if (ret == ESP_OK) {
                    s_worker_camera = true;
                    atomic_store(&s_tx_enabled, atomic_load(&s_camera_enabled));
                }
            }
        }
        if (ret != ESP_OK) {
            atomic_store(&s_live_generation, 0);
            ESP_LOGE("p4_video", "video start failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(500));
            return;
        }
        maintain_subscription();
        camera_pipeline_request_stream_start_key_frame();
    }
    s_worker_generation = generation;
    s_worker_mode = mode;
    s_worker_video = video;
}

void p4_video_key_frame(void) { camera_pipeline_request_key_frame(); }
uint32_t p4_video_sent(void) { return atomic_load(&s_sent); }

void p4_video_submit(uint32_t generation, const starter_tirtc_frame_t *frame, const void *data)
{
    if (frame && generation == atomic_load(&s_live_generation) && generation) {
        atomic_fetch_add(&s_rx_seen, 1);
        atomic_store(&s_rx_last_media, frame->media);
        atomic_store(&s_rx_last_stream, frame->stream_id);
    }
    if (!generation || generation != atomic_load(&s_live_generation) ||
        frame == NULL || data == NULL) return;
    /* H5 uses the advertised device-relative downlink stream. Legacy call and
     * WeChat paths retain their negotiated stream semantics and validate codec. */
    starter_tirtc_mode_t mode = starter_tirtc_mode();
    if (!((mode == STARTER_TIRTC_H5 &&
           frame->stream_id == STARTER_H5_DOWN_VIDEO_STREAM_ID &&
           frame->media == TIRTC_VIDEO_H264) ||
          (mode == STARTER_TIRTC_VOIP && frame->media == TIRTC_VIDEO_JPEG) ||
          (mode == STARTER_TIRTC_CALL && frame->media == TIRTC_VIDEO_H264))) {
        atomic_fetch_add(&s_rx_dropped, 1);
        return;
    }
    if (!s_free || frame->length == 0 || frame->length > VIDEO_INGRESS_BYTES) {
        atomic_fetch_add(&s_rx_dropped, 1);
        atomic_store(&s_need_idr, true);
        return;
    }
    uint8_t slot;
    if (xQueueReceive(s_free, &slot, 0) != pdTRUE) {
        atomic_fetch_add(&s_rx_dropped, 1);
        atomic_store(&s_need_idr, true);
        return;
    }
    s_ingress[slot].generation = generation;
    s_ingress[slot].frame = *frame;
    memcpy(s_ingress[slot].data, data, frame->length);
    if (xQueueSend(s_pending, &slot, 0) != pdTRUE) {
        atomic_fetch_add(&s_rx_dropped, 1);
        (void)xQueueSend(s_free, &slot, 0);
        atomic_store(&s_need_idr, true);
    }
}

void p4_video_ui_reset(void) { s_image = NULL; }

void p4_video_ui_tick(lv_obj_t *screen, bool video_visible)
{
    if (!video_visible || !atomic_load(&s_live_generation)) {
        if (s_image) lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!s_canvas) s_canvas = heap_caps_malloc(480 * 320 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas) return;
    int64_t present_started_us = esp_timer_get_time();
    const uint16_t *pixels = NULL;
    size_t count = 0;
    if (call_video_renderer_present_next_rgb565(&pixels, &count, NULL, NULL) != ESP_OK) return;
    if (pixels && count == 480 * 320) {
        memcpy(s_canvas, pixels, count * 2);
        if (!s_image) {
            s_image_desc = (lv_img_dsc_t){.header = {.cf = LV_IMG_CF_TRUE_COLOR, .w = 480, .h = 320},
                .data_size = 480 * 320 * 2, .data = (const uint8_t *)s_canvas};
            s_image = lv_img_create(screen);
            lv_img_set_src(s_image, &s_image_desc);
            lv_obj_clear_flag(s_image, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_background(s_image);
        }
        lv_obj_clear_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_image);
    }
    call_video_renderer_release_presented_rgb565();
    static int64_t last_slow_present_log_us;
    int64_t elapsed_us = esp_timer_get_time() - present_started_us;
    if (elapsed_us > 45000 &&
        present_started_us - last_slow_present_log_us >= 10000000) {
        last_slow_present_log_us = present_started_us;
        ESP_LOGW("p4_video", "UIP present=%lldus", (long long)elapsed_us);
    }
}
