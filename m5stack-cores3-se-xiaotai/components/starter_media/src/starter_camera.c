/* GC2145 viewing producer. One owner holds every camera/encoder resource;
 * SDK callbacks only publish subscription flags. Audio never waits for JPEG.
 * No capture, encoder, framebuffer or camera DMA allocation at boot. */
#include "starter_camera.h"
#include "starter_media.h"

#include <stdatomic.h>
#include <stdint.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#if CONFIG_CAMERA_PSRAM_DMA || !CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW
#error "S3 viewing requires internal DMA staging and the shared new I2C driver"
#endif

#define VIEW_STACK_BYTES 16384U
#define VIEW_FRAME_US (1000000 / 12)
#define VIEW_JPEG_BYTES (32U * 1024U)
#define VIEW_MAX_AGE_MS 250U
#define VIEW_FRAME_TIMEOUT_US 1500000

static const char *TAG = "view_camera";
static TaskHandle_t s_task; /* Created/retained by the single runtime owner. */
static atomic_uint s_requested;
static struct {
    atomic_int phase, error;
    atomic_uint generation, width, height, quality;
    atomic_uint captured, sent, stale, congested, send_failed, encode_failed;
    atomic_uint encode_max_us, send_max_us, age_max_ms, late, fps_x10;
} s_diag;

typedef struct {
    bool initialized;
    jpeg_enc_handle_t encoder;
    uint8_t *jpeg;
    unsigned width, height, quality;
} camera_owner_t;

static void maximum(atomic_uint *counter, uint32_t value)
{
    uint32_t before = atomic_load(counter);
    if (value > before) atomic_store(counter, value); /* Sole writer. */
}

static int64_t next_frame_deadline(int64_t previous, int64_t done)
{
    int64_t next = previous + VIEW_FRAME_US;
    /* A small overrun must not add an entire idle frame period. Reset the
     * phase at completion; the one-entry queue supplies only a fresh frame,
     * so missed frames are never replayed in a catch-up burst. */
    return next < done ? done : next;
}

static bool admitted(uint32_t generation)
{
    return generation && atomic_load(&s_requested) == generation &&
           starter_media_h5_current(generation) && starter_tirtc_video_ready() &&
           starter_tirtc_generation() == generation;
}

static void heap_log(const char *stage)
{
    ESP_LOGI(TAG, "%s internal=%u largest=%u psram=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t close_camera(camera_owner_t *owner)
{
    /* Called only after returning the borrowed frame and finishing SDK send.
     * SCCB pin_sccb_sda=-1 means camera deinit must not delete I2C0. */
    atomic_store(&s_diag.phase, STARTER_CAMERA_STOPPING);
    esp_err_t ret = ESP_OK;
    if (owner->initialized) ret = esp_camera_deinit();
    owner->initialized = false;
    if (owner->encoder) {
        if (jpeg_enc_close(owner->encoder) != JPEG_ERR_OK && ret == ESP_OK) ret = ESP_FAIL;
        owner->encoder = NULL;
    }
    heap_caps_free(owner->jpeg);
    owner->jpeg = NULL;
    esp_err_t power_ret = starter_media_camera_power(false);
    if (ret == ESP_OK) ret = power_ret;
    atomic_store(&s_diag.error, ret);
    atomic_store(&s_diag.phase, ret == ESP_OK ? STARTER_CAMERA_OFF : STARTER_CAMERA_ERROR);
    if (ret != ESP_OK) ESP_LOGE(TAG, "close failed rc=%s", esp_err_to_name(ret));
    heap_log("closed");
    return ret;
}

static esp_err_t open_camera(camera_owner_t *owner, uint32_t generation, bool small)
{
    if (!admitted(generation)) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_diag.phase, STARTER_CAMERA_STARTING);
    heap_log("open");
    /* Full-system measurements, not the standalone JPEG benchmark, set the
     * ceiling. QVGA exceeded the 83 ms budget with AFE/audio/UI running. */
    owner->width = small ? 160 : 240;
    owner->height = small ? 120 : 176;
    owner->quality = 45;
    esp_err_t err = starter_media_camera_power(true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!admitted(generation)) return ESP_ERR_INVALID_STATE;
    const camera_config_t cfg = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 5,
        .pin_sccb_sda = -1, .pin_sccb_scl = -1, .sccb_i2c_port = 0,
        .pin_d0 = 16, .pin_d1 = 18, .pin_d2 = 8, .pin_d3 = 17,
        .pin_d4 = 15, .pin_d5 = 6, .pin_d6 = 4, .pin_d7 = 9,
        .pin_vsync = 3, .pin_href = 46, .pin_pclk = 7,
        /* LCD_CAM uses an integer 160 MHz divider: 20 MHz is exact; a
         * requested 24 MHz becomes 26.67 MHz in this driver. */
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_1, .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = PIXFORMAT_YUV422, .frame_size = small ? FRAMESIZE_QQVGA : FRAMESIZE_HQVGA,
        .jpeg_quality = 12, /* Ignored for raw capture; encoder quality is separate. */
        /* Capture overlaps JPEG at this measured resolution. A one-entry
         * latest queue bounds age; the borrowed frame cannot be overwritten. */
        .fb_count = 2, .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    err = esp_camera_init(&cfg);
    if (err != ESP_OK) return err; /* Driver releases partial init on failure. */
    owner->initialized = true;
    sensor_t *sensor = esp_camera_sensor_get();
    /* Use the sensor ID measured on the target board, not its product name. */
    if (!sensor || sensor->id.PID != GC2145_PID || esp_camera_get_psram_mode()) {
        ESP_LOGE(TAG, "camera contract failed: pid=%x psram_dma=%d",
                 sensor ? sensor->id.PID : 0, esp_camera_get_psram_mode());
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!admitted(generation)) return ESP_ERR_INVALID_STATE;
    /* Stock board mount; do not inherit a previous view's orientation. */
    if (sensor->set_hmirror(sensor, 0) != 0 || sensor->set_vflip(sensor, 0) != 0)
        return ESP_FAIL;
    owner->jpeg = heap_caps_aligned_alloc(16, VIEW_JPEG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!owner->jpeg) return ESP_ERR_NO_MEM;
    jpeg_enc_config_t jpeg_cfg = DEFAULT_JPEG_ENC_CONFIG();
    jpeg_cfg.width = owner->width;
    jpeg_cfg.height = owner->height;
    /* GC2145 emits Y-Cb-Y-Cr directly. Avoid an RGB-to-YUV conversion on CPU. */
    jpeg_cfg.src_type = JPEG_PIXEL_FORMAT_YCbYCr;
    jpeg_cfg.subsampling = JPEG_SUBSAMPLE_420;
    jpeg_cfg.quality = owner->quality;
    jpeg_cfg.task_enable = false; /* No hidden parallel encoder worker. */
    jpeg_error_t jpeg_result = jpeg_enc_open(&jpeg_cfg, &owner->encoder);
    if (jpeg_result != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "encoder open failed rc=%d", jpeg_result);
        return jpeg_result == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    atomic_store(&s_diag.width, owner->width);
    atomic_store(&s_diag.height, owner->height);
    atomic_store(&s_diag.quality, owner->quality);
    atomic_store(&s_diag.phase, STARTER_CAMERA_RUNNING);
    ESP_LOGI(TAG, "ready gen=%lu size=%ux%u fps=12 quality=%u raw=%ux2 jpeg=%u stack=%u psram_dma=0",
             (unsigned long)generation, owner->width, owner->height, owner->quality,
             owner->width * owner->height * 2, VIEW_JPEG_BYTES, VIEW_STACK_BYTES);
    heap_log("ready");
    return ESP_OK;
}

static void reset_counters(uint32_t generation)
{
    atomic_store(&s_diag.generation, generation);
    atomic_store(&s_diag.error, ESP_OK);
#define CLEAR(field) atomic_store(&s_diag.field, 0)
    CLEAR(captured); CLEAR(sent); CLEAR(stale); CLEAR(congested);
    CLEAR(send_failed); CLEAR(encode_failed); CLEAR(encode_max_us);
    CLEAR(send_max_us); CLEAR(age_max_ms); CLEAR(late); CLEAR(fps_x10);
#undef CLEAR
}

static void camera_worker(void *arg)
{
    (void)arg;
    camera_owner_t owner = {0};
    uint32_t failed_generation = 0;
    for (;;) {
        uint32_t generation = atomic_load(&s_requested);
        if (!admitted(generation) || generation == failed_generation) {
            if (generation == 0) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            else (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        reset_counters(generation);
        bool small = false;
        esp_err_t err = open_camera(&owner, generation, small);
        int64_t next = esp_timer_get_time();
        int64_t last_frame = next, window = next;
        uint32_t sent_before = 0, pressure = 0;
        uint32_t encode_sum = 0, encode_count = 0, encode_peak = 0;
        while (err == ESP_OK && admitted(generation)) {
            int64_t now = esp_timer_get_time();
            if (now < next) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
                continue;
            }
            /* Avoid the driver's four-second wait on a normally empty queue.
             * Its internal GDMA recovery can still wait after a NULL sentinel;
             * only this worker may call get/deinit, never the runtime or UI. */
            if (!esp_camera_available_frames()) {
                if (now - last_frame > VIEW_FRAME_TIMEOUT_US) err = ESP_ERR_TIMEOUT;
                else (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
                continue;
            }
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) { err = ESP_FAIL; break; }
            last_frame = now;
            atomic_fetch_add(&s_diag.captured, 1);
            int64_t captured_us = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
            uint32_t age_ms = captured_us <= now ? (uint32_t)((now - captured_us) / 1000) : UINT32_MAX;
            /* CPU copies from a small internal DMA ring into PSRAM. No direct
             * DMA/cache invalidation touches these frame allocations. */
            bool valid = fb->format == PIXFORMAT_YUV422 && fb->width == owner.width &&
                         fb->height == owner.height && fb->len == owner.width * owner.height * 2 &&
                         esp_ptr_external_ram(fb->buf) && ((uintptr_t)fb->buf % 16 == 0);
            bool current = admitted(generation);
            bool congested = current && starter_tirtc_h5_video_congested(generation);
            int jpeg_size = 0;
            int64_t encode_begin = esp_timer_get_time();
            if (!valid) {
                ESP_LOGE(TAG, "raw invalid: format=%d size=%ux%u len=%u expected=%ux%u psram=%d offset=%u",
                         fb->format, (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len,
                         owner.width, owner.height, esp_ptr_external_ram(fb->buf),
                         (unsigned)((uintptr_t)fb->buf % 16));
                err = ESP_ERR_INVALID_SIZE;
            } else if (!current || age_ms > VIEW_MAX_AGE_MS) {
                atomic_fetch_add(&s_diag.stale, 1);
            } else if (congested) {
                atomic_fetch_add(&s_diag.congested, 1);
                ++pressure;
            } else {
                jpeg_error_t result = jpeg_enc_process(owner.encoder, fb->buf, (int)fb->len,
                                                       owner.jpeg, VIEW_JPEG_BYTES, &jpeg_size);
                uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_begin);
                maximum(&s_diag.encode_max_us, encode_us);
                encode_sum += encode_us;
                ++encode_count;
                if (encode_us > encode_peak) encode_peak = encode_us;
                if (result != JPEG_ERR_OK || jpeg_size < 4 || (unsigned)jpeg_size > VIEW_JPEG_BYTES ||
                    owner.jpeg[0] != 0xff || owner.jpeg[1] != 0xd8 ||
                    owner.jpeg[jpeg_size - 2] != 0xff || owner.jpeg[jpeg_size - 1] != 0xd9) {
                    atomic_fetch_add(&s_diag.encode_failed, 1);
                    err = ESP_FAIL; /* A broken/partial image is never sent. */
                    ESP_LOGE(TAG, "encode failed rc=%d len=%d", result, jpeg_size);
                }
            }
            /* Release only after JPEG no longer reads the borrowed frame. */
            esp_camera_fb_return(fb);
            fb = NULL;
            if (err == ESP_OK && jpeg_size > 0) {
                int64_t send_begin = esp_timer_get_time();
                uint32_t send_age = (uint32_t)((send_begin - captured_us) / 1000);
                maximum(&s_diag.age_max_ms, send_age);
                if (!admitted(generation) || send_age > VIEW_MAX_AGE_MS) {
                    atomic_fetch_add(&s_diag.stale, 1);
                } else if (starter_tirtc_h5_video_congested(generation)) {
                    atomic_fetch_add(&s_diag.congested, 1);
                    ++pressure;
                } else {
                    int sent = starter_tirtc_send_h5_jpeg(generation, captured_us / 1000,
                                                         owner.jpeg, (uint32_t)jpeg_size);
                    maximum(&s_diag.send_max_us, esp_timer_get_time() - send_begin);
                    if (sent > 0) atomic_fetch_add(&s_diag.sent, 1);
                    else { atomic_fetch_add(&s_diag.send_failed, 1); ++pressure; }
                }
            }
            int64_t done = esp_timer_get_time();
            if (done - now > VIEW_FRAME_US) { atomic_fetch_add(&s_diag.late, 1); ++pressure; }
            /* Keep the nominal phase when on time; resume immediately after
             * an overrun instead of adding up to 83 ms of artificial idle. */
            next = next_frame_deadline(next, done);
            if (done - window >= 5000000) {
                uint32_t sent = atomic_load(&s_diag.sent);
                atomic_store(&s_diag.fps_x10, (uint32_t)((sent - sent_before) * 10000000ULL / (done - window)));
                ESP_LOGI(TAG, "gen=%lu enqueue_fps10=%u captured=%u sent=%u stale=%u busy=%u fail=%u/%u enc_avg/peak_us=%u/%u send_us=%u age_ms=%u late=%u stack_free=%u",
                         (unsigned long)generation, atomic_load(&s_diag.fps_x10),
                         atomic_load(&s_diag.captured), sent,
                         atomic_load(&s_diag.stale), atomic_load(&s_diag.congested),
                         atomic_load(&s_diag.encode_failed), atomic_load(&s_diag.send_failed),
                         encode_count ? encode_sum / encode_count : 0, encode_peak,
                         atomic_load(&s_diag.send_max_us),
                         atomic_load(&s_diag.age_max_ms), atomic_load(&s_diag.late),
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
                /* Read-only audio health alongside video load. These counters
                 * do not reset or reconfigure the shared audio pipeline. */
                starter_media_status_t audio = starter_media_status();
                starter_media_pipeline_status_t pipeline = starter_media_pipeline_status();
                ESP_LOGI(TAG, "audio tx=%lu drop=%lu aec_err=%lu read_err=%lu write_err=%lu",
                         (unsigned long)audio.audio_sent, (unsigned long)audio.audio_dropped,
                         (unsigned long)audio.aec_errors, (unsigned long)pipeline.read_errors,
                         (unsigned long)audio.audio_write_failed);
                if (pressure >= 12 && admitted(generation) && err == ESP_OK) {
                    if (owner.quality > 30) {
                        owner.quality -= 5;
                        if (jpeg_enc_set_quality(owner.encoder, owner.quality) != JPEG_ERR_OK) err = ESP_FAIL;
                        atomic_store(&s_diag.quality, owner.quality);
                        ESP_LOGI(TAG, "quality=%u pressure=%u", owner.quality, pressure);
                    } else if (!small) {
                        /* Resize only between complete frames, through full
                         * driver teardown. Never alter live DMA dimensions. */
                        err = close_camera(&owner);
                        small = true;
                        if (err == ESP_OK) err = open_camera(&owner, generation, small);
                        last_frame = esp_timer_get_time();
                    }
                }
                pressure = 0;
                encode_sum = 0;
                encode_count = 0;
                encode_peak = 0;
                sent_before = sent;
                window = esp_timer_get_time();
            }
            vTaskDelay(1); /* Encoder has lower priority than audio/LVGL. */
        }
        bool cancelled = !admitted(generation);
        esp_err_t close_err = close_camera(&owner);
        if (close_err != ESP_OK || (err != ESP_OK && !cancelled)) {
            failed_generation = generation;
            atomic_store(&s_diag.error, close_err != ESP_OK ? close_err : err);
            atomic_store(&s_diag.phase, STARTER_CAMERA_ERROR);
            ESP_LOGE(TAG, "view failed gen=%lu rc=%s; reopen viewing to retry",
                     (unsigned long)generation, esp_err_to_name(atomic_load(&s_diag.error)));
        }
    }
}

esp_err_t starter_camera_prepare(uint32_t generation)
{
    if (s_task) return ESP_OK;
    /* Keep the measured HQVGA worker placement below realtime audio/driver
     * tasks; raising its priority must not steal AFE or capture deadlines. */
    if (xTaskCreatePinnedToCoreWithCaps(camera_worker, "view_camera", VIEW_STACK_BYTES,
                                      NULL, 3, &s_task, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) return ESP_OK;
    atomic_store(&s_diag.generation, generation);
    atomic_store(&s_diag.error, ESP_ERR_NO_MEM);
    atomic_store(&s_diag.phase, STARTER_CAMERA_ERROR);
    return ESP_ERR_NO_MEM;
}

void starter_camera_set_session(uint32_t generation)
{
    atomic_store(&s_requested, generation);
    if (s_task) xTaskNotifyGive(s_task);
}

starter_camera_status_t starter_media_camera_status(void)
{
    return (starter_camera_status_t){
#define FIELD(name) .name = atomic_load(&s_diag.name)
        FIELD(phase), FIELD(error), FIELD(generation), FIELD(width), FIELD(height), FIELD(quality),
        FIELD(captured), FIELD(sent), FIELD(stale), FIELD(congested), FIELD(send_failed),
        FIELD(encode_failed), FIELD(encode_max_us), FIELD(send_max_us), FIELD(age_max_ms), FIELD(late), FIELD(fps_x10),
#undef FIELD
    };
}
